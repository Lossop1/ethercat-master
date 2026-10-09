#!/usr/bin/env python3
"""从 ESI 事实推导设备档里的"硬件事实"部分。

为什么要这个脚本
    设备档原来是把 ESI 里的硬件事实手抄一遍来的。抄写这一步就是交付的断点：
    第三方换电机时必须重抄，而且不知道该抄哪些。硬件事实在 ESI 里是全的，
    gen/device/esi_facts.py 也已经能把它读出来，缺的只是拼成设备档的形状。

本脚本产出什么
    只产出"换一台电机就会变、而人不需要做任何决定"的字段：
    身份、协议能力、PDO 集合的 module_ident / 字节数 / 映射条目。
    它不产出任何工程决策 —— 选哪套 PDO、停机判据用哪个掩码、读哪些遥测、
    换算期望值 —— 那些由人写在策略档里，本脚本一个字都不碰。

语义名从哪来
    src/cia402/process_image.c:141 按字符串名把运行方案要的字段和设备档
    的 PDO 条目对起来。那些名字不是厂商起的，是 CiA 402 / CiA 301 规定的对象
    索引决定的，所以可以由索引查表得到。下表属于标准，不属于任何台架。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from esi_facts import load_esi_facts  # noqa: E402

# CiA 402 / CiA 301 规定的对象索引 → 主站内部使用的字段名。
# 这张表是标准的一部分：换任何一台合规的 CiA 402 驱动器，同一索引的含义不变。
# 键一律小写。查表时把索引 .lower() —— ESI 里的十六进制大小写不固定。
# 不要用 .upper()：那会把 "0x" 的前缀也变成 "0X"，而表里是小写的 x。
# 这个坑我在这一个函数里踩了三次，所以键和查表都固定用小写。
STANDARD_FIELD_NAMES: dict[str, str] = {
    "0x0000": "padding",
    "0x6040": "control_word",
    "0x6041": "status_word",
    "0x6060": "mode_of_operation",
    "0x6061": "mode_display",
    "0x6064": "actual_position",
    "0x606c": "actual_velocity",
    "0x6071": "target_torque",
    "0x6077": "actual_torque",
    "0x607a": "target_position",
    "0x60ff": "target_velocity",
}

# ESI 里 module 的 type 字符串 → CiA 402 模式号。同样是标准取值。
STANDARD_MODE_VALUES: dict[str, int] = {"csp": 8, "csv": 9, "cst": 10}


def _field_name(index: str) -> str:
    key = index.lower()
    if key in STANDARD_FIELD_NAMES:
        return STANDARD_FIELD_NAMES[key]
    # 不认识的索引不猜，原样保留索引当名字 —— 后面的人工审阅会看到它。
    return "unknown_" + key[2:]


def _entries(mapping: dict[str, Any]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for entry in mapping.get("entries", []):
        out.append(
            {
                # 不做大小写变换：esi_facts.py 的 _hex_text 已经给出规范形式
                # （0x 前缀小写、十六进制数字大写）。上一版在这里 .upper() 过，
                # 结果变成 0X00119800 —— 前缀也被大写了。
                "index": entry["index"],
                "subindex": int(entry.get("subindex", 0)),
                "bits": int(entry["bits"]),
                "data_type": entry["data_type"],
                "name": _field_name(entry["index"]),
            }
        )
    return out


def _direction(module: dict[str, Any], key: str) -> dict[str, Any]:
    pdos = module.get(key) or []
    if not pdos:
        return {"bytes": 0, "mappings": []}
    return {
        "bytes": int(pdos[0]["byte_length"]),
        "mappings": [
            {"index": pdo["index"], "entries": _entries(pdo)} for pdo in pdos
        ],
    }


def _modes(modules: list[dict[str, Any]]) -> dict[str, int]:
    modes: dict[str, int] = {}
    for module in modules:
        # ESI 里的写法是 "csv,csp,cst - axis"：逗号分隔，末尾那个还带 " - axis"
        # 后缀。只按逗号切会把 "cst - axis" 整个当成一个记号，"cst" 就漏掉了
        # —— 第一版就是这么错的，实测推导出的模式集合少了 cst。
        kind = (module.get("type") or "").lower().replace("-", ",").replace("/", ",")
        for token in kind.split(","):
            token = token.strip()
            if token in STANDARD_MODE_VALUES:
                modes[token] = STANDARD_MODE_VALUES[token]
    return dict(sorted(modes.items()))


def derive(facts: dict[str, Any]) -> dict[str, Any]:
    device = facts["device"]
    modules = facts.get("modules") or []
    return {
        "model": device.get("model"),
        "identity": {
            "vendor_name": device.get("vendor_name"),
            "vendor_id": device["vendor_id"],
            "product_code": device["product_code"],
            "revision": device["revision"],
        },
        "protocol": {
            "profile": "CiA 402",
            "mailbox": "CoE",
            "supports_pdo_assignment": facts["capabilities"]["supports_pdo_assignment"],
            "supports_pdo_configuration": facts["capabilities"][
                "supports_pdo_configuration"
            ],
            "supports_distributed_clocks": facts["capabilities"][
                "supports_distributed_clocks"
            ],
            "supported_modes": _modes(modules),
        },
        "pdo_sets": [
            {
                "module_ident": module["module_ident"],
                "rx": _direction(module, "rx"),
                "tx": _direction(module, "tx"),
            }
            for module in modules
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="从 ESI 推导设备档的硬件事实部分")
    parser.add_argument("esi", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    text = json.dumps(derive(load_esi_facts(args.esi)), ensure_ascii=False, indent=2)
    if args.output is None:
        print(text)
    else:
        args.output.write_text(text + "\n", encoding="utf-8")
        print(f"已写出 {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
