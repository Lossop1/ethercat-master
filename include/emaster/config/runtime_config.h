#ifndef EMASTER_CONFIG_RUNTIME_CONFIG_H
#define EMASTER_CONFIG_RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    EMASTER_SYNC_STRATEGY_SM = 0,
    EMASTER_SYNC_STRATEGY_DC = 1
} emaster_sync_strategy_t;

/* 审批状态是运行资格，不使用自由文本在业务代码中判断。 */
typedef enum
{
    EMASTER_OPERATION_PROFILE_DRAFT = 0,
    EMASTER_OPERATION_PROFILE_APPROVED = 1
} emaster_operation_profile_approval_t;

typedef enum
{
    EMASTER_CONFIG_SDO_VALUE_U16 = 0
} emaster_config_sdo_value_type_t;

typedef enum
{
    EMASTER_MODE_DISPLAY_REQUIRED = 0,
    EMASTER_MODE_DISPLAY_DIAGNOSTIC = 1
} emaster_mode_display_policy_t;

typedef enum
{
    EMASTER_MOTION_PROFILE_DRAFT = 0,
    EMASTER_MOTION_PROFILE_APPROVED = 1
} emaster_motion_profile_approval_t;

typedef enum
{
    EMASTER_MOTION_COORDINATE_MOTOR_ROTOR = 0,
    EMASTER_MOTION_COORDINATE_OUTPUT_SHAFT = 1
} emaster_motion_coordinate_t;

typedef enum
{
    EMASTER_MOTION_TRAJECTORY_RELATIVE_LINEAR_POSITION = 0
} emaster_motion_trajectory_t;

typedef struct
{
    uint16_t index;
    uint8_t subindex;
    emaster_config_sdo_value_type_t type;
    uint16_t value_u16;
} emaster_sdo_write_config_t;

/* 一个运行方案声明可选的 CiA 402 模式及其需要的收发 PDO 字段。 */
typedef struct
{
    const char *mode_id;
    int8_t value;
    const char *const *required_rx_fields;
    size_t required_rx_field_count;
    const char *const *required_tx_fields;
    size_t required_tx_field_count;
    /* 模式显示异常是否阻断控制由运行方案声明，诊断模式仍会持续采集并报告 6061。 */
    emaster_mode_display_policy_t mode_display_policy;
    /* 设备模式除标准 6060 外需要的供应商 SDO，由运行配置显式声明。 */
    const emaster_sdo_write_config_t *safeop_to_op_sdo_writes;
    size_t safeop_to_op_sdo_write_count;
} emaster_operation_mode_t;

/* 运行方案是设备事实之上的可选择配置，不把任何数值写死在通用代码中。 */
typedef struct
{
    const char *profile_id;
    emaster_operation_profile_approval_t approval;
    const char *device_profile_id;
    const char *pdo_set_id;
    emaster_sync_strategy_t sync_strategy;
    bool has_assign_activate;
    uint32_t assign_activate;
    bool has_cycle_ns;
    uint32_t cycle_ns;
    bool has_sync0_shift_ns;
    int32_t sync0_shift_ns;
    bool has_sm2_sync_type;
    uint16_t sm2_sync_type;
    bool has_sm3_sync_type;
    uint16_t sm3_sync_type;
    /* 草案可以不选择模式；已批准方案必须指向下方 modes 中的一项。 */
    const char *selected_mode_id;
    const emaster_operation_mode_t *modes;
    size_t mode_count;
} emaster_operation_profile_t;

/* 每轴只保存工程单位下的命令和边界；运行时必须使用该轴真机换算参数生成 PDO 原始值。 */
typedef struct
{
    const char *axis_id;
    int32_t relative_angle_millidegrees;
    uint32_t max_following_error_millidegrees;
    /* 执行前必须与当前物理从站的 608F/6091 读回完全一致。 */
    uint32_t expected_encoder_increments;
    uint32_t expected_encoder_motor_revolutions;
    uint32_t expected_gear_motor_revolutions;
    uint32_t expected_gear_shaft_revolutions;
} emaster_motion_axis_config_t;

/* 一份运动方案描述一次全轴原子发布的轨迹，不能只覆盖部署中的部分轴。 */
typedef struct
{
    const char *motion_profile_id;
    emaster_motion_profile_approval_t approval;
    const char *required_mode_id;
    emaster_motion_trajectory_t trajectory;
    emaster_motion_coordinate_t coordinate;
    uint32_t duration_ms;
    uint32_t settle_ms;
    const emaster_motion_axis_config_t *axes;
    size_t axis_count;
} emaster_motion_profile_t;

/*
 * 拓扑条目只保存用户在拓扑配置中确认的静态关系。position 是总线顺序，
 * profile_id 用于在设备目录中选择身份和 PDO 事实；本模块不解释过程数据。
 */
typedef struct
{
    uint16_t position;
    const char *axis_id;
    const char *profile_id;
} emaster_topology_slave_config_t;

/* status 原样保存用户给出的配置事实，基础设施不自行解释审批语义。 */
typedef struct
{
    const char *topology_id;
    const char *status;
    const emaster_topology_slave_config_t *slaves;
    size_t slave_count;
} emaster_topology_config_t;

/* 部署配置把主机、专用 EtherCAT 网口和逻辑拓扑绑定在一起。 */
typedef struct
{
    const char *deployment_id;
    const char *hostname;
    const char *ethercat_interface;
    const char *management_interface;
    const emaster_topology_config_t *topology;
    const emaster_operation_profile_t *const *operation_profiles;
    size_t operation_profile_count;
    const emaster_motion_profile_t *motion_profile;
} emaster_deployment_config_t;

/* 以下接口只返回生成目录中的只读对象，调用者不得释放或修改返回值。 */
size_t emaster_topology_config_count(void);
const emaster_topology_config_t *emaster_topology_config_at(size_t index);
const emaster_topology_config_t *emaster_topology_config_by_id(const char *topology_id);

size_t emaster_deployment_config_count(void);
const emaster_deployment_config_t *emaster_deployment_config_at(size_t index);
const emaster_deployment_config_t *emaster_deployment_config_by_id(const char *deployment_id);

size_t emaster_operation_profile_count(void);
const emaster_operation_profile_t *emaster_operation_profile_at(size_t index);
const emaster_operation_profile_t *emaster_operation_profile_by_id(const char *profile_id);
const emaster_operation_profile_t *emaster_deployment_operation_profile_by_id(
    const emaster_deployment_config_t *deployment, const char *profile_id);

size_t emaster_motion_profile_count(void);
const emaster_motion_profile_t *emaster_motion_profile_at(size_t index);
const emaster_motion_profile_t *emaster_motion_profile_by_id(const char *profile_id);

#endif
