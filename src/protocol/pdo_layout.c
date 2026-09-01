#include "emaster/protocol/pdo_layout.h"

#include <stdlib.h>
#include <string.h>

/* 这些索引由 ETG CoE PDO 分配模型规定，不是某台设备或某次部署的参数。 */
enum
{
    EMASTER_RXPDO_ASSIGNMENT_INDEX = 0x1C12,
    EMASTER_TXPDO_ASSIGNMENT_INDEX = 0x1C13
};

static void destroy_direction(emaster_pdo_direction_layout_t *direction)
{
    size_t mapping_index;

    if (direction == NULL)
    {
        return;
    }
    for (mapping_index = 0U;
         direction->mappings != NULL && mapping_index < direction->mapping_count;
         ++mapping_index)
    {
        free(direction->mappings[mapping_index].entries);
    }
    free(direction->mappings);
    memset(direction, 0, sizeof(*direction));
}

void emaster_pdo_layout_destroy(emaster_pdo_layout_t *layout)
{
    if (layout == NULL)
    {
        return;
    }
    destroy_direction(&layout->rx);
    destroy_direction(&layout->tx);
    memset(layout, 0, sizeof(*layout));
}

static emaster_pdo_discovery_status_t fail(emaster_pdo_layout_t *layout,
                                            emaster_pdo_discovery_status_t status,
                                            uint16_t index, uint8_t subindex)
{
    layout->status = status;
    layout->failed_index = index;
    layout->failed_subindex = subindex;
    return status;
}

static emaster_pdo_discovery_status_t
discover_direction(const emaster_pdo_sdo_reader_t *reader, uint16_t assignment_index,
                   emaster_pdo_direction_layout_t *direction, emaster_pdo_layout_t *layout)
{
    uint8_t mapping_count;
    uint32_t direction_offset = 0U;
    size_t mapping_ordinal;

    direction->assignment_index = assignment_index;
    if (!reader->read_u8(reader->user_data, assignment_index, UINT8_C(0), &mapping_count))
    {
        return fail(layout, EMASTER_PDO_DISCOVERY_READ_FAILED, assignment_index, UINT8_C(0));
    }
    direction->mapping_count = mapping_count;
    if (mapping_count == UINT8_C(0))
    {
        return EMASTER_PDO_DISCOVERY_COMPLETE;
    }

    direction->mappings = calloc((size_t)mapping_count, sizeof(*direction->mappings));
    if (direction->mappings == NULL)
    {
        return fail(layout, EMASTER_PDO_DISCOVERY_OUT_OF_MEMORY, assignment_index,
                    UINT8_C(0));
    }

    for (mapping_ordinal = 0U; mapping_ordinal < (size_t)mapping_count; ++mapping_ordinal)
    {
        emaster_pdo_mapping_t *mapping = &direction->mappings[mapping_ordinal];
        uint8_t assignment_subindex = (uint8_t)(mapping_ordinal + 1U);
        uint8_t entry_count;
        size_t entry_ordinal;

        if (!reader->read_u16(reader->user_data, assignment_index, assignment_subindex,
                              &mapping->mapping_index))
        {
            return fail(layout, EMASTER_PDO_DISCOVERY_READ_FAILED, assignment_index,
                        assignment_subindex);
        }
        if (mapping->mapping_index == UINT16_C(0) ||
            !reader->read_u8(reader->user_data, mapping->mapping_index, UINT8_C(0),
                             &entry_count))
        {
            return fail(layout,
                        mapping->mapping_index == UINT16_C(0)
                            ? EMASTER_PDO_DISCOVERY_INVALID_DESCRIPTOR
                            : EMASTER_PDO_DISCOVERY_READ_FAILED,
                        mapping->mapping_index, UINT8_C(0));
        }
        mapping->entry_count = entry_count;
        if (entry_count == UINT8_C(0))
        {
            continue;
        }
        mapping->entries = calloc((size_t)entry_count, sizeof(*mapping->entries));
        if (mapping->entries == NULL)
        {
            return fail(layout, EMASTER_PDO_DISCOVERY_OUT_OF_MEMORY, mapping->mapping_index,
                        UINT8_C(0));
        }

        for (entry_ordinal = 0U; entry_ordinal < (size_t)entry_count; ++entry_ordinal)
        {
            emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];
            uint8_t mapping_subindex = (uint8_t)(entry_ordinal + 1U);

            if (!reader->read_u32(reader->user_data, mapping->mapping_index,
                                  mapping_subindex, &entry->raw_descriptor))
            {
                return fail(layout, EMASTER_PDO_DISCOVERY_READ_FAILED,
                            mapping->mapping_index, mapping_subindex);
            }
            entry->mapping_subindex = mapping_subindex;
            entry->object_index = (uint16_t)(entry->raw_descriptor >> 16U);
            entry->object_subindex = (uint8_t)(entry->raw_descriptor >> 8U);
            entry->bit_length = (uint8_t)entry->raw_descriptor;
            entry->bit_offset = direction_offset;
            if (entry->bit_length == UINT8_C(0) ||
                direction_offset > UINT32_MAX - (uint32_t)entry->bit_length)
            {
                return fail(layout, EMASTER_PDO_DISCOVERY_INVALID_DESCRIPTOR,
                            mapping->mapping_index, mapping_subindex);
            }
            mapping->bit_length += (uint32_t)entry->bit_length;
            direction_offset += (uint32_t)entry->bit_length;
        }
    }
    direction->bit_length = direction_offset;
    return EMASTER_PDO_DISCOVERY_COMPLETE;
}

emaster_pdo_discovery_status_t
emaster_pdo_layout_discover(const emaster_pdo_sdo_reader_t *reader,
                            emaster_pdo_layout_t *result)
{
    emaster_pdo_discovery_status_t status;

    if (result == NULL)
    {
        return EMASTER_PDO_DISCOVERY_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    if (reader == NULL || reader->read_u8 == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL)
    {
        result->status = EMASTER_PDO_DISCOVERY_INVALID_ARGUMENT;
        return result->status;
    }

    status = discover_direction(reader, (uint16_t)EMASTER_RXPDO_ASSIGNMENT_INDEX,
                                &result->rx, result);
    if (status == EMASTER_PDO_DISCOVERY_COMPLETE)
    {
        status = discover_direction(reader, (uint16_t)EMASTER_TXPDO_ASSIGNMENT_INDEX,
                                    &result->tx, result);
    }
    result->status = status;
    return status;
}
