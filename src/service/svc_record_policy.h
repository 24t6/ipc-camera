/**
 * @file    svc_record_policy.h
 * @brief   录制策略 —— **纯函数**:分段文件名 + 环形覆盖该删哪些
 *
 * 【模块职责】决定"文件叫什么名"和"容量满了删哪些", 不含任何 I/O 与 mp4v2 调用
 * 【依赖方向】只依赖 libc(<string.h> / <stdio.h> / <time.h>)—— 不碰 mp4v2、不碰文件系统
 * 【线程模型】无状态全局、纯函数; 调用方自带缓冲, 可在多线程中并发使用
 * 【资源边界】无动态分配; 内部栈数组 order[] 最大 64×4 = 256 字节
 *
 * @note **为什么把这一层单独拆出来**:它是 M3 里**唯一能在 PC 上原生单测**的部分
 *       (和 `bsp_osd_render` 同一个手法)。mp4v2 的调用只能上板验,
 *       但"文件名对不对""该删哪些"是纯逻辑 —— 用 PC 单测覆盖掉, 板上就少一类要查的错。
 *
 * @note ⚠️ **文件名必须补零**(`%02d`), 这不是美观问题:
 *       环形覆盖靠**字典序 == 时间序**来找"最旧的文件"。
 *       厂商 sample 用的是 `%d-%d-%d-...`, 于是 `2026-9-16-...` 与 `2026-10-1-...`
 *       按字典序**排反**(`1` < `9`), 会删错文件。这里修正为补零。
 */
#ifndef __SVC_RECORD_POLICY_H__
#define __SVC_RECORD_POLICY_H__

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/** 分段文件名的最大字节数(含结尾 '\0')。`2026-09-16-21-05-03.mp4` = 23 字节 */
#define SVC_RECORD_POLICY_NAME_MAX 64

/** 一次决策最多处理多少个文件(超过返回 -1, 不做部分处理) */
#define SVC_RECORD_POLICY_MAX_FILES 64

/** 分段文件的后缀名 */
#define SVC_RECORD_POLICY_EXT ".mp4"

/** 目录里一个分段文件的描述 */
typedef struct {
    char     name[SVC_RECORD_POLICY_NAME_MAX];  /**< 文件名(只比较, 不解析) */
    uint64_t size;                          /**< 字节数 */
    int      locked;                        /**< 1 = 被用户**锁定**, 环形覆盖必须跳过它 */
} svc_record_policy_file_t;

/** 环形覆盖的两个上限。**填 0 表示该项不限** */
typedef struct {
    uint64_t limit_bytes;  /**< 所有分段的总字节上限 */
    int      limit_files;  /**< 分段个数上限 */
} svc_record_policy_limits_t;

/**
 * @brief 按"起始时间"生成分段文件名, 形如 `年-月-日-时-分-秒.mp4`
 *
 * @param[in]  tmv 该段的起始本地时间
 * @param[out] out 输出缓冲
 * @param[in]  cap 缓冲容量
 * @return 写出的字符数(不含 '\0'); <=0 失败(参数非法 / 放不下)
 *
 * @note ⚠️ **各字段一律补零到固定宽度** —— 这样**字典序就等于时间序**,
 *       环形覆盖才能靠 `strcmp` 找出最旧的文件。补零不是美观问题(见文件头说明)。
 */
int svc_record_policy_make_name(const struct tm *tmv, char *out, size_t cap);

/**
 * @brief 环形覆盖决策:从**最旧**开始挑出要删的文件
 *
 * @param[in]  files   目录里的分段文件(顺序无所谓, 函数内部会按名字排序)
 * @param[in]  count   文件个数(0 ~ `SVC_RECORD_POLICY_MAX_FILES`)
 * @param[in]  lim     两个上限(都为 0 时返回 0 = 不删)
 * @param[out] del_idx 输出:要删的下标,**按"该删的先后"排列**(最旧在前)
 * @param[in]  del_cap `del_idx` 能放几个
 * @return >=0 要删的个数; <0 参数非法
 *
 * @note ★ **永远至少留一个文件**(最新的那个)。两条理由:
 *       ① 正在写的那个文件**绝不能删** —— 而它恰好是最新的(名字就是当前时间);
 *       ② 上限被设得比单个分段还小时(**配置错误**), 删光会导致"一个文件都不剩",
 *          比"略微超限"糟得多。
 *       所以:当只剩 1 个时停止, 即使这样仍超限。
 *
 * @note 只**给出决策**, 不执行删除 —— 删文件是 I/O, 由调用方做。
 *       这样这个函数保持纯的、可 PC 单测。
 *
 * @note ★ **被锁定的分段永不删除**(`files[i].locked == 1`)—— 这是监控行业的通用语义:
 *       海康:检索结果可**锁定, 锁定后不会被覆盖**;ISAPI 的 `mediaSegmentDescriptor`
 *       带 `lockStatus` 字段。跳过锁定的那一个之后,**继续看下一个最旧的**,
 *       而不是"遇到锁就放弃清理"。
 * @note ⚠️ 因此会出现"**能删的都删完了还是超限**"的情况(全被锁定)⇒ 本函数只返回
 *       实际能删的个数, **不做自动解锁**:锁代表用户的意图, 程序不替他改。
 *       调用方(见 `svc_record.c` 的 `apply_limits()`)在"超限却无段可删"时**明确报错**。
 */
int svc_record_policy_plan_delete(const svc_record_policy_file_t *files, int count,
                              const svc_record_policy_limits_t *lim,
                              int *del_idx, int del_cap);

/**
 * @brief 分段"该收了吗" —— **时间到 或 大小到, 谁先到算谁**(纯函数)
 *
 * @param[in] frames_in_seg   本段**已真正写进文件**的帧数
 * @param[in] segment_frames  帧数上限;**0 = 不限**
 * @param[in] bytes_in_seg    本段**已真正写进文件**的码流字节数
 * @param[in] segment_bytes   字节数上限;**0 = 不限**
 * @return 1 = 该收段了; 0 = 继续写
 *
 * @note ★ **为什么要有大小这一维**:TF 卡是 vfat(FAT32), **单文件硬上限 4 GiB**。
 *       只按时间切时, 段大小是"时长 × 码率"的结果 —— 而码率会随画面内容浮动
 *       (静态画面约 4 Mbps, 暗光/噪声大的画面能翻好几倍)。30 分钟段一旦超过 4 GiB,
 *       写入就会在**段中间**失败。加一条大小上限, 让文件大小有**确定性上界**。
 * @note 两个条件都是 `>=`:`should_close` 由调用方在**写完这一帧之后**调用,
 *       "刚好到达上限"也该收段。
 * @note ⚠️ **它只回答"该收了", 不决定"什么时候收"** —— 真正的切段必须等
 *       下一个关键帧(IDR), 否则新段建不了轨、刀口上还会丢帧(见 B033 与
 *       `svc_record.c` 的 `handle_slot()`)。所以实际段长会比上限**多出一个 GOP 的零头**。
 * @note 传入的必须是**产物计数**(真正落盘的帧/字节), 不是"处理过多少"
 *       —— 口径错了这个函数就是在给假数据下判断(B033 的教训)。
 */
int svc_record_policy_should_close(uint32_t frames_in_seg, uint32_t segment_frames,
                                   uint64_t bytes_in_seg, uint64_t segment_bytes);

/**
 * @brief 按名字(= 时间)排序分段数组
 *
 * @param[in,out] files 分段数组
 * @param[in]     count 个数
 * @param[in]     desc  1 = **新 → 旧**(回放列表要这个);0 = 旧 → 新(环形覆盖要这个)
 *
 * @note 名字是补零的固定宽度 ⇒ **字典序 == 时间序**, 所以 `strcmp` 就够。
 * @note 用插入排序:个数 ≤ 64, 而且顺序通常已接近有序(目录按创建时间返回)。
 */
void svc_record_policy_sort(svc_record_policy_file_t *files, int count, int desc);

/**
 * @brief 在锁定清单**文本**里加入/移除一个名字(纯函数, 就地改文本)
 *
 * @param[in,out] buf  清单全文(以 '\0' 结尾), 由调用方提供并拥有
 * @param[in]     cap  缓冲总容量(含结尾 '\0')
 * @param[in]     used 当前有效长度
 * @param[in]     name 要加/删的分段名
 * @param[in]     on   1 = 加入;0 = 移除
 * @return 新的有效长度; -1 = 参数非法或放不下(缓冲**未被修改**)
 *
 * @note 已经在清单里再加一次 = 原样返回(幂等);不在清单里删一次也一样。
 * @note 只做**文本**操作 —— 读文件/写文件在 `svc_record.c`, 这样这段逻辑能在 PC 上单测。
 * @note ⚠️ 名字里不允许出现换行(`\n` / `\r`)—— 否则一行能变成两行,
 *       清单就被注入了。出现换行时返回 -1。
 */
int svc_record_policy_lock_edit(char *buf, size_t cap, size_t used,
                                const char *name, int on);

/**
 * @brief 名字在不在锁定清单文本里(逐行比较)
 *
 * @param[in] buf  清单全文(以 '\0' 结尾;长度以 `used` 为准)
 * @param[in] used 有效长度
 * @param[in] name 名字
 * @return 1 = 在; 0 = 不在(或参数非法)
 *
 * @note 忽略空行与 `#` 开头的注释行;行尾的 `\r`/空格会被容忍。
 */
int svc_record_policy_lock_has(const char *buf, size_t used, const char *name);

#endif /* __SVC_RECORD_POLICY_H__ */
