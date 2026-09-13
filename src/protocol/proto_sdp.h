/**
 * @file    proto_sdp.h
 * @brief   SDP 会话描述生成 —— RTSP DESCRIBE 的返回值
 *
 * @details
 * SDP(Session Description Protocol, 会话描述协议)是**媒体说明书**:
 * 客户端在收第一个 RTP 包之前, 必须先知道"这流该怎么解"。
 * 它回答五件事:
 *      这流是 H.264 还是 H.265?   → 决定用哪个解码器
 *      画面多大、每秒几帧?         → 决定解码器初始化参数
 *      载荷类型 96 代表什么?       → 见 a=rtpmap
 *      解码需要的 SPS/PPS 是什么?  → 见 a=fmtp 里的 sprop-parameter-sets
 *      这条流叫什么?               → 见 a=control(SETUP 时要用)
 *
 * ─────────────────────────────────────────────────────────────────
 *  为什么 SPS/PPS 要塞进 SDP(Base64)
 * ─────────────────────────────────────────────────────────────────
 *  码流里每个 IDR 前面本来就带了参数集(见第 2 课), 所以"不给也能播"。
 *  但那样客户端必须**等到第一个关键帧**才拿到参数 —— 开播慢、容易花屏。
 *  放进 SDP 的好处是:**客户端一连上就知道了**, 不等第一帧。
 *
 *  SDP 是纯文本, 而 SPS/PPS 是二进制(含 0x00 等不可打印字节),
 *  所以必须 Base64 编码:3 字节 → 4 个可打印字符(A-Z a-z 0-9 + /)。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 最容易踩的坑: packetization-mode 必须是 1
 * ─────────────────────────────────────────────────────────────────
 *      0 = 只允许单 NALU 一个包, **不许分片**   ← 我们不能用!
 *      1 = 允许 FU-A 分片                       ← 我们用这个
 *      2 = 允许交错模式(更复杂)                ← 不用
 *
 *  为什么 0 不行: 实测最大 NALU 有 55 KB(H.264)/ 115 KB(H.265),
 *  一个 UDP 包最多带 1400 字节 —— **不许分片就等于大关键帧根本发不出去**。
 *  症状是"能连上、能播, 但每隔一秒花屏一次", 极难反查。
 *
 * ─────────────────────────────────────────────────────────────────
 *  H.264 与 H.265 的写法差异(必须分支)
 * ─────────────────────────────────────────────────────────────────
 *  H.264 (RFC 6184): SPS 和 PPS 用**逗号**连成一个值
 *      a=fmtp:96 packetization-mode=1;sprop-parameter-sets=<b64(SPS)>,<b64(PPS)>
 *  H.265 (RFC 7798): 多了 VPS, 三个参数集用**三个独立字段**
 *      a=fmtp:97 sprop-vps=<b64(VPS)>
 *      a=fmtp:97 sprop-sps=<b64(SPS)>
 *      a=fmtp:97 sprop-pps=<b64(PPS)>
 *
 * @note 本模块属于 protocol 层: **不碰硬件、不做动态分配**, 可在 PC 上原生单测。
 */
#ifndef __PROTO_SDP_H__
#define __PROTO_SDP_H__

#include <stddef.h>
#include <stdint.h>

/** SDP 输出缓冲建议大小(实测两套 SDP 都不足 400 字节, 留足余量) */
#define PROTO_SDP_BUF_SIZE 1024

/** 参数集最大长度(VPS/SPS/PPS 通常几十字节, 留足余量) */
#define PROTO_SDP_PARAM_MAX 256

/**
 * 生成 SDP 所需的全部输入。
 *
 * @note 所有 `*_len` 为 0 时, 表示该参数集「暂时没有」——
 *       此时对应字段会被跳过, 客户端只能等码流里的参数集。
 *       这是为 M2(OSD)之后「参数集动态变化」预留的路径。
 */
typedef struct {
    /* ── 会话信息 ── */
    const char *origin_ip;      /* o= 行里的 IP, 如 "192.168.16.88" */
    const char *session_name;   /* s= 行, 如 "IPC Camera" */

    /* ── 媒体信息 ── */
    uint8_t     payload_type;   /* 96=H.264 / 97=H.265 */
    int         is_h265;        /* 1=H.265, 0=H.264 */

    /* ── 参数集(原始字节, 不含起始码、不含 NALU 头之外的东西) ── */
    uint8_t     vps[PROTO_SDP_PARAM_MAX];  size_t vps_len;   /* H.265 用 */
    uint8_t     sps[PROTO_SDP_PARAM_MAX];  size_t sps_len;
    uint8_t     pps[PROTO_SDP_PARAM_MAX];  size_t pps_len;
} proto_sdp_cfg_t;

/**
 * 生成 SDP 文本。
 *
 * @param cfg   输入参数(见上)
 * @param out   输出缓冲(调用方提供, 建议 PROTO_SDP_BUF_SIZE)
 * @param cap   out 的容量(含结尾 '\0')
 * @return 写入的字符数(不含 '\0'); 负值为错误:
 *         -1 = 参数非法, -2 = 缓冲不够
 *
 * @note 线程安全: 只写调用方提供的缓冲, 无静态状态, 可重入。
 * @note 不阻塞、不分配内存。
 */
int proto_sdp_build(const proto_sdp_cfg_t *cfg, char *out, size_t cap);

/**
 * Base64 编码(单独暴露, 便于单测「3 字节 → 4 字符」这条规则)。
 *
 * @param in       输入字节
 * @param in_len   输入长度
 * @param out      输出缓冲
 * @param cap      输出容量(含 '\0'); 需要 ⌈in_len/3⌉×4 + 1
 * @return 写入字符数(不含 '\0'); 负值表示缓冲不够
 *
 * @note 标准字母表 A-Z a-z 0-9 + /, 末尾按需补 '='。
 */
int proto_sdp_base64(const uint8_t *in, size_t in_len, char *out, size_t cap);

#endif /* __PROTO_SDP_H__ */
