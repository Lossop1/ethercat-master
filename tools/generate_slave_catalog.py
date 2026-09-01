#!/usr/bin/env python3
"""根据设备配置集合生成编译期从站目录。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def parse_unsigned(value: object, field: str, maximum: int) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise ValueError(f"{field} 必须是十六进制字符串")
    number = int(value, 16)
    if not 0 <= number <= maximum:
        raise ValueError(f"{field} 超出范围")
    return number


def c_string(value: object) -> str:
    return json.dumps(str(value), ensure_ascii=True)


def direction_values(direction: dict[str, Any], field: str) -> dict[str, object]:
    """提取一个 PDO 方向中的全部映射表，保留配置声明的顺序。"""
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


def profile_values(document: dict[str, Any]) -> dict[str, object]:
    """提取 C 目录所需字段，并在生成前完成类型和范围转换。"""
    identity = document["identity"]
    pdo_sets = document["pdo_sets"]
    selected_id = document["selected_pdo_set"]
    if not isinstance(identity, dict) or not isinstance(pdo_sets, list):
        raise ValueError("identity 或 pdo_sets 类型无效")

    selected = next(
        (item for item in pdo_sets if isinstance(item, dict) and item.get("id") == selected_id),
        None,
    )
    if selected is None:
        raise ValueError(f"选择的 PDO 集合 {selected_id!r} 不存在")

    conversion = document["conversion"]
    protocol = document["protocol"]
    if not all(isinstance(item, dict) for item in (conversion, protocol)):
        raise ValueError("设备配置包含无效对象")

    return {
        "profile_id": c_string(document["profile_id"]),
        "model": c_string(document["model"]),
        "vendor_id": parse_unsigned(identity["vendor_id"], "identity.vendor_id", 0xFFFFFFFF),
        "product_code": parse_unsigned(
            identity["product_code"], "identity.product_code", 0xFFFFFFFF
        ),
        "revision": parse_unsigned(identity["revision"], "identity.revision", 0xFFFFFFFF),
        "rx": direction_values(selected["rx"], "rx"),
        "tx": direction_values(selected["tx"], "tx"),
        "encoder_counts": int(conversion["encoder_counts_per_motor_revolution_default"]),
        "requires_dc": "true" if protocol["requires_distributed_clocks"] else "false",
    }


def profile_initializer(values: dict[str, object], ordinal: int) -> str:
    """把一个设备配置渲染为只读 C 结构初始化器。"""
    rx = values["rx"]
    tx = values["tx"]
    return f"""    {{
        .profile_id = {values['profile_id']},
        .model = {values['model']},
        .identity = {{
            .vendor_id = UINT32_C(0x{values['vendor_id']:08X}),
            .product_code = UINT32_C(0x{values['product_code']:08X}),
            .revision = UINT32_C(0x{values['revision']:08X}),
        }},
        .rx_pdo_bytes = UINT16_C({rx['bytes']}),
        .tx_pdo_bytes = UINT16_C({tx['bytes']}),
        .rx_pdo_mappings = profile_{ordinal}_rx_mappings,
        .rx_pdo_mapping_count = sizeof(profile_{ordinal}_rx_mappings) / sizeof(profile_{ordinal}_rx_mappings[0]),
        .tx_pdo_mappings = profile_{ordinal}_tx_mappings,
        .tx_pdo_mapping_count = sizeof(profile_{ordinal}_tx_mappings) / sizeof(profile_{ordinal}_tx_mappings[0]),
        .encoder_counts_per_motor_revolution_default = UINT32_C({values['encoder_counts']}),
        .requires_distributed_clocks = {values['requires_dc']},
    }}"""


def mapping_declarations(values: dict[str, object], profile_ordinal: int) -> list[str]:
    """生成一个设备全部 PDO 条目数组和映射表数组。"""
    declarations = []
    for direction_name in ("rx", "tx"):
        direction = values[direction_name]
        mapping_initializers = []
        for mapping_ordinal, mapping in enumerate(direction["mappings"]):
            entries_name = (
                f"profile_{profile_ordinal}_{direction_name}_mapping_{mapping_ordinal}_entries"
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
        declarations.append(
            f"static const emaster_pdo_mapping_profile_t "
            f"profile_{profile_ordinal}_{direction_name}_mappings[] = {{\n"
            + ",\n".join(mapping_initializers)
            + "\n};"
        )
    return declarations


def generate(documents: list[dict[str, Any]]) -> str:
    """按稳定 ID 排序生成目录，保证相同输入产生逐字节一致的输出。"""
    if not documents:
        raise ValueError("至少需要一个设备配置")

    ordered = sorted(documents, key=lambda item: str(item["profile_id"]))
    profile_ids = [str(item["profile_id"]) for item in ordered]
    if len(profile_ids) != len(set(profile_ids)):
        raise ValueError("设备配置的 profile_id 必须唯一")

    values = [profile_values(item) for item in ordered]
    declarations = []
    for ordinal, item in enumerate(values):
        declarations.extend(mapping_declarations(item, ordinal))
    initializers = ",\n".join(
        profile_initializer(item, ordinal) for ordinal, item in enumerate(values)
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

    # CMake 传入完整文件集合，生成器不感知具体型号名或设备数量。
    documents = [json.loads(path.read_text(encoding="utf-8")) for path in args.input]
    output = generate(documents)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
