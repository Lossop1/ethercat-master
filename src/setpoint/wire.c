#include "emaster/setpoint/wire.h"

/*
 * 头字段偏移，载荷之前固定 24 字节：
 *
 *   0   2  魔数 'E','S'
 *   2   1  版本
 *   3   1  保留（0）
 *   4   2  record_bytes（含头，整条记录的长度）
 *   6   2  axis_count
 *   8   8  sequence
 *  16   8  client_time_ns
 *
 * 每轴条目 24 字节：
 *
 *   0   1  mode（有符号）
 *   1   1  flags
 *   2   2  保留（0）
 *   4   4  q
 *   8   4  dq
 *  12   4  tau
 *  16   4  kp
 *  20   4  kd
 */
#define REC_OFF_MAGIC          0U
#define REC_OFF_VERSION        2U
#define REC_OFF_RESERVED       3U
#define REC_OFF_RECORD_BYTES   4U
#define REC_OFF_AXIS_COUNT     6U
#define REC_OFF_SEQUENCE       8U
#define REC_OFF_CLIENT_TIME_NS 16U

#define AX_OFF_MODE   0U
#define AX_OFF_FLAGS  1U
#define AX_OFF_Q      4U
#define AX_OFF_DQ     8U
#define AX_OFF_TAU   12U
#define AX_OFF_KP    16U
#define AX_OFF_KD    20U

/* 拒收原因的可读文本。服务端的 stderr 与报告里的那一列都调这里，两处说法必须一致。 */
const char *emaster_setpoint_reject_text(emaster_setpoint_reject_t reason)
{
    switch (reason)
    {
    case EMASTER_SETPOINT_REJECT_NONE:
        return "未被拒";
    case EMASTER_SETPOINT_REJECT_TRUNCATED:
        return "记录超长（超过接收缓冲，已被内核截断）";
    case EMASTER_SETPOINT_REJECT_BAD_FORMAT:
        return "格式不认识（魔数 / 版本 / 长度与轴数不自洽）";
    case EMASTER_SETPOINT_REJECT_AXIS_COUNT:
        return "轴数与本部署不符";
    case EMASTER_SETPOINT_REJECT_MODE_MISMATCH:
        return "某轴的运行模式与本部署不符";
    default:
        /* 走到这里说明报告的取值比这个枚举新：如实说出认不出，不要假装是别的。 */
        return "认不出的拒收原因";
    }
}

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

size_t emaster_setpoint_wire_record_bytes(uint16_t axis_count)
{
    if (axis_count > (uint16_t)EMASTER_SETPOINT_WIRE_MAX_AXES)
    {
        axis_count = (uint16_t)EMASTER_SETPOINT_WIRE_MAX_AXES;
    }
    return (size_t)EMASTER_SETPOINT_WIRE_HEADER_BYTES +
           ((size_t)axis_count * (size_t)EMASTER_SETPOINT_WIRE_AXIS_BYTES);
}

bool emaster_setpoint_wire_decode(const uint8_t *buffer,
                                  size_t length,
                                  emaster_setpoint_record_t *record)
{
    uint16_t axis_count;
    uint16_t record_bytes;
    size_t offset;
    uint16_t axis_index;

    if (buffer == NULL || length < (size_t)EMASTER_SETPOINT_WIRE_HEADER_BYTES)
    {
        return false;
    }
    if (buffer[REC_OFF_MAGIC] != EMASTER_SETPOINT_WIRE_MAGIC_0 ||
        buffer[REC_OFF_MAGIC + 1U] != EMASTER_SETPOINT_WIRE_MAGIC_1)
    {
        return false;
    }
    if (buffer[REC_OFF_VERSION] != (uint8_t)EMASTER_SETPOINT_WIRE_VERSION)
    {
        /* 版本不认识就整体拒。按旧偏移硬解会得到错位但"看起来合理"的目标值。 */
        return false;
    }

    axis_count = wire_load_u16(buffer, REC_OFF_AXIS_COUNT);
    if (axis_count == 0U || axis_count > (uint16_t)EMASTER_SETPOINT_WIRE_MAX_AXES)
    {
        return false;
    }
    record_bytes = wire_load_u16(buffer, REC_OFF_RECORD_BYTES);
    if ((size_t)record_bytes != emaster_setpoint_wire_record_bytes(axis_count))
    {
        /* 长度必须与轴数自洽，不单独相信 record_bytes。 */
        return false;
    }
    if (length < (size_t)record_bytes)
    {
        return false;
    }

    if (record != NULL)
    {
        record->version = buffer[REC_OFF_VERSION];
        record->record_bytes = record_bytes;
        record->axis_count = axis_count;
        record->sequence = wire_load_u64(buffer, REC_OFF_SEQUENCE);
        record->client_time_ns = wire_load_u64(buffer, REC_OFF_CLIENT_TIME_NS);

        offset = (size_t)EMASTER_SETPOINT_WIRE_HEADER_BYTES;
        for (axis_index = 0U; axis_index < axis_count; ++axis_index)
        {
            emaster_setpoint_axis_t *axis = &record->axes[axis_index];

            axis->mode = (int8_t)buffer[offset + AX_OFF_MODE];
            axis->flags = buffer[offset + AX_OFF_FLAGS];
            axis->q = (int32_t)wire_load_u32(buffer, offset + AX_OFF_Q);
            axis->dq = (int32_t)wire_load_u32(buffer, offset + AX_OFF_DQ);
            axis->tau = (int32_t)wire_load_u32(buffer, offset + AX_OFF_TAU);
            axis->kp = (int32_t)wire_load_u32(buffer, offset + AX_OFF_KP);
            axis->kd = (int32_t)wire_load_u32(buffer, offset + AX_OFF_KD);
            offset += (size_t)EMASTER_SETPOINT_WIRE_AXIS_BYTES;
        }
    }
    return true;
}

bool emaster_setpoint_wire_encode(const emaster_setpoint_record_t *record,
                                  uint8_t *buffer,
                                  size_t capacity,
                                  size_t *written)
{
    uint16_t axis_count;
    size_t total;
    size_t offset;
    uint16_t axis_index;

    if (record == NULL || buffer == NULL)
    {
        return false;
    }
    axis_count = record->axis_count;
    if (axis_count == 0U || axis_count > (uint16_t)EMASTER_SETPOINT_WIRE_MAX_AXES)
    {
        return false;
    }
    total = emaster_setpoint_wire_record_bytes(axis_count);
    if (capacity < total)
    {
        return false;
    }

    buffer[REC_OFF_MAGIC] = EMASTER_SETPOINT_WIRE_MAGIC_0;
    buffer[REC_OFF_MAGIC + 1U] = EMASTER_SETPOINT_WIRE_MAGIC_1;
    buffer[REC_OFF_VERSION] = (uint8_t)EMASTER_SETPOINT_WIRE_VERSION;
    buffer[REC_OFF_RESERVED] = 0U;
    wire_store_u16(buffer, REC_OFF_RECORD_BYTES, (uint16_t)total);
    wire_store_u16(buffer, REC_OFF_AXIS_COUNT, axis_count);
    wire_store_u64(buffer, REC_OFF_SEQUENCE, record->sequence);
    wire_store_u64(buffer, REC_OFF_CLIENT_TIME_NS, record->client_time_ns);

    offset = (size_t)EMASTER_SETPOINT_WIRE_HEADER_BYTES;
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        const emaster_setpoint_axis_t *axis = &record->axes[axis_index];

        buffer[offset + AX_OFF_MODE] = (uint8_t)axis->mode;
        buffer[offset + AX_OFF_FLAGS] = axis->flags;
        buffer[offset + 2U] = 0U;
        buffer[offset + 3U] = 0U;
        wire_store_u32(buffer, offset + AX_OFF_Q, (uint32_t)axis->q);
        wire_store_u32(buffer, offset + AX_OFF_DQ, (uint32_t)axis->dq);
        wire_store_u32(buffer, offset + AX_OFF_TAU, (uint32_t)axis->tau);
        wire_store_u32(buffer, offset + AX_OFF_KP, (uint32_t)axis->kp);
        wire_store_u32(buffer, offset + AX_OFF_KD, (uint32_t)axis->kd);
        offset += (size_t)EMASTER_SETPOINT_WIRE_AXIS_BYTES;
    }

    if (written != NULL)
    {
        *written = total;
    }
    return true;
}
