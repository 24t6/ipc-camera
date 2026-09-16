/**
 * @file    infra_poll.c
 * @brief   事件轮询实现 —— epoll(见 infra_poll.h 的取舍说明)
 *
 * 【模块职责】epoll 的薄封装(注册 / 等待 / 移除), 让上层不必直接写 epoll_ctl
 * 【依赖方向】只依赖 Linux epoll 与 libc
 * 【线程模型】**一个 poller 只允许一个线程 wait**; 注册/移除也建议在同一线程做
 * 【资源边界】epoll fd 一个; poller 结构在创建时分配, destroy 时释放
 */
#include "infra_poll.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

struct infra_poller {
    int epfd;
};

/** @brief 我们的掩码 → epoll 掩码 */
static uint32_t to_epoll(uint32_t events)
{
    uint32_t e = 0;

    if (events & INFRA_POLL_IN)
        e |= EPOLLIN;
    if (events & INFRA_POLL_OUT)
        e |= EPOLLOUT;
    return e;
}

/** @brief epoll 掩码 → 我们的掩码 */
static uint32_t from_epoll(uint32_t e)
{
    uint32_t r = 0;

    if (e & EPOLLIN)
        r |= INFRA_POLL_IN;
    if (e & EPOLLOUT)
        r |= INFRA_POLL_OUT;
    if (e & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
        r |= INFRA_POLL_ERR;
    return r;
}

infra_poller_t *infra_poller_create(void)
{
    infra_poller_t *p = (infra_poller_t *)malloc(sizeof(*p));

    if (p == NULL)
        return NULL;

    p->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (p->epfd < 0) {
        free(p);
        return NULL;
    }
    return p;
}

void infra_poller_destroy(infra_poller_t *p)
{
    if (p == NULL)
        return;
    if (p->epfd >= 0)
        close(p->epfd);
    free(p);
}

int infra_poller_add(infra_poller_t *p, int fd, uint32_t events)
{
    struct epoll_event ev;

    if (p == NULL || fd < 0)
        return -1;

    memset(&ev, 0, sizeof(ev));
    ev.events  = to_epoll(events);
    ev.data.fd = fd;

    /*
     * 先试 MOD: 已注册过就改事件。
     * MOD 报 ENOENT 说明这个 fd 还没注册过, 再 ADD。
     * 这样调用方可以无脑重复调用 add, 不必自己记状态。
     */
    if (epoll_ctl(p->epfd, EPOLL_CTL_MOD, fd, &ev) == 0)
        return 0;
    if (errno == ENOENT && epoll_ctl(p->epfd, EPOLL_CTL_ADD, fd, &ev) == 0)
        return 0;
    return -1;
}

int infra_poller_del(infra_poller_t *p, int fd)
{
    if (p == NULL || fd < 0)
        return -1;
    return (epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, NULL) == 0) ? 0 : -1;
}

int infra_poller_wait(infra_poller_t *p, infra_poll_event_t *events,
                      int max, int timeout_ms)
{
    struct epoll_event evs[INFRA_POLL_MAX_EVENTS];
    int                n;
    int                i;
    int                cap;

    if (p == NULL || events == NULL || max <= 0)
        return -1;
    if (max > INFRA_POLL_MAX_EVENTS)
        max = INFRA_POLL_MAX_EVENTS;
    cap = max;

    n = epoll_wait(p->epfd, evs, cap, timeout_ms);
    if (n < 0) {
        if (errno == EINTR)
            return 0;                   /* 被信号打断 → 当作"没有事件" */
        return -1;
    }
    for (i = 0; i < n; i++) {
        events[i].fd     = evs[i].data.fd;
        events[i].events = from_epoll(evs[i].events);
    }
    return n;
}
