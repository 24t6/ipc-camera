/**
 * @file    svc_record_policy_test.c
 * @brief   录制策略(纯函数)的 PC 原生单测 —— 不需要板子
 *
 * 为什么这层能不上板就测:`svc_record_policy.c` **不碰 mp4v2、不碰文件系统**, 纯逻辑。
 *
 * 本测试的重点是 **两个最容易错的地方**:
 *   ① 文件名**必须补零** —— 否则 `2026-9-16` 与 `2026-10-1` 按字典序排反,
 *      环形覆盖会**删错文件**(厂商 sample 就是这么写的, 这里专门断言它)
 *   ② 环形删除**从最旧开始**、且**永远至少留一个**(正在写的那个不能删)
 *
 * 编译运行: python work/build_svc_record_policy_test.py
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "svc_record_policy.h"

static int g_pass;
static int g_fail;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) {                                                 \
            g_pass++;                                               \
        } else {                                                    \
            g_fail++;                                               \
            printf("  [FAIL] L%d: %s\n", __LINE__, msg);            \
        }                                                           \
    } while (0)

/** 造一个文件名(测试用;它自己也是被测对象的一部分, 所以另有格式断言) */
static void mk(svc_record_policy_file_t *f, const char *name, uint64_t size)
{
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->size = size;
}

/* ─────────── ① 文件名 ─────────── */

static void test_name(void)
{
    struct tm tmv;
    char      buf[SVC_RECORD_POLICY_NAME_MAX];
    char      older[SVC_RECORD_POLICY_NAME_MAX];
    char      newer[SVC_RECORD_POLICY_NAME_MAX];
    int       n;

    printf("\n[1] 分段文件名\n");
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = 2026 - 1900;
    tmv.tm_mon  = 9 - 1;
    tmv.tm_mday = 16;
    tmv.tm_hour = 21;
    tmv.tm_min  = 5;            /* 故意个位数, 检验补零 */
    tmv.tm_sec  = 3;

    n = svc_record_policy_make_name(&tmv, buf, sizeof(buf));
    printf("    结果: \"%s\"  (长度 %d)\n", buf, n);
    CHECK(n == 23, "长度应为 23");
    CHECK(strcmp(buf, "2026-09-16-21-05-03.mp4") == 0, "格式应为 年-月-日-时-分-秒.mp4 且补零");

    /* ★ 补零的意义:字典序必须 == 时间序 */
    tmv.tm_mon = 9 - 1;  tmv.tm_mday = 16;      /* 2026-09-16 */
    svc_record_policy_make_name(&tmv, older, sizeof(older));
    tmv.tm_mon = 10 - 1; tmv.tm_mday = 1;       /* 2026-10-01, 更晚 */
    svc_record_policy_make_name(&tmv, newer, sizeof(newer));
    printf("    排序断言: \"%s\" < \"%s\"\n", older, newer);
    CHECK(strcmp(older, newer) < 0,
          "★ 09-16 必须排在 10-01 之前(不补零就会排反 —— 厂商 sample 的坑)");

    CHECK(svc_record_policy_make_name(&tmv, buf, 4) < 0, "缓冲太小应失败");
    CHECK(svc_record_policy_make_name(NULL, buf, sizeof(buf)) < 0, "NULL 时间应失败");
    CHECK(svc_record_policy_make_name(&tmv, NULL, sizeof(buf)) < 0, "NULL 缓冲应失败");
}

/* ─────────── ② 环形覆盖 ─────────── */

/** 造 5 个文件,**故意打乱数组顺序**, 用来验证函数真的按名字排序了 */
static void fill_shuffled(svc_record_policy_file_t *f)
{
    mk(&f[0], "2026-09-16-21-00-05.mp4", 10);   /* 最新 */
    mk(&f[1], "2026-09-16-21-00-02.mp4", 10);
    mk(&f[2], "2026-09-16-21-00-04.mp4", 10);
    mk(&f[3], "2026-09-16-21-00-01.mp4", 10);   /* 最旧 */
    mk(&f[4], "2026-09-16-21-00-03.mp4", 10);
}

static void test_ring(void)
{
    svc_record_policy_file_t     f[5];
    svc_record_policy_limits_t   lim;
    int                      del[8];
    int                      n;

    printf("\n[2] 环形覆盖决策(5 个文件 × 10 字节, 数组顺序故意打乱)\n");
    fill_shuffled(f);

    /* 上限 30 → 必须删 2 个, 且删的是**最旧的两个** */
    memset(&lim, 0, sizeof(lim));
    lim.limit_bytes = 30;
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 8);
    printf("    上限 30 → 要删 %d 个, 下标 %d,%d(最旧的应在 21-00-01 与 -02)\n",
           n, n > 0 ? del[0] : -1, n > 1 ? del[1] : -1);
    CHECK(n == 2, "应删 2 个");
    CHECK(n == 2 && strcmp(f[del[0]].name, "2026-09-16-21-00-01.mp4") == 0,
          "第一个删的必须是最旧的 21-00-01(证明排序生效)");
    CHECK(n == 2 && strcmp(f[del[1]].name, "2026-09-16-21-00-02.mp4") == 0,
          "第二个删的必须是次旧的 21-00-02");

    /* 刚好等于上限 → 一个都不删 */
    lim.limit_bytes = 50;
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 8);
    printf("    上限 50(刚好等于总量) → 要删 %d 个\n", n);
    CHECK(n == 0, "刚好等于上限不该删");

    /* ★ 上限比单个文件还小 → 仍要留一个(正在写的那个不能删) */
    lim.limit_bytes = 1;
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 8);
    printf("    上限 1(比单个文件还小) → 要删 %d 个(应留 1 个)\n", n);
    CHECK(n == 4, "★ 必须至少留 1 个文件");
    CHECK(n == 4 && strcmp(f[del[3]].name, "2026-09-16-21-00-04.mp4") == 0,
          "留下的那个应是最新的 21-00-05");

    /* 只有一个文件 → 永不删 */
    lim.limit_bytes = 1;
    n = svc_record_policy_plan_delete(f, 1, &lim, del, 8);
    CHECK(n == 0, "只有一个文件时不该删");
}

/** 环形覆盖的**其余边界**:按个数上限 / 输出缓冲 / 不限 / 参数非法 */
static void test_ring_edges(void)
{
    svc_record_policy_file_t   f[5];
    svc_record_policy_limits_t lim;
    int                        del[8];
    int                        n;

    fill_shuffled(f);

    /* 按**个数**上限 */
    memset(&lim, 0, sizeof(lim));
    lim.limit_files = 2;
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 8);
    printf("    个数上限 2 → 要删 %d 个\n", n);
    CHECK(n == 3, "个数上限 2 应删 3 个");
    CHECK(n == 3 && strcmp(f[del[0]].name, "2026-09-16-21-00-01.mp4") == 0,
          "仍应从最旧的开始删");

    /* 输出缓冲不够 → 只给出能放下的个数(剩下的下一轮再删) */
    lim.limit_bytes = 10;
    lim.limit_files = 0;
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 2);
    CHECK(n == 2, "输出缓冲只有 2 格时应只返回 2 个");

    /* 两个上限都为 0 = 不限 */
    memset(&lim, 0, sizeof(lim));
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 8);
    CHECK(n == 0, "不限时应删 0 个");

    /* 参数非法 */
    CHECK(svc_record_policy_plan_delete(NULL, 5, &lim, del, 8) < 0, "NULL files 应失败");
    CHECK(svc_record_policy_plan_delete(f, 5, NULL, del, 8) < 0, "NULL lim 应失败");
    CHECK(svc_record_policy_plan_delete(f, 5, &lim, NULL, 8) < 0, "NULL 输出应失败");
    CHECK(svc_record_policy_plan_delete(f, 5, &lim, del, 0) < 0, "del_cap=0 应失败");
    CHECK(svc_record_policy_plan_delete(f, SVC_RECORD_POLICY_MAX_FILES + 1,
                                        &lim, del, 8) < 0,
          "文件数超上限应失败(不做部分处理)");
    CHECK(svc_record_policy_plan_delete(f, 0, &lim, del, 8) == 0, "0 个文件应返回 0");
}

int main(void)
{
    printf("==== 录制策略(纯函数)PC 单测 ====\n");
    test_name();
    test_ring();
    test_ring_edges();

    printf("\n==== 结果: %d 通过 / %d 失败 ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
