#!/usr/bin/env python3
"""运行方案与运动方案的"填充代码"渲染层（原型）。

与 generate_runtime_config.py 的关系
    共用模型层（runtime_config_model 的 operation_values / motion_values），只换渲染层。
    枚举标识符沿用现有渲染层那套字符串格式（EMASTER_SYNC_STRATEGY_DC 等），
    所以两个渲染层对同一份配置给出同一组符号。

枚举怎么落到运行时
    扁平文件里存的是标识符**文本**；生成的 strcmp 链把它还原成符号。符号由 C 编译器
    解析，所以枚举的数值不需要在 Python 里再抄一遍。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_config_model import motion_values, operation_values  # noqa: E402


def read_str(key: str, target: str) -> list[str]:
    return [f'    if (!emaster_config_reader_string(reader, "{key}", &{target}))', "    {", "        return false;", "    }"]


def read_num(key: str, target: str, maximum: int | None = None, cast: str = "") -> list[str]:
    lines = [f'    if (!emaster_config_reader_u32(reader, "{key}", &number))', "    {", "        return false;", "    }"]
    if maximum is not None:
        lines.append(f"    if (number > {maximum}) {{ return false; }}")
    lines.append(f"    {target} = {'(' + cast + ')number' if cast else 'number'};")
    return lines


def read_signed(key: str, target: str, cast: str = "int32_t") -> list[str]:
    return [
        f'    if (!emaster_config_reader_i32(reader, "{key}", &signed_number))',
        "    {",
        "        return false;",
        "    }",
        f"    {target} = ({cast})signed_number;",
    ]


def read_flag(key: str, target: str) -> list[str]:
    return [f'    if (!emaster_config_reader_bool(reader, "{key}", &flag))', "    {", "        return false;", "    }", f"    {target} = flag;"]


def enum_resolver(name: str, c_type: str, identifiers: set[str]) -> list[str]:
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
    for identifier in sorted(identifiers):
        lines += [f'    if (strcmp(text, "{identifier}") == 0)', "    {", f"        *out = {identifier};", "        return true;", "    }"]
    lines += ["    return false;", "}"]
    return lines


def operation_enum_ids(operations: list[dict[str, Any]]) -> dict[str, set[str]]:
    """收集这批配置里实际用到的枚举标识符，只为它们生成 strcmp 分支。"""
    ids: dict[str, set[str]] = {"approval": set(), "strategy": set(), "display": set(), "sdo": set()}
    for operation in operations:
        ids["approval"].add(f"EMASTER_OPERATION_PROFILE_{operation['status'].upper()}")
        ids["strategy"].add(f"EMASTER_SYNC_STRATEGY_{operation['strategy'].upper()}")
        for mode in operation["modes"]:
            ids["display"].add(f"EMASTER_MODE_DISPLAY_{mode['mode_display_policy'].upper()}")
            for item in list(mode["safeop_to_op_sdo_writes"]) + list(mode["final_sdo_reads"]):
                ids["sdo"].add(f"EMASTER_CONFIG_SDO_VALUE_{item['type'].upper()}")
    return ids


def motion_enum_ids(motions: list[dict[str, Any]]) -> dict[str, set[str]]:
    ids: dict[str, set[str]] = {"approval": set(), "trajectory": set(), "coordinate": set()}
    for motion in motions:
        ids["approval"].add(f"EMASTER_MOTION_PROFILE_{motion['status'].upper()}")
        ids["trajectory"].add(f"EMASTER_MOTION_TRAJECTORY_{motion['trajectory'].upper()}")
        ids["coordinate"].add(f"EMASTER_MOTION_COORDINATE_{motion['coordinate'].upper()}")
    return ids


def runtime_rows(operations: list[dict[str, Any]], motions: list[dict[str, Any]]) -> list[tuple[str, str]]:
    rows: list[tuple[str, str]] = [("operation.count", str(len(operations)))]
    for oi, operation in enumerate(operations):
        base = f"operation.{oi}"
        rows += [
            (f"{base}.id", operation["id"]),
            (f"{base}.approval", f"EMASTER_OPERATION_PROFILE_{operation['status'].upper()}"),
            (f"{base}.device_profile_id", operation["device_profile_id"]),
            (f"{base}.pdo_set_id", operation["pdo_set_id"]),
            (f"{base}.strategy", f"EMASTER_SYNC_STRATEGY_{operation['strategy'].upper()}"),
            (f"{base}.assign_present", "true" if operation["assign_present"] else "false"),
            (f"{base}.assign_activate", f"0x{operation['assign_activate']:08X}"),
            (f"{base}.cycle_present", "true" if operation["cycle_present"] else "false"),
            (f"{base}.cycle_ns", str(operation["cycle_ns"])),
            (f"{base}.phase_present", "true" if operation["phase_present"] else "false"),
            (f"{base}.process_data_phase_ns", str(operation["process_data_phase_ns"])),
            (f"{base}.startup_present", "true" if operation["startup_present"] else "false"),
            (f"{base}.dc_startup_cycles", str(operation["dc_startup_cycles"])),
            (f"{base}.shift_present", "true" if operation["shift_present"] else "false"),
            (f"{base}.shift_ns", str(operation["shift_ns"])),
            (f"{base}.sm2_present", "true" if operation["sm2_present"] else "false"),
            (f"{base}.sm2_type", f"0x{operation['sm2_type']:04X}"),
            (f"{base}.sm3_present", "true" if operation["sm3_present"] else "false"),
            (f"{base}.sm3_type", f"0x{operation['sm3_type']:04X}"),
        ]
        selected = operation["selected_mode_id"]
        rows.append((f"{base}.selected_mode_id_present", "true" if selected is not None else "false"))
        if selected is not None:
            rows.append((f"{base}.selected_mode_id", selected))
        rows.append((f"{base}.modes.count", str(len(operation["modes"]))))
        for mi, mode in enumerate(operation["modes"]):
            mbase = f"{base}.modes.{mi}"
            rows += [
                (f"{mbase}.id", mode["id"]),
                (f"{mbase}.value", str(mode["value"])),
                (f"{mbase}.display", f"EMASTER_MODE_DISPLAY_{mode['mode_display_policy'].upper()}"),
                (f"{mbase}.rx_fields.count", str(len(mode["rx_fields"]))),
                (f"{mbase}.tx_fields.count", str(len(mode["tx_fields"]))),
            ]
            for fi, field in enumerate(mode["rx_fields"]):
                rows.append((f"{mbase}.rx_fields.{fi}", field))
            for fi, field in enumerate(mode["tx_fields"]):
                rows.append((f"{mbase}.tx_fields.{fi}", field))
            rows.append((f"{mbase}.sdo_writes.count", str(len(mode["safeop_to_op_sdo_writes"]))))
            for wi, write in enumerate(mode["safeop_to_op_sdo_writes"]):
                wbase = f"{mbase}.sdo_writes.{wi}"
                rows += [
                    (f"{wbase}.index", f"0x{write['index']:04X}"),
                    (f"{wbase}.subindex", str(write["subindex"])),
                    (f"{wbase}.type", f"EMASTER_CONFIG_SDO_VALUE_{write['type'].upper()}"),
                    (f"{wbase}.value", str(write["value"])),
                ]
            rows.append((f"{mbase}.sdo_reads.count", str(len(mode["final_sdo_reads"]))))
            for ri, read in enumerate(mode["final_sdo_reads"]):
                rbase = f"{mbase}.sdo_reads.{ri}"
                rows += [
                    (f"{rbase}.name", read["name"]),
                    (f"{rbase}.index", f"0x{read['index']:04X}"),
                    (f"{rbase}.subindex", str(read["subindex"])),
                    (f"{rbase}.type", f"EMASTER_CONFIG_SDO_VALUE_{read['type'].upper()}"),
                ]
    rows.append(("motion.count", str(len(motions))))
    for mi, motion in enumerate(motions):
        base = f"motion.{mi}"
        rows += [
            (f"{base}.id", motion["id"]),
            (f"{base}.approval", f"EMASTER_MOTION_PROFILE_{motion['status'].upper()}"),
            (f"{base}.required_mode_id", motion["required_mode_id"]),
            (f"{base}.trajectory", f"EMASTER_MOTION_TRAJECTORY_{motion['trajectory'].upper()}"),
            (f"{base}.coordinate", f"EMASTER_MOTION_COORDINATE_{motion['coordinate'].upper()}"),
            (f"{base}.duration_ms", str(motion["duration_ms"])),
            (f"{base}.settle_ms", str(motion["settle_ms"])),
            (f"{base}.axes.count", str(len(motion["axes"]))),
        ]
        for ai, axis in enumerate(motion["axes"]):
            abase = f"{base}.axes.{ai}"
            rows += [
                (f"{abase}.axis_id", axis["axis_id"]),
                (f"{abase}.relative_angle", str(axis["relative_angle_millidegrees"])),
                (f"{abase}.target_velocity", str(axis["target_velocity_millidegrees_per_second"])),
                (f"{abase}.acceleration", str(axis["acceleration_millidegrees_per_second2"])),
                (f"{abase}.deceleration", str(axis["deceleration_millidegrees_per_second2"])),
                (f"{abase}.following_error", str(axis["max_following_error_millidegrees"])),
                (f"{abase}.max_step", str(axis["max_step_millidegrees"])),
                (f"{abase}.velocity_error", str(axis["max_velocity_error_millidegrees_per_second"])),
                (f"{abase}.encoder_increments", str(axis["expected_position_scale"]["encoder_increments"])),
                (f"{abase}.encoder_motor_revolutions", str(axis["expected_position_scale"]["encoder_motor_revolutions"])),
                (f"{abase}.gear_motor_revolutions", str(axis["expected_position_scale"]["gear_motor_revolutions"])),
                (f"{abase}.gear_shaft_revolutions", str(axis["expected_position_scale"]["gear_shaft_revolutions"])),
            ]
    return rows


def render(operations: list[dict[str, Any]], motions: list[dict[str, Any]]) -> str:
    """运行方案与运动方案的填充代码。

    条目数全部在运行时从 flat 文件读，嵌套数组按读出来的数量 calloc —— 这一族
    嵌套也深（方案 -> 模式 -> 字段名 / SDO 表），按上限预留会白占内存。
    分配一次、永不释放，生命周期与读取器相同。

    几处 const 的处理：结构体里有三类只读指针字段 —— 指向结构体的（modes、
    safeop_to_op_sdo_writes、final_sdo_reads、axes）、以及**指针本身也 const** 的
    （required_rx_fields / required_tx_fields 是 const char *const *）。
    前一类用非 const 局部指针装完再赋给字段（加 const 是合法转换）；
    后一类只能声明成"指针可变、指向内容只读"，赋值处显式转型。
    """
    L: list[str] = [
        "/* 由 tools/generate_runtime_apply.py 生成，不要手改。 */",
        '#include "emaster/config/generated_runtime.h"',
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "",
        "#define EMASTER_KEY_CAPACITY 192U",
        "",
        "static emaster_operation_profile_t *operations = NULL;",
        "static emaster_motion_profile_t *motions = NULL;",
        "static size_t operation_count = 0U;",
        "static size_t motion_count = 0U;",
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
    oids = operation_enum_ids(operations)
    mids = motion_enum_ids(motions)
    L += enum_resolver("resolve_operation_approval", "emaster_operation_profile_approval_t", oids["approval"])
    L += [""]
    L += enum_resolver("resolve_sync_strategy", "emaster_sync_strategy_t", oids["strategy"])
    L += [""]
    L += enum_resolver("resolve_display_policy", "emaster_mode_display_policy_t", oids["display"])
    L += [""]
    L += enum_resolver("resolve_sdo_type", "emaster_config_sdo_value_type_t", oids["sdo"])
    L += [""]
    L += enum_resolver("resolve_motion_approval", "emaster_motion_profile_approval_t", mids["approval"])
    L += [""]
    L += enum_resolver("resolve_trajectory", "emaster_motion_trajectory_t", mids["trajectory"])
    L += [""]
    L += enum_resolver("resolve_coordinate", "emaster_motion_coordinate_t", mids["coordinate"])
    L += [
        "",
        "/* 读一个模式。base 形如 operation.0.modes.1。 */",
        "static bool load_mode(const emaster_config_reader_t *reader, char *base,",
        "                      emaster_operation_mode_t *out)",
        "{",
        "    char leaf[EMASTER_KEY_CAPACITY];",
        "    size_t index;",
        "    size_t count;",
        "    const char **fields;",
        "    emaster_sdo_write_config_t *writes;",
        "    emaster_sdo_read_config_t *reads;",
        "    uint32_t number = 0U;",
        "    int32_t signed_number = 0;",
        "",
        "    memcpy(leaf, base, sizeof(leaf));",
        '    if (!key_push(leaf, sizeof(leaf), "id") ||',
        "        !emaster_config_reader_string(reader, leaf, &out->mode_id)) { return false; }",
        "    memcpy(leaf, base, sizeof(leaf));",
        '    if (!key_push(leaf, sizeof(leaf), "value") ||',
        "        !emaster_config_reader_i32(reader, leaf, &signed_number)) { return false; }",
        "    out->value = (int8_t)signed_number;",
        "    memcpy(leaf, base, sizeof(leaf));",
        '    if (!key_push(leaf, sizeof(leaf), "display") ||',
        "        !resolve_display_policy(reader, leaf, &out->mode_display_policy)) { return false; }",
        "",
        "    /* 字段名数组：const char *const *，运行期没法逐项赋值，",
        "       所以数组声明成指针可变的，赋给字段时显式转型。 */",
        "    memcpy(leaf, base, sizeof(leaf));",
        '    if (!key_push(leaf, sizeof(leaf), "rx_fields") ||',
        '        !key_push(leaf, sizeof(leaf), "count") ||',
        "        !emaster_config_reader_size(reader, leaf, &count)) { return false; }",
        "    fields = (const char **)calloc(count == 0U ? 1U : count, sizeof(*fields));",
        "    if (fields == NULL) { return false; }",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        memcpy(leaf, base, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "rx_fields") ||',
        "            !key_push_index(leaf, sizeof(leaf), index) ||",
        "            !emaster_config_reader_string(reader, leaf, &fields[index])) { return false; }",
        "    }",
        "    out->required_rx_fields = (const char *const *)fields;",
        "    out->required_rx_field_count = count;",
        "    memcpy(leaf, base, sizeof(leaf));",
        '    if (!key_push(leaf, sizeof(leaf), "tx_fields") ||',
        '        !key_push(leaf, sizeof(leaf), "count") ||',
        "        !emaster_config_reader_size(reader, leaf, &count)) { return false; }",
        "    fields = (const char **)calloc(count == 0U ? 1U : count, sizeof(*fields));",
        "    if (fields == NULL) { return false; }",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        memcpy(leaf, base, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "tx_fields") ||',
        "            !key_push_index(leaf, sizeof(leaf), index) ||",
        "            !emaster_config_reader_string(reader, leaf, &fields[index])) { return false; }",
        "    }",
        "    out->required_tx_fields = (const char *const *)fields;",
        "    out->required_tx_field_count = count;",
        "",
        "    /* SDO 写表 */",
        "    memcpy(leaf, base, sizeof(leaf));",
        '    if (!key_push(leaf, sizeof(leaf), "sdo_writes") ||',
        '        !key_push(leaf, sizeof(leaf), "count") ||',
        "        !emaster_config_reader_size(reader, leaf, &count)) { return false; }",
        "    writes = (emaster_sdo_write_config_t *)calloc(count == 0U ? 1U : count, sizeof(*writes));",
        "    if (writes == NULL) { return false; }",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        char item[EMASTER_KEY_CAPACITY];",
        "",
        "        memcpy(item, base, sizeof(item));",
        '        if (!key_push(item, sizeof(item), "sdo_writes") ||',
        "            !key_push_index(item, sizeof(item), index)) { return false; }",
        "        memcpy(leaf, item, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "index") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
        "        if (number > UINT16_MAX) { emaster_config_reader_note_failure(\"超出 uint16 范围\"); return false; }",
        "        writes[index].index = (uint16_t)number;",
        "        memcpy(leaf, item, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "subindex") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
        "        if (number > UINT8_MAX) { emaster_config_reader_note_failure(\"超出 uint8 范围\"); return false; }",
        "        writes[index].subindex = (uint8_t)number;",
        "        memcpy(leaf, item, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "type") ||',
        "            !resolve_sdo_type(reader, leaf, &writes[index].type)) { return false; }",
        "        memcpy(leaf, item, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "value") ||',
        "            !emaster_config_reader_i64(reader, leaf, &writes[index].value)) { return false; }",
        "    }",
        "    out->safeop_to_op_sdo_writes = writes;",
        "    out->safeop_to_op_sdo_write_count = count;",
        "",
        "    /* SDO 读表 */",
        "    memcpy(leaf, base, sizeof(leaf));",
        '    if (!key_push(leaf, sizeof(leaf), "sdo_reads") ||',
        '        !key_push(leaf, sizeof(leaf), "count") ||',
        "        !emaster_config_reader_size(reader, leaf, &count)) { return false; }",
        "    reads = (emaster_sdo_read_config_t *)calloc(count == 0U ? 1U : count, sizeof(*reads));",
        "    if (reads == NULL) { return false; }",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        char item[EMASTER_KEY_CAPACITY];",
        "",
        "        memcpy(item, base, sizeof(item));",
        '        if (!key_push(item, sizeof(item), "sdo_reads") ||',
        "            !key_push_index(item, sizeof(item), index)) { return false; }",
        "        memcpy(leaf, item, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "name") ||',
        "            !emaster_config_reader_string(reader, leaf, &reads[index].name)) { return false; }",
        "        memcpy(leaf, item, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "index") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
        "        if (number > UINT16_MAX) { emaster_config_reader_note_failure(\"超出 uint16 范围\"); return false; }",
        "        reads[index].index = (uint16_t)number;",
        "        memcpy(leaf, item, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "subindex") ||',
        "            !emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
        "        if (number > UINT8_MAX) { emaster_config_reader_note_failure(\"超出 uint8 范围\"); return false; }",
        "        reads[index].subindex = (uint8_t)number;",
        "        memcpy(leaf, item, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "type") ||',
        "            !resolve_sdo_type(reader, leaf, &reads[index].type)) { return false; }",
        "    }",
        "    out->final_sdo_reads = reads;",
        "    out->final_sdo_read_count = count;",
        "    return true;",
        "}",
        "",
        "static bool load_operation(const emaster_config_reader_t *reader, size_t ordinal,",
        "                           emaster_operation_profile_t *out)",
        "{",
        "    char base[EMASTER_KEY_CAPACITY];",
        "    char leaf[EMASTER_KEY_CAPACITY];",
        "    char item[EMASTER_KEY_CAPACITY];",
        "    emaster_operation_mode_t *modes;",
        "    size_t index;",
        "    size_t count;",
        "    bool flag = false;",
        "    uint32_t number = 0U;",
        "    int32_t signed_number = 0;",
        "",
        '    if (snprintf(base, sizeof(base), "operation.%zu", ordinal) < 0) { return false; }',
    ]
    # 逐字段：标量
    scalars = [
        ("id", "str", "out->profile_id"),
        ("approval", "enum_approval", "out->approval"),
        ("device_profile_id", "str", "out->device_profile_id"),
        ("pdo_set_id", "str", "out->pdo_set_id"),
        ("strategy", "enum_strategy", "out->sync_strategy"),
        ("assign_present", "flag", "out->has_assign_activate"),
        ("assign_activate", "hex32", "out->assign_activate"),
        ("cycle_present", "flag", "out->has_cycle_ns"),
        ("cycle_ns", "num32", "out->cycle_ns"),
        ("phase_present", "flag", "out->has_process_data_phase_ns"),
        ("process_data_phase_ns", "num32", "out->process_data_phase_ns"),
        ("startup_present", "flag", "out->has_dc_startup_cycles"),
        ("dc_startup_cycles", "num32", "out->dc_startup_cycles"),
        ("shift_present", "flag", "out->has_sync0_shift_ns"),
        ("shift_ns", "signed", "out->sync0_shift_ns"),
        ("sm2_present", "flag", "out->has_sm2_sync_type"),
        ("sm2_type", "u16", "out->sm2_sync_type"),
        ("sm3_present", "flag", "out->has_sm3_sync_type"),
        ("sm3_type", "u16", "out->sm3_sync_type"),
        ("selected_mode_id_present", "flag", "flag"),
    ]
    for name, kind, target in scalars:
        L += [
            "    memcpy(leaf, base, sizeof(leaf));",
            f'    if (!key_push(leaf, sizeof(leaf), "{name}")) {{ return false; }}',
        ]
        if kind == "str":
            L += [f"    if (!emaster_config_reader_string(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "flag":
            L += [f"    if (!emaster_config_reader_bool(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "num32":
            L += [f"    if (!emaster_config_reader_u32(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "signed":
            L += [
                "    if (!emaster_config_reader_i32(reader, leaf, &signed_number)) { return false; }",
                f"    {target} = signed_number;",
            ]
        elif kind == "hex32":
            L += [f"    if (!emaster_config_reader_u32(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "u16":
            L += [
                "    if (!emaster_config_reader_u32(reader, leaf, &number)) { return false; }",
                "    if (number > UINT16_MAX) { emaster_config_reader_note_failure(\"超出 uint16 范围\"); return false; }",
                f"    {target} = (uint16_t)number;",
            ]
        elif kind == "enum_approval":
            L += [f"    if (!resolve_operation_approval(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "enum_strategy":
            L += [f"    if (!resolve_sync_strategy(reader, leaf, &{target})) {{ return false; }}"]
    L += [
        "    if (flag)",
        "    {",
        "        memcpy(leaf, base, sizeof(leaf));",
        '        if (!key_push(leaf, sizeof(leaf), "selected_mode_id") ||',
        "            !emaster_config_reader_string(reader, leaf, &out->selected_mode_id)) { return false; }",
        "    }",
        "    else",
        "    {",
        "        out->selected_mode_id = NULL;",
        "    }",
        "    memcpy(leaf, base, sizeof(leaf));",
        '    if (!key_push(leaf, sizeof(leaf), "modes") ||',
        '        !key_push(leaf, sizeof(leaf), "count") ||',
        "        !emaster_config_reader_size(reader, leaf, &count)) { return false; }",
        "    modes = (emaster_operation_mode_t *)calloc(count == 0U ? 1U : count, sizeof(*modes));",
        "    if (modes == NULL) { return false; }",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        memcpy(item, base, sizeof(item));",
        '        if (!key_push(item, sizeof(item), "modes") ||',
        "            !key_push_index(item, sizeof(item), index)) { return false; }",
        "        if (!load_mode(reader, item, &modes[index])) { return false; }",
        "    }",
        "    out->modes = modes;",
        "    out->mode_count = count;",
        "    return true;",
        "}",
        "",
        "static bool load_motion(const emaster_config_reader_t *reader, size_t ordinal,",
        "                        emaster_motion_profile_t *out)",
        "{",
        "    char base[EMASTER_KEY_CAPACITY];",
        "    char leaf[EMASTER_KEY_CAPACITY];",
        "    char item[EMASTER_KEY_CAPACITY];",
        "    emaster_motion_axis_config_t *axes;",
        "    size_t index;",
        "    size_t count;",
        "    int32_t signed_number = 0;",
        "",
        '    if (snprintf(base, sizeof(base), "motion.%zu", ordinal) < 0) { return false; }',
    ]
    motion_scalars = [
        ("id", "str", "out->motion_profile_id"),
        ("approval", "enum_approval", "out->approval"),
        ("required_mode_id", "str", "out->required_mode_id"),
        ("trajectory", "enum_trajectory", "out->trajectory"),
        ("coordinate", "enum_coordinate", "out->coordinate"),
        ("duration_ms", "num32", "out->duration_ms"),
        ("settle_ms", "num32", "out->settle_ms"),
    ]
    for name, kind, target in motion_scalars:
        L += [
            "    memcpy(leaf, base, sizeof(leaf));",
            f'    if (!key_push(leaf, sizeof(leaf), "{name}")) {{ return false; }}',
        ]
        if kind == "str":
            L += [f"    if (!emaster_config_reader_string(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "num32":
            L += [f"    if (!emaster_config_reader_u32(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "enum_approval":
            L += [f"    if (!resolve_motion_approval(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "enum_trajectory":
            L += [f"    if (!resolve_trajectory(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "enum_coordinate":
            L += [f"    if (!resolve_coordinate(reader, leaf, &{target})) {{ return false; }}"]
    axis_fields = [
        ("axis_id", "str", "axis_id"),
        ("relative_angle", "signed", "relative_angle_millidegrees"),
        ("target_velocity", "signed", "target_velocity_millidegrees_per_second"),
        ("acceleration", "num32", "acceleration_millidegrees_per_second2"),
        ("deceleration", "num32", "deceleration_millidegrees_per_second2"),
        ("following_error", "num32", "max_following_error_millidegrees"),
        ("max_step", "num32", "max_step_millidegrees"),
        ("velocity_error", "num32", "max_velocity_error_millidegrees_per_second"),
        ("encoder_increments", "num32", "expected_encoder_increments"),
        ("encoder_motor_revolutions", "num32", "expected_encoder_motor_revolutions"),
        ("gear_motor_revolutions", "num32", "expected_gear_motor_revolutions"),
        ("gear_shaft_revolutions", "num32", "expected_gear_shaft_revolutions"),
    ]
    L += [
        "    memcpy(leaf, base, sizeof(leaf));",
        '    if (!key_push(leaf, sizeof(leaf), "axes") ||',
        '        !key_push(leaf, sizeof(leaf), "count") ||',
        "        !emaster_config_reader_size(reader, leaf, &count)) { return false; }",
        "    axes = (emaster_motion_axis_config_t *)calloc(count == 0U ? 1U : count, sizeof(*axes));",
        "    if (axes == NULL) { return false; }",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        memcpy(item, base, sizeof(item));",
        '        if (!key_push(item, sizeof(item), "axes") ||',
        "            !key_push_index(item, sizeof(item), index)) { return false; }",
    ]
    for name, kind, field in axis_fields:
        L += [
            "        memcpy(leaf, item, sizeof(leaf));",
            f'        if (!key_push(leaf, sizeof(leaf), "{name}")) {{ return false; }}',
        ]
        target = f"axes[index].{field}"
        if kind == "str":
            L += [f"        if (!emaster_config_reader_string(reader, leaf, &{target})) {{ return false; }}"]
        elif kind == "signed":
            L += [
                "        if (!emaster_config_reader_i32(reader, leaf, &signed_number)) { return false; }",
                f"        {target} = signed_number;",
            ]
        else:
            L += [f"        if (!emaster_config_reader_u32(reader, leaf, &{target})) {{ return false; }}"]
    L += [
        "    }",
        "    out->axes = axes;",
        "    out->axis_count = count;",
        "    return true;",
        "}",
        "",
        "bool emaster_generated_runtime_apply(const emaster_config_reader_t *reader)",
        "{",
        "    size_t index;",
        "",
        '    if (!emaster_config_reader_size(reader, "operation.count", &operation_count)) { return false; }',
        "    operations = (emaster_operation_profile_t *)calloc(",
        "        operation_count == 0U ? 1U : operation_count, sizeof(*operations));",
        "    if (operations == NULL) { return false; }",
        "    for (index = 0U; index < operation_count; ++index)",
        "    {",
        "        if (!load_operation(reader, index, &operations[index])) { return false; }",
        "    }",
        '    if (!emaster_config_reader_size(reader, "motion.count", &motion_count)) { return false; }',
        "    motions = (emaster_motion_profile_t *)calloc(",
        "        motion_count == 0U ? 1U : motion_count, sizeof(*motions));",
        "    if (motions == NULL) { return false; }",
        "    for (index = 0U; index < motion_count; ++index)",
        "    {",
        "        if (!load_motion(reader, index, &motions[index])) { return false; }",
        "    }",
        "    return true;",
        "}",
        "",
        "size_t emaster_operation_profile_count(void)",
        "{",
        "    return operation_count;",
        "}",
        "",
        "const emaster_operation_profile_t *emaster_operation_profile_at(size_t index)",
        "{",
        "    return index < operation_count ? &operations[index] : NULL;",
        "}",
        "",
        "const emaster_operation_profile_t *emaster_operation_profile_by_id(const char *profile_id)",
        "{",
        "    size_t index;",
        "",
        "    if (profile_id == NULL) { return NULL; }",
        "    for (index = 0U; index < operation_count; ++index)",
        "    {",
        "        if (strcmp(operations[index].profile_id, profile_id) == 0) { return &operations[index]; }",
        "    }",
        "    return NULL;",
        "}",
        "",
        "size_t emaster_motion_profile_count(void)",
        "{",
        "    return motion_count;",
        "}",
        "",
        "const emaster_motion_profile_t *emaster_motion_profile_at(size_t index)",
        "{",
        "    return index < motion_count ? &motions[index] : NULL;",
        "}",
        "",
        "const emaster_motion_profile_t *emaster_motion_profile_by_id(const char *profile_id)",
        "{",
        "    size_t index;",
        "",
        "    if (profile_id == NULL) { return NULL; }",
        "    for (index = 0U; index < motion_count; ++index)",
        "    {",
        "        if (strcmp(motions[index].motion_profile_id, profile_id) == 0) { return &motions[index]; }",
        "    }",
        "    return NULL;",
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
            "/* 由 tools/generate_runtime_apply.py 生成，不要手改。 */",
            "#ifndef EMASTER_CONFIG_GENERATED_RUNTIME_H",
            "#define EMASTER_CONFIG_GENERATED_RUNTIME_H",
            "",
            '#include "emaster/config/config_reader.h"',
            '#include "emaster/config/runtime_config.h"',
            "",
            "#include <stdbool.h>",
            "#include <stddef.h>",
            "",
            "bool emaster_generated_runtime_apply(const emaster_config_reader_t *reader);",
            "",
            "/* 公开访问函数：与编译期生成物同名同签名，上层无需改动。 */",
            "size_t emaster_operation_profile_count(void);",
            "const emaster_operation_profile_t *emaster_operation_profile_at(size_t index);",
            "const emaster_operation_profile_t *emaster_operation_profile_by_id(const char *profile_id);",
            "size_t emaster_motion_profile_count(void);",
            "const emaster_motion_profile_t *emaster_motion_profile_at(size_t index);",
            "const emaster_motion_profile_t *emaster_motion_profile_by_id(const char *profile_id);",
            "",
            "#endif /* EMASTER_CONFIG_GENERATED_RUNTIME_H */",
            "",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="运行方案与运动方案 -> 填充代码 + 扁平文件")
    parser.add_argument("--operations", type=Path, required=True)
    parser.add_argument("--motions", type=Path, required=True)
    parser.add_argument("--c-output", type=Path, required=True)
    parser.add_argument("--h-output", type=Path, required=True)
    parser.add_argument("--flat-output", type=Path, required=True)
    args = parser.parse_args()

    operations = operation_values(
        [json.loads(p.read_text(encoding="utf-8")) for p in sorted(args.operations.glob("*.json"))]
    )
    motions = motion_values(
        [json.loads(p.read_text(encoding="utf-8")) for p in sorted(args.motions.glob("*.json"))]
    )
    head = "/* 由 tools/generate_runtime_apply.py 生成，不要手改。 */\n"
    args.c_output.write_text(
        head + render(operations, motions),
        encoding="utf-8",
        newline="\n",
    )
    rows = runtime_rows(operations, motions)
    args.flat_output.write_text("".join(f"{k}\t{v}\n" for k, v in rows), encoding="utf-8", newline="\n")
    print(f"运行方案 {len(operations)} 份、运动方案 {len(motions)} 份，扁平记录 {len(rows)} 条")
    args.h_output.write_text(render_header(), encoding="utf-8", newline="\n")
    print(f"已写出 {args.c_output}")
    print(f"已写出 {args.h_output}")
    print(f"已写出 {args.flat_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
