/**
 * @file    infra_queue.c
 * @brief   线程安全环形队列实现 —— 见 infra_queue.h
 *
 * 实现要点:
 *   · **环形缓冲**: head/tail 两个索引对 capacity 取模, 不搬移数据
 *   · **一把互斥锁**保护所有字段; **一个条件变量**让 pop 能"睡到有数据"
 *   · **队满丢最旧**: tail 前移一格(丢弃最旧), 再写入新数据
 *   · 槽位内存创建时一次性 malloc, 运行期只 memcpy
 */
#include "infra_queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/** 队列内部结构(对外不透明) */
struct infra_queue {
    uint8_t  *slots;            /* capacity × slot_size 的连续内存 */
    size_t   *lens;             /* 每个槽位的实际数据长度 */
    size_t    capacity;
    size_t    slot_size;
    size_t    head;             /* 下一个要取的位置 */
    size_t    tail;             /* 下一个要写的位置 */
    size_t    count;            /* 当前元素个数 */
    size_t    max_depth;        /* 历史最高水位 */

    pthread_mutex_t lock;
    pthread_cond_t  not_empty;  /* 有数据了, 唤醒 pop */

    uint64_t  pushed;
    uint64_t  popped;
    uint64_t  dropped;
    uint64_t  rejected;
};

/** 取第 idx 个槽位的地址 */
static uint8_t *slot_at(infra_queue_t *q, size_t idx)
{
    return q->slots + (idx % q->capacity) * q->slot_size;
}

/** 把毫秒超时换算成绝对时间(pthread_cond_timedwait 要的是绝对时刻) */
static void ms_to_abstime(int timeout_ms, struct timespec *ts)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    ts->tv_sec  = tv.tv_sec + timeout_ms / 1000;
    ts->tv_nsec = (long)tv.tv_usec * 1000L + (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {       /* 纳秒进位 */
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

infra_queue_t *infra_queue_create(size_t capacity, size_t slot_size)
{
    infra_queue_t *q;

    if (capacity == 0 || slot_size == 0)
        return NULL;
    /* capacity × slot_size 不能溢出 size_t */
    if (slot_size > (size_t)-1 / capacity)
        return NULL;

    q = (infra_queue_t *)calloc(1, sizeof(*q));
    if (q == NULL)
        return NULL;

    q->slots = (uint8_t *)malloc(capacity * slot_size);
    q->lens  = (size_t *)calloc(capacity, sizeof(size_t));
    if (q->slots == NULL || q->lens == NULL) {
        free(q->slots);
        free(q->lens);
        free(q);
        return NULL;
    }

    q->capacity  = capacity;
    q->slot_size = slot_size;
    q->head      = 0;
    q->tail      = 0;
    q->count     = 0;
    q->max_depth = 0;

    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q->slots);
        free(q->lens);
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        free(q->slots);
        free(q->lens);
        free(q);
        return NULL;
    }
    return q;
}

void infra_queue_destroy(infra_queue_t *q)
{
    if (q == NULL)
        return;
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->slots);
    free(q->lens);
    free(q);
}

int infra_queue_push(infra_queue_t *q, const void *data, size_t len)
{
    int rc = 0;

    if (q == NULL || data == NULL || len == 0)
        return -1;

    pthread_mutex_lock(&q->lock);

    if (len > q->slot_size) {
        /*
         * 单帧超长: 整条拒绝(不做截断)。
         * 截断一帧码流会产出**无法解码**的垃圾 —— 宁可丢整帧并计数。
         * 计数在锁内做, 否则和 infra_queue_get_stats() 的读构成数据竞争。
         */
        q->rejected++;
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    if (q->count == q->capacity) {
        /*
         * 队满 → **丢最旧的**(head 前移一格), 再写入新数据。
         * 这样队列里永远是"最近的 capacity 帧", 不会越积越旧。
         */
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->dropped++;
        rc = 1;                         /* 告诉调用方"这次丢了旧帧" */
    }

    memcpy(slot_at(q, q->tail), data, len);
    q->lens[q->tail] = len;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    q->pushed++;
    if (q->count > q->max_depth)
        q->max_depth = q->count;

    pthread_cond_signal(&q->not_empty); /* 唤醒可能在等的 pop */
    pthread_mutex_unlock(&q->lock);
    return rc;
}

int infra_queue_pop(infra_queue_t *q, void *buf, size_t cap,
                    size_t *out_len, int timeout_ms)
{
    int rc = 0;

    if (q == NULL || buf == NULL || cap == 0)
        return -1;
    if (cap < q->slot_size)
        return -1;                      /* 缓冲比槽位还小, 一定装不下 */

    pthread_mutex_lock(&q->lock);

    /* ── 等数据(队列空时)── */
    while (q->count == 0) {
        if (timeout_ms == 0) {
            rc = 1;                     /* 不等待, 直接报超时 */
            break;
        }
        if (timeout_ms < 0) {
            pthread_cond_wait(&q->not_empty, &q->lock);     /* 一直等 */
        } else {
            struct timespec ts;
            ms_to_abstime(timeout_ms, &ts);
            if (pthread_cond_timedwait(&q->not_empty, &q->lock, &ts)
                == ETIMEDOUT) {
                if (q->count == 0) {    /* 再确认一次(防虚假唤醒) */
                    rc = 1;
                    break;
                }
            }
        }
    }

    if (rc == 0) {
        size_t len = q->lens[q->head];

        memcpy(buf, slot_at(q, q->head), len);
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->popped++;
        if (out_len != NULL)
            *out_len = len;
    }

    pthread_mutex_unlock(&q->lock);
    return rc;
}

size_t infra_queue_depth(infra_queue_t *q)
{
    size_t d;

    if (q == NULL)
        return 0;
    pthread_mutex_lock(&q->lock);
    d = q->count;
    pthread_mutex_unlock(&q->lock);
    return d;
}

void infra_queue_get_stats(infra_queue_t *q, infra_queue_stats_t *out)
{
    if (q == NULL || out == NULL)
        return;

    pthread_mutex_lock(&q->lock);
    out->pushed    = q->pushed;
    out->popped    = q->popped;
    out->dropped   = q->dropped;
    out->rejected  = q->rejected;
    out->depth     = q->count;
    out->capacity  = q->capacity;
    out->max_depth = q->max_depth;
    pthread_mutex_unlock(&q->lock);
}
