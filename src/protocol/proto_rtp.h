/**
 * @file    rtp.h
 * @brief   RTP 打包 —— 把一个 NALU 切成 RTP 包并发送
 *
 * ─────────────────────────────────────────────────────────────────
 *  为什么需要分片?
 * ─────────────────────────────────────────────────────────────────
 *  实测: H.264 最大 NALU 55KB, H.265 最大 NALU 115KB
 *  而一个 UDP 包最多能带 ~1460 字节有效载荷。
 *  115KB 的关键帧必须切成 ~83 个 RTP 包。
 *
 * ─────────────────────────────────────────────────────────────────
 *  两种打包模式
 * ─────────────────────────────────────────────────────────────────
 *  ① 单 NALU 模式 (Single NALU Packet)
 *     当 NALU 长度 <= 最大载荷时, 一个 RTP 包装一个完整 NALU:
 *
 *        RTP 头 (12B) | NALU 原始字节(含 NALU 头, 不含起始码)
 *
 *  ② FU-A 分片模式 (Fragmentation Unit, RFC 6184 §5.8)
 *     当 NALU 太大时, 切成多片, 每片加 2 字节分片头:
 *
 *        RTP 头 (12B) | FU indicator (1B) | FU header (1B) | 分片数据
 *
 *        FU indicator:  F(1) NRI(2) Type(5)   Type 固定 = 28 表示 FU-A
 *        FU header:     S(1) E(1) R(1) 原始Type(5)
 *                         S=1 首片, E=1 末片, 中间片 S=E=0
 *
 *        接收端(此处为 VLC)据此把分片重组回完整 NALU。
 *
 * ─────────────────────────────────────────────────────────────────
 *  H.265 的差异 (RFC 7798)
 * ─────────────────────────────────────────────────────────────────
 *      - NALU 头是 2 字节
 *      - 分片时 PayloadHdr 的 Type 置为 49 表示 FU
 *      - FU header 是: S(1) E(1) FuType(6)
 *      - 因此每个分片剥掉 2 字节 PayloadHdr, 再补 3 字节(FU PayloadHdr 2B + FU header 1B)
 */
#ifndef __PROTO_RTP_H__
#define __PROTO_RTP_H__

#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>

#include "proto_nalu.h"

/** RTP 最大载荷(保守值, 避免 IP 分片) */
#define PROTO_RTP_MAX_PAYLOAD 1400

/** 动态载荷类型 (RTP 规范: 96~127 由应用自定义) */
#define PROTO_RTP_PT_H264 96
#define PROTO_RTP_PT_H265 97

/** RTP 固定头长度 */
#define PROTO_RTP_HEADER_LEN 12

/** RTP 时间戳时钟频率(视频标准 90kHz) */
#define PROTO_RTP_CLOCK_RATE 90000

/**
 * 一个 RTP 发送会话 —— 对应「一个客户端的一个媒体通道」。
 * 每个客户端/通道必须独立, 因为序列号和时间戳不能共用。
 */
typedef struct {
    uint16_t seq;           /* 序列号, 每发一个包 +1 */
    uint32_t ssrc;          /* 同步源标识, 初始化后固定 */
    uint32_t timestamp;     /* 时间戳, 同一帧的所有分片相同 */
    uint32_t ts_step;       /* 每帧时间戳增量 = 90000 / fps */
    uint8_t  pt;            /* 载荷类型 96=H.264 / 97=H.265 */
} proto_rtp_session_t;

/**
 * 初始化发送会话。
 * @param is_h265    1=H.265, 0=H.264
 * @param ssrc_seed  生成 SSRC 的种子(通常传时间或 pid)
 * @param fps        帧率, 用于计算时间戳步长
 */
void proto_rtp_session_init(proto_rtp_session_t *s, int is_h265,
                      uint32_t ssrc_seed, int fps);

/**
 * 帧边界: 递增时间戳。
 * 同一个视频帧的所有 NALU 共享一个时间戳, 换帧时调用本函数。
 */
void proto_rtp_session_next_frame(proto_rtp_session_t *s);

/**
 * 发送一个 NALU(自动决定单包还是 FU-A 分片)。
 *
 * @param sockfd    用于发送的 UDP socket
 * @param dst       目标地址
 * @param dstlen    地址长度
 * @param s         会话状态
 * @param n         要发送的 NALU(不含起始码)
 * @param is_last   该 NALU 是否是本帧的最后一个 —— 是则置 RTP marker 位
 * @return 已发送的 RTP 包个数; 负值为错误
 */
int proto_rtp_send_nalu(int sockfd, const struct sockaddr *dst, socklen_t dstlen,
                  proto_rtp_session_t *s, const proto_nalu_t *n, int is_last);

#endif /* __PROTO_RTP_H__ */
