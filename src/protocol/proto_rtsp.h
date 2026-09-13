/**
 * @file    proto_rtsp.h
 * @brief   RTSP 请求解析 + 响应构造 —— 流媒体的"遥控器"
 *
 * @details
 * RTSP(Real Time Streaming Protocol, 实时流传输协议)在项目里扮演**遥控器**:
 * 它**不搬视频数据**(那是 RTP 的活), 只负责"开始 / 暂停 / 停止 / 查询"。
 *
 * ─────────────────────────────────────────────────────────────────
 *  完整对话(本项目设计, 与 docs/概念笔记/07-SDP会话描述.md 一致)
 * ─────────────────────────────────────────────────────────────────
 *      ① OPTIONS   → 问"你支持哪些命令?"      答 Public: OPTIONS, DESCRIBE, …
 *      ② DESCRIBE  → 要媒体说明书              答 application/sdp(见 proto_sdp.c)
 *      ③ SETUP     → 告诉我你的 UDP 端口       答 Session: <id> + Transport
 *      ④ PLAY      → 开始发数据                此后视频走 RTP/UDP, RTSP 基本不管
 *      ⑤ PAUSE     → 暂停(不断开)
 *      ⑥ TEARDOWN  → 结束、释放
 *
 * ─────────────────────────────────────────────────────────────────
 *  为什么 RTSP 走 TCP, 而 RTP 走 UDP(见第 3 课)
 * ─────────────────────────────────────────────────────────────────
 *  命令**不能丢**:丢了 PLAY 客户端会一直等, 丢了 TEARDOWN 会话会泄漏。
 *  命令量小、频率低, 用 TCP 的可靠性换性能损失非常划算。
 *  而视频数据量大、要求实时, 宁可丢也不等重传 → UDP。
 *
 * ─────────────────────────────────────────────────────────────────
 *  本模块的边界(刻意划清)
 * ─────────────────────────────────────────────────────────────────
 *  ✅ 只做:解析请求文本、构造响应文本、管理"会话号"这个纯数字状态
 *  ❌ 不做:收发 socket(那是 infra_netio)、维护客户端列表(那是 svc_net)
 *
 *  这样它**不碰网络、不碰硬件、不做动态分配** → 可以在 PC 上原生单测,
 *  不需要板子、不需要网线, 甚至不需要 socket。
 *
 * @note 文本行尾一律 `\r\n`(RTSP/SDP 都按 CRLF 解析), 响应里必须写对。
 */
#ifndef __PROTO_RTSP_H__
#define __PROTO_RTSP_H__

#include <stddef.h>
#include <stdint.h>

/** 单行最大长度(含 \r\n)。RTSP 请求行/头行都很短, 512 足够 */
#define PROTO_RTSP_LINE_MAX 512

/** 响应缓冲建议大小 */
#define PROTO_RTSP_BUF_SIZE 2048

/** RTSP 方法(本项目支持的 6 个) */
typedef enum {
    PROTO_RTSP_METHOD_UNKNOWN = 0,
    PROTO_RTSP_METHOD_OPTIONS,
    PROTO_RTSP_METHOD_DESCRIBE,
    PROTO_RTSP_METHOD_SETUP,
    PROTO_RTSP_METHOD_PLAY,
    PROTO_RTSP_METHOD_PAUSE,
    PROTO_RTSP_METHOD_TEARDOWN,
    PROTO_RTSP_METHOD_GET_PARAMETER,    /* 有些客户端用它做保活, 我们不实现但可识别 */
} proto_rtsp_method_t;

/** 传输方式(由 SETUP 的 Transport 头决定) */
typedef enum {
    PROTO_RTSP_TRANSPORT_UNKNOWN = 0,
    PROTO_RTSP_TRANSPORT_UDP,           /* RTP/AVP;unicast;client_port=5000-5001 */
    PROTO_RTSP_TRANSPORT_TCP_INTERLEAVED, /* RTP/AVP/TCP;interleaved=0-1 (ADR-1 预留) */
} proto_rtsp_transport_t;

/** 一个解析出来的 RTSP 请求 */
typedef struct {
    proto_rtsp_method_t method;
    char                method_name[16];    /* 原始方法名, 便于打日志/报错 */

    char                url[256];           /* 请求行里的 URL */
    char                version[16];        /* 通常是 "RTSP/1.0" */

    int                 cseq;               /* CSeq, 必须存在; 不存在则 cseq_present=0 */
    int                 cseq_present;

    int                 has_session;        /* 有没有带 Session 头 */
    uint32_t            session_id;

    proto_rtsp_transport_t transport;
    uint16_t            client_rtp_port;    /* UDP 时有效: RTP 端口 */
    uint16_t            client_rtcp_port;   /* UDP 时有效: RTCP 端口(= RTP+1) */
    uint8_t             interleaved_rtp;    /* TCP 交错时有效: RTP 通道号 */
    uint8_t             interleaved_rtcp;   /* TCP 交错时有效: RTCP 通道号 */
} proto_rtsp_request_t;

/**
 * 解析一段 RTSP 请求。
 *
 * @param buf   收到的字节(不一定以 '\0' 结尾, 所以必须给 len)
 * @param len   buf 长度
 * @param req   输出:解析结果(失败时也会尽量填上已解析到的部分, 便于打日志)
 * @return 0=成功; 负值为错误:
 *         -1 = 参数非法, -2 = 不是合法的 RTSP 请求行, -3 = 缺少 CSeq(不可恢复)
 *
 * @note 只解析本项目需要的字段; 未知的头**直接忽略**(协议允许, 也让实现更健壮)。
 * @note 不阻塞、不分配内存、无静态状态(可重入)。
 */
int proto_rtsp_parse_request(const char *buf, size_t len, proto_rtsp_request_t *req);

/** 方法名(用于日志)。未知返回 "UNKNOWN"。 */
const char *proto_rtsp_method_name(proto_rtsp_method_t m);

/* ─────────────────── 响应构造 ─────────────────── */

/**
 * OPTIONS 响应 —— 告诉客户端我们支持哪些命令。
 * @return 写入字符数; 负值 = 缓冲不够
 * @note 必须在 Public 头里列出 **PAUSE**(我们实现了), 否则部分客户端不显示暂停按钮。
 */
int proto_rtsp_build_options(const proto_rtsp_request_t *req, char *out, size_t cap);

/**
 * DESCRIBE 响应 —— 把 SDP 作为消息体返回。
 *
 * @param sdp       已经生成好的 SDP 文本(通常来自 proto_sdp_build)
 * @param sdp_len   SDP 字节数
 * @note Content-Length 必须是**字节数**, 写错了 VLC 会一直等消息体直到超时。
 */
int proto_rtsp_build_describe(const proto_rtsp_request_t *req,
                              const char *sdp, size_t sdp_len,
                              char *out, size_t cap);

/**
 * SETUP 响应 —— 分配会话号, 并把协商结果回给客户端。
 *
 * @param session_id      服务器分配的随机会话号
 * @param server_rtp_port 服务器本地收 RTCP 用的端口(回显给客户端便于诊断)
 */
int proto_rtsp_build_setup(const proto_rtsp_request_t *req, uint32_t session_id,
                           uint16_t server_rtp_port, char *out, size_t cap);

/**
 * PLAY / PAUSE 响应 —— 只是确认, 但要带上 Session。
 * @note PLAY 之后数据才真正开始发; 但"开始发"这个动作在 svc_net 里, 不在这里。
 */
int proto_rtsp_build_play_pause(const proto_rtsp_request_t *req, uint32_t session_id,
                                char *out, size_t cap);

/** TEARDOWN 响应 —— 会话就此结束。 */
int proto_rtsp_build_teardown(const proto_rtsp_request_t *req, uint32_t session_id,
                              char *out, size_t cap);

/**
 * 通用错误响应(如 400 Bad Request / 454 Session Not Found)。
 * @param code  三位状态码, 如 400 / 454 / 500
 * @param reason 原因短语, 如 "Bad Request"
 */
int proto_rtsp_build_error(const proto_rtsp_request_t *req, int code,
                           const char *reason, char *out, size_t cap);

#endif /* __PROTO_RTSP_H__ */
