#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/dc_prepare.h"

#include "soem_common.h"

#include "soem/soem.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool write_u16(ecx_contextt *context, uint16_t slave, uint16_t index,
                      uint8_t subindex, uint16_t value)
{
    uint16_t raw = htoes(value);
    return ecx_SDOwrite(context, slave, index, subindex, FALSE, (int)sizeof(raw), &raw,
                        EC_TIMEOUTRXM) > 0;
}

static bool read_u16_direct(ecx_contextt *context, uint16_t slave, uint16_t index,
                            uint8_t subindex, uint16_t *value)
{
    int size = (int)sizeof(*value);
    uint16_t raw;
    if (value == NULL || ecx_SDOread(context, slave, index, subindex, FALSE, &size, &raw,
                                     EC_TIMEOUTRXM) <= 0 || size != (int)sizeof(raw))
    {
        return false;
    }
    *value = etohs(raw);
    return true;
}

static bool write_i8(ecx_contextt *context, uint16_t slave, uint16_t index,
                     uint8_t subindex, int8_t value)
{
    return ecx_SDOwrite(context, slave, index, subindex, FALSE, (int)sizeof(value), &value,
                        EC_TIMEOUTRXM) > 0;
}

static bool read_i8(ecx_contextt *context, uint16_t slave, uint16_t index,
                    uint8_t subindex, int8_t *value)
{
    int size = (int)sizeof(*value);
    return value != NULL && ecx_SDOread(context, slave, index, subindex, FALSE, &size, value,
                                        EC_TIMEOUTRXM) > 0 && size == (int)sizeof(*value);
}

static bool read_dc_register(ecx_contextt *context, uint16_t slave, uint16_t address,
                             void *value, uint16_t size)
{
    const uint16_t config_address = context->slavelist[slave].configadr;
    return ecx_FPRD(&context->port, config_address, address, size, value, EC_TIMEOUTRET) > 0;
}

static void disable_sync0(ecx_contextt *context, size_t slave_count,
                          const emaster_dc_prepare_axis_result_t *axes)
{
    size_t index;
    if (context == NULL || axes == NULL)
    {
        return;
    }
    for (index = 0U; index < slave_count; ++index)
    {
        if (axes[index].sync0_configured)
        {
            /* SOEM 官方接口负责生成寄存器写入，禁止在此手工拼装 EtherCAT 帧。 */
            ecx_dcsync0(context, axes[index].position, FALSE, 0U, 0);
        }
    }
}

emaster_dc_prepare_status_t emaster_soem_dc_prepare(
    const emaster_session_plan_t *plan,
    emaster_dc_prepare_axis_result_t *axis_storage,
    size_t axis_capacity,
    emaster_dc_prepare_report_t *report)
{
    ecx_contextt context;
    emaster_dc_prepare_status_t status = EMASTER_DC_PREPARE_OK;
    int slave_count;
    size_t axis_index;
    bool context_open = false;

    if (plan == NULL || plan->status != EMASTER_SESSION_PLAN_READY ||
        plan->deployment == NULL || plan->deployment->ethercat_interface == NULL ||
        axis_storage == NULL || axis_capacity < plan->axis_count || report == NULL)
    {
        return EMASTER_DC_PREPARE_INVALID_ARGUMENT;
    }
    memset(report, 0, sizeof(*report));
    report->status = EMASTER_DC_PREPARE_INVALID_ARGUMENT;
    report->axes = axis_storage;
    report->axis_count = plan->axis_count;
    (void)snprintf(report->interface_name, sizeof(report->interface_name), "%s",
                   plan->deployment->ethercat_interface);
    memset(axis_storage, 0, plan->axis_count * sizeof(*axis_storage));
    memset(&context, 0, sizeof(context));

    if (!ecx_init(&context, plan->deployment->ethercat_interface))
    {
        report->status = EMASTER_DC_PREPARE_INTERFACE_OPEN_FAILED;
        return report->status;
    }
    context_open = true;
    slave_count = ecx_config_init(&context);
    if (slave_count <= 0)
    {
        status = EMASTER_DC_PREPARE_NO_SLAVES;
        goto cleanup;
    }
    if ((size_t)slave_count != plan->axis_count)
    {
        status = EMASTER_DC_PREPARE_TOPOLOGY_MISMATCH;
        goto cleanup;
    }
    if (ecx_statecheck(&context, 0U, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 4) != EC_STATE_PRE_OP)
    {
        status = EMASTER_DC_PREPARE_PREOP_NOT_REACHED;
        goto cleanup;
    }
    ecx_readstate(&context);

    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const emaster_session_axis_plan_t *axis = &plan->axes[axis_index];
        const ec_slavet *slave = &context.slavelist[axis_index + 1U];
        emaster_pdo_layout_t actual_layout;
        uint16_t sm2_value;
        uint16_t sm3_value;
        int8_t mode_value = 0;
        int8_t mode_display = 0;

        axis_storage[axis_index].position = (uint16_t)(axis_index + 1U);
        axis_storage[axis_index].identity_match =
            emaster_slave_identity_matches(axis->device_profile,
                                           &(emaster_slave_identity_t){slave->eep_man,
                                                                       slave->eep_id,
                                                                       slave->eep_rev});
        if (!axis_storage[axis_index].identity_match)
        {
            status = EMASTER_DC_PREPARE_IDENTITY_MISMATCH;
            goto cleanup;
        }
        if (slave->hasdc == 0U)
        {
            status = EMASTER_DC_PREPARE_DC_UNAVAILABLE;
            goto cleanup;
        }

        if (!emaster_soem_discover_pdo_layout(&context, (uint16_t)(axis_index + 1U),
                                              &actual_layout))
        {
            status = EMASTER_DC_PREPARE_PDO_MISMATCH;
            emaster_pdo_layout_destroy(&actual_layout);
            goto cleanup;
        }
        axis_storage[axis_index].pdo_match =
            emaster_session_axis_validate_layout(axis, &actual_layout) ==
            EMASTER_SESSION_LAYOUT_MATCH;
        emaster_pdo_layout_destroy(&actual_layout);
        if (!axis_storage[axis_index].pdo_match)
        {
            status = EMASTER_DC_PREPARE_PDO_MISMATCH;
            goto cleanup;
        }

        axis_storage[axis_index].sm2_written = write_u16(
            &context, (uint16_t)(axis_index + 1U), UINT16_C(0x1C32), UINT8_C(0x01),
            axis->operation_profile->sm2_sync_type);
        axis_storage[axis_index].sm3_written = write_u16(
            &context, (uint16_t)(axis_index + 1U), UINT16_C(0x1C33), UINT8_C(0x01),
            axis->operation_profile->sm3_sync_type);
        axis_storage[axis_index].mode_written = write_i8(
            &context, (uint16_t)(axis_index + 1U), UINT16_C(0x6060), UINT8_C(0x00),
            axis->operation_mode->value);
        if (!axis_storage[axis_index].sm2_written || !axis_storage[axis_index].sm3_written ||
            !axis_storage[axis_index].mode_written)
        {
            status = EMASTER_DC_PREPARE_SDO_WRITE_FAILED;
            goto cleanup;
        }

        axis_storage[axis_index].sm2_readback_match =
            read_u16_direct(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x1C32),
                            UINT8_C(0x01), &sm2_value) &&
            sm2_value == axis->operation_profile->sm2_sync_type;
        axis_storage[axis_index].sm3_readback_match =
            read_u16_direct(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x1C33),
                            UINT8_C(0x01), &sm3_value) &&
            sm3_value == axis->operation_profile->sm3_sync_type;
        axis_storage[axis_index].mode_readback_match =
            read_i8(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x6060), UINT8_C(0x00),
                    &mode_value) &&
            mode_value == axis->operation_mode->value;
        axis_storage[axis_index].mode_value_readback_match =
            axis_storage[axis_index].mode_readback_match;
        axis_storage[axis_index].mode_display_readback_match =
            read_i8(&context, (uint16_t)(axis_index + 1U), UINT16_C(0x6061), UINT8_C(0x00),
                    &mode_display) &&
            mode_display == axis->operation_mode->value;
        axis_storage[axis_index].mode_readback_match =
            axis_storage[axis_index].mode_value_readback_match &&
            axis_storage[axis_index].mode_display_readback_match;
        axis_storage[axis_index].mode_display = mode_display;
        /* 6061 是只读 TPDO 反馈；在 PRE-OP 尚未交换 PDO 时允许暂不可读，后续周期验证再关门。 */
        if (!axis_storage[axis_index].sm2_readback_match ||
            !axis_storage[axis_index].sm3_readback_match ||
            !axis_storage[axis_index].mode_value_readback_match)
        {
            status = EMASTER_DC_PREPARE_SDO_READBACK_FAILED;
            goto cleanup;
        }
    }

    if (!ecx_configdc(&context))
    {
        status = EMASTER_DC_PREPARE_DC_CONFIG_FAILED;
        goto cleanup;
    }
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        uint16_t slave = (uint16_t)(axis_index + 1U);
        uint32_t dc_cycle = 0U;
        uint16_t dc_assign_activate = 0U;
        axis_storage[axis_index].sync0_configured = true;
        ecx_dcsync0(&context, slave, TRUE, plan->cycle_ns,
                    plan->axes[axis_index].operation_profile->sync0_shift_ns);
        axis_storage[axis_index].sync0_readback_match =
            read_dc_register(&context, slave, ECT_REG_DCCYCLE0, &dc_cycle,
                             (uint16_t)sizeof(dc_cycle)) &&
            read_dc_register(&context, slave, ECT_REG_DCCUC, &dc_assign_activate,
                             (uint16_t)sizeof(dc_assign_activate));
        if (axis_storage[axis_index].sync0_readback_match)
        {
            dc_cycle = etohl(dc_cycle);
            axis_storage[axis_index].sync0_cycle_ns = dc_cycle;
            dc_assign_activate = etohs(dc_assign_activate);
            axis_storage[axis_index].sync0_activation =
                (uint8_t)(dc_assign_activate >> CHAR_BIT);
            axis_storage[axis_index].sync0_readback_match =
                dc_cycle == plan->cycle_ns &&
                plan->axes[axis_index].operation_profile->assign_activate <= UINT16_MAX &&
                dc_assign_activate ==
                    (uint16_t)plan->axes[axis_index].operation_profile->assign_activate;
        }
        if (!axis_storage[axis_index].sync0_readback_match)
        {
            status = EMASTER_DC_PREPARE_SYNC0_READBACK_FAILED;
            goto cleanup;
        }
    }

cleanup:
    if (context_open)
    {
        disable_sync0(&context, plan->axis_count, axis_storage);
        report->sync0_disabled = true;
        report->restore_init_succeeded = emaster_soem_restore_init(&context);
        ecx_close(&context);
    }
    report->status = status == EMASTER_DC_PREPARE_OK && !report->restore_init_succeeded
                         ? EMASTER_DC_PREPARE_RESTORE_INIT_FAILED
                         : status;
    return report->status;
}
