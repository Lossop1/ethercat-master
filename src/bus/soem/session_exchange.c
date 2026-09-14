#include "session_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * WKC 错误恢复由配置策略决定，不再硬编码阈值。
 * 配置策略在 config/error_recovery_policies/ 中定义，部署配置引用具体策略。
 */

void emaster_soem_session_note_deadline_missed(emaster_soem_session_t *session)
{
    for (size_t axis = 0U; axis < session->plan->axis_count; ++axis)
    {
        emaster_cyclic_timing_stats_note_deadline_missed(&session->axes[axis].timing);
    }
}

/*
 * 按配置策略判断本次死区超时是否在可恢复范围内。
 * 若策略允许：重置时钟 deadline 至最近未来边界，清除 deadline_missed，返回 true。
 * 若策略不允许或时钟重置失败：返回 false，调用者负责锁存失败。
 */
bool emaster_soem_session_try_deadline_recovery(emaster_soem_session_t *session)
{
    if (session->error_recovery_policy == NULL ||
        !session->error_recovery_policy->deadline_recovery.enabled)
    {
        return false;
    }
    if (session->clock.consecutive_deadline_misses >
        session->error_recovery_policy->deadline_recovery.consecutive_error_threshold)
    {
        return false;
    }
    return emaster_cycle_clock_recover(&session->clock);
}

/* CLOCK_MONOTONIC 转 ns；失败时不动输出，调用者据此放弃这一段计时。 */
static bool monotonic_ns(const struct timespec *stamp, uint64_t *out)
{
    if (stamp == NULL || out == NULL || stamp->tv_sec < 0 || stamp->tv_nsec < 0)
    {
        return false;
    }
    *out = (uint64_t)stamp->tv_sec * UINT64_C(1000000000) + (uint64_t)stamp->tv_nsec;
    return true;
}

/* 只保留最近 EMASTER_CYCLE_TRACE_CAPACITY 个周期，写满后覆盖最旧的一条。 */
static void cycle_trace_push(emaster_cycle_trace_t *trace,
                             const emaster_cycle_trace_sample_t *sample)
{
    trace->samples[trace->write_index] = *sample;
    trace->write_index = (trace->write_index + 1U) % EMASTER_CYCLE_TRACE_CAPACITY;
    if (trace->sample_count < EMASTER_CYCLE_TRACE_CAPACITY)
    {
        ++trace->sample_count;
    }
}

/* 按时间顺序把当前环冻结进"首次不符之前"的副本，整个会话只做一次。 */
static void cycle_trace_freeze(emaster_cycle_trace_t *trace)
{
    size_t count = trace->sample_count;
    size_t start = (trace->write_index + EMASTER_CYCLE_TRACE_CAPACITY - count) %
                   EMASTER_CYCLE_TRACE_CAPACITY;

    for (size_t index = 0U; index < count; ++index)
    {
        trace->mismatch_samples[index] =
            trace->samples[(start + index) % EMASTER_CYCLE_TRACE_CAPACITY];
    }
    trace->mismatch_sample_count = count;
}

/*
 * 第一个 WKC 不符当下的现场：冻结不符之前的周期现场，并读一次三轴 AL 状态。
 * 这两件事都不发邮箱，因此不会给驱动器增加任何额外负担——这是刻意的。
 *
 * 更直接的问法是"第一个不符那一刻，驱动器自己的 SM2 事件丢失计数是多少"：
 * 若已经接近阈值，说明驱动器先开始丢同步事件、帧异常是后果；若是 0，说明帧先出
 * 了问题。但读出这个计数只能走邮箱（1C32:11），而邮箱往返在本台架上约 2.7 ms
 * （观测线程一轮 21 次往返约 56 ms，加 50 ms 睡眠），三轴两次读约 16 ms 的过程
 * 数据静默，远超把驱动器推出 OP 所需的 6 次 SM2 事件。也就是说这个仪表会亲手
 * 制造它正在观测的那次掉出——观测线程改用 50 轮一次的低频探针，正是同一个理由。
 *
 * 因此现场只记录免费的事实：帧已经短了(WKC)，而驱动器还没有被任何停机流程碰过
 * (AL state)。因果方向由两者的先后回答；计数是否已经在涨，留给低频探针和停机
 * 后的一次性读数去回答。
 */
static void record_first_mismatch_scene(emaster_soem_session_t *session)
{
    emaster_control_session_report_t *report = session->report;

    report->first_mismatch_present = true;
    report->first_mismatch_exchange = session->exchange;
    report->first_mismatch_wkc = report->actual_wkc;
    cycle_trace_freeze(&report->cycle_trace);
    report->cycle_trace.mismatch_present = true;
    report->cycle_trace.mismatch_exchange = session->exchange;
    report->cycle_trace.mismatch_wkc = report->actual_wkc;
    /*
     * 读一次从站状态（寄存器 datagram，不发邮箱）。此前的报告只有 WKC 数值，
     * 无法区分"驱动器先掉出 OP 导致 WKC 少计"和"帧本身出问题导致 WKC 少计"。
     */
    ecx_readstate(&session->context);
    for (size_t axis = 0U; axis < session->plan->axis_count; ++axis)
    {
        emaster_control_session_axis_result_t *result = &session->axes[axis];
        const ec_slavet *slave = &session->context.slavelist[axis + 1U];

        result->first_mismatch_al_read = true;
        result->first_mismatch_al_state = slave->state;
        result->first_mismatch_al_status_code = slave->ALstatuscode;
        printf("[WKC] 首次不符 交换号=%" PRIu64 " 轴%zu: AL state=%u, status_code=0x%04X\n",
               session->exchange, axis + 1U, (unsigned int)slave->state,
               (unsigned int)slave->ALstatuscode);
    }
}

/* 现场样本里的时长用 32 位存储：单周期任何一段都不可能接近 4.29 s，超界即钳位。 */
static uint32_t clamp_u32(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static emaster_control_session_status_t fail_exchange(emaster_soem_session_t *session,
                                                      emaster_audit_phase_t phase,
                                                      emaster_control_session_status_t status,
                                                      bool wkc_available) {
    emaster_cycle_failure_t *failure = &session->report->first_cycle_failure;

    if (!failure->present) {
        failure->present = true;
        failure->status = status;
        failure->phase = phase;
        failure->exchange = session->exchange;
        failure->wkc_available = wkc_available;
        failure->wkc = session->report->actual_wkc;
        failure->dc_time_ns = session->context.DCtime;
    }
    return status;
}

emaster_control_session_status_t emaster_soem_session_exchange(emaster_soem_session_t *session,
                                                               emaster_audit_phase_t phase) {
    bool matched;
    uint64_t deadline_ns;
    uint64_t now_ns;
    uint64_t next_deadline_ns;
    struct timespec send_start;
    struct timespec send_end;
    struct timespec receive_done;
    struct timespec mailbox_done;
    struct timespec receive_end;
    uint64_t receive_done_ns;
    uint64_t mailbox_done_ns;
    uint64_t receive_end_ns;
    emaster_cycle_trace_sample_t trace_sample;
    emaster_cyclic_timing_observation_t timing;
    bool tail_timing_valid;
    bool error_counter_read_failed = false;

    if (!emaster_cycle_clock_wait(&session->clock)) {
        session->report->cycle_deadline_missed |= session->clock.deadline_missed;
        if (session->clock.deadline_missed)
        {
            emaster_soem_session_note_deadline_missed(session);
            if (session->report->first_deadline_missed_exchange == 0U)
            {
                /* 此处早于 ++exchange，本次超限对应的周期号是 exchange + 1。 */
                session->report->first_deadline_missed_exchange = session->exchange + 1U;
            }
            if (emaster_soem_session_try_deadline_recovery(session))
            {
                /* 连续超次数在阈值内，重新对齐 deadline，跳过本周期交换继续运行。 */
                return EMASTER_CONTROL_SESSION_OK;
            }
            emaster_soem_session_latch_failure(
                session, EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED);
        }
        return fail_exchange(session, phase,
                             session->clock.deadline_missed
                                 ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                                 : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED,
                             false);
    }
    /* 唤醒后立即发送。审计不再位于定时点与发包之间，输出值在下一次编码前不变。 */
    if (!emaster_cycle_clock_deadline_ns(&session->clock, &deadline_ns) ||
        clock_gettime(CLOCK_MONOTONIC, &send_start) != 0)
    {
        return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED, false);
    }

    /*
     * 帧级超时：取周期的 1/4，限制在 [50, 500] µs。
     * EC_TIMEOUTRET=2000µs 超过 1ms 周期；一次接收失败会直接把当前周期
     * 拖延 2ms，连同每个从站的 FPRD 等待（各 2ms）累计超过 deadline，
     * 在非实时内核上极易触发连续 deadline miss 直至阈值终止会话。
     * 缩短后单次帧丢失最多占用 1/4 周期预算，recovery 有足够空间追回。
     */
    int frame_timeout_us = (int)((session->plan->cycle_ns / 4U) / 1000U);
    if (frame_timeout_us < 50)  { frame_timeout_us = 50; }
    if (frame_timeout_us > 500) { frame_timeout_us = 500; }

    ++session->exchange;
    (void)ecx_send_processdata(&session->context);
    if (clock_gettime(CLOCK_MONOTONIC, &send_end) != 0)
    {
        return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED, false);
    }
    session->report->actual_wkc = ecx_receive_processdata(&session->context, frame_timeout_us);
    /*
     * 收包后立刻打点，把周期尾部分成三段：收包 / 邮箱推进 / 3×FPRD(0x0300)。
     * timing 的 round_trip 从发到"最后一个 FPRD 之后"，把后两段一起算了进去，
     * 因此 1.077 ms 的往返极值无法回答"时间花在收帧上还是在主站自己的诊断读上"。
     */
    tail_timing_valid = clock_gettime(CLOCK_MONOTONIC, &receive_done) == 0;

    /* P4.1: 周期内有界邮箱推进。
     * 在 receive 后调用，让 SDO 慢速通道（P4.3 的观察线程）与周期共存。
     * limit=4 经实测（2026-09-12）对 round_trip 无显著影响（±3μs 噪声范围内），
     * 相比 limit=2 为 SDO 观测提供更多推进机会，同时避免 limit=8 的过度推进。 */
    ecx_mbxhandler(&session->context, 0, 4);
    if (tail_timing_valid && clock_gettime(CLOCK_MONOTONIC, &mailbox_done) != 0)
    {
        tail_timing_valid = false;
    }

    /* P4.4: 周期内读取错误计数器（0x0300-0x030F）。
     * FPRD 使用从站配置地址（configadr），不是位置序号。
     * 0x0300 起始 16 字节覆盖全部计数器。每个从站独立读取。 */
    for (size_t axis = 0U; axis < session->plan->axis_count; ++axis)
    {
        uint16_t configadr = session->context.slavelist[axis + 1U].configadr;
        uint8_t error_block[16];
        int wkc = ecx_FPRD(&session->context.port, configadr, 0x0300U, sizeof(error_block),
                          error_block, frame_timeout_us);
        if (wkc > 0)
        {
            session->axes[axis].error_counters_read = true;
            memcpy(session->axes[axis].rx_error_counter, &error_block[0], 8);
            memcpy(session->axes[axis].forwarded_rx_error_counter, &error_block[8], 8);
            session->axes[axis].ecat_processing_unit_error_counter = error_block[12];
            session->axes[axis].pdi_error_counter = error_block[13];
            session->axes[axis].pdi_error_code = error_block[14];
            /* 0x0310 lost_link_counter 是独立寄存器，当前跳过读取。后续可扩展。 */

            /* 首周期输出解析后的错误计数器值 */
            if (session->exchange == 1U)
            {
                uint16_t rx_p0 = ((uint16_t)error_block[0]) | (((uint16_t)error_block[1]) << 8);
                uint16_t rx_p1 = ((uint16_t)error_block[2]) | (((uint16_t)error_block[3]) << 8);
                uint16_t rx_p2 = ((uint16_t)error_block[4]) | (((uint16_t)error_block[5]) << 8);
                uint16_t rx_p3 = ((uint16_t)error_block[6]) | (((uint16_t)error_block[7]) << 8);
                fprintf(stderr, "[P4.4] 从站 %zu 错误计数器: "
                        "RX[P0=%u P1=%u P2=%u P3=%u] "
                        "FwdRX[P0=%u P1=%u P2=%u P3=%u] "
                        "EPU=%u PDI=%u PDI_code=0x%02X\n",
                        axis + 1U, rx_p0, rx_p1, rx_p2, rx_p3,
                        error_block[8], error_block[9], error_block[10], error_block[11],
                        error_block[12], error_block[13], error_block[14]);
            }
        }
        else
        {
            session->axes[axis].error_counters_read = false;
            error_counter_read_failed = true;
            /*
             * 每个 FPRD 各自带 frame_timeout_us 超时，三次读全部用掉就是一个远超
             * 周期的预算。此前只记录"本周期没读到"，不记录发生过多少次、从哪个
             * 周期开始——而这条路径本身会拖长周期，是自激式劣化的候选。
             */
            if (session->report->error_counter_read_fail_count != UINT64_MAX)
            {
                ++session->report->error_counter_read_fail_count;
            }
            if (session->report->first_error_counter_read_fail_exchange == 0U)
            {
                session->report->first_error_counter_read_fail_exchange = session->exchange;
            }
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &receive_end) != 0)
    {
        return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED, true);
    }
    ++session->report->cycle_count;
    matched = session->report->actual_wkc == (int)session->report->expected_wkc;
    if (!matched) {
        /*
         * 连续段的第一个不符是唯一能定因果的时刻：此前的报告只有 WKC 数值，
         * 无法区分"驱动器先掉出 OP 导致 WKC 少计"和"帧本身出问题导致 WKC 少计"。
         * 现场只在连续段首次触发一次，且只记录免费事实（见现场函数说明）。
         */
        if (session->wkc_consecutive_errors == 0U) {
            record_first_mismatch_scene(session);
        }
        ++session->wkc_consecutive_errors;
        ++session->wkc_total_errors;
        ++session->report->wkc_error_count;
        session->report->wkc_consecutive_errors = session->wkc_consecutive_errors;
        if (session->wkc_consecutive_errors > session->report->wkc_max_consecutive_errors) {
            session->report->wkc_max_consecutive_errors = session->wkc_consecutive_errors;
        }

        /* 检查是否超过配置的容错阈值 */
        bool should_fail = false;
        if (session->error_recovery_policy != NULL &&
            session->error_recovery_policy->wkc_recovery.enabled) {
            const emaster_wkc_recovery_config_t *wkc_cfg =
                &session->error_recovery_policy->wkc_recovery;
            if (session->wkc_consecutive_errors >= wkc_cfg->consecutive_error_threshold ||
                session->wkc_total_errors >= wkc_cfg->total_error_threshold) {
                should_fail = true;
            }
        } else {
            /* 未配置策略或WKC恢复未启用，首次错误即停机（保守默认行为） */
            should_fail = true;
        }

        if (should_fail) {
            (void)fail_exchange(session, phase, EMASTER_CONTROL_SESSION_WKC_MISMATCH, true);
        }
    } else {
        /* WKC 恢复正常，重置连续错误计数 */
        session->wkc_consecutive_errors = 0U;
        session->report->wkc_consecutive_errors = 0U;
    }
    memset(&timing, 0, sizeof(timing));
    timing.exchange = session->exchange;
    timing.host_time_valid = true;
    timing.scheduled_send_ns = deadline_ns;
    timing.host_send_start_ns =
        (uint64_t)send_start.tv_sec * UINT64_C(1000000000) + (uint64_t)send_start.tv_nsec;
    timing.host_send_end_ns =
        (uint64_t)send_end.tv_sec * UINT64_C(1000000000) + (uint64_t)send_end.tv_nsec;
    timing.host_receive_end_ns =
        (uint64_t)receive_end.tv_sec * UINT64_C(1000000000) + (uint64_t)receive_end.tv_nsec;
    timing.dc_time_valid = session->dc_required && session->context.DCtime > 0;
    timing.dc_time_ns = session->context.DCtime;
    timing.wkc_match = matched;
    for (size_t axis = 0U; axis < session->plan->axis_count; ++axis)
    {
        const emaster_operation_profile_t *operation =
            session->plan->axes[axis].operation_profile;
        if (!emaster_cyclic_timing_stats_record(
                &session->axes[axis].timing, &timing, session->plan->cycle_ns,
                operation->process_data_phase_ns, operation->sync0_shift_ns,
                session->context.slavelist[axis + 1U].pdelay))
        {
            return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_AUDIT_FAILED, true);
        }
    }
    /*
     * 周期现场入环，放在审计输出之前：不符的那些周期和终止会话的最后几个周期
     * 本身就是要看的一段，不能因为后面的提前 return 而漏掉。
     */
    {
        const emaster_cyclic_timing_stats_t *host_stats = &session->axes[0].timing;
        uint64_t send_start_ns = 0U;
        uint64_t send_end_ns = 0U;
        uint64_t total_ns = 0U;
        uint64_t send_duration_ns = 0U;
        uint64_t receive_duration_ns = 0U;
        uint64_t mailbox_duration_ns = 0U;
        uint64_t error_counter_duration_ns = 0U;

        memset(&trace_sample, 0, sizeof(trace_sample));
        trace_sample.exchange = session->exchange;
        trace_sample.wkc = session->report->actual_wkc;
        if (!matched)
        {
            trace_sample.flags |= EMASTER_CYCLE_TRACE_FLAG_WKC_MISMATCH;
        }
        if (error_counter_read_failed)
        {
            trace_sample.flags |= EMASTER_CYCLE_TRACE_FLAG_ERROR_COUNTER_READ_FAILED;
        }
        /*
         * 发送迟到与 SYNC0 裕量取自轴 1 的统计：主机时间戳三轴共用，DC 相位在
         * 参考从站（位置 1）上定义，三轴不会各自不同。scheduled 必须等于本周期的
         * deadline，否则这份统计还停在上一次成功记录的周期上，值不是本周期的。
         */
        bool stats_belong_to_cycle = host_stats->last_scheduled_send_ns == deadline_ns;

        if (stats_belong_to_cycle)
        {
            uint64_t scheduled = host_stats->last_scheduled_send_ns;
            uint64_t started = host_stats->last_host_send_start_ns;

            trace_sample.send_lateness_ns =
                started >= scheduled
                    ? (int32_t)((started - scheduled) > (uint64_t)INT32_MAX
                                    ? INT32_MAX
                                    : (started - scheduled))
                    : (int32_t)((scheduled - started) > (uint64_t)INT32_MAX
                                    ? INT32_MIN
                                    : -(int64_t)(scheduled - started));
        }
        trace_sample.sync0_margin_ns = stats_belong_to_cycle && host_stats->last_dc_sample_valid
                                           ? (int32_t)host_stats->last_sync0_margin_ns
                                           : INT32_MIN;
        if (tail_timing_valid && monotonic_ns(&receive_done, &receive_done_ns) &&
            monotonic_ns(&mailbox_done, &mailbox_done_ns) &&
            monotonic_ns(&receive_end, &receive_end_ns) &&
            monotonic_ns(&send_start, &send_start_ns) && monotonic_ns(&send_end, &send_end_ns))
        {
            send_duration_ns = send_end_ns - send_start_ns;
            receive_duration_ns = receive_done_ns - send_end_ns;
            mailbox_duration_ns = mailbox_done_ns - receive_done_ns;
            error_counter_duration_ns = receive_end_ns - mailbox_done_ns;
            total_ns = receive_end_ns - send_start_ns;
            trace_sample.send_duration_ns = clamp_u32(send_duration_ns);
            trace_sample.receive_duration_ns = clamp_u32(receive_duration_ns);
            trace_sample.mailbox_duration_ns = clamp_u32(mailbox_duration_ns);
            trace_sample.error_counter_duration_ns = clamp_u32(error_counter_duration_ns);
            if (receive_duration_ns > session->report->tail_max_receive_ns)
            {
                session->report->tail_max_receive_ns = receive_duration_ns;
            }
            if (mailbox_duration_ns > session->report->tail_max_mailbox_ns)
            {
                session->report->tail_max_mailbox_ns = mailbox_duration_ns;
            }
            if (error_counter_duration_ns > session->report->tail_max_error_counter_ns)
            {
                session->report->tail_max_error_counter_ns = error_counter_duration_ns;
            }
            if (total_ns > (uint64_t)session->plan->cycle_ns)
            {
                if (session->report->over_budget_cycle_count != UINT64_MAX)
                {
                    ++session->report->over_budget_cycle_count;
                }
                if (session->report->first_over_budget_exchange == 0U)
                {
                    session->report->first_over_budget_exchange = session->exchange;
                }
            }
        }
        cycle_trace_push(&session->report->cycle_trace, &trace_sample);
    }
    for (size_t axis = 0U; axis < session->plan->axis_count; ++axis) {
        if (!emaster_cia_process_image_audit_output(&session->images[axis], &session->report->audit,
                                                    phase, (uint16_t)(axis + 1U), session->exchange,
                                                    matched)) {
            return matched
                       ? fail_exchange(session, phase, EMASTER_CONTROL_SESSION_AUDIT_FAILED, true)
                       : EMASTER_CONTROL_SESSION_WKC_MISMATCH;
        }
    }
    /*
     * WKC 容错策略：单次或少量WKC错误可以容忍，只有持续或频繁错误才停机。
     * 这样可以避免瞬态干扰导致的误停机，同时保持对严重通信故障的响应。
     */
    if (!matched) {
        if (session->error_recovery_policy != NULL &&
            (session->wkc_consecutive_errors >=
             session->error_recovery_policy->wkc_recovery.consecutive_error_threshold ||
             session->wkc_total_errors >=
             session->error_recovery_policy->wkc_recovery.total_error_threshold)) {
            emaster_soem_session_latch_failure(session,
                                                EMASTER_CONTROL_SESSION_WKC_MISMATCH);
            return EMASTER_CONTROL_SESSION_WKC_MISMATCH;
        }
        /* 未达阈值，记录但继续运行 */
        return EMASTER_CONTROL_SESSION_OK;
    }
    if (session->dc_required &&
        !emaster_cycle_clock_observe_dc(&session->clock, session->context.DCtime)) {
        emaster_soem_session_latch_failure(session,
                                            EMASTER_CONTROL_SESSION_DC_SYNC_FAILED);
        return fail_exchange(session, phase, EMASTER_CONTROL_SESSION_DC_SYNC_FAILED, true);
    }
    if (!emaster_cycle_clock_sample(&session->clock, &now_ns, &next_deadline_ns)) {
        session->report->cycle_deadline_missed |= session->clock.deadline_missed;
        if (session->clock.deadline_missed)
        {
            emaster_soem_session_note_deadline_missed(session);
            if (session->report->first_deadline_missed_exchange == 0U)
            {
                /* 此处已在 ++exchange 之后，本次超限对应的周期号就是 exchange。 */
                session->report->first_deadline_missed_exchange = session->exchange;
            }
            /*
             * 帧已收发完毕，仅周期末尾检查超限。本周期控制输出已写入，
             * 允许按策略恢复而不丢弃本次成果。
             */
            if (emaster_soem_session_try_deadline_recovery(session))
            {
                return EMASTER_CONTROL_SESSION_OK;
            }
            emaster_soem_session_latch_failure(
                session, EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED);
        }
        return fail_exchange(session, phase,
                             session->clock.deadline_missed
                                 ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                                 : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED,
                             true);
    }
    return EMASTER_CONTROL_SESSION_OK;
}
