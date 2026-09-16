#include "emaster/observation/wire.h"

/*
 * 头字段偏移。载荷之前固定 56 字节：
 *
 *   0   2  魔数 'E','O'
 *   2   1  版本
 *   3   1  类型
 *   4   2  frame_bytes（含头，整条消息的长度）
 *   6   2  axis_count
 *   8   4  flags
 *  12   4  wkc（有符号，按补码存）
 *  16   8  publish_index
 *  24   8  cycle
 *  32   8  monotonic_ns
 *  40   8  deadline_ns
 *  48   8  frame_interval_ns
 */
#define WIRE_OFF_MAGIC          0U
#define WIRE_OFF_VERSION        2U
#define WIRE_OFF_KIND           3U
#define WIRE_OFF_FRAME_BYTES    4U
#define WIRE_OFF_AXIS_COUNT     6U
#define WIRE_OFF_FLAGS          8U
#define WIRE_OFF_WKC           12U
#define WIRE_OFF_PUBLISH_INDEX 16U
#define WIRE_OFF_CYCLE         24U
#define WIRE_OFF_MONOTONIC_NS  32U
#define WIRE_OFF_DEADLINE_NS   40U
#define WIRE_OFF_INTERVAL_NS   48U

/* 轴条目内部的偏移（相对条目起点）。 */
#define AXIS_OFF_ACTUAL_POSITION 0U
#define AXIS_OFF_TARGET_POSITION 4U
#define AXIS_OFF_ACTUAL_VELOCITY 8U
#define AXIS_OFF_ACTUAL_TORQUE  12U
#define AXIS_OFF_STATUS_WORD    16U
#define AXIS_OFF_CONTROL_WORD   18U
#define AXIS_OFF_FLAGS          20U

static void wire_store_u16(uint8_t *base, size_t offset, uint16_t value)
{
    base[offset] = (uint8_t)(value & UINT16_C(0x00FF));
    base[offset + 1U] = (uint8_t)((value >> 8U) & UINT16_C(0x00FF));
}

static void wire_store_u32(uint8_t *base, size_t offset, uint32_t value)
{
    base[offset] = (uint8_t)(value & UINT32_C(0xFF));
    base[offset + 1U] = (uint8_t)((value >> 8U) & UINT32_C(0xFF));
    base[offset + 2U] = (uint8_t)((value >> 16U) & UINT32_C(0xFF));
    base[offset + 3U] = (uint8_t)((value >> 24U) & UINT32_C(0xFF));
}

static void wire_store_u64(uint8_t *base, size_t offset, uint64_t value)
{
    wire_store_u32(base, offset, (uint32_t)(value & UINT64_C(0xFFFFFFFF)));
    wire_store_u32(base, offset + 4U, (uint32_t)((value >> 32U) & UINT64_C(0xFFFFFFFF)));
}

static uint16_t wire_load_u16(const uint8_t *base, size_t offset)
{
    return (uint16_t)(((uint16_t)base[offset]) |
                      (uint16_t)(((uint16_t)base[offset + 1U]) << 8U));
}

static uint32_t wire_load_u32(const uint8_t *base, size_t offset)
{
    return ((uint32_t)base[offset]) | (((uint32_t)base[offset + 1U]) << 8U) |
           (((uint32_t)base[offset + 2U]) << 16U) | (((uint32_t)base[offset + 3U]) << 24U);
}

static uint64_t wire_load_u64(const uint8_t *base, size_t offset)
{
    return ((uint64_t)wire_load_u32(base, offset)) |
           (((uint64_t)wire_load_u32(base, offset + 4U)) << 32U);
}

size_t emaster_observation_wire_frame_bytes(uint16_t axis_count)
{
    if (axis_count > (uint16_t)EMASTER_OBSERVATION_MAX_AXES)
    {
        axis_count = (uint16_t)EMASTER_OBSERVATION_MAX_AXES;
    }
    return (size_t)EMASTER_OBSERVATION_WIRE_HEADER_BYTES +
           ((size_t)axis_count * (size_t)EMASTER_OBSERVATION_WIRE_AXIS_BYTES);
}

bool emaster_observation_wire_encode_frame(const emaster_observation_frame_t *frame,
                                           uint8_t *buffer,
                                           size_t capacity,
                                           size_t *written)
{
    uint16_t axis_count;
    size_t total;
    size_t offset;
    uint16_t axis_index;

    if (frame == NULL || buffer == NULL)
    {
        return false;
    }

    axis_count = frame->axis_count;
    if (axis_count > (uint16_t)EMASTER_OBSERVATION_MAX_AXES)
    {
        axis_count = (uint16_t)EMASTER_OBSERVATION_MAX_AXES;
    }
    total = emaster_observation_wire_frame_bytes(axis_count);
    if (capacity < total)
    {
        return false;
    }

    buffer[WIRE_OFF_MAGIC] = EMASTER_OBSERVATION_WIRE_MAGIC_0;
    buffer[WIRE_OFF_MAGIC + 1U] = EMASTER_OBSERVATION_WIRE_MAGIC_1;
    buffer[WIRE_OFF_VERSION] = (uint8_t)EMASTER_OBSERVATION_WIRE_VERSION;
    buffer[WIRE_OFF_KIND] = (uint8_t)EMASTER_OBSERVATION_WIRE_KIND_FRAME;
    wire_store_u16(buffer, WIRE_OFF_FRAME_BYTES, (uint16_t)total);
    wire_store_u16(buffer, WIRE_OFF_AXIS_COUNT, axis_count);
    wire_store_u32(buffer, WIRE_OFF_FLAGS, frame->flags);
    wire_store_u32(buffer, WIRE_OFF_WKC, (uint32_t)frame->wkc);
    wire_store_u64(buffer, WIRE_OFF_PUBLISH_INDEX, frame->publish_index);
    wire_store_u64(buffer, WIRE_OFF_CYCLE, frame->cycle);
    wire_store_u64(buffer, WIRE_OFF_MONOTONIC_NS, frame->monotonic_ns);
    wire_store_u64(buffer, WIRE_OFF_DEADLINE_NS, frame->deadline_ns);
    wire_store_u64(buffer, WIRE_OFF_INTERVAL_NS, frame->frame_interval_ns);

    offset = (size_t)EMASTER_OBSERVATION_WIRE_HEADER_BYTES;
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        const emaster_observation_axis_t *axis = &frame->axes[axis_index];

        wire_store_u32(buffer, offset + AXIS_OFF_ACTUAL_POSITION, (uint32_t)axis->actual_position);
        wire_store_u32(buffer, offset + AXIS_OFF_TARGET_POSITION, (uint32_t)axis->target_position);
        wire_store_u32(buffer, offset + AXIS_OFF_ACTUAL_VELOCITY, (uint32_t)axis->actual_velocity);
        wire_store_u32(buffer, offset + AXIS_OFF_ACTUAL_TORQUE, (uint32_t)axis->actual_torque);
        wire_store_u16(buffer, offset + AXIS_OFF_STATUS_WORD, axis->status_word);
        wire_store_u16(buffer, offset + AXIS_OFF_CONTROL_WORD, axis->control_word);
        wire_store_u32(buffer, offset + AXIS_OFF_FLAGS, axis->flags);
        offset += (size_t)EMASTER_OBSERVATION_WIRE_AXIS_BYTES;
    }

    if (written != NULL)
    {
        *written = total;
    }
    return true;
}

bool emaster_observation_wire_decode_header(const uint8_t *buffer,
                                            size_t length,
                                            emaster_observation_wire_header_t *header)
{
    uint16_t axis_count;
    uint16_t frame_bytes;
    uint8_t kind;
    uint8_t version;

    if (buffer == NULL || length < (size_t)EMASTER_OBSERVATION_WIRE_HEADER_BYTES)
    {
        return false;
    }
    if (buffer[WIRE_OFF_MAGIC] != EMASTER_OBSERVATION_WIRE_MAGIC_0 ||
        buffer[WIRE_OFF_MAGIC + 1U] != EMASTER_OBSERVATION_WIRE_MAGIC_1)
    {
        return false;
    }

    version = buffer[WIRE_OFF_VERSION];
    if (version != (uint8_t)EMASTER_OBSERVATION_WIRE_VERSION)
    {
        /* 版本不认识就整体拒绝。按旧偏移硬解会得到错位但"看起来合理"的数字。 */
        return false;
    }
    kind = buffer[WIRE_OFF_KIND];
    if (kind != (uint8_t)EMASTER_OBSERVATION_WIRE_KIND_FRAME)
    {
        return false;
    }

    axis_count = wire_load_u16(buffer, WIRE_OFF_AXIS_COUNT);
    if (axis_count > (uint16_t)EMASTER_OBSERVATION_MAX_AXES)
    {
        return false;
    }
    frame_bytes = wire_load_u16(buffer, WIRE_OFF_FRAME_BYTES);
    if ((size_t)frame_bytes != emaster_observation_wire_frame_bytes(axis_count))
    {
        /* 长度必须与轴数自洽。不信任 frame_bytes 单独给出的值。 */
        return false;
    }

    if (header != NULL)
    {
        header->version = version;
        header->kind = kind;
        header->frame_bytes = frame_bytes;
        header->axis_count = axis_count;
        header->flags = wire_load_u32(buffer, WIRE_OFF_FLAGS);
        header->wkc = (int32_t)wire_load_u32(buffer, WIRE_OFF_WKC);
        header->publish_index = wire_load_u64(buffer, WIRE_OFF_PUBLISH_INDEX);
        header->cycle = wire_load_u64(buffer, WIRE_OFF_CYCLE);
        header->monotonic_ns = wire_load_u64(buffer, WIRE_OFF_MONOTONIC_NS);
        header->deadline_ns = wire_load_u64(buffer, WIRE_OFF_DEADLINE_NS);
        header->frame_interval_ns = wire_load_u64(buffer, WIRE_OFF_INTERVAL_NS);
    }
    return true;
}

bool emaster_observation_wire_decode_frame(const uint8_t *buffer,
                                           size_t length,
                                           emaster_observation_frame_t *frame,
                                           emaster_observation_wire_header_t *header)
{
    emaster_observation_wire_header_t local;
    emaster_observation_frame_t decoded;
    size_t offset;
    uint16_t axis_index;

    if (frame == NULL)
    {
        return false;
    }
    if (!emaster_observation_wire_decode_header(buffer, length, &local))
    {
        return false;
    }
    if (length < (size_t)local.frame_bytes)
    {
        return false;
    }

    emaster_observation_frame_clear(&decoded, local.axis_count);
    decoded.publish_index = local.publish_index;
    decoded.cycle = local.cycle;
    decoded.monotonic_ns = local.monotonic_ns;
    decoded.deadline_ns = local.deadline_ns;
    decoded.frame_interval_ns = local.frame_interval_ns;
    decoded.wkc = local.wkc;
    decoded.flags = local.flags;

    offset = (size_t)EMASTER_OBSERVATION_WIRE_HEADER_BYTES;
    for (axis_index = 0U; axis_index < local.axis_count; ++axis_index)
    {
        emaster_observation_axis_t *axis = &decoded.axes[axis_index];

        axis->actual_position = (int32_t)wire_load_u32(buffer, offset + AXIS_OFF_ACTUAL_POSITION);
        axis->target_position = (int32_t)wire_load_u32(buffer, offset + AXIS_OFF_TARGET_POSITION);
        axis->actual_velocity = (int32_t)wire_load_u32(buffer, offset + AXIS_OFF_ACTUAL_VELOCITY);
        axis->actual_torque = (int32_t)wire_load_u32(buffer, offset + AXIS_OFF_ACTUAL_TORQUE);
        axis->status_word = wire_load_u16(buffer, offset + AXIS_OFF_STATUS_WORD);
        axis->control_word = wire_load_u16(buffer, offset + AXIS_OFF_CONTROL_WORD);
        axis->flags = wire_load_u32(buffer, offset + AXIS_OFF_FLAGS);
        offset += (size_t)EMASTER_OBSERVATION_WIRE_AXIS_BYTES;
    }

    *frame = decoded;
    if (header != NULL)
    {
        *header = local;
    }
    return true;
}
