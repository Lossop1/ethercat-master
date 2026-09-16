#ifndef EMASTER_OBSERVATION_WIRE_H
#define EMASTER_OBSERVATION_WIRE_H

/*
 * 观测通道的线格式：定长小端二进制。
 *
 * 为什么不用文本 JSON。快速帧是 440 字节、按 50 Hz–1 kHz 拉取，文本编码在实时链路
 * 边上做浮点格式化和字符串分配，既慢又不确定；而且策略侧真正需要的是能直接 memcpy
 * 进结构体数组的数字。人要看的时候走 INFO / LATEST 这两个文本动词——两者分工明确。
 *
 * 为什么头里带 version 和 frame_bytes。布局一旦变更（加字段、改轴数上限），旧客户端
 * 会把新载荷按旧偏移解析，**静默**读出错位的数据——那比连接失败危险得多。带上这两个
 * 字段，客户端可以在第一帧就检出并拒绝，而不是跑出错误的控制量。
 *
 * 全部整数按**小端**显式逐字节读写，绝不 memcpy 结构体：结构体的填充、对齐、
 * 字节序都是编译器的事，把它当线格式等于把 ABI 焊进协议。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emaster/observation/frame.h"

/* 线格式版本。任何字段增删改都必须 +1。 */
#define EMASTER_OBSERVATION_WIRE_VERSION 1U

/* 消息魔数，防止把别的流（例如命令 socket 的文本响应）误当观测帧解析。 */
#define EMASTER_OBSERVATION_WIRE_MAGIC_0 ((uint8_t)'E')
#define EMASTER_OBSERVATION_WIRE_MAGIC_1 ((uint8_t)'O')

/* 消息类型。 */
typedef enum
{
    EMASTER_OBSERVATION_WIRE_KIND_FRAME = 1U
} emaster_observation_wire_kind_t;

/* 头的字节数。 */
#define EMASTER_OBSERVATION_WIRE_HEADER_BYTES 56U

/* 每个轴条目在线上的字节数：4×int32 + 2×uint16 + uint32。 */
#define EMASTER_OBSERVATION_WIRE_AXIS_BYTES 24U

/* 解码后的头。字段布局与线上一致，顺序按对齐排过。 */
typedef struct
{
    uint8_t  version;
    uint8_t  kind;
    uint16_t frame_bytes;
    uint16_t axis_count;
    uint32_t flags;
    int32_t  wkc;
    uint64_t publish_index;
    uint64_t cycle;
    uint64_t monotonic_ns;
    uint64_t deadline_ns;
    uint64_t frame_interval_ns;
} emaster_observation_wire_header_t;

/*
 * 给定轴数时一条 FRAME 消息的字节数（含头）。axis_count 超过上限时按上限算。
 * 服务器用它规划发送缓冲，客户端用它校验 frame_bytes。
 */
size_t emaster_observation_wire_frame_bytes(uint16_t axis_count);

/*
 * 编码一帧。成功返回 true 并写 *written；capacity 不足时返回 false 且不写 *written。
 * frame->axis_count 超过上限时按上限截断（与环形缓冲的发布语义一致）。
 */
bool emaster_observation_wire_encode_frame(const emaster_observation_frame_t *frame,
                                           uint8_t *buffer,
                                           size_t capacity,
                                           size_t *written);

/*
 * 只解头，不碰轴数组。用于 DUMP 一次回多帧时按 frame_bytes 逐帧推进。
 * 任何一个字段不自洽（魔数、版本、类型、轴数超限、frame_bytes 与轴数不匹配、
 * length 不足）都返回 false——**不做部分信任**。
 */
bool emaster_observation_wire_decode_header(const uint8_t *buffer,
                                            size_t length,
                                            emaster_observation_wire_header_t *header);

/*
 * 解一条完整的 FRAME 消息。header 非 NULL 时一并回填（调用者常需要 frame_bytes
 * 才能推进到下一条）。任何畸形输入都返回 false 且不写 *frame。
 */
bool emaster_observation_wire_decode_frame(const uint8_t *buffer,
                                           size_t length,
                                           emaster_observation_frame_t *frame,
                                           emaster_observation_wire_header_t *header);

#endif /* EMASTER_OBSERVATION_WIRE_H */
