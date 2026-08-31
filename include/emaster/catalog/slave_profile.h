#ifndef EMASTER_CATALOG_SLAVE_PROFILE_H
#define EMASTER_CATALOG_SLAVE_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "emaster/types.h"

typedef struct
{
    /* profile_id 是配置间引用使用的稳定键；model 仅用于显示和证据核对。 */
    const char *profile_id;
    const char *model;
    emaster_slave_identity_t identity;

    /* 这里保存经审查的默认 PDO 布局；实际从站映射仍须在进入过程数据阶段前核验。 */
    uint16_t rx_pdo_index;
    uint16_t tx_pdo_index;
    uint16_t rx_pdo_bytes;
    uint16_t tx_pdo_bytes;
    /* 默认换算值来自设备资料，不能替代每台物理从站的指纹事实。 */
    uint32_t encoder_counts_per_motor_revolution_default;
    bool requires_distributed_clocks;
} emaster_slave_profile_t;

/* 精确比较身份三元组；空指针和任何字段不一致均返回 false。 */
bool emaster_slave_identity_matches(const emaster_slave_profile_t *profile,
                                    const emaster_slave_identity_t *actual);

/* 以下接口只返回生成目录中的只读对象，调用者不得释放或修改返回值。 */
const emaster_slave_profile_t *emaster_slave_profile_by_id(const char *profile_id);
size_t emaster_slave_profile_count(void);
const emaster_slave_profile_t *emaster_slave_profile_at(size_t index);

#endif
