#include "fingerprint_json.h"

#include "emaster/catalog/slave_profile.h"
#include "emaster/version.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool fixed_string_is_valid(const char *value, size_t capacity)
{
    return value[0] != '\0' && memchr(value, '\0', capacity) != NULL;
}

static bool sdo_type_is_valid(emaster_sdo_value_type_t type)
{
    return type >= EMASTER_SDO_U8 && type <= EMASTER_SDO_STRING;
}

static bool report_is_valid(const emaster_preop_report_t *report)
{
    size_t slave_index;

    if (report == NULL || report->slave_count == 0U ||
        report->slave_count > EMASTER_PREOP_MAX_SLAVES || report->slaves == NULL ||
        !fixed_string_is_valid(report->interface_name, sizeof(report->interface_name)))
    {
        return false;
    }

    /* 序列化器只接受完整、连续且有界的报告，拒绝输出部分损坏的测试证据。 */
    for (slave_index = 0U; slave_index < report->slave_count; ++slave_index)
    {
        const emaster_preop_slave_t *slave = &report->slaves[slave_index];
        size_t read_index;

        if (slave->position != slave_index + 1U ||
            !fixed_string_is_valid(slave->name, sizeof(slave->name)) ||
            slave->sdo_read_count > EMASTER_PREOP_MAX_SDO_REQUESTS ||
            (slave->sdo_read_count > 0U && slave->sdo_reads == NULL))
        {
            return false;
        }
        for (read_index = 0U; read_index < slave->sdo_read_count; ++read_index)
        {
            const emaster_sdo_read_t *read = &slave->sdo_reads[read_index];
            if (!fixed_string_is_valid(read->name, sizeof(read->name)) ||
                !sdo_type_is_valid(read->type) ||
                (read->ok && read->type == EMASTER_SDO_STRING &&
                 memchr(read->value.string, '\0', sizeof(read->value.string)) == NULL))
            {
                return false;
            }
        }
    }
    return true;
}

static void json_string(FILE *output, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    fputc('"', output);
    while (*cursor != '\0')
    {
        switch (*cursor)
        {
            case '"':
                fputs("\\\"", output);
                break;
            case '\\':
                fputs("\\\\", output);
                break;
            case '\b':
                fputs("\\b", output);
                break;
            case '\f':
                fputs("\\f", output);
                break;
            case '\n':
                fputs("\\n", output);
                break;
            case '\r':
                fputs("\\r", output);
                break;
            case '\t':
                fputs("\\t", output);
                break;
            default:
                if (*cursor < UINT8_C(0x20) || *cursor >= UINT8_C(0x7F))
                {
                    fprintf(output, "\\u%04x", (unsigned int)*cursor);
                }
                else
                {
                    fputc((int)*cursor, output);
                }
                break;
        }
        ++cursor;
    }
    fputc('"', output);
}

static const char *sdo_type_name(emaster_sdo_value_type_t type)
{
    switch (type)
    {
        case EMASTER_SDO_U8:
            return "u8";
        case EMASTER_SDO_I8:
            return "i8";
        case EMASTER_SDO_U16:
            return "u16";
        case EMASTER_SDO_U32:
            return "u32";
        case EMASTER_SDO_STRING:
            return "string";
    }
    return "unknown";
}

static bool identity_is_in_catalog(const emaster_slave_identity_t *identity)
{
    size_t profile_index;

    for (profile_index = 0U; profile_index < emaster_slave_profile_count(); ++profile_index)
    {
        const emaster_slave_profile_t *profile = emaster_slave_profile_at(profile_index);
        if (emaster_slave_identity_matches(profile, identity))
        {
            return true;
        }
    }
    return false;
}

static void write_sdo_read(FILE *output, const emaster_sdo_read_t *read)
{
    fprintf(output, "{\"index\":\"0x%04X\",\"subindex\":%u,\"name\":", read->index,
            (unsigned int)read->subindex);
    json_string(output, read->name);
    fputs(",\"type\":", output);
    json_string(output, sdo_type_name(read->type));
    fprintf(output, ",\"ok\":%s", read->ok ? "true" : "false");

    if (read->ok)
    {
        if (read->type == EMASTER_SDO_U8)
        {
            fprintf(output, ",\"value\":%u", (unsigned int)read->value.u8);
        }
        else if (read->type == EMASTER_SDO_I8)
        {
            fprintf(output, ",\"value\":%d", (int)read->value.i8);
        }
        else if (read->type == EMASTER_SDO_U16)
        {
            fprintf(output, ",\"value\":%u", (unsigned int)read->value.u16);
        }
        else if (read->type == EMASTER_SDO_U32)
        {
            fprintf(output, ",\"value\":%u", (unsigned int)read->value.u32);
        }
        else
        {
            fputs(",\"value\":", output);
            json_string(output, read->value.string);
        }
    }
    fputc('}', output);
}

int emaster_fingerprint_write_json(FILE *output, const emaster_preop_report_t *report,
                                   const char *captured_at_utc)
{
    size_t slave_index;

    if (output == NULL || captured_at_utc == NULL || captured_at_utc[0] == '\0' ||
        !report_is_valid(report))
    {
        return 1;
    }

    fputs("{\n  \"schema_version\": 1,\n  \"tool\": {\"name\": \"emaster-fingerprint\", ",
          output);
    fputs("\"version\": ", output);
    json_string(output, EMASTER_VERSION);
    fputs(", \"soem_revision\": ", output);
    json_string(output, EMASTER_SOEM_REVISION);
    fputs("},\n  \"captured_at_utc\": ", output);
    json_string(output, captured_at_utc);
    fputs(",\n  \"interface\": ", output);
    json_string(output, report->interface_name);
    fprintf(output, ",\n  \"slave_count\": %u,\n  \"slaves\": [\n",
            (unsigned int)report->slave_count);

    for (slave_index = 0U; slave_index < report->slave_count; ++slave_index)
    {
        const emaster_preop_slave_t *slave = &report->slaves[slave_index];
        size_t read_index;

        if (slave_index > 0U)
        {
            fputs(",\n", output);
        }
        fprintf(output, "    {\"position\":%u,\"name\":", (unsigned int)slave->position);
        json_string(output, slave->name);
        fprintf(output,
                ",\"sii_identity\":{\"vendor_id\":%u,\"product_code\":%u,"
                "\"revision\":%u},\"state\":%u,\"has_dc\":%s,\"has_coe\":%s,"
                "\"target_profile_match\":%s,\"sdo_reads\":[",
                (unsigned int)slave->identity.vendor_id,
                (unsigned int)slave->identity.product_code,
                (unsigned int)slave->identity.revision, (unsigned int)slave->state,
                slave->has_dc ? "true" : "false", slave->has_coe ? "true" : "false",
                identity_is_in_catalog(&slave->identity) ? "true" : "false");

        for (read_index = 0U; read_index < slave->sdo_read_count; ++read_index)
        {
            if (read_index > 0U)
            {
                fputc(',', output);
            }
            write_sdo_read(output, &slave->sdo_reads[read_index]);
        }
        fputs("]}", output);
    }

    fprintf(output,
            "\n  ],\n  \"behavior\": {\"pdo_mapping_performed\": false,"
            "\"dc_configuration_performed\": false,\"highest_requested_state\": "
            "\"PRE-OP\",\"restore_init_succeeded\": %s}\n}\n",
            report->restore_init_succeeded ? "true" : "false");
    return ferror(output) ? 1 : 0;
}
