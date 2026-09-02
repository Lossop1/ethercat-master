#include "emaster/audit/run_audit.h"

#include <stdlib.h>
#include <string.h>

static emaster_audit_access_t *append_access(emaster_run_audit_t *audit)
{
    emaster_audit_access_t *resized;
    size_t new_capacity;

    if (audit == NULL || audit->allocation_failed)
    {
        return NULL;
    }
    if (audit->access_count == audit->access_capacity)
    {
        if (audit->capacity_sealed)
        {
            audit->allocation_failed = true;
            return NULL;
        }
        new_capacity = audit->access_capacity == 0U ? 64U : audit->access_capacity * 2U;
        if (new_capacity < audit->access_capacity ||
            new_capacity > SIZE_MAX / sizeof(*audit->accesses))
        {
            audit->allocation_failed = true;
            return NULL;
        }
        resized = realloc(audit->accesses, new_capacity * sizeof(*audit->accesses));
        if (resized == NULL)
        {
            audit->allocation_failed = true;
            return NULL;
        }
        audit->accesses = resized;
        audit->access_capacity = new_capacity;
    }
    memset(&audit->accesses[audit->access_count], 0,
           sizeof(audit->accesses[audit->access_count]));
    return &audit->accesses[audit->access_count++];
}

bool emaster_run_audit_reserve(emaster_run_audit_t *audit, size_t capacity)
{
    emaster_audit_access_t *resized;

    if (audit == NULL || audit->capacity_sealed || audit->allocation_failed)
    {
        return false;
    }
    if (capacity <= audit->access_capacity)
    {
        return true;
    }
    if (capacity > SIZE_MAX / sizeof(*audit->accesses))
    {
        audit->allocation_failed = true;
        return false;
    }
    resized = realloc(audit->accesses, capacity * sizeof(*audit->accesses));
    if (resized == NULL)
    {
        audit->allocation_failed = true;
        return false;
    }
    audit->accesses = resized;
    audit->access_capacity = capacity;
    return true;
}

void emaster_run_audit_seal_capacity(emaster_run_audit_t *audit)
{
    if (audit != NULL)
    {
        audit->capacity_sealed = true;
    }
}

void emaster_run_audit_init(emaster_run_audit_t *audit)
{
    if (audit != NULL)
    {
        memset(audit, 0, sizeof(*audit));
    }
}

void emaster_run_audit_destroy(emaster_run_audit_t *audit)
{
    if (audit == NULL)
    {
        return;
    }
    free(audit->accesses);
    memset(audit, 0, sizeof(*audit));
}

bool emaster_run_audit_record_access(
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    emaster_audit_transport_t transport,
    emaster_audit_direction_t direction,
    emaster_audit_value_kind_t value_kind,
    uint16_t slave_position,
    uint16_t index,
    uint8_t subindex,
    uint8_t bit_length,
    uint32_t bit_offset,
    uint64_t exchange,
    const void *raw,
    uint8_t raw_size,
    bool succeeded,
    uint64_t unsigned_value,
    int64_t signed_value)
{
    emaster_audit_access_t *access;

    if (audit == NULL || raw_size > sizeof(((emaster_audit_access_t *)0)->raw) ||
        (raw_size > 0U && raw == NULL))
    {
        return false;
    }
    access = append_access(audit);
    if (access == NULL)
    {
        return false;
    }
    access->order = audit->next_order++;
    access->sample_count = 1U;
    access->first_exchange = exchange;
    access->last_exchange = exchange;
    access->phase = phase;
    access->transport = transport;
    access->direction = direction;
    access->value_kind = value_kind;
    access->slave_position = slave_position;
    access->index = index;
    access->subindex = subindex;
    access->bit_length = bit_length;
    access->bit_offset = bit_offset;
    access->raw_size = raw_size;
    access->succeeded = succeeded;
    access->unsigned_value = unsigned_value;
    access->signed_value = signed_value;
    if (raw_size > 0U)
    {
        memcpy(access->raw, raw, raw_size);
    }
    return true;
}

static bool same_pdo_field(const emaster_audit_access_t *access,
                           emaster_audit_phase_t phase,
                           emaster_audit_direction_t direction,
                           emaster_audit_value_kind_t value_kind,
                           uint16_t slave_position,
                           uint16_t index,
                           uint8_t subindex,
                           uint8_t bit_length,
                           uint32_t bit_offset)
{
    return access->transport == EMASTER_AUDIT_TRANSPORT_PDO &&
           access->phase == phase && access->direction == direction &&
           access->value_kind == value_kind &&
           access->slave_position == slave_position && access->index == index &&
           access->subindex == subindex && access->bit_length == bit_length &&
           access->bit_offset == bit_offset;
}

bool emaster_run_audit_record_pdo(
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    emaster_audit_direction_t direction,
    emaster_audit_value_kind_t value_kind,
    uint16_t slave_position,
    uint16_t index,
    uint8_t subindex,
    uint8_t bit_length,
    uint32_t bit_offset,
    uint64_t exchange,
    uint64_t unsigned_value,
    int64_t signed_value)
{
    size_t cursor;
    emaster_audit_access_t *access;

    if (audit == NULL)
    {
        return false;
    }
    cursor = audit->access_count;
    while (cursor > 0U)
    {
        access = &audit->accesses[--cursor];
        if (same_pdo_field(access, phase, direction, value_kind, slave_position,
                           index, subindex, bit_length, bit_offset))
        {
            if (access->unsigned_value == unsigned_value &&
                access->signed_value == signed_value && access->succeeded)
            {
                access->last_exchange = exchange;
                ++access->sample_count;
                return true;
            }
            break;
        }
    }
    access = append_access(audit);
    if (access == NULL)
    {
        return false;
    }
    access->order = audit->next_order++;
    access->first_exchange = exchange;
    access->last_exchange = exchange;
    access->sample_count = 1U;
    access->phase = phase;
    access->transport = EMASTER_AUDIT_TRANSPORT_PDO;
    access->direction = direction;
    access->value_kind = value_kind;
    access->slave_position = slave_position;
    access->index = index;
    access->subindex = subindex;
    access->bit_length = bit_length;
    access->bit_offset = bit_offset;
    access->succeeded = true;
    access->unsigned_value = unsigned_value;
    access->signed_value = signed_value;
    return true;
}
