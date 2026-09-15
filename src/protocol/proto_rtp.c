/**
 * @file    rtp.c
 * @brief   RTP 打包实现 —— 单 NALU 模式 + FU-A 分片模式
 */
#include "proto_rtp.h"

#include <string.h>
#include <unistd.h>

/* ── H.264 相关常量 ───────────────────────────────────────────── */
#define H264_FU_A_TYPE  28      /* FU-A 的 NALU 类型 */
#define H264_NRI_MASK   0x60    /* nal_ref_idc 位 */
#define H264_F_MASK     0x80    /* forbidden_zero_bit */

/* ── H.265 相关常量 ───────────────────────────────────────────── */
#define H265_FU_TYPE    49      /* H.265 FU 的 NALU 类型 */

/*
 * 发送缓冲改为各发送函数内的栈上局部变量(约 1.4KB, 远小于栈限制)。
 * 这样函数天然可重入 —— 多个发送线程可并发调用, 无需加锁。
 * 不用文件级静态: 静态缓冲会让本模块隐式变成「单线程才能用」, 是并发隐患。
 */

/**
 * 写 RTP 固定头(12 字节)。
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |V=2|P|X|  CC   |M|     PT      |       sequence number         |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                           timestamp                           |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                             SSRC                              |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
static void rtp_write_header(uint8_t *p, const proto_rtp_session_t *s, int marker)
{
    p[0] = 0x80;                    /* V=2, P=0, X=0, CC=0 */
    p[1] = (uint8_t)((marker ? 0x80 : 0x00) | (s->pt & 0x7F));
    p[2] = (uint8_t)(s->seq >> 8);
    p[3] = (uint8_t)(s->seq);
    p[4] = (uint8_t)(s->timestamp >> 24);
    p[5] = (uint8_t)(s->timestamp >> 16);
    p[6] = (uint8_t)(s->timestamp >> 8);
    p[7] = (uint8_t)(s->timestamp);
    p[8] = (uint8_t)(s->ssrc >> 24);
    p[9] = (uint8_t)(s->ssrc >> 16);
    p[10] = (uint8_t)(s->ssrc >> 8);
    p[11] = (uint8_t)(s->ssrc);
}

/* ─────────────────────── 单 NALU 模式 ─────────────────────── */

static int send_single(int fd, const struct sockaddr *dst, socklen_t dstlen,
                       proto_rtp_session_t *s, const proto_nalu_t *n, int marker)
{
    uint8_t pkt[PROTO_RTP_HEADER_LEN + 3 + PROTO_RTP_MAX_PAYLOAD];
    ssize_t sent;

    rtp_write_header(pkt, s, marker);
    memcpy(pkt + PROTO_RTP_HEADER_LEN, n->data, n->len);

    sent = sendto(fd, pkt, PROTO_RTP_HEADER_LEN + n->len, 0, dst, dstlen);
    if (sent < 0)
        return -1;

    s->seq++;
    return 1;
}

/* ─────────────────────── H.264 FU-A 分片 ─────────────────────── */

static int send_fua_h264(int fd, const struct sockaddr *dst, socklen_t dstlen,
                         proto_rtp_session_t *s, const proto_nalu_t *n, int marker)
{
    uint8_t pkt[PROTO_RTP_HEADER_LEN + 3 + PROTO_RTP_MAX_PAYLOAD];
    const uint8_t *payload = n->data + 1;       /* 跳过 1 字节 NALU 头 */
    size_t         remain  = n->len - 1;
    const size_t   maxfrag = PROTO_RTP_MAX_PAYLOAD - 2;   /* 减去 2 字节 FU 头 */
    uint8_t        nalu_hdr = n->data[0];
    int            npkt = 0;
    int            first = 1;

    while (remain > 0) {
        size_t frag = (remain > maxfrag) ? maxfrag : remain;
        int    last = (frag == remain);
        ssize_t sent;

        rtp_write_header(pkt, s, (last && marker) ? 1 : 0);

        /* FU indicator: 保留 F 和 NRI, Type 换成 28 */
        pkt[PROTO_RTP_HEADER_LEN] = (uint8_t)((nalu_hdr & (H264_F_MASK | H264_NRI_MASK))
                                             | H264_FU_A_TYPE);
        /* FU header: S / E / 原始 Type(低 5 bit) */
        pkt[PROTO_RTP_HEADER_LEN + 1] = (uint8_t)((first ? 0x80 : 0x00)
                                                 | (last ? 0x40 : 0x00)
                                                 | (nalu_hdr & 0x1F));
        memcpy(pkt + PROTO_RTP_HEADER_LEN + 2, payload, frag);

        sent = sendto(fd, pkt, PROTO_RTP_HEADER_LEN + 2 + frag, 0, dst, dstlen);
        if (sent < 0)
            return (npkt > 0) ? npkt : -1;

        s->seq++;
        npkt++;
        payload += frag;
        remain  -= frag;
        first    = 0;
    }

    return npkt;
}

/* ─────────────────────── H.265 FU 分片 ─────────────────────── */

static int send_fu_h265(int fd, const struct sockaddr *dst, socklen_t dstlen,
                        proto_rtp_session_t *s, const proto_nalu_t *n, int marker)
{
    uint8_t pkt[PROTO_RTP_HEADER_LEN + 3 + PROTO_RTP_MAX_PAYLOAD];
    const uint8_t *payload = n->data + 2;       /* 跳过 2 字节 PayloadHdr */
    size_t         remain  = n->len - 2;
    const size_t   maxfrag = PROTO_RTP_MAX_PAYLOAD - 3;   /* 减去 3 字节 FU 头 */
    uint8_t        hdr0 = n->data[0];
    uint8_t        hdr1 = n->data[1];
    uint8_t        fu_type = (uint8_t)((hdr0 >> 1) & 0x3F);   /* 原始 NALU 类型 */
    int            npkt = 0;
    int            first = 1;

    while (remain > 0) {
        size_t frag = (remain > maxfrag) ? maxfrag : remain;
        int    last = (frag == remain);
        ssize_t sent;

        rtp_write_header(pkt, s, (last && marker) ? 1 : 0);

        /*
         * FU 的 PayloadHdr: 保持 F 和 LayerId 高位, Type 换成 49
         *   byte0 = F(1) | Type(6) | LayerId高位(1)
         *   byte1 = LayerId低5位(5) | TID(3)   —— 原样保留
         */
        pkt[PROTO_RTP_HEADER_LEN]     = (uint8_t)((hdr0 & 0x81) | (H265_FU_TYPE << 1));
        pkt[PROTO_RTP_HEADER_LEN + 1] = hdr1;
        /* FU header: S(1) E(1) FuType(6) */
        pkt[PROTO_RTP_HEADER_LEN + 2] = (uint8_t)((first ? 0x80 : 0x00)
                                                 | (last ? 0x40 : 0x00)
                                                 | (fu_type & 0x3F));
        memcpy(pkt + PROTO_RTP_HEADER_LEN + 3, payload, frag);

        sent = sendto(fd, pkt, PROTO_RTP_HEADER_LEN + 3 + frag, 0, dst, dstlen);
        if (sent < 0)
            return (npkt > 0) ? npkt : -1;

        s->seq++;
        npkt++;
        payload += frag;
        remain  -= frag;
        first    = 0;
    }

    return npkt;
}

/* ─────────────────────── 对外接口 ─────────────────────── */

void proto_rtp_session_init(proto_rtp_session_t *s, int is_h265,
                      uint32_t ssrc_seed, int fps)
{
    memset(s, 0, sizeof(*s));

    /*
     * 序列号与 SSRC 都取随机初值。
     * RFC 3550 要求随机化 —— 避免同一 SSRC 在不同会话间串号。
     */
    s->seq       = (uint16_t)(ssrc_seed * 7919u);       /* 简单伪随机 */
    s->ssrc      = ssrc_seed * 2654435761u;             /* Knuth 乘法散列 */
    s->timestamp = (uint32_t)(ssrc_seed * 1103515245u);
    s->pt        = (uint8_t)(is_h265 ? PROTO_RTP_PT_H265 : PROTO_RTP_PT_H264);
    s->ts_step   = (fps > 0) ? (uint32_t)(PROTO_RTP_CLOCK_RATE / fps)
                             : (uint32_t)(PROTO_RTP_CLOCK_RATE / 30);
}

void proto_rtp_session_next_frame(proto_rtp_session_t *s)
{
    s->timestamp += s->ts_step;
}

void proto_rtp_session_frame_pts(proto_rtp_session_t *s, uint64_t pts)
{
    uint64_t delta;

    if (s == NULL)
        return;

    /*
     * 第一次调用: 只记下基准 PTS, **不动时间戳**。
     * 为什么不在首帧就把它换算成时间戳: RTP 时间戳的起点应当是随机的
     * (RFC 3550), 而且首帧的绝对 PTS 可能是个很大的数(实测 ~39 亿),
     * 直接换算会把起点钉死在某个可预测的值上。从第二帧起用**差值**推进。
     */
    if (s->last_pts == 0) {
        s->last_pts = pts;
        return;
    }

    /* PTS 倒退或持平: 异常(或同一帧被调两次) → 不动时间戳, 保持单调 */
    if (pts <= s->last_pts)
        return;

    delta = pts - s->last_pts;
    s->last_pts = pts;

    /*
     * 1MHz → 90kHz: × 90000 / 1000000 = / 100。
     *
     * ⚠️ 先乘后除会溢出吗: delta 是"两帧之差"(实测 33333), 乘 90000
     *    约 3×10^9, 在 uint64_t 里绰绰有余。即便 delta 大到 10^10
     *    (约 2.8 小时的两帧间隔, 不现实)也不会溢出。
     *    所以这里用 `delta * PROTO_RTP_CLOCK_RATE / PROTO_RTP_PTS_HZ`
     *    保留精度, 比先除后乘准确。
     */
    s->timestamp += (uint32_t)(delta * PROTO_RTP_CLOCK_RATE / PROTO_RTP_PTS_HZ);
}

int proto_rtp_send_nalu(int sockfd, const struct sockaddr *dst, socklen_t dstlen,
                  proto_rtp_session_t *s, const proto_nalu_t *n, int is_last)
{
    size_t hdr_len;

    if (n == NULL || n->len == 0)
        return 0;

    /* NALU 头本身要占位, 太短说明数据异常 */
    hdr_len = n->is_h265 ? 2u : 1u;
    if (n->len <= hdr_len)
        return 0;

    /*
     * 不分片的上限就是载荷上限(1400)。
     * 只要 NALU 塞得进一个包, 就走单包模式 —— 少 2 字节分片头, 也少一层状态。
     * 只有大 NALU 才需要分片: 实测关键帧 55KB(H.264)/ 115KB(H.265)。
     */
    if (n->len <= PROTO_RTP_MAX_PAYLOAD)
        return send_single(sockfd, dst, dstlen, s, n, is_last);

    if (n->is_h265)
        return send_fu_h265(sockfd, dst, dstlen, s, n, is_last);

    return send_fua_h264(sockfd, dst, dstlen, s, n, is_last);
}
