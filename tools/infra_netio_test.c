/**
 * @file    infra_netio_test.c
 * @brief   infra_netio / infra_poll / infra_log 的单元测试
 *
 * 【怎么验证】(可证伪)
 *   ① **UDP sender 真的发出去了吗** —— 不靠"我觉得发了", 而是发到本机另一个
 *      UDP socket, 收回来逐字节比对, 并读计数确认统计对得上
 *   ② **TCP 交错帧格式对不对** —— 让 sender 写到一条本机 TCP 连接,
 *      另一端按 RFC 2326 §10.12 解析 `$ + channel + 2 字节长度`, 逐字段核对
 *   ③ **poller 会不会骗人** —— 没数据时返回 0; 有数据时准确报告是哪个 fd 可读;
 *      删除后不再报告
 *   ④ **socket 助手** —— listen 后真能连上; 非阻塞设置生效; close 后句柄被置 -1
 *
 * 编译(在 Ubuntu VM 上, 因为 infra 层用 epoll):
 *   gcc -Wall -Wextra -O2 -o infra_test infra_netio_test.c \
 *       ../src/infra/infra_netio.c ../src/infra/infra_poll.c \
 *       ../src/infra/infra_log.c -I../src/infra
 *
 * 用法: ./infra_test
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "infra_log.h"
#include "infra_netio.h"
#include "infra_poll.h"

static int g_fails;

static int check(const char *name, int cond)
{
    printf("   %-52s %s\n", name, cond ? "[通过]" : "[失败] <<<");
    if (!cond)
        g_fails++;
    return cond;
}

/** 建一个本机 UDP socket 并绑定到内核分配的端口 */
static int make_udp(uint16_t *out_port)
{
    int                fd;
    struct sockaddr_in a;
    socklen_t          alen = sizeof(a);

    fd = infra_udp_bind("127.0.0.1", 0);
    if (fd < 0)
        return -1;
    if (getsockname(fd, (struct sockaddr *)&a, &alen) < 0) {
        close(fd);
        return -1;
    }
    if (out_port)
        *out_port = ntohs(a.sin_port);
    return fd;
}

/** 建一条本机 TCP 连接, 返回 {listen_fd, client_fd, server_fd} */
static int make_tcp_pair(int *out_cli, int *out_srv, uint16_t *out_port)
{
    int                lfd, cfd, sfd;
    struct sockaddr_in a;
    socklen_t          alen = sizeof(a);
    uint16_t           port;

    lfd = infra_tcp_listen("127.0.0.1", 0, 4);
    if (lfd < 0)
        return -1;
    if (getsockname(lfd, (struct sockaddr *)&a, &alen) < 0) {
        close(lfd);
        return -1;
    }
    port = ntohs(a.sin_port);

    cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) {
        close(lfd);
        return -1;
    }
    a.sin_port = htons(port);
    if (connect(cfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(cfd);
        close(lfd);
        return -1;
    }
    sfd = accept(lfd, NULL, NULL);
    if (sfd < 0) {
        close(cfd);
        close(lfd);
        return -1;
    }
    close(lfd);                         /* 监听使命完成 */
    if (out_cli)
        *out_cli = cfd;
    if (out_srv)
        *out_srv = sfd;
    if (out_port)
        *out_port = port;
    return 0;
}

int main(void)
{
    printf("===== infra_netio / infra_poll / infra_log 单元测试 =====\n\n");

    /* ═══ ① UDP sender: 真发真收 ═══ */
    printf("① UDP sender(发到本机另一个 socket 并收回比对)\n");
    {
        uint16_t           rport = 0;
        int                rx = make_udp(&rport);
        int                tx;
        struct sockaddr_in dst;
        infra_sender_t    *s;
        const char         payload[] = "RTP-PAYLOAD-TEST-0123456789";
        char               got[128];
        ssize_t            n;
        uint64_t           bytes = 0, pkts = 0, errs = 0;

        check("收端 socket 已建立", rx >= 0);
        tx = make_udp(NULL);
        check("发端 socket 已建立", tx >= 0);

        memset(&dst, 0, sizeof(dst));
        dst.sin_family      = AF_INET;
        dst.sin_port        = htons(rport);
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        s = infra_sender_udp(&dst, tx);
        check("sender 创建成功", s != NULL);

        if (s != NULL) {
            int rc = s->send(s->ctx, payload, sizeof(payload));
            check("send() 返回发送字节数", rc == (int)sizeof(payload));

            n = recv(rx, got, sizeof(got), 0);
            check("对端确实收到了数据", n == (ssize_t)sizeof(payload));
            check("内容逐字节一致", n == (ssize_t)sizeof(payload) &&
                  memcmp(got, payload, sizeof(payload)) == 0);

            /* 再发两次, 看统计是否累加 */
            s->send(s->ctx, payload, sizeof(payload));
            s->send(s->ctx, payload, sizeof(payload));
            infra_sender_stats(s, &bytes, &pkts, &errs);
            check("统计: 累计包数 = 3", pkts == 3);
            check("统计: 累计字节 = 3 × 长度",
                  bytes == 3u * (uint64_t)sizeof(payload));
            check("统计: 失败次数 = 0", errs == 0);

            /* sender 不接管 fd 的所有权: 销毁它之后, 原 fd 必须仍可用 */
            s->destroy(&s);
            check("destroy 后 sender 指针被置 NULL", s == NULL);
            check("destroy 不关闭调用方的 socket(原 fd 仍能发)",
                  sendto(tx, "x", 1, 0, (struct sockaddr *)&dst, sizeof(dst)) == 1);
        }
        close(rx);
        close(tx);
    }
    printf("\n");

    /* ═══ ② TCP 交错 sender: 帧格式 ═══ */
    printf("② TCP interleaved sender(RFC 2326 §10.12 帧格式)\n");
    {
        int             cli = -1, srv = -1;
        infra_sender_t *s;
        uint8_t         rtp[20];
        uint8_t         frame[64];
        ssize_t         n;

        check("建立本机 TCP 连接", make_tcp_pair(&cli, &srv, NULL) == 0);
        if (cli >= 0 && srv >= 0) {
            memset(rtp, 0xAB, sizeof(rtp));
            s = infra_sender_tcp_interleaved(cli, 3);   /* 用通道号 3, 便于区分 */
            check("sender 创建成功", s != NULL);

            if (s != NULL) {
                int rc = s->send(s->ctx, rtp, sizeof(rtp));
                check("send() 返回原始数据长度(不含交错头)",
                      rc == (int)sizeof(rtp));

                n = recv(srv, frame, sizeof(frame), 0);
                check("对端收到 4 + 20 = 24 字节",
                      n == (ssize_t)(4 + sizeof(rtp)));
                check("字节 0 = 0x24 ('$' 魔数)", n > 0 && frame[0] == 0x24);
                check("字节 1 = 通道号 3", n > 1 && frame[1] == 3);
                check("字节 2-3 = 大端长度 0x0014",
                      n > 3 && frame[2] == 0x00 && frame[3] == 0x14);
                check("之后是原始 RTP 数据",
                      n >= (ssize_t)(4 + sizeof(rtp)) &&
                      memcmp(frame + 4, rtp, sizeof(rtp)) == 0);

                /* 超长帧必须被拒绝, 不能截断 */
                {
                    static uint8_t big[8192];
                    rc = s->send(s->ctx, big, sizeof(big));
                    check("超长帧被拒绝(返回负值, 不截断)", rc < 0);
                }
                s->destroy(&s);
                check("destroy 后 sender 指针被置 NULL", s == NULL);
            }
            close(cli);
            close(srv);
        }
    }
    printf("\n");

    /* ═══ ③ poller ═══ */
    printf("③ infra_poller(epoll)\n");
    {
        infra_poller_t     *p = infra_poller_create();
        infra_poll_event_t  evs[INFRA_POLL_MAX_EVENTS];
        uint16_t            port = 0;
        int                 rx, tx, n;
        struct sockaddr_in  dst;

        check("poller 创建成功", p != NULL);
        rx = make_udp(&port);
        tx = make_udp(NULL);

        check("add 成功", infra_poller_add(p, rx, INFRA_POLL_IN) == 0);
        check("重复 add(改事件)也成功", infra_poller_add(p, rx, INFRA_POLL_IN) == 0);

        n = infra_poller_wait(p, evs, INFRA_POLL_MAX_EVENTS, 0);
        check("没数据时 wait 返回 0(不骗人)", n == 0);

        /* 真发一个包, 再看 poller 报不报 */
        memset(&dst, 0, sizeof(dst));
        dst.sin_family      = AF_INET;
        dst.sin_port        = htons(port);
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(tx, "Z", 1, 0, (struct sockaddr *)&dst, sizeof(dst));

        n = infra_poller_wait(p, evs, INFRA_POLL_MAX_EVENTS, 1000);
        check("有数据时 wait 返回 1", n == 1);
        check("报的正是那个 fd", n == 1 && evs[0].fd == rx);
        check("事件里含可读标志", n == 1 && (evs[0].events & INFRA_POLL_IN));

        check("del 成功", infra_poller_del(p, rx) == 0);
        n = infra_poller_wait(p, evs, INFRA_POLL_MAX_EVENTS, 0);
        check("删除后不再报告该 fd", n == 0);

        infra_poller_destroy(p);
        close(rx);
        close(tx);
    }
    printf("\n");

    /* ═══ ④ socket 助手 ═══ */
    printf("④ socket 助手\n");
    {
        int cli = -1, srv = -1, fd;
        uint16_t port = 0;

        check("infra_tcp_listen + 真能连上", make_tcp_pair(&cli, &srv, &port) == 0);
        check("分配到了非 0 端口", port != 0);
        if (cli >= 0) {
            check("infra_set_nonblocking 成功", infra_set_nonblocking(cli) == 0);
            close(cli);
        }
        if (srv >= 0)
            close(srv);

        fd = make_udp(NULL);
        check("infra_udp_bind(端口 0)成功", fd >= 0);
        infra_close(&fd);
        check("infra_close 后句柄被置 -1", fd == -1);
        infra_close(&fd);
        check("对已关闭的句柄再 close 是安全的(不会重复关闭)", fd == -1);
    }
    printf("\n");

    /* ═══ ⑤ 日志 ═══ */
    printf("⑤ infra_log\n");
    {
        int n;

        infra_log_set_level(INFRA_LOG_INFO);
        check("get_level 返回刚设置的级别",
              infra_log_get_level() == INFRA_LOG_INFO);

        n = infra_log_write(INFRA_LOG_INFO, "test.c", 1, "这条 INFO 应当出现: %d", 42);
        check("INFO 级别正常输出(返回写出字节数)", n > 0);

        n = infra_log_write(INFRA_LOG_DEBUG, "test.c", 2, "这条 DEBUG 应被过滤");
        check("低于级别的日志被过滤(返回 -1)", n == -1);

        infra_log_set_level(INFRA_LOG_DEBUG);
        n = infra_log_write(INFRA_LOG_DEBUG, "test.c", 3, "调低级后 DEBUG 能出来");
        check("调低级别后 DEBUG 能输出", n > 0);

        check("级别名可读", strcmp(infra_log_level_name(INFRA_LOG_ERROR), "ERROR") == 0);
        infra_log_set_level(INFRA_LOG_INFO);
    }

    printf("\n===== 结果: %s(%d 项失败)=====\n",
           g_fails == 0 ? "全部通过" : "有失败", g_fails);
    return g_fails == 0 ? 0 : 1;
}
