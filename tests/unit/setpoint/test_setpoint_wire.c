/*
 * 设点记录的线格式：逐字节编解码与拒收判据。
 *
 * 这一层是主站与外部控制器之间唯一的书面契约。它错的方式都很安静：偏移算错一个字节
 * 之后，解出来的仍是"看起来合理"的目标值（位置计数本来就没有量纲），主站会照它驱
 * 电机。所以这里钉的不是"能不能解"，而是**不自洽的输入一条都不许解出来**。
 *
 * 红了先看这里再改 include/emaster/setpoint/wire.h：偏移表在 wire.c 顶上，两处
 * 一起改。tools/setpoint_send.py 是同一份格式的 Python 实现，也要一起改——
 * tests/checks/setpoint_channel_check.py 会拿同一个用例对两边。
 */

#include <stdio.h>
#include <string.h>

#include "emaster/setpoint/wire.h"

static int failures = 0;

static void check_bool(const char *name, bool actual, bool expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "FAIL %s: expected %s, got %s\n", name, expected ? "true" : "false",
                actual ? "true" : "false");
        ++failures;
    }
}

static void check_u64(const char *name, uint64_t actual, uint64_t expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "FAIL %s: expected %llu, got %llu\n", name,
                (unsigned long long)expected, (unsigned long long)actual);
        ++failures;
    }
}

static void check_i32(const char *name, int32_t actual, int32_t expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "FAIL %s: expected %d, got %d\n", name, (int)expected, (int)actual);
        ++failures;
    }
}

/* 造一条三轴记录，各轴取值互不相同——填成一样的值，偏移写错也看不出来。 */
static void fill_record(emaster_setpoint_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->axis_count = 3U;
    record->sequence = UINT64_C(0x0102030405060708);
    record->client_time_ns = UINT64_C(0x1112131415161718);

    record->axes[0].mode = INT8_C(8);
    record->axes[0].flags = UINT8_C(0x01);
    record->axes[0].q = INT32_C(123456);
    record->axes[0].dq = INT32_C(-2345);
    record->axes[0].tau = INT32_C(678);
    record->axes[0].kp = INT32_C(900000);
    record->axes[0].kd = INT32_C(-1);

    record->axes[1].mode = INT8_C(10);
    record->axes[1].flags = UINT8_C(0x80);
    record->axes[1].q = INT32_C(-2147483647);
    record->axes[1].dq = INT32_C(2147483647);
    record->axes[1].tau = INT32_C(0);
    record->axes[1].kp = INT32_C(-12345);
    record->axes[1].kd = INT32_C(54321);

    record->axes[2].mode = INT8_C(-8);
    record->axes[2].flags = UINT8_C(0);
    record->axes[2].q = INT32_C(7);
    record->axes[2].dq = INT32_C(0);
    record->axes[2].tau = INT32_C(-7);
    record->axes[2].kp = INT32_C(0);
    record->axes[2].kd = INT32_C(7);
}

int main(void)
{
    uint8_t buffer[EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES];
    emaster_setpoint_record_t source;
    size_t written = 0U;

    /* 长度公式：头 24 + 每轴 24，轴数超上限时按上限算。 */
    check_u64("一轴长度", (uint64_t)emaster_setpoint_wire_record_bytes(1U), 48U);
    check_u64("三轴长度", (uint64_t)emaster_setpoint_wire_record_bytes(3U), 96U);
    check_u64("上限轴数长度", (uint64_t)emaster_setpoint_wire_record_bytes(16U),
              (uint64_t)EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES);
    check_u64("超上限按上限算", (uint64_t)emaster_setpoint_wire_record_bytes(99U),
              (uint64_t)EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES);

    /* 往返：编出来再解回去，每个字段都要原样回来。 */
    fill_record(&source);
    check_bool("编码成功", emaster_setpoint_wire_encode(&source, buffer, sizeof(buffer), &written),
               true);
    check_u64("编码长度", (uint64_t)written, 96U);
    {
        emaster_setpoint_record_t decoded;

        check_bool("解码成功", emaster_setpoint_wire_decode(buffer, written, &decoded), true);
        check_u64("轴数", decoded.axis_count, 3U);
        check_u64("序号", decoded.sequence, source.sequence);
        check_u64("客户端时标", decoded.client_time_ns, source.client_time_ns);

        for (size_t axis = 0U; axis < 3U; ++axis)
        {
            char name[64];

            (void)snprintf(name, sizeof(name), "轴%zu 往返 mode", axis + 1U);
            if (decoded.axes[axis].mode != source.axes[axis].mode)
            {
                fprintf(stderr, "FAIL %s: expected %d, got %d\n", name,
                        (int)source.axes[axis].mode, (int)decoded.axes[axis].mode);
                ++failures;
            }
            (void)snprintf(name, sizeof(name), "轴%zu 往返 flags", axis + 1U);
            check_u64(name, (uint64_t)decoded.axes[axis].flags,
                      (uint64_t)source.axes[axis].flags);
            (void)snprintf(name, sizeof(name), "轴%zu 往返 q", axis + 1U);
            check_i32(name, decoded.axes[axis].q, source.axes[axis].q);
            (void)snprintf(name, sizeof(name), "轴%zu 往返 dq", axis + 1U);
            check_i32(name, decoded.axes[axis].dq, source.axes[axis].dq);
            (void)snprintf(name, sizeof(name), "轴%zu 往返 tau", axis + 1U);
            check_i32(name, decoded.axes[axis].tau, source.axes[axis].tau);
            (void)snprintf(name, sizeof(name), "轴%zu 往返 kp", axis + 1U);
            check_i32(name, decoded.axes[axis].kp, source.axes[axis].kp);
            (void)snprintf(name, sizeof(name), "轴%zu 往返 kd", axis + 1U);
            check_i32(name, decoded.axes[axis].kd, source.axes[axis].kd);
        }
    }

    /* 小端是契约的一部分：第一个轴条目里 q 的低字节必须落在偏移 28 上。 */
    check_u64("magic0", (uint64_t)buffer[0], (uint64_t)EMASTER_SETPOINT_WIRE_MAGIC_0);
    check_u64("magic1", (uint64_t)buffer[1], (uint64_t)EMASTER_SETPOINT_WIRE_MAGIC_1);
    check_u64("版本", (uint64_t)buffer[2], (uint64_t)EMASTER_SETPOINT_WIRE_VERSION);
    check_u64("record_bytes 低位在前", (uint64_t)buffer[4], 96U);
    check_u64("record_bytes 高位", (uint64_t)buffer[5], 0U);
    check_u64("axis_count", (uint64_t)buffer[6], 3U);
    check_u64("序号最低字节", (uint64_t)buffer[8], UINT64_C(0x08));
    check_u64("序号最高字节", (uint64_t)buffer[15], UINT64_C(0x01));
    check_u64("首轴 mode", (uint64_t)buffer[24], 8U);
    check_u64("首轴 q 最低字节", (uint64_t)buffer[28], (uint64_t)(123456U & 0xFFU));

    /* 魔数不对：整体拒。 */
    {
        uint8_t bad[sizeof(buffer)];

        memcpy(bad, buffer, sizeof(bad));
        bad[0] = (uint8_t)'X';
        check_bool("魔数不对", emaster_setpoint_wire_decode(bad, written, NULL), false);
    }

    /* 版本不认识：整体拒，不做部分信任。 */
    {
        uint8_t bad[sizeof(buffer)];

        memcpy(bad, buffer, sizeof(bad));
        bad[2] = (uint8_t)(EMASTER_SETPOINT_WIRE_VERSION + 1U);
        check_bool("版本不认识", emaster_setpoint_wire_decode(bad, written, NULL), false);
    }

    /* record_bytes 与轴数不自洽：拒。 */
    {
        uint8_t bad[sizeof(buffer)];

        memcpy(bad, buffer, sizeof(bad));
        bad[4] = 95U; /* 声称 95，轴数仍是 3（应为 96） */
        check_bool("长度与轴数不自洽", emaster_setpoint_wire_decode(bad, written, NULL), false);
    }

    /* 实际收到的字节数不足 record_bytes：拒，而不是解出一半。 */
    check_bool("字节数不足", emaster_setpoint_wire_decode(buffer, written - 1U, NULL), false);

    /* 轴数为 0 或超上限：拒。上限之上不是"截断着用"，是格式不认识。 */
    {
        uint8_t bad[sizeof(buffer)];

        memcpy(bad, buffer, sizeof(bad));
        bad[6] = 0U;
        check_bool("轴数为 0", emaster_setpoint_wire_decode(bad, written, NULL), false);

        memcpy(bad, buffer, sizeof(bad));
        bad[6] = (uint8_t)(EMASTER_SETPOINT_WIRE_MAX_AXES + 1U);
        check_bool("轴数超上限", emaster_setpoint_wire_decode(bad, written, NULL), false);
    }

    /* 头都不够长：拒，不读越界。 */
    check_bool("字节数为 0", emaster_setpoint_wire_decode(buffer, 0U, NULL), false);
    check_bool("只有半条头", emaster_setpoint_wire_decode(buffer, 12U, NULL), false);

    /* 容量不足时编码失败，且不写出长度——调用方不能拿到一个"写成功"的假象。 */
    {
        size_t refused = 999U;

        check_bool("容量不足", emaster_setpoint_wire_encode(&source, buffer, 95U, &refused),
                   false);
        check_u64("容量不足不改长度", (uint64_t)refused, 999U);
    }

    /* 轴数为 0 的记录编不出来：空记录不是合法输入，是调用方搞错了。 */
    {
        emaster_setpoint_record_t empty;

        memset(&empty, 0, sizeof(empty));
        check_bool("零轴记录", emaster_setpoint_wire_encode(&empty, buffer, sizeof(buffer), NULL),
                   false);
    }

    /* 上限轴数也要能编能解，且长度正好等于接收缓冲减一（见 server.h 的截断判据）。 */
    {
        emaster_setpoint_record_t full;
        emaster_setpoint_record_t decoded;

        memset(&full, 0, sizeof(full));
        full.axis_count = (uint16_t)EMASTER_SETPOINT_WIRE_MAX_AXES;
        for (size_t axis = 0U; axis < EMASTER_SETPOINT_WIRE_MAX_AXES; ++axis)
        {
            full.axes[axis].mode = INT8_C(8);
            full.axes[axis].q = (int32_t)(axis * 1000U);
        }
        check_bool("满轴编码", emaster_setpoint_wire_encode(&full, buffer, sizeof(buffer),
                                                            &written), true);
        check_u64("满轴长度", (uint64_t)written, (uint64_t)EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES);
        check_bool("满轴解码", emaster_setpoint_wire_decode(buffer, written, &decoded), true);
        check_i32("满轴末轴 q", decoded.axes[EMASTER_SETPOINT_WIRE_MAX_AXES - 1U].q,
                  (int32_t)((EMASTER_SETPOINT_WIRE_MAX_AXES - 1U) * 1000U));
    }

    /* 拒收原因的文本：四个取值各有一句，且 NONE 与非法值都不是 NULL。 */
    check_bool("TRUNCATED 有文本",
               emaster_setpoint_reject_text(EMASTER_SETPOINT_REJECT_TRUNCATED) != NULL, true);
    check_bool("BAD_FORMAT 有文本",
               emaster_setpoint_reject_text(EMASTER_SETPOINT_REJECT_BAD_FORMAT) != NULL, true);
    check_bool("AXIS_COUNT 有文本",
               emaster_setpoint_reject_text(EMASTER_SETPOINT_REJECT_AXIS_COUNT) != NULL, true);
    check_bool("MODE_MISMATCH 有文本",
               emaster_setpoint_reject_text(EMASTER_SETPOINT_REJECT_MODE_MISMATCH) != NULL, true);
    check_bool("NONE 有文本", emaster_setpoint_reject_text(EMASTER_SETPOINT_REJECT_NONE) != NULL,
               true);
    check_bool("非法取值也有文本",
               emaster_setpoint_reject_text((emaster_setpoint_reject_t)200) != NULL, true);

    if (failures != 0)
    {
        fprintf(stderr, "setpoint_wire: %d 项不符\n", failures);
        return 1;
    }
    printf("setpoint_wire: 全部通过\n");
    return 0;
}
