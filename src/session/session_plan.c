#include "emaster/session/session_plan.h"

#include "emaster/cia402/controller.h"

#include <stdbool.h>
#include <string.h>

static emaster_session_plan_status_t
fail_plan(emaster_session_plan_t *result, emaster_session_axis_plan_t *axes,
          size_t axis_count, emaster_session_plan_status_t status)
{
    if (axes != NULL && axis_count > 0U)
    {
        memset(axes, 0, axis_count * sizeof(*axes));
    }
    if (result != NULL)
    {
        memset(result, 0, sizeof(*result));
        result->status = status;
    }
    return status;
}

/*
 * 带失败现场的收尾。只有模式类失败用它 —— 那两种失败的下一步取决于"缺的是哪一边"，
 * 不点名就只能让操作员把三份配置从头翻一遍。
 */
static emaster_session_plan_status_t
fail_mode_plan(emaster_session_plan_t *result, emaster_session_axis_plan_t *axes,
               size_t axis_count, emaster_session_plan_status_t status,
               size_t axis_ordinal, const emaster_operation_mode_t *mode)
{
    fail_plan(result, axes, axis_count, status);
    if (result != NULL)
    {
        result->failing_axis_ordinal = axis_ordinal;
        result->failing_mode_id = mode->mode_id;
        result->failing_mode_value = mode->value;
    }
    return status;
}

/* 设备档声明的模式集合里有没有这个取值。设备档缺集合时按"没声明"处理，不许瞎放行。 */
static bool device_declares_mode(const emaster_slave_profile_t *device, int8_t mode_value)
{
    size_t index;

    if (device->supported_modes == NULL)
    {
        return false;
    }
    for (index = 0U; index < device->supported_mode_count; ++index)
    {
        if (device->supported_modes[index] == mode_value)
        {
            return true;
        }
    }
    return false;
}

static emaster_session_plan_status_t
find_operation_profile(const emaster_deployment_config_t *deployment,
                       const char *device_profile_id,
                       const emaster_operation_profile_t **result)
{
    const emaster_operation_profile_t *match = NULL;
    size_t index;

    if (deployment->operation_profile_count > 0U &&
        deployment->operation_profiles == NULL)
    {
        return EMASTER_SESSION_PLAN_OPERATION_PROFILE_INCOMPLETE;
    }
    for (index = 0U; index < deployment->operation_profile_count; ++index)
    {
        const emaster_operation_profile_t *candidate =
            deployment->operation_profiles[index];

        if (candidate == NULL || candidate->device_profile_id == NULL)
        {
            return EMASTER_SESSION_PLAN_OPERATION_PROFILE_INCOMPLETE;
        }
        if (strcmp(candidate->device_profile_id, device_profile_id) == 0)
        {
            if (match != NULL)
            {
                return EMASTER_SESSION_PLAN_OPERATION_PROFILE_AMBIGUOUS;
            }
            match = candidate;
        }
    }
    if (match == NULL)
    {
        return EMASTER_SESSION_PLAN_OPERATION_PROFILE_NOT_FOUND;
    }
    *result = match;
    return EMASTER_SESSION_PLAN_READY;
}

static bool operation_profile_is_complete(
    const emaster_operation_profile_t *operation)
{
    bool common_parameters_present;

    if (operation == NULL || operation->profile_id == NULL ||
        operation->device_profile_id == NULL || operation->pdo_set_id == NULL ||
        operation->selected_mode_id == NULL ||
        operation->modes == NULL || operation->mode_count == 0U)
    {
        return false;
    }
    common_parameters_present = operation->has_assign_activate &&
                                operation->has_cycle_ns && operation->cycle_ns > 0U &&
                                operation->has_sm2_sync_type &&
                                operation->has_sm3_sync_type;
    if (!common_parameters_present)
    {
        return false;
    }
    if (operation->sync_strategy == EMASTER_SYNC_STRATEGY_DC)
    {
        return operation->has_sync0_shift_ns &&
               operation->has_process_data_phase_ns &&
               operation->process_data_phase_ns < operation->cycle_ns &&
               operation->has_dc_startup_cycles &&
               operation->dc_startup_cycles > 0U;
    }
    return operation->sync_strategy == EMASTER_SYNC_STRATEGY_SM;
}

static const emaster_operation_mode_t *
find_operation_mode(const emaster_operation_profile_t *operation)
{
    size_t index;

    for (index = 0U; index < operation->mode_count; ++index)
    {
        const emaster_operation_mode_t *mode = &operation->modes[index];
        if (mode->mode_id != NULL &&
            strcmp(mode->mode_id, operation->selected_mode_id) == 0)
        {
            return mode;
        }
    }
    return NULL;
}

static const emaster_motion_axis_config_t *
find_motion_axis(const emaster_motion_profile_t *motion, const char *axis_id)
{
    const emaster_motion_axis_config_t *match = NULL;
    size_t index;

    if (motion == NULL || motion->axes == NULL || axis_id == NULL)
    {
        return NULL;
    }
    for (index = 0U; index < motion->axis_count; ++index)
    {
        const emaster_motion_axis_config_t *candidate = &motion->axes[index];
        if (candidate->axis_id != NULL && strcmp(candidate->axis_id, axis_id) == 0)
        {
            if (match != NULL)
            {
                return NULL;
            }
            match = candidate;
        }
    }
    return match;
}

emaster_session_plan_status_t
emaster_session_plan_build(const emaster_deployment_config_t *deployment,
                           emaster_session_axis_plan_t *axis_storage,
                           size_t axis_capacity, emaster_session_plan_t *result)
{
    const emaster_topology_config_t *topology;
    uint32_t cycle_ns = 0U;
    uint32_t process_data_phase_ns = 0U;
    uint32_t dc_startup_cycles = 0U;
    bool dc_timing_initialized = false;
    size_t axis_index;

    if (result == NULL)
    {
        return EMASTER_SESSION_PLAN_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->status = EMASTER_SESSION_PLAN_INVALID_ARGUMENT;
    if (deployment == NULL)
    {
        return result->status;
    }
    topology = deployment->topology;
    if (topology == NULL || topology->slaves == NULL || topology->slave_count == 0U)
    {
        return fail_plan(result, NULL, 0U, EMASTER_SESSION_PLAN_INVALID_TOPOLOGY);
    }
    if (deployment->operation_profile_count == 0U)
    {
        return fail_plan(result, NULL, 0U, EMASTER_SESSION_PLAN_DISABLED);
    }
    if (deployment->motion_profile != NULL &&
        deployment->motion_profile->approval != EMASTER_MOTION_PROFILE_APPROVED)
    {
        return fail_plan(result, NULL, 0U,
                         EMASTER_SESSION_PLAN_MOTION_PROFILE_NOT_APPROVED);
    }
    if (deployment->motion_profile != NULL &&
        (deployment->motion_profile->axes == NULL ||
         deployment->motion_profile->required_mode_id == NULL ||
         deployment->motion_profile->axis_count != topology->slave_count ||
         deployment->motion_profile->duration_ms == 0U))
    {
        return fail_plan(result, NULL, 0U,
                         EMASTER_SESSION_PLAN_MOTION_PROFILE_INCOMPLETE);
    }
    if (axis_storage == NULL)
    {
        return fail_plan(result, NULL, 0U, EMASTER_SESSION_PLAN_INVALID_ARGUMENT);
    }
    if (axis_capacity < topology->slave_count)
    {
        return fail_plan(result, NULL, 0U,
                         EMASTER_SESSION_PLAN_AXIS_STORAGE_TOO_SMALL);
    }
    memset(axis_storage, 0, topology->slave_count * sizeof(*axis_storage));

    for (axis_index = 0U; axis_index < topology->slave_count; ++axis_index)
    {
        const emaster_topology_slave_config_t *topology_slave =
            &topology->slaves[axis_index];
        const emaster_slave_profile_t *device;
        const emaster_operation_profile_t *operation = NULL;
        const emaster_operation_mode_t *operation_mode;
        const emaster_pdo_set_profile_t *pdo_set;
        const emaster_motion_axis_config_t *motion_axis = NULL;
        emaster_session_plan_status_t status;

        if (topology_slave->profile_id == NULL)
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_INVALID_TOPOLOGY);
        }
        device = emaster_slave_profile_by_id(topology_slave->profile_id);
        if (device == NULL)
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_DEVICE_PROFILE_NOT_FOUND);
        }
        status = find_operation_profile(deployment, topology_slave->profile_id,
                                        &operation);
        if (status != EMASTER_SESSION_PLAN_READY)
        {
            return fail_plan(result, axis_storage, topology->slave_count, status);
        }
        if (operation->approval != EMASTER_OPERATION_PROFILE_APPROVED)
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_OPERATION_PROFILE_NOT_APPROVED);
        }
        if (!operation_profile_is_complete(operation))
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_OPERATION_PROFILE_INCOMPLETE);
        }
        operation_mode = find_operation_mode(operation);
        if (operation_mode == NULL ||
            (operation_mode->safeop_to_op_sdo_write_count > 0U &&
             operation_mode->safeop_to_op_sdo_writes == NULL) ||
            (operation_mode->final_sdo_read_count > 0U &&
             operation_mode->final_sdo_reads == NULL))
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_OPERATION_PROFILE_INCOMPLETE);
        }
        /*
         * 模式能力是两个独立的事实，缺哪一边决定了下一步该做什么，所以分开报。
         *
         * 设备这一边：设备档声明它认哪几个 0x6060 取值。这个事实推不出来又必须准 ——
         * ESI 的模块类型只有模块化设备才写，非模块化设备一个字都没有；而同一个取值在
         * 两家的说明书里可能指的不是同一件事（台架那台 MT 的通道 0x1601 是 PVT 控制
         * 通道，良质的 0x1601 是精简 CSP）。所以运行期不按型号分支、也不猜通道号，
         * 只认档里这一份声明。它不在里面，就是这份配置配错了设备。
         *
         * 主站这一边：值认出来了，还得有把目标字段绑进过程映像的能力。8/9/10 各有目标，
         * 模式 6（回零）没有目标、只写控制字与模式本身（见 cia402/controller.h）。
         * 缺口在主站这一侧，跟设备没关系。
         */
        if (!device_declares_mode(device, operation_mode->value))
        {
            return fail_mode_plan(result, axis_storage, topology->slave_count,
                                  EMASTER_SESSION_PLAN_MODE_NOT_DECLARED,
                                  axis_index, operation_mode);
        }
        if (!emaster_cia402_mode_is_supported(operation_mode->value))
        {
            return fail_mode_plan(result, axis_storage, topology->slave_count,
                                  EMASTER_SESSION_PLAN_MODE_NOT_DRIVABLE,
                                  axis_index, operation_mode);
        }
        if (operation->sync_strategy == EMASTER_SYNC_STRATEGY_DC &&
            !device->supports_distributed_clocks)
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_DC_NOT_SUPPORTED);
        }
        pdo_set = emaster_slave_pdo_set_by_id(device, operation->pdo_set_id);
        if (pdo_set == NULL)
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_PDO_SET_NOT_FOUND);
        }
        /* 槽位只在模块化分配里有含义；直连分配恒为 0，那不是缺项。 */
        if ((pdo_set->assignment_style == EMASTER_PDO_ASSIGNMENT_MODULE_SLOT &&
             pdo_set->module_slot == 0U) ||
            (pdo_set->mode_init_on_safeop_to_op &&
             (pdo_set->mode_init_index == 0U ||
              pdo_set->mode_init_value != operation_mode->value)))
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_OPERATION_PROFILE_INCOMPLETE);
        }
        if (deployment->motion_profile != NULL)
        {
            if (operation_mode->mode_id == NULL ||
                strcmp(operation_mode->mode_id,
                       deployment->motion_profile->required_mode_id) != 0)
            {
                return fail_plan(result, axis_storage, topology->slave_count,
                                 EMASTER_SESSION_PLAN_MOTION_PROFILE_INCOMPLETE);
            }
            motion_axis = find_motion_axis(deployment->motion_profile,
                                           topology_slave->axis_id);
            if (motion_axis == NULL)
            {
                return fail_plan(result, axis_storage, topology->slave_count,
                                 EMASTER_SESSION_PLAN_MOTION_PROFILE_INCOMPLETE);
            }
        }
        if (axis_index == 0U)
        {
            cycle_ns = operation->cycle_ns;
        }
        else if (cycle_ns != operation->cycle_ns)
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_CYCLE_TIME_MISMATCH);
        }
        if (operation->sync_strategy == EMASTER_SYNC_STRATEGY_DC)
        {
            if (!dc_timing_initialized)
            {
                process_data_phase_ns = operation->process_data_phase_ns;
                dc_timing_initialized = true;
            }
            else if (process_data_phase_ns != operation->process_data_phase_ns)
            {
                return fail_plan(result, axis_storage, topology->slave_count,
                                 EMASTER_SESSION_PLAN_DC_PHASE_MISMATCH);
            }
            if (dc_startup_cycles < operation->dc_startup_cycles)
            {
                dc_startup_cycles = operation->dc_startup_cycles;
            }
        }

        axis_storage[axis_index].topology_slave = topology_slave;
        axis_storage[axis_index].device_profile = device;
        axis_storage[axis_index].operation_profile = operation;
        axis_storage[axis_index].operation_mode = operation_mode;
        axis_storage[axis_index].pdo_set = pdo_set;
        axis_storage[axis_index].motion_axis = motion_axis;
    }

    result->status = EMASTER_SESSION_PLAN_READY;
    result->deployment = deployment;
    result->axes = axis_storage;
    result->axis_count = topology->slave_count;
    result->cycle_ns = cycle_ns;
    result->process_data_phase_ns = process_data_phase_ns;
    result->dc_startup_cycles = dc_startup_cycles;
    result->motion_profile = deployment->motion_profile;
    result->fault_policy = deployment->fault_policy;  /* P2.5: 传递故障策略 */
    return result->status;
}

bool emaster_session_plan_has_cyclic_targets(const emaster_session_plan_t *plan)
{
    size_t axis_index;

    if (plan == NULL || plan->axes == NULL || plan->axis_count == 0U)
    {
        return false;
    }
    for (axis_index = 0U; axis_index < plan->axis_count; ++axis_index)
    {
        /*
         * "有没有周期目标"就是宣不声明量：声明了 none 的模式（回零）没有周期目标。
         * 不按模式号认——同一个号在不同厂商的说明书里指的不是同一件事。
         */
        if (plan->axes[axis_index].operation_mode == NULL ||
            plan->axes[axis_index].operation_mode->quantity == EMASTER_MODE_QUANTITY_NONE)
        {
            return false;
        }
    }
    return true;
}

bool emaster_session_axis_mode_allows_control(const emaster_session_axis_plan_t *axis,
                                              bool mode_matches)
{
    if (axis == NULL || axis->operation_mode == NULL ||
        (axis->operation_mode->mode_display_policy != EMASTER_MODE_DISPLAY_REQUIRED &&
         axis->operation_mode->mode_display_policy != EMASTER_MODE_DISPLAY_DIAGNOSTIC))
    {
        return false;
    }
    return mode_matches ||
           (axis->motion_axis == NULL &&
            axis->operation_mode->mode_display_policy == EMASTER_MODE_DISPLAY_DIAGNOSTIC);
}

/* 设备档里名字叫 name 的那一条；找不到说明运行方案与设备档本身对不上。 */
static const emaster_pdo_entry_t *declared_entry_named(
    const emaster_pdo_mapping_profile_t *mappings, size_t mapping_count, const char *name)
{
    size_t mapping_ordinal;

    if (mappings == NULL || name == NULL)
    {
        return NULL;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_profile_t *mapping = &mappings[mapping_ordinal];
        size_t entry_ordinal;

        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_entry_t *entry = &mapping->entries[entry_ordinal];

            if (entry->name != NULL && strcmp(entry->name, name) == 0)
            {
                return entry;
            }
        }
    }
    return NULL;
}

/* 这一条要用的对象，设备当前映射里有没有、位宽对不对。按对象号找，不看位置。 */
static bool device_provides_field(const emaster_pdo_direction_layout_t *actual,
                                  const emaster_pdo_mapping_profile_t *mappings,
                                  size_t mapping_count, const char *name)
{
    const emaster_pdo_entry_t *declared = declared_entry_named(mappings, mapping_count, name);
    size_t mapping_ordinal;

    if (declared == NULL || actual == NULL)
    {
        return false;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < actual->mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &actual->mappings[mapping_ordinal];
        size_t entry_ordinal;

        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];

            if (entry->object_index == declared->index &&
                entry->object_subindex == declared->subindex)
            {
                return entry->bit_length == declared->bit_length;
            }
        }
    }
    return false;
}

/*
 * 设备当前生效的映射能不能直接用。
 *
 * 只问两件事：方向总长对不对；运行方案真正依赖的每个字段在不在、位宽对不对（按对象号
 * 找，不看位置）。设备多映射的、或与声明标签不同而主站从不读的字节（填充）不影响这个
 * 判断 —— 映像里每个字段的偏移都由设备实际布局现算，不由声明的位置假设。
 *
 * 这一条存在的理由：厂商 ESI 与固件出厂默认可以逐条不同，而设备未必允许改写。MT 台架
 * 实测：ESI 的 0x1600 第 7 条是假字节 0x2FFD，设备映射 0x5FFE；且 0x1600:00 是只读对象，
 * 改写被中止码 0x06010002 拒绝。差异只落在谁都不读的字节上时，映像照样能用。
 */
static bool layout_usable_with_device(const emaster_session_axis_plan_t *axis,
                                      const emaster_pdo_layout_t *actual)
{
    size_t field_ordinal;

    if (actual->rx.bit_length != (uint32_t)axis->pdo_set->rx_pdo_bytes * UINT32_C(8) ||
        actual->tx.bit_length != (uint32_t)axis->pdo_set->tx_pdo_bytes * UINT32_C(8))
    {
        return false;
    }
    for (field_ordinal = 0U; field_ordinal < axis->operation_mode->required_rx_field_count;
         ++field_ordinal)
    {
        if (!device_provides_field(&actual->rx, axis->pdo_set->rx_mappings,
                                   axis->pdo_set->rx_mapping_count,
                                   axis->operation_mode->required_rx_fields[field_ordinal]))
        {
            return false;
        }
    }
    for (field_ordinal = 0U; field_ordinal < axis->operation_mode->required_tx_field_count;
         ++field_ordinal)
    {
        if (!device_provides_field(&actual->tx, axis->pdo_set->tx_mappings,
                                   axis->pdo_set->tx_mapping_count,
                                   axis->operation_mode->required_tx_fields[field_ordinal]))
        {
            return false;
        }
    }
    return true;
}

emaster_session_layout_status_t
emaster_session_axis_validate_layout(const emaster_session_axis_plan_t *axis,
                                     const emaster_pdo_layout_t *actual)
{
    if (axis == NULL || axis->device_profile == NULL || axis->pdo_set == NULL ||
        actual == NULL)
    {
        return EMASTER_SESSION_LAYOUT_INVALID_ARGUMENT;
    }
    if (actual->status != EMASTER_PDO_DISCOVERY_COMPLETE)
    {
        return EMASTER_SESSION_LAYOUT_DISCOVERY_INCOMPLETE;
    }
    if (emaster_slave_pdo_set_layout_matches(axis->pdo_set, actual))
    {
        return EMASTER_SESSION_LAYOUT_MATCH;
    }
    if (layout_usable_with_device(axis, actual))
    {
        return EMASTER_SESSION_LAYOUT_DEVICE_ADOPTED;
    }
    return (axis->device_profile->supports_pdo_assignment ||
            axis->device_profile->supports_pdo_configuration)
               ? EMASTER_SESSION_LAYOUT_CONFIGURATION_REQUIRED
               : EMASTER_SESSION_LAYOUT_CONFIGURATION_UNSUPPORTED;
}

size_t emaster_session_axis_output_byte_count(const emaster_session_axis_plan_t *axis)
{
    return axis == NULL || axis->pdo_set == NULL ? 0U
                                                 : (size_t)axis->pdo_set->rx_pdo_bytes;
}

emaster_session_zero_output_status_t
emaster_session_axis_prepare_zero_output(const emaster_session_axis_plan_t *axis,
                                         uint8_t *buffer, size_t buffer_capacity)
{
    size_t required_size = emaster_session_axis_output_byte_count(axis);

    if (required_size == 0U || buffer == NULL)
    {
        return EMASTER_SESSION_ZERO_OUTPUT_INVALID_ARGUMENT;
    }
    if (buffer_capacity < required_size)
    {
        return EMASTER_SESSION_ZERO_OUTPUT_BUFFER_TOO_SMALL;
    }
    memset(buffer, 0, required_size);
    return EMASTER_SESSION_ZERO_OUTPUT_OK;
}
