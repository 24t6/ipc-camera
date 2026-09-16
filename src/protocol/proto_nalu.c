/**
 * @file    nalu.c
 * @brief   Annex-B 码流解析实现
 *
 * 【模块职责】把 Annex-B 码流切成一个个 NALU, 并识别参数集 / IDR
 * 【依赖方向】只依赖 libc —— **不碰 socket、不碰硬件**(所以能在 PC 上原生单测)
 * 【线程模型】纯函数, 无全局可变状态, 可在多线程中并发调用
 * 【资源边界】无动态分配; 解析结果写到调用方给的数组里
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

/**
 * @brief 按 H.265 的 **6 位**类型值判定 NALU 种类
 *
 * @param[in,out] n 已填好类型值的 NALU(本函数补上 kind)
 * @note H.265 的类型是 6 位(H.264 是 5 位), 所以两者的判定表**不能共用**。
 */
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

/**
 * @brief 算出一个 NALU 的右边界。
 *
 * @param nalu_start NALU 第一个字节(已在起始码之后)
 * @param end        整段缓冲的末尾
 * @param next       find_start_code() 找到的**下一个**起始码位置; NULL = 这是最后一个
 * @return 右边界指针(不含)
 *
 * @note 两件事, 都容易写错, 所以单独放一处:
 *   ① 下一个起始码可能带 3 字节或 4 字节版本, find_start_code 返回的是
 *      "00 00 01" 里那个 `01` 的位置, 所以要 **-3** 才算上一个 NALU 的结束。
 *   ② 4 字节起始码(`00 00 00 01`)会把多出来的那个 `00` 留在**上一个** NALU
 *      的尾巴上, 必须裁掉 —— 否则 NALU 会长出一个字节的 0x00。
 */
static const uint8_t *nalu_right_edge(const uint8_t *nalu_start,
                                      const uint8_t *end,
                                      const uint8_t *next)
{
    const uint8_t *nalu_end = next ? (next - 3) : end;

    while (nalu_end > nalu_start && nalu_end[-1] == 0x00)
        nalu_end--;
    return nalu_end;
}

/** 回调的三种意图 —— emit_nalu 的返回值 */
#define EMIT_SKIPPED  0     /* 长度不够放下 NALU 头, 跳过(不算一个 NALU) */
#define EMIT_DELIVERED 1    /* 已回调, 回调说"继续" */
#define EMIT_STOP     2     /* 回调要求提前终止 */

/**
 * @brief 给一个 NALU 分类并回调。
 *
 * @return EMIT_SKIPPED / EMIT_DELIVERED / EMIT_STOP
 *
 * @note 回调的返回值**必须原样传出去** —— 上层靠它支持"只想看前 N 个 NALU"
 *       (我们的 rtsp_test 就是用它找到 SPS/PPS 就停)。第一版重构时我把这个
 *       返回值吞掉了, 幸好立刻发现 —— 这正是"重构必须重跑测试"的理由:
 *       **这类语义丢失, 编译器不会报错, 类型也对。**
 */
static int emit_nalu(const uint8_t *start, size_t nlen, int is_h265,
                     proto_nalu_cb_t cb, void *user)
{
    proto_nalu_t n;

    if (nlen < (size_t)(is_h265 ? 2 : 1))
        return EMIT_SKIPPED;

    memset(&n, 0, sizeof(n));
    n.data    = start;
    n.len     = nlen;
    n.is_h265 = is_h265;

    if (is_h265)
        classify_h265(&n);
    else
        classify_h264(&n);

    return (cb(&n, user) != 0) ? EMIT_STOP : EMIT_DELIVERED;
}

int proto_nalu_foreach(const uint8_t *buf, size_t len, int is_h265,
                 proto_nalu_cb_t cb, void *user)
{
    const uint8_t *end;
    const uint8_t *nalu_start;
    int            count = 0;

    if (buf == NULL || len < 4 || cb == NULL)
        return -1;

    end = buf + len;

    /* 1) 定位第一个起始码 */
    nalu_start = find_start_code(buf, end);
    if (nalu_start == NULL)
        return 0;               /* 整段没有起始码 —— 不是 Annex-B */

    for (;;) {
        const uint8_t *next;    /* 下一个起始码的位置 */
        const uint8_t *nalu_end;
        int            r;

        /* 2) 找下一个起始码, 它就是当前 NALU 的右边界 */
        next     = find_start_code(nalu_start, end);
        nalu_end = nalu_right_edge(nalu_start, end, next);

        /* 3) 分类 + 回调 */
        r = emit_nalu(nalu_start, (size_t)(nalu_end - nalu_start), is_h265,
                      cb, user);
        if (r == EMIT_STOP)
            return count + 1;   /* 回调要求提前终止 */
        if (r == EMIT_DELIVERED)
            count++;

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

/**
 * @brief 遍历回调: 只要见到 IDR 就置位并立即停止
 *
 * @param n 当前 NALU
 * @param user 指向"命中标志"(int)的指针
 * @return 1 = 命中, 要求遍历提前结束; 0 = 继续
 */
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
