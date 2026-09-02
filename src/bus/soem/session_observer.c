#include "session_observer.h"

#include <limits.h>
#include <string.h>

enum
{
    /* 仅用于审计容量上界，不是 EtherCAT 或运动参数。 */
    EMASTER_AUDIT_EXCHANGE_MARGIN = 8,
    EMASTER_AUDIT_FINAL_ACCESS_MARGIN_PER_AXIS = 32
};

void emaster_session_observer_read_drive(
    emaster_soem_sdo_reader_context_t *reader,
    emaster_drive_diagnostic_t *diagnostic)
{
    if (reader == NULL || diagnostic == NULL)
    {
        return;
    }
    diagnostic->read_succeeded =
        emaster_soem_read_u16(reader, UINT16_C(0x603F), UINT8_C(0),
                              &diagnostic->cia402_error_code) &&
        emaster_soem_read_u8(reader, UINT16_C(0x1001), UINT8_C(0),
                             &diagnostic->error_register) &&
        emaster_soem_read_u32(reader, UINT16_C(0x203E), UINT8_C(0),
                              &diagnostic->extended_servo_error_code) &&
        emaster_soem_read_u32(reader, UINT16_C(0x203F), UINT8_C(0),
                              &diagnostic->servo_error_code);
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
    size_t axis_index;

    if (plan == NULL || runtime == NULL || audit == NULL ||
        plan->motion_profile == NULL || plan->cycle_ns == 0U)
    {
        return plan != NULL && plan->motion_profile == NULL;
    }
    motion_time_ns =
        ((uint64_t)plan->motion_profile->duration_ms +
         (uint64_t)plan->motion_profile->settle_ms) * UINT64_C(1000000);
    motion_cycles =
        (motion_time_ns + plan->cycle_ns - UINT64_C(1)) / plan->cycle_ns;
    max_exchanges = motion_cycles + transition_cycles * UINT64_C(3) +
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
    capacity = (size_t)max_exchanges * fields_per_exchange;
    if (plan->axis_count >
            (SIZE_MAX - capacity) / EMASTER_AUDIT_FINAL_ACCESS_MARGIN_PER_AXIS ||
        audit->access_count >
            SIZE_MAX - capacity -
                plan->axis_count * EMASTER_AUDIT_FINAL_ACCESS_MARGIN_PER_AXIS)
    {
        return false;
    }
    capacity += plan->axis_count * EMASTER_AUDIT_FINAL_ACCESS_MARGIN_PER_AXIS +
                audit->access_count;
    if (!emaster_run_audit_reserve(audit, capacity))
    {
        return false;
    }
    emaster_run_audit_seal_capacity(audit);
    return true;
}
