#define _POSIX_C_SOURCE 200809L

#include "emaster/diagnostic_probe.h"

#include "emaster/slave_profile.h"
#include "emaster/version.h"
#include "soem/soem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum
{
    SDO_U8,
    SDO_I8,
    SDO_U16,
    SDO_U32,
    SDO_STRING
} sdo_value_type_t;

typedef struct
{
    uint16_t index;
    uint8_t subindex;
    sdo_value_type_t type;
    const char *name;
} sdo_descriptor_t;

static const sdo_descriptor_t fingerprint_objects[] = {
    {UINT16_C(0x1008), UINT8_C(0x00), SDO_STRING, "device_name"},
    {UINT16_C(0x1009), UINT8_C(0x00), SDO_STRING, "hardware_version"},
    {UINT16_C(0x100A), UINT8_C(0x00), SDO_STRING, "software_version"},
    {UINT16_C(0x1018), UINT8_C(0x01), SDO_U32, "identity_vendor_id"},
    {UINT16_C(0x1018), UINT8_C(0x02), SDO_U32, "identity_product_code"},
    {UINT16_C(0x1018), UINT8_C(0x03), SDO_U32, "identity_revision"},
    {UINT16_C(0x1018), UINT8_C(0x04), SDO_U32, "identity_serial_number"},
    {UINT16_C(0x10F1), UINT8_C(0x02), SDO_U16, "sync_error_counter_limit"},
    {UINT16_C(0x1C12), UINT8_C(0x00), SDO_U8, "rx_pdo_assignment_count"},
    {UINT16_C(0x1C12), UINT8_C(0x01), SDO_U16, "rx_pdo_assignment_1"},
    {UINT16_C(0x1C12), UINT8_C(0x02), SDO_U16, "rx_pdo_assignment_2"},
    {UINT16_C(0x1C13), UINT8_C(0x00), SDO_U8, "tx_pdo_assignment_count"},
    {UINT16_C(0x1C13), UINT8_C(0x01), SDO_U16, "tx_pdo_assignment_1"},
    {UINT16_C(0x1C13), UINT8_C(0x02), SDO_U16, "tx_pdo_assignment_2"},
    {UINT16_C(0x1600), UINT8_C(0x00), SDO_U8, "rx_pdo_1600_mapping_count"},
    {UINT16_C(0x1600), UINT8_C(0x01), SDO_U32, "rx_pdo_1600_mapping_1"},
    {UINT16_C(0x1600), UINT8_C(0x02), SDO_U32, "rx_pdo_1600_mapping_2"},
    {UINT16_C(0x1600), UINT8_C(0x03), SDO_U32, "rx_pdo_1600_mapping_3"},
    {UINT16_C(0x1600), UINT8_C(0x04), SDO_U32, "rx_pdo_1600_mapping_4"},
    {UINT16_C(0x1600), UINT8_C(0x05), SDO_U32, "rx_pdo_1600_mapping_5"},
    {UINT16_C(0x1600), UINT8_C(0x06), SDO_U32, "rx_pdo_1600_mapping_6"},
    {UINT16_C(0x1A00), UINT8_C(0x00), SDO_U8, "tx_pdo_1a00_mapping_count"},
    {UINT16_C(0x1A00), UINT8_C(0x01), SDO_U32, "tx_pdo_1a00_mapping_1"},
    {UINT16_C(0x1A00), UINT8_C(0x02), SDO_U32, "tx_pdo_1a00_mapping_2"},
    {UINT16_C(0x1A00), UINT8_C(0x03), SDO_U32, "tx_pdo_1a00_mapping_3"},
    {UINT16_C(0x1A00), UINT8_C(0x04), SDO_U32, "tx_pdo_1a00_mapping_4"},
    {UINT16_C(0x1A00), UINT8_C(0x05), SDO_U32, "tx_pdo_1a00_mapping_5"},
    {UINT16_C(0x1A00), UINT8_C(0x06), SDO_U32, "tx_pdo_1a00_mapping_6"},
    {UINT16_C(0x1C32), UINT8_C(0x01), SDO_U16, "sm2_sync_type"},
    {UINT16_C(0x1C32), UINT8_C(0x02), SDO_U32, "sm2_cycle_time_ns"},
    {UINT16_C(0x1C32), UINT8_C(0x04), SDO_U16, "sm2_supported_sync_types"},
    {UINT16_C(0x1C32), UINT8_C(0x05), SDO_U32, "sm2_minimum_cycle_time_ns"},
    {UINT16_C(0x1C32), UINT8_C(0x0A), SDO_U32, "sm2_sync0_cycle_time_ns"},
    {UINT16_C(0x1C32), UINT8_C(0x0B), SDO_U16, "sm2_event_missed_count"},
    {UINT16_C(0x1C32), UINT8_C(0x0C), SDO_U16, "sm2_cycle_too_small_count"},
    {UINT16_C(0x1C32), UINT8_C(0x0D), SDO_U16, "sm2_shift_too_short_count"},
    {UINT16_C(0x1C32), UINT8_C(0x20), SDO_U8, "sm2_sync_error"},
    {UINT16_C(0x1C33), UINT8_C(0x01), SDO_U16, "sm3_sync_type"},
    {UINT16_C(0x1C33), UINT8_C(0x02), SDO_U32, "sm3_cycle_time_ns"},
    {UINT16_C(0x1C33), UINT8_C(0x04), SDO_U16, "sm3_supported_sync_types"},
    {UINT16_C(0x1C33), UINT8_C(0x05), SDO_U32, "sm3_minimum_cycle_time_ns"},
    {UINT16_C(0x1C33), UINT8_C(0x0A), SDO_U32, "sm3_sync0_cycle_time_ns"},
    {UINT16_C(0x1C33), UINT8_C(0x0B), SDO_U16, "sm3_event_missed_count"},
    {UINT16_C(0x1C33), UINT8_C(0x0C), SDO_U16, "sm3_cycle_too_small_count"},
    {UINT16_C(0x1C33), UINT8_C(0x0D), SDO_U16, "sm3_shift_too_short_count"},
    {UINT16_C(0x1C33), UINT8_C(0x20), SDO_U8, "sm3_sync_error"},
    {UINT16_C(0x2002), UINT8_C(0x01), SDO_U16, "input_mode"},
    {UINT16_C(0x603F), UINT8_C(0x00), SDO_U16, "error_code"},
    {UINT16_C(0x6060), UINT8_C(0x00), SDO_I8, "mode_of_operation"},
    {UINT16_C(0x6075), UINT8_C(0x00), SDO_U32, "motor_rated_current"},
    {UINT16_C(0x6076), UINT8_C(0x00), SDO_U32, "motor_rated_torque"},
    {UINT16_C(0x608F), UINT8_C(0x01), SDO_U32, "encoder_increments"},
    {UINT16_C(0x608F), UINT8_C(0x02), SDO_U32, "encoder_motor_revolutions"},
    {UINT16_C(0x6091), UINT8_C(0x01), SDO_U32, "gear_motor_revolutions"},
    {UINT16_C(0x6091), UINT8_C(0x02), SDO_U32, "gear_shaft_revolutions"},
    {UINT16_C(0x60C2), UINT8_C(0x01), SDO_U8, "interpolation_time_period"},
    {UINT16_C(0x60C2), UINT8_C(0x02), SDO_I8, "interpolation_time_index"},
    {UINT16_C(0x6502), UINT8_C(0x00), SDO_U32, "supported_drive_modes"},
};

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

static const char *sdo_type_name(sdo_value_type_t type)
{
    switch (type)
    {
        case SDO_U8:
            return "u8";
        case SDO_I8:
            return "i8";
        case SDO_U16:
            return "u16";
        case SDO_U32:
            return "u32";
        case SDO_STRING:
            return "string";
    }
    return "unknown";
}

static void print_sdo_read(FILE *output, ecx_contextt *context, uint16_t slave,
                           const sdo_descriptor_t *descriptor)
{
    uint8_t buffer[128] = {0};
    int size;
    int work_counter;

    if (descriptor->type == SDO_U8 || descriptor->type == SDO_I8)
    {
        size = 1;
    }
    else if (descriptor->type == SDO_U16)
    {
        size = 2;
    }
    else if (descriptor->type == SDO_U32)
    {
        size = 4;
    }
    else
    {
        size = (int)sizeof(buffer) - 1;
    }

    work_counter = ecx_SDOread(context, slave, descriptor->index, descriptor->subindex,
                               FALSE, &size, buffer, EC_TIMEOUTRXM);

    fputs("{\"index\":", output);
    fprintf(output, "\"0x%04X\",\"subindex\":%u,\"name\":", descriptor->index,
            (unsigned int)descriptor->subindex);
    json_string(output, descriptor->name);
    fputs(",\"type\":", output);
    json_string(output, sdo_type_name(descriptor->type));
    fprintf(output, ",\"ok\":%s", work_counter > 0 ? "true" : "false");

    if (work_counter > 0)
    {
        if (descriptor->type == SDO_U8)
        {
            fprintf(output, ",\"value\":%u", (unsigned int)buffer[0]);
        }
        else if (descriptor->type == SDO_I8)
        {
            int8_t value;
            memcpy(&value, buffer, sizeof(value));
            fprintf(output, ",\"value\":%d", (int)value);
        }
        else if (descriptor->type == SDO_U16)
        {
            uint16_t raw;
            memcpy(&raw, buffer, sizeof(raw));
            fprintf(output, ",\"value\":%u", (unsigned int)etohs(raw));
        }
        else if (descriptor->type == SDO_U32)
        {
            uint32_t raw;
            memcpy(&raw, buffer, sizeof(raw));
            fprintf(output, ",\"value\":%u", (unsigned int)etohl(raw));
        }
        else
        {
            size_t string_size = size > 0 && (size_t)size < sizeof(buffer) ? (size_t)size : 0U;
            while (string_size > 0U && buffer[string_size - 1U] == UINT8_C(0))
            {
                --string_size;
            }
            buffer[string_size] = UINT8_C(0);
            fputs(",\"value\":", output);
            json_string(output, (const char *)buffer);
        }
    }
    fputc('}', output);
}

static void utc_timestamp(char *buffer, size_t capacity)
{
    time_t now = time(NULL);
    struct tm result;

    if (gmtime_r(&now, &result) == NULL ||
        strftime(buffer, capacity, "%Y-%m-%dT%H:%M:%SZ", &result) == 0U)
    {
        snprintf(buffer, capacity, "unknown");
    }
}

static bool restore_init(ecx_contextt *context)
{
    ec_slavet *all_slaves = &context->slavelist[0];

    all_slaves->state = EC_STATE_INIT;
    ecx_writestate(context, 0U);
    return ecx_statecheck(context, 0U, EC_STATE_INIT, EC_TIMEOUTSTATE) == EC_STATE_INIT;
}

int emaster_diagnostic_list_interfaces(void)
{
    ec_adaptert *adapters = ec_find_adapters();
    ec_adaptert *adapter;

    if (adapters == NULL)
    {
        fprintf(stderr, "No network interfaces found.\n");
        return 1;
    }

    for (adapter = adapters; adapter != NULL; adapter = adapter->next)
    {
        printf("%s\t%s\n", adapter->name, adapter->desc);
    }
    ec_free_adapters(adapters);
    return 0;
}

int emaster_diagnostic_probe_run(const emaster_diagnostic_probe_options_t *options)
{
    ecx_contextt context;
    const emaster_slave_profile_t *target_profile;
    char timestamp[32];
    char temporary_path[4096];
    FILE *output = NULL;
    int file_descriptor = -1;
    int slave_count;
    int position;
    bool init_restored = false;
    int result = 1;

    if (options == NULL || options->interface_name == NULL || options->output_path == NULL)
    {
        fprintf(stderr, "Invalid diagnostic probe options.\n");
        return 1;
    }
    if (access(options->output_path, F_OK) == 0)
    {
        fprintf(stderr, "Refusing to overwrite existing fingerprint: %s\n", options->output_path);
        return 1;
    }
    if (snprintf(temporary_path, sizeof(temporary_path), "%s.partial.%ld", options->output_path,
                 (long)getpid()) >= (int)sizeof(temporary_path))
    {
        fprintf(stderr, "Output path is too long.\n");
        return 1;
    }

    memset(&context, 0, sizeof(context));
    if (!ecx_init(&context, options->interface_name))
    {
        fprintf(stderr, "Unable to open raw EtherCAT socket on %s.\n", options->interface_name);
        return 1;
    }

    slave_count = ecx_config_init(&context);
    if (slave_count <= 0)
    {
        fprintf(stderr, "No EtherCAT slaves found on %s.\n", options->interface_name);
        if (context.slavecount > 0)
        {
            (void)restore_init(&context);
        }
        ecx_close(&context);
        return 1;
    }
    ecx_readstate(&context);

    file_descriptor = open(temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0640);
    if (file_descriptor < 0)
    {
        fprintf(stderr, "Unable to create %s: %s\n", temporary_path, strerror(errno));
        init_restored = restore_init(&context);
        ecx_close(&context);
        return init_restored ? 1 : 2;
    }
    output = fdopen(file_descriptor, "w");
    if (output == NULL)
    {
        fprintf(stderr, "Unable to open fingerprint stream: %s\n", strerror(errno));
        close(file_descriptor);
        unlink(temporary_path);
        init_restored = restore_init(&context);
        ecx_close(&context);
        return init_restored ? 1 : 2;
    }

    utc_timestamp(timestamp, sizeof(timestamp));
    target_profile = emaster_slave_profile_by_id("cyberbeast.isvd90rc-300b-100-70.rev1");

    fputs("{\n  \"schema_version\": 1,\n  \"tool\": {\"name\": \"emaster-fingerprint\", ", output);
    fputs("\"version\": ", output);
    json_string(output, EMASTER_VERSION);
    fputs(", \"soem_revision\": ", output);
    json_string(output, EMASTER_SOEM_REVISION);
    fputs("},\n  \"captured_at_utc\": ", output);
    json_string(output, timestamp);
    fputs(",\n  \"interface\": ", output);
    json_string(output, options->interface_name);
    fprintf(output, ",\n  \"slave_count\": %d,\n  \"slaves\": [\n", slave_count);

    for (position = 1; position <= slave_count; ++position)
    {
        ec_slavet *slave = &context.slavelist[position];
        emaster_slave_identity_t actual = {
            .vendor_id = slave->eep_man,
            .product_code = slave->eep_id,
            .revision = slave->eep_rev,
        };
        size_t object_index;

        if (position > 1)
        {
            fputs(",\n", output);
        }
        fprintf(output, "    {\"position\":%d,\"name\":", position);
        json_string(output, slave->name);
        fprintf(output,
                ",\"sii_identity\":{\"vendor_id\":%u,\"product_code\":%u,"
                "\"revision\":%u},\"state\":%u,\"has_dc\":%s,\"has_coe\":%s,"
                "\"target_profile_match\":%s,\"sdo_reads\":[",
                (unsigned int)actual.vendor_id, (unsigned int)actual.product_code,
                (unsigned int)actual.revision, (unsigned int)slave->state,
                slave->hasdc != 0U ? "true" : "false",
                (slave->mbx_proto & ECT_MBXPROT_COE) != 0U ? "true" : "false",
                target_profile != NULL && emaster_slave_identity_matches(target_profile, &actual)
                    ? "true"
                    : "false");

        for (object_index = 0;
             object_index < sizeof(fingerprint_objects) / sizeof(fingerprint_objects[0]);
             ++object_index)
        {
            if (object_index > 0U)
            {
                fputc(',', output);
            }
            print_sdo_read(output, &context, (uint16_t)position,
                           &fingerprint_objects[object_index]);
        }
        fputs("]}", output);
    }

    init_restored = restore_init(&context);
    fprintf(output,
            "\n  ],\n  \"behavior\": {\"pdo_mapping_performed\": false,"
            "\"dc_configuration_performed\": false,\"highest_requested_state\": "
            "\"PRE-OP\",\"restore_init_succeeded\": %s}\n}\n",
            init_restored ? "true" : "false");

    if (fclose(output) != 0)
    {
        fprintf(stderr, "Failed to flush fingerprint: %s\n", strerror(errno));
        output = NULL;
        unlink(temporary_path);
    }
    else if (link(temporary_path, options->output_path) != 0)
    {
        fprintf(stderr, "Failed to publish fingerprint: %s\n", strerror(errno));
        unlink(temporary_path);
    }
    else
    {
        unlink(temporary_path);
        result = init_restored ? 0 : 2;
    }

    ecx_close(&context);
    if (!init_restored)
    {
        fprintf(stderr, "WARNING: one or more slaves did not confirm INIT restoration.\n");
    }
    return result;
}
