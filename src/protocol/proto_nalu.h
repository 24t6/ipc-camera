/**
 * @file    nalu.h
 * @brief   Annex-B 码流解析:剥离起始码、识别 NALU 类型
 *
 * 海思 VENC 输出的码流是 Annex-B 格式, 形如:
 *
 *     00 00 00 01 | 67 42 00 1F ... | 00 00 00 01 | 68 CE 31 B2 ... | 00 00 00 01 | 65 ...
 *     ↑起始码     ↑SPS               ↑起始码      ↑PPS              ↑起始码     ↑IDR 帧
 *
 * 而 RTP 传输时**不能带起始码**(RFC 6184), 必须切成一个个裸 NALU 分别打包。
 * 所以第一件事就是把 Annex-B 拆成 NALU 列表。
 *
 * ─────────────────────────────────────────────────────────────────
 *  H.264 NALU 头 (1 字节)
 * ─────────────────────────────────────────────────────────────────
 *    +---------------+
 *    |0|1|2|3|4|5|6|7|
 *    +-+-+-+-+-+-+-+-+
 *    |F|NRI|  Type   |
 *    +---------------+
 *      F   : forbidden_zero_bit, 必须为 0
 *      NRI : nal_ref_idc, 2 bit, 非 0 表示该 NALU 被其他帧参考
 *      Type: 5 bit
 *              1  = 非 IDR 的图像切片 (普通 P 帧)
 *              5  = IDR 图像切片 (关键帧)
 *              6  = SEI
 *              7  = SPS (序列参数集)
 *              8  = PPS (图像参数集)
 *              9  = AUD (访问单元分隔符)
 *
 * ─────────────────────────────────────────────────────────────────
 *  H.265 NALU 头 (2 字节)
 * ─────────────────────────────────────────────────────────────────
 *    +---------------+---------------+
 *    |0|1|2|3|4|5|6|7|0|1|2|3|4|5|6|7|
 *    +-+-+-+-+-+-+-+-+---------------+
 *    |F|   Type    |  LayerId  | TID |
 *    +---------------+---------------+
 *      F     : forbidden_zero_bit, 必须为 0
 *      Type  : 6 bit
 *               0..9   = VCL (图像数据)
 *               32 VPS / 33 SPS / 34 PPS / 39..40 SEI
 *      LayerId: 6 bit
 *      TID    : 3 bit + 1 保留位
 */
#ifndef __PROTO_NALU_H__
#define __PROTO_NALU_H__

#include <stddef.h>
#include <stdint.h>

/** NALU 最大长度上限(1080p I 帧通常几十 KB, 留足余量) */
#define PROTO_NALU_MAX_SIZE (1024 * 1024)

/** NALU 分类, 便于上层决定怎么打包/发送 */
typedef enum {
    PROTO_NALU_KIND_UNKNOWN = 0,
    PROTO_NALU_KIND_SPS,          /* H.264:7   / H.265:33 */
    PROTO_NALU_KIND_PPS,          /* H.264:8   / H.265:34 */
    PROTO_NALU_KIND_VPS,          /* H.265:32  (H.264 无此概念) */
    PROTO_NALU_KIND_SEI,          /* H.264:6   / H.265:39,40 */
    PROTO_NALU_KIND_IDR,          /* 关键帧切片 */
    PROTO_NALU_KIND_SLICE,        /* 普通帧切片 */
    PROTO_NALU_KIND_AUD,          /* 访问单元分隔符 */
} proto_nalu_kind_t;

/** 一个解析出来的 NALU(不含起始码) */
typedef struct {
    const uint8_t *data;    /* 指向原始缓冲内部, 不拥有内存 */
    size_t         len;     /* NALU 字节数(含 NALU 头) */
    uint8_t        type;    /* 原始类型值 */
    proto_nalu_kind_t    kind;    /* 归一化分类 */
    int            is_h265; /* 1=H.265, 0=H.264 */
    int            is_key;  /* 1=关键帧(IDR), 用于判断能否作为新客户端起点 */
} proto_nalu_t;

/**
 * 遍历回调。返回 0 继续, 非 0 提前终止。
 */
typedef int (*proto_nalu_cb_t)(const proto_nalu_t *nalu, void *user);

/**
 * @brief 把一段 Annex-B 缓冲拆成 NALU 逐个回调。
 *
 * @param buf      码流起始
 * @param len      码流长度
 * @param is_h265  1=按 H.265 解析, 0=按 H.264 解析
 * @param cb       回调
 * @param user     透传给回调
 * @return 已回调的 NALU 个数; 负值为错误
 */
int proto_nalu_foreach(const uint8_t *buf, size_t len, int is_h265,
                 proto_nalu_cb_t cb, void *user);

/**
 * @brief 判断一段 Annex-B 缓冲里是否含关键帧(IDR)。
 * 用于「新客户端何时可以开始看」的决策 —— 必须从 IDR 起才不出花屏。
 */
int proto_nalu_has_idr(const uint8_t *buf, size_t len, int is_h265);

/**
 * @brief 根据类型值给出可读名称, 便于打日志。
 */
const char *proto_nalu_kind_name(proto_nalu_kind_t kind);

#endif /* __PROTO_NALU_H__ */
