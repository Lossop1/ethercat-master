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
        "\"sync0_late_count\":%" PRIu64 "}",
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
        timing->sync0_late_count) >= 0);
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
    REQUIRE_WRITE(fprintf(stream,
        ",\"shutdown_al\":{\"state\":%u,\"status_code\":%u}",
        (unsigned int)axis->shutdown_al_state, (unsigned int)axis->shutdown_al_status_code) >= 0);
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
        "\"initial_actual_position\":%" PRId32
        ",\"actual_position\":%" PRId32 ",\"target_position\":%" PRId32
        ",\"operation_enabled_seen\":%s,\"motion_final_position\":%" PRId32
        ",\"max_following_error_counts\":%" PRIu64
        ",\"max_observed_following_error_counts\":%" PRIu64
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
        axis->initial_actual_position, axis->actual_position,
        axis->target_position,
        axis->operation_enabled_seen ? "true" : "false",
        axis->motion_final_position, axis->max_following_error_counts,
        axis->max_observed_following_error_counts,
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
            ",\"max_following_error_millidegrees\":%" PRIu32
            ",\"expected_position_scale\":{\"encoder_increments\":%" PRIu32
            ",\"encoder_motor_revolutions\":%" PRIu32
            ",\"gear_motor_revolutions\":%" PRIu32
            ",\"gear_shaft_revolutions\":%" PRIu32 "}}",
            axis->relative_angle_millidegrees,
            axis->max_following_error_millidegrees,
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
    REQUIRE_WRITE(fputs("{\"schema_version\":3,\"generated_at_utc\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, generated_at_utc));
    REQUIRE_WRITE(fputs(",\"deployment\":{\"id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, plan->deployment->deployment_id));
    REQUIRE_WRITE(fputs(",\"hostname\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, plan->deployment->hostname));
    REQUIRE_WRITE(fputs(",\"ethercat_interface\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, plan->deployment->ethercat_interface));
    REQUIRE_WRITE(fputs(",\"topology_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, plan->deployment->topology->topology_id));
    REQUIRE_WRITE(fprintf(
        stream,
        "},\"result\":{\"status_code\":%u,\"control_state\":%u,\"io_map_size\":%zu,"
        "\"expected_wkc\":%u,\"actual_wkc\":%d,\"cycle_count\":%" PRIu64
        ",\"process_data_exchange_count\":%" PRIu64
        ",\"cycle_deadline_missed\":%s,\"fault_latched\":%s,"
        "\"safety_control_permitted\":%s,\"safety_blocking_reasons\":%" PRIu32
        ",\"safe_op_reached\":%s,\"op_reached\":%s,"
        "\"all_axes_enabled_reached\":%s,\"motion_started\":%s,"
        "\"motion_completed\":%s,\"safe_output_sent\":%s,"
        "\"safe_state_reached\":%s,\"sync0_disabled\":%s,"
        "\"restore_init_succeeded\":%s},",
        (unsigned int)report->status, (unsigned int)report->state, report->io_map_size,
        (unsigned int)report->expected_wkc, report->actual_wkc,
        report->cycle_count, report->process_data_exchange_count,
        report->cycle_deadline_missed ? "true" : "false",
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
        report->restore_init_succeeded ? "true" : "false") >= 0);
    REQUIRE_WRITE(fputs("\"first_cycle_failure\":", stream) != EOF);
    REQUIRE_WRITE(write_cycle_failure(stream, &report->first_cycle_failure));
    REQUIRE_WRITE(fprintf(stream,
        ",\"audit\":{\"omitted_pdo_samples\":%" PRIu64 "},"
        "\"diagnostic_preop_reached\":%s,",
        report->audit.omitted_pdo_samples,
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
