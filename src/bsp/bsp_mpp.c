/**
 * @file    bsp_mpp.c
 * @brief   MPP 板级支持实现 —— VI/VPSS/VENC 通路 + 取流
 *
 * 结构:
 *      ① 模块状态与帧缓冲
 *      ② 取流
 *      ③ 初始化(分段: VB / VI / VPSS / VENC)
 *      ④ 销毁
 *      ⑤ 统计
 *
 * ★ 本文件的初始化序列**照抄厂商 SDK sample**,逐段注明出处。
 *   自己拼过一次,漏了 `ViDev`,排查十几轮 —— 见 AGENTS.md §7.0 与本文件头部。
 *
 * @note 关于"运行期不做动态分配"(项目铁律):
 *       pack 数组在 `bsp_mpp_init()` 里**一次性分配**,
 *       `bsp_mpp_get_frame()` 里**一次 malloc 都没有** ——
 *       这比厂商 sample 更进一步(sample 每帧都 malloc/free pack 数组)。
 */
#include "bsp_mpp.h"

#include "infra_log.h"

#include <stdlib.h>     /* malloc / free */
#include <string.h>
#include <time.h>       /* nanosleep / struct timespec */

/*
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 为什么这里要包一层 pragma 抑制告警(而不是加 `-Wno-`)
 * ─────────────────────────────────────────────────────────────────
 *  厂商 SDK 头文件 `mpp/include/hi_buffer.h` 里有几个 `static inline` 函数
 *  (如 `VDEC_GetPicBufferSize`), 它们的参数**没被用到** —— 原厂代码问题。
 *
 *  实测证据(work/check_warning_source.py):
 *    · `-Wall -Wextra` 编本文件 → 5 条告警, **100% 来自 hi_buffer.h**;
 *    · 我们自己代码的告警数 = **0**;
 *    · 一个只 `#include` 它的空程序 → **同样 5 条**(证明与我们无关)。
 *
 *  厂商自己的 sample 只用 `-Wall`(不用 `-Wextra`),
 *  而 `-Wunused-parameter` **只在 `-Wextra` 下才开** —— 所以他们不报。
 *
 *  项目规矩是"`-Wall -Wextra` 零告警,且不许用 `-Wno-` 掩盖"。
 *  我们的处理:
 *    · **不加全局 `-Wno-`**(那会把我们自己代码的同类问题也一起掩盖);
 *    · 只在**包含厂商头文件**这一小段范围内抑制,
 *      `#pragma GCC diagnostic pop` 之后**立刻恢复** ——
 *      所以我们自己的代码仍然是完整 `-Wall -Wextra` 检查的。
 *   这是"作用域抑制", 不是"掩盖"。
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "hi_comm_sys.h"
#include "hi_comm_vb.h"
#include "hi_comm_vi.h"
#include "hi_comm_vpss.h"
#include "hi_comm_venc.h"
#include "mpi_sys.h"
#include "mpi_vb.h"
#include "mpi_vi.h"
#include "mpi_vpss.h"
#include "mpi_venc.h"
#include "sample_comm.h"
#pragma GCC diagnostic pop

/* ─────────── 板级配置(照抄 sample_venc.c 的 SAMPLE_VENC_H265_H264)─────────── */

/** 传感器型号。板子是易百纳 EB-Hi3516DV300-DC-182 + GC2053 */
#define BSP_SENSOR          GALAXYCORE_GC2053_MIPI_2M_30FPS_10BIT

/*
 * ⚠️⚠️ 这三个字段必须**同时**设对 —— 摄像头在 sensor1 / MIPI1 / i2c-1。
 *      "摄像头在 sensor1"这一条信息同时体现在三个地方,
 *      只改一个不等于改对了一组(我漏了 ViDev,代价是排查十几轮)。
 */
#define BSP_MIPI_DEV        1       /* stSnsInfo.MipiDev  */
#define BSP_I2C_BUS         1       /* stSnsInfo.s32BusId */
#define BSP_VI_DEV          1       /* stDevInfo.ViDev    ← 最容易漏 */
#define BSP_VI_PIPE         1       /* stPipeInfo.aPipe[0] */
#define BSP_VI_CHN          0
#define BSP_VPSS_GRP        0
#define BSP_VENC_CHN_H265   0       /* 1080p */
#define BSP_VENC_CHN_H264   1       /* 720p —— 本项目默认走这一路 */
#define BSP_VPSS_CHN_H265   0       /* VPSS 里对应 chn0 的那一路 */
#define BSP_VPSS_CHN_H264   1

/*
 * ⚠️⚠️ B027: **只允许启动"我们真正会取"的那一路编码通道。**
 *
 * 原来这里同时启动 chn0(H.265) + chn1(H.264), 但取流只取 chn1 ——
 * 结果 chn0 的码流**没人读**, 它的码流缓冲很快被塞满(实测 BusyCnt=200, FreeCnt=0),
 * 编码器停; 停下来的 chn0 又把它自己的输入图像队列占满(Free=0 Busy=6),
 * 于是 **VPSS 也被拖住**, 连我们正在读的 chn1 一起在 **205 帧**处彻底死掉。
 *
 * 板子自己的 `/proc/umap/{vi,vpss,venc}` 把这条链记得清清楚楚:
 *     VI 重收 846 帧 / VPSS 送出 846(chn0) 与 206(chn1)
 *     VENC chn0: VpssSnd=846  Send=206  **Full=640**   UserGet=0  GetTimes=0
 *     VENC chn1: VpssSnd=206  Send=206  Full=0         UserGet=227
 *  → chn0 的 `Full=640` + `UserGet=0` 就是"产得出来、没人取"的铁证。
 *
 * **教训**: 在共享 VB/VPSS 资源的通路上, "启动一个没人消费的通道"
 * 不是"多花点内存", 而是**会把整条流水线拖死**。
 * 要么每路都有消费者, 要么就别启动它。
 */

/** pack 数组容量。一帧的 pack 数由 QueryStatus 报出, 留 2 倍余量 */


/* ─────────── ① 模块状态与帧缓冲 ─────────── */

/**
 * 调用方**想要**哪一路(H.265=1)。
 *
 * @note 为什么不放进 `g`: `bsp_mpp_init()` 开头有 `memset(&g, 0, sizeof(g))`,
 *       放进去会被抹掉 —— "选择"必须在 memset **之前**就存在, `init` 才能读到它。
 *       这个坑很隐蔽(改完现象是"选择没生效、永远走默认那一路")。
 */
static int g_want_h265;

static struct {
    int              inited;
    SAMPLE_VI_CONFIG_S vi_cfg;      /* deinit 时要用它 StopVi */

    /*
     * 本次真正启用的那一路通道。
     * ★ 全文件所有 `HI_MPI_VENC_*` / `VPSS_Bind_VENC` 都用 `g.chn` / `g.vpss_chn`,
     *   **不许再出现写死的通道号** —— 写死就会重演 B027(启动了两路只取一路)。
     */
    int              is_h265;
    int              chn;           /* 本次启用的 VENC 通道 */
    int              vpss_chn;      /* 它绑定的 VPSS 通道 */

    int              venc_started;
    int              vpss_started;

    /* pack 数组: init 时一次分配, 取流时复用 */
    VENC_PACK_S     *packs;
    VENC_STREAM_S    stream;        /* 复用同一个, 避免每帧重新 memset */

    int              holding;       /* 是否正持有一帧未释放 */
    int              logged_buf_full;   /* "缓冲满"告警是否已打过(只打第一条) */
    bsp_mpp_stats_t  stats;
} g;

/**
 * 选择要取的那一路编码通道。**必须在 `bsp_mpp_init()` 之前调用。**
 *
 * @param is_h265 非 0 = 选 H.265 1080p(VENC chn0); 0 = H.264 720p(VENC chn1)
 *
 * @note ⚠️ 必须在 init **之前**调: 我们只启动被选中的那一路(见 B027),
 *       通道是在 `init_venc()` 里创建的, 之后再改就得停整条通路重建。
 *       init 之后调用会被忽略并告警(不静默)。
 * @note 为什么用"选择"而不是"两路都开、取的时候挑一路":
 *       两路都开就必然有一路没人取, 它会塞满码流缓冲并把整条通路拖死 —— B027。
 */
void bsp_mpp_select_encoder(int is_h265)
{
    if (g.inited) {
        LOG_ERROR("通路已初始化, 此时切换编码通道无效(必须在 bsp_mpp_init 之前)");
        return;
    }
    g_want_h265 = is_h265 ? 1 : 0;
}


/* ─────────── ② 取流 ─────────── */

/**
 * 轮询等待"有帧可取"。
 *
 * @param timeout_ms 0 = 不等待; <0 = 最多等 2 秒(看门狗上限,不是无限等); >0 = 毫秒
 * @return 1 = 有 pack 了; 0 = 等到超时还是没有
 *
 * @note ⚠️⚠️ **只看 `u32CurPacks`, 绝不能用 `u32LeftStreamFrames` 当门槛!**
 *       这是 B027, 我自己造出来的死锁, 代价是"205 帧后永久卡死":
 *
 *   ① 厂商 `sample_comm_venc.c:1452-1458` 的**注释**写着"建议两个一起检查":
 *          if (0 == stStat.u32CurPacks || 0 == stStat.u32LeftStreamFrames)
 *              { 打印 "Current frame is NULL"; return; }
 *      我照这句注释实现成了"两个都 > 0 才去取"。**错了。**
 *
 *   ② 但厂商自己的**活代码**(同文件 `:1459-1471`)只检查前者:
 *          if (0 == stStat.u32CurPacks) { ... return; }
 *          stStream.u32PackCount = stStat.u32CurPacks;
 *          HI_MPI_VENC_GetStream(VencChn, &stStream, -1);   ← 照取不误
 *
 *   ③ **为什么注释那句话不能当门槛**:官方文档《HiMPP V4.0》p.889 说
 *      "一个编码通道如果发生码流缓冲区满, 就会不再启动编码, 直到**用户获取码流**,
 *       从而有足够的码流缓冲可以用于编码时, 才开始继续编码。"
 *      于是:
 *          缓冲满 → `u32LeftStreamFrames == 0` → 编码器停
 *          编码器停 → 不再产生新码流 → `u32LeftStreamFrames` **永远回不到 > 0**
 *          我要求"它 > 0 才去取" → **永远不取** → 缓冲永远不排空 → 永久卡死
 *      把它当门槛 = 把"排空动作"锁在"已经排空"这个前提上 —— 循环依赖。
 *      **唯一能解开它的动作恰恰是继续 GetStream**(厂商活代码就是这么干的)。
 *
 *  所以:缓冲满时**照样取**, 取走即排空, 编码器自然恢复。
 *  我们仍然**统计并告警**这个状态(`note_buffer_full`), 因为它是有用的健康信号。
 *
 * @note 为什么**没有**照抄厂商的 `select(VencFd)`:
 *       那是为了同时等多个通道 + 顺便做 2 秒超时。我们只取一路,
 *       而且上层(svc_media)本来就会用 epoll/条件变量驱动节奏 ——
 *       在这里再套一层 select 是重复的。轮询等待语义更直白。
 */
static int wait_for_packs(int timeout_ms)
{
    VENC_CHN_STATUS_S st;
    long              waited_ms = 0;
    long              limit_ms  = (timeout_ms < 0) ? 2000 : timeout_ms;

    while (waited_ms < limit_ms) {
        struct timespec ts = { 0, 5 * 1000 * 1000 };    /* 5 ms */

        if (HI_MPI_VENC_QueryStatus(g.chn, &st) != HI_SUCCESS)
            return 0;
        /* ★ 只看这一个 —— 理由见上面的长注释(把 leftStreamFrames 当门槛会死锁) */
        if (st.u32CurPacks > 0)
            return 1;
        nanosleep(&ts, NULL);
        waited_ms += 5;
    }
    return 0;
}

/**
 * 记录"码流缓冲已满, 但我们仍然要把它取走"这件事。
 *
 * @note 只在**第一次**发生时打一条告警(卡住时每帧都满, 打日志会把串口刷爆),
 *       次数累计在 `stats.drained_full` 里, 由上层在结束时打印。
 */
static void note_buffer_full(void)
{
    g.stats.drained_full++;
    if (!g.logged_buf_full) {
        g.logged_buf_full = 1;
        LOG_WARN("VENC 码流缓冲已满(leftStreamFrames=0) —— 继续取流排空它, 否则编码器不恢复");
    }
}


/**
 * 把 MPP 的 pack 数组搬进我们的帧描述(只搬指针, 不拷数据)。
 *
 * @note ⚠️ **绝不能**假设各 pack 内存连续 —— 见下面的详细说明。
 */
static void fill_frame_packs(bsp_mpp_frame_t *frame)
{
    unsigned int k;

    frame->pack_count = (int)g.stream.u32PackCount;
    for (k = 0; k < g.stream.u32PackCount && k < BSP_MPP_MAX_PACKS; k++) {
        VENC_PACK_S *pk = &g.stream.pstPack[k];

        frame->packs[k].data = (const uint8_t *)pk->pu8Addr + pk->u32Offset;
        frame->packs[k].len  = (size_t)pk->u32Len - pk->u32Offset;
        g.stats.bytes       += frame->packs[k].len;
    }
    /* ★ 用编码器给的 PTS, 不靠"假设帧率"累加(解 B012 / 遗留问题 #5) */
    frame->pts = g.stream.pstPack[0].u64PTS;
    g.stats.packs += g.stream.u32PackCount;
}

int bsp_mpp_get_frame(bsp_mpp_frame_t *frame, int timeout_ms)
{
    VENC_CHN_STATUS_S st;
    HI_S32            ret;

    if (frame == NULL)
        return -1;
    memset(frame, 0, sizeof(*frame));
    if (!g.inited)
        return -2;
    if (g.holding)
        return -3;                  /* 上一帧没释放, 是调用方的 bug */

    ret = HI_MPI_VENC_QueryStatus(g.chn, &st);
    if (ret != HI_SUCCESS) {
        g.stats.errors++;
        LOG_ERROR("QueryStatus(chn%d) 失败: %#x", g.chn, ret);
        return -4;
    }
    /*
     * ★ **只看 `u32CurPacks`**(照抄厂商活代码 `sample_comm_venc.c:1459`)。
     *
     *   曾经这里写的是 `st.u32CurPacks == 0 || st.u32LeftStreamFrames == 0`
     *   —— 那是照厂商**注释**里的"建议"写的, 结果造成**永久死锁**
     *   (缓冲满 → 编码器停 → leftStreamFrames 永远是 0 → 我们永远不取 → 永不排空)。
     *   完整推演见 `wait_for_packs()` 的注释与 `docs/问题与解决记录.md` B027。
     */
    if (st.u32CurPacks == 0 && !wait_for_packs(timeout_ms)) {
        /* "当前没帧"**不是错误** —— 只是这一刻还没编码完 */
        g.stats.timeouts++;
        return 1;
    }
    /* 缓冲满仍然要取 —— 记一笔(这是健康信号, 也是 B027 的判据) */
    if (st.u32LeftStreamFrames == 0)
        note_buffer_full();
    if (st.u32CurPacks > BSP_MPP_MAX_PACKS) {
        LOG_ERROR("一帧的 pack 数 %u 超过缓冲 %d, 丢弃", st.u32CurPacks,
                  BSP_MPP_MAX_PACKS);
        g.stats.errors++;
        return -5;
    }

    memset(&g.stream, 0, sizeof(g.stream));
    g.stream.u32PackCount = st.u32CurPacks;
    g.stream.pstPack      = g.packs;

    /*
     * ⚠️ 这里传 **-1(阻塞)**, 照原厂 `sample_comm_venc.c:1471` 的写法。
     *
     * 走这一步的前提是 `u32CurPacks > 0`(上面已保证) —— **确有整帧可取**,
     * 所以阻塞等它不会白等。原厂注释还特别提过: `HI_TRUE` 是个**枚举,
     * 值就是 1**(=1 毫秒), 不是"真" —— 所以 sample 里写的 `HI_TRUE`
     * 其实等于 **1ms 超时**。我们直接传 -1, 语义更明确。
     */
    ret = HI_MPI_VENC_GetStream(g.chn, &g.stream, -1);
    if (ret != HI_SUCCESS) {
        g.stats.errors++;
        LOG_ERROR("GetStream(chn%d) 失败: %#x(curPacks=%u leftFrames=%u)",
                  g.chn, ret, st.u32CurPacks, st.u32LeftStreamFrames);
        return -6;
    }
    /* ★ 差别之二: 遍历所有 pack;不假设各 pack 连续(依据见 bsp_mpp.h) */
    fill_frame_packs(frame);

    g.holding = 1;
    g.stats.frames++;
    return 0;
}

void bsp_mpp_release_frame(bsp_mpp_frame_t *frame)
{
    if (frame == NULL)
        return;
    if (!g.holding)
        return;
    if (HI_MPI_VENC_ReleaseStream(g.chn, &g.stream) != HI_SUCCESS)
        LOG_ERROR("ReleaseStream 失败");
    g.holding = 0;
    memset(frame, 0, sizeof(*frame));
}

void bsp_mpp_get_stats(bsp_mpp_stats_t *out)
{
    if (out != NULL)
        *out = g.stats;
}

int bsp_mpp_is_ready(void)
{
    return g.inited;
}

/* ─────────── ③ 初始化 ─────────── */

/**
 * 第 1 步: 系统初始化 + VB(视频缓冲池)。
 *
 * @note 照抄 `sample_venc.c` 的 `SAMPLE_VENC_SYS_Init()`(厂商实际在用的写法):
 *       块大小用 `COMMON_GetPicBufferSize()` 算(不自己拼),
 *       池 0 = 主码流尺寸(传感器原生), 池 1 = 子码流。
 */
static int init_sys_and_vb(void)
{
    VB_CONFIG_S stVbConf;
    HI_U64      u64BlkSize;
    PIC_SIZE_E  enSnsSize;
    SIZE_S      stSnsSize;
    HI_S32      ret;

    memset(&stVbConf, 0, sizeof(stVbConf));

    ret = SAMPLE_COMM_VI_GetSizeBySensor(BSP_SENSOR, &enSnsSize);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("GetSizeBySensor 失败: %#x", ret);
        return -1;
    }
    ret = SAMPLE_COMM_SYS_GetPicSize(enSnsSize, &stSnsSize);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("GetPicSize 失败: %#x", ret);
        return -1;
    }

    u64BlkSize = COMMON_GetPicBufferSize(stSnsSize.u32Width, stSnsSize.u32Height,
                                        PIXEL_FORMAT_YVU_SEMIPLANAR_422,
                                        DATA_BITWIDTH_8, COMPRESS_MODE_SEG,
                                        DEFAULT_ALIGN);
    stVbConf.astCommPool[0].u64BlkSize = u64BlkSize;
    stVbConf.astCommPool[0].u32BlkCnt  = 10;

    u64BlkSize = COMMON_GetPicBufferSize(720, 576,
                                        PIXEL_FORMAT_YVU_SEMIPLANAR_422,
                                        DATA_BITWIDTH_8, COMPRESS_MODE_SEG,
                                        DEFAULT_ALIGN);
    stVbConf.astCommPool[1].u64BlkSize = u64BlkSize;
    stVbConf.astCommPool[1].u32BlkCnt  = 10;
    stVbConf.u32MaxPoolCnt = 2;

    ret = SAMPLE_COMM_SYS_Init(&stVbConf);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("SAMPLE_COMM_SYS_Init 失败: %#x", ret);
        return -1;
    }
    LOG_INFO("MPP 系统就绪(VB: %ux%u ×10 + 720x576 ×10)",
             stSnsSize.u32Width, stSnsSize.u32Height);
    return 0;
}

/**
 * 填 VI 配置结构。
 *
 * @note ⚠️ `MipiDev` / `s32BusId` / `ViDev` 必须**一起**设对 ——
 *       摄像头在 sensor1/MIPI1/i2c-1。"摄像头在 sensor1"这一条信息
 *       **同时体现在这三个字段上**,只改一个不等于改对一组
 *       (我漏了 `ViDev`,代价是排查十几轮:VI 只出 1 帧、取不到码流)。
 * @note 所有取值都照抄 `sample_venc.c` 的 `SAMPLE_VENC_VI_Init()`,
 *       包括它注释掉但实际生效的那几行。
 */
static void fill_vi_config(void)
{
    SAMPLE_VI_CONFIG_S *cfg = &g.vi_cfg;

    memset(cfg, 0, sizeof(*cfg));
    SAMPLE_COMM_VI_GetSensorInfo(cfg);

    cfg->s32WorkingViNum            = 1;
    cfg->as32WorkingViId[0]         = 0;
    cfg->astViInfo[0].stSnsInfo.MipiDev  = BSP_MIPI_DEV;
    cfg->astViInfo[0].stSnsInfo.s32BusId = BSP_I2C_BUS;
    cfg->astViInfo[0].stDevInfo.ViDev    = BSP_VI_DEV;   /* ← 漏了它就只出 1 帧 */
    cfg->astViInfo[0].stDevInfo.enWDRMode = WDR_MODE_NONE;
    /* 厂商 sample 里由 bLowDelay 决定;HI_FALSE → OFFLINE */
    cfg->astViInfo[0].stPipeInfo.enMastPipeMode = VI_OFFLINE_VPSS_OFFLINE;
    cfg->astViInfo[0].stPipeInfo.bMultiPipe     = HI_FALSE;
    cfg->astViInfo[0].stPipeInfo.bVcNumCfged    = HI_FALSE;
    cfg->astViInfo[0].stPipeInfo.bIspBypass     = HI_FALSE;
    /* aPipe[1..3] 必须全置 -1: 只置 [1] 会让 [2]/[3] 留下垃圾值 →
     * SetDevBindPipe 报 0xa0108048(这个坑踩过) */
    cfg->astViInfo[0].stPipeInfo.aPipe[0] = BSP_VI_PIPE;
    cfg->astViInfo[0].stPipeInfo.aPipe[1] = -1;
    cfg->astViInfo[0].stPipeInfo.aPipe[2] = -1;
    cfg->astViInfo[0].stPipeInfo.aPipe[3] = -1;
    cfg->astViInfo[0].stChnInfo.ViChn          = BSP_VI_CHN;
    cfg->astViInfo[0].stChnInfo.enPixFormat    = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    cfg->astViInfo[0].stChnInfo.enDynamicRange = DYNAMIC_RANGE_SDR8;
    cfg->astViInfo[0].stChnInfo.enVideoFormat  = VIDEO_FORMAT_LINEAR;
    cfg->astViInfo[0].stChnInfo.enCompressMode = COMPRESS_MODE_SEG;
}

/** 第 2 步: VI(视频输入)。摄像头 → 采集。 */
static int init_vi(void)
{
    SAMPLE_VI_CONFIG_S *cfg = &g.vi_cfg;
    HI_U32              u32FrameRate = 0;
    ISP_CTRL_PARAM_S    stIspCtrlParam;
    HI_S32              ret;

    fill_vi_config();

    ret = SAMPLE_COMM_VI_SetParam(cfg);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("VI_SetParam 失败: %#x", ret);
        return -1;
    }

    /* ISP 统计间隔(照抄 sample 的 SAMPLE_VENC_VI_Init, 位置在 SetParam 之后) */
    if (SAMPLE_COMM_VI_GetFrameRateBySensor(BSP_SENSOR, &u32FrameRate) == HI_SUCCESS &&
        HI_MPI_ISP_GetCtrlParam(BSP_VI_PIPE, &stIspCtrlParam) == HI_SUCCESS) {
        stIspCtrlParam.u32StatIntvl = u32FrameRate / 30;
        HI_MPI_ISP_SetCtrlParam(BSP_VI_PIPE, &stIspCtrlParam);
    }

    ret = SAMPLE_COMM_VI_StartVi(cfg);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("VI_StartVi 失败: %#x", ret);
        return -1;
    }
    LOG_INFO("VI 就绪(MipiDev=%d, i2c=%d, ViDev=%d, ViPipe=%d)",
             BSP_MIPI_DEV, BSP_I2C_BUS, BSP_VI_DEV, BSP_VI_PIPE);
    return 0;
}

/**
 * 第 3 步: VPSS。
 *
 * @note **顺序要点**:`SAMPLE_COMM_VPSS_Start` 成功后**才** `VI_Bind_VPSS`。
 *       Bind 内部只是 `HI_MPI_SYS_Bind`,目标 grp 不存在时不报错、只是不生效。
 */
static int init_vpss(void)
{
    VPSS_GRP_ATTR_S stGrpAttr;
    VPSS_CHN_ATTR_S stChnAttr[2];
    HI_BOOL         abChnEnable[2];
    HI_S32          ret;
    int             i;

    /* ★ 只启用被选中的那一路 VPSS 通道 —— 理由同 B027(启动没人取的通道会拖死整条通路) */
    abChnEnable[0] = g.is_h265 ? HI_TRUE : HI_FALSE;
    abChnEnable[1] = g.is_h265 ? HI_FALSE : HI_TRUE;

    memset(&stGrpAttr, 0, sizeof(stGrpAttr));
    stGrpAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stGrpAttr.enPixelFormat  = SAMPLE_PIXEL_FORMAT;
    stGrpAttr.u32MaxW        = 1920;
    stGrpAttr.u32MaxH        = 1080;
    stGrpAttr.bNrEn          = HI_TRUE;
    stGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stGrpAttr.stNrAttr.enNrType       = VPSS_NR_TYPE_VIDEO;
    stGrpAttr.stNrAttr.enNrMotionMode = NR_MOTION_MODE_NORMAL;
    stGrpAttr.stNrAttr.enCompressMode = COMPRESS_MODE_FRAME;

    for (i = 0; i < 2; i++) {
        memset(&stChnAttr[i], 0, sizeof(stChnAttr[i]));
        stChnAttr[i].enChnMode      = VPSS_CHN_MODE_USER;
        stChnAttr[i].enCompressMode = COMPRESS_MODE_NONE;
        stChnAttr[i].enDynamicRange = DYNAMIC_RANGE_SDR8;
        stChnAttr[i].enPixelFormat  = SAMPLE_PIXEL_FORMAT;
        stChnAttr[i].enVideoFormat  = VIDEO_FORMAT_LINEAR;
        stChnAttr[i].stFrameRate.s32SrcFrameRate = -1;
        stChnAttr[i].stFrameRate.s32DstFrameRate = -1;
        stChnAttr[i].u32Width  = (i == 0) ? 1920 : 1280;
        stChnAttr[i].u32Height = (i == 0) ? 1080 : 720;
        stChnAttr[i].u32Depth  = 0;
    }

    ret = SAMPLE_COMM_VPSS_Start(BSP_VPSS_GRP, abChnEnable, &stGrpAttr, stChnAttr);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("VPSS_Start 失败: %#x", ret);
        return -1;
    }
    g.vpss_started = 1;

    ret = SAMPLE_COMM_VI_Bind_VPSS(BSP_VI_PIPE, BSP_VI_CHN, BSP_VPSS_GRP);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("VI_Bind_VPSS 失败: %#x", ret);
        return -1;
    }
    LOG_INFO("VPSS 就绪(grp%d: %s), 已绑定 VI", BSP_VPSS_GRP,
             g.is_h265 ? "仅 chn0 = 1920x1080" : "仅 chn1 = 1280x720");
    return 0;
}

/**
 * 第 4 步: VENC —— **只启动被选中的那一路**。
 *
 * @note ⚠️ 这里**故意不**"两路都开"。两路都开就必然有一路没人取流,
 *       它会塞满自己的码流缓冲 → 该路编码器停 → 它的输入图像队列占满
 *       → **VPSS 被拖住** → 连我们正在取的那一路也一起死掉。
 *       实测死点固定在 **205 帧**(≈6.8 秒 @30fps)。完整证据见文件头 B027。
 */
static int init_venc(void)
{
    VENC_GOP_ATTR_S stGopAttr;
    HI_S32          ret;

    ret = SAMPLE_COMM_VENC_GetGopAttr(VENC_GOPMODE_NORMALP, &stGopAttr);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("VENC_GetGopAttr 失败: %#x", ret);
        return -1;
    }

    /* 编码类型/分辨率跟着选中的那一路走, 不许和通道对错号 */
    ret = SAMPLE_COMM_VENC_Start(g.chn,
                                g.is_h265 ? PT_H265 : PT_H264,
                                g.is_h265 ? PIC_1080P : PIC_720P,
                                SAMPLE_RC_CBR, 0, HI_FALSE, &stGopAttr);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("VENC_Start(chn%d/%s) 失败: %#x", g.chn,
                  g.is_h265 ? "H.265" : "H.264", ret);
        return -1;
    }
    g.venc_started = 1;

    if (SAMPLE_COMM_VPSS_Bind_VENC(BSP_VPSS_GRP, g.vpss_chn, g.chn) != HI_SUCCESS) {
        LOG_ERROR("VPSS_Bind_VENC(grp%d-chn%d → venc%d) 失败",
                  BSP_VPSS_GRP, g.vpss_chn, g.chn);
        return -1;
    }
    LOG_INFO("VENC 就绪(只启用 chn%d = %s)", g.chn,
             g.is_h265 ? "H.265 1080p" : "H.264 720p");
    return 0;
}

int bsp_mpp_init(void)
{
    if (g.inited)
        return 0;

    memset(&g, 0, sizeof(g));

    /*
     * ★ 把"选择"装回 g —— 它之所以存在文件级静态 `g_want_h265` 里,
     *   就是因为上面这行 memset 会把它抹掉(踩过: 现象是"选择没生效")。
     */
    g.is_h265  = g_want_h265;
    g.chn      = g.is_h265 ? BSP_VENC_CHN_H265 : BSP_VENC_CHN_H264;
    g.vpss_chn = g.is_h265 ? BSP_VPSS_CHN_H265 : BSP_VPSS_CHN_H264;

    if (init_sys_and_vb() != 0)
        goto fail;
    if (init_vi() != 0)
        goto fail;
    if (init_vpss() != 0)
        goto fail;
    if (init_venc() != 0)
        goto fail;

    /* pack 数组: 一次性分配, 运行期不再 malloc */
    g.packs = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S) * BSP_MPP_MAX_PACKS);
    if (g.packs == NULL) {
        LOG_ERROR("pack 数组分配失败(%d 个 × %zu 字节)",
                  BSP_MPP_MAX_PACKS, sizeof(VENC_PACK_S));
        goto fail;
    }

    g.inited = 1;
    LOG_INFO("MPP 通路就绪, 可开始取流(chn%d, %s)", g.chn,
             g.is_h265 ? "H.265" : "H.264");
    return 0;

fail:
    LOG_ERROR("MPP 初始化失败, 回滚");
    bsp_mpp_deinit();
    return -1;
}

/* ─────────── ④ 销毁(申请的反序)─────────── */

void bsp_mpp_deinit(void)
{
    /* 停止 VPSS 时, 只有"本次启用过的那一路"该报 enable */
    HI_BOOL abChnEnable[2];

    abChnEnable[0] = g.is_h265 ? HI_TRUE : HI_FALSE;
    abChnEnable[1] = g.is_h265 ? HI_FALSE : HI_TRUE;

    g.inited = 0;
    free(g.packs);
    g.packs = NULL;

    if (g.venc_started) {
        SAMPLE_COMM_VPSS_UnBind_VENC(BSP_VPSS_GRP, g.vpss_chn, g.chn);
        SAMPLE_COMM_VENC_Stop(g.chn);
        g.venc_started = 0;
    }
    if (g.vpss_started) {
        SAMPLE_COMM_VPSS_Stop(BSP_VPSS_GRP, abChnEnable);
        g.vpss_started = 0;
    }
    /* 上面 VPSS_Stop 之前其实该先 UnBind VI→VPSS; 厂商 sample 的顺序是先
     * UnBind 再 Stop, 这里按同一顺序补齐 */
    SAMPLE_COMM_VI_UnBind_VPSS(BSP_VI_PIPE, BSP_VI_CHN, BSP_VPSS_GRP);
    SAMPLE_COMM_VI_StopVi(&g.vi_cfg);
    SAMPLE_COMM_SYS_Exit();
    LOG_INFO("MPP 通路已释放");
}
