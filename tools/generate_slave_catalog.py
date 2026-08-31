#!/usr/bin/env python3
"""根据设备配置集合生成编译期从站目录。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def parse_u32(value: object, field: str) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise ValueError(f"{field} 必须是十六进制字符串")
    number = int(value, 16)
    if not 0 <= number <= 0xFFFFFFFF:
        raise ValueError(f"{field} 超出 uint32 范围")
    return number


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


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

    rx = selected["rx"]
    tx = selected["tx"]
    conversion = document["conversion"]
    protocol = document["protocol"]
    if not all(isinstance(item, dict) for item in (rx, tx, conversion, protocol)):
        raise ValueError("设备配置包含无效对象")

    return {
        "profile_id": c_string(str(document["profile_id"])),
        "model": c_string(str(document["model"])),
        "vendor_id": parse_u32(identity["vendor_id"], "identity.vendor_id"),
        "product_code": parse_u32(identity["product_code"], "identity.product_code"),
        "revision": parse_u32(identity["revision"], "identity.revision"),
        "rx_index": parse_u32(rx["index"], "rx.index"),
        "tx_index": parse_u32(tx["index"], "tx.index"),
        "rx_bytes": int(rx["bytes"]),
        "tx_bytes": int(tx["bytes"]),
        "encoder_counts": int(conversion["encoder_counts_per_motor_revolution_default"]),
        "requires_dc": "true" if protocol["requires_distributed_clocks"] else "false",
    }


def profile_initializer(values: dict[str, object]) -> str:
    """把一个设备配置渲染为只读 C 结构初始化器。"""
    return f"""    {{
        .profile_id = {values['profile_id']},
        .model = {values['model']},
        .identity = {{
            .vendor_id = UINT32_C(0x{values['vendor_id']:08X}),
            .product_code = UINT32_C(0x{values['product_code']:08X}),
            .revision = UINT32_C(0x{values['revision']:08X}),
        }},
        .rx_pdo_index = UINT16_C(0x{values['rx_index']:04X}),
        .tx_pdo_index = UINT16_C(0x{values['tx_index']:04X}),
        .rx_pdo_bytes = UINT16_C({values['rx_bytes']}),
        .tx_pdo_bytes = UINT16_C({values['tx_bytes']}),
        .encoder_counts_per_motor_revolution_default = UINT32_C({values['encoder_counts']}),
        .requires_distributed_clocks = {values['requires_dc']},
    }}"""


def generate(documents: list[dict[str, Any]]) -> str:
    """按稳定 ID 排序生成目录，保证相同输入产生逐字节一致的输出。"""
    if not documents:
        raise ValueError("至少需要一个设备配置")

    ordered = sorted(documents, key=lambda item: str(item["profile_id"]))
    profile_ids = [str(item["profile_id"]) for item in ordered]
    if len(profile_ids) != len(set(profile_ids)):
        raise ValueError("设备配置的 profile_id 必须唯一")

    initializers = ",\n".join(profile_initializer(profile_values(item)) for item in ordered)
    return f"""/* 由 tools/generate_slave_catalog.py 生成，禁止手工修改。 */
#include "emaster/catalog/slave_profile.h"

#include <stddef.h>

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
