/**
 * @file    svc_sender.h
 * @brief   RTP 发送服务 —— 从队列取帧, 分片成 RTP 包, 发给所有正在播放的客户端
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它在整条链路里的位置(这就是 M1-9 的"最后一根线")
 * ─────────────────────────────────────────────────────────────────
 *
 *      MPP ──▶ svc_media ──push──▶ [infra_queue] ──pop──▶ **svc_sender** ──▶ 网络
 *                                        ▲                    │
 *                                        │                    └─ 每个客户端一份
 *                                   svc_net 通过             RTP 会话(seq/ts/ssrc)
 *                                   on_play / on_teardown
 *                                   告诉本模块"谁在看"
 *
 *  三个模块各管一件事, 通过队列和回调**解耦**:
 *      · `svc_media` 只管取流 + 入队(不知道有网络)
 *      · `svc_net`   只管 RTSP 控制(不知道有码流)
 *      · `svc_sender` 只管"把队里的帧发给在看的人"
 *  这正是 ADR-3:**取流线程绝不 sendto**。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⭐ 三个关键设计决定
 * ─────────────────────────────────────────────────────────────────
 *
 *  **① 每个客户端一份独立的 RTP 会话**
 *     序列号、时间戳、SSRC **不能共用** —— 两个客户端各有自己的播放进度,
 *     共用序列号会让其中一个的丢包判断全乱。所以 `rtp_session_t` 放在
 *     客户端槽位里, 一客户端一份。
 *
 *  **② 新客户端必须从 IDR(关键帧)开始**
 *     P 帧是"相对上一帧的差分", 中途加入的客户端没有参考帧 → **花屏**。
 *     所以新客户端先标记为"等待 IDR", 中间的 P 帧**直接跳过**, 直到
 *     遇到关键帧才开始发。
 *     好消息: 海思的关键帧里**自带 SPS/PPS/SEI**(实测每个 IDR 帧 4 个 pack:
 *     SPS+PPS+SEI+IDR), 所以客户端从 IDR 起就能**自足解码**, 不需要我们再补发参数集。
 *
 *  **③ 发送线程只有一条, 客户端槽位不加锁**
 *     队列只有一个消费者(本模块的发送线程), `on_play`/`on_teardown`
 *     由 `svc_net` 的事件循环线程调用 → **两个线程会碰同一张客户端表**。
 *     处理办法: 用一把**只保护"增删客户端"**的锁(不进发送热路径),
 *     发送时只读快照。详细取舍见 `svc_sender.c` 顶部。
 *
 * ─────────────────────────────────────────────────────────────────
 *  时间戳来自编码器(B012 的正解)
 * ─────────────────────────────────────────────────────────────────
 *  用队列帧头里的 `pts`(实测本板 **1 MHz**), 经
 *  `proto_rtp_session_frame_pts()` 换算成 90kHz。
 *  **不再按"假设帧率"自己加** —— 那正是 B012 记录的问题。
 *
 * @note 本模块**自己起一个线程**(发送线程), `svc_sender_start` 返回后即在跑。
 * @note 线程所有权: 队列由调用方创建并拥有, 本模块只 pop, 不销毁它。
 */
#ifndef __SVC_SENDER_H__
#define __SVC_SENDER_H__

#include <netinet/in.h>
#include <stdint.h>

/** 最多同时服务多少个"正在播放"的客户端。与 svc_net 的上限一致 */
#define SVC_SENDER_MAX_CLIENTS 8

/** 发送服务统计 —— 用于日志与验收断言 */
typedef struct {
    uint64_t frames_sent;      /**< 成功分发给客户端的帧数(按"帧×客户端"计) */
    uint64_t packets_sent;     /**< 累计发出的 RTP 包数 */
    uint64_t bytes_sent;       /**< 累计发出的字节数(含 RTP 头) */
    uint64_t send_errors;      /**< 发送失败次数 */
    uint64_t frames_dropped;   /**< 因队列丢旧帧而没发出去的帧数(队列水位告警) */
    uint64_t waiting_idr;      /**< 当前有几个客户端还在等 IDR */
    uint64_t clients_joined;   /**< 累计加入过的客户端数 */
    uint64_t clients_left;     /**< 累计离开的客户端数 */

    /**
     * 「**帧龄**」统计(微秒)= `CLOCK_MONOTONIC(现在) − u64PTS(该帧采集时刻)`。
     *
     * @note ★ 这是回答"延迟大到底怪谁"的**可证伪**诊断量。
     *
     *   ⚠️ **实测发现:这个差不等于绝对延迟, 它有一个约 −115 ms 的固定偏置**
     *   (即 PTS 比单调时钟**超前**约 115 ms; 首帧日志会打出原始值)。
     *   两个时钟**同源**(都是 1 MHz、都从开机起算) ——
     *   判据是偏置**稳定**:若真不同源, 差值会是随机的巨量。
     *
     *   所以**要看的是相对量**, 偏置在相减时自动抵消:
     *     · `age_max − age_min` → 帧龄**抖动**(有没有忽快忽慢)
     *     · `age_last − age_first` → 帧龄**漂移**(有没有在攒积压)
     *   两者都接近 0 就说明**板子侧既不抖也不攒**, 延迟的大头不在我们这里。
     *
     *   覆盖范围:摄像头曝光 → VI → VPSS → VENC 编码 → 队列 → 准备发出。
     *   ⚠️ **不含**客户端(VLC)自己的缓冲 —— VLC 的 RTSP **默认网络缓存 1000 ms**。
     */
    int64_t  age_last_us;      /**< 最近一帧的帧龄 */
    int64_t  age_first_us;     /**< 第一帧的帧龄(算漂移的基准) */
    int64_t  age_min_us;       /**< 最小帧龄(= 偏置 + 最好的情况) */
    int64_t  age_max_us;       /**< 最大帧龄 */
    uint64_t age_samples;      /**< 采样帧数 */
} svc_sender_stats_t;

/**
 * 启动发送服务。
 *
 * @param queue 帧队列(**由调用方创建并拥有**; 本模块只 pop)
 * @param is_h265 1 = 按 H.265 打包; 0 = H.264。**必须和编码器那一路一致**
 * @return 0 成功; 负值失败:
 *         -1 = 参数非法  -2 = 建 UDP socket 失败  -3 = 线程创建失败
 *
 * @note **非阻塞**: 建 socket + 起线程, 不等客户端。
 * @note 重复调用(已启动)返回 0 且不做任何事。
 */
int svc_sender_start(void *queue, int is_h265);

/**
 * 停止发送服务: 通知线程退出 → join → 释放资源。
 * @note **阻塞**, 会等线程真正退出。未启动时调用安全(no-op)。
 */
void svc_sender_stop(void);

/** 是否在运行。@return 1 = 在运行 */
int svc_sender_is_running(void);

/**
 * 通知"有个客户端开始播放了"。
 *
 * @param client_index 客户端槽位下标(`svc_net` 的 `on_play` 里的那个)
 * @param rtp_dst      该客户端的 RTP 目的地(**非 NULL**, 由 svc_net 给出)
 * @return 0 成功; -1 参数非法; -2 客户端槽位已满
 *
 * @note 由 `svc_net` 的**事件循环线程**调用(回调里), 内部只是登记,
 *       **不阻塞、不发送**。
 * @note 新客户端会被标记为"**等待 IDR**" —— 见文件头设计决定 ②。
 */
int svc_sender_add_client(int client_index, const struct sockaddr_in *rtp_dst);

/**
 * 通知"某个客户端停止播放/断开了"。
 *
 * @param client_index 客户端槽位下标
 *
 * @note 由 `svc_net` 的**事件循环线程**调用(TEARDOWN/断开/超时)。
 * @note 对不存在的下标调用是安全的(no-op) —— 客户端可能因超时被清两次。
 */
void svc_sender_remove_client(int client_index);

/** 取统计快照。 */
void svc_sender_get_stats(svc_sender_stats_t *out);

/** 当前有多少个客户端在播放(用于验收断言)。 */
int svc_sender_client_count(void);

#endif /* __SVC_SENDER_H__ */
