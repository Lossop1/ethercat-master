#include "process_image.h"

#include "emaster/catalog/slave_profile.h"
#include "emaster/cia402/controller.h"

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
    EMASTER_CIA402_DC_LINK_VOLTAGE_INDEX = 0x6079,
    EMASTER_CIA402_ERROR_CODE_INDEX = 0x603F
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

static const emaster_pdo_entry_t *profile_entry_for(
    const emaster_pdo_mapping_profile_t *mapping, uint16_t object_index,
    uint8_t object_subindex)
{
    size_t entry_ordinal;

    if (mapping == NULL || mapping->entries == NULL)
    {
        return NULL;
    }
    for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
    {
        const emaster_pdo_entry_t *entry = &mapping->entries[entry_ordinal];

        if (entry->index == object_index && entry->subindex == object_subindex)
        {
            return entry;
        }
    }
    return NULL;
}

/* 展开后的字段序号对应的布局条目；序号越界返回 NULL。 */
static const emaster_pdo_mapping_entry_t *layout_entry_for_field(
    const emaster_pdo_direction_layout_t *layout, size_t ordinal)
{
    size_t mapping_ordinal;
    size_t current = 0U;

    if (layout == NULL)
    {
        return NULL;
    }
    for (mapping_ordinal = 0U; mapping_ordinal < layout->mapping_count; ++mapping_ordinal)
    {
        const emaster_pdo_mapping_t *mapping = &layout->mappings[mapping_ordinal];
        size_t entry_ordinal;

        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            if (current == ordinal)
            {
                return &mapping->entries[entry_ordinal];
            }
            ++current;
        }
    }
    return NULL;
}

/*
 * 配置声明的类型决定该对象在过程数据里的位宽与符号 —— 位宽只有一个来源，不靠猜。
 */
static bool sdo_value_type_geometry(emaster_config_sdo_value_type_t type,
                                    uint32_t *bit_length, bool *is_signed)
{
    switch (type)
    {
    case EMASTER_CONFIG_SDO_VALUE_U8:
        *bit_length = 8U;
        *is_signed = false;
        return true;
    case EMASTER_CONFIG_SDO_VALUE_I8:
        *bit_length = 8U;
        *is_signed = true;
        return true;
    case EMASTER_CONFIG_SDO_VALUE_U16:
        *bit_length = 16U;
        *is_signed = false;
        return true;
    case EMASTER_CONFIG_SDO_VALUE_I16:
        *bit_length = 16U;
        *is_signed = true;
        return true;
    case EMASTER_CONFIG_SDO_VALUE_U32:
        *bit_length = 32U;
        *is_signed = false;
        return true;
    case EMASTER_CONFIG_SDO_VALUE_I32:
        *bit_length = 32U;
        *is_signed = true;
        return true;
    default:
        break;
    }
    return false;
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

        if (profile_mapping == NULL || profile_mapping->entries == NULL)
        {
            return false;
        }
        for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
        {
            const emaster_pdo_mapping_entry_t *actual_entry =
                &mapping->entries[entry_ordinal];
            /*
             * 按对象号认字段，不按位置认。设备实际映射里多出来的那些条目 —— 厂商 ESI
             * 没声明、主站也不读 —— 在这里就是填充：不参与解码，也不会顶着别的字段的
             * 名字被写出去。厂商 ESI 与固件出厂默认可以逐条不同（MT 台架实测：ESI 第 7
             * 条假字节是 0x2FFD，设备映射 0x5FFE），差在谁都不读的字节上时映像照样能用，
             * 设备也不必为这条差异被改写。
             */
            const emaster_pdo_entry_t *profile_entry = profile_entry_for(
                profile_mapping, actual_entry->object_index, actual_entry->object_subindex);
            emaster_pdo_codec_value_kind_t kind = EMASTER_PDO_CODEC_VALUE_PADDING;

            if (profile_entry != NULL)
            {
                if (actual_entry->bit_length != profile_entry->bit_length)
                {
                    return false;
                }
                kind = field_kind_for(profile_entry);
                if (kind == EMASTER_PDO_CODEC_VALUE_PADDING &&
                    profile_entry->index != UINT16_C(0))
                {
                    return false;
                }
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

/*
 * 把方案为对象声明的固定值落到字段上。
 *
 * 只有"这个对象也在这套 RxPDO 里"才需要落：不在里面的对象没有周期值会顶掉那次 SDO
 * 写。位宽对不上、或者那个字段本来就是主站每拍要写的量（控制字、模式、目标），都是
 * 配置里两处说法打架，返回 false 让会话起不来，而不是挑一个写下去。
 */
static bool build_declared_outputs(const emaster_session_axis_plan_t *axis,
                                   emaster_cia_process_image_t *image)
{
    size_t command_index;

    image->rx_declared = calloc(image->rx_field_count, sizeof(*image->rx_declared));
    if (image->rx_declared == NULL)
    {
        return false;
    }
    for (command_index = 0U;
         command_index < axis->operation_mode->safeop_to_op_sdo_write_count;
         ++command_index)
    {
        const emaster_sdo_write_config_t *command =
            &axis->operation_mode->safeop_to_op_sdo_writes[command_index];
        size_t ordinal = field_ordinal_for(&image->layout.rx, command->index,
                                           command->subindex);
        const emaster_pdo_mapping_entry_t *entry;
        uint32_t bit_length = 0U;
        bool is_signed = false;

        if (ordinal == SIZE_MAX)
        {
            continue;
        }
        entry = layout_entry_for_field(&image->layout.rx, ordinal);
        if (entry == NULL ||
            !sdo_value_type_geometry(command->type, &bit_length, &is_signed) ||
            bit_length != entry->bit_length)
        {
            return false;
        }
        if (ordinal == image->rx_control_ordinal || ordinal == image->rx_mode_ordinal ||
            ordinal == image->rx_target_position_ordinal ||
            ordinal == image->rx_target_velocity_ordinal ||
            ordinal == image->rx_target_torque_ordinal)
        {
            return false;
        }
        image->rx_fields[ordinal].kind = is_signed ? EMASTER_PDO_CODEC_VALUE_SIGNED
                                                   : EMASTER_PDO_CODEC_VALUE_UNSIGNED;
        image->rx_declared[ordinal].present = true;
        image->rx_declared[ordinal].value = command->value;
    }
    return true;
}

/* 每拍把声明的固定值填进过程映像；声明里没有的字段不动。 */
static void apply_declared_outputs(emaster_cia_process_image_t *image)
{
    size_t field_ordinal;

    for (field_ordinal = 0U; field_ordinal < image->rx_field_count; ++field_ordinal)
    {
        if (!image->rx_declared[field_ordinal].present)
        {
            continue;
        }
        image->rx_values[field_ordinal].kind = image->rx_fields[field_ordinal].kind;
        if (image->rx_fields[field_ordinal].kind == EMASTER_PDO_CODEC_VALUE_SIGNED)
        {
            image->rx_values[field_ordinal].value.signed_value =
                image->rx_declared[field_ordinal].value;
        }
        else
        {
            image->rx_values[field_ordinal].value.unsigned_value =
                (uint64_t)image->rx_declared[field_ordinal].value;
        }
    }
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
    image->tx_error_code_ordinal = SIZE_MAX;
    image->rx_declared = NULL;
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
    image->configured_quantity = axis->operation_mode->quantity;
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
    image->tx_error_code_ordinal = field_ordinal_for(
        &image->layout.tx, EMASTER_CIA402_ERROR_CODE_INDEX, UINT8_C(0));
    /*
     * 有周期的模式要求"主站下发的那个量"在本套 PDO 里；反馈是遥测，缺了不算错。
     * 回零连目标字段都不要求——一台只回零、不报位置的驱动器照样能把参考点找到。
     *
     * **位置模式是唯一的例外**：它的停机含义是"停在当前位置"，命令值直接由当前位置
     * 算出来（见 session_start.c 的 hold_target_value）。位置读不回来，主站就只能拿 0
     * 当保持目标，一使能轴就朝原点走——那比拒开更危险。
     *
     * 速度模式为什么不能要求反馈：厂商 ESI 里专给速度模式的那道帧（模块 #x319800，
     * Rx 0x1602 / Tx 0x1A02）反馈只有状态字与实际位置，**没有 0x606C 实际速度**。
     * 2026-10-15 台架实测：0xF030 选这道模块后真机回报的正是 6041+6064。要求它有
     * 606C，等于判厂商自己声明的那道帧不可用。（手册 5.7.1.1 / 5.8.1 的表把 1A02
     * 写成 6041+606C，与 ESI 和真机都不符；以真机为准。）
     *
     * 按声明的量挑字段，不按模式号：同一个模式号在不同厂商的说明书里指的不是同一件事。
     */
    switch (axis->operation_mode->quantity)
    {
    case EMASTER_MODE_QUANTITY_POSITION:
        if (image->rx_target_position_ordinal == SIZE_MAX ||
            image->tx_actual_position_ordinal == SIZE_MAX)
        {
            return false;
        }
        break;
    case EMASTER_MODE_QUANTITY_VELOCITY:
        if (image->rx_target_velocity_ordinal == SIZE_MAX)
        {
            return false;
        }
        break;
    case EMASTER_MODE_QUANTITY_TORQUE:
        if (image->rx_target_torque_ordinal == SIZE_MAX)
        {
            return false;
        }
        break;
    case EMASTER_MODE_QUANTITY_NONE:
    default:
        break;
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
    return build_declared_outputs(axis, image);
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
    apply_declared_outputs(image);
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
    /* 先落声明值：它填的字段与控制字、模式、目标都不重叠（建表时已挡住）。 */
    apply_declared_outputs(image);
    image->rx_values[image->rx_control_ordinal].value.unsigned_value = control_word;
    if (image->rx_mode_available)
    {
        image->rx_values[image->rx_mode_ordinal].value.signed_value =
            axis->operation_mode->value;
    }
    if (axis->operation_mode->quantity == EMASTER_MODE_QUANTITY_NONE)
    {
        /*
         * 回零没有周期目标：参考点搜寻由驱动器自己走，主站每拍只送控制字与模式本身。
         * 目标字段一个都不写——写哪个都是错的，而"写 0"与"写当前实际位置"在不同
         * 驱动器上含义不同，没有一条是本模式定义的。
         */
        return emaster_pdo_codec_encode(&image->layout.rx, image->rx_fields,
                                        image->rx_field_count, image->rx_values,
                                        image->rx_field_count, output,
                                        output_capacity) == EMASTER_PDO_CODEC_OK;
    }
    /* 目标写进本模式声明的那个量对应的字段；声明里没有的量的字段不写。 */
    switch (axis->operation_mode->quantity)
    {
    case EMASTER_MODE_QUANTITY_POSITION:
        image->rx_values[image->rx_target_position_ordinal].value.signed_value = target_value;
        break;
    case EMASTER_MODE_QUANTITY_VELOCITY:
        image->rx_values[image->rx_target_velocity_ordinal].value.signed_value = target_value;
        break;
    case EMASTER_MODE_QUANTITY_TORQUE:
        image->rx_values[image->rx_target_torque_ordinal].value.signed_value = target_value;
        break;
    case EMASTER_MODE_QUANTITY_NONE:
    default:
        return false;
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
    /*
     * 保留位回读必须为零。判据按**布局里的条目**认，不按字段类型认：类型为填充的
     * 字段有两种身份——对象号零是布局明写的保留位，非零是设备实际映射里有、设备档
     * 没声明的厂商对象（同样按填充编码）。后者不是保留位，设备往里放什么值主站没有
     * 契约可依，拿它判反馈无效会把一台正常的设备判死。MT 台架就是这么一条。
     */
    {
        size_t mapping_ordinal;
        size_t field_ordinal = 0U;

        for (mapping_ordinal = 0U; mapping_ordinal < image->layout.tx.mapping_count;
             ++mapping_ordinal)
        {
            const emaster_pdo_mapping_t *mapping = &image->layout.tx.mappings[mapping_ordinal];
            size_t entry_ordinal;

            for (entry_ordinal = 0U; entry_ordinal < mapping->entry_count; ++entry_ordinal)
            {
                const emaster_pdo_mapping_entry_t *entry = &mapping->entries[entry_ordinal];

                if (field_ordinal >= image->tx_field_count)
                {
                    return false;
                }
                if (entry->object_index == UINT16_C(0) &&
                    entry->object_subindex == UINT8_C(0) &&
                    image->tx_values[field_ordinal].value.unsigned_value != UINT64_C(0))
                {
                    return false;
                }
                ++field_ordinal;
            }
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
        /* 反馈按声明的量挑；回零与位置模式同一路（参考点搜寻的结果就是实际位置）。 */
        if (image->configured_quantity == EMASTER_MODE_QUANTITY_VELOCITY)
        {
            feedback_ordinal = image->tx_actual_velocity_ordinal;
        }
        else if (image->configured_quantity == EMASTER_MODE_QUANTITY_TORQUE)
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

bool emaster_cia_process_image_feedback(const emaster_cia_process_image_t *image,
                                        emaster_cia_feedback_t feedback,
                                        int32_t *value)
{
    size_t ordinal;
    int64_t raw;

    if (image == NULL || value == NULL)
    {
        return false;
    }
    ordinal = feedback == EMASTER_CIA_FEEDBACK_VELOCITY
                  ? image->tx_actual_velocity_ordinal
                  : feedback == EMASTER_CIA_FEEDBACK_TORQUE
                        ? image->tx_actual_torque_ordinal
                        : image->tx_actual_position_ordinal;
    if (ordinal == SIZE_MAX || ordinal >= image->tx_field_count ||
        image->tx_values[ordinal].kind != EMASTER_PDO_CODEC_VALUE_SIGNED)
    {
        return false;
    }
    raw = image->tx_values[ordinal].value.signed_value;
    if (raw < INT32_MIN || raw > INT32_MAX ||
        (feedback == EMASTER_CIA_FEEDBACK_TORQUE && (raw < INT16_MIN || raw > INT16_MAX)))
    {
        return false;
    }
    *value = (int32_t)raw;
    return true;
}

bool emaster_cia_process_image_drive_error_code(const emaster_cia_process_image_t *image,
                                                uint16_t *value)
{
    size_t ordinal;

    if (image == NULL || value == NULL)
    {
        return false;
    }
    ordinal = image->tx_error_code_ordinal;
    if (ordinal == SIZE_MAX || ordinal >= image->tx_field_count ||
        image->tx_values[ordinal].kind != EMASTER_PDO_CODEC_VALUE_UNSIGNED)
    {
        return false;
    }
    if (image->tx_values[ordinal].value.unsigned_value > UINT16_MAX)
    {
        return false;
    }
    *value = (uint16_t)image->tx_values[ordinal].value.unsigned_value;
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
        free(images[axis_index].rx_declared);
    }
    free(images);
}
