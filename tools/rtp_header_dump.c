/**
 * @file    rtp_header_dump.c
 * @brief   把 proto_rtp.c 真正生成的 RTP 包抓下来, 逐字段解码打印。
 *
 * 【为什么需要这个工具】
 *   `rtp_test.c` 只验证"重组后和原始 NALU 逐字节一致", 证明了代码对,
 *   但**看不到那 12 个字节里到底填了什么**。
 *   而面试/调试真正要能说清的, 恰恰是"每个字段为什么是这个值"。
 *   本工具把真实发送的包头解出来 —— 用实物讲协议, 不靠背表。
 *
 * 【原理】
 *   用 UDP 自发自收: proto_rtp_send_nalu() 发到 127.0.0.1, recv() 收回来,
 *   然后按 RFC 3550 的位布局把 12 字节拆成字段。
 *   全程不碰硬件、不碰网络设备 —— 这就是 protocol 层"不碰硬件"的价值。
 *
 * 编译(主机):
 *   gcc -Wall -Wextra -O2 -o rtp_header_dump rtp_header_dump.c \
 *       ../src/protocol/proto_rtp.c ../src/protocol/proto_nalu.c -I../src/protocol
 *
 * 用法:
 *   ./rtp_header_dump <file.h264|file.h265> [要打印的包数, 默认 12]
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "proto_nalu.h"
#include "proto_rtp.h"

#define PRINT_MAX 4096

static int      g_sockfd;
static struct sockaddr_in g_dst;
static proto_rtp_session_t g_sess;
static int      g_print_budget = 12;      /* 还能打印几个包 */
static int      g_pkt_index;              /* 全局包序号 */
static int      g_nalu_index;
static int      g_last_frame_ts = -1;     /* 用来演示"同一帧时间戳相同" */
static const char *g_pt_name = "?";

/* ── 字段名表(把位域翻译成人话)────────────────────────────────── */

static const char *h264_type_name(uint8_t t)
{
    switch (t) {
    case 1:  return "普通帧切片";
    case 5:  return "IDR 关键帧";
    case 6:  return "SEI";
    case 7:  return "SPS";
    case 8:  return "PPS";
    case 9:  return "AUD";
    case 28: return "FU-A 分片";
    default: return "其他";
    }
}

static const char *h265_type_name(uint8_t t)
{
    switch (t) {
    case 0:  return "普通帧切片";
    case 19: return "IDR_W_RADL 关键帧";
    case 20: return "IDR_N_LP 关键帧";
    case 32: return "VPS";
    case 33: return "SPS";
    case 34: return "PPS";
    case 35: return "AUD";
    case 39: return "SEI(前缀)";
    case 40: return "SEI(后缀)";
    case 49: return "FU 分片";
    default: return "其他";
    }
}

/**
 * 解码并打印一个 RTP 包的固定头(12 字节)。
 *
 * @param pkt   收到的完整 RTP 包
 * @param len   包长度
 * @param is_h265  1=H.265, 0=H.264(影响载荷类型名字的翻译)
 */
static void dump_packet(const uint8_t *pkt, size_t len, int is_h265)
{
    uint8_t  b0        = pkt[0];
    uint8_t  b1        = pkt[1];
    uint8_t  v         = (b0 >> 6) & 0x03;   /* 版本 */
    uint8_t  p_bit     = (b0 >> 5) & 0x01;   /* 填充位 */
    uint8_t  x_bit     = (b0 >> 4) & 0x01;   /* 扩展位 */
    uint8_t  cc        = b0 & 0x0F;          /* CSRC 个数 */
    uint8_t  marker    = (b1 >> 7) & 0x01;   /* 帧边界标志 */
    uint8_t  pt        = b1 & 0x7F;          /* 载荷类型 */
    uint16_t seq       = (uint16_t)((pkt[2] << 8) | pkt[3]);
    uint32_t ts        = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                         ((uint32_t)pkt[6] << 8)  | (uint32_t)pkt[7];
    uint32_t ssrc      = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) |
                         ((uint32_t)pkt[10] << 8) | (uint32_t)pkt[11];

    printf("── 包 #%-4d 长度 %zu 字节 ────────────────────────────────────\n",
           g_pkt_index, len);
    printf("   原始 12 字节: ");
    for (int i = 0; i < 12; i++)
        printf("%02X ", pkt[i]);
    printf("\n");

    printf("   ┌─ 字节 0: 0x%02X ──────────────────────────────────────\n", b0);
    printf("   │   V (版本)      = %u    %s\n", v, v == 2 ? "(RTP 固定为 2 ✅)" : "(应为 2!)");
    printf("   │   P (填充位)    = %u    %s\n", p_bit, p_bit ? "包尾有填充" : "无填充");
    printf("   │   X (扩展位)    = %u    %s\n", x_bit, x_bit ? "有扩展头" : "无扩展头");
    printf("   │   CC(CSRC 个数) = %u    %s\n", cc, cc ? "有贡献源列表" : "无贡献源(单源流)");
    printf("   │   代码里写的是 p[0] = 0x%02X → V=2,P=0,X=0,CC=0\n", 0x80);

    printf("   ├─ 字节 1: 0x%02X ──────────────────────────────────────\n", b1);
    printf("   │   M (Marker)    = %u    %s\n", marker,
           marker ? "★ 本帧最后一个包 → 告诉接收端「可以送解码了」"
                  : "本帧还有后续包");
    printf("   │   PT(载荷类型)  = %u    %s\n", pt, g_pt_name);

    printf("   ├─ 字节 2-3: 0x%04X ────────────────────────────────────\n", seq);
    printf("   │   序列号        = %u   (每发 1 个包 +1;接收端靠它发现丢包/乱序)\n", seq);

    printf("   ├─ 字节 4-7: 0x%08X ───────────────────────────────\n", ts);
    printf("   │   时间戳        = %u\n", ts);
    printf("   │   换算成秒      = %u / 90000 = %.4f 秒\n", ts, (double)ts / 90000.0);
    if (g_last_frame_ts == (int)ts)
        printf("   │   ★ 和上一个包【相同】→ 证明它们属于同一帧(同一帧所有分片 ts 一样)\n");
    else
        printf("   │   (和上一个包不同 → 进入了新的一帧)\n");

    printf("   └─ 字节 8-11: 0x%08X ──────────────────────────────\n", ssrc);
    printf("       SSRC(同步源)  = %u   (标识「我是哪一路流」;本会话固定不变)\n", ssrc);

    /* 载荷开头 —— 看打包模式 */
    if (len > PROTO_RTP_HEADER_LEN) {
        const uint8_t *pl = pkt + PROTO_RTP_HEADER_LEN;
        size_t pl_len = len - PROTO_RTP_HEADER_LEN;

        printf("   载荷前几字节  : ");
        for (size_t i = 0; i < pl_len && i < 8; i++)
            printf("%02X ", pl[i]);
        printf(" (%zu 字节)\n", pl_len);

        if (is_h265) {
            uint8_t t = (pl[0] >> 1) & 0x3F;
            printf("   └ 载荷第 1-2 字节是 NALU 头(H.265 头 2 字节), Type=%u → %s\n",
                   t, h265_type_name(t));
        } else {
            uint8_t t = pl[0] & 0x1F;
            printf("   └ 载荷第 1 字节是 NALU 头(H.264 头 1 字节), Type=%u → %s\n",
                   t, h264_type_name(t));
        }
    }
    printf("\n");
    g_last_frame_ts = (int)ts;
}

/* ── 发送 + 抓包 ─────────────────────────────────────────────── */

static int on_nalu(const proto_nalu_t *n, void *user)
{
    int npkt;
    int i;

    (void)user;

    /* 每个 NALU 当作独立帧推进时间戳(和 rtp_test.c 一致, 便于对照) */
    proto_rtp_session_next_frame(&g_sess);

    npkt = proto_rtp_send_nalu(g_sockfd, (struct sockaddr *)&g_dst, sizeof(g_dst),
                               &g_sess, n, 1 /* marker: 本 NALU 即本帧全部 */);
    if (npkt < 0) {
        printf("  [错误] NALU[%d] 发送失败\n", g_nalu_index);
        return 1;
    }

    for (i = 0; i < npkt; i++) {
        uint8_t pkt[PROTO_RTP_HEADER_LEN + 3 + PROTO_RTP_MAX_PAYLOAD];
        ssize_t rlen = recv(g_sockfd, pkt, sizeof(pkt), 0);

        if (rlen <= 0) {
            printf("  [错误] NALU[%d] 第 %d 包接收失败\n", g_nalu_index, i);
            break;
        }
        g_pkt_index++;

        if (g_print_budget > 0) {
            printf("[NALU %d / %zu 字节 / 共 %d 个 RTP 包] 第 %d 片\n",
                   g_nalu_index, n->len, npkt, i + 1);
            dump_packet(pkt, (size_t)rlen, n->is_h265);
            g_print_budget--;
            if (g_print_budget == 0)
                printf("===== 打印额度用完,后续包只统计不打印 =====\n\n");
        }
        /* 收完必须继续把剩下的包收掉, 否则缓冲区里残留会干扰下一个 NALU */
    }

    g_nalu_index++;
    return 0;
}

int main(int argc, char **argv)
{
    const char *path;
    uint8_t    *buf;
    long        size;
    FILE       *fp;
    int         is_h265;
    int         n;
    struct sockaddr_in self;
    socklen_t   slen = sizeof(self);

    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.h264|file.h265> [打印包数]\n", argv[0]);
        return 1;
    }
    path = argv[1];
    if (argc >= 3)
        g_print_budget = atoi(argv[2]);

    is_h265 = (strstr(path, ".h265") != NULL);
    g_pt_name = is_h265 ? "97 = H.265(本项目自定义)" : "96 = H.264(本项目自定义)";

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "读文件失败: %s\n", path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "读文件内容失败\n");
        fclose(fp);
        free(buf);
        return 1;
    }
    fclose(fp);

    g_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sockfd < 0) {
        perror("socket");
        free(buf);
        return 1;
    }
    memset(&self, 0, sizeof(self));
    self.sin_family      = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    self.sin_port        = 0;                      /* 让内核选端口 */
    if (bind(g_sockfd, (struct sockaddr *)&self, sizeof(self)) < 0) {
        perror("bind");
        close(g_sockfd);
        free(buf);
        return 1;
    }
    if (getsockname(g_sockfd, (struct sockaddr *)&self, &slen) < 0) {
        perror("getsockname");
        close(g_sockfd);
        free(buf);
        return 1;
    }
    g_dst = self;                                  /* 发给自己 */

    proto_rtp_session_init(&g_sess, is_h265, 0x1234, 30);

    printf("===== RTP 12 字节头 逐字段解码 =====\n");
    printf("码流文件 : %s (%ld 字节, %s)\n", path, size, is_h265 ? "H.265" : "H.264");
    printf("本地端口 : %u(自发自收)\n", ntohs(self.sin_port));
    printf("会话初值 : SSRC 种子=0x1234 → SSRC=%u, PT=%u, ts_step=%u(=90000/30)\n",
           g_sess.ssrc, g_sess.pt, g_sess.ts_step);
    printf("打印额度 : 前 %d 个包\n\n", g_print_budget);

    n = proto_nalu_foreach(buf, (size_t)size, is_h265, on_nalu, NULL);
    printf("\n----- 统计 -----\n");
    printf("NALU 总数 : %d\n", n);
    printf("RTP 包总数: %d\n", g_pkt_index);

    close(g_sockfd);
    free(buf);
    return 0;
}
