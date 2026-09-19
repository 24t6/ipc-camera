/**
 * @file    infra_netio.h
 * @brief   socket 封装 + 发送抽象 —— 让协议层不关心底层是 UDP 还是 TCP
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  为什么要有 infra_sender_t(见 ARCHITECTURE.md 的 ADR-1)
 * ─────────────────────────────────────────────────────────────────
 *  ADR-1 的决策是:**先实现 UDP, 但为 TCP interleaved 预留接口**。
 *  做法就是把"发送"抽象成一个函数指针 + 上下文:
 *
 *      infra_sender_t *s = infra_sender_udp(&dst, fd);
 *      s->send(s->ctx, buf, len);        ← 调用方不关心底层是什么
 *      s->destroy(s);                     ← 归还资源
 *
 *  这样将来加 TCP interleaved,**只需新增一个实现, proto_rtp.c 一行都不用改**。
 *  另外单测时可以注入"计数 sender", 不真发网络也能验证发了多少字节。
 *
 * ─────────────────────────────────────────────────────────────────
 *  两种实现
 * ─────────────────────────────────────────────────────────────────
 *  ① UDP: 每包一次 sendto()。无状态, 最省事, 是项目当前的主路径。
 *  ② TCP interleaved(RFC 2326 §10.12): 把 RTP 包塞进 RTSP 那条 TCP 连接,
 *     用 4 字节前缀分帧:
 *          [0x24]['$'][channel][2 字节大端长度][RTP 包]
 *     好处是能穿防火墙(只需一个端口); 坏处是**队头阻塞**(见第 3 课)。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 一个刻意破例的地方, 必须写清楚
 * ─────────────────────────────────────────────────────────────────
 *  本项目规矩是"运行期不做动态分配"。但 `infra_sender_tcp_interleaved()`
 *  需要在**创建时 malloc 一个发送缓冲**:
 *      · 必须一次送出完整帧(否则多线程交错会破坏 TCP 流), 所以要先拼好
 *      · 缓冲若放栈上, 就不能跨函数存活; 放静态又会让多客户端互相踩
 *  所以这里**创建期分配一次、销毁时释放**, 运行期不再分配 —— 不违反规矩的本意。
 *  单独把这条列出来, 是为了面试被追问"你不是说不 malloc 吗"时能答上来。
 *
 * @note 本模块只做系统调用封装, 不解析任何协议内容。
 * @note 除创建/销毁外, 发送函数均**可重入**(UDP 无状态; TCP 用各自缓冲)。
 */
#ifndef __INFRA_NETIO_H__
#define __INFRA_NETIO_H__

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* ─────────────────── 发送抽象 ─────────────────── */

/** 发送抽象: 一个函数指针 + 上下文。见文件头说明。 */
typedef struct infra_sender infra_sender_t;

struct infra_sender {
    /** 发送一段数据。@return 实际发送字节数; -1 = 出错 */
    int  (*send)(void *ctx, const void *buf, size_t len);
    /**
     * 释放这个 sender(连同内部上下文与缓冲), 并把 `*ps` 置 NULL。
     *
     * @param ps 指向 sender 指针的指针 —— 用法: `s->destroy(&s);`
     *
     * @note 之所以收 `infra_sender_t **` 而不是 `void *ctx`:
     *       调用者手上拿的是 `s`, 如果让它自己传 `ctx` 极易传错
     *       (本项目就真的踩过: 传了 `s` 进去导致 free 野指针)。
     *       收二级指针后, destroy 一次性负责"释放 ctx + 释放 s + 置空",
     *       调用方不可能用错, 也不会漏掉释放 sender 本身。
     */
    void (*destroy)(infra_sender_t **ps);
    void  *ctx;
};

/**
 * 一个客户端的**传输方式** —— 由 `svc_net` 在 PLAY 时给出, 决定用哪种 sender。
 *
 * @note 为什么要这么个结构(2026-09-18 修 bug 时加的): 原来 `svc_net` 只传出
 *       "RTP 目的地", 于是 `svc_sender` **无论 SETUP 协商成什么都建 UDP sender** ——
 *       TCP 交错那条实现(`infra_sender_tcp_interleaved()`)写好了却**没人调用**,
 *       症状是"客户端勾了 TCP 播放就一帧都发不出去"(日志里 RTP 端口是 0)。
 *       把"传输方式"显式传出来, 这类"**写好了没接线**"的病才不会再犯。
 * @note 放在 infra 层是因为 **`svc_net` 与 `svc_sender` 是平级模块**,
 *       谁都不该 include 对方的头; 而"传输方式"本来就属于发送/接收抽象这一层。
 */
typedef struct {
    int                is_tcp;       /**< 1 = RTP over TCP 交错(RFC 2326 §10.12); 0 = UDP */
    int                rtsp_fd;      /**< is_tcp 时: RTSP 那条 TCP 的 fd(交错帧写在它上面) */
    uint8_t            rtp_channel;  /**< is_tcp 时: SETUP 协商的 RTP 交错通道号 */
    struct sockaddr_in rtp_dst;      /**< is_tcp=0 时: UDP 目的地(IP + 客户端 RTP 端口) */
} infra_transport_t;

/**
 * @brief 创建一个 UDP sender(包到包一次 sendto)。
 *
 * @param dst    目标地址(会**按值拷贝**进 ctx, 调用方不必保持有效)
 * @param sockfd 已创建的 UDP socket
 * @return sender; 失败返回 NULL
 *
 * @note 不接管 sockfd 的所有权 —— 销毁 sender **不会**关闭 socket。
 */

infra_sender_t *infra_sender_udp(const struct sockaddr_in *dst, int sockfd);

/**
 * @brief 创建一个 RTP-over-TCP 交错 sender(RFC 2326 §10.12)。
 *
 * @param fd          已连接的 TCP socket(通常是 RTSP 那条连接)
 * @param rtp_channel 交错通道号(SETUP 时协商, 通常 RTP=0 / RTCP=1)
 * @return sender; 失败返回 NULL
 *
 * @note 创建期会分配一个发送缓冲(见文件头说明), 销毁时释放。
 * @note 不接管 fd 的所有权 —— 销毁 sender **不会**关闭 socket。
 * @note ⚠️ 会阻塞: TCP 发送缓冲满时 send() 会等待。**不要在取流线程里调用**
 *       (正是 ADR-3 要避免的事)。
 */
infra_sender_t *infra_sender_tcp_interleaved(int fd, uint8_t rtp_channel);

/**
 * @brief 发送统计 —— 用于"发了多少、失败多少"的日志与单测。
 *
 * @param s            sender
 * @param out_bytes    输出: 累计发送字节数(可为 NULL)
 * @param out_packets  输出: 累计发送次数(可为 NULL)
 * @param out_errors   输出: 累计失败次数(可为 NULL)
 *
 * @note UDP 与 TCP 实现都支持; 传 NULL 的项不会被写。
 */
void infra_sender_stats(const infra_sender_t *s, uint64_t *out_bytes,
                        uint64_t *out_packets, uint64_t *out_errors);

/* ─────────────────── socket 助手 ─────────────────── */

/**
 * @brief 创建并绑定一个 TCP 监听 socket。
 *
 * @param ip       绑定地址(如 "0.0.0.0")
 * @param port     端口(如 554)
 * @param backlog  listen 队列长度
 * @return socket fd; 失败返回 -1
 *
 * @note 会设置 SO_REUSEADDR —— 否则程序重启后 554 端口会"TIME_WAIT 占用"
 *       而绑不上, 开发期反复重启时非常烦。
 */
int infra_tcp_listen(const char *ip, uint16_t port, int backlog);

/** @brief 创建并绑定一个 UDP socket(端口传 0 表示由内核选)。@return fd; 失败 -1 */
int infra_udp_bind(const char *ip, uint16_t port);

/** @brief 把 socket 设为非阻塞。@return 0 成功, -1 失败 */
int infra_set_nonblocking(int fd);

/**
 * @brief 把一段数据**写完**(处理 `EINTR` / `EAGAIN`, 必要时 `poll` 等可写)
 *
 * @param[in] fd  已连接的 socket
 * @param[in] buf 数据
 * @param[in] len 字节数
 * @return 0 = 全部写完; -1 = 失败(等可写超时 / 对端关闭 / 其它错误)
 *
 * @note ★ **唯一的"写满一个 TCP 流"实现**:TCP 交错发送器、RTSP 响应、
 *       HTTP 回放服务都走这里 —— 三处各写一遍的话, 改一处忘两处就是 bug
 *       (本项目已经因为"两份实现"栽过:见 B036)。
 * @note ⚠️ **会阻塞**(最长 poll 100 ms 一轮)。只允许在**网络线程 / HTTP 线程**里调用;
 *       取流线程里绝不能调 —— 那是 ADR-3 要防的"网络慢拖死取流"。
 * @note 失败时把 `errno` 与已发字节数打进日志, 但**只打前 3 次**(免得刷屏)。
 */
int infra_tcp_write_all(int fd, const void *buf, size_t len);

/** @brief 关闭 socket 并置 -1(避免野句柄重复关闭)。 */
void infra_close(int *fd);

#endif /* __INFRA_NETIO_H__ */
