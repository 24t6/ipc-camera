/**
 * @file    bsp_mpp.c
 * @brief   MPP 板级支持实现 —— VI/VPSS/VENC 通路 + 取流
 *
 * 【模块职责】MPP 通路(VB → VI → VPSS → VENC)的初始化、取流、销毁
 * 【依赖方向】依赖厂商 MPP(libmpi 与 sample common)与 infra_log; 不依赖 service / protocol
 * 【线程模型】**非线程安全**: 只允许 svc_media 的取流线程调用(MPP 通路由它独占)
 * 【资源边界】VENC 码流缓冲由 MPP 持有(取到后必须立刻 ReleaseStream); 无自有堆分配
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
 * MJPEG 用的两路通道号(2026-09-20)
 *   · `BSP_VENC_CHN_MJPEG` = 第三路编码器(0/1 已被 H.265/H.264 占)
 *   · `BSP_VPSS_CHN_MJPEG` = 专门给小图 scaler 的那一路 VPSS(优先方案)
 * 实测(A22): 这两路可以按需建/拆, 也可以"同一个 VPSS 通道双绑"(回退方案)。
 */
#define BSP_VENC_CHN_MJPEG  2
#define BSP_VPSS_CHN_MJPEG  2

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
 * @brief 选择要取的那一路编码通道。**必须在 `bsp_mpp_init()` 之前调用。**
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


int bsp_mpp_get_venc_chn(void)
{
    return g_want_h265 ? BSP_VENC_CHN_H265 : BSP_VENC_CHN_H264;
}

void bsp_mpp_get_encoder_size(int *width, int *height)
{
    /* 这三个数与 init_vpss() 里给 VPSS 通道设的尺寸、以及传给
     * SAMPLE_COMM_VENC_Start 的 PIC_1080P / PIC_720P 是**同一组事实**,
     * 改这里必须同时改那里。 */
    if (width != NULL) {
        *width = g_want_h265 ? 1920 : 1280;
    }
    if (height != NULL) {
        *height = g_want_h265 ? 1080 : 720;
    }
}


/* ─────────── ② 取流 ─────────── */

/**
 * @brief 轮询等待"有帧可取"。
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
 * @brief 记录"码流缓冲已满, 但我们仍然要把它取走"这件事。
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
 * @brief 把 MPP 的 pack 数组搬进我们的帧描述(只搬指针, 不拷数据)。
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

/**
 * @brief 等不到帧时/状态可能过期时,**重新查一次**通道状态
 *
 * @param[in,out] st 状态(原地刷新)
 * @return 1 = 现在确实有整帧可取; 0 = 还是没有(当"这一刻没帧"处理)
 *
 * @note ★★ 修 B047(2026-09-20 实验里抓到):**轮询等到的状态必须重新查一遍**。
 *       `wait_for_packs()` 是"轮询到 `u32CurPacks > 0` 就返回", 而调用方手里那个快照
 *       是**轮询之前**的 —— 于是 `u32CurPacks == 0` 却继续往下走, 把 `u32PackCount = 0`
 *       传给 `GetStream`, 驱动直接报 `0xa0088003`(**ILLEGAL_PARAM**): 一帧白丢,
 *       而且丢的是**已经编码好、躺在缓冲里**的那一帧。
 *       板上现象(实测): 主路第一次取流就报
 *         `码流缓冲已满(leftStreamFrames=0)` + `GetStream 失败 … (curPacks=0 leftFrames=0)`
 *       然后整条基线 0 帧 —— **这两个数字自相矛盾**(缓冲满却是 0 个 pack)正是泄漏点。
 *       正常跑的时候这个竞态窗口只有几毫秒, 所以一直没暴露。
 * @note 错误码名字是用一个交叉编译的小探针在板上打印出来的(不是猜的):
 *       `ILLEGAL_PARAM = 0xa0088003`,`BUF_EMPTY = 0xa008800e` —— 两者只差一位,
 *       我第一次就猜错了(`work/diag_fps_and_err.py`)。
 */
static int refresh_packs(VENC_CHN_STATUS_S *st)
{
    if (st->u32CurPacks == 0) {
        if (HI_MPI_VENC_QueryStatus(g.chn, st) != HI_SUCCESS
            || st->u32CurPacks == 0) {
            return 0;
        }
    }
    return 1;
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
    if (!refresh_packs(&st)) {      /* ★ B047: 轮询之后必须刷新快照 */
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
 * @brief 第 1 步: 系统初始化 + VB(视频缓冲池)。
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
 * @brief 填 VI 配置结构。
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

/** @brief 第 2 步: VI(视频输入)。摄像头 → 采集。 */
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
 * @brief 第 3 步: VPSS。
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
 * @brief 第 4 步: VENC —— **只启动被选中的那一路**。
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

    bsp_mpp_mjpeg_close();          /* MJPEG 通道(若开着)必须先拆掉 */

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

/* ─────────── ⑤ MJPEG:按需启停的一路 JPEG 编码(2026-09-20) ─────────── */

/*
 * MJPEG 的 pack 数组
 *   容量依据 : 一帧 JPEG 只有 1 个 pack;给 8 个是余量(与主路的 64 个分开, 不能共用)
 *   内存区域 : **文件级静态**(8 × sizeof(VENC_PACK_S))
 *   唯一所有者: bsp_mpp 模块自己
 *   释放时机 : 进程生命周期内常驻
 */
static VENC_PACK_S g_mjpeg_packs[8];

static struct {
    int mjpeg_open;                 /* 1 = MJPEG 通道已建好并绑上 */
    int mjpeg_vpss_chn;             /* 绑的是哪一路 VPSS(-1 = 没绑) */
    int mjpeg_used_chn2;            /* 1 = 我们启用了 VPSS chn2, 关的时候要停用 */
    int mjpeg_w;                    /* 实际编码尺寸 */
    int mjpeg_h;
    uint64_t mjpeg_frames;          /* 累计取到的 JPEG 帧数 */
    uint64_t mjpeg_bytes;           /* 累计 JPEG 字节数 */
    uint64_t mjpeg_errors;
} mj = { .mjpeg_vpss_chn = -1 };    /* ⚠️ 静态初值是 0, 必须显式给 -1(0 是合法通道号) */

/**
 * @brief 启用 VPSS 的 chn2(小图那一路)
 *
 * @param[in] w 宽
 * @param[in] h 高
 * @return 0 成功; -1 失败
 *
 * @note 照抄 `init_vpss()` 里那两路的填法, 只改尺寸;`u32Depth = 0` = 不用用户队列
 *       (绑定模式由 VENC 直接取)。
 */
static int mjpeg_vpss_chn2_on(int w, int h)
{
    VPSS_CHN_ATTR_S attr;
    HI_S32          ret;

    memset(&attr, 0, sizeof(attr));
    attr.enChnMode      = VPSS_CHN_MODE_USER;
    attr.enCompressMode = COMPRESS_MODE_NONE;
    attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    attr.enPixelFormat  = SAMPLE_PIXEL_FORMAT;
    attr.enVideoFormat  = VIDEO_FORMAT_LINEAR;
    attr.stFrameRate.s32SrcFrameRate = -1;
    attr.stFrameRate.s32DstFrameRate = -1;
    attr.u32Width  = (HI_U32)w;
    attr.u32Height = (HI_U32)h;
    attr.u32Depth  = 0;
    ret = HI_MPI_VPSS_SetChnAttr(BSP_VPSS_GRP, BSP_VPSS_CHN_MJPEG, &attr);
    if (ret != HI_SUCCESS) {
        LOG_WARN("VPSS SetChnAttr(chn%d) 失败: %#x —— 退回『同通道双绑』",
                 BSP_VPSS_CHN_MJPEG, ret);
        return -1;
    }
    ret = HI_MPI_VPSS_EnableChn(BSP_VPSS_GRP, BSP_VPSS_CHN_MJPEG);
    if (ret != HI_SUCCESS) {
        LOG_WARN("VPSS EnableChn(chn%d) 失败: %#x —— 退回『同通道双绑』",
                 BSP_VPSS_CHN_MJPEG, ret);
        return -1;
    }
    return 0;
}

/**
 * @brief 建 MJPEG 通道并绑到指定 VPSS 通道(字段填法照抄厂商 `case PT_MJPEG`)
 *
 * @param[in] vpss_chn 绑哪一路 VPSS
 * @param[in] w        输入/编码宽(**必须 ≥ 源的实际尺寸**, 否则驱动报 ILLEGAL_PARAM)
 * @param[in] h        输入/编码高
 * @param[in] qfactor  质量 1~99
 * @param[in] fps      目标帧率
 * @return 0 成功; -1 失败
 */
static int mjpeg_chn_create(int vpss_chn, int w, int h, int qfactor, int fps)
{
    VENC_CHN_ATTR_S    attr;
    VENC_MJPEG_FIXQP_S q;
    HI_S32             ret;

    memset(&attr, 0, sizeof(attr));
    memset(&q, 0, sizeof(q));
    attr.stVencAttr.enType          = PT_MJPEG;
    attr.stVencAttr.u32MaxPicWidth  = (HI_U32)w;
    attr.stVencAttr.u32MaxPicHeight = (HI_U32)h;
    attr.stVencAttr.u32PicWidth     = (HI_U32)w;
    attr.stVencAttr.u32PicHeight    = (HI_U32)h;
    attr.stVencAttr.u32BufSize      = (HI_U32)(w * h * 2);
    attr.stVencAttr.bByFrame        = HI_TRUE;
    attr.stVencAttr.u32Profile      = 0;
    attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;
    q.u32Qfactor       = (HI_U32)qfactor;
    q.u32SrcFrameRate  = 30;
    q.fr32DstFrameRate = (HI_S32)fps;
    memcpy(&attr.stRcAttr.stMjpegFixQp, &q, sizeof(q));
    attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    attr.stGopAttr.stNormalP.s32IPQpDelta = 0;

    ret = HI_MPI_VENC_CreateChn(BSP_VENC_CHN_MJPEG, &attr);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("VENC CreateChn(MJPEG chn%d, %dx%d) 失败: %#x",
                  BSP_VENC_CHN_MJPEG, w, h, ret);
        return -1;
    }
    ret = SAMPLE_COMM_VPSS_Bind_VENC(BSP_VPSS_GRP, vpss_chn, BSP_VENC_CHN_MJPEG);
    if (ret != HI_SUCCESS) {
        LOG_ERROR("VPSS(grp%d-chn%d) 绑 MJPEG chn%d 失败: %#x",
                  BSP_VPSS_GRP, vpss_chn, BSP_VENC_CHN_MJPEG, ret);
        (void)HI_MPI_VENC_DestroyChn(BSP_VENC_CHN_MJPEG);
        return -1;
    }
    return 0;
}

int bsp_mpp_mjpeg_open(int qfactor, int fps)
{
    VENC_RECV_PIC_PARAM_S p;
    int                   w = BSP_MPP_MJPEG_WIDTH;
    int                   h = BSP_MPP_MJPEG_HEIGHT;

    if (mj.mjpeg_open) {
        return 0;                       /* 幂等: 已经有客户端开着了 */
    }
    if (!g.inited) {
        return -1;
    }
    mj.mjpeg_vpss_chn  = -1;            /* 每次开之前重置(静态初值 0 是合法通道号) */
    mj.mjpeg_used_chn2 = 0;
    /* 路径 ①: 另加一路 VPSS chn2(小图, 省带宽) */
    if (mjpeg_vpss_chn2_on(w, h) == 0) {
        if (mjpeg_chn_create(BSP_VPSS_CHN_MJPEG, w, h, qfactor, fps) == 0) {
            mj.mjpeg_used_chn2  = 1;
            mj.mjpeg_vpss_chn   = BSP_VPSS_CHN_MJPEG;
        } else {
            (void)HI_MPI_VPSS_DisableChn(BSP_VPSS_GRP, BSP_VPSS_CHN_MJPEG);
        }
    }
    /* 路径 ②(回退): 同一个 VPSS 通道**双绑**(实测可用, 只是图大、带宽高)。
     * ⚠️ 尺寸必须 ≥ 源的实际尺寸, 否则驱动报 ILLEGAL_PARAM(踩过: 一开始给 640x360
     *    去接 720p 的源, 直接 `0xa0088003`)。 */
    if (mj.mjpeg_vpss_chn < 0) {
        bsp_mpp_get_encoder_size(&w, &h);
        if (mjpeg_chn_create(g.vpss_chn, w, h, qfactor, fps) != 0) {
            return -1;
        }
        mj.mjpeg_vpss_chn = g.vpss_chn;
    }
    memset(&p, 0, sizeof(p));
    p.s32RecvPicNum = -1;               /* -1 = 一直收(照抄厂商) */
    if (HI_MPI_VENC_StartRecvFrame(BSP_VENC_CHN_MJPEG, &p) != HI_SUCCESS) {
        LOG_ERROR("MJPEG StartRecvFrame 失败");
        bsp_mpp_mjpeg_close();
        return -1;
    }
    mj.mjpeg_open = 1;
    mj.mjpeg_w    = w;
    mj.mjpeg_h    = h;
    LOG_INFO("MJPEG 通道就绪(chn%d, %dx%d, q=%d, %d fps, VPSS chn%d%s)",
             BSP_VENC_CHN_MJPEG, w, h, qfactor, fps, mj.mjpeg_vpss_chn,
             mj.mjpeg_used_chn2 ? "" : ", 双绑主通道");
    return 0;
}

void bsp_mpp_mjpeg_close(void)
{
    MPP_CHN_S src;
    MPP_CHN_S dst;

    if (!mj.mjpeg_open && mj.mjpeg_vpss_chn < 0) {
        return;                         /* 幂等 */
    }
    if (mj.mjpeg_vpss_chn >= 0) {
        src.enModId  = HI_ID_VPSS;
        src.s32DevId = 0;
        src.s32ChnId = mj.mjpeg_vpss_chn;
        dst.enModId  = HI_ID_VENC;
        dst.s32DevId = 0;
        dst.s32ChnId = BSP_VENC_CHN_MJPEG;
        (void)HI_MPI_SYS_UnBind(&src, &dst);
        (void)HI_MPI_VENC_StopRecvFrame(BSP_VENC_CHN_MJPEG);
        (void)HI_MPI_VENC_DestroyChn(BSP_VENC_CHN_MJPEG);
        mj.mjpeg_vpss_chn = -1;
    }
    if (mj.mjpeg_used_chn2) {
        (void)HI_MPI_VPSS_DisableChn(BSP_VPSS_GRP, BSP_VPSS_CHN_MJPEG);
        mj.mjpeg_used_chn2 = 0;
    }
    if (mj.mjpeg_open) {
        mj.mjpeg_open = 0;
        LOG_INFO("MJPEG 通道已关(没人看实时画面了, MPP 回到『只有主路』的状态)");
    }
}

int bsp_mpp_mjpeg_is_open(void)
{
    return mj.mjpeg_open;
}

int bsp_mpp_mjpeg_get_frame(uint8_t *buf, size_t cap, size_t *out_len,
                            int timeout_ms)
{
    VENC_CHN_STATUS_S st;
    VENC_STREAM_S     s;
    size_t            used = 0;
    HI_U32            i;
    HI_S32            ret;

    if (!mj.mjpeg_open || buf == NULL || out_len == NULL) {
        return -1;
    }
    *out_len = 0;
    (void)HI_MPI_VENC_QueryStatus(BSP_VENC_CHN_MJPEG, &st);
    if (st.u32CurPacks == 0) {
        return 0;                       /* 还没编好 */
    }
    memset(&s, 0, sizeof(s));
    s.u32PackCount = st.u32CurPacks;
    s.pstPack      = g_mjpeg_packs;
    ret = HI_MPI_VENC_GetStream(BSP_VENC_CHN_MJPEG, &s, timeout_ms);
    if (ret != HI_SUCCESS) {
        mj.mjpeg_errors++;
        return -2;
    }
    for (i = 0; i < s.u32PackCount && i < 8; i++) {
        size_t n = (size_t)s.pstPack[i].u32Len;

        if (used + n > cap) {
            n = cap - used;             /* 截断: JPEG 截断也解不出图, 但要避免溢出 */
            mj.mjpeg_errors++;
        }
        memcpy(buf + used, s.pstPack[i].pu8Addr + s.pstPack[i].u32Offset, n);
        used += n;
    }
    (void)HI_MPI_VENC_ReleaseStream(BSP_VENC_CHN_MJPEG, &s);
    *out_len = used;
    mj.mjpeg_frames++;
    mj.mjpeg_bytes += used;
    return 1;
}
