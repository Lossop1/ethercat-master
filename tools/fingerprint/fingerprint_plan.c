#include "fingerprint_plan.h"

#include <stdint.h>

/*
 * 当前计划由通用身份/CiA 402 对象和已知设备的 PDO 映射对象组成。读不到的可选对象会在
 * 指纹中保留 ok=false，而不会被伪造为默认值。0x1600/0x1A00 仍是当前型号耦合；在第二种
 * PDO 布局接入前，必须改为“通用必读项 + 按设备配置扩展”或按分配对象动态发现。
 */
static const emaster_sdo_request_t requests[] = {
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
    {UINT16_C(0x1C12), UINT8_C(0x02), "rx_pdo_assignment_2", EMASTER_SDO_U16},
    {UINT16_C(0x1C13), UINT8_C(0x00), "tx_pdo_assignment_count", EMASTER_SDO_U8},
    {UINT16_C(0x1C13), UINT8_C(0x01), "tx_pdo_assignment_1", EMASTER_SDO_U16},
    {UINT16_C(0x1C13), UINT8_C(0x02), "tx_pdo_assignment_2", EMASTER_SDO_U16},
    {UINT16_C(0x1600), UINT8_C(0x00), "rx_pdo_1600_mapping_count", EMASTER_SDO_U8},
    {UINT16_C(0x1600), UINT8_C(0x01), "rx_pdo_1600_mapping_1", EMASTER_SDO_U32},
    {UINT16_C(0x1600), UINT8_C(0x02), "rx_pdo_1600_mapping_2", EMASTER_SDO_U32},
    {UINT16_C(0x1600), UINT8_C(0x03), "rx_pdo_1600_mapping_3", EMASTER_SDO_U32},
    {UINT16_C(0x1600), UINT8_C(0x04), "rx_pdo_1600_mapping_4", EMASTER_SDO_U32},
    {UINT16_C(0x1600), UINT8_C(0x05), "rx_pdo_1600_mapping_5", EMASTER_SDO_U32},
    {UINT16_C(0x1600), UINT8_C(0x06), "rx_pdo_1600_mapping_6", EMASTER_SDO_U32},
    {UINT16_C(0x1A00), UINT8_C(0x00), "tx_pdo_1a00_mapping_count", EMASTER_SDO_U8},
    {UINT16_C(0x1A00), UINT8_C(0x01), "tx_pdo_1a00_mapping_1", EMASTER_SDO_U32},
    {UINT16_C(0x1A00), UINT8_C(0x02), "tx_pdo_1a00_mapping_2", EMASTER_SDO_U32},
    {UINT16_C(0x1A00), UINT8_C(0x03), "tx_pdo_1a00_mapping_3", EMASTER_SDO_U32},
    {UINT16_C(0x1A00), UINT8_C(0x04), "tx_pdo_1a00_mapping_4", EMASTER_SDO_U32},
    {UINT16_C(0x1A00), UINT8_C(0x05), "tx_pdo_1a00_mapping_5", EMASTER_SDO_U32},
    {UINT16_C(0x1A00), UINT8_C(0x06), "tx_pdo_1a00_mapping_6", EMASTER_SDO_U32},
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

const emaster_sdo_request_t *emaster_fingerprint_sdo_plan(size_t *request_count)
{
    if (request_count != NULL)
    {
        *request_count = sizeof(requests) / sizeof(requests[0]);
    }
    return requests;
}
