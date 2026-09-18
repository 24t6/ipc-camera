/**
 * @file    svc_record.h
 * @brief   MP4 录制服务 —— 录制线程:队列 → NALU → mp4v2 → SD 卡(分段 + 环形覆盖)
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  【模块职责】把发送队列里那一路帧, 封装成分段的 MP4 文件写到 TF 卡
 *  【依赖方向】依赖 infra_queue / infra_log / proto_nalu / svc_media(槽位布局的唯一出处)
 *             与第三方 **mp4v2**;不依赖 bsp/svc_net/svc_sender
 *  【线程模型】自己起 **1 个**线程(record_thread, 线程名 `ipc_rec`);
 *             **独占** mp4v2 句柄与 MP4 写缓冲
 *  【资源边界】无运行期动态分配(mp4v2 内部除外 —— 那是库的行为, 我们控制不了);
 *             文件级静态:取流槽位 ~256KB + MP4 写缓冲 ~256KB + 目录扫描表 ~5KB
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⭐ 复用说明(AGENTS.md §7.0) —— ⚠️ 出处已于 2026-09-16 更正
 * ─────────────────────────────────────────────────────────────────
 *  mp4v2 的**调用序列照抄** `SAMPLE_COMM_VENC_SaveH264ToMP4()`:
 *      · `MP4Create` → `MP4SetTimeScale(90000)`
 *      · `MP4AddH264VideoTrack(90000, 3000, w, h, sps[1], sps[2], sps[3], 3)`
 *        (最后那个 `3` = **每 NALU 前面有 4 字节长度前缀**, 填的是"长度-1")
 *      · `MP4SetVideoProfileLevel(0x7F)` / `MP4AddH264SequenceParameterSet` /
 *        `MP4AddH264PictureParameterSet`
 *      · `MP4WriteSample(..., syncFlag)` —— 关键帧 syncFlag=1
 *      · 每 N 帧 `MP4Close` 再开新文件 = **分段**
 *
 *  ⚠️ **这个函数不是厂商 sample 自带的** —— 出处是**参考项目 IPC-Camera**(公开仓库):
 *      它的 `common/sample_comm_venc.c`(在厂商 sample 上改出来的那份)里
 *      `SAMPLE_COMM_VENC_SaveH264ToMP4()` 是**它自己添加的函数**。
 *      证据: `work/ref_diffs/common__sample_comm_venc.c.diff` 的 `---` 是 SDK 原版、
 *      `+++` 是参考项目版, 该函数行带 `+`;且 SDK 原版 `sample_comm_venc.c` 里
 *      `SaveH264` / `SaveH265` **零命中**, 只有 `SAMPLE_COMM_VENC_SaveStream`
 *      (**只存裸码流**), **全文件没有任何 MP4 调用**。
 *  ⇒ 也就是说:"**MP4 封装 + 分段**"这件事**厂商 sample 完全没做**,
 *     是我们(参照该项目)自己实现的 —— 面试问到时如实这么讲。
 *
 *  ⚠️ **有一处故意不抄**:那份实现是 `malloc(length)` **每帧一次**再 `free`
 *     —— 违反本项目 §6.1「运行期零动态分配」。这里改用**文件级静态缓冲**。
 *
 *  ⚠️ **只支持 H.264**:mp4v2 这一版没有 `MP4AddH265VideoTrack`。
 *     我们默认那路正好是 H.264 720p(见 PROJECT_PLAN 风险 R3 的缓解办法:
 *     录制用低分辨率通道)。跑 `-h265` 时上层要**明确告警**, 不要静默不录。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 关于"强杀后文件能不能播"
 * ─────────────────────────────────────────────────────────────────
 *  MP4 的索引(`moov`)是 **`MP4Close` 时才写**的。所以:
 *      · `kill -TERM`(优雅) → 录制线程收到停止标志 → `MP4Close` → 当前段可播
 *      · `kill -9`(强杀)   → **当前那一段没有 moov, 不可播**
 *  这是 MP4 格式的固有限制, 不是 bug。缓解办法就是 M3-5:**把单段时长缩短**,
 *  把"最坏损失"从几十分钟压到一分钟量级。
 */
#ifndef __SVC_RECORD_H__
#define __SVC_RECORD_H__

#include <stdint.h>

/** 目录路径的最大长度(含结尾 '\0') */
#define SVC_RECORD_PATH_MAX 256

/** 单个 NALU 的上限。720p 实测最大约 55KB, 留足余量;超过的 NALU 会被计数丢弃 */
#define SVC_RECORD_MAX_NALU (256 * 1024)

/** 编码帧率。分段长度对外用**秒**表达, 换算成帧数要乘它 */
#define SVC_RECORD_FPS 30

/** 默认分段长度:**30 分钟**(1800 秒 = 54000 帧) */
#define SVC_RECORD_DEFAULT_SEGMENT_SEC 1800

/**
 * 默认每段帧数 = 默认秒数 × 帧率。
 *
 * @note 为什么是 30 分钟:这是**监控录像的行业惯例**(NVR/DVR 普遍按 30 或 60
 *       分钟切文件)—— 段太长不便于检索和删除, 段太短则文件数爆炸。
 * @note ⚠️ 代价:**段越长, 异常掉电丢得越多**。mp4v2 的索引(moov)只在
 *       `MP4Close` 时落盘, 所以 `kill -9` / 断电最多丢"正在写的那一段",
 *       也就是**最多 30 分钟**。想要更小的掉电损失就把 `-s` 调小。
 */
#define SVC_RECORD_DEFAULT_SEGMENT_FRAMES \
    (SVC_RECORD_DEFAULT_SEGMENT_SEC * SVC_RECORD_FPS)

/**
 * 兜底预留空间(字节):**还没写出过任何一段**时, 用它当"一段大小"的估计。
 *
 * @note 实测码率 4.19 Mbps ⇒ 1 分钟约 31 MB, 这里取 **64 MB ≈ 2 分钟**。
 * @note ⚠️ **行业没有权威水位数值** —— 调研结论:流传的"不超过 80%"都出自
 *       容量规划建议, 不是运行时参数。所以这个数是**我们自己按实测定**的, 不是抄的。
 *       写出过一段之后, 改用**见过的最大段 × 1.25** 当预留(见 `headroom_bytes()`)——
 *       实测用"上一段大小"当预留**偏紧**:24 MB 小盘上每段末尾都撞一次 ENOSPC。
 */
#define SVC_RECORD_HEADROOM_MIN (64ULL * 1024 * 1024)

/** mp4v2 的时间基与"每帧时长"。90000 / 3000 = 30fps, 与码流一致 */
#define SVC_RECORD_TIMESCALE  90000
#define SVC_RECORD_SAMPLE_DUR 3000

/*
 * ★ 分段文件的生命周期(2026-09-17 改进:掉电可见 + 掉电可救)
 *
 *   正在写:      <stamp>.mp4.tmp     ← mp4v2 写这里(没走完 MP4Close 就没有 moov)
 *                <stamp>.h264.tmp    ← 旁路裸流侧车(Annex-B, 靠起始码自定界,
 *                                      所以**被截断也照样能解**)
 *   MP4Close 成功:
 *                rename(<stamp>.mp4.tmp → <stamp>.mp4)   ← 这时才"对外可见"
 *                unlink(<stamp>.h264.tmp)                ← 侧车使命完成, 立刻删
 *   进程启动:    删掉残留的 `*.mp4.tmp`(没有 moov, 救不回来);
 *                把残留的 `*.h264.tmp` **就地重新封装成 `<stamp>.mp4`** ——
 *                Annex-B 靠起始码自定界、**截断也能解**, 所以这一救就把
 *                "断电丢整段"变成"**只丢最后约 30 秒**"。
 *                ⚠️ 这件事**在板子上完成**(复用同一套 mp4v2 调用),
 *                不需要把文件拷到 PC 上再转。
 *
 *   为什么正在写的那段要带 `.tmp`:目录扫描只认 `.mp4`(见 `name_is_mp4()`),
 *   于是半成品**不进环形容量统计、不出现在检索列表**;而且断电残留**一眼可辨** ——
 *   不再是"看着像正常文件、其实播不了"。
 *
 *   为什么侧车只在段进行中存在:它的唯一用途是"这一段中途死了还能救";
 *   MP4 一收尾就多余 —— 关段即删, **稳态磁盘占用仍然是 1 份**, 环形覆盖一行都不用改。
 *
 *   ⚠️ 侧车让**写入量翻倍**(4.19 → 8.4 Mbps)。实测卡写速 17.5 MB/s, 吞吐不是问题;
 *      代价在**卡寿命**(约 6 年 → 约 3 年, 见 `PROJECT_PLAN.md` §3.9)。`-no-raw` 可关。
 *   ⚠️ `-h265` 模式下既不写侧车也不录 MP4(mp4v2 不支持 H.265 封装)。
 */

/** 录制配置(由调用方填) */
typedef struct {
    const char *dir;             /**< 录制目录, 例如 "/mnt/sdcard" */
    int         width;           /**< 编码宽(给 mp4v2 建轨用) */
    int         height;          /**< 编码高 */
    uint64_t    limit_bytes;     /**< 环形容量上限(字节);0 = 不限 */
    int         limit_files;     /**< 环形文件数上限;0 = 不限 */
    int         segment_frames;  /**< 每段多少帧后切新文件;<=0 = 用默认(54000 ≈ 30 分钟) */
    int         raw_sidecar;     /**< 1 = 同时写旁路裸流侧车(掉电可救);0 = 不写 */
} svc_record_cfg_t;

/** 录制统计(用于日志与验收断言) */
typedef struct {
    uint64_t segments;        /**< 已写完(已 MP4Close)的分段数 */
    uint64_t frames_written;  /**< 已写进 MP4 的帧数 */
    uint64_t bytes_written;   /**< 已写进 MP4 的码流字节数 */
    uint64_t deleted;         /**< 环形覆盖删掉的分段数 */
    uint64_t oversized;       /**< 因超过 SVC_RECORD_MAX_NALU 被丢弃的 NALU 数 */
    uint64_t slot_errors;     /**< 槽位魔数不符(不是我们写的帧)的次数 */
    uint64_t write_errors;    /**< mp4v2 / 文件错误次数 */
    uint64_t raw_bytes;       /**< 写进旁路裸流侧车的字节数 */
    uint64_t raw_errors;      /**< 旁路裸流写失败次数(尽力而为, 不影响 MP4) */
    uint64_t disk_full;       /**< 写入失败且判定为"空间不足"的次数 */
    uint64_t write_retry_ok;  /**< 清理腾空间后重试成功的次数 */
    char     cur_name[64];    /**< 当前正在写的分段名(空 = 还没开) */
} svc_record_stats_t;

/**
 * @brief 启动录制服务。
 *
 * @param[in] cfg   配置(会**拷进内部**, 调用方不必保持存活)
 * @param[in] queue 录制队列(与发送队列**分开**的那个;由调用方创建并拥有)
 * @return 0 成功; 负值失败(参数非法 / 线程建不起来)
 *
 * @note 线程起来后**不会立刻建文件** —— 等第一帧到了才 `MP4Create`
 *       (否则会留下一个 0 帧的空 MP4)。
 * @note 重复调用(已启动)返回 0, 不做任何事。
 */
int svc_record_start(const svc_record_cfg_t *cfg, void *queue);

/**
 * @brief 停止录制:通知线程退出 → join → 线程收尾时 `MP4Close`(写 moov)。
 *
 * @note **这一句就是"优雅关闭"的全部意义** —— `MP4Close` 不执行, 当前分段就不可播。
 * @note 未启动时调用是安全的(no-op)。
 */
void svc_record_stop(void);

/** @brief 录制是否在运行。@return 1 = 在运行 */
int svc_record_is_running(void);

/** @brief 取统计快照。 */
void svc_record_get_stats(svc_record_stats_t *out);

#endif /* __SVC_RECORD_H__ */
