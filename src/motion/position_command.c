#include "emaster/motion/position_command.h"

#include <limits.h>
#include <string.h>

static bool axis_model_is_valid(
    const emaster_position_command_axis_model_t *model)
{
    if (model == NULL ||
        (model->polarity != INT8_C(1) && model->polarity != INT8_C(-1)) ||
        (model->coordinate != EMASTER_MOTION_COORDINATE_MOTOR_ROTOR &&
         model->coordinate != EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT))
    {
        return false;
    }
    if (!model->scale.read_succeeded || model->scale.encoder_increments == 0U ||
        model->scale.encoder_motor_revolutions == 0U)
    {
        return false;
    }
    if (model->coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT &&
        (model->scale.gear_motor_revolutions == 0U ||
         model->scale.gear_shaft_revolutions == 0U))
    {
        return false;
    }
    return !model->limits_enabled || model->minimum_counts <= model->maximum_counts;
}

emaster_position_target_status_t emaster_position_command_target_from_angle(
    const emaster_position_command_axis_model_t *model,
    emaster_position_command_target_kind_t kind,
    int32_t reference_counts,
    int32_t angle_millidegrees,
    int32_t *target_counts)
{
    int64_t angle_counts;
    int64_t target;

    if (model == NULL || target_counts == NULL ||
        (kind != EMASTER_POSITION_COMMAND_RELATIVE_TARGET &&
         kind != EMASTER_POSITION_COMMAND_ABSOLUTE_TARGET))
    {
        return EMASTER_POSITION_TARGET_INVALID_ARGUMENT;
    }
    if (!model->scale.read_succeeded)
    {
        return EMASTER_POSITION_TARGET_INVALID_SCALE;
    }
    if (!axis_model_is_valid(model))
    {
        return EMASTER_POSITION_TARGET_INVALID_ARGUMENT;
    }
    if (!emaster_motion_angle_to_counts(
            angle_millidegrees, model->coordinate, &model->scale, &angle_counts))
    {
        return EMASTER_POSITION_TARGET_OUT_OF_RANGE;
    }
    angle_counts *= (int64_t)model->polarity;
    target = kind == EMASTER_POSITION_COMMAND_ABSOLUTE_TARGET
                 ? (int64_t)model->absolute_zero_counts + angle_counts
                 : (int64_t)reference_counts + angle_counts;
    if (target < INT32_MIN || target > INT32_MAX)
    {
        return EMASTER_POSITION_TARGET_OUT_OF_RANGE;
    }
    if (model->limits_enabled &&
        (target < model->minimum_counts || target > model->maximum_counts))
    {
        return EMASTER_POSITION_TARGET_LIMIT_VIOLATION;
    }
    *target_counts = (int32_t)target;
    return EMASTER_POSITION_TARGET_OK;
}

static size_t next_index(size_t index, size_t slot_count)
{
    return index + 1U == slot_count ? 0U : index + 1U;
}

bool emaster_position_command_queue_init(
    emaster_position_command_queue_t *queue,
    emaster_position_command_header_t *headers,
    int32_t *position_storage,
    size_t slot_count,
    size_t axis_count)
{
    if (queue == NULL || headers == NULL || position_storage == NULL ||
        slot_count < 2U || axis_count == 0U || slot_count > SIZE_MAX / axis_count)
    {
        return false;
    }
    memset(queue, 0, sizeof(*queue));
    queue->headers = headers;
    queue->position_storage = position_storage;
    queue->slot_count = slot_count;
    queue->axis_count = axis_count;
    atomic_init(&queue->pending_count, 0U);
    queue->initialized = true;
    return true;
}

emaster_position_command_status_t emaster_position_command_queue_publish(
    emaster_position_command_queue_t *queue,
    const emaster_position_command_header_t *header,
    const int32_t *positions,
    size_t axis_count)
{
    size_t pending_count;
    size_t slot;

    if (queue == NULL || !queue->initialized || header == NULL || positions == NULL)
    {
        return EMASTER_POSITION_COMMAND_INVALID_ARGUMENT;
    }
    if (axis_count != queue->axis_count)
    {
        return EMASTER_POSITION_COMMAND_AXIS_COUNT_MISMATCH;
    }
    if (header->sequence == 0U || header->sequence <= queue->last_published_sequence)
    {
        return EMASTER_POSITION_COMMAND_SEQUENCE_NOT_MONOTONIC;
    }
    if (header->activate_cycle < queue->last_published_activate_cycle ||
        header->expire_cycle <= header->activate_cycle)
    {
        return EMASTER_POSITION_COMMAND_INVALID_ARGUMENT;
    }
    pending_count = atomic_load_explicit(&queue->pending_count, memory_order_acquire);
    if (pending_count >= queue->slot_count)
    {
        return EMASTER_POSITION_COMMAND_QUEUE_FULL;
    }
    slot = queue->write_index;
    queue->headers[slot] = *header;
    memcpy(&queue->position_storage[slot * queue->axis_count], positions,
           queue->axis_count * sizeof(*positions));
    queue->write_index = next_index(slot, queue->slot_count);
    queue->last_published_sequence = header->sequence;
    queue->last_published_activate_cycle = header->activate_cycle;
    atomic_fetch_add_explicit(&queue->pending_count, 1U, memory_order_release);
    return EMASTER_POSITION_COMMAND_UPDATED;
}

emaster_position_command_status_t emaster_position_command_queue_take(
    emaster_position_command_queue_t *queue,
    uint64_t current_cycle,
    int32_t *positions,
    size_t axis_capacity,
    emaster_position_command_observation_t *observation)
{
    emaster_position_command_status_t result = EMASTER_POSITION_COMMAND_NO_COMMAND;
    size_t pending_count;

    if (queue == NULL || !queue->initialized || positions == NULL || observation == NULL)
    {
        return EMASTER_POSITION_COMMAND_INVALID_ARGUMENT;
    }
    if (axis_capacity < queue->axis_count)
    {
        return EMASTER_POSITION_COMMAND_AXIS_COUNT_MISMATCH;
    }
    memset(observation, 0, sizeof(*observation));
    pending_count = atomic_load_explicit(&queue->pending_count, memory_order_acquire);
    while (pending_count > 0U)
    {
        const size_t slot = queue->read_index;
        const emaster_position_command_header_t header = queue->headers[slot];
        int32_t *slot_positions = &queue->position_storage[slot * queue->axis_count];

        if (header.activate_cycle > current_cycle)
        {
            break;
        }
        queue->read_index = next_index(slot, queue->slot_count);
        atomic_fetch_sub_explicit(&queue->pending_count, 1U, memory_order_release);
        pending_count = atomic_load_explicit(&queue->pending_count, memory_order_acquire);
        if (header.sequence <= queue->last_consumed_sequence)
        {
            ++observation->superseded_count;
            continue;
        }
        if (header.expire_cycle <= current_cycle)
        {
            ++observation->expired_count;
            queue->last_consumed_sequence = header.sequence;
            if (result == EMASTER_POSITION_COMMAND_NO_COMMAND)
            {
                result = EMASTER_POSITION_COMMAND_EXPIRED;
            }
            continue;
        }
        if (result == EMASTER_POSITION_COMMAND_UPDATED)
        {
            ++observation->superseded_count;
        }
        memcpy(positions, slot_positions, queue->axis_count * sizeof(*positions));
        observation->sequence = header.sequence;
        observation->activate_cycle = header.activate_cycle;
        observation->expire_cycle = header.expire_cycle;
        queue->last_consumed_sequence = header.sequence;
        result = EMASTER_POSITION_COMMAND_UPDATED;
    }
    return result;
}

void emaster_position_command_queue_clear(emaster_position_command_queue_t *queue)
{
    if (queue == NULL || !queue->initialized)
    {
        return;
    }
    queue->read_index = queue->write_index;
    atomic_store_explicit(&queue->pending_count, 0U, memory_order_release);
}

bool emaster_position_command_is_active(uint64_t current_cycle,
                                        uint64_t expire_cycle)
{
    return current_cycle < expire_cycle;
}
