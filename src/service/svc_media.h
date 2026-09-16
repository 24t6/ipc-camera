/**
 * @file    svc_media.h
 * @brief   取流服务 —— 从 MPP 取一帧 → 解析 NALU → 写进发送队列
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它解决的问题(ADR-3)
 * ─────────────────────────────────────────────────────────────────
 *  **取流线程绝不能直接 sendto。** 原因:
 *    VENC 的编码缓冲有限, 一旦取流被网络拖住 → 缓冲满 → **编码器停止编码**
 *    (官方文档《HiMPP V4.0 媒体处理软件开发参考》原话:
 *     "码流缓冲区就会满……就会不再启动编码, 直到用户获取码流")。
 *  所以拆成:
 *
 *      MPP ──▶ 【本模块: 取流线程】 ──push──▶ 队列 ──pop──▶ 发送线程 ──▶ 网络
 *
 *  取流线程只做"取 → 解析 → 拷贝进队列 → 立刻 ReleaseStream",
 *  **绝不碰网络、绝不阻塞**。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⭐ 队里放什么?(一个关键设计决定)
 * ─────────────────────────────────────────────────────────────────
 *  `proto_queue`(infra_queue)的槽位是**一块字节缓冲**, 所以要自己定义
 *  槽内布局。我们放**一帧**(而不是一个 NALU), 并在前面加一个小头部:
 *
 *      ┌──────────────┬────────────────────────────────────┐
 *      │ 帧头(24 字节)│        这一段是 Annex-B 码流        │
 *      └──────────────┴────────────────────────────────────┘
 *        magic / len / cap / pts / session_id / frame_index
 *
 *  **为什么按帧而不是按 NALU**:
 *    RTP 的时间戳是"**每帧 +1**"(见第 4 课)。按帧入队, 槽头里就带上了
 *    这一帧的 `pts`, 发送端照着打时间戳即可 —— **不用自己数帧**。
 *    这正好落实 B012:时间戳来自编码器, 不来自"假设帧率"。
 *
 *  **为什么要把多个 pack 拷进一段连续内存**:
 *    官方文档明确:多包模式(默认)下**每个 pack 各有地址、不保证连续**
 *    (见 `bsp_mpp.h` 的 `bsp_mpp_pack_t` 说明)。
 *    队列槽位是一段连续内存, 所以这里**必须逐 pack 拷贝**拼起来。
 *    —— 这也是"取流线程做拷贝"的代价, 换来的是"运行期只在初始化分配内存"。
 *
 * @note 本模块**自己起一个线程**(取流线程), `svc_media_start` 返回后即在跑。
 * @note 线程所有权:MPP 通路由本线程独占(见 ARCHITECTURE.md 第四节),
 *       `bsp_mpp_*` 只在**本线程**里被调用。
 */
#ifndef __SVC_MEDIA_H__
#define __SVC_MEDIA_H__

#include <stdint.h>

/** 槽位里那个帧头的魔数。用来验证"取出来的是我们自己写的帧", 而不是垃圾 */
#define SVC_MEDIA_FRAME_MAGIC 0x4D464942u    /* "MFIB" */

/**
 * 一个队列槽位开头的帧头。
 *
 * @note 发送线程 `memcpy` 或强转读出它, 然后 `data` 就是紧随其后的码流。
 *
 * @note ⚠️ **不要手写这个结构体的大小** —— 用 `SVC_MEDIA_HDR_SIZE`(见下),
 *       它由编译器算出来。原因见那个宏的说明。
 */
typedef struct {
    uint32_t magic;        /**< 固定 SVC_MEDIA_FRAME_MAGIC, 校验用 */
    uint32_t len;          /**< 码流字节数 */
    uint32_t cap;          /**< 槽位容量(便于发送端判断有没有被截断) */
    uint32_t frame_index;  /**< 帧序号(从 0 开始递增。**不是** RTP 序列号) */
    uint64_t pts;          /**< 编码器给的时间戳。**单位 1 MHz**(实测, 见下) */
    uint32_t session_id;   /**< 预留: 将来多路流时可区分 */
    uint32_t reserved;     /**< 备用字段(不是"填充到 24 字节", 见下) */
} svc_media_frame_hdr_t;

/**
 * 槽位里帧头占的字节数 = `sizeof(svc_media_frame_hdr_t)`。
 *
 * @note ⚠️⚠️ **这里踩过一个坑, 完整因果链值得记住(2026-09-15)**:
 *
 *   **① 我算错了结构体大小。**
 *   原来手写 `#define SVC_MEDIA_HDR_SIZE 24`, 把 `4+4+4+4+8+4+4`
 *   心算成了 24, 实际是 **32**。
 *
 *   (⚠️ 我当时第一反应是把根因归给"编译器插了填充", **那是错的**。
 *    实测字段偏移是 `magic@0 len@4 cap@8 frame_index@12 pts@16
 *    session_id@24 reserved@28`, **没有任何填充**, sizeof 就是 32。
 *    纯粹是我加错了 —— 这个区别很重要, 否则下次遇到**真**填充时会套错解释。)
 *
 *   **② 于是帧头的"后半截"和码流**抢同一块内存**。**
 *   期望布局(常量 24):   [帧头 24 字节][码流从 24 开始……]
 *   实际布局(真实 32):   [magic len cap idx | pts | session_id | reserved]
 *                          ↑ 偏移 0        ↑16   ↑24          ↑28
 *   码流被写进了偏移 24, 而 `session_id` 恰好也住在偏移 24。
 *
 *   **③ `handle_one_frame` 里后写的那两行把码流头 8 字节清零了:**
 *         pack_into_slot(...)          // 先把码流写进 slot+24
 *         hdr->frame_index = ...;      // 偏移 12, 安全
 *         hdr->session_id  = 0;        // 偏移 24 → **把码流前 4 字节写成 0**
 *         (reserved 同理, 偏移 28 → 再写 4 个 0)
 *
 *   所以 `00 00 00 01 61 …` 被改成了 `00 00 00 00 61 …` ——
 *   **起始码的 `01` 被抹成了 `00`**, 解析器(正确地)再也找不到起始码。
 *
 *   **④ 现象**: 取到 206 帧、字节数/帧数/PTS 全部正常, 但整帧解析出
 *   **0 个 NALU** —— 表观上"206 帧只解析出 21 个 NALU"(只有关键帧那几个
 *   侥幸落在后面没被覆盖)。编译器和类型系统**都不会**报这个错。
 *
 *   **⑤ 教训(比这个 bug 本身更值钱)**:
 *     · **不要手写结构体大小**, 用 `sizeof` —— 现在这里就是。
 *     · 症状是"数据被改坏"时, 先查**自己数据结构有没有重叠**,
 *       别急着怀疑传输/硬件把字节弄坏了(**一个字节都没坏**)。
 *     · 现在 `pack_into_slot` 放在最后调用, 即使将来再算错, 也是
 *       帧头被码流覆盖(一眼看得出)而不是码流被静默毁掉。
 *
 * @note 帧头为什么不用 `#pragma pack` 压到更小: 压包会让 `pts` 变成
 *       **非对齐访问**, ARMv7 上要么变慢、要么触发对齐异常。
 *       为省几字节冒这个险不值得 —— 槽位本来就是 256 KB 级。
 *
 * @note ⚠️ **`pts` 的单位是 1 MHz, 不是 90 kHz**(2026-09-15 实测更正)。
 *       这里原来写的是"90kHz 单位", 那是照抄的一般说法, **没核实过**。
 *       板上实测: 相邻帧 PTS 间隔稳定在 33323~33343(标称 33333),
 *       按 `33333 × 30fps ≈ 999,990` 反推 → **1 MHz**。
 *       (90kHz/30fps 应当是 3000, 差 11 倍。)
 *       消费方(`proto_rtp`)必须按 `PROTO_RTP_PTS_HZ` 换算成 90kHz,
 *       **不能直接把这个值当 RTP 时间戳**。
 *       实测脚本: `ipc_camera/tools/media_smoke.c`(会打印标称间隔并反推时基)。
 */
#define SVC_MEDIA_HDR_SIZE ((int)sizeof(svc_media_frame_hdr_t))

/*
 * 编译期兜底: 帧头必须是 4 的倍数(强转读字段才安全), 且不能超过 64 字节
 * (槽位是按 256 KB 预算的, 帧头占太多没道理)。
 * 万一将来有人给结构体加了 `#pragma pack` 或塞进大数组, 这里会**直接编译失败**
 * —— 比等到板子上"码流解析不出来"再查要好得多。
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(svc_media_frame_hdr_t) % 4 == 0,
               "svc_media 帧头大小必须是 4 的倍数(强转读字段要求对齐)");
_Static_assert(sizeof(svc_media_frame_hdr_t) <= 64,
               "svc_media 帧头太大, 检查是否误加了字段");
#endif

/** 取流服务统计 —— 用于日志与验收断言 */
typedef struct {
    uint64_t frames;        /**< 累计取到并成功入队的帧数 */
    uint64_t bytes;         /**< 累计入队字节数 */
    uint64_t get_timeouts;  /**< "当时没帧"的次数(**不是错误**) */
    uint64_t get_errors;    /**< 取流出错次数 */
    uint64_t parse_rejects; /**< 因为"整帧里一个 NALU 都没有"而丢掉的帧 */
    uint64_t queue_dropped; /**< 队列满而丢掉的最旧帧数(来自 infra_queue) */
    uint64_t oversize;      /**< 因为超过槽位容量而丢掉的帧数 */
} svc_media_stats_t;

/**
 * @brief 启动取流服务。
 *
 * @param queue  发送队列(**由调用方创建并拥有**; 本模块只 push, 不销毁它)
 * @param is_h265 1 = 按 H.265 解析; 0 = H.264。**必须和 bsp_mpp 编的那一路一致**
 * @return 0 成功; 负值失败
 *
 * @note **阻塞**:会先 `bsp_mpp_init()`(约 5~10 秒), 再起线程。
 * @note 重复调用(已启动)返回 0 且不做任何事。
 */
int svc_media_start(void *queue, int is_h265);

/**
 * @brief 停止取流服务: 通知线程退出 → join → `bsp_mpp_deinit()`。
 *
 * @note **阻塞**, 会等线程真正退出(避免 use-after-free)。
 * @note 未启动时调用是安全的(no-op)。
 */
void svc_media_stop(void);

/** @brief 服务是否在运行。@return 1 = 在运行 */
int svc_media_is_running(void);

/** @brief 取统计快照。 */
void svc_media_get_stats(svc_media_stats_t *out);

/**
 * @brief 从队列槽位里取出帧头指针。
 *
 * @param slot 从队列 pop 出来的槽位(开头就是帧头)
 * @return 帧头指针; 槽位内容不是我们写的帧时返回 NULL(魔数不符)
 *
 * @note 给**发送线程**用的一个小工具, 免得它也去算偏移、还得记得校验魔数。
 *       放在本模块是因为"槽内布局"是本模块定义的, 别处不该重复这个知识。
 */
const svc_media_frame_hdr_t *svc_media_slot_hdr(const void *slot);

/** @brief 取槽位里的码流数据指针(紧跟帧头)。 */
const uint8_t *svc_media_slot_data(const void *slot);

#endif /* __SVC_MEDIA_H__ */
