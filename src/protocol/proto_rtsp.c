/**
 * @file    proto_rtsp.c
 * @brief   RTSP 请求解析 + 响应构造实现
 *
 * 【模块职责】RTSP 请求解析与响应构造(CSeq / Session / Transport / Content-Length)
 * 【依赖方向】只依赖 proto_str 与 libc
 * 【线程模型】纯函数; 解析结果写到调用方提供的结构体里
 * 【资源边界】无动态分配; 不保存任何跨调用的状态
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

/** @brief 取出一行到 line 缓冲; 返回下一行起点, 到末尾返回 NULL */
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

/** @brief 把方法名文本映射成枚举 */
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

/** @brief 解析 "a-b" 这种端口/通道对, 写进 lo/hi。返回是否解析到了 hi。 */
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
 * @brief 在一行头值里找 `key=`, 找到了就把后面的 "a-b" 解析出来。
 *
 * @param v        头值(如 "RTP/AVP;unicast;client_port=5000-5001")
 * @param key      要找的参数名(含 '=', 如 "client_port=")
 * @param[out] lo  输出: 前半部分
 * @param[out] hi  输出: 后半部分
 * @return 1 = 找到了且解析成功
 *
 * @note 抽出来的理由: 原来这段 while 嵌在 if 里, 加上类型分支一共 **6 层缩进**。
 *       内核的规矩是"超过 3 层就该改程序" —— 这里正是那个情况。
 */
static int find_param_pair(const char *v, const char *key,
                           uint32_t *lo, uint32_t *hi)
{
    size_t      key_len = strlen(key);
    const char *p       = v;

    for (; *p != '\0'; p++) {
        if (eq_ci_n(p, key, key_len))
            return parse_pair(p + key_len, lo, hi);
    }
    return 0;
}

/** @brief 把解析出来的 lo/hi 存进请求结构(按传输方式决定存哪一组字段) */
static void store_transport_pair(proto_rtsp_request_t *req,
                                 uint32_t lo, uint32_t hi)
{
    if (req->transport == PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED) {
        req->interleaved_rtp  = (uint8_t)lo;
        req->interleaved_rtcp = (uint8_t)hi;
    } else {
        req->client_rtp_port  = (uint16_t)lo;
        req->client_rtcp_port = (uint16_t)hi;
    }
}

/**
 * @brief 解析 Transport 头。
 *
 * 两种写法(见第 3 课 / ADR-1):
 *   UDP : RTP/AVP;unicast;client_port=5000-5001
 *   TCP : RTP/AVP/TCP;unicast;interleaved=0-1
 *
 * @note 只认这两种。不认识的传输方式保持 UNKNOWN ——
 *       **不猜**, 让上层能明确地拒绝, 而不是按错误的假设继续跑。
 */
static void parse_transport(proto_rtsp_request_t *req, const char *v)
{
    const char *key;
    uint32_t    lo = 0;
    uint32_t    hi = 0;

    req->transport = PROTO_RTSP_TRANSPORT_UNKNOWN;

    if (eq_ci_n(v, "RTP/AVP/TCP", 11)) {
        req->transport = PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED;
        key            = "interleaved=";
    } else if (eq_ci_n(v, "RTP/AVP", 7)) {
        req->transport = PROTO_RTSP_TRANSPORT_UDP;
        key            = "client_port=";
    } else {
        return;
    }

    if (find_param_pair(v, key, &lo, &hi))
        store_transport_pair(req, lo, hi);
}

/**
 * @brief 把字段名和值搬进 dst(带长度检查)。
 * @return 0 成功; -2 = 放不下(报文畸形或恶意超长)
 */
static int copy_field(char *dst, size_t cap, const char *src, size_t len)
{
    if (len == 0 || len >= cap)
        return -2;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

/**
 * @brief 解析请求行: `METHOD SP URL SP VERSION`。
 * @return 0 成功; -2 = 畸形
 *
 * @note 拆出来的理由: 原来这段嵌在"逐行扫描"的循环里(while + if(first) + 三个
 *       逐段解析), 缩进 5 层、逻辑两件事混在一起("怎么分行"和"怎么切三段")。
 */
static int parse_request_line(proto_rtsp_request_t *req, const char *line)
{
    const char *sp1 = strchr(line, ' ');
    const char *sp2;
    int         rc;

    if (sp1 == NULL)
        return -2;                      /* 连方法名和 URL 都分不开 */

    rc = copy_field(req->method_name, sizeof(req->method_name),
                    line, (size_t)(sp1 - line));
    if (rc != 0)
        return rc;
    req->method = method_from_name(req->method_name);

    sp2 = strchr(sp1 + 1, ' ');
    if (sp2 == NULL)
        return -2;                      /* 缺版本号 */

    rc = copy_field(req->url, sizeof(req->url),
                    sp1 + 1, (size_t)(sp2 - (sp1 + 1)));
    if (rc != 0)
        return rc;

    return copy_field(req->version, sizeof(req->version),
                      sp2 + 1, strlen(sp2 + 1));
}

/**
 * @brief 解析一行头: `Name: value`。
 *
 * @note 本项目只关心 CSeq / Session / Transport 三个, **其它头一律忽略** ——
 *       协议允许扩展头, 忽略比报错健壮(客户端会发一堆 User-Agent/Accept)。
 */
static void parse_header_line(proto_rtsp_request_t *req, const char *line)
{
    const char *colon = strchr(line, ':');
    const char *val;
    size_t      name_len;
    uint32_t    v = 0;

    if (colon == NULL)
        return;                         /* 不是合法头行, 忽略 */
    name_len = (size_t)(colon - line);
    val      = colon + 1;
    while (*val == ' ' || *val == '\t')
        val++;                          /* 跳过 OWS(可选空白) */

    if (name_len == 4 && eq_ci_n(line, "CSeq", 4)) {
        if (parse_u32(val, &v) > 0) {
            req->cseq         = (int)v;
            req->cseq_present = 1;
        }
    } else if (name_len == 7 && eq_ci_n(line, "Session", 7)) {
        if (parse_u32(val, &v) > 0) {
            req->session_id  = v;
            req->has_session = 1;
        }
    } else if (name_len == 9 && eq_ci_n(line, "Transport", 9)) {
        parse_transport(req, val);
    }
    /* 其它头: 忽略 */
}

/**
 * @brief 逐行扫描, 填 req。@return 0 成功; 其余为错误码(见 proto_rtsp.h)
 *
 * @note 拆出来的理由: 让 parse_request 只剩"清空 → 扫描 → 校验"三步。
 *       原来三者挤在一个函数里, 到 58 行。
 */
static int scan_lines(const char *buf, size_t len, proto_rtsp_request_t *req)
{
    char        line[PROTO_RTSP_LINE_MAX];
    const char *p   = buf;
    const char *end = buf + len;
    int         first = 1;

    while (p != NULL && p < end) {
        int rc;

        p = next_line(p, end, line, sizeof(line));
        if (line[0] == '\0')
            break;                      /* 空行 = 头结束 */

        if (first) {
            rc = parse_request_line(req, line);
            if (rc != 0)
                return rc;
            first = 0;
            continue;
        }
        parse_header_line(req, line);
    }

    return first ? -2 : 0;              /* first 还是 1 = 一行都没读到 */
}

/**
 * @brief 扫描完之后的一致性校验与默认值补齐。
 * @return 0 通过; -3 = 缺 CSeq; -4 = 方法不认识(请求行本身合法)
 *
 * @note -3 和 -4 为什么要分开: 见下面 -4 处的注释。
 */
static int validate_request(proto_rtsp_request_t *req)
{
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

    /* 顺带把 client_rtcp_port 的默认值补齐(RTCP 约定是 RTP 端口 +1) */
    if (req->transport == PROTO_RTSP_TRANSPORT_UDP &&
        req->client_rtp_port != 0 && req->client_rtcp_port == 0)
        req->client_rtcp_port = (uint16_t)(req->client_rtp_port + 1);

    return 0;
}

int proto_rtsp_parse_request(const char *buf, size_t len, proto_rtsp_request_t *req)
{
    int rc;

    if (buf == NULL || req == NULL || len == 0)
        return -1;

    memset(req, 0, sizeof(*req));
    req->cseq   = -1;
    req->method = PROTO_RTSP_METHOD_UNKNOWN;

    rc = scan_lines(buf, len, req);
    if (rc != 0)
        return rc;

    return validate_request(req);
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
 * @brief 写状态行 + 必备的 CSeq 头(**不写结尾空行**)。
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

/**
 * @brief 取响应体类型对应的 Content-Type 文本
 *
 * @param t 响应体类型
 * @return 静态字符串(**不需要释放**)
 */
static const char *content_type_text(resp_body_type_t t)
{
    switch (t) {
    case RES_TYPE_SDP: return "application/sdp";
    default:           return NULL;
    }
}

/**
 * @brief ★★ 所有响应的**唯一出口** —— 由它保证"每个响应都带 Content-Length"。
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

/**
 * @brief 写 SETUP 响应的 Transport 头 —— **把协商结果原样回给客户端**。
 *
 * 这是 SETUP 响应最关键的部分: 客户端要据此知道"服务器同意用什么方式收流"。
 *
 * @param server_rtp_port 服务器本地端口(UDP 时回显, 便于客户端诊断)
 * @return 0 成功; -1 缓冲不够
 *
 * @note 拆出来的理由: 原来两个分支各写 4~8 次 "append + 判返回",
 *       把 build_setup 撑到 47 行代码。**两个分支的差别只是"写哪些字段",
 *       不是"怎么判断错误"** —— 所以把字段拼装独立出来。
 */
static int append_transport(char *out, size_t cap, size_t *used,
                            const proto_rtsp_request_t *req,
                            uint16_t server_rtp_port)
{
    if (append(out, cap, used, "Transport: ") != 0)
        return -1;

    if (req->transport == PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED) {
        /* 交错模式: 回显客户端要求的通道号(RFC 2326 §10.12) */
        if (append(out, cap, used, "RTP/AVP/TCP;unicast;interleaved=") != 0 ||
            append_u32(out, cap, used, req->interleaved_rtp) != 0 ||
            append(out, cap, used, "-") != 0 ||
            append_u32(out, cap, used, req->interleaved_rtcp) != 0)
            return -1;
    } else {
        /* UDP: 回显客户端端口, 并告知我们的端口 */
        if (append(out, cap, used, "RTP/AVP;unicast;client_port=") != 0 ||
            append_u32(out, cap, used, req->client_rtp_port) != 0 ||
            append(out, cap, used, "-") != 0 ||
            append_u32(out, cap, used, req->client_rtcp_port) != 0 ||
            append(out, cap, used, ";server_port=") != 0 ||
            append_u32(out, cap, used, server_rtp_port) != 0 ||
            append(out, cap, used, "-") != 0 ||
            append_u32(out, cap, used, (uint32_t)(server_rtp_port + 1)) != 0)
            return -1;
    }
    return append(out, cap, used, "\r\n");
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
    if (append_transport(out, cap, &used, req, server_rtp_port) != 0)
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
