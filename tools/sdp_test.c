/**
 * @file    sdp_test.c
 * @brief   proto_sdp.c 的单元测试 —— 拿真实参数集验证生成的 SDP
 *
 * 【怎么验证】(可证伪)
 *   ① 参数集是**从真实码流里抽出来的**(不是手写的), 所以输入可信
 *   ② 生成的 SDP 会打印出来, 可以和 work/gen_sdp_reference.py 用 Python
 *      独立算出的结果**逐字符比对** —— 两套实现(shell 的 base64 vs 我们手写的)
 *      得到同样结果, 才说明 Base64 没写错
 *   ③ 另外做几组边界测试: 缓冲太小、空参数集、非法 PT
 *
 * 编译(主机):
 *   gcc -Wall -Wextra -O2 -o sdp_test sdp_test.c \
 *       ../src/protocol/proto_sdp.c ../src/protocol/proto_nalu.c -I../src/protocol
 *
 * 用法:
 *   ./sdp_test <file.h264|file.h265>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto_nalu.h"
#include "proto_sdp.h"

static proto_sdp_cfg_t g_cfg;
static int             g_is_h265;
static int             g_done;

/** 从码流里抓第一个 SPS/PPS/VPS 并填进 cfg。 */
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
    } else if (n->kind == PROTO_NALU_KIND_VPS && g_cfg.vps_len == 0 &&
               n->len <= PROTO_SDP_PARAM_MAX) {
        memcpy(g_cfg.vps, n->data, n->len);
        g_cfg.vps_len = n->len;
    }

    /* 收齐了 H.265 的 3 个 / H.264 的 2 个就停 */
    if (g_is_h265)
        g_done = (g_cfg.vps_len && g_cfg.sps_len && g_cfg.pps_len);
    else
        g_done = (g_cfg.sps_len && g_cfg.pps_len);

    return g_done ? 1 : 0;
}

static int check(const char *name, int cond)
{
    printf("   %-46s %s\n", name, cond ? "[通过]" : "[失败] <<<");
    return cond ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char     *path;
    uint8_t        *buf;
    long            size;
    FILE           *fp;
    char            sdp[PROTO_SDP_BUF_SIZE];
    int             n;
    int             fails = 0;

    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.h264|file.h265>\n", argv[0]);
        return 1;
    }
    path     = argv[1];
    g_is_h265 = (strstr(path, ".h265") != NULL);

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "读文件失败: %s\n", path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "读文件内容失败\n");
        fclose(fp);
        free(buf);
        return 1;
    }
    fclose(fp);

    printf("===== proto_sdp 单元测试 =====\n");
    printf("码流文件: %s (%ld 字节, %s)\n\n", path, size, g_is_h265 ? "H.265" : "H.264");

    /* ── ① 抽出真实参数集 ── */
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.origin_ip    = "192.168.16.88";
    g_cfg.session_name = "IPC Camera";
    g_cfg.is_h265      = g_is_h265;
    g_cfg.payload_type = g_is_h265 ? 97 : 96;

    n = proto_nalu_foreach(buf, (size_t)size, g_is_h265, on_nalu, NULL);
    printf("① 从码流抽参数集(遍历了 %d 个 NALU)\n", n);
    printf("   VPS %zu 字节 / SPS %zu 字节 / PPS %zu 字节\n\n",
           g_cfg.vps_len, g_cfg.sps_len, g_cfg.pps_len);
    fails += check("SPS 已取到", g_cfg.sps_len > 0);
    fails += check("PPS 已取到", g_cfg.pps_len > 0);
    if (g_is_h265)
        fails += check("VPS 已取到(H.265 必需)", g_cfg.vps_len > 0);
    printf("\n");

    /* ── ② 生成 SDP ── */
    n = proto_sdp_build(&g_cfg, sdp, sizeof(sdp));
    printf("② proto_sdp_build() 返回 %d 字节\n", n);
    fails += check("返回值为正(成功)", n > 0);
    if (n > 0) {
        printf("\n----- 生成的 SDP(逐字节)-----\n");
        fputs(sdp, stdout);
        printf("----- 结束(共 %d 字节)-----\n\n", n);
    }

    /* ── ③ 结构检查 ── */
    printf("③ 结构检查\n");
    fails += check("以 v=0 开头", strncmp(sdp, "v=0", 3) == 0);
    fails += check("含 m=video 行", strstr(sdp, "m=video 0 RTP/AVP ") != NULL);
    fails += check("含 a=rtpmap", strstr(sdp, "a=rtpmap:") != NULL);
    fails += check("含 a=fmtp", strstr(sdp, "a=fmtp:") != NULL);
    fails += check("含 a=control:streamid=0", strstr(sdp, "a=control:streamid=0") != NULL);
    fails += check("行尾是 CRLF", strstr(sdp, "\r\n") != NULL);

    if (g_is_h265) {
        fails += check("H.265 用 sprop-vps", strstr(sdp, "sprop-vps=") != NULL);
        fails += check("H.265 用 sprop-sps", strstr(sdp, "sprop-sps=") != NULL);
        fails += check("H.265 用 sprop-pps", strstr(sdp, "sprop-pps=") != NULL);
        /* ★ RFC 7798 §7.2.1: 三个参数集必须在同一行, 用分号分隔 */
        fails += check("H.265 三个 sprop 在同一 fmtp 行(分号分隔)",
                       strstr(sdp, "sprop-vps=") != NULL &&
                       strstr(sdp, ";sprop-sps=") != NULL &&
                       strstr(sdp, ";sprop-pps=") != NULL);
        fails += check("H.265 没有用逗号连参数集", strstr(sdp, "sprop-vps=") != NULL &&
                       strstr(sdp, "sprop-pps=") != NULL &&
                       /* 逗号只应出现在没有的地方: 检查 sprop 段里无逗号 */
                       (strchr(strstr(sdp, "sprop-vps="), ',') == NULL));
    } else {
        fails += check("H.264 用 sprop-parameter-sets", strstr(sdp, "sprop-parameter-sets=") != NULL);
        fails += check("H.264 的 SPS/PPS 用逗号分隔",
                       strstr(sdp, ",") != NULL);
        fails += check("packetization-mode=1", strstr(sdp, "packetization-mode=1") != NULL);
        fails += check("不能出现 packetization-mode=0", strstr(sdp, "packetization-mode=0") == NULL);
    }
    printf("\n");

    /* ── ④ 边界: 缓冲太小必须报错, 不能溢出 ── */
    printf("④ 边界测试\n");
    {
        char small[64];
        int  rc = proto_sdp_build(&g_cfg, small, sizeof(small));
        fails += check("缓冲不足时返回负值(不溢出)", rc < 0);
    }
    {
        proto_sdp_cfg_t bad = g_cfg;
        int rc;
        bad.payload_type = 10;              /* 静态 PT 区, 不允许 */
        rc = proto_sdp_build(&bad, sdp, sizeof(sdp));
        fails += check("非法 PT(<96)被拒绝", rc < 0);
    }
    {
        proto_sdp_cfg_t none = g_cfg;
        none.sps_len = none.pps_len = none.vps_len = 0;
        int rc = proto_sdp_build(&none, sdp, sizeof(sdp));
        fails += check("参数集为空时仍能生成(降级为无 sprop)", rc > 0);
        if (rc > 0 && g_is_h265)
            fails += check("  且未输出空的 sprop 字段", strstr(sdp, "sprop-vps=") == NULL);
    }
    printf("\n");

    /* ── ⑤ Base64 独立性验证 ── */
    printf("⑤ Base64 单独验证\n");
    {
        /* 已知答案: "Man" → "TWFu"(RFC 4648 经典例子) */
        char b64[16];
        const uint8_t man[3] = { 'M', 'a', 'n' };
        int rc = proto_sdp_base64(man, 3, b64, sizeof(b64));
        printf("   base64(\"Man\") = %s (期望 TWFu)\n", b64);
        fails += check("RFC 4648 已知向量 \"Man\"→\"TWFu\"", rc == 4 && strcmp(b64, "TWFu") == 0);
    }
    {
        char b64[16];
        const uint8_t m[1] = { 'M' };
        proto_sdp_base64(m, 1, b64, sizeof(b64));
        printf("   base64(\"M\")   = %s (期望 TQ==)\n", b64);
        fails += check("1 字节补两个 '='", strcmp(b64, "TQ==") == 0);
    }
    {
        char b64[16];
        const uint8_t ma[2] = { 'M', 'a' };
        proto_sdp_base64(ma, 2, b64, sizeof(b64));
        printf("   base64(\"Ma\")  = %s (期望 TWE=)\n", b64);
        fails += check("2 字节补一个 '='", strcmp(b64, "TWE=") == 0);
    }
    printf("\n");

    printf("===== 结果: %s(%d 项失败)=====\n", fails == 0 ? "全部通过" : "有失败", fails);
    free(buf);
    return fails == 0 ? 0 : 1;
}
