/**
 * @file bsp_osd.c
 * @brief OSD 时间水印的板级实现(海思 REGION / OVERLAY → VENC 通道)
 *
 * 详细的分层位置、复用出处与颜色约定见 `bsp_osd.h`。
 * 这里只强调一件事:**本文件是全项目唯一碰 `HI_MPI_RGN_*` 的地方**。
 */
#include "bsp_osd.h"

#include <string.h>

#include "hi_common.h"
#include "hi_comm_sys.h"
#include "hi_comm_region.h"
#include "mpi_region.h"

#include "bsp_mpp.h"
#include "infra_log.h"

/** 区域句柄。依据厂商 `sample_comm_region.c`:`#define OverlayMinHandle 0` */
#define BSP_OSD_RGN_HANDLE 0

/** 水印像素缓冲(ARGB1555, 每像素 2 字节)。
 *  304 × 32 × 2 = 19,456 字节 —— **必须放 .bss, 不能放栈**
 *  (本项目规矩:栈数组不超过 4 KB)。 */
static uint16_t g_pixels[BSP_OSD_REGION_W * BSP_OSD_REGION_H];

static struct {
    int      created;     /**< `RGN_Create` 成功过(决定要不要 Destroy) */
    int      attached;    /**< 已挂到 VENC 通道(决定要不要 Detach) */
    int      venc_chn;    /**< 挂在哪一路 */
    int      pos_x;       /**< 区域左上角 x(图像坐标) */
    int      pos_y;       /**< 区域左上角 y */
    uint64_t shows;       /**< 成功提交次数 */
    uint64_t errors;      /**< 失败次数 */
} g;

/**
 * @brief 建 OVERLAY 区域
 *
 * @return 0 成功; -1 失败
 * @note 属性照抄 `SAMPLE_REGION_CreateOverLay()`(只改尺寸与背景色)。
 * @note `u32BgColor = 0` 让区域底色透明; 文字像素的 alpha 位仍是 1。
 */
static int osd_create_region(void)
{
    RGN_ATTR_S attr;
    HI_S32     ret;

    memset(&attr, 0, sizeof(attr));
    attr.enType = OVERLAY_RGN;
    attr.unAttr.stOverlay.enPixelFmt       = PIXEL_FORMAT_ARGB_1555;
    attr.unAttr.stOverlay.stSize.u32Width  = BSP_OSD_REGION_W;
    attr.unAttr.stOverlay.stSize.u32Height = BSP_OSD_REGION_H;
    attr.unAttr.stOverlay.u32BgColor       = 0x00000000;   /* 透明 */
    attr.unAttr.stOverlay.u32CanvasNum     = 2;            /* 照抄 sample 的双缓冲 */

    ret = HI_MPI_RGN_Create((RGN_HANDLE)BSP_OSD_RGN_HANDLE, &attr);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("RGN_Create(handle=%d, %dx%d) 失败: %#x",
                  BSP_OSD_RGN_HANDLE, BSP_OSD_REGION_W, BSP_OSD_REGION_H, ret);
        return -1;
    }
    g.created = 1;
    return 0;
}

/**
 * @brief 把区域挂到 VENC 通道并摆好位置
 *
 * @param[in] venc_chn VENC 通道号
 * @param[in] x,y      区域左上角(图像坐标, 必须偶数对齐)
 * @return 0 成功; -1 失败
 *
 * @note 属性照抄 `SAMPLE_COMM_REGION_AttachToChn()` 的 `OVERLAY_RGN` 分支 ——
 *       包括 QP / 反色 / LUT 那几个字段(它们只影响 JPEG 抓拍)。
 *       **照抄能跑的配置, 比自己发明安全**(AGENTS.md §7.0)。
 * @note ★ 两处**故意与 sample 不同**:
 *       `u32BgAlpha = 0`(sample 是 128)—— 我们要背景全透明;
 *       `stPoint` 由参数决定 —— sample 里是写死的 20/200 网格位置。
 */
static int osd_attach(int venc_chn, int x, int y)
{
    RGN_CHN_ATTR_S attr;
    MPP_CHN_S      chn;
    HI_S32         ret;

    memset(&attr, 0, sizeof(attr));
    attr.bShow  = HI_TRUE;
    attr.enType = OVERLAY_RGN;

    attr.unChnAttr.stOverlayChn.stPoint.s32X = x;
    attr.unChnAttr.stOverlayChn.stPoint.s32Y = y;
    attr.unChnAttr.stOverlayChn.u32FgAlpha   = 128;   /* 文字不透明(sample 同值) */
    attr.unChnAttr.stOverlayChn.u32BgAlpha   = 0;     /* ★ 背景全透明(见上) */
    attr.unChnAttr.stOverlayChn.u32Layer     = 0;

    attr.unChnAttr.stOverlayChn.stQpInfo.bQpDisable = HI_FALSE;
    attr.unChnAttr.stOverlayChn.stQpInfo.bAbsQp     = HI_TRUE;
    attr.unChnAttr.stOverlayChn.stQpInfo.s32Qp      = 30;

    attr.unChnAttr.stOverlayChn.stInvertColor.stInvColArea.u32Width  = 16;
    attr.unChnAttr.stOverlayChn.stInvertColor.stInvColArea.u32Height = 16;
    attr.unChnAttr.stOverlayChn.stInvertColor.u32LumThresh = 128;
    attr.unChnAttr.stOverlayChn.stInvertColor.enChgMod     = LESSTHAN_LUM_THRESH;
    attr.unChnAttr.stOverlayChn.stInvertColor.bInvColEn    = HI_FALSE;

    attr.unChnAttr.stOverlayChn.enAttachDest   = ATTACH_JPEG_MAIN;
    attr.unChnAttr.stOverlayChn.u16ColorLUT[0] = 0x2abc;
    attr.unChnAttr.stOverlayChn.u16ColorLUT[1] = 0x7FF0;

    chn.enModId  = HI_ID_VENC;
    chn.s32DevId = 0;
    chn.s32ChnId = venc_chn;

    ret = HI_MPI_RGN_AttachToChn((RGN_HANDLE)BSP_OSD_RGN_HANDLE, &chn, &attr);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("RGN_AttachToChn(handle=%d, chn%d) 失败: %#x",
                  BSP_OSD_RGN_HANDLE, venc_chn, ret);
        return -1;
    }
    g.attached = 1;
    return 0;
}

/**
 * @brief 把渲染好的像素提交给区域
 *
 * @param[in] px 紧密排列的 ARGB1555 像素(每行 `BSP_OSD_REGION_W` 个, 无行间填充)
 * @return 0 成功; -1 失败
 * @note `BITMAP_S` **没有 stride 字段** —— 所以缓冲必须是紧密排列的,
 *       `osd_render_text()` 输出的正好是。
 */
static int osd_push_bitmap(uint16_t *px)
{
    BITMAP_S bmp;
    HI_S32   ret;

    memset(&bmp, 0, sizeof(bmp));
    bmp.enPixelFormat = PIXEL_FORMAT_ARGB_1555;
    bmp.u32Width      = BSP_OSD_REGION_W;
    bmp.u32Height     = BSP_OSD_REGION_H;
    bmp.pData         = (HI_VOID *)px;

    ret = HI_MPI_RGN_SetBitMap((RGN_HANDLE)BSP_OSD_RGN_HANDLE, &bmp);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("RGN_SetBitMap 失败: %#x", ret);
        return -1;
    }
    return 0;
}

int bsp_osd_init(void)
{
    int img_w = 0;
    int img_h = 0;

    if (g.attached) {
        return 0;
    }
    bsp_mpp_get_encoder_size(&img_w, &img_h);
    if (img_w < BSP_OSD_REGION_W + BSP_OSD_MARGIN) {
        LOG_ERROR("画面太窄(%dx%d), 放不下 %d 宽的水印",
                  img_w, img_h, BSP_OSD_REGION_W);
        return -1;
    }
    g.venc_chn = bsp_mpp_get_venc_chn();
    g.pos_x    = img_w - BSP_OSD_REGION_W - BSP_OSD_MARGIN;   /* 右上角 */
    g.pos_y    = BSP_OSD_MARGIN;

    if (osd_create_region() != 0 || osd_attach(g.venc_chn, g.pos_x, g.pos_y) != 0) {
        bsp_osd_deinit();          /* 半成品要收干净, 免得下次 Create 撞句柄 */
        return -1;
    }
    LOG_INFO("OSD 就绪: VENC chn%d, %dx%d @(%d,%d), 画面 %dx%d, 放大 %d 倍",
             g.venc_chn, BSP_OSD_REGION_W, BSP_OSD_REGION_H,
             g.pos_x, g.pos_y, img_w, img_h, BSP_OSD_SCALE);
    return 0;
}

/**
 * @brief 把文本渲染进 `g_pixels`, 并校验尺寸与区域一致
 *
 * @param[in] text 文本
 * @return 0 成功; -1 失败
 *
 * @note 拆出来有两个好处: `bsp_osd_show()` 短到一眼能看完;
 *       顺便把一处"深续行"(对齐到函数参数)的缩进消掉 —— 项目硬约束缩进 ≤ 5 层。
 * @note 区域尺寸在 `Create` 时就定死了、改不了 —— 文本长度不符**必须明确拒绝**,
 *       否则会画出一个错位的水印, 而且没人知道。
 */
static int osd_render_into_buffer(const char *text)
{
    osd_render_size_t sz;
    size_t            pixels = (size_t)BSP_OSD_REGION_W * BSP_OSD_REGION_H;

    if (osd_render_text(text, BSP_OSD_SCALE, g_pixels, pixels, &sz) != 0) {
        LOG_ERROR("水印渲染失败");
        return -1;
    }
    if (sz.width != BSP_OSD_REGION_W || sz.height != BSP_OSD_REGION_H) {
        LOG_ERROR("水印文本 %d 字符, 应为 %d 字符", sz.chars, BSP_OSD_TIME_CHARS);
        return -1;
    }
    return 0;
}

int bsp_osd_show(const char *text)
{
    if (!g.attached || text == NULL) {
        return -1;
    }
    if (osd_render_into_buffer(text) != 0 || osd_push_bitmap(g_pixels) != 0) {
        g.errors++;
        return -1;
    }
    g.shows++;
    return 0;
}

void bsp_osd_deinit(void)
{
    MPP_CHN_S chn;
    HI_S32    ret;

    if (g.attached) {
        chn.enModId  = HI_ID_VENC;
        chn.s32DevId = 0;
        chn.s32ChnId = g.venc_chn;
        ret = HI_MPI_RGN_DetachFromChn((RGN_HANDLE)BSP_OSD_RGN_HANDLE, &chn);
        if (ret != HI_SUCCESS) {
            LOG_WARN("RGN_DetachFromChn 失败(继续销毁): %#x", ret);
        }
        g.attached = 0;
    }
    if (g.created) {
        ret = HI_MPI_RGN_Destroy((RGN_HANDLE)BSP_OSD_RGN_HANDLE);
        if (ret != HI_SUCCESS) {
            LOG_WARN("RGN_Destroy 失败: %#x", ret);
        }
        g.created = 0;
    }
}

int bsp_osd_started(void)
{
    return g.attached ? 1 : 0;
}

void bsp_osd_get_stats(bsp_osd_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->started   = g.attached ? 1 : 0;
    out->venc_chn  = g.venc_chn;
    out->pos_x     = g.pos_x;
    out->pos_y     = g.pos_y;
    out->width     = BSP_OSD_REGION_W;
    out->height    = BSP_OSD_REGION_H;
    out->shows     = g.shows;
    out->errors    = g.errors;
}
