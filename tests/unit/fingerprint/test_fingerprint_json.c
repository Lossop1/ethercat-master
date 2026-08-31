#include "fingerprint_json.h"
#include "fingerprint_plan.h"

#include <stdio.h>
#include <string.h>

static int validate_plan(void)
{
    const emaster_sdo_request_t *requests;
    size_t request_count;
    size_t left;

    requests = emaster_fingerprint_sdo_plan(&request_count);
    if (requests == NULL || request_count == 0U ||
        request_count > EMASTER_PREOP_MAX_SDO_REQUESTS)
    {
        fprintf(stderr, "指纹 SDO 读取计划数量无效\n");
        return 1;
    }

    for (left = 0U; left < request_count; ++left)
    {
        size_t right;

        if (requests[left].name == NULL || requests[left].name[0] == '\0' ||
            strlen(requests[left].name) >= EMASTER_PREOP_OBJECT_NAME_CAPACITY ||
            requests[left].type < EMASTER_SDO_U8 || requests[left].type > EMASTER_SDO_STRING)
        {
            fprintf(stderr, "指纹 SDO 读取计划第 %u 项无效\n", (unsigned int)left);
            return 1;
        }
        for (right = left + 1U; right < request_count; ++right)
        {
            if (requests[left].index == requests[right].index &&
                requests[left].subindex == requests[right].subindex)
            {
                fprintf(stderr, "指纹 SDO 读取计划存在重复对象\n");
                return 1;
            }
        }
    }
    return 0;
}

int main(void)
{
    emaster_sdo_read_t reads[3] = {0};
    emaster_preop_slave_t slave = {0};
    emaster_preop_report_t report = {0};

    if (validate_plan() != 0)
    {
        return 1;
    }

    reads[0].index = UINT16_C(0x1008);
    reads[0].type = EMASTER_SDO_STRING;
    reads[0].ok = true;
    (void)snprintf(reads[0].name, sizeof(reads[0].name), "device_name");
    (void)snprintf(reads[0].value.string, sizeof(reads[0].value.string), "drive \"A\"\n");

    reads[1].index = UINT16_C(0x1018);
    reads[1].subindex = UINT8_C(4);
    reads[1].type = EMASTER_SDO_U32;
    reads[1].ok = true;
    reads[1].value.u32 = UINT32_C(4294967295);
    (void)snprintf(reads[1].name, sizeof(reads[1].name), "identity_serial_number");

    reads[2].index = UINT16_C(0x1C33);
    reads[2].subindex = UINT8_C(1);
    reads[2].type = EMASTER_SDO_U16;
    (void)snprintf(reads[2].name, sizeof(reads[2].name), "sm3_sync_type");

    slave.position = UINT16_C(1);
    (void)snprintf(slave.name, sizeof(slave.name), "ISVD90RC-300B-100-70");
    slave.identity.vendor_id = UINT32_C(0x000C0B00);
    slave.identity.product_code = UINT32_C(0x00080117);
    slave.identity.revision = UINT32_C(0x00000001);
    slave.state = UINT16_C(2);
    slave.has_dc = true;
    slave.has_coe = true;
    slave.sdo_read_count = sizeof(reads) / sizeof(reads[0]);
    slave.sdo_reads = reads;

    (void)snprintf(report.interface_name, sizeof(report.interface_name), "test0");
    report.slave_count = 1U;
    report.slaves = &slave;
    report.restore_init_succeeded = true;

    slave.position = UINT16_C(2);
    if (emaster_fingerprint_write_json(stdout, &report, "2026-08-31T00:00:00Z") == 0)
    {
        fprintf(stderr, "序列化器未拒绝位置不连续的报告\n");
        return 1;
    }
    slave.position = UINT16_C(1);

    return emaster_fingerprint_write_json(stdout, &report, "2026-08-31T00:00:00Z");
}
