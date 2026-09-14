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
    int      index;                     /* 自己在 g.clients[] 里的下标 ——
                                           缓存下来, 免得各处反复用 (c - g.clients) 指针运算 */
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
            c->index       = i;
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
 * 拼一个错误响应并发出去。
 *
 * @return 0 已回应(连接保留); -1 发送失败(应当断开)
 * @note 抽出来的理由: 原来 400/405/454 三个分支各自重复"构造 + 判断 + 发送"三段,
 *       重复三遍就是三处可能漏掉返回值检查的地方。
 */
static int reply_error(svc_net_client_t *c, const proto_rtsp_request_t *req,
                       int code, const char *reason)
{
    char resp[SVC_NET_RESP_BUF_SIZE];
    int  n = proto_rtsp_build_error(req, code, reason, resp, sizeof(resp));

    if (n <= 0) {
        g.stats.send_errors++;
        return -1;                      /* 构造不出来 = 缓冲出问题, 断连比装死好 */
    }
    return (send_all(c->fd, resp, (size_t)n) == 0) ? 0 : -1;
}

/**
 * 处理解析失败的请求。
 *
 * @return 0 已处理完(连接保留); -1 已回应但发送失败, 必须断开
 *
 * @note 三类失败要分开(错误码见 proto_rtsp.h):
 *       -3 缺 CSeq → 连"回给谁"都对不上, **静默丢弃**(回了也没意义)
 *       -4 请求行合法但方法不认识 → **405**(让客户端知道是"不支持"而不是"写错了")
 *       其它 → **400 Bad Request**
 */
static int handle_parse_failure(svc_net_client_t *c,
                                const proto_rtsp_request_t *req, int rc)
{
    g.stats.parse_errors++;

    if (rc == -3) {
        LOG_WARN("RTSP 请求缺 CSeq, 无法回应, 丢弃");
        return 0;
    }
    if (rc == -4) {
        LOG_WARN("不支持的方法 '%s' → 405", req->method_name);
        return reply_error(c, req, 405, "Method Not Allowed");
    }
    LOG_WARN("RTSP 请求解析失败 rc=%d → 400(连接保留)", rc);
    return reply_error(c, req, 400, "Bad Request");
}

/**
 * 处理 SETUP: 记录会话与传输通道。
 *
 * @note 会话号 = 槽位下标 + 基准值: 同一连接每次 SETUP 都得到确定的号,
 *       而槽位下标天然唯一 → 不会和别的连接撞上。
 * @note 交错通道号只在客户端要 TCP 时才有意义; UDP 时填默认值,
 *       这样上层(发 RTP)不必再判断传输方式。
 */
static void setup_session(svc_net_client_t *c, const proto_rtsp_request_t *req)
{
    int tcp = (req->transport == PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED);

    c->session_id       = 1000u + (uint32_t)c->index;
    c->session_assigned = 1;
    c->rtp_channel      = tcp ? req->interleaved_rtp  : SVC_NET_RTP_CHANNEL;
    c->rtcp_channel     = tcp ? req->interleaved_rtcp : SVC_NET_RTCP_CHANNEL;
}

/**
 * 处理 PLAY / PAUSE(两者只差 playing 标志和要不要通知上层)。
 *
 * @return 同 handle_request
 */
static int handle_play_pause(svc_net_client_t *c, const proto_rtsp_request_t *req)
{
    char resp[SVC_NET_RESP_BUF_SIZE];
    int  playing = (req->method == PROTO_RTSP_METHOD_PLAY);
    int  n;

    if (!c->session_assigned)
        return reply_error(c, req, 454, "Session Not Found");

    c->playing = playing;
    n = proto_rtsp_build_play_pause(req, c->session_id, resp, sizeof(resp));
    if (n <= 0) {
        g.stats.send_errors++;
        return -1;
    }
    if (send_all(c->fd, resp, (size_t)n) != 0)
        return -1;

    /* 先把响应发出去, 再通知上层 —— 否则上层的回调若阻塞, 客户端会先等到超时 */
    if (playing && g.on_play != NULL)
        g.on_play(c->index, g.user);
    return 0;
}

/**
 * 按方法分派, 把响应文本拼进 out。
 *
 * @return >0 = 响应字节数; 0 = 这个请求不需要普通响应(见 out_kind);
 *         -1 = 构造失败或断连
 * @param[out] out_teardown 置 1 表示"回完就该断开"(TEARDOWN)
 *
 * @note 与 handle_request 分成两个函数的理由: 前者是**纯粹的"请求 → 响应文本"**,
 *       后者负责"解析和收尾(统计/发送/断开判定)"。混在一起时, switch 的
 *       缩进和 return 路径会互相纠缠, 函数很快就长到读不完。
 */
static int build_response(svc_net_client_t *c, const proto_rtsp_request_t *req,
                          char *out, size_t cap, int *out_teardown)
{
    *out_teardown = 0;

    switch (req->method) {
    case PROTO_RTSP_METHOD_OPTIONS:
        return proto_rtsp_build_options(req, out, cap);

    case PROTO_RTSP_METHOD_DESCRIBE:
        return proto_rtsp_build_describe(req, g.sdp, g.sdp_len, out, cap);

    case PROTO_RTSP_METHOD_SETUP:
        setup_session(c, req);
        return proto_rtsp_build_setup(req, c->session_id, 0, out, cap);

    case PROTO_RTSP_METHOD_TEARDOWN: {
        int n = proto_rtsp_build_teardown(req, c->session_id, out, cap);

        c->playing          = 0;
        c->session_assigned = 0;
        *out_teardown       = 1;
        return n;
    }

    case PROTO_RTSP_METHOD_GET_PARAMETER:
        /*
         * 有些客户端(VLC/ffmpeg)用它做保活。我们不实现任何参数, 但**必须回 200**
         * —— 否则客户端会认为连接已经坏了。
         * 借 build_error 来拼一个"空体的成功响应"(它的作用就是状态码+空体),
         * 免得在 svc_net 里又出现一处手拼报文的地方(纪律①)。
         */
        return proto_rtsp_build_error(req, 200, "OK", out, cap);

    default:
        /* 正常走不到这里: 不认识的方法在解析阶段就返回 -4 了。
           留着是为了"以后新增方法枚举但忘了加分支"时行为明确。 */
        return proto_rtsp_build_error(req, 405, "Method Not Allowed", out, cap);
    }
}

/**
 * 处理一个**完整的** RTSP 请求并回应。
 *
 * @param req_buf 请求文本(已被截断到边界处, 保证以 '\0' 结尾)
 * @param req_len 请求字节数
 * @return 0 已回应(连接保留); -1 连接应当关闭; 1 = 收到 TEARDOWN, 回完就断
 *
 * @note 本函数**不构造任何报文文本**, 只做"解析 → 分派 → 发送"。
 */
static int handle_request(svc_net_client_t *c, const char *req_buf, size_t req_len)
{
    proto_rtsp_request_t req;
    char                 resp[SVC_NET_RESP_BUF_SIZE];
    int                  teardown = 0;
    int                  n;
    int                  rc;

    rc = proto_rtsp_parse_request(req_buf, req_len, &req);
    if (rc != 0)
        return handle_parse_failure(c, &req, rc);

    g.stats.requests++;

    /* PLAY/PAUSE 要"发完响应再通知上层", 单独处理 */
    if (req.method == PROTO_RTSP_METHOD_PLAY ||
        req.method == PROTO_RTSP_METHOD_PAUSE)
        return handle_play_pause(c, &req);

    n = build_response(c, &req, resp, sizeof(resp), &teardown);
    if (n <= 0) {
        LOG_ERROR("响应构造失败(缓冲 %d 字节是否够?)", SVC_NET_RESP_BUF_SIZE);
        g.stats.send_errors++;
        return -1;
    }
    if (send_all(c->fd, resp, (size_t)n) != 0)
        return -1;

    return teardown ? 1 : 0;
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

/**
 * 接受一个新连接。
 *
 * ⚠️ **耦合点**: 监听 socket 目前注册的是**电平触发**(LT), 所以这里
 * 每次事件只 `accept()` 一个就够了 —— 没接完的连接, 下次 epoll_wait 还会报。
 * **如果以后把监听口改成 `EPOLLET`, 这里必须同步改成循环 accept 到 `EAGAIN`**,
 * 否则新连接会堆在队列里没人接(边沿触发不会为它们再报一次)。
 * 改一处必须改两处 —— 所以把这个约束写在代码旁边, 而不是只写在文档里。
 */
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

/**
 * 分派一个就绪事件。
 *
 * @note 抽出来的理由有二: 让 loop_thread 只剩"等 → 内务 → 分派"三步;
 *       以及把"fd → 槽位"的查找集中在一处(fd 号会被内核复用,
 *       散在多处查是 bug 的温床)。
 */
static void dispatch_event(const infra_poll_event_t *ev)
{
    int idx;

    if (ev->fd == g.listen_fd) {
        if (ev->events & INFRA_POLL_IN)
            on_accept();
        return;
    }

    idx = client_find(ev->fd);
    if (idx < 0)
        return;                         /* 已经被前面的内务检查清理掉了 */

    if (ev->events & INFRA_POLL_ERR) {
        client_free(idx);
        return;
    }
    if ((ev->events & INFRA_POLL_IN) && read_client(&g.clients[idx]) != 0)
        client_free(idx);
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

        for (i = 0; i < n; i++)
            dispatch_event(&events[i]);
    }

    LOG_INFO("RTSP 事件循环退出");
    return NULL;
}

/* ═══════════════ ⑤ 生命周期 ═══════════════ */

/**
 * 释放启动阶段申请过的一切(无论启动成功与否都可用)。
 *
 * @note ⚠️ **清理顺序必须是申请顺序的反序** —— 这是统一清理函数唯一要守的纪律。
 *       这里的申请顺序是: ①槽位表/读缓冲 → ②SDP 副本 → ③监听 fd → ④epoll。
 *       所以关闭时: 先关所有客户端 fd → 销毁 epoll → 关监听 fd → free SDP → free 槽位表。
 *
 * @note 抽出来的理由: 原来 svc_net_start 里有 **5 条失败路径各自手写一遍"
 *       关 fd + free + 置 NULL"**, 一共 25 行重复代码。
 *       那种写法只要有一条路径漏了某一步, 就是内存泄漏或重复关闭 ——
 *       而且新增一条失败路径时要记得同步 5 处。**统一出口一次就够。**
 */
static void release_all(void)
{
    int i;

    if (g.clients != NULL) {
        for (i = 0; i < SVC_NET_MAX_CLIENTS; i++) {
            if (g.clients[i].fd != -1)
                infra_close(&g.clients[i].fd);
        }
    }
    if (g.poller != NULL) {
        infra_poller_destroy(g.poller);
        g.poller = NULL;
    }
    if (g.listen_fd != -1)
        infra_close(&g.listen_fd);

    free(g.sdp);
    free(g.clients);
    g.sdp     = NULL;
    g.clients = NULL;
}

/**
 * 第 1 步: 分配槽位表 + 拷贝 SDP。
 * @return 0 成功; -3 内存不足(失败时不会留下半成品)
 */
static int start_alloc(const svc_net_cfg_t *cfg)
{
    int i;

    g.clients = (svc_net_client_t *)malloc(sizeof(svc_net_client_t) *
                                           SVC_NET_MAX_CLIENTS);
    if (g.clients == NULL) {
        LOG_ERROR("客户端槽位分配失败");
        return -3;
    }
    for (i = 0; i < SVC_NET_MAX_CLIENTS; i++) {
        memset(&g.clients[i], 0, sizeof(g.clients[i]));
        g.clients[i].fd    = -1;
        g.clients[i].index = i;
    }

    /* SDP 拷贝一份: 调用方传进来的字符串随时可能失效 */
    if (cfg->sdp != NULL && cfg->sdp_len > 0) {
        g.sdp = (char *)malloc(cfg->sdp_len + 1);
        if (g.sdp == NULL) {
            LOG_ERROR("SDP 副本分配失败");
            release_all();
            return -3;
        }
        memcpy(g.sdp, cfg->sdp, cfg->sdp_len);
        g.sdp[cfg->sdp_len] = '\0';
        g.sdp_len = cfg->sdp_len;
    }
    return 0;
}

/**
 * 第 2 步: 建监听 socket(非阻塞)并问出真实端口。
 * @return 0 成功; -2 建不起来(端口被占用?); -3 epoll 建不起来
 *
 * @note 端口传 0 时由内核分配 —— 必须用 getsockname 问回真实值,
 *       否则日志和测试都不知道服务在哪个端口上。
 */
static int start_listen(const svc_net_cfg_t *cfg)
{
    struct sockaddr_in sa;
    socklen_t          sl = sizeof(sa);

    g.listen_fd = infra_tcp_listen(cfg->listen_ip ? cfg->listen_ip : "0.0.0.0",
                                  cfg->port,
                                  cfg->backlog > 0 ? cfg->backlog : 8);
    if (g.listen_fd < 0) {
        LOG_ERROR("监听 %s:%u 失败(端口被占用?)",
                  cfg->listen_ip ? cfg->listen_ip : "0.0.0.0",
                  (unsigned)cfg->port);
        return -2;
    }
    if (infra_set_nonblocking(g.listen_fd) != 0) {
        LOG_ERROR("监听 socket 设非阻塞失败");
        return -2;
    }
    if (getsockname(g.listen_fd, (struct sockaddr *)&sa, &sl) == 0)
        g.port = ntohs(sa.sin_port);

    g.poller = infra_poller_create();
    if (g.poller == NULL) {
        LOG_ERROR("epoll 实例创建失败");
        return -3;
    }
    if (infra_poller_add(g.poller, g.listen_fd, INFRA_POLL_IN) != 0) {
        LOG_ERROR("监听 fd 注册进 epoll 失败");
        return -2;
    }
    return 0;
}

int svc_net_start(const svc_net_cfg_t *cfg)
{
    int rc;

    if (g.running)
        return 0;                       /* 幂等 */
    if (cfg == NULL)
        return -1;

    memset(&g, 0, sizeof(g));
    g.listen_fd        = -1;
    g.port             = cfg->port;
    g.idle_timeout_sec = (cfg->idle_timeout_sec > 0)
                         ? cfg->idle_timeout_sec : SVC_NET_IDLE_TIMEOUT_SEC;
    g.on_play          = cfg->on_play;
    g.on_teardown      = cfg->on_teardown;
    g.user             = cfg->user;

    if ((rc = start_alloc(cfg)) != 0)       /* rc 已经是 -3 */
        return rc;
    if ((rc = start_listen(cfg)) != 0) {
        release_all();
        return rc;
    }

    /* 先置 running 再起线程: 否则线程可能先跑起来, 而 running 还是 0 */
    g.stop_requested = 0;
    g.running        = 1;
    if (pthread_create(&g.thread, NULL, loop_thread, NULL) != 0) {
        LOG_ERROR("事件循环线程创建失败");
        g.running = 0;
        release_all();
        return -4;
    }
    g.thread_valid = 1;

    LOG_INFO("RTSP 服务已启动: 端口 %u, SDP %u 字节, 空闲超时 %d 秒",
             (unsigned)g.port, (unsigned)g.sdp_len, g.idle_timeout_sec);
    return 0;
}

void svc_net_stop(void)
{
    if (!g.running)
        return;

    g.stop_requested = 1;
    if (g.thread_valid) {
        pthread_join(g.thread, NULL);   /* 等循环真正退出, 避免 use-after-free */
        g.thread_valid = 0;
    }
    g.running = 0;

    release_all();

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
