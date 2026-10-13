#!/usr/bin/env python3
"""设备目录的"填充代码"渲染层。

与 generate_slave_catalog.py 的关系
    两者共用同一套校验与归一化（直接 import 那个脚本的 profile_values），只换渲染层：
    它产出静态初始化器，本脚本产出 apply(reader)。

字符串
    模型层用 c_string()（就是 json.dumps）把字符串套成 C 字面量。渲染层要原始字符串，
    用 json.loads 还原 —— 那是这一步的精确逆运算。

枚举
    遥测的类型与语义在模型层已经是 C 标识符。扁平文件里存这个标识符文本，生成的
    strcmp 链把它还原成符号 —— 符号由 C 编译器解析，所以不需要在 Python 里再抄一遍
    枚举的数值。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_slave_catalog import (  # noqa: E402
    ASSIGNMENT_STYLE_VALUES,
    TELEMETRY_SEMANTIC_VALUES,
    TELEMETRY_TYPE_VALUES,
    profile_values,
)


def raw(c_literal: str) -> str:
    """把模型层套好的 C 字符串字面量还原成原始字符串。"""
    return json.loads(c_literal)


def catalog_rows(values: list[dict[str, Any]]) -> list[tuple[str, str]]:
    rows: list[tuple[str, str]] = [("profile.count", str(len(values)))]
    for pi, profile in enumerate(values):
        base = f"profile.{pi}"
        rows += [
            (f"{base}.profile_id", raw(profile["profile_id"])),
            (f"{base}.model", raw(profile["model"])),
            (f"{base}.identity.vendor_id", f"0x{profile['vendor_id']:08X}"),
            (f"{base}.identity.product_code", f"0x{profile['product_code']:08X}"),
            (f"{base}.identity.revision", f"0x{profile['revision']:08X}"),
            (f"{base}.reference_pdo_set_id", raw(profile["reference_pdo_set_id"])),
            (f"{base}.encoder_counts", str(profile["encoder_counts"])),
            (f"{base}.supports_pdo_assignment", profile["supports_pdo_assignment"]),
            (f"{base}.supports_pdo_configuration", profile["supports_pdo_configuration"]),
            (f"{base}.supports_distributed_clocks", profile["supports_dc"]),
            (f"{base}.safe_stop_status_mask", str(profile["safe_stop_status_mask"])),
            (f"{base}.safe_stop_status_value", str(profile["safe_stop_status_value"])),
            (f"{base}.pdo_sets.count", str(len(profile["pdo_sets"]))),
        ]
        for si, pdo in enumerate(profile["pdo_sets"]):
            pbase = f"{base}.pdo_sets.{si}"
            rows += [
                (f"{pbase}.pdo_set_id", pdo["id"]),
                (f"{pbase}.module_ident", f"0x{pdo['module_ident']:08X}"),
                (f"{pbase}.module_slot", str(pdo["module_slot"])),
                (f"{pbase}.assignment_style", pdo["assignment_style"]),
                (f"{pbase}.mode_init_value", str(pdo["mode_init_value"])),
            ]
            for direction in ("rx", "tx"):
                side = pdo[direction]
                rows.append((f"{pbase}.{direction}_pdo_bytes", str(side["bytes"])))
                rows.append((f"{pbase}.{direction}_mappings.count", str(len(side["mappings"]))))
                for mi, mapping in enumerate(side["mappings"]):
                    mbase = f"{pbase}.{direction}_mappings.{mi}"
                    rows += [
                        (f"{mbase}.index", f"0x{mapping['index']:04X}"),
                        (f"{mbase}.bit_length", str(mapping["bit_length"])),
                        (f"{mbase}.entries.count", str(len(mapping["entries"]))),
                    ]
                    for ei, entry in enumerate(mapping["entries"]):
                        ebase = f"{mbase}.entries.{ei}"
                        rows += [
                            (f"{ebase}.index", f"0x{entry['index']:04X}"),
                            (f"{ebase}.subindex", str(entry["subindex"])),
                            (f"{ebase}.bit_length", str(entry["bits"])),
                            (f"{ebase}.data_type", raw(entry["data_type"])),
                            (f"{ebase}.name", raw(entry["name"])),
                        ]
        telemetry = profile["slow_telemetry"]
        rows.append((f"{base}.slow_telemetry.count", str(len(telemetry))))
        for ti, entry in enumerate(telemetry):
            tbase = f"{base}.slow_telemetry.{ti}"
            rows += [
                (f"{tbase}.name", raw(entry["name"])),
                (f"{tbase}.index", f"0x{entry['index']:04X}"),
                (f"{tbase}.subindex", str(entry["subindex"])),
                (f"{tbase}.type", entry["type"]),
                (f"{tbase}.semantic", entry["semantic"]),
                (f"{tbase}.unit", raw(entry["unit"])),
            ]
    return rows


# --- 生成辅助 ---------------------------------------------------------------
#
# 键与目标都走参数：直接写 f-string 时外层引号和 C 代码里的引号会撞车。


def read_str(key: str, target: str) -> list[str]:
    return [f'    if (!emaster_config_reader_string(reader, {key}, &{target}))', "    {", "        return false;", "    }"]


def read_num(key: str, target: str, maximum: int | None = None, cast: str = "") -> list[str]:
    lines = [f"    if (!emaster_config_reader_u32(reader, {key}, &number))", "    {", "        return false;", "    }"]
    if maximum is not None:
        lines.append(f"    if (number > {maximum}) {{ return false; }}")
    lines.append(f"    {target} = {'(' + cast + ')number' if cast else 'number'};")
    return lines


def read_flag(key: str, target: str) -> list[str]:
    return [f"    if (!emaster_config_reader_bool(reader, {key}, &flag))", "    {", "        return false;", "    }", f"    {target} = flag;"]


def enum_resolver(name: str, c_type: str, mapping: dict[str, str]) -> list[str]:
    lines = [
        f"static bool {name}(const emaster_config_reader_t *reader, const char *key, {c_type} *out)",
        "{",
        "    const char *text = NULL;",
        "",
        "    if (!emaster_config_reader_string(reader, key, &text))",
        "    {",
        "        return false;",
        "    }",
    ]
    for identifier in sorted(set(mapping.values())):
        lines += [f'    if (strcmp(text, "{identifier}") == 0)', "    {", f"        *out = {identifier};", "        return true;", "    }"]
    lines += ["    return false;", "}"]
    return lines


def render(values: list[dict[str, Any]]) -> str:
    """设备目录族的填充代码。

    条目数全部在运行时从 flat 文件读，数组按读出来的数量 calloc —— 不用静态容量，
    因为这一族是四层嵌套（设备 -> PDO 方案 -> 映射 -> 条目），按上限预留会白占
    好几 MB。分配一次、永不释放，生命周期与读取器相同。

    超限不再存在：唯一的限制是内存，而不是某个生成期写死的数。
    """
    L: list[str] = [
        "/* 由 gen/generate_catalog_apply.py 生成，不要手改。 */",
        '#include "emaster/config/generated_catalog.h"',
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "",
        "#define EMASTER_KEY_CAPACITY 192U",
        "",
        "static emaster_slave_profile_t *profiles = NULL;",
        "static size_t profile_count = 0U;",
        "",
        "/* 键名逐级拼接；每一级都判截断。截断当失败，否则会拿截短的前缀去查，",
        '   报出来的是"缺键"，与真正的原因（键太长）无关。 */',
        "static bool key_push(char *buffer, size_t capacity, const char *suffix)",
        "{",
        "    size_t used = strlen(buffer);",
        "    int written;",
        "",
        "    if (used + 1U >= capacity)",
        "    {",
        "        return false;",
        "    }",
        '    written = snprintf(buffer + used, capacity - used, ".%s", suffix);',
        "    return written > 0 && (size_t)written < capacity - used;",
        "}",
        "",
        "static bool key_push_index(char *buffer, size_t capacity, size_t index)",
        "{",
        "    size_t used = strlen(buffer);",
        "    int written;",
        "",
        "    if (used + 1U >= capacity)",
        "    {",
        "        return false;",
        "    }",
        '    written = snprintf(buffer + used, capacity - used, ".%zu", index);',
        "    return written > 0 && (size_t)written < capacity - used;",
        "}",
        "",
    ]
    L += enum_resolver("resolve_telemetry_type", "emaster_telemetry_type_t", TELEMETRY_TYPE_VALUES)
    L += [""]
    L += enum_resolver("resolve_telemetry_semantic", "emaster_telemetry_semantic_t", TELEMETRY_SEMANTIC_VALUES)
    L += [""]
    L += enum_resolver(
        "resolve_assignment_style", "emaster_pdo_assignment_style_t", ASSIGNMENT_STYLE_VALUES
    )
    L += [
        "",
        "/* 读一个方向。base 就是映射数组本身的键：profile.0.pdo_sets.1.rx_mappings",
        "   —— 注意 rx_mappings 是一个记号，不是 rx + mappings 两级；扁平文件就是这么写的。 */",
        "static bool load_direction(const emaster_config_reader_t *reader, char *base,",
        "                           emaster_pdo_mapping_profile_t **out_mappings,",
        "                           size_t *out_mapping_count)",
        "{",
        "    char key[EMASTER_KEY_CAPACITY];",
        "    char leaf[EMASTER_KEY_CAPACITY];",
        "    emaster_pdo_mapping_profile_t *mappings;",
        "    size_t mapping_index;",
        "    size_t mapping_count;",
        "    size_t entry_index;",
        "    size_t entry_count;",
        "    uint32_t number = 0U;",
        "",
        "    memcpy(key, base, sizeof(key));",
        '    if (!key_push(key, sizeof(key), "count") ||',
        "        !emaster_config_reader_size(reader, key, &mapping_count))",
        "    {",
        "        return false;",
        "    }",
        "    mappings = (emaster_pdo_mapping_profile_t *)calloc(",
        "        mapping_count == 0U ? 1U : mapping_count, sizeof(*mappings));",
        "    if (mappings == NULL)",
        "    {",
        "        return false;",
        "    }",
        "    for (mapping_index = 0U; mapping_index < mapping_count; ++mapping_index)",
        "    {",
        "        emaster_pdo_entry_t *entries;",
        "",
        "        /* 本映射的前缀：base.<i> */",
        "        memcpy(key, base, sizeof(key));",
        "        if (!key_push_index(key, sizeof(key), mapping_index))",
        "        {",
        "            return false;",
        "        }",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "index") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number))",
        "        {",
        "            return false;",
        "        }",
        "        if (number > UINT16_MAX) { emaster_config_reader_note_failure(\"超出 uint16 范围\"); return false; }",
        "        mappings[mapping_index].index = (uint16_t)number;",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "bit_length") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number))",
        "        {",
        "            return false;",
        "        }",
        "        mappings[mapping_index].bit_length = number;",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "entries") ||',
        '            !key_push(leaf, sizeof(leaf), "count") ||',
        "            !emaster_config_reader_size(reader, leaf, &entry_count))",
        "        {",
        "            return false;",
        "        }",
        "        entries = (emaster_pdo_entry_t *)calloc(",
        "            entry_count == 0U ? 1U : entry_count, sizeof(*entries));",
        "        if (entries == NULL)",
        "        {",
        "            return false;",
        "        }",
        "        for (entry_index = 0U; entry_index < entry_count; ++entry_index)",
        "        {",
        "            /* 本条目的前缀：base.mappings.<i>.entries.<j> */",
        "            char item[EMASTER_KEY_CAPACITY];",
        "",
        "            memcpy(item, key, sizeof(item));",
        '            if (!key_push(item, sizeof(item), "entries") ||',
        "                !key_push_index(item, sizeof(item), entry_index))",
        "            {",
        "                return false;",
        "            }",
        "            memcpy(leaf, item, sizeof(leaf));",
        '            if (!key_push(leaf, sizeof(leaf), "index") ||',
        "                !emaster_config_reader_u32(reader, leaf, &number))",
        "            {",
        "                return false;",
        "            }",
        "            if (number > UINT16_MAX) { emaster_config_reader_note_failure(\"超出 uint16 范围\"); return false; }",
        "            entries[entry_index].index = (uint16_t)number;",
        "            memcpy(leaf, item, sizeof(leaf));",
        '            if (!key_push(leaf, sizeof(leaf), "subindex") ||',
        "                !emaster_config_reader_u32(reader, leaf, &number))",
        "            {",
        "                return false;",
        "            }",
        "            if (number > UINT8_MAX) { emaster_config_reader_note_failure(\"超出 uint8 范围\"); return false; }",
        "            entries[entry_index].subindex = (uint8_t)number;",
        "            memcpy(leaf, item, sizeof(leaf));",
        '            if (!key_push(leaf, sizeof(leaf), "bit_length") ||',
        "                !emaster_config_reader_u32(reader, leaf, &number))",
        "            {",
        "                return false;",
        "            }",
        "            if (number > UINT8_MAX) { emaster_config_reader_note_failure(\"超出 uint8 范围\"); return false; }",
        "            entries[entry_index].bit_length = (uint8_t)number;",
        "            memcpy(leaf, item, sizeof(leaf));",
        '            if (!key_push(leaf, sizeof(leaf), "data_type") ||',
        "                !emaster_config_reader_string(reader, leaf, &entries[entry_index].data_type))",
        "            {",
        "                return false;",
        "            }",
        "            memcpy(leaf, item, sizeof(leaf));",
        '            if (!key_push(leaf, sizeof(leaf), "name") ||',
        "                !emaster_config_reader_string(reader, leaf, &entries[entry_index].name))",
        "            {",
        "                return false;",
        "            }",
        "        }",
        "        mappings[mapping_index].entries = entries;",
        "        mappings[mapping_index].entry_count = entry_count;",
        "    }",
        "    *out_mappings = mappings;",
        "    *out_mapping_count = mapping_count;",
        "    return true;",
        "}",
        "",
    ]
    # ---- 逐字段读 profile ----
    def field(base: str, name: str, indent: str = "    ") -> list[str]:
        return [
            f"{indent}memcpy(leaf, {base}, sizeof(leaf));",
            f'{indent}if (!key_push(leaf, sizeof(leaf), "{name}"))',
            f"{indent}{{",
            f"{indent}    return false;",
            f"{indent}}}",
        ]

    L += [
        "static bool load_profile(const emaster_config_reader_t *reader, size_t ordinal,",
        "                         emaster_slave_profile_t *out)",
        "{",
        "    char base[EMASTER_KEY_CAPACITY];",
        "    char key[EMASTER_KEY_CAPACITY];",
        "    char leaf[EMASTER_KEY_CAPACITY];",
        "    emaster_pdo_set_profile_t *sets;",
        "    emaster_slow_telemetry_t *telemetry;",
        "    size_t index;",
        "    size_t count;",
        "    uint32_t number = 0U;",
        "    int32_t signed_number = 0;",
        "    bool flag = false;",
        "",
        '    if (snprintf(base, sizeof(base), "profile.%zu", ordinal) < 0)',
        "    {",
        "        return false;",
        "    }",
    ]
    L += field("base", "profile_id") + [
        "    if (!emaster_config_reader_string(reader, leaf, &out->profile_id)) { return false; }"
    ]
    L += field("base", "model") + [
        "    if (!emaster_config_reader_string(reader, leaf, &out->model)) { return false; }"
    ]
    for name, target in (("vendor_id", "out->identity.vendor_id"),
                         ("product_code", "out->identity.product_code"),
                         ("revision", "out->identity.revision")):
        L += [
            "    memcpy(key, base, sizeof(key));",
            '    if (!key_push(key, sizeof(key), "identity")) { return false; }',
        ]
        L += field("key", name) + [
            f"    if (!emaster_config_reader_u32(reader, leaf, &{target})) {{ return false; }}"
        ]
    L += field("base", "reference_pdo_set_id") + [
        "    if (!emaster_config_reader_string(reader, leaf, &out->reference_pdo_set_id)) { return false; }"
    ]
    L += field("base", "encoder_counts") + [
        "    if (!emaster_config_reader_u32(reader, leaf, &out->encoder_counts_per_motor_revolution_default)) { return false; }"
    ]
    for name, target in (("supports_pdo_assignment", "out->supports_pdo_assignment"),
                         ("supports_pdo_configuration", "out->supports_pdo_configuration"),
                         ("supports_distributed_clocks", "out->supports_distributed_clocks")):
        L += field("base", name) + [
            f"    if (!emaster_config_reader_bool(reader, leaf, &flag)) {{ return false; }}",
            f"    {target} = flag;",
        ]
    for name, target in (("safe_stop_status_mask", "out->safe_stop_status_mask"),
                         ("safe_stop_status_value", "out->safe_stop_status_value")):
        L += field("base", name) + [
            "    if (!emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
            "    if (number > UINT16_MAX) { emaster_config_reader_note_failure(\"超出 uint16 范围\"); return false; }",
            f"    {target} = (uint16_t)number;",
        ]
    # PDO 方案
    L += [
        "    memcpy(key, base, sizeof(key));",
        '    if (!key_push(key, sizeof(key), "pdo_sets") ||',
        '        !key_push(key, sizeof(key), "count") ||',
        "        !emaster_config_reader_size(reader, key, &count))",
        "    {",
        "        return false;",
        "    }",
        "    /* 结构体字段是 const 指针（指向内容只读），但加载期必须写进去，",
        "       所以先拿一个非 const 的局部指针装，最后再赋给字段 —— 那是加 const",
        "       的合法转换，不需要强转。 */",
        "    sets = (emaster_pdo_set_profile_t *)calloc(",
        "        count == 0U ? 1U : count, sizeof(*sets));",
        "    if (sets == NULL) { return false; }",
        "    out->pdo_sets = sets;",
        "    out->pdo_set_count = count;",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        emaster_pdo_set_profile_t *set = &sets[index];",
        "        char direction[EMASTER_KEY_CAPACITY];",
        "        emaster_pdo_mapping_profile_t *maps;",
        "        size_t map_count;",
        "",
        "        memcpy(key, base, sizeof(key));",
        '        if (!key_push(key, sizeof(key), "pdo_sets") ||',
        "            !key_push_index(key, sizeof(key), index))",
        "        {",
        "            return false;",
        "        }",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "pdo_set_id") ||',
        "            !emaster_config_reader_string(reader, leaf, &set->pdo_set_id)) { return false; }",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "module_ident") ||',
        "            !emaster_config_reader_u32(reader, leaf, &set->module_ident)) { return false; }",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "module_slot") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
        "        if (number > UINT8_MAX) { emaster_config_reader_note_failure(\"超出 uint8 范围\"); return false; }",
        "        set->module_slot = (uint8_t)number;",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "assignment_style") ||',
        "            !resolve_assignment_style(reader, leaf, &set->assignment_style)) { return false; }",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "mode_init_value") ||',
        "            !emaster_config_reader_i32(reader, leaf, &signed_number)) { return false; }",
        "        set->mode_init_value = (int8_t)signed_number;",
        "        set->mode_init_on_safeop_to_op = true;",
        "        set->mode_init_index = UINT16_C(0x6060);",
        "        set->mode_init_subindex = UINT8_C(0);",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "rx_pdo_bytes") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
        "        if (number > UINT16_MAX) { emaster_config_reader_note_failure(\"超出 uint16 范围\"); return false; }",
        "        set->rx_pdo_bytes = (uint16_t)number;",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "tx_pdo_bytes") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
        "        if (number > UINT16_MAX) { emaster_config_reader_note_failure(\"超出 uint16 范围\"); return false; }",
        "        set->tx_pdo_bytes = (uint16_t)number;",
        "        memcpy(direction, key, sizeof(direction));",
        '        if (!key_push(direction, sizeof(direction), "rx_mappings") ||',
        "            !load_direction(reader, direction, &maps, &map_count)) { return false; }",
        "        set->rx_mappings = maps;",
        "        set->rx_mapping_count = map_count;",
        "        memcpy(direction, key, sizeof(direction));",
        '        if (!key_push(direction, sizeof(direction), "tx_mappings") ||',
        "            !load_direction(reader, direction, &maps, &map_count)) { return false; }",
        "        set->tx_mappings = maps;",
        "        set->tx_mapping_count = map_count;",
        "    }",
    ]
    # 慢速遥测
    L += [
        "    memcpy(key, base, sizeof(key));",
        '    if (!key_push(key, sizeof(key), "slow_telemetry") ||',
        '        !key_push(key, sizeof(key), "count") ||',
        "        !emaster_config_reader_size(reader, key, &count))",
        "    {",
        "        return false;",
        "    }",
        "    telemetry = (emaster_slow_telemetry_t *)calloc(",
        "        count == 0U ? 1U : count, sizeof(*telemetry));",
        "    if (telemetry == NULL) { return false; }",
        "    out->slow_telemetry = telemetry;",
        "    out->slow_telemetry_count = count;",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        emaster_slow_telemetry_t *item = &telemetry[index];",
        "",
        "        memcpy(key, base, sizeof(key));",
        '        if (!key_push(key, sizeof(key), "slow_telemetry") ||',
        "            !key_push_index(key, sizeof(key), index)) { return false; }",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "name") ||',
        "            !emaster_config_reader_string(reader, leaf, &item->name)) { return false; }",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "index") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
        "        if (number > UINT16_MAX) { emaster_config_reader_note_failure(\"超出 uint16 范围\"); return false; }",
        "        item->index = (uint16_t)number;",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "subindex") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
        "        if (number > UINT8_MAX) { emaster_config_reader_note_failure(\"超出 uint8 范围\"); return false; }",
        "        item->subindex = (uint8_t)number;",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "type") ||',
        "            !resolve_telemetry_type(reader, leaf, &item->type)) { return false; }",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "semantic") ||',
        "            !resolve_telemetry_semantic(reader, leaf, &item->semantic)) { return false; }",
        "        memcpy(leaf, key, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "unit") ||',
        "            !emaster_config_reader_string(reader, leaf, &item->unit)) { return false; }",
        "    }",
        "    return true;",
        "}",
        "",
        "bool emaster_generated_catalog_apply(const emaster_config_reader_t *reader)",
        "{",
        "    size_t index;",
        "",
        '    if (!emaster_config_reader_size(reader, "profile.count", &profile_count))',
        "    {",
        "        return false;",
        "    }",
        "    profiles = (emaster_slave_profile_t *)calloc(",
        "        profile_count == 0U ? 1U : profile_count, sizeof(*profiles));",
        "    if (profiles == NULL)",
        "    {",
        "        return false;",
        "    }",
        "    for (index = 0U; index < profile_count; ++index)",
        "    {",
        "        if (!load_profile(reader, index, &profiles[index]))",
        "        {",
        "            return false;",
        "        }",
        "    }",
        "    return true;",
        "}",
        "",
        "size_t emaster_slave_profile_count(void)",
        "{",
        "    return profile_count;",
        "}",
        "",
        "const emaster_slave_profile_t *emaster_slave_profile_at(size_t index)",
        "{",
        "    return index < profile_count ? &profiles[index] : NULL;",
        "}",
        "",
    ]
    return "\n".join(L)


def render_header() -> str:
    """产出配套头文件。

    没有它，生成的入口就是"没有前置声明"的函数，在项目的严格告警旗标下会撞
    -Wmissing-prototypes；而且部署族要按 ID 查这几族的指针，必须能引用它们。
    """
    return "\n".join(
        [
            "/* 由 gen/generate_catalog_apply.py 生成，不要手改。 */",
            "#ifndef EMASTER_CONFIG_GENERATED_CATALOG_H",
            "#define EMASTER_CONFIG_GENERATED_CATALOG_H",
            "",
            '#include "emaster/catalog/slave_profile.h"',
            '#include "emaster/config/config_reader.h"',
            "",
            "#include <stdbool.h>",
            "#include <stddef.h>",
            "",
            "bool emaster_generated_catalog_apply(const emaster_config_reader_t *reader);",
            "",
            "/* 公开访问函数：与编译期生成物同名同签名，上层无需改动。 */",
            "size_t emaster_slave_profile_count(void);",
            "const emaster_slave_profile_t *emaster_slave_profile_at(size_t index);",
            "",
            "#endif /* EMASTER_CONFIG_GENERATED_CATALOG_H */",
            "",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="设备目录 -> 填充代码 + 扁平文件")
    parser.add_argument("--devices", type=Path, required=True)
    parser.add_argument("--c-output", type=Path, help="省略时只产出扁平文件")
    parser.add_argument("--h-output", type=Path)
    parser.add_argument("--flat-output", type=Path, required=True)
    args = parser.parse_args()

    documents = [json.loads(p.read_text(encoding="utf-8")) for p in sorted(args.devices.glob("*.json"))]
    values = [profile_values(document) for document in documents]
    # 一律 LF：两个产物都是给 Linux 上的 C 程序读的。
    for path, text in ((args.c_output, render(values)), (args.h_output, render_header())):
        if path is not None:
            path.write_text(text, encoding="utf-8", newline="\n")
            print(f"已写出 {path}")
    rows = catalog_rows(values)
    args.flat_output.write_text("".join(f"{k}\t{v}\n" for k, v in rows), encoding="utf-8", newline="\n")
    print(f"设备 {len(values)} 份，扁平记录 {len(rows)} 条")
    print(f"已写出 {args.flat_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
