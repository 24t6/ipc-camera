/**
 * @file osd_render.c
 * @brief OSD 时间水印的纯渲染逻辑(字模 + 位图生成)
 *
 * ## 字模为什么写成源码里的 ASCII 图, 而不是复用参考项目的汉字库
 *
 * `work/ipc_camera_ref/.../osd/hh_hz16lib.c` 里确实有字库, 但:
 *   · 它是 **2.9 MB / 37,371 行**的**汉字**库(8170 字 × 32 字节), 板子 rootfs 只剩 10.2 MB
 *   · 真正启用的 12/8 点阵用**十进制**存, 且**字形顺序是私有的**
 *     (实测画出前几个字形, 既不是 `!` 也不是 `"`)—— 要取 `0-9` 得先逆向它的索引
 *   · 我们只需要 **13 个字形**
 *
 * 所以按"够用就好"自己写, 而且是**用可见的 ASCII 图直接写在源码里**:
 * 每个字形长什么样**一眼就能核对**, 不需要再写一个工具去验证它。
 * (规矩 7.0 说"优先复用" —— 这里真正复用下来的是 **REGION 的调用流程**,
 *  见 `bsp_osd.c`; 见 `docs/问题与解决记录.md` 的选型记录。)
 *
 * ## 字模格式
 *
 * 每个字形是 **16 行 × 8 列**, 连续 128 个字符, 每行 8 个。
 * 取第 `row` 行第 `col` 列 = `art[row * 8 + col] == '#'`。
 * 有效笔画画在 **第 2~13 行**, 左右各留 1 列, 这样数字之间自然有空隙。
 */
#include "osd_render.h"

#include <string.h>

/* ---------- 字模 ---------- */

static const char ART_0[] =
    "........" "........" "..####.." ".##..##."
    ".##..##." ".##..##." ".##..##." ".##..##."
    ".##..##." ".##..##." ".##..##." ".##..##."
    ".##..##." "..####.." "........" "........";

static const char ART_1[] =
    "........" "........" "...##..." "..###..."
    ".####..." "...##..." "...##..." "...##..."
    "...##..." "...##..." "...##..." "...##..."
    "...##..." ".######." "........" "........";

static const char ART_2[] =
    "........" "........" "..####.." ".##..##."
    ".##..##." ".....##." "....##.." "...##..."
    "..##...." ".##....." ".##....." ".##....."
    ".##....." ".######." "........" "........";

static const char ART_3[] =
    "........" "........" "..####.." ".##..##."
    ".....##." ".....##." "....##.." "...###.."
    "....##.." ".....##." ".....##." ".##..##."
    ".##..##." "..####.." "........" "........";

static const char ART_4[] =
    "........" "........" "....##.." "...###.."
    "..####.." ".#..##.." ".#..##.." "##..##.."
    ".######." "....##.." "....##.." "....##.."
    "....##.." "....##.." "........" "........";

static const char ART_5[] =
    "........" "........" ".######." ".##....."
    ".##....." ".##....." ".#####.." ".....##."
    ".....##." ".....##." ".....##." ".##..##."
    ".##..##." "..####.." "........" "........";

static const char ART_6[] =
    "........" "........" "..####.." ".##..##."
    ".##....." ".##....." ".##....." ".#####.."
    ".##..##." ".##..##." ".##..##." ".##..##."
    ".##..##." "..####.." "........" "........";

static const char ART_7[] =
    "........" "........" ".######." ".##..##."
    ".....##." "....##.." "....##.." "...##..."
    "...##..." "..##...." "..##...." "..##...."
    "..##...." "..##...." "........" "........";

static const char ART_8[] =
    "........" "........" "..####.." ".##..##."
    ".##..##." ".##..##." "..####.." ".##..##."
    ".##..##." ".##..##." ".##..##." ".##..##."
    ".##..##." "..####.." "........" "........";

static const char ART_9[] =
    "........" "........" "..####.." ".##..##."
    ".##..##." ".##..##." ".##..##." "..#####."
    ".....##." ".....##." ".....##." ".##..##."
    ".##..##." "..####.." "........" "........";

/* 减号: 画在第 7、8 行(字形纵向正中) */
static const char ART_DASH[] =
    "........" "........" "........" "........"
    "........" "........" "........" "..####.."
    "..####.." "........" "........" "........"
    "........" "........" "........" "........";

/* 冒号: 两个点, 分别在 5~6 行和 9~10 行 */
static const char ART_COLON[] =
    "........" "........" "........" "........"
    "........" "...##..." "...##..." "........"
    "........" "...##..." "...##..." "........"
    "........" "........" "........" "........";

/* 空格: 全灭 */
static const char ART_SPACE[] =
    "........" "........" "........" "........"
    "........" "........" "........" "........"
    "........" "........" "........" "........"
    "........" "........" "........" "........";

/** 一个字形: 字符 → 它的 ASCII 图(长度必须是 8×16 = 128) */
typedef struct {
    char        ch;
    const char *art;
} osd_glyph_t;

static const osd_glyph_t GLYPHS[] = {
    { '0', ART_0 },     { '1', ART_1 },     { '2', ART_2 },
    { '3', ART_3 },     { '4', ART_4 },     { '5', ART_5 },
    { '6', ART_6 },     { '7', ART_7 },     { '8', ART_8 },
    { '9', ART_9 },     { '-', ART_DASH },  { ':', ART_COLON },
    { ' ', ART_SPACE },
};

#define GLYPH_COUNT ((int)(sizeof(GLYPHS) / sizeof(GLYPHS[0])))

/**
 * @brief 查字符对应的字模图
 *
 * @param[in] ch 字符
 * @return 字模图指针; 不认识的字符返回空格的字模(不是 NULL, 调用方省一次判空)
 * @note 纯函数。13 项线性查找, 每秒只调几百次, 不值得为它建哈希。
 */
static const char *osd_lookup_art(char ch)
{
    int i;

    for (i = 0; i < GLYPH_COUNT; i++) {
        if (GLYPHS[i].ch == ch) {
            return GLYPHS[i].art;
        }
    }
    return ART_SPACE;
}

/* ---------- 画图 ---------- */

/**
 * @brief 把一个 scale×scale 的实心方块涂成 color
 *
 * @param[out] buf        ARGB1555 缓冲
 * @param[in]  stride_px  一行的像素数
 * @param[in]  x,y        方块左上角
 * @param[in]  scale      方块边长
 * @param[in]  color      像素值
 * @note 纯函数。放大就是"一个点涂成 scale×scale 的块", 不做插值
 *       —— 水印要的是清晰, 不是平滑。
 */
static void osd_fill_block(uint16_t *buf, int stride_px, int x, int y,
                           int scale, uint16_t color)
{
    int dy;
    int dx;

    for (dy = 0; dy < scale; dy++) {
        uint16_t *row = buf + (size_t)(y + dy) * (size_t)stride_px + (size_t)x;

        for (dx = 0; dx < scale; dx++) {
            row[dx] = color;
        }
    }
}

/**
 * @brief 把一个字形画进缓冲
 *
 * @param[out] buf        ARGB1555 缓冲
 * @param[in]  stride_px  一行的像素数
 * @param[in]  ch         字符
 * @param[in]  x0         字形左上角的 x
 * @param[in]  scale      放大倍数
 * @note 只画"亮"的点; 背景已被调用方 memset 成透明, 所以不用画暗点。
 */
static void osd_draw_glyph(uint16_t *buf, int stride_px, char ch,
                           int x0, int scale)
{
    int row;
    int col;

    for (row = 0; row < OSD_GLYPH_H; row++) {
        for (col = 0; col < OSD_GLYPH_W; col++) {
            if (osd_render_glyph_bit(ch, row, col)) {
                osd_fill_block(buf, stride_px, x0 + col * scale, row * scale,
                               scale, OSD_PIXEL_FG);
            }
        }
    }
}

/* ---------- 对外接口 ---------- */

int osd_render_format_time(const struct tm *tmv, char *out, size_t cap)
{
    size_t n;

    if (tmv == NULL || out == NULL || cap == 0) {
        return -1;
    }
    n = strftime(out, cap, "%Y-%m-%d %H:%M:%S", tmv);
    if (n == 0) {
        return -1;
    }
    return (int)n;
}

int osd_render_measure(const char *text, int scale, osd_render_size_t *out)
{
    size_t n;

    if (text == NULL || out == NULL || scale < 1) {
        return -1;
    }
    n = strlen(text);
    if (n == 0 || n >= (size_t)OSD_RENDER_MAX_CHARS) {
        return -1;
    }
    out->chars  = (int)n;
    out->scale  = scale;
    out->width  = (int)n * OSD_GLYPH_W * scale;
    out->height = OSD_GLYPH_H * scale;
    return 0;
}

int osd_render_text(const char *text, int scale, uint16_t *buf,
                    size_t buf_pixels, osd_render_size_t *out)
{
    osd_render_size_t sz;
    size_t            need;
    int               i;

    if (buf == NULL || osd_render_measure(text, scale, &sz) != 0) {
        return -1;
    }
    need = (size_t)sz.width * (size_t)sz.height;
    if (buf_pixels < need) {
        return -1;
    }
    /* 先把整块填成全透明, 再往上面点笔画 ——
     * 这样"没画到的地方一定是透明的", 不会留下上一帧的残影。 */
    memset(buf, 0, need * sizeof(uint16_t));
    for (i = 0; i < sz.chars; i++) {
        osd_draw_glyph(buf, sz.width, text[i], i * OSD_GLYPH_W * scale, scale);
    }
    if (out != NULL) {
        *out = sz;
    }
    return 0;
}

int osd_render_glyph_bit(char ch, int row, int col)
{
    if (row < 0 || row >= OSD_GLYPH_H || col < 0 || col >= OSD_GLYPH_W) {
        return 0;
    }
    return (osd_lookup_art(ch)[row * OSD_GLYPH_W + col] == '#') ? 1 : 0;
}
