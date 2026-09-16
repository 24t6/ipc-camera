/**
 * @file    infra_log.c
 * @brief   分级日志实现 —— 见 infra_log.h
 *
 * 【模块职责】分级日志(DEBUG/INFO/WARN/ERROR)输出到 stdout
 * 【依赖方向】只依赖 libc —— **不依赖任何项目内模块**
 * 【线程模型】每条日志一次输出, 无共享可变状态(级别只在启动时设一次)
 * 【资源边界】无动态分配; 格式化缓冲在栈上且有长度上限
 *
 * 实现要点:
 *   · 先在本线程栈上的 buf 里拼完整行, 再**一次性 write()**
 *     —— 这样多线程同时打日志时, 输出不会互相插进半行
 *   · 用一把互斥锁保护最后的写动作; 拼串在锁外(各线程用各自的栈)
 *   · 写失败不崩溃、不打印错误 —— 日志模块自己报错会造成递归
 */
#include "infra_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/** 单条日志最大长度(超出的会被截断, 但一定以 '\n' 结尾) */
#define LOG_BUF_SIZE 512

static infra_log_level_t g_level = INFRA_LOG_INFO;
static pthread_mutex_t   g_lock  = PTHREAD_MUTEX_INITIALIZER;

void infra_log_set_level(infra_log_level_t level)
{
    g_level = level;
}

/**
 * @brief 取当前日志级别
 *
 * @return 当前的 infra_log_level_t
 */
infra_log_level_t infra_log_get_level(void)
{
    return g_level;
}

const char *infra_log_level_name(infra_log_level_t level)
{
    switch (level) {
    case INFRA_LOG_DEBUG: return "DEBUG";
    case INFRA_LOG_INFO:  return "INFO ";
    case INFRA_LOG_WARN:  return "WARN ";
    case INFRA_LOG_ERROR: return "ERROR";
    default:              return "?    ";
    }
}

int infra_log_write(infra_log_level_t level, const char *file, int line,
                    const char *fmt, ...)
{
    char    buf[LOG_BUF_SIZE];
    size_t  used = 0;
    int     n;
    int     rc;
    va_list ap;

    if (level < g_level)
        return -1;                      /* 被级别过滤 */

    /* ① 前缀: "[级别] 文件:行 " */
    n = snprintf(buf, sizeof(buf), "[%s] %s:%d ",
                 infra_log_level_name(level), file ? file : "?", line);
    if (n < 0)
        return -1;
    /* snprintf 返回"本该写的长度"; 截断时它 >= sizeof(buf), 所以要夹住 */
    used = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;

    /* ② 正文(只有还有空间时才拼) */
    if (used + 1 < sizeof(buf)) {
        va_start(ap, fmt);
        n = vsnprintf(buf + used, sizeof(buf) - used, fmt, ap);
        va_end(ap);
        if (n > 0) {
            size_t room = sizeof(buf) - used - 1;   /* 不含结尾 '\0' */
            used += ((size_t)n < room) ? (size_t)n : room;
        }
    }

    /* ③ 保证以 '\n' 结尾: 空间够就追加, 不够就覆盖最后一个字符 */
    if (used + 1 < sizeof(buf)) {
        buf[used++] = '\n';
        buf[used]   = '\0';
    } else {
        buf[sizeof(buf) - 2] = '\n';
        buf[sizeof(buf) - 1] = '\0';
        used = sizeof(buf) - 1;
    }

    rc = pthread_mutex_lock(&g_lock);
    if (rc == 0) {
        ssize_t w = write(STDERR_FILENO, buf, used);
        pthread_mutex_unlock(&g_lock);
        return (w < 0) ? -1 : (int)w;
    }
    return -1;
}
