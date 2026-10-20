#include "session_internal.h"

#include "emaster/config/runtime_config.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint64_t absolute_position_difference(int32_t left, int32_t right) {
    int64_t difference = (int64_t)left - (int64_t)right;

    return (uint64_t)(difference < 0 ? -difference : difference);
}

/*
 * 慢速通道取不到时的印法（P12.50/P12.52）。
 *
 * 这些量的 0 全都是合法读数——0 A、0 rpm、0x00000000 就是"没有错误"——所以印 0
 * 会把一次邮箱超时伪装成一个读数。而排查"轴不动"时最容易下错的那几个结论，正好
 * 都落在这几个数上。取不到就印 na，客户端把它当"没有"而不是当 0。
 */
static void format_optional_int(char *out, size_t size, bool read, int32_t value)
{
    if (read)
    {
        (void)snprintf(out, size, "%d", (int)value);
    }
    else
    {
        (void)snprintf(out, size, "na");
    }
}

static void format_optional_hex32(char *out, size_t size, bool read, uint32_t value)
{
    if (read)
    {
        (void)snprintf(out, size, "0x%08X", (unsigned)value);
    }
    else
    {
        (void)snprintf(out, size, "na");
    }
}

/*
 * P12.17: 记下"这一轴第一次被判故障"。
 *
 * 这几个字段原先只有 AXIS_ISOLATION 分支才写，而默认策略是全局停机，于是报告里
 * 既读不出这一轮走了哪条臂，也读不出故障发生在哪一拍——P12.17 那份 22.4 s 的报告
 * 里，轴 2/3/5 已经是 CiA402 Fault 而这三个字段全空，只能说"故障过一次"。
 *
 * 刻意**不**置 fault_isolated：那个标志不只用于审计，还被观测路径读成
 * AXIS_FLAG_ISOLATED 并参与"跳过此轴"的判定（session_observation.c）。在全局停机
 * 下置它，会把一次记录变成一次控制行为改变。
 *
 * fault_cycle 装的是 session->exchange，不是 report->cycle_count。二者在正常会话里
 * 相等，失败路径上会分叉，报告里的键名必须说清是哪一个。
 */
static void record_axis_fault(emaster_soem_session_t *session, size_t axis_index,
                              emaster_control_session_status_t fault_reason)
{
    emaster_control_session_axis_result_t *axis = &session->axes[axis_index];

    /* 首因优先：故障之后又判出来的别的理由，不覆盖第一次那一个。 */
    if (axis->fault_recorded)
    {
        return;
    }
    axis->fault_recorded = true;
    axis->axis_status = EMASTER_AXIS_STATUS_FAULTED;
    axis->fault_cycle = session->exchange;
    axis->fault_reason = fault_reason;
    axis->fault_status_word = axis->status_word;
}

/* P2.5: 单轴故障处理 - 隔离故障轴，不影响其他轴 */
static void handle_axis_fault(
    emaster_soem_session_t *session,
    size_t axis_index,
    emaster_control_session_status_t fault_reason)
{
    emaster_control_session_axis_result_t *axis = &session->axes[axis_index];

    record_axis_fault(session, axis_index, fault_reason);
    /* 隔离标志只在这一条分支上置——它会让这一轴被跳过，是控制行为而不只是记录。 */
    axis->fault_isolated = true;

    /* 将该轴控制器目标设为 Quick-Stop */
    emaster_cia402_controller_set_goal(&session->controllers[axis_index],
                                       EMASTER_CIA402_GOAL_QUICK_STOP);

    /* 记录到审计 */
    fprintf(stderr, "[P2.5] 轴%zu 故障隔离: 原因=%d, 周期=%" PRIu64 "\n",
            axis_index, fault_reason, session->exchange);
}

/* P2.5: 检查单轴健康状态 */
static emaster_control_session_status_t check_axis_health(
    emaster_soem_session_t *session,
    size_t axis_index,
    bool *should_isolate)
{
    emaster_control_session_axis_result_t *axis = &session->axes[axis_index];

    *should_isolate = false;

    /* 跳过已隔离的轴 */
    if (axis->fault_isolated) {
        return EMASTER_CONTROL_SESSION_OK;
    }

    /* 检查故障条件 */
    if (session->controller_outputs[axis_index].fault_present) {
        *should_isolate = true;
        return EMASTER_CONTROL_SESSION_DRIVE_FAULT;
    }

    if (!session->controller_outputs[axis_index].state_known) {
        *should_isolate = true;
        return EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
    }

    if (axis->internal_limit_active) {
        *should_isolate = true;
        return EMASTER_CONTROL_SESSION_INTERNAL_LIMIT_ACTIVE;
    }

    return EMASTER_CONTROL_SESSION_OK;
}

/*
 * P8.3: 连续迟到是否已经越线。
 *
 * 驱动器判"这一拍帧来晚了"用的是它自己的 SM 事件丢失计数，攒满 6 次就置 AL 0x001A
 * 掉出 OP。主站手里有同一个信号
 * ——每轴的 sync0_margin_ns 为负就说明本周期输出对驱动器已经迟到——此前却只在统计
 * 里加一，回路照常推进，等于看着驱动器攒到 6。
 *
 * 阈值默认关（enabled=false 或阈值 0），因为连续段能到几拍还没有实测数据：8 臂里
 * sync0_late_count 到过 84 次而一轮都没掉出，说明那些迟到多半是散的。本轮先把
 * sync0_late_max_consecutive 量出来，下一轮再把阈值定在实测最长段与 6 之间。
 * 拿一个没量过的数去中止真实运行，比不管它更糟。
 */
static bool dc_late_threshold_exceeded(emaster_soem_session_t *session)
{
    uint32_t threshold;
    size_t axis_index;

    if (session->error_recovery_policy == NULL)
    {
        return false;
    }
    threshold = session->error_recovery_policy->dc_late_recovery.consecutive_error_threshold;
    if (!session->error_recovery_policy->dc_late_recovery.enabled || threshold == 0U)
    {
        return false;
    }
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        if (session->axes[axis_index].timing.sync0_late_consecutive >= (uint64_t)threshold) {
            session->report->dc_late_threshold_exceeded = true;
            session->report->dc_late_threshold_axis = axis_index;
            return true;
        }
    }
    return false;
}

/* P2.5: 检查恢复进度 */
static void check_recovery_progress(
    emaster_soem_session_t *session,
    size_t axis_index)
{
    emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
    emaster_cia402_output_t *output = &session->controller_outputs[axis_index];

    if (axis->axis_status != EMASTER_AXIS_STATUS_RECOVERING) {
        return;
    }

    /* 检查是否已到达 Switch On Disabled */
    if (output->observed_state == EMASTER_CIA402_STATE_SWITCH_ON_DISABLED &&
        !output->fault_present)
    {
        /* 恢复成功，重新启动轴 */
        axis->axis_status = EMASTER_AXIS_STATUS_NORMAL;
        axis->fault_isolated = false;

        /* 请求上电 */
        emaster_cia402_controller_set_goal(&session->controllers[axis_index],
                                           EMASTER_CIA402_GOAL_OPERATION_ENABLED);

        fprintf(stderr, "[P2.5] 轴%zu 恢复成功，重新启动\n", axis_index);
    }
    else if (session->exchange - axis->fault_cycle > 5000U)
    {
        /* 超时（5秒），恢复失败 */
        fprintf(stderr, "[P2.5] 轴%zu 恢复超时\n", axis_index);
        axis->axis_status = EMASTER_AXIS_STATUS_FAULTED;
    }
}

/*
 * 解析一个无符号整数，base=0 接受 0x 前缀。
 *
 * 用 strtoul 而不是 atoi：atoi 对 "0x2009" 返回 0，对 "12abc" 返回 12，两种都会
 * 让一条打错的命令悄悄读到别的对象。
 */
static bool parse_unsigned_token(const char *text, unsigned long *out_value)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0')
    {
        return false;
    }
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
    {
        return false;
    }
    *out_value = value;
    return true;
}

/*
 * 把观测线程读回的原始字节写成一条响应。
 *
 * 原始字节按 SDO 线上顺序（CANopen 小端）如实列出；1/2/4 字节再给一个有符号、一个
 * 无符号的解释，省得客户端自己拼。更大的对象只给 raw——它的语义只有设备文档知道，
 * 主站不猜。
 */
static void format_sdo_read_response(const emaster_soem_session_t *session,
                                     emaster_command_response_t *response)
{
    char raw[3U * sizeof(session->sdo_read_data) + 1U];
    size_t offset = 0U;
    int size = session->sdo_read_size;
    int index;

    if (size < 0)
    {
        size = 0;
    }
    if (size > (int)sizeof(session->sdo_read_data))
    {
        size = (int)sizeof(session->sdo_read_data);
    }
    for (index = 0; index < size; ++index)
    {
        offset += (size_t)snprintf(raw + offset, sizeof(raw) - offset, "%02X",
                                   (unsigned)session->sdo_read_data[index]);
    }
    if (offset == 0U)
    {
        raw[0] = '\0';
    }

    if (session->sdo_read_wkc <= 0)
    {
        response->success = false;
        (void)snprintf(response->message, sizeof(response->message),
                       "sdo read failed: slave=%u,index=0x%04X,sub=0x%02X,wkc=%d"
                       " (对象不存在/不可读，或邮箱超时)",
                       (unsigned)session->sdo_read_slave,
                       (unsigned)session->sdo_read_index,
                       (unsigned)session->sdo_read_subindex, session->sdo_read_wkc);
        return;
    }

    response->success = true;
    if (size == 1 || size == 2 || size == 4)
    {
        uint64_t unsigned_value = 0U;
        int64_t signed_value;
        const uint64_t sign_bit = UINT64_C(1) << (8 * size - 1);
        const uint64_t mask = (UINT64_C(1) << (8 * size)) - UINT64_C(1);
        int shift;

        for (shift = 0; shift < size; ++shift)
        {
            unsigned_value |= (uint64_t)session->sdo_read_data[shift] << (8 * shift);
        }
        signed_value = (unsigned_value & sign_bit) != 0U
                           ? -(int64_t)((~unsigned_value & mask) + UINT64_C(1))
                           : (int64_t)unsigned_value;
        (void)snprintf(response->message, sizeof(response->message),
                       "slave=%u,index=0x%04X,sub=0x%02X,size=%d,raw=%s,u=%" PRIu64
                       ",i=%" PRId64,
                       (unsigned)session->sdo_read_slave,
                       (unsigned)session->sdo_read_index,
                       (unsigned)session->sdo_read_subindex, size, raw, unsigned_value,
                       signed_value);
        return;
    }
    (void)snprintf(response->message, sizeof(response->message),
                   "slave=%u,index=0x%04X,sub=0x%02X,size=%d,raw=%s",
                   (unsigned)session->sdo_read_slave,
                   (unsigned)session->sdo_read_index,
                   (unsigned)session->sdo_read_subindex, size, raw);
}

/*
 * 回零（G-02）。
 *
 * 回零与 CSP/CSV/CST 的根本差别是：目标不是主站给的。方法（0x6098）、两段速度
 * （0x6099）与开关都在驱动器自己手里，主站只做三件事——把驱动器带进该模式、
 * 在 Operation enabled 下置控制字 bit4、然后读状态字。所以这里没有轨迹，只有
 * 两个坐标（启动交换号、完成交换号）和两条判据。
 *
 * 判据只用 6041 的两位：bit12 = homing attained，bit13 = homing error。这是
 * CiA 402 对所有回零方法共同的两位，不依赖厂商码。bit10（target reached）不进来：
 * 它说的是"上一个周期目标到了"，回零没有周期目标，部分驱动器在这两位之前就把它
 * 置真，拿它判完成会提前放手。
 *
 * 为什么必须有完成时限：bit12 不来的时候，驱动器既没故障也没掉出使能，会话会
 * 一直"正常运行"地等下去，而轴一直在找参考点。时限由运行方案逐模式给，配置层
 * 拒绝没给的已选回零模式。
 *
 * 启动位只在"全部轴已使能、模式已确认、本拍没有别的安全否决"时置。bit4 的上升沿
 * 才启动；驱动器报完成或报错之后立刻清零（下一次回零还要用这个上升沿）。
 */
static void homing_step(emaster_soem_session_t *session, bool all_axes_enabled,
                        bool all_modes_confirmed,
                        emaster_control_session_status_t *safety_status,
                        bool *safety_denied)
{
    enum
    {
        /* mode_specific_status 的低两位就是 6041 的 bit12 / bit13。 */
        HOMING_STATUS_ATTAINED = 0x01U,
        HOMING_STATUS_ERROR = 0x02U
    };
    const uint64_t cycle_ns = (uint64_t)session->plan->cycle_ns;
    bool any_configured = false;
    bool all_attained = true;
    bool failed = false;
    emaster_control_session_status_t failure_status = EMASTER_CONTROL_SESSION_OK;
    size_t axis_index;

    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const emaster_operation_mode_t *mode =
            session->plan->axes[axis_index].operation_mode;
        emaster_control_session_axis_result_t *axis = &session->axes[axis_index];
        bool attained;
        bool error;

        /* 回零由模式的 quantity 声明，不按模式号猜。 */
        if (mode == NULL || mode->quantity != EMASTER_MODE_QUANTITY_NONE) {
            continue;
        }
        any_configured = true;

        attained = (axis->mode_specific_status & HOMING_STATUS_ATTAINED) != 0U;
        error = (axis->mode_specific_status & HOMING_STATUS_ERROR) != 0U;

        if (error) {
            axis->homing_error = true;
            if (!axis->homing_attained) {
                all_attained = false;
            }
            failed = true;
            failure_status = EMASTER_CONTROL_SESSION_HOMING_FAILED;
            emaster_cia402_controller_set_homing_start(
                &session->controllers[axis_index], false);
            continue;
        }
        if (!attained) {
            all_attained = false;
        } else if (!axis->homing_attained) {
            /* 首次为准：报告要的是"哪一拍到的"，不是最后一次读到的那一拍。 */
            axis->homing_attained = true;
            axis->homing_attained_exchange = session->exchange;
        }

        {
            const bool start = all_axes_enabled && all_modes_confirmed &&
                               !*safety_denied && !attained;

            emaster_cia402_controller_set_homing_start(
                &session->controllers[axis_index], start);
            if (start && !axis->homing_started) {
                axis->homing_started = true;
                axis->homing_start_exchange = session->exchange;
            }
            /*
             * 超时从第一次真正发出 bit4 的那一拍算起。时限按配置的毫秒数，
             * 用交换号乘周期折算——周期线程里没有别的时基比它更贴近驱动器
             * 实际经历的时间。
             */
            if (start && mode->completion_timeout_ms > 0U &&
                session->exchange >= axis->homing_start_exchange) {
                const uint64_t elapsed_ms =
                    (session->exchange - axis->homing_start_exchange) * cycle_ns /
                    UINT64_C(1000000);
                if (elapsed_ms >= (uint64_t)mode->completion_timeout_ms) {
                    failed = true;
                    /* 驱动器自己报了错比"等太久"更有信息量，不覆盖它。 */
                    if (failure_status != EMASTER_CONTROL_SESSION_HOMING_FAILED) {
                        failure_status = EMASTER_CONTROL_SESSION_HOMING_TIMEOUT;
                    }
                }
            }
        }
    }

    if (!any_configured) {
        return;
    }
    if (failed) {
        session->report->homing_failed = true;
        emaster_soem_session_note_runtime_failure(session, failure_status, safety_status);
        *safety_denied = true;
    }
    if (all_attained && !failed) {
        session->report->homing_completed = true;
    }
}

emaster_control_session_status_t emaster_soem_session_run(emaster_soem_session_t *session) {
    size_t axis_index;
    bool motion_initialized = false;
    bool enable_started = false;
    uint64_t mode_confirmation_deadline_cycle;
    uint64_t enable_deadline_cycle = 0U;
    emaster_control_session_status_t status = EMASTER_CONTROL_SESSION_OK;
    emaster_control_session_status_t safety_status = EMASTER_CONTROL_SESSION_OK;

    emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_ENABLING,
                                   EMASTER_CONTROL_SESSION_OK);
    /*
     * 回零方案在计划里就定了。先把"本部署有没有回零轴、是哪几轴、时限多少"写进
     * 报告：即使会话在启动阶段就失败，一份回零配置也不该被读成没有回零。运行期
     * 的启动/完成/报错仍由 homing_step 逐拍更新。
     */
    session->report->homing_axis_count = 0U;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const emaster_operation_mode_t *mode =
            session->plan->axes[axis_index].operation_mode;
        if (mode != NULL && mode->quantity == EMASTER_MODE_QUANTITY_NONE) {
            session->axes[axis_index].homing_configured = true;
            session->axes[axis_index].homing_timeout_ms = mode->completion_timeout_ms;
            ++session->report->homing_axis_count;
        }
    }
    session->report->homing_configured = session->report->homing_axis_count > 0U;

    mode_confirmation_deadline_cycle =
        session->report->cycle_count + session->transition_cycles;
    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
        const ec_slavet *slave = &session->context.slavelist[
            emaster_soem_session_axis_slave(session, axis_index)];
        int8_t mode_display;
        uint16_t status_word;
        int32_t actual_position;

        if (!emaster_cia_process_image_decode_input(&session->images[axis_index], slave->inputs,
                                                    slave->Ibytes, &mode_display, &status_word,
                                                    &actual_position) ||
            !emaster_cia_process_image_audit_input(
                &session->images[axis_index], &session->report->audit,
                EMASTER_AUDIT_PHASE_OPERATION_REQUEST, (uint16_t)(axis_index + 1U),
                session->exchange)) {
            status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
            return status;
        }
    }
    {
        /*
         * 循环顶部超时被恢复时，本周期不做协调器推进（deadline_ns 已失效）。
         * 这个计数给"持续超时导致每周期都跳过"设上限：否则会话既不推进也不
         * 终止，比直接停机更糟。任何一个正常推进的周期都会把它清零。
         */
        uint32_t consecutive_deadline_skips = 0U;
        uint32_t deadline_skip_limit = 0U;
        if (session->error_recovery_policy != NULL) {
            deadline_skip_limit =
                session->error_recovery_policy->deadline_recovery.consecutive_error_threshold;
        }
        while (true) {
            emaster_multiaxis_frame_t frame;
            uint64_t now_ns;
            uint64_t deadline_ns;
            bool all_axes_enabled = true;
            bool all_modes_confirmed = true;
            bool feedback_valid = true;
            bool safety_denied = false;

            status = emaster_soem_session_exchange(session, EMASTER_AUDIT_PHASE_CYCLIC_OPERATION);
            if (status != EMASTER_CONTROL_SESSION_OK) {
                return emaster_soem_session_publish_feedback(session, status);
            }
            /*
             * P8.3: 连续迟到越线就主动停，不等驱动器自己攒满 SM 事件丢失掉出 OP。
             * 放在交换之后——每轴的裕量是本次交换里刚解出来的。阈值默认关，此时
             * 这里只是每轴一次整数比较。
             */
            if (dc_late_threshold_exceeded(session)) {
                status = EMASTER_CONTROL_SESSION_DC_SYNC_FAILED;
                emaster_soem_session_latch_failure(session, status);
                return emaster_soem_session_publish_feedback(session, status);
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                const ec_slavet *slave = &session->context.slavelist[
                    emaster_soem_session_axis_slave(session, axis_index)];
                int8_t mode_display;
                uint16_t status_word;
                int32_t actual_position;

                session->axes[axis_index].input_decoded = emaster_cia_process_image_decode_input(
                    &session->images[axis_index], slave->inputs, slave->Ibytes, &mode_display,
                    &status_word, &actual_position);
                if (!session->axes[axis_index].input_decoded) {
                    feedback_valid = false;
                }
                if (!session->axes[axis_index].input_decoded)
                {
                    continue;
                }
                if (!emaster_cia_process_image_audit_input(
                        &session->images[axis_index], &session->report->audit,
                        EMASTER_AUDIT_PHASE_CYCLIC_OPERATION, (uint16_t)(axis_index + 1U),
                        session->exchange)) {
                    status = EMASTER_CONTROL_SESSION_AUDIT_FAILED;
                    return status;
                }
                session->axes[axis_index].mode_display = mode_display;
                session->axes[axis_index].status_word = status_word;
                /*
                 * C1：把 PDO 里实际有的反馈都填上，不再按模式二选一。模式 10 下
                 * decode_input 只解力矩，位置会永远是 0——面板、报告、跟随误差看到
                 * 的位置全是假的，也就看不见轴到底动没动。三路各按字段在不在填，
                 * 缺哪路就不动哪路（保留上一次的值，而不是写 0 冒充读数）。
                 */
                {
                    emaster_control_session_axis_result_t *axis_result =
                        &session->axes[axis_index];
                    int32_t feedback_value;

                    if (emaster_cia_process_image_feedback(
                            &session->images[axis_index], EMASTER_CIA_FEEDBACK_POSITION,
                            &feedback_value))
                        axis_result->actual_position = feedback_value;
                    if (emaster_cia_process_image_feedback(
                            &session->images[axis_index], EMASTER_CIA_FEEDBACK_VELOCITY,
                            &feedback_value))
                        axis_result->actual_velocity = feedback_value;
                    if (emaster_cia_process_image_feedback(
                            &session->images[axis_index], EMASTER_CIA_FEEDBACK_TORQUE,
                            &feedback_value))
                        axis_result->actual_torque = (int16_t)feedback_value;
                }

                /*
                 * P4.3 的慢速遥测（6078h 电流、6079h 母线电压、200Bh 温度、200Bh:08/09
                 * 速度）原先在这里逐轴加锁抄一遍。整块已删除：那把 observer_mutex 没有
                 * 继承优先级，而持锁的观测线程会在锁内做 fprintf 和邮箱往返，周期线程
                 * 等它就成了实时路径上唯一一处**无界**等待。
                 *
                 * 慢速量现在住在 seqlock 快照里，读到它的地方有两处，都不是本循环：
                 * 处理 status 命令时按需读一次，以及停机时 join 之后的 flush_slow 回填
                 * 报告字段。两者都在下面对应的位置，理由写在那里。
                 */
                session->status_words[axis_index] = status_word;
                {
                    emaster_cia402_status_t decoded_status;

                    (void)emaster_cia402_decode_status(status_word, &decoded_status);
                    session->axes[axis_index].status_warning = decoded_status.warning;
                    session->axes[axis_index].voltage_enabled = decoded_status.voltage_enabled;
                    session->axes[axis_index].remote = decoded_status.remote;
                    session->axes[axis_index].target_reached = decoded_status.target_reached;
                    session->axes[axis_index].internal_limit_active =
                        decoded_status.internal_limit_active;
                    session->axes[axis_index].mode_specific_status =
                        decoded_status.mode_specific_bits;
                    session->axes[axis_index].manufacturer_specific_status =
                        decoded_status.manufacturer_specific_bits;
                }
                /*
                 * 刷新条件必须与 control_session.c 里的分配条件同一句。此前这里只
                 * 认运动方案路径，于是外部目标路径上的 actual_positions 从会话建立
                 * 那一刻起再没动过——而外部目标路径现在要拿它算"目标离实际多远"，
                 * 读一个冻结的起点值会让每一条正常目标都被判成超距。外部目标路径的
                 * 目标位置一直是对的（session_target.c 提交时写 target_positions），
                 * 只有实际位置这一半没跟上。
                 */
                if (session->plan->motion_profile != NULL ||
                    session->position_target_source != NULL) {
                    session->actual_positions[axis_index] = actual_position;
                }
            }

            if (!emaster_cycle_clock_sample(&session->clock, &now_ns, &deadline_ns)) {
                session->report->cycle_deadline_missed |= session->clock.deadline_missed;
                if (session->clock.deadline_missed) {
                    /*
                     * 与交换函数末尾那次检查是同一个条件、同一个 deadline，只是晚
                     * 了几十微秒（本次输入解码与审计的开销）。因此必须与那里同样
                     * 处理：先记账，再交给容错策略。原先直接返回会让同一次超时因
                     * 检出位置不同而结局相反——交换函数内检出按策略可恢复，这里却
                     * 直接终止会话，且不留现场（deadline_missed_count 停在 0，与
                     * cycle_deadline_missed=true 自相矛盾）。
                     */
                    emaster_soem_session_note_deadline_missed(session);
                    if (session->report->first_deadline_missed_exchange == 0U)
                    {
                        /* 第三个记账点（另两处在交换函数内）。本次交换已完成，
                         * 周期号就是 session->exchange；不记就会只剩计数。 */
                        session->report->first_deadline_missed_exchange = session->exchange;
                    }
                    if (consecutive_deadline_skips < deadline_skip_limit &&
                        emaster_soem_session_try_deadline_recovery(session)) {
                        /* 本周期不再推进协调器：deadline_ns 已经过期，喂给协调器会
                         * 被判为整帧逾期并清空输出。重新对齐后回到循环顶部，下一
                         * 周期按新边界发包。 */
                        ++consecutive_deadline_skips;
                        continue;
                    }
                    emaster_soem_session_latch_failure(
                        session, EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED);
                }
                return emaster_soem_session_publish_feedback(
                    session, session->clock.deadline_missed
                                 ? EMASTER_CONTROL_SESSION_CYCLE_DEADLINE_MISSED
                                 : EMASTER_CONTROL_SESSION_CYCLE_WAIT_FAILED);
            }
            consecutive_deadline_skips = 0U;
            /*
             * 观测发布点。位置是刻意选的，四件事都要成立：
             *
             * - 在轴解码循环之后：axes[] 里是本拍刚解出来的反馈；
             * - 在时钟采样之后：与协调器共用同一时基，now_ns/deadline_ns 是这一拍的；
             * - 在本拍截止时间检查之后：只有成功走完交换的拍才发帧，跳拍的 continue
             *   在上面，消费者因此从 cycle 缺口里看到真实的断点，而不是被填平；
             * - 在协调器/安全/输出块之前：不落在周期尾部那段截止关键的路径上，
             *   也不被周期里最重的部分拖累。
             *
             * 开关关闭时这是一次指针判空。见 session_observation.c。
             */
            emaster_soem_session_observation_publish(session, now_ns, deadline_ns);
            frame.sequence = session->report->cycle_count;
            frame.deadline_ns = deadline_ns;
            frame.axis_count = session->plan->axis_count;
            frame.status_words = session->status_words;
            frame.outputs = session->controller_outputs;
            /* 判定先落盘再判断：故障现场要能区分"协调器拒绝整帧"和"其后才失败" */
            session->last_coordinator_status =
                (int)emaster_multiaxis_coordinator_step(&session->coordinator, &frame, now_ns);
            if (session->last_coordinator_status != (int)EMASTER_MULTIAXIS_OK) {
                status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
                emaster_soem_session_latch_failure(session, status);
                return emaster_soem_session_publish_feedback(session, status);
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                emaster_control_session_axis_result_t *axis_result = &session->axes[axis_index];
                bool operation_enabled = session->controller_outputs[axis_index].observed_state ==
                                         EMASTER_CIA402_STATE_OPERATION_ENABLED;
                bool mode_control_ready;

                axis_result->control_word = session->controller_outputs[axis_index].control_word;
                axis_result->cia402_state = session->controller_outputs[axis_index].observed_state;
                axis_result->operation_enabled_seen |= operation_enabled;
                axis_result->switch_on_disabled_seen |=
                    session->controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_SWITCH_ON_DISABLED;
                axis_result->ready_to_switch_on_seen |=
                    session->controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_READY_TO_SWITCH_ON;
                axis_result->switched_on_seen |=
                    session->controller_outputs[axis_index].observed_state ==
                    EMASTER_CIA402_STATE_SWITCHED_ON;
                /*
                 * P12.17: 每轴各自"第一次出现这个信号"的交换号与当时状态字。
                 *
                 * 放在这里是因为它是逐轴、每拍、且发生在本次解码之后——报告要问的
                 * "轴 3 是从第几拍开始不在 Operation Enabled 的"，只有这个位置答得
                 * 出来。现场环答不出：它是每会话一条记录、只留最后 64 拍，而且入环
                 * 点在交换函数里、比这里早，环里的状态字还是上一拍的。
                 *
                 * 每项都是**首次**为准（_seen 一旦为真就不再改写），因为"哪一拍开始
                 * 坏的"要的是起点，不是最后一次。
                 */
                if (axis_result->operation_enabled_seen && !operation_enabled &&
                    !axis_result->enable_lost_seen) {
                    /* 以"已经使能过一次"为前提：使能阶段本来就不在 Operation
                     * Enabled，不设这个前提的话每根轴的第 0 拍都会命中。 */
                    axis_result->enable_lost_seen = true;
                    axis_result->enable_lost_exchange = session->exchange;
                    axis_result->enable_lost_status_word = axis_result->status_word;
                }
                if (session->controller_outputs[axis_index].fault_present &&
                    !axis_result->fault_bit_seen) {
                    axis_result->fault_bit_seen = true;
                    axis_result->fault_bit_exchange = session->exchange;
                    axis_result->fault_bit_status_word = axis_result->status_word;
                }
                if (axis_result->status_warning && !axis_result->warning_bit_seen) {
                    axis_result->warning_bit_seen = true;
                    axis_result->warning_bit_exchange = session->exchange;
                    axis_result->warning_bit_status_word = axis_result->status_word;
                }
                if (axis_result->internal_limit_active && !axis_result->internal_limit_seen) {
                    axis_result->internal_limit_seen = true;
                    axis_result->internal_limit_exchange = session->exchange;
                    axis_result->internal_limit_status_word = axis_result->status_word;
                }
                if (!session->images[axis_index].tx_mode_available &&
                    axis_result->mode_display_sdo_read) {
                    axis_result->mode_display = axis_result->mode_display_sdo;
                }
                axis_result->mode_display_match =
                    session->images[axis_index].tx_mode_available
                        ? axis_result->mode_display ==
                              session->plan->axes[axis_index].operation_mode->value
                        : axis_result->mode_display_sdo_read &&
                              axis_result->mode_display ==
                                  session->plan->axes[axis_index].operation_mode->value;
                mode_control_ready = axis_result->mode_display_match;
                if (!session->images[axis_index].tx_mode_available)
                {
                    /* 固定 PDO 无法提供周期 6061，只能以 SAFE-OP 的 6060 写后读回作为使能前证据 */
                    mode_control_ready = axis_result->mode_command_sdo_read &&
                        axis_result->mode_command_sdo ==
                            session->plan->axes[axis_index].operation_mode->value;
                }
                if (!emaster_session_axis_mode_allows_control(&session->plan->axes[axis_index],
                                                              mode_control_ready)) {
                    all_modes_confirmed = false;
                }

                /* P2.5: 故障检测和处理 - 根据策略选择行为 */
                bool should_isolate = false;
                emaster_control_session_status_t axis_health_status =
                    check_axis_health(session, axis_index, &should_isolate);

                if (should_isolate) {
                    /* 根据部署配置的故障策略处理 */
                    if (session->plan->fault_policy == EMASTER_FAULT_POLICY_AXIS_ISOLATION) {
                        /* 单轴隔离：故障轴进入 Quick-Stop，其他轴继续运行 */
                        handle_axis_fault(session, axis_index, axis_health_status);

                        /* 第一次故障记录到全局状态（审计用） */
                        if (safety_status == EMASTER_CONTROL_SESSION_OK) {
                            safety_status = axis_health_status;
                        }

                        /* 跳过此轴后续处理，继续其他轴 */
                        continue;
                    } else {
                        /* 全局停止（默认）：任一轴故障 → 所有轴停止 */
                        /*
                         * P12.17: 也要落盘。这条路不隔离任何轴（不置 fault_isolated），
                         * 但"哪根轴、第几拍、什么原因、当时状态字是多少"必须留下——
                         * 否则报告里只剩一个 first_runtime_failure，读不出是几号轴。
                         */
                        record_axis_fault(session, axis_index, axis_health_status);
                        emaster_soem_session_note_runtime_failure(session, axis_health_status, &safety_status);
                        safety_denied = true;
                        /* 继续检查其他轴，收集所有故障信息 */
                    }
                }

                /* 已隔离的轴：只在 AXIS_ISOLATION 策略下处理恢复 */
                if (session->axes[axis_index].fault_isolated) {
                    if (session->plan->fault_policy == EMASTER_FAULT_POLICY_AXIS_ISOLATION) {
                        /* 检查恢复进度 */
                        check_recovery_progress(session, axis_index);
                    }
                    continue;
                }

                if (!operation_enabled) {
                    all_axes_enabled = false;
                }
                else if (!axis_result->voltage_enabled)
                {
                    emaster_soem_session_note_runtime_failure(session, EMASTER_CONTROL_SESSION_CONTROLLER_FAILED,
                                         &safety_status);
                    safety_denied = true;
                }
            }
            session->report->all_axes_enabled_reached |= all_axes_enabled;
            if (all_modes_confirmed && !enable_started)
            {
                enable_started = true;
                enable_deadline_cycle =
                    session->report->cycle_count + session->transition_cycles;
            }
            if (enable_started && !all_axes_enabled &&
                session->report->cycle_count >= enable_deadline_cycle) {
                status = EMASTER_CONTROL_SESSION_CONTROLLER_FAILED;
                emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                safety_denied = true;
            }
            if (!all_modes_confirmed &&
                session->report->cycle_count >= mode_confirmation_deadline_cycle) {
                /*
                 * 说原因之前先问驱动器自己报了什么。
                 *
                 * 原先这里一律写"过程数据反馈解码失败"。可模式回显立不住，往往不是
                 * 主站解码错了，而是驱动器先坏了、不再回模式。装的人顺着"解码失败"
                 * 去查过程数据映像和线序，真原因一个字也看不见。台架实测就是这样：
                 * 驱动器过速后 603F=0x0100，6061 回 0，报告里写的是解码失败。
                 *
                 * 0x603F 是标准 CiA 402 错误码，标准状态通道（0x1A00 第 5 条）里就
                 * 有它，逐拍随 TxPDO 回来，不引入厂商语义。布局里没这一条就查不到，
                 * 判据自动退化成原来的说法。
                 */
                status = EMASTER_CONTROL_SESSION_FEEDBACK_INVALID;
                for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                    uint16_t drive_error_code;

                    if (emaster_cia_process_image_drive_error_code(
                            &session->images[axis_index], &drive_error_code) &&
                        drive_error_code != 0U) {
                        status = EMASTER_CONTROL_SESSION_DRIVE_FAULT;
                        break;
                    }
                }
                emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                safety_denied = true;
            }
            /*
             * 回零：使能与模式确认之后启动控制字 bit4，并核对 bit12/bit13。
             * 放在运动块之前——回零轴没有运动方案，下面的块本来就不会进。
             * 无论本拍是否已被别的失败否决都要调用：它负责把启动位清零。
             */
            homing_step(session, all_axes_enabled, all_modes_confirmed, &safety_status,
                        &safety_denied);
            if (feedback_valid && !safety_denied &&
                (session->plan->motion_profile != NULL ||
                 session->position_target_source != NULL) &&
                all_axes_enabled && all_modes_confirmed) {
                if (session->position_target_source != NULL)
                {
                    bool command_updated = false;

                    /*
                     * 这里原先还有一道"上一周期目标的跟随误差"预检，已删。
                     *
                     * 它读的是每轴的 axes[i].max_following_error_counts，而那个值只由
                     * 运动方案路径写入（session_start.c 里两处赋值都在
                     * if (motion_profile != NULL) 之内）。外部目标路径没有运动方案，
                     * 该值恒为 0，循环体第一句就 continue——这是一道从来没跳过的闸。
                     * 删掉它不改变任何行为，只移走一个以后会被误当成保护的东西。
                     *
                     * "轴跟没跟上"改由驱动器自己的跟随误差窗管（6065/6066）。这两项在
                     * 设置期读回来写进报告，让"驱动器到底有没有在守"成为出厂可核对的
                     * 事实，而不是主站再猜一遍。主站在这条路径上只判一件事：目标离
                     * 实际位置太远就拒收（session_target.c）。
                     */
                    status = emaster_soem_session_position_target_step(
                        session, &command_updated);
                    if (status == EMASTER_CONTROL_SESSION_TARGET_REJECTED)
                    {
                        /*
                         * 拒收不是失败：这一条目标没被采纳，上一目标继续，其余轴照常。
                         * 轴号、偏移量与次数已由 session_target.c 记进报告。这里把状态
                         * 收回 OK——否则它会以故障的身份走完下面全部判据，而那正是
                         * "一条坏目标打死整台机器人"的老路。
                         */
                        status = EMASTER_CONTROL_SESSION_OK;
                    }
                    else if (status != EMASTER_CONTROL_SESSION_OK)
                    {
                        emaster_soem_session_note_runtime_failure(
                            session, status, &safety_status);
                        safety_denied = true;
                    }
                    else if (command_updated && !motion_initialized)
                    {
                        motion_initialized = true;
                        session->report->motion_started = true;
                    }
                }
                else
                {
                emaster_relative_motion_status_t motion_status;

                if (!motion_initialized) {
                    if (!session->motion_prepared) {
                        status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                        return status;
                    }
                    motion_initialized = true;
                    session->report->motion_started = true;
                }
                if (session->plan->motion_profile->trajectory ==
                    EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION)
                {
                    motion_status = emaster_relative_motion_step(
                        &session->motion, session->actual_positions, session->target_positions,
                        session->plan->axis_count);
                }
                else if (session->plan->motion_profile->trajectory ==
                         EMASTER_MOTION_TRAJECTORY_CONSTANT_VELOCITY)
                {
                    motion_status = emaster_velocity_motion_step(
                        &session->velocity_motion, session->actual_positions,
                        session->target_positions, session->plan->axis_count);
                }
                else
                {
                    motion_status = EMASTER_RELATIVE_MOTION_INVALID_ARGUMENT;
                }
                /* 即使本周期因跟随误差退出，也要把触发值保留到会话报告。 */
                for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                    session->axes[axis_index].max_observed_following_error_counts =
                        session->motion_axes[axis_index].max_observed_following_error_counts;
                }
                if (motion_status == EMASTER_RELATIVE_MOTION_FOLLOWING_ERROR) {
                    status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                    emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                    safety_denied = true;
                }
                else if (motion_status != EMASTER_RELATIVE_MOTION_ACTIVE &&
                         motion_status != EMASTER_RELATIVE_MOTION_SETTLING &&
                         motion_status != EMASTER_RELATIVE_MOTION_COMPLETE) {
                    status = EMASTER_CONTROL_SESSION_MOTION_INVALID;
                    emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                    safety_denied = true;
                }
                if (!safety_denied)
                {
                    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                        if (!emaster_soem_axis_set_target_value(
                                &session->plan->axes[axis_index], &session->axes[axis_index],
                                session->target_positions[axis_index]))
                            safety_denied = true;
                    }
                    session->report->motion_completed =
                        motion_status == EMASTER_RELATIVE_MOTION_COMPLETE;
                }
                if (session->report->motion_completed &&
                    session->plan->motion_profile->trajectory ==
                        EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION) {
                    for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                        emaster_control_session_axis_result_t *axis_result =
                            &session->axes[axis_index];
                        const int32_t planned_angle =
                            session->motion_axis_configs[axis_index]->relative_angle_millidegrees;

                        axis_result->motion_completion_actual_position =
                            axis_result->actual_position;
                        axis_result->motion_actual_delta_counts =
                            (int64_t)axis_result->motion_completion_actual_position -
                            (int64_t)axis_result->initial_actual_position;
                        axis_result->motion_final_error_counts = absolute_position_difference(
                            axis_result->motion_completion_actual_position,
                            axis_result->motion_final_position);
                        axis_result->motion_direction_match =
                            planned_angle == 0
                                ? axis_result->motion_actual_delta_counts == 0
                                : (planned_angle > 0 &&
                                   axis_result->motion_actual_delta_counts > 0) ||
                                      (planned_angle < 0 &&
                                       axis_result->motion_actual_delta_counts < 0);
                        if (!axis_result->motion_direction_match ||
                            axis_result->motion_final_error_counts >
                                axis_result->max_following_error_counts) {
                            status = EMASTER_CONTROL_SESSION_FOLLOWING_ERROR;
                            emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                            safety_denied = true;
                        }
                    }
                    if (safety_denied)
                    {
                        session->report->motion_completed = false;
                    }
                }
                }
            }

            /* P2.5: 只在轴级隔离策略下检查是否所有轴都故障 */
            if (session->plan->fault_policy == EMASTER_FAULT_POLICY_AXIS_ISOLATION)
            {
                size_t healthy_count = 0;
                for (axis_index = 0; axis_index < session->plan->axis_count; ++axis_index) {
                    if (!session->axes[axis_index].fault_isolated) {
                        healthy_count++;
                    }
                }

                if (healthy_count == 0) {
                    /* 所有轴都故障了，停止整个会话 */
                    status = EMASTER_CONTROL_SESSION_ALL_AXES_FAULTED;
                    emaster_soem_session_note_runtime_failure(session, status, &safety_status);
                    safety_denied = true;
                }
            }

            {
                safety_status = emaster_soem_session_apply_safety(
                    session, feedback_valid, all_modes_confirmed, safety_status,
                    &safety_denied);
            }
            for (axis_index = 0U; axis_index < session->plan->axis_count; ++axis_index) {
                emaster_control_session_axis_result_t *axis_result = &session->axes[axis_index];

                if (!emaster_cia_process_image_update_output(
                        &session->plan->axes[axis_index], &session->images[axis_index],
                        session->controller_outputs[axis_index].control_word,
                        emaster_soem_axis_target_value(&session->plan->axes[axis_index],
                                                       axis_result),
                        session->context.slavelist[
                            emaster_soem_session_axis_slave(session, axis_index)].outputs,
                        session->context.slavelist[
                            emaster_soem_session_axis_slave(session, axis_index)].Obytes)) {
                    status = EMASTER_CONTROL_SESSION_PROCESS_MAP_FAILED;
                    return status;
                }
            }
            if (!safety_denied && all_axes_enabled && all_modes_confirmed)
            {
                emaster_soem_session_set_state(session, EMASTER_CONTROL_STATE_RUNNING,
                                               EMASTER_CONTROL_SESSION_OK);
            }
            (void)emaster_soem_session_publish_feedback(session, safety_status);

            /* 处理实时命令（非阻塞） */
            if (session->command_server != NULL) {
                emaster_command_t command;

                /*
                 * 未决的 sdo_read：结果由观测线程填、这里只取。两条路径都在周期
                 * 线程上完成响应，客户端拿到的是一个与命令一一对应的答复。
                 *
                 * 超时只报一次，但**不**清掉代次：观测线程可能正在那次邮箱往返里，
                 * 提前清掉会让它的结果错配到下一次请求。未决期间上面不取新命令，
                 * 队列顶住；观测线程一轮 50 ms、单次读超时 30 ms，正常情况下
                 * 5 s 这个上限碰不到。
                 */
                if (session->sdo_read_pending) {
                    const uint32_t completed = atomic_load_explicit(
                        &session->sdo_read_completed_generation, memory_order_acquire);

                    if (completed == session->sdo_read_pending_generation) {
                        if (!session->sdo_read_timed_out) {
                            emaster_command_response_t response;

                            format_sdo_read_response(session, &response);
                            (void)emaster_command_server_respond(session->command_server,
                                                                 &response);
                        }
                        session->sdo_read_pending = false;
                    }
                    else if (!session->sdo_read_timed_out &&
                             now_ns >= session->sdo_read_deadline_ns) {
                        emaster_command_response_t response;

                        response.success = false;
                        (void)snprintf(response.message, sizeof(response.message),
                                       "sdo read timeout: slave=%u,index=0x%04X,sub=0x%02X",
                                       (unsigned)session->sdo_read_slave,
                                       (unsigned)session->sdo_read_index,
                                       (unsigned)session->sdo_read_subindex);
                        (void)emaster_command_server_respond(session->command_server,
                                                             &response);
                        session->sdo_read_timed_out = true;
                    }
                    /*
                     * 观测线程已不在：完成代次不会再来。释放交接，否则命令口从此
                     * 只收不发。这里只可能发生在停机路径上。
                     */
                    if (session->sdo_read_pending &&
                        (!session->observer_thread_created || !session->observer_running)) {
                        session->sdo_read_pending = false;
                    }
                }
                if (!session->sdo_read_pending &&
                    emaster_command_server_receive(session->command_server, &command)) {
                    emaster_command_response_t response;
                    response.success = false;

                    switch (command.type) {
                        case EMASTER_COMMAND_QUERY_STATUS: {
                            /*
                             * 状态回读只报告本周期已解码的 PDO 内容，不回读 SDO：
                             * 命令在周期线程处理，任何邮箱访问都会挤占周期预算。
                             * planned 用于区分「主站写入的目标」和「PDO 实际目标」，
                             * 两者不一致说明输出映射有问题而不是驱动器不动。
                             *
                             * 唯一一处非 PDO 的量是 err=0x%04x，它来自慢速快照：读是
                             * seqlock，不取锁、不阻塞、有界重试两次，而且只在真的有命令
                             * 要回答时发生——原先是每拍每轴各取放一次锁。
                             */
                            emaster_observation_slow_state_t slow;
                            bool slow_ok = emaster_observation_slow_read(
                                &session->observation_slow, &slow);
                            /*
                             * 外部目标的年龄。缓冲区一条记录带一个时间戳，所以它是
                             * 会话级量，不逐轴重复。没有收到过目标时印 na，不印 0
                             * —— 0 ms 会被读成"刚刚收到"。
                             */
                            char target_age_text[24];
                            bool target_age_valid = false;
                            uint64_t target_age_ms = 0U;
                            if (session->external_target_buffer != NULL)
                            {
                                uint64_t last_update_ns;
                                pthread_mutex_lock(&session->external_target_buffer->mutex);
                                last_update_ns = session->external_target_buffer->last_update_ns;
                                pthread_mutex_unlock(&session->external_target_buffer->mutex);
                                struct timespec age_ts;
                                if (last_update_ns != 0U &&
                                    clock_gettime(CLOCK_MONOTONIC, &age_ts) == 0)
                                {
                                    uint64_t age_now_ns = (uint64_t)age_ts.tv_sec *
                                                              UINT64_C(1000000000) +
                                                          (uint64_t)age_ts.tv_nsec;
                                    if (age_now_ns >= last_update_ns)
                                    {
                                        target_age_ms =
                                            (age_now_ns - last_update_ns) / UINT64_C(1000000);
                                        target_age_valid = true;
                                    }
                                }
                            }
                            if (target_age_valid)
                            {
                                (void)snprintf(target_age_text, sizeof(target_age_text),
                                               "%llu", (unsigned long long)target_age_ms);
                            }
                            else
                            {
                                (void)snprintf(target_age_text, sizeof(target_age_text), "na");
                            }
                            /*
                             * 最后一次拒收的现场。没有拒收过就印 na，不印轴 0、
                             * 偏移 0——那看起来像一次真实的拒收。轴号按人读惯例从 1 起，
                             * 与逐轴字段 a1、a2 对齐；报告里的同名键仍是 0 起。
                             */
                            char reject_axis_text[24];
                            char reject_offset_text[24];
                            if (session->report->target_rejected_count == 0U)
                            {
                                (void)snprintf(reject_axis_text, sizeof(reject_axis_text), "na");
                                (void)snprintf(reject_offset_text,
                                               sizeof(reject_offset_text), "na");
                            }
                            else
                            {
                                (void)snprintf(reject_axis_text, sizeof(reject_axis_text),
                                               "%zu",
                                               session->report->target_rejection_axis + 1U);
                                (void)snprintf(reject_offset_text,
                                               sizeof(reject_offset_text), "%llu",
                                               (unsigned long long)
                                                   session->report
                                                       ->target_rejection_offset_counts);
                            }
                            /*
                             * 会话级计数一并给出去（P12.52）：此前它们只在报告里，
                             * 也就是只有跑完才看得到，而"这一轮现在健康吗"是运行中
                             * 就要回答的问题。全是累加计数，读它们不改任何状态。
                             */
                            int offset = snprintf(response.message, sizeof(response.message),
                                "state=%d cycle=%lu axes=%zu enabled=%d completed=%d"
                                ",wkc=%d,wkc_expect=%u,wkc_err=%llu,no_frame=%llu"
                                ",over_budget=%llu,deadline_missed=%d"
                                ",rejected=%llu,tgt_age_ms=%s,rej_axis=%s,rej_offset=%s",
                                (int)session->report->state,
                                (unsigned long)session->report->cycle_count,
                                session->plan->axis_count,
                                session->report->all_axes_enabled_reached ? 1 : 0,
                                session->report->motion_completed ? 1 : 0,
                                (int)session->report->actual_wkc,
                                (unsigned)session->report->expected_wkc,
                                (unsigned long long)session->report->wkc_error_count,
                                (unsigned long long)session->report->wkc_no_frame_count,
                                (unsigned long long)session->report->over_budget_cycle_count,
                                session->report->cycle_deadline_missed ? 1 : 0,
                                (unsigned long long)session->report->target_rejected_count,
                                target_age_text, reject_axis_text, reject_offset_text);

                            for (axis_index = 0U;
                                 axis_index < session->plan->axis_count; ++axis_index) {
                                const emaster_control_session_axis_result_t *axis =
                                    &session->axes[axis_index];
                                /*
                                 * 已提交的目标只在有规划者时才存在：运动方案或外部目标源，
                                 * 二者之一，会话初始化时据此分配 target_positions。
                                 * 两者都没有（空转会话）时这个缓冲区根本没分配 —— 那时主站
                                 * 每拍写下去的就是驱动器自报的那个目标位，planned 与
                                 * target_pos 本来就相同。印它，而不是解引用空指针。
                                 */
                                const int32_t planned_position =
                                    session->target_positions != NULL
                                        ? session->target_positions[axis_index]
                                        : axis->target_position;
                                const bool slow_has_axis =
                                    slow_ok && axis_index < (size_t)slow.axis_count &&
                                    axis_index < (size_t)EMASTER_OBSERVATION_MAX_AXES;
                                /*
                                 * vel 的口径按模式声明的量分，两种情形里这个字段本来就不是
                                 * 同一个量：
                                 *
                                 * - 速度模式（CSV）：set_feedback 每拍把 PDO 的 606Ch 解进
                                 *   actual_velocity，那就是最干净的值。旧的慢速拷贝会在其后
                                 *   用 SDO 读到的转速把它盖掉——盖掉的是新值，留下的是旧值；
                                 *   现在不盖了。
                                 * - 其余（位置、力矩、回零）：actual_velocity 只有慢速通道
                                 *   这一个来源，快照给它。
                                 */
                                const bool velocity_from_slow =
                                    !(session->plan->axes[axis_index].operation_mode != NULL &&
                                      session->plan->axes[axis_index].operation_mode->quantity ==
                                          EMASTER_MODE_QUANTITY_VELOCITY);
                                /* 快照没读到（观测线程没发布过，或这次撞上写入中途）就
                                 * 退回 0；err 是诊断量，宁可显示"没读到"也不显示一个猜的。 */
                                const unsigned int error_code =
                                    slow_has_axis
                                        ? (unsigned int)slow.axes[axis_index].error_code
                                        : 0U;
                                /*
                                 * 慢速通道能取到的全部读数（P12.50/P12.52）：0x6078 电流、
                                 * 0x6079 母线电压、0x200B 的温度与转速、0x607A 的驱动器自报值、
                                 * 0x6072／0x6080 极限值、0x203E／0x203F 厂商错误码。
                                 *
                                 * 它们进状态回读，是为了让"轴为什么不动"当场可判，而不是跑完
                                 * 再去翻报告或翻详细日志。取不到一律印 na，理由见 format_optional_*。
                                 *
                                 * 判据一律是**字段级**时间戳：轴级标志只说明"这一轴至少有一条
                                 * 读到过"，拿它印电流会把"设备没有这个对象"印成 0。
                                 * 607A 回读额外连年龄一起报：它的值在运动中连续变，读失败留下的
                                 * 旧值是静止的，而"静止"正是要判成"目标没送到"的那个形状，
                                 * 光看值分不开，只能看年龄。
                                 * 年龄用快照自己的发布时刻算，周期线程里不额外取时钟。
                                 */
                                const emaster_observation_slow_axis_t *slow_axis =
                                    slow_has_axis ? &slow.axes[axis_index] : NULL;
                                uint64_t slow_age_ns = UINT64_C(0);
                                uint64_t field_age_ns = UINT64_C(0);
                                /*
                                 * 逐字段可用性。用轴级年龄会把"设备没声明 0x6078"
                                 * 印成 current=0——那是这个回读口最容易被当成
                                 * "读到零电流"的一句话。
                                 */
                                const bool current_read =
                                    slow_axis != NULL &&
                                    emaster_observation_slow_field_age(
                                        &slow, (uint32_t)axis_index,
                                        EMASTER_SLOW_FIELD_ACTUAL_CURRENT, slow.monotonic_ns,
                                        &field_age_ns);
                                const bool bus_voltage_read =
                                    slow_axis != NULL &&
                                    emaster_observation_slow_field_age(
                                        &slow, (uint32_t)axis_index,
                                        EMASTER_SLOW_FIELD_BUS_VOLTAGE, slow.monotonic_ns,
                                        &field_age_ns);
                                const bool mosfet_read =
                                    slow_axis != NULL &&
                                    emaster_observation_slow_field_age(
                                        &slow, (uint32_t)axis_index,
                                        EMASTER_SLOW_FIELD_MOSFET_TEMPERATURE,
                                        slow.monotonic_ns, &field_age_ns);
                                const bool motor_temperature_read =
                                    slow_axis != NULL &&
                                    emaster_observation_slow_field_age(
                                        &slow, (uint32_t)axis_index,
                                        EMASTER_SLOW_FIELD_MOTOR_TEMPERATURE,
                                        slow.monotonic_ns, &field_age_ns);
                                const bool motor_speed_read =
                                    slow_axis != NULL &&
                                    emaster_observation_slow_field_age(
                                        &slow, (uint32_t)axis_index,
                                        EMASTER_SLOW_FIELD_MOTOR_SPEED, slow.monotonic_ns,
                                        &field_age_ns);
                                const bool speed_command_read =
                                    slow_axis != NULL &&
                                    emaster_observation_slow_field_age(
                                        &slow, (uint32_t)axis_index,
                                        EMASTER_SLOW_FIELD_SPEED_COMMAND, slow.monotonic_ns,
                                        &field_age_ns);
                                const int32_t reported_velocity =
                                    (velocity_from_slow && motor_speed_read)
                                        ? slow.axes[axis_index].motor_speed
                                        : axis->actual_velocity;
                                const bool readback_read =
                                    slow_axis != NULL &&
                                    emaster_observation_slow_axis_target_position_age(
                                        &slow, (uint32_t)axis_index, slow.monotonic_ns,
                                        &slow_age_ns);
                                const bool max_limits_read =
                                    slow_axis != NULL &&
                                    emaster_observation_slow_axis_max_limits_age(
                                        &slow, (uint32_t)axis_index, slow.monotonic_ns,
                                        &slow_age_ns);
                                const bool vendor_error_read =
                                    slow_axis != NULL &&
                                    emaster_observation_slow_axis_vendor_error_age(
                                        &slow, (uint32_t)axis_index, slow.monotonic_ns,
                                        &slow_age_ns);
                                /*
                                 * target_pos 这个字段在非位置模式下装的是速度或力矩
                                 * （由模式声明的 quantity 决定）。q= 说明它是哪一种，
                                 * 客户端不用按模式号猜。why= 把"轴为什么不动"压成
                                 * 一个词；判据只用这一行已有的量，不额外读 SDO。
                                 */
                                const emaster_mode_quantity_t quantity =
                                    session->plan->axes[axis_index].operation_mode != NULL
                                        ? session->plan->axes[axis_index].operation_mode->quantity
                                        : EMASTER_MODE_QUANTITY_NONE;
                                const char *quantity_text =
                                    quantity == EMASTER_MODE_QUANTITY_POSITION ? "pos" :
                                    quantity == EMASTER_MODE_QUANTITY_VELOCITY ? "vel" :
                                    quantity == EMASTER_MODE_QUANTITY_TORQUE   ? "tor" : "none";
                                /*
                                 * 回零进度压成一个数：0 不是回零轴，1 已启动未完成，
                                 * 2 已到参考点，3 驱动器报了回零错误。与 why=homing
                                 * 并排看，"在找"和"报错"才分得开。
                                 */
                                const int homing_state =
                                    axis->homing_error ? 3 :
                                    axis->homing_attained ? 2 :
                                    axis->homing_started ? 1 : 0;
                                const char *why_text = "ok";
                                if ((axis->status_word & 0x0008U) != 0U || error_code != 0U)
                                {
                                    why_text = "fault";
                                }
                                else if ((session->controller_outputs[axis_index].control_word &
                                          EMASTER_CIA402_CONTROL_HALT_BIT) != 0U)
                                {
                                    why_text = "halt";
                                }
                                else if (axis->cia402_state !=
                                         EMASTER_CIA402_STATE_OPERATION_ENABLED)
                                {
                                    why_text = "disabled";
                                }
                                else if (session->plan->axes[axis_index].operation_mode != NULL &&
                                         axis->mode_display !=
                                             session->plan->axes[axis_index]
                                                 .operation_mode->value)
                                {
                                    why_text = "mode";
                                }
                                else if (axis->homing_configured && !axis->homing_attained)
                                {
                                    /* 回零的轴本来就不该有位置目标，它"不动"是正常的
                                     * ——除非 bit13 已经报了错，那条在 fault 分支。 */
                                    why_text = "homing";
                                }
                                else if (max_limits_read && slow_axis != NULL &&
                                         slow_axis->max_torque == 0)
                                {
                                    why_text = "maxT";
                                }
                                else if (target_age_valid &&
                                         session->plan->deployment->external_target_timeout_ms >
                                             0U &&
                                         target_age_ms >
                                             session->plan->deployment
                                                 ->external_target_timeout_ms)
                                {
                                    why_text = "timeout";
                                }
                                char readback_text[24];
                                char readback_age_text[24];
                                char max_torque_text[24];
                                char max_motor_speed_text[24];
                                char current_text[24];
                                char bus_voltage_text[24];
                                char mosfet_text[24];
                                char motor_temperature_text[24];
                                char motor_rpm_text[24];
                                char command_rpm_text[24];
                                char extended_error_text[32];
                                char servo_error_text[32];

                                format_optional_int(readback_text, sizeof(readback_text),
                                                    readback_read,
                                                    slow_axis != NULL
                                                        ? slow_axis->target_position_readback
                                                        : 0);
                                if (readback_read)
                                {
                                    (void)snprintf(readback_age_text,
                                                   sizeof(readback_age_text), "%llu",
                                                   (unsigned long long)(slow_age_ns /
                                                                        UINT64_C(1000000)));
                                }
                                else
                                {
                                    (void)snprintf(readback_age_text,
                                                   sizeof(readback_age_text), "na");
                                }
                                format_optional_int(max_torque_text, sizeof(max_torque_text),
                                                    max_limits_read,
                                                    slow_axis != NULL ? slow_axis->max_torque
                                                                      : 0);
                                format_optional_int(max_motor_speed_text,
                                                    sizeof(max_motor_speed_text),
                                                    max_limits_read,
                                                    slow_axis != NULL
                                                        ? slow_axis->max_motor_speed
                                                        : 0);
                                format_optional_int(current_text, sizeof(current_text),
                                                    current_read,
                                                    slow_axis != NULL ? slow_axis->actual_current
                                                                      : 0);
                                format_optional_int(bus_voltage_text, sizeof(bus_voltage_text),
                                                    bus_voltage_read,
                                                    slow_axis != NULL
                                                        ? slow_axis->dc_link_voltage
                                                        : 0);
                                format_optional_int(mosfet_text, sizeof(mosfet_text), mosfet_read,
                                                    slow_axis != NULL
                                                        ? slow_axis->mosfet_temperature
                                                        : 0);
                                format_optional_int(motor_temperature_text,
                                                    sizeof(motor_temperature_text),
                                                    motor_temperature_read,
                                                    slow_axis != NULL
                                                        ? slow_axis->motor_temperature
                                                        : 0);
                                format_optional_int(motor_rpm_text, sizeof(motor_rpm_text),
                                                    motor_speed_read,
                                                    slow_axis != NULL ? slow_axis->motor_speed
                                                                      : 0);
                                format_optional_int(command_rpm_text, sizeof(command_rpm_text),
                                                    speed_command_read,
                                                    slow_axis != NULL ? slow_axis->speed_command
                                                                      : 0);
                                format_optional_hex32(extended_error_text,
                                                      sizeof(extended_error_text),
                                                      vendor_error_read,
                                                      slow_axis != NULL
                                                          ? slow_axis->extended_error_code
                                                          : 0U);
                                format_optional_hex32(servo_error_text,
                                                      sizeof(servo_error_text),
                                                      vendor_error_read,
                                                      slow_axis != NULL
                                                          ? slow_axis->servo_error_code
                                                          : 0U);
                                int written;

                                if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                    break;
                                }
                                written = snprintf(response.message + offset,
                                                   sizeof(response.message) - (size_t)offset,
                                                   "|a%u:pos=%d,vel=%d,torque=%d,status=0x%04x,"
                                                   "target_pos=%d,planned=%d,err=0x%04x,state=%d,"
                                                   "mode=%d,tgt_rb=%s,rb_age_ms=%s,maxT=%s,maxV=%s,"
                                                   "control=0x%04x,current=%s,bus_v=%s,mosfet_t=%s,"
                                                   "motor_t=%s,motor_rpm=%s,cmd_rpm=%s,"
                                                   "ext_err=%s,servo_err=%s,q=%s,home=%d,why=%s",
                                                   (unsigned int)(axis_index + 1U),
                                                   axis->actual_position,
                                                   reported_velocity,
                                                   (int)axis->actual_torque,
                                                   (unsigned int)axis->status_word,
                                                   axis->target_position,
                                                   planned_position,
                                                   error_code,
                                                   (int)axis->cia402_state,
                                                   /* 驱动器自报的 6061。力矩模式下"到底进没进
                                                    * 模式 10"只能从这里看：状态字使能了也可能是
                                                    * 模式没切过去，那样 6071 会被驱动器忽略。 */
                                                   (int)axis->mode_display,
                                                   readback_text, readback_age_text,
                                                   max_torque_text, max_motor_speed_text,
                                                   /* 0x6040：主站这一拍实际写下去的控制字。
                                                    * 与状态字并排看，才分得清"没使能"和
                                                    * "使能了但驱动器不执行"。 */
                                                   (unsigned int)
                                                       session->controller_outputs[axis_index]
                                                           .control_word,
                                                   current_text, bus_voltage_text, mosfet_text,
                                                   motor_temperature_text, motor_rpm_text,
                                                   command_rpm_text, extended_error_text,
                                                   servo_error_text, quantity_text, homing_state,
                                                   why_text);
                                if (written < 0) {
                                    break;
                                }
                                offset += written;
                            }
                            /* 轴数多到填满缓冲区时显式标注，避免客户端把截断当完整。 */
                            if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                (void)snprintf(response.message + sizeof(response.message) - 8U, 8U,
                                              "|TRUNC");
                            }
                            response.success = true;
                            break;
                        }

                        /*
                         * DC/SYNC0 查询（P12.53）。
                         *
                         * 分开成一条命令而不是塞进 status：DC 的量比过程数据还多（每轴 14 项），
                         * 九轴叠进状态回读会把响应缓冲顶到截断，而且终端上那一行就没法读了。
                         *
                         * 三类量在这里合流，缺一类都答不了"同步到底怎么样"：
                         *   配置期 —— 主站请求的 assign_activate／cycle／shift，以及驱动器读完
                         *             写回后**实际生效**的 assign_activate。后者是 P12.47 那条
                         *             空缺：驱动器自己的同步配置此前主站一条读数都没有，
                         *             而 0x001A 正是驱动器自己报出来的。
                         *   运行期 —— 每拍记的 DC 相位、sync0 裕量、迟到次数。
                         *             sync0_late_max_consecutive 是直接对着驱动器那个
                         *             "连续 6 次就掉出 OP"的阈值看的数。
                         *   会话级 —— 参考从站、启动相位误差、迟到阈值。
                         */
                        case EMASTER_COMMAND_QUERY_DC: {
                            int offset = snprintf(response.message, sizeof(response.message),
                                "required=%d,configured=%d,ref=%u,stable=%d"
                                ",startup_done=%u,startup_req=%u,startup_exchanges=%llu"
                                ",startup_phase_err_ns=%lld,late_threshold=%u"
                                ",late_exceeded=%d,late_axis=%d",
                                session->report->dc_required ? 1 : 0,
                                session->report->dc_configured ? 1 : 0,
                                (unsigned)session->report->dc_reference_slave,
                                session->report->dc_startup_stable ? 1 : 0,
                                (unsigned)session->report->dc_startup_cycles_completed,
                                (unsigned)session->report->dc_startup_cycles_requested,
                                (unsigned long long)session->report->dc_startup_exchanges,
                                (long long)session->report->dc_startup_phase_error_ns,
                                (unsigned)session->report->dc_late_consecutive_threshold,
                                session->report->dc_late_threshold_exceeded ? 1 : 0,
                                session->report->dc_late_threshold_exceeded
                                    ? (int)session->report->dc_late_threshold_axis
                                    : -1);

                            for (axis_index = 0U;
                                 axis_index < session->plan->axis_count; ++axis_index) {
                                const emaster_control_session_axis_result_t *axis =
                                    &session->axes[axis_index];
                                const emaster_cyclic_timing_stats_t *timing = &axis->timing;
                                char observed_text[16];
                                char margin_text[24];
                                char margin_min_text[24];
                                char phase_text[24];
                                int written;

                                /*
                                 * 读失败时 observed 里是没意义的数，印出来会被当成
                                 * "驱动器回的配置就是这个"——而这一栏的全部价值就在于
                                 * 它是驱动器真正生效的那一份。
                                 */
                                if (axis->dc.register_read_succeeded)
                                {
                                    (void)snprintf(observed_text, sizeof(observed_text),
                                                   "0x%04X",
                                                   (unsigned)axis->dc.observed_assign_activate);
                                }
                                else
                                {
                                    (void)snprintf(observed_text, sizeof(observed_text), "na");
                                }
                                /*
                                 * 没有 DC 样本时裕量是 0，而 0 ns 裕量看起来像"刚好卡在边界
                                 * 上"——比"没测到"严重得多。所以按 has_*／last_dc_sample_valid 判。
                                 */
                                format_optional_int(margin_text, sizeof(margin_text),
                                                    timing->last_dc_sample_valid,
                                                    (int32_t)timing->last_sync0_margin_ns);
                                format_optional_int(margin_min_text, sizeof(margin_min_text),
                                                    timing->has_sync0_margin,
                                                    (int32_t)timing->min_sync0_margin_ns);
                                format_optional_int(phase_text, sizeof(phase_text),
                                                    timing->has_phase_error,
                                                    (int32_t)timing->last_phase_error_ns);

                                if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                    break;
                                }
                                written = snprintf(response.message + offset,
                                                   sizeof(response.message) - (size_t)offset,
                                                   "|a%u:aa_req=0x%04X,aa_obs=%s"
                                                   ",sync0_req=%d,sync0=%d"
                                                   ",cycle_req_ns=%u,cycle_obs_ns=%u"
                                                   ",shift_req_ns=%d,shift_soem_ns=%d"
                                                   ",samples=%llu,margin_ns=%s"
                                                   ",margin_min_ns=%s,phase_err_ns=%s"
                                                   ",late=%llu,late_run=%llu"
                                                   ",late_first=%llu,late_last=%llu"
                                                   ",late_run_first=%llu",
                                                   (unsigned int)(axis_index + 1U),
                                                   (unsigned)axis->dc.requested_assign_activate,
                                                   observed_text,
                                                   axis->dc.sync0_requested ? 1 : 0,
                                                   axis->dc.sync0_active ? 1 : 0,
                                                   (unsigned)axis->dc.requested_cycle_ns,
                                                   (unsigned)axis->dc.observed_cycle_ns,
                                                   (int)axis->dc.requested_shift_ns,
                                                   (int)axis->dc.soem_shift_ns,
                                                   (unsigned long long)
                                                       timing->dc_time_sample_count,
                                                   margin_text, margin_min_text, phase_text,
                                                   (unsigned long long)timing->sync0_late_count,
                                                   (unsigned long long)
                                                       timing->sync0_late_max_consecutive,
                                                   /* 坐标不能省：实测里 25 次迟到全落在
                                                    * 交换号 1–25（开局 DC 环路还没收敛），
                                                    * 只看计数会当成"运动中途迟到"，
                                                    * 而这两个结论的处置完全相反。 */
                                                   (unsigned long long)
                                                       timing->first_sync0_late_exchange,
                                                   (unsigned long long)
                                                       timing->last_sync0_late_exchange,
                                                   (unsigned long long)
                                                       timing->first_sync0_late_run_exchange);
                                if (written < 0) {
                                    break;
                                }
                                offset += written;
                            }
                            if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                (void)snprintf(response.message + sizeof(response.message) - 8U, 8U,
                                              "|TRUNC");
                            }
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_QUERY_TOPOLOGY: {
                            /*
                             * 拓扑查询：返回实际轴数、总线位置和关键参数
                             * 格式: axes=N,max_offset=S|a1:bus=B,enc=E1/E2,gear=G1/G2,torque=T|a2:...
                             *
                             * max_offset 是**各轴目标偏移上限中最小的那个**（原始 counts）。
                             * 它回答的问题和以前那栏 max_step 不同：max_step 说的是"相邻两条
                             * 目标允许差多少"，那是个随客户端频率变化、由客户端自己决定的数，
                             * 主站已经不再判它；max_offset 说的是"目标离实际位置允许多远"，
                             * 这是主站真的会拒收的线（见 session_target.c 与
                             * include/emaster/motion/target_guard.h）。
                             *
                             * 给最小值而不是逐轴给：这一栏是给客户端一个保守的节奏参考，
                             * 单一标量够用；逐轴的真值在报告里（offset_limit_counts）。
                             * 0 表示没有一轴在用这条判据。
                             */
                            /*
                             * 上下限表与外部目标源同生共死：没有源就没有这张表。
                             * 因此整段（含逐轴取最小值）都在源存在时才算，不能只把
                             * 首轴那条判掉、然后让循环去读空表。
                             */
                            uint64_t min_offset = 0U;
                            int offset;

                            if (session->position_target_source != NULL &&
                                session->external_target_limits_counts != NULL &&
                                session->plan->axis_count > 0U) {
                                min_offset = session->external_target_limits_counts[0];
                                for (axis_index = 1U;
                                     axis_index < session->plan->axis_count; ++axis_index) {
                                    uint64_t limit =
                                        session->external_target_limits_counts[axis_index];
                                    if (limit != 0U && (min_offset == 0U || limit < min_offset)) {
                                        min_offset = limit;
                                    }
                                }
                            }
                            offset = snprintf(response.message, sizeof(response.message),
                                              "axes=%zu,max_offset=%lu",
                                              session->plan->axis_count,
                                              (unsigned long)min_offset);

                            for (axis_index = 0U;
                                 axis_index < session->plan->axis_count; ++axis_index) {
                                const emaster_control_session_axis_result_t *axis =
                                    &session->axes[axis_index];
                                const emaster_position_scale_t *scale = &axis->position_scale;
                                char serial_text[16];
                                int written;

                                if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                    break;
                                }
                                if (axis->serial_number_read) {
                                    (void)snprintf(serial_text, sizeof(serial_text),
                                                   "0x%08X",
                                                   (unsigned)axis->actual_serial_number);
                                } else {
                                    (void)snprintf(serial_text, sizeof(serial_text), "na");
                                }
                                /* 编码器分辨率 = encoder_increments / encoder_motor_revolutions
                                 * 齿轮比 = gear_motor_revolutions : gear_shaft_revolutions
                                 * 分辨率的两段都要给：只给分子，encoder_motor_revolutions
                                 * 不是 1 的设备（编码器不直接装在电机轴上）在客户端会被算错，
                                 * 而算错的是一个把角度折成脉冲的系数，看不出来。
                                 * torque 是保留字段，恒为 0：额定力矩目前没有数据源
                                 * （ESI、设备档、运动方案里都没有这个数），而凭空造一个
                                 * 会让客户端拿它把千分比折成牛米。保留位置、不编值。 */
                                written = snprintf(response.message + offset,
                                                   sizeof(response.message) - (size_t)offset,
                                                   "|a%u:bus=%u,enc=%u/%u,gear=%u/%u,torque=0,ser=%s",
                                                   (unsigned int)(axis_index + 1U),
                                                   (unsigned int)axis->position,
                                                   (unsigned int)scale->encoder_increments,
                                                   (unsigned int)scale->encoder_motor_revolutions,
                                                   (unsigned int)scale->gear_motor_revolutions,
                                                   (unsigned int)scale->gear_shaft_revolutions,
                                                   serial_text);
                                if (written < 0) {
                                    break;
                                }
                                offset += written;
                            }
                            if (offset < 0 || (size_t)offset >= sizeof(response.message)) {
                                (void)snprintf(response.message + sizeof(response.message) - 8U, 8U,
                                              "|TRUNC");
                            }
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_SWITCH_MOTION: {
                            const emaster_motion_profile_t *new_profile;
                            emaster_control_session_status_t switch_status;

                            /* payload 应该是运动配置 ID */
                            if (command.payload[0] == '\0') {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "missing motion profile ID");
                                response.success = false;
                                break;
                            }

                            new_profile = emaster_motion_profile_by_id(command.payload);
                            if (new_profile == NULL) {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "motion profile not found: %s", command.payload);
                                response.success = false;
                                break;
                            }

                            switch_status = emaster_soem_session_switch_motion(session, new_profile);
                            if (switch_status != EMASTER_CONTROL_SESSION_OK) {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "motion switch failed: status=%d", (int)switch_status);
                                response.success = false;
                                break;
                            }

                            (void)snprintf(response.message, sizeof(response.message),
                                "motion switched to %s", command.payload);
                            response.success = true;
                            break;
                        }

                        /*
                         * P12.1: 这两条原先写的是 report->stop_requested，而安全门读的是
                         * session->stop_requested 那个**回调**。两个名字像，意思不同，于是
                         * "从命令口停机"是一句空话：报告里多一个 true，轴上什么都没发生。
                         * 现在改成抬高安全门真正读的那个输入，由它按与 SIGINT 完全相同
                         * 的那段代码停机——安全停机帧、AL 快照、审计轨迹逐拍一致，报告里
                         * 唯一的差别是 stop_source。
                         *
                         * 响应字符串不动：它说的是"已受理"，而这正是事实；生效在下一拍。
                         */
                        case EMASTER_COMMAND_STOP_MOTION: {
                            /*
                             * stop 是"停运动"，不是"退会话"。此前它和 shutdown 走同一条
                             * 停机路径，客户端发 stop 后连接直接没了。现在逐轴置 Halt
                             * （控制字 bit8），驱动器减速并保持使能；halt 0 解除。
                             */
                            size_t axis_idx;
                            for (axis_idx = 0U; axis_idx < session->plan->axis_count; ++axis_idx)
                            {
                                emaster_cia402_controller_set_halt(
                                    &session->controllers[axis_idx], true);
                            }
                            (void)snprintf(response.message, sizeof(response.message),
                                "halt set for %zu axes; send 'halt 0' to resume",
                                session->plan->axis_count);
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_SHUTDOWN:
                            emaster_soem_session_request_stop(
                                session, EMASTER_STOP_SOURCE_COMMAND);
                            (void)snprintf(response.message, sizeof(response.message),
                                "shutdown requested");
                            response.success = true;
                            break;

                        case EMASTER_COMMAND_QUICK_STOP: {
                            size_t axis_idx;
                            for (axis_idx = 0U; axis_idx < session->plan->axis_count; ++axis_idx)
                            {
                                emaster_cia402_controller_set_goal(
                                    &session->controllers[axis_idx],
                                    EMASTER_CIA402_GOAL_QUICK_STOP);
                            }
                            (void)snprintf(response.message, sizeof(response.message),
                                "quick stop requested for %zu axes",
                                session->plan->axis_count);
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_HALT: {
                            bool activate = (command.payload[0] == '1');
                            size_t axis_idx;
                            for (axis_idx = 0U; axis_idx < session->plan->axis_count; ++axis_idx)
                            {
                                emaster_cia402_controller_set_halt(
                                    &session->controllers[axis_idx], activate);
                            }
                            (void)snprintf(response.message, sizeof(response.message),
                                "halt %s for %zu axes",
                                activate ? "set" : "cleared",
                                session->plan->axis_count);
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_FAULT_RESET: {
                            size_t axis_idx;
                            for (axis_idx = 0U; axis_idx < session->plan->axis_count; ++axis_idx)
                            {
                                emaster_cia402_controller_request_fault_reset(
                                    &session->controllers[axis_idx]);
                            }
                            (void)snprintf(response.message, sizeof(response.message),
                                "fault reset requested for %zu axes",
                                session->plan->axis_count);
                            response.success = true;
                            break;
                        }

                        case EMASTER_COMMAND_RECOVER_AXIS: {
                            /* P2.5: 恢复单个故障轴 */
                            char *endptr;
                            long axis_idx_long;
                            size_t axis_idx;

                            if (command.payload[0] == '\0') {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "缺少轴索引参数");
                                response.success = false;
                                break;
                            }

                            axis_idx_long = strtol(command.payload, &endptr, 10);
                            if (*endptr != '\0' || axis_idx_long < 0 ||
                                (size_t)axis_idx_long >= session->plan->axis_count)
                            {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "无效的轴索引: %s", command.payload);
                                response.success = false;
                                break;
                            }

                            axis_idx = (size_t)axis_idx_long;

                            if (!session->axes[axis_idx].fault_isolated) {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "轴%zu 未处于故障状态", axis_idx);
                                response.success = false;
                                break;
                            }

                            /* 标记为恢复中 */
                            session->axes[axis_idx].axis_status = EMASTER_AXIS_STATUS_RECOVERING;

                            /* 发送 Fault Reset */
                            emaster_cia402_controller_request_fault_reset(
                                &session->controllers[axis_idx]);

                            (void)snprintf(response.message, sizeof(response.message),
                                "轴%zu 恢复流程已启动", axis_idx);
                            response.success = true;
                            break;
                        }

                        /*
                         * 任意对象字典读：payload "<从站> <索引> <子索引>"。
                         *
                         * 这里只把请求交给观测线程，绝不自己发邮箱——周期线程上
                         * 任何一次 SDO 往返都是周期预算里的一段无界等待。响应在
                         * 结果回来后的某一拍由循环顶部那一段发出，因此本条命令
                         * 用 goto 跳过本拍的统一响应。
                         */
                        case EMASTER_COMMAND_SDO_READ: {
                            char payload_copy[256];
                            char *saveptr = NULL;
                            char *token;
                            unsigned long values[3];
                            size_t count = 0U;
                            bool parse_ok = true;
                            uint32_t generation;

                            strncpy(payload_copy, command.payload,
                                    sizeof(payload_copy) - 1U);
                            payload_copy[sizeof(payload_copy) - 1U] = '\0';
                            token = strtok_r(payload_copy, " \t", &saveptr);
                            while (token != NULL && count < 3U) {
                                if (!parse_unsigned_token(token, &values[count])) {
                                    parse_ok = false;
                                    break;
                                }
                                ++count;
                                token = strtok_r(NULL, " \t", &saveptr);
                            }
                            if (!parse_ok || count != 3U || token != NULL) {
                                (void)snprintf(response.message, sizeof(response.message),
                                               "用法: sdo_read <从站> <索引> <子索引>"
                                               "（例: sdo_read 1 0x2009 0）");
                                response.success = false;
                                break;
                            }
                            if (values[0] < 1UL || values[0] > 65535UL ||
                                values[1] > 65535UL || values[2] > 255UL) {
                                (void)snprintf(response.message, sizeof(response.message),
                                               "参数越界: slave=%lu,index=0x%lX,sub=%lu",
                                               values[0], values[1], values[2]);
                                response.success = false;
                                break;
                            }
                            if (session->bus_slave_count > 0 &&
                                values[0] > (unsigned long)session->bus_slave_count) {
                                (void)snprintf(response.message, sizeof(response.message),
                                               "从站号越界: 环上只有 %d 台",
                                               (int)session->bus_slave_count);
                                response.success = false;
                                break;
                            }
                            if (!session->observer_thread_created || !session->observer_running) {
                                (void)snprintf(response.message, sizeof(response.message),
                                               "观测线程未运行，无法执行 SDO 读");
                                response.success = false;
                                break;
                            }

                            generation = atomic_load_explicit(
                                             &session->sdo_read_generation,
                                             memory_order_relaxed) +
                                         UINT32_C(1);
                            if (generation == 0U) {
                                generation = 1U;
                            }
                            session->sdo_read_slave = (uint16_t)values[0];
                            session->sdo_read_index = (uint16_t)values[1];
                            session->sdo_read_subindex = (uint8_t)values[2];
                            session->sdo_read_wkc = 0;
                            session->sdo_read_size = 0;
                            memset(session->sdo_read_data, 0,
                                   sizeof(session->sdo_read_data));
                            session->sdo_read_pending = true;
                            session->sdo_read_pending_generation = generation;
                            /* 5 s：观测线程一轮 50 ms，单次读超时 30 ms，正常碰不到。 */
                            session->sdo_read_deadline_ns =
                                now_ns + UINT64_C(5000000000);
                            session->sdo_read_timed_out = false;
                            /* 字段写完之后才抬代次，release 保证观测线程看到完整请求。 */
                            atomic_store_explicit(&session->sdo_read_generation, generation,
                                                  memory_order_release);
                            goto sdo_submitted;
                        }

                        case EMASTER_COMMAND_SET_EXTERNAL_TARGET: {
                            /* 外部位置目标命令：解析并更新注入的目标缓冲区 */
                            emaster_external_target_buffer_t *buf = session->external_target_buffer;

                            char *token;
                            char *saveptr;
                            char payload_copy[256];
                            size_t count = 0;
                            int32_t positions[EMASTER_EXTERNAL_TARGET_MAX_AXES];

                            if (buf == NULL) {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "ERROR|No external target buffer configured");
                                response.success = false;
                                break;
                            }

                            /* P6.2: 只在 RUNNING 状态下接受外部目标，防止 OP 前注入导致首周期跳变。
                             * 客户端应先查询 status 确认 state=4 (RUNNING) 后再发送 set_external_target。 */
                            if (session->report->state != EMASTER_CONTROL_STATE_RUNNING) {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "ERROR|Not ready: state=%d (need RUNNING=4)",
                                    (int)session->report->state);
                                response.success = false;
                                break;
                            }

                            strncpy(payload_copy, command.payload, sizeof(payload_copy) - 1);
                            payload_copy[sizeof(payload_copy) - 1] = '\0';

                            token = strtok_r(payload_copy, " \t", &saveptr);
                            while (token != NULL && count < EMASTER_EXTERNAL_TARGET_MAX_AXES) {
                                char *endptr;
                                long val = strtol(token, &endptr, 10);
                                /* 检查转换错误：非空字符串、完全转换、范围有效 */
                                if (*token == '\0' || *endptr != '\0' || val < INT32_MIN || val > INT32_MAX) {
                                    (void)snprintf(response.message, sizeof(response.message),
                                        "ERROR|Invalid position value: %s", token);
                                    response.success = false;
                                    goto send_response;
                                }
                                positions[count] = (int32_t)val;
                                count++;
                                token = strtok_r(NULL, " \t", &saveptr);
                            }

                            if (count == buf->axis_count && count > 0) {
                                struct timespec ts_cmd;
                                uint64_t cmd_time_ns = 0U;
                                if (clock_gettime(CLOCK_MONOTONIC, &ts_cmd) == 0) {
                                    cmd_time_ns = (uint64_t)ts_cmd.tv_sec * UINT64_C(1000000000) +
                                                  (uint64_t)ts_cmd.tv_nsec;
                                }
                                pthread_mutex_lock(&buf->mutex);
                                memcpy(buf->positions, positions, count * sizeof(int32_t));
                                /*
                                 * 模式一并写进汇点。文本口没有逐轴模式字段，它写进去的
                                 * 就是"本部署配的那一种"；不写的话，先走过二进制通道、
                                 * 再走文本口，缓冲区里会留着上一个客户端声称的模式——
                                 * 那个值不是这条入口的意思，读的人会当成当前的。
                                 */
                                for (size_t mode_index = 0U; mode_index < count; ++mode_index) {
                                    if (session->plan->axes[mode_index].operation_mode != NULL) {
                                        buf->modes[mode_index] =
                                            session->plan->axes[mode_index].operation_mode->value;
                                    }
                                }
                                buf->available = 1;
                                buf->last_update_ns = cmd_time_ns;
                                pthread_mutex_unlock(&buf->mutex);
                                (void)snprintf(response.message, sizeof(response.message),
                                    "OK|Updated %zu external targets", count);
                                response.success = true;
                            } else {
                                (void)snprintf(response.message, sizeof(response.message),
                                    "ERROR|Wrong count: expected %zu, got %zu", buf->axis_count, count);
                                response.success = false;
                            }
                            break;
                        }

                        default:
                            (void)snprintf(response.message, sizeof(response.message),
                                "unknown command type");
                            response.success = false;
                            break;
                    }

                send_response:
                    (void)emaster_command_server_respond(session->command_server, &response);
                sdo_submitted:
                    /* sdo_read 已交给观测线程：本拍没有可发的响应。 */
                    ;
                }
            }

            // 只有配置了运动配置时，motion_completed 才导致退出
            // 无运动配置时保持运行，等待外部实时指令
            if (session->plan->motion_profile != NULL && session->report->motion_completed) {
                break;
            }
            /*
             * 回零全部到点就停机。回零是一次性动作：所有轴都报了 bit12 之后继续
             * 使能等待没有任何意义，而"等外部停机"会让一台已经到参考点的轴一直
             * 带电。与运动方案走完共用同一条出口（安全停用、诊断、写报告）。
             */
            if (session->report->homing_completed) {
                break;
            }
            if (safety_denied &&
                (safety_status != EMASTER_CONTROL_SESSION_OK ||
                 session->report->stop_requested))
            {
                emaster_soem_session_set_state(
                    session,
                    safety_status == EMASTER_CONTROL_SESSION_OK
                        ? EMASTER_CONTROL_STATE_STOPPING
                        : EMASTER_CONTROL_STATE_FAULTED,
                    safety_status);
                return emaster_soem_session_publish_feedback(session, safety_status);
            }
        }
    }
    return status;
}
