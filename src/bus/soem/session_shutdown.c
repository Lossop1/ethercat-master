#include "session_internal.h"

#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/*
 * 相对停机序言起点的纳秒数。起点为 0（没有成功的周期交换）或时刻早于起点时返回 0：
 * 报告里的 0 一律表示"没测到"，不能让分段标记伪装成"耗时为零"。
 */
static uint64_t prologue_mark_ns(const emaster_soem_session_t *session, uint64_t now_ns)
{
    if (session->shutdown_prologue_start_ns == 0U ||
        now_ns < session->shutdown_prologue_start_ns)
    {
        return 0U;
    }
    return now_ns - session->shutdown_prologue_start_ns;
}

/*
 * 环境变量开关：只认写明的几种真值。不能用"首字母是 o 就是 on"这种写法——off 也会命中，
 * 而这条开关唯一的用途就是对照实验，静默地把 off 当成 on 会让整批对照作废。
 */
bool emaster_soem_env_flag_enabled(const char *name)
{
    const char *value = getenv(name);

    if (value == NULL)
    {
        return false;
    }
    return strcmp(value, "1") == 0 || strcmp(value, "on") == 0 || strcmp(value, "ON") == 0 ||
           strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 || strcmp(value, "YES") == 0;
}

/*
 * 停机路径上的 AL 快照。停机结束后读到的 AL 状态（SAFE-OP + 0x1A）只能说明
 * 退出时驱动器不在 OP，无法区分三种来源：周期运行中掉出、停机序言阻塞期间掉出、
 * 或者被安全停机帧打掉。在两个时刻各读一次，把时间坐标补上。
 *
 * 同时写进报告：判据必须留在报告里，否则"序言阻塞导致掉出 OP"这条结论只能靠
 * 日志复述，报告本身仍然只有停机后的一个坐标。
 *
 * 返回值是这次快照的自身耗时（含逐轴日志行）。它落在过程数据断供窗口之内，所以必须
 * 记账：2026-09-15 的 12 臂实测里，缺口的主项正是"两次快照加审计收尾"这段非 join 工作，
 * 不把快照自身量出来，"该砍哪一段"就只能靠推断。
 */
static uint64_t snapshot_al_states(emaster_soem_session_t *session, const char *label,
                                   bool at_entry)
{
    size_t axis_index;
    uint64_t start_ns;
    uint64_t end_ns;

    if (session == NULL || session->plan == NULL)
    {
        return 0U;
    }
    start_ns = monotonic_ns();
    ecx_readstate(&session->context);
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index)
    {
        const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];

        printf("[SHUTDOWN] %s 轴%zu: AL state=%u, status_code=0x%04X\n", label,
               axis_index + 1U, (unsigned int)slave->state,
               (unsigned int)slave->ALstatuscode);
        if (at_entry)
        {
            session->axes[axis_index].shutdown_entry_al_state = slave->state;
            session->axes[axis_index].shutdown_entry_al_status_code = slave->ALstatuscode;
        }
        else
        {
            session->axes[axis_index].shutdown_pre_stop_al_state = slave->state;
            session->axes[axis_index].shutdown_pre_stop_al_status_code = slave->ALstatuscode;
        }
    }
    end_ns = monotonic_ns();
    return end_ns > start_ns ? end_ns - start_ns : 0U;
}

static void disable_sync0(ecx_contextt *context, emaster_cia_process_image_t *runtime,
                          size_t count) {
    size_t axis_index;

    if (context == NULL || runtime == NULL) {
        return;
    }
    for (axis_index = 0U; axis_index < count; ++axis_index) {
        if (runtime[axis_index].sync0_configured) {
            ecx_dcsync0(context, (uint16_t)(axis_index + 1U), FALSE, 0U, 0);
        }
    }
}

/*
 * 采一拍的现场：交换号、回帧计数、逐轴状态字与控制字。axes_decoded 说明状态字是这一拍的
 * 回帧解出来的，还是上一拍留下的——交换失败会提前返回，那一拍没有新回帧，不能把上一拍的
 * 状态冒充成"掉出 OP 的那一刻"。
 *
 * 只读会话内存，不做 I/O：这个函数在断供窗口里被调用，仪表不得改变被观测的量。
 */
static void shutdown_cycle_capture(emaster_soem_session_t *session,
                                   emaster_shutdown_cycle_sample_t *sample,
                                   bool axes_decoded)
{
    size_t axis_index;
    size_t axis_count = session->plan->axis_count;

    /* 与 first_runtime_failure 同规矩：数组是定长的，轴数超上限时截断而不是越界写。 */
    if (axis_count > EMASTER_RUNTIME_FAILURE_MAX_AXES)
    {
        axis_count = EMASTER_RUNTIME_FAILURE_MAX_AXES;
    }
    memset(sample, 0, sizeof(*sample));
    sample->exchange = session->exchange;
    sample->wkc = (int32_t)session->report->actual_wkc;
    sample->axis_count = axis_count;
    sample->axes_decoded = axes_decoded;
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        sample->status_words[axis_index] = session->status_words[axis_index];
        sample->control_words[axis_index] = session->axes[axis_index].control_word;
        sample->cia402_states[axis_index] = (uint8_t)session->axes[axis_index].cia402_state;
        sample->states_known[axis_index] =
            axes_decoded && session->axes[axis_index].input_decoded;
    }
}

/*
 * 入环：头 EMASTER_SHUTDOWN_CYCLE_CAPACITY 拍保留，最后一拍总是覆盖保存。掉出 OP 发生在
 * 停机循环的开头几拍（循环之前四轴还在 OP），所以要保头；循环跑满 max_cycles 不收敛时
 * 尾巴才有意义，所以要保尾。
 */
static void shutdown_cycle_append(emaster_shutdown_cycle_trace_t *trace,
                                  const emaster_shutdown_cycle_sample_t *sample)
{
    if (trace->sample_count < EMASTER_SHUTDOWN_CYCLE_CAPACITY)
    {
        trace->samples[trace->sample_count] = *sample;
        ++trace->sample_count;
    }
    trace->last = *sample;
    trace->last_valid = true;
}

/*
 * 停机首拍的"为什么这么久"仪表，见 emaster_shutdown_attempt_t 的注释。
 * 只读会话内存与周期时钟，不做 I/O；前两拍各记一次，第三拍起不再记。
 */
static emaster_shutdown_attempt_t *attempt_begin(emaster_soem_session_t *session,
                                                 emaster_shutdown_cycle_trace_t *trace)
{
    emaster_shutdown_attempt_t *attempt;
    uint64_t deadline_ns = 0U;

    if (trace->attempt_count >= EMASTER_SHUTDOWN_ATTEMPT_CAPACITY)
    {
        return NULL;
    }
    attempt = &trace->attempts[trace->attempt_count];
    ++trace->attempt_count;
    memset(attempt, 0, sizeof(*attempt));
    attempt->begin_ns = monotonic_ns();
    if (emaster_cycle_clock_deadline_ns(&session->clock, &deadline_ns))
    {
        attempt->deadline_before_ns = deadline_ns;
    }
    attempt->correction_ns = session->clock.correction_ns;
    attempt->phase_error_ns = session->clock.phase_error_ns;
    return attempt;
}

static void attempt_end(emaster_shutdown_attempt_t *attempt,
                        emaster_soem_session_t *session, int exchange_status)
{
    uint64_t deadline_ns = 0U;

    if (attempt == NULL)
    {
        return;
    }
    attempt->end_ns = monotonic_ns();
    attempt->exchange_after = session->exchange;
    attempt->exchange_status = exchange_status;
    if (emaster_cycle_clock_deadline_ns(&session->clock, &deadline_ns))
    {
        attempt->deadline_after_ns = deadline_ns;
    }
}

/*
 * 把首拍仪表的时刻换成"相对序言起点多少毫秒"。记不到的时刻（交换前的读数为 0、
 * 或那一拍在交换之前就中止了）写成"无"：减出来会是一个巨大的负数，比不写更误导。
 */
static void format_offset(char *buffer, size_t size, int64_t origin, uint64_t value)
{
    if (value == 0U || origin == 0)
    {
        (void)snprintf(buffer, size, "无");
        return;
    }
    (void)snprintf(buffer, size, "%+.3f", (double)((int64_t)value - origin) / 1000000.0);
}

static void log_shutdown_attempts(const emaster_soem_session_t *session)
{
    const emaster_shutdown_cycle_trace_t *trace = &session->report->shutdown_cycles;
    int64_t origin = (int64_t)session->report->shutdown_prologue.origin_monotonic_ns;
    size_t index;

    if (trace->attempt_count == 0U || origin == 0)
    {
        return;
    }
    for (index = 0U; index < trace->attempt_count; ++index)
    {
        const emaster_shutdown_attempt_t *attempt = &trace->attempts[index];
        char begin_text[64];
        char before_text[64];
        char after_text[64];
        char end_text[64];

        format_offset(begin_text, sizeof(begin_text), origin, attempt->begin_ns);
        format_offset(before_text, sizeof(before_text), origin, attempt->deadline_before_ns);
        format_offset(after_text, sizeof(after_text), origin, attempt->deadline_after_ns);
        format_offset(end_text, sizeof(end_text), origin, attempt->end_ns);
        printf("[SHUTDOWN] 停机第%zu拍：%s ms 开始，交换前边界 %s ms，交换后边界 %s ms，"
               "返回于 %s ms，交换号 %" PRIu64 "，状态 %d，修正 %+" PRId64 " ns，"
               "相位误差 %+" PRId64 " ns\n",
               index, begin_text, before_text, after_text, end_text, attempt->exchange_after,
               (int)attempt->exchange_status, attempt->correction_ns, attempt->phase_error_ns);
    }
}

/*
 * 停机仍使用相同的控制器和过程映像；任何一次交换失败都不能当作停用确认。
 *
 * 这里刻意不经过多轴协调器。协调器会重新解码状态字，解不出来就拒绝整帧并返回
 * 错误——而"状态字解不出来"恰恰是最需要发出停用命令的情形。早退会让安全输出一次
 * 都发不出去：现场表现是报告里 safe_output_sent=false，而主站的失败消息却声称
 * "已尝试发送安全输出"。目标在进入本函数前已固定为 SAFE_STOP，控制器对未知状态
 * 只会产生 Disable Voltage（0x0000），逐轴直接计算不会产生任何使能位；跨轴一致性
 * 在此也没有意义，每轴独立撤销使能就是想要的终点。
 *
 * 被放弃的只有协调器的两项检查：序号单调和帧截止时间。两者都是为"继续驱动轴"
 * 服务的，在撤销使能的路径上没有对应风险。
 */
static bool stop_process_data(emaster_soem_session_t *session) {
    uint64_t timeout_ns;
    uint64_t max_cycles;
    uint64_t cycle_index;
    size_t axis_index;
    emaster_shutdown_cycle_trace_t *trace = &session->report->shutdown_cycles;
    /*
     * 首次 WKC 不符的基准。wkc_error_count 是整轮运行的累计值，周期阶段可能已经涨过；
     * 不比基准的话，"停机循环第一拍"会被当成首次不符，快照就钉错了周期。
     */
    uint64_t mismatch_baseline = session->report->wkc_error_count;

    timeout_ns = (uint64_t)EC_TIMEOUTSTATE * UINT64_C(1000);
    max_cycles = (timeout_ns + session->plan->cycle_ns - UINT64_C(1)) / session->plan->cycle_ns;
    if (max_cycles == 0U) {
        max_cycles = 1U;
    }
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        session->status_words[axis_index] = session->axes[axis_index].status_word;
        if (!emaster_cia402_controller_set_goal(&session->controllers[axis_index],
                                                EMASTER_CIA402_GOAL_SAFE_STOP)) {
            return false;
        }
    }

    for (cycle_index = 0U; cycle_index < max_cycles; ++cycle_index) {
        bool all_axes_safe = true;
        uint64_t exchange_before = session->exchange;
        emaster_shutdown_cycle_sample_t sample;
        emaster_shutdown_attempt_t *attempt;

        ++trace->cycle_total;
        attempt = attempt_begin(session, trace);

        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            ec_slavet *slave = &session->context.slavelist[axis_index + 1U];

            /* 失败时 controller_outputs 保持上周期值，此时直接返回不发帧。
             * 这一拍连帧都没发，过程数据没有出现新的缺口，只记断在哪一轴哪一步。 */
            if (!emaster_cia402_controller_step(&session->controllers[axis_index],
                                                session->status_words[axis_index],
                                                &session->controller_outputs[axis_index])) {
                trace->aborted_before_exchange = true;
                trace->aborted_axis = axis_index;
                trace->aborted_stage = 0;
                return false;
            }
            session->axes[axis_index].control_word =
                session->controller_outputs[axis_index].control_word;
            if (!emaster_cia_process_image_update_output(
                    &session->plan->axes[axis_index], &session->images[axis_index],
                    session->controller_outputs[axis_index].control_word,
                    emaster_soem_axis_target_value(&session->plan->axes[axis_index],
                                                   &session->axes[axis_index]),
                    slave->outputs, slave->Obytes)) {
                trace->aborted_before_exchange = true;
                trace->aborted_axis = axis_index;
                trace->aborted_stage = 1;
                return false;
            }
        }
        emaster_control_session_status_t exchange_status =
            emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_SAFE_STOP);

        attempt_end(attempt, session, (int)exchange_status);
        /*
         * 记账先于判定。交换返回失败（WKC 不符、周期截止时间错过等）只说明本帧
         * 没有得到确认，不说明帧没有发出去——而 safe_output_sent 要回答的正是
         * "发没发"。此前先判失败再记账，于是"帧已发出但 WKC 不符"的停机被记成
         * safe_output_sent=false，报告在这一点上把"没发"和"发了没被确认"混成一句，
         * 而这恰是 SAFE-OP+0x1A 掉出场景下最需要区分的一对事实。
         *
         * 只有真正发出过帧才算"已发送安全输出"。周期时钟恢复会把本周期整帧跳过
         * （session_exchange.c 的 deadline_recovery 分支），那时交换号不变，不算数。
         */
        if (session->exchange > exchange_before) {
            session->report->safe_output_sent = true;
            /*
             * 序言缺口只记一次：起点是停机入口抓的最后一条周期帧发送时刻，终点是这里——
             * 第一条真正发出去的安全停机帧。两者同为 CLOCK_MONOTONIC 的发送结束时刻，
             * 差值就是驱动器在这段停机序言里没收到任何过程数据的窗口。
             */
            if (session->report->shutdown_prologue_gap_ns == 0U &&
                session->shutdown_prologue_start_ns != 0U) {
                uint64_t send_end_ns = session->axes[0].timing.last_host_send_end_ns;

                if (send_end_ns > session->shutdown_prologue_start_ns) {
                    session->report->shutdown_prologue_gap_ns =
                        send_end_ns - session->shutdown_prologue_start_ns;
                    /* 与缺口同一个时刻：分段计时表里"第一条安全停机帧"就是缺口终点。 */
                    session->report->shutdown_prologue.mark_first_safe_frame_ns =
                        session->report->shutdown_prologue_gap_ns;
                }
            }
        }
        /*
         * 交换那一拍的现场。状态字是上一拍解出来的（axes_decoded=false）——本拍的解码在
         * 下面，而交换失败会直接返回；把"哪一拍开始不对劲"钉住比补一个假的状态字重要。
         */
        shutdown_cycle_capture(session, &sample, false);
        sample.exchange_status = (int)exchange_status;
        /*
         * 循环内首次 WKC 不符：立刻读一次 AL 状态，把"驱动器什么时候掉出 OP"钉到周期
         * 粒度。12 臂实测只有循环前后两个坐标，中间 3～4 ms 没有仪表，掉出点只能靠猜。
         *
         * 位置必须在下面的提前返回之前：驱动器掉出 OP 正是以 WKC 短计的形式表现出来的，
         * 而那种交换多半直接返回失败、循环当拍就结束——快照放在判定之后等于永不触发。
         *
         * 这次读取本身要在断供窗口里占时间（一次 ecx_readstate），耗时记进
         * mismatch_al_read_ns；只触发一次，且只在已经出错之后——正常路径上零代价。
         */
        if (!trace->mismatch_al_read &&
            session->report->wkc_error_count > mismatch_baseline) {
            uint64_t read_start_ns = monotonic_ns();
            uint64_t read_end_ns;

            ecx_readstate(&session->context);
            read_end_ns = monotonic_ns();
            trace->mismatch_al_read = true;
            trace->mismatch_al_exchange = session->exchange;
            trace->mismatch_al_read_ns =
                read_end_ns > read_start_ns ? read_end_ns - read_start_ns : 0U;
            trace->mismatch_al_axis_count = session->plan->axis_count;
            if (trace->mismatch_al_axis_count > EMASTER_RUNTIME_FAILURE_MAX_AXES) {
                trace->mismatch_al_axis_count = EMASTER_RUNTIME_FAILURE_MAX_AXES;
            }
            for (axis_index = 0U; axis_index < trace->mismatch_al_axis_count; ++axis_index) {
                trace->mismatch_al_state[axis_index] =
                    session->context.slavelist[axis_index + 1U].state;
                trace->mismatch_al_status_code[axis_index] =
                    session->context.slavelist[axis_index + 1U].ALstatuscode;
            }
        }
        /* 记账之后才判失败：本帧的去向、以及出错当拍的现场都已经记进报告。 */
        if (exchange_status != EMASTER_CONTROL_SESSION_OK) {
            shutdown_cycle_append(trace, &sample);
            return false;
        }
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            const ec_slavet *slave = &session->context.slavelist[axis_index + 1U];
            emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
            emaster_cia402_state_t state;
            int32_t actual_position = 0;

            axis->input_decoded = emaster_cia_process_image_decode_input(
                &session->images[axis_index], slave->inputs, slave->Ibytes, &axis->mode_display,
                &axis->status_word, &actual_position);
            (void)emaster_soem_axis_set_feedback(&session->plan->axes[axis_index], axis,
                                                  actual_position);
            session->status_words[axis_index] = axis->status_word;
            if (axis->input_decoded) {
                (void)emaster_cia_process_image_audit_input(
                    &session->images[axis_index], &session->report->audit,
                    EMASTER_AUDIT_PHASE_SAFE_STOP, (uint16_t)(axis_index + 1U), session->exchange);
            }
            if (!axis->input_decoded ||
                !emaster_cia402_decode_status_word(axis->status_word, &state)) {
                all_axes_safe = false;
                continue;
            }
            axis->cia402_state = state;
            {
                const emaster_slave_profile_t *profile =
                    session->plan->axes[axis_index].device_profile;
                bool device_safe = profile != NULL && profile->safe_stop_status_mask != 0U &&
                                   (axis->status_word & profile->safe_stop_status_mask) ==
                                       profile->safe_stop_status_value;
                bool standard_safe = state == EMASTER_CIA402_STATE_SWITCH_ON_DISABLED ||
                                     state == EMASTER_CIA402_STATE_NOT_READY_TO_SWITCH_ON;

                if (!device_safe && !standard_safe) {
                    all_axes_safe = false;
                }
            }
        }
        /* 本拍解码完的现场覆盖交换时那份：状态字与 CiA402 状态都已是本拍的。 */
        shutdown_cycle_capture(session, &sample, true);
        sample.exchange_status = (int)exchange_status;
        sample.all_axes_safe = all_axes_safe;
        shutdown_cycle_append(trace, &sample);
        if (all_axes_safe) {
            return true;
        }
    }
    return false;
}

/*
 * 收尾时抓一次各线程的调度累计值（/proc/self/task/<tid>/schedstat）。运行期当然也能读，
 * 但在实时窗口里做文件 I/O 本身就是干扰——这正是要测的东西。放在停机之后只回答一个问题：
 * 这一轮里各线程各被调度了多久、在运行队列上等了多久（wait_ns）。周期线程所在进程里有
 * 三个同优先级 SCHED_FIFO 线程、亲和掩码同为 11，同优先级 FIFO 之间不抢占，任何一个
 * 长时间占核都会把周期线程整段挡住——wait_ns 是这条假设的直接度量。
 *
 * 报告里没有线程名（内核 comm 对同进程线程是同一个），行靠 tid 识别：主线程的 tid 可从
 * /proc/self/status 的 Pid 读出，观测线程的 tid 在 observer_stop 相位现场里对齐。
 */
static void capture_thread_schedstat(emaster_control_session_report_t *report)
{
    DIR *task_dir;
    struct dirent *entry;

    report->thread_schedstat_count = 0U;
    task_dir = opendir("/proc/self/task");
    if (task_dir == NULL)
    {
        return;
    }
    while ((entry = readdir(task_dir)) != NULL &&
           report->thread_schedstat_count < EMASTER_THREAD_SCHEDSTAT_CAPACITY)
    {
        emaster_thread_schedstat_t *row;
        /*
         * 缓冲按最坏情况给足：d_name 最长 255 字节，"/proc/self/task/<255>/schedstat"
         * 约 282 字节。64 字节会让 -Wformat-truncation 报警（tid 实际不会那么长，
         * 但"实际不会发生"不该靠编译器去猜）。
         */
        char path[300];
        char line[128];
        FILE *stream;
        unsigned long long exec_ns = 0ULL;
        unsigned long long wait_ns = 0ULL;
        unsigned long long switches = 0ULL;
        int matched;
        uint32_t tid;

        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
        {
            continue;
        }
        tid = (uint32_t)strtoul(entry->d_name, NULL, 10);
        if (tid == 0U)
        {
            continue;
        }
        (void)snprintf(path, sizeof(path), "/proc/self/task/%s/schedstat", entry->d_name);
        stream = fopen(path, "r");
        if (stream == NULL)
        {
            continue;
        }
        matched = fscanf(stream, "%llu %llu %llu", &exec_ns, &wait_ns, &switches);
        (void)fclose(stream);
        if (matched != 3)
        {
            continue;
        }
        row = &report->thread_schedstat[report->thread_schedstat_count];
        memset(row, 0, sizeof(*row));
        row->tid = tid;
        row->exec_ns = (uint64_t)exec_ns;
        row->wait_ns = (uint64_t)wait_ns;
        row->switches = (uint64_t)switches;
        (void)snprintf(path, sizeof(path), "/proc/self/task/%s/sched", entry->d_name);
        stream = fopen(path, "r");
        if (stream != NULL)
        {
            while (fgets(line, sizeof(line), stream) != NULL)
            {
                char *colon = strchr(line, ':');

                if (colon == NULL)
                {
                    continue;
                }
                if (strncmp(line, "policy", 6) == 0)
                {
                    row->policy = atoi(colon + 1);
                }
                else if (strncmp(line, "prio", 4) == 0)
                {
                    row->priority = atoi(colon + 1);
                }
            }
            (void)fclose(stream);
        }
        ++report->thread_schedstat_count;
    }
    (void)closedir(task_dir);
}

/*
 * 停机序言的步数上限。观测线程的单次邮箱读有超时上限（OBSERVER_MAILBOX_TIMEOUT_US），
 * 正常几毫秒内就会退出；50 拍（50 ms）只用于兜住"线程卡住不动"这种异常。
 * 超限时不做补救，直接把剩下的序言交回常规路径——那里用的是阻塞 join，行为与改造前一致。
 */
#define EMASTER_PROLOGUE_DRAIN_MAX_CYCLES 50U

/*
 * 序言融入周期：把停机准备拆成每周期一步，步与步之间照常走一次过程数据交换。
 *
 * 原来的序言是"先停供、再准备"：周期回路一退出过程数据就断了，而准备工作（等观测
 * 线程退出、两次 AL 快照、审计收尾）要 1.4～2.0 ms，经 1 ms 时钟栅格量化成 2～3 ms
 * 的断供窗口。驱动器按 CiA402 同步容差判：窗口 ≈2 ms 约四成掉出 OP，≈3 ms 全掉，
 * 掉出即 AL 0x1A，之后停机帧再正确也无效——24 轮交替对照里 7 轮这样收尾。
 *
 * 这里改成"边发边准备"：驱动器整段停机序言里一直有过程数据。缺口是被消除的，
 * 不是被缩短的——每一拍之间都有帧，所以没有哪一段可以叫做断供窗口。
 *
 * 只在未锁存故障、过程映像可用、且跑过周期时调用：故障态的通信已经可疑，
 * 快退优先于供帧连续；没跑过周期则根本没有缺口可言，白等 50 拍。
 *
 * 返回 true 表示序言已全部做完；false 表示中途通信失败或超限，调用者按常规路径收尾。
 * 每一步都写成幂等的：即便这里做到一半退出，常规序言重做一遍也不会出错。
 */
static bool prologue_drain(emaster_soem_session_t *session)
{
    enum
    {
        STEP_STOP_OBSERVER = 0, /* 置停止标志（不 join） */
        STEP_WAIT_OBSERVER,     /* 每拍看一眼线程退没退 */
        STEP_ENTRY_AL,          /* 入口 AL 快照 */
        STEP_PRE_STOP_AL,       /* 安全停机前 AL 快照 */
        STEP_AUDIT,             /* 审计解封 */
        STEP_DONE
    };
    int stage = STEP_STOP_OBSERVER;
    uint64_t cycles = 0U;
    uint64_t origin_ns = session->shutdown_prologue_start_ns;

    while (stage != STEP_DONE && cycles < EMASTER_PROLOGUE_DRAIN_MAX_CYCLES)
    {
        switch (stage)
        {
        case STEP_STOP_OBSERVER:
            /* 只置标志：join 会等到观测线程把手上那次邮箱往返做完（实测最长 5.34 ms），
             * 那正是原先把缺口拉长的东西。 */
            emaster_soem_session_request_observer_stop(session);
            stage = STEP_WAIT_OBSERVER;
            break;
        case STEP_WAIT_OBSERVER:
            /* 还没退就什么都不做——本拍只发帧，等下一拍再看。等待因此不产生断供。 */
            if (!emaster_soem_session_observer_exited(session))
            {
                break;
            }
            emaster_soem_session_reap_observer(session);
            stage = STEP_ENTRY_AL;
            break;
        case STEP_ENTRY_AL:
            session->report->shutdown_prologue.entry_al_read_ns =
                snapshot_al_states(session, "停机入口(序言内)", true);
            stage = STEP_PRE_STOP_AL;
            break;
        case STEP_PRE_STOP_AL:
            session->report->shutdown_prologue.pre_stop_al_read_ns =
                snapshot_al_states(session, "安全停机前(序言内)", false);
            stage = STEP_AUDIT;
            break;
        case STEP_AUDIT:
            emaster_run_audit_end_cyclic(&session->report->audit);
            stage = STEP_DONE;
            break;
        default:
            stage = STEP_DONE;
            break;
        }
        if (stage == STEP_DONE)
        {
            break;
        }
        if (emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_SAFE_STOP) !=
            EMASTER_CONTROL_SESSION_OK)
        {
            /* 通信坏了：剩下的序言交回常规路径，那里既能收尾也负责记失败。 */
            return false;
        }
        ++cycles;
    }
    if (stage != STEP_DONE)
    {
        return false;
    }

    /*
     * 序言做完了，最后一条帧就是刚才那一拍。缺口起点改记到这里：序言帧也是过程数据，
     * 驱动器在这段里从没断过供，把序言长度算进缺口只会得出一个吓人的假数。
     * 序言本身有多长另外记——它不是缺口，但它解释了停机为什么晚了这么多。
     */
    session->report->shutdown_prologue.inline_drain_cycles = cycles;
    {
        uint64_t last_send_end_ns = session->axes[0].timing.last_host_send_end_ns;

        if (origin_ns != 0U && last_send_end_ns > origin_ns)
        {
            session->report->shutdown_prologue.inline_drain_ns = last_send_end_ns - origin_ns;
        }
        session->shutdown_prologue_start_ns = last_send_end_ns;
        session->report->shutdown_prologue.start_valid = last_send_end_ns != 0U;
        session->report->shutdown_prologue.origin_monotonic_ns = last_send_end_ns;
    }
    return true;
}

void emaster_soem_session_shutdown(emaster_soem_session_t *session) {
    size_t axis_index;
    /*
     * EMASTER_SHUTDOWN_FAST：默认关，默认行为与加装仪表之前逐字一致（报告里
     * fast_mode=false 自证）。打开后把两次 AL 快照移出过程数据断供窗口——这是对照实验：
     * 若失败率随之下降，说明诊断自身就是缺口的一部分。快路径下
     * shutdown_entry_al_* / shutdown_pre_stop_al_* 保持 0，表示"本轮没取"，
     * 不是"状态为 0"；停机后的那个坐标（shutdown_al_*）照常取。
     */
    bool fast_mode = emaster_soem_env_flag_enabled("EMASTER_SHUTDOWN_FAST");
    /*
     * 序言融入周期现在是默认行为：停机序言不再"先停供、再准备"，等观测线程退出与
     * 两次 AL 快照被拆成每周期一步，步与步之间照常发帧，2～3 ms 的断供窗口从物理上
     * 消失。见 prologue_drain。
     *
     * EMASTER_SHUTDOWN_LEGACY_PROLOGUE=1 可以退回改造前的行为——只在需要复现旧故障
     * 或做对照时才设（报告里 inline_mode=false 自证走了旧路）。
     *
     * 依据（2026-09-16，四轴、25 s × 21 轮、同条件交替）：旧路径 10 轮里有 8 轮停机
     * 缺供 ≥1.99 ms、3 轮掉出 OP；新路径 10 轮缺口一律 1.000 ms（= 一个时钟栅格，
     * 即停机首拍本来就该在的位置），0 轮掉出，且全程最大帧距压在 1.02～1.04 ms。
     */
    bool inline_mode = !emaster_soem_env_flag_enabled("EMASTER_SHUTDOWN_LEGACY_PROLOGUE");
    bool prologue_done = false;

    /*
     * 缺口测量的起点：最后一条周期帧的发送结束时刻。此刻 timing 里的最后一条样本就是
     * 周期回路的最后一条帧，安全停机帧还没发（下面的 join 与快照都在窗口之内）。
     */
    if (session->plan != NULL && session->plan->axis_count > 0U)
    {
        session->shutdown_prologue_start_ns = session->axes[0].timing.last_host_send_end_ns;
    }
    session->report->shutdown_prologue.start_valid =
        session->shutdown_prologue_start_ns != 0U;
    session->report->shutdown_prologue.fast_mode = fast_mode;
    session->report->shutdown_prologue.inline_mode = inline_mode;
    /* 绝对原点：停机首拍的仪表记的是时钟绝对时刻，要跟分段标记放在同一条时间轴上。 */
    session->report->shutdown_prologue.origin_monotonic_ns =
        session->shutdown_prologue_start_ns;

    /*
     * 序言融入周期。走完之后序言已经全部做完，下面的常规序言整块跳过——两者的关系是
     * 二选一，不是叠加：这里返回 true 才知道可以跳过，返回 false 就原样走常规路径收尾。
     * 只在没有锁存故障时走：故障态的通信已经可疑，快退优先于供帧连续。
     */
    if (inline_mode && !session->fault_latched && session->context_open &&
        session->process_map_ready && session->cycle_output_active &&
        session->report->status == EMASTER_CONTROL_SESSION_OK &&
        session->shutdown_prologue_start_ns != 0U)
    {
        prologue_done = prologue_drain(session);
    }

    if (!prologue_done)
    {
    /* 进入停机时的 AL 快照：此时周期回路已经退出，但一个停机帧都还没发。 */
    if (session->context_open && !fast_mode)
    {
        session->report->shutdown_prologue.entry_al_read_ns =
            snapshot_al_states(session, "停机入口", true);
    }
    session->report->shutdown_prologue.mark_after_entry_al_ns =
        prologue_mark_ns(session, monotonic_ns());

    /* P4.3: 停止 SDO 慢速观测线程。这是停机序言里唯一的阻塞点，计时留证。 */
    {
        struct timespec join_start;
        struct timespec join_end;

        (void)clock_gettime(CLOCK_MONOTONIC, &join_start);
        emaster_soem_session_stop_observer(session);
        if (clock_gettime(CLOCK_MONOTONIC, &join_end) == 0)
        {
            uint64_t start_ns = (uint64_t)join_start.tv_sec * UINT64_C(1000000000) +
                                (uint64_t)join_start.tv_nsec;
            uint64_t end_ns = (uint64_t)join_end.tv_sec * UINT64_C(1000000000) +
                              (uint64_t)join_end.tv_nsec;

            session->report->shutdown_observer_join_ns = end_ns - start_ns;
            printf("[SHUTDOWN] 停止观测线程耗时 %.3f ms（该窗口内不发送过程数据）\n",
                   (double)(end_ns - start_ns) / 1000000.0);
        }
    }
    session->report->shutdown_prologue.mark_after_join_ns =
        prologue_mark_ns(session, monotonic_ns());

    if (!session->context_open) {
        return;
    }
    /*
     * 安全停机帧之前再读一次。两次快照相同 → 驱动器是在周期运行期间掉出 OP；
     * 第一次在 OP、第二次掉出 → 掉出发生在停机序言（观测线程收尾等）阻塞期间，
     * 与安全停机帧无关。这正是 WKC 从 9 掉到 3 的两种互斥解释。
     */
    if (!fast_mode)
    {
        session->report->shutdown_prologue.pre_stop_al_read_ns =
            snapshot_al_states(session, "安全停机前", false);
    }
    session->report->shutdown_prologue.mark_after_pre_stop_al_ns =
        prologue_mark_ns(session, monotonic_ns());
    /*
     * 审计在周期阶段是封顶的（超出预算的样本只计数不保存）。若在这里才解封，
     * 则安全停机阶段——恰恰是故障发生的阶段——一个样本都留不下。
     */
    emaster_run_audit_end_cyclic(&session->report->audit);
    session->report->shutdown_prologue.mark_after_audit_ns =
        prologue_mark_ns(session, monotonic_ns());
    }
    if (session->process_map_ready && session->cycle_output_active) {
        bool communication_usable =
            session->report->status != EMASTER_CONTROL_SESSION_WKC_MISMATCH &&
            session->report->status != EMASTER_CONTROL_SESSION_DC_SYNC_FAILED &&
            session->report->status != EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED &&
            session->report->status != EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED;

        if (session->report->status == EMASTER_CONTROL_SESSION_OK)
        {
            emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_STOPPING,
                                           EMASTER_CONTROL_SESSION_OK);
        }
        /*
         * 只有通信和周期时钟仍可用时，才能把逐级停用称为已确认
         * WKC、DC 或截止时间失效后继续发送只能算尽力而为，不能伪造安全到达结论
         */
        if (communication_usable) {
            session->report->safe_state_reached = stop_process_data(session);
            if (session->report->shutdown_prologue_gap_ns != 0U)
            {
                printf("[SHUTDOWN] 停机序言过程数据缺口 %.3f ms"
                       "（最后一条周期帧 → 第一条安全停机帧）\n",
                       (double)session->report->shutdown_prologue_gap_ns / 1000000.0);
                if (session->report->shutdown_prologue.inline_mode)
                {
                    printf("[SHUTDOWN] 序言融入周期：%llu 拍、跨度 %.3f ms，"
                           "期间过程数据未中断；缺口只剩余上面的 %.3f ms\n",
                           (unsigned long long)
                               session->report->shutdown_prologue.inline_drain_cycles,
                           (double)session->report->shutdown_prologue.inline_drain_ns /
                               1000000.0,
                           (double)session->report->shutdown_prologue_gap_ns / 1000000.0);
                }
                /*
                 * 分段账：四段依次是 入口 AL 快照 → 停观测线程(join) → 安全停机前 AL 快照
                 * → 审计收尾，到第一条安全停机帧为止。此处报的是各分段终点的累计位置，
                 * 不是单段耗时；两次快照的自身耗时单独列出。只报数，不下结论。
                 *
                 * inline 模式下不打这张表：序言工作在帧与帧之间完成，不落在缺口窗口里，
                 * 相对缺口原点记出来全是一串 0，看的人只会以为仪表坏了。
                 */
                if (!session->report->shutdown_prologue.inline_mode)
                printf("[SHUTDOWN] 缺口分段累计 ms：入口快照后 %.3f / join 后 %.3f / "
                       "停机前快照后 %.3f / 审计收尾后 %.3f / 首帧 %.3f"
                       "（两次快照自身 %.3f + %.3f）\n",
                       (double)session->report->shutdown_prologue.mark_after_entry_al_ns / 1000000.0,
                       (double)session->report->shutdown_prologue.mark_after_join_ns / 1000000.0,
                       (double)session->report->shutdown_prologue.mark_after_pre_stop_al_ns / 1000000.0,
                       (double)session->report->shutdown_prologue.mark_after_audit_ns / 1000000.0,
                       (double)session->report->shutdown_prologue.mark_first_safe_frame_ns / 1000000.0,
                       (double)session->report->shutdown_prologue.entry_al_read_ns / 1000000.0,
                       (double)session->report->shutdown_prologue.pre_stop_al_read_ns / 1000000.0);
            }
        }
        log_shutdown_attempts(session);
        if (communication_usable && !session->report->safe_state_reached &&
            session->report->status == EMASTER_CONTROL_SESSION_OK) {
            session->report->status = EMASTER_CONTROL_SESSION_SAFE_STOP_FAILED;
            session->report->fault_latched = true;
            emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_FAULTED,
                                           session->report->status);
        }
    }

    /* 先结束输出应用状态，再执行同步邮箱读取；诊断不再制造运行中的周期空洞。 */
    if (session->report->op_reached) {
        ecx_readstate(&session->context);
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            session->axes[axis_index].shutdown_al_state =
                session->context.slavelist[axis_index + 1U].state;
            session->axes[axis_index].shutdown_al_status_code =
                session->context.slavelist[axis_index + 1U].ALstatuscode;
        }
        session->context.slavelist[0].state = EC_STATE_PRE_OP;
        session->report->diagnostic_preop_reached =
            ecx_writestate(&session->context, 0U) > 0 &&
            ecx_statecheck(&session->context, 0U, EC_STATE_PRE_OP, EC_TIMEOUTSTATE) ==
                EC_STATE_PRE_OP;
    }
    if (session->sync0_started) {
        disable_sync0(&session->context, session->images, session->plan->axis_count);
        session->report->sync0_disabled = true;
    }
    /*
     * 停机诊断此前只在 PRE-OP 到达后执行。驱动器掉出 OP（SAFE-OP + 0x1A）会让
     * PRE-OP 切换超时，于是"最需要诊断的那次运行"里，全部停机 SDO 诊断一起被跳过：
     * 报告中的 read_succeeded=false 分不清"没尝试"和"读失败"，SM 同步计数器
     * （1C32/1C33）就是这样一轮都没读到。邮箱在 SAFE-OP 下仍然可用，会话还在就能读。
     */
    if (session->report->diagnostic_preop_reached || session->report->safe_op_reached) {
        for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
            emaster_control_session_axis_result_t *axis_result = &session->axes[axis_index];
            emaster_soem_sdo_reader_context_t sdo;

            emaster_soem_sdo_context_init(&sdo, &session->context, (uint16_t)(axis_index + 1U),
                                          &session->report->audit,
                                          EMASTER_AUDIT_PHASE_FINAL_DIAGNOSTIC, session->exchange);
            axis_result->mode_command_sdo_read = emaster_soem_read_i8(
                &sdo, UINT16_C(0x6060), UINT8_C(0), &axis_result->mode_command_sdo);
            axis_result->mode_display_sdo_read = emaster_soem_read_i8(
                &sdo, UINT16_C(0x6061), UINT8_C(0), &axis_result->mode_display_sdo);
            axis_result->following_error_read = emaster_soem_read_i32(
                &sdo, UINT16_C(0x60F4), UINT8_C(0),
                &axis_result->following_error_actual);
            /*
             * 这里是退出 OP 后的诊断快照。计数器可能包含停机和状态转换期间的事件，
             * 不能与 first_cycle_failure 中的首次周期异常等同。
             */
            emaster_session_observer_read_drive(&sdo, &axis_result->drive_diagnostic);
            emaster_session_observer_read_sync(&sdo, UINT16_C(0x1C32),
                                               &axis_result->sm2_diagnostic);
            emaster_session_observer_read_sync(&sdo, UINT16_C(0x1C33),
                                               &axis_result->sm3_diagnostic);
            if (!axis_result->position_scale.read_succeeded) {
                emaster_session_observer_read_position_scale(&sdo, &axis_result->position_scale);
            }
            axis_result->final_diagnostic_read_count =
                session->plan->axes[axis_index].operation_mode->final_sdo_read_count;
            for (size_t diagnostic_index = 0U;
                 diagnostic_index <
                 session->plan->axes[axis_index].operation_mode->final_sdo_read_count;
                 ++diagnostic_index) {
                if (emaster_session_observer_read_configured_sdo(
                        &sdo, &session->plan->axes[axis_index]
                                   .operation_mode->final_sdo_reads[diagnostic_index])) {
                    ++axis_result->final_diagnostic_success_count;
                }
            }
        }
    }
    session->report->last_dc_time_ns = session->context.DCtime;
    session->report->restore_init_succeeded = emaster_soem_restore_init(&session->context);
    if (!session->report->restore_init_succeeded &&
        session->report->status == EMASTER_CONTROL_SESSION_OK) {
        session->report->status = EMASTER_CONTROL_SESSION_RESTORE_INIT_FAILED;
        session->report->fault_latched = true;
        emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_FAULTED,
                                       session->report->status);
    }
    if (session->report->status == EMASTER_CONTROL_SESSION_OK &&
        session->report->safe_state_reached)
    {
        emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_STOPPED,
                                       EMASTER_CONTROL_SESSION_OK);
    }
    else if (session->report->status != EMASTER_CONTROL_SESSION_OK)
    {
        emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_FAULTED,
                                       session->report->status);
    }
    /*
     * 各线程调度累计值：停机之后、关总线之前，此时观测线程已经退出、命令线程还在。
     * 这是"同优先级 FIFO 线程互相遮挡"这条假设的直接度量，不在任何实时窗口内。
     */
    capture_thread_schedstat(session->report);
    for (size_t row_index = 0U; row_index < session->report->thread_schedstat_count;
         ++row_index) {
        const emaster_thread_schedstat_t *row = &session->report->thread_schedstat[row_index];

        printf("[SHUTDOWN] 线程 tid=%u policy=%d prio=%d 运行 %.3f s 排队等待 %.3f ms 切换 %"
               PRIu64 " 次\n",
               row->tid, row->policy, row->priority, (double)row->exec_ns / 1e9,
               (double)row->wait_ns / 1e6, row->switches);
    }

    ecx_close(&session->context);
    session->context_open = false;

    /* 销毁实时命令服务器 */
    if (session->command_server != NULL) {
        emaster_command_server_destroy(session->command_server);
        session->command_server = NULL;
    }
}
