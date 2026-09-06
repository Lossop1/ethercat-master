#include "session_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/*
 * 按配置声明的精确类型写入并立即读回，避免对象宽度由调用点猜测。
 * 返回成功只表示邮箱事务成功且读回值与配置值一致。
 */
static bool write_and_verify_configured_sdo(emaster_soem_sdo_reader_context_t *sdo,
                                            const emaster_sdo_write_config_t *command) {
    uint8_t u8_value;
    int8_t i8_value;
    uint16_t u16_value;
    int16_t i16_value;
    uint32_t u32_value;
    int32_t i32_value;

    if (sdo == NULL || command == NULL) {
        return false;
    }
    switch (command->type) {
    case EMASTER_CONFIG_SDO_VALUE_U8:
        u8_value = (uint8_t)command->value;
        return emaster_soem_write_u8(sdo, command->index, command->subindex, u8_value) &&
               emaster_soem_read_u8(sdo, command->index, command->subindex, &u8_value) &&
               u8_value == (uint8_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_I8:
        i8_value = (int8_t)command->value;
        return emaster_soem_write_i8(sdo, command->index, command->subindex, i8_value) &&
               emaster_soem_read_i8(sdo, command->index, command->subindex, &i8_value) &&
               i8_value == (int8_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_U16:
        u16_value = (uint16_t)command->value;
        return emaster_soem_write_u16(sdo, command->index, command->subindex, u16_value) &&
               emaster_soem_read_u16(sdo, command->index, command->subindex, &u16_value) &&
               u16_value == (uint16_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_I16:
        i16_value = (int16_t)command->value;
        return emaster_soem_write_i16(sdo, command->index, command->subindex, i16_value) &&
               emaster_soem_read_i16(sdo, command->index, command->subindex, &i16_value) &&
               i16_value == (int16_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_U32:
        u32_value = (uint32_t)command->value;
        return emaster_soem_write_u32(sdo, command->index, command->subindex, u32_value) &&
               emaster_soem_read_u32(sdo, command->index, command->subindex, &u32_value) &&
               u32_value == (uint32_t)command->value;
    case EMASTER_CONFIG_SDO_VALUE_I32:
        i32_value = (int32_t)command->value;
        return emaster_soem_write_i32(sdo, command->index, command->subindex, i32_value) &&
               emaster_soem_read_i32(sdo, command->index, command->subindex, &i32_value) &&
               i32_value == (int32_t)command->value;
    }
    return false;
}

emaster_control_session_status_t emaster_soem_session_configure(emaster_soem_session_t *session) {
    size_t axis_index;
    size_t io_map_capacity;
    int io_map_size;
    int slave_count;
    emaster_control_session_status_t status;

    memset(&session->context, 0, sizeof(session->context));
    if (!emaster_soem_interface_carrier(session->plan->deployment->ethercat_interface)) {
        status = EMASTER_CONTROL_SESSION_INTERFACE_NOT_READY;
        return status;
    }
    if (!ecx_init(&session->context, session->plan->deployment->ethercat_interface)) {
        status = EMASTER_CONTROL_SESSION_INTERFACE_OPEN_FAILED;
        return status;
    }
    session->context_open = true;
    slave_count = ecx_config_init(&session->context);
    if (slave_count <= 0) {
        status = EMASTER_CONTROL_SESSION_NO_SLAVES;
        return status;
    }
    if ((size_t)slave_count != session->plan->axis_count) {
        status = EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH;
        return status;
    }
    if (!emaster_soem_wait_preop(&session->context)) {
        status = EMASTER_CONTROL_SESSION_PREOP_NOT_REACHED;
        return status;
    }
    ecx_readstate(&session->context);

    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const emaster_session_axis_plan_t *axis = &session->plan->axes[axis_index];
        const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
        emaster_session_layout_status_t layout_status;
        uint16_t sm2_value;
        uint16_t sm3_value;
        int8_t mode_value;
        emaster_soem_sdo_reader_context_t sdo;

        emaster_soem_sdo_context_init(&sdo, &session->context, (uint16_t)(axis_index + 1U),
                                      &session->report->audit,
                                      EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, session->exchange);
        session->axes[axis_index].position = (uint16_t)(axis_index + 1U);
        emaster_cyclic_timing_stats_init(&session->axes[axis_index].timing);
        session->axes[axis_index].requested_mode = axis->operation_mode->value;
        session->axes[axis_index].actual_vendor_id = slave->eep_man;
        session->axes[axis_index].actual_product_code = slave->eep_id;
        session->axes[axis_index].actual_revision = slave->eep_rev;
        session->axes[axis_index].identity_match = emaster_slave_identity_matches(
            axis->device_profile,
            &(emaster_slave_identity_t){slave->eep_man, slave->eep_id, slave->eep_rev});
        if (!session->axes[axis_index].identity_match) {
            status = EMASTER_CONTROL_SESSION_IDENTITY_MISMATCH;
            return status;
        }
        if (!emaster_soem_discover_pdo_layout_recorded(&sdo, &session->images[axis_index].layout)) {
            status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
            return status;
        }
        layout_status =
            emaster_session_axis_validate_layout(axis, &session->images[axis_index].layout);
        if (layout_status == EMASTER_SESSION_LAYOUT_CONFIGURATION_REQUIRED &&
            axis->device_profile->supports_pdo_assignment) {
            emaster_soem_pdo_assignment_result_t assignment_result;

            emaster_pdo_layout_destroy(&session->images[axis_index].layout);
            if (!emaster_soem_assign_pdo_set_recorded(&sdo, axis->pdo_set, &assignment_result)) {
                session->axes[axis_index].pdo_assignment_failed_index =
                    assignment_result.failed_index;
                session->axes[axis_index].pdo_assignment_failed_subindex =
                    assignment_result.failed_subindex;
                session->axes[axis_index].pdo_assignment_abort_code_available =
                    assignment_result.abort_code_available;
                session->axes[axis_index].pdo_assignment_abort_code = assignment_result.abort_code;
                status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
                return status;
            }
            if (!emaster_soem_discover_pdo_layout_recorded(&sdo,
                                                           &session->images[axis_index].layout)) {
                status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
                return status;
            }
            layout_status =
                emaster_session_axis_validate_layout(axis, &session->images[axis_index].layout);
        }
        if (layout_status != EMASTER_SESSION_LAYOUT_MATCH ||
            !emaster_cia_process_image_init(axis, &session->images[axis_index])) {
            status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
            return status;
        }
        session->axes[axis_index].pdo_match = true;

        if (!emaster_soem_write_u16(&sdo, UINT16_C(0x1C32), UINT8_C(0x01),
                                    axis->operation_profile->sm2_sync_type) ||
            !emaster_soem_write_u16(&sdo, UINT16_C(0x1C33), UINT8_C(0x01),
                                    axis->operation_profile->sm3_sync_type) ||
            (!axis->pdo_set->mode_init_on_safeop_to_op &&
             !emaster_soem_write_i8(&sdo, UINT16_C(0x6060), UINT8_C(0x00),
                                    axis->operation_mode->value))) {
            status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
            return status;
        }
        if (!emaster_soem_read_u16(&sdo, UINT16_C(0x1C32), UINT8_C(0x01), &sm2_value) ||
            !emaster_soem_read_u16(&sdo, UINT16_C(0x1C33), UINT8_C(0x01), &sm3_value) ||
            sm2_value != axis->operation_profile->sm2_sync_type ||
            sm3_value != axis->operation_profile->sm3_sync_type ||
            (!axis->pdo_set->mode_init_on_safeop_to_op &&
             (!emaster_soem_read_i8(&sdo, UINT16_C(0x6060), UINT8_C(0x00), &mode_value) ||
              mode_value != axis->operation_mode->value))) {
            status = EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED;
            return status;
        }
        if (!axis->pdo_set->mode_init_on_safeop_to_op)
        {
            session->axes[axis_index].mode_command_sdo_read = true;
            session->axes[axis_index].mode_command_sdo = mode_value;
        }
        emaster_session_observer_read_position_scale(&sdo,
                                                     &session->axes[axis_index].position_scale);
        if (session->plan->motion_profile != NULL) {
            if (!session->axes[axis_index].position_scale.read_succeeded ||
                axis->motion_axis == NULL) {
                status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                return status;
            }
            session->axes[axis_index].position_scale_match = emaster_position_scale_matches(
                axis->motion_axis, &session->axes[axis_index].position_scale);
            if (!session->axes[axis_index].position_scale_match) {
                status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                return status;
            }
            session->motion_scales[axis_index] = session->axes[axis_index].position_scale;
            session->motion_axis_configs[axis_index] = axis->motion_axis;
        }
        if (session->report->audit.allocation_failed) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
    }

    io_map_capacity = (size_t)EC_MAXIOSEGMENTS * (size_t)EC_MAXLRWDATA;
    if (io_map_capacity == 0U || io_map_capacity > SIZE_MAX / sizeof(*session->io_map)) {
        status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
        return status;
    }
    session->io_map = calloc(io_map_capacity, sizeof(*session->io_map));
    if (session->io_map == NULL) {
        status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
        return status;
    }
    /* 禁止映射函数隐式请求 SAFE-OP，状态切换由本会话在 DC 配置后显式执行。 */
    session->context.manualstatechange = 1;
    io_map_size = ecx_config_map_group(&session->context, session->io_map, 0U);
    if (io_map_size <= 0 || (size_t)io_map_size > io_map_capacity) {
        status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
        return status;
    }
    session->report->io_map_size = (size_t)io_map_size;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
        const emaster_session_axis_plan_t *axis = &session->plan->axes[axis_index];

        session->axes[axis_index].process_map_match =
            slave->Obytes == axis->pdo_set->rx_pdo_bytes &&
            slave->Ibytes == axis->pdo_set->tx_pdo_bytes &&
            slave->Obits == (uint32_t)axis->pdo_set->rx_pdo_bytes * UINT32_C(8) &&
            slave->Ibits == (uint32_t)axis->pdo_set->tx_pdo_bytes * UINT32_C(8) &&
            slave->outputs != NULL && slave->inputs != NULL;
        session->axes[axis_index].output_bytes = slave->Obytes;
        session->axes[axis_index].input_bytes = slave->Ibytes;
        session->axes[axis_index].output_bits = slave->Obits;
        session->axes[axis_index].input_bits = slave->Ibits;
        if (slave->outputs != NULL) {
            session->axes[axis_index].output_offset_bytes =
                (size_t)(slave->outputs - session->io_map);
        }
        if (slave->inputs != NULL) {
            session->axes[axis_index].input_offset_bytes =
                (size_t)(slave->inputs - session->io_map);
        }
        if (!session->axes[axis_index].process_map_match) {
            status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
            return status;
        }
    }
    session->process_map_ready = true;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        if (session->plan->axes[axis_index].operation_profile->sync_strategy ==
            EMASTER_SYNC_STRATEGY_DC) {
            session->dc_required = true;
            session->axes[axis_index].dc.requested = true;
            session->axes[axis_index].dc.requested_cycle_ns = session->plan->cycle_ns;
            session->axes[axis_index].dc.requested_shift_ns =
                session->plan->axes[axis_index].operation_profile->sync0_shift_ns;
            if (session->plan->axes[axis_index].operation_profile->assign_activate <= UINT16_MAX) {
                session->axes[axis_index].dc.requested_assign_activate =
                    (uint16_t)session->plan->axes[axis_index].operation_profile->assign_activate;
            }
        }
    }
    session->report->dc_required = session->dc_required;
    if (session->dc_required) {
        uint32_t cycle_value;
        uint16_t activation;

        if (!ecx_configdc(&session->context)) {
            status = EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED;
            return status;
        }
        session->report->dc_configured = true;
        session->report->dc_reference_slave = session->context.grouplist[0].DCnext;
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];

            if (session->plan->axes[axis_index].operation_profile->sync_strategy !=
                EMASTER_SYNC_STRATEGY_DC) {
                continue;
            }
            session->axes[axis_index].dc.slave_capable = slave->hasdc != 0U;
            if (slave->hasdc == 0U ||
                session->plan->axes[axis_index].operation_profile->assign_activate > UINT16_MAX) {
                status = EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED;
                return status;
            }
            ecx_dcsync0(&session->context, (uint16_t)(axis_index + 1U), TRUE,
                        session->plan->cycle_ns,
                        session->plan->axes[axis_index].operation_profile->sync0_shift_ns);
            session->images[axis_index].sync0_configured = true;
            session->axes[axis_index].dc.sync0_requested = true;
            session->axes[axis_index].dc.sync0_active = slave->DCactive != 0U;
            session->axes[axis_index].dc.soem_shift_ns = slave->DCshift;
            /* 只要某一轴已经激活，后续任何失败路径都必须执行统一关闭。 */
            session->sync0_started = true;
            /* DCCUC 与 DCSYNCACT 是连续的 8 位寄存器；按 16 位读回才能核对完整
             * AssignActivate，而不是只确认 SOEM 当前默认激活的 Sync0 位。 */
            if (!emaster_session_observer_read_dc_register(
                    &session->context, (uint16_t)(axis_index + 1U), ECT_REG_DCCYCLE0, &cycle_value,
                    (uint16_t)sizeof(cycle_value), &session->report->audit,
                    EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, session->exchange) ||
                !emaster_session_observer_read_dc_register(
                    &session->context, (uint16_t)(axis_index + 1U), ECT_REG_DCCUC, &activation,
                    (uint16_t)sizeof(activation), &session->report->audit,
                    EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, session->exchange) ||
                etohl(cycle_value) != session->plan->cycle_ns ||
                etohs(activation) !=
                    (uint16_t)session->plan->axes[axis_index].operation_profile->assign_activate) {
                status = EMASTER_CONTROL_SESSION_SYNC0_CONFIG_FAILED;
                return status;
            }
            session->axes[axis_index].dc.register_read_succeeded = true;
            session->axes[axis_index].dc.observed_cycle_ns = etohl(cycle_value);
            session->axes[axis_index].dc.observed_assign_activate = etohs(activation);
        }
        if (session->report->audit.allocation_failed) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
    }

    session->context.slavelist[0].state = EC_STATE_SAFE_OP;
    if (ecx_writestate(&session->context, 0U) <= 0 ||
        ecx_statecheck(&session->context, 0U, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4) !=
            EC_STATE_SAFE_OP) {
        ecx_readstate(&session->context);
        status = EMASTER_CONTROL_SESSION_SAFE_OP_NOT_REACHED;
        return status;
    }
    session->report->safe_op_reached = true;

    /* ESI 的 SO 初始化命令必须在从站已到 SAFE-OP、请求 OP 之前执行。 */
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const emaster_pdo_set_profile_t *pdo_set = session->plan->axes[axis_index].pdo_set;
        const emaster_operation_mode_t *operation_mode =
            session->plan->axes[axis_index].operation_mode;
        int8_t mode_value;
        size_t command_index;
        emaster_soem_sdo_reader_context_t sdo;

        emaster_soem_sdo_context_init(&sdo, &session->context, (uint16_t)(axis_index + 1U),
                                      &session->report->audit,
                                      EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION, session->exchange);

        if (pdo_set->mode_init_on_safeop_to_op) {
            if (!emaster_soem_write_i8(&sdo, pdo_set->mode_init_index, pdo_set->mode_init_subindex,
                                       pdo_set->mode_init_value)) {
                status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
                return status;
            }
            if (!emaster_soem_read_i8(&sdo, pdo_set->mode_init_index, pdo_set->mode_init_subindex,
                                      &mode_value) ||
                mode_value != pdo_set->mode_init_value) {
                status = EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED;
                return status;
            }
            session->axes[axis_index].mode_command_sdo_read = true;
            session->axes[axis_index].mode_command_sdo = mode_value;
        }
        for (command_index = 0U; command_index < operation_mode->safeop_to_op_sdo_write_count;
             ++command_index) {
            const emaster_sdo_write_config_t *command =
                &operation_mode->safeop_to_op_sdo_writes[command_index];

            if (!write_and_verify_configured_sdo(&sdo, command)) {
                status = EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED;
                return status;
            }
        }
        /* 所有 SAFE-OP 初始化项生效后，再采集模式显示和同一时刻的故障对象。 */
        session->axes[axis_index].safeop_mode_display_sdo_read = emaster_soem_read_i8(
            &sdo, UINT16_C(0x6061), UINT8_C(0), &session->axes[axis_index].safeop_mode_display_sdo);
        emaster_session_observer_read_drive(&sdo,
                                            &session->axes[axis_index].safeop_drive_diagnostic);
        if (session->report->audit.allocation_failed) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
    }

    return EMASTER_CONTROL_SESSION_OK;
}
