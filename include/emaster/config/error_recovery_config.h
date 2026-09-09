#ifndef EMASTER_CONFIG_ERROR_RECOVERY_CONFIG_H
#define EMASTER_CONFIG_ERROR_RECOVERY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 错误恢复策略配置：与总线逻辑解耦的容错参数。
 * 策略文件独立维护，不同部署可选用不同策略，支持运行时切换。
 */

typedef struct
{
    bool enabled;
    uint32_t consecutive_error_threshold;
    uint32_t total_error_threshold;
} emaster_wkc_recovery_config_t;

typedef struct
{
    bool enabled;
    uint32_t consecutive_error_threshold;
} emaster_deadline_recovery_config_t;

typedef struct
{
    bool enabled;
    uint32_t check_interval_cycles;
    uint32_t max_recovery_attempts;
} emaster_al_state_recovery_config_t;

typedef struct
{
    bool enabled;
    uint32_t max_reset_attempts;
    const uint16_t *recoverable_error_codes;
    size_t recoverable_error_code_count;
} emaster_cia402_fault_recovery_config_t;

typedef struct
{
    const char *policy_id;
    emaster_wkc_recovery_config_t wkc_recovery;
    emaster_deadline_recovery_config_t deadline_recovery;
    emaster_al_state_recovery_config_t al_state_recovery;
    emaster_cia402_fault_recovery_config_t cia402_fault_recovery;
} emaster_error_recovery_policy_t;

/*
 * 查询已注册的错误恢复策略数量和具体策略。
 * 策略由配置生成器在编译期从 JSON 转换并注册。
 */
size_t emaster_error_recovery_policy_count(void);
const emaster_error_recovery_policy_t *emaster_error_recovery_policy_at(size_t index);
const emaster_error_recovery_policy_t *emaster_error_recovery_policy_by_id(const char *policy_id);

#endif
