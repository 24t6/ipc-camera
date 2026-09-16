/**
 * @file    svc_media.c
 * @brief   取流服务实现 —— MPP 取帧 → 拼成连续缓冲 → 入队
 *
 * 【模块职责】取流线程: MPP 取帧 → 逐 pack 拼进连续槽位 → 写帧头 → 入队 → 立刻释放
 * 【依赖方向】依赖 bsp_mpp、infra_queue、proto_nalu、infra_log
 * 【线程模型】自己起 **1 个**线程(stream_thread, 线程名 `ipc_media`); MPP 由它独占
 * 【资源边界】槽位缓冲在 start 时一次分配(start 后视为静态), stop 时释放; 运行期零分配
 *
 * 结构:
 *      ① 模块状态
 *      ② 槽位读写小工具
 *      ③ 取流线程
 *      ④ 生命周期
 *
 * 设计要点(详见 svc_media.h):
 *      · 队里放**一帧**(带帧头, 头部含 pts)→ 发送端不用自己数帧
 *      · 多个 pack **必须逐 pack 拷贝**拼成连续内存
 *        (官方文档: 多包模式下各 pack 不保证连续)
 *      · 槽位缓冲 **start 时一次分配**, 运行期零 malloc
 */
#include "svc_media.h"

#include "bsp_mpp.h"
#include "infra_log.h"
#include "infra_queue.h"
#include "proto_nalu.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>      /* prctl(PR_SET_NAME) —— 给线程起名, ps/top 能看出来 */
#include <time.h>

/** 槽位容量 = 帧头 + 最大帧字节数。架构预算里单帧 256 KB */
#define SVC_SLOT_BYTES (SVC_MEDIA_HDR_SIZE + BSP_MPP_FRAME_MAX_BYTES)

/** 取不到帧时的等待粒度(毫秒)。太密会空转 CPU, 太疏会攒帧 */
#define SVC_WAIT_MS 5

/**
 * 启动时最多等多久才有第一帧(毫秒)。
 *
 * @note ⚠️ 这个等待**是被一次真实的间歇性故障逼出来的**(2026-09-15):
 *   板端连跑 `media_smoke` 时, 有大约 1/3 的次数**整整 10 秒一帧都取不到**,
 *   而 VI/ISP 的启动日志看起来完全正常。对比两种情况的日志:
 *
 *     · 正常: `bsp_mpp_init()` 耗时 **5~10 秒**, 之后立刻有帧
 *     · 失败: `bsp_mpp_init()` 耗时 **0.1 秒**, 之后 10 秒一帧都没有
 *
 *   init 快得反常反而说明 **ISP/AE 还没稳**(它在 0.1 秒内就返回了,
 *   说明相关初始化路径走了"已就绪"的分支), 这时 VENC 暂时不产码流。
 *
 *   所以启动时**主动等第一帧**再宣布"服务已启动":
 *   这既能让调用方拿到一个可信的"就绪"信号, 也能让间歇性失败变成
 *   "多等一两秒"而不是"整轮一帧没有"。
 */
#define SVC_FIRST_FRAME_TIMEOUT_MS 5000

/** 等首帧时, 每次试探之间睡多久(毫秒)。用睡眠而不是忙等, 免得抢 CPU */
#define SVC_WARMUP_STEP_MS 50

/* ─────────── ① 模块状态 ─────────── */

static struct {
    int             running;
    infra_queue_t  *queue;          /* 不拥有: 队列由调用方创建/销毁 */
    infra_queue_t  *record_queue;   /* 同上;NULL = 不录制(M3 的扇出目标) */
    size_t          slot_data_cap;  /* 槽位里能放多少码流字节 */
    int             is_h265;
    pthread_t       thread;
    int             thread_valid;
    volatile int    stop_requested;

    /** @brief 启动时等首帧实际等了多久(毫秒); 只用于日志 */
    long            warmup_ms;

    /* 槽位缓冲: start 时一次分配 */
    uint8_t        *slot;

    svc_media_stats_t stats;
} g;

/* ─────────── ② 槽位读写小工具 ─────────── */

const svc_media_frame_hdr_t *svc_media_slot_hdr(const void *slot)
{
    const svc_media_frame_hdr_t *h = (const svc_media_frame_hdr_t *)slot;

    if (h == NULL || h->magic != SVC_MEDIA_FRAME_MAGIC)
        return NULL;
    return h;
}

const uint8_t *svc_media_slot_data(const void *slot)
{
    if (svc_media_slot_hdr(slot) == NULL)
        return NULL;
    return (const uint8_t *)slot + SVC_MEDIA_HDR_SIZE;
}

/* ─────────── ③ 取流线程 ─────────── */

/**
 * @brief 把一帧的所有 pack 拼进槽位缓冲, 并**一次性写好帧头**。
 *
 * @param frame 取到的帧(指向 MPP 内部缓冲)
 * @param index 帧序号(写进帧头)
 * @return 码流字节数; 0 = 超容量(已计 oversize)
 *
 * @note ⚠️ **本函数负责把帧头全部写完, 之后调用方不许再往槽位里写**。
 *       这条约定是被一个真 bug 逼出来的(2026-09-15):
 *       原来 `handle_one_frame` 在调用本函数**之后**才写 `hdr->session_id`,
 *       而 `session_id` 的偏移是 24; 当时 `SVC_MEDIA_HDR_SIZE` 被手写成了 24
 *       (实际 32), 于是那行赋值**正好把码流的头 4 个字节清零** ——
 *       起始码 `00 00 00 01` 变成 `00 00 00 00`, 整帧解析不出 NALU。
 *
 *       现在把"写头"和"填码流"收进同一个函数、且**帧头先写**:
 *       即使将来又把大小算错, 被覆盖的也只会是**帧头**(一眼看得出),
 *       而不是码流被静默毁掉。详见 `svc_media.h` 的 HDR_SIZE 说明。
 *
 * @note ⚠️ **必须逐 pack 拷贝** —— 官方文档《HiMPP V4.0 媒体处理软件开发参考》
 *       6.2.8/6.2.13:多包模式(默认)下**每个 pack 各有 pu8Addr, 不保证连续**;
 *       只有单包模式(u32OneStreamBuffer=1)才是一整帧连续。
 *       槽位是一段连续内存, 所以这里只能一个个搬。
 */
static size_t pack_into_slot(const bsp_mpp_frame_t *frame, uint32_t index)
{
    uint8_t               *dst = g.slot + SVC_MEDIA_HDR_SIZE;
    svc_media_frame_hdr_t *hdr = (svc_media_frame_hdr_t *)g.slot;
    size_t                 total = 0;
    int                    i;

    for (i = 0; i < frame->pack_count; i++) {
        size_t len = frame->packs[i].len;

        if (total + len > g.slot_data_cap)
            return 0;               /* 超容量, 整帧丢弃 */
        memcpy(dst + total, frame->packs[i].data, len);
        total += len;
    }

    /* ── 码流拷完, 一次性把帧头写全(在本函数之后没人再写槽位)── */
    hdr->magic       = SVC_MEDIA_FRAME_MAGIC;
    hdr->len         = (uint32_t)total;
    hdr->cap         = (uint32_t)g.slot_data_cap;
    hdr->frame_index = index;
    hdr->pts         = frame->pts;
    hdr->session_id  = 0;
    hdr->reserved    = 0;
    return total;
}

/**
 * @brief NALU 计数回调 —— 只数个数, 不做别的。
 * @return 0 = 继续遍历
 */
static int count_nalu_cb(const proto_nalu_t *n, void *user)
{
    int *cnt = (int *)user;

    (void)n;
    if (cnt != NULL)
        (*cnt)++;
    return 0;
}

/**
 * @brief 处理一帧: 校验 → 拼进槽位 → 入队。
 * @return 0 正常(含"丢弃"的情况); -1 表示应当退出线程
 */
/**
 * @brief 把当前槽位**再 push 一份**给录制队列(M3 的扇出)
 *
 * @param[in] total 槽位里的有效字节数(不含帧头)
 * @return `infra_queue_push` 的返回值(1 = 为腾位置丢了最旧的)
 *
 * @note 抽出来是为了让调用处那行**短到不用续行** ——
 *       `infra_queue_push` 三个参数摊在 `if (... && ...)` 里要续行到 25 空格,
 *       会被 `check_style.py` 判成"缩进 6 层 > 5"。
 *       ⚠️ 这个坑今天踩了**四次**(bsp_osd / svc_record_policy / svc_record / 这里),
 *       统一对策就是"抽 helper", 见 CODING_STYLE.md §9.2。
 */
static int push_record_copy(size_t total)
{
    return infra_queue_push(g.record_queue, g.slot,
                            SVC_MEDIA_HDR_SIZE + total);
}

/**
 * @brief 处理一帧:拼进槽位 → 校验 → 入队(发送队列 + 录制队列)
 *
 * @param[in] frame 刚从 MPP 取到的帧
 * @return 0 处理完(成功或按策略丢弃)
 */
static int handle_one_frame(const bsp_mpp_frame_t *frame)
{
    size_t total;
    int    rc;

    /*
     * ★ 帧序号用**写之前的**计数, 所以从 0 开始递增。
     * @note 这一调用会把帧头和码流**一次写好**, 之后不许再往槽位里写 ——
     *       原因见 pack_into_slot 的说明(曾经因此把整帧码流毁掉)。
     */
    total = pack_into_slot(frame, (uint32_t)g.stats.frames);
    if (total == 0) {
        /* 单帧超过槽位容量。**丢弃但继续跑** —— 不能因为一帧异常就停服务。
         * (架构里单帧按 256 KB 预算, 实测最大 NALU 115 KB, 正常不会走到这) */
        g.stats.oversize++;
        LOG_WARN("一帧超过槽位容量(%zu 字节), 丢弃", g.slot_data_cap);
        return 0;
    }

    /*
     * 入队前先数一下"这一帧里到底有没有 NALU"。
     * 为什么值得做: NALU 解析是**下游**(发送端)的事, 但如果这里能提前发现
     * "整帧一个起始码都没有", 就说明码流形态和预期不符(比如把 H.265 当 H.264 解),
     * 早一点计数比等发送端失败更容易定位。
     */
    {
        int nalu_cnt = 0;

        proto_nalu_foreach(g.slot + SVC_MEDIA_HDR_SIZE,
                           total, g.is_h265, count_nalu_cb, &nalu_cnt);
        if (nalu_cnt <= 0) {
            g.stats.parse_rejects++;
            LOG_WARN("这一帧没解析出任何 NALU(%zu 字节, is_h265=%d), 丢弃",
                     total, g.is_h265);
            return 0;
        }
    }

    /* ★ push 永不阻塞(队满丢最旧)。返回值 1 表示"为腾位置丢了最旧的" */
    rc = infra_queue_push(g.queue, g.slot, SVC_MEDIA_HDR_SIZE + total);
    if (rc < 0) {
        LOG_ERROR("入队失败(参数非法或单帧超长)");
        return 0;
    }
    if (rc == 1)
        g.stats.queue_dropped++;

    /*
     * ★ 扇出:同一个槽位**再 push 一份**给录制队列。
     *   两个队列互相独立 —— 磁盘慢只会撑满录制队列(丢录制的帧),
     *   发送队列照样满速;反之亦然。这就是"采集/发送/录制三者解耦"。
     *   没开录制时(NULL)直接跳过, 零开销。
     */
    if (g.record_queue != NULL && push_record_copy(total) == 1) {
        g.stats.record_dropped++;
    }

    g.stats.frames++;
    g.stats.bytes += total;
    return 0;
}

/** @brief 取流线程主体 */
static void *stream_thread(void *arg)
{
    (void)arg;
    /* §7.1: 线程名让 `ps` / `top` 一眼看出这是谁, 不用靠 pid 猜 */
    (void)prctl(PR_SET_NAME, "ipc_media", 0, 0, 0);
    LOG_INFO("取流线程启动(H.26%d, 槽位数据容量 %zu 字节)",
             g.is_h265 ? 5 : 4, g.slot_data_cap);

    while (!g.stop_requested) {
        bsp_mpp_frame_t frame;
        int             rc = bsp_mpp_get_frame(&frame, SVC_WAIT_MS);

        if (rc == 0) {
            handle_one_frame(&frame);
            /*
             * ★ 立刻释放。官方文档明确警告:
             *   "建议用户获取码流接口调用与释放码流的接口调用成对出现,
             *     且尽快释放码流, 防止……码流 buffer 满, 停止编码。"
             *   所以**不能**把 frame 的数据一直拿着 —— 我们已经在
             *   handle_one_frame 里拷进槽位了, 这里马上还回去。
             */
            bsp_mpp_release_frame(&frame);
        } else if (rc == 1) {
            g.stats.get_timeouts++;     /* 此刻没帧, 正常, 不刷日志 */
        } else {
            g.stats.get_errors++;
            LOG_ERROR("取帧失败 rc=%d(连续出错会拖慢编码, 需要查)", rc);
            /* 短暂退避, 避免出错时疯狂刷日志 */
            {
                struct timespec ts = { 0, 20 * 1000 * 1000 };

                nanosleep(&ts, NULL);
            }
        }
    }

    LOG_INFO("取流线程退出(累计 %llu 帧 / %llu 字节)",
             (unsigned long long)g.stats.frames,
             (unsigned long long)g.stats.bytes);
    return NULL;
}

/* ─────────── ④ 生命周期 ─────────── */

/**
 * @brief 校验队列并算出一帧最多能放多少码流字节。
 *
 * @return 0 成功(已设好 `g.slot_data_cap`); 负值失败
 *
 * @note 抽出来的理由:`svc_media_start()` 里"**检查调用方给的东西**"和
 *       "**把服务拉起来**"是两件事, 混在一个函数里会互相淹没。
 *       (函数规模只是症状, "两件事挤在一起"才是原因。)
 */
static int setup_from_queue(void)
{
    infra_queue_stats_t qs;

    /*
     * 槽位数据容量 = 队列槽位大小 - 帧头。
     * 这样即使调用方给的槽位比预期小, 我们也不会写越界(按实际的算)。
     */
    infra_queue_get_stats(g.queue, &qs);
    if (qs.slot_size <= (size_t)SVC_MEDIA_HDR_SIZE + 1024) {
        LOG_ERROR("队列槽位太小(%zu 字节), 至少要有帧头+1KB", qs.slot_size);
        return -1;
    }
    g.slot_data_cap = qs.slot_size - (size_t)SVC_MEDIA_HDR_SIZE;
    return 0;
}

/**
 * @brief 等第一帧出现(启动握手)。
 *
 * @return 0 = 等到了; -1 = 超时仍没有
 *
 * @note 等到的这一帧**立刻释放但不入队** —— 它的作用是确认"编码器真在出码流",
 *       不是当数据用。所以这里不会污染 stats.frames(帧序号仍从 0 开始)。
 * @note 只在 `svc_media_start()` 里被调用一次, 取流线程**还没起来**,
 *       所以这里独占 `bsp_mpp_*`, 不违反"MPP 只允许一个线程碰"的约定。
 */
static int warmup_wait_first_frame(void)
{
    long waited = 0;

    while (waited < SVC_FIRST_FRAME_TIMEOUT_MS) {
        bsp_mpp_frame_t frame;
        struct timespec ts = { 0, SVC_WARMUP_STEP_MS * 1000L * 1000L };
        int             rc = bsp_mpp_get_frame(&frame, 0);   /* 不等待, 探一下 */

        if (rc == 0) {
            bsp_mpp_release_frame(&frame);
            g.warmup_ms = waited;
            return 0;
        }
        nanosleep(&ts, NULL);
        waited += SVC_WARMUP_STEP_MS;
    }
    return -1;
}

int svc_media_start(void *queue, void *record_queue, int is_h265)
{
    if (g.running)
        return 0;
    if (queue == NULL)
        return -1;

    memset(&g, 0, sizeof(g));
    g.queue        = (infra_queue_t *)queue;
    g.record_queue = (infra_queue_t *)record_queue;   /* NULL = 不录制 */
    g.is_h265      = is_h265 ? 1 : 0;

    if (setup_from_queue() != 0)
        return -2;

    g.slot = (uint8_t *)malloc(SVC_MEDIA_HDR_SIZE + g.slot_data_cap);
    if (g.slot == NULL) {
        LOG_ERROR("槽位缓冲分配失败(%zu 字节)",
                  SVC_MEDIA_HDR_SIZE + g.slot_data_cap);
        return -3;
    }

    /*
     * ★ 先告诉 bsp "取哪一路", **再**初始化 —— bsp 只会启动被选中的那一路。
     *   两路都开的话, **没人取的那一路**会塞满自己的码流缓冲并把 VPSS 拖住,
     *   最终连我们在取的那一路一起死掉(实测死在 205 帧)。详见 B027 / bsp_mpp.h。
     */
    bsp_mpp_select_encoder(g.is_h265);
    if (bsp_mpp_init() != 0) {
        LOG_ERROR("MPP 通路初始化失败");
        free(g.slot);
        g.slot = NULL;
        return -4;
    }

    /*
     * ★ 等第一帧再宣布启动成功。
     *   MPP init 返回不代表编码器已经在产码流 —— 实测 init 只花 0.1 秒时
     *   往往一帧都取不到(见 SVC_FIRST_FRAME_TIMEOUT_MS 的说明)。
     *   这里阻塞等一会儿, 把"间歇性启动失败"变成"多等一两秒"。
     */
    if (warmup_wait_first_frame() != 0) {
        LOG_WARN("等 %.1f 秒仍没有第一帧, 仍然启动(可能是环境问题, 稍后会自己好)",
                 SVC_FIRST_FRAME_TIMEOUT_MS / 1000.0);
    } else {
        LOG_INFO("已确认编码器在出码流(等首帧用了 %ld 毫秒)",
                 g.warmup_ms);
    }
    g.warmup_ms = 0;

    g.stop_requested = 0;
    g.running        = 1;
    if (pthread_create(&g.thread, NULL, stream_thread, NULL) != 0) {
        LOG_ERROR("取流线程创建失败");
        g.running = 0;
        bsp_mpp_deinit();
        free(g.slot);
        g.slot = NULL;
        return -5;
    }
    g.thread_valid = 1;
    LOG_INFO("取流服务已启动");
    return 0;
}

void svc_media_stop(void)
{
    if (!g.running)
        return;

    g.stop_requested = 1;
    if (g.thread_valid) {
        pthread_join(g.thread, NULL);   /* 等线程真退出, 否则 bsp_mpp_deinit 会 use-after-free */
        g.thread_valid = 0;
    }
    g.running = 0;

    bsp_mpp_deinit();                   /* 按申请反序释放通路 */
    free(g.slot);
    g.slot = NULL;

    LOG_INFO("取流服务已停止(队列丢弃 %llu / 超长 %llu / 无 NALU %llu)",
             (unsigned long long)g.stats.queue_dropped,
             (unsigned long long)g.stats.oversize,
             (unsigned long long)g.stats.parse_rejects);
}

int svc_media_is_running(void)
{
    return g.running;
}

void svc_media_get_stats(svc_media_stats_t *out)
{
    if (out != NULL)
        *out = g.stats;
}
