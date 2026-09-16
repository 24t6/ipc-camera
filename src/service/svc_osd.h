/**
 * @file svc_osd.h
 * @brief OSD 时间水印服务 —— 1 Hz 线程:取当前时间 → 渲染 → 提交给 REGION
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  为什么 OSD 要单独起一个线程
 * ─────────────────────────────────────────────────────────────────
 *  M2 的验收要求"每秒更新一次"。看起来最省事的做法是**在取流线程里顺便更新**,
 *  但那是错的:
 *    · `HI_MPI_RGN_SetBitMap` 是**阻塞**的 MPP 调用, 会往 VENC 通道塞数据;
 *    · 取流线程的职责是"取 → 入队 → 立刻 ReleaseStream"(ADR-3),
 *      任何多余阻塞都可能让 VENC 缓冲淤死 → **编码器停止编码**(B027 的同一类风险)。
 *  所以更新频率与取流频率**解耦**:另起一个 1 Hz 线程, 各自管各自的。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 启动顺序:必须在 `svc_media_start()` **之后**
 * ─────────────────────────────────────────────────────────────────
 *  OSD 挂在 **VENC 通道**上, 而那个通道是 `bsp_mpp_init()` 建的
 *  (由 `svc_media_start()` 触发)。所以顺序是:
 *
 *      起: 队列 → svc_net → svc_sender → svc_media → **svc_osd**
 *      停: **svc_osd** → svc_media → svc_sender → svc_net   (反序)
 *
 *  停的时候 OSD **必须最先** —— `svc_media_stop()` 会 `bsp_mpp_deinit()` 拆掉
 *  VENC 通道, 那时再想 Detach 就晚了。
 *
 * @note 本服务**失败不影响推流**:水印是锦上添花, 不该因为它起不来就没画面。
 *       `app_main` 里对启动失败只告警、不退出。
 */
#ifndef __SVC_OSD_H__
#define __SVC_OSD_H__

#include <stdint.h>

/** 更新周期(秒)。M2 验收要求"秒级更新" */
#define SVC_OSD_PERIOD_SEC 1

/** OSD 服务统计(用于日志与验收断言) */
typedef struct {
    uint64_t ticks;        /**< 成功提交的次数 —— **应 ≈ 运行秒数**(验收判据) */
    uint64_t init_errors;  /**< 初始化失败次数 */
    uint64_t show_errors;  /**< 提交失败次数 —— **验收要求 = 0** */
} svc_osd_stats_t;

/**
 * @brief 启动 OSD 服务。
 *
 * @return 0 成功; 负值失败(此时不会有水印, 但推流照常)
 *
 * @note 内部先 `bsp_osd_init()`, 再起 1 Hz 线程。
 * @note **必须在 `svc_media_start()` 之后调**(要挂到已存在的 VENC 通道上)。
 * @note 重复调用(已启动)返回 0, 不做任何事。
 */
int svc_osd_start(void);

/**
 * @brief 停止 OSD 服务: 通知线程退出 → join → `bsp_osd_deinit()`。
 *
 * @note **必须在 `svc_media_stop()` 之前调** —— 否则 VENC 通道已经被拆掉。
 * @note 未启动时调用是安全的(no-op)。
 */
void svc_osd_stop(void);

/** @brief 服务是否在运行。@return 1 = 在运行 */
int svc_osd_is_running(void);

/** @brief 取统计快照。 */
void svc_osd_get_stats(svc_osd_stats_t *out);

#endif /* __SVC_OSD_H__ */
