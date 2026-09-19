/**
 * @file    app_main.c
 * @brief   应用入口 —— 把四个模块接起来, 让整条链路真正跑起来(M1-9)
 *
 * 【模块职责】把 queue + svc_net + svc_sender + svc_media + svc_osd 接起来, 并管起停顺序
 * 【依赖方向】依赖全部 service 层与 infra_queue / proto_sdp / bsp_mpp; 不碰 MPP 细节
 * 【线程模型】main 线程只做信号处理与定期汇报; 真正干活的是 4 个 service 线程
 * 【资源边界】队列 8 槽 × 256KB(启动时一次分配, 退出时销毁); 无运行期动态分配
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它做的事:一根线, 四个模块
 * ─────────────────────────────────────────────────────────────────
 *
 *      ① svc_media   取流线程: MPP 取帧 → 拼槽位 → 入队
 *      ② svc_sender  发送线程: 出队 → 分片成 RTP → 发给所有在看的人
 *      ③ svc_net     RTSP 服务端: 处理 OPTIONS/DESCRIBE/SETUP/PLAY…
 *      ④ svc_osd     1 Hz 线程: 时间 → 点阵渲染 → 叠加到 VENC 通道(M2 水印)
 *      ⑤ 本文件      接线: 队列 + 两个回调(on_play / on_teardown)
 *
 *  接线图(箭头 = 数据流; 虚线 = 回调):
 *
 *      ┌───────────┐  push   ┌────────┐  pop   ┌────────────┐
 *      │ svc_media │────────▶│ 队列    │───────▶│ svc_sender │──▶ RTP/UDP
 *      └───────────┘         └────────┘        └────────────┘
 *                                                     ▲
 *                           on_play(client, dst) ─────┘
 *      ┌───────────┐              │
 *      │  svc_net  │──────────────┘        (PLAY 时告诉发送端"往哪发")
 *      └───────────┘
 *            ▲
 *            │ RTSP (TCP 554)
 *         VLC / ffplay
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⭐ 启动顺序为什么是这样(有讲究)
 * ─────────────────────────────────────────────────────────────────
 *      ① 建队列          —— 后面几个都要用它
 *      ② 起 svc_net      —— 先把"控制面"立起来(此时还没码流, 但能应答 OPTIONS)
 *      ③ 起 svc_sender   —— 消费端先就位
 *      ④ 起 svc_media    —— 生产者最后开(它会 bsp_mpp_init(), 约 5~10 秒)
 *      ⑤ 起 svc_osd      —— **必须在 svc_media 之后**: 水印挂在 VENC 通道上,
 *                          而那个通道是 bsp_mpp_init() 建的。
 *                          失败**不影响推流**(水印只是锦上添花)。
 *
 *  为什么生产者最后: `svc_media` 一起来就开始往队列里推帧。如果队列还没有
 *  消费者(或队列还没建), 那些帧要么没地方去、要么立刻被"丢最旧"丢掉。
 *  **先让下游就位, 再开上游** —— 这是流水线的通用原则。
 *
 * ─────────────────────────────────────────────────────────────────
 *  关于 SDP 里的 SPS/PPS
 * ─────────────────────────────────────────────────────────────────
 *  本程序**故意不填** SDP 的 `sprop-parameter-sets`(留空)。
 *  理由:
 *    · 我们的发送端**保证客户端从 IDR 开始收**(见 svc_sender.h 设计决定②),
 *      而海思的关键帧**自带 SPS/PPS/SEI**(实测每个 IDR 帧 4 个 pack)。
 *      所以客户端一定拿得到参数集, 不会花屏。
 *    · 要从码流里"偷"出 SPS/PPS 填进 SDP, 得先取到第一帧 ——
 *      那会把启动顺序搞复杂(要先跑一下 MPP 才能生成 SDP)。
 *  代价: 客户端要等到第一个关键帧才拿到参数(最坏等一个 GOP = 1 秒)。
 *  这是一处**清醒的取舍**, 不是遗漏 —— 见 `docs/进度与下一步.md`。
 *
 *  用法(板子上):
 *      ./ipc_app                 # 监听 0.0.0.0:554
 *      ./ipc_app -p 8554         # 换端口(免 root)
 *      ./ipc_app -h265           # 取 H.265 那一路(默认 H.264)
 *      ./ipc_app -no-osd         # 不叠时间水印(默认在右上角叠)
 *      ./ipc_app -v              # 打开 DEBUG 日志
 *
 *  然后客户端拉流:
 *      ffplay -rtsp_transport udp rtsp://<板子IP>:554/live
 *      vlc rtsp://<板子IP>:554/live
 */
#include <arpa/inet.h>      /* inet_ntoa / ntohs —— 日志里打 RTP 目的地 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bsp_mpp.h"
#include "infra_log.h"
#include "infra_queue.h"
#include "proto_sdp.h"
#include "svc_http.h"
#include "svc_media.h"
#include "svc_net.h"
#include "svc_osd.h"
#include "svc_record.h"
#include "svc_sender.h"

/** 队列槽位容量 = 帧头 + 单帧最大字节(与 svc_media 的预算一致) */
#define APP_SLOT_BYTES (SVC_MEDIA_HDR_SIZE + BSP_MPP_FRAME_MAX_BYTES)

/** 队列槽位数 8 = 约 0.27 秒的缓冲(见 ARCHITECTURE.md 预算表) */
#define APP_QUEUE_SLOTS 8

/** 录制目录。开机由 rcS 自动把 TF 卡挂到这里(见 work/setup_board_sdcard_mount.py) */
#define APP_REC_DIR "/mnt/sdcard"

/**
 * 默认环形容量上限(MB)。
 *
 * @note ⚠️ 这个值**必须大于一段的大小**, 否则每录满一段就把上一段删掉,
 *       环形覆盖变成"只留一段"。
 *       实测码流约 4 Mbps ⇒ 30 分钟一段 ≈ **900 MB**;
 *       8 GB ≈ 9 段 ≈ 4.5 小时, 占 30G 卡的 27%, 留足余量。
 */
#define APP_REC_LIMIT_MB 8192

/** 录制队列(与发送队列**互相独立**, 见 svc_media.h 的说明) */
static infra_queue_t *g_record_queue;

/** 默认 RTSP 端口。554 才是标准端口, 但 <1024 需要 root —— 板子上本来就是 root */
#define APP_DEFAULT_PORT 554

/** 状态打印间隔(秒) */
#define APP_REPORT_SEC 5

static volatile int g_stop;

/**
 * @brief 信号处理: 只置"该停了"的标志
 *
 * @param sig 信号号(未使用)
 * @note ⚠️ **只置标志, 不做任何清理** —— 在信号处理函数里做清理是不安全的。
 * @note 执行线程: 收到信号的那个线程(通常是 main)。
 */
static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
    /* 只置标志, 不做任何清理 —— 信号处理函数里做清理是不安全的 */
}

/** 命令行参数 */
typedef struct {
    uint16_t    port;
    const char *bind_ip;
    int         is_h265;
    int         no_osd;      /**< 1 = 不叠时间水印(默认叠) */
    int         no_record;   /**< 1 = 不录 MP4(默认录到 /mnt/sdcard) */
    int         no_raw;      /**< 1 = 不写旁路裸流侧车(默认写;掉电后可救前一段) */
    int         no_http;     /**< 1 = 不起回放服务(默认起) */
    int         rec_mb;      /**< 环形容量上限(MB);0 = 不限 */
    int         rec_seg;     /**< 每段**秒数**;0 = 用默认(1800 秒 = 30 分钟) */
    int         rec_seg_mb;  /**< 每段**大小上限**(MB);0 = 用默认(1024 MB) */
    uint16_t    http_port;   /**< 回放服务端口;0 = 用默认(8080) */
    int         verbose;
} app_opts_t;

/*
 * ⚠️ `-h265` / `-no-osd` / `-no-record` / `-no-raw` 这些"多字符选项"**必须走长选项**
 *    (2026-09-19 修的 B040):原来它们交给 `getopt()` 之后**一个都不生效** ——
 *    `-no-record` 被当成选项簇 → `-n` 不认识 → 直接打印用法退出;
 *    `-h265` 更阴, 被当成 `-h`(帮助)→ **退出码还是 0**, 脚本根本看不出错。
 *    glibc 的 `getopt_long()` **接受单横线长选项**(`-no-record` 与 `--no-record` 等价),
 *    所以文档里的写法不用改, 功能就修好了。
 */
#define OPT_H265       0x101
#define OPT_NO_OSD     0x102
#define OPT_NO_RECORD  0x103
#define OPT_NO_RAW     0x104
#define OPT_NO_HTTP    0x105

/** 长选项表(单横线写法也认, 见上面的说明) */
static const struct option LONG_OPTS[] = {
    { "port",       required_argument, NULL, 'p' },
    { "bind",       required_argument, NULL, 'b' },
    { "ring",       required_argument, NULL, 'r' },
    { "segment",    required_argument, NULL, 's' },
    { "segment-mb", required_argument, NULL, 'm' },
    { "http",       required_argument, NULL, 'H' },
    { "h265",       no_argument,       NULL, OPT_H265 },
    { "no-osd",     no_argument,       NULL, OPT_NO_OSD },
    { "no-record",  no_argument,       NULL, OPT_NO_RECORD },
    { "no-raw",     no_argument,       NULL, OPT_NO_RAW },
    { "no-http",    no_argument,       NULL, OPT_NO_HTTP },
    { "verbose",    no_argument,       NULL, 'v' },
    { "help",       no_argument,       NULL, 'h' },
    { NULL,         0,                 NULL, 0 }
};

/**
 * @brief 打印命令行用法
 *
 * @param prog 程序名(argv[0])
 */
static void usage(const char *prog)
{
    printf("用法: %s [选项]\n"
           "  -p <端口>   RTSP 端口(默认 %d)\n"
           "  -b <地址>   监听地址(默认 0.0.0.0)\n"
           "  -h265       取 H.265 那一路(默认 H.264)\n"
           "  -no-osd     不叠时间水印(默认在右上角叠)\n"
           "  -no-record  不录 MP4(默认录到 " APP_REC_DIR ")\n"
           "  -no-raw     不写旁路裸流侧车(默认写:掉电/强杀后能把前一段救回来)\n"
           "  -no-http    不起回放服务(默认起)\n"
           "  -H <端口>   回放服务的 HTTP 端口(默认 %d)\n"
           "  -r <MB>     录制环形容量上限(默认 %d MB;0=不限)\n"
           "  -s <秒数>   每段多少秒后切新文件(默认 1800 = 30 分钟)\n"
           "  -m <MB>     每段多少 MB 后切新文件(默认 %d MB;与 -s 谁先到算谁;\n"
           "              0=用默认。⚠️ 这一维不能关 —— vfat 单文件上限 4 GiB)\n"
           "  -v          打开 DEBUG 日志\n"
           "  -h          显示本帮助\n",
           prog, APP_DEFAULT_PORT, SVC_HTTP_DEFAULT_PORT, APP_REC_LIMIT_MB,
           SVC_RECORD_DEFAULT_SEGMENT_MB);
}

/**
 * @brief 解析命令行。
 *
 * @return 0 = 继续运行; 1 = 参数错(已打印用法); 2 = 用户要 -h 帮助(正常退出)
 *
 * @note 用 `getopt_long` 而不是 `getopt`:这样"多字符选项"才有地方放(见上面 B040 说明)。
 */
static int parse_args(int argc, char **argv, app_opts_t *o)
{
    int opt;

    o->port        = APP_DEFAULT_PORT;
    o->bind_ip     = "0.0.0.0";
    o->is_h265     = 0;
    o->no_osd      = 0;
    o->no_record   = 0;
    o->no_raw      = 0;
    o->no_http     = 0;
    o->rec_mb      = APP_REC_LIMIT_MB;
    o->rec_seg     = 0;
    o->rec_seg_mb  = 0;
    o->http_port   = SVC_HTTP_DEFAULT_PORT;
    o->verbose     = 0;

    while ((opt = getopt_long(argc, argv, "p:b:r:s:m:H:hv", LONG_OPTS, NULL)) != -1) {
        switch (opt) {
        case 'p': o->port       = (uint16_t)atoi(optarg); break;
        case 'b': o->bind_ip    = optarg;                 break;
        case 'r': o->rec_mb     = atoi(optarg);           break;
        case 's': o->rec_seg    = atoi(optarg);           break;
        case 'm': o->rec_seg_mb = atoi(optarg);           break;
        case 'H': o->http_port  = (uint16_t)atoi(optarg); break;
        case OPT_H265:      o->is_h265   = 1;             break;
        case OPT_NO_OSD:    o->no_osd    = 1;             break;
        case OPT_NO_RECORD: o->no_record = 1;             break;
        case OPT_NO_RAW:    o->no_raw    = 1;             break;
        case OPT_NO_HTTP:   o->no_http   = 1;             break;
        case 'v': o->verbose    = 1;                      break;
        case 'h': return 2;
        default:  return 1;
        }
    }
    return 0;
}

/**
 * @brief 打印运行状态(链路的四段各一个数字)。
 *
 * @note 为什么值得定期打: 板子上跑起来之后, 你要能**一眼看出卡在哪一环**。
 *       `丢` 非 0 = 队列在丢帧(下游跟不上); `错误` 非 0 = 网络或编码有问题。
 */
static void report(int secs)
{
    svc_media_stats_t  ms;
    svc_sender_stats_t ss;
    svc_net_stats_t    ns;
    svc_osd_stats_t    os;
    svc_record_stats_t rs;
    svc_http_stats_t   hs;

    svc_media_get_stats(&ms);
    svc_sender_get_stats(&ss);
    svc_net_get_stats(&ns);
    svc_osd_get_stats(&os);
    svc_record_get_stats(&rs);
    svc_http_get_stats(&hs);

    printf("[%4ds] 取流 %llu 帧 | 发送 %llu 帧/%llu 包 | 客户端 %d 在发(%llu 等IDR)"
           " | RTSP %llu 连接/%llu 请求 | 丢 %llu | 错误 %llu"
           " | ★帧龄 现/最小/最大 %.0f/%.0f/%.0f ms 漂移 %.0f ms"
           " | OSD %llu 次/错 %llu"
           " | 录制 %llu 段/%llu 帧 删 %llu 丢 %llu 错 %llu"
           " | 回放 %llu 请求/%llu 是206\n",
           secs,
           (unsigned long long)ms.frames,
           (unsigned long long)ss.frames_sent,
           (unsigned long long)ss.packets_sent,
           svc_sender_client_count(),
           (unsigned long long)ss.waiting_idr,
           (unsigned long long)ns.conns_accepted,
           (unsigned long long)ns.requests,
           (unsigned long long)(ms.queue_dropped + ms.oversize),
           (unsigned long long)(ms.get_errors + ss.send_errors),
           ss.age_last_us / 1000.0, ss.age_min_us / 1000.0, ss.age_max_us / 1000.0,
           (ss.age_last_us - ss.age_first_us) / 1000.0,
           (unsigned long long)os.ticks,
           (unsigned long long)os.show_errors,
           (unsigned long long)rs.segments,
           (unsigned long long)rs.frames_written,
           (unsigned long long)rs.deleted,
           (unsigned long long)ms.record_dropped,
           (unsigned long long)rs.write_errors,
           (unsigned long long)hs.requests,
           (unsigned long long)hs.partials);
    fflush(stdout);
}

/* ─────────────────── svc_net 的两个回调 ─────────────────── */

/**
 * @brief 客户端开始播放(PLAY 之后)。
 *
 * @note ⚠️ **本函数在 svc_net 的事件循环线程里执行** —— 绝不能阻塞,
 *       否则所有客户端的 RTSP 请求都会被卡住。
 *       `svc_sender_add_client()` 只是"登记 + 建 sender", 不发送, 符合要求。
 */
static void on_play(int client_index, const infra_transport_t *tr, void *user)
{
    int rc;

    (void)user;
    rc = svc_sender_add_client(client_index, tr);
    if (rc != 0) {
        LOG_WARN("client%d 加入发送失败 rc=%d(客户端满?)", client_index, rc);
    } else if (tr->is_tcp) {
        LOG_INFO("client%d 开始播放 → RTP over TCP 交错(fd=%d 通道=%u)",
                 client_index, tr->rtsp_fd, (unsigned)tr->rtp_channel);
    } else {
        LOG_INFO("client%d 开始播放 → RTP over UDP %s:%u", client_index,
                 inet_ntoa(tr->rtp_dst.sin_addr),
                 (unsigned)ntohs(tr->rtp_dst.sin_port));
    }
}

/** @brief 客户端停止播放(TEARDOWN / 断开 / 空闲超时) */
static void on_teardown(int client_index, void *user)
{
    (void)user;
    svc_sender_remove_client(client_index);
    LOG_INFO("client%d 停止播放", client_index);
}

/**
 * @brief 生成 SDP 文本。
 *
 * @return 写入的字符数; <=0 = 失败
 */
static int build_sdp(const app_opts_t *o, char *out, size_t cap)
{
    proto_sdp_cfg_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.origin_ip    = o->bind_ip;
    cfg.session_name = "IPC Camera (Hi3516DV300)";
    cfg.is_h265      = o->is_h265;
    cfg.payload_type = o->is_h265 ? 97 : 96;
    /* 参数集故意留空 —— 理由见文件头说明 */
    return proto_sdp_build(&cfg, out, cap);
}

/**
 * @brief 按**申请的反序**停掉已启动的东西。
 *
 * @param started 位掩码: bit0=svc_media, bit1=svc_sender, bit2=svc_net,
 *                bit3=svc_osd, bit4=svc_record
 *
 * @note 抽出来是为了让每一处失败都能"从当前状态干净回退",
 *       而不必在每个失败分支里重复写一遍清理 —— 那种重复**漏一个就是资源泄漏**。
 * @note ⚠️ **svc_osd 必须最先停**:它挂在 VENC 通道上, 而 `svc_media_stop()`
 *       会 `bsp_mpp_deinit()` 把整条 MPP 通路拆掉 —— 那时再想 Detach 就晚了。
 * @note ★ **svc_record 排在 svc_media 之后**:先停生产者(不再有新帧),
 *       录制线程才能安静地把队里剩的写完、再 `MP4Close` 把 moov 落盘。
 */
static void shutdown_chain(int started)
{
    /* ★ 回放服务最先停:它是"纯读卡"的旁观者, 先让它收手, 免得录制收尾时
     *   还有人正在读卡(卡上的 I/O 是共享的, 越早安静越好)。 */
    if (started & 32)
        svc_http_stop();
    if (started & 8)
        svc_osd_stop();
    if (started & 1)
        svc_media_stop();
    if (started & 16)
        svc_record_stop();
    if (started & 2)
        svc_sender_stop();
    if (started & 4)
        svc_net_stop();
}

/**
 * @brief 起回放服务(阶段 2 的 HTTP 服务)
 *
 * @param[in]  o       命令行选项(端口 / 是否关闭)
 * @param[out] started 位掩码,成功则置上 bit5
 * @return 0 成功; 负值失败
 *
 * @note 抽出来是为了让 `start_chain()` 别太长(项目硬约束: 代码行 ≤ 50)。
 * @note 依赖"录制已经起来了"(要它的录制目录)—— 所以排在 `start_record()` 之后。
 * @note 失败**不致命**:看不了回放不该让直播和录制起不来, 但要明确告警。
 */
static int start_http(const app_opts_t *o, int *started)
{
    svc_http_cfg_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_ip = o->bind_ip;
    cfg.port    = o->http_port;
    if (svc_http_start(&cfg) != 0) {
        return -1;
    }
    *started |= 32;
    printf("回放服务  : http://<板子IP>:%u/recordings(列分段 / Range 取流 / 锁定)\n",
           (unsigned)svc_http_port());
    return 0;
}

/**
 * @brief 起录制服务:建录制队列 → 配 mp4v2 → 起线程
 *
 * @param[in]  o       命令行选项(容量上限 / 每段秒数)
 * @param[out] started 位掩码,成功则置上 bit4
 * @return 0 成功; 负值失败
 *
 * @note 抽出来是为了让 `start_chain()` 别太长(项目硬约束: 代码行 ≤ 50)。
 * @note 宽高从 `bsp_mpp` 取 —— **不在这里写死分辨率**, 免得将来改通道时漏改一处。
 */
static int start_record(const app_opts_t *o, int *started)
{
    svc_record_cfg_t rec;
    int              w = 0;
    int              h = 0;

    g_record_queue = infra_queue_create(APP_QUEUE_SLOTS, APP_SLOT_BYTES);
    if (g_record_queue == NULL) {
        printf("❌ 录制队列创建失败\n");
        return -1;
    }
    bsp_mpp_get_encoder_size(&w, &h);
    memset(&rec, 0, sizeof(rec));
    rec.dir            = APP_REC_DIR;
    rec.width          = w;
    rec.height         = h;
    rec.limit_bytes    = (o->rec_mb > 0) ? (uint64_t)o->rec_mb * 1024 * 1024 : 0;
    rec.limit_files    = 0;
    rec.segment_frames = o->rec_seg * SVC_RECORD_FPS;   /* 命令行给的是秒 */
    /* 命令行给的是 MB;0 = 交给 svc_record 用默认值(1024 MB) */
    rec.segment_bytes  = (o->rec_seg_mb > 0)
                         ? (uint64_t)o->rec_seg_mb * 1024 * 1024 : 0;
    rec.raw_sidecar    = o->no_raw ? 0 : 1;             /* 默认写:掉电/强杀后能救回前一段 */

    if (svc_record_start(&rec, g_record_queue) != 0) {
        infra_queue_destroy(g_record_queue);
        g_record_queue = NULL;
        return -2;
    }
    *started |= 16;
    printf("录制      : %s · %dx%d · 每段 %d 秒 或 %d MB(谁先到算谁, 边界对齐关键帧)"
           " · 环形上限 %d MB · 旁路裸流 %s\n",
           APP_REC_DIR, w, h,
           (o->rec_seg > 0) ? o->rec_seg : SVC_RECORD_DEFAULT_SEGMENT_SEC,
           (o->rec_seg_mb > 0) ? o->rec_seg_mb : SVC_RECORD_DEFAULT_SEGMENT_MB,
           o->rec_mb, o->no_raw ? "关" : "开");
    return 0;
}

/**
 * @brief 把整条链路建起来并启动。
 *
 * @param o       命令行选项
 * @param started 输出: 位掩码(bit0=svc_media, bit1=svc_sender, bit2=svc_net, bit3=svc_osd)
 * @return 队列句柄(**调用方负责销毁**); NULL = 启动失败(已打印原因)
 *
 * @note 启动顺序为什么是这样(有讲究):
 *         ① 建队列        —— 后面两个都要用它
 *         ② 起 svc_net    —— 控制面先立起来(此时还没码流, 但能应答 OPTIONS)
 *         ③ 起 svc_sender —— **消费端先就位**
 *         ④ 最后起 svc_media —— **生产者最后开**
 *       为什么生产者最后: `svc_media` 一起来就往队列推帧。下游没就位的话,
 *       那些帧要么没地方去、要么立刻被"丢最旧"丢掉。
 *       **先让下游就位, 再开上游** —— 流水线的通用原则。
 */
static infra_queue_t *start_chain(const app_opts_t *o, int *started)
{
    infra_queue_t *queue;
    svc_net_cfg_t  net_cfg;
    char           sdp[PROTO_SDP_BUF_SIZE];
    int            sdp_len;

    *started = 0;

    /* ① 队列 —— 槽位必须 >= 帧头 + 单帧最大字节 */
    queue = infra_queue_create(APP_QUEUE_SLOTS, APP_SLOT_BYTES);
    if (queue == NULL) {
        printf("❌ 队列创建失败(%d 槽 × %d 字节)\n",
               APP_QUEUE_SLOTS, APP_SLOT_BYTES);
        return NULL;
    }
    printf("队列      : %d 槽 × %d 字节 = %.2f MB\n",
           APP_QUEUE_SLOTS, APP_SLOT_BYTES,
           (double)APP_SLOT_BYTES * APP_QUEUE_SLOTS / 1024.0 / 1024.0);

    /* ② RTSP 服务端 */
    sdp_len = build_sdp(o, sdp, sizeof(sdp));
    if (sdp_len <= 0) {
        printf("❌ SDP 生成失败 rc=%d\n", sdp_len);
        infra_queue_destroy(queue);
        return NULL;
    }
    printf("SDP       : %d 字节\n", sdp_len);

    memset(&net_cfg, 0, sizeof(net_cfg));
    net_cfg.listen_ip        = o->bind_ip;
    net_cfg.port             = o->port;
    net_cfg.sdp              = sdp;
    net_cfg.sdp_len          = (size_t)sdp_len;
    net_cfg.on_play          = on_play;
    net_cfg.on_teardown      = on_teardown;
    /*
     * 空闲超时给足一点(默认 15 秒偏短): 播放中的客户端**不发 RTSP 请求是正常的**
     * (数据走 RTP/UDP, 控制面是安静的)。超时太短会把正在正常播放的客户端踢掉。
     * 60 秒是折中 —— 客户端每 ~30 秒通常会发一次 GET_PARAMETER 保活。
     */
    net_cfg.idle_timeout_sec = 60;

    if (svc_net_start(&net_cfg) != 0) {
        printf("❌ RTSP 服务端启动失败(端口 %u 被占?)\n", (unsigned)o->port);
        infra_queue_destroy(queue);
        return NULL;
    }
    *started |= 4;
    printf("RTSP      : 已监听(实际端口 %u)\n", (unsigned)svc_net_port());

    /* ③ 发送服务(消费端就位) */
    if (svc_sender_start(queue, o->is_h265) != 0) {
        printf("❌ 发送服务启动失败\n");
        shutdown_chain(*started);
        infra_queue_destroy(queue);
        return NULL;
    }
    *started |= 2;
    printf("发送服务  : 已启动\n");

    /*
     * ③.5 录制服务(M3)。
     *   位置: 在 svc_media **之前**起 —— 它只是个消费者, 队列空着也无所谓;
     *         先就位, 等 media 一开始推帧就能立刻写盘(不丢开头几帧)。
     *   失败**不致命**(与 OSD 同): 录不了不该让推流也起不来, 但要明确告警。
     */
    if (o->no_record) {
        printf("录制      : 已按 -no-record 关闭\n");
    } else if (o->is_h265) {
        printf("录制      : ⚠️ 关闭 —— mp4v2 这版只支持 H.264, 而当前取的是 H.265 那一路\n");
    } else {
        if (start_record(o, started) != 0) {
            printf("⚠️  录制启动失败(推流照常, 只是没有录像)\n");
        }
    }

    /*
     * ③.6 回放服务(阶段 2)。
     *   位置: 在录制**之后** —— 它要读录制目录(分段列表 / 取流 / 锁定清单)。
     *   失败**不致命**(与录制同): 看不了回放不该让直播起不来, 但要明确告警。
     */
    if (o->no_http) {
        printf("回放服务  : 已按 -no-http 关闭\n");
    } else if (start_http(o, started) != 0) {
        printf("⚠️  回放服务启动失败(端口 %u 被占?推流/录制照常)\n",
               (unsigned)o->http_port);
    }

    /* ④ 最后起取流(生产者) */
    printf("\n正在初始化 MPP 通路(约 5~10 秒, 请稍等)…\n");
    if (svc_media_start(queue, g_record_queue, o->is_h265) != 0) {
        printf("❌ 取流服务启动失败(MPP 初始化失败? 摄像头没插好?)\n");
        shutdown_chain(*started);
        infra_queue_destroy(queue);
        *started = 0;
        return NULL;
    }
    *started |= 1;

    /* ⑤ OSD 时间水印(可选)。⚠️ **失败不影响推流** —— 水印只是锦上添花,
     *    不该因为它起不来就没画面。所以这里只告警、不 return。 */
    if (!o->no_osd) {
        if (svc_osd_start() != 0) {
            printf("⚠️  OSD 水印启动失败(画面照常, 只是没有时间)\n");
        } else {
            *started |= 8;
            printf("OSD 水印  : 已叠加到右上角\n");
        }
    } else {
        printf("OSD 水印  : 已按 -no-osd 关闭\n");
    }

    printf("✅ 全链路已启动。用 VLC / ffplay 拉流:\n");
    printf("     ffplay -rtsp_transport udp rtsp://<板子IP>:%u/live\n",
           (unsigned)svc_net_port());
    if ((*started & 32) != 0) {
        printf("     回放列表: curl http://<板子IP>:%u/recordings\n\n",
               (unsigned)svc_http_port());
    } else {
        printf("\n");
    }
    return queue;
}

/* ─────────────────── 主流程 ─────────────────── */

/**
 * @brief 程序入口: 解析参数 → 建链路 → 主循环汇报 → 收尾
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 0 正常退出; 1 = 参数错或启动失败
 * @note 主线程只做汇报与信号处理, 真正干活的是 4 个 service 线程。
 */
int main(int argc, char **argv)
{
    app_opts_t     o;
    infra_queue_t *queue;
    int            rc      = parse_args(argc, argv, &o);
    int            started = 0;
    int            secs    = 0;

    if (rc == 2) {                  /* -h */
        usage(argv[0]);
        return 0;
    }
    if (rc != 0) {
        usage(argv[0]);
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);   /* 行缓冲: 被 kill 时也能看到日志 */
    infra_log_set_level(o.verbose ? INFRA_LOG_DEBUG : INFRA_LOG_INFO);

    printf("===== IPC 网络监控 —— M1 取流+RTSP 推流 / M2 OSD 时间水印 =====\n");
    printf("编码      : %s\n", o.is_h265 ? "H.265 (VENC chn0)" : "H.264 (VENC chn1)");
    printf("RTSP 监听 : %s:%u\n", o.bind_ip, (unsigned)o.port);
    printf("OSD 水印  : %s\n", o.no_osd ? "关闭" : "右上角时间(每秒更新)");
    printf("录制      : %s\n", o.no_record ? "关闭" : APP_REC_DIR " 的 MP4 分段");
    printf("回放服务  : %s\n", o.no_http ? "关闭"
           : "HTTP(列分段 / Range 取流 / 锁定)");
    printf("拉流地址  : rtsp://<板子IP>:%u/live\n\n", (unsigned)o.port);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    queue = start_chain(&o, &started);
    if (queue == NULL)
        return 1;

    /* 主循环: 只做状态汇报, 真正的工作在三个线程里 */
    while (!g_stop) {
        sleep(1);
        secs++;
        if (secs % APP_REPORT_SEC == 0)
            report(secs);
    }

    /* 收尾: 按**申请的反序**停 */
    printf("\n[退出, 正在停止…]\n");
    shutdown_chain(started);
    infra_queue_destroy(queue);
    if (g_record_queue != NULL)
        infra_queue_destroy(g_record_queue);
    printf("已退出。\n");
    return 0;
}
