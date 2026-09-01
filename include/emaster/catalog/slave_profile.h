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

typedef struct
{
    /* profile_id 是配置间引用使用的稳定键；model 仅用于显示和证据核对。 */
    const char *profile_id;
    const char *model;
    emaster_slave_identity_t identity;

    /* 这里保存经审查的默认 PDO 布局；实际从站映射仍须在进入过程数据阶段前核验。 */
    uint16_t rx_pdo_bytes;
    uint16_t tx_pdo_bytes;
    const emaster_pdo_mapping_profile_t *rx_pdo_mappings;
    size_t rx_pdo_mapping_count;
    const emaster_pdo_mapping_profile_t *tx_pdo_mappings;
    size_t tx_pdo_mapping_count;
    /* 默认换算值来自设备资料，不能替代每台物理从站的指纹事实。 */
    uint32_t encoder_counts_per_motor_revolution_default;
    bool requires_distributed_clocks;
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

/* 以下接口只返回生成目录中的只读对象，调用者不得释放或修改返回值。 */
const emaster_slave_profile_t *emaster_slave_profile_by_id(const char *profile_id);
size_t emaster_slave_profile_count(void);
const emaster_slave_profile_t *emaster_slave_profile_at(size_t index);

#endif
