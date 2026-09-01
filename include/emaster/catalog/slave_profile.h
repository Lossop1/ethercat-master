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

/* 一个设备可以声明多个 PDO 方案；运行方案负责选择其中一个。 */
typedef struct
{
    const char *pdo_set_id;
    uint32_t module_ident;
    uint16_t rx_pdo_bytes;
    uint16_t tx_pdo_bytes;
    const emaster_pdo_mapping_profile_t *rx_mappings;
    size_t rx_mapping_count;
    const emaster_pdo_mapping_profile_t *tx_mappings;
    size_t tx_mapping_count;
} emaster_pdo_set_profile_t;

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
    /* 默认换算值来自设备资料，不能替代每台物理从站的指纹事实。 */
    uint32_t encoder_counts_per_motor_revolution_default;
    /* 这是 ESI 的 CoE/PdoConfig 能力；为 false 时生命周期层不得尝试 SDO 重映射。 */
    bool supports_pdo_configuration;
    /* 这是 ESI 声明的能力，不表示所有运行方案都必须启用 DC。 */
    bool supports_distributed_clocks;
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
const emaster_pdo_set_profile_t *emaster_slave_reference_pdo_set(
    const emaster_slave_profile_t *profile);

/* 以下接口只返回生成目录中的只读对象，调用者不得释放或修改返回值。 */
const emaster_slave_profile_t *emaster_slave_profile_by_id(const char *profile_id);
size_t emaster_slave_profile_count(void);
const emaster_slave_profile_t *emaster_slave_profile_at(size_t index);

#endif
