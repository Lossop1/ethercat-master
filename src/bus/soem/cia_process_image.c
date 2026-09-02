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
    EMASTER_CIA402_ACTUAL_POSITION_INDEX = 0x6064
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

static bool build_direction_codec(
    const emaster_pdo_direction_layout_t *actual,
    const emaster_pdo_mapping_profile_t *profiles,
    size_t profile_count,
    emaster_pdo_codec_field_t *fields,
    size_t field_count,
    uint16_t mode_index,
    size_t *mode_ordinal,
    size_t *status_ordinal,
    size_t *control_ordinal)
{
    size_t mapping_ordinal;
    size_t field_ordinal = 0U;
    bool mode_found = false;
    bool status_found = status_ordinal == NULL;
    bool control_found = control_ordinal == NULL;

    if (actual == NULL || fields == NULL || field_count == 0U || mode_ordinal == NULL)
    {
        return false;
    }
    *mode_ordinal = SIZE_MAX;
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
                *mode_ordinal = field_ordinal;
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
    return field_ordinal == field_count && mode_found && status_found && control_found;
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
    if (image->rx_fields == NULL || image->tx_fields == NULL ||
        image->rx_values == NULL || image->tx_values == NULL)
    {
        return false;
    }
    image->rx_field_count = rx_count;
    image->tx_field_count = tx_count;
    image->rx_target_position_ordinal = SIZE_MAX;
    image->tx_actual_position_ordinal = SIZE_MAX;
    if (!build_direction_codec(&image->layout.rx, axis->pdo_set->rx_mappings,
                               axis->pdo_set->rx_mapping_count, image->rx_fields,
                               rx_count, EMASTER_CIA402_MODE_OF_OPERATION_INDEX,
                               &image->rx_mode_ordinal, NULL,
                               &image->rx_control_ordinal) ||
        !build_direction_codec(&image->layout.tx, axis->pdo_set->tx_mappings,
                               axis->pdo_set->tx_mapping_count, image->tx_fields,
                               tx_count, EMASTER_CIA402_MODE_DISPLAY_INDEX,
                               &image->tx_mode_ordinal, &image->tx_status_ordinal, NULL))
    {
        return false;
    }
    if (axis->operation_mode->value == INT8_C(8))
    {
        image->rx_target_position_ordinal = field_ordinal_for(
            &image->layout.rx, EMASTER_CIA402_TARGET_POSITION_INDEX, UINT8_C(0));
        image->tx_actual_position_ordinal = field_ordinal_for(
            &image->layout.tx, EMASTER_CIA402_ACTUAL_POSITION_INDEX, UINT8_C(0));
        if (image->rx_target_position_ordinal == SIZE_MAX ||
            image->tx_actual_position_ordinal == SIZE_MAX ||
            image->rx_fields[image->rx_target_position_ordinal].kind !=
                EMASTER_PDO_CODEC_VALUE_SIGNED ||
            image->tx_fields[image->tx_actual_position_ordinal].kind !=
                EMASTER_PDO_CODEC_VALUE_SIGNED)
        {
            return false;
        }
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
    image->rx_values[image->rx_mode_ordinal].value.signed_value =
        axis->operation_mode->value;
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
    int32_t hold_target_position,
    uint8_t *output,
    size_t output_capacity)
{
    if (axis == NULL || axis->operation_mode == NULL || image == NULL || output == NULL)
    {
        return false;
    }
    image->rx_values[image->rx_control_ordinal].value.unsigned_value = control_word;
    image->rx_values[image->rx_mode_ordinal].value.signed_value =
        axis->operation_mode->value;
    if (image->rx_target_position_ordinal != SIZE_MAX)
    {
        image->rx_values[image->rx_target_position_ordinal].value.signed_value =
            hold_target_position;
    }
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
    int32_t *actual_position)
{
    if (image == NULL || input == NULL || mode_display == NULL ||
        status_word == NULL || actual_position == NULL ||
        emaster_pdo_codec_decode(&image->layout.tx, image->tx_fields,
                                 image->tx_field_count, input, input_length,
                                 image->tx_values, image->tx_field_count) !=
            EMASTER_PDO_CODEC_OK)
    {
        return false;
    }
    if (image->tx_values[image->tx_mode_ordinal].kind !=
            EMASTER_PDO_CODEC_VALUE_SIGNED ||
        image->tx_values[image->tx_status_ordinal].kind !=
            EMASTER_PDO_CODEC_VALUE_UNSIGNED)
    {
        return false;
    }
    *mode_display =
        (int8_t)image->tx_values[image->tx_mode_ordinal].value.signed_value;
    *status_word =
        (uint16_t)image->tx_values[image->tx_status_ordinal].value.unsigned_value;
    *actual_position = 0;
    if (image->tx_actual_position_ordinal != SIZE_MAX)
    {
        int64_t value =
            image->tx_values[image->tx_actual_position_ordinal].value.signed_value;

        if (image->tx_values[image->tx_actual_position_ordinal].kind !=
                EMASTER_PDO_CODEC_VALUE_SIGNED ||
            value < INT32_MIN || value > INT32_MAX)
        {
            return false;
        }
        *actual_position = (int32_t)value;
    }
    return true;
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
    }
    free(images);
}
