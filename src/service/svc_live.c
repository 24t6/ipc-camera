/**
 * @file    svc_live.c
 * @brief   MJPEG 实时预览服务实现(单线程 poll + 一帧广播给所有客户端)
 *
 * 【模块职责】按需开 MJPEG 编码通道 → 取 JPEG → `multipart/x-mixed-replace` 推给浏览器
 * 【依赖方向】infra_netio / infra_log / proto_http / **bsp_mpp**(拿 MJPEG 帧);
 *             不依赖 svc_record(录像那条路)也不依赖 svc_http(回放那条路)
 * 【线程模型】自己起 1 个线程(`ipc_live`);**单线程 + poll** ——
 *             一帧只从编码器取一次, 然后依次写给所有客户端;客户端有界(4 个)
 * 【资源边界】无动态分配;文件级静态:一帧缓冲 192KB + 4 个客户端结构(每个只有 fd 与
 *             状态, **不缓存待发数据** —— 写不完就丢这个客户端, 见【简化上限】)
 */
#include "svc_live.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bsp_mpp.h"
#include "infra_log.h"
#include "infra_netio.h"
#include "proto_http.h"

/** 监听 backlog(实时画面客户端不会很多) */
#define LIVE_BACKLOG 4

/** 收请求头的最长等待(毫秒)—— 防"连上不发请求"把唯一的服务线程占住 */
#define LIVE_REQ_TIMEOUT_MS 1000

/** 请求缓冲(与回放服务同一口径) */
#define LIVE_REQ_BUF 2048

/** 一帧 JPEG 的缓冲:720p 约 55KB、1080p(回退方案)约 120KB ⇒ 192KB 留足余量 */
#define LIVE_FRAME_BUF (192 * 1024)

/** 给一个客户端写一"片"(分片头 + JPEG)最多等多久;等不到就丢它 */
#define LIVE_WRITE_WAIT_MS 200

/** 分片边界字符串(客户端按它切帧) */
#define LIVE_BOUNDARY "ipcframe"

/** 默认质量与帧率(实测 640x360/q80 ⇒ ~16KB/帧, 10fps ⇒ ~1.3Mbps) */
#define LIVE_DEFAULT_QFACTOR 80
#define LIVE_DEFAULT_FPS     10

/** 一个客户端槽(不缓存待发数据: 写不完这一片就丢连接) */
typedef struct {
    int  fd;                /* -1 = 空槽 */
    char peer[32];          /* 对端地址, 只用于日志 */
} live_slot_t;

static struct {
    int              running;
    int              stop_requested;
    pthread_t        thread;
    int              thread_valid;
    int              listen_fd;
    uint16_t         port;
    char             bind_ip[32];
    int              qfactor;
    int              fps;
    int              clients;
    live_slot_t      slot[SVC_LIVE_MAX_CLIENTS];
    svc_live_stats_t stats;

    /* 只归本线程用 */
    char    req[LIVE_REQ_BUF];
    uint8_t frame[LIVE_FRAME_BUF];      /* 一帧 JPEG(取一次, 广播给所有人) */
} g;

/* ─────────── 小工具 ─────────── */

/**
 * @brief 单调毫秒时钟(排帧用;板子没有 RTC)
 */
static long long now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * @brief 发 MJPEG 的响应头(之后就是一片接一片的 JPEG)
 *
 * @param[in] cfd 连接(**非阻塞**)
 * @return 0 成功; -1 失败
 *
 * @note `multipart/x-mixed-replace` 是**浏览器原生支持**的"动图"格式
 *       (老式 IP 摄像机全用它):每个分片是一张完整 JPEG, 客户端收到就替换上一张。
 *       ⇒ 不需要 JS 解码、不需要 WebSocket、不需要 HLS。
 */
static int send_stream_head(int cfd)
{
    char head[256];
    int  n = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: multipart/x-mixed-replace; "
                      "boundary=" LIVE_BOUNDARY "\r\n"
                      "Cache-Control: no-store\r\n"
                      "Connection: close\r\n"
                      "\r\n");

    if (n <= 0 || n >= (int)sizeof(head)) {
        return -1;
    }
    return infra_tcp_write_all(cfd, head, (size_t)n);
}

/**
 * @brief 回一个"没有 body"的简单应答(用于拒绝连接)
 *
 * @param[in] cfd    连接
 * @param[in] status 例如 "400 Bad Request"
 * @return 0 成功; -1 失败
 */
static int send_simple(int cfd, const char *status)
{
    char head[96];
    int  n = snprintf(head, sizeof(head),
                      "HTTP/1.1 %s\r\nContent-Length: 0\r\n"
                      "Connection: close\r\n\r\n", status);

    if (n <= 0 || n >= (int)sizeof(head)) {
        return -1;
    }
    return infra_tcp_write_all(cfd, head, (size_t)n);
}

/**
 * @brief 给一个客户端写一整片(分片头 + JPEG)
 *
 * @param[in]  fd   连接(非阻塞)
 * @param[in]  jpeg 帧数据
 * @param[in]  len  帧长度
 * @param[out] sent 实际发给这个客户端的字节数(含分片头)
 * @return 0 成功; -1 失败(调用方应当丢掉这个客户端)
 *
 * @note 【简化上限】**没有"待发缓冲"**:写不完(客户端太慢)就由调用方丢掉它。
 *       天花板: 客户端瞬间抖动超过 `LIVE_WRITE_WAIT_MS` 就会被断开(浏览器会自动重连)。
 *       升级路径: 每个槽加一个固定大小的待发环形缓冲 + `POLLOUT` 续写 ——
 *       代价是每个客户端 ~64KB 静态内存与一套"写游标"状态机, 现在不值当。
 */
static int send_part(int fd, const uint8_t *jpeg, size_t len, size_t *sent)
{
    char   ph[128];
    int    n = snprintf(ph, sizeof(ph),
                        "\r\n--" LIVE_BOUNDARY "\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: %zu\r\n\r\n", len);
    size_t off = 0;
    long long t0 = now_ms();

    *sent = 0;
    if (n <= 0 || n >= (int)sizeof(ph)) {
        return -1;
    }
    if (infra_tcp_write_all(fd, ph, (size_t)n) != 0) {
        LOG_WARN("实时: 发分片头失败(errno=%d %s)", errno, strerror(errno));
        return -1;
    }
    while (off < len) {
        ssize_t w = send(fd, jpeg + off, len - off, MSG_NOSIGNAL);

        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)
            && now_ms() - t0 < LIVE_WRITE_WAIT_MS) {
            struct pollfd p;

            p.fd      = fd;
            p.events  = POLLOUT;
            p.revents = 0;
            (void)poll(&p, 1, 20);
            continue;
        }
        /* ⚠️ 把 errno 与进度打出来 —— "客户端太慢"和"客户端已经走了"要能分开,
         *    否则排查时只能猜(B032 的教训: 讲不通的因果就是没找到因)。 */
        LOG_WARN("实时: 写帧失败(errno=%d %s, 已发 %zu/%zu 字节, 等了 %lld ms)",
                 errno, strerror(errno), off, len, now_ms() - t0);
        return -1;                      /* 写不动/出错 ⇒ 让调用方丢连接 */
    }
    *sent = (size_t)n + len;
    return 0;
}

/* ─────────── 客户端槽 ─────────── */

/** @brief 找一个空槽。@return 下标; -1 = 满了 */
static int slot_alloc(void)
{
    int i;

    for (i = 0; i < SVC_LIVE_MAX_CLIENTS; i++) {
        if (g.slot[i].fd < 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 丢掉一个客户端(关 fd + 记日志 + 清槽)
 *
 * @param[in] i     槽下标
 * @param[in] why   原因(日志用)
 */
static void slot_drop(int i, const char *why)
{
    if (g.slot[i].fd < 0) {
        return;
    }
    (void)shutdown(g.slot[i].fd, SHUT_RDWR);    /* 打断可能卡住的写 */
    (void)close(g.slot[i].fd);
    g.slot[i].fd = -1;
    g.clients--;
    g.stats.dropped++;
    LOG_INFO("实时: 断开客户端 %s(%s), 还剩 %d 个", g.slot[i].peer, why,
             g.clients);

    /* ★ 最后一个客户端走了 ⇒ **拆掉 MJPEG 通道**(MPP 回到"只有主路") */
    if (g.clients <= 0) {
        g.clients = 0;
        bsp_mpp_mjpeg_close();
    }
}

/**
 * @brief 收全一个请求头(有界 + 有超时)
 *
 * @param[in]  cfd 连接(非阻塞)
 * @param[out] out 输出缓冲
 * @param[in]  cap 容量
 * @return 收到多少个字节; <=0 = 失败/超时
 *
 * @note ⚠️ **不能阻塞**:这个线程同时在给大家推 MJPEG 帧。所以是"poll + 非阻塞 recv +
 *       总超时"(与 B041 同一条思路: 对端不按剧本走时, 必须能被丢掉)。
 */
static int read_request(int cfd, char *out, size_t cap)
{
    size_t    len = 0;
    long long t0 = now_ms();

    while (len + 1 < cap) {
        struct pollfd p;
        ssize_t       n;

        if (now_ms() - t0 > LIVE_REQ_TIMEOUT_MS) {
            return -1;                  /* 连上不发请求: 丢掉 */
        }
        n = recv(cfd, out + len, cap - 1 - len, 0);
        if (n > 0) {
            len += (size_t)n;
            out[len] = '\0';
            if (strstr(out, "\r\n\r\n") != NULL) {
                return (int)len;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            p.fd      = cfd;
            p.events  = POLLIN;
            p.revents = 0;
            (void)poll(&p, 1, 50);
            continue;
        }
        return -1;                      /* EOF 或错误 */
    }
    return -1;                          /* 头太长 */
}

/**
 * @brief 判一个请求是不是"要看实时画面"
 *
 * @param[in] cfd 连接(非阻塞)
 * @return 0 = 可以开流; -1 = 已经回过错误应答(调用方只需关连接)
 *
 * @note 抽出来是为了让 `accept_client()` 不超"代码行 ≤ 50"(§9.1)。
 */
static int request_wants_live(int cfd)
{
    char                 path[PROTO_HTTP_TARGET_MAX];
    proto_http_request_t req;
    int                  n = read_request(cfd, g.req, sizeof(g.req));

    if (n <= 0 || proto_http_parse(g.req, (size_t)n, &req) <= 0) {
        (void)send_simple(cfd, "400 Bad Request");
        return -1;
    }
    if (proto_http_path(req.target, path, sizeof(path)) < 0) {
        (void)send_simple(cfd, "400 Bad Request");
        return -1;
    }
    if (req.method != PROTO_HTTP_GET) {
        (void)send_simple(cfd, "405 Method Not Allowed");
        return -1;
    }
    if (strcmp(path, "/live.mjpg") != 0 && strcmp(path, "/live") != 0
        && strcmp(path, "/stream") != 0) {
        (void)send_simple(cfd, "404 Not Found");
        LOG_INFO("实时: 不认识这个路径: %s", path);
        return -1;
    }
    return 0;
}

/**
 * @brief 收下一个客户端:读请求 → 判路径 → 发流头 → 占一个槽(第一个客户端开通道)
 *
 * @return 0 已接入; -1 拒绝(已回错误应答并关闭)
 */
static int accept_client(void)
{
    struct sockaddr_in peer;
    socklen_t          plen = sizeof(peer);
    int                cfd;
    int                i;

    cfd = accept(g.listen_fd, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) {
        return -1;
    }
    (void)infra_set_nonblocking(cfd);
    g.stats.conns++;

    if (request_wants_live(cfd) != 0) {
        (void)close(cfd);
        g.stats.dropped++;
        return -1;
    }
    i = slot_alloc();
    if (i < 0) {
        LOG_WARN("实时: 客户端已满(%d 个), 拒绝新连接", SVC_LIVE_MAX_CLIENTS);
        (void)send_simple(cfd, "503 Service Unavailable");
        (void)close(cfd);
        g.stats.dropped++;
        return -1;
    }
    /* ★ 第一个客户端 ⇒ 开 MJPEG 通道(按需启停;见文件头 A22 的说明) */
    if (g.clients == 0 && bsp_mpp_mjpeg_open(g.qfactor, g.fps) != 0) {
        LOG_ERROR("实时: MJPEG 通道开不起来, 拒绝这个客户端");
        (void)send_simple(cfd, "503 Service Unavailable");
        (void)close(cfd);
        return -1;
    }
    if (send_stream_head(cfd) != 0) {
        (void)close(cfd);
        if (g.clients == 0) {
            bsp_mpp_mjpeg_close();
        }
        return -1;
    }
    g.slot[i].fd = cfd;
    snprintf(g.slot[i].peer, sizeof(g.slot[i].peer), "%s:%u",
             inet_ntoa(peer.sin_addr), (unsigned)ntohs(peer.sin_port));
    g.clients++;
    LOG_INFO("实时: 新客户端 %s(共 %d 个, %s)", g.slot[i].peer, g.clients,
             (g.clients == 1) ? "已按需开启 MJPEG 通道" : "复用已开的通道");
    return 0;
}

/* ─────────── 主循环 ─────────── */

/**
 * @brief 取一帧 JPEG 并广播给所有客户端(一帧只取一次)
 */
static void broadcast_frame(void)
{
    size_t len = 0;
    int    rc = bsp_mpp_mjpeg_get_frame(g.frame, sizeof(g.frame), &len, 0);
    int    i;

    if (rc == 0) {
        return;                         /* 这一刻还没编好 */
    }
    if (rc < 0) {
        g.stats.frame_errors++;
        LOG_WARN("实时: 取 JPEG 帧失败(第 %llu 次)",
                 (unsigned long long)g.stats.frame_errors);
        return;
    }
    for (i = 0; i < SVC_LIVE_MAX_CLIENTS; i++) {
        size_t sent = 0;

        if (g.slot[i].fd < 0) {
            continue;
        }
        if (send_part(g.slot[i].fd, g.frame, len, &sent) != 0) {
            slot_drop(i, "写不动了(客户端太慢/已断开)");
        } else {
            g.stats.bytes += sent;
        }
    }
    g.stats.frames++;
}

/**
 * @brief 组 pollfd 集合 + 算出这次该等多久 + poll
 *
 * @param[out] p      pollfd 数组(容量 `SVC_LIVE_MAX_CLIENTS + 1`)
 * @param[out] nfds   实际个数
 * @param[in]  interval 一个帧周期(毫秒)
 * @param[in]  next_frame 下一帧的到点时刻(单调毫秒)
 * @return poll 的返回值(>0 有事件; 0 超时; <0 出错)
 *
 * @note 抽出来是为了让 `live_thread()` 不超"代码行 ≤ 50"(§9.1 对策①)。
 * @note ⚠️ **没客户端时别空转**: `next_frame` 是过去时刻, 直接用它算会得到 0 超时,
 *       于是 `poll` 立刻返回、循环狂转(一个纯粹的 CPU 浪费)。所以没人看时按一个
 *       帧周期等 —— 这也让新连接最多等一个周期就被 accept。
 */
static int wait_events(struct pollfd *p, int *nfds, int interval,
                       long long next_frame)
{
    long long wait;
    int       i;

    *nfds       = 1;
    p[0].fd     = g.listen_fd;
    p[0].events = POLLIN;
    p[0].revents = 0;
    for (i = 0; i < SVC_LIVE_MAX_CLIENTS; i++) {
        if (g.slot[i].fd >= 0) {
            p[*nfds].fd      = g.slot[i].fd;
            p[*nfds].events  = POLLIN;
            p[*nfds].revents = 0;
            (*nfds)++;
        }
    }
    wait = next_frame - now_ms();
    if (g.clients <= 0) {
        wait = interval;
    } else if (wait < 0) {
        wait = 0;
    }
    if (wait > interval) {
        wait = interval;
    }
    return poll(p, (nfds_t)(*nfds), (int)wait);
}

/**
 * @brief 检查客户端有没有"不该有的动静"(断开 / 请求之后又发数据) —— 有就丢掉它
 *
 * @param[in] p    pollfd 数组
 * @param[in] nfds 个数
 *
 * @note 实时流是**单向**的: 请求之后客户端不该再说话。真说了(或断开)就丢。
 *       ⚠️ 这一步不能省: 不丢掉的死连接会一直占着槽位与 fd。
 */
static void drop_dead_clients(const struct pollfd *p, int nfds)
{
    int i;

    for (i = 0; i < SVC_LIVE_MAX_CLIENTS; i++) {
        int k;

        if (g.slot[i].fd < 0) {
            continue;
        }
        for (k = 1; k < nfds; k++) {
            char    junk[64];
            ssize_t n;

            if (p[k].fd != g.slot[i].fd) {
                continue;
            }
            if (!(p[k].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            n = recv(g.slot[i].fd, junk, sizeof(junk), 0);
            if (n == 0) {
                slot_drop(i, "对端关闭");
            } else if (n > 0) {
                slot_drop(i, "请求之后又发了数据");
            }
        }
    }
}

/**
 * @brief 实时服务线程:单线程 poll, 一帧广播给所有客户端
 *
 * @note 循环三件事: ①新连接 ②客户端异常 ③到点就取一帧广播出去。
 *       **任何一步都不许阻塞** —— 这个线程同时在给所有客户端供帧。
 */
static void *live_thread(void *arg)
{
    int       interval = 1000 / ((g.fps > 0) ? g.fps : LIVE_DEFAULT_FPS);
    long long next_frame = 0;
    int       i;

    (void)arg;
    (void)prctl(PR_SET_NAME, "ipc_live", 0, 0, 0);
    LOG_INFO("实时服务线程启动(端口 %u, %d fps, q=%d)", (unsigned)g.port,
             g.fps, g.qfactor);

    while (!g.stop_requested) {
        struct pollfd p[SVC_LIVE_MAX_CLIENTS + 1];
        int           nfds = 0;

        if (wait_events(p, &nfds, interval, next_frame) > 0
            && (p[0].revents & POLLIN) != 0) {
            (void)accept_client();
        }
        drop_dead_clients(p, nfds);
        if (g.clients > 0 && now_ms() >= next_frame) {
            next_frame = now_ms() + interval;
            broadcast_frame();
        }
    }
    for (i = 0; i < SVC_LIVE_MAX_CLIENTS; i++) {
        if (g.slot[i].fd >= 0) {
            (void)close(g.slot[i].fd);
            g.slot[i].fd = -1;
        }
    }
    g.clients = 0;
    bsp_mpp_mjpeg_close();              /* 线程退出前一定把通道关掉 */
    LOG_INFO("实时服务线程退出(连接 %llu / 帧 %llu / 丢客户端 %llu)",
             (unsigned long long)g.stats.conns,
             (unsigned long long)g.stats.frames,
             (unsigned long long)g.stats.dropped);
    return NULL;
}

/* ─────────── 对外接口 ─────────── */

int svc_live_start(const svc_live_cfg_t *cfg)
{
    int i;

    if (g.running) {
        return 0;
    }
    memset(&g.stats, 0, sizeof(g.stats));
    snprintf(g.bind_ip, sizeof(g.bind_ip), "%s",
             (cfg != NULL && cfg->bind_ip != NULL) ? cfg->bind_ip : "0.0.0.0");
    g.port    = (cfg != NULL && cfg->port != 0) ? cfg->port : SVC_LIVE_DEFAULT_PORT;
    g.qfactor = (cfg != NULL && cfg->qfactor > 0) ? cfg->qfactor
                                                  : LIVE_DEFAULT_QFACTOR;
    g.fps     = (cfg != NULL && cfg->fps > 0) ? cfg->fps : LIVE_DEFAULT_FPS;
    g.stop_requested = 0;
    for (i = 0; i < SVC_LIVE_MAX_CLIENTS; i++) {
        g.slot[i].fd = -1;
    }
    g.clients   = 0;
    g.listen_fd = infra_tcp_listen(g.bind_ip, g.port, LIVE_BACKLOG);
    if (g.listen_fd < 0) {
        LOG_ERROR("实时: 端口 %u 监听失败: %s", (unsigned)g.port, strerror(errno));
        return -1;
    }
    if (pthread_create(&g.thread, NULL, live_thread, NULL) != 0) {
        LOG_ERROR("实时: 线程创建失败");
        infra_close(&g.listen_fd);
        return -2;
    }
    g.thread_valid = 1;
    g.running      = 1;
    return 0;
}

void svc_live_stop(void)
{
    if (!g.running) {
        bsp_mpp_mjpeg_close();          /* 万一有残留, 幂等收尾 */
        return;
    }
    g.stop_requested = 1;
    /* 打断 accept 与所有在途写(与 B041 的正解同一条: 停之前先 shutdown) */
    if (g.listen_fd >= 0) {
        (void)shutdown(g.listen_fd, SHUT_RDWR);
    }
    if (g.thread_valid) {
        (void)pthread_join(g.thread, NULL);
        g.thread_valid = 0;
    }
    infra_close(&g.listen_fd);
    bsp_mpp_mjpeg_close();              /* 双保险: 线程里已经关过, 这里幂等 */
    g.running = 0;
    LOG_INFO("实时服务已停止(帧 %llu / 发出 %llu 字节)",
             (unsigned long long)g.stats.frames,
             (unsigned long long)g.stats.bytes);
}

int svc_live_is_running(void)
{
    return g.running ? 1 : 0;
}

uint16_t svc_live_port(void)
{
    return g.port;
}

void svc_live_get_stats(svc_live_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = g.stats;
    out->clients = g.clients;
}
