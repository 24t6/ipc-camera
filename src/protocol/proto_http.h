/**
 * @file    proto_http.h
 * @brief   HTTP/1.1 请求解析 + 响应构造(**纯函数**, 不碰 socket)
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  【模块职责】解析 HTTP 请求(请求行 + 头)与构造响应头; 判定"这个路径要什么"
 *  【依赖方向】只依赖 proto_str 与 libc
 *  【线程模型】纯函数, 无状态; 解析结果写进调用方提供的结构体
 *  【资源边界】无动态分配, 无静态状态, 全部输出带容量检查
 *
 * ─────────────────────────────────────────────────────────────────
 *  为什么单独做一个 HTTP 协议模块
 * ─────────────────────────────────────────────────────────────────
 *  阶段 2「回放」要在板子上提供:
 *      GET  /recordings                  列出现有分段(设备给 URI, 客户端原样回填)
 *      GET  /recordings/<名字>            按 **Range** 取流(播放器拖进度条靠它)
 *      PUT  /recordings/<名字>/lock       锁定 / 解锁(只影响 `.locked` 清单)
 *  这些全靠"解析请求 + 拼响应头"这两件事 —— 和 RTSP 一样是**纯文本逻辑**,
 *  所以照老规矩放进 `protocol` 层: 能在 PC 上原生单测, 板上就少一类要查的错。
 *  真正的 socket / 文件 I/O 在 `svc_http.c`, 本模块一行都不碰。
 *
 * ─────────────────────────────────────────────────────────────────
 *  几个刻意的取舍(面试会被问)
 * ─────────────────────────────────────────────────────────────────
 *  · **只做 HTTP/1.1 的必需子集**: 请求行 + 头 + `Range` + `Content-Length`。
 *    chunked、gzip、keep-alive 流水线、多段 Range(`bytes=0-9,20-29`)**都不做** ——
 *    回放场景用不到, 而每一个都要引入状态机。遇到多段 Range: 当"没有 Range"处理(整段 200),
 *    这是 RFC 7233 §4.1 允许的("MAY ignore the Range header field")。
 *  · **不生成 `Date` 头**: 板子**没有 RTC**(开机时间从 1970 数起),
 *    发一个错的时间比不发更糟。RFC 只说 SHOULD, 不是 MUST。
 *  · **路径严格白名单**: 分段名只允许 `[0-9A-Za-z._-]`,`/` 与 `..` 一律拒绝
 *    —— 这条不是"顺手加的", 是回放服务**最容易出的安全洞**(目录穿越)。
 */
#ifndef __PROTO_HTTP_H__
#define __PROTO_HTTP_H__

#include <stddef.h>
#include <stdint.h>

/** 请求头缓冲上限(超过就当请求非法, 而不是无限收) */
#define PROTO_HTTP_HEAD_MAX 1024

/** 目标(路径)缓冲上限 */
#define PROTO_HTTP_TARGET_MAX 192

/** `Host` 头缓冲上限(生成绝对 URL 要用它: 形如 `192.168.16.88:8080`) */
#define PROTO_HTTP_HOST_MAX 64

/** 分段名字缓冲上限(与 svc_record 的文件名上限一致) */
#define PROTO_HTTP_NAME_MAX 64

/** HTTP 方法(只列我们支持的; 其余归 UNKNOWN) */
typedef enum {
    PROTO_HTTP_GET = 0,     /**< 取列表 / 取文件 */
    PROTO_HTTP_HEAD,        /**< 只要头(播放器常用来探大小) */
    PROTO_HTTP_PUT,         /**< 锁定 / 解锁 */
    PROTO_HTTP_OPTIONS,     /**< 能力探测 */
    PROTO_HTTP_UNKNOWN      /**< 其它 */
} proto_http_method_t;

/** 解析结果(<0 都是"请求不合法") */
#define PROTO_HTTP_EAGAIN    (-1)   /**< 头还没收全 —— **不是错误**, 继续收 */
#define PROTO_HTTP_EBADREQ   (-2)   /**< 请求行/头有语法错 → 应答 400 */
#define PROTO_HTTP_EBADVER   (-3)   /**< 版本不是 HTTP/1.x → 应答 505 */
#define PROTO_HTTP_ETOOLONG  (-4)   /**< 头超过 `PROTO_HTTP_HEAD_MAX` → 应答 431 */

/** 解析出来的请求 */
typedef struct {
    proto_http_method_t method;
    char     method_name[8];                  /**< 原文方法名(UNKNOWN 时也要能回显) */
    char     target[PROTO_HTTP_TARGET_MAX];   /**< 原始请求目标(含可能的查询串) */
    int      ver_major;                       /**< HTTP/1.1 → 1 */
    int      ver_minor;                       /**< HTTP/1.1 → 1 */
    int      has_range;                       /**< 1 = 有可用的单段 Range */
    int      range_is_suffix;                 /**< 1 = `bytes=-N`(最后 N 字节) */
    uint64_t range_first;                     /**< 起始偏移(后缀形式时为 N) */
    uint64_t range_last;                      /**< 结束偏移(含); `bytes=N-` 时为 UINT64_MAX */
    int      has_content_length;              /**< 1 = 有 Content-Length(锁定的 body) */
    uint64_t content_length;
    int      has_host;                        /**< 1 = 有 Host 头 */
    char     host[PROTO_HTTP_HOST_MAX];       /**< `Host` 头原文(用于拼**绝对 URL**) */
} proto_http_request_t;

/** 路径判决(回放服务的三个入口) */
typedef enum {
    PROTO_HTTP_PATH_NONE = -1,   /**< 不认识的路径 → 应答 404 */
    PROTO_HTTP_PATH_LIST = 0,    /**< `/recordings` 或 `/recordings/` */
    PROTO_HTTP_PATH_SEGMENT = 1, /**< `/recordings/<名字>` */
    PROTO_HTTP_PATH_LOCK = 2     /**< `/recordings/<名字>/lock` */
} proto_http_path_kind_t;

/**
 * @brief 解析一个 HTTP 请求(必须已经收全到 `\r\n\r\n`)。
 *
 * @param[in]  buf 收到的字节
 * @param[in]  len 收到的字节数
 * @param[out] out 解析结果
 * @return >0 = **头区**消耗的字节数(即 body 从 `buf + 返回值` 开始);
 *         或者上面的负错误码
 *
 * @note ★ 返回值是**头区长度**, 不是"整段长度" —— 调用方靠它把 body 切出来。
 *       (原来返回整段长度, 于是服务层"明明已经收到了 body"却又去 socket 上等,
 *        结果 500 ms 等不到就连 body 一起报 400。见 B042 的后半段。)
 * @note 头部名**大小写无关**(RFC 7230 §3.2);值两侧空白会被裁掉。
 * @note 未知的头**一律忽略**(HTTP 允许扩展头, 忽略比报错健壮 —— 与 `proto_rtsp` 同一条规矩)。
 * @note 只认 `\r\n` 行尾:HTTP 规范要求 CRLF, 容错 LF 会把"半个包"也当合法。
 */
int proto_http_parse(const char *buf, size_t len, proto_http_request_t *out);

/**
 * @brief 从请求目标里取出**路径**部分(去掉 `?query` 与 `#frag`)。
 *
 * @param[in]  target 请求目标(如 `/recordings?from=1`)
 * @param[out] out    输出缓冲
 * @param[in]  cap    容量
 * @return 写入长度(不含 '\0'); -1 = 放不下
 */
int proto_http_path(const char *target, char *out, size_t cap);

/**
 * @brief 判路径是哪一类, 并取出分段名。
 *
 * @param[in]  path 已去查询串的路径
 * @param[out] name 分段名(仅 `SEGMENT` / `LOCK` 时写入; 可为 NULL)
 * @param[in]  cap  name 的容量
 * @return `proto_http_path_kind_t`
 *
 * @note 名字**不合法**(含 `/`、`..` 等)时返回 `PROTO_HTTP_PATH_NONE` ——
 *       宁可 404, 也不要把 `../../etc/passwd` 送进 `open()`。
 */
proto_http_path_kind_t proto_http_match_path(const char *path, char *name, size_t cap);

/**
 * @brief 分段名是否安全(防目录穿越)。
 *
 * @param[in] name 名字
 * @return 1 = 安全; 0 = 拒绝
 *
 * @note 白名单:非空、长度 < `PROTO_HTTP_NAME_MAX`、只含 `[0-9A-Za-z._-]`、
 *       且**不以 `.` 开头**(否则能命中 `.locked` / `.`/`..`)。
 */
int proto_http_name_ok(const char *name);

/**
 * @brief 状态码 → reason phrase(HTTP/1.1 里那几个固定的)。
 *
 * @param[in] status 状态码
 * @return 常量字符串; 不认识的码返回 "Unknown"
 */
const char *proto_http_reason(int status);

/**
 * @brief 构造响应头(不含 body)。
 *
 * @param[out] out           输出缓冲
 * @param[in]  cap           容量
 * @param[in]  status        状态码(如 200 / 206 / 404 / 416)
 * @param[in]  content_type  如 "video/mp4"; **NULL = 不带**(如 204)
 * @param[in]  content_length body 字节数(HEAD 请求也要按真实长度写)
 * @param[in]  content_range `bytes a-b/total`; **NULL = 不带**
 * @param[in]  keep_alive    1 = 保持连接; 0 = 带 `Connection: close`
 * @param[in]  extra         额外头(整行, 以 CRLF 结尾; 如 `Accept-Ranges: bytes\r\n`);可 NULL
 * @return 写入字节数; -1 = 缓冲不够
 *
 * @note **一定**会写 `Content-Length` —— 这是 RTSP 那条 B017 教训的同一类问题:
 *       响应没有长度, 客户端就只能靠关连接来判断结束, 而且**无法复用连接**。
 */
int proto_http_build_head(char *out, size_t cap, int status,
                          const char *content_type, uint64_t content_length,
                          const char *content_range, int keep_alive,
                          const char *extra);

#endif /* __PROTO_HTTP_H__ */
