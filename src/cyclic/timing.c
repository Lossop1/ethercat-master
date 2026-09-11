#include "emaster/cyclic/timing.h"

#include <limits.h>
#include <string.h>

static int64_t positive_mod(int64_t value, uint32_t modulus)
{
    int64_t result = value % (int64_t)modulus;

    return result < 0 ? result + (int64_t)modulus : result;
}

static int64_t centered_difference(int64_t value, int64_t reference, uint32_t cycle_ns)
{
    int64_t difference = value - reference;
    int64_t half_cycle = (int64_t)cycle_ns / INT64_C(2);

    difference %= (int64_t)cycle_ns;
    if (difference > half_cycle)
    {
        difference -= (int64_t)cycle_ns;
    }
    else if (difference < -half_cycle)
    {
        difference += (int64_t)cycle_ns;
    }
    return difference;
}

static void update_unsigned_range(bool *present, uint64_t *minimum,
                                  uint64_t *maximum, uint64_t value)
{
    if (!*present)
    {
        *minimum = value;
        *maximum = value;
        *present = true;
    }
    else
    {
        if (value < *minimum)
        {
            *minimum = value;
        }
        if (value > *maximum)
        {
            *maximum = value;
        }
    }
}

static void update_signed_range(bool *present, int64_t *minimum,
                                int64_t *maximum, int64_t value)
{
    if (!*present)
    {
        *minimum = value;
        *maximum = value;
        *present = true;
    }
    else
    {
        if (value < *minimum)
        {
            *minimum = value;
        }
        if (value > *maximum)
        {
            *maximum = value;
        }
    }
}

void emaster_cyclic_timing_stats_init(emaster_cyclic_timing_stats_t *stats)
{
    if (stats == NULL)
    {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    emaster_cyclic_histogram_init(&stats->send_duration_histogram,
                                  EMASTER_TIMING_SEND_DURATION_ORIGIN_NS,
                                  EMASTER_TIMING_SEND_DURATION_BUCKET_NS);
    emaster_cyclic_histogram_init(&stats->round_trip_histogram,
                                  EMASTER_TIMING_ROUND_TRIP_ORIGIN_NS,
                                  EMASTER_TIMING_ROUND_TRIP_BUCKET_NS);
    emaster_cyclic_histogram_init(&stats->send_lateness_histogram,
                                  EMASTER_TIMING_SEND_LATENESS_ORIGIN_NS,
                                  EMASTER_TIMING_SEND_LATENESS_BUCKET_NS);
    emaster_cyclic_histogram_init(&stats->dc_arrival_phase_histogram,
                                  EMASTER_TIMING_DC_PHASE_ORIGIN_NS,
                                  EMASTER_TIMING_DC_PHASE_BUCKET_NS);
    emaster_cyclic_histogram_init(&stats->phase_error_histogram,
                                  EMASTER_TIMING_PHASE_ERROR_ORIGIN_NS,
                                  EMASTER_TIMING_PHASE_ERROR_BUCKET_NS);
    emaster_cyclic_histogram_init(&stats->sync0_margin_histogram,
                                  EMASTER_TIMING_SYNC0_MARGIN_ORIGIN_NS,
                                  EMASTER_TIMING_SYNC0_MARGIN_BUCKET_NS);
}

bool emaster_cyclic_timing_stats_record(
    emaster_cyclic_timing_stats_t *stats,
    const emaster_cyclic_timing_observation_t *observation,
    uint32_t cycle_ns,
    uint32_t target_phase_ns,
    int32_t sync0_shift_ns,
    int32_t propagation_delay_ns)
{
    int64_t arrival_time_ns;
    int64_t arrival_phase_ns;
    int64_t sync0_phase_ns;
    int64_t sync0_margin_ns;
    int64_t phase_error_ns;

    if (stats == NULL || observation == NULL || cycle_ns == 0U ||
        target_phase_ns >= cycle_ns || observation->exchange == 0U)
    {
        return false;
    }
    if (stats->sample_count == 0U)
    {
        stats->first_exchange = observation->exchange;
    }
    stats->last_exchange = observation->exchange;
    ++stats->sample_count;
    if (!observation->wkc_match && stats->wkc_mismatch_count != UINT64_MAX)
    {
        ++stats->wkc_mismatch_count;
    }
    if (observation->host_time_valid)
    {
        uint64_t send_duration;
        uint64_t round_trip;
        int64_t send_lateness;

        if (observation->host_send_end_ns < observation->host_send_start_ns ||
            observation->host_receive_end_ns < observation->host_send_start_ns)
        {
            return false;
        }
        send_duration = observation->host_send_end_ns - observation->host_send_start_ns;
        round_trip = observation->host_receive_end_ns - observation->host_send_start_ns;
        if (observation->host_send_start_ns >= observation->scheduled_send_ns)
        {
            uint64_t difference = observation->host_send_start_ns -
                                  observation->scheduled_send_ns;
            send_lateness = difference > (uint64_t)INT64_MAX
                                ? INT64_MAX
                                : (int64_t)difference;
        }
        else
        {
            uint64_t difference = observation->scheduled_send_ns -
                                  observation->host_send_start_ns;
            send_lateness = difference > (uint64_t)INT64_MAX
                                ? INT64_MIN
                                : -(int64_t)difference;
        }
        ++stats->host_time_sample_count;
        stats->last_scheduled_send_ns = observation->scheduled_send_ns;
        stats->last_host_send_start_ns = observation->host_send_start_ns;
        stats->last_host_send_end_ns = observation->host_send_end_ns;
        stats->last_host_receive_end_ns = observation->host_receive_end_ns;
        update_unsigned_range(&stats->has_send_duration, &stats->min_send_duration_ns,
                              &stats->max_send_duration_ns, send_duration);
        update_unsigned_range(&stats->has_round_trip, &stats->min_round_trip_ns,
                              &stats->max_round_trip_ns, round_trip);
        update_signed_range(&stats->has_send_lateness, &stats->min_send_lateness_ns,
                            &stats->max_send_lateness_ns, send_lateness);
        /*
         * 两个耗时量在此处已保证不超过 INT64_MAX：它们由同一时基的差值得到，
         * 且上面已拒绝了终点早于起点的样本。
         */
        emaster_cyclic_histogram_record(&stats->send_duration_histogram,
                                        (int64_t)send_duration);
        emaster_cyclic_histogram_record(&stats->round_trip_histogram,
                                        (int64_t)round_trip);
        emaster_cyclic_histogram_record(&stats->send_lateness_histogram, send_lateness);
    }
    if (!observation->dc_time_valid)
    {
        stats->last_dc_sample_valid = false;
        return true;
    }
    if ((propagation_delay_ns > 0 && observation->dc_time_ns > INT64_MAX - propagation_delay_ns) ||
        (propagation_delay_ns < 0 && observation->dc_time_ns < INT64_MIN - propagation_delay_ns))
    {
        return false;
    }
    ++stats->dc_time_sample_count;
    stats->last_dc_time_ns = observation->dc_time_ns;
    arrival_time_ns = observation->dc_time_ns + propagation_delay_ns;
    arrival_phase_ns = positive_mod(arrival_time_ns, cycle_ns);
    sync0_phase_ns = positive_mod(sync0_shift_ns, cycle_ns);
    /*
     * 两个相位都由取模得到，直接相减会在周期边界附近折返：帧比原点略早到达时
     * arrival_phase 取模后接近一个完整周期，差值会退化成接近 -cycle_ns 的伪迟到。
     * 与 phase_error 一样按半周期归一化，使裕量始终表示到 Sync0 的最短有向距离。
     */
    sync0_margin_ns =
        centered_difference(sync0_phase_ns, arrival_phase_ns, cycle_ns);
    phase_error_ns = centered_difference(arrival_phase_ns, target_phase_ns, cycle_ns);
    update_signed_range(&stats->has_dc_phase, &stats->min_dc_arrival_phase_ns,
                        &stats->max_dc_arrival_phase_ns, arrival_phase_ns);
    update_signed_range(&stats->has_phase_error, &stats->min_phase_error_ns,
                        &stats->max_phase_error_ns, phase_error_ns);
    update_signed_range(&stats->has_sync0_margin, &stats->min_sync0_margin_ns,
                        &stats->max_sync0_margin_ns, sync0_margin_ns);
    emaster_cyclic_histogram_record(&stats->dc_arrival_phase_histogram, arrival_phase_ns);
    emaster_cyclic_histogram_record(&stats->phase_error_histogram, phase_error_ns);
    emaster_cyclic_histogram_record(&stats->sync0_margin_histogram, sync0_margin_ns);
    stats->last_phase_error_ns = phase_error_ns;
    stats->last_sync0_margin_ns = sync0_margin_ns;
    stats->last_dc_sample_valid = true;
    if (sync0_margin_ns < 0 && stats->sync0_late_count != UINT64_MAX)
    {
        ++stats->sync0_late_count;
    }
    return true;
}

bool emaster_cyclic_timing_last_sample_is_safe(
    const emaster_cyclic_timing_stats_t *stats)
{
    return stats != NULL && stats->last_dc_sample_valid &&
           stats->last_sync0_margin_ns >= 0;
}

void emaster_cyclic_timing_stats_note_deadline_missed(
    emaster_cyclic_timing_stats_t *stats)
{
    if (stats != NULL && stats->deadline_missed_count != UINT64_MAX)
    {
        ++stats->deadline_missed_count;
    }
}
