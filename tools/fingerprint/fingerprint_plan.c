#include "fingerprint_plan.h"

#include <stdint.h>

/* 这些对象是 CiA 402/CoE 通用诊断事实，与具体 PDO 索引无关。 */
static const emaster_sdo_request_t common_requests[] = {
    {UINT16_C(0x1008), UINT8_C(0x00), "device_name", EMASTER_SDO_STRING},
    {UINT16_C(0x1009), UINT8_C(0x00), "hardware_version", EMASTER_SDO_STRING},
    {UINT16_C(0x100A), UINT8_C(0x00), "software_version", EMASTER_SDO_STRING},
    {UINT16_C(0x1018), UINT8_C(0x01), "identity_vendor_id", EMASTER_SDO_U32},
    {UINT16_C(0x1018), UINT8_C(0x02), "identity_product_code", EMASTER_SDO_U32},
    {UINT16_C(0x1018), UINT8_C(0x03), "identity_revision", EMASTER_SDO_U32},
    {UINT16_C(0x1018), UINT8_C(0x04), "identity_serial_number", EMASTER_SDO_U32},
    {UINT16_C(0x10F1), UINT8_C(0x02), "sync_error_counter_limit", EMASTER_SDO_U16},
    {UINT16_C(0x1C12), UINT8_C(0x00), "rx_pdo_assignment_count", EMASTER_SDO_U8},
    {UINT16_C(0x1C12), UINT8_C(0x01), "rx_pdo_assignment_1", EMASTER_SDO_U16},
    {UINT16_C(0x1C13), UINT8_C(0x00), "tx_pdo_assignment_count", EMASTER_SDO_U8},
    {UINT16_C(0x1C13), UINT8_C(0x01), "tx_pdo_assignment_1", EMASTER_SDO_U16},
    {UINT16_C(0x1C32), UINT8_C(0x01), "sm2_sync_type", EMASTER_SDO_U16},
    {UINT16_C(0x1C32), UINT8_C(0x02), "sm2_cycle_time_ns", EMASTER_SDO_U32},
    {UINT16_C(0x1C32), UINT8_C(0x04), "sm2_supported_sync_types", EMASTER_SDO_U16},
    {UINT16_C(0x1C32), UINT8_C(0x05), "sm2_minimum_cycle_time_ns", EMASTER_SDO_U32},
    {UINT16_C(0x1C32), UINT8_C(0x0A), "sm2_sync0_cycle_time_ns", EMASTER_SDO_U32},
    {UINT16_C(0x1C32), UINT8_C(0x0B), "sm2_event_missed_count", EMASTER_SDO_U16},
    {UINT16_C(0x1C32), UINT8_C(0x0C), "sm2_cycle_too_small_count", EMASTER_SDO_U16},
    {UINT16_C(0x1C32), UINT8_C(0x0D), "sm2_shift_too_short_count", EMASTER_SDO_U16},
    {UINT16_C(0x1C32), UINT8_C(0x20), "sm2_sync_error", EMASTER_SDO_U8},
    {UINT16_C(0x1C33), UINT8_C(0x01), "sm3_sync_type", EMASTER_SDO_U16},
    {UINT16_C(0x1C33), UINT8_C(0x02), "sm3_cycle_time_ns", EMASTER_SDO_U32},
    {UINT16_C(0x1C33), UINT8_C(0x04), "sm3_supported_sync_types", EMASTER_SDO_U16},
    {UINT16_C(0x1C33), UINT8_C(0x05), "sm3_minimum_cycle_time_ns", EMASTER_SDO_U32},
    {UINT16_C(0x1C33), UINT8_C(0x0A), "sm3_sync0_cycle_time_ns", EMASTER_SDO_U32},
    {UINT16_C(0x1C33), UINT8_C(0x0B), "sm3_event_missed_count", EMASTER_SDO_U16},
    {UINT16_C(0x1C33), UINT8_C(0x0C), "sm3_cycle_too_small_count", EMASTER_SDO_U16},
    {UINT16_C(0x1C33), UINT8_C(0x0D), "sm3_shift_too_short_count", EMASTER_SDO_U16},
    {UINT16_C(0x1C33), UINT8_C(0x20), "sm3_sync_error", EMASTER_SDO_U8},
    {UINT16_C(0x2002), UINT8_C(0x01), "input_mode", EMASTER_SDO_U16},
    {UINT16_C(0x603F), UINT8_C(0x00), "error_code", EMASTER_SDO_U16},
    {UINT16_C(0x6060), UINT8_C(0x00), "mode_of_operation", EMASTER_SDO_I8},
    {UINT16_C(0x6075), UINT8_C(0x00), "motor_rated_current", EMASTER_SDO_U32},
    {UINT16_C(0x6076), UINT8_C(0x00), "motor_rated_torque", EMASTER_SDO_U32},
    {UINT16_C(0x608F), UINT8_C(0x01), "encoder_increments", EMASTER_SDO_U32},
    {UINT16_C(0x608F), UINT8_C(0x02), "encoder_motor_revolutions", EMASTER_SDO_U32},
    {UINT16_C(0x6091), UINT8_C(0x01), "gear_motor_revolutions", EMASTER_SDO_U32},
    {UINT16_C(0x6091), UINT8_C(0x02), "gear_shaft_revolutions", EMASTER_SDO_U32},
    {UINT16_C(0x60C2), UINT8_C(0x01), "interpolation_time_period", EMASTER_SDO_U8},
    {UINT16_C(0x60C2), UINT8_C(0x02), "interpolation_time_index", EMASTER_SDO_I8},
    {UINT16_C(0x6502), UINT8_C(0x00), "supported_drive_modes", EMASTER_SDO_U32},
};

static bool append_request(emaster_sdo_request_t *requests, size_t capacity, size_t *count,
                           emaster_sdo_request_t request)
{
    size_t index;

    for (index = 0U; index < *count; ++index)
    {
        if (requests[index].index == request.index &&
            requests[index].subindex == request.subindex)
        {
            return true;
        }
    }
    if (*count >= capacity)
    {
        return false;
    }
    requests[*count] = request;
    ++*count;
    return true;
}

bool emaster_fingerprint_sdo_plan(emaster_sdo_request_t *requests, size_t capacity,
                                  size_t *request_count)
{
    size_t count = 0U;
    size_t request_index;
    size_t profile_index;

    if (requests == NULL || request_count == NULL || capacity == 0U)
    {
        return false;
    }
    for (request_index = 0U;
         request_index < sizeof(common_requests) / sizeof(common_requests[0]); ++request_index)
    {
        if (!append_request(requests, capacity, &count, common_requests[request_index]))
        {
            return false;
        }
    }
    for (profile_index = 0U; profile_index < emaster_slave_profile_count(); ++profile_index)
    {
        const emaster_slave_profile_t *profile = emaster_slave_profile_at(profile_index);
        if (profile == NULL)
        {
            return false;
        }

        const emaster_pdo_entry_t *entries[2] = {profile->rx_pdo_entries, profile->tx_pdo_entries};
        const size_t entry_counts[2] = {profile->rx_pdo_entry_count, profile->tx_pdo_entry_count};
        const uint16_t pdo_indices[2] = {profile->rx_pdo_index, profile->tx_pdo_index};
        const char *count_names[2] = {"rx_pdo_mapping_count", "tx_pdo_mapping_count"};
        size_t direction;

        for (direction = 0U; direction < 2U; ++direction)
        {
            emaster_sdo_request_t count_request = {
                .index = pdo_indices[direction],
                .subindex = UINT8_C(0),
                .name = count_names[direction],
                .type = EMASTER_SDO_U8,
            };
            if (!append_request(requests, capacity, &count, count_request))
            {
                return false;
            }
            for (request_index = 0U; request_index < entry_counts[direction]; ++request_index)
            {
                emaster_sdo_request_t entry_request = {
                    .index = pdo_indices[direction],
                    .subindex = entries[direction][request_index].subindex,
                    .name = entries[direction][request_index].name,
                    .type = EMASTER_SDO_U32,
                };
                if (!append_request(requests, capacity, &count, entry_request))
                {
                    return false;
                }
            }
        }
    }
    *request_count = count;
    return true;
}
