/**
 * @file    jpeg_smoke.c
 * @brief   MJPEG(实时画面进浏览器)之前的**孤立实验** —— 只回答几个"上板才有答案"的问题
 *
 * 【模块职责】板上实验:一路**按需启停**的 MJPEG 编码通道, 对主路(H.264)有没有害
 * 【依赖方向】依赖 bsp_mpp(主路取流)与 HI_MPI_*(MJPEG 通道**自己直连** ——
 *             实验阶段先不进 bsp 层:结论明确了再把验证过的代码搬进去, 见 AGENTS.md §7.0)
 * 【线程模型】单线程顺序跑;每个阶段都有明确判据
 * 【资源边界】无动态分配;MJPEG 的 pack 数组是文件级静态(不能和 bsp_mpp 的 `g.packs` 共用)
 *
 * ─────────────────────────────────────────────────────────────────
 *  为什么先做这个实验(而不是直接写 svc_live)
 * ─────────────────────────────────────────────────────────────────
 *  1. 本项目最贵的一课(B027)是"**没有人取流的编码通道会把整条流水线拖死**"
 *     (实测主路固定停在 205 帧)。MJPEG 要一路"按需启停"的编码通道, 所以
 *     "空闲态安不安全""饿死会不会拖死主路""拆掉能不能恢复"必须先用实验说清楚。
 *  2. 厂商 sample 的抓拍用的是**另一路 VPSS 通道**(`VPSS chn1 → VENC chn1` 抓拍、
 *     `VPSS chn0 → VENC chn0` 主路)。我们主路已经占了 VPSS chn1, 所以要么
 *     "同一个 VPSS 通道双绑", 要么**再加一路 VPSS chn2** —— 两条路都要试。
 *  3. 编码类型:**用 `PT_MJPEG` 而不是 `PT_JPEG`**。
 *     实测(2026-09-20):自己拼 `PT_JPEG` 的 attr 建通道, 驱动回
 *     `0xa0088003` = **ILLEGAL_PARAM**(用探针在板上确认了错误码名字, 见
 *     `work/diag_fps_and_err.py`);而 MJPEG 本来就是为"连续出图"设计的
 *     (厂商 `sample_comm_venc.c` 的 `case PT_MJPEG` 里给了完整的 RC 写法)。
 *
 * ─────────────────────────────────────────────────────────────────
 *  阶段与判据
 * ─────────────────────────────────────────────────────────────────
 *   阶段 0  预热 + 基线:只有主路, 看它自己多少 fps(后面所有判据的对照)
 *   阶段 1  方案 A:**同一个 VPSS 通道双绑**(chn1 → MJPEG chn2), 但**不开收流**
 *           —— 判据: 主路 fps 不掉
 *   阶段 2  A 的活动态:开收流并持续取走 —— 判据: 主路不掉, 且 MJPEG 真的出 JPEG
 *   阶段 3  ★ **开着收流但不取**(B027 场景)—— 判据: 主路 fps **会**掉(证明"必须消费")
 *   阶段 4  ★ 拆掉通道 —— 判据: 主路恢复
 *   阶段 5  方案 B:**另加一路 VPSS chn2(640x360)** 再绑 —— 量小图方案的字节数
 *   阶段 6  qfactor 扫描(在 B 上)—— 给"带宽/画质"选型一个数字
 *
 *  用法(板上;先 `killall ipc_app`):/tmp/jpeg_smoke
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hi_comm_sys.h"
#include "hi_comm_vpss.h"
#include "hi_comm_venc.h"
#include "mpi_sys.h"
#include "mpi_venc.h"
#include "mpi_vpss.h"
#include "sample_comm.h"

#include "bsp_mpp.h"

/** MJPEG 用哪个 VENC 通道(0/1 已经被 H.265/H.264 占了) */
#define MJPEG_VENC_CHN  2

/** 主路取流的单次等待(毫秒) */
#define MAIN_POLL_MS    200

/** MJPEG 一帧只有 1 个 pack, 给 4 个余量 */
#define MJPEG_MAX_PACKS 4

/** 主路(VPSS chn1)的分辨率 —— 双绑方案里 MJPEG 也得按这个尺寸开 */
#define MAIN_W  1280
#define MAIN_H  720

/** 小图方案的分辨率 */
#define SMALL_W 640
#define SMALL_H 360

static VENC_PACK_S g_mpacks[MJPEG_MAX_PACKS];

/** 实际绑给 MJPEG 通道的那一路 VPSS(拆的时候要按它解绑) */
static int g_mjpeg_vpss_chn = -1;

/**
 * @brief 单调毫秒时钟(算 fps 用;板子没有 RTC, 不能用墙上时间)
 */
static long long now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/** @brief 睡一会儿(与项目其它地方一致: nanosleep) */
static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    (void)nanosleep(&ts, NULL);
}

/*
 * ─────────── MJPEG 通道:建 / 收 / 取 / 拆 ───────────
 */

/**
 * @brief 建 MJPEG 通道并绑到指定的 VPSS 通道
 *
 * @param[in] vpss_chn 用哪一路 VPSS 输出喂它
 * @param[in] w        输入(也是编码)宽 —— **必须 ≥ 源的实际尺寸**, 否则驱动报 ILLEGAL_PARAM
 * @param[in] h        输入(也是编码)高
 * @param[in] qfactor  质量(1~99, 越大越清晰越大)
 * @param[in] fps      目标帧率(**让编码器自己降帧**, 而不是"编 30 帧只取 10 帧")
 * @return 0 成功; -1 失败
 *
 * @note ★ 字段填法**照抄厂商** `sample_comm_venc.c` 的 `case PT_MJPEG` +
 *       `SAMPLE_COMM_VENC_Creat()` 的公共部分(出处已记在 `docs/问题与解决记录.md`):
 *       `u32BufSize = w*h*2`、`bByFrame = HI_TRUE`、RC 用 `VENC_RC_MODE_MJPEGFIXQP`
 *       (`u32Qfactor` + `u32SrcFrameRate`/`fr32DstFrameRate`)、
 *       并且 **MJPEG/JPEG 必须显式设 `stGopAttr.enGopMode = VENC_GOPMODE_NORMALP`**
 *       (厂商在 switch 之后专门为这两种类型补了这一段)。
 * @note 为什么 `fr32DstFrameRate` 填 10 而不是 30:让**编码器**降帧。
 *       如果让编码器按 30 帧编、而我们只取 10 帧, 缓冲必然塞满 → B027 拖死整条路。
 */
static int mjpeg_create(int vpss_chn, int w, int h, int qfactor, int fps)
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
    attr.stVencAttr.u32Profile      = 0;
    attr.stVencAttr.bByFrame        = HI_TRUE;

    attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;
    q.u32Qfactor        = (HI_U32)qfactor;
    q.u32SrcFrameRate   = 30;
    q.fr32DstFrameRate  = (HI_S32)fps;
    memcpy(&attr.stRcAttr.stMjpegFixQp, &q, sizeof(q));

    attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    attr.stGopAttr.stNormalP.s32IPQpDelta = 0;

    ret = HI_MPI_VENC_CreateChn(MJPEG_VENC_CHN, &attr);
    if (ret != HI_SUCCESS) {
        printf("   ❌ CreateChn(MJPEG chn%d, %dx%d) 失败: %#x\n",
               MJPEG_VENC_CHN, w, h, ret);
        return -1;
    }
    printf("   ✅ 建好 MJPEG 通道 chn%d(%dx%d, q=%d, %d fps)\n",
           MJPEG_VENC_CHN, w, h, qfactor, fps);

    ret = SAMPLE_COMM_VPSS_Bind_VENC(0, vpss_chn, MJPEG_VENC_CHN);
    printf("   ★ 绑定 VPSS grp0-chn%d → VENC chn%d(MJPEG) → %s\n",
           vpss_chn, MJPEG_VENC_CHN, (ret == HI_SUCCESS) ? "成功" : "**失败**");
    if (ret != HI_SUCCESS) {
        (void)HI_MPI_VENC_DestroyChn(MJPEG_VENC_CHN);
        return -1;
    }
    g_mjpeg_vpss_chn = vpss_chn;
    return 0;
}

/**
 * @brief 拆掉 MJPEG 通道(解绑 → 停收流 → 销毁;申请的反序释放)
 */
static void mjpeg_destroy(void)
{
    MPP_CHN_S src;
    MPP_CHN_S dst;

    if (g_mjpeg_vpss_chn < 0) {
        return;
    }
    src.enModId  = HI_ID_VPSS;
    src.s32DevId = 0;
    src.s32ChnId = g_mjpeg_vpss_chn;
    dst.enModId  = HI_ID_VENC;
    dst.s32DevId = 0;
    dst.s32ChnId = MJPEG_VENC_CHN;
    (void)HI_MPI_SYS_UnBind(&src, &dst);
    (void)HI_MPI_VENC_StopRecvFrame(MJPEG_VENC_CHN);
    (void)HI_MPI_VENC_DestroyChn(MJPEG_VENC_CHN);
    g_mjpeg_vpss_chn = -1;
    printf("   ✅ MJPEG 通道已拆(解绑+停收流+销毁)\n");
}

/**
 * @brief 让 MJPEG 通道开始收流(`s32RecvPicNum = -1` = 一直收, 照抄厂商)
 */
static int mjpeg_recv_on(void)
{
    VENC_RECV_PIC_PARAM_S p;
    HI_S32                ret;

    memset(&p, 0, sizeof(p));
    p.s32RecvPicNum = -1;
    ret = HI_MPI_VENC_StartRecvFrame(MJPEG_VENC_CHN, &p);
    if (ret != HI_SUCCESS) {
        printf("   ❌ StartRecvFrame 失败: %#x\n", ret);
        return -1;
    }
    return 0;
}

/**
 * @brief 取走一帧 MJPEG(不取会被 B027 拖死;取完立刻释放)
 *
 * @param[in]  timeout_ms 等多久
 * @param[out] size       这一帧的字节数(可为 NULL)
 * @return 1 取到; 0 还没编好; 负值失败
 *
 * @note 顺手看**JPEG 魔数**(`FF D8` 开头 / `FF D9` 结尾):
 *       "取到字节"和"取到的是合法 JPEG"是两件事。
 */
static int mjpeg_grab(int timeout_ms, int *size)
{
    VENC_CHN_STATUS_S st;
    VENC_STREAM_S     s;
    HI_S32            ret;
    int               total = 0;
    HI_U32            i;
    const uint8_t    *p0 = NULL;

    (void)HI_MPI_VENC_QueryStatus(MJPEG_VENC_CHN, &st);
    if (st.u32CurPacks == 0) {
        return 0;
    }
    memset(&s, 0, sizeof(s));
    s.u32PackCount = st.u32CurPacks;
    s.pstPack      = g_mpacks;
    ret = HI_MPI_VENC_GetStream(MJPEG_VENC_CHN, &s, timeout_ms);
    if (ret != HI_SUCCESS) {
        printf("      ⚠️ MJPEG GetStream 失败 %#x(curPacks=%u)\n",
               ret, st.u32CurPacks);
        return -1;
    }
    for (i = 0; i < s.u32PackCount && i < MJPEG_MAX_PACKS; i++) {
        total += (int)s.pstPack[i].u32Len;
        if (i == 0) {
            p0 = s.pstPack[0].pu8Addr + s.pstPack[0].u32Offset;
        }
    }
    if (size != NULL) {
        *size = total;
    }
    if (p0 != NULL && total >= 4) {
        static int printed;

        if (printed < 3) {
            printf("      头 %02X %02X … 尾 %02X %02X(%d 字节%s)\n",
                   p0[0], p0[1], p0[s.pstPack[0].u32Len - 2],
                   p0[s.pstPack[0].u32Len - 1], total,
                   (p0[0] == 0xFF && p0[1] == 0xD8) ? ", JPEG 魔数 OK"
                                                    : ", ⚠️ 魔数不对");
            printed++;
        }
    }
    (void)HI_MPI_VENC_ReleaseStream(MJPEG_VENC_CHN, &s);
    return 1;
}

/*
 * ─────────── 主路:持续取走 + 每秒报一次 ───────────
 */

/**
 * @brief 持续取走主路(H.264)的帧, 每秒打印一次 fps
 *
 * @param[in] ms  持续多久
 * @param[in] tag 打印用的标签
 * @return 取到的帧数
 *
 * @note ★ 这个函数本身就是判据:MJPEG 那边一旦把 VPSS 拖住, 这里的 fps 就会掉。
 */
static int drain_main(int ms, const char *tag)
{
    bsp_mpp_frame_t f;
    long long       t0 = now_ms();
    int             n = 0;
    int             in_sec = 0;
    int             sec_n = 0;
    int             bad = 0;
    int             rc;

    while (now_ms() - t0 < (long long)ms) {
        rc = bsp_mpp_get_frame(&f, MAIN_POLL_MS);
        if (rc == 0) {
            bsp_mpp_release_frame(&f);
            n++;
            sec_n++;
        } else if (rc != 1) {
            bad++;                      /* 冷启动偶尔失败是正常的(见 B047), 记数继续 */
        }
        if ((int)((now_ms() - t0) / 1000) != in_sec) {
            printf("      [%s] 第 %d 秒: 主路 %d 帧(失败 %d)\n",
                   tag, in_sec + 1, sec_n, bad);
            in_sec++;
            sec_n = 0;
        }
    }
    printf("    → [%s] 主路 %d 帧 / %d ms = **%.1f fps**(失败 %d)\n",
           tag, n, ms, n * 1000.0 / ms, bad);
    return n;
}

/*
 * ─────────── MJPEG:活动态 / 饿死 / 建小图通道 / qfactor ───────────
 */

/**
 * @brief 一边持续取走 MJPEG, 一边看主路 —— MJPEG 的 fps 与每帧字节
 *
 * @param[in] ms     测多久
 * @param[in] expect 期望的 MJPEG 帧率(只用于打印判据)
 */
static void phase_mjpeg_active(int ms, int expect)
{
    long long t0 = now_ms();
    int       n = 0;
    int       min = 1 << 30;
    int       max = 0;
    long long sum = 0;
    int       main_n = 0;
    int       sz;
    int       rc;

    while (now_ms() - t0 < (long long)ms) {
        rc = mjpeg_grab(200, &sz);
        if (rc == 1) {
            n++;
            sum += sz;
            if (sz < min) {
                min = sz;
            }
            if (sz > max) {
                max = sz;
            }
        }
        {
            bsp_mpp_frame_t f;

            if (bsp_mpp_get_frame(&f, 1) == 0) {
                bsp_mpp_release_frame(&f);
                main_n++;
            }
        }
    }
    printf("    → MJPEG %d 帧 / %d ms = **%.1f fps**(期望 ~%d);"
           " 字节 最小 %d / 平均 %lld / 最大 %d\n",
           n, ms, n * 1000.0 / ms, expect, min, (n > 0) ? sum / n : 0LL, max);
    printf("    → 同期主路 %d 帧 = %.1f fps(判据: 仍应 ~30)\n",
           main_n, main_n * 1000.0 / ms);
}

/**
 * @brief qfactor 扫描:同一个通道改参数, 看每帧字节怎么变
 */
static int phase_qfactor(void)
{
    static const int qs[] = { 90, 75, 50, 30 };
    HI_U32           i;

    for (i = 0; i < sizeof(qs) / sizeof(qs[0]); i++) {
        VENC_MJPEG_FIXQP_S q;
        long long          t0;
        int                n = 0;
        long long          sum = 0;
        int                sz;
        int                rc;

        memset(&q, 0, sizeof(q));
        q.u32Qfactor       = (HI_U32)qs[i];
        q.u32SrcFrameRate  = 30;
        q.fr32DstFrameRate = 10;
        if (HI_MPI_VENC_SetMjpegParam(MJPEG_VENC_CHN, &q) != HI_SUCCESS) {
            printf("    (SetMjpegParam 不支持, 跳过扫描)\n");
            return 0;
        }
        sleep_ms(300);
        t0 = now_ms();
        while (now_ms() - t0 < 1500) {
            rc = mjpeg_grab(200, &sz);
            if (rc == 1) {
                n++;
                sum += sz;
            }
            {
                bsp_mpp_frame_t f;

                if (bsp_mpp_get_frame(&f, 1) == 0) {
                    bsp_mpp_release_frame(&f);
                }
            }
        }
        printf("    q=%2d → %d 帧/1.5s, 平均 **%lld 字节/帧**(%.0f KB)\n",
               qs[i], n, (n > 0) ? sum / n : 0LL,
               (n > 0) ? sum / n / 1024.0 : 0.0);
    }
    return 0;
}

/**
 * @brief 启用 VPSS chn2(小图), 供"小图方案"用
 *
 * @return 0 成功; -1 失败
 */
static int vpss_chn2_on(int w, int h)
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
    ret = HI_MPI_VPSS_SetChnAttr(0, 2, &attr);
    if (ret != HI_SUCCESS) {
        printf("   ❌ VPSS SetChnAttr(chn2) 失败: %#x\n", ret);
        return -1;
    }
    ret = HI_MPI_VPSS_EnableChn(0, 2);
    if (ret != HI_SUCCESS) {
        printf("   ❌ VPSS EnableChn(chn2) 失败: %#x\n", ret);
        return -1;
    }
    printf("   ✅ VPSS chn2 已启用(%dx%d)\n", w, h);
    return 0;
}

/**
 * @brief 跑完"方案 A"(同一个 VPSS 通道双绑)的全部阶段
 *
 * @return 0 = 方案 A 可用; -1 = 建不起来(调用方去试方案 B)
 */
static int try_plan_a(void)
{
    printf("\n[阶段 1] 方案 A:双绑(VPSS chn1 → MJPEG chn2), **不开收流**, 看主路 5 秒\n");
    if (mjpeg_create(1, MAIN_W, MAIN_H, 80, 10) != 0) {
        printf("   ⚠️ 方案 A 建不起来(双绑可能不被支持)\n");
        return -1;
    }
    (void)drain_main(5000, "A 空闲");

    printf("\n[阶段 2] A 的活动态:开收流 + 持续取走 5 秒\n");
    if (mjpeg_recv_on() == 0) {
        phase_mjpeg_active(5000, 10);
    }

    printf("\n[阶段 3] ★ 开着收流但**不取**(B027 场景), 6 秒\n");
    (void)drain_main(6000, "饿死中");

    printf("\n[阶段 4] 拆掉 MJPEG 通道, 看主路能否恢复\n");
    mjpeg_destroy();
    (void)drain_main(4000, "拆掉后");
    return 0;
}

/**
 * @brief 入口:按阶段跑一遍(每步都打印判据, 便于事后核对)
 */
int main(void)
{
    int base;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("===== MJPEG 孤立实验:按需启停安不安全、字节多大 =====\n");

    bsp_mpp_select_encoder(0);          /* 必须在 init 之前(见 B027) */
    if (bsp_mpp_init() != 0) {
        printf("❌ bsp_mpp_init 失败\n");
        return 1;
    }
    printf("✅ 主路就绪(VENC chn%d)\n", bsp_mpp_get_venc_chn());

    printf("\n[阶段 0] 预热 + 基线(冷启动 ISP 曝光/VENC 第一帧要几秒)\n");
    (void)drain_main(3000, "预热");
    base = drain_main(3000, "基线");
    if (base < 45) {
        printf("❌ 基线只有 %d 帧/3s(<15fps)—— 后面没有判别力, 先查主路\n", base);
        bsp_mpp_deinit();
        printf("DIAG_DONE\n");
        return 1;
    }

    if (try_plan_a() != 0) {
        printf("\n(方案 A 不可用, 直接试方案 B)\n");
    }

    printf("\n[阶段 5] 方案 B:另加一路 VPSS chn2(%dx%d) + MJPEG(%dx%d)\n",
           SMALL_W, SMALL_H, SMALL_W, SMALL_H);
    if (vpss_chn2_on(SMALL_W, SMALL_H) == 0
        && mjpeg_create(2, SMALL_W, SMALL_H, 80, 10) == 0
        && mjpeg_recv_on() == 0) {
        phase_mjpeg_active(5000, 10);
        printf("\n[阶段 6] qfactor 扫描(小图方案)\n");
        (void)phase_qfactor();
        mjpeg_destroy();
    } else {
        printf("   ⚠️ 方案 B 也建不起来\n");
    }
    (void)HI_MPI_VPSS_DisableChn(0, 2);
    (void)drain_main(3000, "全部拆完");

    bsp_mpp_deinit();
    printf("\n实验结束:请把每阶段的数字抄进 STATUS.md 的验收行。\n");
    /* ⚠️ 这一行是给 `work/run_board_bin.py` 看的**结束标记** —— 没有它, 运行器
     *    会一直等到超时并打印"程序可能卡住了"(假警报)。工具之间要有约定。 */
    printf("DIAG_DONE\n");
    return 0;
}
