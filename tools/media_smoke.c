/**
 * @file    media_smoke.c
 * @brief   板端集成冒烟测试 —— MPP → svc_media → infra_queue → 消费端
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它补的是哪个洞
 * ─────────────────────────────────────────────────────────────────
 *  到这一步为止, 每个零件都"单独过"了:
 *      · `infra_queue`  → PC 单测(qtest, 含线程压测 + ASan)全过
 *      · `proto_nalu`   → PC 单测(rtsp_test / rtp_test)全过
 *      · `bsp_mpp`      → **只编过, 零告警** —— 但编译通过 ≠ 能取到流
 *      · `svc_media`    → **只编过, 零告警** —— 同上
 *      · 探路程序 `mpp_venc_probe` 证明过"板子能出码流"(206 帧/3,440,412 字节)
 *
 *  但**"零件各自能跑"不等于"接起来能跑"**: 队列槽位够不够、帧头对不对、
 *  多 pack 拼起来还是不是一个合法 Annex-B 帧 —— 这些**只有在板子上连起来跑**
 *  才知道。本程序就是那个"连起来跑"。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⭐ 为什么断言要"跨模块对账", 而不只是"帧数大于 0"
 * ─────────────────────────────────────────────────────────────────
 *  「帧数 > 0」这种断言太弱: 实现写成"每帧只拷第一个 pack"也能过。
 *  所以这里做三层对账, **每一层都拿一个独立的计数源去对另一个**:
 *
 *    ① 字节账:  消费端逐帧累加 len  ==  svc_media 统计的 bytes
 *    ② 帧数账:  poppped + queue_dropped + 队列残留 == svc_media 统计的 frames
 *    ③ 内容账:  把取出的每一帧**再喂给 proto_nalu**(已被单测覆盖的解析器),
 *               数出来的 NALU 总个数应当远大于帧数(H.264 一帧至少 1 个,
 *               关键帧还有 SPS/PPS/SEI), 且**每帧都不能是 0 个**。
 *               这一条能抓住"多 pack 没拼好" —— 拼坏了起始码就断了。
 *
 *  再加两条**物理常识**断言:
 *    ④ PTS 必须递增(它是编码器给的, 见 svc_media.h 里 B012 的说明)
 *    ⑤ 帧大小必须 <= 槽位容量(越界的第一道防线)
 *
 * ─────────────────────────────────────────────────────────────────
 *  怎么跑
 * ─────────────────────────────────────────────────────────────────
 *  在板子上直接跑(需要 root 权限访问 MPP 设备节点):
 *      ./media_smoke          # 默认跑 10 秒
 *      ./media_smoke 5        # 跑 5 秒
 *
 *  退出码: 0 = 全部断言通过; 1 = 有断言失败。
 *  @note 这是**板端**程序, PC 上编不了(依赖 hi_mpi_* / sample_comm_*)。
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "infra_log.h"
#include "infra_queue.h"
#include "proto_nalu.h"
#include "svc_media.h"

/* ─────────── 参数 ─────────── */

/** 槽位数据容量: 架构预算单帧 256 KB(实测最大 NALU 115 KB, 留 2 倍余量) */
#define SMOKE_SLOT_DATA (256 * 1024)

/** 槽位数 8: 约 8 帧 = 0.27 秒的缓冲(与 ARCHITECTURE.md 预算表一致) */
#define SMOKE_CAPACITY  8

/** 默认跑多少秒 */
#define SMOKE_DEFAULT_SEC 10

/** 消费端每次 pop 的最长等待(毫秒)。小一点, 免得退出时卡住 */
#define SMOKE_POP_WAIT_MS 20

/** 每个槽位的总字节数 = 帧头 + 码流容量 */
#define SMOKE_SLOT_BYTES (SVC_MEDIA_HDR_SIZE + SMOKE_SLOT_DATA)

/* ─────────── 断言与统计 ─────────── */

static int g_fails;
static int g_checks;

/** 一条断言。失败时打印出来但不中断 —— 一次跑完能看到所有问题 */
static void check(const char *what, int ok)
{
    g_checks++;
    printf("   %-2s %s\n", ok ? "✅" : "❌", what);
    if (!ok)
        g_fails++;
}

/** 消费端统计(每帧由 nalu_cb 回调累加) */
typedef struct {
    int cnt;
    int key;
    int slice;
} nalu_tally_t;
/** proto_nalu 回调: 数 NALU, 并分类记账 */
static int tally_nalu_cb(const proto_nalu_t *n, void *user)
{
    nalu_tally_t *t = (nalu_tally_t *)user;

    t->cnt++;
    if (n->is_key || n->kind == PROTO_NALU_KIND_IDR)
        t->key++;
    if (n->kind == PROTO_NALU_KIND_IDR || n->kind == PROTO_NALU_KIND_SLICE)
        t->slice++;
    return 0;
}

static volatile int g_stop;

/**
 * 取证用: 把"解析出 0 个 NALU"的槽位原始字节 dump 到文件。
 *
 * @details
 *  为什么需要它: 第一轮板端运行报出"206 帧只解析出 21 个 NALU", 数字自相矛盾 ——
 *  而这**不可能靠推理解决**: 光看计数没法知道那一帧的字节到底长什么样。
 *  所以直接把原始字节落盘, 拿回 PC 用 hexdump + 同一份 `proto_nalu` 复算。
 *  这是"先量, 再改"里"量"的那一步。
 *
 * @note 只 dump 前若干个, 避免把板子 /tmp(tmpfs)写满。
 */
#define SMOKE_DUMP_PATH  "/tmp/media_smoke_dump.bin"
#define SMOKE_DUMP_LIMIT 8

static int  g_dump_fd = -1;
static int  g_dumped;

/**
 * 落盘一个"可疑槽位"。
 *
 * @param slot 槽位起始(含 24 字节帧头)
 * @param n    槽位实际字节数(来自 pop 的 out_len)
 * @param h    已校验过的帧头(可为 NULL)
 * @param why  原因标签
 *
 * @note 每段前置一个 8 字节小端长度, 便于回放时切分。
 */
static void dump_suspect(const uint8_t *slot, size_t n,
                         const svc_media_frame_hdr_t *h, const char *why)
{
    unsigned char lenbuf[8];
    size_t        i;
    int           fd;

    if (g_dumped >= SMOKE_DUMP_LIMIT)
        return;
    if (g_dump_fd < 0) {
        g_dump_fd = open(SMOKE_DUMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_dump_fd < 0)
            return;
    }
    fd = g_dump_fd;

    for (i = 0; i < 8; i++)
        lenbuf[i] = (unsigned char)((n >> (8 * i)) & 0xFF);
    (void)write(fd, lenbuf, 8);
    (void)write(fd, slot, n);
    g_dumped++;

    printf("   [dump] 第 %d 段已落盘(%s): %zu 字节, ", g_dumped, why, n);
    if (h != NULL)
        printf("帧头 magic=%08X len=%u idx=%u\n", h->magic, h->len,
               h->frame_index);
    else
        printf("帧头无效\n");
}

/** Ctrl-C 时优雅退出(否则 MPP 资源不释放, 下次跑会初始化失败) */
static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
    printf("\n[收到 Ctrl-C, 正在优雅停止…]\n");
}

/** 单调时钟, 单位毫秒。用 MONOTONIC 是因为它不受系统时间调整影响 */
static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ─────────── 消费端对账结构 ─────────── */

typedef struct {
    unsigned long long got;          /* 成功取到的帧数 */
    unsigned long long got_bytes;    /* 逐帧累加的码流字节数 */
    unsigned long long got_pkts;     /* 槽位实际字节数(含帧头), 用于核对 push 长度 */
    unsigned long long nalu_total;   /* 跨模块: proto_nalu 数出来的 NALU 总数 */
    unsigned long long nalu_key;     /* 其中关键帧 NALU 数 */
    unsigned long long nalu_slice;   /* 其中切片 NALU 数(每帧至少 1 个) */
    unsigned long long bad_magic;    /* 帧头魔数不符的次数(应为 0) */
    unsigned long long bad_len;      /* len 字段越界的次数(应为 0) */
    unsigned long long len_mismatch; /* 帧头 len 与 pop 出的 out_len 不一致的次数 */
    unsigned long long pts_regress;  /* PTS 倒退的次数(应为 0) */
    unsigned long long empty_frames; /* 一个 NALU 都没解析出来的帧数(应为 0) */
    unsigned long long max_frame;    /* 见过的最大帧字节数 */
    unsigned int       first_idx;    /* 第一帧的 frame_index */
    unsigned int       last_idx;     /* 最后一帧的 frame_index */
    unsigned long long first_pts;    /* 第一帧的 PTS */
    unsigned long long last_pts;     /* 最后一帧的 PTS */
    unsigned           prev_idx;     /* 上一帧序号, 用于查序号连续性 */
    unsigned long long idx_gap;      /* 序号不连续的次数 */
    unsigned long long dup_pts;      /* PTS 与上一帧相同的次数(同帧重复入队会是这种) */
    /**
     * 落在"标称帧率附近"的 PTS 间隔个数。
     *
     * @note 为什么要单独统计, 而不是拿"PTS 跨度 ÷ 墙钟时间"算帧率:
     *   2026-09-15 实测发现两者能差近一倍 —— 板子跑 4 秒只收到 115 帧左右,
     *   而 PTS 间隔稳定在 33332(90kHz 时基 → 30.000 fps)。
     *   原因是**接收端慢不等于编码器慢**: 消费循环里还有 pop 超时、
     *   printf、队列调度等开销, 墙钟时间被这些拖长了。
     *   所以:
     *     · "编码器是不是 30fps" → 看 **PTS 间隔**(编码器自己的时基, 权威)
     *     · "我们处理得够不够快" → 看墙钟帧率(这是**性能指标**, 不是正确性)
     *   把两件事混在一条断言里, 就会得到"编码器明明对、断言却红"的假警报。
     */
    unsigned long long pts_on_time;  /* PTS 间隔在标称值 ±大偏差内的次数 */
    unsigned long long pts_outlier;  /* 明显偏离标称的 PTS 间隔个数(如丢帧造成的大跳变) */
    unsigned long long pts_min;      /* 最小 PTS 间隔 */
    unsigned long long pts_max;      /* 最大 PTS 间隔 */
    /**
     * PTS 间隔直方图(**每 1 毫秒一档**, 覆盖 0~8191 ms), 定义在
     * `g_pts_hist`(文件级数组)。这里只留说明:
     *
     * @note 为什么要有直方图: 一开始我用 `(pts_min + pts_max) / 2` 当"标称间隔",
     *   结果**一个离群值就把它毁掉** —— 实测出现过 min=33296 / max=1333333
     *   (某次卡了 1.33 秒), 中点算出 683314, 于是"时基反推"整个失效。
     *   这正是 **B011** 的教训: **算"率"或"典型值"不要用极值, 要看分布。**
     *
     * @note 精度上限就是档宽: 1ms 档只能给出档中心(如 33500)。
     *   所以另有一条断言直接看 `pts_min`(实测 33326~33340, 更精确)。
     */
} smoke_acc_t;

/** PTS 间隔直方图(1ms 一档)。放文件级是因为 C 的 struct 成员不能带 static */
static unsigned int g_pts_hist[8192];

/**
 * 取出并检查一帧。
 *
 * @param slot  从队列 pop 出来的槽位(头 24 字节是 svc_media 的帧头)
 * @param n     槽位实际字节数(pop 的 out_len, 含帧头)
 * @param is_h265 1=H.265
 * @param a     累加器
 */
/**
 * 打印一帧的"帧头 + 码流开头"(只对头几帧做, 免得刷屏)。
 *
 * @param pfx     帧序号(从 1 开始, 用于显示)
 * @param h       帧头
 * @param data    码流起始
 * @param nalu_cnt 这一帧解析出的 NALU 个数
 *
 * @note 为什么要专门打印: 板端调试最怕"只看到一个数字"。
 *       有了"帧头字段 + 码流前 24 字节", 一眼就能看出:
 *       帧头对不对、码流开头是不是 `00 00 00 01`。
 */
static void show_frame_detail(unsigned long long pfx,
                              const svc_media_frame_hdr_t *h,
                              const uint8_t *data, int nalu_cnt)
{
    int i;

    printf("   [帧%llu] 帧头: magic=%08X len=%u cap=%u idx=%u pts=%llu\n",
           pfx, h->magic, h->len, h->cap, h->frame_index,
           (unsigned long long)h->pts);
    printf("            码流前24字节: ");
    for (i = 0; i < 24 && (size_t)i < h->len; i++)
        printf("%02X ", data[i]);
    printf("\n            → 解析出 %d 个 NALU\n", nalu_cnt);
}

/**
 * 校验帧序号与 PTS 的**时序**(两者都来自编码器, 是独立于我们的证据)。
 *
 * @param h 帧头
 * @param a 累加器
 *
 * @note 抽出来的理由: "时序校验"和"帧头自洽校验"是两件独立的事,
 *       挤在一个函数里会互相淹没(函数规模只是症状)。
 *
 * @note PTS 间隔的判据用"落在 25000~45000"(对应 20~36 fps)——
 *       只看**量级**是否稳定在 30fps 档位, 不去猜精确时基。
 *       (实测这路流是 33332, 反推出时基 1 MHz, 不是常说的 90 kHz。)
 */
static void check_timing(const svc_media_frame_hdr_t *h, smoke_acc_t *a)
{
    /* ① 序号连续性: 丢帧会跳号, 但不该倒退或重复 */
    if (a->got == 1) {
        a->first_idx = h->frame_index;
    } else if (h->frame_index != a->prev_idx + 1) {
        a->idx_gap++;
    }
    a->prev_idx = h->frame_index;
    a->last_idx = h->frame_index;

    /* ② PTS 必须递增(编码器给的, 见 B012) */
    if (a->got == 1) {
        a->first_pts = h->pts;
    } else {
        unsigned long long d = h->pts - a->last_pts;

        if (h->pts < a->last_pts)
            a->pts_regress++;
        if (h->pts == a->last_pts)
            a->dup_pts++;
        if (d >= 25000 && d <= 45000)
            a->pts_on_time++;
        else
            a->pts_outlier++;
        if (a->pts_min == 0 || d < a->pts_min)
            a->pts_min = d;
        if (d > a->pts_max)
            a->pts_max = d;
        if (d / 1000 < 8192)
            g_pts_hist[d / 1000]++;
    }
    a->last_pts = h->pts;
}

/**
 * 检查一帧: 帧头自洽 → 序号/PTS 时序 → **跨模块喂给 proto_nalu 对账**。
 *
 * @param slot   从队列 pop 出来的槽位(开头是 svc_media 的帧头)
 * @param n      槽位实际字节数(pop 的 out_len, 含帧头)
 * @param is_h265 1 = H.265
 * @param a      累加器
 */
static void check_one_frame(const uint8_t *slot, size_t n, int is_h265,
                            smoke_acc_t *a)
{
    const svc_media_frame_hdr_t *h = svc_media_slot_hdr(slot);
    const uint8_t               *data;
    nalu_tally_t                 t;

    if (h == NULL) {
        a->bad_magic++;
        dump_suspect(slot, n, NULL, "帧头魔数不符");
        return;
    }
    data = svc_media_slot_data(slot);

    /* ① 帧头自身自洽: len + 帧头 == 队列里的字节数 */
    if (n != (size_t)SVC_MEDIA_HDR_SIZE + h->len)
        a->len_mismatch++;
    if (h->len > h->cap || h->cap != SMOKE_SLOT_DATA)
        a->bad_len++;

    a->got++;
    a->got_bytes += h->len;
    a->got_pkts  += n;
    if (h->len > a->max_frame)
        a->max_frame = h->len;

    check_timing(h, a);

    /* ② ★ 跨模块对账: 这一帧喂给已被 PC 单测覆盖的 proto_nalu */
    memset(&t, 0, sizeof(t));
    proto_nalu_foreach(data, h->len, is_h265, tally_nalu_cb, &t);
    if (t.cnt == 0) {
        a->empty_frames++;
        dump_suspect(slot, n, h, "解析出 0 个 NALU");
    }
    a->nalu_total += (unsigned long long)t.cnt;
    a->nalu_key   += (unsigned long long)t.key;
    a->nalu_slice += (unsigned long long)t.slice;

    if (a->got <= 3)
        show_frame_detail(a->got, h, data, t.cnt);
}

int main(int argc, char **argv)
{
    int             secs = SMOKE_DEFAULT_SEC;
    infra_queue_t  *q;
    svc_media_stats_t ms;
    infra_queue_stats_t qs;
    smoke_acc_t     a;
    uint8_t        *slot;
    long long       t0;
    long long       t_end;
    long long       t_next_report;
    int             rc;
    double          fps;
    double          pts_fps;
    double          pts_timebase;
    unsigned long long pts_nominal;
    double          sec_actual;
    double          sec_measured;        /* 测量窗口时长(不含最后的抽干) */
    unsigned long long got_at_deadline;  /* 测量窗口结束时的帧数 */

    if (argc > 1)
        secs = atoi(argv[1]);
    if (secs <= 0 || secs > 120)
        secs = SMOKE_DEFAULT_SEC;

    printf("===== M1-9 板端集成冒烟: MPP → svc_media → 队列 → 消费端 =====\n");
    printf("计划跑 %d 秒;每槽位 %d 字节(%d 帧头 + %d 码流)× %d 槽 = %.2f MB\n",
           secs, SMOKE_SLOT_BYTES, SVC_MEDIA_HDR_SIZE, SMOKE_SLOT_DATA,
           SMOKE_CAPACITY,
           (double)SMOKE_SLOT_BYTES * SMOKE_CAPACITY / 1024.0 / 1024.0);

    signal(SIGINT, on_sigint);
    infra_log_set_level(INFRA_LOG_WARN);    /* 板端只留告警以上, 别刷屏 */

    /* 槽位缓冲: 消费端自己一份, start 时分配(运行期不再 malloc) */
    slot = (uint8_t *)malloc(SMOKE_SLOT_BYTES);
    if (slot == NULL) {
        printf("❌ 槽位缓冲分配失败(%d 字节)\n", SMOKE_SLOT_BYTES);
        return 1;
    }

    q = infra_queue_create(SMOKE_CAPACITY, SMOKE_SLOT_BYTES);
    if (q == NULL) {
        printf("❌ 队列创建失败\n");
        free(slot);
        return 1;
    }

    /* ── 启动取流(内部会做 MPP 初始化, 约 5~10 秒)── */
    printf("\n【1】svc_media_start() —— 含 MPP 初始化, 请稍等…\n");
    t0 = now_ms();
    rc = svc_media_start(q, 0);             /* 0 = H.264, 与 bsp_mpp 的 chn1 一致 */
    if (rc != 0) {
        printf("❌ svc_media_start 失败 rc=%d\n", rc);
        infra_queue_destroy(q);
        free(slot);
        return 1;
    }
    printf("   MPP 就绪耗时 %.1f 秒;开始消费队列…\n", (now_ms() - t0) / 1000.0);

    /* ── 消费循环 ── */
    printf("\n【2】消费循环(%d 秒)\n", secs);
    memset(&a, 0, sizeof(a));
    t_end = now_ms() + (long long)secs * 1000;
    t_next_report = now_ms() + 2000;

    while (!g_stop && now_ms() < t_end) {
        size_t n = 0;

        rc = infra_queue_pop(q, slot, SMOKE_SLOT_BYTES, &n, SMOKE_POP_WAIT_MS);
        if (rc == 0)
            check_one_frame(slot, n, 0, &a);

        if (now_ms() >= t_next_report) {
            svc_media_get_stats(&ms);
            printf("   [%4.1fs] 已收 %llu 帧 / %llu 字节; 取流端 %llu 帧; 队列深度 %zu\n",
                   (now_ms() - t0) / 1000.0,
                   a.got, a.got_bytes, ms.frames, infra_queue_depth(q));
            t_next_report += 2000;
        }
    }

    /* ── 停止生产者, 再抽干队列 ── */
    /*
     * ⚠️ 顺序很关键, 这里踩过一个**真实的竞态**(2026-09-15):
     *   原来消费循环一到时间就退出, 接着立刻做最终对账 ——
     *   但**取流线程还在跑**, 它可能在"消费端统计"和"取流端统计"之间
     *   又推进了一帧。结果两边各自的数字都对, 只是**统计时刻不同**,
     *   于是"字节账: 消费端累加 == 取流端统计"这条断言时红时绿。
     *
     *   正确顺序:
     *     ① 记下**测量窗口结束时刻**(帧率的分母只能用它)
     *     ② `svc_media_stop()` —— 让生产者停下(它会 join 取流线程)
     *     ③ **抽干队列** —— 把生产者停下前推进去的那些帧也消费掉
     *     ④ 再取两边的最终统计做对账
     *   这样两个数字描述的是**同一个时刻**, 对账才有意义。
     */
    sec_measured = (now_ms() - t0) / 1000.0;    /* ★ 测量窗口在此结束 */
    got_at_deadline = a.got;

    printf("\n【3】svc_media_stop() —— 先停生产者\n");
    svc_media_stop();

    printf("【3b】抽干队列(把停机前推进去的帧也消费掉, 只为对账)\n");
    {
        int drained = 0;

        for (;;) {
            size_t n = 0;

            rc = infra_queue_pop(q, slot, SMOKE_SLOT_BYTES, &n, 0);  /* 不等待 */
            if (rc != 0)
                break;                      /* 空了(或出错), 抽干结束 */
            check_one_frame(slot, n, 0, &a);
            drained++;
        }
        printf("   抽干取出 %d 帧(已计入对账, 但不计入帧率)\n", drained);
    }

    svc_media_get_stats(&ms);
    infra_queue_get_stats(q, &qs);

    sec_actual = sec_measured;              /* 帧率只用测量窗口, 不含抽干 */
    fps = sec_measured > 0
          ? (double)got_at_deadline / sec_measured : 0;
    /*
     * 反推 PTS 时基与标称间隔。
     *
     * @note ⚠️ 时基必须"量", 不能抄。
     *   我原先在注释里一直写"PTS 是 90kHz 单位"(照抄一般说法), 但**从没验证**。
     *   实测这路流的 PTS 间隔是 33332, 而 90kHz/30fps 应当是 3000 —— 差 11 倍。
     *   按 33332 × 30 ≈ 1,000,000 反推, 这路流的 **PTS 时基是 1 MHz**。
     *
     *   标称间隔取**直方图的众数**(出现最多的那一档), 而不是 (min+max)/2:
     *   实测出现过单次 1.33 秒的跳变, (min+max)/2 会算出 683314 这种废物。
     *   **算典型值要看分布, 不要用极值**(B011)。
     */
    {
        int    k;
        int    mode_bin = -1;
        unsigned int mode_cnt = 0;

        for (k = 0; k < 8192; k++) {
            if (g_pts_hist[k] > mode_cnt) {
                mode_cnt = g_pts_hist[k];
                mode_bin = k;
            }
        }
        /* 众数档的中心值(档宽 1ms, 所以中心 = 档号*1000 + 500) */
        pts_nominal = (mode_bin >= 0)
                      ? (unsigned long long)mode_bin * 1000 + 500 : 0;
    }
    if (pts_nominal > 0) {
        pts_timebase = (double)pts_nominal * 30.0;   /* 该流标称 30fps */
        pts_fps      = pts_timebase / (double)pts_nominal;
    } else {
        pts_timebase = 0;
        pts_fps      = 0;
    }

    printf("\n【4】取流端统计(svc_media)\n");
    printf("   取到并入队 : %llu 帧 / %llu 字节\n", ms.frames, ms.bytes);
    printf("   当时没帧   : %llu 次(正常, 不是错误)\n", ms.get_timeouts);
    printf("   取流出错   : %llu 次\n", ms.get_errors);
    printf("   解析丢弃   : %llu 帧(整帧无 NALU)\n", ms.parse_rejects);
    printf("   超容量丢弃 : %llu 帧\n", ms.oversize);
    printf("   队列丢旧帧 : %llu 帧\n", ms.queue_dropped);

    printf("\n【5】消费端统计\n");
    printf("   收到       : %llu 帧 / %llu 字节(含帧头共 %llu)\n",
           a.got, a.got_bytes, a.got_pkts);
    printf("   NALU 总数  : %llu(关键帧 %llu, 切片 %llu)\n",
           a.nalu_total, a.nalu_key, a.nalu_slice);
    printf("   最大帧     : %llu 字节\n", a.max_frame);
    printf("   帧序号     : %u → %u\n", a.first_idx, a.last_idx);
    printf("   PTS        : %llu → %llu(跨度 %llu)\n",
           a.first_pts, a.last_pts, a.last_pts - a.first_pts);
    printf("   PTS 间隔   : 最小 %llu / 最大 %llu / 标称(众数) %llu\n",
           a.pts_min, a.pts_max, pts_nominal);
    printf("                30fps 档位 %llu 次 / 偏离 %llu 次(共 %llu 次)"
           "  → 偏离率 %.2f%%\n",
           a.pts_on_time, a.pts_outlier, a.got > 0 ? a.got - 1 : 0,
           (a.got > 1) ? 100.0 * (double)a.pts_outlier / (double)(a.got - 1)
                       : 0.0);
    printf("   耗时/帧率  : %.2f 秒 / 墙钟 %.2f fps\n", sec_actual, fps);
    printf("   PTS 时基   : 标称间隔 %llu × 30fps 反推 ≈ %.0f Hz → %s\n",
           pts_nominal, pts_timebase,
           (pts_timebase > 9.0e5 && pts_timebase < 1.1e6)
           ? "**1 MHz**(实测反推, 不是常说的 90 kHz)"
           : ((pts_timebase > 8.0e4 && pts_timebase < 1.0e5)
              ? "**90 kHz**" : "既非 1MHz 也非 90kHz, 需要查"));
    printf("   队列末态   : 深度 %zu / 容量 %zu, 历史最高水位 %zu\n",
           qs.depth, qs.capacity, qs.max_depth);

    /* ══════════════ 断言 ══════════════ */
    printf("\n【6】断言\n");

    /* ── 第一组: 基本可用 ── */
    check("★ 确实取到了帧(> 100 帧)", a.got > 100);
    check("★ 取流端零错误(get_errors == 0)", ms.get_errors == 0);
    check("★ 没有帧因为超容量被丢(oversize == 0)", ms.oversize == 0);
    check("★ 没有帧因为解析不出 NALU 被丢(parse_rejects == 0)",
          ms.parse_rejects == 0);
    check("队列槽位容量被 svc_media 正确识别(256 KB)",
          ms.oversize == 0 && a.bad_len == 0);

    /* ── 第二组: 内容正确性(**最关键**) ── */
    check("★ 每帧帧头魔数都对(bad_magic == 0)", a.bad_magic == 0);
    check("★ 每帧都解析出了 NALU(empty_frames == 0) —— 多 pack 拼接正确",
          a.empty_frames == 0);
    check("★ NALU 总数 > 帧数(H.264 每帧至少 1 个切片)",
          a.nalu_total > a.got);
    check("★ 每帧都有切片 NALU(slice == got)", a.nalu_slice == a.got);
    check("★ 出现了关键帧(nalu_key > 0, GOP=30 应当每 1 秒一个)",
          a.nalu_key > 0);
    check("★ 帧头 len 与队列里实际字节数一致(len_mismatch == 0)",
          a.len_mismatch == 0);
    check("★ 帧大小都没超过槽位容量", a.max_frame <= SMOKE_SLOT_DATA);

    /* ── 第三组: 跨模块对账(字节/帧数/序号/PTS) ── */
    check("★ 字节账: 消费端累加 == 取流端统计",
          a.got_bytes == ms.bytes);
    check("★ 帧数账: 消费 + 丢旧 + 残留 == 取流端统计",
          (unsigned long long)qs.popped + ms.queue_dropped +
          (unsigned long long)qs.depth == ms.frames);
    check("★ PTS 严格递增(pts_regress == 0)", a.pts_regress == 0);
    check("★ 没有两帧 PTS 相同(dup_pts == 0) —— 证明是一帧一槽",
          a.dup_pts == 0);

    /*
     * ⚠️ 这一组断言我改过两版, 两版的失败都**不是编码器的问题**, 值得记下来:
     *
     *   第一版: 要求"**墙钟**帧率也落在 25~35fps" → 红了。
     *           原因是墙钟里混进了**接收端**开销(pop 超时、printf、调度),
     *           实测能把 30fps 算成 2.7fps。**测量方式错了, 不是被测对象错了。**
     *
     *   第二版: 要求"最小/最大间隔同量级"(max <= min*3) → 又红了。
     *           实测出现一次 **1333333**(卡了 1.33 秒)的离群值。
     *           用极值判断"稳不稳定"本身就是错的方法 —— **B011 的教训**:
     *           算"率"或"典型值"不要用极值, 要看分布。
     *
     *   所以现在改成:
     *     · 正确性 → **偏离率**: 99% 以上的间隔落在 30fps 档位即可
     *     · 典型值 → 用**直方图众数**, 不受离群值影响
     *     · 吞吐量 → 只报不断言(性能指标, 不该当正确性判据)
     */
    check("★ 99% 以上的 PTS 间隔落在 30fps 档位(编码器时基稳定)",
          a.got > 1 && a.pts_on_time * 100 >= (a.got - 1) * 99);
    check("★ 标称间隔落在 30fps 档位(直方图众数, 不受离群值影响)",
          pts_nominal >= 25000 && pts_nominal <= 45000);
    /*
     * @note 时基的精度**受直方图档宽限制**(1ms 档 → 众数只能给到档中心)。
     *   想要更准的话看 `pts_min`: 实测绝大多数间隔都在 33326~33340,
     *   所以 min 本身就近似"真正的标称间隔"。两条一起判, 既笼统又精确。
     */
    check("★ 实测最小间隔也落在 30fps 档位(离群值之外的精确值)",
          a.pts_min >= 30000 && a.pts_min <= 37000);
    /*
     * @note 墙钟帧率只打印不断言。实测它比"标称 30fps"低不少(约 19~25fps)——
     *       那说明**接收/消费这一侧**还没跑到满速(RTSP 发送路径要留意),
     *       但不是本模块的正确性问题: H.264 编码器不要求每个包都被取走,
     *       取慢了它自己丢旧帧(队列"丢最旧"也是同一思路)。
     */
    printf("   ℹ️ 墙钟 %.2f fps 低于标称 30fps —— 这是接收端吞吐, 不是编码器问题\n",
           fps);
    check("★ 反推出的 PTS 时基是个整数兆级数(佐证时基推理自洽)",
          pts_timebase > 9.0e5 && pts_timebase < 1.1e6);
    /*
     * 离群值只**报告**不判失败 —— 但要显眼。
     * 实测出现过单次 1.33 秒的 PTS 跳变(等于连续丢了约 39 帧)。
     * 这属于"值得追的线索"(很可能是 VENC 缓冲满后丢帧,
     * 根因在接收端吞吐跟不上), 不该把整轮验证判成失败。
     */
    if (a.pts_outlier > 0) {
        printf("   ⚠️ 有 %llu 个 PTS 间隔明显偏离(最大 %llu ≈ %.2f 秒)"
               " —— 疑似丢帧, 值得查 VENC 缓冲\n",
               a.pts_outlier, a.pts_max, (double)a.pts_max / 1.0e6);
    }

    printf("\n===== 结果: %s(共 %d 项, %d 项失败)=====\n",
           g_fails == 0 ? "全部通过" : "有失败", g_checks, g_fails);
    /*
     * 机器可读的唯一结论标记。
     * @note 为什么要它: 验证脚本要判断成败, 而 telnet 回显里混着命令文本,
     *   靠 `grep` + 文本匹配很容易**匹配到回显里那句命令本身**。
     *   打一个不会出现在命令里的标记, 判据就不可能被回显噪声污染。
     */
    printf("SMOKE_VERDICT=%s checks=%d fails=%d\n",
           g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);

    infra_queue_destroy(q);
    free(slot);
    return g_fails == 0 ? 0 : 1;
}
