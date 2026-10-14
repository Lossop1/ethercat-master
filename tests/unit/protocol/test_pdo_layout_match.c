/*
 * 设备实际映射与厂商 ESI 声明只差一个假字节时，判定必须是"不匹配"。
 *
 * 台架事实（MT，2026-10-14 实测）：ESI 的 0x1600:07 / 0x1A00:07 是假字节 0x2FFD / 0x2FFE，
 * 设备实际两个方向都映射 0x5FFE。位长相同、过程映像同宽，但描述符不是同一个 ——
 * 主站必须当作不一致，否则"选对了哪张映射表"会被当成"表里的内容也对"，而设备侧到底
 * 映射了什么没有任何人核对过。
 *
 * 这一条钉四件事：
 *   1. 发现侧把读回来的裸字拆成了什么（对象号 / 子索引 / 位长 / 位偏移）；
 *   2. 拼接公式两侧共用 —— 把档里的条目拼回去，等于设备读回来的裸字；
 *   3. 只差这一条描述符时 matches 为假，换成设备实际值才为真；
 *   4. 差在没人读的字节上时判定为"按设备实际建映像"（DEVICE_ADOPTED），差异逐条列出；
 *      差异落在运行方案要用的字段上时才要求改写设备。
 *
 * 不接总线：发现侧的读函数是一个进程内的表驱动器，与 checks/ 下那些离线检查同一路子。
 */

#include "emaster/catalog/slave_profile.h"
#include "emaster/protocol/pdo_layout.h"
#include "emaster/session/session_plan.h"

#include <stdio.h>
#include <string.h>

enum
{
    RX_MAPPING_ENTRIES = 7,
    PDO_BYTES = 16
};

typedef struct
{
    uint16_t index;
    uint8_t subindex;
    uint8_t width;
    uint32_t value;
} cell_t;

/* 设备侧实际生效的分配与映射，逐条照 2026-10-14 台架读回来的裸字写。 */
static const cell_t device_cells[] = {
    {0x1C12, 0U, 1U, 1U},
    {0x1C12, 1U, 2U, 0x1600U},
    {0x1C13, 0U, 1U, 1U},
    {0x1C13, 1U, 2U, 0x1A00U},
    {0x1600, 0U, 1U, 7U},
    {0x1600, 1U, 4U, 0x60400010U},
    {0x1600, 2U, 4U, 0x607A0020U},
    {0x1600, 3U, 4U, 0x60FF0020U},
    {0x1600, 4U, 4U, 0x60710010U},
    {0x1600, 5U, 4U, 0x60720010U},
    {0x1600, 6U, 4U, 0x60600008U},
    {0x1600, 7U, 4U, 0x5FFE0008U},
    {0x1A00, 0U, 1U, 7U},
    {0x1A00, 1U, 4U, 0x60410010U},
    {0x1A00, 2U, 4U, 0x60640020U},
    {0x1A00, 3U, 4U, 0x606C0020U},
    {0x1A00, 4U, 4U, 0x60770010U},
    {0x1A00, 5U, 4U, 0x603F0010U},
    {0x1A00, 6U, 4U, 0x60610008U},
    {0x1A00, 7U, 4U, 0x5FFE0008U},
};

static bool cell_lookup(uint16_t index, uint8_t subindex, uint8_t width, uint32_t *value)
{
    size_t ordinal;

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

/* 设备档里的一张映射表。descriptor 逐条给，最后一条由调用者换成设备实际值。 */
static emaster_pdo_entry_t rx_entries[RX_MAPPING_ENTRIES];
static emaster_pdo_entry_t tx_entries[RX_MAPPING_ENTRIES];
static emaster_pdo_mapping_profile_t mappings[2];
static emaster_pdo_set_profile_t pdo_set;

/* 条目名按对象号给：会话侧判"能不能直接用"就是按这些名字找对象。 */
static const char *const rx_names[RX_MAPPING_ENTRIES] = {
    "controlword",     "target-position",     "target-velocity",
    "torque-offset",   "max-torque",          "mode-of-operation",
    "filler-unused"};
static const char *const tx_names[RX_MAPPING_ENTRIES] = {
    "statusword",      "actual-position",     "actual-velocity",
    "actual-torque",   "error-code",          "mode-display",
    "filler-unused"};

/* 运行方案真正依赖的字段：正好落在没人读的那条差异之外。 */
static const char *const required_rx[] = {"controlword", "target-position", "mode-of-operation"};
static const char *const required_tx[] = {"statusword", "actual-position", "mode-display"};

static emaster_operation_mode_t operation_mode;
static emaster_slave_profile_t device_profile;

static void build_axis(emaster_session_axis_plan_t *axis)
{
    const char *const *rx_fields = required_rx;
    const char *const *tx_fields = required_tx;

    memset(&operation_mode, 0, sizeof(operation_mode));
    operation_mode.mode_id = "check-mode";
    operation_mode.required_rx_fields = rx_fields;
    operation_mode.required_rx_field_count = sizeof(required_rx) / sizeof(required_rx[0]);
    operation_mode.required_tx_fields = tx_fields;
    operation_mode.required_tx_field_count = sizeof(required_tx) / sizeof(required_tx[0]);

    memset(&device_profile, 0, sizeof(device_profile));
    device_profile.supports_pdo_configuration = true;

    memset(axis, 0, sizeof(*axis));
    axis->device_profile = &device_profile;
    axis->operation_mode = &operation_mode;
    axis->pdo_set = &pdo_set;
}

static const uint32_t esi_rx[RX_MAPPING_ENTRIES] = {0x60400010U, 0x607A0020U, 0x60FF0020U,
                                                    0x60710010U, 0x60720010U, 0x60600008U,
                                                    0x2FFD0008U};
static const uint32_t esi_tx[RX_MAPPING_ENTRIES] = {0x60410010U, 0x60640020U, 0x606C0020U,
                                                    0x60770010U, 0x603F0010U, 0x60610008U,
                                                    0x2FFE0008U};

static void fill_entries(emaster_pdo_entry_t *entries, const uint32_t *descriptors,
                         const char *const *names)
{
    size_t ordinal;

    for (ordinal = 0U; ordinal < RX_MAPPING_ENTRIES; ++ordinal)
    {
        entries[ordinal].index = (uint16_t)(descriptors[ordinal] >> 16U);
        entries[ordinal].subindex = (uint8_t)(descriptors[ordinal] >> 8U);
        entries[ordinal].bit_length = (uint8_t)descriptors[ordinal];
        entries[ordinal].data_type = "UINT";
        entries[ordinal].name = names[ordinal];
    }
}

static void build_profile(uint32_t rx_last, uint32_t tx_last)
{
    fill_entries(rx_entries, esi_rx, rx_names);
    fill_entries(tx_entries, esi_tx, tx_names);
    rx_entries[RX_MAPPING_ENTRIES - 1].index = (uint16_t)(rx_last >> 16U);
    tx_entries[RX_MAPPING_ENTRIES - 1].index = (uint16_t)(tx_last >> 16U);

    mappings[0].index = 0x1600U;
    mappings[0].entries = rx_entries;
    mappings[0].entry_count = RX_MAPPING_ENTRIES;
    mappings[0].bit_length = PDO_BYTES * 8U;
    mappings[1].index = 0x1A00U;
    mappings[1].entries = tx_entries;
    mappings[1].entry_count = RX_MAPPING_ENTRIES;
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
}

int main(void)
{
    const emaster_pdo_sdo_reader_t reader = {stub_read_u8, stub_read_u16, stub_read_u32, NULL};
    emaster_pdo_layout_t layout;
    int failures = 0;

    if (emaster_pdo_layout_discover(&reader, &layout) != EMASTER_PDO_DISCOVERY_COMPLETE)
    {
        printf("不合格：设备侧映射没有完整发现，status=%d\n", (int)layout.status);
        emaster_pdo_layout_destroy(&layout);
        return 1;
    }
    if (layout.rx.mapping_count != 1U || layout.rx.mappings[0].mapping_index != 0x1600U ||
        layout.rx.mappings[0].entry_count != RX_MAPPING_ENTRIES ||
        layout.rx.bit_length != PDO_BYTES * 8U || layout.tx.bit_length != PDO_BYTES * 8U)
    {
        printf("不合格：发现出来的分配/条目数与设备不符\n");
        ++failures;
    }
    if (layout.rx.mappings[0].entries[0].bit_offset != 0U ||
        layout.rx.mappings[0].entries[1].bit_offset != 16U ||
        layout.rx.mappings[0].entries[RX_MAPPING_ENTRIES - 1].bit_offset != 120U)
    {
        printf("不合格：逐条位偏移不是按声明顺序累加的\n");
        ++failures;
    }

    /* 拼接公式与拆分互为逆运算：把设备读回来的裸字拼回去必须一模一样。 */
    for (size_t ordinal = 0U; ordinal < RX_MAPPING_ENTRIES; ++ordinal)
    {
        const emaster_pdo_mapping_entry_t *entry = &layout.rx.mappings[0].entries[ordinal];
        uint32_t rebuilt = emaster_pdo_mapping_descriptor(
            entry->object_index, entry->object_subindex, entry->bit_length);
        if (rebuilt != entry->raw_descriptor)
        {
            printf("不合格：第 %zu 条描述符拼回去不是原值（0x%08X != 0x%08X）\n", ordinal,
                   (unsigned)rebuilt, (unsigned)entry->raw_descriptor);
            ++failures;
        }
    }

    /* 厂商 ESI 声明的那一份：最后一条是 0x2FFD / 0x2FFE —— 必须判为不匹配。 */
    build_profile(0x2FFD0008U, 0x2FFE0008U);
    if (emaster_slave_pdo_set_layout_matches(&pdo_set, &layout))
    {
        printf("不合格：ESI 声明的假字节与设备实际不同，却判成了匹配\n");
        ++failures;
    }

    /* 换成设备实际映射的那一份：匹配。 */
    build_profile(0x5FFE0008U, 0x5FFE0008U);
    if (!emaster_slave_pdo_set_layout_matches(&pdo_set, &layout))
    {
        printf("不合格：与设备实际一致的映射被判成了不匹配\n");
        ++failures;
    }

    /* 位长相同但对象号不同，仍然是不匹配 —— 不能只看字节数。 */
    build_profile(0x5FFD0008U, 0x5FFE0008U);
    if (emaster_slave_pdo_set_layout_matches(&pdo_set, &layout))
    {
        printf("不合格：只有位长相同就判成了匹配\n");
        ++failures;
    }

    /* 差异逐条列出：两处，都在第 7 条，各写清声明值与设备值。 */
    {
        emaster_pdo_layout_difference_t differences[4];
        size_t difference_count;

        build_profile(0x2FFD0008U, 0x2FFE0008U);
        difference_count =
            emaster_slave_pdo_layout_differences(&pdo_set, &layout, differences, 4U);
        if (difference_count != 2U ||
            differences[0].mapping_index != 0x1600U || differences[0].subindex != 7U ||
            differences[0].declared_descriptor != 0x2FFD0008U ||
            differences[0].device_descriptor != 0x5FFE0008U ||
            differences[1].mapping_index != 0x1A00U || differences[1].subindex != 7U ||
            differences[1].declared_descriptor != 0x2FFE0008U ||
            differences[1].device_descriptor != 0x5FFE0008U)
        {
            printf("不合格：差异清单与设备实际不符（%zu 处）\n", difference_count);
            ++failures;
        }
        /* 容量小于差异数时只写得出容量那么多，但计数仍是全部。 */
        if (emaster_slave_pdo_layout_differences(&pdo_set, &layout, differences, 1U) != 2U)
        {
            printf("不合格：容量不够时差异总数被截短了\n");
            ++failures;
        }

        /* 与设备一致的一份：一处差异都没有。 */
        build_profile(0x5FFE0008U, 0x5FFE0008U);
        if (emaster_slave_pdo_layout_differences(&pdo_set, &layout, differences, 4U) != 0U)
        {
            printf("不合格：与设备一致的映射仍被列成差异\n");
            ++failures;
        }
    }

    /* 差异只落在没人读的字节上：按设备实际建映像，不改写设备。 */
    {
        emaster_session_axis_plan_t axis;

        build_profile(0x2FFD0008U, 0x2FFE0008U);
        build_axis(&axis);
        if (emaster_session_axis_validate_layout(&axis, &layout) !=
            EMASTER_SESSION_LAYOUT_DEVICE_ADOPTED)
        {
            printf("不合格：差异落在没人读的字节上，却没判成按设备实际建映像\n");
            ++failures;
        }

        /* 差异落到运行方案要用的字段上（target-position 的对象号对不上）：要求改写。 */
        build_profile(0x2FFD0008U, 0x2FFE0008U);
        rx_entries[1].index = 0x607BU;
        if (emaster_session_axis_validate_layout(&axis, &layout) !=
            EMASTER_SESSION_LAYOUT_CONFIGURATION_REQUIRED)
        {
            printf("不合格：运行方案要用的字段不在设备映射里，却没要求改写设备\n");
            ++failures;
        }

        /* 对象号对得上但位宽变了：同样不能直接用。 */
        build_profile(0x2FFD0008U, 0x2FFE0008U);
        rx_entries[1].bit_length = 8U;
        if (emaster_session_axis_validate_layout(&axis, &layout) !=
            EMASTER_SESSION_LAYOUT_CONFIGURATION_REQUIRED)
        {
            printf("不合格：要用的字段位宽对不上，却判成了能用\n");
            ++failures;
        }

        /* 设备声明自己不能改映射：判成改不了，让上层按"设备不支持"处置。 */
        build_profile(0x2FFD0008U, 0x2FFE0008U);
        rx_entries[1].index = 0x607BU;
        device_profile.supports_pdo_configuration = false;
        if (emaster_session_axis_validate_layout(&axis, &layout) !=
            EMASTER_SESSION_LAYOUT_CONFIGURATION_UNSUPPORTED)
        {
            printf("不合格：设备没声明可改映射，却仍要求改写\n");
            ++failures;
        }

        /* 与设备逐条相同：走的是严格匹配那一档，不是"采用"。 */
        build_profile(0x5FFE0008U, 0x5FFE0008U);
        if (emaster_session_axis_validate_layout(&axis, &layout) !=
            EMASTER_SESSION_LAYOUT_MATCH)
        {
            printf("不合格：与设备逐条相同的映射没判成匹配\n");
            ++failures;
        }
    }

    emaster_pdo_layout_destroy(&layout);
    printf("不合格 %d 项\n", failures);
    return failures == 0 ? 0 : 1;
}
