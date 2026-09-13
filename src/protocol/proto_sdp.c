/**
 * @file    proto_sdp.c
 * @brief   SDP 生成实现 —— 两套模板(H.264 / H.265)+ Base64
 *
 * 实现刻意保持短小:职责只有「把参数拼成文本」。
 * 具体分三个部分:
 *      ① Base64 编码(把二进制的 SPS/PPS 变成 SDP 能携带的文本)
 *      ② 一个带容量检查的字符串追加助手
 *      ③ 两套模板: build_h264 / build_h265
 */
#include "proto_sdp.h"
#include "proto_rtp.h"      /* 复用 PROTO_RTP_PT_H264 / PT_H265 与时钟频率 */
#include "proto_str.h"      /* 与 proto_rtsp.c 共用的"整数转文本 + 追加"工具 */

#include <string.h>

/* ─────────────────────── ① Base64 ─────────────────────── */

/** Base64 字母表: 64 个可打印字符 */
static const char B64_TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** 编码一个 3 字节组; count 不足 3 时补 '='。返回写入的字符数(固定 4)。 */
static size_t b64_group(const uint8_t *g, size_t count, char *out)
{
    uint32_t v = ((uint32_t)g[0] << 16);

    if (count > 1)
        v |= ((uint32_t)g[1] << 8);
    if (count > 2)
        v |= (uint32_t)g[2];

    out[0] = B64_TAB[(v >> 18) & 0x3F];
    out[1] = B64_TAB[(v >> 12) & 0x3F];
    out[2] = (count > 1) ? B64_TAB[(v >> 6) & 0x3F] : '=';
    out[3] = (count > 2) ? B64_TAB[v & 0x3F]        : '=';
    return 4;
}

int proto_sdp_base64(const uint8_t *in, size_t in_len, char *out, size_t cap)
{
    size_t need;
    size_t i = 0;
    size_t o = 0;

    if (in == NULL || out == NULL)
        return -1;

    /* ⌈in_len/3⌉ × 4,再加结尾 '\0' */
    need = ((in_len + 2) / 3) * 4 + 1;
    if (cap < need)
        return -1;
    if (in_len == 0) {
        out[0] = '\0';
        return 0;
    }

    while (i + 3 <= in_len) {
        o += b64_group(in + i, 3, out + o);
        i += 3;
    }
    if (i < in_len)
        o += b64_group(in + i, in_len - i, out + o);

    out[o] = '\0';
    return (int)o;
}

/* ─────────────────── ② 带容量检查的追加助手 ─────────────────── */

/*
 * append / append_u32 只是给 proto_str_* 起个短名字 —— 本文件里调用次数很多,
 * 短名字让模板部分更好读。真正的实现是共用的(见 proto_str.c)。
 */
#define append(out, cap, used, text)        proto_str_append((out), (cap), (used), (text))
#define append_u32(out, cap, used, v)       proto_str_append_u32((out), (cap), (used), (v))

/** 追加 "前缀 + Base64(参数集)"; 参数集为空时整行跳过。 */
static int append_param(char *out, size_t cap, size_t *used,
                        const char *prefix, const uint8_t *data, size_t len)
{
    char b64[PROTO_SDP_PARAM_MAX * 2];
    int  n;

    if (len == 0)
        return 0;                       /* 没有这个参数集, 跳过 */

    n = proto_sdp_base64(data, len, b64, sizeof(b64));
    if (n < 0)
        return -1;

    if (append(out, cap, used, prefix) != 0)
        return -1;
    return append(out, cap, used, b64);
}

/* ─────────────────── ③ 两套 SDP 模板 ─────────────────── */

/**
 * 把载荷类型数字追加到输出(动态 PT 只可能是 96~127)。
 * @note 直接复用 proto_str_append_u32, 不再手写十进制转换。
 */
static int append_pt(char *out, size_t cap, size_t *used, uint8_t pt)
{
    return append_u32(out, cap, used, (uint32_t)pt);
}

/** 写入两个协议共有的头几行(v= o= s= c= t= m=)。 */
static int build_common(const proto_sdp_cfg_t *cfg, char *out, size_t cap,
                        size_t *used)
{
    if (append(out, cap, used, "v=0\r\n") != 0)
        return -1;
    if (append(out, cap, used, "o=- 0 0 IN IP4 ") != 0)
        return -1;
    if (append(out, cap, used, cfg->origin_ip) != 0)
        return -1;
    if (append(out, cap, used, "\r\ns=") != 0)
        return -1;
    if (append(out, cap, used, cfg->session_name) != 0)
        return -1;
    if (append(out, cap, used, "\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\nm=video 0 RTP/AVP ") != 0)
        return -1;
    return append_pt(out, cap, used, cfg->payload_type);
}

/**
 * H.264 模板(RFC 6184 §8.1)。
 *
 *   a=rtpmap:96 H264/90000
 *   a=fmtp:96 packetization-mode=1;sprop-parameter-sets=<b64(SPS)>,<b64(PPS)>
 *
 * 注意 SPS 与 PPS 是**逗号**分隔, 且属于同一个 sprop-parameter-sets 字段。
 */
static int build_h264(const proto_sdp_cfg_t *cfg, char *out, size_t cap,
                      size_t *used, const char *codec_name)
{
    if (build_common(cfg, out, cap, used) != 0)
        return -1;
    if (append(out, cap, used, "\r\na=rtpmap:") != 0)
        return -1;
    if (append_pt(out, cap, used, cfg->payload_type) != 0)
        return -1;
    if (append(out, cap, used, " ") != 0 || append(out, cap, used, codec_name) != 0)
        return -1;
    /* 时钟频率按 RTP 视频标准固定写 90000(与 proto_rtp.h 的
       PROTO_RTP_CLOCK_RATE 一致), 不动态拼接 —— 少一次出错机会。 */
    if (append(out, cap, used, "/90000\r\na=fmtp:") != 0)
        return -1;
    if (append_pt(out, cap, used, cfg->payload_type) != 0)
        return -1;
    if (append(out, cap, used, " packetization-mode=1;sprop-parameter-sets=") != 0)
        return -1;
    if (append_param(out, cap, used, "", cfg->sps, cfg->sps_len) != 0)
        return -1;
    if (append(out, cap, used, ",") != 0)
        return -1;
    if (append_param(out, cap, used, "", cfg->pps, cfg->pps_len) != 0)
        return -1;
    return append(out, cap, used, "\r\na=control:streamid=0\r\n");
}

/**
 * H.265 模板(RFC 7798 §7.2.1)。
 *
 *   a=rtpmap:97 H265/90000
 *   a=fmtp:97 sprop-vps=<b64(VPS)>;sprop-sps=<b64(SPS)>;sprop-pps=<b64(PPS)>
 *
 * ★ 关键: 三个参数集必须在【同一个 a=fmtp 行】里, 用【分号】分隔。
 *   RFC 7798 原文: "these parameters are expressed as a media type string,
 *   in the form of a semicolon-separated list of parameter=value pairs."
 *   (写成三行 a=fmtp 是错的 —— 那是本项目笔记早期的笔误, 已修正。)
 */
static int build_h265(const proto_sdp_cfg_t *cfg, char *out, size_t cap,
                      size_t *used, const char *codec_name)
{
    int has_vps = (cfg->vps_len > 0);
    int has_sps = (cfg->sps_len > 0);

    if (build_common(cfg, out, cap, used) != 0)
        return -1;
    if (append(out, cap, used, "\r\na=rtpmap:") != 0)
        return -1;
    if (append_pt(out, cap, used, cfg->payload_type) != 0)
        return -1;
    if (append(out, cap, used, " ") != 0 || append(out, cap, used, codec_name) != 0)
        return -1;
    if (append(out, cap, used, "/90000\r\na=fmtp:") != 0)
        return -1;
    if (append_pt(out, cap, used, cfg->payload_type) != 0)
        return -1;
    if (append(out, cap, used, " ") != 0)
        return -1;

    /* 三个 sprop-* 依次拼接, 之间用 ';' 分隔(不是换行) —— 见 RFC 7798 §7.2.1 */
    if (append_param(out, cap, used, "sprop-vps=", cfg->vps, cfg->vps_len) != 0)
        return -1;
    if (has_vps && (has_sps || cfg->pps_len)) {
        if (append(out, cap, used, ";") != 0)
            return -1;
    }
    if (append_param(out, cap, used, "sprop-sps=", cfg->sps, cfg->sps_len) != 0)
        return -1;
    if (has_sps && cfg->pps_len) {
        if (append(out, cap, used, ";") != 0)
            return -1;
    }
    if (append_param(out, cap, used, "sprop-pps=", cfg->pps, cfg->pps_len) != 0)
        return -1;

    return append(out, cap, used, "\r\na=control:streamid=0\r\n");
}

/* ─────────────────────── 对外接口 ─────────────────────── */

int proto_sdp_build(const proto_sdp_cfg_t *cfg, char *out, size_t cap)
{
    const char *codec_name;
    size_t      used = 0;
    int         rc;

    if (cfg == NULL || out == NULL || cap == 0)
        return -1;
    if (cfg->origin_ip == NULL || cfg->session_name == NULL)
        return -1;
    if (cfg->payload_type < 96 || cfg->payload_type > 127)
        return -1;                      /* 只允许动态 PT 区 */

    out[0] = '\0';

    codec_name = cfg->is_h265 ? "H265" : "H264";
    rc = cfg->is_h265 ? build_h265(cfg, out, cap, &used, codec_name)
                      : build_h264(cfg, out, cap, &used, codec_name);
    if (rc != 0)
        return -2;                      /* 缓冲不够 */

    return (int)used;
}
