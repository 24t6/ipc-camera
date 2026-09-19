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
    f->size   = size;
    f->locked = 0;      /* ★ 必须清掉:局部数组是栈上的垃圾值, 不清会让用例随机失败 */
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

/**
 * @brief **锁定段保护**:被锁的分段环形覆盖必须跳过, 并且继续删下一个最旧的
 *
 * @note 这是"锁一个不影响清理其它"的核心语义 —— 反例(错的实现)是"遇到锁就 break",
 *       那会导致:一个锁定段把整个环形清理**卡死**(盘满了却谁也不删)。
 */
static void test_lock(void)
{
    svc_record_policy_file_t   f[5];
    svc_record_policy_limits_t lim;
    int                        del[8];
    int                        n;

    printf("\n[4] 锁定段保护(锁定的跳过, 但继续删下一个最旧的)\n");

    /* ① 锁住**最旧**的那个:上限 30(总量 50)本该删 2 个 ⇒
     *    现在应改成删 -02 和 -03, 而 -01(锁定)**一个字节都不许动** */
    fill_shuffled(f);
    f[3].locked = 1;                            /* f[3] = 21-00-01, 最旧 */
    memset(&lim, 0, sizeof(lim));
    lim.limit_bytes = 30;
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 8);
    printf("    锁住最旧的 21-00-01, 上限 30 → 要删 %d 个: %s\n", n,
           (n > 0 && n <= 2) ? f[del[0]].name : "?");
    CHECK(n == 2, "锁定一个不该减少清理个数(应照样删 2 个)");
    if (n == 2) {
        CHECK(strcmp(f[del[0]].name, "2026-09-16-21-00-02.mp4") == 0,
              "★ 跳过锁定的 21-00-01, 第一个该删 21-00-02");
        CHECK(strcmp(f[del[1]].name, "2026-09-16-21-00-03.mp4") == 0,
              "★ 继续删下一个最旧的 21-00-03(而不是遇到锁就放弃)");
    }

    /* ② 锁住**最新**的那个:它本来就不该被删, 结果应完全不变 */
    fill_shuffled(f);
    f[0].locked = 1;                            /* f[0] = 21-00-05, 最新 */
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 8);
    CHECK(n == 2 && strcmp(f[del[0]].name, "2026-09-16-21-00-01.mp4") == 0 &&
          strcmp(f[del[1]].name, "2026-09-16-21-00-02.mp4") == 0,
          "锁最新的那个不影响结果(仍从最旧开始删)");

    /* ③ **全被锁定** ⇒ 一个也删不掉(调用方要如实报错, 不许自动解锁) */
    fill_shuffled(f);
    for (n = 0; n < 5; n++) {
        f[n].locked = 1;
    }
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 8);
    printf("    全部锁定 + 上限 30 → 要删 %d 个(应为 0, 由调用方报错)\n", n);
    CHECK(n == 0, "全被锁定时不许删任何东西");

    /* ④ 只剩 1 个且被锁 ⇒ 仍然"至少留一个", 不越界 */
    fill_shuffled(f);
    f[3].locked = 1;
    lim.limit_bytes = 1;
    n = svc_record_policy_plan_delete(f, 5, &lim, del, 8);
    CHECK(n == 4, "上限 1 时仍应删 4 个(留最新那个)");
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

/**
 * @brief 分段"该收了吗" —— 时间到 **或** 大小到(谁先到算谁)
 *
 * @note 重点测**两个条件互相独立**:只到时间 / 只到大小 / 都到 / 都没到;
 *       以及"0 = 不限"这条约定(否则 `-s 0` 或 `-m 0` 会被误判成"立刻收段")。
 */
static void test_should_close(void)
{
    const uint32_t F = 54000;                    /* 30 分钟 × 30fps */
    const uint64_t B = 1024ULL * 1024 * 1024;    /* 1 GiB */

    printf("\n[3] 分段该收了吗(时间 OR 大小)\n");

    /* 都没到 → 继续写 */
    CHECK(svc_record_policy_should_close(100, F, 1000, B) == 0, "都没到不该收段");
    /* 时间到(边界是 >=) */
    CHECK(svc_record_policy_should_close(F - 1, F, 0, B) == 0, "差一帧不该收段");
    CHECK(svc_record_policy_should_close(F, F, 0, B) == 1, "刚好到帧数上限应收段");
    CHECK(svc_record_policy_should_close(F + 29, F, 0, B) == 1, "超过帧数上限应收段");
    /* 大小到 */
    CHECK(svc_record_policy_should_close(0, F, B - 1, B) == 0, "差一字节不该收段");
    CHECK(svc_record_policy_should_close(0, F, B, B) == 1, "刚好到大小上限应收段");
    /* 只到大小、帧数远没到 —— 这是新增的那一维, 必须能单独触发 */
    CHECK(svc_record_policy_should_close(500, F, B + 1, B) == 1,
          "帧数远没到、但大小到了, 也该收段");
    /* 两个都到 */
    CHECK(svc_record_policy_should_close(F, F, B, B) == 1, "两个都到应收段");
    /* 0 = 该项不限(⚠️ 注意: 要把**另一维**给到上限才能验证"这一维不再拦" ——
     *   第一版这两条断言我把参数填反了, 于是"测试失败"其实是**测试写错了**) */
    CHECK(svc_record_policy_should_close(F + 1, 0, B, B) == 1,
          "帧数不限(0)时, 仍能由大小触发");
    CHECK(svc_record_policy_should_close(F, F, B * 100, 0) == 1,
          "大小不限(0)时, 仍能由时间触发");
    CHECK(svc_record_policy_should_close(100, 0, 1000, 0) == 0,
          "两个都不限 ⇒ 永不自动收段(只能靠停机收尾)");
}

/**
 * @brief 排序(回放列表要"新→旧", 环形覆盖要"旧→新")
 */
static void test_sort(void)
{
    svc_record_policy_file_t f[5];

    printf("\n[5] 按名字(= 时间)排序\n");

    fill_shuffled(f);
    svc_record_policy_sort(f, 5, 0);            /* 旧 → 新 */
    CHECK(strcmp(f[0].name, "2026-09-16-21-00-01.mp4") == 0 &&
          strcmp(f[4].name, "2026-09-16-21-00-05.mp4") == 0,
          "升序: 最旧在前");

    fill_shuffled(f);
    svc_record_policy_sort(f, 5, 1);            /* 新 → 旧(回放列表用) */
    CHECK(strcmp(f[0].name, "2026-09-16-21-00-05.mp4") == 0 &&
          strcmp(f[4].name, "2026-09-16-21-00-01.mp4") == 0,
          "降序: 最新在前");
    CHECK(f[1].size == 10 && f[1].locked == 0, "排序时 size/locked 跟着一起搬");

    svc_record_policy_sort(f, 0, 1);            /* 不该崩 */
    svc_record_policy_sort(NULL, 5, 1);
    CHECK(1, "0 个 / NULL 不崩");
}

/**
 * @brief 锁定清单的**文本**增删查(读写文件在 svc_record.c, 这里只测纯逻辑)
 */
static void test_lock_list(void)
{
    char   buf[128];
    size_t used = 0;
    int    n;

    printf("\n[6] 锁定清单文本(增 / 删 / 查)\n");
    buf[0] = '\0';

    n = svc_record_policy_lock_edit(buf, sizeof(buf), used, "a.mp4", 1);
    CHECK(n == 6 && strcmp(buf, "a.mp4\n") == 0, "空清单加入 a.mp4");
    used = (size_t)n;

    n = svc_record_policy_lock_edit(buf, sizeof(buf), used, "a.mp4", 1);
    CHECK(n == (int)used && strcmp(buf, "a.mp4\n") == 0, "重复加入是幂等的");

    n = svc_record_policy_lock_edit(buf, sizeof(buf), used, "b.mp4", 1);
    used = (size_t)n;
    CHECK(strcmp(buf, "a.mp4\nb.mp4\n") == 0, "再加一个 → 两行");

    CHECK(svc_record_policy_lock_has(buf, used, "b.mp4") == 1, "查得到 b.mp4");
    CHECK(svc_record_policy_lock_has(buf, used, "c.mp4") == 0, "查不到 c.mp4");
    CHECK(svc_record_policy_lock_has(buf, used, "") == 0, "空名字查不到");

    n = svc_record_policy_lock_edit(buf, sizeof(buf), used, "a.mp4", 0);
    used = (size_t)n;
    CHECK(strcmp(buf, "b.mp4\n") == 0, "删掉 a.mp4 只剩 b.mp4");
    CHECK(svc_record_policy_lock_has(buf, used, "a.mp4") == 0, "删完查不到");

    n = svc_record_policy_lock_edit(buf, sizeof(buf), used, "zzz.mp4", 0);
    CHECK(n == (int)used, "删一个不在清单里的名字 = 原样返回");
    n = svc_record_policy_lock_edit(buf, sizeof(buf), used, "x.mp4", 1);
    used = (size_t)n;
    CHECK(strcmp(buf, "b.mp4\nx.mp4\n") == 0, "再删再加后顺序正确");

    /* 注释行 / 空行 / 行尾 CR 都要能容忍(清单可能被人手工编辑过) */
    {
        const char *raw = "# 这是注释\n\nb.mp4\r\n  \n";

        CHECK(svc_record_policy_lock_has(raw, strlen(raw), "b.mp4") == 1,
              "手工编辑过的清单: CR/空行/注释都能容忍");
        CHECK(svc_record_policy_lock_has(raw, strlen(raw), "#") == 0,
              "注释行不算名字");
    }

    /* 放不下 → -1, 且**缓冲没被改坏** */
    {
        char small[12];

        strcpy(small, "b.mp4\nx.mp4");      /* 12 字节, 正好放满 */
        n = svc_record_policy_lock_edit(small, sizeof(small), strlen(small),
                                        "yyyyyyyy.mp4", 1);
        CHECK(n == -1 && strcmp(small, "b.mp4\nx.mp4") == 0,
              "放不下 → -1 且原文本不变");
    }

    /* ★ 名字里带换行 = 会往清单里注入额外的行 —— 必须拒绝 */
    n = svc_record_policy_lock_edit(buf, sizeof(buf), used, "bad\nname", 1);
    CHECK(n == -1, "★ 名字含换行 → 拒绝(否则能注入清单)");
}

int main(void)
{
    printf("==== 录制策略(纯函数)PC 单测 ====\n");
    test_name();
    test_ring();
    test_ring_edges();
    test_should_close();
    test_lock();
    test_sort();
    test_lock_list();

    printf("\n==== 结果: %d 通过 / %d 失败 ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
