#define _POSIX_C_SOURCE 200809L

#include "soem_common.h"

#include <stdio.h>
#include <string.h>

static bool write_u8(ecx_contextt *context, uint16_t slave, uint16_t index,
                     uint8_t subindex, uint8_t value)
{
    return context != NULL &&
           ecx_SDOwrite(context, slave, index, subindex, FALSE, (int)sizeof(value), &value,
                        EC_TIMEOUTRXM) > 0;
}

static bool write_u16(ecx_contextt *context, uint16_t slave, uint16_t index,
                      uint8_t subindex, uint16_t value)
{
    uint16_t raw = htoes(value);

    return context != NULL &&
           ecx_SDOwrite(context, slave, index, subindex, FALSE, (int)sizeof(raw), &raw,
                        EC_TIMEOUTRXM) > 0;
}

static bool write_u32(ecx_contextt *context, uint16_t slave, uint16_t index,
                      uint8_t subindex, uint32_t value)
{
    uint32_t raw = htoel(value);

    return context != NULL &&
           ecx_SDOwrite(context, slave, index, subindex, FALSE, (int)sizeof(raw), &raw,
                        EC_TIMEOUTRXM) > 0;
}

static bool read_u8(ecx_contextt *context, uint16_t slave, uint16_t index,
                    uint8_t subindex, uint8_t *value)
{
    int size = value == NULL ? 0 : (int)sizeof(*value);

    return context != NULL && value != NULL &&
           ecx_SDOread(context, slave, index, subindex, FALSE, &size, value,
                       EC_TIMEOUTRXM) > 0 &&
           size == (int)sizeof(*value);
}

static bool read_u16(ecx_contextt *context, uint16_t slave, uint16_t index,
                     uint8_t subindex, uint16_t *value)
{
    uint16_t raw;
    int size = (int)sizeof(raw);

    if (context == NULL || value == NULL ||
        ecx_SDOread(context, slave, index, subindex, FALSE, &size, &raw,
                    EC_TIMEOUTRXM) <= 0 || size != (int)sizeof(raw))
    {
        return false;
    }
    *value = etohs(raw);
    return true;
}

static bool read_u32(ecx_contextt *context, uint16_t slave, uint16_t index,
                     uint8_t subindex, uint32_t *value)
{
    uint32_t raw;
    int size = (int)sizeof(raw);

    if (context == NULL || value == NULL ||
        ecx_SDOread(context, slave, index, subindex, FALSE, &size, &raw,
                    EC_TIMEOUTRXM) <= 0 || size != (int)sizeof(raw))
    {
        return false;
    }
    *value = etohl(raw);
    return true;
}

static bool assignment_failed(ecx_contextt *context,
                              emaster_soem_pdo_assignment_result_t *result,
                              uint16_t index, uint8_t subindex)
{
    ec_errort error;

    result->failed_index = index;
    result->failed_subindex = subindex;
    while (context != NULL && ecx_poperror(context, &error))
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

static bool assign_direction(ecx_contextt *context, uint16_t slave, uint16_t index,
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
    if (!write_u8(context, slave, index, UINT8_C(0), UINT8_C(0)))
    {
        return assignment_failed(context, result, index, UINT8_C(0));
    }
    if (!read_u8(context, slave, index, UINT8_C(0), &count) || count != 0U)
    {
        return assignment_failed(context, result, index, UINT8_C(0));
    }
    for (ordinal = 0U; ordinal < mapping_count; ++ordinal)
    {
        if (!write_u16(context, slave, index, (uint8_t)(ordinal + 1U),
                       mappings[ordinal].index) ||
            !read_u16(context, slave, index, (uint8_t)(ordinal + 1U), &observed_index) ||
            observed_index != mappings[ordinal].index)
        {
            return assignment_failed(context, result, index,
                                     (uint8_t)(ordinal + 1U));
        }
    }
    if (!write_u8(context, slave, index, UINT8_C(0), (uint8_t)mapping_count) ||
        !read_u8(context, slave, index, UINT8_C(0), &count))
    {
        return assignment_failed(context, result, index, UINT8_C(0));
    }
    return count == (uint8_t)mapping_count;
}

static bool assignment_matches(ecx_contextt *context, uint16_t slave, uint16_t index,
                               const emaster_pdo_mapping_profile_t *mappings,
                               size_t mapping_count, bool *matches,
                               emaster_soem_pdo_assignment_result_t *result)
{
    uint8_t count;
    size_t ordinal;

    if (mappings == NULL || matches == NULL || result == NULL ||
        !read_u8(context, slave, index, UINT8_C(0), &count))
    {
        return assignment_failed(context, result, index, UINT8_C(0));
    }
    *matches = (size_t)count == mapping_count;
    for (ordinal = 0U; *matches && ordinal < mapping_count; ++ordinal)
    {
        uint16_t observed_index;

        if (!read_u16(context, slave, index, (uint8_t)(ordinal + 1U),
                      &observed_index))
        {
            return assignment_failed(context, result, index,
                                     (uint8_t)(ordinal + 1U));
        }
        *matches = observed_index == mappings[ordinal].index;
    }
    return true;
}

bool emaster_soem_assign_pdo_set(ecx_contextt *context, uint16_t slave,
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
    if (context == NULL || slave == 0U || pdo_set == NULL || result == NULL ||
        pdo_set->rx_mappings == NULL || pdo_set->tx_mappings == NULL)
    {
        return false;
    }
    /* F030 是模块化从站的配置模块列表；先选择方案对应模块，再分配 PDO。 */
    module_ident = pdo_set->module_ident;
    if (!write_u32(context, slave, UINT16_C(0xF030), pdo_set->module_slot, module_ident) ||
        !read_u32(context, slave, UINT16_C(0xF030), pdo_set->module_slot,
                  &observed_module_ident) ||
        observed_module_ident != module_ident)
    {
        return assignment_failed(context, result, UINT16_C(0xF030),
                                 pdo_set->module_slot);
    }
    if (!assignment_matches(context, slave, UINT16_C(0x1C12), pdo_set->rx_mappings,
                            pdo_set->rx_mapping_count, &rx_matches, result) ||
        !assignment_matches(context, slave, UINT16_C(0x1C13), pdo_set->tx_mappings,
                            pdo_set->tx_mapping_count, &tx_matches, result))
    {
        return false;
    }
    return (rx_matches ||
            assign_direction(context, slave, UINT16_C(0x1C12), pdo_set->rx_mappings,
                             pdo_set->rx_mapping_count, result)) &&
           (tx_matches ||
            assign_direction(context, slave, UINT16_C(0x1C13), pdo_set->tx_mappings,
                             pdo_set->tx_mapping_count, result));
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

    return value != NULL && read_integer(reader, index, subindex, value, (int)sizeof(*value));
}

bool emaster_soem_read_u16(void *user_data, uint16_t index, uint8_t subindex,
                           uint16_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint16_t raw;

    if (value == NULL || !read_integer(reader, index, subindex, &raw, (int)sizeof(raw)))
    {
        return false;
    }
    *value = etohs(raw);
    return true;
}

bool emaster_soem_read_u32(void *user_data, uint16_t index, uint8_t subindex,
                           uint32_t *value)
{
    emaster_soem_sdo_reader_context_t *reader = user_data;
    uint32_t raw;

    if (value == NULL || !read_integer(reader, index, subindex, &raw, (int)sizeof(raw)))
    {
        return false;
    }
    *value = etohl(raw);
    return true;
}

bool emaster_soem_discover_pdo_layout(ecx_contextt *context, uint16_t slave,
                                      emaster_pdo_layout_t *layout)
{
    emaster_soem_sdo_reader_context_t reader_context;
    emaster_pdo_sdo_reader_t reader;

    if (context == NULL || layout == NULL || slave == 0U)
    {
        return false;
    }
    memset(&reader_context, 0, sizeof(reader_context));
    reader_context.context = context;
    reader_context.slave = slave;
    reader.read_u8 = emaster_soem_read_u8;
    reader.read_u16 = emaster_soem_read_u16;
    reader.read_u32 = emaster_soem_read_u32;
    reader.user_data = &reader_context;
    return emaster_pdo_layout_discover(&reader, layout) == EMASTER_PDO_DISCOVERY_COMPLETE;
}
