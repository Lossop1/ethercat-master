#include "emaster/cia402/controller.h"

#include <string.h>

/* CiA 402 标准状态字掩码和值；这些是协议语义，不是部署运行参数。 */
enum
{
    EMASTER_CIA402_STATUS_MASK = 0x006FU,
    EMASTER_CIA402_STATUS_NOT_READY = 0x0000U,
    EMASTER_CIA402_STATUS_SWITCH_ON_DISABLED = 0x0040U,
    EMASTER_CIA402_STATUS_READY_TO_SWITCH_ON = 0x0021U,
    EMASTER_CIA402_STATUS_SWITCHED_ON = 0x0023U,
    EMASTER_CIA402_STATUS_OPERATION_ENABLED = 0x0027U,
    EMASTER_CIA402_STATUS_QUICK_STOP_ACTIVE = 0x0007U,
    EMASTER_CIA402_STATUS_FAULT_REACTION_ACTIVE = 0x000FU,
    EMASTER_CIA402_STATUS_FAULT = 0x0008U,
    EMASTER_CIA402_CONTROL_DISABLE_VOLTAGE = 0x0000U,
    EMASTER_CIA402_CONTROL_SHUTDOWN = 0x0006U,
    EMASTER_CIA402_CONTROL_SWITCH_ON = 0x0007U,
    EMASTER_CIA402_CONTROL_ENABLE_OPERATION = 0x000FU,
    EMASTER_CIA402_CONTROL_FAULT_RESET = 0x0080U
};

static bool goal_is_valid(emaster_cia402_goal_t goal)
{
    return goal >= EMASTER_CIA402_GOAL_SAFE_STOP &&
           goal <= EMASTER_CIA402_GOAL_OPERATION_ENABLED;
}

bool emaster_cia402_decode_status_word(uint16_t status_word,
                                       emaster_cia402_state_t *state)
{
    uint16_t masked_status;

    if (state == NULL)
    {
        return false;
    }
    masked_status = (uint16_t)(status_word & EMASTER_CIA402_STATUS_MASK);
    switch (masked_status)
    {
        case EMASTER_CIA402_STATUS_NOT_READY:
            *state = EMASTER_CIA402_STATE_NOT_READY_TO_SWITCH_ON;
            break;
        case EMASTER_CIA402_STATUS_SWITCH_ON_DISABLED:
            *state = EMASTER_CIA402_STATE_SWITCH_ON_DISABLED;
            break;
        case EMASTER_CIA402_STATUS_READY_TO_SWITCH_ON:
            *state = EMASTER_CIA402_STATE_READY_TO_SWITCH_ON;
            break;
        case EMASTER_CIA402_STATUS_SWITCHED_ON:
            *state = EMASTER_CIA402_STATE_SWITCHED_ON;
            break;
        case EMASTER_CIA402_STATUS_OPERATION_ENABLED:
            *state = EMASTER_CIA402_STATE_OPERATION_ENABLED;
            break;
        case EMASTER_CIA402_STATUS_QUICK_STOP_ACTIVE:
            *state = EMASTER_CIA402_STATE_QUICK_STOP_ACTIVE;
            break;
        case EMASTER_CIA402_STATUS_FAULT_REACTION_ACTIVE:
            *state = EMASTER_CIA402_STATE_FAULT_REACTION_ACTIVE;
            break;
        case EMASTER_CIA402_STATUS_FAULT:
            *state = EMASTER_CIA402_STATE_FAULT;
            break;
        default:
            *state = EMASTER_CIA402_STATE_UNKNOWN;
            return false;
    }
    return true;
}

static bool goal_reached(emaster_cia402_goal_t goal, emaster_cia402_state_t state)
{
    switch (goal)
    {
        case EMASTER_CIA402_GOAL_SAFE_STOP:
            return state == EMASTER_CIA402_STATE_NOT_READY_TO_SWITCH_ON ||
                   state == EMASTER_CIA402_STATE_SWITCH_ON_DISABLED ||
                   state == EMASTER_CIA402_STATE_QUICK_STOP_ACTIVE ||
                   state == EMASTER_CIA402_STATE_FAULT;
        case EMASTER_CIA402_GOAL_READY_TO_SWITCH_ON:
            return state == EMASTER_CIA402_STATE_READY_TO_SWITCH_ON;
        case EMASTER_CIA402_GOAL_SWITCHED_ON:
            return state == EMASTER_CIA402_STATE_SWITCHED_ON;
        case EMASTER_CIA402_GOAL_OPERATION_ENABLED:
            return state == EMASTER_CIA402_STATE_OPERATION_ENABLED;
    }
    return false;
}

static uint16_t control_word_for_goal(emaster_cia402_goal_t goal,
                                      emaster_cia402_state_t state)
{
    if (goal == EMASTER_CIA402_GOAL_SAFE_STOP)
    {
        return EMASTER_CIA402_CONTROL_DISABLE_VOLTAGE;
    }
    switch (state)
    {
        case EMASTER_CIA402_STATE_SWITCH_ON_DISABLED:
            return EMASTER_CIA402_CONTROL_SHUTDOWN;
        case EMASTER_CIA402_STATE_READY_TO_SWITCH_ON:
            return goal == EMASTER_CIA402_GOAL_READY_TO_SWITCH_ON
                       ? EMASTER_CIA402_CONTROL_SHUTDOWN
                       : EMASTER_CIA402_CONTROL_SWITCH_ON;
        case EMASTER_CIA402_STATE_SWITCHED_ON:
            return goal == EMASTER_CIA402_GOAL_OPERATION_ENABLED
                       ? EMASTER_CIA402_CONTROL_ENABLE_OPERATION
                       : EMASTER_CIA402_CONTROL_SWITCH_ON;
        case EMASTER_CIA402_STATE_OPERATION_ENABLED:
            return goal == EMASTER_CIA402_GOAL_OPERATION_ENABLED
                       ? EMASTER_CIA402_CONTROL_ENABLE_OPERATION
                       : EMASTER_CIA402_CONTROL_SWITCH_ON;
        case EMASTER_CIA402_STATE_QUICK_STOP_ACTIVE:
            return goal == EMASTER_CIA402_GOAL_OPERATION_ENABLED
                       ? EMASTER_CIA402_CONTROL_ENABLE_OPERATION
                       : EMASTER_CIA402_CONTROL_SHUTDOWN;
        case EMASTER_CIA402_STATE_NOT_READY_TO_SWITCH_ON:
        case EMASTER_CIA402_STATE_FAULT_REACTION_ACTIVE:
        case EMASTER_CIA402_STATE_FAULT:
        case EMASTER_CIA402_STATE_UNKNOWN:
            return EMASTER_CIA402_CONTROL_DISABLE_VOLTAGE;
    }
    return EMASTER_CIA402_CONTROL_DISABLE_VOLTAGE;
}

void emaster_cia402_controller_init(emaster_cia402_controller_t *controller)
{
    if (controller == NULL)
    {
        return;
    }
    memset(controller, 0, sizeof(*controller));
    controller->goal = EMASTER_CIA402_GOAL_SAFE_STOP;
}

bool emaster_cia402_controller_set_goal(emaster_cia402_controller_t *controller,
                                         emaster_cia402_goal_t goal)
{
    if (controller == NULL || !goal_is_valid(goal))
    {
        return false;
    }
    controller->goal = goal;
    return true;
}

void emaster_cia402_controller_request_fault_reset(
    emaster_cia402_controller_t *controller)
{
    if (controller != NULL)
    {
        controller->fault_reset_requested = true;
    }
}

bool emaster_cia402_controller_step(emaster_cia402_controller_t *controller,
                                     uint16_t status_word,
                                     emaster_cia402_output_t *output)
{
    emaster_cia402_state_t state;

    if (controller == NULL || output == NULL || !goal_is_valid(controller->goal))
    {
        return false;
    }
    memset(output, 0, sizeof(*output));
    output->control_word = EMASTER_CIA402_CONTROL_DISABLE_VOLTAGE;
    output->observed_state = EMASTER_CIA402_STATE_UNKNOWN;
    output->state_known = emaster_cia402_decode_status_word(status_word, &state);
    if (!output->state_known)
    {
        controller->fault_reset_requested = false;
        return true;
    }
    output->observed_state = state;
    output->fault_present = state == EMASTER_CIA402_STATE_FAULT ||
                             state == EMASTER_CIA402_STATE_FAULT_REACTION_ACTIVE;
    output->goal_reached = goal_reached(controller->goal, state);
    if (state == EMASTER_CIA402_STATE_FAULT && controller->fault_reset_requested)
    {
        output->control_word = EMASTER_CIA402_CONTROL_FAULT_RESET;
        output->fault_reset_pulse = true;
        controller->fault_reset_requested = false;
        return true;
    }
    controller->fault_reset_requested = false;
    output->control_word = control_word_for_goal(controller->goal, state);
    return true;
}
