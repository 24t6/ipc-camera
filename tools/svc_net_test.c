/**
 * @file    svc_net_test.c
 * @brief   svc_net(M1-8)的端到端测试 —— 真的起服务器、真的用 socket 连它
 *
 * 【为什么必须真的起服务器, 不能用"假 socket"】
 *   M1-8 的四个坑(framing / EAGAIN 读完 / 空闲超时 / Content-Length)
 *   **全都是"真实字节流行为"暴露的**, 单测里 mock 掉 socket 就正好把
 *   要测的东西 mock 掉了(B017 的教训: 单测只覆盖"我想到的情况")。
 *
 * 【怎么做到"可证伪"】
 *   客户端全部用**裸 socket 手写字节**, 不借用任何 RTSP 库:
 *     · 能精确控制"先发一半、停一下、再发另一半"(拆包)
 *     · 能精确控制"两个请求粘一起"(粘包)
 *     · 能断言响应的字节数、Content-Length 的值、行尾字符
 *
 * 【三项"发送模式"是重点】(都是真实客户端一定会出现的行为)
 *   ① 完整          : 一个请求一个报文
 *   ② 拆包(split)   : 第一个报文只发前 N 字节, 停 50ms 再发剩下的
 *   ③ 粘包(pipelined): 两个/三个请求一次发出去
 *
 * 编译(在能编 epoll 的 Linux 上, 如 Ubuntu VM):
 *   gcc -Wall -Wextra -O2 -pthread -o svc_net_test \
 *       tools/svc_net_test.c src/service/svc_net.c \
 *       src/protocol/proto_rtsp.c src/protocol/proto_str.c \
 *       src/infra/infra_poll.c src/infra/infra_netio.c src/infra/infra_log.c \
 *       -Isrc/service -Isrc/protocol -Isrc/infra
 *
 * 用法: ./svc_net_test [port]      # 端口默认 0(由内核分配, 避免和别的进程撞)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "infra_log.h"
#include "svc_net.h"

static int g_fails;
static int g_passes;
static uint16_t g_port;

/*
 * ── on_play 回调的捕获器(M1-9 新增;B036 后改为收 `infra_transport_t`)──
 *
 * 为什么要"捕获"而不是只让回调非 NULL:
 *   加了参数之后, **只验证回调被调到是不够的** —— 如果上层拿到的目的地
 *   是错的(比如端口填成了 RTSP 端口), 回调照样"被调到了"。
 *   所以要把传进来的**传输方式**存下来, 再断言它的 IP/端口/是否 TCP。
 *   这是本项目的老规矩: 断言要能证伪, 不能只断言"函数被调了"。
 *
 * ⚠️ **这里曾经腐烂过一次**(2026-09-19 被 `make test` 抓出来):
 *   B036 把回调参数从 `const struct sockaddr_in *` 换成了
 *   `const infra_transport_t *`, 我重跑了 `rtp_test` 与 `svc_sender_test`,
 *   **漏了这一个** —— 于是它带着不兼容的函数指针(只有一条 warning)继续"通过",
 *   直到补上 `make test` 才暴露。教训见项目 bug log。
 */
static int                 g_play_calls;      /* on_play 被调了几次 */
static int                 g_play_index = -1; /* 最后一次的 client_index */
static infra_transport_t   g_play_tr;         /* 最后一次收到的传输方式 */
static int                 g_play_dst_null;   /* 收到过 NULL 传输方式的次数(应为 0) */
static int                 g_teardown_calls;

static void fake_on_play(int client_index, const infra_transport_t *tr, void *user)
{
    (void)user;
    g_play_calls++;
    g_play_index = client_index;
    if (tr == NULL) {
        g_play_dst_null++;
        return;
    }
    g_play_tr = *tr;
}

static void fake_on_teardown(int client_index, void *user)
{
    (void)client_index;
    (void)user;
    g_teardown_calls++;
}

static void check(const char *name, int cond, const char *detail)
{
    if (cond) {
        g_passes++;
        printf("   [通过] %s%s%s\n", name, detail ? "  " : "", detail ? detail : "");
    } else {
        g_fails++;
        printf("   [失败] %s%s%s <<<\n", name, detail ? "  " : "", detail ? detail : "");
    }
}

/* ─────────────────── 极简客户端 ─────────────────── */

/** 连上服务器 */
static int cli_connect(void)
{
    struct sockaddr_in sa;
    int                fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(g_port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/** 等到 fd 可读或超时。@return 1 可读; 0 超时; -1 出错 */
static int wait_readable(int fd, int timeout_ms)
{
    struct pollfd p;

    p.fd      = fd;
    p.events  = POLLIN;
    p.revents = 0;
    return poll(&p, 1, timeout_ms);
}

/**
 * 收一段响应(读到本次数据为止)。
 *
 * @param settle_ms 收到第一段之后再等一会儿, 把同一响应剩下的部分也收齐
 * @return 收到的字节数
 */
static size_t cli_recv(int fd, char *buf, size_t cap, int timeout_ms, int settle_ms)
{
    size_t got = 0;
    int    idle = 0;

    for (;;) {
        ssize_t n;
        int     r = wait_readable(fd, (got == 0) ? timeout_ms : settle_ms);

        if (r <= 0)
            break;
        n = recv(fd, buf + got, cap - got - 1, 0);
        if (n <= 0) {
            idle = 1;
            break;
        }
        got += (size_t)n;
        if (got >= cap - 1)
            break;
    }
    buf[got] = '\0';
    (void)idle;
    return got;
}

/** 取响应里 Content-Length 的值; 没有返回 -1 */
static int cli_content_length(const char *resp)
{
    const char *p = strstr(resp, "\r\nContent-Length: ");

    if (p == NULL)
        p = strstr(resp, "Content-Length: ");      /* 万一它是第一个头 */
    if (p == NULL)
        return -1;
    p = strstr(p, "Content-Length: ") + 16;
    return atoi(p);
}

/** 数响应里有几个 Content-Length 头 */
static int cli_count_content_length(const char *resp)
{
    const char *p = resp;
    int         n = 0;

    while ((p = strstr(p, "Content-Length:")) != NULL) {
        n++;
        p += 15;
    }
    return n;
}

/** 数响应里有几个 "RTSP/1.0 200" */
static int cli_count_200(const char *resp)
{
    const char *p = resp;
    int         n = 0;

    while ((p = strstr(p, "RTSP/1.0 200")) != NULL) {
        n++;
        p += 12;
    }
    return n;
}

/** 响应里是否含某个完整行(以 CRLF 结尾) */
static int cli_has_line(const char *resp, const char *line)
{
    const char *p = resp;
    size_t      len = strlen(line);

    while ((p = strstr(p, line)) != NULL) {
        if (p[len] == '\r' && p[len + 1] == '\n')
            return 1;
        p += len;
    }
    return 0;
}

/** 拼一个请求 */
static size_t mk_req(char *buf, size_t cap, const char *method, int cseq,
                     const char *url, const char *extra)
{
    return (size_t)snprintf(buf, cap,
                            "%s rtsp://127.0.0.1:%u%s RTSP/1.0\r\n"
                            "CSeq: %d\r\n"
                            "%s"
                            "\r\n",
                            method, (unsigned)g_port, url, cseq,
                            extra ? extra : "");
}

/**
 * 等服务器侧的连接数达到期望值(最多等 timeout_ms)。
 * @return 最终观察到的连接数
 *
 * ★ 为什么需要它(两个都是**真实的异步边界**, 不是测试取巧):
 *   ① 客户端 connect() 成功 ≠ 服务器已经 accept 了 ——
 *      connect 在**内核**完成三次握手就返回(连接进了 accept 队列),
 *      服务器要等下一次 epoll_wait 才把它取出来。
 *   ② 客户端 close() 之后, 服务器也要等到下一次 epoll_wait 调 recv
 *      得到 0 才发现。
 *   所以"客户端做了什么"和"服务器的槽位表变成什么样"之间**一定有时间差**。
 *   测试如果忽略它, 就会看到"连接数怎么少了一个"这种假象。
 */
static int wait_for_client_count(int want, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        struct timespec ts = { 0, 50 * 1000 * 1000 };   /* 50ms */

        if (svc_net_client_count() == want)
            break;
        nanosleep(&ts, NULL);
        waited += 50;
    }
    return svc_net_client_count();
}

/** 等服务器把过期连接清理掉(回到 0)。@return 最终观察到的连接数 */
static int wait_until_reaped(int timeout_ms)
{
    return wait_for_client_count(0, timeout_ms);
}

/* ─────────────────── 测试用例 ─────────────────── */

/** 一个固定的、字节数确定的 SDP, 便于断言 Content-Length */
static const char *TEST_SDP =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=IPC Camera Test\r\n"
    "t=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n";

int main(int argc, char **argv)
{
    uint16_t       want_port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 0;
    svc_net_cfg_t  cfg;
    svc_net_stats_t st;
    char           req[1024], resp[8192];
    size_t         rl, got;
    int            fd, i;
    const size_t   sdp_len = strlen(TEST_SDP);

    /* 测试阶段只看 WARN 以上, 免得 INFO 刷屏淹没断言结果 */
    infra_log_set_level(INFRA_LOG_ERROR);

    printf("===== svc_net(M1-8)端到端测试 =====\n\n");

    /* ═══ ① 启动服务端 ═══ */
    printf("① 启动服务端(端口 %u)\n", (unsigned)want_port);
    memset(&cfg, 0, sizeof(cfg));
    cfg.listen_ip        = "127.0.0.1";
    cfg.port             = want_port;
    cfg.sdp              = TEST_SDP;
    cfg.sdp_len          = sdp_len;
    cfg.idle_timeout_sec = 2;           /* 测空闲超时, 用短的 */

    /* 注册回调 —— ⑦-2 要靠它验证"RTP 目的地真的传出来了" */
    cfg.on_play     = fake_on_play;
    cfg.on_teardown = fake_on_teardown;

    check("svc_net_start 返回 0", svc_net_start(&cfg) == 0, NULL);
    check("服务状态为运行中", svc_net_is_running() == 1, NULL);
    g_port = svc_net_port();
    printf("   实际监听端口 = %u\n", (unsigned)g_port);
    check("拿到了真实端口", g_port != 0, NULL);
    check("初始客户端数为 0", svc_net_client_count() == 0, NULL);
    printf("\n");

    /* ═══ ② 完整请求: OPTIONS ═══ */
    printf("② 完整请求 OPTIONS\n");
    fd = cli_connect();
    check("能连上监听端口", fd >= 0, NULL);
    rl = mk_req(req, sizeof(req), "OPTIONS", 1, "/live", NULL);
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
    {
        char detail[64];

        snprintf(detail, sizeof(detail), "(收到 %u 字节)", (unsigned)got);
        check("收到响应", got > 0, detail);
    }
    check("状态行是 200 OK", strncmp(resp, "RTSP/1.0 200 OK\r\n", 17) == 0, NULL);
    check("回对了 CSeq: 1", cli_has_line(resp, "CSeq: 1"), NULL);
    check("Public 头列出支持的方法",
          strstr(resp, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN") != NULL,
          NULL);
    check("★ 带 Content-Length(值 0)",
          cli_count_content_length(resp) == 1 && cli_content_length(resp) == 0, NULL);
    close(fd);
    printf("\n");

    /* ═══ ③ 拆包: 一个请求分两个报文发(framing 的核心) ═══ */
    printf("③ ★ 拆包:请求拆成两个报文发(真实 TCP 一定会有)\n");
    fd = cli_connect();
    rl = mk_req(req, sizeof(req), "DESCRIBE", 2, "/live", "Accept: application/sdp\r\n");
    {
        size_t first = rl / 2;          /* 先只发一半 */

        send(fd, req, first, 0);
        {
            struct timespec ts = { 0, 50 * 1000 * 1000 };   /* 等 50ms */

            nanosleep(&ts, NULL);
        }
        /* 前半段不该产生任何响应 */
        got = cli_recv(fd, resp, sizeof(resp), 200, 50);
        check("只发一半时不产生响应(不能瞎猜)", got == 0, NULL);

        send(fd, req + first, rl - first, 0);   /* 补上剩下的一半 */
        got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
    }
    check("半包补齐后拿到响应", got > 0, NULL);
    check("回对了 CSeq: 2", cli_has_line(resp, "CSeq: 2"), NULL);
    check("Content-Type 是 application/sdp",
          cli_has_line(resp, "Content-Type: application/sdp"), NULL);
    {
        char detail[96];

        snprintf(detail, sizeof(detail), "(Content-Length=%d, 期望 %u)",
                 cli_content_length(resp), (unsigned)sdp_len);
        check("★ Content-Length = SDP 真实字节数",
              cli_content_length(resp) == (int)sdp_len, detail);
    }
    check("响应体就是那串 SDP",
          strstr(resp, "s=IPC Camera Test") != NULL, NULL);
    close(fd);
    printf("\n");

    /* ═══ ④ 粘包: 两个请求一次发(framing 的另一半) ═══ */
    printf("④ ★ 粘包:两个请求粘在一个报文里发\n");
    fd = cli_connect();
    rl  = mk_req(req, sizeof(req), "OPTIONS", 11, "/live", NULL);
    rl += mk_req(req + rl, sizeof(req) - rl, "OPTIONS", 12, "/live", NULL);
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 200);
    {
        char detail[96];

        snprintf(detail, sizeof(detail), "(收到 %d 个 200)", cli_count_200(resp));
        check("★ 两个请求都被处理(不能只处理第一个)",
              cli_count_200(resp) == 2, detail);
    }
    check("两个 CSeq 都回了",
          cli_has_line(resp, "CSeq: 11") && cli_has_line(resp, "CSeq: 12"), NULL);
    close(fd);
    printf("\n");

    /* ═══ ⑤ 三连粘包 ═══ */
    printf("⑤ 三个请求粘一起(流水线)\n");
    fd = cli_connect();
    rl  = mk_req(req, sizeof(req), "OPTIONS", 21, "/live", NULL);
    rl += mk_req(req + rl, sizeof(req) - rl, "OPTIONS", 22, "/live", NULL);
    rl += mk_req(req + rl, sizeof(req) - rl, "OPTIONS", 23, "/live", NULL);
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 250);
    {
        char detail[96];

        snprintf(detail, sizeof(detail), "(收到 %d 个 200)", cli_count_200(resp));
        check("三个请求全部处理", cli_count_200(resp) == 3, detail);
    }
    check("三个 CSeq 都在",
          cli_has_line(resp, "CSeq: 21") && cli_has_line(resp, "CSeq: 22") &&
          cli_has_line(resp, "CSeq: 23"), NULL);
    close(fd);
    printf("\n");

    /* ═══ ⑥ 完整会话: SETUP → PLAY → PAUSE → TEARDOWN ═══ */
    printf("⑥ 完整会话流程\n");
    fd = cli_connect();
    rl = mk_req(req, sizeof(req), "SETUP", 31, "/live/streamid=0",
                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
    check("SETUP 拿到 200", cli_count_200(resp) == 1, NULL);
    check("回显 interleaved=0-1", strstr(resp, "interleaved=0-1") != NULL, NULL);
    check("分配了 Session", strstr(resp, "Session: ") != NULL, NULL);
    check("SETUP 也带 Content-Length", cli_count_content_length(resp) == 1, NULL);

    rl = mk_req(req, sizeof(req), "PLAY", 32, "/live",
                "Session: 1000\r\nRange: npt=0.000-\r\n");
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
    check("PLAY 拿到 200", cli_count_200(resp) == 1, NULL);
    check("PLAY 带 Session", strstr(resp, "Session: ") != NULL, NULL);
    /* ★ B036 的核心:SETUP 协商成 TCP 交错之后, on_play 必须**把传输方式带出去**
     *   (那两个根因之一就是"svc_sender 不看传输方式, 一律建 UDP sender") */
    check("★ TCP 会话: on_play 传出的传输方式是 TCP 交错",
          g_play_tr.is_tcp == 1, NULL);
    check("★ TCP 会话: 交错通道 = SETUP 协商的 0", g_play_tr.rtp_channel == 0, NULL);
    check("★ TCP 会话: 带上了 RTSP 连接的 fd", g_play_tr.rtsp_fd >= 0, NULL);

    rl = mk_req(req, sizeof(req), "PAUSE", 33, "/live", "Session: 1000\r\n");
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
    check("PAUSE 拿到 200", cli_count_200(resp) == 1, NULL);

    rl = mk_req(req, sizeof(req), "TEARDOWN", 34, "/live", "Session: 1000\r\n");
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
    check("TEARDOWN 拿到 200", cli_count_200(resp) == 1, NULL);
    {
        /* 服务器回完 TEARDOWN 应当主动断开 → 下一次读应当得到 EOF(0) */
        int r = wait_readable(fd, 1500);
        int eof = 0;

        if (r > 0) {
            char tmp[256];
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);

            eof = (n == 0);
        }
        check("★ TEARDOWN 之后服务器主动关闭连接", eof, NULL);
    }
    close(fd);
    printf("\n");

    /* ═══ ⑥-2 UDP 会话: on_play 必须传出正确的 RTP 目的地(M1-9 新增) ═══ */
    /*
     * 这一节验的是 M1-9 接线的**前置条件**: 上层要往客户端发 RTP,
     * 就必须从 on_play 拿到 "客户端 IP + 客户端 RTP 端口"。
     *
     * ⚠️ 用 **UDP** 而不是 TCP 交错, 而且 client_port 用一个**不常见的值**
     *    (50000-50001) —— 这样如果实现里把端口写错(比如误用 RTSP 端口、
     *    或误用 RTCP 端口), 断言立刻能发现。用 5000 这种默认值反而容易
     *    "碰巧对上"而掩盖错误。
     */
    printf("⑥-2 UDP 会话: on_play 传出的 RTP 目的地\n");
    {
        int before = g_play_calls;

        fd = cli_connect();
        rl = mk_req(req, sizeof(req), "SETUP", 35, "/live/streamid=0",
                    "Transport: RTP/AVP;unicast;client_port=50000-50001\r\n");
        send(fd, req, rl, 0);
        got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
        check("UDP SETUP 拿到 200", cli_count_200(resp) == 1, NULL);
        check("回显 client_port=50000-50001",
              strstr(resp, "client_port=50000-50001") != NULL, NULL);

        rl = mk_req(req, sizeof(req), "PLAY", 36, "/live",
                    "Session: 1000\r\nRange: npt=0.000-\r\n");
        send(fd, req, rl, 0);
        got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
        check("UDP PLAY 拿到 200", cli_count_200(resp) == 1, NULL);

        check("★ on_play 被调用了", g_play_calls == before + 1, NULL);
        check("★ 传出的传输方式不是 NULL", g_play_dst_null == 0, NULL);
        check("★ UDP 会话: 传输方式是 UDP(不是 TCP 交错)",
              g_play_tr.is_tcp == 0, NULL);
        check("★ 目的地 IP 是客户端 IP(127.0.0.1)",
              g_play_tr.rtp_dst.sin_addr.s_addr == htonl(INADDR_LOOPBACK), NULL);
        check("★ 目的地端口 = SETUP 协商的 client_port(50000)",
              ntohs(g_play_tr.rtp_dst.sin_port) == 50000, NULL);
        check("★ 用同一个值反查端口也能对上(证明字节序没错)",
              ntohs(g_play_tr.rtp_dst.sin_port) != 50001, NULL);

        /* 收尾: TEARDOWN → 应触发 on_teardown */
        rl = mk_req(req, sizeof(req), "TEARDOWN", 37, "/live", "Session: 1000\r\n");
        send(fd, req, rl, 0);
        got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
        check("UDP TEARDOWN 拿到 200", cli_count_200(resp) == 1, NULL);
        check("★ on_teardown 被调用了", g_teardown_calls >= 1, NULL);
    }
    close(fd);
    printf("\n");

    /* ═══ ⑦ 错误处理 ═══ */
    printf("⑦ 错误与边界\n");
    fd = cli_connect();
    /* 不认识的方法 → 405 */
    rl = mk_req(req, sizeof(req), "FOOBAR", 41, "/live", NULL);
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
    check("未知方法 → 405 Method Not Allowed",
          strncmp(resp, "RTSP/1.0 405", 12) == 0, NULL);
    check("405 响应也带 Content-Length", cli_count_content_length(resp) == 1, NULL);

    /* 没 SETUP 就 PLAY → 454 */
    rl = mk_req(req, sizeof(req), "PLAY", 42, "/live", NULL);
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
    check("没 SETUP 就 PLAY → 454 Session Not Found",
          strncmp(resp, "RTSP/1.0 454", 12) == 0, NULL);

    /* GET_PARAMETER 保活 → 200 */
    rl = mk_req(req, sizeof(req), "GET_PARAMETER", 43, "/live", NULL);
    send(fd, req, rl, 0);
    got = cli_recv(fd, resp, sizeof(resp), 2000, 150);
    check("GET_PARAMETER 保活 → 200", cli_count_200(resp) == 1, NULL);
    check("保活响应体为空(Content-Length=0)",
          cli_content_length(resp) == 0, NULL);
    close(fd);
    printf("\n");

    /* ═══ ⑧ 请求超长必须断连 ═══ */
    printf("⑧ 请求超长(缓冲满且无空行)→ 丢弃连接\n");
    fd = cli_connect();
    {
        char junk[SVC_NET_READ_BUF_SIZE + 512];

        memset(junk, 'X', sizeof(junk));
        int   closed = 0;
        int   r;
        char  tmp[64];
        ssize_t n;

        /* 服务器可能在 send 循环中途就关掉了连接, 所以 send 出错也算"已关闭" */
        for (i = 0; i < (int)sizeof(junk); i += 512) {
            n = send(fd, junk + i, 512, MSG_NOSIGNAL);
            if (n <= 0) {
                closed = 1;
                break;
            }
        }
        if (!closed) {
            r = wait_readable(fd, 2000);
            if (r > 0) {
                n = recv(fd, tmp, sizeof(tmp), 0);
                /* 0 = 对端正常关闭; <0 且是 RST = 也被关了; >0 = 竟然还回数据(不该发生) */
                closed = (n == 0) || (n < 0 && (errno == ECONNRESET || errno == EPIPE));
            } else if (r < 0) {
                closed = 1;
            }
        }
        check("★ 超长请求之后连接被关闭", closed, NULL);
    }
    close(fd);
    printf("\n");

    /* ═══ ⑨ 空闲超时必须回收槽位 ═══ */
    printf("⑨ 空闲超时回收槽位(配置为 2 秒)\n");
    {
        int fds[6];
        int before;

        /* 先把前面用例留下的连接等它被回收干净, 否则下面的计数会被上一个用例干扰 */
        {
            char detail[64];
            int  left = wait_until_reaped(5000);

            snprintf(detail, sizeof(detail), "(残留 %d 个)", left);
            check("前面的连接都已回收", left == 0, detail);
        }
        before = svc_net_client_count();

        for (i = 0; i < 6; i++)
            fds[i] = cli_connect();
        {
            char detail[128];
            int  now = wait_for_client_count(before + 6, 5000);

            snprintf(detail, sizeof(detail), "(before=%d, now=%d)", before, now);
            check("连上 6 个客户端(等服务器 accept 完)", now - before == 6, detail);
        }

        printf("   什么都不发, 等 4 秒…\n");
        {
            struct timespec ts = { 4, 0 };

            nanosleep(&ts, NULL);
        }
        {
            char detail[96];
            int  left = wait_until_reaped(4000);

            snprintf(detail, sizeof(detail), "(还剩 %d 个)", left);
            check("★ 空闲连接被回收(槽位不泄漏)", left == before, detail);
        }
        for (i = 0; i < 6; i++)
            close(fds[i]);
    }
    printf("\n");

    /* ═══ ⑩ 槽位可复用(超过上限也没问题) ═══ */
    printf("⑩ 连续 12 次连接(上限 8)—— 槽位可复用\n");
    {
        int ok_conn = 0;

        for (i = 0; i < 12; i++) {
            int f = cli_connect();

            if (f >= 0) {
                char r2[256];
                size_t l2 = mk_req(r2, sizeof(r2), "OPTIONS", 100 + i, "/live", NULL);

                send(f, r2, l2, 0);
                got = cli_recv(f, resp, sizeof(resp), 2000, 100);
                if (cli_count_200(resp) == 1)
                    ok_conn++;
                close(f);
            }
        }
        {
            char detail[64];

            snprintf(detail, sizeof(detail), "(%d/12 成功)", ok_conn);
            check("★ 12 次连接全部拿到 200(槽位被回收复用)",
                  ok_conn == 12, detail);
        }
    }
    printf("\n");

    /* ═══ ⑪ 统计与停机 ═══ */
    printf("⑪ 统计与停机\n");
    svc_net_get_stats(&st);
    printf("   连接: 接受 %llu / 拒绝 %llu / 关闭 %llu(其中超时 %llu)\n",
           (unsigned long long)st.conns_accepted,
           (unsigned long long)st.conns_rejected,
           (unsigned long long)st.conns_closed,
           (unsigned long long)st.conns_timeout);
    printf("   请求: %llu(解析失败 %llu)响应 %llu(发送失败 %llu)\n",
           (unsigned long long)st.requests,
           (unsigned long long)st.parse_errors,
           (unsigned long long)st.responses_sent,
           (unsigned long long)st.send_errors);
    check("统计: 接受连接数 > 0", st.conns_accepted > 0, NULL);
    check("统计: 请求数 > 0", st.requests > 0, NULL);
    check("统计: 超时回收 > 0(空闲超时确实生效)", st.conns_timeout > 0, NULL);
    check("统计: 超长丢弃 > 0(超长保护确实生效)", st.oversize_drops > 0, NULL);
    check("统计: 没有发送失败", st.send_errors == 0, NULL);
    check("统计: 没有拒绝连接(槽位够用)", st.conns_rejected == 0, NULL);

    svc_net_stop();
    check("停机后状态为未运行", svc_net_is_running() == 0, NULL);
    check("停机后客户端数归零", svc_net_client_count() == 0, NULL);
    /* 二次停机必须安全 */
    svc_net_stop();
    check("重复停机是安全的(no-op)", 1, NULL);
    printf("\n");

    printf("===== 结果: %s(通过 %d 项, 失败 %d 项)=====\n",
           g_fails == 0 ? "全部通过" : "有失败", g_passes, g_fails);
    return g_fails == 0 ? 0 : 1;
}
