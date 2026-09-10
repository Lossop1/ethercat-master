#define _POSIX_C_SOURCE 200809L

#include "console.h"
#include "emaster/audit/run_report.h"
#include "emaster/bus/control_session.h"
#include "emaster/config/runtime_config.h"
#include "emaster/messages.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static bool application_stop_requested(void *user_data) {
    (void)user_data;
    return stop_requested != 0;
}

/*
 * 实时位置目标源回调：每个周期被主站调用以获取位置目标。
 *
 * 验证模式：硬编码正弦波运动（周期 10 秒，幅度 ±180°），验证回调机制是否工作。
 * 后续扩展：从共享缓冲区读取外部控制器发送的目标。
 */
static emaster_position_target_source_result_t position_target_source(
    uint64_t cycle,
    const emaster_control_session_axis_result_t *axes,
    size_t axis_count,
    int32_t *target_positions,
    size_t target_capacity,
    void *user_data)
{
    size_t i;
    double time_seconds;
    double angle_degrees;
    int32_t target_counts;

    (void)axes;
    (void)target_capacity;
    (void)user_data;

    /* 计算当前时间（假设 1ms 周期）*/
    time_seconds = (double)cycle * 0.001;

    /* 生成正弦波：周期 10 秒，幅度 ±180° */
    angle_degrees = 180.0 * sin(2.0 * 3.14159265358979323846 * time_seconds / 10.0);

    /* 转换为编码器 counts（16384 counts/rev = 360°）*/
    target_counts = (int32_t)(angle_degrees * 16384.0 / 360.0);

    /* 所有轴使用相同的目标（验证模式）*/
    for (i = 0; i < axis_count; i++) {
        target_positions[i] = target_counts;
    }

    return EMASTER_POSITION_TARGET_SOURCE_UPDATED;
}

static bool install_signal_handlers(void) {
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0) {
        return false;
    }
    return sigaction(SIGINT, &action, NULL) == 0 && sigaction(SIGTERM, &action, NULL) == 0;
}

/*
 * 在当前主机上查找唯一匹配的部署配置。
 * 若找到多个匹配，返回 NULL 并输出候选配置列表到 stderr。
 */
static const emaster_deployment_config_t *deployment_for_current_host(void) {
    char hostname[256];
    const emaster_deployment_config_t *match = NULL;
    const emaster_deployment_config_t *candidates[16];
    size_t candidate_count = 0U;
    size_t index;

    if (gethostname(hostname, sizeof(hostname) - 1U) != 0) {
        return NULL;
    }
    hostname[sizeof(hostname) - 1U] = '\0';

    /* 收集所有匹配的配置 */
    for (index = 0U; index < emaster_deployment_config_count(); ++index) {
        const emaster_deployment_config_t *candidate = emaster_deployment_config_at(index);
        if (candidate != NULL && candidate->hostname != NULL &&
            strcmp(hostname, candidate->hostname) == 0) {
            if (match == NULL) {
                match = candidate;
            }
            if (candidate_count < sizeof(candidates) / sizeof(candidates[0])) {
                candidates[candidate_count++] = candidate;
            }
        }
    }

    /* 唯一匹配时返回配置 */
    if (candidate_count == 1U) {
        return match;
    }

    /* 多个匹配时输出候选列表并返回 NULL */
    if (candidate_count > 1U) {
        fprintf(stderr, "错误：当前主机 %s 有 %zu 个匹配的部署配置：\n",
                hostname, candidate_count);
        for (index = 0U; index < candidate_count; ++index) {
            fprintf(stderr, "  %zu. %s", index + 1U, candidates[index]->deployment_id);
            if (candidates[index]->topology != NULL) {
                fprintf(stderr, " (拓扑=%s, 从站数=%zu)",
                        candidates[index]->topology->topology_id,
                        candidates[index]->topology->slave_count);
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "请使用 --deployment 参数指定：\n");
        fprintf(stderr, "  emaster-master --deployment %s\n",
                candidates[0]->deployment_id);
    }

    return NULL;
}

int main(int argc, char **argv) {
    const emaster_deployment_config_t *deployment;
    emaster_session_axis_plan_t *plan_axes;
    emaster_session_plan_t plan;
    emaster_control_session_axis_result_t *results;
    emaster_control_session_report_t report;
    emaster_control_session_callbacks_t callbacks;
    emaster_session_plan_status_t plan_status;
    emaster_control_session_status_t session_status;
    bool report_published;
    size_t axis_capacity;

    /* 解析命令行参数：支持 --deployment <id> 指定部署配置 */
    if (argc == 3 && strcmp(argv[1], "--deployment") == 0) {
        deployment = emaster_deployment_config_by_id(argv[2]);
        if (deployment == NULL) {
            fprintf(stderr, "错误：未找到部署配置 '%s'\n", argv[2]);
            fprintf(stderr, "可用的部署配置：\n");
            for (size_t i = 0U; i < emaster_deployment_config_count(); ++i) {
                const emaster_deployment_config_t *cfg = emaster_deployment_config_at(i);
                if (cfg != NULL) {
                    fprintf(stderr, "  %s", cfg->deployment_id);
                    if (cfg->hostname != NULL) {
                        fprintf(stderr, " (主机=%s)", cfg->hostname);
                    }
                    fprintf(stderr, "\n");
                }
            }
            return 2;
        }
    } else if (argc == 1) {
        /* 无参数时自动匹配当前主机 */
        deployment = deployment_for_current_host();
        if (deployment == NULL) {
            /* deployment_for_current_host 已输出详细错误信息 */
            return 1;
        }
    } else {
        fprintf(stderr, "用法：%s [--deployment <deployment-id>]\n", argv[0]);
        fprintf(stderr, "  无参数：自动匹配当前主机的部署配置\n");
        fprintf(stderr, "  --deployment：显式指定部署配置ID\n");
        fprintf(stderr, "注意：运行参数由部署配置决定，不接受参数覆盖\n");
        return 2;
    }

    if (!install_signal_handlers()) {
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_SIGNAL_FAILED), stderr);
        return 1;
    }
    if (deployment->topology == NULL) {
        fprintf(stderr, "错误：部署配置 '%s' 未指定拓扑\n", deployment->deployment_id);
        return 1;
    }
    axis_capacity = deployment->topology->slave_count;
    plan_axes = calloc(axis_capacity, sizeof(*plan_axes));
    results = calloc(axis_capacity, sizeof(*results));
    if (plan_axes == NULL || results == NULL) {
        free(plan_axes);
        free(results);
        fputs(emaster_text(EMASTER_TEXT_PROBE_OUT_OF_MEMORY), stderr);
        return 1;
    }
    plan_status = emaster_session_plan_build(deployment, plan_axes, axis_capacity, &plan);
    if (plan_status != EMASTER_SESSION_PLAN_READY) {
        free(plan_axes);
        free(results);
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_PLAN_FAILED), stderr);
        return 1;
    }
    fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_START), deployment->hostname,
            deployment->ethercat_interface, deployment->topology->topology_id,
            (unsigned int)plan.axis_count);
    (void)fflush(stdout);

    memset(&report, 0, sizeof(report));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.stop_requested = application_stop_requested;
    callbacks.position_target_source = position_target_source;
    callbacks.position_target_source_user_data = NULL;
    session_status = emaster_soem_control_session(&plan, results, axis_capacity,
                                                  &callbacks, &report);
    report_published = emaster_run_report_publish(&plan, &report, deployment->run_report_path);
    emaster_master_console_result(&plan, &report, report_published);
    emaster_control_session_report_destroy(&report);
    free(plan_axes);
    free(results);
    return session_status == EMASTER_CONTROL_SESSION_OK && report_published ? 0 : 1;
}
