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
    EMASTER_SESSION_PLAN_PDO_SET_NOT_FOUND,
    EMASTER_SESSION_PLAN_DC_NOT_SUPPORTED,
    EMASTER_SESSION_PLAN_CYCLE_TIME_MISMATCH
} emaster_session_plan_status_t;

/* 每个轴的计划只持有生成目录中的只读指针，不复制或拥有配置对象。 */
typedef struct
{
    const emaster_topology_slave_config_t *topology_slave;
    const emaster_slave_profile_t *device_profile;
    const emaster_operation_profile_t *operation_profile;
    const emaster_operation_mode_t *operation_mode;
    const emaster_pdo_set_profile_t *pdo_set;
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
} emaster_session_plan_t;

/*
 * 根据部署启用的运行方案建立会话计划。axis_storage 由调用者提供，容量必须覆盖实际拓扑；
 * 函数不分配内存。部署未启用运行方案时返回 DISABLED，这是当前第一阶段的预期状态。
 */
emaster_session_plan_status_t
emaster_session_plan_build(const emaster_deployment_config_t *deployment,
                           emaster_session_axis_plan_t *axis_storage,
                           size_t axis_capacity, emaster_session_plan_t *result);

typedef enum
{
    EMASTER_SESSION_LAYOUT_MATCH = 0,
    EMASTER_SESSION_LAYOUT_INVALID_ARGUMENT,
    EMASTER_SESSION_LAYOUT_DISCOVERY_INCOMPLETE,
    EMASTER_SESSION_LAYOUT_CONFIGURATION_REQUIRED,
    EMASTER_SESSION_LAYOUT_CONFIGURATION_UNSUPPORTED
} emaster_session_layout_status_t;

/*
 * 把 PRE-OP 只读发现的实际布局与所选 PDO 方案严格比较。返回 CONFIGURATION_REQUIRED 只表示
 * 设备能力允许未来考虑重映射，不会执行 SDO 写入；本阶段对任何非 MATCH 结果都必须停止。
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
