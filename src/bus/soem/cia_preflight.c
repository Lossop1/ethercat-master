#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/cia_preflight.h"

#include "emaster/catalog/slave_profile.h"
#include "emaster/protocol/pdo_codec.h"
#include "soem_common.h"

#include "soem/soem.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 这些对象是 CiA 402 标准语义，不是某次部署的可变运行参数。 */
enum
{
    EMASTER_CIA402_CONTROL_WORD_INDEX = 0x6040,
    EMASTER_CIA402_STATUS_WORD_INDEX = 0x6041,
    EMASTER_CIA402_MODE_OF_OPERATION_INDEX = 0x6060,
    EMASTER_CIA402_MODE_DISPLAY_INDEX = 0x6061
};

typedef struct
{
    emaster_pdo_layout_t layout;
    emaster_pdo_codec_field_t *rx_fields;
    emaster_pdo_codec_field_t *tx_fields;
    emaster_pdo_codec_value_t *rx_values;
    emaster_pdo_codec_value_t *tx_values;
    size_t rx_field_count;
    size_t tx_field_count;
    size_t rx_mode_ordinal;
    size_t rx_control_ordinal;
    size_t tx_mode_ordinal;
    size_t tx_status_ordinal;
    bool sync0_configured;
} cia_axis_runtime_t;

static bool write_u16(ecx_contextt *context, uint16_t slave, uint16_t index,
                      uint8_t subindex, uint16_t value)
{
    uint16_t raw = htoes(value);

    return ecx_SDOwrite(context, slave, index, subindex, FALSE, (int)sizeof(raw), &raw,
                        EC_TIMEOUTRXM) > 0;
}

static bool write_i8(ecx_contextt *context, uint16_t slave, uint16_t index,
                     uint8_t subindex, int8_t value)
{
    return ecx_SDOwrite(context, slave, index, subindex, FALSE, (int)sizeof(value), &value,
                        EC_TIMEOUTRXM) > 0;
}

static bool read_u16(ecx_contextt *context, uint16_t slave, uint16_t index,
                     uint8_t subindex, uint16_t *value)
{
    uint16_t raw;
    int size = (int)sizeof(raw);

    if (value == NULL ||
        ecx_SDOread(context, slave, index, subindex, FALSE, &size, &raw, EC_TIMEOUTRXM) <= 0 ||
        size != (int)sizeof(raw))
    {
        return false;
    }
    *value = etohs(raw);
    return true;
}

static bool read_i8(ecx_contextt *context, uint16_t slave, uint16_t index,
                    uint8_t subindex, int8_t *value)
{
    int size = (int)sizeof(*value);

    return value != NULL &&
           ecx_SDOread(context, slave, index, subindex, FALSE, &size, value, EC_TIMEOUTRXM) > 0 &&
           size == (int)sizeof(*value);
}

static bool read_dc_register(ecx_contextt *context, uint16_t slave, uint16_t address,
                             void *value, uint16_t size)
{
    if (context == NULL || value == NULL || slave == 0U)
    {
        return false;
    }
    return ecx_FPRD(&context->port, context->slavelist[slave].configadr, address, size, value,
                    EC_TIMEOUTRET) > 0;
}

static const emaster_pdo_mapping_profile_t *profile_mapping_for(
    const emaster_pdo_mapping_profile_t *mappings, size_t mapping_count, uint16_t mapping_index)
{
    size_t mapping_ordinal;

    if (mappings == NULL)
    {
        return NULL;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < mapping_count; ++mapping_ordinal)
    {
        if (mappings[mapping_ordinal].index == mapping_index)
        {
            return &mappings[mapping_ordinal];
        }
    }
    return NULL;
}

static emaster_pdo_codec_value_kind_t field_kind_for(const emaster_pdo_entry_t *entry)
{
    if (entry == NULL || entry->data_type == NULL)
    {
        return EMASTER_PDO_CODEC_VALUE_PADDING;
    }
    if (strcmp(entry->data_type, "PADDING") == 0)
    {
        return EMASTER_PDO_CODEC_VALUE_PADDING;
    }
    if (strcmp(entry->data_type, "SINT") == 0 || strcmp(entry->data_type, "INT") == 0 ||
        strcmp(entry->data_type, "DINT") == 0 || strcmp(entry->data_type, "LINT") == 0)
    {
        return EMASTER_PDO_CODEC_VALUE_SIGNED;
    }
    if (strcmp(entry->data_type, "USINT") == 0 || strcmp(entry->data_type, "UINT") == 0 ||
        strcmp(entry->data_type, "UDINT") == 0 || strcmp(entry->data_type, "ULINT") == 0)
    {
        return EMASTER_PDO_CODEC_VALUE_UNSIGNED;
    }
    return EMASTER_PDO_CODEC_VALUE_PADDING;
}

static bool build_direction_codec(
    const emaster_pdo_direction_layout_t *actual,
    const emaster_pdo_mapping_profile_t *profiles,
    size_t profile_count,
    emaster_pdo_codec_field_t *fields,
    size_t field_count,
    uint16_t mode_index,
    size_t *mode_ordinal,
    size_t *status_ordinal,
    size_t *control_ordinal)
{
    size_t mapping_ordinal;
    size_t field_ordinal = 0U;
    bool mode_found = false;
    bool status_found = status_ordinal == NULL;
    bool control_found = control_ordinal == NULL;

    if (actual == NULL || fields == NULL || field_count == 0U || mode_ordinal == NULL)
    {
        return false;
    }
    *mode_ordinal = SIZE_MAX;
    if (status_ordinal != NULL)
    {
        *status_ordinal = SIZE_MAX;
    }
    if (control_ordinal != NULL)
    {
        *control_ordinal = SIZE_MAX;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < actual->mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &actual->mappings[mapping_ordinal];
        const emaster_pdo_mapping_profile_t *profile_mapping =
            profile_mapping_for(profiles, profile_count, mapping->mapping_index);
        size_t entry_ordinal;

        if (profile_mapping == NULL || profile_mapping->entries == NULL ||
            profile_mapping->entry_count != mapping->entry_count)
        {
            return false;
        }
        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *actual_entry = &mapping->entries[entry_ordinal];
            const emaster_pdo_entry_t *profile_entry = &profile_mapping->entries[entry_ordinal];
            emaster_pdo_codec_value_kind_t kind;

            if (actual_entry->object_index != profile_entry->index ||
                actual_entry->object_subindex != profile_entry->subindex ||
                actual_entry->bit_length != profile_entry->bit_length)
            {
                return false;
            }
            kind = field_kind_for(profile_entry);
            if (kind == EMASTER_PDO_CODEC_VALUE_PADDING &&
                profile_entry->index != UINT16_C(0))
            {
                return false;
            }
            fields[field_ordinal].kind = kind;
            if (actual_entry->object_index == mode_index && actual_entry->object_subindex == 0U)
            {
                if (mode_found || kind != EMASTER_PDO_CODEC_VALUE_SIGNED)
                {
                    return false;
                }
                *mode_ordinal = field_ordinal;
                mode_found = true;
            }
            if (status_ordinal != NULL && actual_entry->object_index == EMASTER_CIA402_STATUS_WORD_INDEX &&
                actual_entry->object_subindex == 0U)
            {
                if (status_found || kind != EMASTER_PDO_CODEC_VALUE_UNSIGNED)
                {
                    return false;
                }
                *status_ordinal = field_ordinal;
                status_found = true;
            }
            if (control_ordinal != NULL && actual_entry->object_index == EMASTER_CIA402_CONTROL_WORD_INDEX &&
                actual_entry->object_subindex == 0U)
            {
                if (control_found || kind != EMASTER_PDO_CODEC_VALUE_UNSIGNED)
                {
                    return false;
                }
                *control_ordinal = field_ordinal;
                control_found = true;
            }
            ++field_ordinal;
        }
    }
    return field_ordinal == field_count && mode_found && status_found && control_found;
}

static bool prepare_axis_codec(const emaster_session_axis_plan_t *axis,
                               cia_axis_runtime_t *runtime)
{
    size_t rx_count;
    size_t tx_count;

    if (axis == NULL || axis->pdo_set == NULL || runtime == NULL)
    {
        return false;
    }
    rx_count = emaster_pdo_codec_field_count(&runtime->layout.rx);
    tx_count = emaster_pdo_codec_field_count(&runtime->layout.tx);
    if (rx_count == SIZE_MAX || tx_count == SIZE_MAX || rx_count == 0U || tx_count == 0U)
    {
        return false;
    }
    runtime->rx_fields = calloc(rx_count, sizeof(*runtime->rx_fields));
    runtime->tx_fields = calloc(tx_count, sizeof(*runtime->tx_fields));
    runtime->rx_values = calloc(rx_count, sizeof(*runtime->rx_values));
    runtime->tx_values = calloc(tx_count, sizeof(*runtime->tx_values));
    if (runtime->rx_fields == NULL || runtime->tx_fields == NULL || runtime->rx_values == NULL ||
        runtime->tx_values == NULL)
    {
        return false;
    }
    runtime->rx_field_count = rx_count;
    runtime->tx_field_count = tx_count;
    if (!build_direction_codec(&runtime->layout.rx, axis->pdo_set->rx_mappings,
                               axis->pdo_set->rx_mapping_count, runtime->rx_fields, rx_count,
                               EMASTER_CIA402_MODE_OF_OPERATION_INDEX, &runtime->rx_mode_ordinal,
                               NULL, &runtime->rx_control_ordinal) ||
        !build_direction_codec(&runtime->layout.tx, axis->pdo_set->tx_mappings,
                               axis->pdo_set->tx_mapping_count, runtime->tx_fields, tx_count,
                               EMASTER_CIA402_MODE_DISPLAY_INDEX, &runtime->tx_mode_ordinal,
                               &runtime->tx_status_ordinal, NULL))
    {
        return false;
    }
    return true;
}

static bool prepare_axis_output(const emaster_session_axis_plan_t *axis,
                                cia_axis_runtime_t *runtime,
                                uint8_t *output,
                                size_t output_capacity)
{
    size_t field_ordinal;

    if (axis == NULL || axis->operation_mode == NULL || runtime == NULL || output == NULL)
    {
        return false;
    }
    memset(runtime->rx_values, 0, runtime->rx_field_count * sizeof(*runtime->rx_values));
    for (field_ordinal = 0U; field_ordinal < runtime->rx_field_count; ++field_ordinal)
    {
        runtime->rx_values[field_ordinal].kind = runtime->rx_fields[field_ordinal].kind;
    }
    runtime->rx_values[runtime->rx_mode_ordinal].value.signed_value = axis->operation_mode->value;
    runtime->rx_values[runtime->rx_control_ordinal].value.unsigned_value = UINT64_C(0);
    return emaster_pdo_codec_encode(&runtime->layout.rx, runtime->rx_fields,
                                    runtime->rx_field_count, runtime->rx_values,
                                    runtime->rx_field_count, output, output_capacity) ==
           EMASTER_PDO_CODEC_OK;
}

static bool decode_axis_input(cia_axis_runtime_t *runtime, const uint8_t *input,
                              size_t input_length, int8_t *mode_display, uint16_t *status_word)
{
    if (runtime == NULL || input == NULL || mode_display == NULL || status_word == NULL ||
        emaster_pdo_codec_decode(&runtime->layout.tx, runtime->tx_fields,
                                 runtime->tx_field_count, input, input_length, runtime->tx_values,
                                 runtime->tx_field_count) != EMASTER_PDO_CODEC_OK)
    {
        return false;
    }
    if (runtime->tx_values[runtime->tx_mode_ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED ||
        runtime->tx_values[runtime->tx_status_ordinal].kind != EMASTER_PDO_CODEC_VALUE_UNSIGNED)
    {
        return false;
    }
    *mode_display = (int8_t)runtime->tx_values[runtime->tx_mode_ordinal].value.signed_value;
    *status_word = (uint16_t)runtime->tx_values[runtime->tx_status_ordinal].value.unsigned_value;
    return true;
}

static void runtime_destroy(cia_axis_runtime_t *runtime, size_t count)
{
    size_t axis_index;

    if (runtime == NULL)
    {
        return;
    }
    for (axis_index = 0U; axis_index < count; ++axis_index)
    {
        emaster_pdo_layout_destroy(&runtime[axis_index].layout);
        free(runtime[axis_index].rx_fields);
        free(runtime[axis_index].tx_fields);
        free(runtime[axis_index].rx_values);
        free(runtime[axis_index].tx_values);
    }
    free(runtime);
}

static void disable_sync0(ecx_contextt *context, cia_axis_runtime_t *runtime, size_t count)
{
    size_t axis_index;

    if (context == NULL || runtime == NULL)
    {
        return;
    }
    for (axis_index = 0U; axis_index < count; ++axis_index)
    {
        if (runtime[axis_index].sync0_configured)
        {
            ecx_dcsync0(context, (uint16_t)(axis_index + 1U), FALSE, 0U, 0);
        }
    }
}

emaster_cia_preflight_status_t emaster_soem_cia_preflight(
    const emaster_session_plan_t *plan,
    emaster_cia_preflight_axis_result_t *axis_storage,
    size_t axis_capacity,
    emaster_cia_preflight_report_t *report)
{
    ecx_contextt context;
    cia_axis_runtime_t *runtime = NULL;
    uint8_t *io_map = NULL;
    emaster_cia_preflight_status_t status = EMASTER_CIA_PREFLIGHT_OK;
    size_t io_map_capacity;
    size_t axis_index;
    int slave_count;
    int io_map_size;
    bool context_open = false;
    bool sync0_started = false;
    bool dc_required = false;

    if (plan == NULL || plan->status != EMASTER_SESSION_PLAN_READY || plan->deployment == NULL ||
        plan->deployment->ethercat_interface == NULL || axis_storage == NULL ||
        axis_capacity < plan->axis_count || report == NULL || plan->axis_count == 0U)
    {
        return EMASTER_CIA_PREFLIGHT_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    report->status = EMASTER_CIA_PREFLIGHT_INVALID_ARGUMENT;
    report->axes = axis_storage;
    report->axis_count = plan->axis_count;
    (void)snprintf(report->interface_name, sizeof(report->interface_name), "%s",
                   plan->deployment->ethercat_interface);
    memset(axis_storage, 0, plan->axis_count * sizeof(*axis_storage));
    runtime = calloc(plan->axis_count, sizeof(*runtime));
    if (runtime == NULL)
    {
        status = EMASTER_CIA_PREFLIGHT_OUT_OF_MEMORY;
        goto cleanup;
    }
    memset(&context, 0, sizeof(context));
    if (!emaster_soem_interface_carrier(plan->deployment->ethercat_interface))
    {
        status = EMASTER_CIA_PREFLIGHT_INTERFACE_NOT_READY;
        goto cleanup;
    }
    if (!ecx_init(&context, plan->deployment->ethercat_interface))
    {
        status = EMASTER_CIA_PREFLIGHT_INTERFACE_OPEN_FAILED;
        goto cleanup;
    }
    context_open = true;
    slave_count = ecx_config_init(&context);
    if (slave_count <= 0)
    {
        status = EMASTER_CIA_PREFLIGHT_NO_SLAVES;
        goto cleanup;
    }
    if ((size_t)slave_count != plan->axis_count)
    {
        status = EMASTER_CIA_PREFLIGHT_TOPOLOGY_MISMATCH;
        goto cleanup;
    }
    if (!emaster_soem_wait_preop(&context))
    {
        status = EMASTER_CIA_PREFLIGHT_PREOP_NOT_REACHED;
        goto cleanup;
    }
    ecx_readstate(&context);

    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const emaster_session_axis_plan_t *axis = &plan->axes[axis_index];
        const ec_slavet *slave = &context.slavelist[axis_index + 1U];
        uint16_t sm2_value;
        uint16_t sm3_value;
        int8_t mode_value;

        axis_storage[axis_index].position = (uint16_t)(axis_index + 1U);
        axis_storage[axis_index].identity_match = emaster_slave_identity_matches(
            axis->device_profile,
            &(emaster_slave_identity_t){slave->eep_man, slave->eep_id, slave->eep_rev});
        if (!axis_storage[axis_index].identity_match)
        {
            status = EMASTER_CIA_PREFLIGHT_IDENTITY_MISMATCH;
            goto cleanup;
        }
        if (!emaster_soem_discover_pdo_layout(&context, (uint16_t)(axis_index + 1U),
                                              &runtime[axis_index].layout) ||
            emaster_session_axis_validate_layout(axis, &runtime[axis_index].layout) !=
                EMASTER_SESSION_LAYOUT_MATCH ||
            !prepare_axis_codec(axis, &runtime[axis_index]))
        {
            status = EMASTER_CIA_PREFLIGHT_PDO_MISMATCH;
            goto cleanup;
        }
        axis_storage[axis_index].pdo_match = true;

        if (!write_u16(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x1C32), UINT8_C(0x01),
                       axis->operation_profile->sm2_sync_type) ||
            !write_u16(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x1C33), UINT8_C(0x01),
                       axis->operation_profile->sm3_sync_type) ||
            !write_i8(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x6060), UINT8_C(0x00),
                      axis->operation_mode->value))
        {
            status = EMASTER_CIA_PREFLIGHT_SDO_WRITE_FAILED;
            goto cleanup;
        }
        if (!read_u16(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x1C32), UINT8_C(0x01),
                      &sm2_value) ||
            !read_u16(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x1C33), UINT8_C(0x01),
                       &sm3_value) ||
            !read_i8(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x6060), UINT8_C(0x00),
                     &mode_value) ||
            sm2_value != axis->operation_profile->sm2_sync_type ||
            sm3_value != axis->operation_profile->sm3_sync_type ||
            mode_value != axis->operation_mode->value)
        {
            status = EMASTER_CIA_PREFLIGHT_SDO_READBACK_FAILED;
            goto cleanup;
        }
    }

    io_map_capacity = (size_t)EC_MAXIOSEGMENTS * (size_t)EC_MAXLRWDATA;
    if (io_map_capacity == 0U || io_map_capacity > SIZE_MAX / sizeof(*io_map))
    {
        status = EMASTER_CIA_PREFLIGHT_PROCESS_MAP_FAILED;
        goto cleanup;
    }
    io_map = calloc(io_map_capacity, sizeof(*io_map));
    if (io_map == NULL)
    {
        status = EMASTER_CIA_PREFLIGHT_OUT_OF_MEMORY;
        goto cleanup;
    }
    io_map_size = ecx_config_map_group(&context, io_map, 0U);
    if (io_map_size <= 0 || (size_t)io_map_size > io_map_capacity)
    {
        status = EMASTER_CIA_PREFLIGHT_PROCESS_MAP_FAILED;
        goto cleanup;
    }
    report->io_map_size = (size_t)io_map_size;
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const ec_slavet *slave = &context.slavelist[axis_index + 1U];
        const emaster_session_axis_plan_t *axis = &plan->axes[axis_index];

        axis_storage[axis_index].process_map_match =
            slave->Obytes == axis->pdo_set->rx_pdo_bytes &&
            slave->Ibytes == axis->pdo_set->tx_pdo_bytes &&
            slave->Obits == (uint32_t)axis->pdo_set->rx_pdo_bytes * UINT32_C(8) &&
            slave->Ibits == (uint32_t)axis->pdo_set->tx_pdo_bytes * UINT32_C(8) &&
            slave->outputs != NULL && slave->inputs != NULL;
        if (!axis_storage[axis_index].process_map_match)
        {
            status = EMASTER_CIA_PREFLIGHT_PROCESS_MAP_FAILED;
            goto cleanup;
        }
    }
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        if (plan->axes[axis_index].operation_profile->sync_strategy == EMASTER_SYNC_STRATEGY_DC)
        {
            dc_required = true;
            break;
        }
    }
    if (dc_required)
    {
        uint32_t cycle_value;
        uint16_t activation;

        if (!ecx_configdc(&context))
        {
            status = EMASTER_CIA_PREFLIGHT_DC_CONFIG_FAILED;
            goto cleanup;
        }
        for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
        {
            const ec_slavet *slave = &context.slavelist[axis_index + 1U];

            if (plan->axes[axis_index].operation_profile->sync_strategy != EMASTER_SYNC_STRATEGY_DC)
            {
                continue;
            }
            if (slave->hasdc == 0U)
            {
                status = EMASTER_CIA_PREFLIGHT_DC_CONFIG_FAILED;
                goto cleanup;
            }
            ecx_dcsync0(&context, (uint16_t)(axis_index + 1U), TRUE, plan->cycle_ns,
                        plan->axes[axis_index].operation_profile->sync0_shift_ns);
            runtime[axis_index].sync0_configured = true;
            /* 只要某一轴已经激活，后续任何失败路径都必须执行统一关闭。 */
            sync0_started = true;
            /* DCCUC 与 DCSYNCACT 是连续的 8 位寄存器；按 16 位读回才能核对完整
             * AssignActivate，而不是只确认 SOEM 当前默认激活的 Sync0 位。 */
            if (!read_dc_register(&context, (uint16_t)(axis_index + 1U), ECT_REG_DCCYCLE0,
                                  &cycle_value, (uint16_t)sizeof(cycle_value)) ||
                !read_dc_register(&context, (uint16_t)(axis_index + 1U), ECT_REG_DCCUC,
                                  &activation, (uint16_t)sizeof(activation)) ||
                etohl(cycle_value) != plan->cycle_ns ||
                plan->axes[axis_index].operation_profile->assign_activate > UINT16_MAX ||
                etohs(activation) !=
                    (uint16_t)plan->axes[axis_index].operation_profile->assign_activate)
            {
                status = EMASTER_CIA_PREFLIGHT_SYNC0_CONFIG_FAILED;
                goto cleanup;
            }
        }
    }

    if (ecx_statecheck(&context, 0U, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4) !=
        EC_STATE_SAFE_OP)
    {
        status = EMASTER_CIA_PREFLIGHT_SAFE_OP_NOT_REACHED;
        goto cleanup;
    }
    report->safe_op_reached = true;

    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const emaster_session_axis_plan_t *axis = &plan->axes[axis_index];
        ec_slavet *slave = &context.slavelist[axis_index + 1U];

        axis_storage[axis_index].output_initialized = prepare_axis_output(
            axis, &runtime[axis_index], slave->outputs, slave->Obytes);
        if (!axis_storage[axis_index].output_initialized)
        {
            status = EMASTER_CIA_PREFLIGHT_PROCESS_MAP_FAILED;
            goto cleanup;
        }
    }
    report->expected_wkc = (uint16_t)(context.grouplist[0].outputsWKC * 2U +
                                      context.grouplist[0].inputsWKC);
    /* OP 请求前必须先发送一帧已经完全初始化的安全输出。 */
    (void)ecx_send_processdata(&context);
    report->actual_wkc = ecx_receive_processdata(&context, EC_TIMEOUTRET);
    if (report->actual_wkc != (int)report->expected_wkc)
    {
        status = EMASTER_CIA_PREFLIGHT_INITIAL_WKC_FAILED;
        goto cleanup;
    }
    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context, 0U);
    if (ecx_statecheck(&context, 0U, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE * 4) !=
        EC_STATE_OPERATIONAL)
    {
        status = EMASTER_CIA_PREFLIGHT_OP_NOT_REACHED;
        goto cleanup;
    }
    report->op_reached = true;
    (void)ecx_send_processdata(&context);
    report->actual_wkc = ecx_receive_processdata(&context, EC_TIMEOUTRET);
    if (report->actual_wkc != (int)report->expected_wkc)
    {
        status = EMASTER_CIA_PREFLIGHT_INITIAL_WKC_FAILED;
        goto cleanup;
    }
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const ec_slavet *slave = &context.slavelist[axis_index + 1U];
        int8_t mode_display;
        uint16_t status_word;

        axis_storage[axis_index].input_decoded = decode_axis_input(
            &runtime[axis_index], slave->inputs, slave->Ibytes, &mode_display, &status_word);
        if (!axis_storage[axis_index].input_decoded)
        {
            status = EMASTER_CIA_PREFLIGHT_FEEDBACK_INVALID;
            goto cleanup;
        }
        axis_storage[axis_index].mode_display = mode_display;
        axis_storage[axis_index].status_word = status_word;
        axis_storage[axis_index].mode_display_match =
            mode_display == plan->axes[axis_index].operation_mode->value;
        if (!axis_storage[axis_index].mode_display_match)
        {
            status = EMASTER_CIA_PREFLIGHT_FEEDBACK_INVALID;
            goto cleanup;
        }
    }

cleanup:
    if (context_open)
    {
        if (sync0_started)
        {
            disable_sync0(&context, runtime, plan->axis_count);
            report->sync0_disabled = true;
        }
        report->restore_init_succeeded = emaster_soem_restore_init(&context);
        ecx_close(&context);
    }
    free(io_map);
    runtime_destroy(runtime, plan->axis_count);
    report->status = status == EMASTER_CIA_PREFLIGHT_OK && !report->restore_init_succeeded
                         ? EMASTER_CIA_PREFLIGHT_RESTORE_INIT_FAILED
                         : status;
    return report->status;
}
