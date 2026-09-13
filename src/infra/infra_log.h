/**
 * @file    infra_log.h
 * @brief   分级日志 —— 所有线程共用, 极短临界区
 *
 * @details
 * 为什么要一个日志模块而不是到处 printf:
 *   ① **分级**: 开发期打 INFO, 上板后只留 WARN/ERROR, 免得日志本身拖慢实时流
 *   ② **统一格式**: `[级别] 文件:行 消息` —— 板子上抓串口日志时一眼定位
 *   ③ **可重定向**: 默认写 stderr, 但保留接口(将来可同时写文件)
 *
 * @note 本模块**不是** protocol 层: 它做系统调用(写 fd), 属于 infra 层。
 * @note 单条日志用一个 buf 拼好再一次性写出, 尽量让多线程下的输出不交错。
 */
#ifndef __INFRA_LOG_H__
#define __INFRA_LOG_H__

#include <string.h>     /* 宏里用到 strrchr 取文件名 */

/** 日志级别(数值越大越严重) */
typedef enum {
    INFRA_LOG_DEBUG = 0,
    INFRA_LOG_INFO  = 1,
    INFRA_LOG_WARN  = 2,
    INFRA_LOG_ERROR = 3,
} infra_log_level_t;

/** 低于该级别的日志不输出; 默认 INFRA_LOG_INFO */
void infra_log_set_level(infra_log_level_t level);
infra_log_level_t infra_log_get_level(void);

/** 级别名(用于输出), 未知返回 "?" */
const char *infra_log_level_name(infra_log_level_t level);

/**
 * 输出一条日志(一般不用直接调, 用下面的宏)。
 *
 * @param level  级别
 * @param file   文件名(由宏用 __FILE__ 传入)
 * @param line   行号(由宏用 __LINE__ 传入)
 * @param fmt    printf 风格格式串
 * @return 实际写出的字节数; -1 表示被级别过滤掉或写失败
 *
 * @note 线程安全(内部加锁); 不阻塞(只写 stderr, 必要时由内核缓冲)。
 */
int infra_log_write(infra_log_level_t level, const char *file, int line,
                    const char *fmt, ...);

/** 取 __FILE__ 的最后一段(去掉路径), 让日志更短 */
#define INFRA_LOG_BASENAME(f) (strrchr((f), '/') ? strrchr((f), '/') + 1 : \
                              (strrchr((f), '\\') ? strrchr((f), '\\') + 1 : (f)))

#define INFRA_LOGF(level, ...) \
    infra_log_write((level), INFRA_LOG_BASENAME(__FILE__), __LINE__, __VA_ARGS__)

#define LOG_DEBUG(...) INFRA_LOGF(INFRA_LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  INFRA_LOGF(INFRA_LOG_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  INFRA_LOGF(INFRA_LOG_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) INFRA_LOGF(INFRA_LOG_ERROR, __VA_ARGS__)

#endif /* __INFRA_LOG_H__ */
