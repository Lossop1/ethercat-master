#!/usr/bin/env python3
"""根据设备事实配置生成编译期只读的设备目录。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def parse_unsigned(value: object, field: str, maximum: int) -> int:
    """解析十六进制无符号字段，并在生成前拒绝超范围输入。"""
    if not isinstance(value, str) or not value.startswith("0x"):
        raise ValueError(f"{field} 必须是十六进制字符串")
    number = int(value, 16)
    if not 0 <= number <= maximum:
        raise ValueError(f"{field} 超出范围")
    return number


def required_string(document: dict[str, Any], field: str, kind: str) -> str:
    """读取非空字符串；配置错误必须在构建期失败。"""
    value = document.get(field)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{kind}缺少非空字段 {field}")
    return value


def c_string(value: object) -> str:
    """使用 JSON 转义规则生成 C 字符串字面量。"""
    return json.dumps(str(value), ensure_ascii=True)


def direction_values(direction: dict[str, Any], field: str) -> dict[str, object]:
    """提取一个 PDO 方向中的全部映射表，保留配置声明顺序。"""
    mappings = []
    for mapping_ordinal, mapping in enumerate(direction["mappings"], start=1):
        entries = []
        bit_length = 0
        for entry_ordinal, entry in enumerate(mapping["entries"], start=1):
            bits = int(entry["bits"])
            bit_length += bits
            entries.append(
                {
                    "index": parse_unsigned(
                        entry["index"],
                        f"{field}.mappings[{mapping_ordinal}].entries[{entry_ordinal}].index",
                        0xFFFF,
                    ),
                    "subindex": int(entry["subindex"]),
                    "bits": bits,
                    "data_type": c_string(entry["data_type"]),
                    "name": c_string(entry["name"]),
                }
            )
        mappings.append(
            {
                "index": parse_unsigned(
                    mapping["index"], f"{field}.mappings[{mapping_ordinal}].index", 0xFFFF
                ),
                "entries": entries,
                "bit_length": bit_length,
            }
        )
    return {"bytes": int(direction["bytes"]), "mappings": mappings}


def pdo_set_values(pdo_set: dict[str, Any], profile_id: str) -> dict[str, object]:
    """生成一个设备 PDO 方案的结构化值，保留所有可选方案。"""
    pdo_id = required_string(pdo_set, "id", f"设备 {profile_id} 的 PDO 方案")
    return {
        "id": pdo_id,
        "module_ident": parse_unsigned(
            pdo_set["module_ident"], f"设备 {profile_id} PDO 方案 {pdo_id} 的 module_ident", 0xFFFFFFFF
        ),
        "rx": direction_values(pdo_set["rx"], f"设备 {profile_id} PDO 方案 {pdo_id} rx"),
        "tx": direction_values(pdo_set["tx"], f"设备 {profile_id} PDO 方案 {pdo_id} tx"),
    }


def profile_values(document: dict[str, Any]) -> dict[str, object]:
    """提取设备事实，并保留全部 PDO 方案供运行方案选择。"""
    profile_id = required_string(document, "profile_id", "设备配置")
    identity = document["identity"]
    pdo_sets = document["pdo_sets"]
    protocol = document["protocol"]
    conversion = document["conversion"]
    if not all(isinstance(item, dict) for item in (identity, protocol, conversion)):
        raise ValueError(f"设备 {profile_id} 的 identity、protocol、pdo_sets 或 conversion 类型无效")
    if not isinstance(pdo_sets, list) or not pdo_sets:
        raise ValueError(f"设备 {profile_id} 的 pdo_sets 必须是非空数组")

    reference_id = required_string(document, "reference_pdo_set_id", f"设备 {profile_id}")
    pdo_values = [pdo_set_values(item, profile_id) for item in pdo_sets]
    if reference_id not in {item["id"] for item in pdo_values}:
        raise ValueError(f"设备 {profile_id} 的 reference_pdo_set_id 不存在")

    return {
        "profile_id": c_string(profile_id),
        "model": c_string(document["model"]),
        "vendor_id": parse_unsigned(identity["vendor_id"], "identity.vendor_id", 0xFFFFFFFF),
        "product_code": parse_unsigned(
            identity["product_code"], "identity.product_code", 0xFFFFFFFF
        ),
        "revision": parse_unsigned(identity["revision"], "identity.revision", 0xFFFFFFFF),
        "reference_pdo_set_id": c_string(reference_id),
        "pdo_sets": pdo_values,
        "encoder_counts": int(conversion["encoder_counts_per_motor_revolution_default"]),
        "supports_pdo_configuration": "true"
        if protocol["supports_pdo_configuration"]
        else "false",
        "supports_dc": "true"
        if protocol["supports_distributed_clocks"]
        else "false",
    }


def mapping_declarations(
    pdo_set: dict[str, object], profile_ordinal: int, pdo_set_ordinal: int
) -> list[str]:
    """生成一个 PDO 方案的条目数组和映射表数组。"""
    declarations = []
    for direction_name in ("rx", "tx"):
        direction = pdo_set[direction_name]
        mapping_initializers = []
        for mapping_ordinal, mapping in enumerate(direction["mappings"]):
            entries_name = (
                f"profile_{profile_ordinal}_pdo_{pdo_set_ordinal}_{direction_name}_mapping_"
                f"{mapping_ordinal}_entries"
            )
            rendered_entries = ",\n".join(
                "    {"
                f"UINT16_C(0x{entry['index']:04X}), UINT8_C({entry['subindex']}), "
                f"UINT8_C({entry['bits']}), {entry['data_type']}, {entry['name']}"
                "}"
                for entry in mapping["entries"]
            )
            declarations.append(
                f"static const emaster_pdo_entry_t {entries_name}[] = {{\n"
                f"{rendered_entries}\n}};"
            )
            mapping_initializers.append(
                "    {"
                f"UINT16_C(0x{mapping['index']:04X}), {entries_name}, "
                f"sizeof({entries_name}) / sizeof({entries_name}[0]), "
                f"UINT32_C({mapping['bit_length']})"
                "}"
            )
        mappings_name = (
            f"profile_{profile_ordinal}_pdo_{pdo_set_ordinal}_{direction_name}_mappings"
        )
        declarations.append(
            f"static const emaster_pdo_mapping_profile_t {mappings_name}[] = {{\n"
            + ",\n".join(mapping_initializers)
            + "\n};"
        )
    return declarations


def pdo_set_initializer(pdo_set: dict[str, object], profile_ordinal: int, pdo_set_ordinal: int) -> str:
    """生成一个 PDO 方案结构体初始化器。"""
    rx = pdo_set["rx"]
    tx = pdo_set["tx"]
    return (
        "    {"
        f"{c_string(pdo_set['id'])}, UINT32_C(0x{pdo_set['module_ident']:08X}), "
        f"UINT16_C({rx['bytes']}), UINT16_C({tx['bytes']}), "
        f"profile_{profile_ordinal}_pdo_{pdo_set_ordinal}_rx_mappings, "
        f"sizeof(profile_{profile_ordinal}_pdo_{pdo_set_ordinal}_rx_mappings) / "
        f"sizeof(profile_{profile_ordinal}_pdo_{pdo_set_ordinal}_rx_mappings[0]), "
        f"profile_{profile_ordinal}_pdo_{pdo_set_ordinal}_tx_mappings, "
        f"sizeof(profile_{profile_ordinal}_pdo_{pdo_set_ordinal}_tx_mappings) / "
        f"sizeof(profile_{profile_ordinal}_pdo_{pdo_set_ordinal}_tx_mappings[0])"
        "}"
    )


def profile_initializer(values: dict[str, object], ordinal: int) -> str:
    """生成一个设备事实目录结构体初始化器。"""
    return f"""    {{
        .profile_id = {values['profile_id']},
        .model = {values['model']},
        .identity = {{
            .vendor_id = UINT32_C(0x{values['vendor_id']:08X}),
            .product_code = UINT32_C(0x{values['product_code']:08X}),
            .revision = UINT32_C(0x{values['revision']:08X}),
        }},
        .reference_pdo_set_id = {values['reference_pdo_set_id']},
        .pdo_sets = profile_{ordinal}_pdo_sets,
        .pdo_set_count = sizeof(profile_{ordinal}_pdo_sets) / sizeof(profile_{ordinal}_pdo_sets[0]),
        .encoder_counts_per_motor_revolution_default = UINT32_C({values['encoder_counts']}),
        .supports_pdo_configuration = {values['supports_pdo_configuration']},
        .supports_distributed_clocks = {values['supports_dc']},
    }}"""


def generate(documents: list[dict[str, Any]]) -> str:
    """按稳定设备 ID 生成目录，保证相同输入产生逐字节一致的输出。"""
    if not documents:
        raise ValueError("至少需要一个设备配置")

    ordered = sorted(documents, key=lambda item: str(item.get("profile_id", "")))
    profile_ids = [str(item.get("profile_id", "")) for item in ordered]
    if len(profile_ids) != len(set(profile_ids)):
        raise ValueError("设备配置的 profile_id 必须唯一")

    values = [profile_values(item) for item in ordered]
    declarations: list[str] = []
    for profile_ordinal, value in enumerate(values):
        for pdo_set_ordinal, pdo_set in enumerate(value["pdo_sets"]):
            declarations.extend(mapping_declarations(pdo_set, profile_ordinal, pdo_set_ordinal))
        declarations.append(
            f"static const emaster_pdo_set_profile_t profile_{profile_ordinal}_pdo_sets[] = {{\n"
            + ",\n".join(
                pdo_set_initializer(pdo_set, profile_ordinal, pdo_set_ordinal)
                for pdo_set_ordinal, pdo_set in enumerate(value["pdo_sets"])
            )
            + "\n};"
        )

    initializers = ",\n".join(
        profile_initializer(value, ordinal) for ordinal, value in enumerate(values)
    )
    return f"""/* 由 tools/generate_slave_catalog.py 生成，禁止手工修改。 */
#include "emaster/catalog/slave_profile.h"

#include <stddef.h>

{"\n\n".join(declarations)}

static const emaster_slave_profile_t profiles[] = {{
{initializers},
}};

size_t emaster_slave_profile_count(void)
{{
    return sizeof(profiles) / sizeof(profiles[0]);
}}

const emaster_slave_profile_t *emaster_slave_profile_at(size_t index)
{{
    if (index >= emaster_slave_profile_count())
    {{
        return NULL;
    }}
    return &profiles[index];
}}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path, nargs="+")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    documents = [json.loads(path.read_text(encoding="utf-8")) for path in args.input]
    output = generate(documents)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
