#include "emaster/catalog/slave_profile.h"

#include <string.h>

bool emaster_slave_identity_matches(const emaster_slave_profile_t *profile,
                                    const emaster_slave_identity_t *actual)
{
    if (profile == NULL || actual == NULL)
    {
        return false;
    }

    return profile->identity.vendor_id == actual->vendor_id &&
           profile->identity.product_code == actual->product_code &&
           profile->identity.revision == actual->revision;
}

static bool direction_matches(const emaster_pdo_mapping_profile_t *expected_mappings,
                              size_t expected_mapping_count, uint16_t expected_bytes,
                              const emaster_pdo_direction_layout_t *actual)
{
    size_t mapping_ordinal;

    if (actual == NULL || expected_mappings == NULL ||
        actual->mapping_count != expected_mapping_count ||
        actual->bit_length != (uint32_t)expected_bytes * UINT32_C(8) ||
        (actual->mapping_count > 0U && actual->mappings == NULL))
    {
        return false;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < expected_mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_profile_t *expected = &expected_mappings[mapping_ordinal];
        const emaster_pdo_mapping_t *observed = &actual->mappings[mapping_ordinal];
        uint32_t expected_offset = 0U;
        size_t entry_ordinal;

        if (observed->mapping_index != expected->index ||
            observed->entry_count != expected->entry_count ||
            observed->bit_length != expected->bit_length ||
            (observed->entry_count > 0U &&
             (observed->entries == NULL || expected->entries == NULL)))
        {
            return false;
        }
        for (entry_ordinal = 0U; entry_ordinal < expected->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_entry_t *expected_entry = &expected->entries[entry_ordinal];
            const emaster_pdo_mapping_entry_t *observed_entry =
                &observed->entries[entry_ordinal];
            uint32_t descriptor = ((uint32_t)expected_entry->index << 16U) |
                                  ((uint32_t)expected_entry->subindex << 8U) |
                                  (uint32_t)expected_entry->bit_length;

            if (observed_entry->mapping_subindex != (uint8_t)(entry_ordinal + 1U) ||
                observed_entry->raw_descriptor != descriptor ||
                observed_entry->object_index != expected_entry->index ||
                observed_entry->object_subindex != expected_entry->subindex ||
                observed_entry->bit_length != expected_entry->bit_length ||
                observed_entry->bit_offset != expected_offset)
            {
                return false;
            }
            expected_offset += (uint32_t)expected_entry->bit_length;
        }
    }
    return true;
}

bool emaster_slave_pdo_set_layout_matches(const emaster_pdo_set_profile_t *pdo_set,
                                          const emaster_pdo_layout_t *actual)
{
    return pdo_set != NULL && actual != NULL &&
           actual->status == EMASTER_PDO_DISCOVERY_COMPLETE &&
           direction_matches(pdo_set->rx_mappings, pdo_set->rx_mapping_count,
                             pdo_set->rx_pdo_bytes, &actual->rx) &&
           direction_matches(pdo_set->tx_mappings, pdo_set->tx_mapping_count,
                             pdo_set->tx_pdo_bytes, &actual->tx);
}

const emaster_pdo_set_profile_t *emaster_slave_pdo_set_by_id(
    const emaster_slave_profile_t *profile, const char *pdo_set_id)
{
    size_t index;

    if (profile == NULL || pdo_set_id == NULL || profile->pdo_sets == NULL)
    {
        return NULL;
    }

    for (index = 0U; index < profile->pdo_set_count; ++index)
    {
        const emaster_pdo_set_profile_t *pdo_set = &profile->pdo_sets[index];
        if (pdo_set->pdo_set_id != NULL && strcmp(pdo_set->pdo_set_id, pdo_set_id) == 0)
        {
            return pdo_set;
        }
    }
    return NULL;
}

const emaster_pdo_set_profile_t *emaster_slave_reference_pdo_set(
    const emaster_slave_profile_t *profile)
{
    return profile == NULL
               ? NULL
               : emaster_slave_pdo_set_by_id(profile, profile->reference_pdo_set_id);
}

bool emaster_slave_pdo_layout_matches(const emaster_slave_profile_t *profile,
                                      const emaster_pdo_layout_t *actual)
{
    return emaster_slave_pdo_set_layout_matches(emaster_slave_reference_pdo_set(profile),
                                                actual);
}

const emaster_slave_profile_t *emaster_slave_profile_by_id(const char *profile_id)
{
    size_t index;

    if (profile_id == NULL)
    {
        return NULL;
    }

    for (index = 0; index < emaster_slave_profile_count(); ++index)
    {
        const emaster_slave_profile_t *profile = emaster_slave_profile_at(index);
        if (profile != NULL && strcmp(profile->profile_id, profile_id) == 0)
        {
            return profile;
        }
    }

    return NULL;
}
