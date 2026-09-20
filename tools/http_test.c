/**
 * @file    http_test.c
 * @brief   proto_http.c 的单元测试 —— **纯逻辑, 不需要 SDK / 板子 / 网络**
 *
 * 【怎么验证】(可证伪)
 *   ① 解析:正常请求、头没收全、语法错、版本不支持、头太长、头名大小写无关
 *   ② Range:三种形式(`a-b` / `a-` / `-N`) + 多段 + 非 bytes 单位 + 垃圾值
 *   ③ 路径:列表 / 单段 / 锁定,以及**目录穿越必须被拒**(`..`、`/`、`.` 开头、`%`)
 *   ④ 响应头:200 / 206 / 416 / 204 的关键字段;**一定带 Content-Length**;
 *      64 位长度(>4 GiB 的边界)不能被截断
 *   ⑤ 共用的 proto_str_u64 / parse_u64 也在这里盯一眼(HTTP 的 Content-Range 靠它)
 *   ⑥ 查询串解析:分页用,值太长必须报错而不是静默截断
 *
 * 编译(主机, 由 Makefile 的 `make test` 驱动):
 *   gcc -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE -Isrc/protocol \
 *       -o http_test tools/http_test.c src/protocol/proto_http.c \
 *       src/protocol/proto_str.c
 */
#include <stdio.h>
#include <string.h>

#include "proto_http.h"
#include "proto_str.h"

static int g_fails;

/**
 * @brief 一条断言
 *
 * @param[in] name 说明
 * @param[in] cond 条件
 * @return 0 = 通过; 1 = 失败
 */
static int check(const char *name, int cond)
{
    printf("   %-52s %s\n", name, cond ? "[通过]" : "[失败] <<<");
    return cond ? 0 : 1;
}

/* ─────────────── ① 解析 ─────────────── */

/**
 * @brief 拿一段原始请求文本跑解析, 返回错误码/消耗字节数
 */
static int parse(const char *text, proto_http_request_t *req)
{
    return proto_http_parse(text, strlen(text), req);
}

/**
 * @brief 解析相关的全部断言
 */
static void test_parse(void)
{
    proto_http_request_t r;
    int                  n;
    char                 big[PROTO_HTTP_HEAD_MAX + 16];

    printf("\n[1] 解析请求\n");

    n = parse("GET /recordings HTTP/1.1\r\nHost: 192.168.16.88\r\n\r\n", &r);
    g_fails += check("正常 GET: 消耗字节数 = 整段长度",
                     n == (int)strlen("GET /recordings HTTP/1.1\r\n"
                                      "Host: 192.168.16.88\r\n\r\n"));
    g_fails += check("方法 = GET", r.method == PROTO_HTTP_GET);
    g_fails += check("目标 = /recordings", strcmp(r.target, "/recordings") == 0);
    g_fails += check("版本 = HTTP/1.1", r.ver_major == 1 && r.ver_minor == 1);
    g_fails += check("没有 Range", r.has_range == 0);

    n = parse("GET /recordings HTTP/1.1\r\nHost: x\r\n", &r);
    g_fails += check("头没收全 → EAGAIN(不是错误)", n == PROTO_HTTP_EAGAIN);

    n = parse("GET\r\n\r\n", &r);
    g_fails += check("请求行缺段 → EBADREQ", n == PROTO_HTTP_EBADREQ);

    n = parse("GET /x HTTP/2.0\r\n\r\n", &r);
    g_fails += check("HTTP/2.0 → EBADVER(我们只做 1.x)", n == PROTO_HTTP_EBADVER);

    n = parse("GET /x HTTP/1.1\r\nThisHeaderHasNoColon\r\n\r\n", &r);
    g_fails += check("头行没有冒号 → EBADREQ", n == PROTO_HTTP_EBADREQ);

    n = parse("HEAD /recordings/2026-09-19-21-42-43.mp4 HTTP/1.1\r\n\r\n", &r);
    g_fails += check("HEAD 方法认得", r.method == PROTO_HTTP_HEAD && n > 0);
    n = parse("PUT /recordings/a.mp4/lock HTTP/1.1\r\nContent-Length: 1\r\n\r\n",
              &r);
    g_fails += check("PUT + Content-Length 都解析到",
                     r.method == PROTO_HTTP_PUT && r.has_content_length == 1 &&
                     r.content_length == 1);

    n = parse("DELETE /x HTTP/1.1\r\n\r\n", &r);
    g_fails += check("不认识的方法 → UNKNOWN 且保留原文(便于回 405)",
                     r.method == PROTO_HTTP_UNKNOWN &&
                     strcmp(r.method_name, "DELETE") == 0);

    /*
     * ★ B042 的回归防线:**带 body 的请求不能被当成语法错**。
     *   原来的实现把整段(含 body)都喂给逐行解析, 于是 body 那一行成了"没有冒号的头行"
     *   ⇒ 400。现象是"`curl -X PUT -d 0 .../lock` 解锁不掉", 而**不带 body** 的 PUT 正常。
     */
    n = parse("PUT /recordings/a.mp4/lock HTTP/1.1\r\nHost: x\r\n"
              "Content-Length: 1\r\n\r\n0", &r);
    g_fails += check("★ 带 body 的 PUT 不能被判成语法错(B042)",
                     n > 0 && r.method == PROTO_HTTP_PUT &&
                     r.has_content_length == 1 && r.content_length == 1);
    /* ★ 返回值必须是**头区长度**:调用方靠它把 body 切出来(B042 后半段) */
    {
        const char *req = "PUT /x HTTP/1.1\r\nContent-Length: 1\r\n\r\n0";
        int head_len = (int)(strstr(req, "\r\n\r\n") - req) + 4;

        n = parse(req, &r);
        g_fails += check("★ 返回值 = 头区长度(body 从 buf+n 开始), 不是整段",
                         n == head_len && (int)strlen(req) == head_len + 1);
    }
    n = parse("PUT /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello", &r);
    g_fails += check("★ body 里的 ':' 也不能被当成头行",
                     n > 0 && r.content_length == 5);
    n = parse("GET /x HTTP/1.1\r\nContent-Length: 3\r\n\r\n:bad", &r);
    g_fails += check("★ body 以冒号开头也不影响(B042 同类)",
                     n > 0 && r.method == PROTO_HTTP_GET);

    /* Host:回放列表要拼**绝对 URL**, 而服务端不该猜自己 IP —— 用客户端给的 Host */
    n = parse("GET / HTTP/1.1\r\nHost: 192.168.16.88:8080\r\n\r\n", &r);
    g_fails += check("Host 头被解析(含端口)",
                     n > 0 && r.has_host == 1 &&
                     strcmp(r.host, "192.168.16.88:8080") == 0);
    n = parse("GET / HTTP/1.1\r\nhOsT:   board.local  \r\n\r\n", &r);
    g_fails += check("Host 头名大小写无关且裁掉空白",
                     n > 0 && r.has_host == 1 &&
                     strcmp(r.host, "board.local") == 0);
    n = parse("GET / HTTP/1.1\r\n\r\n", &r);
    g_fails += check("没有 Host 也不报错(回退相对 URL)", n > 0 && r.has_host == 0);

    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    memcpy(big, "GET /x HTTP/1.1\r\n", 17);
    n = proto_http_parse(big, sizeof(big) - 1, &r);
    g_fails += check("头超过上限 → ETOOLONG(而不是无限收)",
                     n == PROTO_HTTP_ETOOLONG);
}

/* ─────────────── ② Range ─────────────── */

/**
 * @brief Range 解析的全部断言(RFC 7233 §2.1 的三种形式 + 各种垃圾)
 */
static void test_range(void)
{
    proto_http_request_t r;

    printf("\n[2] Range 解析\n");

    (void)parse("GET /recordings/a.mp4 HTTP/1.1\r\nRange: bytes=0-499\r\n\r\n", &r);
    g_fails += check("bytes=0-499 → first=0 last=499",
                     r.has_range == 1 && r.range_is_suffix == 0 &&
                     r.range_first == 0 && r.range_last == 499);

    (void)parse("GET /a.mp4 HTTP/1.1\r\nRange: bytes=100-\r\n\r\n", &r);
    g_fails += check("bytes=100- → first=100 last=UINT64_MAX(到结尾)",
                     r.has_range == 1 && r.range_first == 100 &&
                     r.range_last == UINT64_MAX);

    (void)parse("GET /a.mp4 HTTP/1.1\r\nRange: bytes=-100\r\n\r\n", &r);
    g_fails += check("bytes=-100 → 后缀形式(N=100)",
                     r.has_range == 1 && r.range_is_suffix == 1 &&
                     r.range_first == 100);

    (void)parse("GET /a.mp4 HTTP/1.1\r\nrAnGe: ByTeS=5-6\r\n\r\n", &r);
    g_fails += check("头名/单位大小写无关", r.has_range == 1 && r.range_first == 5);

    (void)parse("GET /a.mp4 HTTP/1.1\r\nRange: bytes=0-9,20-29\r\n\r\n", &r);
    g_fails += check("多段 Range → 当没有 Range(回整段 200, RFC 允许)",
                     r.has_range == 0);

    (void)parse("GET /a.mp4 HTTP/1.1\r\nRange: items=0-9\r\n\r\n", &r);
    g_fails += check("非 bytes 单位 → 当没有 Range", r.has_range == 0);

    (void)parse("GET /a.mp4 HTTP/1.1\r\nRange: bytes=abc\r\n\r\n", &r);
    g_fails += check("垃圾值 → 当没有 Range", r.has_range == 0);

    (void)parse("GET /a.mp4 HTTP/1.1\r\nRange: bytes=99999999999999999999-\r\n\r\n",
                &r);
    g_fails += check("超长数字(溢出)→ 当没有 Range", r.has_range == 0);
}

/* ─────────────── ③ 路径与名字 ─────────────── */

/**
 * @brief 路径判决 + 目录穿越防护
 */
static void test_path(void)
{
    char path[PROTO_HTTP_TARGET_MAX];
    char name[PROTO_HTTP_NAME_MAX];

    printf("\n[3] 路径判决与目录穿越防护\n");

    g_fails += check("/recordings → 列表",
                     proto_http_match_path("/recordings", name, sizeof(name)) ==
                     PROTO_HTTP_PATH_LIST);
    g_fails += check("/recordings/ → 列表",
                     proto_http_match_path("/recordings/", name, sizeof(name)) ==
                     PROTO_HTTP_PATH_LIST);

    memset(name, 0, sizeof(name));
    g_fails += check("/recordings/<名> → 单段且取出名字",
                     proto_http_match_path("/recordings/2026-09-19-21-42-43.mp4",
                                           name, sizeof(name)) ==
                     PROTO_HTTP_PATH_SEGMENT &&
                     strcmp(name, "2026-09-19-21-42-43.mp4") == 0);

    memset(name, 0, sizeof(name));
    g_fails += check("/recordings/<名>/lock → 锁定且取出名字",
                     proto_http_match_path("/recordings/a.mp4/lock",
                                           name, sizeof(name)) ==
                     PROTO_HTTP_PATH_LOCK && strcmp(name, "a.mp4") == 0);

    g_fails += check("★ ../etc/passwd → 拒(目录穿越)",
                     proto_http_match_path("/recordings/../etc/passwd",
                                           name, sizeof(name)) ==
                     PROTO_HTTP_PATH_NONE);
    g_fails += check("★ a.mp4/../../x → 拒",
                     proto_http_match_path("/recordings/a.mp4/../../x",
                                           name, sizeof(name)) ==
                     PROTO_HTTP_PATH_NONE);
    g_fails += check("★ .locked(点开头)→ 拒",
                     proto_http_match_path("/recordings/.locked",
                                           name, sizeof(name)) ==
                     PROTO_HTTP_PATH_NONE);
    g_fails += check("★ 百分号编码(%2e%2e)→ 拒",
                     proto_http_match_path("/recordings/%2e%2e",
                                           name, sizeof(name)) ==
                     PROTO_HTTP_PATH_NONE);
    g_fails += check("别的路径 → NONE",
                     proto_http_match_path("/index.html", name, sizeof(name)) ==
                     PROTO_HTTP_PATH_NONE);
    g_fails += check("名字后面的未知子路径 → NONE",
                     proto_http_match_path("/recordings/a.mp4/delete",
                                           name, sizeof(name)) ==
                     PROTO_HTTP_PATH_NONE);

    g_fails += check("查询串被裁掉: /recordings?from=1 → 列表",
                     proto_http_path("/recordings?from=1", path, sizeof(path)) > 0 &&
                     proto_http_match_path(path, name, sizeof(name)) ==
                     PROTO_HTTP_PATH_LIST);

    g_fails += check("名字白名单: 正常时间戳名 → 通过",
                     proto_http_name_ok("2026-09-19-20-59-55.mp4") == 1);
    g_fails += check("名字白名单: 空 → 拒", proto_http_name_ok("") == 0);
    g_fails += check("名字白名单: .. → 拒", proto_http_name_ok("..") == 0);
    g_fails += check("名字白名单: a/b → 拒", proto_http_name_ok("a/b") == 0);
    g_fails += check("名字白名单: 带空格 → 拒", proto_http_name_ok("a b") == 0);
    g_fails += check("名字白名单: 超长 → 拒",
                     proto_http_name_ok("0123456789012345678901234567890123456789"
                                        "012345678901234567890123456789") == 0);
}

/* ─────────────── ④ 响应头 ─────────────── */

/**
 * @brief 响应头构造的全部断言
 */
static void test_build(void)
{
    char buf[512];
    int  n;

    printf("\n[4] 响应头构造\n");

    n = proto_http_build_head(buf, sizeof(buf), 200, "video/mp4", 1234, NULL, 0,
                              "Accept-Ranges: bytes\r\n");
    g_fails += check("200 开头是 `HTTP/1.1 200 OK`",
                     n > 0 && strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0);
    g_fails += check("200 带 Content-Type", strstr(buf, "Content-Type: video/mp4") != NULL);
    g_fails += check("200 带 Content-Length: 1234",
                     strstr(buf, "Content-Length: 1234\r\n") != NULL);
    g_fails += check("200 带 Accept-Ranges(客户端据此知道能拖进度条)",
                     strstr(buf, "Accept-Ranges: bytes") != NULL);
    g_fails += check("200 用 Connection: close", strstr(buf, "Connection: close") != NULL);
    g_fails += check("头区以空行结束", n > 4 && strcmp(buf + n - 4, "\r\n\r\n") == 0);

    n = proto_http_build_head(buf, sizeof(buf), 206, "video/mp4", 100,
                              "bytes 0-99/1000", 0, "Accept-Ranges: bytes\r\n");
    g_fails += check("206 状态行正确",
                     strncmp(buf, "HTTP/1.1 206 Partial Content\r\n", 30) == 0);
    g_fails += check("206 带 Content-Range: bytes 0-99/1000",
                     strstr(buf, "Content-Range: bytes 0-99/1000\r\n") != NULL);
    g_fails += check("206 的 Content-Length 是**这一段**的长度(100)",
                     strstr(buf, "Content-Length: 100\r\n") != NULL);

    n = proto_http_build_head(buf, sizeof(buf), 416, NULL, 0, "bytes */1000", 0, NULL);
    g_fails += check("416 状态行 + Content-Range: bytes */1000",
                     strncmp(buf, "HTTP/1.1 416 Range Not Satisfiable\r\n", 36) == 0 &&
                     strstr(buf, "Content-Range: bytes */1000\r\n") != NULL);
    g_fails += check("416 不带 Content-Type(NULL 就是不写)",
                     strstr(buf, "Content-Type") == NULL);
    g_fails += check("416 也带 Content-Length: 0",
                     strstr(buf, "Content-Length: 0\r\n") != NULL);

    n = proto_http_build_head(buf, sizeof(buf), 204, NULL, 0, NULL, 0, NULL);
    g_fails += check("204 No Content", n > 0 && strstr(buf, "204 No Content") != NULL);

    /* ★ 64 位长度:4 GiB 正好越过 uint32, 用 32 位版本会静默截断 */
    n = proto_http_build_head(buf, sizeof(buf), 200, "video/mp4", 4294967296ULL,
                              NULL, 0, NULL);
    g_fails += check("★ Content-Length 支持 >4 GiB(4294967296, 不截断)",
                     strstr(buf, "Content-Length: 4294967296\r\n") != NULL);

    n = proto_http_build_head(buf, 32, 200, "video/mp4", 1, NULL, 0, NULL);
    g_fails += check("缓冲太小 → -1(而不是写出半个头)", n == -1);

    g_fails += check("reason phrase: 404 / 405 / 500",
                     strcmp(proto_http_reason(404), "Not Found") == 0 &&
                     strcmp(proto_http_reason(405), "Method Not Allowed") == 0 &&
                     strcmp(proto_http_reason(500), "Internal Server Error") == 0);
}

/* ─────────────── ⑤ 共用的 64 位数字工具 ─────────────── */

/**
 * @brief proto_str_u64 / parse_u64 的边界(HTTP 的 Content-Range 全靠它)
 */
static void test_u64(void)
{
    char     b[24];
    uint64_t v = 0;

    printf("\n[5] 64 位数字工具(共用)\n");

    g_fails += check("u64(0) = \"0\"", proto_str_u64(0, b, sizeof(b)) == 1 &&
                     strcmp(b, "0") == 0);
    g_fails += check("u64(4294967296) 正确",
                     proto_str_u64(4294967296ULL, b, sizeof(b)) > 0 &&
                     strcmp(b, "4294967296") == 0);
    g_fails += check("u64(UINT64_MAX) 正确(20 位)",
                     proto_str_u64(UINT64_MAX, b, sizeof(b)) == 20 &&
                     strcmp(b, "18446744073709551615") == 0);
    g_fails += check("u64 缓冲不够 → -1", proto_str_u64(1, b, 8) == -1);

    g_fails += check("parse_u64(\"18446744073709551615\") 正确",
                     proto_str_parse_u64("18446744073709551615", &v) == 20 &&
                     v == UINT64_MAX);
    g_fails += check("parse_u64 遇非数字即停",
                     proto_str_parse_u64("12abc", &v) == 2 && v == 12);
    g_fails += check("parse_u64(\"abc\") = 0(没解析到)", 
                     proto_str_parse_u64("abc", &v) == 0);

    {
        uint32_t w = 0;

        /* ★ 32 位那条也一样: 溢出必须停住, 否则 4294967296 会绕成 0 */
        g_fails += check("★ parse_u32 溢出前停住(9999999999 → 999999999/9 位)",
                         proto_str_parse_u32("9999999999", &w) == 9 &&
                         w == 999999999u);
    }
}

/* ─────────────── ⑥ 查询串(?limit=&before=) ─────────────── */

/**
 * @brief 查询串解析 —— B044 的分页(`/recordings?limit=20&before=xxx.mp4`)靠它
 */
static void test_query(void)
{
    char v[64];

    printf("\n[6] 查询串解析\n");

    g_fails += check("?limit=20 → 取到 \"20\"",
                     proto_http_query("/recordings?limit=20", "limit", v,
                                      sizeof(v)) == 1 && strcmp(v, "20") == 0);
    g_fails += check("多参数:第二个键也取得到(值里有 '.')",
                     proto_http_query("/r?limit=20&before=2026-09-20-14-45-35.mp4",
                                      "before", v, sizeof(v)) == 1 &&
                     strcmp(v, "2026-09-20-14-45-35.mp4") == 0);
    g_fails += check("没有查询串 → 0",
                     proto_http_query("/recordings", "limit", v, sizeof(v)) == 0);
    g_fails += check("有查询串但没这个键 → 0",
                     proto_http_query("/recordings?x=1", "limit", v,
                                      sizeof(v)) == 0);
    g_fails += check("键名只是前缀不算命中(lim ≠ limit)",
                     proto_http_query("/recordings?lim=9", "limit", v,
                                      sizeof(v)) == 0);
    g_fails += check("键名后面不是 '=' 也不算(limitx=9)",
                     proto_http_query("/recordings?limitx=9", "limit", v,
                                      sizeof(v)) == 0);
    g_fails += check("空值也命中(长度为 0, 不当成没这个键)",
                     proto_http_query("/recordings?before=", "before", v,
                                      sizeof(v)) == 1 && v[0] == '\0');
    g_fails += check("★ 值放不下 → -1(明确失败, 不静默截断)",
                     proto_http_query("/recordings?limit=123456789", "limit", v, 4)
                     == -1);
    g_fails += check("路径不被当成查询串(? 之前的部分不参与)",
                     proto_http_query("/recordings?limit=20", "recordings", v,
                                      sizeof(v)) == 0);
}

int main(void)
{
    printf("===== proto_http 单元测试(PC 原生, 不需要 SDK/板子)=====\n");

    test_parse();
    test_range();
    test_path();
    test_build();
    test_u64();
    test_query();

    printf("\n===== 结果: %s(%d 项失败)=====\n",
           g_fails == 0 ? "全部通过" : "有失败", g_fails);
    return g_fails == 0 ? 0 : 1;
}
