#ifndef EMASTER_CATALOG_SLAVE_PROFILE_H
#define EMASTER_CATALOG_SLAVE_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "emaster/protocol/pdo_layout.h"
#include "emaster/types.h"

typedef struct
{
    uint16_t index;
    uint8_t subindex;
    uint8_t bit_length;
    const char *data_type;
    const char *name;
} emaster_pdo_entry_t;

typedef struct
{
    uint16_t index;
    const emaster_pdo_entry_t *entries;
    size_t entry_count;
    uint32_t bit_length;
} emaster_pdo_mapping_profile_t;

/*
 * 一套 PDO 方案在总线上怎么被选中。
 *
 * 模块化设备（用 <Module> 声明 PDO 的那些）先往 0xF030 的模块槽位写模块标识，
 * 再分配 1C12/1C13；非模块化设备没有模块槽位这个对象，只有 1C12/1C13 可写，
 * 写模块标识那一步会以 SDO 中止告终。
 *
 * 取 0 为模块化：设备目录里没有这个字段时，行为与加它之前完全一致。
 */
typedef enum
{
    EMASTER_PDO_ASSIGNMENT_MODULE_SLOT = 0,
    EMASTER_PDO_ASSIGNMENT_DIRECT = 1
} emaster_pdo_assignment_style_t;

/* 一个设备可以声明多个 PDO 方案；运行方案负责选择其中一个。 */
typedef struct
{
    const char *pdo_set_id;
    /* 直连分配时这两个字段没有含义，恒为 0。 */
    uint32_t module_ident;
    uint8_t module_slot;
    emaster_pdo_assignment_style_t assignment_style;
    uint16_t rx_pdo_bytes;
    uint16_t tx_pdo_bytes;
    const emaster_pdo_mapping_profile_t *rx_mappings;
    size_t rx_mapping_count;
    const emaster_pdo_mapping_profile_t *tx_mappings;
    size_t tx_mapping_count;
    /* 固定模块可声明在 SAFE-OP 到 OP 过渡时执行的模式初始化。 */
    bool mode_init_on_safeop_to_op;
    uint16_t mode_init_index;
    uint8_t mode_init_subindex;
    int8_t mode_init_value;
} emaster_pdo_set_profile_t;

/*
 * 慢速遥测对象在总线上的字面宽度。这里不复用 emaster/config/runtime_config.h 的
 * emaster_config_sdo_value_type_t：设备目录只链接 emaster::protocol，不得反向依赖运行
 * 配置层，否则设备事实会被运行方案拉进链接依赖。两套枚举各自独立，语义由使用处解释。
 */
typedef enum
{
    EMASTER_TELEMETRY_TYPE_U8 = 0,
    EMASTER_TELEMETRY_TYPE_I8,
    EMASTER_TELEMETRY_TYPE_U16,
    EMASTER_TELEMETRY_TYPE_I16,
    EMASTER_TELEMETRY_TYPE_U32,
    EMASTER_TELEMETRY_TYPE_I32
} emaster_telemetry_type_t;

/* 语义决定这个对象落到会话结果的哪个命名槽位；通用总线层不认识供应商对象号。 */
typedef enum
{
    EMASTER_TELEMETRY_NONE = 0,
    EMASTER_TELEMETRY_ACTUAL_CURRENT,
    EMASTER_TELEMETRY_ERROR_CODE,
    EMASTER_TELEMETRY_BUS_VOLTAGE,
    EMASTER_TELEMETRY_MOSFET_TEMPERATURE,
    EMASTER_TELEMETRY_MOTOR_TEMPERATURE,
    EMASTER_TELEMETRY_ACTUAL_VELOCITY,
    EMASTER_TELEMETRY_TARGET_VELOCITY,
    EMASTER_TELEMETRY_EXTENDED_ERROR_CODE,
    EMASTER_TELEMETRY_SERVO_ERROR_CODE,
    /*
     * 驱动器**自己持有的**目标位置（0x607A）。与 TxPDO 里那几个反馈不是一类东西：
     * 反馈说的是"它现在在哪"，这一条说的是"它以为你要它去哪"。
     *
     * 两个值在报告里必须分得开。会话结果里的 target_position 是**主站写进过程映像**
     * 的那个数，主站永远知道自己写了什么；它说明不了驱动器有没有把那一串字节收下并
     * 用起来。排查"轴不动"时正是卡在这一步：主站写了一个扫过全行程的目标、WKC 24/24
     * 说明所有从站都回了帧，而轴一步没跟——这时只有驱动器自报的目标能切开两种情形
     * （P12.41）：
     *   读到的跟着主站的指令一起扫 ⇒ 目标到了驱动器，是它不执行（功率级/使能/电机侧）；
     *   读到的一直是旧值       ⇒ 值在"过程映像"到"驱动器应用"之间就断了，与电机无关。
     * 因此名字里带 readback，不与 target_position 混用。
     */
    EMASTER_TELEMETRY_TARGET_POSITION_READBACK,
    /*
     * 两个**配置型**对象的运行期读数：最大力矩 0x6072、最大电机转速 0x6080。
     *
     * 为什么必须能在 OP 期间读。它们在停机后也读得到（post_cycle_diagnostic），但那是
     * 停用之后的值；而 0x6072 恰恰是主站在 SAFE-OP 进 OP 时**写**进去的（运行方案的
     * safeop_to_op_sdo_writes），所以"写进去没有、运行时到底是多少"只有 OP 期间的读数
     * 能回答。
     *
     * 这两个语义**不进报告**：慢速遥测的槽位与报告字段是给"会随时间变"的量准备的，
     * 而这两个是静态配置值，停机后那一次读已经覆盖了报告侧的需要。本语义只让观测线程
     * 在 OP 期间把它们读一遍，读数落在审计段（带 phase=cyclic_operation）——与
     * extended／servo error code 同一个形状，见下面 observer_store_telemetry 的说明。
     */
    EMASTER_TELEMETRY_MAX_TORQUE,
    EMASTER_TELEMETRY_MAX_MOTOR_SPEED
} emaster_telemetry_semantic_t;

/* 一个供应商私有遥测对象：对象号来自设备配置，解释权归声明它的设备。 */
typedef struct
{
    const char *name;
    uint16_t index;
    uint8_t subindex;
    emaster_telemetry_type_t type;
    emaster_telemetry_semantic_t semantic;
    const char *unit;
} emaster_slow_telemetry_t;

typedef struct
{
    /* profile_id 是配置间引用使用的稳定键；model 仅用于显示和证据核对。 */
    const char *profile_id;
    const char *model;
    emaster_slave_identity_t identity;

    /* reference_pdo_set_id 只用于当前指纹基线的静态参照，不是运行时选择。 */
    const char *reference_pdo_set_id;
    const emaster_pdo_set_profile_t *pdo_sets;
    size_t pdo_set_count;
    /*
     * 设备资料给的换算与方向事实（设备档的 conversion）。设备实现了 608F/6091/607E
     * 时以运行期读回为准，这三项只作对账；设备不实现这些对象时它们接手，报告写明
     * 来源是"设备档声明"。缺省 0/0/-1 的含义是"资料里没有这条"，不是"每转 0 个计数"。
     * polarity_default 是 607E 的取值 0/1，-1 表示设备档没声明方向。
     */
    uint32_t encoder_counts_per_motor_revolution_default;
    uint32_t gear_motor_revolutions_default;
    uint32_t gear_shaft_revolutions_default;
    int8_t polarity_default;
    /* 这是 ESI 的 CoE/PdoConfig 能力；为 false 时生命周期层不得尝试 SDO 重映射。 */
    /* PdoAssign 与 PdoConfig 是两个独立能力，不能混为一个开关。 */
    bool supports_pdo_assignment;
    bool supports_pdo_configuration;
    /* 这是 ESI 声明的能力，不表示所有运行方案都必须启用 DC。 */
    bool supports_distributed_clocks;
    /* 供应商状态字位条件仅作设备诊断，停用完成由完整 CiA 402 状态确认。 */
    uint16_t safe_stop_status_mask;
    uint16_t safe_stop_status_value;
    /* 慢速遥测对象字典。设备不需要就留空；通用代码遍历它，不写死对象号。 */
    const emaster_slow_telemetry_t *slow_telemetry;
    size_t slow_telemetry_count;
} emaster_slave_profile_t;

/* 精确比较身份三元组；空指针和任何字段不一致均返回 false。 */
bool emaster_slave_identity_matches(const emaster_slave_profile_t *profile,
                                    const emaster_slave_identity_t *actual);

/*
 * 严格比较从站当前生效的完整 PDO 映射与经 ESI 校验的设备目录。数量、顺序、索引、
 * 子索引、位宽和方向总长度均须一致；函数只比较内存，不访问总线或修改从站。
 */
bool emaster_slave_pdo_layout_matches(const emaster_slave_profile_t *profile,
                                      const emaster_pdo_layout_t *actual);

/* 按稳定 ID 查找设备声明的 PDO 方案，并严格比较实际布局。 */
const emaster_pdo_set_profile_t *emaster_slave_pdo_set_by_id(
    const emaster_slave_profile_t *profile, const char *pdo_set_id);
bool emaster_slave_pdo_set_layout_matches(const emaster_pdo_set_profile_t *pdo_set,
                                          const emaster_pdo_layout_t *actual);

/* 设备实际映射里与设备档声明不同的一条，按位置对齐后逐条记下。 */
typedef struct
{
    uint16_t mapping_index;
    uint8_t subindex;
    uint32_t declared_descriptor;
    uint32_t device_descriptor;
} emaster_pdo_layout_difference_t;

/*
 * 逐条列出"设备实际与设备档声明不同"的位置，供报告写明主站到底采用了什么。
 * 返回差异总数（可能大于 capacity，实际写出的最多 capacity 条）。
 *
 * 有了这张清单，"按设备实际建映像"才不是一句无据的话：哪条不同、声明是什么、设备
 * 是什么，报告里都看得到。差异本身不拦会话 —— 拦不拦由会话层的可用性判定说了算。
 */
size_t emaster_slave_pdo_layout_differences(const emaster_pdo_set_profile_t *pdo_set,
                                            const emaster_pdo_layout_t *actual,
                                            emaster_pdo_layout_difference_t *out,
                                            size_t capacity);
const emaster_pdo_set_profile_t *emaster_slave_reference_pdo_set(
    const emaster_slave_profile_t *profile);

/* 以下接口只返回生成目录中的只读对象，调用者不得释放或修改返回值。 */
const emaster_slave_profile_t *emaster_slave_profile_by_id(const char *profile_id);
size_t emaster_slave_profile_count(void);
const emaster_slave_profile_t *emaster_slave_profile_at(size_t index);

#endif
