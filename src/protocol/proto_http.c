/**
 * @file    proto_http.c
 * @brief   HTTP/1.1 请求解析 + 响应构造实现(见 proto_http.h 的说明)
 *
 * 【模块职责】解析请求行/头、判定回放路径、构造响应头
 * 【依赖方向】只依赖 proto_str 与 libc
 * 【线程模型】纯函数, 无状态
 * 【资源边界】无动态分配; 无静态缓冲; 输出全部走调用方缓冲
 *
 * 结构(每块都短):
 *      ① 小工具: 空白裁剪 / 名字白名单
 *      ② 请求行与头行解析
 *      ③ Range 解析(RFC 7233 §2.1 的三种形式)
 *      ④ 路径判决
 *      ⑤ 响应头构造
 */
#include "proto_http.h"
#include "proto_str.h"

#include <stdio.h>
#include <string.h>

/* 给共用工具起短名字(本文件调用多) */
#define append(out, cap, used, text)    proto_str_append((out), (cap), (used), (text))
#define append_u64(out, cap, used, v)   proto_str_append_u64((out), (cap), (used), (v))

/* ─────────────── ① 小工具 ─────────────── */

/**
 * @brief 裁掉一段文本两侧的空格/制表符(就地改)
 *
 * @param[in] s 以 '\0' 结尾的文本(可写)
 * @return 去掉前导空白后的起点
 */
static char *trim(char *s)
{
    size_t n;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    return s;
}

int proto_http_name_ok(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0' || name[0] == '.') {
        return 0;
    }
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];

        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-') {
            continue;
        }
        return 0;                       /* `/`、`\`、空格… 一律拒绝(防目录穿越) */
    }
    return (i < PROTO_HTTP_NAME_MAX) ? 1 : 0;
}

/**
 * @brief 头区在哪结束(出现空行 `\r\n\r\n`)
 *
 * @param[in] buf 收到的字节
 * @param[in] len 字节数
 * @return 头区结束后的下标(即 **body 从这里开始**); 0 = 还没收全
 *
 * @note 自己扫而不用 `memmem` —— 那是 GNU 扩展, 会让"只要有个 C 编译器就能编"
 *       这件事不再成立(单测用 `-std=c11 -D_DEFAULT_SOURCE`, 拿不到它)。
 * @note ★ 这个下标**必须**用来把头区和 body 切开: 见 `proto_http_parse()` 里 B042 的说明。
 */
static size_t head_end(const char *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 4 <= len; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
            return i + 4;
        }
    }
    return 0;
}

/* ─────────────── ② 请求行 / 头行 ─────────────── */

/**
 * @brief 方法名文本 → 枚举(未知方法也保留原文, 便于回 405 时说明)
 *
 * @param[in]  m   方法名
 * @param[out] out 结果(写 method_name)
 * @return 方法枚举
 */
static proto_http_method_t method_of(const char *m, proto_http_request_t *out)
{
    snprintf(out->method_name, sizeof(out->method_name), "%s", m);
    if (strcmp(m, "GET") == 0) {
        return PROTO_HTTP_GET;
    }
    if (strcmp(m, "HEAD") == 0) {
        return PROTO_HTTP_HEAD;
    }
    if (strcmp(m, "PUT") == 0) {
        return PROTO_HTTP_PUT;
    }
    if (strcmp(m, "OPTIONS") == 0) {
        return PROTO_HTTP_OPTIONS;
    }
    return PROTO_HTTP_UNKNOWN;
}

/**
 * @brief 解析请求行:`METHOD SP target SP HTTP/major.minor`
 *
 * @param[in]  line 请求行(就地切分)
 * @param[out] out  结果
 * @return 0 成功; 负值为错误码
 *
 * @note 版本号**手写解析**而不是 `sscanf("%d.%d")` —— 与本项目"解析外来数据
 *       不依赖 locale"的规矩一致(见 `proto_str.h` 的说明)。
 */
static int parse_request_line(char *line, proto_http_request_t *out)
{
    char       *sp1 = strchr(line, ' ');
    char       *sp2 = (sp1 != NULL) ? strchr(sp1 + 1, ' ') : NULL;
    const char *v;
    uint64_t    major = 0;
    uint64_t    minor = 0;
    size_t      n;

    if (sp1 == NULL || sp2 == NULL) {
        return PROTO_HTTP_EBADREQ;      /* 请求行必须三段 */
    }
    *sp1 = '\0';
    *sp2 = '\0';
    out->method = method_of(line, out);
    if (strlen(sp1 + 1) >= PROTO_HTTP_TARGET_MAX) {
        return PROTO_HTTP_EBADREQ;      /* 目标太长: 明确拒绝, 不静默截断 */
    }
    snprintf(out->target, sizeof(out->target), "%s", sp1 + 1);

    v = sp2 + 1;
    if (!proto_str_eq_ci_n(v, "HTTP/", 5)) {
        return PROTO_HTTP_EBADREQ;
    }
    v += 5;
    n = proto_str_parse_u64(v, &major);
    if (n == 0 || v[n] != '.') {
        return PROTO_HTTP_EBADREQ;
    }
    v += n + 1;
    n = proto_str_parse_u64(v, &minor);
    if (n == 0 || v[n] != '\0') {
        return PROTO_HTTP_EBADREQ;
    }
    if (major != 1) {
        return PROTO_HTTP_EBADVER;      /* 只做 HTTP/1.x */
    }
    out->ver_major = (int)major;
    out->ver_minor = (int)minor;
    return 0;
}

/**
 * @brief 解析 `Range: bytes=...` 的值部分(RFC 7233 §2.1)
 *
 * @param[in]  v   头的值(已裁掉两侧空白), 形如 `bytes=0-499`
 * @param[out] req 写回解析结果
 * @return 1 = 解析成功; 0 = 不是 bytes 单位 / 语法不认识(调用方按"没有 Range"处理)
 *
 * @note 三种形式都支持:`a-b`(闭区间)、`a-`(到结尾)、`-N`(最后 N 字节)。
 * @note **多段 Range**(带逗号)刻意不支持 —— 见头文件说明;这里当"没有 Range"。
 */
static int parse_range(const char *v, proto_http_request_t *req)
{
    const char *p = v;
    uint64_t    a = 0;
    uint64_t    b = 0;
    size_t      n;

    if (p == NULL || !proto_str_eq_ci_n(p, "bytes=", 6)) {
        return 0;
    }
    p += 6;
    if (strchr(p, ',') != NULL) {
        return 0;
    }
    if (*p == '-') {                    /* 后缀形式: -N */
        n = proto_str_parse_u64(p + 1, &b);
        if (n == 0 || p[1 + n] != '\0') {
            return 0;
        }
        req->has_range       = 1;
        req->range_is_suffix = 1;
        req->range_first     = b;       /* 先存 N, 由 svc_http 结合文件大小换算 */
        req->range_last      = UINT64_MAX;
        return 1;
    }
    n = proto_str_parse_u64(p, &a);
    if (n == 0 || p[n] != '-') {
        return 0;
    }
    p += n + 1;
    req->has_range       = 1;
    req->range_is_suffix = 0;
    req->range_first     = a;
    req->range_last      = UINT64_MAX;  /* 默认"到结尾"; 后面有数字再覆盖 */
    if (*p == '\0') {
        return 1;                       /* `a-` */
    }
    n = proto_str_parse_u64(p, &b);
    if (n == 0 || p[n] != '\0') {
        return 0;
    }
    req->range_last = b;
    return 1;
}

/**
 * @brief 处理一个头行(只关心 Range / Content-Length / Host, 其余忽略)
 *
 * @param[in]  line 头行(就地切分)
 * @param[out] req  结果
 * @param[out] bad  置 1 表示语法错(头行没有冒号)
 */
static void handle_header(char *line, proto_http_request_t *req, int *bad)
{
    char *colon = strchr(line, ':');
    char *value;

    if (colon == NULL) {
        *bad = 1;
        return;
    }
    *colon = '\0';
    value = trim(colon + 1);
    if (proto_str_eq_ci(trim(line), "Range")) {
        (void)parse_range(value, req);          /* 不合法就当没有 Range */
        return;
    }
    if (proto_str_eq_ci(trim(line), "Content-Length")) {
        uint64_t v = 0;

        if (proto_str_parse_u64(value, &v) > 0) {
            req->has_content_length = 1;
            req->content_length     = v;
        }
        return;
    }
    if (proto_str_eq_ci(trim(line), "Host")) {
        /* ★ 留着 Host:回放播放列表里的 URL 要**绝对**的(给 VLC 的 m3u 用),
         *   而服务端自己不该猜"板子 IP 是多少"(可能多网卡/NAT)——
         *   客户端发什么 Host, 就用什么。 */
        if (value[0] != '\0') {
            snprintf(req->host, sizeof(req->host), "%s", value);
            req->has_host = 1;
        }
        return;
    }
    /* 其它头( User-Agent / Connection … )一律忽略 */
}

/**
 * @brief 逐行解析整个头区(第一行是请求行)
 *
 * @param[in]  head 头区文本(以 '\0' 结尾, 会被就地切分)
 * @param[out] out  结果
 * @return 0 成功; 负值为错误码
 */
static int parse_head(char *head, proto_http_request_t *out)
{
    char *save = NULL;
    char *line = strtok_r(head, "\r\n", &save);
    int   bad  = 0;
    int   rc;

    if (line == NULL) {
        return PROTO_HTTP_EBADREQ;
    }
    rc = parse_request_line(line, out);
    if (rc != 0) {
        return rc;
    }
    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        handle_header(line, out, &bad);
    }
    return bad ? PROTO_HTTP_EBADREQ : 0;
}

int proto_http_parse(const char *buf, size_t len, proto_http_request_t *out)
{
    char   head[PROTO_HTTP_HEAD_MAX];
    size_t hlen;
    int    rc;

    if (buf == NULL || out == NULL) {
        return PROTO_HTTP_EBADREQ;
    }
    if (len == 0 || len >= PROTO_HTTP_HEAD_MAX) {
        return PROTO_HTTP_ETOOLONG;
    }
    if (memchr(buf, '\0', len) != NULL) {
        return PROTO_HTTP_EBADREQ;
    }
    hlen = head_end(buf, len);
    if (hlen == 0) {
        return PROTO_HTTP_EAGAIN;       /* 还没收全, 不是错误 */
    }
    memset(out, 0, sizeof(*out));
    /*
     * ★ **只把头区喂给解析器**(2026-09-19 修的 B042)。
     *   原来把整段(含 body)都交出去, 于是 body 的第一行会被当成头行:
     *   `PUT /recordings/xxx/lock` + body `1` ⇒ 那一行没有冒号 ⇒ 判"语法错" ⇒ **400**。
     *   实测现象: `curl -X PUT -d 0 .../lock` **解锁不掉**, 而"不带 body 的 PUT"却正常
     *   (因为那一次没有 body 可被误解析)—— 典型的"只在某条路径上错"的 bug。
     */
    memcpy(head, buf, hlen);
    head[hlen] = '\0';
    rc = parse_head(head, out);
    /* 返回**头区长度**:body(若有)从 `buf + hlen` 开始, 由调用方按 Content-Length 读 */
    return (rc != 0) ? rc : (int)hlen;
}

/* ─────────────── ③ 路径判决 ─────────────── */

int proto_http_path(const char *target, char *out, size_t cap)
{
    size_t n = 0;

    if (target == NULL || out == NULL || cap == 0) {
        return -1;
    }
    while (target[n] != '\0' && target[n] != '?' && target[n] != '#') {
        if (n + 1 >= cap) {
            return -1;
        }
        out[n] = target[n];
        n++;
    }
    out[n] = '\0';
    return (int)n;
}

/**
 * @brief 路径是不是以 `prefix` 开头, 并把剩下的一段拷出来
 *
 * @param[in]  path   路径
 * @param[in]  prefix 前缀
 * @param[out] rest   剩下的部分(可能为空串)
 * @param[in]  cap    rest 的容量
 * @return 1 = 匹配; 0 = 不匹配
 */
static int tail_after(const char *path, const char *prefix, char *rest, size_t cap)
{
    size_t n = strlen(prefix);

    if (strncmp(path, prefix, n) != 0) {
        return 0;
    }
    snprintf(rest, cap, "%s", path + n);
    return 1;
}

proto_http_path_kind_t proto_http_match_path(const char *path, char *name, size_t cap)
{
    char rest[PROTO_HTTP_TARGET_MAX];
    char *slash;

    if (path == NULL) {
        return PROTO_HTTP_PATH_NONE;
    }
    if (strcmp(path, "/recordings") == 0 || strcmp(path, "/recordings/") == 0) {
        return PROTO_HTTP_PATH_LIST;
    }
    if (!tail_after(path, "/recordings/", rest, sizeof(rest))) {
        return PROTO_HTTP_PATH_NONE;
    }
    slash = strchr(rest, '/');          /* rest 形如 `<名字>` 或 `<名字>/lock` */
    if (slash != NULL) {
        *slash = '\0';
        if (strcmp(slash + 1, "lock") != 0) {
            return PROTO_HTTP_PATH_NONE;
        }
    }
    if (!proto_http_name_ok(rest)) {
        return PROTO_HTTP_PATH_NONE;    /* 宁可 404, 也不把 `../..` 送进 open() */
    }
    if (name != NULL) {
        snprintf(name, cap, "%s", rest);
    }
    return (slash != NULL) ? PROTO_HTTP_PATH_LOCK : PROTO_HTTP_PATH_SEGMENT;
}

/* ─────────────── ④ 响应构造 ─────────────── */

const char *proto_http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int proto_http_build_head(char *out, size_t cap, int status,
                          const char *content_type, uint64_t content_length,
                          const char *content_range, int keep_alive,
                          const char *extra)
{
    char   code[12];                    /* ⚠️ 至少 11 字节: proto_str_u32 的硬要求 */
    size_t used = 0;

    if (out == NULL || cap == 0 ||
        proto_str_u32((uint32_t)status, code, sizeof(code)) < 0) {
        return -1;
    }
    if (append(out, cap, &used, "HTTP/1.1 ") != 0 ||
        append(out, cap, &used, code) != 0 ||
        append(out, cap, &used, " ") != 0 ||
        append(out, cap, &used, proto_http_reason(status)) != 0 ||
        append(out, cap, &used, "\r\n") != 0) {
        return -1;
    }
    if (content_type != NULL &&
        (append(out, cap, &used, "Content-Type: ") != 0 ||
         append(out, cap, &used, content_type) != 0 ||
         append(out, cap, &used, "\r\n") != 0)) {
        return -1;
    }
    if (content_range != NULL &&
        (append(out, cap, &used, "Content-Range: ") != 0 ||
         append(out, cap, &used, content_range) != 0 ||
         append(out, cap, &used, "\r\n") != 0)) {
        return -1;
    }
    /* ★ Content-Length 一定要有(见头文件说明); 204 也带 0, 语义明确 */
    if (append(out, cap, &used, "Content-Length: ") != 0 ||
        append_u64(out, cap, &used, content_length) != 0 ||
        append(out, cap, &used, "\r\n") != 0) {
        return -1;
    }
    if (extra != NULL && append(out, cap, &used, extra) != 0) {
        return -1;
    }
    if (append(out, cap, &used, keep_alive ? "Connection: keep-alive\r\n"
                                           : "Connection: close\r\n") != 0 ||
        append(out, cap, &used, "\r\n") != 0) {
        return -1;
    }
    return (int)used;
}
