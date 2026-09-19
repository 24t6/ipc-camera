/**
 * @file    svc_record.c
 * @brief   MP4 录制服务实现(mp4v2 封装 + 分段 + 环形覆盖)
 *
 * 【模块职责】录制线程:从队列取帧 → 切 NALU → 写 MP4;满一段就切新文件;超限删最旧
 * 【依赖方向】依赖 infra_queue / infra_log / proto_nalu / svc_media / svc_record_policy
 *             与 mp4v2;不依赖 bsp / svc_net / svc_sender
 * 【线程模型】自己起 1 个线程(record_thread, 线程名 `ipc_rec`);独占 mp4v2 句柄
 * 【资源边界】无自行动态分配;文件级静态:槽位 ~256KB、MP4 写缓冲 ~256KB、扫描表 ~5KB
 *
 * 复用出处、只支持 H.264 的原因、以及"强杀后当前段不可播"的解释, 见 `svc_record.h`。
 */
#include "svc_record.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/*
 * ⚠️ 这里**故意不** `#include <mp4v2/mp4v2.h>` —— 它的头文件是 C++ 的
 *    (`= nullptr` + `DEFAULT()` 宏), 从 C 里编不过。
 *    改用我们自己的 C 接口垫片, 理由与出处见 `svc_record_mp4.h`。
 */
#include "svc_record_mp4.h"

#include "infra_log.h"
#include "infra_queue.h"
#include "proto_nalu.h"
#include "svc_media.h"
#include "svc_record_policy.h"

/** 取流槽位容量 = 帧头 + 单帧最大字节(与发送侧同一口径) */
#define SVC_RECORD_SLOT_BYTES (SVC_MEDIA_HDR_SIZE + SVC_RECORD_MAX_NALU)

/** 队列 pop 的等待(毫秒)。超时返回 1 = 队列空, 不是错误 */
#define SVC_RECORD_POP_WAIT_MS 200

static struct {
    int              running;
    pthread_t        thread;
    int              thread_valid;
    volatile int     stop_requested;

    infra_queue_t   *queue;                       /* 不拥有:由调用方创建/销毁 */
    char             dir[SVC_RECORD_PATH_MAX];    /* 拷一份, 不引用调用方的串 */
    int              width;
    int              height;
    uint64_t         limit_bytes;
    int              limit_files;
    int              segment_frames;

    MP4FileHandle    mp4;                         /* NULL = 当前没有开着的分段 */
    MP4TrackId       track;
    int              have_sps;
    int              have_pps;
    int              frames_in_seg;
    int              want_close;                  /* 1 = 已达段长, 等下一个 IDR 再切 */
    char             cur_name[SVC_RECORD_POLICY_NAME_MAX];

    /* ★ 分段的三条路径 + 旁路裸流(生命周期见 svc_record.h 顶部那段注释) */
    char             path_final[SVC_RECORD_PATH_MAX];  /* <dir>/<stamp>.mp4     */
    char             path_tmp[SVC_RECORD_PATH_MAX];    /* <dir>/<stamp>.mp4.tmp */
    char             path_raw[SVC_RECORD_PATH_MAX];    /* <dir>/<stamp>.h264.tmp */
    FILE            *raw;                              /* NULL = 没开侧车 */
    int              raw_sidecar;                      /* 1 = 写侧车 */
    uint64_t         max_seg_bytes;                     /* 见过的最大段(=预留依据) */
    int              alerted_ro;                        /* 1 = 已经报过"卡变只读" */
    int              alerted_tiny;                      /* 1 = 已经报过"盘比预留还小" */

    svc_record_stats_t stats;
} g;

/*
 * 取流槽位缓冲(从录制队列 pop 到这里)
 *   容量依据 : 帧头 32 字节 + 单帧最大 256KB
 *   内存区域 : 文件级静态(.bss) —— 不放栈(远超 4KB 规矩)
 *   唯一所有者: 本模块
 *   释放时机 : 进程生命周期内常驻
 */
static uint8_t g_slot[SVC_RECORD_SLOT_BYTES];

/*
 * MP4 写缓冲:4 字节长度前缀 + 一个 NALU
 *   容量依据 : MP4 里每个 NALU 前面要放 **4 字节大端长度**(不是 Annex-B 起始码)
 *   内存区域 : 文件级静态(.bss)
 *   唯一所有者: 本模块
 *   释放时机 : 进程生命周期内常驻
 */
static uint8_t g_mp4buf[4 + SVC_RECORD_MAX_NALU];

/** 恢复残留裸流时每次读的块大小 */
#define SVC_RECOVER_CHUNK (256 * 1024)

/*
 * 恢复残留裸流用的块缓冲
 *   容量依据 : 一块 SVC_RECOVER_CHUNK + 一个最大 NALU(跨块的不完整 NALU 要留到下一轮)
 *   内存区域 : 文件级静态(.bss) —— 约 512KB, 远低于 128MB 内存
 *   唯一所有者: 本模块
 *   释放时机 : 进程生命周期内常驻
 */
static uint8_t g_rec_buf[SVC_RECOVER_CHUNK + SVC_RECORD_MAX_NALU];

/*
 * 环形覆盖的目录扫描表
 *   容量依据 : 最多同时管理 64 个分段(策略层的上限)
 *   内存区域 : **文件级静态** —— 若放栈上, 64 × 72 字节 ≈ 4.6KB, 会超 4KB 规矩
 *   唯一所有者: 本模块
 *   释放时机 : 进程生命周期内常驻
 */
static svc_record_policy_file_t g_scan[SVC_RECORD_POLICY_MAX_FILES];
static int                      g_del[SVC_RECORD_POLICY_MAX_FILES];

/* ─────────── 文件名/路径小工具 ─────────── */

/**
 * @brief 名字是不是本模块关心的分段文件(`.mp4` 结尾)
 *
 * @param[in] name 文件名
 * @return 1 = 是; 0 = 不是
 *
 * @note ★ 这里**故意只认 `.mp4`** —— 于是 `<stamp>.mp4.tmp`(正在写)与
 *       `<stamp>.h264.tmp`(旁路裸流)**都不算分段**:不进环形容量统计、
 *       也不会被当成可回放的录像列出来。这是"正在写的段是显式状态"的实现方式。
 */
static int name_is_mp4(const char *name)
{
    size_t n = strlen(name);
    size_t e = strlen(SVC_RECORD_POLICY_EXT);

    return (n > e && strcmp(name + n - e, SVC_RECORD_POLICY_EXT) == 0) ? 1 : 0;
}

/**
 * @brief 拼出分段文件的完整路径
 *
 * @param[in]  name 文件名
 * @param[out] out  输出缓冲
 * @param[in]  cap  容量
 * @return 写入的字符数; <=0 = 放不下
 */
static int make_path(const char *name, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", g.dir, name);

    return (n > 0 && (size_t)n < cap) ? n : -1;
}

/**
 * @brief 取一个分段文件的字节数(stat)
 *
 * @param[in] name 文件名
 * @return 字节数; 0 = 取不到(文件没了 / stat 失败)
 */
static uint64_t file_size_of(const char *name)
{
    char          path[SVC_RECORD_PATH_MAX];
    struct stat   st;

    if (make_path(name, path, sizeof(path)) <= 0) {
        return 0;
    }
    return (stat(path, &st) == 0) ? (uint64_t)st.st_size : 0;
}

/* ─────────── 环形覆盖:扫描目录 + 执行删除 ─────────── */

/**
 * @brief 扫描录制目录, 收集所有分段文件(排除**正在写的那个**)
 *
 * @param[out] files 输出表
 * @param[in]  cap   表容量
 * @return 收集到的个数; 负值 = 目录打不开
 *
 * @note ⚠️ **必须排除正在写的那个**。策略层虽然保证"至少留最新的一个",
 *       但那是**按名字排序**推断的;这里直接按名字比对排除, 更硬。
 */
static int scan_dir(svc_record_policy_file_t *files, int cap)
{
    DIR           *d;
    struct dirent *e;
    int            n = 0;

    d = opendir(g.dir);
    if (d == NULL) {
        return -1;
    }
    while ((e = readdir(d)) != NULL && n < cap) {
        if (!name_is_mp4(e->d_name) || strcmp(e->d_name, g.cur_name) == 0) {
            continue;
        }
        snprintf(files[n].name, sizeof(files[n].name), "%s", e->d_name);
        files[n].size = file_size_of(e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

/**
 * @brief 按给定上限**真的删文件**(决策来自纯函数 `svc_record_policy`)
 *
 * @param[in] lim 上限(总字节 / 文件数)
 * @return 实际删掉的个数
 *
 * @note 只在这一处"决策 + 执行"交界处做 I/O;决策本身是纯函数。
 */
static int apply_limits(const svc_record_policy_limits_t *lim)
{
    char path[SVC_RECORD_PATH_MAX];
    int  cnt;
    int  n;
    int  i;
    int  done = 0;

    cnt = scan_dir(g_scan, SVC_RECORD_POLICY_MAX_FILES);
    if (cnt <= 0) {
        return 0;
    }
    n = svc_record_policy_plan_delete(g_scan, cnt, lim, g_del,
                                      SVC_RECORD_POLICY_MAX_FILES);
    for (i = 0; i < n; i++) {
        if (make_path(g_scan[g_del[i]].name, path, sizeof(path)) <= 0) {
            continue;
        }
        if (unlink(path) == 0) {
            g.stats.deleted++;
            done++;
            LOG_INFO("录制: 环形覆盖删除最旧分段 %s(%llu 字节)",
                     g_scan[g_del[i]].name,
                     (unsigned long long)g_scan[g_del[i]].size);
        }
    }
    return done;
}

/**
 * @brief 执行环形覆盖:超上限就删最旧的分段
 *
 * @note 触发点:每次**分段收尾**(`close_segment()`)。
 */
static void enforce_limits(void)
{
    svc_record_policy_limits_t lim;

    if (g.limit_bytes == 0 && g.limit_files == 0) {
        return;                                  /* 两个都不限 = 不管 */
    }
    lim.limit_bytes = g.limit_bytes;
    lim.limit_files = g.limit_files;
    apply_limits(&lim);
}

/**
 * @brief 预留多少空间(= "一段大小"的估计 + 余量)
 *
 * @return 字节数
 *
 * @note ★ **取"见过的最大段"而不是"上一段"**:同样时长的段, 大小会随画面复杂度浮动
 *       (静态画面 4 Mbps, 噪声大的画面能翻好几倍)。用上一段当预留会**偏紧** ——
 *       实测在 24 MB 小盘上, "上一段大小"当预留时每段末尾都撞一次 ENOSPC(75 秒里
 *       876 次写失败)。改用历史最大值, 再留 25% 余量。
 * @note 一次都没写过时用 `SVC_RECORD_HEADROOM_MIN`。
 */
static uint64_t headroom_bytes(void)
{
    uint64_t base = (g.max_seg_bytes > 0) ? g.max_seg_bytes
                                          : SVC_RECORD_HEADROOM_MIN;

    if (g.raw_sidecar) {
        /* ★ 开了侧车, "写一段"期间要同时占 **两份**地方(MP4 + 裸流) ——
         *   预留必须跟着翻倍, 否则会在段末尾撞 ENOSPC(实测 24 MB 小盘上必现)。 */
        base *= 2;
    }
    return base + base / 4;
}

/**
 * @brief 记住这一段的大小(只增不减, 作为下一段的预留依据)
 *
 * @note 在 `close_segment()` 里、`rename` 之后调用(那时文件已经是正式名)。
 */
static void note_seg_size(void)
{
    uint64_t sz = file_size_of(g.cur_name);

    if (sz > g.max_seg_bytes) {
        g.max_seg_bytes = sz;
    }
}

/**
 * @brief 保证文件系统至少还剩"一段大小"的可用空间(不够就提前删最旧)
 *
 * @note ★ 为什么这么做:清理原本只在**分段收尾**时发生, 于是"写到一半盘满"会一直
 *       失败到这段结束(默认最多 30 分钟)。提前把空间腾出来, 让**新段一开始就有
 *       一整段的空间**, 从源头减少 ENOSPC。
 * @note 手法:`statvfs` 拿到容量, 把 `容量 − 预留` 当成"总字节上限"交给纯函数 ——
 *       效果就是"删到剩余 ≥ 预留", 而且**决策仍然在纯函数里**(可 PC 单测)。
 * @note ⚠️ 预留数值**行业没有权威出处**(见 `SVC_RECORD_HEADROOM_MIN` 的说明)。
 */
static void ensure_headroom(void)
{
    svc_record_policy_limits_t lim;
    struct statvfs             vfs;
    uint64_t                   headroom = headroom_bytes();
    uint64_t                   cap;
    uint64_t                   avail;

    if (headroom == 0 || statvfs(g.dir, &vfs) != 0) {
        return;                                  /* 查不到就不管, 别把录制停了 */
    }
    cap   = (uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize;
    avail = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    if (avail >= headroom) {
        return;
    }
    if (cap <= headroom) {
        /* 盘比"一段"还小 —— 预留无从谈起。**不删**, 让"写不进去"如实报出来,
         * 而不是把仅有的几段也删光(注意: 策略层把 limit_bytes==0 当"不限",
         * 所以这里必须提前返回, 不能靠"算出来是 0"来表达"不删")。 */
        if (!g.alerted_tiny) {
            g.alerted_tiny = 1;             /* 每帧都会走到这里, 只报一次 */
            LOG_ERROR("录制: 磁盘容量 %llu 字节比预留 %llu 字节还小, 无法预留",
                      (unsigned long long)cap, (unsigned long long)headroom);
        }
        return;
    }
    lim.limit_bytes = cap - headroom;
    lim.limit_files = 0;
    /* ★ 只在**真的删掉了东西**时才打日志 —— 否则失败路径每帧都会刷一行(实测刷了 1392 行) */
    if (apply_limits(&lim) > 0) {
        LOG_ERROR("录制: 磁盘剩余 %llu 字节 < 预留 %llu 字节 → 已提前清理最旧分段",
                  (unsigned long long)avail, (unsigned long long)headroom);
    }
}

/* ─────────── 旁路裸流侧车 + 残留清理 ─────────── */

/**
 * @brief 开旁路裸流侧车(尽力而为;开不了也要继续录 MP4)
 *
 * @note 用 `_IONBF` **不缓冲**:侧车的全部意义就是"进程突然没了, 文件里也有东西",
 *       缓冲会让它白写。数据仍会经过内核页缓存(见 `write_raw_frame()` 的【简化上限】)。
 */
static void open_raw_sidecar(void)
{
    if (!g.raw_sidecar || g.path_raw[0] == '\0') {
        return;
    }
    g.raw = fopen(g.path_raw, "wb");
    if (g.raw == NULL) {
        g.stats.raw_errors++;
        LOG_ERROR("录制: 裸流侧车打不开: %s", g.path_raw);
        return;
    }
    (void)setvbuf(g.raw, NULL, _IONBF, 0);
}

/**
 * @brief 关掉旁路裸流侧车(可选顺手删掉)
 *
 * @param[in] unlink_it 1 = 同时删掉文件(收尾时用);0 = 只关不删
 */
static void close_raw_sidecar(int unlink_it)
{
    if (g.raw == NULL) {
        return;
    }
    fclose(g.raw);
    g.raw = NULL;
    if (unlink_it && g.path_raw[0] != '\0') {
        (void)unlink(g.path_raw);       /* 删不掉也只是多占一份, 不算错 */
    }
}

/**
 * @brief 把整帧 Annex-B 追加进旁路裸流侧车
 *
 * @param[in] data 整帧的 Annex-B 码流(槽位里那段)
 * @param[in] len  字节数
 *
 * @note 与 MP4 里写的**是同一份码流**, 只是不加长度前缀(裸流靠起始码自定界)——
 *       所以 MP4 被截断就打不开, 而这份**截断也能解**。
 * @note 尽力而为:侧车写失败**不影响 MP4 录制**, 只累加 `raw_errors`。
 *
 * @note 【简化上限】**不调 `fsync`/`fdatasync`**, 数据只到内核页缓存
 *       (Linux 默认 30 秒左右回写)⇒ **断电最多丢约 30 秒的侧车**, 而不是整段。
 *       天花板:更短的丢失窗口需要周期 `fdatasync`(每秒一次即可),
 *       但那会在录制线程里引入可能长达数百毫秒的阻塞(卡上 fsync 慢),
 *       有拖慢录制、撑满队列的风险。**升级路径**:把侧车写到一个独立的小线程里,
 *       再由它按秒 `fdatasync`, 这样录制线程一行都不用改。
 */
static void write_raw_frame(const uint8_t *data, uint32_t len)
{
    if (g.raw == NULL || len == 0) {
        return;
    }
    if (fwrite(data, 1, len, g.raw) != len) {
        g.stats.raw_errors++;
        return;
    }
    g.stats.raw_bytes += len;
}

/**
 * @brief 名字是不是旁路裸流侧车(`.h264.tmp` 结尾)
 *
 * @param[in] name 文件名
 * @return 1 = 是; 0 = 不是
 */
static int is_raw_sidecar(const char *name)
{
    size_t n = strlen(name);

    return (n > 9 && strcmp(name + n - 9, ".h264.tmp") == 0) ? 1 : 0;
}

/**
 * @brief 删掉录制目录里残留的 `*.mp4.tmp`(上次异常退出留下的半成品)
 *
 * @note ⚠️ **只删 MP4 的半成品, 不碰裸流侧车** —— 侧车会被
 *       `recover_leftovers()` **重新封装成能播的 MP4**(那才是它存在的意义)。
 * @note 启动时调一次(在录制线程起来之前), 避免与正在写的文件抢名字。
 */
static void cleanup_tmp_files(void)
{
    DIR           *d = opendir(g.dir);
    struct dirent *e;
    char           path[SVC_RECORD_PATH_MAX];
    int            n = 0;

    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);

        if (len < 5 || strcmp(e->d_name + len - 4, ".tmp") != 0) {
            continue;
        }
        if (is_raw_sidecar(e->d_name)) {
            continue;                   /* 裸流留给恢复步骤, **别删** */
        }
        if (make_path(e->d_name, path, sizeof(path)) <= 0) {
            continue;
        }
        if (unlink(path) == 0) {
            n++;
            LOG_INFO("录制: 清掉上次异常退出留下的半成品 %s", e->d_name);
        }
    }
    closedir(d);
    if (n > 0) {
        LOG_INFO("录制: 启动清理完成, 共删除 %d 个 MP4 半成品", n);
    }
}

/* ─────────── 分段开关 ─────────── */

/**
 * @brief 开一个新分段(用**当前时间**命名)
 *
 * @return 0 成功; -1 失败(时间取不到 / 名字放不下 / MP4Create 失败)
 *
 * @note `MP4Create` 之后**立刻** `MP4SetTimeScale(90000)` —— 照抄参考项目那份
 *       `SAMPLE_COMM_VENC_SaveH264ToMP4()` 的顺序(出处见 `svc_record.h` 的复用说明)。
 */
static int open_segment(void)
{
    struct tm tmv;
    time_t    now = time(NULL);
    size_t    n;

    if (localtime_r(&now, &tmv) == NULL) {
        return -1;
    }
    if (svc_record_policy_make_name(&tmv, g.cur_name, sizeof(g.cur_name)) <= 0) {
        return -1;
    }
    if (make_path(g.cur_name, g.path_final, sizeof(g.path_final)) <= 0 ||
        make_path(g.cur_name, g.stats.cur_name, sizeof(g.stats.cur_name)) <= 0) {
        return -1;
    }
    snprintf(g.path_tmp, sizeof(g.path_tmp), "%s.tmp", g.path_final);
    /* 侧车路径:把结尾的 ".mp4" 换成 ".h264.tmp"(同样以 .tmp 结尾, 一起被清理) */
    snprintf(g.path_raw, sizeof(g.path_raw), "%s", g.path_final);
    n = strlen(g.path_raw);
    if (n > 4 && strcmp(g.path_raw + n - 4, ".mp4") == 0) {
        snprintf(g.path_raw + n - 4, sizeof(g.path_raw) - (n - 4), ".h264.tmp");
    }

    /* ★ 开新段之前先把空间腾够:保证"这一段有一整段的地方写" */
    ensure_headroom();

    /* ★ 写进 `.tmp`:走不完 MP4Close 就**永远不是可播的分段**(见 svc_record.h) */
    g.mp4 = MP4Create(g.path_tmp, 0);
    if (g.mp4 == MP4_INVALID_FILE_HANDLE) {
        g.mp4 = NULL;
        g.stats.cur_name[0] = '\0';
        g.stats.write_errors++;
        LOG_ERROR("录制: MP4Create 失败: %s", g.path_tmp);
        return -1;
    }
    MP4SetTimeScale(g.mp4, SVC_RECORD_TIMESCALE);
    open_raw_sidecar();
    g.track         = MP4_INVALID_TRACK_ID;
    g.have_sps      = 0;
    g.have_pps      = 0;
    g.frames_in_seg = 0;
    g.want_close    = 0;
    LOG_INFO("录制: 开始新分段 %s(%dx%d, 旁路裸流 %s)",
             g.cur_name, g.width, g.height, (g.raw != NULL) ? "开" : "关");
    return 0;
}

/**
 * @brief 关掉当前分段
 *
 * @note ★ **`MP4Close` 就是"索引(moov)落盘"的时刻** —— 不调它, 这个文件不可播。
 * @note ★ 顺序很重要:`MP4Close` → 删侧车 → **`rename` 成正式名**。
 *       `rename` 放最后, 是为了让"目录里出现 `.mp4`"这件事**等价于"这段已经完整"**。
 */
static void close_segment(void)
{
    if (g.mp4 == NULL) {
        return;
    }
    MP4Close(g.mp4, 0);
    g.mp4        = NULL;
    g.track      = MP4_INVALID_TRACK_ID;
    g.want_close = 0;

    /* MP4 已完整 ⇒ 侧车的使命结束:关掉并删掉(稳态磁盘占用仍是一份) */
    close_raw_sidecar(1);
    if (rename(g.path_tmp, g.path_final) != 0) {
        g.stats.write_errors++;
        LOG_ERROR("录制: rename 失败(%s → %s), 该段仍以 .tmp 存在",
                  g.path_tmp, g.path_final);
    }
    note_seg_size();                               /* 记住本段大小(下段的预留依据) */
    g.stats.segments++;
    LOG_INFO("录制: 分段收尾 %s(%d 帧, 共 %llu 段, 裸流 %llu 字节/错 %llu)",
             g.cur_name, g.frames_in_seg,
             (unsigned long long)g.stats.segments,
             (unsigned long long)g.stats.raw_bytes,
             (unsigned long long)g.stats.raw_errors);
    g.stats.cur_name[0] = '\0';
    enforce_limits();
}

/* ─────────── 写一个 NALU / 一帧 ─────────── */

/**
 * @brief 建视频轨并写入 SPS(每个分段**只做一次**)
 *
 * @param[in] n SPS 那个 NALU(含 1 字节 NALU 头)
 *
 * @note 参数含义照抄参考项目那份 `SAMPLE_COMM_VENC_SaveH264ToMP4()`(见 `svc_record.h`):
 *       `s[1]`=AVCProfileIndication `s[2]`=profile_compatibility `s[3]`=AVCLevelIndication;
 *       最后那个 `3` = **每个 NALU 前有 4 字节长度**, 填"长度-1"。
 */
static void add_track_with_sps(const proto_nalu_t *n)
{
    const uint8_t *s = n->data;

    if (n->len < 4) {                   /* 至少要能取到 s[1..3] */
        g.stats.write_errors++;
        return;
    }
    g.track = MP4AddH264VideoTrack(g.mp4, SVC_RECORD_TIMESCALE,
                                   SVC_RECORD_SAMPLE_DUR,
                                   (uint16_t)g.width, (uint16_t)g.height,
                                   s[1], s[2], s[3], 3);
    if (g.track == MP4_INVALID_TRACK_ID) {
        g.stats.write_errors++;
        LOG_ERROR("录制: MP4AddH264VideoTrack 失败");
        return;
    }
    MP4SetVideoProfileLevel(g.mp4, 0x7F);
    MP4AddH264SequenceParameterSet(g.mp4, g.track, s, (uint16_t)n->len);
}

/**
 * @brief 调 mp4v2 写一个 sample(把常用参数收进来, 让调用处短到不用续行)
 *
 * @param[in] total  缓冲总字节数(4 字节长度前缀 + NALU)
 * @param[in] is_key 1 = 关键帧(syncFlag=1)
 * @return 1 成功; 0 失败
 *
 * @note 抽出来纯粹是为了**避免深续行** —— `MP4WriteSample` 有 7 个参数,
 *       摊在调用处要续行到 24 空格, 会被 `check_style.py` 判成"缩进 6 层 > 5"。
 *       (同类问题今天已经踩了三次, 统一用"抽 helper"解决。)
 */
static int write_mp4_sample(uint32_t total, int is_key)
{
    return MP4WriteSample(g.mp4, g.track, g_mp4buf, total,
                          MP4_INVALID_DURATION, 0, is_key ? 1 : 0) ? 1 : 0;
}

/**
 * @brief 写入失败时的第一反应:先判原因, 该清理就清理
 *
 * @return 1 = 已清理(调用方可以重试写);0 = 清理也救不了(或不需要)
 *
 * @note 三种失败要**分开对待**(别混成一个"失败"):
 *       · `EROFS`/`EACCES`/`EPERM` —— 文件系统已经不可写。板的挂载参数是
 *         `errors=remount-ro`, 卡出 I/O 错后内核会把整盘**重挂成只读**,
 *         这时删文件也腾不出空间 ⇒ 只报一次警, 不白费力气。
 *       · 其它(含 `ENOSPC`) —— 当"空间不够"处理:删最旧 + 保底预留, 让调用方重试。
 *       · `errno` 压根没设置 —— 也走清理那条路(总比什么都不做好)。
 */
static int recover_space_on_write_error(void)
{
    int saved = errno;

    if (saved == EROFS || saved == EACCES || saved == EPERM) {
        if (!g.alerted_ro) {
            g.alerted_ro = 1;
            LOG_ERROR("录制: 写入失败(%s) —— TF 卡可能已被内核重挂为只读"
                      "(挂载参数 errors=remount-ro), 删文件也救不回来, 请检查卡",
                      strerror(saved));
        }
        return 0;
    }
    g.stats.disk_full++;
    if (g.stats.disk_full <= 3) {
        LOG_ERROR("录制: 写入失败(%s) → 立即清理最旧分段并重试",
                  (saved != 0) ? strerror(saved) : "errno 未设置");
    }
    enforce_limits();
    ensure_headroom();
    return 1;
}

/**
 * @brief 把一个切片 NALU 写成一个 MP4 sample
 *
 * @param[in] n      切片 NALU(含 NALU 头)
 * @param[in] is_key 1 = 关键帧(syncFlag=1, 播放器可从它开始解)
 *
 * @note ★ **MP4 用的是"4 字节大端长度前缀", 不是 Annex-B 起始码**。
 *       我们的 `proto_nalu` 已经把起始码剥掉了, 所以这里是**自己补上长度前缀**;
 *       参考项目那份实现因为拿到的是带起始码的缓冲, 做的是"把起始码就地改成长度" ——
 *       结果一样(起始码正好也是 4 字节), 但它那个写法会**改动源缓冲**。
 * @note ★ 写失败时**先怀疑盘满**:清理腾出空间后把同一个 NALU **重写一次**
 *       (见 `recover_space_on_write_error()`)。**只重试一次** —— 免得在坏卡上死循环。
 */
static void write_sample(const proto_nalu_t *n, int is_key)
{
    uint32_t len = (uint32_t)n->len;

    if (n->len > SVC_RECORD_MAX_NALU) {
        g.stats.oversized++;
        return;
    }
    g_mp4buf[0] = (uint8_t)(len >> 24);
    g_mp4buf[1] = (uint8_t)(len >> 16);
    g_mp4buf[2] = (uint8_t)(len >> 8);
    g_mp4buf[3] = (uint8_t)len;
    memcpy(g_mp4buf + 4, n->data, n->len);

    errno = 0;                          /* 先清零, 才能判断失败时 errno 有没有被设置 */
    if (!write_mp4_sample(len + 4, is_key)) {
        if (!recover_space_on_write_error() || !write_mp4_sample(len + 4, is_key)) {
            g.stats.write_errors++;
            return;
        }
        g.stats.write_retry_ok++;
        if (g.stats.write_retry_ok <= 3) {
            LOG_INFO("录制: 清理后写入已恢复(第 %llu 次)",
                     (unsigned long long)g.stats.write_retry_ok);
        }
    }
    g.stats.bytes_written += n->len;
}

/**
 * @brief 遍历回调:按 NALU 类型分别处理
 *
 * @param[in] n    当前 NALU
 * @param[in] user 未使用
 * @return 永远 0(不提前终止)
 *
 * @note 只写三类:SPS(建轨) / PPS / 切片。SEI、AUD 等**不写进 MP4**
 *       —— 它们对离线播放没用, 写进去反而要额外处理。
 */
static int on_nalu_cb(const proto_nalu_t *n, void *user)
{
    (void)user;

    if (n->kind == PROTO_NALU_KIND_SPS) {
        if (!g.have_sps) {
            add_track_with_sps(n);
            g.have_sps = 1;
        }
        return 0;
    }
    if (n->kind == PROTO_NALU_KIND_PPS) {
        if (!g.have_pps && g.track != MP4_INVALID_TRACK_ID) {
            MP4AddH264PictureParameterSet(g.mp4, g.track, n->data,
                                          (uint16_t)n->len);
            g.have_pps = 1;
        }
        return 0;
    }
    if (n->kind != PROTO_NALU_KIND_IDR && n->kind != PROTO_NALU_KIND_SLICE) {
        return 0;
    }
    if (g.track != MP4_INVALID_TRACK_ID) {
        write_sample(n, n->is_key);
    }
    return 0;
}

/**
 * @brief 处理一个队列槽位(一帧)
 *
 * @param[in] len 槽位里的有效字节数
 *
 * @note ★ **分段边界对齐到 IDR** —— 这是修 B033 的关键, 两条规则:
 *       ① 新分段**必须从关键帧(IDR)开始**: MP4 建轨要 SPS/PPS, 而它们是
 *          跟着 IDR 一起发的; 从 P 帧起写, 播放器解不出来(花屏/播不了)。
 *       ② 收满 `segment_frames` 后**不立刻关**, 而是继续把这帧写进**当前段**
 *          (它是合法的后续帧), 等**下一个关键帧**才关旧段、并**用它开新段**。
 *          于是边界正好落在 IDR 上, **一帧都不丢**。
 *
 *       ❌ 修之前是"满了立刻关, 新段干等下一个 IDR":等的那些帧被计数但
 *          **没落盘**, 实测每段丢 29 帧(≈1 秒); 而 `frames_in_seg` 统计的是
 *          "处理过"的帧, 于是日志说 900 帧、文件里只有 871 帧(见 B033)。
 *
 * @note 代价:段长从"恰好 segment_frames 帧"变成
 *       **segment_frames ~ segment_frames + GOP-1 帧**(30fps/GOP=30 时约
 *       30.0~31.0 秒)。"每段不短于设定值"是更强的语义, 这是有意的取舍。
 */
static void handle_slot(size_t len)
{
    const svc_media_frame_hdr_t *h;
    const uint8_t               *data;
    int                          is_key;
    int                          n;

    (void)len;
    h = svc_media_slot_hdr(g_slot);
    if (h == NULL) {
        g.stats.slot_errors++;
        return;
    }
    data   = svc_media_slot_data(g_slot);
    is_key = proto_nalu_has_idr(data, h->len, 0 /* 只支持 H.264 */);

    if (g.mp4 != NULL && g.want_close && is_key) {
        close_segment();                /* ★ 边界落在这个 IDR 上 */
    }
    if (g.mp4 == NULL) {
        if (!is_key) {
            return;                     /* 等关键帧(只在刚启动那一下会发生) */
        }
        if (open_segment() != 0) {
            return;
        }
    }
    n = proto_nalu_foreach(data, h->len, 0 /* 只支持 H.264 */, on_nalu_cb, NULL);
    if (n <= 0) {
        g.stats.write_errors++;
        return;
    }
    /* ★ 旁路裸流:把**整帧 Annex-B** 也写一份 —— 掉电/强杀后这份截断也能解 */
    write_raw_frame(data, (uint32_t)h->len);
    /* ★ 只统计**真正落盘**的帧 —— 让日志里的帧数等于文件里的帧数(B033 的教训) */
    g.frames_in_seg++;
    g.stats.frames_written++;
    if (g.segment_frames > 0 && g.frames_in_seg >= g.segment_frames) {
        g.want_close = 1;               /* 不立刻关:等下一个 IDR 再切 */
    }
}

/**
 * @brief 把路径结尾的后缀 `from` 换成 `to`
 *
 * @param[in]  path 原路径
 * @param[in]  from 要换掉的后缀(如 ".h264.tmp")
 * @param[in]  to   换成什么(如 ".mp4")
 * @param[out] out  输出
 * @param[in]  cap  容量
 * @return 0 成功; -1 后缀不匹配或放不下
 */
static int swap_ext(const char *path, const char *from, const char *to,
                    char *out, size_t cap)
{
    size_t n = strlen(path);
    size_t f = strlen(from);
    size_t t = strlen(to);

    if (n <= f || strcmp(path + n - f, from) != 0 || (n - f + t + 1) > cap) {
        return -1;
    }
    memcpy(out, path, n - f);
    snprintf(out + n - f, cap - (n - f), "%s", to);
    return 0;
}

/**
 * @brief 找**最后一个** Annex-B 起始码的偏移(从右往左扫)
 *
 * @param[in] buf 缓冲
 * @param[in] len 字节数
 * @return 最后一个起始码的偏移; 0 = 没找到(整块都得留到下一轮)
 *
 * @note 用来切块:起始码之后的内容可能是**不完整**的 NALU, 必须留到下一块 ——
 *       否则一个切片 NALU 会被切成两个 sample, 播出来就花了。
 * @note 只认 3 字节起始码 `00 00 01`(4 字节的 `00 00 00 01` 里也含它)。
 */
static size_t last_start_code(const uint8_t *buf, size_t len)
{
    size_t i;

    if (len < 3) {
        return 0;
    }
    for (i = len - 3; i > 0; i--) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            return i;
        }
    }
    return 0;
}

/**
 * @brief 把裸流文件**分块**喂给 mp4v2(块边界上的不完整 NALU 留到下一轮)
 *
 * @param[in] fp 已打开的裸流文件
 * @return 处理过的 NALU 个数
 *
 * @note 抽出来是为了让 `recover_one_raw()` 不超"函数 ≤ 50 行"的硬约束。
 * @note 三处细节:① 用"找**最后一个**起始码"来切, 保证不把 NALU 切两半;
 *       ② 攒够一个最大 NALU 还没等到下一个起始码就**不再等**(避免卡住);
 *       ③ EOF 时剩下的全算完整。
 */
static int remux_stream(FILE *fp)
{
    size_t carry = 0;
    int    nalus = 0;

    for (;;) {
        size_t got   = fread(g_rec_buf + carry, 1, SVC_RECOVER_CHUNK, fp);
        size_t total = carry + got;
        size_t cut;

        if (got == 0 || carry >= SVC_RECORD_MAX_NALU) {
            cut = total;
        } else {
            cut = last_start_code(g_rec_buf, total);
        }
        if (cut > 0) {
            int n = proto_nalu_foreach(g_rec_buf, cut, 0, on_nalu_cb, NULL);

            if (n > 0) {
                nalus += n;
            }
        }
        if (got == 0) {
            break;
        }
        carry = total - cut;
        memmove(g_rec_buf, g_rec_buf + cut, carry);
    }
    return nalus;
}

/**
 * @brief 把一个残留的裸流侧车**就地重新封装**成 `<stamp>.mp4`
 *
 * @param[in] raw_name 裸流文件名(不含目录, 形如 `2026-09-18-22-35-13.h264.tmp`)
 * @return 1 = 救回来了(生成了 .mp4 并删掉裸流); 0 = 救不回来(裸流保留);
 *         -1 = 连文件都打不开
 *
 * @note ★ **为什么这件事值得在板子上做**: 强杀/断电后 `<stamp>.mp4.tmp` 没有 moov,
 *       永远救不回来; 而侧车是 Annex-B、**靠起始码自定界、截断也能解** ——
 *       在板子上重封一次就变回正常录像 ⇒ **断电只丢最后约 30 秒**, 而不是丢整段。
 *       用户不用把文件拷到 PC 上再转。
 * @note 复用**与正常录制同一套** mp4v2 调用(`on_nalu_cb` → `write_sample`),
 *       所以封装结果和录像完全一致(同样是"4 字节长度前缀"那一套)。
 * @note 先写成 `<stamp>.mp4.tmp`、**全部成功才改名** —— 与正常分段同一套约定:
 *       中途被打断, 下次启动还能再来一次。
 * @note 执行线程: 录制线程启动时、**取任何队列帧之前**(那时 mp4v2 状态是空闲的)。
 */
static int recover_one_raw(const char *raw_name)
{
    char   raw_path[SVC_RECORD_PATH_MAX];
    char   mp4_path[SVC_RECORD_PATH_MAX];
    char   tmp_path[SVC_RECORD_PATH_MAX];
    FILE  *fp;
    int    nalus;

    if (make_path(raw_name, raw_path, sizeof(raw_path)) <= 0 ||
        swap_ext(raw_path, ".h264.tmp", ".mp4", mp4_path, sizeof(mp4_path)) != 0) {
        return -1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", mp4_path);
    fp = fopen(raw_path, "rb");
    if (fp == NULL) {
        return -1;
    }

    g.mp4 = MP4Create(tmp_path, 0);
    if (g.mp4 == MP4_INVALID_FILE_HANDLE) {
        g.mp4 = NULL;
        fclose(fp);
        return -1;
    }
    MP4SetTimeScale(g.mp4, SVC_RECORD_TIMESCALE);
    g.track    = MP4_INVALID_TRACK_ID;
    g.have_sps = 0;
    g.have_pps = 0;

    nalus = remux_stream(fp);
    fclose(fp);

    MP4Close(g.mp4, 0);
    g.mp4   = NULL;
    g.track = MP4_INVALID_TRACK_ID;

    /* 没有 SPS(建不了轨)或一个 NALU 都没有 ⇒ 救不回来, 别留垃圾 */
    if (g.have_sps == 0 || nalus == 0 || rename(tmp_path, mp4_path) != 0) {
        (void)unlink(tmp_path);
        return 0;
    }
    (void)unlink(raw_path);             /* 救成功了才删裸流 */
    LOG_INFO("恢复: 残留裸流 %s → 已封装成可播 MP4(%d 个 NALU)",
             raw_name, nalus);
    return 1;
}

/**
 * @brief 把目录里所有残留的裸流侧车都救一遍
 *
 * @return 救回来的个数
 *
 * @note ⚠️ 本函数**同步跑在录制线程里**、而且在取第一帧之前 ⇒ 重封期间
 *       取流线程照常往录制队列塞帧, 队列只有 `SVC_MEDIA_QUEUE_SLOTS` 槽,
 *       **满了就丢** ⇒ 这段时间的实时画面录不进去。
 *       上板实测(2026-09-19): 80 MB 的裸流重封 13 秒, 期间录制丢 **252 帧**
 *       (≈8.4 秒); 按 30 分钟一段(~950 MB)线性外推大约丢 **2 分钟**。
 * @note 【简化上限】停机式恢复: 好处是**一行代码都不用改 mp4v2 的用法**
 *       (重封与正常录制共用同一套全局状态 `g.mp4`/`g.track`, 天然不会并发),
 *       天花板是"恢复期间不录新帧"。
 *       **升级路径**: ① 把 `on_nalu_cb`/`write_sample` 的 mp4v2 句柄从全局 `g.*`
 *       改成显式上下文结构体, 恢复用**另一套**上下文 ⇒ 就能挪到独立低优先级线程,
 *       与录制并行而不丢帧; ② 更省事的折中: 恢复挪到 `bsp_mpp_init()` **之前**
 *       (取流还没开始, 队列里不会有帧被丢), 代价是开机多等同样长的时间。
 */
static int recover_leftovers(void)
{
    DIR           *d = opendir(g.dir);
    struct dirent *e;
    int            n = 0;
    int            i = 0;

    if (d == NULL) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (!is_raw_sidecar(e->d_name)) {
            continue;
        }
        /* ★ 先报一声再干活: 重封大文件要几十秒, 没有这一行操作者只会
         *   看到"程序起来了但不录像"(见上面的【简化上限】) */
        i++;
        LOG_INFO("恢复: 第 %d 个残留裸流 %s —— 重封期间不录新帧, 请稍等",
                 i, e->d_name);
        if (recover_one_raw(e->d_name) == 1) {
            n++;
        }
    }
    closedir(d);
    if (n > 0) {
        LOG_INFO("恢复: 共把 %d 个残留裸流转成了可播 MP4", n);
    }
    return n;
}

/**
 * @brief 录制线程主循环
 *
 * @param arg 未使用
 * @return 永远返回 NULL
 *
 * @note **退出前必须 `close_segment()`** —— 那是 `MP4Close` 唯一被调用的地方,
 *       也就是 moov 落盘的唯一机会(优雅关闭的全部意义)。
 */
static void *record_thread(void *arg)
{
    (void)arg;
    /* §7.1: 线程名让 `ps` / `top` 一眼看出这是谁 */
    (void)prctl(PR_SET_NAME, "ipc_rec", 0, 0, 0);
    LOG_INFO("录制线程启动(目录 %s, 每段 %d 帧 ≈ %d 秒)",
             g.dir, g.segment_frames, g.segment_frames / SVC_RECORD_FPS);

    /* ★ 开工前先把上次异常退出留下的**裸流侧车**救成能播的 MP4
     *   (必须在取任何队列帧之前 —— 那时 mp4v2 状态是空闲的) */
    (void)recover_leftovers();

    while (!g.stop_requested) {
        size_t len = 0;
        int    rc  = infra_queue_pop(g.queue, g_slot, SVC_RECORD_SLOT_BYTES,
                                     &len, SVC_RECORD_POP_WAIT_MS);

        if (rc == 1) {
            continue;                   /* 队列空(超时), 正常 */
        }
        if (rc != 0) {
            g.stats.write_errors++;
            break;
        }
        handle_slot(len);
    }
    close_segment();
    LOG_INFO("录制线程退出(段 %llu / 帧 %llu / 删 %llu)",
             (unsigned long long)g.stats.segments,
             (unsigned long long)g.stats.frames_written,
             (unsigned long long)g.stats.deleted);
    return NULL;
}

/* ─────────── 对外接口 ─────────── */

int svc_record_start(const svc_record_cfg_t *cfg, void *queue)
{
    if (g.running) {
        return 0;
    }
    if (cfg == NULL || cfg->dir == NULL || queue == NULL) {
        return -1;
    }
    if (cfg->width <= 0 || cfg->height <= 0) {
        return -2;                      /* mp4v2 建轨需要宽高 */
    }
    memset(&g.stats, 0, sizeof(g.stats));
    snprintf(g.dir, sizeof(g.dir), "%s", cfg->dir);
    g.width          = cfg->width;
    g.height         = cfg->height;
    g.limit_bytes    = cfg->limit_bytes;
    g.limit_files    = cfg->limit_files;
    g.segment_frames = (cfg->segment_frames > 0)
                       ? cfg->segment_frames : SVC_RECORD_DEFAULT_SEGMENT_FRAMES;
    g.raw_sidecar    = (cfg->raw_sidecar != 0) ? 1 : 0;
    g.queue          = (infra_queue_t *)queue;
    g.mp4            = NULL;
    g.raw            = NULL;
    g.track          = MP4_INVALID_TRACK_ID;
    g.path_final[0]  = '\0';
    g.path_tmp[0]    = '\0';
    g.path_raw[0]    = '\0';
    g.stop_requested = 0;
    g.running        = 1;

    /* ★ 清掉上次异常退出留下的 *.tmp —— 必须在录制线程起来**之前**做, 免得抢名字 */
    cleanup_tmp_files();

    if (pthread_create(&g.thread, NULL, record_thread, NULL) != 0) {
        LOG_ERROR("录制线程创建失败");
        g.running = 0;
        return -3;
    }
    g.thread_valid = 1;
    return 0;
}

void svc_record_stop(void)
{
    if (!g.running) {
        return;
    }
    g.stop_requested = 1;
    if (g.thread_valid) {
        pthread_join(g.thread, NULL);   /* 等线程跑完 close_segment 再返回 */
        g.thread_valid = 0;
    }
    g.running = 0;
    LOG_INFO("录制服务已停止(分段 %llu / 帧 %llu / 删 %llu / 错 %llu)",
             (unsigned long long)g.stats.segments,
             (unsigned long long)g.stats.frames_written,
             (unsigned long long)g.stats.deleted,
             (unsigned long long)g.stats.write_errors);
}

int svc_record_is_running(void)
{
    return g.running ? 1 : 0;
}

void svc_record_get_stats(svc_record_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = g.stats;
}
