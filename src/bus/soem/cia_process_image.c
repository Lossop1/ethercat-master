#include "cia_process_image.h"

#include "emaster/catalog/slave_profile.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* 以下索引属于 CiA 402 标准对象，不是部署参数。 */
enum
{
    EMASTER_CIA402_CONTROL_WORD_INDEX = 0x6040,
    EMASTER_CIA402_STATUS_WORD_INDEX = 0x6041,
    EMASTER_CIA402_MODE_OF_OPERATION_INDEX = 0x6060,
    EMASTER_CIA402_MODE_DISPLAY_INDEX = 0x6061,
    EMASTER_CIA402_TARGET_POSITION_INDEX = 0x607A,
    EMASTER_CIA402_ACTUAL_POSITION_INDEX = 0x6064,
    EMASTER_CIA402_TARGET_VELOCITY_INDEX = 0x60FF,
    EMASTER_CIA402_ACTUAL_VELOCITY_INDEX = 0x606C,
    EMASTER_CIA402_TARGET_TORQUE_INDEX = 0x6071,
    EMASTER_CIA402_ACTUAL_TORQUE_INDEX = 0x6077,
    EMASTER_CIA402_ACTUAL_CURRENT_INDEX = 0x6078,
    EMASTER_CIA402_DC_LINK_VOLTAGE_INDEX = 0x6079
};

static const emaster_pdo_mapping_profile_t *profile_mapping_for(
    const emaster_pdo_mapping_profile_t *mappings, size_t mapping_count,
    uint16_t mapping_index)
{
    size_t mapping_ordinal;

    if (mappings == NULL)
    {
        return NULL;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < mapping_count; ++mapping_ordinal)
    {
        if (mappings[mapping_ordinal].index == mapping_index)
        {
            return &mappings[mapping_ordinal];
        }
    }
    return NULL;
}

static emaster_pdo_codec_value_kind_t field_kind_for(const emaster_pdo_entry_t *entry)
{
    if (entry == NULL || entry->data_type == NULL)
    {
        return EMASTER_PDO_CODEC_VALUE_PADDING;
    }
    if (strcmp(entry->data_type, "PADDING") == 0)
    {
        return EMASTER_PDO_CODEC_VALUE_PADDING;
    }
    if (strcmp(entry->data_type, "SINT") == 0 ||
        strcmp(entry->data_type, "INT") == 0 ||
        strcmp(entry->data_type, "DINT") == 0 ||
        strcmp(entry->data_type, "LINT") == 0)
    {
        return EMASTER_PDO_CODEC_VALUE_SIGNED;
    }
    if (strcmp(entry->data_type, "USINT") == 0 ||
        strcmp(entry->data_type, "UINT") == 0 ||
        strcmp(entry->data_type, "UDINT") == 0 ||
        strcmp(entry->data_type, "ULINT") == 0)
    {
        return EMASTER_PDO_CODEC_VALUE_UNSIGNED;
    }
    return EMASTER_PDO_CODEC_VALUE_PADDING;
}

static size_t field_ordinal_for(const emaster_pdo_direction_layout_t *layout,
                                uint16_t object_index, uint8_t object_subindex)
{
    size_t mapping_ordinal;
    size_t field_ordinal = 0U;

    if (layout == NULL)
    {
        return SIZE_MAX;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < layout->mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &layout->mappings[mapping_ordinal];
        size_t entry_ordinal;

        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];

            if (entry->object_index == object_index &&
                entry->object_subindex == object_subindex)
            {
                return field_ordinal;
            }
            ++field_ordinal;
        }
    }
    return SIZE_MAX;
}

/*
 * 运行方案中的 required_*_fields 是控制器实际依赖的字段契约
 * 不能只由 Python 校验器检查，否则其他构建方式可能绕过校验后仍然发送错误 PDO
 */
static bool required_fields_present(
    const emaster_pdo_mapping_profile_t *profiles,
    size_t profile_count,
    const char *const *required,
    size_t required_count)
{
    size_t required_index;

    if (required_count > 0U && (required == NULL || profiles == NULL))
    {
        return false;
    }
    for (required_index = 0U; required_index < required_count; ++required_index)
    {
        size_t mapping_index;
        bool found = false;

        if (required[required_index] == NULL || required[required_index][0] == '\0')
        {
            return false;
        }
        for (mapping_index = 0U; mapping_index < profile_count && !found; ++mapping_index)
        {
            const emaster_pdo_mapping_profile_t *mapping = &profiles[mapping_index];
            size_t entry_index;

            if (mapping->entries == NULL)
            {
                return false;
            }
            for (entry_index = 0U; entry_index < mapping->entry_count; ++entry_index)
            {
                const emaster_pdo_entry_t *entry = &mapping->entries[entry_index];

                if (entry->name != NULL && strcmp(entry->name, required[required_index]) == 0)
                {
                    found = true;
                    break;
                }
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

static bool build_direction_codec(
    const emaster_pdo_direction_layout_t *actual,
    const emaster_pdo_mapping_profile_t *profiles,
    size_t profile_count,
    emaster_pdo_codec_field_t *fields,
    size_t field_count,
    uint16_t mode_index,
    bool mode_required,
    size_t *mode_ordinal,
    size_t *status_ordinal,
    size_t *control_ordinal)
{
    size_t mapping_ordinal;
    size_t field_ordinal = 0U;
    bool mode_found = false;
    bool status_found = status_ordinal == NULL;
    bool control_found = control_ordinal == NULL;

    if (actual == NULL || fields == NULL || field_count == 0U)
    {
        return false;
    }
    if (mode_ordinal != NULL)
    {
        *mode_ordinal = SIZE_MAX;
    }
    if (status_ordinal != NULL)
    {
        *status_ordinal = SIZE_MAX;
    }
    if (control_ordinal != NULL)
    {
        *control_ordinal = SIZE_MAX;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < actual->mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &actual->mappings[mapping_ordinal];
        const emaster_pdo_mapping_profile_t *profile_mapping =
            profile_mapping_for(profiles, profile_count, mapping->mapping_index);
        size_t entry_ordinal;

        if (profile_mapping == NULL || profile_mapping->entries == NULL ||
            profile_mapping->entry_count != mapping->entry_count)
        {
            return false;
        }
        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *actual_entry =
                &mapping->entries[entry_ordinal];
            const emaster_pdo_entry_t *profile_entry =
                &profile_mapping->entries[entry_ordinal];
            emaster_pdo_codec_value_kind_t kind;

            if (actual_entry->object_index != profile_entry->index ||
                actual_entry->object_subindex != profile_entry->subindex ||
                actual_entry->bit_length != profile_entry->bit_length)
            {
                return false;
            }
            kind = field_kind_for(profile_entry);
            if (kind == EMASTER_PDO_CODEC_VALUE_PADDING &&
                profile_entry->index != UINT16_C(0))
            {
                return false;
            }
            fields[field_ordinal].kind = kind;
            if (actual_entry->object_index == mode_index &&
                actual_entry->object_subindex == 0U)
            {
                if (mode_found || kind != EMASTER_PDO_CODEC_VALUE_SIGNED)
                {
                    return false;
                }
                if (mode_ordinal != NULL)
                {
                    *mode_ordinal = field_ordinal;
                }
                mode_found = true;
            }
            if (status_ordinal != NULL &&
                actual_entry->object_index == EMASTER_CIA402_STATUS_WORD_INDEX &&
                actual_entry->object_subindex == 0U)
            {
                if (status_found || kind != EMASTER_PDO_CODEC_VALUE_UNSIGNED)
                {
                    return false;
                }
                *status_ordinal = field_ordinal;
                status_found = true;
            }
            if (control_ordinal != NULL &&
                actual_entry->object_index == EMASTER_CIA402_CONTROL_WORD_INDEX &&
                actual_entry->object_subindex == 0U)
            {
                if (control_found || kind != EMASTER_PDO_CODEC_VALUE_UNSIGNED)
                {
                    return false;
                }
                *control_ordinal = field_ordinal;
                control_found = true;
            }
            ++field_ordinal;
        }
    }
    return field_ordinal == field_count && (!mode_required || mode_found) && status_found &&
           control_found;
}

bool emaster_cia_process_image_init(const emaster_session_axis_plan_t *axis,
                                    emaster_cia_process_image_t *image)
{
    size_t rx_count;
    size_t tx_count;

    if (axis == NULL || axis->pdo_set == NULL || axis->operation_mode == NULL ||
        image == NULL)
    {
        return false;
    }
    rx_count = emaster_pdo_codec_field_count(&image->layout.rx);
    tx_count = emaster_pdo_codec_field_count(&image->layout.tx);
    if (rx_count == SIZE_MAX || tx_count == SIZE_MAX || rx_count == 0U || tx_count == 0U)
    {
        return false;
    }
    image->rx_fields = calloc(rx_count, sizeof(*image->rx_fields));
    image->tx_fields = calloc(tx_count, sizeof(*image->tx_fields));
    image->rx_values = calloc(rx_count, sizeof(*image->rx_values));
    image->tx_values = calloc(tx_count, sizeof(*image->tx_values));
    image->rx_audit_records = calloc(rx_count, sizeof(*image->rx_audit_records));
    image->tx_audit_records = calloc(tx_count, sizeof(*image->tx_audit_records));
    if (image->rx_fields == NULL || image->tx_fields == NULL ||
        image->rx_values == NULL || image->tx_values == NULL ||
        image->rx_audit_records == NULL || image->tx_audit_records == NULL)
    {
        return false;
    }
    image->rx_field_count = rx_count;
    image->tx_field_count = tx_count;
    for (size_t field = 0U; field < rx_count; ++field)
    {
        image->rx_audit_records[field] = SIZE_MAX;
    }
    for (size_t field = 0U; field < tx_count; ++field)
    {
        image->tx_audit_records[field] = SIZE_MAX;
    }
    image->rx_target_position_ordinal = SIZE_MAX;
    image->tx_actual_position_ordinal = SIZE_MAX;
    image->rx_target_velocity_ordinal = SIZE_MAX;
    image->tx_actual_velocity_ordinal = SIZE_MAX;
    image->rx_target_torque_ordinal = SIZE_MAX;
    image->tx_actual_torque_ordinal = SIZE_MAX;
    image->tx_actual_current_ordinal = SIZE_MAX;
    image->tx_dc_link_voltage_ordinal = SIZE_MAX;
    image->rx_mode_available = false;
    image->tx_mode_available = false;
    if (!build_direction_codec(&image->layout.rx, axis->pdo_set->rx_mappings,
                               axis->pdo_set->rx_mapping_count, image->rx_fields,
                               rx_count, EMASTER_CIA402_MODE_OF_OPERATION_INDEX,
                               false,
                               &image->rx_mode_ordinal, NULL,
                               &image->rx_control_ordinal) ||
        !build_direction_codec(&image->layout.tx, axis->pdo_set->tx_mappings,
                               axis->pdo_set->tx_mapping_count, image->tx_fields,
                               tx_count, EMASTER_CIA402_MODE_DISPLAY_INDEX,
                               false,
                               &image->tx_mode_ordinal, &image->tx_status_ordinal, NULL))
    {
        return false;
    }
    if (!required_fields_present(axis->pdo_set->rx_mappings,
                                 axis->pdo_set->rx_mapping_count,
                                 axis->operation_mode->required_rx_fields,
                                 axis->operation_mode->required_rx_field_count) ||
        !required_fields_present(axis->pdo_set->tx_mappings,
                                 axis->pdo_set->tx_mapping_count,
                                 axis->operation_mode->required_tx_fields,
                                 axis->operation_mode->required_tx_field_count))
    {
        return false;
    }
    image->rx_mode_available = image->rx_mode_ordinal != SIZE_MAX;
    image->tx_mode_available = image->tx_mode_ordinal != SIZE_MAX;
    image->configured_mode = axis->operation_mode->value;
    image->rx_target_position_ordinal = field_ordinal_for(
        &image->layout.rx, EMASTER_CIA402_TARGET_POSITION_INDEX, UINT8_C(0));
    image->tx_actual_position_ordinal = field_ordinal_for(
        &image->layout.tx, EMASTER_CIA402_ACTUAL_POSITION_INDEX, UINT8_C(0));
    image->rx_target_velocity_ordinal = field_ordinal_for(
        &image->layout.rx, EMASTER_CIA402_TARGET_VELOCITY_INDEX, UINT8_C(0));
    image->tx_actual_velocity_ordinal = field_ordinal_for(
        &image->layout.tx, EMASTER_CIA402_ACTUAL_VELOCITY_INDEX, UINT8_C(0));
    image->rx_target_torque_ordinal = field_ordinal_for(
        &image->layout.rx, EMASTER_CIA402_TARGET_TORQUE_INDEX, UINT8_C(0));
    image->tx_actual_torque_ordinal = field_ordinal_for(
        &image->layout.tx, EMASTER_CIA402_ACTUAL_TORQUE_INDEX, UINT8_C(0));
    image->tx_actual_current_ordinal = field_ordinal_for(
        &image->layout.tx, EMASTER_CIA402_ACTUAL_CURRENT_INDEX, UINT8_C(0));
    image->tx_dc_link_voltage_ordinal = field_ordinal_for(
        &image->layout.tx, EMASTER_CIA402_DC_LINK_VOLTAGE_INDEX, UINT8_C(0));
    if (axis->operation_mode->value == INT8_C(8) &&
        (image->rx_target_position_ordinal == SIZE_MAX || image->tx_actual_position_ordinal == SIZE_MAX))
    {
        return false;
    }
    if (axis->operation_mode->value == INT8_C(9) &&
        (image->rx_target_velocity_ordinal == SIZE_MAX || image->tx_actual_velocity_ordinal == SIZE_MAX))
    {
        return false;
    }
    if (axis->operation_mode->value == INT8_C(10) &&
        (image->rx_target_torque_ordinal == SIZE_MAX || image->tx_actual_torque_ordinal == SIZE_MAX))
    {
        return false;
    }
    if ((image->rx_target_position_ordinal != SIZE_MAX && image->rx_fields[image->rx_target_position_ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED) ||
        (image->tx_actual_position_ordinal != SIZE_MAX && image->tx_fields[image->tx_actual_position_ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED) ||
        (image->rx_target_velocity_ordinal != SIZE_MAX && image->rx_fields[image->rx_target_velocity_ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED) ||
        (image->tx_actual_velocity_ordinal != SIZE_MAX && image->tx_fields[image->tx_actual_velocity_ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED) ||
        (image->rx_target_torque_ordinal != SIZE_MAX && image->rx_fields[image->rx_target_torque_ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED) ||
        (image->tx_actual_torque_ordinal != SIZE_MAX && image->tx_fields[image->tx_actual_torque_ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED))
    {
        return false;
    }
    return true;
}

bool emaster_cia_process_image_prepare_output(
    const emaster_session_axis_plan_t *axis,
    emaster_cia_process_image_t *image,
    uint8_t *output,
    size_t output_capacity)
{
    size_t field_ordinal;

    if (axis == NULL || axis->operation_mode == NULL || image == NULL || output == NULL)
    {
        return false;
    }
    memset(image->rx_values, 0, image->rx_field_count * sizeof(*image->rx_values));
    for (field_ordinal = 0U; field_ordinal < image->rx_field_count; ++field_ordinal)
    {
        image->rx_values[field_ordinal].kind = image->rx_fields[field_ordinal].kind;
    }
    if (image->rx_mode_available)
    {
        image->rx_values[image->rx_mode_ordinal].value.signed_value =
            axis->operation_mode->value;
    }
    image->rx_values[image->rx_control_ordinal].value.unsigned_value = UINT64_C(0);
    return emaster_pdo_codec_encode(&image->layout.rx, image->rx_fields,
                                    image->rx_field_count, image->rx_values,
                                    image->rx_field_count, output, output_capacity) ==
           EMASTER_PDO_CODEC_OK;
}

bool emaster_cia_process_image_update_output(
    const emaster_session_axis_plan_t *axis,
    emaster_cia_process_image_t *image,
    uint16_t control_word,
    int32_t target_value,
    uint8_t *output,
    size_t output_capacity)
{
    if (axis == NULL || axis->operation_mode == NULL || image == NULL || output == NULL)
    {
        return false;
    }
    image->rx_values[image->rx_control_ordinal].value.unsigned_value = control_word;
    if (image->rx_mode_available)
    {
        image->rx_values[image->rx_mode_ordinal].value.signed_value =
            axis->operation_mode->value;
    }
    if (axis->operation_mode->value == INT8_C(8))
        image->rx_values[image->rx_target_position_ordinal].value.signed_value = target_value;
    else if (axis->operation_mode->value == INT8_C(9))
        image->rx_values[image->rx_target_velocity_ordinal].value.signed_value = target_value;
    else if (axis->operation_mode->value == INT8_C(10))
        image->rx_values[image->rx_target_torque_ordinal].value.signed_value = target_value;
    else
        return false;
    return emaster_pdo_codec_encode(&image->layout.rx, image->rx_fields,
                                    image->rx_field_count, image->rx_values,
                                    image->rx_field_count, output, output_capacity) ==
           EMASTER_PDO_CODEC_OK;
}

bool emaster_cia_process_image_decode_input(
    emaster_cia_process_image_t *image,
    const uint8_t *input,
    size_t input_length,
    int8_t *mode_display,
    uint16_t *status_word,
    int32_t *actual_value)
{
    if (image == NULL || input == NULL || mode_display == NULL ||
        status_word == NULL || actual_value == NULL ||
        emaster_pdo_codec_decode(&image->layout.tx, image->tx_fields,
                                 image->tx_field_count, input, input_length,
                                 image->tx_values, image->tx_field_count) !=
            EMASTER_PDO_CODEC_OK)
    {
        return false;
    }
    if (image->tx_values[image->tx_status_ordinal].kind !=
            EMASTER_PDO_CODEC_VALUE_UNSIGNED)
    {
        return false;
    }
    for (size_t field = 0U; field < image->tx_field_count; ++field)
    {
        if (image->tx_values[field].kind == EMASTER_PDO_CODEC_VALUE_PADDING &&
            image->tx_values[field].value.unsigned_value != 0U)
        {
            return false;
        }
    }
    *mode_display = 0;
    if (image->tx_mode_available)
    {
        if (image->tx_values[image->tx_mode_ordinal].kind !=
            EMASTER_PDO_CODEC_VALUE_SIGNED)
        {
            return false;
        }
        *mode_display =
            (int8_t)image->tx_values[image->tx_mode_ordinal].value.signed_value;
    }
    *status_word =
        (uint16_t)image->tx_values[image->tx_status_ordinal].value.unsigned_value;
    *actual_value = 0;
    {
        size_t feedback_ordinal = image->tx_actual_position_ordinal;
        if (image->configured_mode == INT8_C(9))
        {
            feedback_ordinal = image->tx_actual_velocity_ordinal;
        }
        else if (image->configured_mode == INT8_C(10))
            feedback_ordinal = image->tx_actual_torque_ordinal;
        if (feedback_ordinal != SIZE_MAX)
        {
            int64_t value = image->tx_values[feedback_ordinal].value.signed_value;
            if (image->tx_values[feedback_ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED ||
                value < INT32_MIN || value > INT32_MAX) return false;
            *actual_value = (int32_t)value;
        }
    }
    /* 固定 PDO 没有模式字段时，按运行方案的模式值选择反馈。 */
    if (!image->tx_mode_available)
    {
        /* 固定方案目前仅有 CSP，CSV/CST 方案必须声明对应反馈字段并由动态 PDO 提供模式。 */
    }
    return true;
}

static bool audit_direction_values(
    const emaster_pdo_direction_layout_t *layout,
    const emaster_pdo_codec_value_t *values,
    size_t value_count,
    size_t *last_records,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    emaster_audit_direction_t direction,
    uint16_t slave_position,
    uint64_t exchange,
    bool succeeded)
{
    size_t mapping_ordinal;
    size_t value_ordinal = 0U;

    if (layout == NULL || values == NULL || audit == NULL)
    {
        return false;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < layout->mapping_count;
         ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &layout->mappings[mapping_ordinal];
        size_t entry_ordinal;

        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count;
             ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];
            emaster_audit_value_kind_t kind;
            uint64_t unsigned_value = 0U;
            int64_t signed_value = 0;

            if (value_ordinal >= value_count)
            {
                return false;
            }
            kind = values[value_ordinal].kind == EMASTER_PDO_CODEC_VALUE_SIGNED
                       ? EMASTER_AUDIT_VALUE_SIGNED
                       : EMASTER_AUDIT_VALUE_UNSIGNED;
            if (kind == EMASTER_AUDIT_VALUE_SIGNED)
            {
                signed_value = values[value_ordinal].value.signed_value;
            }
            else if (values[value_ordinal].kind == EMASTER_PDO_CODEC_VALUE_UNSIGNED)
            {
                unsigned_value = values[value_ordinal].value.unsigned_value;
            }
            if (!emaster_run_audit_record_pdo(
                    audit, &last_records[value_ordinal], phase, direction, kind, slave_position,
                    entry->object_index, entry->object_subindex,
                    entry->bit_length, entry->bit_offset, exchange, succeeded,
                    unsigned_value, signed_value))
            {
                return false;
            }
            ++value_ordinal;
        }
    }
    return value_ordinal == value_count;
}

bool emaster_cia_process_image_audit_output(
    const emaster_cia_process_image_t *image,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    uint16_t slave_position,
    uint64_t exchange,
    bool succeeded)
{
    return image != NULL &&
           audit_direction_values(&image->layout.rx, image->rx_values,
                                  image->rx_field_count, image->rx_audit_records, audit, phase,
                                  EMASTER_AUDIT_DIRECTION_WRITE, slave_position,
                                  exchange, succeeded);
}

bool emaster_cia_process_image_audit_input(
    const emaster_cia_process_image_t *image,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    uint16_t slave_position,
    uint64_t exchange)
{
    return image != NULL &&
           audit_direction_values(&image->layout.tx, image->tx_values,
                                  image->tx_field_count, image->tx_audit_records, audit, phase,
                                  EMASTER_AUDIT_DIRECTION_READ, slave_position,
                                  exchange, true);
}

void emaster_cia_process_image_destroy(emaster_cia_process_image_t *images,
                                       size_t count)
{
    size_t axis_index;

    if (images == NULL)
    {
        return;
    }
    for (axis_index = 0U; axis_index < count; ++axis_index)
    {
        emaster_pdo_layout_destroy(&images[axis_index].layout);
        free(images[axis_index].rx_fields);
        free(images[axis_index].tx_fields);
        free(images[axis_index].rx_values);
        free(images[axis_index].tx_values);
        free(images[axis_index].rx_audit_records);
        free(images[axis_index].tx_audit_records);
    }
    free(images);
}
