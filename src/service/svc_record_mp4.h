/**
 * @file    svc_record_mp4.h
 * @brief   mp4v2 的 **C 接口垫片** —— 只声明我们用到的那 8 个函数
 *
 * 【模块职责】把 mp4v2 的 C++ 头文件"翻译"成 C 能用的声明
 * 【依赖方向】不依赖任何东西(只用 <stdbool.h> / <stdint.h>)
 * 【线程模型】纯声明, 无状态
 * 【资源边界】纯声明
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 为什么需要这个垫片(2026-09-16 实测, 三步查清)
 * ─────────────────────────────────────────────────────────────────
 *  ① **符号是 C 链接的** —— `nm libmp4v2.a` 输出 `000004e4 T MP4Create`,
 *     **没有 C++ 名字修饰** ⇒ C 代码完全可以调它。
 *  ② **但它的头文件是 C++ 的**:
 *       · `file.h` 里有裸的 `= nullptr`            → C 编译**直接语法错**
 *       · 它的 `DEFAULT(x)` 宏在 C 下展开成**空**   → 那些参数变成必填
 *     所以 **`#include <mp4v2/mp4v2.h>` 从 C 里根本编不过**(已实测)。
 *  ③ 别的路都不通:
 *       · 编译成 C++ —— 要为**单个文件**换编译器, 与"全项目 C11"冲突
 *       · 用别处的旧版头文件 —— 与手上的库**版本可能不一致**, ABI 风险更大
 *
 *  ⇒ 自己声明。**签名逐字抄自库自带头文件**;抓取脚本 `work/probe_mp4v2_api.py`
 *    (它把原文抓出来), 换 mp4v2 版本时重跑一遍对一下即可。
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 垫片的风险与兜底
 * ─────────────────────────────────────────────────────────────────
 *  签名抄错的话**编译器不会报** —— 参数个数对不上会报, 但类型接近时
 *  只是调用约定不对, **运行期才炸**。
 *  兜底就是 M3 的验收本身:**录出来的 MP4 必须能用 `ffprobe` 播出来**(A4)。
 *  那是端到端验证, 覆盖了这里每一个调用。
 */
#ifndef __SVC_RECORD_MP4_H__
#define __SVC_RECORD_MP4_H__

#include <stdbool.h>
#include <stdint.h>

/* ── 类型与常量:抄自 mp4v2/general.h ── */

/** mp4v2 里就是 `void*`, 这里保持一致 */
typedef void     *MP4FileHandle;
typedef uint32_t  MP4TrackId;
typedef uint64_t  MP4Duration;

/** 无效文件句柄(`(MP4FileHandle)NULL`) */
#define MP4_INVALID_FILE_HANDLE ((MP4FileHandle)NULL)
/** 无效轨道号 */
#define MP4_INVALID_TRACK_ID    ((MP4TrackId)0)
/** "用默认时长"的哨兵(`(MP4Duration)-1`) */
#define MP4_INVALID_DURATION    ((MP4Duration)-1)

/* ── 函数:抄自 file.h / file_prop.h / track.h / sample.h ── */

/** @brief 新建一个 MP4 文件。@param flags 目前填 0 */
MP4FileHandle MP4Create(const char *fileName, uint32_t flags);

/** @brief 关闭文件。**索引(moov)就是在这里落盘的**。@param flags 填 0 */
void MP4Close(MP4FileHandle hFile, uint32_t flags);

/** @brief 设文件时间基(我们用 90000, 与 RTP 一致) */
bool MP4SetTimeScale(MP4FileHandle hFile, uint32_t value);

/** @brief 设 ProfileLevel(厂商 sample 用 0x7F = Unconstrained) */
void MP4SetVideoProfileLevel(MP4FileHandle hFile, uint8_t value);

/**
 * @brief 加一条 H.264 视频轨
 *
 * @param sampleLenFieldSizeMinusOne **每个 NALU 前的长度字段字节数减 1**
 *        —— 我们用 4 字节长度前缀, 所以填 **3**
 * @return 轨道号;`MP4_INVALID_TRACK_ID` = 失败
 */
MP4TrackId MP4AddH264VideoTrack(MP4FileHandle hFile, uint32_t timeScale,
                                MP4Duration sampleDuration, uint16_t width,
                                uint16_t height, uint8_t AVCProfileIndication,
                                uint8_t profile_compat,
                                uint8_t AVCLevelIndication,
                                uint8_t sampleLenFieldSizeMinusOne);

/** @brief 写入 SPS(每条轨道一次即可) */
void MP4AddH264SequenceParameterSet(MP4FileHandle hFile, MP4TrackId trackId,
                                    const uint8_t *pSequence,
                                    uint16_t sequenceLen);

/** @brief 写入 PPS(每条轨道一次即可) */
void MP4AddH264PictureParameterSet(MP4FileHandle hFile, MP4TrackId trackId,
                                   const uint8_t *pPict, uint16_t pictLen);

/**
 * @brief 写一个 sample(一个 NALU)
 *
 * @param pBytes          **含 4 字节大端长度前缀**的缓冲
 * @param numBytes        含前缀的总字节数
 * @param duration        填 `MP4_INVALID_DURATION` = 用轨道默认时长
 * @param renderingOffset 填 0
 * @param isSyncSample    关键帧填 true
 * @return true 成功
 */
bool MP4WriteSample(MP4FileHandle hFile, MP4TrackId trackId,
                    const uint8_t *pBytes, uint32_t numBytes,
                    MP4Duration duration, MP4Duration renderingOffset,
                    bool isSyncSample);

#endif /* __SVC_RECORD_MP4_H__ */
