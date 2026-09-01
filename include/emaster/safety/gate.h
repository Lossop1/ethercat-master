#ifndef EMASTER_SAFETY_GATE_H
#define EMASTER_SAFETY_GATE_H

#include <stdbool.h>
#include <stdint.h>

/* 安全门阻断原因使用位掩码，便于上层一次记录全部缺失条件。 */
typedef enum
{
    EMASTER_SAFETY_REASON_NONE = 0U,
    EMASTER_SAFETY_REASON_TOPOLOGY_UNVERIFIED = UINT32_C(1) << 0U,
    EMASTER_SAFETY_REASON_PDO_UNVERIFIED = UINT32_C(1) << 1U,
    EMASTER_SAFETY_REASON_OUTPUT_UNINITIALIZED = UINT32_C(1) << 2U,
    EMASTER_SAFETY_REASON_COMMUNICATION_INVALID = UINT32_C(1) << 3U,
    EMASTER_SAFETY_REASON_COMMAND_INVALID = UINT32_C(1) << 4U,
    EMASTER_SAFETY_REASON_FEEDBACK_INVALID = UINT32_C(1) << 5U,
    EMASTER_SAFETY_REASON_ENABLE_NOT_AUTHORIZED = UINT32_C(1) << 6U,
    EMASTER_SAFETY_REASON_STOP_REQUESTED = UINT32_C(1) << 7U,
    EMASTER_SAFETY_REASON_FAULT_LATCHED = UINT32_C(1) << 8U
} emaster_safety_reason_t;

/*
 * 条件由生命周期线程或周期线程在边界处完整填写；安全门不读取时钟、不访问总线，也不
 * 推断任何设备默认行为。enable_authorized 必须来自明确的上层安全授权。
 */
typedef struct
{
    bool topology_verified;
    bool pdo_verified;
    bool output_initialized;
    bool communication_healthy;
    bool command_valid;
    bool feedback_valid;
    bool enable_authorized;
    bool stop_requested;
    bool fault_latched;
} emaster_safety_conditions_t;

typedef struct
{
    uint32_t blocking_reasons;
    bool control_permitted;
    bool force_safe_stop;
} emaster_safety_decision_t;

/* 根据完整条件生成一次安全决定；任何条件缺失都只允许安全停止。 */
bool emaster_safety_evaluate(const emaster_safety_conditions_t *conditions,
                             emaster_safety_decision_t *decision);

#endif
