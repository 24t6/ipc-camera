/**
 * @file svc_osd.c
 * @brief OSD 时间水印服务(1 Hz 线程)
 *
 * 【模块职责】定时驱动: 取当前时间 → 渲染 → 提交给 REGION
 * 【依赖方向】依赖 bsp_osd(提交位图)、bsp_osd_render(格式化+渲染)、infra_log;
 *             **不依赖 svc_media / svc_sender / svc_net**
 * 【线程模型】自己起 **1 个**线程(osd_thread); 该线程独占 bsp_osd_*(非线程安全)
 * 【资源边界】无动态分配。只有一个小状态结构(线程句柄 + 统计);
 *             像素缓冲的所有权在 bsp_osd 那边
 *
 * 设计理由、启动顺序的讲究见 `svc_osd.h`。
 *
 * 三段分工(每一段都能单独验证):
 *      `bsp_osd_render.c`  时间 + 字模 → ARGB1555 位图   (**PC 单测**)
 *      `bsp_osd.c`         位图 → REGION → VENC 通道     (板上验)
 *      `svc_osd.c`         **本文件**: 定时驱动上面两段
 */
#include "svc_osd.h"

#include <pthread.h>
#include <time.h>

#include "bsp_osd.h"
#include "bsp_osd_render.h"
#include "infra_log.h"

static struct {
    int             running;
    pthread_t       thread;
    int             thread_valid;
    volatile int    stop_requested;
    svc_osd_stats_t stats;
} g;

/**
 * @brief 取一次当前时间 → 渲染 → 提交
 *
 * @return 0 成功; -1 失败
 * @note 纯"取时间 + 调两个模块", 没有自己的状态 —— 所以它失败只说明
 *       时间取不到或 REGION 拒了, 追查方向很清楚。
 */
static int osd_update_once(void)
{
    struct tm tmv;
    time_t    now;
    char      text[BSP_OSD_RENDER_MAX_CHARS];

    now = time(NULL);
    if (localtime_r(&now, &tmv) == NULL) {
        LOG_ERROR("localtime_r 失败");
        return -1;
    }
    if (bsp_osd_render_format_time(&tmv, text, sizeof(text)) <= 0) {
        LOG_ERROR("时间格式化失败");
        return -1;
    }
    return bsp_osd_show(text);
}

/**
 * @brief 1 Hz 更新线程
 *
 * @param arg 未使用
 * @return 永远返回 NULL
 *
 * @note 循环结构是"**先更新、再睡**" —— 这样起线程后**立刻**就有水印,
 *       而不是先黑着一秒。
 * @note **不做"补偿追赶"**:如果某次更新卡了很久, 下一次照常从"现在"开始,
 *       不去补发中间漏掉的秒 —— 水印显示的是"现在几点", 补发旧时间没有意义。
 */
static void *osd_thread(void *arg)
{
    struct timespec period;

    (void)arg;
    period.tv_sec  = SVC_OSD_PERIOD_SEC;
    period.tv_nsec = 0;

    while (!g.stop_requested) {
        if (osd_update_once() == 0) {
            g.stats.ticks++;
        } else {
            g.stats.show_errors++;
        }
        nanosleep(&period, NULL);
    }
    return NULL;
}

int svc_osd_start(void)
{
    if (g.running) {
        return 0;
    }
    if (bsp_osd_init() != 0) {
        g.stats.init_errors++;
        LOG_ERROR("OSD 初始化失败(没有水印, 但推流不受影响)");
        return -1;
    }
    g.stop_requested = 0;
    g.running        = 1;
    if (pthread_create(&g.thread, NULL, osd_thread, NULL) != 0) {
        LOG_ERROR("OSD 线程创建失败");
        g.running = 0;
        bsp_osd_deinit();
        g.stats.init_errors++;
        return -2;
    }
    g.thread_valid = 1;
    LOG_INFO("OSD 服务已启动(每 %d 秒更新一次)", SVC_OSD_PERIOD_SEC);
    return 0;
}

void svc_osd_stop(void)
{
    if (!g.running) {
        return;
    }
    g.stop_requested = 1;
    if (g.thread_valid) {
        pthread_join(g.thread, NULL);   /* 等线程真退出, 否则 bsp_osd_deinit 会 use-after-free */
        g.thread_valid = 0;
    }
    g.running = 0;

    /* ⚠️ 必须在 svc_media_stop() 之前走到这里 —— 那时 VENC 通道还在 */
    bsp_osd_deinit();

    LOG_INFO("OSD 服务已停止(提交 %llu 次 / 失败 %llu 次)",
             (unsigned long long)g.stats.ticks,
             (unsigned long long)g.stats.show_errors);
}

int svc_osd_is_running(void)
{
    return g.running ? 1 : 0;
}

void svc_osd_get_stats(svc_osd_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = g.stats;
}
