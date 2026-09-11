#ifndef EMASTER_CIA402_CONTROLLER_H
#define EMASTER_CIA402_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

/* CiA 402 状态字的标准状态集合；未知组合必须作为阻断状态处理。 */
typedef enum
{
    EMASTER_CIA402_STATE_UNKNOWN = 0,
    EMASTER_CIA402_STATE_NOT_READY_TO_SWITCH_ON,
    EMASTER_CIA402_STATE_SWITCH_ON_DISABLED,
    EMASTER_CIA402_STATE_READY_TO_SWITCH_ON,
    EMASTER_CIA402_STATE_SWITCHED_ON,
    EMASTER_CIA402_STATE_OPERATION_ENABLED,
    EMASTER_CIA402_STATE_QUICK_STOP_ACTIVE,
    EMASTER_CIA402_STATE_FAULT_REACTION_ACTIVE,
    EMASTER_CIA402_STATE_FAULT
} emaster_cia402_state_t;

/* 目标是生命周期目标，不包含位置、速度或转矩命令。 */
typedef enum
{
    EMASTER_CIA402_GOAL_SAFE_STOP = 0,
    EMASTER_CIA402_GOAL_READY_TO_SWITCH_ON,
    EMASTER_CIA402_GOAL_SWITCHED_ON,
    EMASTER_CIA402_GOAL_OPERATION_ENABLED,
    /* 主动触发 Quick Stop：向驱动器发 0x0002，使其进入 Quick Stop Active 状态。
     * 驱动器自行执行减速；恢复时将目标改回 OPERATION_ENABLED。 */
    EMASTER_CIA402_GOAL_QUICK_STOP
} emaster_cia402_goal_t;

/* 状态机只保存单轴的确定性控制状态，不拥有任何配置或过程数据缓冲区。 */
typedef struct
{
    emaster_cia402_goal_t goal;
    bool fault_reset_requested;
    /* Halt 位（控制字 bit8）：置位时在 Enable Operation 控制字上叠加 0x0100，
     * 驱动器减速至零后保持使能；清零后恢复跟随目标。持续有效，非单次脉冲。 */
    bool halt_active;
} emaster_cia402_controller_t;

/* 每周期输出的控制字规划结果；调用者负责把控制字编码到已确认的 RxPDO。 */
typedef struct
{
    uint16_t control_word;
    emaster_cia402_state_t observed_state;
    bool state_known;
    bool goal_reached;
    bool fault_present;
    bool fault_reset_pulse;
} emaster_cia402_output_t;

/* 6041 中不属于 PDS FSA 掩码的通用状态，单独解析供运行监督使用 */
typedef struct
{
    emaster_cia402_state_t state;
    bool state_known;
    bool voltage_enabled;
    bool warning;
    bool remote;
    bool target_reached;
    bool internal_limit_active;
    uint8_t mode_specific_bits;
    uint8_t manufacturer_specific_bits;
} emaster_cia402_status_t;

/* 初始化为安全停止目标；不会产生任何总线访问。 */
void emaster_cia402_controller_init(emaster_cia402_controller_t *controller);

/* 设置生命周期目标；无效目标返回 false，且不修改原状态。 */
bool emaster_cia402_controller_set_goal(emaster_cia402_controller_t *controller,
                                         emaster_cia402_goal_t goal);

/* 请求下一次观察到 Fault 时发送一个周期的标准故障复位脉冲。 */
void emaster_cia402_controller_request_fault_reset(
    emaster_cia402_controller_t *controller);

/* 设置或清除 Halt 位（控制字 bit8）。置位后驱动器减速至零并保持使能；
 * 清零后恢复跟随目标。持续有效，非单次脉冲。 */
void emaster_cia402_controller_set_halt(emaster_cia402_controller_t *controller,
                                        bool active);

/*
 * 按当前状态字规划一个周期的控制字。该函数不自动推进目标、不处理超时、不读取时钟，
 * 也不自动发起故障复位；通信监督器和安全层必须在外部决定是否允许调用及是否继续发送。
 */
bool emaster_cia402_controller_step(emaster_cia402_controller_t *controller,
                                     uint16_t status_word,
                                     emaster_cia402_output_t *output);

/* 只解析标准状态字掩码，不访问总线或解释设备厂商扩展位。 */
bool emaster_cia402_decode_status_word(uint16_t status_word,
                                       emaster_cia402_state_t *state);

/* 一次解析完整的 6041 通用语义，模式专用位保持为原始两位值 */
bool emaster_cia402_decode_status(uint16_t status_word,
                                 emaster_cia402_status_t *status);

#endif
