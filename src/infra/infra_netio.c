/**
 * @file    infra_netio.c
 * @brief   发送抽象 + socket 助手实现 —— 见 infra_netio.h
 *
 * 【模块职责】发送抽象 infra_sender_t(把"怎么发"与"发什么"隔开) + socket 小工具
 * 【依赖方向】只依赖 libc 与 infra_log
 * 【线程模型】sender 对象由调用方独占; 底层 socket fd 可跨线程使用(内核保证)
 * 【资源边界】sender 对象在创建时分配一次, 由调用方 destroy; 无运行期分配
 */
#include "infra_netio.h"

#include <arpa/inet.h>      /* inet_pton */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>           /* poll —— 非阻塞 socket 等可写(别空转) */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "infra_log.h"

/** RTP-over-TCP 交错帧的头部长度:$ + channel + 2 字节长度 */
#define INTERLEAVED_HDR_LEN 4

/** 交错帧头首字节固定为 '$'(ASCII 0x24), 见 RFC 2326 §10.12 */
#define INTERLEAVED_MAGIC   0x24

/** `infra_tcp_write_all` 的失败计数(只用来限制日志次数, 不必精确) */
static uint64_t g_tcp_write_errors;

int infra_tcp_write_all(int fd, const void *buf, size_t len)
{
    const char *p    = (const char *)buf;
    size_t      sent = 0;

    if (fd < 0 || (buf == NULL && len > 0)) {
        return -1;
    }
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* 缓冲满: `poll` 等可写, **不要 `continue` 空转** ——
             * 那是 100% CPU 的死循环, 而且永远发不出去(2026-09-18 修掉的隐患)。 */
            struct pollfd pfd;

            pfd.fd      = fd;
            pfd.events  = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 100) > 0) {
                continue;
            }
            if (g_tcp_write_errors < 3) {
                LOG_ERROR("tcp_write_all: 等可写超时(fd=%d, 已发 %zu/%zu)",
                          fd, sent, len);
            }
            g_tcp_write_errors++;
            return -1;
        }
        if (g_tcp_write_errors < 3) {
            LOG_ERROR("tcp_write_all: send() 失败 n=%zd errno=%d(%s), 已发 %zu/%zu",
                      n, errno, strerror(errno), sent, len);
        }
        g_tcp_write_errors++;
        return -1;
    }
    return 0;
}

/* ─────────────────── UDP sender ─────────────────── */

/** UDP sender 的上下文:目标地址 + fd + 统计 */
typedef struct {
    int                  sockfd;
    struct sockaddr_in   dst;       /* 按值持有, 调用方不必保持有效 */
    uint64_t             bytes;
    uint64_t             packets;
    uint64_t             errors;
} udp_ctx_t;

/**
 * @brief UDP 发送实现(infra_sender_t 的 vtable 之一)
 *
 * @param ctx udp_ctx_t(内含目标地址与 socket fd)
 * @param buf 待发数据
 * @param len 字节数
 * @return 0 成功; 负值失败
 */
static int udp_send(void *ctx, const void *buf, size_t len)
{
    udp_ctx_t *c = (udp_ctx_t *)ctx;
    ssize_t    n;

    if (c == NULL || buf == NULL || len == 0)
        return -1;

    n = sendto(c->sockfd, buf, len, 0,
               (const struct sockaddr *)&c->dst, sizeof(c->dst));
    if (n < 0) {
        c->errors++;
        return -1;
    }
    c->bytes   += (uint64_t)n;
    c->packets += 1;
    return (int)n;
}

/**
 * @brief 销毁 UDP sender 并释放上下文
 *
 * @param ps 指向 sender 指针的指针(销毁后置 NULL)
 */
static void udp_destroy(infra_sender_t **ps)
{
    infra_sender_t *s;

    if (ps == NULL || *ps == NULL)
        return;
    s = *ps;
    /* 注意: 只释放 ctx 和 sender 本身, **不关闭 sockfd** —— 所有权归调用方 */
    free(s->ctx);
    s->ctx = NULL;
    free(s);
    *ps = NULL;
}

infra_sender_t *infra_sender_udp(const struct sockaddr_in *dst, int sockfd)
{
    infra_sender_t *s;
    udp_ctx_t      *c;

    if (dst == NULL || sockfd < 0)
        return NULL;

    s = (infra_sender_t *)malloc(sizeof(*s));
    c = (udp_ctx_t *)malloc(sizeof(*c));
    if (s == NULL || c == NULL) {
        free(s);
        free(c);
        return NULL;
    }

    memset(c, 0, sizeof(*c));
    c->sockfd = sockfd;
    c->dst    = *dst;                   /* 结构体整体拷贝 */

    s->send    = udp_send;
    s->destroy = udp_destroy;
    s->ctx     = c;
    return s;
}

/* ─────────────────── TCP interleaved sender ─────────────────── */

/** TCP 交错 sender 的上下文 */
typedef struct {
    int      fd;
    uint8_t  channel;
    uint8_t *frame;                     /* 发送缓冲: 交错头 + 数据, 创建期分配一次 */
    size_t   frame_cap;
    uint64_t bytes;                     /* 只统计 RTP 数据部分, 不含交错头 */
    uint64_t packets;
    uint64_t errors;
} tcp_ctx_t;

/* 单帧上限: RTP 头(12) + FU 头(3) + 载荷上限(1400) + 交错头(4), 留足余量 */
#define TCP_FRAME_CAP 2048

/**
 * @brief 把拼好的交错帧**写完**
 *
 * @param c     上下文
 * @param total 总字节数(交错头 + 数据)
 * @return 0 成功; -1 失败(已累加 `c->errors`)
 *
 * @note 真正的循环在 `infra_tcp_write_all()` —— 这里只负责把失败记进本 sender
 *       的统计(原来是一份独立实现, 2026-09-19 合并成一处)。
 */
static int tcp_write_all(tcp_ctx_t *c, size_t total)
{
    if (infra_tcp_write_all(c->fd, c->frame, total) != 0) {
        c->errors++;
        return -1;
    }
    return 0;
}

/**
 * @brief TCP 交错(interleaved)发送实现 —— 把 RTP 包塞进 RTSP 那条 TCP 连接
 *
 * @param ctx tcp_ctx_t
 * @param buf 待发数据
 * @param len 字节数
 * @return >=0 成功(等于 len); 负值失败
 *
 * @note ADR-1 当时把它当**预留实现**(先做 UDP);2026-09-18 才真正接上 ——
 *       在此之前 `svc_sender` 不管协商成什么都建 UDP sender, 于是客户端勾
 *       "以 TCP 播放"时一帧都发不出去。详见 `infra_transport_t` 的说明。
 * @note 交错帧必须**一次写完**(允许多次 write 但必须字节连续),
 *       否则多线程交错会破坏 TCP 流。
 */
static int tcp_send(void *ctx, const void *buf, size_t len)
{
    tcp_ctx_t *c = (tcp_ctx_t *)ctx;
    size_t     total;

    if (c == NULL || buf == NULL || len == 0)
        return -1;
    if (len + INTERLEAVED_HDR_LEN > c->frame_cap) {
        c->errors++;
        return -1;                      /* 超过单帧上限, 拒绝而不是截断 */
    }

    /* 拼交错帧: 必须一次 write() 送完整帧, 否则多线程交错会破坏 TCP 流 */
    c->frame[0] = INTERLEAVED_MAGIC;
    c->frame[1] = c->channel;
    c->frame[2] = (uint8_t)(len >> 8);  /* 长度是大端 16 位 */
    c->frame[3] = (uint8_t)(len & 0xFF);
    memcpy(c->frame + INTERLEAVED_HDR_LEN, buf, len);
    total = len + INTERLEAVED_HDR_LEN;

    if (tcp_write_all(c, total) != 0) {
        return -1;
    }
    c->bytes   += (uint64_t)len;
    c->packets += 1;
    return (int)len;
}

/**
 * @brief 销毁 TCP sender 并释放上下文(含创建期分配的成帧缓冲)
 *
 * @param ps 指向 sender 指针的指针
 */
static void tcp_destroy(infra_sender_t **ps)
{
    infra_sender_t *s;
    tcp_ctx_t      *c;

    if (ps == NULL || *ps == NULL)
        return;
    s = *ps;
    c = (tcp_ctx_t *)s->ctx;

    if (c != NULL) {
        free(c->frame);                 /* 释放创建期分配的发送缓冲 */
        free(c);
    }
    s->ctx = NULL;
    free(s);
    *ps = NULL;
}

infra_sender_t *infra_sender_tcp_interleaved(int fd, uint8_t rtp_channel)
{
    infra_sender_t *s;
    tcp_ctx_t      *c;

    if (fd < 0)
        return NULL;

    s = (infra_sender_t *)malloc(sizeof(*s));
    c = (tcp_ctx_t *)malloc(sizeof(*c));
    if (s == NULL || c == NULL) {
        free(s);
        free(c);
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    c->frame = (uint8_t *)malloc(TCP_FRAME_CAP);
    if (c->frame == NULL) {
        free(s);
        free(c);
        return NULL;
    }

    c->fd        = fd;
    c->channel   = rtp_channel;
    c->frame_cap = TCP_FRAME_CAP;

    s->send    = tcp_send;
    s->destroy = tcp_destroy;
    s->ctx     = c;
    return s;
}

/* ─────────────────── 统计 ─────────────────── */

/*
 * 两个 ctx 结构体都以 {bytes, packets, errors} 结尾(前两个字段对 UDP 是
 * sockfd/dst, 对 TCP 是 fd/channel/frame/frame_cap)。这里用一个小访问器
 * 按 sender 的实现取统计, 避免在两个结构体间做"猜内存布局"的危险转换。
 */
void infra_sender_stats(const infra_sender_t *s, uint64_t *out_bytes,
                        uint64_t *out_packets, uint64_t *out_errors)
{
    if (s == NULL || s->ctx == NULL)
        return;

    if (s->send == udp_send) {
        const udp_ctx_t *c = (const udp_ctx_t *)s->ctx;
        if (out_bytes)   *out_bytes   = c->bytes;
        if (out_packets) *out_packets = c->packets;
        if (out_errors)  *out_errors  = c->errors;
    } else if (s->send == tcp_send) {
        const tcp_ctx_t *c = (const tcp_ctx_t *)s->ctx;
        if (out_bytes)   *out_bytes   = c->bytes;
        if (out_packets) *out_packets = c->packets;
        if (out_errors)  *out_errors  = c->errors;
    }
}

/* ─────────────────── socket 助手 ─────────────────── */

int infra_tcp_listen(const char *ip, uint16_t port, int backlog)
{
    int                fd;
    int                on = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    /* 必须设: 否则程序重启后端口还在 TIME_WAIT, bind 会失败 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (ip == NULL || strcmp(ip, "0.0.0.0") == 0)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int infra_udp_bind(const char *ip, uint16_t port)
{
    int                fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (ip == NULL || strcmp(ip, "0.0.0.0") == 0)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int infra_set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void infra_close(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        close(*fd);
        *fd = -1;                       /* 置 -1, 防重复关闭 */
    }
}
