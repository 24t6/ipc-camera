/**
 * @file    proto_rtsp.c
 * @brief   RTSP 请求解析 + 响应构造实现
 *
 * 结构(刻意保持每块都短):
 *      ① 解析:请求行 + 头行逐行扫
 *      ② 响应构造:每类响应一个函数
 *
 * 设计取舍:
 *      · 不用 strtok/strcasestr —— 它们依赖 locale 或会改写输入, 不适合解析"别人的数据"
 *      · 不用变参 snprintf —— 整型转文本用共用的 proto_str_* (见 proto_str.h)
 *      · 未知的头一律忽略(RTSP 允许扩展头, 忽略比报错健壮)
 *
 * 文本工具与 proto_sdp.c 共用:proto_str_append() / proto_str_u32() /
 * proto_str_parse_u32() / proto_str_eq_ci*()。
 */
#include "proto_rtsp.h"
#include "proto_str.h"

#include <string.h>

/* 给共用工具起短名字 —— 本文件调用次数多, 短名字让流程更好读 */
#define append(out, cap, used, text)    proto_str_append((out), (cap), (used), (text))
#define append_u32(out, cap, used, v)   proto_str_append_u32((out), (cap), (used), (v))
#define parse_u32(p, out)               proto_str_parse_u32((p), (out))
#define eq_ci(a, b)                     proto_str_eq_ci((a), (b))
#define eq_ci_n(a, b, n)                proto_str_eq_ci_n((a), (b), (n))

/* ─────────────── ① 解析:行扫描 ─────────────── */

/** 取出一行到 line 缓冲; 返回下一行起点, 到末尾返回 NULL */
static const char *next_line(const char *p, const char *end,
                             char *line, size_t cap)
{
    const char *nl = p;
    size_t      n;

    while (nl < end && *nl != '\n')
        nl++;
    n = (size_t)(nl - p);
    if (n > 0 && p[n - 1] == '\r')
        n--;                            /* 去掉 CR */
    if (n >= cap)
        n = cap - 1;
    memcpy(line, p, n);
    line[n] = '\0';

    return (nl < end) ? nl + 1 : NULL;
}

/* ─────────────── ② 解析:请求行与头行 ─────────────── */

/** 把方法名文本映射成枚举 */
static proto_rtsp_method_t method_from_name(const char *name)
{
    if (eq_ci(name, "OPTIONS"))       return PROTO_RTSP_METHOD_OPTIONS;
    if (eq_ci(name, "DESCRIBE"))      return PROTO_RTSP_METHOD_DESCRIBE;
    if (eq_ci(name, "SETUP"))         return PROTO_RTSP_METHOD_SETUP;
    if (eq_ci(name, "PLAY"))          return PROTO_RTSP_METHOD_PLAY;
    if (eq_ci(name, "PAUSE"))         return PROTO_RTSP_METHOD_PAUSE;
    if (eq_ci(name, "TEARDOWN"))      return PROTO_RTSP_METHOD_TEARDOWN;
    if (eq_ci(name, "GET_PARAMETER")) return PROTO_RTSP_METHOD_GET_PARAMETER;
    return PROTO_RTSP_METHOD_UNKNOWN;
}

/** 解析 "a-b" 这种端口/通道对, 写进 lo/hi。返回是否解析到了 hi。 */
static int parse_pair(const char *p, uint32_t *lo, uint32_t *hi)
{
    size_t n = parse_u32(p, lo);

    if (n == 0)
        return 0;
    p += n;
    if (*p != '-')
        return 0;
    p++;
    n = parse_u32(p, hi);
    return (n > 0);
}

/**
 * 解析 Transport 头。
 *
 * 两种写法(见第 3 课 / ADR-1):
 *   UDP : RTP/AVP;unicast;client_port=5000-5001
 *   TCP : RTP/AVP/TCP;unicast;interleaved=0-1
 */
static void parse_transport(proto_rtsp_request_t *req, const char *v)
{
    const char *key;
    uint32_t    lo = 0;
    uint32_t    hi = 0;

    req->transport = PROTO_RTSP_TRANSPORT_UNKNOWN;

    /* 先判断传输方式:值里出现 "RTP/AVP/TCP" 就是交错模式 */
    if (eq_ci_n(v, "RTP/AVP/TCP", 11)) {
        req->transport = PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED;
        key = "interleaved=";
    } else if (eq_ci_n(v, "RTP/AVP", 7)) {
        req->transport = PROTO_RTSP_TRANSPORT_UDP;
        key = "client_port=";
    } else {
        return;                         /* 不认识的传输方式, 保持 UNKNOWN */
    }

    /* 在值里找 key=, 找到就解析 "a-b" */
    {
        size_t key_len = strlen(key);
        const char *p  = v;

        while (*p) {
            if (eq_ci_n(p, key, key_len)) {
                if (parse_pair(p + key_len, &lo, &hi)) {
                    if (req->transport == PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED) {
                        req->interleaved_rtp  = (uint8_t)lo;
                        req->interleaved_rtcp = (uint8_t)hi;
                    } else {
                        req->client_rtp_port  = (uint16_t)lo;
                        req->client_rtcp_port = (uint16_t)hi;
                    }
                }
                return;
            }
            p++;
        }
    }
}

int proto_rtsp_parse_request(const char *buf, size_t len, proto_rtsp_request_t *req)
{
    char        line[PROTO_RTSP_LINE_MAX];
    const char *p;
    const char *end;
    int         first = 1;

    if (buf == NULL || req == NULL || len == 0)
        return -1;

    memset(req, 0, sizeof(*req));
    req->cseq = -1;
    req->method = PROTO_RTSP_METHOD_UNKNOWN;

    p   = buf;
    end = buf + len;

    while (p != NULL && p < end) {
        p = next_line(p, end, line, sizeof(line));
        if (line[0] == '\0')
            break;                      /* 空行 = 头结束 */

        if (first) {
            /* ── 请求行: METHOD SP URL SP VERSION ── */
            const char *sp1 = strchr(line, ' ');
            const char *sp2;
            size_t      n;

            if (sp1 == NULL)
                return -2;              /* 连方法名和 URL 都分不开 */
            n = (size_t)(sp1 - line);
            if (n == 0 || n >= sizeof(req->method_name))
                return -2;
            memcpy(req->method_name, line, n);
            req->method_name[n] = '\0';
            req->method = method_from_name(req->method_name);

            sp2 = strchr(sp1 + 1, ' ');
            if (sp2 == NULL)
                return -2;              /* 缺版本号 */

            n = (size_t)(sp2 - (sp1 + 1));
            if (n == 0 || n >= sizeof(req->url))
                return -2;
            memcpy(req->url, sp1 + 1, n);
            req->url[n] = '\0';

            n = strlen(sp2 + 1);
            if (n == 0 || n >= sizeof(req->version))
                return -2;
            memcpy(req->version, sp2 + 1, n);
            req->version[n] = '\0';

            first = 0;
            continue;
        }

        /* ── 头行: Name: value ── */
        {
            const char *colon = strchr(line, ':');
            const char *val;
            size_t      name_len;

            if (colon == NULL)
                continue;               /* 不是合法头行, 忽略 */
            name_len = (size_t)(colon - line);
            val = colon + 1;
            while (*val == ' ' || *val == '\t')
                val++;                  /* 跳过 OWS */

            if (eq_ci_n(line, "CSeq", name_len) && name_len == 4) {
                uint32_t v = 0;
                if (parse_u32(val, &v) > 0) {
                    req->cseq = (int)v;
                    req->cseq_present = 1;
                }
            } else if (eq_ci_n(line, "Session", name_len) && name_len == 7) {
                uint32_t v = 0;
                if (parse_u32(val, &v) > 0) {
                    req->session_id = v;
                    req->has_session = 1;
                }
            } else if (eq_ci_n(line, "Transport", name_len) && name_len == 9) {
                parse_transport(req, val);
            }
            /* 其它头一律忽略 */
        }
    }

    if (first)
        return -2;                      /* 一行都没读到 */
    if (req->method == PROTO_RTSP_METHOD_UNKNOWN) {
        /*
         * 请求行语法是好的(方法/URL/版本都在), 只是这个方法我们不认识。
         * 这跟"报文畸形"是两回事: 前者该回 **405 Method Not Allowed**,
         * 后者才是 400 Bad Request。所以这里单独给一个错误码 -4。
         *
         * 为什么值得专门区分: 客户端拿到 405 会知道"服务器在, 只是不支持这个动作",
         * 拿到 400 则会怀疑自己报文写错了 —— 排障方向完全不同。
         * method_name 已经被原样保留下来, 调用方可以直接拿去打日志或报错。
         */
        return -4;
    }
    if (req->cseq_present == 0)
        return -3;                      /* 没有 CSeq: 无法构造可对应上的响应 */

    /* 顺带把 client_rtcp_port 的默认值补齐 */
    if (req->transport == PROTO_RTSP_TRANSPORT_UDP &&
        req->client_rtp_port != 0 && req->client_rtcp_port == 0)
        req->client_rtcp_port = (uint16_t)(req->client_rtp_port + 1);

    return 0;
}

const char *proto_rtsp_method_name(proto_rtsp_method_t m)
{
    switch (m) {
    case PROTO_RTSP_METHOD_OPTIONS:       return "OPTIONS";
    case PROTO_RTSP_METHOD_DESCRIBE:      return "DESCRIBE";
    case PROTO_RTSP_METHOD_SETUP:         return "SETUP";
    case PROTO_RTSP_METHOD_PLAY:          return "PLAY";
    case PROTO_RTSP_METHOD_PAUSE:         return "PAUSE";
    case PROTO_RTSP_METHOD_TEARDOWN:      return "TEARDOWN";
    case PROTO_RTSP_METHOD_GET_PARAMETER: return "GET_PARAMETER";
    default:                              return "UNKNOWN";
    }
}

/* ─────────────── ③ 响应构造 ─────────────── */

/**
 * 写状态行 + 必备的 CSeq 头(**不写结尾空行**)。
 *
 * @note **每个响应都必须回 CSeq**, 且值要和请求一致 ——
 *       否则客户端无法把响应和请求对应起来, 会一直等或直接断开。
 * @note 之所以不在这里写结尾空行: 响应头还没写完(后面还要加 Content-Length
 *       和各自的头), 空行必须留到最后由 resp_finish() 来补。
 */
static int status_and_cseq(char *out, size_t cap, size_t *used,
                           int code, const char *reason, int cseq)
{
    if (append(out, cap, used, "RTSP/1.0 ") != 0)
        return -1;
    if (append_u32(out, cap, used, (uint32_t)code) != 0)
        return -1;
    if (append(out, cap, used, " ") != 0 || append(out, cap, used, reason) != 0)
        return -1;
    if (append(out, cap, used, "\r\nCSeq: ") != 0)
        return -1;
    if (cseq >= 0 && append_u32(out, cap, used, (uint32_t)cseq) != 0)
        return -1;
    return append(out, cap, used, "\r\n");
}

/** 消息体的 MIME 类型(只有 DESCRIBE 有体)。用枚举而不是字符串, 防止拼错。 */
typedef enum {
    RES_TYPE_NONE = 0,          /* 没有消息体 → 不发 Content-Type */
    RES_TYPE_SDP,               /* application/sdp */
} resp_body_type_t;

static const char *content_type_text(resp_body_type_t t)
{
    switch (t) {
    case RES_TYPE_SDP: return "application/sdp";
    default:           return NULL;
    }
}

/**
 * ★★ 所有响应的**唯一出口** —— 由它保证"每个响应都带 Content-Length"。
 *
 * @param extra       额外的头(可以为 NULL), 必须以 "\r\n" 结尾
 * @param body_type   消息体的 MIME 类型(无体就传 RES_TYPE_NONE)
 * @param body        消息体(可以为 NULL)
 * @param body_len    消息体字节数
 *
 * @note **为什么必须做成唯一出口**(来自 B017):
 *       原来五个构造函数各写各的头, 结果 `build_options` / `build_setup` /
 *       `build_play_pause` / `build_teardown` 四个都**漏了 Content-Length**。
 *       实测后果:curl 挂到超时、同一客户端反复重连(CSeq 全是 1)。
 *       RTSP(和 HTTP 一样)里"没有长度、又不关连接" = 客户端不知道消息体
 *       在哪结束 → 只能一直等。
 *       **靠"每个分支都记得写"的设计是脆弱的设计**; 改成唯一出口之后,
 *       想漏都漏不掉。这和 B015(接口设计逼着调用方用错)是同一类问题。
 *
 * @note 消息体长度按**字节**算, 不是字符数 —— 写错了客户端会一直等。
 * @note 非阻塞、不分配内存; 缓冲不够返回 -1(调用方负责断连, 不能截断发送)。
 */
static int resp_finish(char *out, size_t cap, size_t *used,
                       const char *extra, resp_body_type_t body_type,
                       const char *body, size_t body_len)
{
    const char *ct = content_type_text(body_type);

    if (body == NULL && body_len > 0)
        return -1;
    if (extra != NULL && append(out, cap, used, extra) != 0)
        return -1;
    if (ct != NULL && append(out, cap, used, "Content-Type: ") != 0)
        return -1;
    if (ct != NULL && append(out, cap, used, ct) != 0)
        return -1;
    if (ct != NULL && append(out, cap, used, "\r\n") != 0)
        return -1;

    /* ↓ 这一行就是"唯一出口"的核心:任何响应都躲不过它 */
    if (append(out, cap, used, "Content-Length: ") != 0)
        return -1;
    if (append_u32(out, cap, used, (uint32_t)body_len) != 0)
        return -1;
    if (append(out, cap, used, "\r\n\r\n") != 0)        /* 空行: 头结束 */
        return -1;

    if (body_len > 0) {
        if (*used + body_len + 1 > cap)
            return -1;
        memcpy(out + *used, body, body_len);
        *used += body_len;
        out[*used] = '\0';
    }
    return (int)*used;
}

int proto_rtsp_build_options(const proto_rtsp_request_t *req, char *out, size_t cap)
{
    size_t used = 0;

    if (req == NULL || out == NULL)
        return -1;
    if (status_and_cseq(out, cap, &used, 200, "OK", req->cseq) != 0)
        return -1;
    /* 列出我们真正实现的方法; VLC 靠这个决定界面按钮 */
    return resp_finish(out, cap, &used,
                       "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n",
                       RES_TYPE_NONE, NULL, 0);
}

int proto_rtsp_build_describe(const proto_rtsp_request_t *req,
                              const char *sdp, size_t sdp_len,
                              char *out, size_t cap)
{
    size_t used = 0;

    if (req == NULL || out == NULL || (sdp == NULL && sdp_len > 0))
        return -1;
    if (status_and_cseq(out, cap, &used, 200, "OK", req->cseq) != 0)
        return -1;
    return resp_finish(out, cap, &used, NULL, RES_TYPE_SDP, sdp, sdp_len);
}

int proto_rtsp_build_setup(const proto_rtsp_request_t *req, uint32_t session_id,
                           uint16_t server_rtp_port, char *out, size_t cap)
{
    size_t used = 0;

    if (req == NULL || out == NULL)
        return -1;
    if (status_and_cseq(out, cap, &used, 200, "OK", req->cseq) != 0)
        return -1;
    if (append(out, cap, &used, "Session: ") != 0)
        return -1;
    if (append_u32(out, cap, &used, session_id) != 0)
        return -1;
    if (append(out, cap, &used, "\r\n") != 0)
        return -1;

    /* 把协商结果原样回给客户端 —— 这是 SETUP 响应最关键的部分 */
    if (append(out, cap, &used, "Transport: ") != 0)
        return -1;
    if (req->transport == PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED) {
        if (append(out, cap, &used, "RTP/AVP/TCP;unicast;interleaved=") != 0)
            return -1;
        if (append_u32(out, cap, &used, req->interleaved_rtp) != 0)
            return -1;
        if (append(out, cap, &used, "-") != 0)
            return -1;
        if (append_u32(out, cap, &used, req->interleaved_rtcp) != 0)
            return -1;
    } else {
        if (append(out, cap, &used, "RTP/AVP;unicast;client_port=") != 0)
            return -1;
        if (append_u32(out, cap, &used, req->client_rtp_port) != 0)
            return -1;
        if (append(out, cap, &used, "-") != 0)
            return -1;
        if (append_u32(out, cap, &used, req->client_rtcp_port) != 0)
            return -1;
        if (append(out, cap, &used, ";server_port=") != 0)
            return -1;
        if (append_u32(out, cap, &used, server_rtp_port) != 0)
            return -1;
        if (append(out, cap, &used, "-") != 0)
            return -1;
        if (append_u32(out, cap, &used, (uint32_t)(server_rtp_port + 1)) != 0)
            return -1;
    }
    if (append(out, cap, &used, "\r\n") != 0)
        return -1;

    return resp_finish(out, cap, &used, NULL, RES_TYPE_NONE, NULL, 0);
}

int proto_rtsp_build_play_pause(const proto_rtsp_request_t *req, uint32_t session_id,
                                char *out, size_t cap)
{
    size_t used = 0;

    if (req == NULL || out == NULL)
        return -1;
    if (status_and_cseq(out, cap, &used, 200, "OK", req->cseq) != 0)
        return -1;
    if (append(out, cap, &used, "Session: ") != 0)
        return -1;
    if (append_u32(out, cap, &used, session_id) != 0)
        return -1;
    if (append(out, cap, &used, "\r\n") != 0)
        return -1;

    return resp_finish(out, cap, &used, NULL, RES_TYPE_NONE, NULL, 0);
}

int proto_rtsp_build_teardown(const proto_rtsp_request_t *req, uint32_t session_id,
                              char *out, size_t cap)
{
    size_t used = 0;

    if (req == NULL || out == NULL)
        return -1;
    if (status_and_cseq(out, cap, &used, 200, "OK", req->cseq) != 0)
        return -1;
    if (append(out, cap, &used, "Session: ") != 0)
        return -1;
    if (append_u32(out, cap, &used, session_id) != 0)
        return -1;
    if (append(out, cap, &used, "\r\n") != 0)
        return -1;

    return resp_finish(out, cap, &used, NULL, RES_TYPE_NONE, NULL, 0);
}

int proto_rtsp_build_error(const proto_rtsp_request_t *req, int code,
                           const char *reason, char *out, size_t cap)
{
    size_t used = 0;

    if (out == NULL || reason == NULL)
        return -1;
    /* 注意: 解析失败的请求可能没有 CSeq, 此时 req->cseq 为 -1, 不回 CSeq 头 */
    if (status_and_cseq(out, cap, &used, code, reason,
                        req ? req->cseq : -1) != 0)
        return -1;

    return resp_finish(out, cap, &used, NULL, RES_TYPE_NONE, NULL, 0);
}
