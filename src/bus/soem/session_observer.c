#include "session_observer.h"

#include <limits.h>
#include <string.h>

enum
{
    /* 仅用于审计容量上界，不是 EtherCAT 或运动参数。 */
    EMASTER_AUDIT_EXCHANGE_MARGIN = 8,
    /*
     * 每个轴留给"解封后的停机阶段"的条数。周期阶段用不到这块（水位封在它前面）。
     * 取 128 而不是 32：一轮停机要记两次全轴 AL 读取 + 每轴若干条 SDO 诊断读写，
     * 5 轴就是上百条；以前这块被周期阶段吃光，解封后第一条记录就触发扩容。
     * 一轴 128 条 × 88 字节 ≈ 11 KB，代价可以忽略。
     */
    EMASTER_AUDIT_FINAL_ACCESS_MARGIN_PER_AXIS = 128
};

/* 在设备声明里按语义查找一条遥测对象；设备没有声明就返回 NULL，调用者据此跳过。 */
static const emaster_slow_telemetry_t *observer_find_telemetry(
    const emaster_slave_profile_t *profile, emaster_telemetry_semantic_t semantic)
{
    if (profile == NULL || profile->slow_telemetry == NULL)
    {
        return NULL;
    }
    for (size_t index = 0U; index < profile->slow_telemetry_count; ++index)
    {
        if (profile->slow_telemetry[index].semantic == semantic)
        {
            return &profile->slow_telemetry[index];
        }
    }
    return NULL;
}

/* 按条目声明的宽度读取一个值到 32 位诊断槽位；宽度和符号由配置声明，不在代码里写死。 */
static bool observer_read_telemetry_u32(emaster_soem_sdo_reader_context_t *reader,
                                        const emaster_slow_telemetry_t *entry, uint32_t *value)
{
    uint8_t u8_value = 0U;
    int8_t i8_value = 0;
    uint16_t u16_value = 0U;
    int16_t i16_value = 0;
    int32_t i32_value = 0;

    switch (entry->type)
    {
        case EMASTER_TELEMETRY_TYPE_U8:
            if (!emaster_soem_read_u8(reader, entry->index, entry->subindex, &u8_value))
            {
                return false;
            }
            *value = u8_value;
            return true;
        case EMASTER_TELEMETRY_TYPE_I8:
            if (!emaster_soem_read_i8(reader, entry->index, entry->subindex, &i8_value))
            {
                return false;
            }
            *value = (uint32_t)i8_value;
            return true;
        case EMASTER_TELEMETRY_TYPE_U16:
            if (!emaster_soem_read_u16(reader, entry->index, entry->subindex, &u16_value))
            {
                return false;
            }
            *value = u16_value;
            return true;
        case EMASTER_TELEMETRY_TYPE_I16:
            if (!emaster_soem_read_i16(reader, entry->index, entry->subindex, &i16_value))
            {
                return false;
            }
            *value = (uint32_t)i16_value;
            return true;
        case EMASTER_TELEMETRY_TYPE_U32:
            return emaster_soem_read_u32(reader, entry->index, entry->subindex, value);
        case EMASTER_TELEMETRY_TYPE_I32:
            if (!emaster_soem_read_i32(reader, entry->index, entry->subindex, &i32_value))
            {
                return false;
            }
            *value = (uint32_t)i32_value;
            return true;
    }
    return false;
}

/*
 * 读取驱动器诊断对象的停机快照。603F 是 CiA 402 标准错误码对象、1001 是标准对象字典里的
 * 错误寄存器，仍然写在这里；203E/203F 是供应商私有语义，只能按设备配置声明的 semantic 查找：
 * 设备没有声明就不读，也不计入失败，否则"没有这两个对象的驱动器"会被记成诊断读失败。
 * 读取顺序沿用此前的短路链：前一条失败就不再发起后续读。
 */
void emaster_session_observer_read_drive(
    emaster_soem_sdo_reader_context_t *reader,
    const emaster_slave_profile_t *profile,
    emaster_drive_diagnostic_t *diagnostic)
{
    const emaster_slow_telemetry_t *entry;
    bool succeeded;

    if (reader == NULL || diagnostic == NULL)
    {
        return;
    }
    succeeded = emaster_soem_read_u16(reader, UINT16_C(0x603F), UINT8_C(0),
                                      &diagnostic->cia402_error_code) &&
                emaster_soem_read_u8(reader, UINT16_C(0x1001), UINT8_C(0),
                                     &diagnostic->error_register);
    entry = succeeded ? observer_find_telemetry(profile, EMASTER_TELEMETRY_EXTENDED_ERROR_CODE)
                      : NULL;
    if (entry != NULL)
    {
        succeeded = observer_read_telemetry_u32(reader, entry,
                                               &diagnostic->extended_servo_error_code);
    }
    entry = succeeded ? observer_find_telemetry(profile, EMASTER_TELEMETRY_SERVO_ERROR_CODE)
                      : NULL;
    if (entry != NULL)
    {
        succeeded = observer_read_telemetry_u32(reader, entry, &diagnostic->servo_error_code);
    }
    diagnostic->read_succeeded = succeeded;
}

void emaster_session_observer_read_sync(
    emaster_soem_sdo_reader_context_t *reader,
    uint16_t index,
    emaster_sync_diagnostic_t *diagnostic)
{
    uint8_t sync_error = 0U;

    if (reader == NULL || diagnostic == NULL)
    {
        return;
    }
    diagnostic->read_succeeded =
        emaster_soem_read_u16(reader, index, UINT8_C(11),
                              &diagnostic->sm_event_missed) &&
        emaster_soem_read_u16(reader, index, UINT8_C(12),
                              &diagnostic->cycle_time_too_small) &&
        emaster_soem_read_u16(reader, index, UINT8_C(13),
                              &diagnostic->shift_time_too_short) &&
        emaster_soem_read_u8(reader, index, UINT8_C(32), &sync_error);
    diagnostic->sync_error = sync_error != 0U;
}

void emaster_session_observer_read_position_scale(
    emaster_soem_sdo_reader_context_t *reader,
    emaster_position_scale_t *scale)
{
    if (reader == NULL || scale == NULL)
    {
        return;
    }
    scale->read_succeeded =
        emaster_soem_read_u32(reader, UINT16_C(0x608F), UINT8_C(1),
                              &scale->encoder_increments) &&
        emaster_soem_read_u32(reader, UINT16_C(0x608F), UINT8_C(2),
                              &scale->encoder_motor_revolutions) &&
        emaster_soem_read_u32(reader, UINT16_C(0x6091), UINT8_C(1),
                              &scale->gear_motor_revolutions) &&
        emaster_soem_read_u32(reader, UINT16_C(0x6091), UINT8_C(2),
                              &scale->gear_shaft_revolutions);
}

bool emaster_session_observer_read_configured_sdo(
    emaster_soem_sdo_reader_context_t *reader,
    const emaster_sdo_read_config_t *command)
{
    uint8_t u8_value;
    int8_t i8_value;
    uint16_t u16_value;
    int16_t i16_value;
    uint32_t u32_value;
    int32_t i32_value;

    if (reader == NULL || command == NULL)
    {
        return false;
    }
    switch (command->type)
    {
        case EMASTER_CONFIG_SDO_VALUE_U8:
            return emaster_soem_read_u8(reader, command->index,
                                        command->subindex, &u8_value);
        case EMASTER_CONFIG_SDO_VALUE_I8:
            return emaster_soem_read_i8(reader, command->index,
                                        command->subindex, &i8_value);
        case EMASTER_CONFIG_SDO_VALUE_U16:
            return emaster_soem_read_u16(reader, command->index,
                                         command->subindex, &u16_value);
        case EMASTER_CONFIG_SDO_VALUE_I16:
            return emaster_soem_read_i16(reader, command->index,
                                         command->subindex, &i16_value);
        case EMASTER_CONFIG_SDO_VALUE_U32:
            return emaster_soem_read_u32(reader, command->index,
                                         command->subindex, &u32_value);
        case EMASTER_CONFIG_SDO_VALUE_I32:
            return emaster_soem_read_i32(reader, command->index,
                                         command->subindex, &i32_value);
    }
    return false;
}

bool emaster_session_observer_read_dc_register(
    ecx_contextt *context,
    uint16_t slave,
    uint16_t address,
    void *value,
    uint16_t size,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    uint64_t exchange)
{
    bool succeeded;
    uint64_t decoded = 0U;

    if (context == NULL || value == NULL || slave == 0U)
    {
        return false;
    }
    memset(value, 0, size);
    succeeded = ecx_FPRD(&context->port, context->slavelist[slave].configadr,
                         address, size, value, EC_TIMEOUTRET) > 0;
    if (succeeded && size == sizeof(uint16_t))
    {
        uint16_t raw;
        memcpy(&raw, value, sizeof(raw));
        decoded = etohs(raw);
    }
    else if (succeeded && size == sizeof(uint32_t))
    {
        uint32_t raw;
        memcpy(&raw, value, sizeof(raw));
        decoded = etohl(raw);
    }
    if (audit != NULL)
    {
        (void)emaster_run_audit_record_access(
            audit, phase, EMASTER_AUDIT_TRANSPORT_ESC_REGISTER,
            EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_UNSIGNED, slave,
            address, UINT8_C(0), (uint8_t)(size * UINT16_C(8)), 0U, exchange,
            value, succeeded ? (uint8_t)size : UINT8_C(0), succeeded, decoded,
            (int64_t)decoded);
    }
    return succeeded;
}

bool emaster_session_observer_prepare_audit(
    const emaster_session_plan_t *plan,
    const emaster_cia_process_image_t *runtime,
    uint64_t transition_cycles,
    emaster_run_audit_t *audit)
{
    uint64_t motion_time_ns;
    uint64_t motion_cycles;
    uint64_t max_exchanges;
    size_t fields_per_exchange = 0U;
    size_t capacity;
    size_t cyclic_capacity;
    size_t final_margin;
    size_t axis_index;

    if (plan == NULL || runtime == NULL || audit == NULL || plan->cycle_ns == 0U)
    {
        return false;
    }
    /* 保持位置会话没有结束时刻，预留启动和状态转换容量；耗尽后只标注截断。 */
    motion_time_ns = plan->motion_profile == NULL ? UINT64_C(0) :
        (((uint64_t)plan->motion_profile->duration_ms +
          (uint64_t)plan->motion_profile->settle_ms) > UINT64_MAX / UINT64_C(1000000)
             ? UINT64_MAX
             : ((uint64_t)plan->motion_profile->duration_ms +
                (uint64_t)plan->motion_profile->settle_ms) * UINT64_C(1000000));
    motion_cycles =
        (motion_time_ns + plan->cycle_ns - UINT64_C(1)) / plan->cycle_ns;
    if (transition_cycles > UINT64_MAX / UINT64_C(4) ||
        motion_cycles > UINT64_MAX - transition_cycles * UINT64_C(4) ||
        (uint64_t)plan->dc_startup_cycles * UINT64_C(2) >
            UINT64_MAX - motion_cycles - transition_cycles * UINT64_C(4) -
                EMASTER_AUDIT_EXCHANGE_MARGIN)
    {
        return false;
    }
    max_exchanges = motion_cycles + transition_cycles * UINT64_C(4) +
                    (uint64_t)plan->dc_startup_cycles * UINT64_C(2) +
                    EMASTER_AUDIT_EXCHANGE_MARGIN;
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        size_t axis_fields = runtime[axis_index].rx_field_count +
                             runtime[axis_index].tx_field_count;
        if (axis_fields < runtime[axis_index].rx_field_count ||
            fields_per_exchange > SIZE_MAX - axis_fields)
        {
            return false;
        }
        fields_per_exchange += axis_fields;
    }
    if (max_exchanges > SIZE_MAX ||
        (fields_per_exchange > 0U &&
         (size_t)max_exchanges > SIZE_MAX / fields_per_exchange))
    {
        return false;
    }
    /*
     * 容量分两段算，一次预留：
     *   cyclic_capacity —— 周期阶段能用到哪（含此前已经记下的启动/转换阶段记录）；
     *   final_margin    —— 留给解封后的停机诊断，周期阶段吃不到它。
     * 分开的理由是 P8.5：两者合起来算的话，周期阶段会把预留吃光，停机解封后第一条
     * 记录就得扩容——实测 84.6 MB 的块翻倍让内核花 3.757 ms，正好落在周期尾部，
     * 下一帧晚发 4 ms，五轴全掉出 OP。
     */
    cyclic_capacity = (size_t)max_exchanges * fields_per_exchange;
    final_margin = plan->axis_count * EMASTER_AUDIT_FINAL_ACCESS_MARGIN_PER_AXIS;
    if (plan->axis_count >
            (SIZE_MAX - cyclic_capacity) / EMASTER_AUDIT_FINAL_ACCESS_MARGIN_PER_AXIS ||
        audit->access_count > SIZE_MAX - cyclic_capacity - final_margin)
    {
        return false;
    }
    cyclic_capacity += audit->access_count;
    capacity = cyclic_capacity + final_margin;
    /* 上限只压低这里算出来的容量，不改变"谁封存"——封存仍然只发生在下面这一处。 */
    capacity = emaster_run_audit_clamp_capacity(audit, capacity);
    if (!emaster_run_audit_reserve(audit, capacity))
    {
        return false;
    }
    /* 水线同样受上限与预留实际容量约束（clamp 可能把总量压到水线以下）。 */
    emaster_run_audit_seal_cyclic(audit, cyclic_capacity);
    return true;
}
