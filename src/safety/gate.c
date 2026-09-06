#include "emaster/safety/gate.h"

#include <string.h>

bool emaster_safety_evaluate(const emaster_safety_conditions_t *conditions,
                             emaster_safety_decision_t *decision)
{
    uint32_t reasons = EMASTER_SAFETY_REASON_NONE;

    if (conditions == NULL || decision == NULL)
    {
        return false;
    }
    memset(decision, 0, sizeof(*decision));
    if (!conditions->topology_verified)
    {
        reasons |= EMASTER_SAFETY_REASON_TOPOLOGY_UNVERIFIED;
    }
    if (!conditions->pdo_verified)
    {
        reasons |= EMASTER_SAFETY_REASON_PDO_UNVERIFIED;
    }
    if (!conditions->output_initialized)
    {
        reasons |= EMASTER_SAFETY_REASON_OUTPUT_UNINITIALIZED;
    }
    if (!conditions->communication_healthy)
    {
        reasons |= EMASTER_SAFETY_REASON_COMMUNICATION_INVALID;
    }
    if (!conditions->command_valid)
    {
        reasons |= EMASTER_SAFETY_REASON_COMMAND_INVALID;
    }
    if (!conditions->feedback_valid)
    {
        reasons |= EMASTER_SAFETY_REASON_FEEDBACK_INVALID;
    }
    if (!conditions->mode_confirmed)
    {
        reasons |= EMASTER_SAFETY_REASON_MODE_UNCONFIRMED;
    }
    if (!conditions->synchronization_healthy)
    {
        reasons |= EMASTER_SAFETY_REASON_SYNCHRONIZATION_INVALID;
    }
    if (!conditions->target_valid)
    {
        reasons |= EMASTER_SAFETY_REASON_TARGET_INVALID;
    }
    if (!conditions->drive_limit_inactive)
    {
        reasons |= EMASTER_SAFETY_REASON_DRIVE_LIMIT_ACTIVE;
    }
    if (!conditions->enable_authorized)
    {
        reasons |= EMASTER_SAFETY_REASON_ENABLE_NOT_AUTHORIZED;
    }
    if (conditions->stop_requested)
    {
        reasons |= EMASTER_SAFETY_REASON_STOP_REQUESTED;
    }
    if (conditions->fault_latched)
    {
        reasons |= EMASTER_SAFETY_REASON_FAULT_LATCHED;
    }
    decision->blocking_reasons = reasons;
    decision->control_permitted = reasons == EMASTER_SAFETY_REASON_NONE;
    decision->force_safe_stop = !decision->control_permitted;
    return true;
}
