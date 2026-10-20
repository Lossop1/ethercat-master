#include "report_sections.h"

#include "json_writer.h"

/* 解码表。审计层负责"写出来"，不负责"怎么解"——那两件事都在 diagnostics 叶子里。 */
#include "emaster/diagnostics/al_status.h"
#include "emaster/diagnostics/cia402_error.h"
#include "emaster/diagnostics/error_class.h"
/* 设点通道的拒收原因文本。与主站往 stderr 打的是同一句话（同一实现）。 */
#include "emaster/setpoint/wire.h"

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

/* 停机判据的稳定 ASCII 键：给分析脚本读，不随重构改名（与 error_class 同一套约定）。 */
static const char *shutdown_criterion_key(emaster_shutdown_stop_criterion_t criterion)
{
    switch (criterion)
    {
    case EMASTER_SHUTDOWN_CRITERION_DEVICE_MASK:
        return "device_mask";
    case EMASTER_SHUTDOWN_CRITERION_STANDARD_CIA402:
        return "standard_cia402";
    case EMASTER_SHUTDOWN_CRITERION_NONE:
        break;
    }
    return "none";
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

/*
 * 设备实际映射与设备档声明不同的地方，逐条写出。
 *
 * 主站按设备实际建的映像，上面那份声明只说明"我们以为设备长什么样"。差异不写出来，
 * 报告里就只剩一个 verified=true，没人知道映像的字节从哪张表来的。
 */
static bool write_device_mapping_differences(
    FILE *stream,
    const emaster_control_session_axis_result_t *axis)
{
    size_t listed = axis->pdo_device_mapping_difference_count;
    size_t ordinal;

    if (listed > EMASTER_CONTROL_SESSION_PDO_DIFFERENCE_SLOTS)
    {
        listed = EMASTER_CONTROL_SESSION_PDO_DIFFERENCE_SLOTS;
    }
    REQUIRE_WRITE(fputs(",\"device_mapping_differences\":[", stream) != EOF);
    for (ordinal = 0U; ordinal < listed; ++ordinal)
    {
        const emaster_pdo_layout_difference_t *difference =
            &axis->pdo_device_mapping_differences[ordinal];

        REQUIRE_WRITE(fprintf(
            stream,
            "%s{\"mapping\":\"0x%04X\",\"subindex\":%u,\"declared\":\"0x%08lX\","
            "\"device\":\"0x%08lX\"}",
            ordinal == 0U ? "" : ",", (unsigned int)difference->mapping_index,
            (unsigned int)difference->subindex,
            (unsigned long)difference->declared_descriptor,
            (unsigned long)difference->device_descriptor) >= 0);
    }
    return fputs("]", stream) != EOF;
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

/* 事实来源写成报告里的字面值。三态各有各的话：设备回答的、设备档声明的、没有的。 */
static const char *fact_source_name(emaster_fact_source_t source)
{
    switch (source)
    {
    case EMASTER_FACT_SOURCE_DEVICE:
        return "device";
    case EMASTER_FACT_SOURCE_CONFIGURED:
        return "configured";
    case EMASTER_FACT_SOURCE_NONE:
    default:
        return "none";
    }
}

static bool write_position_scale(FILE *stream,
                                 const emaster_control_session_axis_result_t *axis)
{
    const emaster_position_scale_t *scale = &axis->position_scale;
    const emaster_position_scale_t *device = &axis->device_position_scale;
    bool device_read = device->source == EMASTER_FACT_SOURCE_DEVICE;
    bool agrees = device_read && device->encoder_increments == scale->encoder_increments &&
                  device->encoder_motor_revolutions == scale->encoder_motor_revolutions &&
                  device->gear_motor_revolutions == scale->gear_motor_revolutions &&
                  device->gear_shaft_revolutions == scale->gear_shaft_revolutions;

    /*
     * 生效值与从站自报值并排写出来，两个都要。
     *
     * source 是生效值的来路；read_succeeded 只回答"设备用 608F/6091 答了没有"——两者不是
     * 同义词，声明生效时设备照样可能答过。设备自报的四条数放在 device 里**原样保留**：
     * 驱动器会把减速比报错（台架从站 1 自报 6091=28000，实测真值 28），此时轴按设备档声明
     * 动，但"设备报了什么"仍是要给人看的事实 —— 报告里只有生效值，看报告的人就分不出
     * "设备在说谎"和"人把档写错了"，而这两件事的处置完全不同。
     *
     * device_agrees 的 null 是"设备没答，无从比对"，与 false（答了、但和生效值不一样）
     * 必须分得开：把没答也写成 false，读起来就成了"设备答了且不一致"。
     */
    if (fprintf(stream,
                "{\"source\":\"%s\",\"read_succeeded\":%s,"
                "\"encoder_increments\":%" PRIu32 ",\"encoder_motor_revolutions\":%" PRIu32
                ",\"gear_motor_revolutions\":%" PRIu32 ",\"gear_shaft_revolutions\":%" PRIu32
                ",\"device\":{\"read_succeeded\":%s,\"encoder_increments\":%" PRIu32
                ",\"encoder_motor_revolutions\":%" PRIu32
                ",\"gear_motor_revolutions\":%" PRIu32
                ",\"gear_shaft_revolutions\":%" PRIu32 "},\"device_agrees\":",
                fact_source_name(scale->source), device_read ? "true" : "false",
                scale->encoder_increments, scale->encoder_motor_revolutions,
                scale->gear_motor_revolutions, scale->gear_shaft_revolutions,
                device_read ? "true" : "false", device->encoder_increments,
                device->encoder_motor_revolutions, device->gear_motor_revolutions,
                device->gear_shaft_revolutions) < 0) {
        return false;
    }
    if (!device_read) {
        return fputs("null}", stream) != EOF;
    }
    return fprintf(stream, "%s}", agrees ? "true" : "false") >= 0;
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
         * P8.3: 连续迟到。驱动器攒满 6 次 SM 事件丢失就置 AL 0x001A 掉出 OP，
         * 而 sync0_late_count 只是全程累计——8 臂里到过 84 次仍一轮都没掉出，
         * 说明那些迟到多半是散的。max_consecutive 才是能跟那个 6 对齐的量。
         */
        "\"sync0_late_max_consecutive\":%" PRIu64 ","
        "\"max_sync0_late_run_exchange\":%" PRIu64 ","
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
        timing->last_sync0_late_exchange,
        timing->sync0_late_max_consecutive, timing->max_sync0_late_run_exchange,
        timing->max_round_trip_exchange,
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

/*
 * 逐轴解码文本。origin 为 NONE（这个坐标压根没读回来）时印"未读到"，**不许**印
 * describe(0) 给出的"无错误/无故障"——"没读到"与"驱动器报 0"在数字上是同一个 0，
 * 意思却相反。报告里把这两者印成同一句话，就等于把本次专门拆开的两种情形又合
 * 回去，而它们指向完全不同的排查方向（一个是通信没回来，一个是驱动器真的没事）。
 */
static const char *axis_al_status_text(const emaster_control_session_axis_result_t *axis,
                                       char *buffer, size_t capacity)
{
    if (axis->al_status_code_origin == EMASTER_AL_ORIGIN_NONE)
    {
        return "未读到";
    }
    (void)emaster_al_status_describe(axis->al_status_code_decoded, buffer, capacity);
    return buffer;
}

static const char *axis_drive_error_text(const emaster_control_session_axis_result_t *axis,
                                         char *buffer, size_t capacity)
{
    if (axis->drive_error_code_origin == EMASTER_DRIVE_ORIGIN_NONE)
    {
        return "未读到";
    }
    (void)emaster_cia402_error_describe(axis->drive_error_code_decoded, buffer, capacity);
    return buffer;
}

static bool write_axis(FILE *stream,
                       const emaster_session_axis_plan_t *axis_plan,
                       const emaster_control_session_axis_result_t *axis)
{
    const emaster_operation_profile_t *operation = axis_plan->operation_profile;
    const emaster_pdo_set_profile_t *pdo = axis_plan->pdo_set;
    char expected_serial[16];
    char observed_serial[16];
    const char *serial_match;

    /*
     * 序列号三态：没登记（null）、登记了但没读到（observed=null）、读到了
     * （两边都是十六进制串）。match 只有登记过才有 true/false。
     */
    if (axis_plan->topology_slave->serial_number != 0U) {
        (void)snprintf(expected_serial, sizeof(expected_serial), "\"0x%08" PRIX32 "\"",
                       axis_plan->topology_slave->serial_number);
    } else {
        (void)snprintf(expected_serial, sizeof(expected_serial), "null");
    }
    if (axis->serial_number_read) {
        (void)snprintf(observed_serial, sizeof(observed_serial), "\"0x%08" PRIX32 "\"",
                       axis->actual_serial_number);
    } else {
        (void)snprintf(observed_serial, sizeof(observed_serial), "null");
    }
    serial_match = axis->serial_number_checked
                       ? (axis->serial_number_match ? "true" : "false")
                       : "null";

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
        "\",\"revision\":\"0x%08" PRIX32 "\",\"serial_number\":%s},\"observed\":{"
        "\"vendor_id\":\"0x%08" PRIX32 "\",\"product_code\":\"0x%08" PRIX32
        "\",\"revision\":\"0x%08" PRIX32 "\",\"serial_number\":%s},"
        "\"match\":%s,\"serial_match\":%s},",
        axis_plan->device_profile->identity.vendor_id,
        axis_plan->device_profile->identity.product_code,
        axis_plan->device_profile->identity.revision,
        expected_serial,
        axis->actual_vendor_id, axis->actual_product_code, axis->actual_revision,
        observed_serial,
        axis->identity_match ? "true" : "false", serial_match) >= 0);
    REQUIRE_WRITE(fputs("\"operation\":{\"profile_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, operation->profile_id));
    REQUIRE_WRITE(fputs(",\"mode_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, operation->selected_mode_id));
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"mode_value\":%d,\"sync_strategy\":\"%s\","
        "\"sm2_sync_type\":\"0x%04X\",\"sm3_sync_type\":\"0x%04X\","
        "\"sm_sync_type_substituted\":%s,"
        "\"sm2_sync_type_used\":\"0x%04X\",\"sm3_sync_type_used\":\"0x%04X\"},",
        (int)axis_plan->operation_mode->value,
        operation->sync_strategy == EMASTER_SYNC_STRATEGY_DC ? "dc" : "sm",
        (unsigned int)operation->sm2_sync_type,
        (unsigned int)operation->sm3_sync_type,
        axis->sm_sync_type_substituted ? "true" : "false",
        (unsigned int)axis->sm2_sync_type_used,
        (unsigned int)axis->sm3_sync_type_used) >= 0);
    REQUIRE_WRITE(fputs("\"pdo\":{\"set_id\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, pdo->pdo_set_id));
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"verified\":%s,\"process_image_verified\":%s,"
        "\"adopted_device_layout\":%s,\"device_mapping_difference_count\":%" PRIu32 ","
        "\"output_bytes\":%" PRIu32 ",\"input_bytes\":%" PRIu32 ","
        "\"output_bits\":%" PRIu32 ",\"input_bits\":%" PRIu32 ","
        "\"output_offset_bytes\":%zu,\"input_offset_bytes\":%zu,\"rx\":",
        axis->pdo_match ? "true" : "false",
        axis->process_map_match ? "true" : "false",
        axis->pdo_device_mapping_adopted ? "true" : "false",
        axis->pdo_device_mapping_difference_count,
        axis->output_bytes, axis->input_bytes,
        axis->output_bits, axis->input_bits, axis->output_offset_bytes,
        axis->input_offset_bytes) >= 0);
    REQUIRE_WRITE(write_mapping_direction(stream, pdo->rx_mappings,
                                          pdo->rx_mapping_count));
    REQUIRE_WRITE(fputs(",\"tx\":", stream) != EOF);
    REQUIRE_WRITE(write_mapping_direction(stream, pdo->tx_mappings,
                                          pdo->tx_mapping_count));
    REQUIRE_WRITE(write_device_mapping_differences(stream, axis));
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
    REQUIRE_WRITE(write_position_scale(stream, axis));
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
     * 第四个坐标：已经请求过"全体回 INIT"之后、关总线之前的那一次逐站读数。上面三个
     * 都在"叫它们回 INIT 之前"，所以只有这一条能回答"主站退出后驱动器在哪一态"。
     */
    REQUIRE_WRITE(fprintf(stream,
        ",\"shutdown_al_exit\":{\"state\":%u,\"status_code\":%u}",
        (unsigned int)axis->shutdown_exit_al_state,
        (unsigned int)axis->shutdown_exit_al_status_code) >= 0);
    /*
     * CiA 402 层：主站退出时驱动器自己报的状态字与故障码。
     *
     * 与上面的 shutdown_al_exit 并排放，因为**只有并排才能说明问题**：AL 状态是 ESC
     * 那一层的，退出时必然回到 INIT；而驱动器层（0x6041）不随它复位，可以仍然是
     * Operation Enabled 且功率级带电。报告此前只有前者，于是"主站停了"被读成
     * "电机没劲了"——2026-10-08 实测两者同时成立（AL=1 而 0x6041=0x1237）。
     *
     * read=false 表示没读到，与读到的 0 分得开：0x0000 在状态字里是有含义的值。
     */
    /*
     * 主站接手时的驱动器状态字，以及那一刻发出的禁用。与上面 shutdown_cia402 配成一对：
     * 一个回答"我来的时候它在不在使劲"，一个回答"我走之后它还在不在使劲"。
     * 两者都是 ESC 那一层读不到的 —— AL 状态与 CiA 402 状态互不决定。
     */
    REQUIRE_WRITE(fprintf(stream,
        ",\"startup_cia402\":{\"statusword_read\":%s,\"statusword\":%u,"
        "\"disable_written\":%s,\"statusword_after_read\":%s,\"statusword_after\":%u,"
        "\"operation_enabled\":%s,\"voltage_enabled\":%s,\"still_enabled\":%s}",
        axis->startup_statusword_read ? "true" : "false",
        (unsigned int)axis->startup_statusword,
        axis->startup_disable_written ? "true" : "false",
        axis->startup_statusword_after_read ? "true" : "false",
        (unsigned int)axis->startup_statusword_after,
        /* 顺序必须与格式串一致：operation_enabled、voltage_enabled、still_enabled。
         * 2026-10-08 我把 still_enabled 插到了 operation_enabled 之前，于是三个字段整体
         * 错位一格 —— 报告里出现"statusword_after=0x1250（未使能）而 still_enabled=true"
         * 这种自相矛盾的读数。**格式串与实参的对应关系没有任何东西替你检查，只能数。** */
        (axis->startup_statusword_read &&
         (axis->startup_statusword & 0x0004U) != 0U) ? "true" : "false",
        (axis->startup_statusword_read &&
         (axis->startup_statusword & 0x0010U) != 0U) ? "true" : "false",
        /* still_enabled：写后读回仍显示 Operation enabled —— 这才是"没禁掉"的证据。 */
        (axis->startup_statusword_after_read &&
         (axis->startup_statusword_after & 0x0004U) != 0U) ? "true" : "false") >= 0);
    /*
     * 字段名里的 before/after 指的是**写"禁用电压"（0x6040=0x0000）之前与之后**。
     *
     * 这一对是本次安全修复的证据位：before 若是 Operation enabled，说明主站退出时
     * 驱动器还使能着 —— 而 2026-10-08 之前那条禁用只走 PDO，从站掉出 OP 就到不了它，
     * 于是"主站报退出 OP、电机仍在保持"这件事在报告里几个月都无法证实。disable_written
     * 则说明这一次的权威禁用（邮箱 SDO）到底写进去没有。
     */
    REQUIRE_WRITE(fprintf(stream,
        ",\"shutdown_cia402\":{\"disable_written\":%s,"
        "\"statusword_before_read\":%s,\"statusword_before\":%u,"
        "\"statusword_read\":%s,\"statusword\":%u,"
        "\"error_code_read\":%s,\"error_code\":%u,"
        "\"operation_enabled_before\":%s,\"operation_enabled\":%s,"
        "\"voltage_enabled\":%s,\"fault\":%s}",
        axis->shutdown_disable_written ? "true" : "false",
        axis->shutdown_statusword_before_read ? "true" : "false",
        (unsigned int)axis->shutdown_statusword_before,
        axis->shutdown_statusword_read ? "true" : "false",
        (unsigned int)axis->shutdown_statusword,
        axis->shutdown_error_code_read ? "true" : "false",
        (unsigned int)axis->shutdown_error_code,
        (axis->shutdown_statusword_before_read &&
         (axis->shutdown_statusword_before & 0x0004U) != 0U) ? "true" : "false",
        (axis->shutdown_statusword_read &&
         (axis->shutdown_statusword & 0x0004U) != 0U) ? "true" : "false",
        (axis->shutdown_statusword_read &&
         (axis->shutdown_statusword & 0x0010U) != 0U) ? "true" : "false",
        (axis->shutdown_statusword_read &&
         (axis->shutdown_statusword & 0x0008U) != 0U) ? "true" : "false") >= 0);
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
    /*
     * 与上面同一时刻、同一批读到的 1C32/1C33 计数器：驱动器自己数到的同步违例。
     * 它们与 sm2_diagnostic/sm3_diagnostic 的区别只在坐标（这里是首次不符当下，
     * 那两处是停机之后），所以用同一个写函数，读者按字段名区分来源。
     */
    REQUIRE_WRITE(fputs(",\"first_mismatch_sm2\":", stream) != EOF);
    REQUIRE_WRITE(write_sync_diagnostic(stream, &axis->first_mismatch_sm2));
    REQUIRE_WRITE(fputs(",\"first_mismatch_sm3\":", stream) != EOF);
    REQUIRE_WRITE(write_sync_diagnostic(stream, &axis->first_mismatch_sm3));
    /*
     * 解码结论：本轴到底报的是哪个 AL 码、哪个 0x603F，以及是从哪个坐标选来的。
     *
     * 为什么还要 origin：AL 码在报告里有好几个坐标（首次不符、停机进/出、停机前言），
     * 各自读到的码可以不同，而"选了哪一个"此前只存在于取值的那一行代码里——报告
     * 记不住，事后就没法复核判据。origin 就是那个判据的落点。
     *
     * origin 取值（见 emaster_al_origin_t / emaster_drive_error_origin_t）：
     *   AL：0 无 / 1 first_mismatch / 2 shutdown_entry / 3 shutdown_pre_stop
     *       / 4 shutdown_exit / 5 shutdown
     *   驱动：0 无 / 1 runtime / 2 shutdown / 3 safeop
     *
     * 这几个键是**平铺**的，不是嵌在一个 "decode" 对象里：报告里凡嵌 {} 的地方，
     * extract_report.py 一类靠 [^}]* 的抽取器都会提前截断（见上文过程映像那段）。
     *
     * 文本在这里现解，报告里不留中间字符串：码表只有一个调用者，多存一份就可能
     * 与码表漂开。codes 是 uint16，但 JSON 里当数字写，与既有 error_code 一致。
     */
    {
        char al_text[128];
        char drive_text[128];
        REQUIRE_WRITE(fprintf(
            stream,
            ",\"al_status_code_decoded\":%u,\"al_status_code_origin\":%u,\"al_status_code_text\":",
            (unsigned int)axis->al_status_code_decoded,
            (unsigned int)axis->al_status_code_origin) >= 0);
        REQUIRE_WRITE(emaster_json_string(
            stream, axis_al_status_text(axis, al_text, sizeof(al_text))));
        REQUIRE_WRITE(fprintf(
            stream,
            ",\"drive_error_code_decoded\":%u,\"drive_error_code_origin\":%u,"
            "\"drive_error_code_text\":",
            (unsigned int)axis->drive_error_code_decoded,
            (unsigned int)axis->drive_error_code_origin) >= 0);
        REQUIRE_WRITE(emaster_json_string(
            stream, axis_drive_error_text(axis, drive_text, sizeof(drive_text))));
    }
    /*
     * P12.17: 这一轴被判故障的坐标。recorded 为假时后四个字段无意义（不是 0 号
     * 交换），所以显式给出记录标志，不靠读者去猜 0 的含义。
     *
     * exchange 装的是 session->exchange。它与 result.cycle_count 在正常会话里相等，
     * 失败路径上会分叉，所以键名不叫 cycle。
     */
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"fault\":{\"recorded\":%s,\"status\":%u,\"isolated\":%s,\"reason\":%u,"
        "\"exchange\":%" PRIu64 ",\"status_word\":\"0x%04X\"}",
        axis->fault_recorded ? "true" : "false", (unsigned int)axis->axis_status,
        axis->fault_isolated ? "true" : "false", (unsigned int)axis->fault_reason,
        axis->fault_cycle, (unsigned int)axis->fault_status_word) >= 0);
    /*
     * P12.17: 每轴各自的"首次越界"坐标。
     *
     * 这是"哪根轴在第几拍开始不对"的唯一来源。报告里另外两处都答不了：现场环
     * （cycle_trace）是每会话一条记录、只留最后 64 拍；first_runtime_failure 只留
     * 全局第一次，且不说是哪一轴的第几个信号。四项都自带 seen，因为"没出现过"与
     * "出现在第 0 拍"必须分得开。
     */
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"fault_timeline\":{"
        "\"enable_lost\":{\"seen\":%s,\"exchange\":%" PRIu64 ",\"status_word\":\"0x%04X\"},"
        "\"fault_bit\":{\"seen\":%s,\"exchange\":%" PRIu64 ",\"status_word\":\"0x%04X\"},"
        "\"warning\":{\"seen\":%s,\"exchange\":%" PRIu64 ",\"status_word\":\"0x%04X\"},"
        "\"internal_limit\":{\"seen\":%s,\"exchange\":%" PRIu64 ",\"status_word\":\"0x%04X\"}}",
        axis->enable_lost_seen ? "true" : "false", axis->enable_lost_exchange,
        (unsigned int)axis->enable_lost_status_word,
        axis->fault_bit_seen ? "true" : "false", axis->fault_bit_exchange,
        (unsigned int)axis->fault_bit_status_word,
        axis->warning_bit_seen ? "true" : "false", axis->warning_bit_exchange,
        (unsigned int)axis->warning_bit_status_word,
        axis->internal_limit_seen ? "true" : "false", axis->internal_limit_exchange,
        (unsigned int)axis->internal_limit_status_word) >= 0);
    /*
     * P12.17: 停机那一刻读回的驱动器诊断。
     *
     * 这块数据一直存在于会话结果里（drive_diagnostic），只是从没写进报告——报告里
     * 只有 safeop_drive_diagnostic（SAFE-OP 时那一次）。键名与形状照抄它，好让分析
     * 脚本共用一套正则。
     */
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"final_drive_diagnostic\":{\"read_succeeded\":%s,\"cia402_error_code\":\"0x%04X\","
        "\"error_register\":\"0x%02X\",\"extended_servo_error_code\":\"0x%08lX\","
        "\"servo_error_code\":\"0x%08lX\"}",
        axis->drive_diagnostic.read_succeeded ? "true" : "false",
        (unsigned int)axis->drive_diagnostic.cia402_error_code,
        (unsigned int)axis->drive_diagnostic.error_register,
        (unsigned long)axis->drive_diagnostic.extended_servo_error_code,
        (unsigned long)axis->drive_diagnostic.servo_error_code) >= 0);
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
        /*
         * 驱动器自己的跟随误差窗与超时（6065 / 6066，设置期读回）。主站不做这道判据，
         * 所以这两个读数就是"这条保护到底在不在"的全部依据；read 与值必须一起看，
         * 只看值会把它当成"驱动器报 0 就是不保护"。
         */
        "\"following_error_window_read\":%s,\"following_error_window\":%" PRIu32 ","
        "\"following_error_timeout_read\":%s,\"following_error_timeout\":%u,"
        /* 外部目标偏移上限（counts）与它到底有没有被判。两个必须一起读：
         * 只看 limit=0 会以为"不设防"，其实可能是"根本没判"（换算用不了）。
         * 逐轴折算，所以 25:1 与 28:1 的轴在这里是不同的数。
         * 没判的轴一律写 0：生产路径本来就不会给它留值，写 0 是为了让
         * "checked=false ⇒ counts=0"在报告里恒成立——读的人不必再想那个数是从哪来的。 */
        "\"offset_limit_counts\":%" PRIu64 ","
        "\"offset_limit_checked\":%s,"
        "\"polarity_source\":\"%s\",\"polarity_read\":%s,\"polarity\":%u,"
        "\"safeop_actual_position\":%" PRId32
        ",\"initial_actual_position\":%" PRId32
        ",\"actual_position\":%" PRId32 ",\"target_position\":%" PRId32
        ",\"actual_velocity\":%" PRId32 ",\"target_velocity\":%" PRId32
        ",\"actual_torque\":%d,\"target_torque\":%d"
        ",\"actual_current\":%d,\"dc_link_voltage\":%" PRIu32
        ",\"mosfet_temperature\":%d,\"motor_temperature\":%d"
        /* 这几个慢速量读没读到、多久以前读到的：没有它，下面的 0 与"没读到"长得一样。 */
        ",\"slow_telemetry_read\":%s,\"slow_telemetry_age_ns\":%" PRIu64
        /*
         * 四列各自的可用性。轴级标志只说明"这一轴至少有一条读到过"；设备没声明
         * 0x6078（MT）或这一条这一轮超时时，值仍是 0，渲染端必须看这里。
         */
        ",\"actual_current_read\":%s,\"dc_link_voltage_read\":%s"
        ",\"mosfet_temperature_read\":%s,\"motor_temperature_read\":%s"
        /*
         * P12.17: 运行期的 603F，与上面几个慢速量同源同约定。单独一套 read/age，
         * 因为它是字段级的：温度读到而错误码这轮超时是常事，共用轴级标志会把
         * "从没读到"印成 0x0000。运行期报过错、停机时 603F 又被清掉的情况下，
         * 这一列是唯一的痕迹。
         */
        ",\"runtime_error_code_read\":%s,\"runtime_error_code\":\"0x%04X\""
        ",\"runtime_error_code_age_ns\":%" PRIu64
        /*
         * P12.41: 驱动器自己报的目标位置（0x607A 慢速读回），与上面那个 target_position
         * 不是一回事：那个是主站写进过程映像的值，这个是驱动器收下并用起来的那个。
         * "有一根轴纹丝不动而主站明明扫过了全行程"靠这两个的对比才切得开。同样的
         * read/age 约定，且是字段级——读失败留下的旧值本身就是"目标没送到"的形状，
         * 没有年龄就分不开"读失败"与"驱动器一直保持着旧值"。
         *
         * 三列平铺，不嵌成对象：报告里凡是嵌一层 {} 的地方，按 "runtime":{...} 抠块的
         * 现成工具（extract_report.py 等用 [^}]* 的）都会被内层那个 } 提前截断，把后面
         * 的字段整段吃掉——603F 那三列当初就是这么平铺的，照它办。
         */
        ",\"target_position_readback_read\":%s"
        ",\"target_position_readback\":%" PRId32
        ",\"target_position_readback_age_ns\":%" PRIu64
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
        /* 0x0310 起每端口一个字节：断链计数。first 是会话第一次读到的基准，
         * 两者相减才是本轮；这个计数器上电以来一直累加。 */
        ",\"lost_link_counter\":[%u,%u,%u,%u]"
        ",\"lost_link_counter_first\":[%u,%u,%u,%u]"
        ",\"lost_link_counter_first_valid\":%s"
        ",\"ecat_processing_unit_error_counter\":%u"
        ",\"pdi_error_counter\":%u,\"pdi_error_code\":%u"
        ",\"esc_registers_read\":%s"
        ",\"esc_dl_control\":%u,\"esc_watchdog_pdi\":%u"
        ",\"final_diagnostic_reads\":%zu,"
        "\"final_diagnostic_successes\":%zu},"
        /*
         * 回零的进度。与 runtime 平级（它不属于过程数据回读），也不嵌成对象：
         * 嵌一层会让按 [^}]* 抠块的现成工具在内层 } 处截断。
         * homing_configured 为假时其余五项无意义。
         */
        "\"homing_configured\":%s,\"homing_start_exchange\":%" PRIu64
        ",\"homing_attained\":%s,\"homing_attained_exchange\":%" PRIu64
        ",\"homing_error\":%s,\"homing_timeout_ms\":%" PRIu32 "}",
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
        axis->following_error_window_read ? "true" : "false",
        axis->following_error_window,
        axis->following_error_timeout_read ? "true" : "false",
        (unsigned int)axis->following_error_timeout,
        axis->offset_limit_checked ? axis->offset_limit_counts : UINT64_C(0),
        axis->offset_limit_checked ? "true" : "false",
        fact_source_name(axis->polarity_source),
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
        axis->slow_telemetry_read ? "true" : "false",
        axis->slow_telemetry_age_ns,
        axis->actual_current_read ? "true" : "false",
        axis->dc_link_voltage_read ? "true" : "false",
        axis->mosfet_temperature_read ? "true" : "false",
        axis->motor_temperature_read ? "true" : "false",
        axis->runtime_error_code_read ? "true" : "false",
        (unsigned int)axis->runtime_error_code,
        axis->runtime_error_code_age_ns,
        axis->target_position_readback_read ? "true" : "false",
        axis->target_position_readback,
        axis->target_position_readback_age_ns,
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
        (unsigned int)axis->lost_link_counter[0],
        (unsigned int)axis->lost_link_counter[1],
        (unsigned int)axis->lost_link_counter[2],
        (unsigned int)axis->lost_link_counter[3],
        (unsigned int)axis->lost_link_counter_first[0],
        (unsigned int)axis->lost_link_counter_first[1],
        (unsigned int)axis->lost_link_counter_first[2],
        (unsigned int)axis->lost_link_counter_first[3],
        axis->lost_link_counter_first_valid ? "true" : "false",
        (unsigned int)axis->ecat_processing_unit_error_counter,
        (unsigned int)axis->pdi_error_counter,
        (unsigned int)axis->pdi_error_code,
        axis->esc_registers_read ? "true" : "false",
        (unsigned int)axis->esc_dl_control,
        (unsigned int)axis->esc_watchdog_pdi,
        axis->final_diagnostic_read_count,
        axis->final_diagnostic_success_count,
        axis->homing_configured ? "true" : "false",
        (uint64_t)axis->homing_start_exchange,
        axis->homing_attained ? "true" : "false",
        (uint64_t)axis->homing_attained_exchange,
        axis->homing_error ? "true" : "false",
        axis->homing_timeout_ms) >= 0);
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
        ",\"coordinator_status\":%d,\"bus_slave_count\":%d,\"axis_count\":%zu,\"axes\":[",
        (unsigned int)failure->status, failure->cycle_count, failure->exchange,
        failure->coordinator_status, failure->bus_slave_count, axis_count) >= 0);
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
 * P9.2: SOEM 邮箱错误环的消费记录。两条计数合起来才完整：events 是本报告存下的，
 * dropped 是取到了但报告环已满、只留计数的，相加应当等于 count。
 *
 * etype 原样写数字而不是字符串：SOEM 的 ec_err_type 是本仓外部依赖的枚举，翻成
 * 中文字符串就等于把外部头文件的取值固化进报告格式，那头一改这里就悄悄说谎。
 * 报告只给原始值，含义由 control_session.h 的注释和 SOEM 头文件负责。
 */
static bool write_soem_errors(FILE *stream, const emaster_control_session_report_t *report)
{
    REQUIRE_WRITE(fprintf(stream, "{\"count\":%" PRIu64 ",\"dropped_count\":%" PRIu64
                                  ",\"capacity\":%u,\"events\":[",
                          report->soem_error_count, report->soem_error_dropped_count,
                          (unsigned)EMASTER_SOEM_ERROR_CAPACITY) >= 0);
    for (size_t index = 0U; index < report->soem_error_event_count; ++index)
    {
        const emaster_soem_error_event_t *event = &report->soem_errors[index];

        REQUIRE_WRITE(fprintf(stream,
            "%s{\"exchange\":%" PRIu64 ",\"time_unix_ns\":%" PRIu64
            ",\"slave\":%u,\"index\":%u,\"subindex\":%u,\"etype\":%u"
            ",\"abort_code\":%d,\"abort_code_valid\":%s,\"error_code\":%u}",
            index == 0U ? "" : ",", event->exchange, event->time_unix_ns, (unsigned)event->slave,
            (unsigned)event->index, (unsigned)event->subindex, (unsigned)event->etype,
            (int)event->abort_code, event->abort_code_valid ? "true" : "false",
            (unsigned)event->error_code) >= 0);
    }
    return fputs("]}", stream) != EOF;
}

/*
 * 周期现场环的两份记录：最近 64 个周期（环形，需按时间顺序展开），
 * 以及第一个 WKC 不符之前冻结的那 64 个周期（冻结时已展开）。
 */
/*
 * 周期时序越界的分档计数与最长连续段。与极值互补：极值回答"最坏有多坏"，
 * 这里回答"有多频繁、连不连续"。两者缺一，就会把偶发当成恶化、或把持续轻微
 * 越界当成没问题。
 *
 * **分档边界一并印出**。把边界藏在代码里，这份分布就只对写它的人有意义；
 * 印出来之后任何读报告的人都能解释每一档，也能发现档位被改过。
 */
static bool write_timing_excursions(FILE *stream,
                                    const emaster_timing_excursions_t *excursions)
{
    REQUIRE_WRITE(fprintf(stream,
        ",\"timing_excursions\":{"
        "\"send_late_bounds_ns\":[10000,25000,50000],"
        "\"receive_bounds_ns\":[150000,200000,250000],"
        "\"margin_low_bounds_ns\":[150000,100000,50000],"
        "\"send_late_buckets\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
        "\"receive_buckets\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
        "\"margin_buckets\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
        "\"send_late_max_consecutive\":%" PRIu64 ","
        "\"send_late_max_exchange\":%" PRIu64 ","
        "\"receive_slow_max_consecutive\":%" PRIu64 ","
        "\"receive_slow_max_exchange\":%" PRIu64 "}",
        excursions->send_late_buckets[0], excursions->send_late_buckets[1],
        excursions->send_late_buckets[2], excursions->send_late_buckets[3],
        excursions->receive_buckets[0], excursions->receive_buckets[1],
        excursions->receive_buckets[2], excursions->receive_buckets[3],
        excursions->margin_buckets[0], excursions->margin_buckets[1],
        excursions->margin_buckets[2], excursions->margin_buckets[3],
        excursions->send_late_max_consecutive, excursions->send_late_max_exchange,
        excursions->receive_slow_max_consecutive, excursions->receive_slow_max_exchange) >= 0);
    return true;
}

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
    /*
     * 异常窗口：前 PRE 拍 + 异常那一拍 + 后 POST 拍，按时间顺序。
     *
     * trigger_index 必须印出来。窗口收满时它是 PRE；会话在"之后"那一段走完之前就结束
     * （异常后很快中止，或 POST 还没收齐）时它小于 PRE，而**这两种情况下 samples 的
     * 长度不同、含义也不同**：读的人要靠这个下标才知道哪一拍是异常，否则只能假设
     * "中间那一拍"——那在窗口没满时会指到错误的位置上去。
     */
    REQUIRE_WRITE(fprintf(stream,
        "]},\"mismatch\":{\"present\":%s,\"exchange\":%" PRIu64
        ",\"wkc\":%d,\"count\":%zu,\"trigger_index\":%zu,"
        "\"pre_capacity\":%u,\"post_capacity\":%u,\"samples\":[",
        trace->mismatch_present ? "true" : "false", trace->mismatch_exchange,
        trace->mismatch_wkc, trace->window_count, trace->trigger_index,
        (unsigned int)EMASTER_CYCLE_TRACE_PRE_CAPACITY,
        (unsigned int)EMASTER_CYCLE_TRACE_POST_CAPACITY) >= 0);
    for (size_t index = 0U; index < trace->window_count; ++index)
    {
        REQUIRE_WRITE(fputs(index == 0U ? "" : ",", stream) != EOF);
        REQUIRE_WRITE(write_cycle_trace_sample(stream, &trace->window[index]));
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
    /*
     * 版本 5：deployment 下新增 report_archive_keep；result 下新增停机来源
     * （stop_requested / stop_source）；每轴新增 fault、fault_timeline、
     * final_drive_diagnostic 三块与 runtime 里的运行期 603F。加字段也升版本——
     * 这个仓库里每次改报告形状都升，读者要能靠版本号判断"这份报告里有没有那一列"，
     * 否则同一版本号下两种形状并存，正是此前 v6 游离提交那份报告造成的问题。
     *
     * 注：原先打算把每轴状态字塞进周期现场环（cycle_trace）来回答"哪根轴第几拍开始
     * 不对"，实际没走那条路——环是**每会话一条**、只留最后 64 拍，且入环点在逐轴解码
     * 之前，读到的状态字还是上一拍的。替代方案是每轴一条全会话有效的首次越界记录，
     * 即上面的 fault_timeline。
     *
     * 版本 7：每轴新增 shutdown_al_exit——已经请求过"全体回 INIT"之后、关总线之前的
     * 那一次逐站 AL 读数。前三个停机 AL 坐标（shutdown_al / _entry / _pre_stop）都拍在
     * "叫它们回 INIT 之前"，所以版本 5 的报告答不了"主站退出之后驱动器停在哪一态"；
     * 2026-09-28 用户连着两轮在台架上看到"电机没有退出 OP"，那两份报告既证实不了也
     * 证伪不了，就是因为少了这一列。
     *
     * **为什么跳过 6**：版本 6 已经被占用了——上面说的那批游离提交（957e9cd 等）产出的
     * 报告就是 6，形状是"带 tracking_error 遥测"，当前 main 复现不出来。要是这次也发 6，
     * 读者拿到一份 6 就得先猜
     * 它是哪一种——这正是紧邻上文刚说过要避免的事。所以这里迈过 6 直接发 7。
     *
     * 版本 8：每轴 runtime 下新增 target_position_readback{read,value,age_ns}——
     * 驱动器自己报的目标位置（0x607A 慢速读回）。带上它是因为版本 7 的报告回答不了
     * 一个问了好几轮的问题：2026-09-28 的八轴 30° 往返里，主站把目标扫过了整个 30°
     * （38229 counts），第 8 轴的实际位置一动没动、跟随误差正好等于全行程、电流约
     * 0 mA，而它其余每一项读数（6061=8、增益、限位、DC、SM2/SM3、603F、ESC 计数器）
     * 都与走得好的七根逐字段相同。**"目标到了驱动器但它不执行"与"目标压根没送到
     * 驱动器"在版本 7 的报告里长得一模一样**，因为那个 target_position 是主站写进
     * 过程映像的值，主站永远知道自己写了什么。这一列是切开这两种情形唯一的读数。
     */
    /* 9：新增 shutdown_disable 块——停机时逐台核对"真的失能了吗"的结论。 */
    /*
     * 10：新增分类层。逐轴多出 al_status_code_decoded/origin/text 与
     * drive_error_code_decoded/origin/text（选了哪个码、从哪个坐标选来、它叫什么），
     * 结果级多出 error_class 一组（类别键、中文、两个证据位、两个轴号）。
     *
     * 升版而不是"就这么加"：这一层改变的是**读者怎么解读整份报告**——以前同一个
     * status_code 就是结论，现在它是输入，结论在 error_class 里。发同一个版本号会
     * 让分析脚本以为两份报告可比，而其实一份有分类一份没有。
     *
     * 11：逐轴多出 first_mismatch_sm2/first_mismatch_sm3——首次不符当下读到的
     * 1C32/1C33 计数器。它回答的是"驱动器自己在这个时刻已经数到几次同步违例"，而
     * 已有的 sm2_/sm3_diagnostic 在停机后读，混了退出期事件。有了它，0x001A 才能和
     * 子机制对上；没有它的旧报告里这一层根本不存在，不可比。
     *
     * 12：shutdown_disable 的三个键改成它们真正数的东西（verified → statusword_bit2_clear
     * 等），并新增结果级 shutdown_stop_criterion。改名也升版本：旧报告里叫 verified 的
     * 那一列读起来像"已失能"，两份同版本的报告会被当成可比，而它们那一列的含义不同。
     *
     * 13：逐轴多出 lost_link_counter（ESC 0x0310，每端口一个字节）与它的 first 基准。
     * 旧报告里"链断过"与"收包超时"长得一样，分不开——台架上环尾瞬断一直查不下去就
     * 卡在这。顺带修 forwarded_rx_error_counter：0x0308 起是**四个单字节**，过去按
     * 四个 16 位读，后两个值一直是 EPUECNT/PECNT 拼出来的假数。
     *
     * 14：外部目标的限幅换了一条判据，报告跟着换：
     *   - 删除运动方案逐轴的 max_step_millidegrees，以及部署级的
     *     external_target_torque_limit_per_mille。两个都是"主站替客户端定多大算合理"
     *     的旋钮，都已不再生效，留着就是一份读起来像保护、实际不存在的配置。
     *   - 结果级新增 target_rejected_count / target_rejection_axis /
     *     target_rejection_offset_counts：被拒的目标条数（不是拍数）与最后一次现场。
     *   - 逐轴新增 offset_limit_counts / offset_limit_checked：这一轴的偏移上限
     *     （由它自己的 608F/6091 读回值折算，所以 25:1 与 28:1 不同），以及它到底
     *     有没有被判。
     *   - 逐轴新增 following_error_window_read / following_error_window 与
     *     following_error_timeout_read / following_error_timeout：设置期读回的
     *     6065 / 6066。主站不再自己判跟随误差，这两个读数就是"驱动器在不在守"的
     *     全部依据——旧报告里根本没这一层。
     * 为什么升版而不是"就这么加"：旧报告里"目标被拒"这件事根本不存在——它过去会以
     * MOTION_INVALID 结束整轮，读者看到的是会话失败。同一个版本号下两种含义并存，
     * 正是上面反复说到要避免的事。
     *
     * 14 同一版里还多一组：设点通道（第三个套接字，只写，见 emaster/setpoint/）。
     * 结果级新增 setpoint_enabled / records_accepted / records_rejected /
     * sequence_gaps / last_sequence / last_client_time_ns / last_record_age_ns /
     * last_reject_reason / last_reject_axis。记录格式与拒收原因见
     * docs/protocol/command-protocol-v1.md。
     *
     * 不把客户端送来的 dq / tau / kp / kd 写进报告，是有意的：主站今天不用这四个量
     * （驱动器在 CSP 下自己闭合位置环），印出来只会像"这四个值生效了"。它们解码、
     * 校验、存进缓冲区，等出口口径定了再接——那时报告里加的是"用了什么"，不是
     * "收到了什么"。同一理由，通道的计数记的是"发生了什么"，不是"应该发生什么"：
     * 主站不假设客户端频率，所以没有"应收条数"这种列。
     *
     * 18：运行时故障现场新增 bus_slave_count——故障那一刻环上实扫到的从站数（-1 =
     * 故障发生在扫描之前）。版本 17 里只有配置轴数，于是"环上少两台"与"尾两席身份
     * 对不上"读出来是同一句 TOPOLOGY_MISMATCH：2026-10-14 台架，报告写着轴数=12，
     * 而环上只有 10 台，凭那一份报告断不出来。升版是因为两份同版本报告并排时，一份
     * 有这一列、一份没有，读者会以为两份的拓扑状况可比。
     *
     * 19：逐轴换算段改成"生效值 + 从站自报值"两笔并排。生效值可能来自设备档声明——
     * 驱动器会把减速比报错（台架从站 1 自报 6091=28000，实测真值 28），此时轴按声明动，
     * 但自报的四条数原样留在 device 里。device_agrees 是 true/false/null 三态，null 表示
     * "设备没答、无从比对"。升版是因为版本 18 里只有一个 position_scale，读者无从知道
     * 设备当时报的是什么，同版本下"设备在说谎"与"人写对了档"两份报告会长得一样。
     *
     * 21：回零（G-02）。以前主站只认 csp/csv/cst，"按回零运行"这件事根本不存在；
     * 现在逐轴记启动/完成交换号、驱动器 bit13 与完成时限，结果级记是否配置、轴数、
     * 是否完成/失败。不升版的话，旧读者会把一份回零报告读成"运动从未开始"——
     * motion_started / motion_completed 对回零没有意义，回零的完成与否只在这组新字段里。
     *
     * 22：错误分类新增编号与下一步（G-06）。版本 21 只有键与中文名，现场报修还得
     * 查手册才知道往哪查。编号一经发布不再变；下一步是一句可执行的排查方向。不升版
     * 的话，同版本报告有的带这两列、有的不带，读者分不清"没有下一步"与"还没写"。
     */
    REQUIRE_WRITE(fputs("{\"schema_version\":22,\"generated_at_utc\":", stream) != EOF);
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
    /*
     * 本次生效的归档保留份数。削旧会真的删掉旧报告，而删掉之后从磁盘上看不出来
     * 曾经有过那些文件——把生效值写进报告，至少让"这一轮按多少份的窗口在留证据"
     * 成为报告自己能回答的问题（P12.29）。
     */
    REQUIRE_WRITE(fprintf(stream, ",\"report_archive_keep\":%u",
                          (unsigned int)plan->deployment->report_archive_keep) >= 0);
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
        "\"motion_completed\":%s,"
        /*
         * 回零这一层与 motion_* 并列而不是塞进去：回零没有周期目标，motion_started
         * 对它是 false 也不代表没在动。homing_axis_count 是配置了回零的轴数，
         * completed/failed 是整轮的结论；逐轴的启动/完成交换号在轴对象里。
         */
        "\"homing_configured\":%s,\"homing_axis_count\":%zu,"
        "\"homing_completed\":%s,\"homing_failed\":%s,"
        "\"safe_output_sent\":%s,"
        /*
         * P12.1: 停机请求与它的来源。stop_requested 此前根本没进报告（只有终端摘要
         * 用），于是"这一轮是被停下来的"在报告里读不出来；来源则是套接字命令与
         * 应用回调之间唯一的差别——两条路走的是同一段停机代码，差别只在这一个数。
         */
        "\"safe_state_reached\":%s,\"stop_requested\":%s,\"stop_source\":%u,"
        "\"shutdown_stop_fault\":%d,"
        "\"dc_late_consecutive_threshold\":%" PRIu32 ","
        "\"dc_late_threshold_exceeded\":%s,\"dc_late_threshold_axis\":%zu,"
        "\"sync0_disabled\":%s,"
        "\"restore_init_succeeded\":%s,"
        "\"shutdown_observer_join_ns\":%" PRIu64
        ",\"shutdown_prologue_gap_ns\":%" PRIu64 ","
        /*
         * 外部目标被拒收的记录。count 是**被拒的段数，不是拍数**——外部目标缓冲区
         * 是覆盖式最新值，主站每拍读同一条目标，按拍计数会把一个卡住的坏值记成
         * 几百万次。axis 用与其他逐轴字段同一口径（0 起）；offset 是被拒那一刻
         * |目标 - 实际位置| 超出的那个量（counts）。两个现场字段只在 count > 0 时
         * 有意义，且记的是最后一次。
         */
        "\"target_rejected_count\":%" PRIu64 ","
        "\"target_rejection_axis\":%zu,"
        "\"target_rejection_offset_counts\":%" PRIu64 ","
        /*
         * 设点通道（只写套接字）这一轮的计数。通道没开时 enabled 为假，其余为零。
         *
         * 这几个都是"发生了什么"，不是"应该发生什么"：主站不假设客户端频率，也就不
         * 报"应该收到多少条"。丢没丢由 sequence_gaps 回答，客户端收尾时是否还活着由
         * last_record_age_ns 回答（-1 表示整轮没有一条被采纳，通道没开也一样）。
         * sequence 与 client_time_ns 是客户端自己给的数，原样记下不做解释——主站判
         * 超时用的是缓冲区的主站时钟，两者不要混看。
         */
        "\"setpoint_enabled\":%s,\"setpoint_records_accepted\":%" PRIu64 ","
        "\"setpoint_records_rejected\":%" PRIu64 ",\"setpoint_sequence_gaps\":%" PRIu64 ","
        "\"setpoint_last_sequence\":%" PRIu64 ","
        "\"setpoint_last_client_time_ns\":%" PRIu64 ","
        "\"setpoint_last_record_age_ns\":%" PRId64 ","
        "\"setpoint_last_reject_axis\":%zu,",
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
        report->homing_configured ? "true" : "false",
        report->homing_axis_count,
        report->homing_completed ? "true" : "false",
        report->homing_failed ? "true" : "false",
        report->safe_output_sent ? "true" : "false",
        report->safe_state_reached ? "true" : "false",
        report->stop_requested ? "true" : "false",
        (unsigned int)report->stop_source,
        (int)report->shutdown_stop_fault,
        report->dc_late_consecutive_threshold,
        report->dc_late_threshold_exceeded ? "true" : "false",
        report->dc_late_threshold_axis,
        report->sync0_disabled ? "true" : "false",
        report->restore_init_succeeded ? "true" : "false",
        report->shutdown_observer_join_ns,
        report->shutdown_prologue_gap_ns,
        report->target_rejected_count,
        report->target_rejection_axis,
        report->target_rejection_offset_counts,
        report->setpoint_enabled ? "true" : "false",
        report->setpoint_records_accepted,
        report->setpoint_records_rejected,
        report->setpoint_sequence_gaps,
        report->setpoint_last_sequence,
        report->setpoint_last_client_time_ns,
        (int64_t)report->setpoint_last_record_age_ns,
        report->setpoint_last_reject_axis) >= 0);

    /*
     * 设点通道的拒收原因写成文本，与主站往 stderr 打的是同一句话。写成数字的话，
     * 读的人还得去翻枚举——而这个字段的用途正是回答"为什么被拒"。
     */
    REQUIRE_WRITE(fputs("\"setpoint_last_reject_reason\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(
        stream, emaster_setpoint_reject_text(report->setpoint_last_reject_reason)));
    REQUIRE_WRITE(fputs(",", stream) != EOF);
    /*
     * 本次事故的分类结论——"这一轮到底出了什么事"，与上面的 status_code（"哪一步
     * 失败了"）是两层。status_code 一个值可能装四种真的事故（WKC_MISMATCH 就是），
     * 要分开看只能靠这一组。
     *
     * 六个证据位随类别一起写，是因为同类别证据不同正是要能分开的东西：一次"整帧
     * 缺失"是环尾还是整条链路，看 evidence_no_frame 与 silent_axes；一次同步事故
     * 是靠 AL 码还是靠 SYNC0 迟到看出来的，看 sync_axes 是否为 0 而 evidence_sync
     * 为真。first_*_axis 给到具体轴号：只说"5 根不应答"分不出"尾部连号"与"散在
     * 几根"，而这两种指向完全不同的排查方向。
     *
     * 键名与 emaster_error_class_* 字段一一对应，读的人不用猜。
     */
    REQUIRE_WRITE(fputs("\"error_class\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, emaster_error_class_key(report->error_class)));
    REQUIRE_WRITE(fputs(",\"error_class_text\":", stream) != EOF);
    REQUIRE_WRITE(emaster_json_string(stream, emaster_error_class_text(report->error_class)));
    /* 编号是给工单用的短码；下一步是给现场的一句话。两者与键一起写，读报告的人
     * 不必再翻手册才知道该动哪里。编号分配记录见 docs/product/error-codes.md。 */
    REQUIRE_WRITE(fprintf(stream, ",\"error_class_code\":%u",
                          (unsigned)emaster_error_class_code(report->error_class)) >= 0);
    REQUIRE_WRITE(fputs(",\"error_class_next_step\":", stream) != EOF);
    REQUIRE_WRITE(
        emaster_json_string(stream, emaster_error_class_next_step(report->error_class)));
    REQUIRE_WRITE(fprintf(
        stream,
        ",\"error_class_evidence_no_frame\":%s,\"error_class_evidence_sync\":%s,"
        "\"error_class_silent_axes\":%u,\"error_class_sync_axes\":%u,"
        "\"error_class_first_silent_axis\":%u,\"error_class_first_sync_axis\":%u},",
        report->error_class_evidence_no_frame ? "true" : "false",
        report->error_class_evidence_sync ? "true" : "false",
        report->error_class_silent_axes,
        report->error_class_sync_axes,
        report->error_class_first_silent_axis,
        report->error_class_first_sync_axis) >= 0);
    REQUIRE_WRITE(fputs("\"first_cycle_failure\":", stream) != EOF);
    REQUIRE_WRITE(write_cycle_failure(stream, &report->first_cycle_failure));
    REQUIRE_WRITE(fputs(",\"first_runtime_failure\":", stream) != EOF);
    REQUIRE_WRITE(write_runtime_failure(stream, &report->first_runtime_failure));
    REQUIRE_WRITE(fputs(",\"soem_errors\":", stream) != EOF);
    REQUIRE_WRITE(write_soem_errors(stream, report));
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
        /*
         * P8.2: 尾部最坏阻塞时长（收包超时 + 轴数 × 每轴诊断读额度），由构造
         * 保证 ≤ 周期。跟上面的 cycle_ns 一比就能核对，不必重算额度怎么来的。
         */
        ",\"tail_blocking_budget_ns\":%" PRIu64
        ",\"error_counter_timeout_us\":%" PRIu32
        ",\"tail_max_mailbox_ns\":%" PRIu64
        ",\"tail_max_error_counter_ns\":%" PRIu64
        ",\"error_counter_read_fail_count\":%" PRIu64
        ",\"first_error_counter_read_fail_exchange\":%" PRIu64
        /*
         * 采样记账四个数：到了采样点并真的读了（attempt）、到了采样点但因为本周期
         * 已经出问题而跳过（skip）、到了采样点但周期与轴数算下来额度不足而整块不读
         * （skip_budget，P8.2）、没到采样点（不记账，可由 cycle_count 反推）。
         * 分开之后，read_fail 才只表示"想读但没读到"。
         *
         * skip_budget 是三者里唯一不会自己消失的：它只随部署的周期/轴数变化。报告里
         * 恒为 0 才说明这台配置读得起诊断；只要它非零，样本就一直在按配置缺口丢，
         * 而 read_fail / skip 都不会提到这件事。
         */
        ",\"error_counter_read_attempt_count\":%" PRIu64
        ",\"error_counter_skip_count\":%" PRIu64
        ",\"first_error_counter_skip_exchange\":%" PRIu64
        ",\"error_counter_skip_budget_count\":%" PRIu64
        ",\"first_error_counter_skip_budget_exchange\":%" PRIu64
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
        ",\"deadline_miss_tail_uncovered_ns\":%" PRIu64
        /*
         * P11.3: 交接段（进 OP → 周期循环）的仪表。这一段原先整块不发帧，实测
         * 让周期线程连续占核 852 µs，把每次会话唯一的那次截止期错失钉在了周期
         * 循环的第一拍上。改法是把交接切碎塞进 OP 请求循环，每轮照常发一帧。
         *
         * sliced 是"这一轮走的是哪条臂"的唯一凭据；max_slice_ns 是"每片有多贵"
         * 的上界，它与周期长度一比就能核对交接还会不会越界——不必再上台架去猜。
         * last_exchange 是交接期间的最后一拍：它之后的帧距若变成两个节拍点，
         * 就是这里没切干净。
         */
        ",\"handover_sliced\":%s"
        ",\"handover_last_exchange\":%" PRIu64
        ",\"handover_exchanges\":%" PRIu64
        ",\"handover_watchdog_read_max_ns\":%" PRIu64
        ",\"handover_position_prepare_ns\":%" PRIu64
        ",\"handover_observer_start_ns\":%" PRIu64
        ",\"handover_max_slice_ns\":%" PRIu64 "},",
        report->tail_max_receive_ns, report->tail_max_receive_ok_ns,
        (unsigned)report->frame_timeout_us,
        report->tail_blocking_budget_ns,
        (unsigned)report->error_counter_timeout_us,
        report->tail_max_mailbox_ns,
        report->tail_max_error_counter_ns, report->error_counter_read_fail_count,
        report->first_error_counter_read_fail_exchange,
        report->error_counter_read_attempt_count, report->error_counter_skip_count,
        report->first_error_counter_skip_exchange,
        report->error_counter_skip_budget_count,
        report->first_error_counter_skip_budget_exchange, report->over_budget_cycle_count,
        report->first_over_budget_exchange, report->first_deadline_missed_exchange,
        report->first_mismatch_present ? "true" : "false", report->first_mismatch_exchange,
        report->first_mismatch_wkc, report->frame_interval_max_ns,
        report->frame_interval_max_exchange, report->frame_interval_gap_count,
        report->first_frame_interval_gap_exchange,
        report->first_mismatch_frame_interval_ns,
        report->deadline_miss_frame_interval_ns, report->tail_uncovered_max_ns,
        report->tail_uncovered_max_exchange,
        report->deadline_miss_tail_uncovered_ns,
        report->handover_sliced ? "true" : "false", report->handover_last_exchange,
        report->handover_exchanges, report->handover_watchdog_read_max_ns,
        report->handover_position_prepare_ns, report->handover_observer_start_ns,
        report->handover_max_slice_ns) >= 0);
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
     * P9.4: 命令服务器的流量记账。三个数穷尽一条命令的三种去向，相加应等于客户端
     * 发来的命令总数——对不上说明有命令卡在队列里没被周期线程取走（停机时队列非空）。
     * 这三个数在停机销毁命令服务器时写入，所以运行中途的报告里是 0。
     */
    REQUIRE_WRITE(fprintf(
        stream,
        "\"command_server\":{\"received_count\":%" PRIu64
        ",\"invalid_count\":%" PRIu64
        ",\"queue_full_count\":%" PRIu64 "},",
        report->command_received_count, report->command_invalid_count,
        report->command_queue_full_count) >= 0);
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
    REQUIRE_WRITE(write_timing_excursions(stream, &report->timing_excursions));
    REQUIRE_WRITE(fputs(",\"cycle_trace\":", stream) != EOF);
    REQUIRE_WRITE(write_cycle_trace(stream, &report->cycle_trace));
    /* access_count 是"实际用掉多少条"，和两个容量放在一起才看得出预留是宽是紧。 */
    REQUIRE_WRITE(fprintf(stream,
        ",\"audit\":{\"omitted_pdo_samples\":%" PRIu64
        ",\"omitted_final_samples\":%" PRIu64
        ",\"access_count\":%zu,\"cyclic_capacity\":%zu,\"final_capacity\":%zu},"
        "\"diagnostic_preop_reached\":%s,"
        /*
         * attempted=写调用台数，statusword_bit2_clear=读回 bit2=0 台数，
         * statusword_bit2_set=读回 bit2=1 台数，statusword_unreadable=读不回台数。
         * 键名说的是**读到了什么**，不是"失能成没成"：真使能时 bit2 同样读到 0，
         * 叫 verified 会让它与结论行的"停用确认"并排出现两个相反的结论。
         */
        "\"shutdown_disable\":{\"attempted\":%zu,\"statusword_bit2_clear\":%zu,"
        "\"statusword_bit2_set\":%zu,\"statusword_unreadable\":%zu},"
        /* 这一轮停机循环按哪条判据判的：device_mask / standard_cia402 / none。 */
        "\"shutdown_stop_criterion\":\"%s\",",
        report->audit.omitted_pdo_samples,
        report->audit.omitted_final_samples,
        report->audit.access_count,
        report->audit.sealed_capacity,
        report->audit.final_capacity,
        report->diagnostic_preop_reached ? "true" : "false",
        report->shutdown_disable_attempted_count,
        report->shutdown_statusword_bit2_clear_count,
        report->shutdown_statusword_bit2_set_count,
        report->shutdown_statusword_unreadable_count,
        shutdown_criterion_key(report->shutdown_stop_criterion)) >= 0);
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
