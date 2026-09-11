#define _GNU_SOURCE

#include "console.h"
#include "emaster/audit/run_report.h"
#include "emaster/bus/control_session.h"
#include "emaster/bus/command_server.h"
#include "emaster/config/runtime_config.h"
#include "emaster/messages.h"
#include "emaster/motion/relative_position.h"

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/*
 * RT 参数：等 P1.4/P1.5 实测抖动数据后，考虑迁移到部署配置。
 * 优先级 80 是 EtherCAT 主站的常见选择，高于驱动中断（通常 50-60），
 * 低于看门狗（通常 90+）。绑核 0；如果 Orange Pi 有 CPU 隔离配置，改为隔离核。
 */
#define EMASTER_RT_PRIORITY  80
#define EMASTER_RT_CPU_CORE  0

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
 * 客户端失活超时：外部目标超过此时间未更新则切换到保持模式（HOLD）。
 * 200ms = 200 个 1kHz 周期；P2.1 判据要求客户端断开后进入受控停止。
 * 等 P1.4 长时数据后按需调整，可迁移到部署配置。
 */
#define EMASTER_EXTERNAL_TARGET_TIMEOUT_NS  UINT64_C(200000000)

/*
 * 外部目标路径跟随误差上限：2° 输出轴（负载侧）。
 * P2.2 要求此值非零，否则 session_control.c 中的检查被完全跳过。
 * 当前硬编码为 2°，P3.3 完成后改为从 plan 的硬件比例参数动态推算。
 */
#define EMASTER_EXTERNAL_FOLLOWING_ERROR_DEGREES  2

/*
 * 外部目标路径单步限幅：相邻两条目标之间的最大增量。
 * 取值与跟随误差上限相同：目标单步超过该值时驱动器无论如何无法在一个周期内跟上，
 * 提前拒绝比事后触发 FOLLOWING_ERROR 更清晰。P3.3 完成后应改为从硬件比例参数推算。
 */
#define EMASTER_EXTERNAL_MAX_STEP_DEGREES  EMASTER_EXTERNAL_FOLLOWING_ERROR_DEGREES

/* 外部目标双缓冲区：命令线程写入，周期回调读取，互斥锁保护。 */
static emaster_external_target_buffer_t external_target_buffer = {
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

/*
 * 实时位置目标源回调：每个周期被主站调用以获取位置目标。
 *
 * 模式：
 * - 如果 buf->available == 1 且未超时：从 buf->positions 读取
 * - 外部目标超时后：保持上一周期位置（HOLD）
 * - 从未收到过外部目标：生成正弦波（演示/测试模式）
 *
 * user_data 指向 emaster_external_target_buffer_t，由 main 注入。
 */
static emaster_position_target_source_result_t position_target_source(
    uint64_t cycle,
    const emaster_control_session_axis_result_t *axes,
    size_t axis_count,
    int32_t *target_positions,
    size_t target_capacity,
    void *user_data)
{
    emaster_external_target_buffer_t *buf = (emaster_external_target_buffer_t *)user_data;
    size_t i;
    static int32_t initial_positions[EMASTER_EXTERNAL_TARGET_MAX_AXES] = {0};
    static int initialized = 0;
    static int last_mode = -1;
    static uint64_t demo_start_cycle = 0;
    static int demo_started = 0;

    (void)target_capacity;

    /* 首次调用：记录每个轴的初始位置 */
    if (!initialized) {
        for (i = 0; i < axis_count && i < EMASTER_EXTERNAL_TARGET_MAX_AXES; i++) {
            initial_positions[i] = axes[i].actual_position;
        }
        initialized = 1;
        fprintf(stderr, "[INIT] Initial positions:");
        for (i = 0; i < axis_count && i < EMASTER_EXTERNAL_TARGET_MAX_AXES; i++) {
            fprintf(stderr, " [%zu]=%d", i, initial_positions[i]);
        }
        fprintf(stderr, "\n");
    }

    /* 检查是否有外部目标可用（加锁保护），同时检查超时 */
    int has_external_targets = 0;
    if (buf != NULL) {
        pthread_mutex_lock(&buf->mutex);
        has_external_targets = buf->available;
        if (has_external_targets) {
            struct timespec now_ts;
            if (clock_gettime(CLOCK_MONOTONIC, &now_ts) == 0) {
                uint64_t now_ns = (uint64_t)now_ts.tv_sec * UINT64_C(1000000000) +
                                  (uint64_t)now_ts.tv_nsec;
                if (buf->last_update_ns > 0 &&
                    now_ns - buf->last_update_ns > EMASTER_EXTERNAL_TARGET_TIMEOUT_NS) {
                    buf->available = 0;
                    has_external_targets = 0;
                }
            }
            if (has_external_targets) {
                for (i = 0; i < axis_count && i < EMASTER_EXTERNAL_TARGET_MAX_AXES; i++) {
                    target_positions[i] = buf->positions[i];
                }
            }
        }
        pthread_mutex_unlock(&buf->mutex);
    }

    if (has_external_targets) {
        if (last_mode != 1) {
            fprintf(stderr, "[MODE] Switched to EXTERNAL control\n");
            last_mode = 1;
        }
    } else if (last_mode == 1) {
        /* 刚从外部模式切出：超时或客户端断开，保持上一周期目标 */
        fprintf(stderr, "[MODE] External target timeout — holding position\n");
        last_mode = 2;
        return EMASTER_POSITION_TARGET_SOURCE_HOLD;
    } else if (last_mode == 2) {
        /* 持续保持中 */
        return EMASTER_POSITION_TARGET_SOURCE_HOLD;
    } else {
        /* 从未收到过外部目标：演示正弦波。
         * 时间基准从首次观察到 OPERATION_ENABLED 时开始，避免上电序列期间累积
         * 相位偏移——到达 OPERATION_ENABLED 时约耗时 1.2s，sin 偏移已超跟随误差限。
         * 未到 OPERATION_ENABLED 之前输出初始位置（偏移为零），接管瞬间误差为零。
         *
         * P3.3/P3.4：正弦幅值使用负载侧坐标（OUTPUT_SHAFT），与 README 口径一致。
         * 调用 emaster_motion_angle_to_counts 集中换算，避免工具层硬编码。 */
        if (last_mode != 0) {
            fprintf(stderr, "[MODE] Running DEMO sine wave (no external input)\n");
            last_mode = 0;
        }
        if (!demo_started) {
            int all_enabled = (axis_count > 0);
            for (i = 0; i < axis_count && i < EMASTER_EXTERNAL_TARGET_MAX_AXES; i++) {
                if (axes[i].cia402_state != EMASTER_CIA402_STATE_OPERATION_ENABLED) {
                    all_enabled = 0;
                    break;
                }
            }
            if (all_enabled) {
                demo_start_cycle = cycle;
                demo_started = 1;
                fprintf(stderr, "[DEMO] Drive enabled at cycle %llu, sine starts from t=0\n",
                        (unsigned long long)cycle);
            }
        }
        int32_t offset_counts = 0;
        if (demo_started && axis_count > 0) {
            /* 使用第一轴的 position_scale 换算 ±180° → counts（负载侧） */
            double time_seconds = (double)(cycle - demo_start_cycle) * 0.001;
            double angle_degrees = 180.0 * sin(2.0 * 3.14159265358979323846 * time_seconds / 10.0);
            int32_t angle_millidegrees = (int32_t)(angle_degrees * 1000.0);
            int64_t signed_counts;
            if (emaster_motion_angle_to_counts(angle_millidegrees,
                                              EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT,
                                              &axes[0].position_scale,
                                              &signed_counts)) {
                offset_counts = (int32_t)signed_counts;
            } else {
                /* 换算失败（position_scale 未就绪或溢出），保持偏移为零 */
                offset_counts = 0;
            }
        }
        for (i = 0; i < axis_count && i < EMASTER_EXTERNAL_TARGET_MAX_AXES; i++) {
            target_positions[i] = initial_positions[i] + offset_counts;
        }
        return EMASTER_POSITION_TARGET_SOURCE_UPDATED;
    }

    return EMASTER_POSITION_TARGET_SOURCE_UPDATED;
}

/*
 * P1.1/P1.2: 锁定内存页，并将调用线程切换到 SCHED_FIFO + 绑核。
 * 必须在 emaster_soem_control_session 之前调用；失败则拒绝进入 OP。
 * 优先级和绑核编号见文件顶部宏定义，等 P1.4/P1.5 实测数据后按需调整。
 */
static bool install_rt_primitives(void)
{
    struct sched_param param;
    cpu_set_t cpuset;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        return false;
    }

    memset(&param, 0, sizeof(param));
    param.sched_priority = EMASTER_RT_PRIORITY;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        perror("pthread_setschedparam");
        return false;
    }

    CPU_ZERO(&cpuset);
    CPU_SET(EMASTER_RT_CPU_CORE, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        perror("pthread_setaffinity_np");
        return false;
    }

    return true;
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

    /* 设置轴数量供命令处理使用 */
    external_target_buffer.axis_count = plan.axis_count;

    /* 启动命令服务器 */
    char socket_path[256];
    snprintf(socket_path, sizeof(socket_path), "/tmp/emaster-%s.sock",
             deployment->deployment_id);
    emaster_command_server_t *cmd_server = emaster_command_server_create(socket_path);
    if (cmd_server != NULL) {
        fprintf(stderr, "命令服务器已启动：%s\n", socket_path);
    }

    /* P1.1/P1.2: 在进入 OP 之前锁定内存并切换到实时调度策略。 */
    if (!install_rt_primitives()) {
        fputs("错误：RT 初始化失败（mlockall 或 SCHED_FIFO），拒绝进入 OP\n", stderr);
        if (cmd_server != NULL) {
            emaster_command_server_destroy(cmd_server);
        }
        free(plan_axes);
        free(results);
        return 1;
    }

    /* P3.3：从 plan 的第一个轴 position_scale 换算跟随误差限制（负载侧角度 → counts） */
    uint64_t following_error_counts = 2560;  /* 后备值：假设 16384 enc × 28:1 gear */
    uint64_t max_step_counts = following_error_counts;
    if (plan.axis_count > 0 && plan_axes[0].position_scale.read_succeeded) {
        int64_t temp_counts;
        if (emaster_motion_angle_to_counts(
                EMASTER_EXTERNAL_FOLLOWING_ERROR_DEGREES * 1000,
                EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT,
                &plan_axes[0].position_scale,
                &temp_counts) && temp_counts > 0) {
            following_error_counts = (uint64_t)temp_counts;
            max_step_counts = following_error_counts;
        }
    }

    memset(&report, 0, sizeof(report));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.stop_requested = application_stop_requested;
    callbacks.position_target_source = position_target_source;
    callbacks.position_target_source_user_data = &external_target_buffer;
    callbacks.external_target_buffer = &external_target_buffer;
    callbacks.position_target_max_following_error_counts = following_error_counts;
    callbacks.position_target_max_step_counts = max_step_counts;
    session_status = emaster_soem_control_session(&plan, results, axis_capacity,
                                                  &callbacks, &report);

    /* 清理命令服务器 */
    if (cmd_server != NULL) {
        emaster_command_server_destroy(cmd_server);
    }

    report_published = emaster_run_report_publish(&plan, &report, deployment->run_report_path);
    emaster_master_console_result(&plan, &report, report_published);
    emaster_control_session_report_destroy(&report);
    free(plan_axes);
    free(results);
    return session_status == EMASTER_CONTROL_SESSION_OK && report_published ? 0 : 1;
}
