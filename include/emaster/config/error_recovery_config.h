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
    /*
     * total_error_threshold 的统计窗口：判据是"最近这么多毫秒内错了 N 次"，
     * 不是"整轮会话累计错了 N 次"。
     *
     * 累计语义会让长跑必然自杀：一次发生在第 11 秒的孤立错误会一直挂在账上，
     * 等到第 50 次把一次本来健康的 25 分钟会话判死（2026-09-16 台架现场）。
     * 窗口取值必须 >= 1；想要"越跑越容易触发"的旧行为，把它设得比会话时长更大即可。
     */
    uint32_t total_error_window_ms;
} emaster_wkc_recovery_config_t;

/*
 * 整帧缺失（actual_wkc <= 0）的容错阈值。与 wkc_recovery 分开配置：
 * 前者是"一个回帧都没有"，后者是"帧回来了但工作计数短"，成因与修法不同。
 * 是否启用沿用 wkc_recovery.enabled——两者都是 WKC 判定，不另设开关，
 * 避免出现"wkc 恢复开着、整帧缺失却立刻停机"这种组合。
 */
typedef struct
{
    uint32_t consecutive_error_threshold;
    uint32_t total_error_threshold;
    /* 同 wkc_recovery.total_error_window_ms。 */
    uint32_t total_error_window_ms;
} emaster_no_frame_recovery_config_t;

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
    emaster_no_frame_recovery_config_t no_frame_recovery;
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
