#include "report_sections.h"

#include "json_writer.h"

#include <inttypes.h>

#define REQUIRE_WRITE(expression) do { if (!(expression)) return false; } while (0)

static const char *phase_name(emaster_audit_phase_t phase)
{
    switch (phase)
    {
        case EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION: return "preop_configuration";
        case EMASTER_AUDIT_PHASE_SAFEOP_INITIALIZATION: return "safeop_initialization";
        case EMASTER_AUDIT_PHASE_OPERATION_REQUEST: return "operation_request";
        case EMASTER_AUDIT_PHASE_CYCLIC_OPERATION: return "cyclic_operation";
        case EMASTER_AUDIT_PHASE_SAFE_STOP: return "safe_stop";
        case EMASTER_AUDIT_PHASE_FINAL_DIAGNOSTIC: return "post_cycle_diagnostic";
    }
    return "unknown";
}

static const char *transport_name(emaster_audit_transport_t transport)
{
    switch (transport)
    {
        case EMASTER_AUDIT_TRANSPORT_SDO: return "sdo";
        case EMASTER_AUDIT_TRANSPORT_PDO: return "pdo";
        case EMASTER_AUDIT_TRANSPORT_ESC_REGISTER: return "esc_register";
    }
    return "unknown";
}

static const char *direction_name(emaster_audit_direction_t direction)
{
    return direction == EMASTER_AUDIT_DIRECTION_WRITE ? "write" : "read";
}

static uint64_t value_bits(const emaster_audit_access_t *access)
{
    uint64_t value = access->value_kind == EMASTER_AUDIT_VALUE_SIGNED
                         ? (uint64_t)access->signed_value
                         : access->unsigned_value;

    if (access->bit_length < UINT8_C(64))
    {
        value &= (UINT64_C(1) << access->bit_length) - UINT64_C(1);
    }
    return value;
}

static const emaster_pdo_entry_t *pdo_entry_for(
    const emaster_session_axis_plan_t *axis,
    emaster_audit_direction_t direction,
    uint16_t index,
    uint8_t subindex)
{
    const emaster_pdo_mapping_profile_t *mappings;
    size_t mapping_count;
    size_t mapping_ordinal;

    if (axis == NULL || axis->pdo_set == NULL)
    {
        return NULL;
    }
    if (direction == EMASTER_AUDIT_DIRECTION_WRITE)
    {
        mappings = axis->pdo_set->rx_mappings;
        mapping_count = axis->pdo_set->rx_mapping_count;
    }
    else
    {
        mappings = axis->pdo_set->tx_mappings;
        mapping_count = axis->pdo_set->tx_mapping_count;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < mapping_count; ++mapping_ordinal)
    {
        size_t entry_ordinal;
        for (entry_ordinal = 0U;
             entry_ordinal < mappings[mapping_ordinal].entry_count; ++entry_ordinal)
        {
            const emaster_pdo_entry_t *entry =
                &mappings[mapping_ordinal].entries[entry_ordinal];
            if (entry->index == index && entry->subindex == subindex)
            {
                return entry;
            }
        }
    }
    return NULL;
}

static const char *configured_sdo_name(
    const emaster_session_axis_plan_t *axis,
    uint16_t index,
    uint8_t subindex)
{
    size_t read_index;

    if (axis == NULL || axis->operation_mode == NULL)
    {
        return NULL;
    }
    for (read_index = 0U;
         read_index < axis->operation_mode->final_sdo_read_count; ++read_index)
    {
        const emaster_sdo_read_config_t *read =
            &axis->operation_mode->final_sdo_reads[read_index];
        if (read->index == index && read->subindex == subindex)
        {
            return read->name;
        }
    }
    return NULL;
}

static bool write_mapping_direction(
    FILE *stream,
    const emaster_pdo_mapping_profile_t *mappings,
    size_t mapping_count)
{
    size_t mapping_ordinal;
    uint32_t direction_offset = 0U;

    REQUIRE_WRITE(fputs("[", stream) != EOF);
    for (mapping_ordinal = 0U; mapping_ordinal < mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_profile_t *mapping = &mappings[mapping_ordinal];
        size_t entry_ordinal;

        REQUIRE_WRITE(fprintf(stream, "%s{\"mapping_index\":\"0x%04X\","
                                     "\"bit_length\":%" PRIu32 ",\"entries\":[",
                              mapping_ordinal == 0U ? "" : ",",
                              (unsigned int)mapping->index,
                              mapping->bit_length) >= 0);
        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_entry_t *entry = &mapping->entries[entry_ordinal];

            REQUIRE_WRITE(fprintf(
                stream,
                "%s{\"object\":\"0x%04X:%02X\",\"bit_length\":%u,"
                "\"bit_offset\":%" PRIu32 ",\"data_type\":",
                entry_ordinal == 0U ? "" : ",", (unsigned int)entry->index,
                (unsigned int)entry->subindex, (unsigned int)entry->bit_length,
                direction_offset) >= 0);
            REQUIRE_WRITE(emaster_json_string(stream, entry->data_type));
            REQUIRE_WRITE(fputs(",\"name\":", stream) != EOF);
            REQUIRE_WRITE(emaster_json_string(stream, entry->name));
            REQUIRE_WRITE(fputs("}", stream) != EOF);
            direction_offset += entry->bit_length;
        }
        REQUIRE_WRITE(fputs("]}", stream) != EOF);
    }
    return fputs("]", stream) != EOF;
}

static bool write_position_scale(FILE *stream,
                                 const emaster_position_scale_t *scale)
{
    return fprintf(
               stream,
               "{\"read_succeeded\":%s,\"encoder_increments\":%" PRIu32
               ",\"encoder_motor_revolutions\":%" PRIu32
               ",\"gear_motor_revolutions\":%" PRIu32
               ",\"gear_shaft_revolutions\":%" PRIu32 "}",
               scale->read_succeeded ? "true" : "false",
               scale->encoder_increments, scale->encoder_motor_revolutions,
               scale->gear_motor_revolutions, scale->gear_shaft_revolutions) >= 0;
}

/*
 * 从站的 1C32/1C33 同步违例计数器。这三个计数器和 sync_error 位是驱动器自身对
 * "输出帧是否在规定窗口内到达"的判定，比主站侧的相位推断更直接；但它们只在停机后
 * 读一次，可能包含退出期间的事件，不能单独据此推断首次故障原因。
 */
static bool write_sync_diagnostic(FILE *stream,
                                  const emaster_sync_diagnostic_t *diagnostic)
{
    return fprintf(
               stream,
               "{\"read_succeeded\":%s,\"sync_error\":%s,"
               "\"sm_event_missed\":%u,\"cycle_time_too_small\":%u,"
               "\"shift_time_too_short\":%u}",
               diagnostic->read_succeeded ? "true" : "false",
               diagnostic->sync_error ? "true" : "false",
               (unsigned int)diagnostic->sm_event_missed,
               (unsigned int)diagnostic->cycle_time_too_small,
               (unsigned int)diagnostic->shift_time_too_short) >= 0;
}

/*
 * 运行期周期性探针的结果。上面那个 sm*_diagnostic 是停机后的一次性读数，
 * 这里回答的是另一个问题：驱动器是什么时候第一次带上同步错误的。
 * first_error_exchange 为 0 表示全程没读到非零（probe_count 说明确实一直在读）。
 */
static bool write_sync_probe(FILE *stream, uint64_t probe_count,
                             uint64_t sm2_first_exchange, uint16_t sm2_first_missed,
                             bool sm2_first_sync_error, uint16_t sm2_last_missed,
                             bool sm2_last_sync_error, uint64_t sm3_first_exchange,
                             uint16_t sm3_first_missed, bool sm3_first_sync_error,
                             uint16_t sm3_last_missed, bool sm3_last_sync_error)
{
    return fprintf(stream,
                   "{\"probe_count\":%" PRIu64 ","
                   "\"sm2\":{\"first_error_exchange\":%" PRIu64
                   ",\"first_error_missed\":%u,\"first_error_sync_error\":%s,"
                   "\"last_missed\":%u,\"last_sync_error\":%s},"
                   "\"sm3\":{\"first_error_exchange\":%" PRIu64
                   ",\"first_error_missed\":%u,\"first_error_sync_error\":%s,"
                   "\"last_missed\":%u,\"last_sync_error\":%s}}",
                   probe_count, sm2_first_exchange, (unsigned int)sm2_first_missed,
                   sm2_first_sync_error ? "true" : "false", (unsigned int)sm2_last_missed,
                   sm2_last_sync_error ? "true" : "false", sm3_first_exchange,
                   (unsigned int)sm3_first_missed, sm3_first_sync_error ? "true" : "false",
                   (unsigned int)sm3_last_missed, sm3_last_sync_error ? "true" : "false") >= 0;
}

/*
 * 输出一个分位点。落在量程外时不编造数值，只标记方向，让读者去看 ranges 的极值。
 * 桶宽同时给出，因为分位点的分辨率就是桶宽，不是精确值。
 */
static bool write_quantile(FILE *stream, const char *name,
                           const emaster_cyclic_histogram_t *histogram,
                           uint64_t numerator, uint64_t denominator)
{
    emaster_cyclic_histogram_quantile_t quantile =
        emaster_cyclic_histogram_quantile(histogram, numerator, denominator);

    REQUIRE_WRITE(fprintf(stream, "\"%s\":", name) >= 0);
    if (!quantile.present)
    {
        REQUIRE_WRITE(fputs("null", stream) != EOF);
        return true;
    }
    if (quantile.below_range)
    {
        REQUIRE_WRITE(fputs("{\"below_range\":true}", stream) != EOF);
        return true;
    }
    if (quantile.above_range)
    {
        REQUIRE_WRITE(fputs("{\"above_range\":true}", stream) != EOF);
        return true;
    }
    REQUIRE_WRITE(fprintf(stream,
                          "{\"lower_ns\":%" PRId64 ",\"upper_ns\":%" PRId64 "}",
                          quantile.lower_bound_ns, quantile.upper_bound_ns) >= 0);
    return true;
}

/* 一个指标的完整分布：四个分位点加量程统计，便于与 baseline 的阈值直接比较。 */
static bool write_distribution(FILE *stream, const char *name,
                               const emaster_cyclic_histogram_t *histogram)
{
    REQUIRE_WRITE(fprintf(stream, "\"%s\":{", name) >= 0);
    REQUIRE_WRITE(write_quantile(stream, "p50", histogram, 500U, 1000U));
    REQUIRE_WRITE(fputc(',', stream) != EOF);
    REQUIRE_WRITE(write_quantile(stream, "p99", histogram, 990U, 1000U));
    REQUIRE_WRITE(fputc(',', stream) != EOF);
    REQUIRE_WRITE(write_quantile(stream, "p99_9", histogram, 9990U, 10000U));
    REQUIRE_WRITE(fputc(',', stream) != EOF);
    REQUIRE_WRITE(write_quantile(stream, "p99_999", histogram, 999990U, 1000000U));
    REQUIRE_WRITE(fprintf(stream,
                          ",\"bucket_width_ns\":%" PRId64
                          ",\"origin_ns\":%" PRId64
                          ",\"samples\":%" PRIu64
                          ",\"underflow\":%" PRIu64
                          ",\"overflow\":%" PRIu64 "}",
                          histogram->bucket_width_ns, histogram->origin_ns,
                          histogram->sample_count, histogram->underflow_count,
                          histogram->overflow_count) >= 0);
    return true;
}

static bool write_timing(FILE *stream, const emaster_cyclic_timing_stats_t *timing)
{
    REQUIRE_WRITE(fprintf(
        stream,
        "{\"samples\":%" PRIu64 ",\"host_time_samples\":%" PRIu64
        ",\"dc_time_samples\":%" PRIu64 ",\"wkc_mismatch_count\":%" PRIu64
        ",\"deadline_missed_count\":%" PRIu64 ",\"first_exchange\":%" PRIu64
        ",\"last_exchange\":%" PRIu64 ",\"last_scheduled_send_ns\":%" PRIu64
        ",\"last_host_send_start_ns\":%" PRIu64
        ",\"last_host_send_end_ns\":%" PRIu64
        ",\"last_host_receive_end_ns\":%" PRIu64
        ",\"last_dc_time_ns\":%" PRId64,
        timing->sample_count, timing->host_time_sample_count, timing->dc_time_sample_count,
        timing->wkc_mismatch_count, timing->deadline_missed_count,
        timing->first_exchange, timing->last_exchange, timing->last_scheduled_send_ns,
        timing->last_host_send_start_ns, timing->last_host_send_end_ns,
        timing->last_host_receive_end_ns, timing->last_dc_time_ns) >= 0);
    REQUIRE_WRITE(fputs(",\"ranges\":{", stream) != EOF);
    REQUIRE_WRITE(fprintf(
        stream,
        "\"send_duration_ns\":{\"present\":%s,\"min\":%" PRIu64 ",\"max\":%" PRIu64 "},"
        "\"round_trip_ns\":{\"present\":%s,\"min\":%" PRIu64 ",\"max\":%" PRIu64 "},"
        "\"send_lateness_ns\":{\"present\":%s,\"min\":%" PRId64 ",\"max\":%" PRId64 "},"
        "\"dc_arrival_phase_ns\":{\"present\":%s,\"min\":%" PRId64 ",\"max\":%" PRId64 "},"
        "\"phase_error_ns\":{\"present\":%s,\"min\":%" PRId64 ",\"max\":%" PRId64 "},"
        "\"sync0_margin_ns\":{\"present\":%s,\"min\":%" PRId64 ",\"max\":%" PRId64 "}},"
        "\"last_phase_error_ns\":%" PRId64
        ",\"last_sync0_margin_ns\":%" PRId64
        ",\"last_dc_sample_valid\":%s,"
        "\"sync0_late_count\":%" PRIu64 ","
        "\"first_sync0_late_exchange\":%" PRIu64 ","
        "\"last_sync0_late_exchange\":%" PRIu64 ","
        /*
         * 极值自己的交换号。有了坐标才能回答"1.077 ms 的往返是不是就落在 WKC 不符
         * 的那几个周期里"，否则只能跟另一轮的极值比对，而两轮极值几乎相同。
         */
        "\"extreme_exchanges\":{\"max_round_trip\":%" PRIu64
        ",\"max_send_lateness\":%" PRIu64 ",\"min_sync0_margin\":%" PRIu64 "},"
        "\"distribution\":{",
        timing->has_send_duration ? "true" : "false", timing->min_send_duration_ns,
        timing->max_send_duration_ns, timing->has_round_trip ? "true" : "false",
        timing->min_round_trip_ns, timing->max_round_trip_ns,
        timing->has_send_lateness ? "true" : "false", timing->min_send_lateness_ns,
        timing->max_send_lateness_ns, timing->has_dc_phase ? "true" : "false",
        timing->min_dc_arrival_phase_ns, timing->max_dc_arrival_phase_ns,
        timing->has_phase_error ? "true" : "false", timing->min_phase_error_ns,
        timing->max_phase_error_ns, timing->has_sync0_margin ? "true" : "false",
        timing->min_sync0_margin_ns, timing->max_sync0_margin_ns,
        timing->last_phase_error_ns, timing->last_sync0_margin_ns,
        timing->last_dc_sample_valid ? "true" : "false",
        timing->sync0_late_count, timing->first_sync0_late_exchange,
        timing->last_sync0_late_exchange, timing->max_round_trip_exchange,
        timing->max_send_lateness_exchange, timing->min_sync0_margin_exchange) >= 0);
    REQUIRE_WRITE(write_distribution(stream, "send_duration_ns",
                                     &timing->send_duration_histogram));
    REQUIRE_WRITE(fputc(',', stream) != EOF);
    REQUIRE_WRITE(write_distribution(stream, "round_trip_ns",
                                     &timing->round_trip_histogram));
    REQUIRE_WRITE(fputc(',', stream) != EOF);
    REQUIRE_WRITE(write_distribution(stream, "send_lateness_ns",
                                     &timing->send_lateness_histogram));
    REQUIRE_WRITE(fputc(',', stream) != EOF);
    REQUIRE_WRITE(write_distribution(stream, "dc_arrival_phase_ns",
                                     &timing->dc_arrival_phase_histogram));
    REQUIRE_WRITE(fputc(',', stream) != EOF);
    REQUIRE_WRITE(write_distribution(stream, "phase_error_ns",
                                     &timing->phase_error_histogram));
    REQUIRE_WRITE(fputc(',', stream) != EOF);
    REQUIRE_WRITE(write_distribution(stream, "sync0_margin_ns",
                                     &timing->sync0_margin_histogram));
    /* close "distribution" and the timing object */
    REQUIRE_WRITE(fputs("}}", stream) != EOF);
    return true;
}

static bool write_axis(FILE *stream,
                       const emaster_session_axis_plan_t *axis_plan,
                       const emaster_control_session_axis_result_t *axis)
{
    const emaster_operation_profile_t *operation = axis_plan->operation_profile;
    const emaster_pdo_set_profile_t *pdo = axis_plan->pdo_set;

    REQUIRE_WRITE(fprintf(
        stream,
        "{\"position\":%u,\"axis_id\":", (unsigned int)axis->position) >= 0);
    REQUIRE_WRITE(emaster_json_string(stream, axis_plan->topology_slave->axis_id));
    REQUIRE_WRITE(fputs(",\"device_profile_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, axis_plan->device_profile->profile_id));
    REQUIRE_WRITE(fputs(",\"identity\":{\"expected\":{", stream) != EOF);
    REQUIRE_WRITE(fprintf(
        stream,
        "\"vendor_id\":\"0x%08" PRIX32 "\",\"product_code\":\"0x%08" PRIX32
        "\",\"revision\":\"0x%08" PRIX32 "\"},\"observed\":{"
        "\"vendor_id\":\"0x%08" PRIX32 "\",\"product_code\":\"0x%08" PRIX32
        "\",\"revision\":\"0x%08" PRIX32 "\"},\"match\":%s},",
        axis_plan->device_profile->identity.vendor_id,
        axis_plan->device_profile->identity.product_code,
        axis_plan->device_profile->identity.revision,
        axis->actual_vendor_id, axis->actual_product_code, axis->actual_revision,
        axis->identity_match ? "true" : "false") >= 0);
    REQUIRE_WRITE(fputs("\"operation\":{\"profile_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, operation->profile_id));
    REQUIRE_WRITE(fputs(",\"mode_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, operation->selected_mode_id));
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"mode_value\":%d,\"sync_strategy\":\"%s\","
        "\"sm2_sync_type\":\"0x%04X\",\"sm3_sync_type\":\"0x%04X\"},",
        (int)axis_plan->operation_mode->value,
        operation->sync_strategy == EMASTER_SYNC_STRATEGY_DC ? "dc" : "sm",
        (unsigned int)operation->sm2_sync_type,
        (unsigned int)operation->sm3_sync_type) >= 0);
    REQUIRE_WRITE(fputs("\"pdo\":{\"set_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, pdo->pdo_set_id));
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"verified\":%s,\"process_image_verified\":%s,"
        "\"output_bytes\":%" PRIu32 ",\"input_bytes\":%" PRIu32 ","
        "\"output_bits\":%" PRIu32 ",\"input_bits\":%" PRIu32 ","
        "\"output_offset_bytes\":%zu,\"input_offset_bytes\":%zu,\"rx\":",
        axis->pdo_match ? "true" : "false",
        axis->process_map_match ? "true" : "false",
        axis->output_bytes, axis->input_bytes,
        axis->output_bits, axis->input_bits, axis->output_offset_bytes,
        axis->input_offset_bytes) >= 0);
    REQUIRE_WRITE(write_mapping_direction(stream, pdo->rx_mappings,
                                          pdo->rx_mapping_count));
    REQUIRE_WRITE(fputs(",\"tx\":", stream) != EOF);
    REQUIRE_WRITE(write_mapping_direction(stream, pdo->tx_mappings,
                                          pdo->tx_mapping_count));
    REQUIRE_WRITE(fputs("},\"dc\":{", stream) != EOF);
    REQUIRE_WRITE(fprintf(
        stream,
        "\"requested\":%s,\"slave_capable\":%s,"
        "\"sync0_requested\":%s,\"sync0_active_after_configuration\":%s,"
        "\"register_read_succeeded\":%s,"
        "\"requested_cycle_ns\":%" PRIu32 ",\"observed_cycle_ns\":%" PRIu32
        ",\"requested_shift_ns\":%" PRId32 ",\"soem_shift_ns\":%" PRId32
        ",\"requested_assign_activate\":\"0x%04X\","
        "\"observed_assign_activate\":\"0x%04X\"},",
        axis->dc.requested ? "true" : "false",
        axis->dc.slave_capable ? "true" : "false",
        axis->dc.sync0_requested ? "true" : "false",
        axis->dc.sync0_active ? "true" : "false",
        axis->dc.register_read_succeeded ? "true" : "false",
        axis->dc.requested_cycle_ns, axis->dc.observed_cycle_ns,
        axis->dc.requested_shift_ns, axis->dc.soem_shift_ns,
        (unsigned int)axis->dc.requested_assign_activate,
        (unsigned int)axis->dc.observed_assign_activate) >= 0);
    REQUIRE_WRITE(fputs("\"position_scale\":", stream) != EOF);
    REQUIRE_WRITE(write_position_scale(stream, &axis->position_scale));
    REQUIRE_WRITE(fputs(",\"timing\":", stream) != EOF);
    REQUIRE_WRITE(write_timing(stream, &axis->timing));
    REQUIRE_WRITE(fputs(",\"sm2_diagnostic\":", stream) != EOF);
    REQUIRE_WRITE(write_sync_diagnostic(stream, &axis->sm2_diagnostic));
    REQUIRE_WRITE(fputs(",\"sm3_diagnostic\":", stream) != EOF);
    REQUIRE_WRITE(write_sync_diagnostic(stream, &axis->sm3_diagnostic));
    REQUIRE_WRITE(fputs(",\"sm_sync_probe\":", stream) != EOF);
    REQUIRE_WRITE(write_sync_probe(stream, axis->sm_sync_probe_count,
                                   axis->sm2_first_error_exchange, axis->sm2_first_error_missed,
                                   axis->sm2_first_error_sync_error, axis->sm2_last_missed,
                                   axis->sm2_last_sync_error, axis->sm3_first_error_exchange,
                                   axis->sm3_first_error_missed, axis->sm3_first_error_sync_error,
                                   axis->sm3_last_missed, axis->sm3_last_sync_error));
    REQUIRE_WRITE(fprintf(stream,
        ",\"shutdown_al\":{\"state\":%u,\"status_code\":%u}",
        (unsigned int)axis->shutdown_al_state, (unsigned int)axis->shutdown_al_status_code) >= 0);
    /* 停机序言的两个坐标：entry 在周期回路刚退出时读，pre_stop 在发出第一个安全
     * 停机帧之前读。两点相同说明序言没有让驱动器掉出 OP。 */
    REQUIRE_WRITE(fprintf(stream,
        ",\"shutdown_al_entry\":{\"state\":%u,\"status_code\":%u}",
        (unsigned int)axis->shutdown_entry_al_state,
        (unsigned int)axis->shutdown_entry_al_status_code) >= 0);
    REQUIRE_WRITE(fprintf(stream,
        ",\"shutdown_al_pre_stop\":{\"state\":%u,\"status_code\":%u}",
        (unsigned int)axis->shutdown_pre_stop_al_state,
        (unsigned int)axis->shutdown_pre_stop_al_status_code) >= 0);
    /*
     * 首个 WKC 不符当下的逐轴 AL 快照，是全报告中唯一一次"帧已经不对、而驱动器
     * 尚未被任何停机流程碰过"的读数：轴若此刻仍在 OP（8），说明帧异常在前；若已到
     * SAFE-OP（20 / 0x001A），说明驱动器侧监督在前。
     */
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"first_mismatch\":{\"al_read\":%s,\"al_state\":%u,\"al_status_code\":%u}",
        axis->first_mismatch_al_read ? "true" : "false",
        (unsigned int)axis->first_mismatch_al_state,
        (unsigned int)axis->first_mismatch_al_status_code) >= 0);
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"safeop_mode_readback\":{\"read_succeeded\":%s,\"mode_display\":%d,"
        "\"diagnostic_read_succeeded\":%s,\"cia402_error_code\":\"0x%04X\","
        "\"error_register\":\"0x%02X\","
        "\"extended_servo_error_code\":\"0x%08lX\",\"servo_error_code\":\"0x%08lX\"},"
        "\"runtime\":{\"status_word\":\"0x%04X\","
        "\"control_word\":\"0x%04X\",\"requested_mode\":%d,"
        "\"mode_display\":%d,\"warning\":%s,\"voltage_enabled\":%s,"
        "\"remote\":%s,\"target_reached\":%s,\"internal_limit_active\":%s,"
        "\"mode_specific_bits\":%u,\"manufacturer_specific_bits\":%u,"
        "\"software_position_limits_read\":%s,"
        "\"software_position_limit_min\":%" PRId32 ","
        "\"software_position_limit_max\":%" PRId32 ","
        "\"following_error_read\":%s,\"following_error_actual\":%" PRId32 ","
        "\"polarity_read\":%s,\"polarity\":%u,"
        "\"safeop_actual_position\":%" PRId32
        ",\"initial_actual_position\":%" PRId32
        ",\"actual_position\":%" PRId32 ",\"target_position\":%" PRId32
        ",\"actual_velocity\":%" PRId32 ",\"target_velocity\":%" PRId32
        ",\"actual_torque\":%d,\"target_torque\":%d"
        ",\"actual_current\":%d,\"dc_link_voltage\":%" PRIu32
        ",\"mosfet_temperature\":%d,\"motor_temperature\":%d"
        ",\"gain_parameters_read\":%s"
        ",\"velocity_loop_kp\":%u,\"velocity_loop_ki\":%u,\"velocity_loop_kd\":%u"
        ",\"position_loop_kp\":%u,\"position_loop_ki\":%u,\"position_loop_kd\":%u"
        ",\"current_loop_kp\":%u,\"current_loop_ki\":%u,\"current_loop_kd\":%u"
        ",\"operation_enabled_seen\":%s,\"motion_final_position\":%" PRId32
        ",\"max_following_error_counts\":%" PRIu64
        ",\"max_observed_following_error_counts\":%" PRIu64
        ",\"error_counters_read\":%s"
        ",\"rx_error_counter\":[%u,%u,%u,%u,%u,%u,%u,%u]"
        ",\"forwarded_rx_error_counter\":[%u,%u,%u,%u]"
        ",\"ecat_processing_unit_error_counter\":%u"
        ",\"pdi_error_counter\":%u,\"pdi_error_code\":%u"
        ",\"esc_registers_read\":%s"
        ",\"esc_dl_control\":%u,\"esc_watchdog_pdi\":%u"
        ",\"final_diagnostic_reads\":%zu,"
        "\"final_diagnostic_successes\":%zu}}",
        axis->safeop_mode_display_sdo_read ? "true" : "false",
        (int)axis->safeop_mode_display_sdo,
        axis->safeop_drive_diagnostic.read_succeeded ? "true" : "false",
        (unsigned int)axis->safeop_drive_diagnostic.cia402_error_code,
        (unsigned int)axis->safeop_drive_diagnostic.error_register,
        (unsigned long)axis->safeop_drive_diagnostic.extended_servo_error_code,
        (unsigned long)axis->safeop_drive_diagnostic.servo_error_code,
        (unsigned int)axis->status_word, (unsigned int)axis->control_word,
        (int)axis->requested_mode, (int)axis->mode_display,
        axis->status_warning ? "true" : "false",
        axis->voltage_enabled ? "true" : "false",
        axis->remote ? "true" : "false",
        axis->target_reached ? "true" : "false",
        axis->internal_limit_active ? "true" : "false",
        (unsigned int)axis->mode_specific_status,
        (unsigned int)axis->manufacturer_specific_status,
        axis->software_position_limits_read ? "true" : "false",
        axis->software_position_limit_min,
        axis->software_position_limit_max,
        axis->following_error_read ? "true" : "false",
        axis->following_error_actual,
        axis->polarity_read ? "true" : "false",
        (unsigned int)axis->polarity,
        axis->safeop_actual_position, axis->initial_actual_position,
        axis->actual_position,
        axis->target_position,
        axis->actual_velocity,
        axis->target_velocity,
        (int)axis->actual_torque,
        (int)axis->target_torque,
        (int)axis->actual_current,
        axis->dc_link_voltage,
        (int)axis->mosfet_temperature,
        (int)axis->motor_temperature,
        axis->gain_parameters_read ? "true" : "false",
        (unsigned int)axis->velocity_loop_kp,
        (unsigned int)axis->velocity_loop_ki,
        (unsigned int)axis->velocity_loop_kd,
        (unsigned int)axis->position_loop_kp,
        (unsigned int)axis->position_loop_ki,
        (unsigned int)axis->position_loop_kd,
        (unsigned int)axis->current_loop_kp,
        (unsigned int)axis->current_loop_ki,
        (unsigned int)axis->current_loop_kd,
        axis->operation_enabled_seen ? "true" : "false",
        axis->motion_final_position, axis->max_following_error_counts,
        axis->max_observed_following_error_counts,
        axis->error_counters_read ? "true" : "false",
        (unsigned int)axis->rx_error_counter[0],
        (unsigned int)axis->rx_error_counter[1],
        (unsigned int)axis->rx_error_counter[2],
        (unsigned int)axis->rx_error_counter[3],
        (unsigned int)axis->rx_error_counter[4],
        (unsigned int)axis->rx_error_counter[5],
        (unsigned int)axis->rx_error_counter[6],
        (unsigned int)axis->rx_error_counter[7],
        (unsigned int)axis->forwarded_rx_error_counter[0],
        (unsigned int)axis->forwarded_rx_error_counter[1],
        (unsigned int)axis->forwarded_rx_error_counter[2],
        (unsigned int)axis->forwarded_rx_error_counter[3],
        (unsigned int)axis->ecat_processing_unit_error_counter,
        (unsigned int)axis->pdi_error_counter,
        (unsigned int)axis->pdi_error_code,
        axis->esc_registers_read ? "true" : "false",
        (unsigned int)axis->esc_dl_control,
        (unsigned int)axis->esc_watchdog_pdi,
        axis->final_diagnostic_read_count,
        axis->final_diagnostic_success_count) >= 0);
    return true;
}

static bool write_motion(FILE *stream, const emaster_motion_profile_t *motion)
{
    size_t axis_index;

    if (motion == NULL)
    {
        return fputs("null", stream) != EOF;
    }
    REQUIRE_WRITE(fputs("{\"profile_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, motion->motion_profile_id));
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"coordinate\":\"%s\",\"duration_ms\":%" PRIu32
        ",\"settle_ms\":%" PRIu32 ",\"axes\":[",
        motion->coordinate == EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT
            ? "output_shaft" : "motor_rotor",
        motion->duration_ms, motion->settle_ms) >= 0);
    for (axis_index = 0U; axis_index < motion->axis_count; ++axis_index)
    {
        const emaster_motion_axis_config_t *axis = &motion->axes[axis_index];
        REQUIRE_WRITE(fprintf(
            stream,
            "%s{\"axis_id\":", axis_index == 0U ? "" : ",") >= 0);
        REQUIRE_WRITE(emaster_json_string(stream, axis->axis_id));
        REQUIRE_WRITE(fprintf(
            stream,
            ",\"relative_angle_millidegrees\":%" PRId32
            ",\"target_velocity_millidegrees_per_second\":%" PRId32
            ",\"acceleration_millidegrees_per_second2\":%" PRIu32
            ",\"deceleration_millidegrees_per_second2\":%" PRIu32
            ",\"max_following_error_millidegrees\":%" PRIu32
            ",\"max_velocity_error_millidegrees_per_second\":%" PRIu32
            ",\"expected_position_scale\":{\"encoder_increments\":%" PRIu32
            ",\"encoder_motor_revolutions\":%" PRIu32
            ",\"gear_motor_revolutions\":%" PRIu32
            ",\"gear_shaft_revolutions\":%" PRIu32 "}}",
            axis->relative_angle_millidegrees,
            axis->target_velocity_millidegrees_per_second,
            axis->acceleration_millidegrees_per_second2,
            axis->deceleration_millidegrees_per_second2,
            axis->max_following_error_millidegrees,
            axis->max_velocity_error_millidegrees_per_second,
            axis->expected_encoder_increments,
            axis->expected_encoder_motor_revolutions,
            axis->expected_gear_motor_revolutions,
            axis->expected_gear_shaft_revolutions) >= 0);
    }
    return fputs("]}", stream) != EOF;
}

static bool write_access(FILE *stream,
                         const emaster_session_plan_t *plan,
                         const emaster_audit_access_t *access,
                         bool first)
{
    const emaster_pdo_entry_t *entry = NULL;
    const char *configured_name = NULL;

    if (access->transport == EMASTER_AUDIT_TRANSPORT_PDO &&
        access->slave_position > 0U &&
        (size_t)access->slave_position <= plan->axis_count)
    {
        entry = pdo_entry_for(&plan->axes[access->slave_position - 1U],
                              access->direction, access->index,
                              access->subindex);
    }
    else if (access->transport == EMASTER_AUDIT_TRANSPORT_SDO &&
             access->slave_position > 0U &&
             (size_t)access->slave_position <= plan->axis_count)
    {
        configured_name = configured_sdo_name(
            &plan->axes[access->slave_position - 1U], access->index,
            access->subindex);
    }
    REQUIRE_WRITE(fprintf(
        stream,
        "%s{\"order\":%" PRIu64 ",\"phase\":",
        first ? "" : ",", access->order) >= 0);
    REQUIRE_WRITE(emaster_json_string(stream, phase_name(access->phase)));
    REQUIRE_WRITE(fputs(",\"transport\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, transport_name(access->transport)));
    REQUIRE_WRITE(fputs(",\"direction\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, direction_name(access->direction)));
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"slave_position\":%u,\"object\":\"0x%04X:%02X\","
        "\"bit_length\":%u,\"bit_offset\":%" PRIu32 ",\"name\":",
        (unsigned int)access->slave_position, (unsigned int)access->index,
        (unsigned int)access->subindex, (unsigned int)access->bit_length,
        access->bit_offset) >= 0);
    if (entry == NULL && configured_name == NULL)
    {
        REQUIRE_WRITE(fputs("null", stream) != EOF);
    }
    else
    {
        REQUIRE_WRITE(emaster_json_string(
            stream, entry != NULL ? entry->name : configured_name));
    }
    REQUIRE_WRITE(fputs(",\"value_type\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(
        stream, access->value_kind == EMASTER_AUDIT_VALUE_SIGNED
                    ? "signed" : "unsigned"));
    REQUIRE_WRITE(fputs(",\"value_dec\":", stream) != EOF);
    if (access->value_kind == EMASTER_AUDIT_VALUE_SIGNED)
    {
        REQUIRE_WRITE(emaster_json_i64(stream, access->signed_value));
    }
    else
    {
        REQUIRE_WRITE(emaster_json_u64(stream, access->unsigned_value));
    }
    REQUIRE_WRITE(fputs(",\"value_hex\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_hex(stream, value_bits(access), access->bit_length));
    REQUIRE_WRITE(fputs(",\"raw_bus_bytes_hex\":", stream) != EOF);
    if (access->raw_size == 0U)
    {
        REQUIRE_WRITE(fputs("null", stream) != EOF);
    }
    else
    {
        REQUIRE_WRITE(emaster_json_raw_hex(stream, access->raw, access->raw_size));
    }
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"succeeded\":%s,\"first_exchange\":%" PRIu64
        ",\"last_exchange\":%" PRIu64 ",\"sample_count\":%" PRIu64 "}",
        access->succeeded ? "true" : "false", access->first_exchange,
        access->last_exchange, access->sample_count) >= 0);
    return true;
}

static bool write_cycle_failure(FILE *stream, const emaster_cycle_failure_t *failure)
{
    if (!failure->present)
    {
        return fputs("null", stream) != EOF;
    }
    REQUIRE_WRITE(fprintf(stream,
        "{\"status_code\":%u,\"phase\":\"%s\",\"exchange\":%" PRIu64
        ",\"last_dc_time_ns\":%" PRId64 ",\"wkc\":",
        (unsigned int)failure->status, phase_name(failure->phase),
        failure->exchange, failure->dc_time_ns) >= 0);
    if (failure->wkc_available)
    {
        REQUIRE_WRITE(fprintf(stream, "%d", failure->wkc) >= 0);
    }
    else
    {
        REQUIRE_WRITE(fputs("null", stream) != EOF);
    }
    return fputs("}", stream) != EOF;
}

/*
 * 运行时故障现场：故障瞬间主站看到的原始状态字、解码结果和已生成的控制字。
 * 与 first_cycle_failure（交换本身失败）互补，覆盖协调器拒绝、健康检查和使能确认。
 * coordinator_status 为 -1 表示故障发生时协调器还没被调用过。
 */
static bool write_runtime_failure(FILE *stream, const emaster_runtime_failure_t *failure)
{
    size_t axis_index;
    size_t axis_count;

    if (!failure->present)
    {
        return fputs("null", stream) != EOF;
    }
    axis_count = failure->axis_count;
    if (axis_count > EMASTER_RUNTIME_FAILURE_MAX_AXES)
    {
        axis_count = EMASTER_RUNTIME_FAILURE_MAX_AXES;
    }
    REQUIRE_WRITE(fprintf(stream,
        "{\"status_code\":%u,\"cycle_count\":%" PRIu64 ",\"exchange\":%" PRIu64
        ",\"coordinator_status\":%d,\"axis_count\":%zu,\"axes\":[",
        (unsigned int)failure->status, failure->cycle_count, failure->exchange,
        failure->coordinator_status, axis_count) >= 0);
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        REQUIRE_WRITE(fprintf(stream,
            "%s{\"status_word\":\"0x%04X\",\"control_word\":\"0x%04X\","
            "\"observed_state\":%u,\"state_known\":%s,\"fault_present\":%s}",
            axis_index == 0U ? "" : ",",
            (unsigned int)failure->status_words[axis_index],
            (unsigned int)failure->control_words[axis_index],
            (unsigned int)failure->observed_states[axis_index],
            failure->state_known[axis_index] ? "true" : "false",
            failure->fault_present[axis_index] ? "true" : "false") >= 0);
    }
    return fputs("]}", stream) != EOF;
}

/* 单周期现场。无 DC 样本时 sync0_margin 输出 null，不编造数值。 */
static bool write_cycle_trace_sample(FILE *stream,
                                     const emaster_cycle_trace_sample_t *sample)
{
    REQUIRE_WRITE(fprintf(
        stream,
        "{\"exchange\":%" PRIu64 ",\"wkc\":%d,\"flags\":%" PRIu32
        ",\"send_duration_ns\":%" PRIu32
        ",\"receive_duration_ns\":%" PRIu32
        ",\"mailbox_duration_ns\":%" PRIu32
        ",\"error_counter_duration_ns\":%" PRIu32
        ",\"send_lateness_ns\":%" PRId32
        ",\"frame_interval_ns\":%" PRIu32 ",\"sync0_margin_ns\":",
        sample->exchange, sample->wkc, sample->flags, sample->send_duration_ns,
        sample->receive_duration_ns, sample->mailbox_duration_ns,
        sample->error_counter_duration_ns, sample->send_lateness_ns,
        sample->frame_interval_ns) >= 0);
    if (sample->sync0_margin_ns == INT32_MIN)
    {
        REQUIRE_WRITE(fputs("null", stream) != EOF);
    }
    else
    {
        REQUIRE_WRITE(fprintf(stream, "%" PRId32, sample->sync0_margin_ns) >= 0);
    }
    return fputc('}', stream) != EOF;
}

/* 停机循环单拍现场。state_known 为假的轴上 state 无意义，仍照原值输出：
 * 报告只负责标出"这一列这一刻是否有效"，不替读者判断。 */
static bool write_shutdown_cycle_sample(FILE *stream,
                                        const emaster_shutdown_cycle_sample_t *sample)
{
    size_t axis_index;
    size_t axis_count = sample->axis_count;

    if (axis_count > EMASTER_RUNTIME_FAILURE_MAX_AXES)
    {
        axis_count = EMASTER_RUNTIME_FAILURE_MAX_AXES;
    }
    REQUIRE_WRITE(fprintf(stream,
        "{\"exchange\":%" PRIu64 ",\"wkc\":%" PRId32 ",\"exchange_status\":%d"
        ",\"all_axes_safe\":%s,\"axes_decoded\":%s,\"axes\":[",
        sample->exchange, sample->wkc, sample->exchange_status,
        sample->all_axes_safe ? "true" : "false",
        sample->axes_decoded ? "true" : "false") >= 0);
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        REQUIRE_WRITE(fprintf(stream,
            "%s{\"status_word\":\"0x%04X\",\"control_word\":\"0x%04X\","
            "\"state\":%u,\"state_known\":%s}",
            axis_index == 0U ? "" : ",",
            (unsigned int)sample->status_words[axis_index],
            (unsigned int)sample->control_words[axis_index],
            (unsigned int)sample->cia402_states[axis_index],
            sample->states_known[axis_index] ? "true" : "false") >= 0);
    }
    return fputs("]}", stream) != EOF;
}

/* 停机循环的逐周期现场：头若干拍 + 最后一拍 + 首次 WKC 不符时的 AL 快照。 */
static bool write_shutdown_cycles(FILE *stream, const emaster_shutdown_cycle_trace_t *trace)
{
    size_t axis_index;
    size_t axis_count;

    REQUIRE_WRITE(fprintf(stream,
        "{\"capacity\":%u,\"cycle_total\":%" PRIu64 ",\"sample_count\":%zu"
        ",\"aborted_before_exchange\":%s,\"aborted_axis\":%zu,\"aborted_stage\":%d"
        ",\"mismatch_al_read\":%s,\"mismatch_al_read_ns\":%" PRIu64
        ",\"mismatch_al_exchange\":%" PRIu64 ",\"mismatch_al\":[",
        (unsigned int)EMASTER_SHUTDOWN_CYCLE_CAPACITY, trace->cycle_total,
        trace->sample_count, trace->aborted_before_exchange ? "true" : "false",
        trace->aborted_axis, trace->aborted_stage,
        trace->mismatch_al_read ? "true" : "false", trace->mismatch_al_read_ns,
        trace->mismatch_al_exchange) >= 0);
    axis_count = trace->mismatch_al_axis_count;
    if (axis_count > EMASTER_RUNTIME_FAILURE_MAX_AXES)
    {
        axis_count = EMASTER_RUNTIME_FAILURE_MAX_AXES;
    }
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        REQUIRE_WRITE(fprintf(stream, "%s{\"axis\":%zu,\"state\":%u,\"status_code\":%u}",
            axis_index == 0U ? "" : ",", axis_index + 1U,
            (unsigned int)trace->mismatch_al_state[axis_index],
            (unsigned int)trace->mismatch_al_status_code[axis_index]) >= 0);
    }
    REQUIRE_WRITE(fputs("],\"samples\":[", stream) != EOF);
    for (size_t index = 0U; index < trace->sample_count; ++index)
    {
        REQUIRE_WRITE(fputs(index == 0U ? "" : ",", stream) != EOF);
        REQUIRE_WRITE(write_shutdown_cycle_sample(stream, &trace->samples[index]));
    }
    REQUIRE_WRITE(fputs("],\"last\":", stream) != EOF);
    if (trace->last_valid)
    {
        REQUIRE_WRITE(write_shutdown_cycle_sample(stream, &trace->last));
    }
    else
    {
        REQUIRE_WRITE(fputs("null", stream) != EOF);
    }
    /*
     * 停机首拍的定位仪表。时刻都是 CLOCK_MONOTONIC 绝对值，与前面的 mark_* 不同；
     * shutdown_prologue.origin_monotonic_ns 是同一条时间轴上的原点。
     */
    REQUIRE_WRITE(fprintf(stream,
        ",\"attempt_count\":%zu,\"attempts\":[", trace->attempt_count) >= 0);
    for (size_t index = 0U; index < trace->attempt_count; ++index)
    {
        const emaster_shutdown_attempt_t *attempt = &trace->attempts[index];

        REQUIRE_WRITE(fprintf(stream,
            "%s{\"begin_ns\":%" PRIu64 ",\"deadline_before_ns\":%" PRIu64
            ",\"deadline_after_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64
            ",\"exchange_after\":%" PRIu64 ",\"exchange_status\":%d"
            ",\"correction_ns\":%" PRId64 ",\"phase_error_ns\":%" PRId64 "}",
            index == 0U ? "" : ",", attempt->begin_ns, attempt->deadline_before_ns,
            attempt->deadline_after_ns, attempt->end_ns, attempt->exchange_after,
            (int)attempt->exchange_status, attempt->correction_ns,
            attempt->phase_error_ns) >= 0);
    }
    REQUIRE_WRITE(fputc(']', stream) != EOF);
    return fputc('}', stream) != EOF;
}

/*
 * 停止观测线程的相位现场。join 是双峰的（≤0.806 ms 全通过 / ≥0.935 ms 全失败），
 * 这里把这两峰拆成三段：标志落下 → 被看见 → 循环退出 → 线程返回。
 * site/phase 是分类，时刻才是判据；phase 为 UNKNOWN 表示标志落在已被覆盖的更早相位里
 * （见 emaster_observer_stop_trace_t 的说明），不是"没有相位"。
 */
static bool write_observer_stop(FILE *stream, const emaster_observer_stop_trace_t *stop)
{
    return fprintf(stream,
        "{\"stop_flag_ns\":%" PRIu64 ",\"stop_seen_ns\":%" PRIu64
        ",\"loop_exit_ns\":%" PRIu64 ",\"exit_ns\":%" PRIu64
        ",\"stop_phase\":%d,\"stop_phase_begin_ns\":%" PRIu64
        ",\"stop_site\":%d,\"stop_axis\":%zu,\"stop_iteration\":%" PRIu64
        ",\"iteration_count\":%" PRIu64 ",\"iteration_max_ns\":%" PRIu64
        ",\"read_count\":%" PRIu64 ",\"read_max_ns\":%" PRIu64
        ",\"read_last_ns\":%" PRIu64 "}",
        stop->stop_flag_ns, stop->stop_seen_ns, stop->loop_exit_ns, stop->exit_ns,
        stop->stop_phase, stop->stop_phase_begin_ns, stop->stop_site, stop->stop_axis,
        stop->stop_iteration, stop->iteration_count, stop->iteration_max_ns,
        stop->read_count, stop->read_max_ns, stop->read_last_ns) >= 0;
}

/* 收尾时抓的各线程调度累计值。tid 是唯一标识（内核 comm 对同进程线程是同一个）。 */
static bool write_thread_schedstat(FILE *stream, const emaster_control_session_report_t *report)
{
    REQUIRE_WRITE(fputc('[', stream) != EOF);
    for (size_t index = 0U; index < report->thread_schedstat_count; ++index)
    {
        const emaster_thread_schedstat_t *row = &report->thread_schedstat[index];

        REQUIRE_WRITE(fprintf(stream,
            "%s{\"tid\":%" PRIu32 ",\"policy\":%d,\"priority\":%d,\"cpus_allowed\":",
            index == 0U ? "" : ",", row->tid, row->policy, row->priority) >= 0);
        REQUIRE_WRITE(emaster_json_string(stream, row->cpus_allowed));
        REQUIRE_WRITE(fprintf(stream,
            ",\"exec_ns\":%" PRIu64 ",\"wait_ns\":%" PRIu64 ",\"switches\":%" PRIu64 "}",
            row->exec_ns, row->wait_ns, row->switches) >= 0);
    }
    return fputc(']', stream) != EOF;
}

/*
 * 周期现场环的两份记录：最近 64 个周期（环形，需按时间顺序展开），
 * 以及第一个 WKC 不符之前冻结的那 64 个周期（冻结时已展开）。
 */
static bool write_cycle_trace(FILE *stream, const emaster_cycle_trace_t *trace)
{
    size_t start = (trace->write_index + EMASTER_CYCLE_TRACE_CAPACITY -
                    trace->sample_count) %
                   EMASTER_CYCLE_TRACE_CAPACITY;

    REQUIRE_WRITE(fprintf(stream,
        "{\"capacity\":%u,\"live\":{\"count\":%zu,\"samples\":[",
        (unsigned int)EMASTER_CYCLE_TRACE_CAPACITY, trace->sample_count) >= 0);
    for (size_t index = 0U; index < trace->sample_count; ++index)
    {
        REQUIRE_WRITE(fputs(index == 0U ? "" : ",", stream) != EOF);
        REQUIRE_WRITE(write_cycle_trace_sample(
            stream, &trace->samples[(start + index) % EMASTER_CYCLE_TRACE_CAPACITY]));
    }
    REQUIRE_WRITE(fprintf(stream,
        "]},\"mismatch\":{\"present\":%s,\"exchange\":%" PRIu64
        ",\"wkc\":%d,\"count\":%zu,\"samples\":[",
        trace->mismatch_present ? "true" : "false", trace->mismatch_exchange,
        trace->mismatch_wkc, trace->mismatch_sample_count) >= 0);
    for (size_t index = 0U; index < trace->mismatch_sample_count; ++index)
    {
        REQUIRE_WRITE(fputs(index == 0U ? "" : ",", stream) != EOF);
        REQUIRE_WRITE(write_cycle_trace_sample(stream, &trace->mismatch_samples[index]));
    }
    return fputs("]}}", stream) != EOF;
}

bool emaster_run_report_write(FILE *stream,
                              const emaster_session_plan_t *plan,
                              const emaster_control_session_report_t *report,
                              const char *generated_at_utc)
{
    size_t index;

    if (stream == NULL || plan == NULL || report == NULL ||
        plan->deployment == NULL || generated_at_utc == NULL ||
        plan->axis_count != report->axis_count || report->axes == NULL)
    {
        return false;
    }
    REQUIRE_WRITE(fputs("{\"schema_version\":4,\"generated_at_utc\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, generated_at_utc));
    REQUIRE_WRITE(fputs(",\"deployment\":{\"id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, plan->deployment->deployment_id));
    REQUIRE_WRITE(fputs(",\"hostname\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, plan->deployment->hostname));
    REQUIRE_WRITE(fputs(",\"ethercat_interface\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, plan->deployment->ethercat_interface));
    REQUIRE_WRITE(fputs(",\"topology_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, plan->deployment->topology->topology_id));
    /*
     * 报告实际落盘的位置。相对路径按进程 CWD 解析，从别处启动就写到别处——没有
     * 这一列，那种事故在报告里看不出任何异常。调用方没解析时留空。
     */
    REQUIRE_WRITE(fputs(",\"report_path\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, report->report_path));
    REQUIRE_WRITE(fprintf(
        stream,
        "},\"result\":{\"status_code\":%u,\"control_state\":%u,\"io_map_size\":%zu,"
        "\"expected_wkc\":%u,\"actual_wkc\":%d,\"cycle_count\":%" PRIu64
        ",\"process_data_exchange_count\":%" PRIu64
        ",\"cycle_deadline_missed\":%s,"
        "\"wkc_error_count\":%" PRIu64
        ",\"wkc_consecutive_errors\":%" PRIu64
        ",\"wkc_max_consecutive_errors\":%" PRIu64
        /*
         * 整帧缺失（actual_wkc<=0，一个回帧都没有）单独一组。wkc_* 是两类故障的
         * 联合计数，无法回答"这轮到底是帧没回来还是从站少计"——长时运行掉出 OP
         * 的证据是前者，从站掉出 OP 的证据是后者，两者需要分开看。
         */
        ",\"wkc_no_frame_count\":%" PRIu64
        ",\"wkc_no_frame_consecutive_errors\":%" PRIu64
        ",\"wkc_no_frame_max_consecutive_errors\":%" PRIu64
        ",\"wkc_no_frame_first_exchange\":%" PRIu64
        /*
         * 累计阈值的滑动窗口内峰值。阈值判据是"最近 N 毫秒内错 M 次"，这两个字段
         * 回答"离阈值还有多远"——只有总量和时长的话，看不出余量是在被慢慢吃掉。
         */
        ",\"wkc_short_frame_window_max\":%" PRIu64
        ",\"wkc_no_frame_window_max\":%" PRIu64
        ",\"fault_latched\":%s,"
        "\"safety_control_permitted\":%s,\"safety_blocking_reasons\":%" PRIu32
        ",\"safe_op_reached\":%s,\"op_reached\":%s,"
        "\"all_axes_enabled_reached\":%s,\"motion_started\":%s,"
        "\"motion_completed\":%s,\"safe_output_sent\":%s,"
        "\"restore_init_succeeded\":%s,"
        "\"shutdown_observer_join_ns\":%" PRIu64
        ",\"shutdown_prologue_gap_ns\":%" PRIu64 "},",
        (unsigned int)report->status, (unsigned int)report->state, report->io_map_size,
        (unsigned int)report->expected_wkc, report->actual_wkc,
        report->cycle_count, report->process_data_exchange_count,
        report->cycle_deadline_missed ? "true" : "false",
        report->wkc_error_count,
        report->wkc_consecutive_errors,
        report->wkc_max_consecutive_errors,
        report->wkc_no_frame_count,
        report->wkc_no_frame_consecutive_errors,
        report->wkc_no_frame_max_consecutive_errors,
        report->wkc_no_frame_first_exchange,
        report->wkc_short_frame_window_max,
        report->wkc_no_frame_window_max,
        report->fault_latched ? "true" : "false",
        report->safety_control_permitted ? "true" : "false",
        report->safety_blocking_reasons,
        report->safe_op_reached ? "true" : "false",
        report->op_reached ? "true" : "false",
        report->all_axes_enabled_reached ? "true" : "false",
        report->motion_started ? "true" : "false",
        report->motion_completed ? "true" : "false",
        report->safe_output_sent ? "true" : "false",
        report->safe_state_reached ? "true" : "false",
        report->sync0_disabled ? "true" : "false",
        report->restore_init_succeeded ? "true" : "false",
        report->shutdown_observer_join_ns,
        report->shutdown_prologue_gap_ns) >= 0);
    REQUIRE_WRITE(fputs("\"first_cycle_failure\":", stream) != EOF);
    REQUIRE_WRITE(write_cycle_failure(stream, &report->first_cycle_failure));
    REQUIRE_WRITE(fputs(",\"first_runtime_failure\":", stream) != EOF);
    REQUIRE_WRITE(write_runtime_failure(stream, &report->first_runtime_failure));
    /*
     * 周期尾部的三个分段耗时极值。round_trip 把"收包/邮箱推进/3×FPRD"算成一个数，
     * 因此 1.077 ms 的往返极值此前无法归因；over_budget 是主站自己吃掉超过一个
     * 周期的次数，直接对应"发得出去但发得不及时"的候选。
     */
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"process_data_delivery\":{\"tail_max_receive_ns\":%" PRIu64
        /*
         * 帧超时的标定对：ok 版上限只收"真有帧回来"的周期（超时周期恒等于超时值，
         * 是删失数据），frame_timeout_us 是由它导出的实际取值。两个一起看才能回答
         * "超时相对回程分布还留了多少余量"——只给 tail_max_receive_ns 会被超时值
         * 本身污染（它至少等于超时）。
         */
        ",\"tail_max_receive_ok_ns\":%" PRIu64
        ",\"frame_timeout_us\":%" PRIu32
        ",\"tail_max_mailbox_ns\":%" PRIu64
        ",\"tail_max_error_counter_ns\":%" PRIu64
        ",\"error_counter_read_fail_count\":%" PRIu64
        ",\"first_error_counter_read_fail_exchange\":%" PRIu64
        /*
         * 采样记账三个数：到了采样点并真的读了（attempt）、到了采样点但因为本周期
         * 已经出问题而跳过（skip）、没到采样点（不记账，可由 cycle_count 反推）。
         * 分开之后，read_fail 才只表示"想读但没读到"。
         */
        ",\"error_counter_read_attempt_count\":%" PRIu64
        ",\"error_counter_skip_count\":%" PRIu64
        ",\"first_error_counter_skip_exchange\":%" PRIu64
        ",\"over_budget_cycle_count\":%" PRIu64
        ",\"first_over_budget_exchange\":%" PRIu64
        ",\"first_deadline_missed_exchange\":%" PRIu64
        ",\"first_mismatch_present\":%s,\"first_mismatch_exchange\":%" PRIu64
        ",\"first_mismatch_wkc\":%d"
        /*
         * 帧距：驱动器真正看到的过程数据节奏。gap_count 用 1.5 个周期做阈值，
         * 单独一条就能回答"总线有没有整周期跳发过"——不需要去逐条翻现场环。
         */
        ",\"frame_interval_max_ns\":%" PRIu64
        ",\"frame_interval_max_exchange\":%" PRIu64
        ",\"frame_interval_gap_count\":%" PRIu64
        ",\"first_frame_interval_gap_exchange\":%" PRIu64
        ",\"first_mismatch_frame_interval_ns\":%" PRIu64
        ",\"deadline_miss_frame_interval_ns\":%" PRIu64
        ",\"tail_uncovered_max_ns\":%" PRIu64
        ",\"tail_uncovered_max_exchange\":%" PRIu64
        ",\"deadline_miss_tail_uncovered_ns\":%" PRIu64 "},",
        report->tail_max_receive_ns, report->tail_max_receive_ok_ns,
        (unsigned)report->frame_timeout_us,
        report->tail_max_mailbox_ns,
        report->tail_max_error_counter_ns, report->error_counter_read_fail_count,
        report->first_error_counter_read_fail_exchange,
        report->error_counter_read_attempt_count, report->error_counter_skip_count,
        report->first_error_counter_skip_exchange, report->over_budget_cycle_count,
        report->first_over_budget_exchange, report->first_deadline_missed_exchange,
        report->first_mismatch_present ? "true" : "false", report->first_mismatch_exchange,
        report->first_mismatch_wkc, report->frame_interval_max_ns,
        report->frame_interval_max_exchange, report->frame_interval_gap_count,
        report->first_frame_interval_gap_exchange,
        report->first_mismatch_frame_interval_ns,
        report->deadline_miss_frame_interval_ns, report->tail_uncovered_max_ns,
        report->tail_uncovered_max_exchange,
        report->deadline_miss_tail_uncovered_ns) >= 0);
    /*
     * 观测通道的自证段。enabled 是这一轮 A/B 走的是哪条臂的唯一凭据（与
     * shutdown_prologue 的 fast_mode/inline_mode 同一个手法）；publish_max_ns 与
     * publish_over_budget_count 是发布点自身的成本证据——发布在周期线程里，它的开销
     * 属于控制回路，不该只存在于离线自测的结论里。
     *
     * publish_budget_ns 一并写出，是为了让"超过预算几次"这个计数始终可解释：
     * 阈值是周期/100，换了周期它就该跟着变，而不是留在判据里当常量。
     */
    REQUIRE_WRITE(fprintf(
        stream,
        "\"observation\":{\"enabled\":%s,\"ring_capacity\":%u"
        ",\"published_frames\":%" PRIu64
        ",\"first_publish_exchange\":%" PRIu64
        ",\"publish_max_ns\":%" PRIu64
        ",\"publish_max_exchange\":%" PRIu64
        ",\"publish_budget_ns\":%" PRIu64
        ",\"publish_over_budget_count\":%" PRIu64 "},",
        report->observation.enabled ? "true" : "false",
        (unsigned)report->observation.ring_capacity,
        report->observation.published_frames,
        report->observation.first_publish_exchange,
        report->observation.publish_max_ns,
        report->observation.publish_max_exchange,
        report->observation.publish_budget_ns,
        report->observation.publish_over_budget_count) >= 0);
    /*
     * 停机段的仪表（缺口为什么是 5～6 ms 而不是 1 ms）：序言分段计时、停机循环逐周期
     * 现场、观测线程的停止相位、各线程调度累计值。mark_* 都是相对序言起点的累计位置，
     * start_valid 为假时全部为 0（"没测到"，不是"耗时为零"）。
     */
    REQUIRE_WRITE(fprintf(stream,
        "\"shutdown_prologue\":{\"start_valid\":%s,\"fast_mode\":%s,\"inline_mode\":%s"
        ",\"inline_drain_ns\":%" PRIu64 ",\"inline_drain_cycles\":%" PRIu64
        ",\"mark_after_entry_al_ns\":%" PRIu64 ",\"mark_after_join_ns\":%" PRIu64
        ",\"mark_after_pre_stop_al_ns\":%" PRIu64 ",\"mark_after_audit_ns\":%" PRIu64
        ",\"mark_first_safe_frame_ns\":%" PRIu64
        ",\"entry_al_read_ns\":%" PRIu64 ",\"pre_stop_al_read_ns\":%" PRIu64
        ",\"origin_monotonic_ns\":%" PRIu64
        "},\"shutdown_cycles\":",
        report->shutdown_prologue.start_valid ? "true" : "false",
        report->shutdown_prologue.fast_mode ? "true" : "false",
        report->shutdown_prologue.inline_mode ? "true" : "false",
        report->shutdown_prologue.inline_drain_ns,
        report->shutdown_prologue.inline_drain_cycles,
        report->shutdown_prologue.mark_after_entry_al_ns,
        report->shutdown_prologue.mark_after_join_ns,
        report->shutdown_prologue.mark_after_pre_stop_al_ns,
        report->shutdown_prologue.mark_after_audit_ns,
        report->shutdown_prologue.mark_first_safe_frame_ns,
        report->shutdown_prologue.entry_al_read_ns,
        report->shutdown_prologue.pre_stop_al_read_ns,
        report->shutdown_prologue.origin_monotonic_ns) >= 0);
    REQUIRE_WRITE(write_shutdown_cycles(stream, &report->shutdown_cycles));
    REQUIRE_WRITE(fputs(",\"observer_stop\":", stream) != EOF);
    REQUIRE_WRITE(write_observer_stop(stream, &report->observer_stop));
    REQUIRE_WRITE(fputs(",\"thread_schedstat\":", stream) != EOF);
    REQUIRE_WRITE(write_thread_schedstat(stream, report));
    REQUIRE_WRITE(fputs(",\"cycle_trace\":", stream) != EOF);
    REQUIRE_WRITE(write_cycle_trace(stream, &report->cycle_trace));
    REQUIRE_WRITE(fprintf(stream,
        ",\"audit\":{\"omitted_pdo_samples\":%" PRIu64 ",\"cyclic_capacity\":%zu},"
        "\"diagnostic_preop_reached\":%s,",
        report->audit.omitted_pdo_samples,
        report->audit.sealed_capacity,
        report->diagnostic_preop_reached ? "true" : "false") >= 0);
    REQUIRE_WRITE(fprintf(
        stream,
        "\"dc\":{\"required\":%s,\"configured\":%s,\"startup_stable\":%s,"
        "\"reference_slave_position\":%u,\"process_data_phase_ns\":%" PRIu32
        ",\"startup_cycles_requested\":%" PRIu32
        ",\"startup_cycles_completed\":%" PRIu32
        ",\"startup_exchanges\":%" PRIu64
        ",\"startup_phase_error_ns\":%" PRId64
        ",\"last_dc_time_ns\":%" PRId64 "},"
        "\"motion\":",
        report->dc_required ? "true" : "false",
        report->dc_configured ? "true" : "false",
        report->dc_startup_stable ? "true" : "false",
        (unsigned int)report->dc_reference_slave,
        report->process_data_phase_ns,
        report->dc_startup_cycles_requested,
        report->dc_startup_cycles_completed,
        report->dc_startup_exchanges,
        report->dc_startup_phase_error_ns,
        report->last_dc_time_ns) >= 0);
    REQUIRE_WRITE(write_motion(stream, plan->motion_profile));
    REQUIRE_WRITE(fputs(",\"axes\":[", stream) != EOF);
    for (index = 0U; index < plan->axis_count; ++index)
    {
        REQUIRE_WRITE(fputs(index == 0U ? "" : ",", stream) != EOF);
        REQUIRE_WRITE(write_axis(stream, &plan->axes[index], &report->axes[index]));
    }
    REQUIRE_WRITE(fputs("],\"accesses\":[", stream) != EOF);
    for (index = 0U; index < report->audit.access_count; ++index)
    {
        REQUIRE_WRITE(write_access(stream, plan, &report->audit.accesses[index],
                                   index == 0U));
    }
    return fputs("]}\n", stream) != EOF;
}
