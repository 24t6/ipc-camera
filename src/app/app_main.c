/**
 * @file    app_main.c
 * @brief   应用入口 —— 把四个模块接起来, 让整条链路真正跑起来(M1-9)
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它做的事:一根线, 四个模块
 * ─────────────────────────────────────────────────────────────────
 *
 *      ① svc_media   取流线程: MPP 取帧 → 拼槽位 → 入队
 *      ② svc_sender  发送线程: 出队 → 分片成 RTP → 发给所有在看的人
 *      ③ svc_net     RTSP 服务端: 处理 OPTIONS/DESCRIBE/SETUP/PLAY…
 *      ④ 本文件      接线: 队列 + 两个回调(on_play / on_teardown)
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
 *      ① 建队列          —— 后面两个都要用它
 *      ② 起 svc_net      —— 先把"控制面"立起来(此时还没码流, 但能应答 OPTIONS)
 *      ③ 起 svc_sender   —— 消费端先就位
 *      ④ **最后**起 svc_media —— 生产者最后开
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
#include "svc_media.h"
#include "svc_net.h"
#include "svc_sender.h"

/** 队列槽位容量 = 帧头 + 单帧最大字节(与 svc_media 的预算一致) */
#define APP_SLOT_BYTES (SVC_MEDIA_HDR_SIZE + BSP_MPP_FRAME_MAX_BYTES)

/** 队列槽位数 8 = 约 0.27 秒的缓冲(见 ARCHITECTURE.md 预算表) */
#define APP_QUEUE_SLOTS 8

/** 默认 RTSP 端口。554 才是标准端口, 但 <1024 需要 root —— 板子上本来就是 root */
#define APP_DEFAULT_PORT 554

/** 状态打印间隔(秒) */
#define APP_REPORT_SEC 5

static volatile int g_stop;

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
    int         verbose;
} app_opts_t;

static void usage(const char *prog)
{
    printf("用法: %s [选项]\n"
           "  -p <端口>   RTSP 端口(默认 %d)\n"
           "  -b <地址>   监听地址(默认 0.0.0.0)\n"
           "  -h265       取 H.265 那一路(默认 H.264)\n"
           "  -v          打开 DEBUG 日志\n"
           "  -h          显示本帮助\n",
           prog, APP_DEFAULT_PORT);
}

/**
 * 解析命令行。
 *
 * @return 0 = 继续运行; 1 = 参数错(已打印用法); 2 = 用户要 -h 帮助(正常退出)
 *
 * @note `-h265` 这种"多字符短选项"getopt 认不了, 所以单独扫一遍 argv。
 *       (要么改成 `--h265` 长选项, 要么手工扫 —— 这里选后者, 保持命令行短。)
 */
static int parse_args(int argc, char **argv, app_opts_t *o)
{
    int i;
    int opt;

    o->port    = APP_DEFAULT_PORT;
    o->bind_ip = "0.0.0.0";
    o->is_h265 = 0;
    o->verbose = 0;

    while ((opt = getopt(argc, argv, "p:b:hv")) != -1) {
        switch (opt) {
        case 'p': o->port    = (uint16_t)atoi(optarg); break;
        case 'b': o->bind_ip = optarg;                 break;
        case 'v': o->verbose = 1;                      break;
        case 'h': return 2;
        default:  return 1;
        }
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h265") == 0)
            o->is_h265 = 1;
    }
    return 0;
}

/**
 * 打印运行状态(链路的四段各一个数字)。
 *
 * @note 为什么值得定期打: 板子上跑起来之后, 你要能**一眼看出卡在哪一环**。
 *       `丢` 非 0 = 队列在丢帧(下游跟不上); `错误` 非 0 = 网络或编码有问题。
 */
static void report(int secs)
{
    svc_media_stats_t  ms;
    svc_sender_stats_t ss;
    svc_net_stats_t    ns;

    svc_media_get_stats(&ms);
    svc_sender_get_stats(&ss);
    svc_net_get_stats(&ns);

    printf("[%4ds] 取流 %llu 帧 | 发送 %llu 帧/%llu 包 | 客户端 %d 在发(%llu 等IDR)"
           " | RTSP %llu 连接/%llu 请求 | 丢 %llu | 错误 %llu"
           " | ★帧龄 现/最小/最大 %.0f/%.0f/%.0f ms 漂移 %.0f ms\n",
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
           (ss.age_last_us - ss.age_first_us) / 1000.0);
    fflush(stdout);
    fflush(stdout);
}

/* ─────────────────── svc_net 的两个回调 ─────────────────── */

/**
 * 客户端开始播放(PLAY 之后)。
 *
 * @note ⚠️ **本函数在 svc_net 的事件循环线程里执行** —— 绝不能阻塞,
 *       否则所有客户端的 RTSP 请求都会被卡住。
 *       `svc_sender_add_client()` 只是"登记 + 建 sender", 不发送, 符合要求。
 */
static void on_play(int client_index, const struct sockaddr_in *rtp_dst, void *user)
{
    int rc;

    (void)user;
    rc = svc_sender_add_client(client_index, rtp_dst);
    if (rc != 0)
        LOG_WARN("client%d 加入发送失败 rc=%d(客户端满?)", client_index, rc);
    else
        LOG_INFO("client%d 开始播放 → RTP %s:%u", client_index,
                 inet_ntoa(rtp_dst->sin_addr),
                 (unsigned)ntohs(rtp_dst->sin_port));
}

/** 客户端停止播放(TEARDOWN / 断开 / 空闲超时) */
static void on_teardown(int client_index, void *user)
{
    (void)user;
    svc_sender_remove_client(client_index);
    LOG_INFO("client%d 停止播放", client_index);
}

/**
 * 生成 SDP 文本。
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
 * 按**申请的反序**停掉已启动的东西。
 *
 * @param started 位掩码: bit0=svc_media, bit1=svc_sender, bit2=svc_net
 *
 * @note 抽出来是为了让每一处失败都能"从当前状态干净回退",
 *       而不必在每个失败分支里重复写一遍清理 —— 那种重复**漏一个就是资源泄漏**。
 */
static void shutdown_chain(int started)
{
    if (started & 1)
        svc_media_stop();
    if (started & 2)
        svc_sender_stop();
    if (started & 4)
        svc_net_stop();
}

/**
 * 把整条链路建起来并启动。
 *
 * @param o       命令行选项
 * @param started 输出: 位掩码(bit0=svc_media, bit1=svc_sender, bit2=svc_net)
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

    /* ④ 最后起取流(生产者) */
    printf("\n正在初始化 MPP 通路(约 5~10 秒, 请稍等)…\n");
    if (svc_media_start(queue, o->is_h265) != 0) {
        printf("❌ 取流服务启动失败(MPP 初始化失败? 摄像头没插好?)\n");
        shutdown_chain(*started);
        infra_queue_destroy(queue);
        *started = 0;
        return NULL;
    }
    *started |= 1;

    printf("✅ 全链路已启动。用 VLC / ffplay 拉流:\n");
    printf("     ffplay -rtsp_transport udp rtsp://<板子IP>:%u/live\n\n",
           (unsigned)svc_net_port());
    return queue;
}

/* ─────────────────── 主流程 ─────────────────── */

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

    printf("===== IPC 网络监控 —— M1-9 取流 + RTSP 推流 =====\n");
    printf("编码      : %s\n", o.is_h265 ? "H.265 (VENC chn0)" : "H.264 (VENC chn1)");
    printf("RTSP 监听 : %s:%u\n", o.bind_ip, (unsigned)o.port);
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
    printf("已退出。\n");
    return 0;
}
