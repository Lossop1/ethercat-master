#ifndef EMASTER_SESSION_SESSION_PLAN_H
#define EMASTER_SESSION_SESSION_PLAN_H

#include "emaster/catalog/slave_profile.h"
#include "emaster/config/runtime_config.h"
#include "emaster/protocol/pdo_layout.h"

#include <stddef.h>
#include <stdint.h>

/*
 * 会话计划只解释已生成的只读配置，不访问网卡、不调用 SOEM，也不改变 EtherCAT 状态。
 * 调用者必须在非周期线程中建立计划，并在进入任何过程数据状态前逐轴核对真实身份和 PDO。
 */
typedef enum
{
    EMASTER_SESSION_PLAN_READY = 0,
    EMASTER_SESSION_PLAN_INVALID_ARGUMENT,
    EMASTER_SESSION_PLAN_DISABLED,
    EMASTER_SESSION_PLAN_INVALID_TOPOLOGY,
    EMASTER_SESSION_PLAN_AXIS_STORAGE_TOO_SMALL,
    EMASTER_SESSION_PLAN_DEVICE_PROFILE_NOT_FOUND,
    EMASTER_SESSION_PLAN_OPERATION_PROFILE_NOT_FOUND,
    EMASTER_SESSION_PLAN_OPERATION_PROFILE_AMBIGUOUS,
    EMASTER_SESSION_PLAN_OPERATION_PROFILE_NOT_APPROVED,
    EMASTER_SESSION_PLAN_OPERATION_PROFILE_INCOMPLETE,
    /*
     * 模式两分。运行方案要跑的模式必须同时满足两个独立的事实，缺哪一边决定了操作员
     * 该去改什么，所以不合成一句"运行方案不完整"：
     *   MODE_NOT_DECLARED  —— 设备档声明的模式集合里没有它。配置配错了设备。
     *   MODE_NOT_DRIVABLE  —— 设备声明支持，主站的过程映像还表达不了它。缺口在主站。
     */
    EMASTER_SESSION_PLAN_MODE_NOT_DECLARED,
    EMASTER_SESSION_PLAN_MODE_NOT_DRIVABLE,
    EMASTER_SESSION_PLAN_PDO_SET_NOT_FOUND,
    EMASTER_SESSION_PLAN_DC_NOT_SUPPORTED,
    EMASTER_SESSION_PLAN_CYCLE_TIME_MISMATCH,
    EMASTER_SESSION_PLAN_DC_PHASE_MISMATCH,
    EMASTER_SESSION_PLAN_MOTION_PROFILE_NOT_APPROVED,
    EMASTER_SESSION_PLAN_MOTION_PROFILE_INCOMPLETE
} emaster_session_plan_status_t;

/* 每个轴的计划只持有生成目录中的只读指针，不复制或拥有配置对象。 */
typedef struct
{
    const emaster_topology_slave_config_t *topology_slave;
    const emaster_slave_profile_t *device_profile;
    const emaster_operation_profile_t *operation_profile;
    const emaster_operation_mode_t *operation_mode;
    const emaster_pdo_set_profile_t *pdo_set;
    const emaster_motion_axis_config_t *motion_axis;
} emaster_session_axis_plan_t;

/*
 * 一个主站周期只有一个周期时间。不同设备可以使用不同同步策略和 PDO 方案，但部署中启用的
 * 运行方案必须给出相同 cycle_ns；该约束由构建期校验和这里的运行时门共同保证。
 */
typedef struct
{
    emaster_session_plan_status_t status;
    const emaster_deployment_config_t *deployment;
    emaster_session_axis_plan_t *axes;
    size_t axis_count;
    uint32_t cycle_ns;
    uint32_t process_data_phase_ns;
    uint32_t dc_startup_cycles;
    const emaster_motion_profile_t *motion_profile;
    /* P2.5: 故障隔离策略 */
    emaster_fault_policy_t fault_policy;
    /*
     * 失败现场：哪一根轴、哪个模式。只在两个模式类失败上有值。
     *
     * 打印中止原因要能点到名字。只说"运行方案不完整"，操作员拿不到任何线索去分辨
     * 是自己配错了设备，还是主站根本还没实现那个模式 —— 这两件事的下一步完全不同。
     */
    size_t failing_axis_ordinal;
    const char *failing_mode_id;
    int8_t failing_mode_value;
} emaster_session_plan_t;

/*
 * 根据部署启用的运行方案建立会话计划。axis_storage 由调用者提供，容量必须覆盖实际拓扑；
 * 函数不分配内存。部署未启用运行方案时返回 DISABLED，这是当前第一阶段的预期状态。
 */
emaster_session_plan_status_t
emaster_session_plan_build(const emaster_deployment_config_t *deployment,
                           emaster_session_axis_plan_t *axis_storage,
                           size_t axis_capacity, emaster_session_plan_t *result);

/*
 * 计划失败时把中止原因写到 stderr。
 *
 * 两个模式类失败会点到名字（哪根轴、哪个模式、缺的是设备还是主站），其余失败原样
 * 打印 fallback —— 那是各入口自己的通用话术。三个入口共用一个实现，同一种失败才
 * 不会在三个工具里报出三种说法。
 */
void emaster_session_plan_report_failure(emaster_session_plan_status_t status,
                                         const emaster_session_plan_t *plan,
                                         const char *fallback);

/* 诊断策略只能放行保持位置；配置了运动的轴始终要求有效模式匹配。 */
bool emaster_session_axis_mode_allows_control(const emaster_session_axis_plan_t *axis,
                                              bool mode_matches);

/*
 * 计划里每一根轴是否都有周期目标。回零（模式 6）没有。
 *
 * 外部目标通道按"一条记录覆盖全部轴"提交，所以只要有一根轴没有目标，这条通道就
 * 不成立：把它建起来，设点口会照常收记录、计数照常涨，而那一根轴的目标无处可去。
 * 空计划返回 false。
 */
bool emaster_session_plan_has_cyclic_targets(const emaster_session_plan_t *plan);

typedef enum
{
    EMASTER_SESSION_LAYOUT_MATCH = 0,
    EMASTER_SESSION_LAYOUT_INVALID_ARGUMENT,
    EMASTER_SESSION_LAYOUT_DISCOVERY_INCOMPLETE,
    /*
     * 与声明逐条不同，但设备实际映射能满足运行方案：方向总长一致，要用的字段都在、
     * 位宽也对。主站按设备实际建映像，不改写设备（设备可能根本不让改）。
     */
    EMASTER_SESSION_LAYOUT_DEVICE_ADOPTED,
    EMASTER_SESSION_LAYOUT_CONFIGURATION_REQUIRED,
    EMASTER_SESSION_LAYOUT_CONFIGURATION_UNSUPPORTED
} emaster_session_layout_status_t;

/*
 * 把 PRE-OP 只读发现的实际布局与所选 PDO 方案比较。
 *
 * MATCH：逐条相同。DEVICE_ADOPTED：逐条不同但能用（见上），运行方案依赖的字段一个不少。
 * CONFIGURATION_REQUIRED：不能用，但设备声明自己可以重配，于是值得试一次改写。
 * 返回 CONFIGURATION_REQUIRED 只表示设备能力允许未来考虑重映射，本函数不做 SDO 写入。
 */
emaster_session_layout_status_t
emaster_session_axis_validate_layout(const emaster_session_axis_plan_t *axis,
                                     const emaster_pdo_layout_t *actual);

typedef enum
{
    EMASTER_SESSION_ZERO_OUTPUT_OK = 0,
    EMASTER_SESSION_ZERO_OUTPUT_INVALID_ARGUMENT,
    EMASTER_SESSION_ZERO_OUTPUT_BUFFER_TOO_SMALL
} emaster_session_zero_output_status_t;

/* 返回该轴所选 RxPDO 方案需要的原始输出字节数；参数无效时返回 0。 */
size_t emaster_session_axis_output_byte_count(const emaster_session_axis_plan_t *axis);

/*
 * 成功时只把所需的 RxPDO 区域清零，容量不足或参数无效时不修改缓冲区。全零原始映像只是
 * 后续“零输出 OP”试验的必要输入，不代表已经获得状态迁移、驱动使能或运动授权。
 */
emaster_session_zero_output_status_t
emaster_session_axis_prepare_zero_output(const emaster_session_axis_plan_t *axis,
                                         uint8_t *buffer, size_t buffer_capacity);

#endif
