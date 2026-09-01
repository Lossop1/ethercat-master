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

static bool identity_matches_expected_profile(const emaster_slave_identity_t *identity,
                                              const emaster_topology_config_t *topology,
                                               size_t slave_index);

static const emaster_slave_profile_t *expected_profile(
    const emaster_topology_config_t *topology, size_t slave_index)
{
    if (topology == NULL || slave_index >= topology->slave_count)
    {
        return NULL;
    }
    return emaster_slave_profile_by_id(topology->slaves[slave_index].profile_id);
}

static bool report_is_valid(const emaster_preop_report_t *report,
                            const emaster_deployment_config_t *deployment)
{
    size_t slave_index;

    if (report == NULL || deployment == NULL || deployment->deployment_id == NULL ||
        deployment->topology == NULL || deployment->topology->topology_id == NULL ||
        deployment->ethercat_interface == NULL || deployment->deployment_id[0] == '\0' ||
        deployment->topology->topology_id[0] == '\0' || deployment->ethercat_interface[0] == '\0' ||
        (deployment->topology->slave_count > 0U && deployment->topology->slaves == NULL) ||
        report->slave_count == 0U ||
        report->slave_count > EMASTER_PREOP_MAX_SLAVES || report->slaves == NULL ||
        report->slave_count != deployment->topology->slave_count ||
        !fixed_string_is_valid(report->interface_name, sizeof(report->interface_name)) ||
        strcmp(report->interface_name, deployment->ethercat_interface) != 0)
    {
        return false;
    }

    /* 序列化器只接受完整、连续且有界的报告，拒绝输出部分损坏的测试证据。 */
    for (slave_index = 0U; slave_index < report->slave_count; ++slave_index)
    {
        const emaster_preop_slave_t *slave = &report->slaves[slave_index];
        const emaster_topology_slave_config_t *expected =
            &deployment->topology->slaves[slave_index];
        const emaster_slave_profile_t *profile =
            expected_profile(deployment->topology, slave_index);
        size_t read_index;

        if (slave->position != slave_index + 1U ||
            slave->position != expected->position ||
            !identity_matches_expected_profile(&slave->identity, deployment->topology,
                                                slave_index) ||
            !emaster_slave_pdo_layout_matches(profile, &slave->pdo_layout) ||
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

static bool identity_matches_expected_profile(const emaster_slave_identity_t *identity,
                                              const emaster_topology_config_t *topology,
                                              size_t slave_index)
{
    const emaster_slave_profile_t *profile;

    if (topology == NULL || slave_index >= topology->slave_count)
    {
        return false;
    }
    profile = emaster_slave_profile_by_id(topology->slaves[slave_index].profile_id);
    return emaster_slave_identity_matches(profile, identity);
}

static void write_pdo_direction(FILE *output,
                                const emaster_pdo_direction_layout_t *direction,
                                const emaster_pdo_mapping_profile_t *expected_mappings)
{
    size_t mapping_ordinal;

    fprintf(output,
            "{\"assignment_index\":\"0x%04X\",\"mapping_count\":%u,"
            "\"bit_length\":%u,\"byte_length\":%u,\"mappings\":[",
            direction->assignment_index, (unsigned int)direction->mapping_count,
            (unsigned int)direction->bit_length,
            (unsigned int)((direction->bit_length + UINT32_C(7)) / UINT32_C(8)));
    for (mapping_ordinal = 0U; mapping_ordinal < direction->mapping_count;
         ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &direction->mappings[mapping_ordinal];
        const emaster_pdo_mapping_profile_t *expected_mapping =
            &expected_mappings[mapping_ordinal];
        size_t entry_ordinal;

        if (mapping_ordinal > 0U)
        {
            fputc(',', output);
        }
        fprintf(output,
                "{\"assignment_subindex\":%u,\"mapping_index\":\"0x%04X\","
                "\"entry_count\":%u,\"bit_length\":%u,\"entries\":[",
                (unsigned int)(mapping_ordinal + 1U), mapping->mapping_index,
                (unsigned int)mapping->entry_count, (unsigned int)mapping->bit_length);
        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];
            const emaster_pdo_entry_t *expected_entry =
                &expected_mapping->entries[entry_ordinal];
            if (entry_ordinal > 0U)
            {
                fputc(',', output);
            }
            fprintf(output,
                    "{\"mapping_subindex\":%u,\"raw_descriptor\":\"0x%08X\","
                    "\"object_index\":\"0x%04X\",\"object_subindex\":%u,"
                    "\"bit_length\":%u,\"bit_offset\":%u,\"data_type\":",
                    (unsigned int)entry->mapping_subindex,
                    (unsigned int)entry->raw_descriptor, entry->object_index,
                    (unsigned int)entry->object_subindex,
                    (unsigned int)entry->bit_length, (unsigned int)entry->bit_offset);
            json_string(output, expected_entry->data_type);
            fputs(",\"name\":", output);
            json_string(output, expected_entry->name);
            fputc('}', output);
        }
        fputs("]}", output);
    }
    fputs("]}", output);
}

bool emaster_fingerprint_has_sdo_evidence(const emaster_preop_report_t *report)
{
    size_t slave_index;

    if (report == NULL || report->slave_count == 0U || report->slaves == NULL)
    {
        return false;
    }
    for (slave_index = 0U; slave_index < report->slave_count; ++slave_index)
    {
        const emaster_preop_slave_t *slave = &report->slaves[slave_index];
        size_t read_index;
        bool found = false;

        if (!slave->has_coe)
        {
            continue;
        }
        for (read_index = 0U; read_index < slave->sdo_read_count; ++read_index)
        {
            if (slave->sdo_reads != NULL && slave->sdo_reads[read_index].ok)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
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
                                   const emaster_deployment_config_t *deployment,
                                   const char *captured_at_utc)
{
    size_t slave_index;

    if (output == NULL || captured_at_utc == NULL || captured_at_utc[0] == '\0' ||
        !report_is_valid(report, deployment))
    {
        return 1;
    }

    fputs("{\n  \"schema_version\": 2,\n  \"tool\": {\"name\": \"emaster-fingerprint\", ",
          output);
    fputs("\"version\": ", output);
    json_string(output, EMASTER_VERSION);
    fputs(", \"soem_revision\": ", output);
    json_string(output, EMASTER_SOEM_REVISION);
    fputs("},\n  \"captured_at_utc\": ", output);
    json_string(output, captured_at_utc);
    fputs(",\n  \"interface\": ", output);
    json_string(output, report->interface_name);
    fputs(",\n  \"deployment_id\": ", output);
    json_string(output, deployment->deployment_id);
    fputs(",\n  \"topology_id\": ", output);
    json_string(output, deployment->topology->topology_id);
    fprintf(output, ",\n  \"slave_count\": %u,\n  \"slaves\": [\n",
            (unsigned int)report->slave_count);

    for (slave_index = 0U; slave_index < report->slave_count; ++slave_index)
    {
        const emaster_preop_slave_t *slave = &report->slaves[slave_index];
        const emaster_topology_slave_config_t *expected =
            &deployment->topology->slaves[slave_index];
        const emaster_slave_profile_t *profile =
            expected_profile(deployment->topology, slave_index);
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
                "\"target_profile_id\":",
                (unsigned int)slave->identity.vendor_id,
                (unsigned int)slave->identity.product_code,
                (unsigned int)slave->identity.revision, (unsigned int)slave->state,
                slave->has_dc ? "true" : "false", slave->has_coe ? "true" : "false");
        json_string(output, expected->profile_id);
        fprintf(output, ",\"target_profile_match\":%s,\"target_pdo_match\":%s,"
                        "\"pdo_layout\":{\"status\":\"complete\",\"rx\":",
                identity_matches_expected_profile(&slave->identity, deployment->topology,
                                                  slave_index) ? "true" : "false",
                emaster_slave_pdo_layout_matches(profile, &slave->pdo_layout)
                    ? "true" : "false");
        write_pdo_direction(output, &slave->pdo_layout.rx, profile->rx_pdo_mappings);
        fputs(",\"tx\":", output);
        write_pdo_direction(output, &slave->pdo_layout.tx, profile->tx_pdo_mappings);
        fputs("},\"sdo_reads\":[", output);

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
