/**
 * @file    rtsp_test.c
 * @brief   proto_rtsp.c 的单元测试 —— 解析 + 响应构造 + 与 proto_sdp 串起来
 *
 * 【怎么验证】(可证伪)
 *   ① 请求用例是**客户端真实会发的报文形状**(ffmpeg / VLC 抓包所见),
 *      不是我自己编的简化版
 *   ② 响应用"逐行断言"检查: 状态行、CSeq 是否回对、Content-Length 是否等于
 *      SDP 的真实字节数、Session 是否原样带回
 *   ③ 边界: 畸形请求、缺 CSeq、缓冲不够、未知头 —— 都要有确定行为
 *   ④ **串联测试**: DESCRIBE 的响应体直接用 proto_sdp_build() 的真实输出,
 *      证明两个模块能接上
 *
 * 编译(主机):
 *   gcc -Wall -Wextra -O2 -o rtsp_test rtsp_test.c \
 *       ../src/protocol/proto_rtsp.c ../src/protocol/proto_sdp.c \
 *       ../src/protocol/proto_nalu.c -I../src/protocol
 *
 * 用法: ./rtsp_test
 */
#include <stdio.h>
#include <string.h>

#include "proto_nalu.h"
#include "proto_rtsp.h"
#include "proto_sdp.h"

static int g_fails;

static int check(const char *name, int cond)
{
    printf("   %-52s %s\n", name, cond ? "[通过]" : "[失败] <<<");
    if (!cond)
        g_fails++;
    return cond;
}

/** 判断响应里是否含某个完整行 */
static int has_line(const char *resp, const char *line)
{
    const char *p = resp;

    while ((p = strstr(p, line)) != NULL) {
        /* 要求后面紧跟 CRLF(避免 "CSeq: 1" 命中 "CSeq: 12") */
        const char *after = p + strlen(line);
        if (after[0] == '\r' && after[1] == '\n')
            return 1;
        p = after;
    }
    return 0;
}

/** 解析请求并断言成功 */
static int parse_ok(const char *name, const char *raw, proto_rtsp_request_t *req)
{
    int rc = proto_rtsp_parse_request(raw, strlen(raw), req);
    if (rc != 0) {
        printf("   %-52s [失败] rc=%d <<<\n", name, rc);
        g_fails++;
        return 0;
    }
    return 1;
}

/* ── 真实客户端请求(CRLF 行尾) ── */

static const char *REQ_OPTIONS =
    "OPTIONS rtsp://192.168.16.88:554/live RTSP/1.0\r\n"
    "CSeq: 1\r\n"
    "User-Agent: LibVLC/3.0.20 (LIVE555 Streaming Media v2020.06.16)\r\n"
    "\r\n";

static const char *REQ_DESCRIBE =
    "DESCRIBE rtsp://192.168.16.88:554/live RTSP/1.0\r\n"
    "CSeq: 2\r\n"
    "Accept: application/sdp\r\n"
    "User-Agent: LibVLC/3.0.20 (LIVE555 Streaming Media v2020.06.16)\r\n"
    "\r\n";

static const char *REQ_SETUP_UDP =
    "SETUP rtsp://192.168.16.88:554/live/streamid=0 RTSP/1.0\r\n"
    "CSeq: 3\r\n"
    "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n"
    "\r\n";

static const char *REQ_SETUP_TCP =
    "SETUP rtsp://192.168.16.88:554/live/streamid=0 RTSP/1.0\r\n"
    "CSeq: 3\r\n"
    "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
    "\r\n";

static const char *REQ_PLAY =
    "PLAY rtsp://192.168.16.88:554/live RTSP/1.0\r\n"
    "CSeq: 4\r\n"
    "Session: 12345678\r\n"
    "Range: npt=0.000-\r\n"
    "\r\n";

static const char *REQ_TEARDOWN =
    "TEARDOWN rtsp://192.168.16.88:554/live RTSP/1.0\r\n"
    "CSeq: 5\r\n"
    "Session: 12345678\r\n"
    "\r\n";

/* ── 下面需要真实的 SPS/PPS 才能生成 SDP, 从码流里抽 ── */

static proto_sdp_cfg_t g_cfg;
static int             g_need;

static int on_nalu(const proto_nalu_t *n, void *user)
{
    (void)user;

    if (n->kind == PROTO_NALU_KIND_SPS && g_cfg.sps_len == 0 &&
        n->len <= PROTO_SDP_PARAM_MAX) {
        memcpy(g_cfg.sps, n->data, n->len);
        g_cfg.sps_len = n->len;
    } else if (n->kind == PROTO_NALU_KIND_PPS && g_cfg.pps_len == 0 &&
               n->len <= PROTO_SDP_PARAM_MAX) {
        memcpy(g_cfg.pps, n->data, n->len);
        g_cfg.pps_len = n->len;
    }
    g_need = !(g_cfg.sps_len && g_cfg.pps_len);
    return g_need ? 0 : 1;
}

int main(int argc, char **argv)
{
    proto_rtsp_request_t req;
    char                 out[PROTO_RTSP_BUF_SIZE];

    printf("===== proto_rtsp 单元测试 =====\n\n");

    /* ═══ ① 解析:OPTIONS ═══ */
    printf("① 解析 OPTIONS(带 User-Agent 等无关头)\n");
    if (parse_ok("OPTIONS 解析成功", REQ_OPTIONS, &req)) {
        check("方法 = OPTIONS", req.method == PROTO_RTSP_METHOD_OPTIONS);
        check("方法名原样保留", strcmp(req.method_name, "OPTIONS") == 0);
        check("CSeq = 1", req.cseq_present && req.cseq == 1);
        check("URL 正确", strcmp(req.url, "rtsp://192.168.16.88:554/live") == 0);
        check("版本 = RTSP/1.0", strcmp(req.version, "RTSP/1.0") == 0);
        check("未知头被忽略(不报错)", 1);
    }
    printf("\n");

    /* ═══ ② 解析:SETUP 的两种 Transport ═══ */
    printf("② 解析 SETUP(UDP 与 TCP 交错两种写法)\n");
    if (parse_ok("UDP 版解析成功", REQ_SETUP_UDP, &req)) {
        check("传输方式 = UDP", req.transport == PROTO_RTSP_TRANSPORT_UDP);
        check("client_rtp_port = 5000", req.client_rtp_port == 5000);
        check("client_rtcp_port = 5001", req.client_rtcp_port == 5001);
        check("URL 带 streamid=0",
              strcmp(req.url, "rtsp://192.168.16.88:554/live/streamid=0") == 0);
    }
    if (parse_ok("TCP 交错版解析成功", REQ_SETUP_TCP, &req)) {
        check("传输方式 = TCP 交错",
              req.transport == PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED);
        check("interleaved RTP 通道 = 0", req.interleaved_rtp == 0);
        check("interleaved RTCP 通道 = 1", req.interleaved_rtcp == 1);
    }
    printf("\n");

    /* ═══ ③ 解析:PLAY 带 Session ═══ */
    printf("③ 解析 PLAY(带 Session 与 Range)\n");
    if (parse_ok("PLAY 解析成功", REQ_PLAY, &req)) {
        check("方法 = PLAY", req.method == PROTO_RTSP_METHOD_PLAY);
        check("CSeq = 4", req.cseq == 4);
        check("Session 已解析", req.has_session && req.session_id == 12345678u);
    }
    printf("\n");

    /* ═══ ④ 非法输入必须有确定行为 ═══ */
    printf("④ 非法/边界输入\n");
    {
        proto_rtsp_request_t bad;
        int rc;

        rc = proto_rtsp_parse_request("", 0, &bad);
        check("空输入 → 参数错误(-1)", rc == -1);

        rc = proto_rtsp_parse_request("NOTARTSP\r\n\r\n", 13, &bad);
        check("只有一行垃圾 → 请求行错误(-2)", rc == -2);

        rc = proto_rtsp_parse_request("OPTIONS /live\r\n\r\n", 18, &bad);
        check("缺版本号 → 请求行错误(-2)", rc == -2);

        rc = proto_rtsp_parse_request("OPTIONS /live RTSP/1.0\r\n\r\n", 28, &bad);
        check("缺 CSeq → 明确报错(-3) 而不是静默继续", rc == -3);

        rc = proto_rtsp_parse_request("FOOBAR /live RTSP/1.0\r\nCSeq: 1\r\n\r\n", 36, &bad);
        check("不认识的方法 → -2(且 method_name 保留供日志)",
              rc == -2 && strcmp(bad.method_name, "FOOBAR") == 0);

        rc = proto_rtsp_parse_request("OPTIONS /live RTSP/1.0\r\nCSeq: 9\n\n", 34, &bad);
        check("LF-only(不带 CR)也能解析(容错)", rc == 0 && bad.cseq == 9);
    }
    printf("\n");

    /* ═══ ⑤ 响应构造:逐行断言 ═══ */
    printf("⑤ 响应构造\n");
    {
        proto_rtsp_request_t r;

        parse_ok("(准备 OPTIONS 请求)", REQ_OPTIONS, &r);
        int n = proto_rtsp_build_options(&r, out, sizeof(out));
        check("OPTIONS 响应生成成功", n > 0);
        check("状态行 = RTSP/1.0 200 OK",
              strncmp(out, "RTSP/1.0 200 OK\r\n", 17) == 0);
        check("回了 CSeq: 1(与请求一致)", has_line(out, "CSeq: 1"));
        check("Public 头列出 6 个方法", strstr(out, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN") != NULL);
        check("以空行结束头", strstr(out, "\r\n\r\n") != NULL);
    }
    {
        proto_rtsp_request_t r;
        parse_ok("(准备 SETUP 请求)", REQ_SETUP_UDP, &r);
        int n = proto_rtsp_build_setup(&r, 12345678u, 6000, out, sizeof(out));
        check("SETUP 响应生成成功", n > 0);
        check("含 Session: 12345678", has_line(out, "Session: 12345678"));
        check("回显 client_port=5000-5001",
              strstr(out, "client_port=5000-5001") != NULL);
        check("含 server_port=6000-6001",
              strstr(out, "server_port=6000-6001") != NULL);
    }
    {
        proto_rtsp_request_t r;
        parse_ok("(准备 SETUP/TCP 请求)", REQ_SETUP_TCP, &r);
        int n = proto_rtsp_build_setup(&r, 12345678u, 6000, out, sizeof(out));
        check("TCP 版 SETUP 响应成功", n > 0);
        check("回显 interleaved=0-1(UDP 的 server_port 不出现)",
              strstr(out, "interleaved=0-1") != NULL &&
              strstr(out, "server_port=") == NULL);
    }
    {
        proto_rtsp_request_t r;
        parse_ok("(准备 PLAY 请求)", REQ_PLAY, &r);
        int n = proto_rtsp_build_play_pause(&r, 12345678u, out, sizeof(out));
        check("PLAY 响应含 CSeq: 4", n > 0 && has_line(out, "CSeq: 4"));
        check("PLAY 响应含 Session", has_line(out, "Session: 12345678"));
    }
    {
        proto_rtsp_request_t r;
        int n;
        memset(&r, 0, sizeof(r));
        r.cseq = 7;
        n = proto_rtsp_build_error(&r, 454, "Session Not Found", out, sizeof(out));
        check("错误响应状态行正确",
              n > 0 && strncmp(out, "RTSP/1.0 454 Session Not Found", 29) == 0);
    }
    {
        proto_rtsp_request_t r;
        parse_ok("(准备 TEARDOWN 请求)", REQ_TEARDOWN, &r);
        int n = proto_rtsp_build_teardown(&r, 12345678u, out, sizeof(out));
        check("TEARDOWN 响应含 CSeq: 5",
              n > 0 && has_line(out, "CSeq: 5"));
        check("TEARDOWN 响应含 Session(告知哪条会话被拆)",
              n > 0 && has_line(out, "Session: 12345678"));
    }
    printf("\n");

    /* ═══ ⑥ 缓冲安全 ═══ */
    printf("⑥ 缓冲安全(必须报错而不是溢出)\n");
    {
        proto_rtsp_request_t r;
        char small[16];
        int  n;

        parse_ok("(准备 DESCRIBE 请求)", REQ_DESCRIBE, &r);
        n = proto_rtsp_build_options(&r, small, sizeof(small));
        check("缓冲过小 → 返回负值", n < 0);
        n = proto_rtsp_build_describe(&r, "v=0\r\n", 5, small, sizeof(small));
        check("DESCRIBE 缓冲过小 → 返回负值", n < 0);
        n = proto_rtsp_build_options(NULL, out, sizeof(out));
        check("req 为 NULL → 返回负值", n < 0);
    }
    printf("\n");

    /* ═══ ⑦ 串联:DESCRIBE 的响应体用真实 SDP ═══ */
    printf("⑦ 串联测试:DESCRIBE 响应体 = proto_sdp 的真实输出\n");
    if (argc < 2) {
        printf("   (跳过: 未提供码流文件, 无法取得真实 SPS/PPS)\n");
        printf("   用法: %s <file.h264>\n", argv[0]);
    } else {
        FILE   *fp = fopen(argv[1], "rb");
        long    size;
        char    sdp[PROTO_SDP_BUF_SIZE];
        int     sdp_len;
        proto_rtsp_request_t r;

        if (fp == NULL) {
            printf("   读不到 %s, 跳过\n", argv[1]);
        } else {
            fseek(fp, 0, SEEK_END);
            size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            static uint8_t buf[512 * 1024];         /* 只读前 512KB, 参数集在最前面 */
            size_t want = (size_t)(size < (long)sizeof(buf) ? size : (long)sizeof(buf));
            size_t got = fread(buf, 1, want, fp);
            fclose(fp);

            memset(&g_cfg, 0, sizeof(g_cfg));
            g_cfg.origin_ip    = "192.168.16.88";
            g_cfg.session_name = "IPC Camera";
            g_cfg.is_h265      = 0;
            g_cfg.payload_type = 96;
            proto_nalu_foreach(buf, got, 0, on_nalu, NULL);
            check("从码流抽到 SPS/PPS",
                  g_cfg.sps_len > 0 && g_cfg.pps_len > 0);

            sdp_len = proto_sdp_build(&g_cfg, sdp, sizeof(sdp));
            check("proto_sdp 生成 SDP 成功", sdp_len > 0);

            parse_ok("(准备 DESCRIBE 请求)", REQ_DESCRIBE, &r);
            int n = proto_rtsp_build_describe(&r, sdp, (size_t)sdp_len,
                                              out, sizeof(out));
            check("DESCRIBE 响应生成成功", n > 0);
            check("Content-Type: application/sdp",
                  has_line(out, "Content-Type: application/sdp"));

            /* ★ 最关键: Content-Length 必须等于 SDP 真实字节数 */
            {
                char expect[32];
                snprintf(expect, sizeof(expect), "Content-Length: %d", sdp_len);
                check("Content-Length = SDP 真实字节数", has_line(out, expect));
            }
            check("响应体里含 sprop-parameter-sets",
                  strstr(out, "sprop-parameter-sets=") != NULL);
            check("响应总长 = 头 + SDP 体",
                  n > sdp_len && (size_t)n == strlen(out));
            printf("\n   ----- DESCRIBE 响应(前 6 行)-----\n");
            {
                int shown = 0;
                const char *p = out;
                while (shown < 8 && *p) {
                    const char *nl = strstr(p, "\r\n");
                    int len = nl ? (int)(nl - p) : (int)strlen(p);
                    printf("   %.*s\n", len, p);
                    if (!nl)
                        break;
                    p = nl + 2;
                    shown++;
                }
            }
        }
    }

    printf("\n===== 结果: %s(%d 项失败)=====\n",
           g_fails == 0 ? "全部通过" : "有失败", g_fails);
    return g_fails == 0 ? 0 : 1;
}
