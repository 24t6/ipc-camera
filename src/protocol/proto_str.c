/**
 * @file    proto_str.c
 * @brief   协议文本工具实现 —— 见 proto_str.h 说明
 *
 * 【模块职责】协议层共用的文本小工具(查找 / 拷贝 / 数字解析与格式化)
 * 【依赖方向】只依赖 libc
 * 【线程模型】纯函数, 无状态
 * 【资源边界】无动态分配; 所有输出都写到调用方缓冲并带容量检查
 */
#include "proto_str.h"

#include <string.h>

int proto_str_u32(uint32_t v, char *out, size_t cap)
{
    char tmp[12];
    int  n = 0;
    int  i;

    if (out == NULL || cap < 11)
        return -1;

    if (v == 0)
        tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10u));     /* 低位先进, 所以是反的 */
        v /= 10u;
    }
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];                /* 倒过来 */
    out[n] = '\0';
    return n;
}

size_t proto_str_parse_u32(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    size_t   n = 0;

    if (p == NULL || out == NULL)
        return 0;

    while (p[n] >= '0' && p[n] <= '9' && n < 10) {
        v = v * 10u + (uint32_t)(p[n] - '0');
        n++;
    }
    if (n == 0)
        return 0;
    *out = v;
    return n;
}

int proto_str_append(char *out, size_t cap, size_t *used, const char *text)
{
    size_t len;

    if (out == NULL || used == NULL || text == NULL)
        return -1;

    len = strlen(text);
    if (*used + len + 1 > cap)
        return -1;                              /* 放不下就整体放弃, 不留半截 */
    memcpy(out + *used, text, len + 1);         /* 连结尾 '\0' 一起拷 */
    *used += len;
    return 0;
}

int proto_str_append_u32(char *out, size_t cap, size_t *used, uint32_t v)
{
    char num[12];

    if (proto_str_u32(v, num, sizeof(num)) < 0)
        return -1;
    return proto_str_append(out, cap, used, num);
}

char proto_str_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int proto_str_eq_ci_n(const char *a, const char *b, size_t n)
{
    size_t i;

    if (a == NULL || b == NULL)
        return 0;

    for (i = 0; i < n; i++) {
        if (a[i] == '\0' || b[i] == '\0')
            return a[i] == b[i];                /* 都到头了才算相同 */
        if (proto_str_lower(a[i]) != proto_str_lower(b[i]))
            return 0;
    }
    return 1;
}

int proto_str_eq_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return 0;

    while (*a != '\0' && *b != '\0') {
        if (proto_str_lower(*a) != proto_str_lower(*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}
