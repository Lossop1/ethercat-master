#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/control_session.h"

#include "cia_process_image.h"
#include "emaster/catalog/slave_profile.h"
#include "emaster/multiaxis/coordinator.h"
#include "session_observer.h"
#include "soem_common.h"

#include "soem/soem.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void disable_sync0(ecx_contextt *context, emaster_cia_process_image_t *runtime, size_t count)
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

static bool timespec_add_ns(struct timespec *value, uint32_t nanoseconds)
{
    uint64_t total_nanoseconds;

    if (value == NULL)
    {
        return false;
    }
    total_nanoseconds = (uint64_t)value->tv_nsec + (uint64_t)nanoseconds;
    value->tv_sec += (time_t)(total_nanoseconds / UINT64_C(1000000000));
    value->tv_nsec = (long)(total_nanoseconds % UINT64_C(1000000000));
    return true;
}

static uint64_t absolute_position_difference(int32_t left, int32_t right)
{
    int64_t difference = (int64_t)left - (int64_t)right;

    return (uint64_t)(difference < 0 ? -difference : difference);
}

static bool wait_for_cycle(struct timespec *deadline)
{
    int result;

    if (deadline == NULL)
    {
        return false;
    }
    do
    {
        result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
    } while (result == EINTR);
    return result == 0;
}

/*
 * 清理阶段按 CiA 402 状态反馈逐级撤销运行使能和电压，不能把“控制字已发出”当作停用完成。
 * 最大周期数复用 SOEM 的状态切换超时，避免在总线或驱动器失联时无限阻塞退出。
 */
static bool stop_process_data(const emaster_session_plan_t *plan,
                              emaster_cia_process_image_t *runtime,
                              emaster_control_session_axis_result_t *axes,
                              emaster_cia402_controller_t *controllers,
                              emaster_cia402_output_t *controller_outputs,
                              uint16_t *status_words,
                              emaster_multiaxis_coordinator_t *coordinator,
                              ecx_contextt *context,
                              emaster_control_session_report_t *report,
                              bool *safe_output_sent,
                              uint64_t *process_exchange)
{
    struct timespec deadline;
    uint64_t timeout_ns;
    uint64_t max_cycles;
    uint64_t cycle_index;
    size_t axis_index;

    if (plan == NULL || runtime == NULL || axes == NULL || controllers == NULL ||
        controller_outputs == NULL || status_words == NULL || coordinator == NULL ||
        context == NULL || report == NULL || safe_output_sent == NULL || plan->cycle_ns == 0U ||
        process_exchange == NULL ||
        clock_gettime(CLOCK_MONOTONIC, &deadline) != 0 ||
        !timespec_add_ns(&deadline, plan->cycle_ns))
    {
        return false;
    }
    timeout_ns = (uint64_t)EC_TIMEOUTSTATE * UINT64_C(1000);
    max_cycles = (timeout_ns + plan->cycle_ns - UINT64_C(1)) / plan->cycle_ns;
    if (max_cycles == 0U)
    {
        max_cycles = 1U;
    }
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        status_words[axis_index] = axes[axis_index].status_word;
        if (!emaster_cia402_controller_set_goal(&controllers[axis_index],
                                                EMASTER_CIA402_GOAL_SAFE_STOP))
        {
            return false;
        }
    }

    for (cycle_index = 0U; cycle_index < max_cycles; ++cycle_index)
    {
        emaster_multiaxis_frame_t frame;
        bool all_axes_safe = true;

        frame.sequence = report->cycle_count + UINT64_C(1);
        frame.deadline_ns = UINT64_MAX;
        frame.axis_count = plan->axis_count;
        frame.status_words = status_words;
        frame.outputs = controller_outputs;
        if (emaster_multiaxis_coordinator_step(coordinator, &frame, UINT64_C(0)) !=
            EMASTER_MULTIAXIS_OK)
        {
            return false;
        }
        for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
        {
            ec_slavet *slave = &context->slavelist[axis_index + 1U];

            axes[axis_index].control_word = controller_outputs[axis_index].control_word;
            if (!emaster_cia_process_image_update_output(
                    &plan->axes[axis_index], &runtime[axis_index],
                    controller_outputs[axis_index].control_word,
                    axes[axis_index].target_position, slave->outputs, slave->Obytes))
            {
                return false;
            }
        }
        for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
        {
            (void)emaster_cia_process_image_audit_output(
                &runtime[axis_index], &report->audit,
                EMASTER_AUDIT_PHASE_SAFE_STOP, (uint16_t)(axis_index + 1U),
                *process_exchange + UINT64_C(1));
        }
        ++(*process_exchange);
        (void)ecx_send_processdata(context);
        report->actual_wkc = ecx_receive_processdata(context, EC_TIMEOUTRET);
        ++report->cycle_count;
        if (report->actual_wkc != (int)report->expected_wkc)
        {
            return false;
        }
        *safe_output_sent = true;
        for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
        {
            const ec_slavet *slave = &context->slavelist[axis_index + 1U];
            emaster_control_session_axis_result_t *axis = &axes[axis_index];
            emaster_cia402_state_t state;
            int32_t actual_position;

            axis->input_decoded = emaster_cia_process_image_decode_input(
                &runtime[axis_index], slave->inputs, slave->Ibytes, &axis->mode_display,
                &axis->status_word, &actual_position);
            axis->actual_position = actual_position;
            status_words[axis_index] = axis->status_word;
            if (axis->input_decoded)
            {
                (void)emaster_cia_process_image_audit_input(
                    &runtime[axis_index], &report->audit,
                    EMASTER_AUDIT_PHASE_SAFE_STOP,
                    (uint16_t)(axis_index + 1U), *process_exchange);
            }
            if (!axis->input_decoded ||
                !emaster_cia402_decode_status_word(axis->status_word, &state))
            {
                all_axes_safe = false;
                continue;
            }
            axis->cia402_state = state;
            if ((axis->status_word & plan->axes[axis_index].device_profile->safe_stop_status_mask) !=
                plan->axes[axis_index].device_profile->safe_stop_status_value)
            {
                all_axes_safe = false;
            }
        }
        if (all_axes_safe)
        {
            return true;
        }
        if (cycle_index + UINT64_C(1) < max_cycles &&
            (!wait_for_cycle(&deadline) || !timespec_add_ns(&deadline, plan->cycle_ns)))
        {
            return false;
        }
    }
    return false;
}

emaster_control_session_status_t emaster_soem_control_session(
    const emaster_session_plan_t *plan,
    emaster_control_session_axis_result_t *axis_storage,
    size_t axis_capacity,
    emaster_control_session_stop_requested_t stop_requested,
    void *stop_user_data,
    emaster_control_session_report_t *report)
{
    ecx_contextt context;
    emaster_cia_process_image_t *runtime = NULL;
    emaster_cia402_controller_t *controllers = NULL;
    emaster_cia402_output_t *controller_outputs = NULL;
    uint16_t *status_words = NULL;
    int32_t *actual_positions = NULL;
    int32_t *target_positions = NULL;
    emaster_position_scale_t *motion_scales = NULL;
    const emaster_motion_axis_config_t **motion_axis_configs = NULL;
    emaster_relative_motion_axis_t *motion_axes = NULL;
    emaster_relative_motion_t motion;
    emaster_multiaxis_coordinator_t coordinator;
    uint8_t *io_map = NULL;
    emaster_control_session_status_t status = EMASTER_CONTROL_SESSION_OK;
    size_t io_map_capacity;
    size_t axis_index;
    int slave_count;
    int io_map_size;
    bool context_open = false;
    bool sync0_started = false;
    bool dc_required = false;
    bool process_map_ready = false;
    bool cycle_output_active = false;
    bool safe_output_sent = false;
    bool motion_initialized = false;
    bool mode_confirmation_started = false;
    uint64_t mode_confirmation_deadline_cycle = 0U;
    uint64_t mode_confirmation_cycles;
    uint64_t enable_deadline_cycle;
    uint64_t process_exchange = 0U;

    if (plan == NULL || plan->status != EMASTER_SESSION_PLAN_READY || plan->deployment == NULL ||
        plan->deployment->ethercat_interface == NULL || axis_storage == NULL ||
        axis_capacity < plan->axis_count || report == NULL || plan->axis_count == 0U)
    {
        return EMASTER_CONTROL_SESSION_INVALID_ARGUMENT;
    }
    mode_confirmation_cycles =
        ((uint64_t)EC_TIMEOUTSTATE * UINT64_C(1000) + plan->cycle_ns - UINT64_C(1)) /
        plan->cycle_ns;
    memset(report, 0, sizeof(*report));
    emaster_run_audit_init(&report->audit);
    memset(&motion, 0, sizeof(motion));
    report->status = EMASTER_CONTROL_SESSION_INVALID_ARGUMENT;
    report->axes = axis_storage;
    report->axis_count = plan->axis_count;
    (void)snprintf(report->interface_name, sizeof(report->interface_name), "%s",
                   plan->deployment->ethercat_interface);
    memset(axis_storage, 0, plan->axis_count * sizeof(*axis_storage));
    runtime = calloc(plan->axis_count, sizeof(*runtime));
    controllers = calloc(plan->axis_count, sizeof(*controllers));
    controller_outputs = calloc(plan->axis_count, sizeof(*controller_outputs));
    status_words = calloc(plan->axis_count, sizeof(*status_words));
    if (runtime == NULL || controllers == NULL || controller_outputs == NULL ||
        status_words == NULL)
    {
        status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (plan->motion_profile != NULL)
    {
        actual_positions = calloc(plan->axis_count, sizeof(*actual_positions));
        target_positions = calloc(plan->axis_count, sizeof(*target_positions));
        motion_scales = calloc(plan->axis_count, sizeof(*motion_scales));
        motion_axis_configs = calloc(plan->axis_count, sizeof(*motion_axis_configs));
        motion_axes = calloc(plan->axis_count, sizeof(*motion_axes));
        if (actual_positions == NULL || target_positions == NULL ||
            motion_scales == NULL || motion_axis_configs == NULL ||
            motion_axes == NULL)
        {
            status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
            goto cleanup;
        }
    }
    if (!emaster_multiaxis_coordinator_init(&coordinator, controllers, plan->axis_count))
    {
        status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
        goto cleanup;
    }
    memset(&context, 0, sizeof(context));
    if (!emaster_soem_interface_carrier(plan->deployment->ethercat_interface))
    {
        status = EMASTER_CONTROL_SESSION_INTERFACE_NOT_READY;
        goto cleanup;
    }
    if (!ecx_init(&context, plan->deployment->ethercat_interface))
    {
        status = EMASTER_CONTROL_SESSION_INTERFACE_OPEN_FAILED;
        goto cleanup;
    }
    context_open = true;
    slave_count = ecx_config_init(&context);
    if (slave_count <= 0)
    {
        status = EMASTER_CONTROL_SESSION_NO_SLAVES;
        goto cleanup;
    }
    if ((size_t)slave_count != plan->axis_count)
    {
        status = EMASTER_CONTROL_SESSION_TOPOLOGY_MISMATCH;
        goto cleanup;
    }
    if (!emaster_soem_wait_preop(&context))
    {
        status = EMASTER_CONTROL_SESSION_PREOP_NOT_REACHED;
        goto cleanup;
    }
    ecx_readstate(&context);

    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const emaster_session_axis_plan_t *axis = &plan->axes[axis_index];
        const ec_slavet *slave = &context.slavelist[axis_index + 1U];
        emaster_session_layout_status_t layout_status;
        uint16_t sm2_value;
        uint16_t sm3_value;
        int8_t mode_value;
        emaster_soem_sdo_reader_context_t sdo;

        emaster_soem_sdo_context_init(
            &sdo, &context, (uint16_t)(axis_index + 1U), &report->audit,
            EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, process_exchange);
        axis_storage[axis_index].position = (uint16_t)(axis_index + 1U);
        axis_storage[axis_index].requested_mode = axis->operation_mode->value;
        axis_storage[axis_index].actual_vendor_id = slave->eep_man;
        axis_storage[axis_index].actual_product_code = slave->eep_id;
        axis_storage[axis_index].actual_revision = slave->eep_rev;
        axis_storage[axis_index].identity_match = emaster_slave_identity_matches(
            axis->device_profile,
            &(emaster_slave_identity_t){slave->eep_man, slave->eep_id, slave->eep_rev});
        if (!axis_storage[axis_index].identity_match)
        {
            status = EMASTER_CONTROL_SESSION_IDENTITY_MISMATCH;
            goto cleanup;
        }
        if (!emaster_soem_discover_pdo_layout_recorded(&sdo,
                                                       &runtime[axis_index].layout))
        {
            status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
            goto cleanup;
        }
        layout_status =
            emaster_session_axis_validate_layout(axis, &runtime[axis_index].layout);
        if (layout_status == EMASTER_SESSION_LAYOUT_CONFIGURATION_REQUIRED &&
            axis->device_profile->supports_pdo_assignment)
        {
            emaster_soem_pdo_assignment_result_t assignment_result;

            emaster_pdo_layout_destroy(&runtime[axis_index].layout);
            if (!emaster_soem_assign_pdo_set_recorded(&sdo, axis->pdo_set,
                                                      &assignment_result))
            {
                axis_storage[axis_index].pdo_assignment_failed_index =
                    assignment_result.failed_index;
                axis_storage[axis_index].pdo_assignment_failed_subindex =
                    assignment_result.failed_subindex;
                axis_storage[axis_index].pdo_assignment_abort_code_available =
                    assignment_result.abort_code_available;
                axis_storage[axis_index].pdo_assignment_abort_code =
                    assignment_result.abort_code;
                status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
                goto cleanup;
            }
            if (
                !emaster_soem_discover_pdo_layout_recorded(
                    &sdo, &runtime[axis_index].layout))
            {
                status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
                goto cleanup;
            }
            layout_status =
                emaster_session_axis_validate_layout(axis, &runtime[axis_index].layout);
        }
        if (layout_status != EMASTER_SESSION_LAYOUT_MATCH ||
            !emaster_cia_process_image_init(axis, &runtime[axis_index]))
        {
            status = EMASTER_CONTROL_SESSION_PDO_MISMATCH;
            goto cleanup;
        }
        axis_storage[axis_index].pdo_match = true;

        if (!emaster_soem_write_u16(&sdo, UINT16_C(0x1C32), UINT8_C(0x01),
                                    axis->operation_profile->sm2_sync_type) ||
            !emaster_soem_write_u16(&sdo, UINT16_C(0x1C33), UINT8_C(0x01),
                                    axis->operation_profile->sm3_sync_type) ||
            (!axis->pdo_set->mode_init_on_safeop_to_op &&
             !emaster_soem_write_i8(&sdo, UINT16_C(0x6060), UINT8_C(0x00),
                                    axis->operation_mode->value)))
        {
            status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
            goto cleanup;
        }
        if (!emaster_soem_read_u16(&sdo, UINT16_C(0x1C32), UINT8_C(0x01),
                                   &sm2_value) ||
            !emaster_soem_read_u16(&sdo, UINT16_C(0x1C33), UINT8_C(0x01),
                                   &sm3_value) ||
            sm2_value != axis->operation_profile->sm2_sync_type ||
            sm3_value != axis->operation_profile->sm3_sync_type ||
            (!axis->pdo_set->mode_init_on_safeop_to_op &&
             (!emaster_soem_read_i8(&sdo, UINT16_C(0x6060), UINT8_C(0x00),
                                    &mode_value) ||
              mode_value != axis->operation_mode->value)))
        {
            status = EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED;
            goto cleanup;
        }
        emaster_session_observer_read_position_scale(
            &sdo, &axis_storage[axis_index].position_scale);
        if (plan->motion_profile != NULL)
        {
            if (!axis_storage[axis_index].position_scale.read_succeeded ||
                axis->motion_axis == NULL)
            {
                status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                goto cleanup;
            }
            axis_storage[axis_index].position_scale_match =
                emaster_position_scale_matches(
                    axis->motion_axis, &axis_storage[axis_index].position_scale);
            if (!axis_storage[axis_index].position_scale_match)
            {
                status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                goto cleanup;
            }
            motion_scales[axis_index] = axis_storage[axis_index].position_scale;
            motion_axis_configs[axis_index] = axis->motion_axis;
        }
        if (report->audit.allocation_failed)
        {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            goto cleanup;
        }
    }

    io_map_capacity = (size_t)EC_MAXIOSEGMENTS * (size_t)EC_MAXLRWDATA;
    if (io_map_capacity == 0U || io_map_capacity > SIZE_MAX / sizeof(*io_map))
    {
        status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
        goto cleanup;
    }
    io_map = calloc(io_map_capacity, sizeof(*io_map));
    if (io_map == NULL)
    {
        status = EMASTER_CONTROL_SESSION_OUT_OF_MEMORY;
        goto cleanup;
    }
    io_map_size = ecx_config_map_group(&context, io_map, 0U);
    if (io_map_size <= 0 || (size_t)io_map_size > io_map_capacity)
    {
        status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
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
        axis_storage[axis_index].output_bytes = slave->Obytes;
        axis_storage[axis_index].input_bytes = slave->Ibytes;
        axis_storage[axis_index].output_bits = slave->Obits;
        axis_storage[axis_index].input_bits = slave->Ibits;
        if (slave->outputs != NULL)
        {
            axis_storage[axis_index].output_offset_bytes =
                (size_t)(slave->outputs - io_map);
        }
        if (slave->inputs != NULL)
        {
            axis_storage[axis_index].input_offset_bytes =
                (size_t)(slave->inputs - io_map);
        }
        if (!axis_storage[axis_index].process_map_match)
        {
            status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
            goto cleanup;
        }
    }
    process_map_ready = true;
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        if (plan->axes[axis_index].operation_profile->sync_strategy == EMASTER_SYNC_STRATEGY_DC)
        {
            dc_required = true;
            axis_storage[axis_index].dc.requested = true;
            axis_storage[axis_index].dc.requested_cycle_ns = plan->cycle_ns;
            axis_storage[axis_index].dc.requested_shift_ns =
                plan->axes[axis_index].operation_profile->sync0_shift_ns;
            if (plan->axes[axis_index].operation_profile->assign_activate <= UINT16_MAX)
            {
                axis_storage[axis_index].dc.requested_assign_activate =
                    (uint16_t)plan->axes[axis_index].operation_profile->assign_activate;
            }
        }
    }
    report->dc_required = dc_required;
    if (dc_required)
    {
        uint32_t cycle_value;
        uint16_t activation;

        if (!ecx_configdc(&context))
        {
            status = EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED;
            goto cleanup;
        }
        report->dc_configured = true;
        report->dc_reference_slave = context.grouplist[0].DCnext;
        for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
        {
            const ec_slavet *slave = &context.slavelist[axis_index + 1U];

            if (plan->axes[axis_index].operation_profile->sync_strategy != EMASTER_SYNC_STRATEGY_DC)
            {
                continue;
            }
            axis_storage[axis_index].dc.slave_capable = slave->hasdc != 0U;
            if (slave->hasdc == 0U ||
                plan->axes[axis_index].operation_profile->assign_activate > UINT16_MAX)
            {
                status = EMASTER_CONTROL_SESSION_DC_CONFIG_FAILED;
                goto cleanup;
            }
            ecx_dcsync0(&context, (uint16_t)(axis_index + 1U), TRUE, plan->cycle_ns,
                        plan->axes[axis_index].operation_profile->sync0_shift_ns);
            runtime[axis_index].sync0_configured = true;
            axis_storage[axis_index].dc.sync0_requested = true;
            axis_storage[axis_index].dc.sync0_active = slave->DCactive != 0U;
            axis_storage[axis_index].dc.soem_shift_ns = slave->DCshift;
            /* 只要某一轴已经激活，后续任何失败路径都必须执行统一关闭。 */
            sync0_started = true;
            /* DCCUC 与 DCSYNCACT 是连续的 8 位寄存器；按 16 位读回才能核对完整
             * AssignActivate，而不是只确认 SOEM 当前默认激活的 Sync0 位。 */
            if (!emaster_session_observer_read_dc_register(
                    &context, (uint16_t)(axis_index + 1U), ECT_REG_DCCYCLE0,
                    &cycle_value, (uint16_t)sizeof(cycle_value), &report->audit,
                    EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, process_exchange) ||
                !emaster_session_observer_read_dc_register(
                    &context, (uint16_t)(axis_index + 1U), ECT_REG_DCCUC,
                    &activation, (uint16_t)sizeof(activation), &report->audit,
                    EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, process_exchange) ||
                etohl(cycle_value) != plan->cycle_ns ||
                etohs(activation) !=
                    (uint16_t)plan->axes[axis_index].operation_profile->assign_activate)
            {
                status = EMASTER_CONTROL_SESSION_SYNC0_CONFIG_FAILED;
                goto cleanup;
            }
            axis_storage[axis_index].dc.register_read_succeeded = true;
            axis_storage[axis_index].dc.observed_cycle_ns = etohl(cycle_value);
            axis_storage[axis_index].dc.observed_assign_activate = etohs(activation);
        }
        if (report->audit.allocation_failed)
        {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            goto cleanup;
        }
    }

    if (ecx_statecheck(&context, 0U, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4) !=
        EC_STATE_SAFE_OP)
    {
        status = EMASTER_CONTROL_SESSION_SAFE_OP_NOT_REACHED;
        goto cleanup;
    }
    report->safe_op_reached = true;

    /* ESI 的 SO 初始化命令必须在从站已到 SAFE-OP、请求 OP 之前执行。 */
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const emaster_pdo_set_profile_t *pdo_set = plan->axes[axis_index].pdo_set;
        const emaster_operation_mode_t *operation_mode =
            plan->axes[axis_index].operation_mode;
        int8_t mode_value;
        size_t command_index;
        emaster_soem_sdo_reader_context_t sdo;

        emaster_soem_sdo_context_init(
            &sdo, &context, (uint16_t)(axis_index + 1U), &report->audit,
            EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION, process_exchange);

        if (pdo_set->mode_init_on_safeop_to_op)
        {
            if (!emaster_soem_write_i8(&sdo, pdo_set->mode_init_index,
                                       pdo_set->mode_init_subindex,
                                       pdo_set->mode_init_value))
            {
                status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
                goto cleanup;
            }
            if (!emaster_soem_read_i8(&sdo, pdo_set->mode_init_index,
                                      pdo_set->mode_init_subindex,
                                      &mode_value) ||
                mode_value != pdo_set->mode_init_value)
            {
                status = EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED;
                goto cleanup;
            }
        }
        for (command_index = 0U;
             command_index < operation_mode->safeop_to_op_sdo_write_count;
             ++command_index)
        {
            const emaster_sdo_write_config_t *command =
                &operation_mode->safeop_to_op_sdo_writes[command_index];
            uint16_t observed_value;

            if (command->type != EMASTER_CONFIG_SDO_VALUE_U16 ||
                !emaster_soem_write_u16(&sdo, command->index, command->subindex,
                                        command->value_u16))
            {
                status = EMASTER_CONTROL_SESSION_SDO_WRITE_FAILED;
                goto cleanup;
            }
            if (!emaster_soem_read_u16(&sdo, command->index, command->subindex,
                                       &observed_value) ||
                observed_value != command->value_u16)
            {
                status = EMASTER_CONTROL_SESSION_SDO_READBACK_FAILED;
                goto cleanup;
            }
        }
        if (report->audit.allocation_failed)
        {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            goto cleanup;
        }
    }

    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const emaster_session_axis_plan_t *axis = &plan->axes[axis_index];
        ec_slavet *slave = &context.slavelist[axis_index + 1U];

        axis_storage[axis_index].output_initialized = emaster_cia_process_image_prepare_output(
            axis, &runtime[axis_index], slave->outputs, slave->Obytes);
        if (!axis_storage[axis_index].output_initialized)
        {
            status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
            goto cleanup;
        }
    }
    if (!emaster_session_observer_prepare_audit(
            plan, runtime, mode_confirmation_cycles, &report->audit))
    {
        status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
        goto cleanup;
    }
    report->expected_wkc = (uint16_t)(context.grouplist[0].outputsWKC * 2U +
                                      context.grouplist[0].inputsWKC);
    /* SAFE-OP 首次反馈用于锁定 CSP 当前实际位置，控制字仍保持为零。 */
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        if (!emaster_cia_process_image_audit_output(
                &runtime[axis_index], &report->audit,
                EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION,
                (uint16_t)(axis_index + 1U), process_exchange + UINT64_C(1)))
        {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            goto cleanup;
        }
    }
    ++process_exchange;
    (void)ecx_send_processdata(&context);
    report->actual_wkc = ecx_receive_processdata(&context, EC_TIMEOUTRET);
    if (report->actual_wkc != (int)report->expected_wkc)
    {
        status = EMASTER_CONTROL_SESSION_INITIAL_WKC_FAILED;
        goto cleanup;
    }
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const ec_slavet *slave = &context.slavelist[axis_index + 1U];
        int8_t mode_display;
        uint16_t status_word;
        int32_t actual_position;

        axis_storage[axis_index].input_decoded = emaster_cia_process_image_decode_input(
            &runtime[axis_index], slave->inputs, slave->Ibytes, &mode_display,
            &status_word, &actual_position);
        if (!axis_storage[axis_index].input_decoded)
        {
            status = EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
            goto cleanup;
        }
        if (!emaster_cia_process_image_audit_input(
                &runtime[axis_index], &report->audit,
                EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION,
                (uint16_t)(axis_index + 1U), process_exchange))
        {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            goto cleanup;
        }
        axis_storage[axis_index].requested_mode =
            plan->axes[axis_index].operation_mode->value;
        axis_storage[axis_index].mode_display = mode_display;
        axis_storage[axis_index].status_word = status_word;
        axis_storage[axis_index].initial_actual_position = actual_position;
        axis_storage[axis_index].actual_position = actual_position;
        axis_storage[axis_index].target_position = actual_position;
        if (plan->motion_profile != NULL)
        {
            actual_positions[axis_index] = actual_position;
            target_positions[axis_index] = actual_position;
        }
        if (!emaster_cia_process_image_update_output(&plan->axes[axis_index], &runtime[axis_index],
                                UINT16_C(0), actual_position,
                                context.slavelist[axis_index + 1U].outputs,
                                context.slavelist[axis_index + 1U].Obytes))
        {
            status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
            goto cleanup;
        }
    }
    cycle_output_active = true;
    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context, 0U);
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        if (!emaster_cia_process_image_audit_output(
                &runtime[axis_index], &report->audit,
                EMASTER_AUDIT_PHASE_OPERATION_REQUEST,
                (uint16_t)(axis_index + 1U), process_exchange + UINT64_C(1)))
        {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            goto cleanup;
        }
    }
    ++process_exchange;
    (void)ecx_send_processdata(&context);
    report->actual_wkc = ecx_receive_processdata(&context, EC_TIMEOUTRET);
    if (ecx_statecheck(&context, 0U, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE * 4) !=
        EC_STATE_OPERATIONAL)
    {
        status = EMASTER_CONTROL_SESSION_OP_NOT_REACHED;
        goto cleanup;
    }
    report->op_reached = true;
    enable_deadline_cycle = report->cycle_count + mode_confirmation_cycles;
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        const ec_slavet *slave = &context.slavelist[axis_index + 1U];
        int8_t mode_display;
        uint16_t status_word;
        int32_t actual_position;

        if (!emaster_cia_process_image_decode_input(
                &runtime[axis_index], slave->inputs, slave->Ibytes,
                &mode_display, &status_word, &actual_position) ||
            !emaster_cia_process_image_audit_input(
                &runtime[axis_index], &report->audit,
                EMASTER_AUDIT_PHASE_OPERATION_REQUEST,
                (uint16_t)(axis_index + 1U), process_exchange))
        {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            goto cleanup;
        }
    }
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        if (!emaster_cia402_controller_set_goal(&controllers[axis_index],
                                                EMASTER_CIA402_GOAL_OPERATION_ENABLED))
        {
            status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
            goto cleanup;
        }
    }
    {
        struct timespec deadline;

        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0 ||
            !timespec_add_ns(&deadline, plan->cycle_ns))
        {
            status = EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED;
            goto cleanup;
        }
        while (true)
        {
            emaster_multiaxis_frame_t frame;
            bool all_axes_enabled = true;
            bool all_modes_confirmed = true;

            if (!wait_for_cycle(&deadline) || !timespec_add_ns(&deadline, plan->cycle_ns))
            {
                status = EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED;
                goto cleanup;
            }
            for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
            {
                if (!emaster_cia_process_image_audit_output(
                        &runtime[axis_index], &report->audit,
                        EMASTER_AUDIT_PHASE_CYCLIC_OPERATION,
                        (uint16_t)(axis_index + 1U),
                        process_exchange + UINT64_C(1)))
                {
                    status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
                    goto cleanup;
                }
            }
            ++process_exchange;
            (void)ecx_send_processdata(&context);
            report->actual_wkc = ecx_receive_processdata(&context, EC_TIMEOUTRET);
            ++report->cycle_count;
            if (report->actual_wkc != (int)report->expected_wkc)
            {
                status = EMASTER_CONTROL_SESSION_INITIAL_WKC_FAILED;
                goto cleanup;
            }
            for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
            {
                const ec_slavet *slave = &context.slavelist[axis_index + 1U];
                int8_t mode_display;
                uint16_t status_word;
                int32_t actual_position;

                axis_storage[axis_index].input_decoded = emaster_cia_process_image_decode_input(
                    &runtime[axis_index], slave->inputs, slave->Ibytes, &mode_display,
                    &status_word, &actual_position);
                if (!axis_storage[axis_index].input_decoded)
                {
                    status = EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
                    goto cleanup;
                }
                if (!emaster_cia_process_image_audit_input(
                        &runtime[axis_index], &report->audit,
                        EMASTER_AUDIT_PHASE_CYCLIC_OPERATION,
                        (uint16_t)(axis_index + 1U), process_exchange))
                {
                    status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
                    goto cleanup;
                }
                axis_storage[axis_index].mode_display = mode_display;
                axis_storage[axis_index].status_word = status_word;
                axis_storage[axis_index].actual_position = actual_position;
                status_words[axis_index] = status_word;
                if (plan->motion_profile != NULL)
                {
                    actual_positions[axis_index] = actual_position;
                }
            }

            frame.sequence = report->cycle_count;
            frame.deadline_ns = UINT64_MAX;
            frame.axis_count = plan->axis_count;
            frame.status_words = status_words;
            frame.outputs = controller_outputs;
            if (emaster_multiaxis_coordinator_step(&coordinator, &frame, UINT64_C(0)) !=
                EMASTER_MULTIAXIS_OK)
            {
                status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
                goto cleanup;
            }
            for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
            {
                emaster_control_session_axis_result_t *axis_result =
                    &axis_storage[axis_index];
                bool operation_enabled =
                    controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_OPERATION_ENABLED;

                axis_result->control_word = controller_outputs[axis_index].control_word;
                axis_result->cia402_state = controller_outputs[axis_index].observed_state;
                axis_result->operation_enabled_seen |= operation_enabled;
                axis_result->switch_on_disabled_seen |=
                    controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_SWITCH_ON_DISABLED;
                axis_result->ready_to_switch_on_seen |=
                    controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_READY_TO_SWITCH_ON;
                axis_result->switched_on_seen |=
                    controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_SWITCHED_ON;
                if (!runtime[axis_index].tx_mode_available && operation_enabled &&
                    (!axis_result->mode_display_sdo_read ||
                     (mode_confirmation_started && !axis_result->mode_display_match &&
                      report->cycle_count >= mode_confirmation_deadline_cycle)))
                {
                    emaster_soem_sdo_reader_context_t sdo;

                    emaster_soem_sdo_context_init(
                        &sdo, &context, (uint16_t)(axis_index + 1U),
                        &report->audit, EMASTER_AUDIT_PHASE_CYCLIC_OPERATION,
                        process_exchange);
                    axis_result->mode_display_sdo_read = emaster_soem_read_i8(
                        &sdo, UINT16_C(0x6061), UINT8_C(0),
                        &axis_result->mode_display_sdo);
                    axis_result->mode_display = axis_result->mode_display_sdo;
                }
                axis_result->mode_display_match =
                    runtime[axis_index].tx_mode_available
                        ? axis_result->mode_display ==
                              plan->axes[axis_index].operation_mode->value
                        : axis_result->mode_display_sdo_read &&
                              axis_result->mode_display ==
                                  plan->axes[axis_index].operation_mode->value;
                if (!axis_result->mode_display_match &&
                    plan->axes[axis_index].operation_mode->mode_display_policy ==
                        EMASTER_MODE_DISPLAY_REQUIRED)
                {
                    all_modes_confirmed = false;
                }
                if (controller_outputs[axis_index].fault_present)
                {
                    status = EMASTER_CONTROL_SESSION_DRIVE_FAULT;
                    goto cleanup;
                }
                if (!operation_enabled)
                {
                    all_axes_enabled = false;
                }
            }
            report->all_axes_enabled_reached |= all_axes_enabled;
            if (!all_axes_enabled && report->cycle_count >= enable_deadline_cycle)
            {
                status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
                goto cleanup;
            }
            if (all_axes_enabled && !mode_confirmation_started)
            {
                mode_confirmation_started = true;
                mode_confirmation_deadline_cycle =
                    report->cycle_count + mode_confirmation_cycles;
            }
            if (mode_confirmation_started && !all_modes_confirmed &&
                report->cycle_count >= mode_confirmation_deadline_cycle)
            {
                status = EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
                goto cleanup;
            }
            if (plan->motion_profile != NULL && all_axes_enabled && all_modes_confirmed)
            {
                emaster_relative_motion_status_t motion_status;

                if (!motion_initialized)
                {
                    int32_t *initial_positions = target_positions;

                    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
                    {
                        initial_positions[axis_index] =
                            axis_storage[axis_index].initial_actual_position;
                    }
                    motion_status = emaster_relative_motion_init(
                        plan->motion_profile, motion_axis_configs, motion_scales,
                        initial_positions, plan->axis_count, plan->cycle_ns,
                        motion_axes, &motion);
                    if (motion_status != EMASTER_RELATIVE_MOTION_ACTIVE)
                    {
                        status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                        goto cleanup;
                    }
                    motion_initialized = true;
                    report->motion_started = true;
                    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
                    {
                        axis_storage[axis_index].motion_final_position =
                            motion_axes[axis_index].final_position;
                        axis_storage[axis_index].max_following_error_counts =
                            motion_axes[axis_index].max_following_error_counts;
                    }
                }
                motion_status = emaster_relative_motion_step(
                    &motion, actual_positions, target_positions, plan->axis_count);
                /* 即使本周期因跟随误差退出，也要把触发值保留到会话报告。 */
                for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
                {
                    axis_storage[axis_index].max_observed_following_error_counts =
                        motion_axes[axis_index].max_observed_following_error_counts;
                }
                if (motion_status == EMASTER_RELATIVE_MOTION_FOLLOWING_ERROR)
                {
                    status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                    goto cleanup;
                }
                if (motion_status != EMASTER_RELATIVE_MOTION_ACTIVE &&
                    motion_status != EMASTER_RELATIVE_MOTION_SETTLING &&
                    motion_status != EMASTER_RELATIVE_MOTION_COMPLETE)
                {
                    status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                    goto cleanup;
                }
                for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
                {
                    axis_storage[axis_index].target_position =
                        target_positions[axis_index];
                }
                report->motion_completed =
                    motion_status == EMASTER_RELATIVE_MOTION_COMPLETE;
                if (report->motion_completed)
                {
                    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
                    {
                        emaster_control_session_axis_result_t *axis_result =
                            &axis_storage[axis_index];
                        const int32_t planned_angle =
                            motion_axis_configs[axis_index]->relative_angle_millidegrees;

                        axis_result->motion_completion_actual_position =
                            axis_result->actual_position;
                        axis_result->motion_actual_delta_counts =
                            (int64_t)axis_result->motion_completion_actual_position -
                            (int64_t)axis_result->initial_actual_position;
                        axis_result->motion_final_error_counts = absolute_position_difference(
                            axis_result->motion_completion_actual_position,
                            axis_result->motion_final_position);
                        axis_result->motion_direction_match =
                            (planned_angle > 0 &&
                             axis_result->motion_actual_delta_counts > 0) ||
                            (planned_angle < 0 &&
                             axis_result->motion_actual_delta_counts < 0);
                        if (!axis_result->motion_direction_match ||
                            axis_result->motion_final_error_counts >
                                axis_result->max_following_error_counts)
                        {
                            status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                            goto cleanup;
                        }
                    }
                }
            }
            for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
            {
                emaster_control_session_axis_result_t *axis_result =
                    &axis_storage[axis_index];

                if (!emaster_cia_process_image_update_output(
                        &plan->axes[axis_index], &runtime[axis_index],
                        controller_outputs[axis_index].control_word,
                        axis_result->target_position,
                        context.slavelist[axis_index + 1U].outputs,
                        context.slavelist[axis_index + 1U].Obytes))
                {
                    status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
                    goto cleanup;
                }
            }
            if (report->motion_completed)
            {
                break;
            }
            if (stop_requested != NULL && stop_requested(stop_user_data))
            {
                report->stop_requested = true;
                break;
            }
        }
    }

cleanup:
    if (context_open)
    {
        if (process_map_ready && cycle_output_active)
        {
            report->safe_state_reached = stop_process_data(
                plan, runtime, axis_storage, controllers, controller_outputs, status_words,
                &coordinator, &context, report, &safe_output_sent,
                &process_exchange);
            if (!report->safe_state_reached && status == EMASTER_CONTROL_SESSION_OK)
            {
                status = EMASTER_CONTROL_SESSION_SAFE_STOP_FAILED;
            }
        }
        if (report->op_reached)
        {
            for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
            {
                emaster_control_session_axis_result_t *axis_result =
                    &axis_storage[axis_index];
                emaster_soem_sdo_reader_context_t sdo;

                emaster_soem_sdo_context_init(
                    &sdo, &context, (uint16_t)(axis_index + 1U), &report->audit,
                    EMASTER_AUDIT_PHASE_FINAL_DIAGNOSTIC, process_exchange);
                axis_result->mode_command_sdo_read = emaster_soem_read_i8(
                    &sdo, UINT16_C(0x6060), UINT8_C(0),
                    &axis_result->mode_command_sdo);
                axis_result->mode_display_sdo_read = emaster_soem_read_i8(
                    &sdo, UINT16_C(0x6061), UINT8_C(0),
                    &axis_result->mode_display_sdo);
                axis_result->input_mode_sdo_read = emaster_soem_read_u16(
                    &sdo, UINT16_C(0x2002), UINT8_C(1),
                    &axis_result->input_mode_sdo);
                if (!runtime[axis_index].tx_mode_available &&
                    axis_result->mode_display_sdo_read)
                {
                    axis_result->mode_display = axis_result->mode_display_sdo;
                    axis_result->mode_display_match =
                        axis_result->mode_display == axis_result->requested_mode;
                }
                /*
                 * 此时安全停机过程已经结束，但从站仍在 EtherCAT 会话中且 Sync0 尚未关闭；
                 * 因而这里能保留本次运行产生的驱动故障和同步质量证据。
                 */
                emaster_session_observer_read_drive(
                    &sdo, &axis_result->drive_diagnostic);
                emaster_session_observer_read_sync(
                    &sdo, UINT16_C(0x1C32), &axis_result->sm2_diagnostic);
                emaster_session_observer_read_sync(
                    &sdo, UINT16_C(0x1C33), &axis_result->sm3_diagnostic);
                if (!axis_result->position_scale.read_succeeded)
                {
                    emaster_session_observer_read_position_scale(
                        &sdo, &axis_result->position_scale);
                }
                axis_result->final_diagnostic_read_count =
                    plan->axes[axis_index].operation_mode->final_sdo_read_count;
                for (size_t diagnostic_index = 0U;
                     diagnostic_index <
                         plan->axes[axis_index].operation_mode->final_sdo_read_count;
                     ++diagnostic_index)
                {
                    if (emaster_session_observer_read_configured_sdo(
                            &sdo,
                            &plan->axes[axis_index].operation_mode
                                 ->final_sdo_reads[diagnostic_index]))
                    {
                        ++axis_result->final_diagnostic_success_count;
                    }
                }
            }
        }
        report->safe_output_sent = safe_output_sent;
        if (sync0_started)
        {
            disable_sync0(&context, runtime, plan->axis_count);
            report->sync0_disabled = true;
            for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
            {
                /* 配置后的激活事实保留在轴结果中，最终关闭事实由会话结果单独表示。 */
            }
        }
        report->last_dc_time_ns = context.DCtime;
        report->restore_init_succeeded = emaster_soem_restore_init(&context);
        ecx_close(&context);
    }
    free(status_words);
    free(motion_axes);
    free(motion_axis_configs);
    free(motion_scales);
    free(target_positions);
    free(actual_positions);
    free(controller_outputs);
    free(controllers);
    free(io_map);
    emaster_cia_process_image_destroy(runtime, plan->axis_count);
    report->process_data_exchange_count = process_exchange;
    if (report->audit.allocation_failed && status == EMASTER_CONTROL_SESSION_OK)
    {
        status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
    }
    report->status = status == EMASTER_CONTROL_SESSION_OK && !report->restore_init_succeeded
                         ? EMASTER_CONTROL_SESSION_RESTORE_INIT_FAILED
                         : status;
    return report->status;
}

void emaster_control_session_report_destroy(emaster_control_session_report_t *report)
{
    if (report != NULL)
    {
        emaster_run_audit_destroy(&report->audit);
    }
}
