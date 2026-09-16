/**
 * @file    svc_sender.c
 * @brief   RTP 发送服务实现 —— 见 svc_sender.h
 *
 * 【模块职责】发送线程: 出队 → 切 NALU → 打 RTP 包 → 发给每个正在看的客户端
 * 【依赖方向】依赖 infra_queue、proto_nalu、proto_rtp、infra_netio、svc_media(槽位布局的唯一出处)
 * 【线程模型】自己起 **1 个**线程(sender_thread, 线程名 `ipc_send`); 每客户端一份独立 RTP 会话
 * 【资源边界】槽位缓冲在 start 时一次分配; 客户端槽位固定 8 个(引用计数保护移除竞态)
 *
 * 结构:
 *      ① 模块状态与客户端槽位
 *      ② 并发策略(本文件最需要看懂的一节)
 *      ③ 帧的扫描与发送
 *      ④ 发送线程
 *      ⑤ 对接 svc_net 的两个入口(add / remove)
 *      ⑥ 生命周期与统计
 */
#include "svc_sender.h"

#include "infra_log.h"
#include "infra_netio.h"
#include "infra_queue.h"
#include "proto_nalu.h"
#include "proto_rtp.h"
#include "svc_media.h"          /* ★ 槽位布局(帧头魔数/大小/字段偏移)的唯一出处 */

#include <pthread.h>
#include <stddef.h>     /* offsetof —— 读帧头字段偏移 */
#include <stdio.h>      /* fprintf —— 临时诊断用(SVC_SENDER_DEBUG) */
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>      /* prctl(PR_SET_NAME) —— 给线程起名, ps/top 能看出来 */
#include <time.h>
#include <unistd.h>

/** 队列空时最多等多久(毫秒)。小一点, 免得停止时要等很久 */
#define SVC_SENDER_POP_WAIT_MS 50

/** 打包用的栈缓冲大小 = 帧头 + 单帧最大字节(与 svc_media 的槽位一致) */
#define SVC_SENDER_SLOT_BYTES (256 * 1024 + 1024)

/* ═══════════════ ① 模块状态与客户端槽位 ═══════════════ */

/**
 * 一个正在播放的客户端。
 *
 * @note `rtp` 必须**每客户端一份** —— 序列号/时间戳/SSRC 不能共用,
 *       否则两个客户端各自的丢包判断会互相污染。
 */
typedef struct {
    int                 used;           /* 0 = 空闲 */
    int                 client_index;   /* svc_net 的槽位下标(唯一标识) */
    struct sockaddr_in  dst;            /* RTP 目的地(IP + 端口) */
    infra_sender_t     *sender;         /* ADR-1 的发送抽象(UDP 实现) */
    proto_rtp_session_t rtp;            /* 本客户端独立的 RTP 会话状态 */
    int                 waiting_idr;    /* 1 = 还没遇到关键帧, 先跳过 */
    uint64_t            frames_sent;    /* 本客户端已发帧数(诊断用) */

    /**
     * 正在被发送线程使用的引用计数。
     *
     * @note 存在的唯一理由: 解决"快照之后、发送之前客户端被移除"的竞态。
     *       发送线程取快照时 +1, 用完 -1;
     *       `remove_client()` 若发现 `in_use > 0`, 就**不立即销毁**,
     *       而是置 `pending_free = 1`, 等发送线程把引用放下再销毁。
     *       这就是最朴素的引用计数, 也是本文件最容易写错的地方。
     */
    int                 in_use;
    int                 pending_free;
} sender_client_t;

static struct {
    int                 running;
    infra_queue_t      *queue;          /* 不拥有: 由调用方创建/销毁 */
    int                 is_h265;
    int                 sockfd;         /* 一个 UDP socket 供所有客户端共用 */
    pthread_t           thread;
    int                 thread_valid;
    volatile int        stop_requested;

    sender_client_t     clients[SVC_SENDER_MAX_CLIENTS];

    /** 保护 `clients[]` 的增删与引用计数的锁。**不进 sendto 的临界区** */
    pthread_mutex_t     lock;

    /** 发送线程自己的槽位缓冲(一次性分配, 运行期复用) */
    uint8_t            *slot;

    svc_sender_stats_t  stats;
} g;

/*
 * ═══════════════ ② 并发策略(本文件最需要看懂的一处) ═══════════════
 *
 *  两个线程会碰 `g.clients[]`:
 *      · **事件循环线程**(svc_net): add_client / remove_client
 *      · **发送线程**(本模块): 每帧遍历客户端去发
 *
 *  三种做法, 我们选第三种:
 *      ❌ **发送全程持锁**: sendto 是系统调用, 持锁会让事件循环在
 *         add/remove 时被卡住 —— 而事件循环还管着所有客户端的 RTSP 请求,
 *         卡它等于卡了整个控制面。
 *      ❌ **完全无锁**: 槽位字段多, "读到一半被改"要一个个处理,
 *         复杂度远超收益, 而且难验证。
 *      ✅ **快照 + 引用计数(本做法)**:
 *         ① 锁内: 把 `used` 的槽位 `in_use++`, 把指针收进栈上数组, 放锁
 *         ② 锁外: 真正 sendto(不在临界区里)
 *         ③ 锁内: 收尾, `in_use--`; 若该槽位 `pending_free` 且 `in_use==0`
 *            → 这时才真正销毁
 *      `remove_client()` 若看到 `in_use > 0`, 只打 `pending_free` 标记,
 *      **不销毁** —— 于是发送线程手里的指针永远有效。
 *
 *  代价: 每帧一次小拷贝(8 个指针) + 两次轻量加锁。
 *  收益: sendto 绝不在临界区里; 且**不持有悬空指针**。
 */

/** @brief 释放一个槽位(调用方必须已持锁, 且确认 in_use == 0) */
static void client_slot_free_locked(sender_client_t *c)
{
    if (c->sender != NULL)
        c->sender->destroy(&c->sender);
    c->used         = 0;
    c->pending_free = 0;
    c->in_use       = 0;
    g.stats.clients_left++;
}

/** @brief 放下引用; 若该槽位正等着被释放且引用已归零, 就地释放 */
static void client_release_locked(sender_client_t *c)
{
    if (c->in_use > 0)
        c->in_use--;
    if (c->pending_free && c->in_use == 0)
        client_slot_free_locked(c);
}

/* ═══════════════ ③ 帧的扫描与发送 ═══════════════ */

/** 扫描一帧得到的元信息 */
typedef struct {
    int nalu_count;
    int has_idr;
} frame_info_t;

/**
 * @brief 遍历回调: 把一个 NALU 打包发给**所有**正在看的客户端
 *
 * @param n 当前 NALU
 * @param user 本次发送的上下文
 * @return 0 = 继续遍历; 非 0 = 要求提前结束
 */
static int scan_nalu_cb(const proto_nalu_t *n, void *user)
{
    frame_info_t *fi = (frame_info_t *)user;

    fi->nalu_count++;
    if (n->is_key)
        fi->has_idr = 1;
    return 0;
}

/**
 * 发一个 NALU 的上下文。
 *
 * @note ⚠️ 这里暴露一处**接口不匹配**, 值得说清:
 *       `proto_rtp_send_nalu()` 收的是 `(sockfd, sockaddr*, dstlen)`,
 *       而架构(ADR-1)要求走 `infra_sender_t` 抽象。
 *       本模块因此**只能用裸 socket**, 拿不到抽象带来的好处
 *       (比如单测注入"计数 sender")。
 *
 *       **本次不改 proto_rtp 的签名** —— 它已被 rtp_test 验到逐字节一致
 *       (H.264 809 / H.265 835), 改签名要动那个验证基线, 风险不值当。
 *       将来若要加 TCP interleaved, 正确做法是**给 proto_rtp 加一个收
 *       `infra_sender_t *` 的新函数**, 而不是改老的 ——
 *       与 `proto_rtp_session_frame_pts` 用同一个策略: **加新的, 不动老的**。
 */
typedef struct {
    sender_client_t     *cli;
    proto_rtp_session_t *rtp;
    int                  npkt;          /* 累计发出的包数 */
    int                  failed;
    /**
     * 上一个 NALU(**存副本, 不存指针**)。
     *
     * @note ⚠️ 这里踩过一个 ASan 才能抓到的 bug, 值得完整记下(2026-09-15):
     *
     *   第一版我写的是 `const proto_nalu_t *pending;` —— 存**指针**。
     *   那个指针指向的是 `proto_nalu_foreach` 里**栈上的 `n`**
     *   (`emit_nalu()` 里的 `proto_nalu_t n;`)。
     *
     *   普通 `-O2` 构建下:那个栈槽在循环里被复用, 生命周期"恰好"覆盖到
     *   我们用它的时候 → **测试 37 项全过, 看不出问题**。
     *   ASan 的 `-O1` 构建下:栈对象作用域被如实跟踪, 跨迭代使用就成了
     *   **失效指针** → `memcpy` 读到非法地址, SEGV。
     *
     *   这正是"**测试全绿但代码是错的**"的典型 —— 而且是**只有 sanitizer
     *   才抓得到**的那一类。教训: **回调里拿到的指针, 要跨回调使用就必须
     *   拷一份**; 编译器的内联会让悬垂指针"看起来能用"。
     */
    proto_nalu_t         pending;
    int                  has_pending;
    int                  total;         /* 本帧 NALU 总数 */
} fanout_ctx_t;

/** @brief 真正发一个 NALU(带 is_last 判断) */
static void emit(fanout_ctx_t *fc, const proto_nalu_t *n, int is_last)
{
    int npkt;

    if (fc->failed)
        return;

    npkt = proto_rtp_send_nalu(g.sockfd,
                               (const struct sockaddr *)&fc->cli->dst,
                               sizeof(fc->cli->dst),
                               fc->rtp, n, is_last);
    if (npkt < 0) {
        fc->failed = 1;
        return;
    }
    fc->npkt += npkt;
}

/**
 * @brief 遍历回调 —— 用"滞后一个 NALU"的办法解决 `is_last`。
 *
 * @note 为什么需要这个技巧: `is_last`(RTP marker 位)要求知道
 *       **当前 NALU 是不是本帧最后一个**, 但遍历时并不知道后面还有没有。
 *       两个办法:
 *         · 先数一遍总数(多一次遍历) —— 简单但多扫一遍
 *         · **滞后一个**: 手里攥着上一个, 等下一个来了才知道"上一个不是最后",
 *           遍历结束时攥在手里的那个就是最后 —— 只遍历一遍
 *       这里用后者, 因为帧短(最多几个 NALU), 但省一次全帧扫描更干净。
 */
static int fanout_nalu_cb(const proto_nalu_t *n, void *user)
{
    fanout_ctx_t *fc = (fanout_ctx_t *)user;

    if (fc->has_pending) {
        /* 上一个确定不是最后一个 */
        emit(fc, &fc->pending, 0);
    }
    fc->pending     = *n;       /* ★ 拷贝一份 —— 绝不能存 n 这个指针 */
    fc->has_pending = 1;
    return 0;
}

/**
 * @brief 把一帧发给一个客户端。
 *
 * @return 0 成功; -1 发送出错; 1 = 本帧被跳过(还在等 IDR)
 */
static int send_frame_to_one(sender_client_t *cli, const uint8_t *data,
                             size_t len, uint64_t pts, const frame_info_t *fi)
{
    fanout_ctx_t fc;

    /* ★ 等 IDR 期间跳过非关键帧 —— 中途加入的客户端没有参考帧会花屏 */
    if (cli->waiting_idr && !fi->has_idr)
        return 1;

    if (cli->waiting_idr) {
        cli->waiting_idr = 0;
        LOG_INFO("client%d 从关键帧开始发送", cli->client_index);
    }

    /* ★ 时间戳用编码器 PTS 推进, 一帧一次(本帧所有 NALU 共享同一时间戳) */
    proto_rtp_session_frame_pts(&cli->rtp, pts);

    memset(&fc, 0, sizeof(fc));
    fc.cli   = cli;
    fc.rtp   = &cli->rtp;
    fc.total = fi->nalu_count;

    (void)proto_nalu_foreach(data, len, g.is_h265, fanout_nalu_cb, &fc);

    /* 遍历结束后手里攥着的那个, 就是本帧最后一个 → marker 位 */
    if (fc.has_pending)
        emit(&fc, &fc.pending, 1);

    if (fc.failed) {
        g.stats.send_errors++;
        return -1;
    }

    cli->frames_sent++;
    g.stats.frames_sent++;
    g.stats.packets_sent += (uint64_t)fc.npkt;
    return 0;
}

/* ═══════════════ ④ 发送线程 ═══════════════ */

/**
 * @brief 把一帧分发给**当前所有正在播放的客户端**。
 *
 * @note 按"② 并发策略"说的三步走: 快照 → 锁外发送 → 放引用。
 */
static void fanout_frame(const uint8_t *data, size_t len, uint64_t pts)
{
    sender_client_t *snap[SVC_SENDER_MAX_CLIENTS];
    frame_info_t     fi;
    int              n = 0;
    int              i;

    /* ① 锁内: 扫一遍帧信息 + 取快照 + 加引用 */
    memset(&fi, 0, sizeof(fi));
    (void)proto_nalu_foreach(data, len, g.is_h265, scan_nalu_cb, &fi);
    if (fi.nalu_count == 0)
        return;                     /* 一帧里没 NALU(不该发生) → 丢弃 */

    pthread_mutex_lock(&g.lock);
    for (i = 0; i < SVC_SENDER_MAX_CLIENTS; i++) {
        if (g.clients[i].used && !g.clients[i].pending_free) {
            g.clients[i].in_use++;
            snap[n++] = &g.clients[i];
        }
    }
    pthread_mutex_unlock(&g.lock);

    /* ② 锁外: 真正发送(sendto 不在临界区里) */
    for (i = 0; i < n; i++)
        (void)send_frame_to_one(snap[i], data, len, pts, &fi);

    /* ③ 锁内: 放引用(可能需要就地释放) */
    pthread_mutex_lock(&g.lock);
    for (i = 0; i < n; i++)
        client_release_locked(snap[i]);
    pthread_mutex_unlock(&g.lock);
}

/**
 * @brief 记一帧的「帧龄」= `CLOCK_MONOTONIC(现在) − u64PTS(采集时刻)`。
 *
 * @param pts 该帧的编码器时间戳(1 MHz 单调时钟, 由 MPP 给)
 *
 * @note ★ 实测:这个差**不是**绝对延迟, 它带一个约 **−115 ms 的固定偏置**
 *       (PTS 比单调时钟超前)。首帧会把原始值打出来, 便于核对。
 *       两个时钟**同源**(判据:偏置稳定;真不同源会是随机巨量),
 *       所以**相对量才有意义**: 抖动 = max−min, 漂移 = last−first。
 *
 * @note ⚠️ 我第一版把"差为负"直接判成"不同源、统计不可信" —— **判断错了**:
 *       一个稳定的负偏移恰恰是"同源 + 固定偏置"的证据。
 *       教训: 校验规则要能区分"**常量偏置**"和"**不同源**", 前者可抵消、后者不可。
 */
static void note_latency(uint64_t pts)
{
    struct timespec ts;
    int64_t         age;

    if (pts == 0 || clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return;
    age = (int64_t)((uint64_t)ts.tv_sec * 1000000ULL +
                    (uint64_t)ts.tv_nsec / 1000ULL) - (int64_t)pts;

    if (g.stats.age_samples == 0) {
        g.stats.age_first_us = age;
        g.stats.age_min_us   = age;
        g.stats.age_max_us   = age;
        LOG_INFO("时钟核对: 首帧 pts=%llu us, 帧龄=%lld us"
                 "(固定偏置, 看相对变化)",
                 (unsigned long long)pts, (long long)age);
    }
    g.stats.age_last_us = age;
    if (age < g.stats.age_min_us)
        g.stats.age_min_us = age;
    if (age > g.stats.age_max_us)
        g.stats.age_max_us = age;
    g.stats.age_samples++;
}

/**
 * @brief 解析发送槽位里的一帧, 并分发给所有客户端。
 *
 * @param len 槽位里的有效字节数(由 `infra_queue_pop` 给出)
 * @return 0 = 已分发; -1 = 槽位内容非法(已告警, 调用方继续下一轮)
 *
 * @note ⚠️ 这里**只读原始字节、自己解帧头**, 而**不用** svc_media 的读取函数 ——
 *       那些函数(`svc_media_slot_hdr` / `svc_media_slot_data`)是按"消费者持有
 *       队列指针"设计的, 而本模块用的是自己的槽位缓冲拷贝, 硬套会绕。
 *       但**布局知识必须只有一个出处**: 魔数与帧头偏移都取自 `svc_media.h`,
 *       不在这里重写数字(B024 的教训: 两处各算一遍大小 → 一处改了一处没改)。
 *
 * @note 抽成独立函数的理由:`sender_thread` 已经顶到 50 行硬上限,
 *       而"读一帧并分发"本来就是自成一件事(与"循环、退出"无关)。
 */
static int dispatch_one_slot(size_t len)
{
    const uint8_t *p = (const uint8_t *)g.slot;
    uint32_t       magic, flen;
    uint64_t       pts = 0;
    size_t         off;
    int            k;

    if (len < (size_t)SVC_MEDIA_HDR_SIZE) {
        LOG_WARN("槽位过短(%zu 字节), 丢弃", len);
        return -1;
    }

    /* 按帧头字段偏移读。偏移用 offsetof 算, 不写魔法数字(见 B024) */
    memcpy(&magic, p + offsetof(svc_media_frame_hdr_t, magic), 4);
    memcpy(&flen,  p + offsetof(svc_media_frame_hdr_t, len),   4);
    for (k = 0; k < 8; k++) {
        pts |= (uint64_t)p[offsetof(svc_media_frame_hdr_t, pts) + k] << (8 * k);
    }

    if (magic != SVC_MEDIA_FRAME_MAGIC) {
        LOG_WARN("槽位魔数不符(%08X, 期望 %08X), 丢弃",
                 magic, SVC_MEDIA_FRAME_MAGIC);
        return -1;
    }
    off = (size_t)SVC_MEDIA_HDR_SIZE;
    if (flen == 0 || off + (size_t)flen > len) {
        LOG_WARN("槽位长度字段异常(%u, 槽位 %zu), 丢弃", flen, len);
        return -1;
    }
    fanout_frame(p + off, flen, pts);
    note_latency(pts);
    return 0;
}

/**
 * @brief 发送线程主循环: 出队一帧 → 切 NALU → 分别发给每个客户端
 *
 * @param arg 未使用
 * @return 永远返回 NULL
 * @note 执行线程: 本模块自己起的线程(线程名 `ipc_send`)。
 */
static void *sender_thread(void *arg)
{
    (void)arg;
    /* §7.1: 线程名让 `ps` / `top` 一眼看出这是谁, 不用靠 pid 猜 */
    (void)prctl(PR_SET_NAME, "ipc_send", 0, 0, 0);
    LOG_INFO("发送线程启动(H.26%d, 客户端上限 %d)",
             g.is_h265 ? 5 : 4, SVC_SENDER_MAX_CLIENTS);

    while (!g.stop_requested) {
        size_t len = 0;
        int    rc  = infra_queue_pop(g.queue, g.slot, SVC_SENDER_SLOT_BYTES,
                                     &len, SVC_SENDER_POP_WAIT_MS);

        if (rc == 1)
            continue;               /* 队列空(超时), 正常 */

        if (rc != 0) {
            LOG_ERROR("取帧失败 rc=%d", rc);
            continue;
        }
        (void)dispatch_one_slot(len);
    }

    LOG_INFO("发送线程退出(帧 %llu / 包 %llu / 字节 %llu)",
             (unsigned long long)g.stats.frames_sent,
             (unsigned long long)g.stats.packets_sent,
             (unsigned long long)g.stats.bytes_sent);
    return NULL;
}

/* ═══════════════ ⑤ 对接 svc_net 的两个入口 ═══════════════ */

int svc_sender_add_client(int client_index, const struct sockaddr_in *rtp_dst)
{
    int i;

    if (rtp_dst == NULL)
        return -1;
    if (!g.running)
        return -1;

    pthread_mutex_lock(&g.lock);

    /* 已存在(同一槽位重复 PLAY)→ 更新目的地, 不新占槽位、不重置会话 */
    for (i = 0; i < SVC_SENDER_MAX_CLIENTS; i++) {
        if (g.clients[i].used && g.clients[i].client_index == client_index) {
            g.clients[i].dst = *rtp_dst;
            if (g.clients[i].sender != NULL) {
                infra_sender_t *old = g.clients[i].sender;

                old->destroy(&old);
            }
            g.clients[i].sender = infra_sender_udp(rtp_dst, g.sockfd);
            pthread_mutex_unlock(&g.lock);
            LOG_INFO("client%d 更新 RTP 目的地", client_index);
            return 0;
        }
    }

    for (i = 0; i < SVC_SENDER_MAX_CLIENTS; i++) {
        if (!g.clients[i].used)
            break;
    }
    if (i == SVC_SENDER_MAX_CLIENTS) {
        pthread_mutex_unlock(&g.lock);
        LOG_WARN("发送端客户端已满(%d), 拒绝 client%d",
                 SVC_SENDER_MAX_CLIENTS, client_index);
        return -2;
    }

    memset(&g.clients[i], 0, sizeof(g.clients[i]));
    g.clients[i].used         = 1;
    g.clients[i].client_index = client_index;
    g.clients[i].dst          = *rtp_dst;
    g.clients[i].waiting_idr  = 1;  /* ★ 从 IDR 开始 —— 见 svc_sender.h 设计决定② */
    /* 两个客户端的 RTP 初值必须不同, 否则序列号/SSRC 会撞 */
    proto_rtp_session_init(&g.clients[i].rtp, g.is_h265,
                           (uint32_t)time(NULL) ^ ((uint32_t)client_index * 2654435761u),
                           30);
    g.clients[i].sender = infra_sender_udp(rtp_dst, g.sockfd);

    g.stats.clients_joined++;
    pthread_mutex_unlock(&g.lock);

    LOG_INFO("client%d 加入发送(RTP 端口 %u, 等 IDR)",
             client_index, (unsigned)ntohs(rtp_dst->sin_port));
    return 0;
}

void svc_sender_remove_client(int client_index)
{
    int i;

    if (!g.running)
        return;

    pthread_mutex_lock(&g.lock);
    for (i = 0; i < SVC_SENDER_MAX_CLIENTS; i++) {
        if (g.clients[i].used && g.clients[i].client_index == client_index) {
            if (g.clients[i].in_use > 0) {
                /*
                 * 发送线程正拿着它 —— **不能现在销毁**, 否则是野指针。
                 * 打个标记, 等发送线程放下引用时由 client_release_locked() 释放。
                 */
                g.clients[i].pending_free = 1;
            } else {
                client_slot_free_locked(&g.clients[i]);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g.lock);
}

/* ═══════════════ ⑥ 生命周期与统计 ═══════════════ */

int svc_sender_start(void *queue, int is_h265)
{
    if (g.running)
        return 0;
    if (queue == NULL)
        return -1;

    memset(&g, 0, sizeof(g));
    g.queue   = (infra_queue_t *)queue;
    g.is_h265 = is_h265 ? 1 : 0;
    g.sockfd  = -1;

    if (pthread_mutex_init(&g.lock, NULL) != 0)
        return -2;

    g.slot = (uint8_t *)malloc(SVC_SENDER_SLOT_BYTES);
    if (g.slot == NULL) {
        pthread_mutex_destroy(&g.lock);
        return -2;
    }

    g.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g.sockfd < 0) {
        free(g.slot);
        g.slot = NULL;
        pthread_mutex_destroy(&g.lock);
        return -2;
    }

    g.stop_requested = 0;
    g.running        = 1;
    if (pthread_create(&g.thread, NULL, sender_thread, NULL) != 0) {
        close(g.sockfd);
        free(g.slot);
        g.slot    = NULL;
        g.running = 0;
        pthread_mutex_destroy(&g.lock);
        return -3;
    }
    g.thread_valid = 1;
    LOG_INFO("发送服务已启动");
    return 0;
}

void svc_sender_stop(void)
{
    if (!g.running)
        return;

    g.stop_requested = 1;
    if (g.thread_valid) {
        pthread_join(g.thread, NULL);   /* 等线程真退出, 否则下面 free 会 UAF */
        g.thread_valid = 0;
    }
    g.running = 0;

    pthread_mutex_lock(&g.lock);
    {
        int i;

        for (i = 0; i < SVC_SENDER_MAX_CLIENTS; i++) {
            if (g.clients[i].used && g.clients[i].sender != NULL)
                g.clients[i].sender->destroy(&g.clients[i].sender);
            g.clients[i].used = 0;
        }
    }
    pthread_mutex_unlock(&g.lock);
    pthread_mutex_destroy(&g.lock);

    if (g.sockfd >= 0) {
        close(g.sockfd);
        g.sockfd = -1;
    }
    free(g.slot);
    g.slot = NULL;

    LOG_INFO("发送服务已停止(帧 %llu / 包 %llu / 字节 %llu / 错误 %llu)",
             (unsigned long long)g.stats.frames_sent,
             (unsigned long long)g.stats.packets_sent,
             (unsigned long long)g.stats.bytes_sent,
             (unsigned long long)g.stats.send_errors);
}

int svc_sender_is_running(void)
{
    return g.running;
}

void svc_sender_get_stats(svc_sender_stats_t *out)
{
    int i;

    if (out == NULL)
        return;
    pthread_mutex_lock(&g.lock);
    *out = g.stats;
    out->waiting_idr = 0;
    for (i = 0; i < SVC_SENDER_MAX_CLIENTS; i++) {
        if (g.clients[i].used && g.clients[i].waiting_idr)
            out->waiting_idr++;
    }
    pthread_mutex_unlock(&g.lock);
}

int svc_sender_client_count(void)
{
    int i, n = 0;

    pthread_mutex_lock(&g.lock);
    for (i = 0; i < SVC_SENDER_MAX_CLIENTS; i++) {
        if (g.clients[i].used)
            n++;
    }
    pthread_mutex_unlock(&g.lock);
    return n;
}
