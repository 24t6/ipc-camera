/**
 * @file    bsp_mpp.h
 * @brief   MPP 板级支持 —— VI/VPSS/VENC 通路的初始化、取流、销毁
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它在分层里的位置
 * ─────────────────────────────────────────────────────────────────
 *      bsp_mpp    ← **本文件**:海思 MPP 的适配层(碰硬件、碰 SDK)
 *      svc_media  ← 取流线程 + NALU 解析 + 入队(不碰 MPP 细节)
 *      svc_net    ← RTSP 会话与网络
 *      protocol   ← 纯逻辑,不碰硬件
 *
 *  规矩(AGENTS.md §7):`bsp_` 前缀 = 板级支持包,允许碰硬件与厂商 SDK;
 *  上层通过本文件的接口使用,不直接调 `HI_MPI_*`。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⭐ 复用说明(AGENTS.md §7.0「优先复用」)
 * ─────────────────────────────────────────────────────────────────
 *  本模块的初始化序列**照抄厂商 SDK 的 sample**, 而不是自己拼:
 *      · `sample/venc/sample_venc.c` 的 `SAMPLE_VENC_H265_H264()`  —— 整体编排
 *      · `SAMPLE_VENC_VI_Init()` / `SAMPLE_VENC_VPSS_Init()`        —— 各段配置
 *      · `sample/common/sample_comm_*.c`                            —— 公共层(直接复用)
 *
 *  **为什么必须照抄**:我们一开始自己拼,漏了 `ViDev`(填 0,应为 1),
 *  结果 VI 只出 1 帧、取不到任何码流,排查了十几轮。
 *  详见 `docs/问题与解决记录.md` B023 附近与 `work/m1-9_上板记录.md`。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 三个"必须知道"的板级事实(都实测过)
 * ─────────────────────────────────────────────────────────────────
 *  ① **摄像头在 sensor1 / MIPI1 / i2c-1** —— 所以**三个字段都要设**:
 *         `stSnsInfo.MipiDev = 1`
 *         `stSnsInfo.s32BusId = 1`
 *         `stDevInfo.ViDev   = 1`      ← 最容易漏的就是这个
 *     厂商依据:`sample_venc.c` 里 `VI_DEV ViDev = 1; VI_PIPE ViPipe = 1;`
 *     ⚠️ "摄像头在 sensor1"这一条信息**同时体现在三个字段上**,
 *        只改一个不等于改对了一组。
 *
 *  ② **`enMastPipeMode` 用 `VI_OFFLINE_VPSS_OFFLINE`**
 *     厂商 sample 里它由 `bLowDelay` 决定(HI_FALSE → OFFLINE)。
 *
 *  ③ **`SAMPLE_COMM_VI_Bind_VPSS` 放在 `SAMPLE_COMM_VPSS_Start` 之后**
 *     它内部只是 `HI_MPI_SYS_Bind`,目标 grp 不存在时**不报错、只是不生效**
 *     (静默失败)。按厂商顺序写更稳。
 *
 * @note 本模块**不是线程安全的**:`bsp_mpp_*` 系列只允许从**一个**线程调用
 *       (按架构, 由 `svc_media` 的取流线程独占)。
 */
#ifndef __BSP_MPP_H__
#define __BSP_MPP_H__

#include <stddef.h>
#include <stdint.h>

/** 取流缓冲的建议大小(单帧最大字节数)。架构预算里给的 256 KB */
#define BSP_MPP_FRAME_MAX_BYTES (256 * 1024)

/**
 * 一帧码流里的一段(pack)。
 *
 * @note ★ 为什么是"一段一段"而不是"一整块":
 *       查官方文档《HiMPP V4.0 媒体处理软件开发参考》6.2.13 与 6.2.8 得知:
 *         · **单包模式**(`u32OneStreamBuffer=1`):`u32PackCount` 为 1,
 *           一整帧码流在**一段连续内存**里,`u32Offset` 指出有效数据起点;
 *         · **多包模式**(默认):一帧由**多个 pack** 组成,
 *           **每个 pack 各有自己的 `pu8Addr` —— 各包数据不保证连续!**
 *             (官方原话:"NAL 包是独立的")
 *       我第一版按"连续"处理(`pstPack[0].pu8Addr` + 累加长度),
 *       **在多包模式下会读到 pack 之间的空隙, 是错的。**
 *       所以接口改成逐 pack 暴露, 由上层自己决定怎么处理。
 *
 * @note H.264 多包模式下, 一个 I 帧**至少 4 个 pack**(sps / pps / sei / Islice),
 *       见官方文档 6.2.13。实测我们这路流一帧 8 个 pack。
 */
typedef struct {
    const uint8_t *data;    /**< 本 pack 的起始地址(= pu8Addr + u32Offset) */
    size_t         len;     /**< 本 pack 的字节数(= u32Len - u32Offset) */
} bsp_mpp_pack_t;

/** 一帧最多几个 pack。官方说 I 帧至少 4 个, 留足余量 */
#define BSP_MPP_MAX_PACKS 64

/**
 * 一帧码流(取出后的形态)。
 *
 * @note `packs[].data` 指向**MPP 内部缓冲**, 只在 `bsp_mpp_release_frame()`
 *       之前有效。之所以不拷贝出来: 拷一份几百 KB 纯属浪费 ——
 *       调用方(取流线程)应该在持有期间**立刻解析并拷进队列**。
 */
typedef struct {
    bsp_mpp_pack_t packs[BSP_MPP_MAX_PACKS];    /**< 各段(按 pack 顺序) */
    int            pack_count;                  /**< 实际 pack 数 */
    uint64_t       pts;                         /**< 编码器时间戳。★ 时基**实测 = 1 MHz**
                                                 *   (相邻两帧差 33333 = 1e6/30),
                                                 *   不是 90 kHz —— RTP 换算见 `proto_rtp.c` */
} bsp_mpp_frame_t;

/**
 * 选择要取的那一路编码通道。**必须在 `bsp_mpp_init()` 之前调用。**
 *
 * @param is_h265 非 0 = H.265 1080p(VENC chn0); 0 = H.264 720p(VENC chn1)
 *
 * @note ⚠️ 为什么是"选一路"而不是"两路都开、取的时候挑一路" —— 这是 **B027**:
 *   曾经两路都启动、只取 chn1, 结果**没人取的 chn0** 把码流缓冲塞满
 *   (`BusyCnt=200 / FreeCnt=0`), 它自己停编码, 输入图像队列随之占满,
 *   于是 **VPSS 被拖住**, 连正在取的 chn1 也一起在 **205 帧**处死掉。
 *   板子 `/proc/umap/venc` 里 chn0 的 `Full=640` 与 `UserGet=0` 是直接铁证。
 *   **结论: 通路上不许出现没有消费者的通道。**
 */
void bsp_mpp_select_encoder(int is_h265);

/**
 * 当前选中的 VENC 通道号(给需要"通道号"的模块用, 例如 OSD 挂载)。
 *
 * @return VENC 通道号
 * @note ★ 为什么不直接读内部结构里的 `chn`: `bsp_mpp_init()` 开头有
 *       `memset(&g, 0, sizeof(g))`, 会把结构里的一切清零 ——
 *       所以"选路意向"存在文件级静态 `g_want_h265`, **它才是唯一真相**。
 *       本接口在 `init` 前后都返回正确的通道号。
 */
int bsp_mpp_get_venc_chn(void);

/**
 * 当前选中通道的**编码尺寸**(像素)。
 *
 * @param[out] width  输出宽度, 可为 NULL
 * @param[out] height 输出高度, 可为 NULL
 * @note OSD 的区域坐标是**图像坐标**, 摆放位置(如右上角)要用到它。
 *       尺寸与选路一一对应: chn0 = 1920×1080, chn1 = 1280×720。
 */
void bsp_mpp_get_encoder_size(int *width, int *height);

/**
 * 初始化 MPP 视频通路(系统/VB → VI → VPSS → VENC)。
 *
 * @return 0 成功; 负值失败
 *
 * @note **阻塞**,约 5~10 秒(要等 ISP 起来)。**不要在网络线程里调**。
 * @note 重复调用(已初始化)返回 0,不做任何事。
 * @note 只启动 `bsp_mpp_select_encoder()` 选中的那一路(默认 H.264 720p)。
 */
int bsp_mpp_init(void);

/**
 * 销毁通路。按**申请的反序**释放(与 `svc_net` 的 `release_all` 同一原则)。
 * @note 未初始化时调用是安全的(no-op)。
 */
void bsp_mpp_deinit(void);

/** 通路是否已就绪。@return 1 = 可以取流 */
int bsp_mpp_is_ready(void);

/**
 * 取一帧。
 *
 * @param[out] frame 输出: 帧描述(指向 MPP 内部缓冲)
 * @param timeout_ms 0 = 不等待立即返回; <0 = 阻塞等到有帧; >0 = 最多等这么久
 * @return 0 = 取到一帧(调用方**必须**随后调 `bsp_mpp_release_frame`);
 *         1 = 超时,当前无帧;
 *         负值 = 出错
 *
 * @note ★ 与厂商 sample 的关键差别: sample 传的是 `HI_TRUE`(= 枚举值 1,
 *       即 **1 毫秒超时**),我们按需要传 0 / -1 / 正数。
 *       —— `HI_TRUE` 是个**枚举**,值就是 1,不是"真"。这是个很容易读错的坑。
 */
int bsp_mpp_get_frame(bsp_mpp_frame_t *frame, int timeout_ms);

/**
 * 释放一帧。必须与上一次成功的 `bsp_mpp_get_frame` 配对调用。
 * 传 NULL 是安全的(no-op)。
 */
void bsp_mpp_release_frame(bsp_mpp_frame_t *frame);

/** 取流统计(用于日志与验收断言) */
typedef struct {
    uint64_t frames;        /**< 累计取到的帧数 */
    uint64_t packs;         /**< 累计 pack 数(一帧可含多个) */
    uint64_t bytes;         /**< 累计字节数 */
    uint64_t timeouts;      /**< 超时次数(不等于错误,只是"当时没帧") */
    uint64_t errors;        /**< 真错误次数 */
    /**
     * 取流时"码流缓冲已满"(`u32LeftStreamFrames == 0`)却仍然取走的次数。
     *
     * @note ★ 这个计数是 B027 的**判据**:它 > 0 就证明"缓冲满 → 必须继续取流排空"
     *       这条路径被走到过。若它一直是 0, 说明压根没碰过缓冲满,
     *       那次"205 帧卡死"就是别的原因。
     */
    uint64_t drained_full;
} bsp_mpp_stats_t;

/** 取统计快照。 */
void bsp_mpp_get_stats(bsp_mpp_stats_t *out);

#endif /* __BSP_MPP_H__ */
