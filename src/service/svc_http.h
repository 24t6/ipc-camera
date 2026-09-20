/**
 * @file    svc_http.h
 * @brief   回放服务(HTTP/1.1)—— 列分段 / 带 Range 取流 / 锁定解锁
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  【模块职责】起一个 HTTP 服务线程, 把"录在 TF 卡上的分段"提供给客户端回放
 *  【依赖方向】依赖 infra_netio / infra_log / proto_http / svc_record
 *             —— **不依赖 bsp**(不碰 MPP)、也不依赖 svc_sender/svc_net(RTSP 那条路)
 *  【线程模型】自己起 1 个线程(线程名 `ipc_http`);**串行处理**客户端
 *             (一次一个, 见下面的【简化上限】)
 *  【资源边界】无动态分配;文件级静态:请求缓冲 2KB + JSON 16KB + 分段表 ~5KB +
 *             取文件块 32KB ≈ 55KB
 *
 * ─────────────────────────────────────────────────────────────────
 *  对外接口(形状照"设备给 URI、客户端原样回填")
 * ─────────────────────────────────────────────────────────────────
 *      GET  /                       回放页面(浏览器直接当客户端用,**零安装**)
 *      GET  /help                   纯文本帮助(curl/终端友好)
 *      GET  /status                 **正在录**的那一段(名字/字节/帧数/起始时间)
 *      GET  /recent.mp4             "**刚录的这段**"立刻能看:请求录制线程把当前段收尾,
 *                                   然后把这**刚收尾的整段**发出去(支持 `Range`)
 *      GET  /playlist.m3u           整段回放列表(**旧→新**)—— VLC/mpv 打开它就能连着看
 *      GET  /recordings             列出现有分段(JSON; 每条带 uri 与 lock_uri)
 *      GET  /recordings/<名字>       取流;**支持 `Range: bytes=…`** ⇒ 206
 *      HEAD /recordings/<名字>       只取头(播放器常用来探大小)
 *      PUT  /recordings/<名字>/lock  body `1` = 锁定, `0` = 解锁(没 body 默认锁)
 *      OPTIONS *                    回 `Allow`(探能力)
 *
 *  ⇒ **"客户端"可以是浏览器**(页面就在板子上), 也可以是 VLC/mpv(播放列表),
 *    或者任何能发 HTTP 的程序 —— 服务端不假设对面是谁。
 *
 * ─────────────────────────────────────────────────────────────────
 *  【简化上限】为什么是"一次一个客户端 + 同步发送"
 * ─────────────────────────────────────────────────────────────────
 *  回放服务是**开发板上的演示功能**:局域网 1~2 个客户端、按需拖进度条。
 *  所以这里刻意用最直白的实现:阻塞 accept + 串行处理 + 同步发送。
 *  天花板很明确:
 *    · **一个"一直在下载"的客户端会占住整个服务** —— 它下 900 MB 分段时, 别人连列表都刷不出来
 *      (30 分钟段 ~900 MB, 千兆局域网约 80 秒);
 *    · 没有 keep-alive 复用(每个请求一条连接), 没有 chunked, 没有 TLS, 没有鉴权。
 *  ⚠️ 但"**客户端不读数据**"**不会**再卡死服务(2026-09-19 修的 B041):客户端 socket
 *     是**非阻塞**的, `send` 拿 `EAGAIN` 就 `poll` 等 100 ms, 等不到直接把这个连接丢掉。
 *     (修之前:阻塞 `send` 会在核里无限等 ⇒ 单线程服务永久卡死, 别人连不上。
 *      实测证据见 `svc_http.c` 的 `http_thread()` 注释。)
 *  ⚠️ **`GET /recent.mp4` 会额外阻塞一会儿**(最多 `HTTP_ROTATE_WAIT_MS` = 5 秒):
 *     它要等录制线程在下一个 IDR 处收尾当前段。这是**有意的**("看最新"本来就要等一次
 *     切段), 但它确实会占住这唯一的服务线程 —— 串行模型下的已知代价。
 *  **升级路径**(按代价从小到大):
 *    ① 每个客户端一个处理线程(改动最小, 但线程数不可控);
 *    ② 单线程非阻塞状态机(epoll + 每客户端一个发送游标, 用 `sendfile()` 零拷贝);
 *    ③ 上 TLS + 摘要鉴权(局域网演示不值当)。
 *  ⚠️ 鉴权这件事**明确写在文档里**:任何能连上 8080 的人都能下载全部录像、也能改锁定清单。
 *     本项目的定位是"局域网内的开发板演示", 不声称适合公网。
 *
 * @note 与 RTSP 那条路**完全分开**:它有自己的线程和 socket, 所以"有人回放"不会
 *       影响"有人看直播"(取流/发送/录制三条线程的实时性不被牵连)。
 * @note 阻塞式发送**只能在这个线程里做** —— 与 ADR-3 同一条理由。
 */
#ifndef __SVC_HTTP_H__
#define __SVC_HTTP_H__

#include <stdint.h>

/** 默认端口(与 RTSP 的 8554 分开) */
#define SVC_HTTP_DEFAULT_PORT 8080

/** HTTP 服务配置 */
typedef struct {
    const char *bind_ip;    /**< 绑定地址, 如 "0.0.0.0" */
    uint16_t    port;       /**< 端口; 0 = 用 `SVC_HTTP_DEFAULT_PORT` */
    uint16_t    live_port;  /**< **实时(MJPEG)服务的端口**;0 = 没有实时服务。
                             *   只用来在 `/status` 与回放页面里报出实时画面的 URL
                             *   (页面要显示 `<img src=...>`) —— 回放服务本身**不碰**
                             *   那个端口, 两个服务各跑各的线程。 */
} svc_http_cfg_t;

/** HTTP 服务统计(用于日志与验收) */
typedef struct {
    uint64_t conns;         /**< 接受过的连接数 */
    uint64_t requests;      /**< 处理过的请求数 */
    uint64_t lists;         /**< 列表请求数(`/recordings`) */
    uint64_t pages;         /**< 回放页面请求数(`/`) */
    uint64_t playlists;     /**< 播放列表请求数(`/playlist.m3u`) */
    uint64_t files;         /**< 文件请求数(含 206) */
    uint64_t partials;      /**< 206 次数 */
    uint64_t rotates;       /**< `/recent.mp4` 请求里**真的触发了切段**的次数 */
    uint64_t locks;         /**< 锁定/解锁次数 */
    uint64_t not_found;     /**< 404 次数 */
    uint64_t bad_requests;  /**< 400/416/431/505 次数 */
    uint64_t bytes_sent;    /**< 已发出的 body 字节数 */
} svc_http_stats_t;

/**
 * @brief 启动回放服务(建监听 socket + 起线程)
 *
 * @param[in] cfg 配置(会拷进内部, 调用方不必保持存活);NULL = 用默认值
 * @return 0 成功; 负值失败(端口被占 / 线程建不起来)
 *
 * @note 依赖 `svc_record` 已经初始化(要它的录制目录)—— 所以 `app_main` 里的
 *       启动顺序是"先录制、再 HTTP"。
 * @note 重复调用(已启动)返回 0, 不做任何事。
 */
int svc_http_start(const svc_http_cfg_t *cfg);

/** @brief 停止服务:通知线程退出 → join → 关监听 socket。未启动时是 no-op。 */
void svc_http_stop(void);

/** @brief 服务是否在运行。@return 1 = 在运行 */
int svc_http_is_running(void);

/** @brief 实际监听端口(配 0 时由内核选, 这个函数给出真实值)。 */
uint16_t svc_http_port(void);

/** @brief 取统计快照。 */
void svc_http_get_stats(svc_http_stats_t *out);

#endif /* __SVC_HTTP_H__ */
