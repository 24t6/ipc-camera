/**
 * @file osd_render.h
 * @brief OSD 时间水印的**纯渲染逻辑** —— 时间格式化 + 点阵字形 → ARGB1555 位图
 *
 * @note **这一层不碰任何硬件、不做动态分配**, 所以能在 PC 上原生单测 ——
 *       和 `protocol` 层用的是同一个手法, 只是这次用在 `bsp` 内部。
 *       真正调 `HI_MPI_RGN_*` 的部分在 `bsp_osd.c`。
 */
#ifndef __BSP_OSD_RENDER_H__
#define __BSP_OSD_RENDER_H__

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/** 水印最长的字符数(含结尾 '\0')。19 个字符的时间串 + 余量 */
#define OSD_RENDER_MAX_CHARS 24

/** 一个字形在字模里的尺寸(像素) */
#define OSD_GLYPH_W 8
#define OSD_GLYPH_H 16

/**
 * 文字像素: ARGB1555, **alpha 位 = 1** → 按 `u32FgAlpha` 混叠。
 * 0xFFFF = 不透明白。
 */
#define OSD_PIXEL_FG 0xFFFFu

/**
 * 背景像素: ARGB1555, **alpha 位 = 0** → 按 `u32BgAlpha` 混叠。
 * 0x0000 配上 `u32BgAlpha = 0` 就是**全透明**, 视频从水印底下透出来。
 */
#define OSD_PIXEL_BG 0x0000u

/** 渲染结果的尺寸 */
typedef struct {
    int width;   /**< 像素宽 = 字符数 × 8 × scale */
    int height;  /**< 像素高 = 16 × scale */
    int chars;   /**< 字符数 */
    int scale;   /**< 放大倍数(1 = 原生 8×16) */
} osd_render_size_t;

/**
 * @brief 把时间格式化成 `YYYY-MM-DD HH:MM:SS`
 *
 * @param[in]  tmv 本地时间(不可为 NULL)
 * @param[out] out 输出缓冲
 * @param[in]  cap 缓冲容量(字节); 19 字符 + '\0' 需要 20
 * @return 写入的字符数(不含 '\0'); <0 表示失败
 * @note 纯函数, 不分配内存, 不碰硬件。可在 PC 上单测。
 */
int osd_render_format_time(const struct tm *tmv, char *out, size_t cap);

/**
 * @brief 算出渲染一段文本需要多大缓冲
 *
 * @param[in]  text  待渲染文本
 * @param[in]  scale 放大倍数(≥1)
 * @param[out] out   尺寸
 * @return 0 成功; -1 参数非法(空串 / 超长 / scale<1)
 * @note 纯函数。
 */
int osd_render_measure(const char *text, int scale, osd_render_size_t *out);

/**
 * @brief 把文本渲染成 ARGB1555 位图
 *
 * @param[in]  text       待渲染文本(只认 `0-9` `-` `:` 空格, 其他按空格处理)
 * @param[in]  scale      放大倍数(≥1)
 * @param[out] buf        ARGB1555 像素缓冲
 * @param[in]  buf_pixels 缓冲能装多少个像素(不是字节数!)
 * @param[out] out        实际渲染尺寸, 可为 NULL
 * @return 0 成功; -1 参数非法或缓冲不够
 * @note 纯函数, 不分配内存。**背景会被完整填成 OSD_PIXEL_BG**,
 *       所以调用方不需要预先清零。
 */
int osd_render_text(const char *text, int scale, uint16_t *buf,
                    size_t buf_pixels, osd_render_size_t *out);

/**
 * @brief 取字模里某个像素(供单测与调试用)
 *
 * @param[in] ch  字符
 * @param[in] row 行(0..15)
 * @param[in] col 列(0..7)
 * @return 1 = 该点亮; 0 = 灭(越界或不认识的字符也返回 0)
 * @note 纯函数。存在的意义是让"字模画对了没有"**可以被测试直接问**。
 */
int osd_render_glyph_bit(char ch, int row, int col);

#endif /* __BSP_OSD_RENDER_H__ */
