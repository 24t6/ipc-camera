/**
 * @file    svc_http.c
 * @brief   回放服务实现(列分段 / 带 Range 取流 / 锁定解锁)—— 见 svc_http.h
 *
 * 【模块职责】HTTP 服务线程:收一个请求 → 解析(proto_http)→ 取数据 → 应答
 * 【依赖方向】infra_netio / infra_log / proto_http / proto_str / svc_record
 * 【线程模型】自己 1 个线程(线程名 `ipc_http`);**串行**处理客户端(见 svc_http.h 的天花板说明)
 * 【资源边界】无动态分配;静态缓冲 ~55KB(见文件末尾的缓冲说明)
 *
 * 结构:
 *      ① 小工具(应答头 / 文本应答 / 错误应答)
 *      ② 三个业务处理:列表 / 取文件(含 Range) / 锁定
 *      ③ 请求读取与分派
 *      ④ 线程主循环 + 对外接口
 */
#include "svc_http.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>       /* nanosleep(与 svc_media/svc_osd 同一种睡法) */
#include <unistd.h>

#include "infra_log.h"
#include "infra_netio.h"
#include "proto_http.h"
#include "proto_str.h"
#include "svc_http_page.h"
#include "svc_record.h"

/** 请求缓冲(头 + 小的 body;`proto_http` 的头区上限是 1024) */
#define HTTP_REQ_BUF   2048

/** 应答头缓冲(JSON 列表的 Content-Length 可能 5 位以上, 留足) */
#define HTTP_HEAD_BUF  1024

/** 文本 body 缓冲(JSON 列表 / m3u 播放列表): 64 条上限, JSON 约 12KB、m3u 约 6KB */
#define HTTP_TEXT_BUF  16384

/** 取文件时每次读多少字节写出去(32KB:卡上顺序读 + 一次 send 都不吃亏) */
#define HTTP_CHUNK     32768

/** accept 前等可读的毫秒数(为了能及时看到"该停了") */
#define HTTP_ACCEPT_POLL_MS 200

/** 一个客户端最长等多久把请求头收全(毫秒) */
#define HTTP_READ_TIMEOUT_MS 3000

/** 声明了 Content-Length 之后, 最多再等多久那个 body(毫秒) */
#define HTTP_BODY_WAIT_MS 500

/**
 * `GET /recent.mp4` 最多等录制线程多久把当前段收尾(毫秒)。
 *
 * @note 5 秒的来历:请求发出后录制线程会在**下一个 IDR** 处切(30fps/GOP=30 ⇒ ≤1 秒),
 *       再加上 `MP4Close` 写 moov 的时间(实测百毫秒级)。5 秒是"正常情况 5 倍余量、
 *       异常情况也不会把服务线程占太久"的折中。
 */
#define HTTP_ROTATE_WAIT_MS 5000

/** "看最新"轮询等待时每次睡多久(毫秒) */
#define HTTP_ROTATE_POLL_MS 50

/**
 * `/recent.mp4` 那条应答头缓冲的字节数。
 *
 * @note 容量依据(按**最坏情况**算, 数字是可复现的): `HTTP_HDR_FILE` 47 字节
 *       + `"X-Recent-Clip: "` 15 字节 + **分段名最长 63 字节**(`PROTO_HTTP_NAME_MAX - 1`)
 *       + 结尾 `\r\n` 2 字节 = **127 字节**, 加 NUL 是 128 ⇒ 取 **160** 还有 32 字节余量,
 *       且远低于 §6.3 的 4KB 线。
 * @note 为什么不直接写 `char hdrs[160]`: §2.3 要求"避免魔法数字"。
 */
#define HTTP_RECENT_HDR_MAX 160

/*
 * 两个应答头常量(2026-09-20 补):
 *   · 页面/列表/状态是**动态内容** ⇒ `no-store`, 每次都要新鲜的。
 *     ⚠️ 少了它会出事:浏览器的**启发式缓存**可能让"刷新"仍然看到旧列表 ——
 *     用户报的"刷新了也不行"里就有这一层(真正的 bug 是 B044, 但缓存让现象更迷惑)。
 *   · 分段文件用 `no-cache`(可缓存但每次校验):拖动进度条会反复请求同一段的不同
 *     Range, 全禁掉会让回退重下。
 */
#define HTTP_HDR_NO_STORE "Cache-Control: no-store\r\n"
#define HTTP_HDR_FILE     "Accept-Ranges: bytes\r\nCache-Control: no-cache\r\n"

/* ─────────── 模块状态 ─────────── */

/** 一次最多回放多少个分段(与策略层的上限一致) */
#define HTTP_SEG_MAX SVC_RECORD_POLICY_MAX_FILES

static struct {
    int              running;
    int              stop_requested;
    pthread_t        thread;
    int              thread_valid;
    int              listen_fd;
    uint16_t         port;                  /* 实际端口 */
    char             bind_ip[32];
    volatile int     cur_fd;                /* 正在服务的客户端(-1 = 没有);
                                             * 停服务时 `shutdown()` 它, 打断卡住的 send */
    svc_http_stats_t stats;

    /* 下面的缓冲只归本线程用(串行处理 ⇒ 不需要加锁) */
    char             req[HTTP_REQ_BUF];
    char             head[HTTP_HEAD_BUF];
    char             text[HTTP_TEXT_BUF];   /* 文本类 body 的拼装缓冲(JSON / m3u) */
    uint8_t          chunk[HTTP_CHUNK];
    svc_record_policy_file_t segs[HTTP_SEG_MAX];
} g;

/* ─────────── ① 小工具 ─────────── */

/**
 * @brief 发一个"只有头"的应答
 *
 * @param[in] cfd           连接
 * @param[in] status        状态码
 * @param[in] content_type  可为 NULL
 * @param[in] content_length 声明的 body 长度
 * @param[in] content_range 可为 NULL
 * @return 0 成功; -1 发送失败
 *
 * @note 【简化上限】一律 `Connection: close` —— 每个请求一条连接。
 *       复用连接要引入"读下一条请求"的状态机, 回放场景不值当(见 svc_http.h)。
 */
static int send_head_only(int cfd, int status, const char *content_type,
                          uint64_t content_length, const char *content_range,
                          const char *extra)
{
    int n = proto_http_build_head(g.head, sizeof(g.head), status, content_type,
                                  content_length, content_range, 0, extra);

    if (n <= 0) {
        return -1;
    }
    return infra_tcp_write_all(cfd, g.head, (size_t)n);
}

/**
 * @brief 发一个小文本应答(body 是纯文本)
 *
 * @param[in] cfd  连接
 * @param[in] text 文本(以 '\0' 结尾)
 * @return 0 成功; -1 失败
 */
static int send_text(int cfd, const char *text)
{
    size_t len = strlen(text);

    if (send_head_only(cfd, 200, "text/plain; charset=utf-8", len, NULL,
                       HTTP_HDR_NO_STORE) != 0) {
        return -1;
    }
    if (len > 0 && infra_tcp_write_all(cfd, text, len) != 0) {
        return -1;
    }
    g.stats.bytes_sent += len;
    return 0;
}

/**
 * @brief 发一个错误应答(带一小段说明文本)
 *
 * @param[in] cfd    连接
 * @param[in] status 状态码
 * @param[in] extra  `Content-Range`(416 时用);可 NULL
 * @return 0 成功; -1 失败
 */
static int send_error(int cfd, int status, const char *extra)
{
    char body[96];
    int  n = snprintf(body, sizeof(body), "%d %s\n", status,
                      proto_http_reason(status));

    if (n < 0 || n >= (int)sizeof(body)) {
        n = 0;
        body[0] = '\0';
    }
    if (send_head_only(cfd, status, "text/plain; charset=utf-8", (uint64_t)n,
                       extra, HTTP_HDR_NO_STORE) != 0) {
        return -1;
    }
    if (n > 0 && infra_tcp_write_all(cfd, body, (size_t)n) != 0) {
        return -1;
    }
    if (status == 404) {
        g.stats.not_found++;
    } else {
        g.stats.bad_requests++;
    }
    return 0;
}

/* ─────────── ② 业务处理 ─────────── */

/**
 * @brief 把一条分段写成 JSON 对象(单个 `{...}`)
 *
 * @param[in,out] buf  输出缓冲
 * @param[in]     cap  容量
 * @param[in,out] used 已用长度
 * @param[in]     s    分段
 * @return 0 成功; -1 放不下
 *
 * @note 抽出来是为了让 `handle_list()` 不超"代码行 ≤ 50"(12 次 append 摊在里面就超了)。
 * @note ★ `uri` / `lock_uri` 是**设备给的完整路径**, 客户端原样回填即可 ——
 *       这就是"设备给 URI、客户端不拼 URL"的形状(海康 ISAPI 的列表接口同一个思路)。
 */
static int append_entry(char *buf, size_t cap, size_t *used,
                        const svc_record_policy_file_t *s)
{
    if (proto_str_append(buf, cap, used, "{\"name\":\"") != 0 ||
        proto_str_append(buf, cap, used, s->name) != 0 ||
        proto_str_append(buf, cap, used, "\",\"size\":") != 0 ||
        proto_str_append_u64(buf, cap, used, s->size) != 0 ||
        proto_str_append(buf, cap, used, ",\"locked\":") != 0 ||
        proto_str_append(buf, cap, used, s->locked ? "1" : "0") != 0 ||
        proto_str_append(buf, cap, used, ",\"uri\":\"/recordings/") != 0 ||
        proto_str_append(buf, cap, used, s->name) != 0 ||
        proto_str_append(buf, cap, used, "\",\"lock_uri\":\"/recordings/") != 0 ||
        proto_str_append(buf, cap, used, s->name) != 0 ||
        proto_str_append(buf, cap, used, "/lock\"}") != 0) {
        return -1;
    }
    return 0;
}

/**
 * @brief 解析列表请求的分页参数(`?limit=N&before=<名字>`)
 *
 * @param[in]  req   请求
 * @param[out] limit 输出:这一页最多几条(1 ~ `HTTP_SEG_MAX`)
 * @param[out] before 输出缓冲:游标名字(没给时为空串)
 * @param[in]  cap   `before` 的容量
 * @return 游标指针(**没给游标时返回 NULL**)
 *
 * @note 抽出来是为了让 `handle_list()` 不超"代码行 ≤ 50"。
 *       `before` 必须过名字白名单 —— 它会被拼进比较, 不能让调用方塞进奇怪的东西。
 */
static const char *list_page_args(const proto_http_request_t *req, uint32_t *limit,
                                  char *before, size_t cap)
{
    char v[16];

    *limit = (uint32_t)HTTP_SEG_MAX;
    before[0] = '\0';
    if (proto_http_query(req->target, "limit", v, sizeof(v)) == 1) {
        uint32_t n = 0;

        if (proto_str_parse_u32(v, &n) > 0 && n > 0) {
            *limit = (n > (uint32_t)HTTP_SEG_MAX) ? (uint32_t)HTTP_SEG_MAX : n;
        }
    }
    if (proto_http_query(req->target, "before", before, cap) == 1 &&
        proto_http_name_ok(before)) {
        return before;
    }
    return NULL;
}

/**
 * @brief `GET /recordings` —— 列出**最新的一页**分段(JSON)
 *
 * @param[in] cfd 连接
 * @param[in] req 请求(可带 `?limit=N&before=<名字>` 分页)
 * @return 0 成功; -1 失败
 *
 * @note 顺序是**新 → 旧**(名字是零填充时间戳 ⇒ 字典序倒序就是时间倒序)。
 * @note ★ **分页**(B044):目录里可能上千条(一天 1 分钟一段 = 1440 条), 一次全给既
 *       塞不进缓冲、客户端也画不动。所以:
 *       · `limit`  = 这一页最多几条(默认 `HTTP_SEG_MAX`, 上限也是它);
 *       · `before` = **游标**, 只要比这个名字更早的 —— 客户端拿上一页最后一条的名字来翻页;
 *       · 响应里给 `count`(目录总数)与 `returned`(本页条数), 客户端据此知道还有没有更早的。
 *       ⚠️ 关键:**永远给最新的一页**(不是随机一批) —— 这条曾经过错(B044)。
 */
static int handle_list(int cfd, const proto_http_request_t *req)
{
    char        before[PROTO_HTTP_NAME_MAX];
    const char *before_p;
    size_t      used = 0;
    uint32_t    limit = 0;
    int         total = 0;
    int         n;
    int         i;

    before_p = list_page_args(req, &limit, before, sizeof(before));
    n = svc_record_list(g.segs, (int)limit, before_p, &total);
    if (n < 0) {
        return send_error(cfd, 500, NULL);
    }
    if (proto_str_append(g.text, sizeof(g.text), &used, "{\"count\":") != 0 ||
        proto_str_append_u32(g.text, sizeof(g.text), &used, (uint32_t)total) != 0 ||
        proto_str_append(g.text, sizeof(g.text), &used, ",\"returned\":") != 0 ||
        proto_str_append_u32(g.text, sizeof(g.text), &used, (uint32_t)n) != 0 ||
        proto_str_append(g.text, sizeof(g.text), &used, ",\"segments\":[") != 0) {
        return send_error(cfd, 500, NULL);
    }
    for (i = 0; i < n; i++) {
        if (i > 0) {
            if (proto_str_append(g.text, sizeof(g.text), &used, ",") != 0) {
                return send_error(cfd, 500, NULL);
            }
        }
        if (append_entry(g.text, sizeof(g.text), &used, &g.segs[i]) != 0) {
            return send_error(cfd, 500, NULL);      /* 列表太长放不下 */
        }
    }
    if (proto_str_append(g.text, sizeof(g.text), &used, "]}\n") != 0) {
        return send_error(cfd, 500, NULL);
    }
    if (send_head_only(cfd, 200, "application/json; charset=utf-8", used,
                       NULL, HTTP_HDR_NO_STORE) != 0) {
        return -1;
    }
    if (infra_tcp_write_all(cfd, g.text, used) != 0) {
        return -1;
    }
    g.stats.bytes_sent += used;
    g.stats.lists++;
    return 0;
}

/**
 * @brief 往 JSON 里追加直播地址(`rtsp://<Host 的主机名>:8554/live`)
 *
 * @param[in]     host 客户端发来的 `Host`(形如 `192.168.16.88:8080` 或 `board.local`)
 * @param[in,out] used 已用长度
 * @return 0 成功; -1 放不下
 *
 * @note 与播放列表同一条理由:服务端**不该猜自己 IP**, 用客户端给的 `Host`;
 *       端口要按冒号切掉 —— 客户端访问的是 HTTP 端口(比如 8080), 直播在 8554,
 *       两者本来就是两回事。
 */
static int append_live_uri(const char *host, size_t *used)
{
    char   host_only[PROTO_HTTP_HOST_MAX];
    size_t hn = 0;

    while (host[hn] != '\0' && host[hn] != ':' && hn + 1 < sizeof(host_only)) {
        host_only[hn] = host[hn];
        hn++;
    }
    host_only[hn] = '\0';
    if (proto_str_append(g.text, sizeof(g.text), used, ",\"live_rtsp\":\"rtsp://") != 0 ||
        proto_str_append(g.text, sizeof(g.text), used, host_only) != 0 ||
        proto_str_append(g.text, sizeof(g.text), used, ":8554/live\"") != 0) {
        return -1;
    }
    return 0;
}

/**
 * @brief 把"正在录"的那几个字段追加进 JSON
 *
 * @param[in,out] used 已用长度
 * @param[in]     st   状态
 * @return 0 成功; -1 放不下
 *
 * @note 抽出来是为了让 `handle_status()` 不超"缩进 ≤ 5 层"(那条长 `&&` 链会续行到 6 层)。
 */
static int append_status_fields(size_t *used, const svc_record_status_t *st)
{
    if (proto_str_append(g.text, sizeof(g.text), used, ",\"name\":\"") != 0 ||
        proto_str_append(g.text, sizeof(g.text), used, st->name) != 0 ||
        proto_str_append(g.text, sizeof(g.text), used, "\",\"bytes\":") != 0 ||
        proto_str_append_u64(g.text, sizeof(g.text), used, st->bytes) != 0 ||
        proto_str_append(g.text, sizeof(g.text), used, ",\"frames\":") != 0 ||
        proto_str_append_u64(g.text, sizeof(g.text), used, st->frames) != 0 ||
        proto_str_append(g.text, sizeof(g.text), used, ",\"raw_bytes\":") != 0 ||
        proto_str_append_u64(g.text, sizeof(g.text), used, st->raw_bytes) != 0 ||
        proto_str_append(g.text, sizeof(g.text), used, ",\"started_at\":") != 0 ||
        proto_str_append_u64(g.text, sizeof(g.text), used, st->started_at) != 0) {
        return -1;
    }
    return 0;
}

/**
 * @brief `GET /status` —— "现在正在录的那一段"(页面用它显示"正在录")
 *
 * @param[in] cfd 连接
 * @param[in] req 请求(用 Host 拼直播地址)
 * @return 0 成功; -1 失败
 *
 * @note ★ 为什么必须有这个接口:正在录的那一段是 `<stamp>.mp4.tmp`, **没有 moov、
 *       不可播**, 所以它**不在** `GET /recordings` 里。用户看不到它, 就会以为
 *       "最近这段时间没录"(2026-09-20 用户正是这么问的)。把它单独报出来 ——
 *       名字、已写字节、已录帧数、从几点开始 —— 时间轴上才有"现在"。
 */
static int handle_status(int cfd, const proto_http_request_t *req)
{
    svc_record_status_t st;
    size_t              used = 0;

    svc_record_get_status(&st);
    if (proto_str_append(g.text, sizeof(g.text), &used, "{\"recording\":") != 0 ||
        proto_str_append(g.text, sizeof(g.text), &used, st.recording ? "1" : "0")
            != 0) {
        return send_error(cfd, 500, NULL);
    }
    if (st.recording && append_status_fields(&used, &st) != 0) {
        return send_error(cfd, 500, NULL);
    }
    if (req->has_host && append_live_uri(req->host, &used) != 0) {
        return send_error(cfd, 500, NULL);
    }
    if (proto_str_append(g.text, sizeof(g.text), &used, "}\n") != 0) {
        return send_error(cfd, 500, NULL);
    }
    if (send_head_only(cfd, 200, "application/json; charset=utf-8", used,
                       NULL, HTTP_HDR_NO_STORE) != 0) {
        return -1;
    }
    if (infra_tcp_write_all(cfd, g.text, used) != 0) {
        return -1;
    }
    g.stats.bytes_sent += used;
    return 0;
}

/**
 * @brief 把请求里的 Range 换算成**文件内的绝对区间**
 *
 * @param[in]  req   请求(含 Range)
 * @param[in]  total 文件总字节数
 * @param[out] first 起始偏移(含)
 * @param[out] last  结束偏移(含)
 * @return 1 = 可以满足; 0 = **不可满足**(调用方回 416)
 *
 * @note 三种形式的换算都在这里(RFC 7233 §2.1):
 *       `a-b` → [a, min(b, total-1)];`a-` → [a, total-1];`-N` → [total-N, total-1]。
 * @note 起点越界(`a >= total`)、空文件、`bytes=-0` 都算不可满足 —— 这些正是
 *       客户端拖到文件尾巴时会发出来的请求, 必须回 416 而不是回垃圾数据。
 */
static int range_resolve(const proto_http_request_t *req, uint64_t total,
                         uint64_t *first, uint64_t *last)
{
    if (total == 0) {
        return 0;
    }
    if (req->range_is_suffix) {
        if (req->range_first == 0) {
            return 0;
        }
        *first = (req->range_first >= total) ? 0 : total - req->range_first;
        *last  = total - 1;
        return 1;
    }
    *first = req->range_first;
    if (*first >= total) {
        return 0;
    }
    *last = (req->range_last == UINT64_MAX || req->range_last >= total)
            ? (total - 1) : req->range_last;
    return (*last >= *first) ? 1 : 0;
}

/**
 * @brief 把文件的一段写出去(循环读 + 写)
 *
 * @param[in] cfd   连接
 * @param[in] fd    已打开的文件
 * @param[in] first 起始偏移
 * @param[in] len   字节数
 * @return 0 成功; -1 失败
 */
static int send_file_body(int cfd, int fd, uint64_t first, uint64_t len)
{
    uint64_t done = 0;

    if (lseek(fd, (off_t)first, SEEK_SET) < 0) {
        return -1;
    }
    while (done < len) {
        size_t want = (size_t)((len - done > HTTP_CHUNK) ? HTTP_CHUNK
                                                         : (len - done));
        ssize_t got;

        if (g.stop_requested) {
            return -1;                  /* 正在停服务: 立刻收手(别让 join 等到天荒地老) */
        }
        got = read(fd, g.chunk, want);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            LOG_ERROR("回放: 读文件提前结束(已发 %llu/%llu 字节)",
                      (unsigned long long)done, (unsigned long long)len);
            return -1;                  /* 文件被改了/坏了: 只能断开(头已经发出去了) */
        }
        if (infra_tcp_write_all(cfd, g.chunk, (size_t)got) != 0) {
            return -1;                  /* 客户端断了/不读了(见 http_thread 的说明) */
        }
        done += (uint64_t)got;
    }
    g.stats.bytes_sent += done;
    return 0;
}

/**
 * @brief 拼 `Content-Range: bytes a-b/total` 并发出应答头
 *
 * @param[in] cfd    连接
 * @param[in] status 200 或 206
 * @param[in] first  区间起点(206 时有效)
 * @param[in] last   区间终点(含)
 * @param[in] total  文件总长
 * @param[in] hdrs   额外应答头(**整串**, 已带 `\r\n`);NULL = 用默认的 `HTTP_HDR_FILE`
 * @return 0 成功; -1 失败
 *
 * @note `hdrs` 这一维是 `/recent.mp4` 加的:它要把"**这次给你的是哪一段**"告诉客户端
 *       (`X-Recent-Clip: <名字>`), 否则用户根本不知道点一下拿到的是哪段录像。
 */
static int send_file_head(int cfd, int status, uint64_t first, uint64_t last,
                          uint64_t total, const char *hdrs)
{
    char   cr[80];
    size_t used = 0;
    size_t len  = (last >= first) ? (size_t)(last - first + 1) : 0;

    if (status == 206) {
        if (proto_str_append(cr, sizeof(cr), &used, "bytes ") != 0 ||
            proto_str_append_u64(cr, sizeof(cr), &used, first) != 0 ||
            proto_str_append(cr, sizeof(cr), &used, "-") != 0 ||
            proto_str_append_u64(cr, sizeof(cr), &used, last) != 0 ||
            proto_str_append(cr, sizeof(cr), &used, "/") != 0 ||
            proto_str_append_u64(cr, sizeof(cr), &used, total) != 0) {
            return -1;
        }
    }
    return send_head_only(cfd, status, "video/mp4", len,
                          (status == 206) ? cr : NULL,
                          (hdrs != NULL) ? hdrs : HTTP_HDR_FILE);
}

/**
 * @brief `GET|HEAD /recordings/<名字>` —— 取流(支持 Range)
 *
 * @param[in] cfd  连接
 * @param[in] name 分段名
 * @param[in] req  请求(取方法、Range)
 * @param[in] hdrs 额外应答头(NULL = 默认);`/recent.mp4` 用它报"这次是哪一段"
 * @return 0 成功; -1 失败
 */
static int handle_file(int cfd, const char *name, const proto_http_request_t *req,
                       const char *hdrs)
{
    char        path[SVC_RECORD_PATH_MAX];
    struct stat st;
    uint64_t    total;
    uint64_t    first = 0;
    uint64_t    last  = 0;
    int         status = 200;
    int         fd;

    if (svc_record_make_path(name, path, sizeof(path)) <= 0) {
        return send_error(cfd, 404, NULL);      /* 名字不合法: 当"没有"处理 */
    }
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return send_error(cfd, 404, NULL);
    }
    total = (uint64_t)st.st_size;
    last  = (total > 0) ? (total - 1) : 0;
    if (req->has_range) {
        if (!range_resolve(req, total, &first, &last)) {
            char cr[48];

            (void)close(fd);
            snprintf(cr, sizeof(cr), "bytes */%llu", (unsigned long long)total);
            g.stats.files++;
            return send_error(cfd, 416, cr);
        }
        status = 206;
        g.stats.partials++;
    }
    g.stats.files++;
    if (send_file_head(cfd, status, first, last, total, hdrs) != 0) {
        (void)close(fd);
        return -1;
    }
    if (req->method == PROTO_HTTP_HEAD || total == 0) {
        (void)close(fd);
        return 0;                       /* HEAD: 只回头; 空文件: 没有 body */
    }
    if (send_file_body(cfd, fd, first, last - first + 1) != 0) {
        (void)close(fd);
        return -1;
    }
    (void)close(fd);
    return 0;
}

/**
 * @brief 取"**已经收尾的最新那一段**"的名字
 *
 * @param[out] out 输出(形如 `<stamp>.mp4`)
 * @param[in]  cap 容量
 * @return 0 成功; -1 = 一段都没有(或目录打不开)
 *
 * @note 调用线程: **回放服务的 HTTP 线程**(`svc_record_list()` 本身任何线程可调)。
 * @note 阻塞行为: 会扫一遍录制目录(几百个文件, 毫秒级), 不发网络、不写盘。
 */
static int newest_closed(char *out, size_t cap)
{
    svc_record_policy_file_t one[1];

    if (svc_record_list(one, 1, NULL, NULL) != 1) {
        return -1;
    }
    snprintf(out, cap, "%s", one[0].name);
    return 0;
}

/**
 * @brief 等某个分段"收尾完成" —— 也就是 `<dir>/<name>` 真的出现
 *
 * @param[in] name 分段名(收尾前它是 `<name>.tmp`)
 * @return 0 = 已经收尾; -1 = 等超时(或名字非法)
 *
 * @note 调用线程: **回放服务的 HTTP 线程**。
 * @note 阻塞行为: ⚠️ **这里会阻塞, 最多 `HTTP_ROTATE_WAIT_MS` = 5 秒**
 *       (每 `HTTP_ROTATE_POLL_MS` 查一次文件)。串行服务下这会占住服务线程 ——
 *       这是"串行模型"的已知代价, 已写在 `svc_http.h` 的【简化上限】里。
 * @note 为什么用"文件出现"当判据:录制线程收尾的顺序是
 *       `MP4Close`(写 moov)→ `rename(<name>.mp4.tmp → <name>.mp4)`。所以
 *       **`<name>.mp4` 存在** 就等于"这个文件已经有 moov 了" —— 正是我们能发给播放器的条件。
 * @note ⚠️ 【简化上限】这里是**轮询 + 睡 50ms**(最多 `HTTP_ROTATE_WAIT_MS`)。
 *       更讲究的做法是让录制线程收尾后 `pthread_cond_signal`, 由这里等条件变量。
 *       现在这样够用:一次"看最新"只轮询几十次, 且这条路径本来就是"用户点了才走一次"。
 */
static int wait_closed(const char *name)
{
    char        path[SVC_RECORD_PATH_MAX];
    struct stat st;
    int         waited = 0;

    while (waited < HTTP_ROTATE_WAIT_MS) {
        if (svc_record_make_path(name, path, sizeof(path)) > 0 &&
            stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            return 0;
        }
        /* 睡法与 `svc_media.c` / `svc_osd.c` 保持一致: 用 `nanosleep`。
         * (2026-09-20 复核: `usleep` 是 POSIX.1-2001 的过时接口, 后面被移出标准;
         *  本项目别处一律用 nanosleep, 别在这里破例。) */
        {
            struct timespec ts = { 0, HTTP_ROTATE_POLL_MS * 1000 * 1000 };

            nanosleep(&ts, NULL);
        }
        waited += HTTP_ROTATE_POLL_MS;
    }
    return -1;
}

/**
 * @brief `GET|HEAD /recent.mp4` —— "**刚录的这段**"立刻能看(2026-09-20 新增)
 *
 * @param[in] cfd 连接
 * @param[in] req 请求(取方法、Range)
 * @return 0 成功; -1 失败
 *
 * @details
 * ─────────────────────────────────────────────────────────────────
 *  它解决什么问题
 * ─────────────────────────────────────────────────────────────────
 *  正在写的那一段是 `<stamp>.mp4.tmp`, **没有 moov(索引)⇒ 任何播放器都打不开**,
 *  所以它不在回放列表里。代价就是"最近这段画面看不了", 而缺口大小 = 段长
 *  (默认 30 分钟, 板上现设 300 秒)。用户的原话是:"后面的时间去哪里了呢"。
 *
 * ─────────────────────────────────────────────────────────────────
 *  怎么做(以及为什么**不**自己重封一段)
 * ─────────────────────────────────────────────────────────────────
 *  最小的正确做法是**借用已有的切段路径**:请录制线程在**下一个 IDR** 处把当前段收尾
 *  (它本来就是这个逻辑, 一帧不丢), 于是这一秒之后 `<stamp>.mp4` 就出现了 ——
 *  一个完全正常、能播、能拖进度条的分段(内容一直到最后 1 秒)。
 *
 *  ⚠️ 被否掉的方案:把旁路裸流侧车 `<stamp>.h264.tmp` 的**尾部 N 秒**重封成小 MP4。
 *     三个问题:① 得**估算**"N 秒大概是多少字节"(码率浮动 ⇒ 估不准);
 *     ② 要在**另一个线程**里动 mp4v2(库的全局状态 + 录制线程正在用同一个库);
 *     ③ 重封期间录制队列会被挤满而丢帧(与 A12 同一个代价)。
 *     而"让录制线程切一段"这三条全都天然没有 —— 复用它才是对的(AGENTS.md §7.0)。
 *
 *  ⚠️ **代价(如实说)**:① 每请求一次就**多一个分段边界**(所以有
 *     `SVC_RECORD_ROTATE_MIN_SEC` = 20 秒的"段太新就不切"保护, 免得被连点切出碎段);
 *     ② 这个请求会**占住服务线程最多 5 秒**(串行模型, 见头文件)。
 */
static int handle_recent(int cfd, const proto_http_request_t *req)
{
    char name[PROTO_HTTP_NAME_MAX];
    char hdrs[HTTP_RECENT_HDR_MAX];
    int  age = -1;
    int  rc;

    rc = svc_record_request_rotate(name, sizeof(name), &age);
    if (rc < 0) {
        return send_error(cfd, 503, NULL);      /* 没在录: 没有"最近的段"可给 */
    }
    if (rc == 0) {
        g.stats.rotates++;
        LOG_INFO("回放: /recent.mp4 请求切段(段 %s 已录 %d 秒)", name, age);
        if (wait_closed(name) != 0) {
            /* 没等到(例如刚巧自然切段、请求被作废): 退而发"已有的最新段" */
            LOG_WARN("回放: 等 %s 收尾超时, 退而发已有的最新段", name);
            if (newest_closed(name, sizeof(name)) != 0) {
                return send_error(cfd, 503, NULL);
            }
        }
    } else if (newest_closed(name, sizeof(name)) != 0) {
        return send_error(cfd, 503, NULL);      /* 连一段都没收尾过 */
    }
    if (snprintf(hdrs, sizeof(hdrs), HTTP_HDR_FILE "X-Recent-Clip: %s\r\n",
                 name) >= (int)sizeof(hdrs)) {
        return send_error(cfd, 500, NULL);
    }
    LOG_INFO("回放: /recent.mp4 → 发 %s(段龄 %d 秒, %s)", name, age,
             (rc == 0) ? "刚请求切段" : "段太新, 用已有的最新段");
    return handle_file(cfd, name, req, hdrs);
}

/**
 * @brief 判定"锁定还是解锁"
 *
 * @param[in] cfd      连接(**非阻塞**)
 * @param[in] req      请求
 * @param[in] body     服务层**已经收到缓冲里**的 body(可为 NULL)
 * @param[in] body_len 缓冲里已有的 body 字节数
 * @return 1 = 锁定; 0 = 解锁; -1 = body 不合法 / 等不到
 *
 * @note **没有 body 就默认"锁定"** —— `curl -X PUT .../lock` 这样最省事;
 *       要解锁就显式发 `0`(`curl -X PUT -d 0 .../lock`)。
 * @note ★ 关键:body 常常**和请求头在同一个 TCP 段里**一起到达, 那时它已经在
 *       `body` 缓冲里(服务层第一次 `recv` 就读进来了)。先看缓冲, **不要**直接去
 *       socket 上等 —— 那样会 500 ms 等不到、把好好的请求判成 400(见 B042 后半段)。
 * @note ⚠️ 真没跟着来(声明了 `Content-Length` 却迟迟不发)时, 只等
 *       `HTTP_BODY_WAIT_MS` 就报 400 —— 免得一个"说要有 body 却不发"的连接把服务卡住。
 */
static int lock_flag_of(int cfd, const proto_http_request_t *req,
                        const char *body, size_t body_len)
{
    char          b[8];
    ssize_t       n;
    struct pollfd p;

    if (!req->has_content_length || req->content_length == 0) {
        return 1;
    }
    if (body_len > 0) {
        return (body[0] == '0') ? 0 : 1;        /* 跟着头一起来的 */
    }
    if (req->content_length > sizeof(b)) {
        return -1;
    }
    p.fd      = cfd;
    p.events  = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, HTTP_BODY_WAIT_MS) <= 0) {
        return -1;
    }
    n = recv(cfd, b, (size_t)req->content_length, 0);
    if (n <= 0) {
        return -1;
    }
    return (b[0] == '0') ? 0 : 1;
}

/**
 * @brief `PUT /recordings/<名字>/lock` —— 锁定 / 解锁
 *
 * @param[in] cfd      连接
 * @param[in] name     分段名
 * @param[in] req      请求(取 Content-Length)
 * @param[in] body     已收到的 body
 * @param[in] body_len body 字节数
 * @return 0 成功; -1 失败
 */
static int handle_lock(int cfd, const char *name, const proto_http_request_t *req,
                       const char *body, size_t body_len)
{
    char reply[80];
    int  on = lock_flag_of(cfd, req, body, body_len);
    int  n;

    if (on < 0) {
        return send_error(cfd, 400, NULL);
    }
    if (svc_record_set_lock(name, on) != 0) {
        return send_error(cfd, 404, NULL);      /* 名字不合法 / 清单写不了 */
    }
    g.stats.locks++;
    n = snprintf(reply, sizeof(reply), "{\"name\":\"%s\",\"locked\":%d}\n",
                 name, on);
    if (n < 0 || n >= (int)sizeof(reply)) {
        return send_error(cfd, 500, NULL);
    }
    if (send_head_only(cfd, 200, "application/json; charset=utf-8",
                       (uint64_t)n, NULL, HTTP_HDR_NO_STORE) != 0) {
        return -1;
    }
    if (infra_tcp_write_all(cfd, reply, (size_t)n) != 0) {
        return -1;
    }
    g.stats.bytes_sent += (uint64_t)n;
    return 0;
}

/**
 * @brief `GET /playlist.m3u` —— 给 VLC / mpv 的**整段回放**播放列表
 *
 * @param[in] cfd 连接
 * @param[in] req 请求(用它的 `Host` 拼绝对 URL)
 * @return 0 成功; -1 失败
 *
 * @note ★ 顺序是**旧 → 新**:播放列表是拿来"从头连着看"的, 时间必须顺着走
 *       (列表接口 `GET /recordings` 反而是新→旧, 因为人先看最新的)。
 * @note ★ URL 用**客户端自己发的 `Host`** 拼绝对地址 —— 服务端不该猜"板子 IP 是多少"
 *       (可能多网卡、可能被 NAT)。客户端没发 `Host` 时退回**相对 URL**
 *       (VLC 会相对播放列表自身地址解析)。
 * @note ⚠️ 【简化上限】`#EXTINF` 的时长写 **-1(未知)**:要报准确时长就得解析每个 MP4 的
 *       `mvhd`/`mdhd`(或用码率估), 而我们**不存**这个信息。
 *       用估算值会让 VLC 的时间轴长度是错的 —— **宁可写"未知"也不写假数字**。
 *       升级路径: 收尾时把时长写进文件名旁的 sidecar, 或在 `GET /recordings` 里带上它。
 */
static int handle_playlist(int cfd, const proto_http_request_t *req)
{
    const char *host = req->has_host ? req->host : NULL;
    size_t      used = 0;
    int         n;
    int         i;

    n = svc_record_list(g.segs, HTTP_SEG_MAX, NULL, NULL);
    if (n < 0) {
        return send_error(cfd, 500, NULL);
    }
    if (proto_str_append(g.text, sizeof(g.text), &used, "#EXTM3U\n") != 0) {
        return send_error(cfd, 500, NULL);
    }
    for (i = n - 1; i >= 0; i--) {              /* 旧 → 新 */
        int bad = 0;

        bad |= proto_str_append(g.text, sizeof(g.text), &used, "#EXTINF:-1,");
        bad |= proto_str_append(g.text, sizeof(g.text), &used, g.segs[i].name);
        bad |= proto_str_append(g.text, sizeof(g.text), &used, "\n");
        if (host != NULL) {
            bad |= proto_str_append(g.text, sizeof(g.text), &used, "http://");
            bad |= proto_str_append(g.text, sizeof(g.text), &used, host);
        }
        bad |= proto_str_append(g.text, sizeof(g.text), &used, "/recordings/");
        bad |= proto_str_append(g.text, sizeof(g.text), &used, g.segs[i].name);
        bad |= proto_str_append(g.text, sizeof(g.text), &used, "\n");
        if (bad != 0) {
            return send_error(cfd, 500, NULL);  /* 列表太长放不下 */
        }
    }
    if (send_head_only(cfd, 200, "audio/x-mpegurl", used, NULL,
                       HTTP_HDR_NO_STORE) != 0) {
        return -1;
    }
    if (infra_tcp_write_all(cfd, g.text, used) != 0) {
        return -1;
    }
    g.stats.bytes_sent += used;
    g.stats.playlists++;
    return 0;
}

/**
 * @brief `GET /` —— 把回放页面发出去(浏览器直接当客户端用)
 *
 * @param[in] cfd 连接
 * @return 0 成功; -1 失败
 *
 * @note 页面是**常量字符串**(`svc_http_page.c`), 不带任何前端构建步骤。
 */
static int handle_page(int cfd)
{
    size_t len = strlen(svc_http_page_html);

    if (send_head_only(cfd, 200, "text/html; charset=utf-8", len, NULL,
                       HTTP_HDR_NO_STORE) != 0) {
        return -1;
    }
    if (infra_tcp_write_all(cfd, svc_http_page_html, len) != 0) {
        return -1;
    }
    g.stats.bytes_sent += len;
    g.stats.pages++;
    return 0;
}

/** 帮助页(纯文本, 给人看的) */
static const char HTTP_HELP[] =
    "IPC 回放服务\n"
    "  GET  /                        回放页面(浏览器直接当客户端用)\n"
    "  GET  /help                    本帮助(纯文本)\n"
    "  GET  /status                  正在录的那一段(JSON:名字/字节/已录时长)\n"
    "  GET  /recent.mp4              \"刚录的这段\"马上能看:请录制线程把当前段收尾后发出\n"
    "                                (代价:多一个分段边界;段龄 < 20 秒时直接发上一段)\n"
    "  GET  /playlist.m3u            整段回放列表(给 VLC / mpv, 旧→新)\n"
    "  GET  /recordings              列出现有分段(JSON, 新→旧)\n"
    "        ?limit=N&before=<名字>   分页:只看某一段之前的 N 条(游标翻页)\n"
    "  GET  /recordings/<名字>        取流(支持 Range: bytes=)\n"
    "  HEAD /recordings/<名字>        只取响应头(探大小)\n"
    "  PUT  /recordings/<名字>/lock   body 1=锁定 / 0=解锁\n"
    "\n"
    "例: curl -r 0-1023 http://<板子IP>:8080/recordings/<名字> -o head.bin\n"
    "    ffplay http://<板子IP>:8080/recordings/<名字>\n"
    "    vlc    http://<板子IP>:8080/playlist.m3u     ← 一整天连着放\n";

/**
 * @brief 按解析结果分派:页面 / 播放列表 / 列表 / 取文件 / 锁定 / 帮助
 *
 * @param[in] cfd      连接
 * @param[in] req      已解析的请求
 * @param[in] body     跟请求头**一起收到**的 body(可为 NULL)
 * @param[in] body_len body 字节数
 * @return 0 成功; -1 发送失败
 */
static int dispatch(int cfd, const proto_http_request_t *req,
                    const char *body, size_t body_len)
{
    char                   path[PROTO_HTTP_TARGET_MAX];
    char                   name[PROTO_HTTP_NAME_MAX];
    proto_http_path_kind_t kind;

    if (req->method == PROTO_HTTP_OPTIONS) {
        return send_head_only(cfd, 204, NULL, 0, NULL, NULL);
    }
    if (proto_http_path(req->target, path, sizeof(path)) < 0) {
        return send_error(cfd, 400, NULL);
    }
    if (strcmp(path, "/") == 0) {
        return handle_page(cfd);
    }
    if (strcmp(path, "/help") == 0) {
        return send_text(cfd, HTTP_HELP);
    }
    if (strcmp(path, "/playlist.m3u") == 0 || strcmp(path, "/playlist") == 0) {
        return handle_playlist(cfd, req);
    }
    if (strcmp(path, "/status") == 0) {
        return handle_status(cfd, req);
    }
    if (strcmp(path, "/recent.mp4") == 0 || strcmp(path, "/recent") == 0) {
        return (req->method == PROTO_HTTP_GET || req->method == PROTO_HTTP_HEAD)
                   ? handle_recent(cfd, req) : send_error(cfd, 405, NULL);
    }
    kind = proto_http_match_path(path, name, sizeof(name));
    if (kind == PROTO_HTTP_PATH_LIST) {
        return (req->method == PROTO_HTTP_GET) ? handle_list(cfd, req)
                                              : send_error(cfd, 405, NULL);
    }
    if (kind == PROTO_HTTP_PATH_SEGMENT) {
        return (req->method == PROTO_HTTP_GET || req->method == PROTO_HTTP_HEAD)
                   ? handle_file(cfd, name, req, NULL) : send_error(cfd, 405, NULL);
    }
    if (kind == PROTO_HTTP_PATH_LOCK) {
        return (req->method == PROTO_HTTP_PUT)
                   ? handle_lock(cfd, name, req, body, body_len)
                   : send_error(cfd, 405, NULL);
    }
    return send_error(cfd, 404, NULL);
}

/* ─────────── ③ 收请求 + 分派 ─────────── */

/**
 * @brief 收全一个请求(读到空行为止), 再分派
 *
 * @param[in] cfd 已接受的连接(**非阻塞**)
 * @return 0 已处理; -1 收不到或发送失败(调用方关连接)
 *
 * @note 读循环**有界**:最多 `sizeof(g.req)` 字节、最长 `HTTP_READ_TIMEOUT_MS` 毫秒。
 *       既防"半个请求吊死线程", 也防"有人一直发数据撑爆缓冲"。
 * @note ⚠️ 客户端 socket 是**非阻塞**的:所以 `recv` 可能返回 `EAGAIN`(poll 说可读,
 *       但数据被别的读法取走了/对端刚关) —— 那是"再等一轮", 不是错误。
 */
static int serve_client(int cfd)
{
    size_t              len = 0;
    int                 waited = 0;
    proto_http_request_t req;
    int                 rc;

    while (len < sizeof(g.req) - 1) {
        struct pollfd p;
        ssize_t       n;

        if (g.stop_requested) {
            return -1;                  /* 停服务: 立刻收手 */
        }
        p.fd      = cfd;
        p.events  = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, HTTP_ACCEPT_POLL_MS) <= 0) {
            waited += HTTP_ACCEPT_POLL_MS;
            if (waited >= HTTP_READ_TIMEOUT_MS) {
                return -1;              /* 客户端磨蹭: 断开, 不占着服务 */
            }
            continue;
        }
        n = recv(cfd, g.req + len, sizeof(g.req) - 1 - len, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;                   /* 还没到: 再等一轮(不是错误) */
        }
        if (n <= 0) {
            return -1;
        }
        len += (size_t)n;
        g.req[len] = '\0';
        rc = proto_http_parse(g.req, len, &req);
        if (rc == PROTO_HTTP_EAGAIN) {
            continue;                   /* 头没收全, 继续收 */
        }
        if (rc == PROTO_HTTP_ETOOLONG) {
            return send_error(cfd, 431, NULL);
        }
        if (rc == PROTO_HTTP_EBADVER) {
            return send_error(cfd, 505, NULL);
        }
        if (rc < 0) {
            return send_error(cfd, 400, NULL);
        }
        g.stats.requests++;
        /* 头区之后的字节就是 body(可能已经在同一次 recv 里收到了) */
        return dispatch(cfd, &req, g.req + rc, len - (size_t)rc);
    }
    return send_error(cfd, 431, NULL);
}

/* ─────────── ④ 线程与对外接口 ─────────── */

/**
 * @brief 服务线程主循环:轮询监听口 → accept → 串行处理一个客户端
 *
 * @param arg 未使用
 * @return 永远返回 NULL
 *
 * @note 用 `poll` 而不是裸 `accept`:这样每 200 ms 有机会看到 `stop_requested`,
 *       停服务时不用靠"连一下自己"来唤醒 accept。
 */
static void *http_thread(void *arg)
{
    (void)arg;
    (void)prctl(PR_SET_NAME, "ipc_http", 0, 0, 0);
    LOG_INFO("回放服务线程启动(端口 %u)", (unsigned)g.port);

    while (!g.stop_requested) {
        struct pollfd p;
        int           cfd;

        p.fd      = g.listen_fd;
        p.events  = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, HTTP_ACCEPT_POLL_MS) <= 0) {
            continue;
        }
        cfd = accept(g.listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno != EINTR) {
                LOG_WARN("回放: accept 失败: %s", strerror(errno));
            }
            continue;
        }
        g.stats.conns++;
        /*
         * ★ 客户端 socket 必须**非阻塞**(2026-09-19 修的 B041):
         *   原来用阻塞 socket, 而"客户端不读数据"时 `send()` 会**在核里无限阻塞** ——
         *   单线程服务就此永久卡死: 其它客户端的连接全堆在 backlog 里没人 accept,
         *   `curl` 直接拿不到任何响应(`%{http_code}` = 000)。
         *   实测证据: `/proc/<tid>/syscall` 显示该线程卡在
         *   `send(fd=27, buf, 32768, MSG_NOSIGNAL)`, 而 `/proc/<tid>/fd/27` 是 `socket:[…]`。
         *   改成非阻塞之后, `send` 会得到 `EAGAIN` ⇒ `infra_tcp_write_all()` 用 `poll`
         *   等 100 ms, 等不到就返回 -1 ⇒ 我们把这个连接**丢掉**, 服务继续转。
         */
        (void)infra_set_nonblocking(cfd);
        g.cur_fd = cfd;
        (void)serve_client(cfd);
        g.cur_fd = -1;
        (void)close(cfd);               /* 【简化上限】每个请求一条连接 */
    }
    LOG_INFO("回放服务线程退出(连接 %llu / 请求 %llu / 206 %llu)",
             (unsigned long long)g.stats.conns,
             (unsigned long long)g.stats.requests,
             (unsigned long long)g.stats.partials);
    return NULL;
}

int svc_http_start(const svc_http_cfg_t *cfg)
{
    if (g.running) {
        return 0;
    }
    memset(&g.stats, 0, sizeof(g.stats));
    snprintf(g.bind_ip, sizeof(g.bind_ip), "%s",
             (cfg != NULL && cfg->bind_ip != NULL) ? cfg->bind_ip : "0.0.0.0");
    g.port = (cfg != NULL && cfg->port != 0) ? cfg->port : SVC_HTTP_DEFAULT_PORT;
    g.stop_requested = 0;
    g.cur_fd         = -1;

    g.listen_fd = infra_tcp_listen(g.bind_ip, g.port, 8);
    if (g.listen_fd < 0) {
        LOG_ERROR("回放: 端口 %u 监听失败: %s", (unsigned)g.port, strerror(errno));
        return -1;
    }
    if (pthread_create(&g.thread, NULL, http_thread, NULL) != 0) {
        LOG_ERROR("回放: 线程创建失败");
        infra_close(&g.listen_fd);
        return -2;
    }
    g.thread_valid = 1;
    g.running      = 1;
    return 0;
}

void svc_http_stop(void)
{
    if (!g.running) {
        return;
    }
    g.stop_requested = 1;
    /*
     * ★ 关键:把**正在服务的那个连接**打断(2026-09-19 修的 B041 的第二半)。
     *   否则 `pthread_join` 会等一个卡在 `send()`/`recv()` 上的线程 —— 那等于
     *   "`kill -TERM` 杀不掉进程"(实测: 旧进程一直活着、还占着 8554 端口,
     *   新进程启动时报 `监听 0.0.0.0:8554 失败`)。
     *   `shutdown()` 会让对端收到 FIN, 阻塞中的 send/recv 立刻返回错误 ⇒ 线程退出。
     */
    if (g.cur_fd >= 0) {
        (void)shutdown(g.cur_fd, SHUT_RDWR);
    }
    if (g.thread_valid) {
        (void)pthread_join(g.thread, NULL);
        g.thread_valid = 0;
    }
    infra_close(&g.listen_fd);
    g.running = 0;
    LOG_INFO("回放服务已停止(请求 %llu / 发出 %llu 字节)",
             (unsigned long long)g.stats.requests,
             (unsigned long long)g.stats.bytes_sent);
}

int svc_http_is_running(void)
{
    return g.running ? 1 : 0;
}

uint16_t svc_http_port(void)
{
    return g.port;
}

void svc_http_get_stats(svc_http_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = g.stats;
}
