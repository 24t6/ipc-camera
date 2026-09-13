/**
 * @file    infra_queue.h
 * @brief   线程安全环形队列 —— 取流线程与发送/录制线程之间的缓冲
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它解决什么(ADR-3)
 * ─────────────────────────────────────────────────────────────────
 *  如果**取流线程直接 sendto**, 网络一慢就会堵在 sendto 里 ——
 *  而 VENC 的编码缓冲有限, 取流被堵住 = **编码器丢帧**。
 *  录制也一样: 磁盘偶尔变慢不能拖累编码。
 *
 *  所以拆成:
 *
 *      取流线程 ──push──▶ [队列] ──pop──▶ 发送线程 ──▶ 网络
 *                              └─pop──▶ 录制线程 ──▶ TF 卡
 *
 *  **生产者只入队, 绝不阻塞在网络上。**
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⭐ 队满时的策略:**丢最旧的帧**(而不是丢新的、也不是阻塞)
 * ─────────────────────────────────────────────────────────────────
 *  实时流的铁律:**过期的数据毫无价值**。
 *  队列里积压 8 帧(约 0.5 秒)说明下游已经跟不上了 ——
 *  此时:
 *      ❌ 丢新的 → 画面停在"最旧的"那一刻, 越拖越旧
 *      ❌ 阻塞生产者 → 拖死编码器(正是要避免的)
 *      ✅ 丢最旧的 → 画面始终是"最新的一帧", 代价是丢帧计数上升
 *
 *  ─────────────────────────────────────────────────────────────────
 *  ⚠️ 内存:**创建时一次性分配, 运行期不再分配**
 *  ─────────────────────────────────────────────────────────────────
 *  队列内部有 `capacity` 个槽位, 每个槽位 `slot_size` 字节,
 *  **在 infra_queue_create() 里一次 malloc 完**, 运行期 push/pop 只做 memcpy。
 *
 *  依据(见 ARCHITECTURE.md 第五节预算表):
 *      发送队列 8 槽 × 256 KB ≈ 2 MB;录制队列 4 槽 × 256 KB ≈ 1 MB
 *      板子用户态可用约 100 MB, 规划占用 < 5 MB —— 很安全。
 *
 * @note 每个实例自带锁与条件变量, **不同实例之间无共享状态**, 可多线程使用。
 * @note push 永不阻塞(满则丢旧); pop 可阻塞等待(这是队列"等数据"的正常用法)。
 */
#ifndef __INFRA_QUEUE_H__
#define __INFRA_QUEUE_H__

#include <stddef.h>
#include <stdint.h>

/** 队列句柄(不透明) */
typedef struct infra_queue infra_queue_t;

/** 队列运行统计 —— 对应 ARCHITECTURE.md "必须监测的指标" */
typedef struct {
    uint64_t pushed;        /* 累计成功入队 */
    uint64_t popped;        /* 累计成功出队 */
    uint64_t dropped;       /* 因队满而丢弃的最旧帧数 */
    uint64_t rejected;      /* 因单帧超长被拒绝的入队次数 */
    size_t   depth;         /* 当前深度(即刻可读水位) */
    size_t   capacity;      /* 总容量 */
    size_t   max_depth;     /* 历史最高水位 —— 用来判断"够不够用" */
} infra_queue_stats_t;

/**
 * 创建队列。
 *
 * @param capacity  槽位数(必须 > 0)
 * @param slot_size 每个槽位的字节数(必须 > 0)
 * @return 句柄; 失败返回 NULL(参数非法或内存不足)
 *
 * @note 总内存 = capacity × slot_size, **一次性分配**。
 * @note 销毁前必须保证没有线程还在用它(本模块不做引用计数)。
 */
infra_queue_t *infra_queue_create(size_t capacity, size_t slot_size);

/** 销毁队列, 释放全部槽位。传 NULL 安全。 */
void infra_queue_destroy(infra_queue_t *q);

/**
 * 入队(拷贝 len 字节)。
 *
 * @param len 数据长度, **必须 <= 创建时的 slot_size**; 否则整条被拒绝
 * @return 0 = 成功入队; -1 = 参数非法或单帧超长(计 rejected);
 *         1 = 成功入队但**为腾位置丢弃了最旧的一帧**(计 dropped)
 *
 * @note **永不阻塞** —— 队满时丢最旧, 生产者不会被下游拖住。
 * @note 返回值区分 0 和 1, 是为了让调用方能单独统计"丢帧率"。
 */
int infra_queue_push(infra_queue_t *q, const void *data, size_t len);

/**
 * 出队(拷贝到一个缓冲)。
 *
 * @param buf 输出缓冲
 * @param cap buf 的容量(必须 >= 创建时的 slot_size 才安全)
 * @param out_len 输出: 实际取出的字节数(可为 NULL)
 * @param timeout_ms 0 = 立即返回; > 0 = 最多等这么久; < 0 = 一直等
 * @return 0 = 成功; -1 = 参数非法; 1 = 超时(队列为空)
 *
 * @note 队列为空时按 timeout_ms 阻塞等待 —— **消费线程靠这个"睡到有数据"**,
 *       不必自己写轮询。
 * @note 允许 out_len 为 NULL(调用方只关心"有没有数据")。
 */
int infra_queue_pop(infra_queue_t *q, void *buf, size_t cap,
                    size_t *out_len, int timeout_ms);

/** 取当前深度(加锁读一次, 用于打日志/水位监测)。 */
size_t infra_queue_depth(infra_queue_t *q);

/** 取统计快照。@note 一次性加锁读全部字段, 保证同一时刻的一致性。 */
void infra_queue_get_stats(infra_queue_t *q, infra_queue_stats_t *out);

#endif /* __INFRA_QUEUE_H__ */
