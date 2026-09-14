/**
 * @file    svc_net.c
 * @brief   RTSP 服务端实现 —— 事件循环 + 客户端会话管理 (M1-8)
 *
 * 结构(刻意按"一层一件事"切):
 *      ① 客户端槽位: 分配 / 释放
 *      ② 收字节 + framing(找完整请求)
 *      ③ 请求处理: 交给 proto_rtsp, 把响应发回去
 *      ④ 事件循环: accept / 读 / 内务超时
 *      ⑤ 生命周期: start / stop / 统计
 *
 * 设计取舍:
 *      · **本文件不拼任何 RTSP 报文文本** —— 全部走 proto_rtsp_build_*()。
 *        理由见 svc_net.h 纪律①; 也正因为如此, 这里零个 "RTSP/1.0" 字面量。
 *      · 单线程事件循环(不为每客户端起线程): 客户端数量少(<=8), 请求是
 *        "低频控制命令", 单线程完全够; 而且省掉一大堆加锁。
 *      · 运行期不 malloc: 槽位和读缓冲都在 start 时一次分好。
 */
#include "svc_net.h"

#include "infra_log.h"
#include "infra_netio.h"
#include "infra_poll.h"
#include "proto_rtsp.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/** 拼一个响应要用的栈缓冲。RTSP 响应(含 SDP)都很小, 4KB 足够 */
#define SVC_NET_RESP_BUF_SIZE 4096

/* ═══════════════ ① 客户端槽位 ═══════════════ */

/**
 * 一个客户端连接。
 *
 * @note `rbuf` 是**累积缓冲**: 收到的字节先囤在这里, 直到能拼出完整请求。
 *       这是纪律②(framing)的物理基础。
 */
typedef struct {
    int      fd;                        /* -1 = 槽位空闲 */
    size_t   rlen;                      /* rbuf 里已累积的字节数 */
    char     rbuf[SVC_NET_READ_BUF_SIZE];

    int      session_assigned;          /* SETUP 之后为 1 */
    uint32_t session_id;
    int      playing;                   /* PLAY 之后为 1 */
    uint8_t  rtp_channel;               /* SETUP 协商结果(TCP 交错用) */
    uint8_t  rtcp_channel;

    time_t   last_active;               /* 最后一次收到数据的时刻 */
} svc_net_client_t;

/** 事件来源分类 —— epoll 只给 fd, 我们要知道"这个 fd 是谁" */
typedef enum {
    EV_NONE = 0,
    EV_LISTEN,              /* 监听 socket: 有新连接 */
    EV_CLIENT,              /* 某个客户端连接: 有数据 */
} ev_kind_t;

/** epoll 事件的自定义数据 */
typedef struct {
    ev_kind_t kind;
    int       index;        /* EV_CLIENT 时是槽位下标 */
} ev_data_t;

/* ═══════════════ 模块状态(单实例) ═══════════════ */

static struct {
    int                 running;
    int                 listen_fd;
    uint16_t            port;               /* 实际监听端口 */
    infra_poller_t     *poller;
    pthread_t           thread;
    int                 thread_valid;
    int                 stop_requested;

    svc_net_client_t   *clients;            /* start 时分配 SVC_NET_MAX_CLIENTS 个 */
    char               *sdp;                /* start 时拷贝一份 */
    size_t              sdp_len;

    int                 idle_timeout_sec;
    void              (*on_play)(int, void *);
    void              (*on_teardown)(int, void *);
    void               *user;

    svc_net_stats_t     stats;
} g;

/* ═══════════════ 小工具 ═══════════════ */

/** 找槽位下标; 找不到返回 -1 */
static int client_find(int fd)
{
    int i;

    for (i = 0; i < SVC_NET_MAX_CLIENTS; i++) {
        if (g.clients[i].fd == fd)
            return i;
    }
    return -1;
}

/**
 * 找一个空槽位并初始化。找不到返回 -1(客户端满了)。
 * @note 分配时清零 → "分配即初始状态", 不留上一次的残留。
 */
static int client_alloc(int fd)
{
    int i;

    for (i = 0; i < SVC_NET_MAX_CLIENTS; i++) {
        if (g.clients[i].fd == -1) {
            svc_net_client_t *c = &g.clients[i];

            memset(c, 0, sizeof(*c));
            c->fd          = fd;
            c->last_active = time(NULL);
            return i;
        }
    }
    return -1;
}

/**
 * 释放槽位: 从 epoll 摘掉 → 关 fd → 标空闲 → 通知上层。
 *
 * @note 关 fd 之前必须先 epoll_ctl(DEL): 否则 fd 号被下一个连接复用时,
 *       epoll 里还留着旧的注册项, 会把新连接的事件张冠李戴。
 */
static void client_free(int idx)
{
    svc_net_client_t *c;

    if (idx < 0 || idx >= SVC_NET_MAX_CLIENTS)
        return;
    c = &g.clients[idx];
    if (c->fd == -1)
        return;

    if (g.poller != NULL)
        infra_poller_del(g.poller, c->fd);

    if (g.on_teardown != NULL)
        g.on_teardown(idx, g.user);

    infra_close(&c->fd);
    c->rlen            = 0;
    c->playing         = 0;
    c->session_assigned = 0;
    g.stats.conns_closed++;
}

/** 数当前用掉的槽位数 */
static int client_count(void)
{
    int i, n = 0;

    for (i = 0; i < SVC_NET_MAX_CLIENTS; i++) {
        if (g.clients[i].fd != -1)
            n++;
    }
    return n;
}

/**
 * 把 len 字节完整发出去。
 *
 * @return 0 成功; -1 失败(**调用方必须关掉这个连接**)
 *
 * @note 阻塞(最多被**本进程自己的** TCP 发送缓冲限制住)。RTSP 响应都很小
 *       (最大的是 DESCRIBE 的 SDP, 几百字节), 所以这里阻塞是可以接受的。
 *       ⚠️ M1-9 之后 **RTP 媒体数据绝不能走这条路** —— 那才是 ADR-3 要防的
 *       "网络慢拖死取流线程", 媒体数据必须走队列 + 发送线程。
 */
static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* 发送缓冲满: 短暂让出 CPU 再试。响应很小, 不会转很久 */
            struct timespec ts = { 0, 1000000 };    /* 1 ms */

            nanosleep(&ts, NULL);
            continue;
        }
        return -1;
    }
    g.stats.responses_sent++;
    return 0;
}

/* ═══════════════ ③ 请求处理 ═══════════════ */

/**
 * 处理一个**完整的** RTSP 请求并回应。
 *
 * @param req_buf 请求文本(已被截断到边界处, 保证以 '\0' 结尾)
 * @param req_len 请求字节数
 * @return 0 已回应; -1 = 连接应当关闭(发送失败或不可恢复)
 *
 * @note 本函数**不构造任何报文文本**, 只做"解析 → 分派 → 发送"。
 */
static int handle_request(svc_net_client_t *c, const char *req_buf, size_t req_len)
{
    proto_rtsp_request_t req;
    char                 resp[SVC_NET_RESP_BUF_SIZE];
    int                  n = -1;
    int                  rc;

    rc = proto_rtsp_parse_request(req_buf, req_len, &req);
    if (rc != 0) {
        g.stats.parse_errors++;

        /*
         * 两类失败要分开对待(错误码见 proto_rtsp.h):
         *   -3: 缺 CSeq → 连"回给谁"都对不上, 静默丢弃
         *   -4: 请求行合法但方法不认识 → 必须回 405, 不能让客户端蒙在鼓里
         *   其它: 报文畸形 → 400
         */
        if (rc == -3) {
            LOG_WARN("RTSP 请求缺 CSeq, 无法回应, 丢弃");
            return 0;
        }
        if (rc == -4) {
            LOG_WARN("不支持的方法 '%s' → 405", req.method_name);
            n = proto_rtsp_build_error(&req, 405, "Method Not Allowed",
                                       resp, sizeof(resp));
        } else {
            LOG_WARN("RTSP 请求解析失败 rc=%d → 400(连接保留)", rc);
            n = proto_rtsp_build_error(&req, 400, "Bad Request",
                                       resp, sizeof(resp));
        }
        if (n <= 0)
            return 0;
        return (send_all(c->fd, resp, (size_t)n) == 0) ? 0 : -1;
    }

    g.stats.requests++;

    switch (req.method) {
    case PROTO_RTSP_METHOD_OPTIONS:
        n = proto_rtsp_build_options(&req, resp, sizeof(resp));
        break;

    case PROTO_RTSP_METHOD_DESCRIBE:
        n = proto_rtsp_build_describe(&req, g.sdp, g.sdp_len, resp, sizeof(resp));
        break;

    case PROTO_RTSP_METHOD_SETUP: {
        /* 会话号用"槽位下标 + 基准值"生成: 同一连接的每次 SETUP 得到确定的号,
           同时又不会和别的连接撞上(槽位下标天然唯一) */
        c->session_id       = 1000u + (uint32_t)(c - g.clients);
        c->session_assigned = 1;
        c->rtp_channel      = (req.transport == PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED)
                              ? req.interleaved_rtp : SVC_NET_RTP_CHANNEL;
        c->rtcp_channel     = (req.transport == PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED)
                              ? req.interleaved_rtcp : SVC_NET_RTCP_CHANNEL;
        n = proto_rtsp_build_setup(&req, c->session_id, 0, resp, sizeof(resp));
        break;
    }

    case PROTO_RTSP_METHOD_PLAY:
        if (!c->session_assigned) {
            n = proto_rtsp_build_error(&req, 454, "Session Not Found",
                                       resp, sizeof(resp));
            break;
        }
        c->playing = 1;
        n = proto_rtsp_build_play_pause(&req, c->session_id, resp, sizeof(resp));
        if (n > 0 && g.on_play != NULL)
            g.on_play((int)(c - g.clients), g.user);
        break;

    case PROTO_RTSP_METHOD_PAUSE:
        if (!c->session_assigned) {
            n = proto_rtsp_build_error(&req, 454, "Session Not Found",
                                       resp, sizeof(resp));
            break;
        }
        c->playing = 0;
        n = proto_rtsp_build_play_pause(&req, c->session_id, resp, sizeof(resp));
        break;

    case PROTO_RTSP_METHOD_TEARDOWN:
        n = proto_rtsp_build_teardown(&req, c->session_id, resp, sizeof(resp));
        if (n > 0 && send_all(c->fd, resp, (size_t)n) != 0)
            return -1;
        c->playing          = 0;
        c->session_assigned = 0;
        return 1;                       /* 1 = 回完就该断开 */

    case PROTO_RTSP_METHOD_GET_PARAMETER:
        /*
         * 有些客户端(VLC/ffmpeg)用它做保活。我们不实现任何参数, 但**必须回 200**
         * —— 否则客户端会认为连接已经坏了。
         * 借 build_error 来拼一个"空体的成功响应"(它的作用就是状态码+空体),
         * 免得在 svc_net 里又出现一处手拼报文的地方(纪律①)。
         */
        n = proto_rtsp_build_error(&req, 200, "OK", resp, sizeof(resp));
        break;

    default:
        g.stats.parse_errors++;
        n = proto_rtsp_build_error(&req, 405, "Method Not Allowed",
                                   resp, sizeof(resp));
        break;
    }

    if (n <= 0) {
        LOG_ERROR("响应构造失败(缓冲 %d 字节是否够?)", SVC_NET_RESP_BUF_SIZE);
        g.stats.send_errors++;
        return -1;
    }
    return (send_all(c->fd, resp, (size_t)n) == 0) ? 0 : -1;
}

/* ═══════════════ ② 收字节 + framing ═══════════════ */

/** 处理累积缓冲里**所有**已经完整的请求。@return 0 继续; -1 断开; 1 回完 TEARDOWN */
static int drain_requests(svc_net_client_t *c)
{
    for (;;) {
        char  *sep = strstr(c->rbuf, "\r\n\r\n");
        size_t req_len;
        int    rc;

        if (sep == NULL)
            return 0;                   /* 还没有完整请求, 继续等 */

        req_len = (size_t)(sep - c->rbuf) + 4;      /* 含那个空行 */
        *sep = '\0';                    /* 截断成 C 串, 交给解析器 */

        rc = handle_request(c, c->rbuf, req_len - 4);
        if (rc != 0)
            return rc;

        if (req_len >= c->rlen) {
            /* 这一个请求正好用完了缓冲里所有字节 */
            c->rlen    = 0;
            c->rbuf[0] = '\0';
            return 0;
        }

        /*
         * ★★ framing 最容易漏的一步: 缓冲里还有**下一个请求的全部或一部分**。
         *    必须把它搬到开头继续解析, **一个字节都不能丢**。
         */
        memmove(c->rbuf, c->rbuf + req_len, c->rlen - req_len);
        c->rlen -= req_len;
        c->rbuf[c->rlen] = '\0';
    }
}

/**
 * 读一个客户端的数据并处理。@return 0 继续; -1 关闭连接; 1 客户端说了 TEARDOWN
 *
 * @note ⚠️ 必须先读完再解析。原因(纪律③): epoll 是水平触发,
 *       只要内核缓冲里还有没读走的字节, 下一次 epoll_wait 会**继续报告同一个 fd**。
 *       如果这里每次只读一次就走, 事件循环会退化成忙等。
 */
static int read_client(svc_net_client_t *c)
{
    int rc;

    for (;;) {
        ssize_t n;
        size_t  space = sizeof(c->rbuf) - c->rlen - 1;   /* 留 1 字节放 '\0' */

        if (space == 0) {
            /*
             * 缓冲满了还找不到边界 → 对方不是一个守规矩的 RTSP 客户端
             * (或者被恶意灌数据)。**丢弃连接**比默默丢弃字节安全:
             * 丢字节会让后面所有请求都错位。
             */
            LOG_WARN("fd=%d 请求超长(%d 字节仍无空行), 丢弃连接",
                     c->fd, (int)sizeof(c->rbuf));
            g.stats.oversize_drops++;
            return -1;
        }

        n = recv(c->fd, c->rbuf + c->rlen, space, 0);
        if (n > 0) {
            c->rlen += (size_t)n;
            continue;
        }
        if (n == 0)
            return -1;                  /* 对端正常关闭 */
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;                      /* ★ 读干净了 —— 这才是退出条件 */
        return -1;                      /* 真错误 */
    }

    c->rbuf[c->rlen] = '\0';
    if (c->rlen == 0)
        return 0;
    c->last_active = time(NULL);

    rc = drain_requests(c);
    return rc;
}

/* ═══════════════ ④ 事件循环 ═══════════════ */

/** 清理所有空闲超时的客户端(纪律④) */
static void reap_idle(void)
{
    int i;

    if (g.idle_timeout_sec <= 0)
        return;

    for (i = 0; i < SVC_NET_MAX_CLIENTS; i++) {
        svc_net_client_t *c = &g.clients[i];

        if (c->fd == -1)
            continue;
        if (time(NULL) - c->last_active >= g.idle_timeout_sec) {
            LOG_INFO("空闲超时: fd=%d 已 %d 秒无数据, 主动断开",
                     c->fd, g.idle_timeout_sec);
            g.stats.conns_timeout++;
            client_free(i);
        }
    }
}

/** 接受一个新连接 */
static void on_accept(void)
{
    int cfd, idx;
    ev_data_t d;

    cfd = accept(g.listen_fd, NULL, NULL);
    if (cfd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            LOG_WARN("accept 失败: %s", strerror(errno));
        return;
    }
    if (infra_set_nonblocking(cfd) != 0) {
        LOG_WARN("设非阻塞失败, 拒绝这个连接");
        infra_close(&cfd);
        return;
    }

    idx = client_alloc(cfd);
    if (idx < 0) {
        LOG_WARN("客户端已满(%d 个), 拒绝新连接", SVC_NET_MAX_CLIENTS);
        g.stats.conns_rejected++;
        infra_close(&cfd);
        return;
    }

    d.kind  = EV_CLIENT;
    d.index = idx;
    if (infra_poller_add(g.poller, cfd, INFRA_POLL_IN) != 0) {
        LOG_ERROR("epoll 注册失败, 拒绝这个连接");
        infra_close(&g.clients[idx].fd);
        return;
    }
    g.stats.conns_accepted++;
    LOG_INFO("新客户端 fd=%d(槽位 %d), 当前 %d 个连接",
             cfd, idx, client_count());
}

/** 事件循环主体 */
static void *loop_thread(void *arg)
{
    infra_poll_event_t events[INFRA_POLL_MAX_EVENTS];
    (void)arg;

    LOG_INFO("RTSP 事件循环启动, 监听端口 %u", (unsigned)g.port);

    while (!g.stop_requested) {
        int n = infra_poller_wait(g.poller, events, INFRA_POLL_MAX_EVENTS,
                                  SVC_NET_TICK_MS);
        int i;

        if (n < 0) {
            LOG_ERROR("poll 出错, 退出事件循环");
            break;
        }

        /*
         * ★ 内务检查放在"每次醒来"的位置, 而不是"只在超时的时候"。
         *   忙碌时也会定期清理, 逻辑更简单、更不容易漏。
         */
        reap_idle();

        for (i = 0; i < n; i++) {
            int fd = events[i].fd;

            if (fd == g.listen_fd) {
                on_accept();
                continue;
            }
            if (events[i].events & (INFRA_POLL_ERR)) {
                int idx = client_find(fd);

                if (idx >= 0)
                    client_free(idx);
                continue;
            }
            if (events[i].events & INFRA_POLL_IN) {
                int idx = client_find(fd);

                if (idx < 0)
                    continue;           /* 已经被别的事件清理掉了 */
                if (read_client(&g.clients[idx]) != 0)
                    client_free(idx);
            }
        }
    }

    LOG_INFO("RTSP 事件循环退出");
    return NULL;
}

/* ═══════════════ ⑤ 生命周期 ═══════════════ */

int svc_net_start(const svc_net_cfg_t *cfg)
{
    int i;

    if (g.running)
        return 0;                       /* 幂等 */
    if (cfg == NULL)
        return -1;

    memset(&g, 0, sizeof(g));
    g.listen_fd = -1;
    g.port      = cfg->port;
    g.idle_timeout_sec = (cfg->idle_timeout_sec > 0)
                         ? cfg->idle_timeout_sec : SVC_NET_IDLE_TIMEOUT_SEC;
    g.on_play     = cfg->on_play;
    g.on_teardown = cfg->on_teardown;
    g.user        = cfg->user;

    /* ── 槽位表 + 读缓冲: 一次性分配, 运行期不再 malloc ── */
    g.clients = (svc_net_client_t *)malloc(sizeof(svc_net_client_t) *
                                           SVC_NET_MAX_CLIENTS);
    if (g.clients == NULL) {
        LOG_ERROR("客户端槽位分配失败");
        return -3;
    }
    for (i = 0; i < SVC_NET_MAX_CLIENTS; i++) {
        memset(&g.clients[i], 0, sizeof(g.clients[i]));
        g.clients[i].fd = -1;
    }

    /* ── SDP 拷贝一份: 调用方传进来的字符串随时可能失效 ── */
    if (cfg->sdp != NULL && cfg->sdp_len > 0) {
        g.sdp = (char *)malloc(cfg->sdp_len + 1);
        if (g.sdp == NULL) {
            free(g.clients);
            g.clients = NULL;
            return -3;
        }
        memcpy(g.sdp, cfg->sdp, cfg->sdp_len);
        g.sdp[cfg->sdp_len] = '\0';
        g.sdp_len = cfg->sdp_len;
    }

    /* ── 监听 socket ── */
    g.listen_fd = infra_tcp_listen(cfg->listen_ip ? cfg->listen_ip : "0.0.0.0",
                                  cfg->port,
                                  cfg->backlog > 0 ? cfg->backlog : 8);
    if (g.listen_fd < 0) {
        LOG_ERROR("监听 %s:%u 失败(端口被占用?)",
                  cfg->listen_ip ? cfg->listen_ip : "0.0.0.0",
                  (unsigned)cfg->port);
        free(g.sdp);
        free(g.clients);
        g.sdp     = NULL;
        g.clients = NULL;
        return -2;
    }
    if (infra_set_nonblocking(g.listen_fd) != 0) {
        infra_close(&g.listen_fd);
        free(g.sdp);
        free(g.clients);
        g.sdp     = NULL;
        g.clients = NULL;
        return -2;
    }

    /* 端口传 0 时问内核要真实端口, 便于测试与日志 */
    {
        struct sockaddr_in sa;
        socklen_t          sl = sizeof(sa);

        if (getsockname(g.listen_fd, (struct sockaddr *)&sa, &sl) == 0)
            g.port = ntohs(sa.sin_port);
    }

    /* ── epoll ── */
    g.poller = infra_poller_create();
    if (g.poller == NULL) {
        infra_close(&g.listen_fd);
        free(g.sdp);
        free(g.clients);
        g.sdp     = NULL;
        g.clients = NULL;
        return -3;
    }
    if (infra_poller_add(g.poller, g.listen_fd, INFRA_POLL_IN) != 0) {
        infra_poller_destroy(g.poller);
        infra_close(&g.listen_fd);
        free(g.sdp);
        free(g.clients);
        g.poller  = NULL;
        g.sdp     = NULL;
        g.clients = NULL;
        return -2;
    }

    /* ── 事件循环线程 ── */
    g.stop_requested = 0;
    g.running        = 1;
    if (pthread_create(&g.thread, NULL, loop_thread, NULL) != 0) {
        LOG_ERROR("事件循环线程创建失败");
        infra_poller_destroy(g.poller);
        infra_close(&g.listen_fd);
        free(g.sdp);
        free(g.clients);
        g.poller   = NULL;
        g.sdp      = NULL;
        g.clients  = NULL;
        g.running  = 0;
        return -4;
    }
    g.thread_valid = 1;

    LOG_INFO("RTSP 服务已启动: 端口 %u, SDP %u 字节, 空闲超时 %d 秒",
             (unsigned)g.port, (unsigned)g.sdp_len, g.idle_timeout_sec);
    return 0;
}

void svc_net_stop(void)
{
    int i;

    if (!g.running)
        return;

    g.stop_requested = 1;
    if (g.thread_valid) {
        pthread_join(g.thread, NULL);   /* 等循环真正退出, 避免 use-after-free */
        g.thread_valid = 0;
    }
    g.running = 0;

    for (i = 0; i < SVC_NET_MAX_CLIENTS; i++) {
        if (g.clients != NULL && g.clients[i].fd != -1)
            infra_close(&g.clients[i].fd);
    }
    if (g.poller != NULL)
        infra_poller_destroy(g.poller);
    if (g.listen_fd != -1)
        infra_close(&g.listen_fd);

    free(g.sdp);
    free(g.clients);
    g.sdp     = NULL;
    g.clients = NULL;
    g.poller  = NULL;

    LOG_INFO("RTSP 服务已停止(累计 %llu 连接 / %llu 请求 / %llu 响应)",
             (unsigned long long)g.stats.conns_accepted,
             (unsigned long long)g.stats.requests,
             (unsigned long long)g.stats.responses_sent);
}

int svc_net_is_running(void)
{
    return g.running;
}

uint16_t svc_net_port(void)
{
    return g.port;
}

void svc_net_get_stats(svc_net_stats_t *out)
{
    if (out != NULL)
        *out = g.stats;
}

int svc_net_client_count(void)
{
    return g.running ? client_count() : 0;
}
