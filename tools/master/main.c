#define _GNU_SOURCE

#include "console.h"
#include "emaster/audit/run_report.h"
#include "emaster/bus/control_session.h"
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
 * 周期线程的实时调度参数（SCHED_FIFO 优先级、绑核编号）不再写在这里，改由部署配置
 * 声明——它们属于主机，不属于设备，换一台机器就要重挑。台架当前的取值与依据：
 *
 * 优先级 80 是 EtherCAT 主站的常见选择，高于驱动中断（通常 50-60），低于看门狗
 * （通常 90+）。
 *
 * 绑核 11：台架实测（2026-09-15，12 核）CPU 0 承担了 33% 的硬中断（arch_timer
 * 一项就 156 万次），管理网卡 enp97s0 的中断 100% 落在 CPU 0；CPU 11 算力 984
 * （2.5 GHz，与 CPU 0 的 1024 同级）而中断占比仅 1.3%。对"别的核上有 CPU 竞争者"
 * 这类干扰，改绑 CPU 11 是决定性的：同一剂量的 SCHED_FIFO 90 压载打在 CPU 0 上时，
 * 绑 0 的周期线程 1.7 s 就死（status=19），绑 11 的两轮都跑满（wkc_no_frame_count=0，
 * send_lateness 最大 29.3 µs）。
 *
 * 但这不构成"原来的病因是中断打在周期核上"的证据，那条因果链已被证伪：管理面轮询
 * 照样打死绑 11 的周期线程（23.1 s，status=16，send_lateness 冲到 399 µs），而此时
 * 中断亲和全程没动——enp49s0-0 仍在 CPU 4、enp97s0-0 仍在 CPU 0，CPU 11 又只有一个
 * 退出时延为 0 的空闲态。机制尚未确定，不要引用。
 */

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
 * 默认 200 ms = 200 个 1 kHz 周期；P2.1 判据要求客户端断开后进入受控停止。
 * 部署配置给了就按配置走（external_target_timeout_ms），没给就是这个默认值。
 * 由 main 在启动时赋值一次，此后只读——position_target_source 每个周期读它。
 */
#define EMASTER_EXTERNAL_TARGET_TIMEOUT_DEFAULT_NS  UINT64_C(200000000)

static uint64_t external_target_timeout_ns = EMASTER_EXTERNAL_TARGET_TIMEOUT_DEFAULT_NS;

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

    (void)cycle;
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
                    now_ns - buf->last_update_ns > external_target_timeout_ns) {
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
        /* 从未收到过外部目标：保持初始位置，不运行 demo。
         * demo 正弦波需要通过外部客户端显式启动，避免主站启动时自动驱动电机。 */
        if (last_mode != 0) {
            fprintf(stderr, "[MODE] Idle (no external input, holding initial position)\n");
            last_mode = 0;
        }
        for (i = 0; i < axis_count && i < EMASTER_EXTERNAL_TARGET_MAX_AXES; i++) {
            target_positions[i] = initial_positions[i];
        }
        return EMASTER_POSITION_TARGET_SOURCE_UPDATED;
    }

    return EMASTER_POSITION_TARGET_SOURCE_UPDATED;
}

/*
 * P1.1/P1.2: 锁定内存页，并按部署声明把调用线程切换到 SCHED_FIFO + 绑核。
 * 必须在 emaster_soem_control_session 之前调用（此后创建的线程继承本线程的调度
 * 策略与亲和掩码），也必须在此之前没有任何别的线程被创建。
 *
 * 返回值：true 表示可以继续进入 OP，false 表示必须中止。中止与否不取决于某一步
 * 有没有失败，而取决于部署声明的 realtime.required：
 *
 * - required 为真：任何一步失败都中止。"以为自己绑上了其实没绑"是计时类实验里
 *   最贵的一种错——它不产生报错，只让抖动变大，整轮数据悄悄作废。
 * - required 为假（缺省）：失败如实打到 stderr 后继续运行。未绑定的进程照样能跑，
 *   只是不保证时序；调用方要判断"这一轮到底绑上没有"，看报告的 thread_schedstat
 *   （里面每个线程都有 policy、prio 和 cpus_allowed，是内核的实况，不是自述）。
 *
 * 三条措施彼此独立，一条失败不影响其余两条执行——把能生效的都生效，再统一决定
 * 是中止还是继续。
 */
static bool install_rt_primitives(const emaster_deployment_config_t *deployment)
{
    struct sched_param param;
    cpu_set_t cpuset;
    bool failed = false;

    /* mlockall 不受 realtime 块控制：不管绑不绑核、设不设优先级，控制周期都不该被
     * 换页打断，这是全有或全无的一条。它是否"声明过"不改变它该不该失败即中止。 */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        failed = true;
    }

    if (deployment->has_realtime_priority) {
        memset(&param, 0, sizeof(param));
        param.sched_priority = deployment->realtime_priority;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
            perror("pthread_setschedparam");
            fprintf(stderr, "[RT] SCHED_FIFO 优先级 %d 未生效\n",
                    deployment->realtime_priority);
            failed = true;
        }
    }

    if (deployment->has_realtime_cpu_core) {
        const int32_t requested_core = deployment->realtime_cpu_core;

        /*
         * 先挡住越界编号。CPU_SET 对 >= CPU_SETSIZE 的下标是**越界写**，不是返回错误：
         * 配置里一个手滑的 9999 会先破坏栈再谈什么降级。CPU_SETSIZE 是 glibc 的
         * 编译期常量，这里按它判，不按实际核数判。
         */
        if (requested_core < 0 || requested_core >= CPU_SETSIZE) {
            fprintf(stderr, "[RT] CPU 核编号 %d 超出可表示范围（0..%d），未绑定\n",
                    (int)requested_core, (int)(CPU_SETSIZE - 1));
            failed = true;
        } else {
            /* 过了上界检查才转无符号：CPU_SET 内部按下标除以每字位数，传有符号数会
             * 触发 -Wsign-conversion（内核宏里的下标是无符号的）。 */
            const size_t core = (size_t)requested_core;

            CPU_ZERO(&cpuset);
            CPU_SET(core, &cpuset);
            if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
                perror("pthread_setaffinity_np");
                fprintf(stderr, "[RT] 绑定 CPU %zu 未生效（该核可能不存在或不被允许）\n",
                        core);
                failed = true;
            }
        }
    }

    if (!failed) {
        if (deployment->has_realtime_priority || deployment->has_realtime_cpu_core) {
            fprintf(stderr, "[RT] 已应用：mlockall + %s%s%s\n",
                    deployment->has_realtime_priority ? "SCHED_FIFO" : "",
                    (deployment->has_realtime_priority &&
                     deployment->has_realtime_cpu_core) ? " + " : "",
                    deployment->has_realtime_cpu_core ? "绑定指定核" : "");
        } else {
            /* 没声明任何实时参数不是失败，但必须说出来：报告里的 thread_schedstat
             * 会显示 policy=0 且亲和掩码是全集，别让人以为那是没配好。 */
            fprintf(stderr, "[RT] 部署未声明实时参数，仅锁定内存，不设置调度策略与亲和\n");
        }
        return true;
    }

    if (deployment->realtime_required) {
        fprintf(stderr, "[RT] 部署 %s 声明 realtime.required=true，实时参数未全部生效，"
                        "拒绝进入 OP\n", deployment->deployment_id);
        return false;
    }
    fprintf(stderr, "[RT] 警告：实时参数未全部生效，按 realtime.required=false 继续运行。"
                    "本轮时序数据不可用于比较——报告的 thread_schedstat 会显示各线程的"
                    "实际 policy/prio/cpus_allowed\n");
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
    char resolved_report_path[EMASTER_REPORT_PATH_CAPACITY];

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

    /*
     * 命令服务器由会话自己创建（control_session.c），这里不再另建一个。
     * 此前两处都按同一个格式串建服务器、绑同一个路径，后建的那个会 unlink 掉
     * 前者的套接字文件：多出来的监听线程带着一个已经没人能连上的 fd 一直跑，
     * 会话结束后它的 destroy 还会再把路径 unlink 一次。主站侧路径的构造点
     * 现在只有一处（command_socket_path.h），CLI 工具用的是同一个函数。
     */

    /* 外部目标失活超时由部署配置给出；0 表示未配置，保持内置默认值。 */
    if (deployment->external_target_timeout_ms != 0U) {
        external_target_timeout_ns =
            (uint64_t)deployment->external_target_timeout_ms * UINT64_C(1000000);
    }

    /* P1.1/P1.2: 在进入 OP 之前锁定内存并切换到实时调度策略。 */
    if (!install_rt_primitives(deployment)) {
        fputs("错误：RT 初始化失败，拒绝进入 OP\n", stderr);
        free(plan_axes);
        free(results);
        return 1;
    }

    /* P6.3：跟随误差和单步限幅从 plan 的硬件参数和配置字段推算，不再硬编码为测试值。
     * 取第一轴的硬件参数作为全轴统一值（当前台架双轴同型号）。
     * 公式：counts_per_degree = (encoder_increments / encoder_motor_revolutions)
     *                          × (gear_motor_revolutions / gear_shaft_revolutions) / 360
     * max_following_error_millidegrees 来自 motion_axis_config，若为 0 则默认为 2000 (2°)。 */
    uint64_t following_error_counts = 0;
    uint64_t max_step_counts = 0;
    if (plan.axis_count > 0 && plan.axes[0].motion_axis != NULL) {
        const emaster_motion_axis_config_t *motion_axis = plan.axes[0].motion_axis;
        /* 编码器分辨率（counts/motor_rev）× 减速比（motor_rev/shaft_rev）/ 360° */
        double counts_per_motor_rev = (double)motion_axis->expected_encoder_increments /
                                      (double)motion_axis->expected_encoder_motor_revolutions;
        double gear_ratio = (double)motion_axis->expected_gear_motor_revolutions /
                           (double)motion_axis->expected_gear_shaft_revolutions;
        double counts_per_degree = counts_per_motor_rev * gear_ratio / 360.0;
        /* 限幅值：从配置读取，0 时默认 2° 负载侧 */
        uint32_t limit_millidegrees = motion_axis->max_following_error_millidegrees;
        if (limit_millidegrees == 0) {
            limit_millidegrees = 2000;  /* 默认 2° */
        }
        following_error_counts = (uint64_t)(counts_per_degree * (double)limit_millidegrees / 1000.0);
        max_step_counts = following_error_counts;
        fprintf(stderr, "[P6.3] 跟随误差限幅: %.2f° = %lu counts "
                        "(enc=%u/%u, gear=%u:%u, %.2f counts/deg)%s\n",
                (double)limit_millidegrees / 1000.0,
                (unsigned long)following_error_counts,
                motion_axis->expected_encoder_increments,
                motion_axis->expected_encoder_motor_revolutions,
                motion_axis->expected_gear_motor_revolutions,
                motion_axis->expected_gear_shaft_revolutions,
                counts_per_degree,
                motion_axis->max_following_error_millidegrees == 0 ? " (default)" : "");
    } else {
        /* 兜底：无 motion_axis 配置时仍给一个保守值。
         * 6400 counts 在 16384 enc × 28:1 gear 下约 5.02°（1274.31 counts/度），
         * 不是 2°。实际值随编码器分辨率和减速比变化。 */
        following_error_counts = 6400;
        max_step_counts = following_error_counts;
        fprintf(stderr, "[P6.3] 警告：无 motion_axis 配置，使用兜底限幅 %lu counts\n",
                (unsigned long)following_error_counts);
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

    /*
     * 报告路径先解成绝对路径再发布。相对路径按进程 CWD 解析，而主站不做 chdir——
     * 只有 bench 脚本先 cd 到仓库根才成立。解不出来时退回配置原值：写到一个不确定
     * 的位置，也好过让整轮已经跑完的运行在最后一步报失败。解析结果同时写进报告，
     * 让"这份文件到底落在哪"成为可核对的事实。
     */
    if (!emaster_run_report_resolve_path(deployment->run_report_path, resolved_report_path,
                                         sizeof(resolved_report_path)))
    {
        (void)snprintf(resolved_report_path, sizeof(resolved_report_path), "%s",
                       deployment->run_report_path);
    }
    (void)snprintf(report.report_path, sizeof(report.report_path), "%s", resolved_report_path);
    report_published = emaster_run_report_publish(&plan, &report, resolved_report_path,
                                                  deployment->report_archive_keep);
    emaster_master_console_result(&plan, &report, report_published);
    emaster_control_session_report_destroy(&report);
    free(plan_axes);
    free(results);
    return session_status == EMASTER_CONTROL_SESSION_OK && report_published ? 0 : 1;
}
