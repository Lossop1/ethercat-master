#include "emaster/session/session_plan.h"

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
        return operation->has_sync0_shift_ns;
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

emaster_session_plan_status_t
emaster_session_plan_build(const emaster_deployment_config_t *deployment,
                           emaster_session_axis_plan_t *axis_storage,
                           size_t axis_capacity, emaster_session_plan_t *result)
{
    const emaster_topology_config_t *topology;
    uint32_t cycle_ns = 0U;
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
        if (operation_mode == NULL)
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_OPERATION_PROFILE_INCOMPLETE);
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
        if (axis_index == 0U)
        {
            cycle_ns = operation->cycle_ns;
        }
        else if (cycle_ns != operation->cycle_ns)
        {
            return fail_plan(result, axis_storage, topology->slave_count,
                             EMASTER_SESSION_PLAN_CYCLE_TIME_MISMATCH);
        }

        axis_storage[axis_index].topology_slave = topology_slave;
        axis_storage[axis_index].device_profile = device;
        axis_storage[axis_index].operation_profile = operation;
        axis_storage[axis_index].operation_mode = operation_mode;
        axis_storage[axis_index].pdo_set = pdo_set;
    }

    result->status = EMASTER_SESSION_PLAN_READY;
    result->deployment = deployment;
    result->axes = axis_storage;
    result->axis_count = topology->slave_count;
    result->cycle_ns = cycle_ns;
    return result->status;
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
    return axis->device_profile->supports_pdo_configuration
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
