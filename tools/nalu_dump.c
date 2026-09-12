/**
 * @file    nalu_dump.c
 * @brief   NALU 解析器测试工具 —— 读一段 Annex-B 码流, 打印 NALU 序列
 *
 * 用途:
 *   1) 验证 nalu.c 的解析是否正确
 *   2) 直观看到码流结构(SPS/PPS/IDR/SLICE 的排列规律)
 *
 * 用法: ./nalu_dump <file.h264|file.h265> [最多打印条数]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto_nalu.h"

typedef struct {
    int count;
    int limit;
    int key_count;
    int sps_count;
    int pps_count;
    int vps_count;
    size_t max_nalu;
} dump_ctx_t;

static int on_nalu(const proto_nalu_t *n, void *user)
{
    dump_ctx_t *ctx = (dump_ctx_t *)user;

    /* 前 limit 条全打, 之后只打关键帧和参数集 */
    if (ctx->count < ctx->limit ||
        n->is_key ||
        n->kind == PROTO_NALU_KIND_SPS ||
        n->kind == PROTO_NALU_KIND_PPS ||
        n->kind == PROTO_NALU_KIND_VPS) {
        printf("  [%5d] %-7s type=%-3u len=%-7zu %s%s\n",
               ctx->count,
               proto_nalu_kind_name(n->kind),
               (unsigned)n->type,
               n->len,
               n->is_h265 ? "H265" : "H264",
               n->is_key ? "   <== KEYFRAME" : "");
    }

    if (n->is_key)                              ctx->key_count++;
    if (n->kind == PROTO_NALU_KIND_SPS)               ctx->sps_count++;
    if (n->kind == PROTO_NALU_KIND_PPS)               ctx->pps_count++;
    if (n->kind == PROTO_NALU_KIND_VPS)               ctx->vps_count++;
    if (n->len > ctx->max_nalu)                 ctx->max_nalu = n->len;

    ctx->count++;
    return 0;
}

int main(int argc, char **argv)
{
    FILE      *fp;
    uint8_t   *buf;
    long       size;
    int        is_h265;
    dump_ctx_t ctx;
    int        n;

    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.h264|file.h265> [最多打印条数]\n", argv[0]);
        return 1;
    }

    /* 按扩展名判断编码格式 */
    is_h265 = (strstr(argv[1], ".h265") != NULL || strstr(argv[1], ".hevc") != NULL);

    fp = fopen(argv[1], "rb");
    if (fp == NULL) {
        perror("fopen");
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL) {
        fprintf(stderr, "malloc %ld 失败\n", size);
        fclose(fp);
        return 1;
    }

    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "读取不完整\n");
        free(buf);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    memset(&ctx, 0, sizeof(ctx));
    ctx.limit = (argc >= 3) ? atoi(argv[2]) : 15;

    printf("===== %s =====\n", argv[1]);
    printf("文件大小   : %ld 字节\n", size);
    printf("编码格式   : %s\n", is_h265 ? "H.265/HEVC" : "H.264/AVC");
    printf("NALU 列表  :\n");

    n = proto_nalu_foreach(buf, (size_t)size, is_h265, on_nalu, &ctx);

    printf("\n----- 统计 -----\n");
    printf("NALU 总数  : %d\n", n);
    printf("关键帧数   : %d\n", ctx.key_count);
    printf("SPS / PPS  : %d / %d\n", ctx.sps_count, ctx.pps_count);
    if (is_h265)
        printf("VPS        : %d\n", ctx.vps_count);
    printf("最大 NALU  : %zu 字节 %s\n", ctx.max_nalu,
           ctx.max_nalu > 1400 ? "(> MTU, 必须用 FU-A 分片!)" : "");

    free(buf);
    return 0;
}
