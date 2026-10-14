#define _GNU_SOURCE

#include "console.h"
#include "emaster/audit/run_report.h"
#include "emaster/bus/command_socket_path.h"
#include "emaster/bus/control_session.h"
#include "emaster/cli.h"
#include "emaster/config/runtime_config.h"
#include "emaster/config/runtime_config_load.h"
#include "emaster/messages.h"
#include "emaster/motion/relative_position.h"
#include "emaster/setpoint/server.h"

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

/*
 * 断供之后写什么：目标通道收的是位置就保持，收的是速度或力矩就写 0。
 *
 * 由 main 在会话计划建好后按**运行方案声明的量**判定一次（不是按模式号，也不在实时
 * 路径上比字符串），此后只读——position_target_source 每周期读它。
 *
 * 这个开关只有一处作用，但缺了它就不安全：位置模式下"没有外部目标"可以保持上一周期
 * 的位置（轴停在那儿不动）；速度模式下同样处理等于"继续按上一条速度跑"，力矩模式下
 * 等于"继续输出上一条力矩"——客户端失联之后轴会一直跑、一直出力。所以这两种一律写 0。
 *
 * **一个开关够用，是因为一个部署里的所有轴共用同一个模式对象**：session_plan.c 的
 * find_operation_mode 只按运行方案的 selected_mode_id 取一个模式，循环里每一轴拿到的
 * 是同一个指针。所以第 0 轴答得了全体——这里不是"只看第一轴"的将就。混着两种量的部署
 * 在配置层就构造不出来；真构造出来了，这个判断也该跟着改成逐轴。
 */
static bool external_target_zero_on_loss = false;

/*
 * 这个部署有没有外部目标可给。回零（模式 6）没有周期目标，整条外部目标通道
 * 与设点口都不建。同样在会话计划建好后判定一次，此后只读。
 */
static bool plan_has_cyclic_targets = false;

static uint64_t external_target_timeout_ns = EMASTER_EXTERNAL_TARGET_TIMEOUT_DEFAULT_NS;

/*
 * 这里原先放着两个毫度常量（外部目标路径的跟随误差上限与单步限幅），都已删除。
 * 它们的去处不同，值得记一笔：
 *   - 单步限幅比的是相邻两条目标的差，与客户端发送频率绑定，主站不再判它；
 *   - 跟随误差上限在这条路径上从来没有生效过（阈值只由运动方案路径写入）；
 *   - 取而代之的只有一条：目标离实际位置太远就拒收，上限逐轴取自运行时的
 *     608F/6091 读回值，在 session_setup.c 里折算，不在这个文件里。
 */

/* 外部目标双缓冲区：命令线程写入，周期回调读取，互斥锁保护。 */
static emaster_external_target_buffer_t external_target_buffer = {
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

/*
 * 设点通道（只写套接字）的开关，默认关。取值集合与另两个开关一致。
 *
 * 默认关是交付面的要求：不开这条通道时，主站的行为与加装之前逐字一致，多出来的
 * 只有一个套接字文件。外部控制器要连它，得由部署方显式打开。
 *
 * 不用 session_internal.h 的 emaster_soem_env_flag_enabled：那是总线层的内部头，
 * 而这里是应用层。为了十行判断把 SOEM 的私有边界拉到应用里不值得，两边取值集合
 * 要一致，改动时一起改。
 */
static bool setpoint_channel_enabled(void)
{
    const char *value = getenv("EMASTER_SETPOINT");

    if (value == NULL)
    {
        return false;
    }
    return strcmp(value, "1") == 0 || strcmp(value, "on") == 0 || strcmp(value, "ON") == 0 ||
           strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 || strcmp(value, "YES") == 0;
}

/*
 * 实时位置目标源回调：每个周期被主站调用以获取位置目标。
 *
 * 模式：
 * - 如果 buf->available == 1 且未超时：从 buf->positions 读取
 * - 外部目标超时后：保持上一周期位置（HOLD）
 * - 从未收到过外部目标：保持首拍记下的初始位置（不动，也不跑演示波形）
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
    } else if (external_target_zero_on_loss) {
        /*
         * C2：速度与力矩模式下 HOLD 不是安全选择——session_target.c 的 HOLD 分支直接
         * 放行（"保持上一周期目标"），在速度里就是"继续按上一条速度跑"，在力矩里就是
         * "继续输出上一条力矩"。客户端失联之后轴会一直跑、一直出力。所以空闲与超时都
         * 写 0，并显式返回 UPDATED。
         */
        if (last_mode != 3) {
            fprintf(stderr, "[MODE] Velocity/torque mode: idle, outputting zero\n");
            last_mode = 3;
        }
        for (i = 0; i < axis_count && i < EMASTER_EXTERNAL_TARGET_MAX_AXES; i++) {
            target_positions[i] = 0;
        }
        return EMASTER_POSITION_TARGET_SOURCE_UPDATED;
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

static void print_usage(FILE *out, const char *program)
{
    fprintf(out, "用法：%s [--config-dir <目录>] [--deployment <部署ID>]\n", program);
    fprintf(out, "  --config-dir：运行时配置目录，默认 config-runtime\n");
    fprintf(out, "  --deployment：显式指定部署配置ID；不给则按当前主机名匹配\n");
    fprintf(out, "  --help：打印本说明后退出\n");
    fprintf(out, "  --version：打印版本号后退出\n");
    fprintf(out, "注意：运行参数由部署配置决定，不接受参数覆盖\n");
}

int main(int argc, char **argv) {
    const emaster_deployment_config_t *deployment;
    emaster_session_axis_plan_t *plan_axes;
    emaster_session_plan_t plan;
    emaster_control_session_axis_result_t *results;
    emaster_control_session_report_t report;
    emaster_control_session_callbacks_t callbacks;
    emaster_setpoint_server_t *setpoint_server = NULL;
    emaster_setpoint_stats_t setpoint_stats;
    emaster_session_plan_status_t plan_status;
    emaster_control_session_status_t session_status;
    bool report_published;
    size_t axis_capacity;
    char resolved_report_path[EMASTER_REPORT_PATH_CAPACITY];

    /*
     * --help 与 --version 先处理，二者都不起会话。
     *
     * 不带参数是 master 的默认流程（按当前主机名匹配部署并起会话），所以这里不把
     * "无参数出帮助"当默认 —— 那是标准里写明豁免的情形。
     */
    switch (emaster_cli_info(argc, argv)) {
    case EMASTER_CLI_HELP:
        print_usage(stdout, argv[0]);
        return 0;
    case EMASTER_CLI_VERSION:
        emaster_cli_print_version("emaster-master");
        return 0;
    default:
        break;
    }

    /*
     * 解析命令行参数。
     *
     * 配置在运行时从文件加载，所以多了一件事：这些文件在哪。默认取当前目录下的
     * config-runtime/，用 --config-dir 覆盖。其余行为不变 —— 给 --deployment 就按
     * ID 查，不给就按当前主机名匹配。
     *
     * 主机名匹配在配置外置之后才有了实际用处：第三方把自己的主机名写进部署档即可，
     * 不需要重编译。以前它匹配的是一个编译期常量。
     */
    {
        const char *config_directory = "config-runtime";
        const char *requested_deployment = NULL;
        int argument_index;
        bool bad_usage = false;

        for (argument_index = 1; argument_index < argc && !bad_usage; ++argument_index) {
            if (strcmp(argv[argument_index], "--config-dir") == 0 && argument_index + 1 < argc) {
                config_directory = argv[++argument_index];
            } else if (strcmp(argv[argument_index], "--deployment") == 0 &&
                       argument_index + 1 < argc) {
                requested_deployment = argv[++argument_index];
            } else {
                bad_usage = true;
            }
        }
        if (bad_usage) {
            print_usage(stderr, argv[0]);
            return 2;
        }
        if (!emaster_runtime_config_load(config_directory)) {
            fprintf(stderr, "错误：无法从 '%s' 加载运行时配置\n", config_directory);
            return 1;
        }
        if (requested_deployment != NULL) {
            deployment = emaster_deployment_config_by_id(requested_deployment);
        } else {
            deployment = deployment_for_current_host();
            if (deployment == NULL) {
                /* deployment_for_current_host 已输出详细错误信息 */
                return 1;
            }
        }
    }
    if (deployment == NULL) {
        fprintf(stderr, "错误：未找到匹配的部署配置\n");
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
        emaster_session_plan_report_failure(
            plan_status, &plan, emaster_text(EMASTER_TEXT_CONTROL_SESSION_PLAN_FAILED));
        free(plan_axes);
        free(results);
        return 1;
    }
    fprintf(stdout, emaster_text(EMASTER_TEXT_CONTROL_SESSION_START), deployment->hostname,
            deployment->ethercat_interface, deployment->topology->topology_id,
            (unsigned int)plan.axis_count);
    (void)fflush(stdout);

    /* 设置轴数量供命令处理使用 */
    external_target_buffer.axis_count = plan.axis_count;

    /*
     * 这个部署有没有外部目标可给。模式 6（回零）没有周期目标：参考点搜寻由驱动器
     * 自己走，主站只负责把它带进那个模式并核对结果。
     *
     * 判据是"计划里每根轴的模式"，不是部署里的字符串——计划已经解析过设备事实与
     * 运行方案的匹配。只要有一根轴没有目标，整条外部目标通道就不建（设点口也不建），
     * 否则设点口会照常收记录、计数照常涨，而那一根轴的目标无处可去。
     */
    plan_has_cyclic_targets = emaster_session_plan_has_cyclic_targets(&plan);
    if (!plan_has_cyclic_targets) {
        fputs(emaster_text(EMASTER_TEXT_CONTROL_SESSION_NO_TARGET_CHANNEL), stderr);
    }

    /*
     * C2：按实际构好的计划判定断供先写什么（不是按部署里的字符串，计划已经解析过
     * 设备事实与运行方案的匹配）——判据是运行方案为这个模式声明的量：位置保持，
     * 速度与力矩写 0。不按模式号认：同一个号在不同厂商的说明书里指的不是同一件事。
     */
    external_target_zero_on_loss = plan_has_cyclic_targets &&
                                   plan.axis_count > 0U &&
                                   plan.axes[0].operation_mode != NULL &&
                                   plan.axes[0].operation_mode->quantity !=
                                       EMASTER_MODE_QUANTITY_POSITION;
    if (external_target_zero_on_loss) {
        const char *mode_id = plan.axes[0].operation_mode->mode_id;
        fprintf(stderr, "[MODE] Non-position mode (%s): target source writes zero when "
                        "idle or timed out\n",
                mode_id != NULL ? mode_id : "non-position mode");
    }

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

    /*
     * P6.3 的那一段在这里算过两个数：跟随误差窗与单步限幅。两者都已删除，
     * 所以这里不再需要从首轴的硬件参数折算系数。
     *
     * 删掉的直接理由：单步限幅比的是相邻两条目标的差，于是同一个配置在 200 Hz
     * 与 1 kHz 下允许的速度差四倍——那是客户端该定的数；跟随误差窗在这条路径上
     * 从来没被赋过值，是一道永远不跳的闸。
     *
     * 保留下来的那一条判据（目标离实际位置太远就拒收）不在这里算系数：它按**每轴**
     * 自己的运行时 608F/6091 读回值折算，在 session_setup.c 里逐轴做。用一个首轴
     * 系数套给所有轴会得到假逐轴——台架第 8、12 席是 25:1，其余 28:1。
     */

    /*
     * 设点通道必须在 install_rt_primitives() **之后**建：它的监听线程要从本线程继承
     * 调度策略与绑核，然后自己退掉实时调度（理由见 server.c 与 thread_sched.h）。
     * 提前建等于让它在未配置的调度策略下出生。
     *
     * 建不起来不拦会话：这条通道是外部输入，没有它主站照样跑（文本动词仍是入口）。
     */
    if (setpoint_channel_enabled() && plan_has_cyclic_targets) {
        char setpoint_path[EMASTER_SOCKET_PATH_CAPACITY];
        int8_t expected_modes[EMASTER_EXTERNAL_TARGET_MAX_AXES];
        size_t mode_index;

        for (mode_index = 0U; mode_index < EMASTER_EXTERNAL_TARGET_MAX_AXES; ++mode_index) {
            expected_modes[mode_index] = EMASTER_SETPOINT_MODE_UNCHECKED;
        }
        for (mode_index = 0U; mode_index < plan.axis_count; ++mode_index) {
            if (plan.axes[mode_index].operation_mode != NULL) {
                expected_modes[mode_index] = plan.axes[mode_index].operation_mode->value;
            }
        }
        if (plan.axis_count <= EMASTER_EXTERNAL_TARGET_MAX_AXES &&
            emaster_setpoint_socket_path(deployment->deployment_id, setpoint_path,
                                         sizeof(setpoint_path))) {
            setpoint_server = emaster_setpoint_server_create(setpoint_path,
                                                             &external_target_buffer,
                                                             plan.axis_count, expected_modes);
        } else {
            fprintf(stderr, "警告：设点通道未开：轴数 %zu 超过上限 %u，或部署 ID 推不出路径\n",
                    plan.axis_count, (unsigned)EMASTER_EXTERNAL_TARGET_MAX_AXES);
        }
    }

    memset(&report, 0, sizeof(report));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.stop_requested = application_stop_requested;
    /*
     * 绑定外部目标源的唯一前提：部署没有引用固定运动方案。两条路径在周期里二选一，
     * 且外部目标源优先；部署已经写死了轨迹，再挂一个外部源进去，动的是回调，不是
     * 配置——记录下来的"本次按部署跑"就是假话（target_probe 存在的理由就是这条约定）。
     *
     * **这里不许再挂 EMASTER_SETPOINT。** 那个开关管的是设点口（-sp.sock，见下）这
     * 一条**新**通道；而命令口的 set_external_target 在这条口存在之前就是入口，
     * README 的"配置模型"一节也只说这个开关决定建不建设点口。把绑定也压到这个开关
     * 上，等于让一个默认关的新开关悄悄废掉一个已经交付的入口：控制台按 `m` 键起的
     * 主站 + 面板点动、各台架脚本的 set_external_target 会一条不落地被拒
     * （"No external target buffer configured"），而它们各自要看的指标——周期照常
     * 推进、命令流连续、自检退出码 0——**全部照样成立**。2026-10-14 在 MT 上就是这么
     * 拿到一次"通过"的，日志里 6000 条目标一条都没生效。
     *
     * 早年那条"不绑就会跑演示正弦波"的理由已经不成立：回调在没有外部目标时保持
     * 初始位置（见 position_target_source 的末段），不会自己动。
     *
     * 不绑的场合（部署引用了固定运动方案）会话按期"没有运动"处理：不要求设备声明
     * 608F/6091/607D、不建换算表、不写目标。设备只要给得出 PDO，就能进 OP 并被观测。
     */
    if (plan.motion_profile == NULL && plan_has_cyclic_targets) {
        callbacks.position_target_source = position_target_source;
        callbacks.position_target_source_user_data = &external_target_buffer;
        callbacks.external_target_buffer = &external_target_buffer;
    }
    session_status = emaster_soem_control_session(&plan, results, axis_capacity,
                                                  &callbacks, &report);

    /*
     * 先停通道再发报告：销毁停掉监听线程之后不会再有记录到达，所以那一份计数就是
     * 整轮的（理由见 setpoint/server.h）。反过来先发报告再停，报告里的数就是个中途
     * 快照，而且窗口多长取决于报告写多久。
     */
    emaster_setpoint_server_destroy(setpoint_server, &setpoint_stats);
    report.setpoint_enabled = setpoint_stats.enabled;
    report.setpoint_records_accepted = setpoint_stats.records_accepted;
    report.setpoint_records_rejected = setpoint_stats.records_rejected;
    report.setpoint_sequence_gaps = setpoint_stats.sequence_gaps;
    report.setpoint_last_sequence = setpoint_stats.last_sequence;
    report.setpoint_last_client_time_ns = setpoint_stats.last_client_time_ns;
    report.setpoint_last_record_age_ns = setpoint_stats.last_record_age_ns;
    report.setpoint_last_reject_reason = setpoint_stats.last_reject_reason;
    report.setpoint_last_reject_axis = setpoint_stats.last_reject_axis;

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
