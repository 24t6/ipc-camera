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
 * 海思 VENC 的 PTS 时基(**实测值, 不是抄来的**)。
 *
 * @note ⚠️ 这个 1 MHz 是**板上实测反推**出来的, 不是"一般说法":
 *       · 相邻帧 PTS 间隔实测 33323~33343(标称 33333), 极稳, 偏离率 0.00%
 *       · 按 33333 × 30fps ≈ 999,990 → **1 MHz**
 *       · 而一般资料说的 90kHz 在这里是**错的**(90kHz/30fps 应是 3000, 差 11 倍)
 *       实测方法见 `ipc_camera/tools/media_smoke.c`(它会把标称间隔打出来,
 *       并用 `间隔 × 30fps` 反推时基, 还断言结果落在 1MHz 附近)。
 *
 * @note 所以 PTS → RTP 时间戳**必须换算**, 不能直接相加:
 *           rtp_ts = pts / 100        (1MHz → 90kHz, 即 × 90000 / 1000000)
 *       直接用 PTS 会让时间戳快 11 倍, 客户端按这个时间戳播放会**加速 11 倍**。
 */
#define PROTO_RTP_PTS_HZ 1000000u

/**
 * 一个 RTP 发送会话 —— 对应「一个客户端的一个媒体通道」。
 * 每个客户端/通道必须独立, 因为序列号和时间戳不能共用。
 */
typedef struct {
    uint16_t seq;           /* 序列号, 每发一个包 +1 */
    uint32_t ssrc;          /* 同步源标识, 初始化后固定 */
    uint32_t timestamp;     /* 时间戳, 同一帧的所有分片相同 */
    uint32_t ts_step;       /* 每帧时间戳增量 = 90000 / fps(**旧路径用**, 见下) */
    uint8_t  pt;            /* 载荷类型 96=H.264 / 97=H.265 */

    /**
     * 上一帧的编码器 PTS(1MHz 单位)。**新路径用**。
     *
     * @note 为什么两条路径并存(2026-09-15):
     *       `proto_rtp_session_next_frame()` 是"**假设帧率**"的做法
     *       (90000/fps 硬加), 它不知道编码器实际什么时候出帧 ——
     *       这就是 B012 记录的问题。
     *       新加的 `proto_rtp_session_frame_pts()` 用**编码器给的 PTS** 换算,
     *       时间戳因此永远和真实帧时刻一致。
     *       老函数**故意保留不动**: 它已经被 `rtp_test` 验到逐字节一致
     *       (H.264 809 / H.265 835 个包), 动它有回归风险; 而新路径只需
     *       额外验证, 两者互不影响。
     */
    uint64_t last_pts;
} proto_rtp_session_t;

/**
 * @brief 初始化发送会话。
 * @param is_h265    1=H.265, 0=H.264
 * @param ssrc_seed  生成 SSRC 的种子(通常传时间或 pid)
 * @param fps        帧率, 用于计算时间戳步长
 */
void proto_rtp_session_init(proto_rtp_session_t *s, int is_h265,
                      uint32_t ssrc_seed, int fps);

/**
 * @brief 帧边界: 递增时间戳(**按假设帧率**, 旧路径)。
 *
 * @deprecated 优先用 `proto_rtp_session_frame_pts()`。
 *   本函数按 `90000/fps` 硬加, 它**不知道编码器实际什么时候出帧** ——
 *   帧率若是变的(或编码器实际不是标称帧率), 时间戳就会漂。
 *   保留它是因为已被 `rtp_test` 验到逐字节一致, 且单测仍在用。
 */
void proto_rtp_session_next_frame(proto_rtp_session_t *s);

/**
 * @brief 帧边界: 用**编码器给的时间戳**推进(**新路径**, 解 B012)。
 *
 * @param s   会话状态
 * @param pts 本帧的编码器时间戳(`bsp_mpp_frame_t.pts`, 单位见
 *            `PROTO_RTP_PTS_HZ` —— 实测本板是 **1 MHz**)
 *
 * @note **为什么必须用编码器的 PTS, 而不是自己数帧**:
 *       自己数 = 假设"每帧正好间隔 1/fps 秒"。而实际:
 *         · 编码器可能达不到标称帧率(我们的板子实测墙钟只有 ~20fps)
 *         · 帧率可能中途变化
 *         · 首帧时刻无从得知
 *       用编码器 PTS, 时间戳就**永远和真实帧时刻一致**, 客户端据此
 *       做音视频同步/播放节奏才是对的。这是 B012 的正解。
 *
 * @note 换算: `rtp_ts = pts / 100`(1MHz → 90kHz)。
 *       **不是**直接把 pts 赋给时间戳 —— 那会让时间戳快 11 倍,
 *       客户端会加速 11 倍播放。
 *
 * @note 用**相邻 PTS 之差**来推进(而不是把 PTS 绝对值当时间戳):
 *       这样 RTP 时间戳的起点仍是随机的(RFC 3550 要求), 但**帧间隔准确**。
 *       差值为 0(同一帧被调了两次)或倒退(异常)时本函数**不动时间戳**,
 *       这是刻意的: 宁可保持原值, 也不要把时间戳搞乱。
 */
void proto_rtp_session_frame_pts(proto_rtp_session_t *s, uint64_t pts);

/**
 * 发一个 RTP 包的动作 —— **打包与传输解耦的关键**。
 *
 * @param user 透传(调用方的上下文)
 * @param pkt  完整的 RTP 包(12 字节头 + 载荷)
 * @param len  字节数
 * @return 0 成功; 负值失败(打包立即中止并返回错误)
 *
 * @note 为什么要这个回调(2026-09-18): 原来 `proto_rtp_send_nalu()` **自己
 *       `sendto`**, 于是"传输方式"被**焊死**在协议层里 ——
 *       `svc_sender` 里新建的 TCP 交错 sender 因此**从来不被调用**
 *       (客户端勾"以 TCP 播放"就一帧都发不出去)。把"发一个包"抽成回调之后,
 *       协议层只管**打包**, 走 UDP 还是走 TCP 交错由上层决定。
 * @note 副作用是好的: 协议层从此**不碰 socket**, 可以纯 PC 单测。
 */
typedef int proto_rtp_pkt_fn(void *user, const uint8_t *pkt, size_t len);

/**
 * @brief 把一个 NALU **打包**成 RTP 包, 每个包交给 `fn` 发出(单包 / FU 分片自动决定)。
 *
 * @param s       会话状态
 * @param n       要打包的 NALU(不含起始码)
 * @param is_last 该 NALU 是否是本帧的最后一个 —— 是则置 RTP marker 位
 * @param fn      每包一次的回调(不能为 NULL)
 * @param user    透传给 `fn`
 * @return 已交付的 RTP 包个数; 负值为错误(含回调返回负值)
 */
int proto_rtp_pack_nalu(proto_rtp_session_t *s, const proto_nalu_t *n, int is_last,
                        proto_rtp_pkt_fn *fn, void *user);

/**
 * @brief 发送一个 NALU(自动决定单包还是 FU-A 分片)。
 *
 * @param sockfd    用于发送的 UDP socket
 * @param dst       目标地址
 * @param dstlen    地址长度
 * @param s         会话状态
 * @param n         要发送的 NALU(不含起始码)
 * @param is_last   该 NALU 是否是本帧的最后一个 —— 是则置 RTP marker 位
 * @return 已发送的 RTP 包个数; 负值为错误
 *
 * @note 它是 `proto_rtp_pack_nalu()` 的**薄包装**(每包一次 `sendto`),
 *       留给 PC 单测与"纯 UDP"的调用方;服务端主路径走 `svc_sender` 的
 *       `sender->send()`, 以便同时支持 UDP 与 RTP over TCP 交错。
 */
int proto_rtp_send_nalu(int sockfd, const struct sockaddr *dst, socklen_t dstlen,
                  proto_rtp_session_t *s, const proto_nalu_t *n, int is_last);

#endif /* __PROTO_RTP_H__ */
