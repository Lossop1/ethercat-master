#include "emaster/protocol/pdo_codec.h"

#include <limits.h>
#include <string.h>

static bool value_kind_is_valid(emaster_pdo_codec_value_kind_t kind)
{
    return kind == EMASTER_PDO_CODEC_VALUE_UNSIGNED ||
           kind == EMASTER_PDO_CODEC_VALUE_SIGNED ||
           kind == EMASTER_PDO_CODEC_VALUE_PADDING;
}

static size_t add_size_checked(size_t left, size_t right, bool *overflow)
{
    if (right > SIZE_MAX - left)
    {
        *overflow = true;
        return SIZE_MAX;
    }
    return left + right;
}

size_t emaster_pdo_codec_field_count(const emaster_pdo_direction_layout_t *layout)
{
    size_t mapping_ordinal;
    size_t count = 0U;
    bool overflow = false;

    if (layout == NULL ||
        (layout->mapping_count > 0U && layout->mappings == NULL))
    {
        return SIZE_MAX;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < layout->mapping_count; ++mapping_ordinal)
    {
        count = add_size_checked(count, layout->mappings[mapping_ordinal].entry_count,
                                 &overflow);
        if (overflow)
        {
            return SIZE_MAX;
        }
    }
    return count;
}

size_t emaster_pdo_codec_byte_length(uint32_t bit_length)
{
    return (size_t)(bit_length / UINT32_C(8)) +
           (bit_length % UINT32_C(8) == 0U ? 0U : 1U);
}

static uint64_t value_mask(uint8_t bit_length)
{
    if (bit_length == EMASTER_PDO_CODEC_MAX_VALUE_BITS)
    {
        return UINT64_MAX;
    }
    return (UINT64_C(1) << bit_length) - UINT64_C(1);
}

static bool entry_fits_layout(const emaster_pdo_mapping_entry_t *entry,
                              uint32_t expected_offset, uint32_t direction_length)
{
    uint32_t bit_length;

    if (entry == NULL || entry->bit_length == 0U ||
        entry->bit_length > EMASTER_PDO_CODEC_MAX_VALUE_BITS ||
        entry->bit_offset != expected_offset)
    {
        return false;
    }
    bit_length = (uint32_t)entry->bit_length;
    return entry->bit_offset <= direction_length &&
           bit_length <= direction_length - entry->bit_offset;
}

static bool entry_is_padding(const emaster_pdo_mapping_entry_t *entry)
{
    return entry->object_index == UINT16_C(0) &&
           entry->object_subindex == UINT8_C(0);
}

emaster_pdo_codec_status_t
emaster_pdo_codec_validate(const emaster_pdo_direction_layout_t *layout,
                           const emaster_pdo_codec_field_t *fields, size_t field_count)
{
    size_t expected_field_count;
    size_t mapping_ordinal;
    size_t field_ordinal = 0U;
    uint32_t direction_offset = 0U;

    if (layout == NULL ||
        (layout->mapping_count > 0U && layout->mappings == NULL) ||
        (field_count > 0U && fields == NULL))
    {
        return EMASTER_PDO_CODEC_INVALID_ARGUMENT;
    }
    expected_field_count = emaster_pdo_codec_field_count(layout);
    if (expected_field_count == SIZE_MAX)
    {
        return EMASTER_PDO_CODEC_INVALID_LAYOUT;
    }
    if (field_count != expected_field_count)
    {
        return EMASTER_PDO_CODEC_FIELD_COUNT_MISMATCH;
    }

    for (mapping_ordinal = 0U; mapping_ordinal < layout->mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &layout->mappings[mapping_ordinal];
        uint32_t mapping_offset = 0U;
        size_t entry_ordinal;

        if (mapping->mapping_index == 0U ||
            (mapping->entry_count > 0U && mapping->entries == NULL))
        {
            return EMASTER_PDO_CODEC_INVALID_LAYOUT;
        }
        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];
            const emaster_pdo_codec_field_t *field = &fields[field_ordinal];
            uint32_t next_offset;

            if (!entry_fits_layout(entry, direction_offset, layout->bit_length))
            {
                return entry->bit_length > EMASTER_PDO_CODEC_MAX_VALUE_BITS
                           ? EMASTER_PDO_CODEC_UNSUPPORTED_BIT_LENGTH
                           : EMASTER_PDO_CODEC_INVALID_LAYOUT;
            }
            if (!value_kind_is_valid(field->kind))
            {
                return EMASTER_PDO_CODEC_INVALID_ARGUMENT;
            }
            if (entry_is_padding(entry) !=
                (field->kind == EMASTER_PDO_CODEC_VALUE_PADDING))
            {
                return EMASTER_PDO_CODEC_VALUE_KIND_MISMATCH;
            }
            if (entry->bit_length > EMASTER_PDO_CODEC_MAX_VALUE_BITS)
            {
                return EMASTER_PDO_CODEC_UNSUPPORTED_BIT_LENGTH;
            }
            next_offset = direction_offset + (uint32_t)entry->bit_length;
            if (next_offset < direction_offset)
            {
                return EMASTER_PDO_CODEC_INVALID_LAYOUT;
            }
            direction_offset = next_offset;
            if ((uint32_t)entry->bit_length > UINT32_MAX - mapping_offset)
            {
                return EMASTER_PDO_CODEC_INVALID_LAYOUT;
            }
            mapping_offset += (uint32_t)entry->bit_length;
            ++field_ordinal;
        }
        if (mapping->bit_length != mapping_offset)
        {
            return EMASTER_PDO_CODEC_INVALID_LAYOUT;
        }
    }
    if (direction_offset != layout->bit_length || field_ordinal != field_count)
    {
        return EMASTER_PDO_CODEC_INVALID_LAYOUT;
    }
    return EMASTER_PDO_CODEC_OK;
}

static bool value_fits(const emaster_pdo_codec_value_t *value, uint8_t bit_length)
{
    if (value->kind == EMASTER_PDO_CODEC_VALUE_UNSIGNED)
    {
        return bit_length == EMASTER_PDO_CODEC_MAX_VALUE_BITS ||
               value->value.unsigned_value <= value_mask(bit_length);
    }
    if (value->kind == EMASTER_PDO_CODEC_VALUE_SIGNED)
    {
        int64_t minimum;
        int64_t maximum;

        if (bit_length == EMASTER_PDO_CODEC_MAX_VALUE_BITS)
        {
            return true;
        }
        minimum = -(INT64_C(1) << (bit_length - UINT8_C(1)));
        maximum = (INT64_C(1) << (bit_length - UINT8_C(1))) - INT64_C(1);
        return value->value.signed_value >= minimum && value->value.signed_value <= maximum;
    }
    if (value->kind == EMASTER_PDO_CODEC_VALUE_PADDING)
    {
        /* 发往从站的保留位必须保持为零，禁止上层借填充位传递隐含数据。 */
        return value->value.unsigned_value == UINT64_C(0);
    }
    return false;
}

static uint64_t value_as_raw(const emaster_pdo_codec_value_t *value, uint8_t bit_length)
{
    uint64_t raw;

    if (value->kind == EMASTER_PDO_CODEC_VALUE_PADDING)
    {
        return UINT64_C(0);
    }
    raw = value->kind == EMASTER_PDO_CODEC_VALUE_UNSIGNED
              ? value->value.unsigned_value
              : (uint64_t)value->value.signed_value;
    return raw & value_mask(bit_length);
}

static void write_bits(uint8_t *buffer, const emaster_pdo_mapping_entry_t *entry,
                       uint64_t raw)
{
    uint8_t bit_ordinal;

    for (bit_ordinal = 0U; bit_ordinal < entry->bit_length; ++bit_ordinal)
    {
        uint32_t absolute_bit = entry->bit_offset + (uint32_t)bit_ordinal;
        uint8_t bit_mask = (uint8_t)(UINT8_C(1) << (absolute_bit % UINT32_C(8)));
        uint8_t *target = &buffer[absolute_bit / UINT32_C(8)];

        if ((raw & (UINT64_C(1) << bit_ordinal)) != 0U)
        {
            *target = (uint8_t)(*target | bit_mask);
        }
    }
}

static uint64_t read_bits(const uint8_t *buffer, const emaster_pdo_mapping_entry_t *entry)
{
    uint8_t bit_ordinal;
    uint64_t raw = 0U;

    for (bit_ordinal = 0U; bit_ordinal < entry->bit_length; ++bit_ordinal)
    {
        uint32_t absolute_bit = entry->bit_offset + (uint32_t)bit_ordinal;
        uint8_t bit_mask = (uint8_t)(UINT8_C(1) << (absolute_bit % UINT32_C(8)));

        if ((buffer[absolute_bit / UINT32_C(8)] & bit_mask) != 0U)
        {
            raw |= UINT64_C(1) << bit_ordinal;
        }
    }
    return raw;
}

static void decoded_value(emaster_pdo_codec_value_t *value,
                          emaster_pdo_codec_value_kind_t kind, uint8_t bit_length,
                          uint64_t raw)
{
    value->kind = kind;
    if (kind == EMASTER_PDO_CODEC_VALUE_UNSIGNED ||
        kind == EMASTER_PDO_CODEC_VALUE_PADDING)
    {
        value->value.unsigned_value = raw;
        return;
    }
    if (bit_length < EMASTER_PDO_CODEC_MAX_VALUE_BITS &&
        (raw & (UINT64_C(1) << (bit_length - UINT8_C(1)))) != 0U)
    {
        raw |= ~value_mask(bit_length);
    }
    value->value.signed_value = (int64_t)raw;
}

emaster_pdo_codec_status_t
emaster_pdo_codec_encode(const emaster_pdo_direction_layout_t *layout,
                         const emaster_pdo_codec_field_t *fields, size_t field_count,
                         const emaster_pdo_codec_value_t *values, size_t value_count,
                         uint8_t *buffer, size_t buffer_capacity)
{
    size_t required_bytes;
    size_t mapping_ordinal;
    size_t field_ordinal = 0U;
    emaster_pdo_codec_status_t status;

    status = emaster_pdo_codec_validate(layout, fields, field_count);
    if (status != EMASTER_PDO_CODEC_OK)
    {
        return status;
    }
    if (value_count != field_count)
    {
        return EMASTER_PDO_CODEC_FIELD_COUNT_MISMATCH;
    }
    if (value_count > 0U && values == NULL)
    {
        return EMASTER_PDO_CODEC_INVALID_ARGUMENT;
    }
    for (field_ordinal = 0U; field_ordinal < field_count; ++field_ordinal)
    {
        if (values[field_ordinal].kind != fields[field_ordinal].kind)
        {
            return EMASTER_PDO_CODEC_VALUE_KIND_MISMATCH;
        }
        /* 位宽在布局校验中已经限制为 1..64，范围检查在第二次遍历中使用对应条目。 */
    }
    required_bytes = emaster_pdo_codec_byte_length(layout->bit_length);
    if (required_bytes > 0U && buffer == NULL)
    {
        return EMASTER_PDO_CODEC_INVALID_ARGUMENT;
    }
    if (buffer_capacity < required_bytes)
    {
        return EMASTER_PDO_CODEC_BUFFER_TOO_SMALL;
    }

    field_ordinal = 0U;
    for (mapping_ordinal = 0U; mapping_ordinal < layout->mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &layout->mappings[mapping_ordinal];
        size_t entry_ordinal;

        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];

            if (!value_fits(&values[field_ordinal], entry->bit_length))
            {
                return EMASTER_PDO_CODEC_VALUE_OUT_OF_RANGE;
            }
            ++field_ordinal;
        }
    }

    if (required_bytes > 0U)
    {
        memset(buffer, 0, required_bytes);
    }
    field_ordinal = 0U;
    for (mapping_ordinal = 0U; mapping_ordinal < layout->mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &layout->mappings[mapping_ordinal];
        size_t entry_ordinal;

        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];
            write_bits(buffer, entry, value_as_raw(&values[field_ordinal], entry->bit_length));
            ++field_ordinal;
        }
    }
    return EMASTER_PDO_CODEC_OK;
}

emaster_pdo_codec_status_t
emaster_pdo_codec_decode(const emaster_pdo_direction_layout_t *layout,
                         const emaster_pdo_codec_field_t *fields, size_t field_count,
                         const uint8_t *buffer, size_t buffer_length,
                         emaster_pdo_codec_value_t *values, size_t value_capacity)
{
    size_t required_bytes;
    size_t mapping_ordinal;
    size_t field_ordinal = 0U;
    emaster_pdo_codec_status_t status;

    status = emaster_pdo_codec_validate(layout, fields, field_count);
    if (status != EMASTER_PDO_CODEC_OK)
    {
        return status;
    }
    if (field_count > 0U && values == NULL)
    {
        return EMASTER_PDO_CODEC_INVALID_ARGUMENT;
    }
    if (value_capacity < field_count)
    {
        return EMASTER_PDO_CODEC_BUFFER_TOO_SMALL;
    }
    required_bytes = emaster_pdo_codec_byte_length(layout->bit_length);
    if (required_bytes > 0U && buffer == NULL)
    {
        return EMASTER_PDO_CODEC_INVALID_ARGUMENT;
    }
    if (buffer_length < required_bytes)
    {
        return EMASTER_PDO_CODEC_BUFFER_TOO_SMALL;
    }

    for (mapping_ordinal = 0U; mapping_ordinal < layout->mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &layout->mappings[mapping_ordinal];
        size_t entry_ordinal;

        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];
            uint64_t raw = read_bits(buffer, entry);

            decoded_value(&values[field_ordinal], fields[field_ordinal].kind,
                          entry->bit_length, raw);
            ++field_ordinal;
        }
    }
    return EMASTER_PDO_CODEC_OK;
}
