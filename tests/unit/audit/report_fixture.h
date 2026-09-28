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

    fixture->report.axis_count = FIXTURE_AXIS_COUNT;
    fixture->report.axes = fixture->axis_results;
    fixture->report.status = EMASTER_CONTROL_SESSION_DRIVE_FAULT;
    fixture->report.stop_requested = true;
    fixture->report.stop_source = EMASTER_STOP_SOURCE_COMMAND;
    (void)snprintf(fixture->report.report_path, sizeof(fixture->report.report_path),
                   "%s", report_path);
}

#endif /* EMASTER_TESTS_AUDIT_REPORT_FIXTURE_H */
