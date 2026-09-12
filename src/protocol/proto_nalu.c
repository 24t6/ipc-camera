/**
 * @file    nalu.c
 * @brief   Annex-B 码流解析实现
 */
#include "proto_nalu.h"

#include <string.h>

/*
 * 在 [p, end) 范围内查找 Annex-B 起始码 (00 00 01)。
 * 返回起始码「之后」第一个字节的位置; 找不到返回 NULL。
 *
 * 注意: 4 字节起始码 00 00 00 01 也包含 00 00 01, 所以统一按 3 字节找,
 *       多出来的那个 00 由调用方当作 trailing zero 裁掉。
 */
static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end)
{
    while (p + 3 <= end) {
        if (p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01)
            return p + 3;
        p++;
    }
    return NULL;
}

/* H.264: 判断类型并归一化 */
static void classify_h264(proto_nalu_t *n)
{
    uint8_t type = n->data[0] & 0x1F;   /* 低 5 bit */
    uint8_t nri  = (n->data[0] >> 5) & 0x03;

    n->type   = type;
    n->is_key = 0;

    switch (type) {
    case 7:  n->kind = PROTO_NALU_KIND_SPS;   break;
    case 8:  n->kind = PROTO_NALU_KIND_PPS;   break;
    case 6:  n->kind = PROTO_NALU_KIND_SEI;   break;
    case 9:  n->kind = PROTO_NALU_KIND_AUD;   break;
    case 5:  n->kind = PROTO_NALU_KIND_IDR;   n->is_key = 1; break;   /* IDR 关键帧 */
    case 1:  n->kind = PROTO_NALU_KIND_SLICE; break;
    default: n->kind = PROTO_NALU_KIND_UNKNOWN; break;
    }

    (void)nri;   /* nri 暂未使用, 保留以便将来判断参考关系 */
}

/* H.265: 判断类型并归一化 */
static void classify_h265(proto_nalu_t *n)
{
    uint8_t type = (n->data[0] >> 1) & 0x3F;   /* 高 6 bit */

    n->type   = type;
    n->is_key = 0;

    if (type == 32) {
        n->kind = PROTO_NALU_KIND_VPS;
    } else if (type == 33) {
        n->kind = PROTO_NALU_KIND_SPS;
    } else if (type == 34) {
        n->kind = PROTO_NALU_KIND_PPS;
    } else if (type == 39 || type == 40) {
        n->kind = PROTO_NALU_KIND_SEI;
    } else if (type >= 16 && type <= 21) {
        /* BLA_W_LP(16) .. CRA_NUT(21) 都属于 IRAP —— 可作为随机接入点 */
        n->kind = PROTO_NALU_KIND_IDR;
        n->is_key = 1;
    } else if (type <= 9) {
        n->kind = PROTO_NALU_KIND_SLICE;
    } else {
        n->kind = PROTO_NALU_KIND_UNKNOWN;
    }
}

int proto_nalu_foreach(const uint8_t *buf, size_t len, int is_h265,
                 proto_nalu_cb_t cb, void *user)
{
    const uint8_t *end;
    const uint8_t *nalu_start;
    size_t         min_hdr = is_h265 ? 2 : 1;
    int            count = 0;

    if (buf == NULL || len < 4 || cb == NULL)
        return -1;

    end = buf + len;

    /* 1) 定位第一个起始码 */
    nalu_start = find_start_code(buf, end);
    if (nalu_start == NULL)
        return 0;               /* 整段没有起始码 —— 不是 Annex-B */

    for (;;) {
        const uint8_t *next;    /* 下一个起始码之后的位置 */
        const uint8_t *nalu_end;
        size_t         nlen;
        proto_nalu_t         n;

        /* 2) 找下一个起始码, 它就是当前 NALU 的右边界 */
        next     = find_start_code(nalu_start, end);
        nalu_end = next ? (next - 3) : end;

        /* 3) 裁掉起始码前可能存在的 00 填充(4 字节起始码的情形) */
        while (nalu_end > nalu_start && nalu_end[-1] == 0x00)
            nalu_end--;

        nlen = (size_t)(nalu_end - nalu_start);

        /* 4) 长度至少要能放下 NALU 头才有效 */
        if (nlen >= min_hdr) {
            memset(&n, 0, sizeof(n));
            n.data    = nalu_start;
            n.len     = nlen;
            n.is_h265 = is_h265;

            if (is_h265)
                classify_h265(&n);
            else
                classify_h264(&n);

            if (cb(&n, user) != 0)
                return count + 1;   /* 回调要求提前终止 */
            count++;
        }

        if (next == NULL)
            break;

        nalu_start = next;
    }

    return count;
}

/* proto_nalu_has_idr 的回调上下文 */
typedef struct {
    int found;
} idr_ctx_t;

static int idr_probe(const proto_nalu_t *n, void *user)
{
    idr_ctx_t *ctx = (idr_ctx_t *)user;

    if (n->is_key) {
        ctx->found = 1;
        return 1;               /* 找到就停 */
    }
    return 0;
}

int proto_nalu_has_idr(const uint8_t *buf, size_t len, int is_h265)
{
    idr_ctx_t ctx;

    ctx.found = 0;
    (void)proto_nalu_foreach(buf, len, is_h265, idr_probe, &ctx);

    return ctx.found;
}

const char *proto_nalu_kind_name(proto_nalu_kind_t kind)
{
    switch (kind) {
    case PROTO_NALU_KIND_SPS:     return "SPS";
    case PROTO_NALU_KIND_PPS:     return "PPS";
    case PROTO_NALU_KIND_VPS:     return "VPS";
    case PROTO_NALU_KIND_SEI:     return "SEI";
    case PROTO_NALU_KIND_IDR:     return "IDR";
    case PROTO_NALU_KIND_SLICE:   return "SLICE";
    case PROTO_NALU_KIND_AUD:     return "AUD";
    default:                return "UNKNOWN";
    }
}
