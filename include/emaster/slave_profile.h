#ifndef EMASTER_SLAVE_PROFILE_H
#define EMASTER_SLAVE_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision;
} emaster_slave_identity_t;

typedef struct
{
    const char *profile_id;
    const char *model;
    emaster_slave_identity_t identity;
    uint16_t rx_pdo_index;
    uint16_t tx_pdo_index;
    uint16_t rx_pdo_bytes;
    uint16_t tx_pdo_bytes;
    uint32_t encoder_counts_per_motor_revolution_default;
    bool requires_distributed_clocks;
} emaster_slave_profile_t;

bool emaster_slave_identity_matches(const emaster_slave_profile_t *profile,
                                    const emaster_slave_identity_t *actual);

const emaster_slave_profile_t *emaster_slave_profile_by_id(const char *profile_id);
size_t emaster_slave_profile_count(void);
const emaster_slave_profile_t *emaster_slave_profile_at(size_t index);

#endif
