/**
 * @file    media_diag.c
 * @brief   诊断: 每帧里到底有几个 pack、NALU 是怎么分布的
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  为什么要写它(被哪个现象逼出来的)
 * ─────────────────────────────────────────────────────────────────
 *  `media_smoke` 在板子上跑出了 206 帧 / 3,488,638 字节, 零错误零丢帧 ——
 *  但断言红了一片, 而且数字**自相矛盾**:
 *
 *      NALU 总数 = 21, 关键帧 NALU = 7, 切片 NALU = 7   (共 206 帧)
 *      → 平均 0.1 个 NALU/帧, 而 H.264 每帧至少 1 个切片 NALU
 *
 *  三种可能, 必须切开:
 *    A. `proto_nalu` 解析器坏了       → 它有 PC 单测覆盖, 但"有单测"不等于"没 bug"
 *    B. 拼槽位拼坏了                  → 起始码被拼断
 *    C. 我们对"一帧有几个 pack / 长什么样"的**假设**本身错了
 *
 * ─────────────────────────────────────────────────────────────────
 *  怎么切: 分离变量
 * ─────────────────────────────────────────────────────────────────
 *  `media_smoke` 里是「先拼成连续缓冲 → 再解析」, 两个变量缠在一起。
 *  本程序**逐个 pack 单独解析**(不拼), 于是:
 *
 *    · 若"逐 pack 解析"能出很多 NALU, 而拼起来就没了 → 是 **B(拼坏了)**
 *    · 若"逐 pack 解析"也出不来                     → 是 **A 或 C**
 *    · 同时打印每个 pack 的前 8 字节和起始码有无      → 直接看出 pack 边界长什么样
 *
 *  这是本工作区的规矩: **先量, 再改**; 而且要**分离变量**, 不要一次改两处。
 *
 * @note 板端程序(依赖 MPP)。用法: ./media_diag [观察帧数, 默认 120]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_mpp.h"
#include "infra_log.h"
#include "proto_nalu.h"

/** dump 文件: 前若干帧的原始拼接内容(每帧前置 4 字节小端长度) */
#define DUMP_PATH   "/tmp/media_diag_dump.bin"
#define DUMP_FRAMES 20

/** 打印明细的帧数 */
#define DETAIL_FRAMES 12

/** 在 [p, p+len) 里找 00 00 01 */
static int has_start_code(const uint8_t *p, size_t len)
{
    size_t i;

    for (i = 0; i + 3 <= len; i++) {
        if (p[i] == 0x00 && p[i + 1] == 0x00 && p[i + 2] == 0x01)
            return 1;
    }
    return 0;
}

/** 在 [p, p+len) 里数 00 00 01 出现次数 */
static int count_start_codes(const uint8_t *p, size_t len)
{
    size_t i;
    int    n = 0;

    for (i = 0; i + 3 <= len; i++) {
        if (p[i] == 0x00 && p[i + 1] == 0x00 && p[i + 2] == 0x01) {
            n++;
            i += 2;                     /* 跳过, 避免 00 00 01 被重复计 */
        }
    }
    return n;
}

/** proto_nalu 回调上下文: 数个数 + 记类型分布 */
typedef struct {
    int cnt;
    int by_type[64];
} diag_tally_t;

static int diag_nalu_cb(const proto_nalu_t *n, void *user)
{
    diag_tally_t *t = (diag_tally_t *)user;

    t->cnt++;
    if (n->type < 64)
        t->by_type[n->type]++;
    return 0;
}

/** 观察过程中累计的统计 */
typedef struct {
    int frames;
    int packs;
    int bytes;
    int start_codes;        /* 逐个 pack 数出来的 00 00 01 个数 */
    int nalu_per_pack;      /* 逐 pack 单独解析得到的 NALU 个数 */
    int nalu_joined;        /* 拼成连续缓冲后解析得到的 NALU 个数 */
    int timeouts;           /* "此刻没帧"的次数(正常) */
    int errors;             /* 取流出错次数 */
    int pack_count_hist[16];
    int type_hist[64];
} diag_acc_t;

/**
 * 观察一帧。
 *
 * @note 这里**同时**做两种解析(逐 pack / 拼起来), 就是为了让差异直接暴露出来。
 */
static void observe_frame(const bsp_mpp_frame_t *f, int show_detail,
                          diag_acc_t *acc)
{
    uint8_t *joined;
    size_t   total = 0;
    size_t   off = 0;
    int      k;
    int      k2;

    acc->frames++;
    acc->packs += f->pack_count;
    if (f->pack_count >= 0 && f->pack_count < 16)
        acc->pack_count_hist[f->pack_count]++;

    for (k = 0; k < f->pack_count; k++)
        total += f->packs[k].len;

    /* 临时缓冲: 只为"拼起来再解析"这一路对照用, 不参与生产逻辑 */
    joined = (total <= BSP_MPP_FRAME_MAX_BYTES)
             ? (uint8_t *)malloc(BSP_MPP_FRAME_MAX_BYTES) : NULL;

    for (k = 0; k < f->pack_count; k++) {
        const uint8_t *d = f->packs[k].data;
        size_t         n = f->packs[k].len;
        diag_tally_t   t;
        int            sc = count_start_codes(d, n);

        acc->bytes += (int)n;
        acc->start_codes += sc;

        if (show_detail) {
            printf("   帧%-3d pack%-2d len=%-7zu 前8字节=%02X %02X %02X %02X "
                   "%02X %02X %02X %02X  起始码=%d\n",
                   acc->frames - 1, k, n,
                   n > 0 ? d[0] : 0, n > 1 ? d[1] : 0,
                   n > 2 ? d[2] : 0, n > 3 ? d[3] : 0,
                   n > 4 ? d[4] : 0, n > 5 ? d[5] : 0,
                   n > 6 ? d[6] : 0, n > 7 ? d[7] : 0, sc);
        }

        /* ★ 分离变量之一: 逐个 pack 单独解析(不拼) */
        memset(&t, 0, sizeof(t));
        (void)proto_nalu_foreach(d, n, 0, diag_nalu_cb, &t);
        acc->nalu_per_pack += t.cnt;
        for (k2 = 0; k2 < 64; k2++)
            acc->type_hist[k2] += t.by_type[k2];

        /* 顺手拷贝 —— "拼起来"这一路的输入 */
        if (joined != NULL) {
            memcpy(joined + off, d, n);
            off += n;
        }
    }

    /* ★ 分离变量之二: 拼成连续缓冲再解析(和 svc_media 做的事一样) */
    if (joined != NULL) {
        diag_tally_t t;

        memset(&t, 0, sizeof(t));
        (void)proto_nalu_foreach(joined, off, 0, diag_nalu_cb, &t);
        acc->nalu_joined += t.cnt;
    }
    free(joined);
}

int main(int argc, char **argv)
{
    int        want = (argc > 1) ? atoi(argv[1]) : 120;
    int        i;
    int        dumped = 0;
    int        err_streak = 0;
    diag_acc_t acc;
    FILE      *dump;

    if (want <= 0 || want > 2000)
        want = 120;

    memset(&acc, 0, sizeof(acc));
    infra_log_set_level(INFRA_LOG_WARN);

    printf("===== MPP 帧结构诊断(观察 %d 帧)=====\n", want);

    if (bsp_mpp_init() != 0) {
        printf("❌ bsp_mpp_init 失败\n");
        return 1;
    }

    dump = fopen(DUMP_PATH, "wb");
    if (dump == NULL)
        printf("⚠️ 打不开 %s, 只做屏幕统计\n", DUMP_PATH);

    printf("\n--- 逐 pack 明细(前 %d 帧)---\n", DETAIL_FRAMES);
    for (i = 0; i < want; i++) {
        bsp_mpp_frame_t f;
        int             rc = bsp_mpp_get_frame(&f, 50);   /* 50ms 超时 */

        if (rc == 1) {
            acc.timeouts++;
            continue;                   /* 此刻没帧, 继续等 */
        }
        if (rc != 0) {
            /*
             * 取流出错。**不要碰到一次就放弃** ——
             * 实测踩过: 紧接上一轮程序跑时, VENC 通道可能还没完全复位,
             * GetStream 返回 0xa0088003(缓冲未就绪), 过一会儿自己就好了。
             * 所以这里只计数并继续; 真的一直失败, 最后 err_streak 会暴露出来。
             */
            acc.errors++;
            if (++err_streak >= 50) {
                printf("连续 %d 次取帧出错(rc=%d), 放弃观察\n", err_streak, rc);
                break;
            }
            continue;
        }
        err_streak = 0;

        observe_frame(&f, acc.frames < DETAIL_FRAMES, &acc);

        /* dump 前 DUMP_FRAMES 帧 */
        if (dump != NULL && dumped < DUMP_FRAMES) {
            unsigned char lenbuf[4];
            size_t        total = 0;
            int           k;

            for (k = 0; k < f.pack_count; k++)
                total += f.packs[k].len;
            lenbuf[0] = (unsigned char)(total & 0xFF);
            lenbuf[1] = (unsigned char)((total >> 8) & 0xFF);
            lenbuf[2] = (unsigned char)((total >> 16) & 0xFF);
            lenbuf[3] = (unsigned char)((total >> 24) & 0xFF);
            fwrite(lenbuf, 1, 4, dump);
            for (k = 0; k < f.pack_count; k++)
                fwrite(f.packs[k].data, 1, f.packs[k].len, dump);
            dumped++;
        }

        bsp_mpp_release_frame(&f);
    }

    if (dump != NULL)
        fclose(dump);

    printf("\n--- 汇总 ---\n");
    printf("帧数              : %d\n", acc.frames);
    printf("当时没帧          : %d 次(正常)\n", acc.timeouts);
    printf("取流出错          : %d 次\n", acc.errors);
    if (acc.frames > 0) {
        printf("pack 总数         : %d(平均 %.2f 个/帧)\n",
               acc.packs, (double)acc.packs / acc.frames);
        printf("字节总数          : %d(平均 %.0f 字节/帧)\n",
               acc.bytes, (double)acc.bytes / acc.frames);
        printf("00 00 01 总数     : %d(平均 %.2f 个/帧)\n",
               acc.start_codes, (double)acc.start_codes / acc.frames);
        printf("\n★ 逐 pack 单独解析 : %d 个 NALU(平均 %.2f 个/帧)\n",
               acc.nalu_per_pack, (double)acc.nalu_per_pack / acc.frames);
        printf("★ 拼起来再解析     : %d 个 NALU(平均 %.2f 个/帧)\n",
               acc.nalu_joined, (double)acc.nalu_joined / acc.frames);
        printf("\n→ 两者若**差距很大**, 问题就在「拼」这一步;\n"
               "  两者若**一样少**, 问题在解析或我们对码流形态的假设。\n");
    }

    printf("\n每帧 pack 数分布:\n");
    for (i = 0; i < 16; i++) {
        if (acc.pack_count_hist[i] > 0)
            printf("   %2d 个 pack: %d 帧\n", i, acc.pack_count_hist[i]);
    }

    printf("\nNALU 类型分布(逐 pack 解析。H.264 类型值: 1=切片 5=IDR 6=SEI 7=SPS 8=PPS 9=AUD):\n");
    for (i = 0; i < 64; i++) {
        if (acc.type_hist[i] > 0)
            printf("   类型 %2d: %d 个\n", i, acc.type_hist[i]);
    }

    if (dump != NULL)
        printf("\ndump 已写入 %s(前 %d 帧, 每帧前置 4 字节小端长度)\n",
               DUMP_PATH, dumped);

    bsp_mpp_deinit();
    printf("\n诊断结束。\n");
    printf("DIAG_DONE frames=%d nalu_per_pack=%d nalu_joined=%d errors=%d\n",
           acc.frames, acc.nalu_per_pack, acc.nalu_joined, acc.errors);
    return 0;
}
