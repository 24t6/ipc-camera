/**
 * @file bsp_osd.h
 * @brief OSD 时间水印的**板级实现** —— 海思 REGION 模块(OVERLAY)挂到 VENC 通道
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它在分层里的位置(M2 拆成三块, 各自能独立验证)
 * ─────────────────────────────────────────────────────────────────
 *      `bsp_osd_render.c`   纯函数: 时间 + 点阵字模 → ARGB1555 位图
 *                       **不碰硬件 → PC 上原生单测**(见 `tools/bsp_osd_render_test.c`)
 *      `bsp_osd.c`      **本文件**: 只有它碰 `HI_MPI_RGN_*`
 *                       → **只能板上验**
 *      `svc_osd.c`      1 Hz 线程, 每秒调一次 `bsp_osd_show()`
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⭐ 复用说明(AGENTS.md §7.0「优先复用」)
 * ─────────────────────────────────────────────────────────────────
 *  调用序列与属性字段**照抄厂商 sample**, 不自己发明:
 *      · `sample/common/sample_comm_region.c` 的 `SAMPLE_REGION_CreateOverLay()`
 *        —— 区域属性(`OVERLAY_RGN` / `ARGB_1555` / `u32CanvasNum = 2`)
 *      · 同文件的 `SAMPLE_COMM_REGION_AttachToChn()` 的 `OVERLAY_RGN` 分支
 *        —— 挂载属性(含 QP / 反色 / LUT 那一堆字段, 它们只影响 JPEG 抓拍)
 *      · 同文件的 `SAMPLE_REGION_SetBitMap()` —— 喂位图
 *      · 句柄取值依据 `#define OverlayMinHandle 0`
 *  **顺序也是有意的**:`Create` → `AttachToChn` → `SetBitMap`
 *  (厂商 sample 就是"先挂载、再喂位图")。
 *
 * ─────────────────────────────────────────────────────────────────
 *  颜色约定(与 `bsp_osd_render.h` 一致)
 * ─────────────────────────────────────────────────────────────────
 *  `PIXEL_FORMAT_ARGB_1555`; 文字 = `0xFFFF`(alpha 位 1),
 *  背景 = `0x0000`(alpha 位 0)。再配 `u32BgAlpha = 0` → **背景全透明**,
 *  视频从水印底下透出来(而不是盖一个黑框)。
 *
 * @note 本模块**不是线程安全的**:`bsp_osd_*` 只允许从**一个**线程调用
 *       (按架构, 由 `svc_osd` 的 1 Hz 线程独占)。
 */
#ifndef __BSP_OSD_H__
#define __BSP_OSD_H__

#include <stdint.h>

#include "bsp_osd_render.h"

/** 水印字符数: `YYYY-MM-DD HH:MM:SS` 恰好 19 个 */
#define BSP_OSD_TIME_CHARS 19

/** 放大倍数(2 → 每个点铺成 2×2, 有效字号 16×32, 远看也清楚) */
#define BSP_OSD_SCALE 2

/**
 * 水印区域尺寸(像素)。
 *
 * @note 海思要求 OVERLAY 的宽高为**偶数**(对齐 2);
 *       `8 × 2 = 16`、`16 × 2 = 32` 天然满足。
 */
#define BSP_OSD_REGION_W (BSP_OSD_TIME_CHARS * BSP_OSD_RENDER_GLYPH_W * BSP_OSD_SCALE)
#define BSP_OSD_REGION_H (BSP_OSD_RENDER_GLYPH_H * BSP_OSD_SCALE)

/** 区域离画面边缘的留白(像素) */
#define BSP_OSD_MARGIN 16

/** OSD 状态与统计(用于日志与验收断言) */
typedef struct {
    int      started;     /**< 1 = 已创建并挂载到 VENC */
    int      venc_chn;    /**< 挂在哪一路 VENC */
    int      pos_x;       /**< 区域左上角 x(图像坐标) */
    int      pos_y;       /**< 区域左上角 y */
    int      width;       /**< 区域像素宽 */
    int      height;      /**< 区域像素高 */
    uint64_t shows;       /**< 成功提交水印的次数(应 ≈ 运行秒数) */
    uint64_t errors;      /**< 失败次数(验收要求 = 0) */
} bsp_osd_stats_t;

/**
 * 初始化 OSD:建区域 → 挂到当前选中的 VENC 通道 → 摆到右上角。
 *
 * @return 0 成功; 负值失败
 *
 * @note 通道号与画面尺寸**由 `bsp_mpp` 提供**(`bsp_mpp_get_venc_chn()` /
 *       `bsp_mpp_get_encoder_size()`), 调用方不需要知道是哪一路 ——
 *       避免把"选路知识"复制到第二个地方。
 * @note 必须在 `bsp_mpp_init()` **之后**调用(要挂到已存在的 VENC 通道上)。
 * @note 重复调用返回 0, 不做任何事。
 */
int bsp_osd_init(void);

/**
 * 渲染一段文本并提交为一帧水印。
 *
 * @param[in] text 文本, **必须恰好 `BSP_OSD_TIME_CHARS` 个字符**
 *                  (区域尺寸在 `Create` 时就定死了, 长度不符会被拒绝并计数)
 * @return 0 成功; 负值失败
 *
 * @note 每秒调一次即可(`svc_osd` 的 1 Hz 线程)。
 * @note 文本长度不符时**明确失败并计入 `errors`**, 而不是硬画上去
 *       —— 否则会画出错位的水印, 而且没人知道。
 */
int bsp_osd_show(const char *text);

/**
 * 销毁 OSD:从通道摘下 → 销毁区域。
 * @note 未初始化时调用是安全的(no-op)。
 * @note 按**申请的反序**释放, 与 `bsp_mpp_deinit()` 同一原则。
 */
void bsp_osd_deinit(void);

/** OSD 是否已就绪。@return 1 = 已在叠加 */
int bsp_osd_started(void);

/** 取统计快照。 */
void bsp_osd_get_stats(bsp_osd_stats_t *out);

#endif /* __BSP_OSD_H__ */
