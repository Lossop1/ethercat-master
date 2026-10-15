#ifndef EMASTER_TESTS_AUDIT_REPORT_FIXTURE_H
#define EMASTER_TESTS_AUDIT_REPORT_FIXTURE_H

/*
 * 报告写出这条路上两个测试共用的夹具：一个三轴会话计划 + 一份会话结果。
 *
 * 抽出来不是为了避免重复几行字：两份用例问的是**同一份报告**的两件不同事——
 * 字段的值对不对（test_report_fault_fields.c）与归档削旧是不是既删了又出声
 * （test_report_prune.c）。两边各造一份夹具的话，两边迟早问的不是同一份东西。
 *
 * 真实会话里这份输入来自 config 生成目录 + 一次真机运行；这里手搭，是因为报告
 * 写出器要测的是"给定了这些值，JSON 里印成什么"，不是配置解析本身。
 */

#include "emaster/bus/control_session.h"
#include "emaster/session/session_plan.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 夹具里的轴数。三根够覆盖"健康 / 判过故障 / 读到 0x0000"三种形状。 */
#define FIXTURE_AXIS_COUNT 3U

/* 判故障的那根轴（下标 1）在夹具里的坐标，照抄 P12.17 那份报告。 */
#define FIXTURE_FAULT_EXCHANGE UINT64_C(22383)
#define FIXTURE_ENABLE_LOST_EXCHANGE UINT64_C(22380)
#define FIXTURE_FAULT_STATUS_WORD UINT16_C(0x1208)
#define FIXTURE_ENABLED_STATUS_WORD UINT16_C(0x0237)
#define FIXTURE_RUNTIME_ERROR_CODE UINT16_C(0x2310)
#define FIXTURE_RUNTIME_ERROR_CODE_AGE_NS UINT64_C(182000000)

/* 本次生效的归档保留份数——削旧会真删文件，报告必须能自答按多少份在留证据。 */
#define FIXTURE_ARCHIVE_KEEP 200U

typedef struct
{
    emaster_session_plan_t plan;
    emaster_control_session_report_t report;
    emaster_session_axis_plan_t axis_plans[FIXTURE_AXIS_COUNT];
    emaster_control_session_axis_result_t axis_results[FIXTURE_AXIS_COUNT];
} report_fixture_t;

static inline void report_fixture_init(report_fixture_t *fixture, const char *report_path)
{
    static const emaster_topology_slave_config_t slaves[] = {
        {.position = 1U, .axis_id = "axis-1", .profile_id = "dev"},
        {.position = 2U, .axis_id = "axis-2", .profile_id = "dev"},
        {.position = 3U, .axis_id = "axis-3", .profile_id = "dev"},
    };
    static const emaster_topology_config_t topology = {
        .topology_id = "bench-topology",
        .status = "approved",
        .slaves = slaves,
        .slave_count = FIXTURE_AXIS_COUNT,
    };
    static const emaster_deployment_config_t deployment = {
        .deployment_id = "bench-deployment",
        .hostname = "bench-host",
        .ethercat_interface = "eth0",
        .topology = &topology,
        .run_report_path = "runtime/reports/bench-latest.json",
        .report_archive_keep = FIXTURE_ARCHIVE_KEEP,
    };
    static const emaster_operation_mode_t mode = {.mode_id = "csp", .value = 8};
    static const emaster_slave_profile_t device = {
        .profile_id = "dev",
        .model = "model",
        .identity = {.vendor_id = 1U, .product_code = 2U, .revision = 3U},
        .supports_distributed_clocks = true,
    };
    static const emaster_pdo_set_profile_t pdo_set = {
        .pdo_set_id = "pdo-a",
        .rx_pdo_bytes = 8U,
        .tx_pdo_bytes = 8U,
    };
    static const emaster_operation_profile_t operation = {
        .profile_id = "op",
        .approval = EMASTER_OPERATION_PROFILE_APPROVED,
        .device_profile_id = "dev",
        .pdo_set_id = "pdo-a",
        .sync_strategy = EMASTER_SYNC_STRATEGY_DC,
        .has_assign_activate = true,
        .assign_activate = 0x0300U,
        .has_cycle_ns = true,
        .cycle_ns = 1000000U,
        .has_process_data_phase_ns = true,
        .process_data_phase_ns = 400000U,
        .has_dc_startup_cycles = true,
        .dc_startup_cycles = 8U,
        .has_sync0_shift_ns = true,
        .sync0_shift_ns = 0,
        .has_sm2_sync_type = true,
        .sm2_sync_type = 0x0004U,
        .has_sm3_sync_type = true,
        .sm3_sync_type = 0x0004U,
        .selected_mode_id = "csp",
        .modes = &mode,
        .mode_count = 1U,
    };
    size_t index;

    memset(fixture, 0, sizeof(*fixture));
    for (index = 0U; index < FIXTURE_AXIS_COUNT; ++index)
    {
        fixture->axis_plans[index].topology_slave = &slaves[index];
        fixture->axis_plans[index].device_profile = &device;
        fixture->axis_plans[index].operation_profile = &operation;
        fixture->axis_plans[index].operation_mode = &mode;
        fixture->axis_plans[index].pdo_set = &pdo_set;
    }
    fixture->plan.deployment = &deployment;
    fixture->plan.axes = fixture->axis_plans;
    fixture->plan.axis_count = FIXTURE_AXIS_COUNT;
    fixture->plan.cycle_ns = 1000000U;

    /*
     * 报告里每轴对象以 {"position":N, 开头，N 取自会话结果的 position（总线顺序），
     * 不是计划数组的下标。真机运行时这个字段由会话装配，夹具里得自己给。
     */
    for (index = 0U; index < FIXTURE_AXIS_COUNT; ++index)
    {
        fixture->axis_results[index].position = (uint16_t)(index + 1U);
    }

    /* 轴 1：健康。任何故障列都没有值，正是"不许凭空长出来"的那根。 */
    fixture->axis_results[0].status_word = FIXTURE_ENABLED_STATUS_WORD;
    fixture->axis_results[0].axis_status = EMASTER_AXIS_STATUS_NORMAL;

    /* 轴 2：照抄 P12.17 的形状——第 22383 拍判故障、状态字 0x1208、未隔离。 */
    fixture->axis_results[1].status_word = FIXTURE_FAULT_STATUS_WORD;
    fixture->axis_results[1].axis_status = EMASTER_AXIS_STATUS_FAULTED;
    fixture->axis_results[1].fault_recorded = true;
    /* 默认策略是 global_stop，不走隔离，所以这里必须是 false。 */
    fixture->axis_results[1].fault_isolated = false;
    fixture->axis_results[1].fault_cycle = FIXTURE_FAULT_EXCHANGE;
    fixture->axis_results[1].fault_reason = EMASTER_CONTROL_SESSION_DRIVE_FAULT;
    fixture->axis_results[1].fault_status_word = FIXTURE_FAULT_STATUS_WORD;
    fixture->axis_results[1].enable_lost_seen = true;
    fixture->axis_results[1].enable_lost_exchange = FIXTURE_ENABLE_LOST_EXCHANGE;
    fixture->axis_results[1].enable_lost_status_word = FIXTURE_ENABLED_STATUS_WORD;
    fixture->axis_results[1].fault_bit_seen = true;
    fixture->axis_results[1].fault_bit_exchange = FIXTURE_FAULT_EXCHANGE;
    fixture->axis_results[1].fault_bit_status_word = FIXTURE_FAULT_STATUS_WORD;
    /*
     * warning / internal_limit 没出现过。这里**故意**把交换号写成非 0 的诱饵，
     * 好让"只认 seen 不认 exchange"的实现当场露馅——0 号交换与"没出现过"长得一样，
     * 正是这一列要分开的东西。
     */
    fixture->axis_results[1].warning_bit_exchange = UINT64_C(7);
    fixture->axis_results[1].internal_limit_exchange = UINT64_C(9);
    fixture->axis_results[1].runtime_error_code = FIXTURE_RUNTIME_ERROR_CODE;
    fixture->axis_results[1].runtime_error_code_read = true;
    fixture->axis_results[1].runtime_error_code_age_ns =
        FIXTURE_RUNTIME_ERROR_CODE_AGE_NS;

    /* 轴 3：603F 真读到了，值是 0x0000。健康形状，但 read 必须是 true。 */
    fixture->axis_results[2].status_word = FIXTURE_ENABLED_STATUS_WORD;
    fixture->axis_results[2].axis_status = EMASTER_AXIS_STATUS_NORMAL;
    fixture->axis_results[2].runtime_error_code = UINT16_C(0x0000);
    fixture->axis_results[2].runtime_error_code_read = true;
    fixture->axis_results[2].runtime_error_code_age_ns = UINT64_C(5000000);

    /*
     * 第四个 AL 坐标（schema_version 7）："已经请求过全体回 INIT、还没关总线"的那次
     * 逐站读数。三根轴特意给成三种不同的样子，好把"照抄一个值给全体"和"逐站读"分开：
     *
     *   轴 1 = 2（PRE-OP）      —— 确实退出了 OP；
     *   轴 2 = 20 / 0x001A      —— 退到了 SAFE-OP 且带错误位，正是用户看到的那种"没退干净"；
     *   轴 3 = 8（OP）          —— 广播叫不动它，还在 OP。
     *
     * 全 0 是"没读过"，不是合法读数之一，所以不能拿一根不赋值的轴来当对照。
     */
    fixture->axis_results[0].shutdown_exit_al_state = UINT16_C(2);
    fixture->axis_results[0].shutdown_exit_al_status_code = UINT16_C(0);
    fixture->axis_results[1].shutdown_exit_al_state = UINT16_C(20);
    fixture->axis_results[1].shutdown_exit_al_status_code = UINT16_C(0x001A);
    fixture->axis_results[2].shutdown_exit_al_state = UINT16_C(8);
    fixture->axis_results[2].shutdown_exit_al_status_code = UINT16_C(0);

    /*
     * 驱动器自报的目标位置（schema_version 8，P12.41）。
     *
     * 三根轴给成三种形状，好把"照抄主站自己的 target_position"和"真的读了驱动器"分开：
     *
     *   轴 1 = read=true、207180 —— 与主站写的 target_position 207148 **不同**。两个值
     *                               各有各的意义，谁照抄谁都会在这两个断言上露馅；
     *   轴 2 = read=true、1031   —— 卡住那一根的形状：主站把目标扫到 38229，驱动器报回来
     *                               的还是起点附近那个数，一步没跟；
     *   轴 3 = read=false、955   —— 这条 SDO 整场没读回来。**值故意留一个非零诱饵**，
     *                               与上面 warning.exchange=7 同一个用意：判据是 read，
     *                               不是"值看起来像不像真的"。
     */
    fixture->axis_results[0].target_position = INT32_C(207148);
    fixture->axis_results[0].target_position_readback = INT32_C(207180);
    fixture->axis_results[0].target_position_readback_read = true;
    fixture->axis_results[0].target_position_readback_age_ns = UINT64_C(120000000);
    fixture->axis_results[1].target_position = INT32_C(38229);
    fixture->axis_results[1].target_position_readback = INT32_C(1031);
    fixture->axis_results[1].target_position_readback_read = true;
    fixture->axis_results[1].target_position_readback_age_ns = UINT64_C(88000000);
    fixture->axis_results[2].target_position = INT32_C(955);
    fixture->axis_results[2].target_position_readback = INT32_C(955);
    /* 轴 3 的 read 保持 memset 的 false：这一条从没读回来过。 */

    /*
     * 停机时那次邮箱失能核对（schema_version 9 起）与它用的判据（12 起）。
     *
     * 计数按不变式配齐（三个数之和 == 轴数 = 3），attempted 故意给 2：写调用只发出去了
     * 两台——若哪个绑定串了位，这一列立刻露馅。判据给 standard_cia402，因为掩码没填的
     * 退化路径才是要钉住的那一条：枚举值到 ASCII 键的映射错了，报告里读起来仍然像真的。
     */
    fixture->report.shutdown_disable_attempted_count = 2U;
    fixture->report.shutdown_statusword_bit2_clear_count = 1U;
    fixture->report.shutdown_statusword_bit2_set_count = 1U;
    fixture->report.shutdown_statusword_unreadable_count = 1U;
    fixture->report.shutdown_stop_criterion = EMASTER_SHUTDOWN_CRITERION_STANDARD_CIA402;

    /*
     * 丢失链计数（13 起）。轴 1 本轮真断过一跳、轴 2 干净、轴 3 read=false 但留非零诱饵：
     * 判据是 error_counters_read，不是值是否为 0。
     *
     * 轴 1 的 first 比当前值小 1，差值才是"本轮断过一次"；first 与当前值相等时本轮
     * 一次没断——只看绝对值会把上电以来的累计当成本轮的。
     */
    fixture->axis_results[0].error_counters_read = true;
    fixture->axis_results[0].lost_link_counter[3] = 3U;
    fixture->axis_results[0].lost_link_counter_first[3] = 2U;
    fixture->axis_results[0].lost_link_counter_first_valid = true;
    fixture->axis_results[1].error_counters_read = true;
    fixture->axis_results[1].lost_link_counter[1] = 5U;
    fixture->axis_results[1].lost_link_counter_first[1] = 5U;
    fixture->axis_results[1].lost_link_counter_first_valid = true;
    fixture->axis_results[2].error_counters_read = false;
    fixture->axis_results[2].lost_link_counter[0] = 7U;

    /*
     * 驱动器自己的跟随误差窗与超时（14 起，设置期读回 6065 / 6066）。
     *
     * 轴 1 两项都读到；轴 2 只读到窗口、超时没读到（这一对必须能各自独立为假，
     * 一起赋值就会在这里露馅）；轴 3 两项都没读到，值留着非零诱饵——判据是 read。
     */
    fixture->axis_results[0].following_error_window_read = true;
    fixture->axis_results[0].following_error_window = UINT32_C(131072);
    fixture->axis_results[0].following_error_timeout_read = true;
    fixture->axis_results[0].following_error_timeout = UINT16_C(200);
    fixture->axis_results[1].following_error_window_read = true;
    fixture->axis_results[1].following_error_window = UINT32_C(4096);
    fixture->axis_results[2].following_error_window = UINT32_C(777777);
    fixture->axis_results[2].following_error_timeout = UINT16_C(31);

    /*
     * 换算与方向的来源（17 起，19 起多出从站自报那一笔）。三种情形必须读得出区别：
     *
     *   轴 1 生效值来自设备档声明，而从站**也答了、答的不一样**——报告里两笔并排，
     *        device_agrees 为假。这是台架从站 1 的形状（自报 6091=28000，真值 28）；
     *   轴 2 生效值来自设备读回（没有声明可压），两笔相同，device_agrees 为真；
     *   轴 3 两个都没有（来源为空），无从比对，device_agrees 为 null。
     *
     * 只判数值判不出这件事：轴 1 的数看着和轴 2 一样是真值，区别在来源那一个键；
     * 而"设备在说谎"与"人写对了档"在 18 及以前的报告里长得一样。
     */
    fixture->axis_results[0].position_scale.source = EMASTER_FACT_SOURCE_CONFIGURED;
    fixture->axis_results[0].position_scale.encoder_increments = UINT32_C(131072);
    fixture->axis_results[0].position_scale.encoder_motor_revolutions = UINT32_C(1);
    fixture->axis_results[0].position_scale.gear_motor_revolutions = UINT32_C(19612);
    fixture->axis_results[0].position_scale.gear_shaft_revolutions = UINT32_C(1000);
    fixture->axis_results[0].device_position_scale.source = EMASTER_FACT_SOURCE_DEVICE;
    fixture->axis_results[0].device_position_scale.encoder_increments = UINT32_C(131072);
    fixture->axis_results[0].device_position_scale.encoder_motor_revolutions = UINT32_C(1);
    fixture->axis_results[0].device_position_scale.gear_motor_revolutions = UINT32_C(28000);
    fixture->axis_results[0].device_position_scale.gear_shaft_revolutions = UINT32_C(1000);
    fixture->axis_results[0].polarity_source = EMASTER_FACT_SOURCE_CONFIGURED;
    fixture->axis_results[1].position_scale.source = EMASTER_FACT_SOURCE_DEVICE;
    fixture->axis_results[1].position_scale.encoder_increments = UINT32_C(16384);
    fixture->axis_results[1].position_scale.encoder_motor_revolutions = UINT32_C(1);
    fixture->axis_results[1].position_scale.gear_motor_revolutions = UINT32_C(25);
    fixture->axis_results[1].position_scale.gear_shaft_revolutions = UINT32_C(1);
    fixture->axis_results[1].device_position_scale = fixture->axis_results[1].position_scale;
    fixture->axis_results[1].polarity_read = true;
    fixture->axis_results[1].polarity_source = EMASTER_FACT_SOURCE_DEVICE;
    fixture->axis_results[2].position_scale.source = EMASTER_FACT_SOURCE_NONE;
    /* 轴 3 的 device_position_scale 留成零值：来源是"没有"，设备没答。 */

    /*
     * 目标偏移上限（14 起）。三根轴给成三种样子，好把"逐轴折算"与"照抄一个值"分开：
     *
     *   轴 1 判过，线在 114688（28:1 的轴，90°）
     *   轴 2 判过，线在 102400（25:1 的轴，换算不同，线就不同）
     *   轴 3 没判（换算没读回来），但留着 0 以外的诱饵值
     */
    fixture->axis_results[0].offset_limit_counts = UINT64_C(114688);
    fixture->axis_results[0].offset_limit_checked = true;
    fixture->axis_results[1].offset_limit_counts = UINT64_C(102400);
    fixture->axis_results[1].offset_limit_checked = true;
    fixture->axis_results[2].offset_limit_counts = UINT64_C(999999);

    fixture->report.axis_count = FIXTURE_AXIS_COUNT;
    fixture->report.axes = fixture->axis_results;
    fixture->report.status = EMASTER_CONTROL_SESSION_DRIVE_FAULT;
    fixture->report.stop_requested = true;
    fixture->report.stop_source = EMASTER_STOP_SOURCE_COMMAND;
    /*
     * 被拒目标（14 起）：计数数的是"被拒的段"而不是拍，轴号与偏移量是**最后**那次的
     * 现场。轴号按其他逐轴字段的口径从 0 起，这里给 2（即第 3 轴）而不是 0——
     * 0 与"从来没拒过"长得一样，正是这一列要分开的东西。
     */
    fixture->report.target_rejected_count = UINT64_C(3);
    fixture->report.target_rejection_axis = 2U;
    fixture->report.target_rejection_offset_counts = UINT64_C(150000);
    /*
     * 设点通道（14 起）。给成"开过、收过、也拒过"的样子，好把三层分开：
     *
     *   enabled 真而计数全零 —— 通道开了但客户端从没连上，是另一回事；
     *   accepted 与 gaps 都给非零且**不等**——丢包数不是收包数减去一个常数；
     *   age 给成正数 —— 收尾时客户端还活着（-1 才是"整轮没收到过"）。
     *
     * 拒收原因取 MODE_MISMATCH 而不是 NONE：NONE 的文本就是"未被拒"，与
     * "这一列没写"长得一样，正是要分开的东西。
     */
    fixture->report.setpoint_enabled = true;
    fixture->report.setpoint_records_accepted = UINT64_C(12345);
    fixture->report.setpoint_records_rejected = UINT64_C(7);
    fixture->report.setpoint_sequence_gaps = UINT64_C(3);
    fixture->report.setpoint_last_sequence = UINT64_C(12349);
    fixture->report.setpoint_last_client_time_ns = UINT64_C(998877665544332211);
    fixture->report.setpoint_last_record_age_ns = INT64_C(4200000);
    fixture->report.setpoint_last_reject_reason = EMASTER_SETPOINT_REJECT_MODE_MISMATCH;
    fixture->report.setpoint_last_reject_axis = 4U;
    /*
     * 不用 snprintf：report_path 是 const char*，编译器不知道它多长，会对每一次调用
     * 报"可能截断"。夹具里传的都是短字面量，照容量卡一刀就够；末尾的 NUL 由上面的
     * memset 留下。
     */
    const size_t path_length = strlen(report_path);
    const size_t kept_length = path_length < sizeof(fixture->report.report_path)
                                   ? path_length
                                   : sizeof(fixture->report.report_path) - 1U;
    memcpy(fixture->report.report_path, report_path, kept_length);
}

#endif /* EMASTER_TESTS_AUDIT_REPORT_FIXTURE_H */
