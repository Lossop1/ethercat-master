#define _POSIX_C_SOURCE 200809L

#include "soem_common.h"

#include <stdio.h>
#include <string.h>

void emaster_soem_sdo_context_init(
    emaster_soem_sdo_reader_context_t *reader,
    ecx_contextt *context,
    uint16_t slave,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    uint64_t exchange)
{
    if (reader == NULL)
    {
        return;
    }
    reader->context = context;
    reader->slave = slave;
    reader->audit = audit;
    reader->phase = phase;
    reader->exchange = exchange;
}

static void record_sdo(emaster_soem_sdo_reader_context_t *reader,
                       emaster_audit_direction_t direction,
                       emaster_audit_value_kind_t value_kind,
                       uint16_t index, uint8_t subindex, const void *raw,
                       uint8_t size, bool succeeded, uint64_t unsigned_value,
                       int64_t signed_value)
{
    uint8_t recorded_size =
        direction == EMASTER_AUDIT_DIRECTION_WRITE || succeeded ? size : 0U;

    if (reader != NULL && reader->audit != NULL)
    {
        (void)emaster_run_audit_record_access(
            reader->audit, reader->phase, EMASTER_AUDIT_TRANSPORT_SDO, direction,
            value_kind, reader->slave, index, subindex, (uint8_t)(size * UINT8_C(8)),
            0U, reader->exchange, raw, recorded_size, succeeded, unsigned_value,
            signed_value);
    }
}

static bool write_integer(emaster_soem_sdo_reader_context_t *reader,
                          uint16_t index, uint8_t subindex, const void *raw,
                          uint8_t size, emaster_audit_value_kind_t value_kind,
                          uint64_t unsigned_value, int64_t signed_value)
{
    bool succeeded = reader != NULL && reader->context != NULL && raw != NULL &&
                     ecx_SDOwrite(reader->context, reader->slave, index, subindex,
                                  FALSE, (int)size, raw, EC_TIMEOUTRXM) > 0;

    record_sdo(reader, EMASTER_AUDIT_DIRECTION_WRITE, value_kind, index, subindex,
               raw, size, succeeded, unsigned_value, signed_value);
    return succeeded;
}

bool emaster_soem_write_u8(emaster_soem_sdo_reader_context_t *reader,
                           uint16_t index, uint8_t subindex, uint8_t value)
{
    return write_integer(reader, index, subindex, &value, (uint8_t)sizeof(value),
                         EMASTER_AUDIT_VALUE_UNSIGNED, value, (int64_t)value);
}

bool emaster_soem_write_u16(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, uint16_t value)
{
    uint16_t raw = htoes(value);

    return write_integer(reader, index, subindex, &raw, (uint8_t)sizeof(raw),
                         EMASTER_AUDIT_VALUE_UNSIGNED, value, (int64_t)value);
}

bool emaster_soem_write_u32(emaster_soem_sdo_reader_context_t *reader,
                            uint16_t index, uint8_t subindex, uint32_t value)
{
    uint32_t raw = htoel(value);

    return write_integer(reader, index, subindex, &raw, (uint8_t)sizeof(raw),
                         EMASTER_AUDIT_VALUE_UNSIGNED, value, (int64_t)value);
}

bool emaster_soem_write_i8(emaster_soem_sdo_reader_context_t *reader,
                           uint16_t index, uint8_t subindex, int8_t value)
{
    return write_integer(reader, index, subindex, &value, (uint8_t)sizeof(value),
                         EMASTER_AUDIT_VALUE_SIGNED, (uint8_t)value, value);
}

static bool assignment_failed(emaster_soem_sdo_reader_context_t *reader,
                               emaster_soem_pdo_assignment_result_t *result,
                               uint16_t index, uint8_t subindex)
{
    ec_errort error;

    result->failed_index = index;
    result->failed_subindex = subindex;
    while (reader != NULL && reader->context != NULL &&
           ecx_poperror(reader->context, &error))
    {
        if (error.Etype == EC_ERR_TYPE_SDO_ERROR && error.Index == index &&
            error.SubIdx == subindex)
        {
            result->abort_code_available = true;
            result->abort_code = (uint32_t)error.AbortCode;
        }
    }
    return false;
}

static bool assign_direction(emaster_soem_sdo_reader_context_t *reader, uint16_t index,
                             const emaster_pdo_mapping_profile_t *mappings,
                             size_t mapping_count,
                             emaster_soem_pdo_assignment_result_t *result)
{
    size_t ordinal;
    uint8_t count;
    uint16_t observed_index;

    if (mapping_count > UINT8_MAX || (mapping_count > 0U && mappings == NULL) ||
        result == NULL)
    {
        return false;
    }
    if (!emaster_soem_write_u8(reader, index, UINT8_C(0), UINT8_C(0)))
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    if (!emaster_soem_read_u8(reader, index, UINT8_C(0), &count) || count != 0U)
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    for (ordinal = 0U; ordinal < mapping_count; ++ordinal)
    {
        if (!emaster_soem_write_u16(reader, index, (uint8_t)(ordinal + 1U),
                                    mappings[ordinal].index) ||
            !emaster_soem_read_u16(reader, index, (uint8_t)(ordinal + 1U),
                                   &observed_index) ||
            observed_index != mappings[ordinal].index)
        {
            return assignment_failed(reader, result, index,
                                     (uint8_t)(ordinal + 1U));
        }
    }
    if (!emaster_soem_write_u8(reader, index, UINT8_C(0), (uint8_t)mapping_count) ||
        !emaster_soem_read_u8(reader, index, UINT8_C(0), &count))
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    return count == (uint8_t)mapping_count;
}

static bool assignment_matches(emaster_soem_sdo_reader_context_t *reader, uint16_t index,
                               const emaster_pdo_mapping_profile_t *mappings,
                               size_t mapping_count, bool *matches,
                               emaster_soem_pdo_assignment_result_t *result)
{
    uint8_t count;
    size_t ordinal;

    if (mappings == NULL || matches == NULL || result == NULL ||
        !emaster_soem_read_u8(reader, index, UINT8_C(0), &count))
    {
        return assignment_failed(reader, result, index, UINT8_C(0));
    }
    *matches = (size_t)count == mapping_count;
    for (ordinal = 0U; *matches && ordinal < mapping_count; ++ordinal)
    {
        uint16_t observed_index;

        if (!emaster_soem_read_u16(reader, index, (uint8_t)(ordinal + 1U),
                                   &observed_index))
        {
            return assignment_failed(reader, result, index,
                                     (uint8_t)(ordinal + 1U));
        }
        *matches = observed_index == mappings[ordinal].index;
    }
    return true;
}

bool emaster_soem_assign_pdo_set_recorded(
    emaster_soem_sdo_reader_context_t *reader,
    const emaster_pdo_set_profile_t *pdo_set,
    emaster_soem_pdo_assignment_result_t *result)
{
    uint32_t module_ident;
    uint32_t observed_module_ident;
    bool rx_matches;
    bool tx_matches;

    if (result != NULL)
    {
        memset(result, 0, sizeof(*result));
    }
    if (reader == NULL || reader->context == NULL || reader->slave == 0U ||
        pdo_set == NULL || result == NULL ||
        pdo_set->rx_mappings == NULL || pdo_set->tx_mappings == NULL)
    {
        return false;
    }
    /* F030 是模块化从站的配置模块列表；先选择方案对应模块，再分配 PDO。 */
    module_ident = pdo_set->module_ident;
    if (!emaster_soem_write_u32(reader, UINT16_C(0xF030), pdo_set->module_slot,
                                module_ident) ||
        !emaster_soem_read_u32(reader, UINT16_C(0xF030), pdo_set->module_slot,
                               &observed_module_ident) ||
        observed_module_ident != module_ident)
    {
        return assignment_failed(reader, result, UINT16_C(0xF030),
                                 pdo_set->module_slot);
    }
    if (!assignment_matches(reader, UINT16_C(0x1C12), pdo_set->rx_mappings,
                            pdo_set->rx_mapping_count, &rx_matches, result) ||
        !assignment_matches(reader, UINT16_C(0x1C13), pdo_set->tx_mappings,
                            pdo_set->tx_mapping_count, &tx_matches, result))
    {
        return false;
    }
    return (rx_matches ||
            assign_direction(reader, UINT16_C(0x1C12), pdo_set->rx_mappings,
                             pdo_set->rx_mapping_count, result)) &&
           (tx_matches ||
            assign_direction(reader, UINT16_C(0x1C13), pdo_set->tx_mappings,
                             pdo_set->tx_mapping_count, result));
}

bool emaster_soem_assign_pdo_set(ecx_contextt *context, uint16_t slave,
                                 const emaster_pdo_set_profile_t *pdo_set,
                                 emaster_soem_pdo_assignment_result_t *result)
{
    emaster_soem_sdo_reader_context_t reader;

    emaster_soem_sdo_context_init(&reader, context, slave, NULL,
                                  EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, 0U);
    return emaster_soem_assign_pdo_set_recorded(&reader, pdo_set, result);
}

bool emaster_soem_restore_init(ecx_contextt *context)
{
    if (context == NULL)
    {
        return false;
    }
    context->slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(context, 0U);
    return ecx_statecheck(context, 0U, EC_STATE_INIT, EC_TIMEOUTSTATE) == EC_STATE_INIT;
}

bool emaster_soem_interface_carrier(const char *interface_name)
{
    char path[256];
    char carrier[4];
    FILE *stream;

    if (interface_name == NULL || interface_name[0] == '\0' ||
        strnlen(interface_name, sizeof(path)) >= sizeof(path) - 24U)
    {
        return false;
    }
    if (snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", interface_name) < 0)
    {
        return false;
    }
    stream = fopen(path, "r");
    if (stream == NULL)
    {
        return false;
    }
    memset(carrier, 0, sizeof(carrier));
    if (fgets(carrier, sizeof(carrier), stream) == NULL)
    {
        (void)fclose(stream);
        return false;
    }
    (void)fclose(stream);
    return carrier[0] == '1';
}

bool emaster_soem_wait_preop(ecx_contextt *context)
{
    if (context == NULL)
    {
        return false;
    }
    return ecx_statecheck(context, 0U, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 4) ==
           EC_STATE_PRE_OP;
}

static bool read_integer(emaster_soem_sdo_reader_context_t *reader, uint16_t index,
                         uint8_t subindex, void *value, int size)
{
    int actual_size = size;

    if (reader == NULL || reader->context == NULL || value == NULL || size <= 0)
    {
        return false;
    }
    return ecx_SDOread(reader->context, reader->slave, index, subindex, FALSE, &actual_size,
                       value, EC_TIMEOUTRXM) > 0 &&
           actual_size == size;
}

bool emaster_soem_read_u8(void *user_data, uint16_t index, uint8_t subindex,
                          uint8_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint8_t raw = 0U;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));

    if (succeeded)
    {
        *value = raw;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_UNSIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, raw, (int64_t)raw);
    return succeeded;
}

bool emaster_soem_read_u16(void *user_data, uint16_t index, uint8_t subindex,
                           uint16_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint16_t raw = 0U;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));
    uint16_t decoded = etohs(raw);

    if (succeeded)
    {
        *value = decoded;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_UNSIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, decoded,
               (int64_t)decoded);
    return succeeded;
}

bool emaster_soem_read_u32(void *user_data, uint16_t index, uint8_t subindex,
                           uint32_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint32_t raw = 0U;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));
    uint32_t decoded = etohl(raw);

    if (succeeded)
    {
        *value = decoded;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_UNSIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, decoded,
               (int64_t)decoded);
    return succeeded;
}

bool emaster_soem_read_i8(void *user_data, uint16_t index, uint8_t subindex,
                          int8_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    int8_t raw = 0;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));

    if (succeeded)
    {
        *value = raw;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_SIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, (uint8_t)raw,
               raw);
    return succeeded;
}

bool emaster_soem_read_i16(void *user_data, uint16_t index, uint8_t subindex,
                           int16_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint16_t raw = 0U;
    uint16_t host_value;
    int16_t decoded = 0;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));

    host_value = etohs(raw);
    memcpy(&decoded, &host_value, sizeof(decoded));
    if (succeeded)
    {
        *value = decoded;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_SIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, host_value,
               decoded);
    return succeeded;
}

bool emaster_soem_read_i32(void *user_data, uint16_t index, uint8_t subindex,
                           int32_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint32_t raw = 0U;
    uint32_t host_value;
    int32_t decoded = 0;
    bool succeeded = value != NULL &&
                     read_integer(reader, index, subindex, &raw, (int)sizeof(raw));

    host_value = etohl(raw);
    memcpy(&decoded, &host_value, sizeof(decoded));
    if (succeeded)
    {
        *value = decoded;
    }
    record_sdo(reader, EMASTER_AUDIT_DIRECTION_READ, EMASTER_AUDIT_VALUE_SIGNED,
               index, subindex, &raw, (uint8_t)sizeof(raw), succeeded, host_value,
               decoded);
    return succeeded;
}

bool emaster_soem_discover_pdo_layout_recorded(
    emaster_soem_sdo_reader_context_t *reader_context,
    emaster_pdo_layout_t *layout)
{
    emaster_pdo_sdo_reader_t reader;

    if (reader_context == NULL || reader_context->context == NULL ||
        reader_context->slave == 0U || layout == NULL)
    {
        return false;
    }
    reader.read_u8 = emaster_soem_read_u8;
    reader.read_u16 = emaster_soem_read_u16;
    reader.read_u32 = emaster_soem_read_u32;
    reader.user_data = reader_context;
    return emaster_pdo_layout_discover(&reader, layout) == EMASTER_PDO_DISCOVERY_COMPLETE;
}

bool emaster_soem_discover_pdo_layout(ecx_contextt *context, uint16_t slave,
                                      emaster_pdo_layout_t *layout)
{
    emaster_soem_sdo_reader_context_t reader_context;

    emaster_soem_sdo_context_init(&reader_context, context, slave, NULL,
                                  EMASTER_AUDIT_PHASE_PREOP_CONFIGURATION, 0U);
    return emaster_soem_discover_pdo_layout_recorded(&reader_context, layout);
}
