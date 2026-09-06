#ifndef EMASTER_CYCLIC_TIMING_H
#define EMASTER_CYCLIC_TIMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 一次过程数据交换的两个时钟观测；主机时间和从站 DC 时间不能混为同一时间戳。 */
typedef struct
{
    uint64_t exchange;
    bool host_time_valid;
    uint64_t scheduled_send_ns;
    uint64_t host_send_start_ns;
    uint64_t host_send_end_ns;
    uint64_t host_receive_end_ns;
    bool dc_time_valid;
    int64_t dc_time_ns;
    bool wkc_match;
} emaster_cyclic_timing_observation_t;

/*
 * 周期统计只保留边界值和计数，不保存每周期数组。传播延迟用于估算指定从站看到
 * 同一帧的 DC 相位；该值来自 SOEM 的拓扑测量，不是从站 SM2 完成时间戳。
 */
typedef struct
{
    uint64_t sample_count;
    uint64_t host_time_sample_count;
    uint64_t dc_time_sample_count;
    uint64_t wkc_mismatch_count;
    uint64_t deadline_missed_count;
    uint64_t first_exchange;
    uint64_t last_exchange;
    uint64_t last_scheduled_send_ns;
    uint64_t last_host_send_start_ns;
    uint64_t last_host_send_end_ns;
    uint64_t last_host_receive_end_ns;
    int64_t last_dc_time_ns;
    uint64_t min_send_duration_ns;
    uint64_t max_send_duration_ns;
    uint64_t min_round_trip_ns;
    uint64_t max_round_trip_ns;
    int64_t min_send_lateness_ns;
    int64_t max_send_lateness_ns;
    int64_t min_dc_arrival_phase_ns;
    int64_t max_dc_arrival_phase_ns;
    int64_t min_phase_error_ns;
    int64_t max_phase_error_ns;
    int64_t min_sync0_margin_ns;
    int64_t max_sync0_margin_ns;
    int64_t last_phase_error_ns;
    int64_t last_sync0_margin_ns;
    uint64_t sync0_late_count;
    bool has_send_duration;
    bool has_round_trip;
    bool has_send_lateness;
    bool has_dc_phase;
    bool has_phase_error;
    bool has_sync0_margin;
    bool last_dc_sample_valid;
} emaster_cyclic_timing_stats_t;

void emaster_cyclic_timing_stats_init(emaster_cyclic_timing_stats_t *stats);

/*
 * 记录一次交换的主机时间和参考 DC 时间。sync0_shift_ns 与 propagation_delay_ns
 * 只用于计算当前周期的理论相位关系；函数不会声称观测到了 SM2 内部完成时刻。
 */
bool emaster_cyclic_timing_stats_record(
    emaster_cyclic_timing_stats_t *stats,
    const emaster_cyclic_timing_observation_t *observation,
    uint32_t cycle_ns,
    uint32_t target_phase_ns,
    int32_t sync0_shift_ns,
    int32_t propagation_delay_ns);

void emaster_cyclic_timing_stats_note_deadline_missed(
    emaster_cyclic_timing_stats_t *stats);

/*
 * 判断最近一次样本是否带有 DC 时间且理论到达相位位于 Sync0 之前
 * 调用者负责连续窗口计数，单个安全样本不等于启动已经稳定
 */
bool emaster_cyclic_timing_last_sample_is_safe(
    const emaster_cyclic_timing_stats_t *stats);

#endif
