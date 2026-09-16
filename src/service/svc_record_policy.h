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
 */
int svc_record_policy_plan_delete(const svc_record_policy_file_t *files, int count,
                              const svc_record_policy_limits_t *lim,
                              int *del_idx, int del_cap);

#endif /* __SVC_RECORD_POLICY_H__ */
