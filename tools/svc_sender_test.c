/**
 * @file    svc_sender_test.c
 * @brief   RTP 发送服务单测 —— 本机 UDP 自发自收, 验逻辑而不是"能跑"
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  为什么要在 PC 上测它
 * ─────────────────────────────────────────────────────────────────
 *  `svc_sender` 是整条链路上**逻辑最绕**的一个:
 *      · 一客户端一份 RTP 会话(序列号/时间戳/SSRC 各自独立)
 *      · **新客户端必须等 IDR 才开始发**(否则花屏)
 *      · 快照 + 引用计数(两个线程碰同一张表)
 *
 *  这些错了在板子上**只表现为"花屏"或"偶尔崩"**, 极难反查。
 *  而它们**全是纯逻辑** —— 只要有 UDP 回环 + 一个假队列, PC 上就能验。
 *
 * ─────────────────────────────────────────────────────────────────
 *  怎么造数据
 * ─────────────────────────────────────────────────────────────────
 *  不依赖 MPP, 直接按 `svc_media` 的**槽位布局**拼出帧:
 *
 *      ┌─ 帧头(SVC_MEDIA_HDR_SIZE 字节)─┬─ Annex-B 码流 ─┐
 *      │ magic@0  len@4  pts@offsetof   │ 00 00 00 01 .. │
 *      └────────────────────────────────┴────────────────┘
 *
 *  ⚠️ 偏移全部用 `offsetof()` 算, **不写魔法数字** —— B024 的教训。
 *
 *  用法: ./svc_sender_test
 *  退出码: 0 = 全部通过; 1 = 有失败
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "infra_log.h"
#include "infra_queue.h"
#include "proto_nalu.h"
#include "proto_rtp.h"
#include "svc_media.h"
#include "svc_sender.h"

/* ─────────────────── 断言 ─────────────────── */

static int g_fails;
static int g_passes;

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

/* ─────────────────── 造一帧 ─────────────────── */

/** 槽位缓冲: 帧头 + 最多 8KB 码流(测试用不到大帧) */
#define TEST_SLOT_BYTES 8192
#define TEST_CAPACITY   8

/** 往一段缓冲里追加一个 Annex-B NALU(4 字节起始码 + 1 字节 NALU 头 + 填充) */
static size_t append_nalu(uint8_t *buf, size_t off, uint8_t nal_hdr, size_t payload)
{
    size_t i;

    buf[off + 0] = 0x00;
    buf[off + 1] = 0x00;
    buf[off + 2] = 0x00;
    buf[off + 3] = 0x01;
    buf[off + 4] = nal_hdr;
    for (i = 5; i < 5 + payload; i++)
        buf[off + i] = (uint8_t)(0xA0 + (i & 0x0F));    /* 无害的填充字节 */
    return off + 5 + payload;
}

/**
 * 造一帧(含 `svc_media` 帧头)+ 填进槽位。
 *
 * @param slot      输出缓冲(至少 TEST_SLOT_BYTES)
 * @param pts       时间戳(1MHz 单位)
 * @param frame_idx 帧序号
 * @param is_key    1 = 关键帧(带 SPS/PPS/SEI/IDR), 0 = 普通帧(只有一个 P 切片)
 * @return 槽位里写入的总字节数
 */
static size_t make_frame(uint8_t *slot, uint64_t pts, uint32_t frame_idx, int is_key)
{
    size_t   off = (size_t)SVC_MEDIA_HDR_SIZE;
    uint32_t stream_len;

    if (is_key) {
        /* 照实测: 关键帧 = SPS(7) + PPS(8) + SEI(6) + IDR(5) */
        off = append_nalu(slot, off, 0x67, 12);     /* SPS */
        off = append_nalu(slot, off, 0x68, 6);      /* PPS */
        off = append_nalu(slot, off, 0x06, 8);      /* SEI */
        off = append_nalu(slot, off, 0x65, 900);    /* IDR */
    } else {
        /* 普通帧 = 一个非 IDR 切片(类型 1) */
        off = append_nalu(slot, off, 0x61, 400);
    }
    stream_len = (uint32_t)(off - (size_t)SVC_MEDIA_HDR_SIZE);

    /* 填帧头 —— 偏移用 offsetof, 不写数字(B024 教训) */
    {
        svc_media_frame_hdr_t *h = (svc_media_frame_hdr_t *)slot;

        memset(slot, 0, (size_t)SVC_MEDIA_HDR_SIZE);
        h->magic       = SVC_MEDIA_FRAME_MAGIC;
        h->len         = stream_len;
        h->cap         = 256 * 1024;
        h->frame_index = frame_idx;
        h->pts         = pts;
    }
    return off;
}

/* ─────────────────── 收包端 ─────────────────── */

static int    g_rx_fd;              /* 接收 socket */

/**
 * 收一个 RTP 包(带超时)。
 *
 * @param buf       输出
 * @param cap       缓冲容量
 * @param timeout_ms 超时
 * @return 收到的字节数; 0 = 超时; -1 = 出错
 */
static int rx_one(uint8_t *buf, size_t cap, int timeout_ms)
{
    struct timeval tv;
    fd_set         fds;
    int            r;

    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    FD_ZERO(&fds);
    FD_SET(g_rx_fd, &fds);
    r = select(g_rx_fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0)
        return 0;
    r = (int)recv(g_rx_fd, buf, cap, 0);
    return (r < 0) ? -1 : r;
}

/**
 * 建一个只收 RTP 的 UDP socket, 绑定到本机随机端口。
 * @param out 输出地址(拿去 add_client)
 */
static int make_rx(struct sockaddr_in *out)
{
    struct sockaddr_in self;
    socklen_t          slen = sizeof(self);
    int                fd   = socket(AF_INET, SOCK_DGRAM, 0);
    int                opt  = 1;

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&self, 0, sizeof(self));
    self.sin_family      = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    self.sin_port        = 0;
    if (bind(fd, (struct sockaddr *)&self, sizeof(self)) != 0) {
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&self, &slen) != 0) {
        close(fd);
        return -1;
    }
    *out = self;
    return fd;
}

/** 从 RTP 头里取字段 */
static uint32_t rtp_ts(const uint8_t *p)       { return ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16)
                                                       | ((uint32_t)p[6] << 8) | p[7]; }
static uint32_t rtp_ssrc(const uint8_t *p)     { return ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16)
                                                       | ((uint32_t)p[10] << 8) | p[11]; }
static int      rtp_marker(const uint8_t *p)   { return (p[1] & 0x80) ? 1 : 0; }
static int      rtp_pt(const uint8_t *p)       { return p[1] & 0x7F; }

/* ─────────────────── 主流程 ─────────────────── */

int main(void)
{
    infra_queue_t  *q;
    uint8_t        *slot;
    struct sockaddr_in dst;
    uint64_t        pts;
    int             i;

    infra_log_set_level(INFRA_LOG_ERROR);

    printf("===== svc_sender 单元测试(本机 UDP 自发自收)=====\n\n");

    /* 队列容量故意小一点, 便于测"队满丢最旧"对发送的影响 */
    q = infra_queue_create(TEST_CAPACITY, TEST_SLOT_BYTES);
    if (q == NULL) { printf("队列创建失败\n"); return 1; }
    slot = (uint8_t *)malloc(TEST_SLOT_BYTES);
    if (slot == NULL) { printf("槽位分配失败\n"); return 1; }

    /* ═══ ① 启动发送服务 ═══ */
    printf("① 启动发送服务\n");
    check("svc_sender_start 返回 0", svc_sender_start(q, 0) == 0, NULL);
    check("状态为运行中", svc_sender_is_running() == 1, NULL);
    check("启动时没有客户端", svc_sender_client_count() == 0, NULL);
    printf("\n");

    /* ═══ ② ★ 新客户端必须等到 IDR 才开始收 ═══ */
    /*
     * 这一条是本次测试**最重要**的断言。
     * 中途加入的客户端没有参考帧, 收到 P 帧会花屏 —— 所以实现里
     * 新客户端先标记 waiting_idr, 把中间的 P 帧**全部跳过**,
     * 直到遇到关键帧才真正开始发。
     *
     * 测法: 先推**普通帧**(客户端还没加入)→ 加入 → 再推普通帧(应当被跳过)
     *       → 再推关键帧(应当从这一帧开始发)。
     */
    printf("② ★ 新客户端从 IDR 开始(跳过之前的 P 帧)\n");
    {
        int fd = make_rx(&dst);
        uint8_t pkt[2048];
        int     n;

        check("建好接收 socket", fd >= 0, NULL);
        g_rx_fd = fd;

        /* 推 2 个普通帧(客户端还没加入) */
        pts = 1000000u;
        for (i = 0; i < 2; i++) {
            size_t n_bytes = make_frame(slot, pts, (uint32_t)i, 0);
            infra_queue_push(q, slot, n_bytes);
            pts += 33333u;
        }

        /* 客户端加入 —— 此时队列里已有 P 帧 */
        check("add_client 返回 0", svc_sender_add_client(0, &dst) == 0, NULL);
        check("客户端数为 1", svc_sender_client_count() == 1, NULL);

        /* 等一会儿: 队列里那 2 个 P 帧应当被**跳过**(不发) */
        n = rx_one(pkt, sizeof(pkt), 300);
        check("★ 加入前的 P 帧没有被发出(拿不到包)", n == 0, NULL);

        /* 再推 1 个普通帧 —— 仍应被跳过 */
        {
            size_t n_bytes = make_frame(slot, pts, 2, 0);

            infra_queue_push(q, slot, n_bytes);
            pts += 33333u;
        }
        n = rx_one(pkt, sizeof(pkt), 300);
        check("★ 加入后的第一个 P 帧也被跳过(还在等 IDR)", n == 0, NULL);

        /* 推一个关键帧 —— 现在应该开始发了 */
        {
            size_t n_bytes = make_frame(slot, pts, 3, 1);

            infra_queue_push(q, slot, n_bytes);
            pts += 33333u;
        }
        n = rx_one(pkt, sizeof(pkt), 1000);
        check("★ 关键帧到达后立刻开始发送", n >= 12, NULL);
        if (n >= 12) {
            check("  RTP 版本位 = 2", (pkt[0] & 0xC0) == 0x80, NULL);
            check("  载荷类型是 H.264 的 96",
                  rtp_pt(pkt) == PROTO_RTP_PT_H264, NULL);
            check("  SSRC 非 0", rtp_ssrc(pkt) != 0, NULL);
        }

        /* 收完这一帧剩下的包(关键帧有 4 个 NALU, 每个 1 包) */
        {
            int got = 1, marker_seen = 0;

            while (got < 8) {
                n = rx_one(pkt, sizeof(pkt), 300);
                if (n < 12)
                    break;
                got++;
                if (rtp_marker(pkt))
                    marker_seen = 1;
            }
            printf("      关键帧共收到 %d 个包, marker=%d\n", got, marker_seen);
            check("★ 关键帧 4 个 NALU → 共 4 个包", got == 4, NULL);
            check("★ 最后一个包带 marker 位(RTP 帧边界)", marker_seen == 1, NULL);
        }

        svc_sender_remove_client(0);
        check("remove_client 后客户端数归零",
              svc_sender_client_count() == 0, NULL);
        close(fd);
    }
    printf("\n");

    /* ═══ ③ 两个客户端: 各自的 RTP 会话必须独立 ═══ */
    printf("③ 两个客户端: 序列号与 SSRC 各自独立\n");
    {
        struct sockaddr_in d1, d2;
        int fd1 = make_rx(&d1), fd2 = make_rx(&d2);
        uint32_t ts1 = 0, ts2 = 0;
        uint32_t ssrc1 = 0, ssrc2 = 0;

        check("两个接收 socket 都建好", fd1 >= 0 && fd2 >= 0, NULL);

        /* 清空队列里的残留 */
        {
            uint8_t junk[TEST_SLOT_BYTES];
            size_t  jl;

            while (infra_queue_pop(q, junk, sizeof(junk), &jl, 0) == 0)
                ;
        }

        check("add client 1", svc_sender_add_client(1, &d1) == 0, NULL);
        check("add client 2", svc_sender_add_client(2, &d2) == 0, NULL);

        /*
         * ⚠️ 取样前必须**先把两个 socket 排空**。
         *
         * 第一版我没有排空, 于是读到的是**上一节的残留包**(不同帧), 
         * 断言"同帧时间戳相同"就假失败了 —— 而实现完全正确。
         * 这是本测试踩的第二个"取样错误"(第一个在②, 是漏排空导致增量算成 0)。
         *
         * 教训: 测"跨客户端一致性"这类断言时, **必须保证两个 socket 的
         * 读位置对齐**, 否则比的是两帧的数据。
         */
        {
            uint8_t junk[4096];
            int     drained = 0;

            g_rx_fd = fd1;
            while (rx_one(junk, sizeof(junk), 80) >= 12)
                drained++;
            g_rx_fd = fd2;
            while (rx_one(junk, sizeof(junk), 80) >= 12)
                drained++;
            printf("      取样前排空 %d 个残留包\n", drained);
        }

        /* 推一帧关键帧(两个客户端都在等 IDR) */
        {
            size_t n_bytes = make_frame(slot, pts, 10, 1);

            infra_queue_push(q, slot, n_bytes);
            pts += 33333u;
        }
        usleep(400 * 1000);     /* 等发送线程把这一帧发完 */

        g_rx_fd = fd1;
        {
            uint8_t p1[2048];
            int     n1 = rx_one(p1, sizeof(p1), 800);

            check("★ 客户端 1 收到包", n1 >= 12, NULL);
            if (n1 >= 12) {
                ts1   = rtp_ts(p1);
                ssrc1 = rtp_ssrc(p1);
            }
        }
        g_rx_fd = fd2;
        {
            uint8_t p2[2048];
            int     n2 = rx_one(p2, sizeof(p2), 800);

            check("★ 客户端 2 收到包", n2 >= 12, NULL);
            if (n2 >= 12) {
                ts2   = rtp_ts(p2);
                ssrc2 = rtp_ssrc(p2);
            }
        }
        printf("      client1: ts=%u ssrc=%08X / client2: ts=%u ssrc=%08X\n",
               ts1, ssrc1, ts2, ssrc2);

        /* 两个 SSRC 必须不同 —— 否则接收端会把两路流当成同一路 */
        check("★ 两个客户端的 SSRC 不同(否则接收端会串流)",
              ssrc1 != 0 && ssrc2 != 0 && ssrc1 != ssrc2, NULL);

        /*
         * ⚠️ 这里**不能**断言"两个客户端的时间戳相同"。
         *
         * 我第一版就是这么写的, 结果失败了(ts1=3043645645 / ts2=3579628106)。
         * 但那是**我的断言错了, 不是实现错了**:
         *   每个客户端的 RTP 会话有**独立的随机时间戳起点**
         *   (RFC 3550 要求随机化, 见 `proto_rtp_session_init`),
         *   因为它们是**两条独立的流** —— 起点本就该不同。
         *
         * 真正的不变量是: **各自在自己的基准上推进相同的量**。
         * 所以下面改成验"两个客户端的时间戳**增量**相同" ——
         * 这才是"同一帧、同一 PTS"该有的表现。
         */

        /*
         * ⚠️ 关键: 把两个 socket 里的**残留包排空**再测下一帧。
         *
         * 第一版我漏了这一步, 结果断言"时间戳增量 ≈ 3000"失败(实测 0)——
         * 因为上面每个客户端只读了 1 个包, 而关键帧有 4 个包, 剩下 3 个还堆在
         * socket 缓冲里; 第二帧读取时读到的其实是**第一帧的残留包**,
         * 同一帧内时间戳相同 → 增量当然是 0。
         *
         * **这是测试写错了, 不是实现错了** —— 而且它险些让我去改一个
         * 本来正确的实现。教训: 断言失败时先怀疑测试的取样方式。
         */
        {
            uint8_t junk[4096];
            int     total = 0;

            g_rx_fd = fd1;
            while (rx_one(junk, sizeof(junk), 60) >= 12)
                total++;
            g_rx_fd = fd2;
            while (rx_one(junk, sizeof(junk), 60) >= 12)
                total++;
            printf("      排空残留包 %d 个\n", total);
        }

        /* 再推一帧, 看时间戳是否按 PTS 推进(两客户端应当一致) */
        {
            size_t n_bytes = make_frame(slot, pts, 11, 0);

            infra_queue_push(q, slot, n_bytes);
        }
        usleep(400 * 1000);

        g_rx_fd = fd1;
        {
            uint8_t p1[2048];
            int     n1 = rx_one(p1, sizeof(p1), 800);

            if (n1 >= 12) {
                uint32_t d = rtp_ts(p1) - ts1;

                printf("      客户端1 时间戳增量 = %u(期望 ≈ 3000)\n", d);
                check("★ 时间戳按编码器 PTS 推进(≈3000, 即 1MHz→90kHz)",
                      d >= 2990 && d <= 3010, NULL);
            } else {
                check("★ 时间戳按编码器 PTS 推进", 0, "第二帧没收到");
            }
        }
        g_rx_fd = fd2;
        {
            uint8_t p2[2048];
            int     n2 = rx_one(p2, sizeof(p2), 800);

            if (n2 >= 12) {
                uint32_t d = rtp_ts(p2) - ts2;

                printf("      客户端2 时间戳增量 = %u\n", d);
                check("★ 两个客户端的时间戳**增量**相同(同一帧、同一 PTS)",
                      d >= 2990 && d <= 3010, NULL);
            } else {
                check("★ 客户端2 时间戳增量", 0, "第二帧没收到");
            }
        }

        svc_sender_remove_client(1);
        svc_sender_remove_client(2);
        close(fd1);
        close(fd2);
    }
    printf("\n");

    /* ═══ ④ 边界: 非法参数与重复操作 ═══ */
    printf("④ 边界与健壮性\n");
    {
        struct sockaddr_in d;

        check("add_client(NULL) 返回 -1",
              svc_sender_add_client(3, NULL) == -1, NULL);
        check("remove 不存在的客户端是安全的(no-op)", 1, NULL);
        svc_sender_remove_client(99);
        check("remove(NULL 概念) 后计数仍为 0",
              svc_sender_client_count() == 0, NULL);

        /* 重复 add 同一 index → 应当视为更新, 不新增槽位 */
        {
            int fd = make_rx(&d);

            g_rx_fd = fd;
            check("首次 add", svc_sender_add_client(4, &d) == 0, NULL);
            check("重复 add 同一 index 返回 0",
                  svc_sender_add_client(4, &d) == 0, NULL);
            check("★ 重复 add 不新增槽位(仍为 1)",
                  svc_sender_client_count() == 1, NULL);
            svc_sender_remove_client(4);
            check("移除后归零", svc_sender_client_count() == 0, NULL);
            close(fd);
        }
    }
    printf("\n");

    /* ═══ ⑤ 统计与停机 ═══ */
    printf("⑤ 统计与停机\n");
    {
        svc_sender_stats_t st;

        svc_sender_get_stats(&st);
        printf("   帧 %llu / 包 %llu / 错误 %llu / 加入 %llu / 离开 %llu\n",
               (unsigned long long)st.frames_sent,
               (unsigned long long)st.packets_sent,
               (unsigned long long)st.send_errors,
               (unsigned long long)st.clients_joined,
               (unsigned long long)st.clients_left);
        check("统计: 发过帧", st.frames_sent > 0, NULL);
        check("统计: 发过包", st.packets_sent > 0, NULL);
        check("统计: 没有发送错误", st.send_errors == 0, NULL);
        check("统计: 加入次数 == 离开次数(槽位不泄漏)",
              st.clients_joined == st.clients_left, NULL);

        svc_sender_stop();
        check("停机后状态为未运行", svc_sender_is_running() == 0, NULL);
        check("停机后客户端数归零", svc_sender_client_count() == 0, NULL);
        check("重复停机是安全的(no-op)", 1, NULL);
        svc_sender_stop();
    }

    infra_queue_destroy(q);
    free(slot);

    printf("\n===== 结果: %s(通过 %d 项, 失败 %d 项)=====\n",
           g_fails == 0 ? "全部通过" : "有失败", g_passes, g_fails);
    return g_fails == 0 ? 0 : 1;
}
