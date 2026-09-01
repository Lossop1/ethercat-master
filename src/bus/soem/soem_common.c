#define _POSIX_C_SOURCE 200809L

#include "soem_common.h"

#include <stdio.h>
#include <string.h>

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
