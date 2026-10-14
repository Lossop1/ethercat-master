/*
 * 采用设备实际映射时，两个方向的填充判定必须各归各位。
 *
 * 台架事实（MT，2026-10-14 实测）：厂商 ESI 第 7 条声明假字节 0x2FFD / 0x2FFE，设备实际
 * 两个方向都映射 0x5FFE。位长一样，物理意义主站不知道，也不读。早先两侧都按"条目对象号
 * 与字段类型必须双向等价"判，等于要求设备与 ESI 逐条相同 —— 那条采用设备实际布局的路
 * 就永远走不通：设备映射对、ESI 不对，进不了 OP。
 *
 * 这一条钉三件事：
 *   1. 非零对象号的填充字段：发出去恒为零，回读非零不判反馈无效；
 *   2. 对象号为零的真保留位：回读非零仍然判反馈无效；
 *   3. 真保留位被设备档声明成值字段：布局与字段两边说法不一致，仍然拒绝。
 *
 * 判错的两个方向代价不同：多拒一次，一台正常设备进不了 OP；少拒一次，保留位被当成数据。
 * 不接总线：发现侧的读函数是一个进程内的表驱动器，与同目录其他单测同一路子。
 */

#include "cia402/process_image.h"
#include "emaster/catalog/slave_profile.h"
#include "emaster/protocol/pdo_layout.h"
#include "emaster/session/session_plan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    MAPPING_ENTRIES = 7,
    PDO_BYTES = 16,
    /* 第 7 条就是那个方向上的假字节：设备实际值与 ESI 声明值分歧在这一条。 */
    VENDOR_BYTE_ORDINAL = 6,
    VENDOR_BYTE_SUBINDEX = 7
};

typedef struct
{
    uint16_t index;
    uint8_t subindex;
    uint8_t width;
    uint32_t value;
} cell_t;

/* TxPDO 第 7 条是唯一被换的那一条：三种形态各问一件事，所以单独提着，不写进表里。 */
static uint32_t device_tx_last = 0x5FFE0008U;

/*
 * 设备侧实际生效的映射，逐条照 2026-10-14 台架读回来的裸字写。
 * RxPDO 第 7 条是 0x5FFE —— 设备档里没有这个对象，主站不读它。
 */
static cell_t device_cells[] = {
    {0x1C12, 0U, 1U, 1U},
    {0x1C12, 1U, 2U, 0x1600U},
    {0x1C13, 0U, 1U, 1U},
    {0x1C13, 1U, 2U, 0x1A00U},
    {0x1600, 0U, 1U, MAPPING_ENTRIES},
    {0x1600, 1U, 4U, 0x60400010U},
    {0x1600, 2U, 4U, 0x607A0020U},
    {0x1600, 3U, 4U, 0x60FF0020U},
    {0x1600, 4U, 4U, 0x60710010U},
    {0x1600, 5U, 4U, 0x60720010U},
    {0x1600, 6U, 4U, 0x60600008U},
    {0x1600, 7U, 4U, 0x5FFE0008U},
    {0x1C13, 0U, 1U, MAPPING_ENTRIES},
    {0x1A00, 0U, 1U, MAPPING_ENTRIES},
    {0x1A00, 1U, 4U, 0x60410010U},
    {0x1A00, 2U, 4U, 0x60640020U},
    {0x1A00, 3U, 4U, 0x606C0020U},
    {0x1A00, 4U, 4U, 0x60770010U},
    {0x1A00, 5U, 4U, 0x603F0010U},
    {0x1A00, 6U, 4U, 0x60610008U},
};

static bool cell_lookup(uint16_t index, uint8_t subindex, uint8_t width, uint32_t *value)
{
    size_t ordinal;

    if (index == 0x1A00U && subindex == VENDOR_BYTE_SUBINDEX && width == 4U)
    {
        *value = device_tx_last;
        return true;
    }
    for (ordinal = 0U; ordinal < sizeof(device_cells) / sizeof(device_cells[0]); ++ordinal)
    {
        const cell_t *cell = &device_cells[ordinal];
        if (cell->index == index && cell->subindex == subindex && cell->width == width)
        {
            *value = cell->value;
            return true;
        }
    }
    return false;
}

static bool stub_read_u8(void *user_data, uint16_t index, uint8_t subindex, uint8_t *value)
{
    uint32_t raw = 0U;

    (void)user_data;
    if (!cell_lookup(index, subindex, 1U, &raw))
    {
        return false;
    }
    *value = (uint8_t)raw;
    return true;
}

static bool stub_read_u16(void *user_data, uint16_t index, uint8_t subindex, uint16_t *value)
{
    uint32_t raw = 0U;

    (void)user_data;
    if (!cell_lookup(index, subindex, 2U, &raw))
    {
        return false;
    }
    *value = (uint16_t)raw;
    return true;
}

static bool stub_read_u32(void *user_data, uint16_t index, uint8_t subindex, uint32_t *value)
{
    (void)user_data;
    return cell_lookup(index, subindex, 4U, value);
}

static const emaster_pdo_sdo_reader_t reader = {stub_read_u8, stub_read_u16, stub_read_u32, NULL};

/* 末条描述符由调用者给：设备侧要换的那一条，以及设备档里对应声明的那一条。 */
static void set_device_last(uint32_t tx_last)
{
    device_tx_last = tx_last;
}

static emaster_pdo_entry_t rx_entries[MAPPING_ENTRIES];
static emaster_pdo_entry_t tx_entries[MAPPING_ENTRIES];
static emaster_pdo_mapping_profile_t mappings[2];
static emaster_pdo_set_profile_t pdo_set;
static emaster_operation_mode_t operation_mode;
static emaster_slave_profile_t device_profile;

static const uint32_t profile_rx[MAPPING_ENTRIES] = {0x60400010U, 0x607A0020U, 0x60FF0020U,
                                                    0x60710010U, 0x60720010U, 0x60600008U,
                                                    0x2FFD0008U};
static const uint32_t profile_tx[MAPPING_ENTRIES] = {0x60410010U, 0x60640020U, 0x606C0020U,
                                                    0x60770010U, 0x603F0010U, 0x60610008U,
                                                    0x2FFE0008U};
static const char *const rx_names[MAPPING_ENTRIES] = {
    "controlword", "target-position", "target-velocity",
    "torque-offset", "max-torque",   "mode-of-operation",
    "dummy-byte-1"};
static const char *const tx_names[MAPPING_ENTRIES] = {
    "statusword", "actual-position", "actual-velocity",
    "actual-torque", "error-code",   "mode-display",
    "dummy-byte-2"};
/* 对象号靠名字不够，字段类型靠声明：过程映像按类型判有符号 / 无符号 / 填充。 */
static const char *const rx_types[MAPPING_ENTRIES] = {"UINT", "DINT", "DINT", "INT",
                                                      "UINT", "SINT", NULL};
static const char *const tx_types[MAPPING_ENTRIES] = {"UINT", "DINT", "DINT", "INT",
                                                      "UINT", "SINT", NULL};

static const char *const required_rx[] = {"controlword", "target-position",
                                          "mode-of-operation"};
static const char *const required_tx[] = {"statusword", "actual-position", "mode-display"};

static void fill_entries(emaster_pdo_entry_t *entries, const uint32_t *descriptors,
                         const char *const *names, const char *const *types)
{
    size_t ordinal;

    for (ordinal = 0U; ordinal < MAPPING_ENTRIES; ++ordinal)
    {
        entries[ordinal].index = (uint16_t)(descriptors[ordinal] >> 16U);
        entries[ordinal].subindex = (uint8_t)(descriptors[ordinal] >> 8U);
        entries[ordinal].bit_length = (uint8_t)descriptors[ordinal];
        entries[ordinal].data_type = types[ordinal];
        entries[ordinal].name = names[ordinal];
    }
}

/*
 * 建运行方案与设备档。设备档第 7 条照厂家 ESI 写：对象号 0x2FFE、没写 DataType。
 * 第三件事要把它换成"对象号零的值字段"这个矛盾形态，所以这一条也由调用者给。
 */
static void build_axis(emaster_session_axis_plan_t *axis, uint32_t profile_tx_last,
                       const char *profile_tx_last_type)
{
    fill_entries(rx_entries, profile_rx, rx_names, rx_types);
    fill_entries(tx_entries, profile_tx, tx_names, tx_types);
    tx_entries[VENDOR_BYTE_ORDINAL].index = (uint16_t)(profile_tx_last >> 16U);
    tx_entries[VENDOR_BYTE_ORDINAL].subindex = (uint8_t)(profile_tx_last >> 8U);
    tx_entries[VENDOR_BYTE_ORDINAL].bit_length = (uint8_t)profile_tx_last;
    tx_entries[VENDOR_BYTE_ORDINAL].data_type = profile_tx_last_type;

    mappings[0].index = 0x1600U;
    mappings[0].entries = rx_entries;
    mappings[0].entry_count = MAPPING_ENTRIES;
    mappings[0].bit_length = PDO_BYTES * 8U;
    mappings[1].index = 0x1A00U;
    mappings[1].entries = tx_entries;
    mappings[1].entry_count = MAPPING_ENTRIES;
    mappings[1].bit_length = PDO_BYTES * 8U;

    memset(&pdo_set, 0, sizeof(pdo_set));
    pdo_set.pdo_set_id = "check";
    pdo_set.assignment_style = EMASTER_PDO_ASSIGNMENT_DIRECT;
    pdo_set.rx_pdo_bytes = PDO_BYTES;
    pdo_set.tx_pdo_bytes = PDO_BYTES;
    pdo_set.rx_mappings = &mappings[0];
    pdo_set.rx_mapping_count = 1U;
    pdo_set.tx_mappings = &mappings[1];
    pdo_set.tx_mapping_count = 1U;

    memset(&operation_mode, 0, sizeof(operation_mode));
    operation_mode.mode_id = "check-csp";
    operation_mode.value = INT8_C(8);
    operation_mode.quantity = EMASTER_MODE_QUANTITY_POSITION;
    operation_mode.required_rx_fields = required_rx;
    operation_mode.required_rx_field_count = sizeof(required_rx) / sizeof(required_rx[0]);
    operation_mode.required_tx_fields = required_tx;
    operation_mode.required_tx_field_count = sizeof(required_tx) / sizeof(required_tx[0]);

    memset(&device_profile, 0, sizeof(device_profile));
    device_profile.supports_pdo_configuration = true;

    memset(axis, 0, sizeof(*axis));
    axis->device_profile = &device_profile;
    axis->operation_mode = &operation_mode;
    axis->pdo_set = &pdo_set;
}

/* 发现 + 建映像。失败返回 NULL，调用者不必清理。 */
static emaster_cia_process_image_t *open_image(const emaster_session_axis_plan_t *axis)
{
    emaster_cia_process_image_t *image = calloc(1U, sizeof(*image));

    if (image == NULL)
    {
        return NULL;
    }
    if (emaster_pdo_layout_discover(&reader, &image->layout) != EMASTER_PDO_DISCOVERY_COMPLETE ||
        !emaster_cia_process_image_init(axis, image))
    {
        emaster_cia_process_image_destroy(image, 1U);
        return NULL;
    }
    return image;
}

static int32_t read_le_i32(const uint8_t *bytes)
{
    return (int32_t)((uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
                     ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U));
}

static void write_le_i32(uint8_t *bytes, int32_t value)
{
    uint32_t raw = (uint32_t)value;

    bytes[0] = (uint8_t)raw;
    bytes[1] = (uint8_t)(raw >> 8U);
    bytes[2] = (uint8_t)(raw >> 16U);
    bytes[3] = (uint8_t)(raw >> 24U);
}

/*
 * 第一件与第二件：设备实际映射里的厂商对象（对象号非零、设备档没声明）在收发两个方向
 * 都按填充处理。发出去恒为零；回读什么值都不影响反馈有效性。
 */
static int check_undeclared_vendor_object(void)
{
    emaster_session_axis_plan_t axis;
    emaster_cia_process_image_t *image;
    uint8_t output[PDO_BYTES];
    uint8_t input[PDO_BYTES];
    int8_t mode_display = 0;
    uint16_t status_word = 0U;
    int32_t actual_value = 0;
    int failures = 0;

    set_device_last(0x5FFE0008U);
    build_axis(&axis, 0x2FFE0008U, NULL);
    image = open_image(&axis);
    if (image == NULL)
    {
        printf("不合格：设备实际映射里多一条厂商对象，映像建不起来\n");
        return 1;
    }

    memset(output, 0xFF, sizeof(output));
    if (!emaster_cia_process_image_prepare_output(&axis, image, output, sizeof(output)))
    {
        printf("不合格：多一条厂商对象的 RxPDO 编不出去\n");
        ++failures;
    }
    else if (output[0] != 0U || output[1] != 0U || (int8_t)output[14] != INT8_C(8) ||
             output[15] != 0U || read_le_i32(&output[2]) != 0)
    {
        printf("不合格：初次输出没按填充置零（控制字=%02X%02X 模式=%02X 厂商字节=%02X）\n",
               (unsigned)output[1], (unsigned)output[0], (unsigned)output[14],
               (unsigned)output[15]);
        ++failures;
    }

    memset(output, 0xFF, sizeof(output));
    if (!emaster_cia_process_image_update_output(&axis, image, UINT16_C(0x0006), 1234, output,
                                                 sizeof(output)))
    {
        printf("不合格：写目标时 RxPDO 编不出去\n");
        ++failures;
    }
    else if (output[0] != 0x06U || output[1] != 0x00U || read_le_i32(&output[2]) != 1234 ||
             output[15] != 0U)
    {
        printf("不合格：写目标时厂商对象被带上了值（控制字=%02X%02X 目标=%d 厂商字节=%02X）\n",
               (unsigned)output[1], (unsigned)output[0], (int)read_le_i32(&output[2]),
               (unsigned)output[15]);
        ++failures;
    }

    /* 设备往那个字节里放什么都行：它不是保留位，主站没有契约可依。 */
    {
        static const uint8_t vendor_readbacks[] = {0x00U, 0x01U, 0xA5U, 0xFFU};
        size_t ordinal;

        for (ordinal = 0U; ordinal < sizeof(vendor_readbacks); ++ordinal)
        {
            memset(input, 0, sizeof(input));
            input[0] = 0x37U;
            input[1] = 0x12U;
            write_le_i32(&input[2], 2085);
            input[14] = (uint8_t)INT8_C(8);
            input[15] = vendor_readbacks[ordinal];
            mode_display = 0;
            status_word = 0U;
            actual_value = 0;
            if (!emaster_cia_process_image_decode_input(image, input, sizeof(input),
                                                        &mode_display, &status_word,
                                                        &actual_value))
            {
                printf("不合格：厂商对象回读到 %02X 就把反馈判成无效\n",
                       (unsigned)vendor_readbacks[ordinal]);
                ++failures;
            }
            else if (status_word != UINT16_C(0x1237) || mode_display != INT8_C(8) ||
                     actual_value != 2085)
            {
                printf("不合格：厂商对象回读到 %02X 时反馈解错了（状态字=%04X 模式=%d 位置=%d）\n",
                       (unsigned)vendor_readbacks[ordinal], (unsigned)status_word,
                       (int)mode_display, (int)actual_value);
                ++failures;
            }
        }
    }

    emaster_cia_process_image_destroy(image, 1U);
    return failures;
}

/*
 * 第二件：对象号为零是真保留位，回读非零仍然是无效反馈。
 */
static int check_reserved_byte_still_enforced(void)
{
    emaster_session_axis_plan_t axis;
    emaster_cia_process_image_t *image;
    uint8_t input[PDO_BYTES];
    int8_t mode_display = 0;
    uint16_t status_word = 0U;
    int32_t actual_value = 0;
    int failures = 0;

    set_device_last(0x00000008U);
    build_axis(&axis, 0x2FFE0008U, NULL);
    image = open_image(&axis);
    if (image == NULL)
    {
        printf("不合格：真保留位的设备映射，映像建不起来\n");
        return 1;
    }

    memset(input, 0, sizeof(input));
    input[0] = 0x37U;
    input[1] = 0x12U;
    write_le_i32(&input[2], 2085);
    input[14] = (uint8_t)INT8_C(8);
    if (!emaster_cia_process_image_decode_input(image, input, sizeof(input), &mode_display,
                                                &status_word, &actual_value))
    {
        printf("不合格：真保留位回读为零，反馈却判成了无效\n");
        ++failures;
    }

    input[15] = 0x01U;
    if (emaster_cia_process_image_decode_input(image, input, sizeof(input), &mode_display,
                                               &status_word, &actual_value))
    {
        printf("不合格：真保留位回读非零，反馈却判成了有效\n");
        ++failures;
    }

    emaster_cia_process_image_destroy(image, 1U);
    return failures;
}

/*
 * 第三件：设备档把真保留位声明成值字段（对象号零、类型 UINT），布局与字段两边说法
 * 不一致，仍然拒绝。这种设备档本身是错的，但错档进来时不能安静地按值字段用起来。
 */
static int check_reserved_declared_as_value(void)
{
    emaster_session_axis_plan_t axis;
    emaster_cia_process_image_t *image;
    uint8_t input[PDO_BYTES];
    int8_t mode_display = 0;
    uint16_t status_word = 0U;
    int32_t actual_value = 0;
    int failures = 0;

    set_device_last(0x00000008U);
    build_axis(&axis, 0x00000008U, "UINT");
    image = open_image(&axis);
    if (image == NULL)
    {
        printf("不合格：保留位被声明成值字段时，映像本身应该建得起来\n");
        return 1;
    }

    memset(input, 0, sizeof(input));
    input[0] = 0x37U;
    input[1] = 0x12U;
    write_le_i32(&input[2], 2085);
    input[14] = (uint8_t)INT8_C(8);
    if (emaster_cia_process_image_decode_input(image, input, sizeof(input), &mode_display,
                                               &status_word, &actual_value))
    {
        printf("不合格：布局说保留位、字段说要读值，却按能用放行了\n");
        ++failures;
    }

    emaster_cia_process_image_destroy(image, 1U);
    return failures;
}

int main(void)
{
    int failures = 0;

    failures += check_undeclared_vendor_object();
    failures += check_reserved_byte_still_enforced();
    failures += check_reserved_declared_as_value();
    printf("不合格 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
