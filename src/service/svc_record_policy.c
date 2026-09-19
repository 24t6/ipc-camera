/**
 * @file    svc_record_policy.c
 * @brief   录制策略的纯逻辑实现(文件名 + 环形覆盖决策)
 *
 * 【模块职责】生成分段文件名; 从文件列表里挑出"该删哪些"
 * 【依赖方向】只依赖 libc —— **不碰 mp4v2、不碰文件系统、不做 I/O**
 * 【线程模型】无状态全局、纯函数; 可在多线程中并发调用
 * 【资源边界】无动态分配; 栈数组 order[SVC_RECORD_POLICY_MAX_FILES] ≤ 256 字节
 *
 * 设计理由(为什么补零、为什么至少留一个)见 `svc_record_policy.h`。
 */
#include "svc_record_policy.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief 按下标比较两个文件谁更旧(名字字典序)
 *
 * @param[in] files 文件列表
 * @param[in] a     下标
 * @param[in] b     下标
 * @return 1 = a 比 b 旧; 0 = 否则
 *
 * @note 抽出来是为了让调用处的那行**短到不用续行** ——
 *       深续行(对齐到函数参数)会被 `check_style.py` 当成"缩进 10 层 > 5"。
 *       同理见 `bsp_osd.c` 里 `bsp_osd_show` 的处理。
 */
static int name_is_older(const svc_record_policy_file_t *files, int a, int b)
{
    return strcmp(files[a].name, files[b].name) < 0 ? 1 : 0;
}

/**
 * @brief 按下标数组把文件按**名字字典序**排好(最旧在前)
 *
 * @param[in]  files 文件列表
 * @param[in]  count 个数
 * @param[out] idx   下标数组(进来时是 0..count-1)
 *
 * @note 用插入排序而不是 `qsort`:个数 ≤ 64, 而 `qsort` 要传函数指针、
 *       还得多引一个头;这点规模不值得。而且**顺序本来就接近有序**
 *       (目录通常按创建时间返回), 插入排序在这种输入上是 O(n)。
 * @note 名字是补零的固定宽度 → **字典序 == 时间序**, 所以 `strcmp` 就够。
 */
static void sort_idx_by_name(const svc_record_policy_file_t *files, int count, int *idx)
{
    int i;
    int j;
    int key;

    for (i = 1; i < count; i++) {
        key = idx[i];
        for (j = i - 1; j >= 0 && name_is_older(files, key, idx[j]); j--) {
            idx[j + 1] = idx[j];
        }
        idx[j + 1] = key;
    }
}

int svc_record_policy_make_name(const struct tm *tmv, char *out, size_t cap)
{
    int n;

    if (tmv == NULL || out == NULL || cap == 0) {
        return -1;
    }
    /* ⚠️ 一律 %0Nd 补零 —— 字典序必须等于时间序(环形覆盖靠它找最旧) */
    n = snprintf(out, cap, "%04d-%02d-%02d-%02d-%02d-%02d%s",
                 tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                 tmv->tm_hour, tmv->tm_min, tmv->tm_sec, SVC_RECORD_POLICY_EXT);
    if (n <= 0 || (size_t)n >= cap) {
        return -1;              /* 放不下(或被截断)—— 明确失败, 不返回半截名字 */
    }
    return n;
}

int svc_record_policy_plan_delete(const svc_record_policy_file_t *files, int count,
                              const svc_record_policy_limits_t *lim,
                              int *del_idx, int del_cap)
{
    int      order[SVC_RECORD_POLICY_MAX_FILES];
    uint64_t total = 0;
    int      kept  = count;
    int      i;
    int      n = 0;

    if (files == NULL || lim == NULL || del_idx == NULL || del_cap <= 0) {
        return -1;
    }
    if (count < 0 || count > SVC_RECORD_POLICY_MAX_FILES) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        order[i] = i;
        total += files[i].size;
    }
    sort_idx_by_name(files, count, order);

    for (i = 0; i < count; i++) {           /* order[0] 最旧 */
        int over_bytes;
        int over_files;

        if (kept <= 1) {
            break;                          /* ★ 永远至少留一个(见头文件说明) */
        }
        over_bytes = (lim->limit_bytes > 0 && total > lim->limit_bytes);
        over_files = (lim->limit_files > 0 && kept > lim->limit_files);
        if (!over_bytes && !over_files) {
            break;                          /* 已经都在上限内, 不用再删 */
        }
        if (n >= del_cap) {
            break;                          /* 输出缓冲满 —— 剩下的下一轮再删 */
        }
        if (files[order[i]].locked) {
            /* ★ 锁定的分段跳过, **但它仍然占着容量** ——
             *   继续看下一个最旧的(不 break), 否则"有一个锁就谁也不删" */
            continue;
        }
        del_idx[n++] = order[i];
        total -= files[order[i]].size;
        kept--;
    }
    return n;
}

int svc_record_policy_should_close(uint32_t frames_in_seg, uint32_t segment_frames,
                                   uint64_t bytes_in_seg, uint64_t segment_bytes)
{
    if (segment_frames > 0 && frames_in_seg >= segment_frames) {
        return 1;               /* 时间(帧数)到了 */
    }
    if (segment_bytes > 0 && bytes_in_seg >= segment_bytes) {
        return 1;               /* 大小到了 */
    }
    return 0;
}

void svc_record_policy_sort(svc_record_policy_file_t *files, int count, int desc)
{
    int i;
    int j;

    if (files == NULL || count <= 1) {
        return;
    }
    for (i = 1; i < count; i++) {
        svc_record_policy_file_t key = files[i];

        for (j = i - 1; j >= 0; j--) {
            int cmp = strcmp(files[j].name, key.name);

            /* desc(新→旧)时把"谁该排前面"反过来; 相等就保持原顺序(稳定) */
            if (desc ? (cmp < 0) : (cmp > 0)) {
                files[j + 1] = files[j];
                continue;
            }
            break;
        }
        files[j + 1] = key;
    }
}

/**
 * @brief 这一行(长度 len)是不是 `name`(容忍行尾的 '\r' 与空格)
 *
 * @param[in] line 行首
 * @param[in] len  行长度(不含换行)
 * @param[in] name 要比的名字
 * @return 1 = 是; 0 = 不是
 */
static int line_is_name(const char *line, size_t len, const char *name)
{
    size_t n = strlen(name);

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                       line[len - 1] == '\t')) {
        len--;                          /* 裁掉行尾空白 */
    }
    return (len == n && memcmp(line, name, n) == 0) ? 1 : 0;
}

int svc_record_policy_lock_has(const char *buf, size_t used, const char *name)
{
    size_t r = 0;

    if (buf == NULL || name == NULL || name[0] == '\0') {
        return 0;
    }
    while (r < used) {
        size_t start = r;

        while (r < used && buf[r] != '\n') {
            r++;
        }
        if (line_is_name(buf + start, r - start, name)) {
            return 1;
        }
        r++;                            /* 跳过换行 */
    }
    return 0;
}

int svc_record_policy_lock_edit(char *buf, size_t cap, size_t used,
                                const char *name, int on)
{
    size_t r;
    size_t w;

    if (buf == NULL || name == NULL || name[0] == '\0' || used >= cap) {
        return -1;
    }
    /* 名字里带换行 = 能往清单里注入额外的行 —— 直接拒绝 */
    if (strpbrk(name, "\r\n") != NULL) {
        return -1;
    }
    if (on) {
        size_t n = strlen(name);

        if (svc_record_policy_lock_has(buf, used, name)) {
            return (int)used;           /* 幂等: 已经在里面了 */
        }
        /* 末尾不是换行就先补一个(文件最后一行也要有换行, 便于逐行读) */
        if (used > 0 && buf[used - 1] != '\n') {
            if (used + 1 >= cap) {
                return -1;
            }
            buf[used++] = '\n';
        }
        if (used + n + 2 > cap) {
            return -1;                  /* 放不下(含结尾的 '\n' 与 '\0') */
        }
        memcpy(buf + used, name, n);
        buf[used + n] = '\n';
        buf[used + n + 1] = '\0';
        return (int)(used + n + 1);
    }

    /* 删除: 逐行搬, 把不是 name 的行留下 */
    r = 0;
    w = 0;
    while (r < used) {
        size_t start = r;

        while (r < used && buf[r] != '\n') {
            r++;
        }
        if (!line_is_name(buf + start, r - start, name)) {
            size_t len = r - start;

            memmove(buf + w, buf + start, len);
            w += len;
            buf[w++] = '\n';
        }
        r++;
    }
    buf[w] = '\0';
    return (int)w;
}
