#define _POSIX_C_SOURCE 200809L

/*
 * 实时位置目标接口的真机验证工具。
 *
 * 存在原因：主程序在部署引用固定运动方案时不绑定 position_target_source，
 * 因此该回调路径从未在真机执行过。本工具用最小的斜坡目标生成器占据该回调，
 * 验证「应用层生成目标 -> 主站周期采纳 -> 电机运动 -> 应用层主动停机」的完整闭环。
 *
 * 与主程序的区别只在命令来源：拓扑、网卡、周期、模式和换算全部仍由部署配置决定，
 * 本工具不接受任何覆盖参数。它不是产品入口，只是回调契约的可重复验证手段。
 *
 * 注意：回调分支不设置 motion_completed，会话没有基于轨迹的正常退出路径。
 * 因此保持阶段结束后必须由本工具通过 stop_requested 请求停机。
 */

#include "emaster/audit/run_report.h"
#include "emaster/bus/control_session.h"
#include "emaster/config/runtime_config.h"
#include "emaster/messages.h"
#include "emaster/motion/position_target.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum
{
    /* 与固定方案基线一致的验证轨迹：5 s 斜坡 + 1 s 保持，便于直接对比结果。 */
    EMASTER_PROBE_RAMP_CYCLES = 5000,
    EMASTER_PROBE_HOLD_CYCLES = 1000,
    EMASTER_PROBE_TARGET_MILLIDEGREES = 36000,
    EMASTER_PROBE_MAX_AXES = 32,
    /*
     * 回调路径的跟随误差保护上限（原始位置计数）。基于真机实测峰值（1274 counts）
     * 取约 2 倍余量（1274 * 2 ≈ 2550，取整为 2560）。与固定方案的 2000 毫度
     * 上限（2548 counts）属于同一量级，能可靠检测驱动器完全不动的场景。
     */
    EMASTER_PROBE_MAX_FOLLOWING_ERROR_COUNTS = 2560
};

/*
 * 回调与主线程共享的斜坡状态。回调在周期线程中运行，因此这里既不分配内存也不加锁：
 * 除 stop_after_hold 以外的字段只被回调读写，主线程仅在会话结束后读取汇总值。
 */
typedef struct
{
    bool prepared;
    bool prepare_failed;
    size_t axis_count;
    int32_t start_counts[EMASTER_PROBE_MAX_AXES];
    int64_t delta_counts[EMASTER_PROBE_MAX_AXES];
    uint64_t ramp_cycles_done;
    uint64_t hold_cycles_done;
    uint64_t update_count;
    uint64_t hold_result_count;
    int32_t last_target[EMASTER_PROBE_MAX_AXES];
    /*
     * 斜坡中点的现场快照。报告字段是 latched 的，无法区分「使能过一瞬」和「全程使能」，
     * 因此必须在运动进行中直接采样控制字、状态字和实际位置。
     */
    bool midpoint_captured;
    uint16_t midpoint_control_word;
    uint16_t midpoint_status_word;
    int32_t midpoint_actual;
    int32_t midpoint_target;
    int midpoint_cia_state;
    volatile sig_atomic_t stop_after_hold;
} emaster_probe_ramp_t;

static volatile sig_atomic_t signal_stop_requested = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    signal_stop_requested = 1;
}

static bool install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0)
    {
        return false;
    }
    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0;
}

/* 保持阶段完成后由回调置位，与 SIGINT 共用同一条安全停机路径。 */
static bool probe_stop_requested(void *user_data)
{
    const emaster_probe_ramp_t *ramp = (const emaster_probe_ramp_t *)user_data;

    return signal_stop_requested != 0 ||
           (ramp != NULL && ramp->stop_after_hold != 0);
}

/*
 * 首次进入时锁定各轴起点并换算目标增量。换算使用当前从站读回的 608F/6091，
 * 与固定方案走同一个 position_target 模块，不引入第二套坐标假设。
 */
static bool prepare_ramp(emaster_probe_ramp_t *ramp,
                         const emaster_control_session_axis_result_t *axes,
                         size_t axis_count)
{
    size_t axis_index;

    if (axis_count == 0U || axis_count > EMASTER_PROBE_MAX_AXES)
    {
        return false;
    }
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        emaster_position_target_axis_model_t model;
        int32_t final_counts;

        memset(&model, 0, sizeof(model));
        model.coordinate = EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT;
        model.scale = axes[axis_index].position_scale;
        model.polarity = INT8_C(1);
        model.limits_enabled = axes[axis_index].software_position_limits_read &&
                               axes[axis_index].software_position_limit_min !=
                                   axes[axis_index].software_position_limit_max;
        model.minimum_counts = axes[axis_index].software_position_limit_min;
        model.maximum_counts = axes[axis_index].software_position_limit_max;
        if (emaster_position_target_from_angle(
                &model, EMASTER_POSITION_TARGET_RELATIVE,
                axes[axis_index].actual_position,
                EMASTER_PROBE_TARGET_MILLIDEGREES,
                &final_counts) != EMASTER_POSITION_TARGET_OK)
        {
            return false;
        }
        ramp->start_counts[axis_index] = axes[axis_index].actual_position;
        ramp->delta_counts[axis_index] =
            (int64_t)final_counts - (int64_t)axes[axis_index].actual_position;
        ramp->last_target[axis_index] = axes[axis_index].actual_position;
    }
    ramp->axis_count = axis_count;
    return true;
}

/*
 * 位置目标来源回调。运行在周期线程：只做整数运算，不分配、不阻塞、不访问 SOEM。
 * 斜坡按已完成周期数线性插值，避免累积浮点误差。
 */
static emaster_position_target_source_result_t probe_position_target_source(
    uint64_t cycle,
    const emaster_control_session_axis_result_t *axes,
    size_t axis_count,
    int32_t *target_positions,
    size_t target_capacity,
    void *user_data)
{
    emaster_probe_ramp_t *ramp = (emaster_probe_ramp_t *)user_data;
    size_t axis_index;

    (void)cycle;
    if (ramp == NULL || axes == NULL || target_positions == NULL ||
        axis_count == 0U || target_capacity < axis_count)
    {
        return EMASTER_POSITION_TARGET_SOURCE_INVALID;
    }
    if (!ramp->prepared)
    {
        if (!prepare_ramp(ramp, axes, axis_count))
        {
            ramp->prepare_failed = true;
            return EMASTER_POSITION_TARGET_SOURCE_INVALID;
        }
        ramp->prepared = true;
    }
    if (ramp->axis_count != axis_count)
    {
        return EMASTER_POSITION_TARGET_SOURCE_INVALID;
    }
    if (ramp->ramp_cycles_done >= EMASTER_PROBE_RAMP_CYCLES)
    {
        /* 保持阶段：明确返回 HOLD，验证主站沿用上一目标而不是重复写入。 */
        if (ramp->hold_cycles_done < EMASTER_PROBE_HOLD_CYCLES)
        {
            ++ramp->hold_cycles_done;
            ++ramp->hold_result_count;
            return EMASTER_POSITION_TARGET_SOURCE_HOLD;
        }
        ramp->stop_after_hold = 1;
        ++ramp->hold_result_count;
        return EMASTER_POSITION_TARGET_SOURCE_HOLD;
    }
    ++ramp->ramp_cycles_done;
    /* 斜坡中点采样一次运行现场，用于事后判断运动期间是否真的处于使能状态。 */
    if (!ramp->midpoint_captured &&
        ramp->ramp_cycles_done == EMASTER_PROBE_RAMP_CYCLES / 2U)
    {
        ramp->midpoint_captured = true;
        ramp->midpoint_control_word = axes[0].control_word;
        ramp->midpoint_status_word = axes[0].status_word;
        ramp->midpoint_actual = axes[0].actual_position;
        ramp->midpoint_target = axes[0].target_position;
        ramp->midpoint_cia_state = (int)axes[0].cia402_state;
    }
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        /* 先乘后除保留整数精度；delta 已按 int32 位置范围校验，不会在此溢出。 */
        int64_t progressed = ramp->delta_counts[axis_index] *
                             (int64_t)ramp->ramp_cycles_done /
                             (int64_t)EMASTER_PROBE_RAMP_CYCLES;
        int64_t target = (int64_t)ramp->start_counts[axis_index] + progressed;

        if (target < INT32_MIN || target > INT32_MAX)
        {
            return EMASTER_POSITION_TARGET_SOURCE_INVALID;
        }
        target_positions[axis_index] = (int32_t)target;
        ramp->last_target[axis_index] = (int32_t)target;
    }
    ++ramp->update_count;
    return EMASTER_POSITION_TARGET_SOURCE_UPDATED;
}

/*
 * 在当前主机上查找唯一匹配的部署配置。
 * 若找到多个匹配，返回 NULL 并输出候选配置列表到 stderr。
 */
static const emaster_deployment_config_t *deployment_for_current_host(void)
{
    char hostname[256];
    const emaster_deployment_config_t *match = NULL;
    const emaster_deployment_config_t *candidates[16];
    size_t candidate_count = 0U;
    size_t index;

    if (gethostname(hostname, sizeof(hostname) - 1U) != 0)
    {
        return NULL;
    }
    hostname[sizeof(hostname) - 1U] = '\0';

    /* 收集所有匹配的配置 */
    for (index = 0U; index < emaster_deployment_config_count(); ++index)
    {
        const emaster_deployment_config_t *candidate =
            emaster_deployment_config_at(index);
        if (candidate != NULL && candidate->hostname != NULL &&
            strcmp(hostname, candidate->hostname) == 0)
        {
            if (match == NULL)
            {
                match = candidate;
            }
            if (candidate_count < sizeof(candidates) / sizeof(candidates[0]))
            {
                candidates[candidate_count++] = candidate;
            }
        }
    }

    /* 唯一匹配时返回配置 */
    if (candidate_count == 1U)
    {
        return match;
    }

    /* 多个匹配时输出候选列表并返回 NULL */
    if (candidate_count > 1U)
    {
        fprintf(stderr, "错误：当前主机 %s 有 %zu 个匹配的部署配置：\n",
                hostname, candidate_count);
        for (index = 0U; index < candidate_count; ++index)
        {
            fprintf(stderr, "  %zu. %s",
                    index + 1U, candidates[index]->deployment_id);
            if (candidates[index]->topology != NULL)
            {
                fprintf(stderr, " (拓扑=%s, 从站数=%zu)",
                        candidates[index]->topology->topology_id,
                        candidates[index]->topology->slave_count);
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "请使用 --deployment 参数指定：\n");
        fprintf(stderr, "  emaster-target-probe --deployment %s\n",
                candidates[0]->deployment_id);
    }

    return NULL;
}

/* 回调闭环的判定只依据本工具自己的计数和会话报告，不复述中文结果目录。 */
static void print_probe_summary(const emaster_probe_ramp_t *ramp,
                                const emaster_control_session_report_t *report,
                                size_t axis_count)
{
    size_t axis_index;

    fprintf(stdout,
            "目标来源回调：准备=%s 换算失败=%s 斜坡周期=%llu 保持周期=%llu "
            "UPDATED=%llu HOLD=%llu\n",
            ramp->prepared ? "完成" : "未执行",
            ramp->prepare_failed ? "是" : "否",
            (unsigned long long)ramp->ramp_cycles_done,
            (unsigned long long)ramp->hold_cycles_done,
            (unsigned long long)ramp->update_count,
            (unsigned long long)ramp->hold_result_count);
    for (axis_index = 0U; axis_index < axis_count && axis_index < ramp->axis_count;
         ++axis_index)
    {
        const emaster_control_session_axis_result_t *axis = &report->axes[axis_index];
        int64_t achieved = (int64_t)axis->actual_position -
                           (int64_t)ramp->start_counts[axis_index];

        fprintf(stdout,
                "轴 %u：起点=%d 计划增量=%lld 最后目标=%d 实际位置=%d "
                "实际增量=%lld 残余误差=%lld counts\n",
                (unsigned int)(axis_index + 1U), ramp->start_counts[axis_index],
                (long long)ramp->delta_counts[axis_index],
                ramp->last_target[axis_index], axis->actual_position,
                (long long)achieved,
                (long long)(ramp->delta_counts[axis_index] - achieved));
    }
    fprintf(stdout,
            "会话结果：状态=%d OP=%s 全轴使能=%s 运动开始=%s 停止请求=%s "
            "安全状态=%s 恢复INIT=%s 周期=%llu WKC=%d/%u\n",
            (int)report->status, report->op_reached ? "通过" : "失败",
            report->all_axes_enabled_reached ? "通过" : "失败",
            report->motion_started ? "通过" : "失败",
            report->stop_requested ? "通过" : "失败",
            report->safe_state_reached ? "通过" : "失败",
            report->restore_init_succeeded ? "通过" : "失败",
            (unsigned long long)report->cycle_count, report->actual_wkc,
            (unsigned int)report->expected_wkc);
    /*
     * 目标写入成功不等于驱动器执行。回调分支没有跟随误差判定，因此必须显式暴露
     * 安全门原因、最终控制字和 PDO 中的目标值，才能区分「主站没发」和「驱动器没动」。
     */
    fprintf(stdout,
            "诊断：状态机=%d 安全门允许=%s 阻塞原因=0x%08lx 锁存故障=%s\n",
            (int)report->state,
            report->safety_control_permitted ? "是" : "否",
            (unsigned long)report->safety_blocking_reasons,
            report->fault_latched ? "是" : "否");
    /* 中点快照是运动进行中的现场，与上面退出时的快照含义不同，不能相互替代。 */
    fprintf(stdout,
            "斜坡中点：采样=%s 控制字=0x%04x 状态字=0x%04x CiA402状态=%d "
            "实际位置=%d PDO目标=%d\n",
            ramp->midpoint_captured ? "是" : "否",
            (unsigned int)ramp->midpoint_control_word,
            (unsigned int)ramp->midpoint_status_word, ramp->midpoint_cia_state,
            ramp->midpoint_actual, ramp->midpoint_target);
    for (axis_index = 0U; axis_index < axis_count; ++axis_index)
    {
        const emaster_control_session_axis_result_t *axis = &report->axes[axis_index];

        fprintf(stdout,
                "轴 %u 诊断：控制字=0x%04x 状态字=0x%04x CiA402状态=%d 模式反馈=%d "
                "模式匹配=%s PDO目标=%d 使能出现=%s 内部限位=%s 电压使能=%s\n",
                (unsigned int)(axis_index + 1U), (unsigned int)axis->control_word,
                (unsigned int)axis->status_word, (int)axis->cia402_state,
                (int)axis->mode_display, axis->mode_display_match ? "是" : "否",
                axis->target_position,
                axis->operation_enabled_seen ? "是" : "否",
                axis->internal_limit_active ? "是" : "否",
                axis->voltage_enabled ? "是" : "否");
        fprintf(stdout,
                "轴 %u 限位：读回=%s 最小=%d 最大=%d 换算读回=%s 608F=%u/%u 6091=%u/%u\n",
                (unsigned int)(axis_index + 1U),
                axis->software_position_limits_read ? "是" : "否",
                axis->software_position_limit_min, axis->software_position_limit_max,
                axis->position_scale.read_succeeded ? "是" : "否",
                (unsigned int)axis->position_scale.encoder_increments,
                (unsigned int)axis->position_scale.encoder_motor_revolutions,
                (unsigned int)axis->position_scale.gear_motor_revolutions,
                (unsigned int)axis->position_scale.gear_shaft_revolutions);
        /*
         * 驱动器自身的故障码是「已使能但不执行」的唯一权威解释来源。
         * 总线层只保留原始读回值，这里同样不解释厂商语义。
         */
        fprintf(stdout,
                "轴 %u 驱动诊断：退出读回=%s 6041错误码=0x%04x 错误寄存器=0x%02x "
                "厂商码=0x%08lx 扩展码=0x%08lx\n",
                (unsigned int)(axis_index + 1U),
                axis->drive_diagnostic.read_succeeded ? "是" : "否",
                (unsigned int)axis->drive_diagnostic.cia402_error_code,
                (unsigned int)axis->drive_diagnostic.error_register,
                (unsigned long)axis->drive_diagnostic.servo_error_code,
                (unsigned long)axis->drive_diagnostic.extended_servo_error_code);
        fprintf(stdout,
                "轴 %u SAFE-OP诊断：读回=%s 6041错误码=0x%04x 错误寄存器=0x%02x "
                "模式读回=%s/%d 跟随误差读回=%s/%d 极性读回=%s/0x%02x\n",
                (unsigned int)(axis_index + 1U),
                axis->safeop_drive_diagnostic.read_succeeded ? "是" : "否",
                (unsigned int)axis->safeop_drive_diagnostic.cia402_error_code,
                (unsigned int)axis->safeop_drive_diagnostic.error_register,
                axis->safeop_mode_display_sdo_read ? "是" : "否",
                (int)axis->safeop_mode_display_sdo,
                axis->following_error_read ? "是" : "否",
                axis->following_error_actual,
                axis->polarity_read ? "是" : "否", (unsigned int)axis->polarity);
    }
}

int main(int argc, char **argv)
{
    const emaster_deployment_config_t *deployment;
    emaster_session_axis_plan_t *plan_axes;
    emaster_session_plan_t plan;
    emaster_control_session_axis_result_t *results;
    emaster_control_session_report_t report;
    emaster_control_session_callbacks_t callbacks;
    emaster_probe_ramp_t ramp;
    emaster_session_plan_status_t plan_status;
    emaster_control_session_status_t session_status;
    size_t axis_capacity;

    /* 解析命令行参数：支持 --deployment <id> 指定部署配置 */
    if (argc == 3 && strcmp(argv[1], "--deployment") == 0)
    {
        deployment = emaster_deployment_config_by_id(argv[2]);
        if (deployment == NULL)
        {
            fprintf(stderr, "错误：未找到部署配置 '%s'\n", argv[2]);
            fprintf(stderr, "可用的部署配置：\n");
            for (size_t i = 0U; i < emaster_deployment_config_count(); ++i)
            {
                const emaster_deployment_config_t *cfg = emaster_deployment_config_at(i);
                if (cfg != NULL)
                {
                    fprintf(stderr, "  %s", cfg->deployment_id);
                    if (cfg->hostname != NULL)
                    {
                        fprintf(stderr, " (主机=%s)", cfg->hostname);
                    }
                    fprintf(stderr, "\n");
                }
            }
            return 2;
        }
    }
    else if (argc == 1)
    {
        /* 无参数时自动匹配当前主机 */
        deployment = deployment_for_current_host();
        if (deployment == NULL)
        {
            /* deployment_for_current_host 已输出详细错误信息 */
            return 1;
        }
    }
    else
    {
        fprintf(stderr, "用法：%s [--deployment <deployment-id>]\n", argv[0]);
        fprintf(stderr, "  无参数：自动匹配当前主机的部署配置\n");
        fprintf(stderr, "  --deployment：显式指定部署配置ID\n");
        fprintf(stderr, "注意：运行参数由部署配置决定，不接受参数覆盖\n");
        return 2;
    }

    if (!install_signal_handlers())
    {
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_SIGNAL_FAILED), stderr);
        return 1;
    }
    if (deployment->topology == NULL)
    {
        fprintf(stderr, "错误：部署配置 '%s' 未指定拓扑\n", deployment->deployment_id);
        return 1;
    }
    axis_capacity = deployment->topology->slave_count;
    if (axis_capacity > EMASTER_PROBE_MAX_AXES)
    {
        fputs("拓扑轴数超出验证工具容量\n", stderr);
        return 1;
    }
    plan_axes = calloc(axis_capacity, sizeof(*plan_axes));
    results = calloc(axis_capacity, sizeof(*results));
    if (plan_axes == NULL || results == NULL)
    {
        free(plan_axes);
        free(results);
        fputs(emaster_text(EMASTER_TEXT_PROBE_OUT_OF_MEMORY), stderr);
        return 1;
    }
    plan_status = emaster_session_plan_build(deployment, plan_axes, axis_capacity, &plan);
    if (plan_status != EMASTER_SESSION_PLAN_READY)
    {
        free(plan_axes);
        free(results);
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_PLAN_FAILED), stderr);
        return 1;
    }
    fprintf(stdout,
            "实时目标验证：主机=%s 接口=%s 拓扑=%s 轴数=%u 斜坡=%d周期 "
            "保持=%d周期 目标=%d毫度\n",
            deployment->hostname, deployment->ethercat_interface,
            deployment->topology->topology_id, (unsigned int)plan.axis_count,
            EMASTER_PROBE_RAMP_CYCLES, EMASTER_PROBE_HOLD_CYCLES,
            EMASTER_PROBE_TARGET_MILLIDEGREES);
    (void)fflush(stdout);

    memset(&report, 0, sizeof(report));
    memset(&ramp, 0, sizeof(ramp));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.stop_requested = probe_stop_requested;
    callbacks.stop_user_data = &ramp;
    callbacks.position_target_source = probe_position_target_source;
    callbacks.position_target_source_user_data = &ramp;
    callbacks.position_target_max_following_error_counts =
        EMASTER_PROBE_MAX_FOLLOWING_ERROR_COUNTS;
    session_status = emaster_soem_control_session(&plan, results, axis_capacity,
                                                  &callbacks, &report);
    /*
     * 运行报告在 print_probe_summary 之前发布，使诊断输出和报告文件的内容保持一致。
     * 发布路径来自部署配置，与主程序共用同一个文件；工具运行后可直接用主程序的报告
     * 分析脚本比较时序数据，无需单独维护解析逻辑。
     */
    if (deployment->run_report_path != NULL)
    {
        if (!emaster_run_report_publish(&plan, &report, deployment->run_report_path))
        {
            fprintf(stderr, "报告发布失败：%s\n", deployment->run_report_path);
        }
        else
        {
            fprintf(stdout, "报告已发布：%s\n", deployment->run_report_path);
        }
    }
    print_probe_summary(&ramp, &report, plan.axis_count);
    emaster_control_session_report_destroy(&report);
    free(plan_axes);
    free(results);
    return session_status == EMASTER_CONTROL_SESSION_OK ? 0 : 1;
}
