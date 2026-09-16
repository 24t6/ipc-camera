/**
 * @file    infra_poll.h
 * @brief   事件轮询封装 —— **Linux epoll**
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  ADR-2:为什么用 epoll 而不是 select
 * ─────────────────────────────────────────────────────────────────
 *      select 每次调用都要传全量 fd 集合(O(n)), 且有 FD_SETSIZE=1024 上限;
 *      epoll  用红黑树注册 + 就绪链表, 只返回就绪的 fd(O(1))。
 *
 * ─────────────────────────────────────────────────────────────────
 *  那为什么还要包一层, 不直接调 epoll_*?
 * ─────────────────────────────────────────────────────────────────
 *  两个具体好处, 都不是"为了可移植"(本项目的目标平台只有 Linux):
 *    ① **调用点集中**: `epoll_ctl` 的 ADD/MOD/DEL 三态很啰嗦(要先试 MOD 再 ADD
 *       才能做到"重复注册即修改"), 包一层后 `svc_net` 里只有 add/del/wait 三个动作
 *    ② **EINTR 处理统一**: 被信号打断时这里直接返回 0(当作没有事件),
 *       上层事件循环不必到处判断 errno
 *
 *  ⚠️ 本层**不做任何平台分支** —— 板上是 epoll, 开发机上也必须用能编译 epoll 的
 *     环境(Ubuntu VM)。理由: 加一个"只在这台开发机上用、永远不上板"的分支,
 *     等于引入一段**没人验证的代码**; 而协议层本来就一直在 Linux 上单测。
 *
 * @note 本模块只做**等待与就绪判定**, 不做读写、不解析协议、不持有 fd 所有权。
 */
#ifndef __INFRA_POLL_H__
#define __INFRA_POLL_H__

#include <stdint.h>

/** 一次最多处理多少个就绪事件 */
#define INFRA_POLL_MAX_EVENTS 64

/** 事件类型(可用按位或组合) */
#define INFRA_POLL_IN   0x01u       /* 可读 / 有新连接 */
#define INFRA_POLL_OUT  0x02u       /* 可写 */
#define INFRA_POLL_ERR  0x04u       /* 出错或对端挂断 */

/** 一个就绪事件 */
typedef struct {
    int      fd;
    uint32_t events;                /* INFRA_POLL_* 的组合 */
} infra_poll_event_t;

/** 轮询器(不透明句柄) */
typedef struct infra_poller infra_poller_t;

/**
 * @brief 创建轮询器(内部 epoll_create1(EPOLL_CLOEXEC))。
 * @return 句柄; 失败返回 NULL
 */
infra_poller_t *infra_poller_create(void);

/** @brief 销毁轮询器。**不关闭**已注册的 fd —— 所有权始终归调用方。 */
void infra_poller_destroy(infra_poller_t *p);

/**
 * @brief 注册(或修改)一个 fd 关心的事件。
 *
 * @param events INFRA_POLL_IN / INFRA_POLL_OUT 的组合
 * @return 0 成功; -1 失败
 *
 * @note 对同一个 fd 重复调用 = **修改**关心的事件, 不必先 del 再 add。
 */
int infra_poller_add(infra_poller_t *p, int fd, uint32_t events);

/** @brief 取消注册。@return 0 成功; -1 失败(如本来就没注册) */
int infra_poller_del(infra_poller_t *p, int fd);

/**
 * @brief 等待事件。
 *
 * @param timeout_ms 超时毫秒; 0 = 立即返回; <0 = 一直等
 * @return 就绪事件个数(>=0); -1 = 出错
 *
 * @note 被信号打断(EINTR)时返回 0 而非 -1 —— 上层按"没有事件"处理即可。
 */
int infra_poller_wait(infra_poller_t *p, infra_poll_event_t *events,
                      int max, int timeout_ms);

#endif /* __INFRA_POLL_H__ */
