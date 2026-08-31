#ifndef EMASTER_TYPES_H
#define EMASTER_TYPES_H

#include <stdint.h>

typedef struct
{
    /* 三元组来自从站 SII/CoE 身份对象，用于精确选择设备配置。 */
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision;
} emaster_slave_identity_t;

#endif
