/**
 * @file    proto_str.h
 * @brief   协议文本构造/解析的公共小工具(整数转文本、带容量检查的追加、十进制解析)
 *
 * @details
 * **为什么单独一个模块**:`proto_sdp.c` 和 `proto_rtsp.c` 都需要
 * "把数字拼进文本"和"从文本里读数字"。各写一份的话, 改一处忘另一处就是 bug。
 * 抽出来共用, 顺便让两个协议模块都短一截。
 *
 * **为什么不用 snprintf / strtol**:
 *   · 不吃 locale(解析外来数据时 `tolower`/`strtol` 的行为受 locale 影响)
 *   · 不做动态分配
 *   · 行为完全可预测, 便于单测
 *
 * **不用 `<ctype.h>` / `<stdlib.h>`** —— 与本项目"可移植、可单测"的定位一致:
 * 只要有个 C 编译器就能编, 不依赖 libc 的 locale 实现。
 *
 * @note 全部函数**不阻塞、不分配内存、无静态状态**, 可重入。
 * @note 属于 protocol 层: 不碰硬件、不碰 socket。
 */
#ifndef __PROTO_STR_H__
#define __PROTO_STR_H__

#include <stddef.h>
#include <stdint.h>

/**
 * 把无符号整数写成十进制文本。
 *
 * @param v    要转换的值
 * @param out  输出缓冲
 * @param cap  out 的容量(含结尾 '\0');**至少 11 字节**(uint32 最大 10 位)
 * @return 写入的字符数(不含 '\0'); -1 = 缓冲不够
 */
int proto_str_u32(uint32_t v, char *out, size_t cap);

/**
 * 解析十进制无符号整数(遇非数字即停, 不报错)。
 *
 * @param p    文本起点
 * @param out  解析结果
 * @return 消耗的字符数; **0 表示没解析到数字**
 *
 * @note 只接受 0~9;允许后面紧跟分隔符(如 ';'、'-'、'\0')
 */
size_t proto_str_parse_u32(const char *p, uint32_t *out);

/**
 * 把一段文本追加到缓冲末尾(就地追加, 自带容量检查)。
 *
 * @param out   目标缓冲
 * @param cap   容量(含结尾 '\0')
 * @param used  已用字符数(调用方维护, 成功时被更新)
 * @param text  要追加的字符串
 * @return 0 = 成功; -1 = 容量不够(此时缓冲**未被修改**)
 */
int proto_str_append(char *out, size_t cap, size_t *used, const char *text);

/**
 * 把无符号整数转文本后追加(等价于 append(u32_to_str(v)))。
 * @return 0 = 成功; -1 = 容量不够
 */
int proto_str_append_u32(char *out, size_t cap, size_t *used, uint32_t v);

/**
 * ASCII 转小写。
 * @note 刻意不用 `<ctype.h>` 的 `tolower`: 它受 locale 影响,
 *       用它比较 HTTP/RTSP 头名在不同语言环境下行为可能不一致。
 */
char proto_str_lower(char c);

/**
 * 大小写无关比较(最多 n 字节)。
 * @return 1 = 前 n 字节相同(或提前遇到相同结尾); 0 = 不同
 */
int proto_str_eq_ci_n(const char *a, const char *b, size_t n);

/**
 * 大小写无关比较(以 '\0' 结尾的整串)。
 * @return 1 = 相同; 0 = 不同
 */
int proto_str_eq_ci(const char *a, const char *b);

#endif /* __PROTO_STR_H__ */
