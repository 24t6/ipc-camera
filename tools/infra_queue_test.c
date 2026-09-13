/**
 * @file    infra_queue_test.c
 * @brief   infra_queue 单元测试 —— 功能 + **线程安全** + 丢最旧语义
 *
 * 【怎么验证】(可证伪)
 *   ① 功能:FIFO 顺序、队满丢最旧、超长拒绝、超时语义
 *   ② 统计:pushed/dropped/popped/rejected 四个计数必须自洽
 *   ③ ★ 线程安全:真开线程压 ——
 *        场景 A:生产者稍慢(不丢帧) → 验证 pushed == popped, 且取出的序号严格递增
 *        场景 B:生产者远快于消费者(必丢帧) → 验证 pushed == popped + dropped
 *        场景 C:消费者阻塞在 pop 上, 生产者 100ms 后才发 → 验证真的被唤醒
 *      这些正是"队列存在的理由", 不压就等于没验。
 *
 * 编译(在 Ubuntu VM 上, 需要 pthread):
 *   gcc -Wall -Wextra -O2 -pthread -o infra_queue_test infra_queue_test.c \
 *       ../src/infra/infra_queue.c -I../src/infra
 *
 * 用法: ./infra_queue_test
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "infra_queue.h"

static int g_fails;

static int check(const char *name, int cond)
{
    printf("   %-56s %s\n", name, cond ? "[通过]" : "[失败] <<<");
    if (!cond)
        g_fails++;
    return cond;
}

/** 每次测试都用同一个"帧"格式: 前 8 字节是序号, 便于验证顺序 */
#define FRAME_SLOT 64

static void make_frame(uint8_t *buf, uint64_t seq)
{
    memcpy(buf, &seq, sizeof(seq));
    memset(buf + sizeof(seq), (int)(seq & 0xFF), FRAME_SLOT - sizeof(seq));
}

static uint64_t frame_seq(const uint8_t *buf)
{
    uint64_t seq;
    memcpy(&seq, buf, sizeof(seq));
    return seq;
}

/* ─────────────── 线程压测用的上下文 ─────────────── */

typedef struct {
    infra_queue_t *q;
    uint64_t       n;          /* 生产/消费多少个 */
    int            us_delay;   /* 每次操作后的微秒延迟(调节快慢) */
    uint64_t       consumed;   /* 消费者: 实际取到多少个 */
    int            order_ok;   /* 消费者: 序号是否严格递增 */
    uint64_t       last_seq;
} stress_ctx_t;

static void *producer_fn(void *arg)
{
    stress_ctx_t *c = (stress_ctx_t *)arg;
    uint8_t       frame[FRAME_SLOT];
    uint64_t      i;

    for (i = 0; i < c->n; i++) {
        make_frame(frame, i);
        infra_queue_push(c->q, frame, FRAME_SLOT);
        if (c->us_delay > 0)
            usleep((useconds_t)c->us_delay);
    }
    return NULL;
}

static void *consumer_fn(void *arg)
{
    stress_ctx_t *c = (stress_ctx_t *)arg;
    uint8_t       frame[FRAME_SLOT];
    size_t        len = 0;

    c->order_ok = 1;
    c->last_seq = 0;
    while (c->consumed < c->n) {
        /* 超时给 2 秒: 宁可能慢, 不要在慢机器上把"生产者还没发完"误判成"结束了" */
        if (infra_queue_pop(c->q, frame, sizeof(frame), &len, 2000) == 0) {
            uint64_t seq = frame_seq(frame);
            if (c->consumed > 0 && seq <= c->last_seq)
                c->order_ok = 0;        /* ★ 顺序必须严格递增 */
            c->last_seq = seq;
            c->consumed++;
        } else {
            break;                      /* 超时: 认为生产者已结束且队列空 */
        }
        if (c->us_delay > 0)
            usleep((useconds_t)c->us_delay);
    }
    return NULL;
}

/* ─────────────── 阻塞唤醒测试用的上下文 ─────────────── */

typedef struct {
    infra_queue_t *q;
    uint64_t       got_seq;
    int            rc;
} wake_ctx_t;

static void *delayed_producer_fn(void *arg)
{
    wake_ctx_t *c = (wake_ctx_t *)arg;
    uint8_t     frame[FRAME_SLOT];

    usleep(100 * 1000);                 /* 等 100ms 再发 —— 让消费者先睡着 */
    make_frame(frame, 42);
    infra_queue_push(c->q, frame, FRAME_SLOT);
    return NULL;
}

static void *blocking_consumer_fn(void *arg)
{
    wake_ctx_t *c = (wake_ctx_t *)arg;
    uint8_t     frame[FRAME_SLOT];
    size_t      len = 0;

    /* 超时给 3 秒 —— 如果阻塞唤醒有问题, 这里会超时 */
    c->rc = infra_queue_pop(c->q, frame, sizeof(frame), &len, 3000);
    if (c->rc == 0)
        c->got_seq = frame_seq(frame);
    return NULL;
}

/* ─────────────── 主测试 ─────────────── */

int main(void)
{
    printf("===== infra_queue 单元测试 =====\n\n");

    /* ═══ ① 功能:FIFO 顺序 ═══ */
    printf("① 基本功能: FIFO 顺序\n");
    {
        infra_queue_t *q = infra_queue_create(4, FRAME_SLOT);
        uint8_t        frame[FRAME_SLOT];
        uint8_t        out[FRAME_SLOT];
        size_t         len = 0;
        int            i;

        check("create 成功", q != NULL);
        check("初始深度 = 0", infra_queue_depth(q) == 0);

        for (i = 0; i < 4; i++) {
            make_frame(frame, (uint64_t)i);
            check("push 成功且未丢帧(返回 0)",
                  infra_queue_push(q, frame, FRAME_SLOT) == 0);
        }
        check("深度 = 4", infra_queue_depth(q) == 4);

        {
            int ok = 1;
            for (i = 0; i < 4; i++) {
                if (infra_queue_pop(q, out, sizeof(out), &len, 0) != 0)
                    ok = 0;
                else if (frame_seq(out) != (uint64_t)i || len != FRAME_SLOT)
                    ok = 0;
            }
            check("取出顺序是 0,1,2,3(FIFO)且长度正确", ok);
        }
        check("取完后深度 = 0", infra_queue_depth(q) == 0);
        infra_queue_destroy(q);
    }
    printf("\n");

    /* ═══ ② 队满:**丢最旧** ═══ */
    printf("② 队满策略: 丢最旧(而不是丢新)\n");
    {
        infra_queue_t       *q = infra_queue_create(4, FRAME_SLOT);
        uint8_t              frame[FRAME_SLOT];
        uint8_t              out[FRAME_SLOT];
        size_t               len = 0;
        infra_queue_stats_t  st;
        int                  i;
        int                  drop_rc = 0;

        /* 塞满 4 个: 序号 0,1,2,3 */
        for (i = 0; i < 4; i++) {
            make_frame(frame, (uint64_t)i);
            infra_queue_push(q, frame, FRAME_SLOT);
        }
        /* 再塞 2 个: 5,6 → 应当丢掉最旧的 0 和 1 */
        for (i = 5; i <= 6; i++) {
            make_frame(frame, (uint64_t)i);
            drop_rc = infra_queue_push(q, frame, FRAME_SLOT);
        }
        check("队满时 push 返回 1(表示丢了旧帧)", drop_rc == 1);
        check("深度仍等于容量(没涨)", infra_queue_depth(q) == 4);

        {
            uint64_t first = 0;
            infra_queue_pop(q, out, sizeof(out), &len, 0);
            first = frame_seq(out);
            check("★ 取出的第一个是 2(最旧的 0,1 已被丢)", first == 2);
        }
        infra_queue_get_stats(q, &st);
        check("统计: dropped = 2", st.dropped == 2);
        check("统计: pushed = 6", st.pushed == 6);
        infra_queue_destroy(q);
    }
    printf("\n");

    /* ═══ ③ 超长帧被拒绝(不截断)═══ */
    printf("③ 单帧超长: 整条拒绝(不截断)\n");
    {
        infra_queue_t       *q = infra_queue_create(4, FRAME_SLOT);
        uint8_t              big[FRAME_SLOT + 1];
        infra_queue_stats_t  st;

        memset(big, 0xAB, sizeof(big));
        check("超过 slot_size 的帧被拒绝(返回 -1)",
              infra_queue_push(q, big, sizeof(big)) == -1);
        infra_queue_get_stats(q, &st);
        check("统计: rejected = 1", st.rejected == 1);
        check("队列深度仍为 0", infra_queue_depth(q) == 0);
        infra_queue_destroy(q);
    }
    printf("\n");

    /* ═══ ④ 超时语义 ═══ */
    printf("④ 超时语义(空队列)\n");
    {
        infra_queue_t *q = infra_queue_create(2, FRAME_SLOT);
        uint8_t        out[FRAME_SLOT];
        struct timespec t0, t1;
        long           ms;

        check("timeout=0 → 立即返回 1(超时)",
              infra_queue_pop(q, out, sizeof(out), NULL, 0) == 1);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        check("timeout=100 → 返回 1",
              infra_queue_pop(q, out, sizeof(out), NULL, 100) == 1);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        /* 允许调度误差, 只要求"确实等过一会儿" */
        check("  且确实等了 >= 90ms(不是立刻返回)", ms >= 90);

        check("缓冲比槽位小 → 返回 -1(参数错)",
              infra_queue_pop(q, out, 1, NULL, 0) == -1);
        infra_queue_destroy(q);
    }
    printf("\n");

    /* ═══ ⑤ 线程压测 A:不丢帧场景 ═══ */
    printf("⑤ 线程压测 A: 生产者【比消费者慢】(验证容量够 → 不丢帧)\n");
    {
        infra_queue_t       *q = infra_queue_create(64, FRAME_SLOT);
        /*
         * ⚠️ 关键: 生产者必须真的比消费者慢, 否则这个测试就失去意义。
         * 第一版我两边都设 0 延迟 —— 结果生产者的空循环比消费者快 3 倍,
         * 丢了 69% 的帧, 而我还写"丢帧应当极少" → 测试自己打自己的脸。
         * 这里给生产者加 50us 延迟: 20000 帧约 1 秒, 消费者能跟上。
         */
        stress_ctx_t         prod = { q, 20000, 50, 0, 0, 0 };
        stress_ctx_t         cons = { q, 20000, 0, 0, 0, 0 };
        pthread_t            tp, tc;
        infra_queue_stats_t  st;

        pthread_create(&tp, NULL, producer_fn, &prod);
        pthread_create(&tc, NULL, consumer_fn, &cons);
        pthread_join(tp, NULL);
        pthread_join(tc, NULL);

        infra_queue_get_stats(q, &st);
        printf("      pushed=%llu popped=%llu dropped=%llu depth=%zu max_depth=%zu\n",
               (unsigned long long)st.pushed, (unsigned long long)st.popped,
               (unsigned long long)st.dropped, st.depth, st.max_depth);
        check("pushed == popped + dropped(统计自洽)",
              st.pushed == st.popped + st.dropped);
        check("★ 消费者把 20000 帧都取到了(没饿死)", st.popped == 20000);
        check("★ 没有丢帧(容量 64 够用)", st.dropped == 0);
        check("★ 消费者取到的序号严格递增(顺序没乱)", cons.order_ok == 1);
        infra_queue_destroy(q);
    }
    printf("\n");

    /* ═══ ⑥ 线程压测 B:必然丢帧场景 ═══ */
    printf("⑥ 线程压测 B: 生产者远快于消费者(必丢帧) —— 验丢最旧\n");
    {
        infra_queue_t       *q = infra_queue_create(8, FRAME_SLOT);
        stress_ctx_t         prod = { q, 3000, 0, 0, 0, 0 };      /* 全速 */
        stress_ctx_t         cons = { q, 3000, 200, 0, 0, 0 };    /* 慢 200us/帧 */
        pthread_t            tp, tc;
        infra_queue_stats_t  st;

        pthread_create(&tp, NULL, producer_fn, &prod);
        pthread_create(&tc, NULL, consumer_fn, &cons);
        pthread_join(tp, NULL);
        pthread_join(tc, NULL);

        infra_queue_get_stats(q, &st);
        printf("      pushed=%llu popped=%llu dropped=%llu max_depth=%zu\n",
               (unsigned long long)st.pushed, (unsigned long long)st.popped,
               (unsigned long long)st.dropped, st.max_depth);
        check("★ 确实发生了丢帧(dropped > 0)", st.dropped > 0);
        check("统计自洽: pushed == popped + dropped",
              st.pushed == st.popped + st.dropped);
        check("★ 最高水位不超过容量(环形缓冲没写爆)",
              st.max_depth <= st.capacity);
        check("★ 消费者取到的序号仍严格递增(丢的是最旧的,不是顺序乱)",
              cons.order_ok == 1);
        infra_queue_destroy(q);
    }
    printf("\n");

    /* ═══ ⑦ 阻塞唤醒 ═══ */
    printf("⑦ 阻塞唤醒: 消费者先睡, 生产者 100ms 后才发\n");
    {
        infra_queue_t *q = infra_queue_create(4, FRAME_SLOT);
        wake_ctx_t     wc = { q, 0, -99 };
        pthread_t      tc, tp;

        pthread_create(&tc, NULL, blocking_consumer_fn, &wc);
        usleep(30 * 1000);              /* 让消费者先进入 pop 阻塞 */
        pthread_create(&tp, NULL, delayed_producer_fn, &wc);
        pthread_join(tp, NULL);
        pthread_join(tc, NULL);

        check("★ 阻塞的 pop 被成功唤醒(返回 0, 没等到超时)",
              wc.rc == 0);
        check("★ 取到的正是那一帧(序号 42)", wc.got_seq == 42);
        infra_queue_destroy(q);
    }
    printf("\n");

    /* ═══ ⑧ 参数健壮性 ═══ */
    printf("⑧ 参数健壮性\n");
    {
        check("创建时 capacity=0 → NULL", infra_queue_create(0, 64) == NULL);
        check("创建时 slot_size=0 → NULL", infra_queue_create(4, 0) == NULL);
        check("destroy(NULL) 安全", 1);
        infra_queue_destroy(NULL);
        check("push(NULL,...) 返回 -1",
              infra_queue_push(NULL, "x", 1) == -1);
        check("depth(NULL) 返回 0", infra_queue_depth(NULL) == 0);
        {
            uint8_t out[FRAME_SLOT];
            check("pop(NULL,...) 返回 -1",
                  infra_queue_pop(NULL, out, sizeof(out), NULL, 0) == -1);
        }
    }

    printf("\n===== 结果: %s(%d 项失败)=====\n",
           g_fails == 0 ? "全部通过" : "有失败", g_fails);
    return g_fails == 0 ? 0 : 1;
}
