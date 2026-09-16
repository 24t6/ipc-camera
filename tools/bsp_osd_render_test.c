/**
 * @file bsp_osd_render_test.c
 * @brief OSD 纯渲染逻辑的 PC 原生单测(不需要板子)
 *
 * 为什么这层能不上板就测: `bsp_osd_render.c` **不碰硬件、不 malloc**,
 * 所以和 `protocol` 层一样能直接在 PC 上编译运行。
 *
 * 本测试的核心手法: **把渲染出来的位图打成 ASCII 图**。
 * 光断言"前景像素数 > 0"是不够的 —— 那只能证明"有东西被画了",
 * 不能证明**画出来的是数字**。把点阵打出来, 一眼就能核对。
 *
 * 编译运行: python work/build_bsp_osd_render_test.py
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp_osd_render.h"

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

/** 渲染缓冲: scale=2、19 字符时需要 304×32 = 9728 像素 */
static uint16_t g_buf[10240];

/**
 * @brief 把一块 ARGB1555 位图打成 ASCII 图
 *
 * @param[in] buf    位图
 * @param[in] width  像素宽
 * @param[in] height 像素高
 * @note 只有"是前景色"才打 '#', 其余打 '.', 所以看起来就是字本身。
 */
static void dump_ascii(const uint16_t *buf, int width, int height)
{
    int y;

    for (y = 0; y < height; y++) {
        int x;

        printf("    |");
        for (x = 0; x < width; x++) {
            putchar(buf[(size_t)y * (size_t)width + (size_t)x] == BSP_OSD_RENDER_PIXEL_FG
                        ? '#' : '.');
        }
        printf("|\n");
    }
}

/** 数一数位图里有几个前景像素、几个背景像素 */
static void count_pixels(const uint16_t *buf, size_t n, int *fg, int *bg, int *other)
{
    size_t i;

    *fg = 0;
    *bg = 0;
    *other = 0;
    for (i = 0; i < n; i++) {
        if (buf[i] == BSP_OSD_RENDER_PIXEL_FG) {
            (*fg)++;
        } else if (buf[i] == BSP_OSD_RENDER_PIXEL_BG) {
            (*bg)++;
        } else {
            (*other)++;
        }
    }
}

/* ---------- 各测试项 ---------- */

static void test_format_time(void)
{
    struct tm tmv;
    char      out[BSP_OSD_RENDER_MAX_CHARS];
    int       n;

    printf("\n[1] 时间格式化\n");
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = 2026 - 1900;
    tmv.tm_mon  = 9 - 1;        /* tm_mon 从 0 开始! */
    tmv.tm_mday = 16;
    tmv.tm_hour = 17;
    tmv.tm_min  = 3;            /* 故意用个位数, 检验是否补零 */
    tmv.tm_sec  = 5;

    n = bsp_osd_render_format_time(&tmv, out, sizeof(out));
    printf("    结果: \"%s\"  (长度 %d)\n", out, n);
    CHECK(n == 19, "长度应为 19");
    CHECK(strcmp(out, "2026-09-16 17:03:05") == 0, "格式应为 YYYY-MM-DD HH:MM:SS 且补零");
    CHECK(bsp_osd_render_format_time(NULL, out, sizeof(out)) < 0, "NULL 时间应失败");
    CHECK(bsp_osd_render_format_time(&tmv, out, 4) < 0, "缓冲太小应失败");
}

static void test_glyph_bits(void)
{
    int all_blank = 1;
    int r;
    int c;

    printf("\n[2] 字模本身\n");
    /* '0' 的第 2 行应该是 ..####.. → 列 2..5 亮, 列 0/1/6/7 灭 */
    CHECK(bsp_osd_render_glyph_bit('0', 2, 2) == 1, "'0' (2,2) 应为亮");
    CHECK(bsp_osd_render_glyph_bit('0', 2, 5) == 1, "'0' (2,5) 应为亮");
    CHECK(bsp_osd_render_glyph_bit('0', 2, 0) == 0, "'0' (2,0) 应为灭");
    CHECK(bsp_osd_render_glyph_bit('0', 2, 7) == 0, "'0' (2,7) 应为灭");
    /* '1' 的第 13 行是底座 .######. */
    CHECK(bsp_osd_render_glyph_bit('1', 13, 1) == 1, "'1' 底座左端应为亮");
    CHECK(bsp_osd_render_glyph_bit('1', 13, 6) == 1, "'1' 底座右端应为亮");
    /* ':' 两点在 5~6 行和 9~10 行, 中间 7~8 行必须是空的 */
    CHECK(bsp_osd_render_glyph_bit(':', 5, 3) == 1, "':' 上点应为亮");
    CHECK(bsp_osd_render_glyph_bit(':', 9, 3) == 1, "':' 下点应为亮");
    CHECK(bsp_osd_render_glyph_bit(':', 7, 3) == 0, "':' 两点之间应为灭");
    /* '-' 只在 7~8 行 */
    CHECK(bsp_osd_render_glyph_bit('-', 7, 3) == 1, "'-' 应为亮");
    CHECK(bsp_osd_render_glyph_bit('-', 2, 3) == 0, "'-' 上方应为灭");
    /* '-' 与 ':' 不能互相污染(它们相邻, 最容易串) */
    CHECK(bsp_osd_render_glyph_bit('-', 5, 3) == 0, "'-' 不该有冒号的上点");
    CHECK(bsp_osd_render_glyph_bit(':', 7, 2) == 0, "':' 不该有减号的笔画");

    for (r = 0; r < BSP_OSD_RENDER_GLYPH_H; r++) {
        for (c = 0; c < BSP_OSD_RENDER_GLYPH_W; c++) {
            if (bsp_osd_render_glyph_bit(' ', r, c)) {
                all_blank = 0;
            }
        }
    }
    CHECK(all_blank == 1, "空格必须全灭");
    CHECK(bsp_osd_render_glyph_bit('0', -1, 0) == 0, "行越界应返回 0");
    CHECK(bsp_osd_render_glyph_bit('0', BSP_OSD_RENDER_GLYPH_H, 0) == 0, "行越界应返回 0");
    CHECK(bsp_osd_render_glyph_bit('x', 2, 2) == 0, "不认识的字符按空格处理");
}

static void test_measure(void)
{
    bsp_osd_render_size_t sz;

    printf("\n[3] 尺寸计算\n");
    CHECK(bsp_osd_render_measure("2026-09-16 17:03:05", 1, &sz) == 0, "scale=1 应成功");
    printf("    scale=1 → %d×%d (%d 字符)\n", sz.width, sz.height, sz.chars);
    CHECK(sz.chars == 19 && sz.width == 152 && sz.height == 16, "scale=1 应为 152×16");

    CHECK(bsp_osd_render_measure("2026-09-16 17:03:05", 2, &sz) == 0, "scale=2 应成功");
    printf("    scale=2 → %d×%d\n", sz.width, sz.height);
    CHECK(sz.width == 304 && sz.height == 32, "scale=2 应为 304×32");

    /* 对齐要求: 海思 OVERLAY 的宽高都必须是偶数, 否则 Create 会报错 */
    CHECK(sz.width % 2 == 0 && sz.height % 2 == 0, "宽高必须是偶数(海思要求)");

    CHECK(bsp_osd_render_measure("", 1, &sz) < 0, "空串应失败");
    CHECK(bsp_osd_render_measure("2026-09-16 17:03:05", 0, &sz) < 0, "scale=0 应失败");
    CHECK(bsp_osd_render_measure(NULL, 1, &sz) < 0, "NULL 应失败");
}

static void test_render(void)
{
    bsp_osd_render_size_t sz;
    int               fg;
    int               bg;
    int               other;
    size_t            n;

    printf("\n[4] 渲染成位图\n");
    CHECK(bsp_osd_render_text("2026-09-16 17:03:05", 1, g_buf, 10240, &sz) == 0,
          "渲染应成功");
    n = (size_t)sz.width * (size_t)sz.height;
    count_pixels(g_buf, n, &fg, &bg, &other);
    printf("    前景 %d 像素 / 背景 %d 像素 / 其他 %d 像素\n", fg, bg, other);

    CHECK(other == 0, "所有像素只能是前景色或背景色");
    CHECK(fg > 0, "必须有前景像素(否则等于什么都没画)");
    CHECK(bg > 0, "必须有背景像素(否则水印会是一整块实心)");
    /* 文字只应占一部分。数字笔画密, 实测约 24%;
     * 这里只防"糊成一整块"(接近 100%)—— 那说明字模或铺像素写错了。 */
    CHECK(fg * 2 < (int)n, "前景占比应小于一半(否则说明糊了)");

    /* 缓冲不够时必须明确失败, 而不是越界写 */
    CHECK(bsp_osd_render_text("2026-09-16 17:03:05", 1, g_buf, 10, &sz) < 0,
          "缓冲不够应失败");
    CHECK(bsp_osd_render_text(NULL, 1, g_buf, 10240, &sz) < 0, "NULL 文本应失败");

    /* 整串空格的极端情况: 全是背景, 没有前景 */
    CHECK(bsp_osd_render_text("   ", 1, g_buf, 10240, &sz) == 0, "全空格应渲染成功");
    count_pixels(g_buf, (size_t)sz.width * (size_t)sz.height, &fg, &bg, &other);
    CHECK(fg == 0, "全空格不该有前景像素");
    CHECK(bg == (int)((size_t)sz.width * (size_t)sz.height), "全空格应全是背景");
}

static void test_visual(void)
{
    bsp_osd_render_size_t sz;

    printf("\n[5] ★ 把渲染结果打出来看(这是最关键的验证: 必须能读出数字)\n");
    if (bsp_osd_render_text("2026-09-16 17:03:05", 1, g_buf, 10240, &sz) != 0) {
        CHECK(0, "渲染失败, 无法目视");
        return;
    }
    printf("    \"2026-09-16 17:03:05\" (scale=1, %d×%d):\n", sz.width, sz.height);
    dump_ascii(g_buf, sz.width, sz.height);

    printf("\n    \"48:48\" —— 补上时间串里没出现的 4 和 8:\n");
    bsp_osd_render_text("48:48", 1, g_buf, 10240, &sz);
    dump_ascii(g_buf, sz.width, sz.height);

    /* 放大 2 倍后, 同一个字形的笔画必须变成 2×2 的块 */
    printf("\n    scale=2 的 \"5\"(放大必须是实心块, 不能稀释):\n");
    bsp_osd_render_text("5", 2, g_buf, 10240, &sz);
    dump_ascii(g_buf, sz.width, sz.height);
}

int main(void)
{
    printf("==== OSD 纯渲染逻辑单测 ====\n");
    test_format_time();
    test_glyph_bits();
    test_measure();
    test_render();
    test_visual();

    printf("\n==== 结果: %d 通过 / %d 失败 ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
