/**
 * @file    svc_net.h
 * @brief   RTSP 服务端 —— 事件循环 + 客户端会话管理 (M1-8)
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它在整个系统里的位置
 * ─────────────────────────────────────────────────────────────────
 *  前面几个模块都是"零件":
 *      proto_rtsp  : 报文文本 ←→ 结构体   (纯函数, PC 可单测)
 *      proto_sdp   : 生成 SDP            (纯函数, PC 可单测)
 *      proto_rtp   : NALU → RTP 包       (纯函数, PC 可单测)
 *      infra_queue : 取流线程 → 发送线程 的缓冲
 *      infra_poll  : epoll 的薄封装
 *
 *  本模块是**把它们接起来的那一条线**:
 *
 *      监听 socket ──accept──▶ 客户端槽位
 *                                  │  收到字节
 *                                  ▼
 *                          累积缓冲 + 找 \r\n\r\n 边界     ← framing
 *                                  │  一个完整请求
 *                                  ▼
 *                          proto_rtsp_parse_request()
 *                                  │
 *                                  ▼
 *                          proto_rtsp_build_*()  →  send()
 *
 *  职责边界(刻意划清):
 *      ✅ 拥有监听 socket、epoll 实例、客户端槽位表、收发缓冲
 *      ✅ 处理 framing 与空闲超时
 *      ❌ **不构造 RTSP 报文的文本**(那是 proto_rtsp 的事)
 *      ❌ **不封装 RTP 包**(那是 proto_rtp 的事)
 *      ❌ **不碰 MPP / 摄像头**(那是 svc_media 的事)
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⭐ 四条"必须做对"的纪律(全部来自实测, 见 docs/问题与解决记录.md B017)
 * ─────────────────────────────────────────────────────────────────
 *  ① **每个响应都带 Content-Length**
 *     这条现在由 `proto_rtsp.c` 的唯一出口 `resp_finish()` 保证,
 *     本模块**不再自己拼报文** —— 拼报文的地方越多, 漏掉的可能性越大。
 *
 *  ② **framing: TCP 是字节流, 没有消息边界**
 *     一次 recv 可能只拿到**半个**请求, 也可能拿到**两个**。
 *     所以: 每客户端一个累积缓冲 → 循环找 `\r\n\r\n` → 处理一个 →
 *     **剩下的字节 memmove 往前搬, 绝不能丢**。
 *     📌 实测过的反面教材: 把两个请求粘在一起发, 只回了一个响应。
 *
 *  ③ **epoll 必须读到 `EAGAIN` 才算读完**
 *     epoll 默认是水平触发(level-triggered): 只要缓冲里还有数据就反复报告同一个 fd。
 *     所以 read 循环必须一直读到 `EAGAIN` 才退出; 否则事件循环会变成**忙等**。
 *
 *  ④ **空闲超时: 靠 `epoll_wait` 的超时当"内务心跳"**
 *     客户端可能**不发 TEARDOWN 就消失**(实测 ffprobe 就是),
 *     只靠 recv 返回 0 发现不了, 连接和会话会一直占着槽位。
 *     所以 `epoll_wait` 不能传 -1 死等, 要定期醒来清理。
 *
 * ─────────────────────────────────────────────────────────────────
 *  内存策略
 * ─────────────────────────────────────────────────────────────────
 *  "运行期不做动态分配"是本项目的规矩。本模块的做法是:
 *      · 客户端槽位表、每个槽位的读缓冲 → **start 时一次性分配**, stop 时释放
 *      · 事件循环里只做 memcpy / send / recv, **一次都不 malloc**
 *  这样既守住了"实时路径不分配"(避免堆碎片与不可预测的延迟),
 *  又不必把几 KB × N 的缓冲全塞进静态数组。
 *
 * @note 本模块**自己起一个线程**跑事件循环; start 返回后即可连接。
 * @note 线程安全: start/stop 不可与事件循环并发调用(由调用方保证);
 *       运行期的事件循环只碰自己的私有状态。
 */
#ifndef __SVC_NET_H__
#define __SVC_NET_H__

#include <netinet/in.h>     /* struct sockaddr_in —— on_play 要传出 RTP 目的地 */
#include <stddef.h>
#include <stdint.h>

#include "infra_netio.h"    /* infra_transport_t —— "传输方式"的唯一定义处 */

/** 最多同时接受多少个客户端。槽位在 start 时一次性分配 */
#define SVC_NET_MAX_CLIENTS 8

/** 每个客户端的读缓冲大小。RTSP 请求(含 User-Agent / Session 等头)远小于 1KB */
#define SVC_NET_READ_BUF_SIZE 2048

/** 客户端空闲多久(秒)就主动断开。<=0 表示不超时 */
#define SVC_NET_IDLE_TIMEOUT_SEC 15

/** 事件循环每次醒来的最长间隔(毫秒)—— 空闲超时检查的精度 */
#define SVC_NET_TICK_MS 1000

/** RTP 通道号(SETUP 协商时的默认值, 与 RFC 2326 §10.12 的常见约定一致) */
#define SVC_NET_RTP_CHANNEL  0
#define SVC_NET_RTCP_CHANNEL 1

/**
 * 服务端配置。全部字段在 svc_net_start() 里按值拷贝, 调用方不必保持有效。
 *
 * @note 之所以用"配置结构体 + start(cfg)"而不是一堆参数:
 *       M1-9 还要往里加参数集、队列等, 加参数不必改所有调用点。
 */
typedef struct {
    const char *listen_ip;      /**< 监听地址, 如 "0.0.0.0"; NULL = "0.0.0.0" */
    uint16_t    port;           /**< RTSP 端口, 通常 554; 测试用 8554 */
    int         backlog;        /**< listen 队列长度; <=0 用 8 */

    /**
     * DESCRIBE 要返回的 SDP 文本(**必须是纯文本, 不能带结尾 '\0' 之外的东西**)。
     * 由 proto_sdp_build() 生成。为 NULL 时 DESCRIBE 返回 500。
     */
    const char *sdp;
    size_t      sdp_len;        /**< SDP 字节数(必须准确, 它决定 Content-Length) */

    int         idle_timeout_sec;   /**< 空闲超时秒数; <=0 用 SVC_NET_IDLE_TIMEOUT_SEC */

    /**
     * 会话开始时被调用(PLAY 之后)。M1-9 用它把客户端挂到 RTP 发送路径上。
     *
     * @param client_index 客户端槽位下标(唯一标识一个客户端)
     * @param tr           **该客户端的传输方式**(UDP 目的地 / TCP 交错的 fd+通道号)。
     *                     这里**保证非 NULL**。上层据此决定建哪种 sender ——
     *                     ⚠️ 别只盯着 `tr->rtp_dst`:TCP 交错时它的端口是 **0**,
     *                     真正的目的地是 `tr->rtsp_fd` 那条连接。
     * @param user         `svc_net_cfg_t.user` 原样回传
     *
     * @note ⚠️ **为什么把目的地当参数传, 而不是让上层去查表**(2026-09-15):
     *       上层(发送线程)需要"往哪发"才能建 sender。有两个做法:
     *         ① 回调只给 index, 上层再调 `svc_net_get_rtp_dst(index)` 去查
     *         ② **回调直接把目的地按值给出来**(本做法)
     *       选 ② 的理由:目的地信息在 **PLAY 那一刻就已经完备**, 直接传出去
     *       **没有任何竞态**; 而 ① 会让上层"拿着 index 去查表", 这期间
     *       客户端可能已被空闲超时清掉 —— 那就是一个真实的竞态窗口。
     *       代价是回调签名改动(测试的假回调要同步改), 但换来的是"不可能用错"。
     *
     * @note 在事件循环线程里被调用, **不要在里面做阻塞操作**。
     *       建 sender / 起线程这类事应当只做"记录", 真正的发送在别的线程。
     */
    void      (*on_play)(int client_index, const infra_transport_t *tr,
                         void *user);
    /** 会话结束时被调用(TEARDOWN / 断开 / 超时)。 */
    void      (*on_teardown)(int client_index, void *user);
    void       *user;           /**< 原样传给上面两个回调 */
} svc_net_cfg_t;

/** 服务运行统计 —— 用于日志与验收断言 */
typedef struct {
    uint64_t conns_accepted;    /* 累计接受的连接数 */
    uint64_t conns_rejected;    /* 因槽位满被拒绝的连接数 */
    uint64_t conns_closed;      /* 累计关闭的连接数(含超时) */
    uint64_t conns_timeout;     /* 其中因空闲超时被关的 */
    uint64_t requests;          /* 累计处理的完整请求数 */
    uint64_t parse_errors;      /* 解析失败的请求数 */
    uint64_t oversize_drops;    /* 因请求超长被丢弃的连接数 */
    uint64_t responses_sent;    /* 累计发出的响应数 */
    uint64_t send_errors;       /* 发送失败的次数 */
} svc_net_stats_t;

/**
 * @brief 启动 RTSP 服务端(创建监听 socket + epoll + 事件循环线程)。
 *
 * @param cfg 配置; **不能为 NULL**, 且 sdp/sdp_len 必须自洽
 * @return 0 = 成功; 负值 = 失败:
 *         -1 = 参数非法 -2 = 监听 socket 建不起来(端口被占?)
 *         -3 = 内存不足 -4 = 线程创建失败
 *
 * @note 重复调用(已启动时再 start)返回 0 且不做任何事。
 * @note 阻塞: 只到"监听就绪 + 线程起来"为止, **不等客户端**。
 */
int svc_net_start(const svc_net_cfg_t *cfg);

/**
 * @brief 停止服务端: 关闭所有客户端 → 停止事件循环 → 释放全部资源。
 *
 * @note 阻塞: 会等事件循环线程真正退出(最多等到当前 tick 结束)。
 * @note 未启动时调用是安全的(no-op)。
 * @note 停止后可以再次 start。
 */
void svc_net_stop(void);

/** @brief 服务是否在运行。@return 1 = 在运行 */
int svc_net_is_running(void);

/** @brief 取实际监听的端口(端口传 0 时由内核分配, 用这个查询真实端口)。 */
uint16_t svc_net_port(void);

/** @brief 取统计快照。 */
void svc_net_get_stats(svc_net_stats_t *out);

/** @brief 有多少个槽位正在使用(用于验收断言"槽位没泄漏")。 */
int svc_net_client_count(void);

#endif /* __SVC_NET_H__ */
