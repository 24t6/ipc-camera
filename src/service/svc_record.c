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
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
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
 * @brief 执行环形覆盖:超上限就删最旧的分段
 *
 * @note 只在这一处"决策 + 执行"交界处做 I/O; 决策本身是纯函数(`svc_record_policy`)。
 */
static void enforce_limits(void)
{
    svc_record_policy_limits_t lim;
    char                       path[SVC_RECORD_PATH_MAX];
    int                        cnt;
    int                        n;
    int                        i;

    if (g.limit_bytes == 0 && g.limit_files == 0) {
        return;
    }
    cnt = scan_dir(g_scan, SVC_RECORD_POLICY_MAX_FILES);
    if (cnt <= 0) {
        return;
    }
    lim.limit_bytes = g.limit_bytes;
    lim.limit_files = g.limit_files;
    n = svc_record_policy_plan_delete(g_scan, cnt, &lim, g_del,
                                      SVC_RECORD_POLICY_MAX_FILES);
    for (i = 0; i < n; i++) {
        if (make_path(g_scan[g_del[i]].name, path, sizeof(path)) <= 0) {
            continue;
        }
        if (unlink(path) == 0) {
            g.stats.deleted++;
            LOG_INFO("录制: 环形覆盖删除最旧分段 %s(%llu 字节)",
                     g_scan[g_del[i]].name,
                     (unsigned long long)g_scan[g_del[i]].size);
        }
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

    if (localtime_r(&now, &tmv) == NULL) {
        return -1;
    }
    if (svc_record_policy_make_name(&tmv, g.cur_name, sizeof(g.cur_name)) <= 0) {
        return -1;
    }
    if (make_path(g.cur_name, g.stats.cur_name, sizeof(g.stats.cur_name)) <= 0) {
        return -1;
    }
    g.mp4 = MP4Create(g.stats.cur_name, 0);
    if (g.mp4 == MP4_INVALID_FILE_HANDLE) {
        g.mp4 = NULL;
        g.stats.cur_name[0] = '\0';
        g.stats.write_errors++;
        LOG_ERROR("录制: MP4Create 失败: %s", g.stats.cur_name);
        return -1;
    }
    MP4SetTimeScale(g.mp4, SVC_RECORD_TIMESCALE);
    g.track         = MP4_INVALID_TRACK_ID;
    g.have_sps      = 0;
    g.have_pps      = 0;
    g.frames_in_seg = 0;
    g.want_close    = 0;
    LOG_INFO("录制: 开始新分段 %s(%dx%d)", g.cur_name, g.width, g.height);
    return 0;
}

/**
 * @brief 关掉当前分段
 *
 * @note ★ **`MP4Close` 就是"索引(moov)落盘"的时刻** —— 不调它, 这个文件不可播。
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
    g.stats.segments++;
    LOG_INFO("录制: 分段收尾 %s(%d 帧, %llu 字节, 共 %llu 段)",
             g.cur_name, g.frames_in_seg,
             (unsigned long long)g.stats.bytes_written,
             (unsigned long long)g.stats.segments);
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
 * @brief 把一个切片 NALU 写成一个 MP4 sample
 *
 * @param[in] n      切片 NALU(含 NALU 头)
 * @param[in] is_key 1 = 关键帧(syncFlag=1, 播放器可从它开始解)
 *
 * @note ★ **MP4 用的是"4 字节大端长度前缀", 不是 Annex-B 起始码**。
 *       我们的 `proto_nalu` 已经把起始码剥掉了, 所以这里是**自己补上长度前缀**;
 *       参考项目那份实现因为拿到的是带起始码的缓冲, 做的是"把起始码就地改成长度" ——
 *       结果一样(起始码正好也是 4 字节), 但它那个写法会**改动源缓冲**。
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

    if (!write_mp4_sample(len + 4, is_key)) {
        g.stats.write_errors++;
        return;
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
    /* ★ 只统计**真正落盘**的帧 —— 让日志里的帧数等于文件里的帧数(B033 的教训) */
    g.frames_in_seg++;
    g.stats.frames_written++;
    if (g.segment_frames > 0 && g.frames_in_seg >= g.segment_frames) {
        g.want_close = 1;               /* 不立刻关:等下一个 IDR 再切 */
    }
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
    g.queue          = (infra_queue_t *)queue;
    g.mp4            = NULL;
    g.track          = MP4_INVALID_TRACK_ID;
    g.stop_requested = 0;
    g.running        = 1;

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
