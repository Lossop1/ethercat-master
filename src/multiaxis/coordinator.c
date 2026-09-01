#include "emaster/multiaxis/coordinator.h"

#include <string.h>

static bool controller_is_valid(const emaster_cia402_controller_t *controller)
{
    return controller != NULL &&
           controller->goal >= EMASTER_CIA402_GOAL_SAFE_STOP &&
           controller->goal <= EMASTER_CIA402_GOAL_OPERATION_ENABLED;
}

static void clear_outputs(emaster_cia402_output_t *outputs, size_t axis_count)
{
    if (outputs != NULL)
    {
        memset(outputs, 0, axis_count * sizeof(*outputs));
    }
}

bool emaster_multiaxis_coordinator_init(
    emaster_multiaxis_coordinator_t *coordinator,
    emaster_cia402_controller_t *controllers,
    size_t axis_count)
{
    size_t axis_index;

    if (coordinator == NULL || controllers == NULL || axis_count == 0U)
    {
        return false;
    }
    memset(coordinator, 0, sizeof(*coordinator));
    coordinator->controllers = controllers;
    coordinator->axis_count = axis_count;
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        emaster_cia402_controller_init(&controllers[axis_index]);
    }
    return true;
}

emaster_multiaxis_status_t emaster_multiaxis_coordinator_step(
    emaster_multiaxis_coordinator_t *coordinator,
    const emaster_multiaxis_frame_t *frame,
    uint64_t now_ns)
{
    size_t axis_index;

    if (coordinator == NULL || frame == NULL || coordinator->controllers == NULL ||
        coordinator->axis_count == 0U || frame->axis_count == 0U ||
        frame->status_words == NULL || frame->outputs == NULL)
    {
        return EMASTER_MULTIAXIS_INVALID_ARGUMENT;
    }
    if (frame->axis_count != coordinator->axis_count)
    {
        return EMASTER_MULTIAXIS_AXIS_COUNT_MISMATCH;
    }
    if (coordinator->has_last_sequence && frame->sequence <= coordinator->last_sequence)
    {
        clear_outputs(frame->outputs, frame->axis_count);
        return EMASTER_MULTIAXIS_SEQUENCE_NOT_MONOTONIC;
    }
    if (now_ns > frame->deadline_ns)
    {
        clear_outputs(frame->outputs, frame->axis_count);
        return EMASTER_MULTIAXIS_DEADLINE_EXPIRED;
    }
    for (axis_index = 0U; axis_index < frame->axis_count; ++axis_index)
    {
        emaster_cia402_state_t state;

        if (!controller_is_valid(&coordinator->controllers[axis_index]))
        {
            clear_outputs(frame->outputs, frame->axis_count);
            return EMASTER_MULTIAXIS_CONTROLLER_INVALID;
        }
        if (!emaster_cia402_decode_status_word(frame->status_words[axis_index], &state))
        {
            clear_outputs(frame->outputs, frame->axis_count);
            return EMASTER_MULTIAXIS_STATUS_UNKNOWN;
        }
    }
    for (axis_index = 0U; axis_index < frame->axis_count; ++axis_index)
    {
        if (!emaster_cia402_controller_step(&coordinator->controllers[axis_index],
                                            frame->status_words[axis_index],
                                            &frame->outputs[axis_index]))
        {
            clear_outputs(frame->outputs, frame->axis_count);
            return EMASTER_MULTIAXIS_CONTROLLER_INVALID;
        }
    }
    coordinator->last_sequence = frame->sequence;
    coordinator->has_last_sequence = true;
    return EMASTER_MULTIAXIS_OK;
}
