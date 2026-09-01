#define _POSIX_C_SOURCE 200809L

#include "emaster/bus/preop_probe.h"

#include "soem/soem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 本文件是项目唯一的 SOEM 调用边界。探测流程最高到 PRE-OP，只读 SII 和 SDO；
 * 无论采集成功与否，都必须在关闭原始套接字前请求全体从站恢复 INIT。
 */
static bool restore_init(ecx_contextt *context)
{
    ec_slavet *all_slaves = &context->slavelist[0];

    all_slaves->state = EC_STATE_INIT;
    ecx_writestate(context, 0U);
    return ecx_statecheck(context, 0U, EC_STATE_INIT, EC_TIMEOUTSTATE) == EC_STATE_INIT;
}

static void prepare_read(const emaster_sdo_request_t *request, emaster_sdo_read_t *read)
{
    read->index = request->index;
    read->subindex = request->subindex;
    read->type = request->type;
    (void)snprintf(read->name, sizeof(read->name), "%s", request->name);
}

static void read_sdo(ecx_contextt *context, uint16_t slave,
                     const emaster_sdo_request_t *request, emaster_sdo_read_t *read)
{
    uint8_t buffer[EMASTER_PREOP_STRING_CAPACITY] = {0};
    int expected_size;
    int size;
    int work_counter;

    prepare_read(request, read);
    if (request->type == EMASTER_SDO_U8 || request->type == EMASTER_SDO_I8)
    {
        expected_size = 1;
    }
    else if (request->type == EMASTER_SDO_U16)
    {
        expected_size = 2;
    }
    else if (request->type == EMASTER_SDO_U32)
    {
        expected_size = 4;
    }
    else
    {
        expected_size = 0;
    }

    /* 数值对象必须返回精确宽度；字符串预留一个字节，保证后续一定能补终止符。 */
    size = expected_size > 0 ? expected_size : (int)sizeof(buffer) - 1;
    work_counter = ecx_SDOread(context, slave, request->index, request->subindex, FALSE, &size,
                               buffer, EC_TIMEOUTRXM);
    read->ok = work_counter > 0 &&
               ((expected_size > 0 && size == expected_size) ||
                (expected_size == 0 && size >= 0 && (size_t)size < sizeof(buffer)));
    if (!read->ok)
    {
        return;
    }

    if (request->type == EMASTER_SDO_U8)
    {
        read->value.u8 = buffer[0];
    }
    else if (request->type == EMASTER_SDO_I8)
    {
        memcpy(&read->value.i8, buffer, sizeof(read->value.i8));
    }
    else if (request->type == EMASTER_SDO_U16)
    {
        uint16_t raw;
        memcpy(&raw, buffer, sizeof(raw));
        read->value.u16 = etohs(raw);
    }
    else if (request->type == EMASTER_SDO_U32)
    {
        uint32_t raw;
        memcpy(&raw, buffer, sizeof(raw));
        read->value.u32 = etohl(raw);
    }
    else
    {
        size_t string_size = (size_t)size;
        while (string_size > 0U && buffer[string_size - 1U] == UINT8_C(0))
        {
            --string_size;
        }
        buffer[string_size] = UINT8_C(0);
        memcpy(read->value.string, buffer, string_size + 1U);
    }
}

typedef struct
{
    ecx_contextt *context;
    uint16_t slave;
} soem_sdo_reader_context_t;

static bool read_integer(soem_sdo_reader_context_t *reader, uint16_t index,
                         uint8_t subindex, void *value, int size)
{
    int actual_size = size;
    int work_counter;

    work_counter = ecx_SDOread(reader->context, reader->slave, index, subindex, FALSE,
                               &actual_size, value, EC_TIMEOUTRXM);
    return work_counter > 0 && actual_size == size;
}

static bool read_u8(void *user_data, uint16_t index, uint8_t subindex, uint8_t *value)
{
    soem_sdo_reader_context_t *reader = user_data;

    return reader != NULL && value != NULL &&
           read_integer(reader, index, subindex, value, (int)sizeof(*value));
}

static bool read_u16(void *user_data, uint16_t index, uint8_t subindex, uint16_t *value)
{
    soem_sdo_reader_context_t *reader = user_data;
    uint16_t raw;

    if (reader == NULL || value == NULL ||
        !read_integer(reader, index, subindex, &raw, (int)sizeof(raw)))
    {
        return false;
    }
    *value = etohs(raw);
    return true;
}

static bool read_u32(void *user_data, uint16_t index, uint8_t subindex, uint32_t *value)
{
    soem_sdo_reader_context_t *reader = user_data;
    uint32_t raw;

    if (reader == NULL || value == NULL ||
        !read_integer(reader, index, subindex, &raw, (int)sizeof(raw)))
    {
        return false;
    }
    *value = etohl(raw);
    return true;
}

static bool capture_slave(ecx_contextt *context, uint16_t position,
                          const emaster_sdo_request_t *requests, size_t request_count,
                          emaster_preop_slave_t *result)
{
    const ec_slavet *slave = &context->slavelist[position];
    size_t request_index;
    soem_sdo_reader_context_t reader_context;
    emaster_pdo_sdo_reader_t reader;

    result->position = position;
    (void)snprintf(result->name, sizeof(result->name), "%s", slave->name);
    result->identity.vendor_id = slave->eep_man;
    result->identity.product_code = slave->eep_id;
    result->identity.revision = slave->eep_rev;
    result->state = slave->state;
    result->has_dc = slave->hasdc != 0U;
    result->has_coe = (slave->mbx_proto & ECT_MBXPROT_COE) != 0U;
    result->sdo_read_count = request_count;
    /* 每个从站拥有独立结果数组，由报告析构函数统一释放。 */
    result->sdo_reads = calloc(request_count, sizeof(*result->sdo_reads));
    if (result->sdo_reads == NULL)
    {
        return false;
    }

    for (request_index = 0U; request_index < request_count; ++request_index)
    {
        if (result->has_coe)
        {
            read_sdo(context, position, &requests[request_index],
                     &result->sdo_reads[request_index]);
        }
        else
        {
            prepare_read(&requests[request_index], &result->sdo_reads[request_index]);
        }
    }
    if (result->has_coe)
    {
        reader_context.context = context;
        reader_context.slave = position;
        reader.read_u8 = read_u8;
        reader.read_u16 = read_u16;
        reader.read_u32 = read_u32;
        reader.user_data = &reader_context;
        (void)emaster_pdo_layout_discover(&reader, &result->pdo_layout);
    }
    return true;
}

static bool slave_has_successful_sdo_read(const emaster_preop_slave_t *slave)
{
    size_t read_index;

    for (read_index = 0U; read_index < slave->sdo_read_count; ++read_index)
    {
        if (slave->sdo_reads[read_index].ok)
        {
            return true;
        }
    }
    return false;
}

emaster_preop_probe_status_t
emaster_soem_visit_interfaces(emaster_interface_visitor_t visitor, void *user_data)
{
    ec_adaptert *adapters;
    ec_adaptert *adapter;

    if (visitor == NULL)
    {
        return EMASTER_PREOP_PROBE_INVALID_ARGUMENT;
    }

    adapters = ec_find_adapters();
    if (adapters == NULL)
    {
        return EMASTER_PREOP_PROBE_INTERFACE_ENUMERATION_FAILED;
    }
    for (adapter = adapters; adapter != NULL; adapter = adapter->next)
    {
        visitor(adapter->name, adapter->desc, user_data);
    }
    ec_free_adapters(adapters);
    return EMASTER_PREOP_PROBE_OK;
}

void emaster_preop_report_destroy(emaster_preop_report_t *report)
{
    if (report == NULL)
    {
        return;
    }
    if (report->slaves != NULL)
    {
        for (size_t slave_index = 0U; slave_index < report->slave_count; ++slave_index)
        {
            free(report->slaves[slave_index].sdo_reads);
            emaster_pdo_layout_destroy(&report->slaves[slave_index].pdo_layout);
        }
    }
    free(report->slaves);
    memset(report, 0, sizeof(*report));
}

emaster_preop_probe_status_t
emaster_soem_preop_probe(const char *interface_name, const emaster_sdo_request_t *requests,
                         size_t request_count, emaster_preop_report_t *report)
{
    ecx_contextt context;
    int slave_count;
    emaster_preop_probe_status_t status = EMASTER_PREOP_PROBE_OK;

    if (interface_name == NULL || interface_name[0] == '\0' ||
        strnlen(interface_name, EMASTER_PREOP_NAME_CAPACITY) >= EMASTER_PREOP_NAME_CAPACITY ||
        requests == NULL ||
        request_count == 0U || request_count > EMASTER_PREOP_MAX_SDO_REQUESTS || report == NULL)
    {
        return EMASTER_PREOP_PROBE_INVALID_ARGUMENT;
    }

    for (size_t request_index = 0U; request_index < request_count; ++request_index)
    {
        if (requests[request_index].name == NULL || requests[request_index].name[0] == '\0' ||
            strnlen(requests[request_index].name, EMASTER_PREOP_OBJECT_NAME_CAPACITY) >=
                EMASTER_PREOP_OBJECT_NAME_CAPACITY ||
            requests[request_index].type < EMASTER_SDO_U8 ||
            requests[request_index].type > EMASTER_SDO_STRING)
        {
            return EMASTER_PREOP_PROBE_INVALID_ARGUMENT;
        }
    }

    memset(report, 0, sizeof(*report));
    (void)snprintf(report->interface_name, sizeof(report->interface_name), "%s", interface_name);
    memset(&context, 0, sizeof(context));
    if (!ecx_init(&context, interface_name))
    {
        return EMASTER_PREOP_PROBE_INTERFACE_OPEN_FAILED;
    }

    /* SOEM 的发现调用会把已发现从站带到 PRE-OP；此后不调用映射或 DC 配置接口。 */
    slave_count = ecx_config_init(&context);
    if (slave_count <= 0)
    {
        bool init_restored = true;
        if (context.slavecount > 0)
        {
            init_restored = restore_init(&context);
        }
        ecx_close(&context);
        return init_restored ? EMASTER_PREOP_PROBE_NO_SLAVES
                             : EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED;
    }
    /*
     * ecx_config_init() 只发出 INIT -> PRE-OP 请求，不保证转换已在返回前完成。邮箱访问前
     * 必须显式等待并确认全体从站到达 PRE-OP，否则快速设备可能偶然成功、慢设备则全部
     * SDO 超时，形成不可复现的竞态。
     */
    if (ecx_statecheck(&context, 0U, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 4) !=
        EC_STATE_PRE_OP)
    {
        bool init_restored;
        ecx_readstate(&context);
        init_restored = restore_init(&context);
        ecx_close(&context);
        return init_restored ? EMASTER_PREOP_PROBE_PREOP_NOT_REACHED
                             : EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED;
    }
    ecx_readstate(&context);

    if (slave_count > (int)EMASTER_PREOP_MAX_SLAVES)
    {
        report->restore_init_succeeded = restore_init(&context);
        ecx_close(&context);
        return report->restore_init_succeeded ? EMASTER_PREOP_PROBE_TOO_MANY_SLAVES
                                              : EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED;
    }

    report->slave_count = (size_t)slave_count;
    report->slaves = calloc(report->slave_count, sizeof(*report->slaves));
    if (report->slaves == NULL)
    {
        status = EMASTER_PREOP_PROBE_OUT_OF_MEMORY;
    }
    else
    {
        for (int position = 1; position <= slave_count; ++position)
        {
            if (!capture_slave(&context, (uint16_t)position, requests, request_count,
                               &report->slaves[position - 1]))
            {
                status = EMASTER_PREOP_PROBE_OUT_OF_MEMORY;
                break;
            }
            if (report->slaves[position - 1].has_coe &&
                !slave_has_successful_sdo_read(&report->slaves[position - 1]))
            {
                /* 部分可选对象失败可以保留；全部失败说明邮箱读取基线无效。 */
                status = EMASTER_PREOP_PROBE_SDO_READ_FAILED;
            }
            else if (report->slaves[position - 1].has_coe &&
                     report->slaves[position - 1].pdo_layout.status !=
                         EMASTER_PDO_DISCOVERY_COMPLETE)
            {
                status = EMASTER_PREOP_PROBE_PDO_DISCOVERY_FAILED;
            }
        }
    }

    /* 采集失败也不能跳过恢复；先记录恢复结果，再关闭主站上下文。 */
    report->restore_init_succeeded = restore_init(&context);
    ecx_close(&context);
    if (status != EMASTER_PREOP_PROBE_OK)
    {
        return status;
    }
    if (!report->restore_init_succeeded)
    {
        return EMASTER_PREOP_PROBE_RESTORE_INIT_FAILED;
    }
    return status;
}
