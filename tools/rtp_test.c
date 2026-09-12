/**
 * @file    rtp_test.c
 * @brief   RTP 打包单元测试 —— 本机 UDP 自发自收, 重组后与原始 NALU 逐字节比对
 *
 * 为什么要在本机测?
 *   FU-A 分片逻辑一旦写错(起始标志位、类型回填、头长度),
 *   表现只是「VLC 花屏」或「无法播放」, 极难反查。
 *   本机自发自收可以把「打包 + 重组」闭环验证, 排除网络因素。
 *
 * 用法: ./rtp_test <file.h264|file.h265>
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "proto_nalu.h"
#include "proto_rtp.h"

/* ── 测试统计 ─────────────────────────────────────────────────── */
typedef struct {
    int nalus_total;
    int nalus_single;       /* 单包发送的 */
    int nalus_fragmented;   /* 分片发送的 */
    int pkts_total;
    int pkts_max_frag;      /* 单个 NALU 最多切成几片 */
    int verify_ok;
    int verify_fail;
    int first_fail_idx;
} stats_t;

/* ── 重组上下文 ───────────────────────────────────────────────── */
typedef struct {
    uint8_t  buf[PROTO_NALU_MAX_SIZE];
    size_t   len;
    int      started;       /* 是否已收到首片 */
    int      is_h265;
} reasm_t;

/**
 * 从 RTP 包里取出媒体载荷并做重组。
 * 返回: 1 = 一个完整 NALU 已重组完成(在 r->buf/r->len)
 *       0 = 还需要更多分片
 *      -1 = 格式错误
 */
static int rtp_reassemble(const uint8_t *pkt, size_t plen, reasm_t *r)
{
    const uint8_t *payload;
    size_t         paylen;
    uint8_t        b0;

    if (plen < PROTO_RTP_HEADER_LEN)
        return -1;

    /* 只接受 RTP version=2 */
    if ((pkt[0] >> 6) != 2)
        return -1;

    payload = pkt + PROTO_RTP_HEADER_LEN;
    paylen  = plen - PROTO_RTP_HEADER_LEN;
    if (paylen == 0)
        return -1;

    b0 = payload[0];

    if (!r->is_h265) {
        /* ── H.264 ── */
        if ((b0 & 0x1F) == 28) {                    /* FU-A */
            uint8_t fu_hdr;
            int     s_bit, e_bit, orig_type;

            if (paylen < 2)
                return -1;

            fu_hdr    = payload[1];
            s_bit     = (fu_hdr & 0x80) ? 1 : 0;
            e_bit     = (fu_hdr & 0x40) ? 1 : 0;
            orig_type = fu_hdr & 0x1F;

            if (s_bit) {
                /* 用 FU indicator 的 F/NRI + FU header 的原始 type 重建 NALU 头 */
                r->buf[0] = (uint8_t)((b0 & 0xE0) | orig_type);
                r->len    = 1;
                r->started = 1;
            } else if (!r->started) {
                return -1;                          /* 丢了首片 */
            }

            if (r->len + (paylen - 2) > PROTO_NALU_MAX_SIZE)
                return -1;

            memcpy(r->buf + r->len, payload + 2, paylen - 2);
            r->len += paylen - 2;

            return e_bit ? 1 : 0;
        }

        /* 单 NALU 模式 */
        if (paylen > PROTO_NALU_MAX_SIZE)
            return -1;
        memcpy(r->buf, payload, paylen);
        r->len     = paylen;
        r->started = 1;
        return 1;
    }

    /* ── H.265 ── */
    if (((b0 >> 1) & 0x3F) == 49) {                 /* FU */
        uint8_t b1, fu_hdr;
        int     s_bit, e_bit, orig_type;

        if (paylen < 3)
            return -1;

        b1        = payload[1];
        fu_hdr    = payload[2];
        s_bit     = (fu_hdr & 0x80) ? 1 : 0;
        e_bit     = (fu_hdr & 0x40) ? 1 : 0;
        orig_type = fu_hdr & 0x3F;

        if (s_bit) {
            /* 重建 2 字节 PayloadHdr: 保留 F 与 LayerId, Type 换回原始值 */
            r->buf[0] = (uint8_t)((b0 & 0x81) | (orig_type << 1));
            r->buf[1] = b1;
            r->len    = 2;
            r->started = 1;
        } else if (!r->started) {
            return -1;
        }

        if (r->len + (paylen - 3) > PROTO_NALU_MAX_SIZE)
            return -1;

        memcpy(r->buf + r->len, payload + 3, paylen - 3);
        r->len += paylen - 3;

        return e_bit ? 1 : 0;
    }

    /* 单 NALU 模式 */
    if (paylen > PROTO_NALU_MAX_SIZE)
        return -1;
    memcpy(r->buf, payload, paylen);
    r->len     = paylen;
    r->started = 1;
    return 1;
}

/* ── 测试主逻辑 ───────────────────────────────────────────────── */

static int   g_sockfd;
static struct sockaddr_in g_dst;
static proto_rtp_session_t      g_sess;
static stats_t            g_stats;
static reasm_t            g_reasm;
static int                g_nalu_idx;

static int on_nalu(const proto_nalu_t *n, void *user)
{
    int      npkt;
    int      i;

    (void)user;

    /* 一帧的第一个 NALU 前推进时间戳 —— 这里简化: 每个 NALU 当作独立帧 */
    proto_rtp_session_next_frame(&g_sess);

    g_stats.nalus_total++;

    npkt = proto_rtp_send_nalu(g_sockfd, (struct sockaddr *)&g_dst, sizeof(g_dst),
                         &g_sess, n, 1 /* marker */);
    if (npkt < 0) {
        printf("  [错误] NALU[%d] 发送失败\n", g_nalu_idx);
        return 1;
    }

    g_stats.pkts_total += npkt;
    if (npkt == 1)
        g_stats.nalus_single++;
    else
        g_stats.nalus_fragmented++;
    if (npkt > g_stats.pkts_max_frag)
        g_stats.pkts_max_frag = npkt;

    /* 收包并重组 */
    memset(&g_reasm, 0, sizeof(g_reasm));
    g_reasm.is_h265 = n->is_h265;

    for (i = 0; i < npkt; i++) {
        uint8_t pkt[PROTO_RTP_HEADER_LEN + 3 + PROTO_RTP_MAX_PAYLOAD];
        ssize_t rlen = recv(g_sockfd, pkt, sizeof(pkt), 0);
        int     done;

        if (rlen <= 0) {
            printf("  [错误] NALU[%d] 第 %d 个包接收失败\n", g_nalu_idx, i);
            g_stats.verify_fail++;
            goto next;
        }

        done = rtp_reassemble(pkt, (size_t)rlen, &g_reasm);
        if (done < 0) {
            printf("  [错误] NALU[%d] 第 %d 个包格式异常\n", g_nalu_idx, i);
            g_stats.verify_fail++;
            goto next;
        }

        if (done == 1) {
            /* 重组完成, 与原始 NALU 比对 */
            if (g_reasm.len == n->len &&
                memcmp(g_reasm.buf, n->data, n->len) == 0) {
                g_stats.verify_ok++;
            } else {
                g_stats.verify_fail++;
                if (g_stats.first_fail_idx < 0)
                    g_stats.first_fail_idx = g_nalu_idx;
                printf("  [失败] NALU[%d] 重组不一致: 原始 %zu 字节, 重组 %zu 字节\n",
                       g_nalu_idx, n->len, g_reasm.len);
            }
        }
    }

next:
    g_nalu_idx++;
    return 0;
}

int main(int argc, char **argv)
{
    FILE        *fp;
    uint8_t     *buf;
    long         size;
    int          is_h265;
    struct sockaddr_in self;
    socklen_t    slen = sizeof(self);
    int          n;
    int          opt = 1;

    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.h264|file.h265>\n", argv[0]);
        return 1;
    }

    is_h265 = (strstr(argv[1], ".h265") != NULL);

    /* 1) 读文件 */
    fp = fopen(argv[1], "rb");
    if (fp == NULL) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "读文件失败\n"); return 1;
    }
    fclose(fp);

    /* 2) 建 UDP socket 并绑定到本机随机端口 */
    g_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sockfd < 0) { perror("socket"); return 1; }

    setsockopt(g_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&self, 0, sizeof(self));
    self.sin_family      = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    self.sin_port        = 0;                       /* 让内核分配 */
    if (bind(g_sockfd, (struct sockaddr *)&self, sizeof(self)) < 0) {
        perror("bind"); return 1;
    }
    if (getsockname(g_sockfd, (struct sockaddr *)&self, &slen) < 0) {
        perror("getsockname"); return 1;
    }

    g_dst = self;                                   /* 发给自己 */

    /* 设个接收超时, 免得收不到包时永久阻塞 */
    {
        struct timeval tv;
        tv.tv_sec  = 2;
        tv.tv_usec = 0;
        setsockopt(g_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* 3) 初始化会话 */
    proto_rtp_session_init(&g_sess, is_h265, (uint32_t)getpid(), 30);

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.first_fail_idx = -1;
    g_nalu_idx = 0;

    printf("===== RTP 打包 / 重组 单元测试 =====\n");
    printf("码流文件 : %s (%ld 字节, %s)\n", argv[1], size,
           is_h265 ? "H.265" : "H.264");
    printf("本地端口 : %u\n\n", ntohs(self.sin_port));

    /* 4) 逐 NALU 打包 → 自发自收 → 重组比对 */
    n = proto_nalu_foreach(buf, (size_t)size, is_h265, on_nalu, NULL);

    printf("\n----- 结果 -----\n");
    printf("NALU 总数        : %d\n", n);
    printf("  单包发送       : %d\n", g_stats.nalus_single);
    printf("  FU-A 分片发送  : %d\n", g_stats.nalus_fragmented);
    printf("RTP 包总数       : %d\n", g_stats.pkts_total);
    printf("单 NALU 最多分片 : %d 片\n", g_stats.pkts_max_frag);
    printf("重组校验通过     : %d\n", g_stats.verify_ok);
    printf("重组校验失败     : %d\n", g_stats.verify_fail);
    if (g_stats.first_fail_idx >= 0)
        printf("首个失败于 NALU  : %d\n", g_stats.first_fail_idx);
    printf("\n%s\n", (g_stats.verify_fail == 0 && g_stats.verify_ok == n)
                     ? ">>> PASS: 全部 NALU 打包→重组 逐字节一致 <<<"
                     : ">>> FAIL: 存在不一致 <<<");

    free(buf);
    close(g_sockfd);
    return (g_stats.verify_fail == 0) ? 0 : 1;
}
