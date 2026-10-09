#!/usr/bin/env python3
"""把"从 ESI 推导的硬件事实"与"人写的策略"合成一份设备档。

为什么要这一步
    设备档原来是手写的一份。查下来它里面混着两类东西：
      · 硬件事实 —— 身份、PDO 映射、每个条目的位宽和数据类型。这些 ESI 里全有，
        换一台电机就变，而人不需要对它们做任何决定；
      · 工程策略 —— 选哪套 PDO、进 OP 前把模式写成几、停机判据用状态字哪一位、
        读哪些遥测、换算从哪个对象读。这些是人决定的。
    混在一起的结果是：第三方换电机时不知道哪些该改、哪些该照抄，只能整份重抄。
    拆开之后，他要写的只有策略那一份。

合成的产物必须与拆分前的设备档逐字段一致 —— scripts 里有一条校验跑这件事。

两个输入
    --facts   由 gen/device/derive_device_facts.py 从 ESI 产出
    --policy  人写的策略档
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

# CiA 402 规定"模式"对象是 0x6060:0，且过渡只允许在 SAFE-OP -> OP 那一步做。
# 校验器（validate_device_config.py）也强制 transition == "safeop_to_op"，
# 所以策略档里不需要写 transition / index / subindex，只写模式值。
MODE_OBJECT_INDEX = "0x6060"
MODE_OBJECT_SUBINDEX = 0
MODE_TRANSITION = "safeop_to_op"

PROTOCOL_PROFILE = "CiA 402"
PROTOCOL_MAILBOX = "CoE"


def build(facts: dict[str, Any], policy: dict[str, Any], esi_path: str) -> dict[str, Any]:
    by_ident = {pdo["module_ident"]: pdo for pdo in facts["pdo_sets"]}

    pdo_sets: list[dict[str, Any]] = []
    for entry in policy["pdo_sets"]:
        ident = entry["module_ident"]
        if ident not in by_ident:
            raise SystemExit(
                f"策略引用了 ESI 里没有的 PDO 模块：{ident}"
                f"（ESI 里有：{', '.join(sorted(by_ident))}）"
            )
        hardware = by_ident[ident]
        pdo_sets.append(
            {
                "id": entry["id"],
                "module_ident": ident,
                "module_slot": entry["module_slot"],
                "mode_initialization": {
                    "transition": MODE_TRANSITION,
                    "index": MODE_OBJECT_INDEX,
                    "subindex": MODE_OBJECT_SUBINDEX,
                    "value": entry["mode_value"],
                },
                "rx": hardware["rx"],
                "tx": hardware["tx"],
            }
        )

    return {
        "schema_version": policy.get("schema_version", 1) + 1,
        "profile_id": policy["profile_id"],
        "model": facts["model"],
        "identity": facts["identity"],
        "protocol": {
            "profile": PROTOCOL_PROFILE,
            "mailbox": PROTOCOL_MAILBOX,
            "supports_pdo_assignment": facts["protocol"]["supports_pdo_assignment"],
            "supports_pdo_configuration": facts["protocol"]["supports_pdo_configuration"],
            "supports_distributed_clocks": facts["protocol"]["supports_distributed_clocks"],
            "safe_stop_status_mask": policy["safe_stop_status_mask"],
            "safe_stop_status_value": policy["safe_stop_status_value"],
            "supported_modes": facts["protocol"]["supported_modes"],
        },
        "reference_pdo_set_id": policy["reference_pdo_set_id"],
        "pdo_sets": pdo_sets,
        "conversion": policy["conversion"],
        "slow_telemetry": policy["slow_telemetry"],
        "source": {"esi": esi_path},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="合成设备档：ESI 事实 + 策略")
    parser.add_argument("--facts", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--esi", default="", help="写进 source.esi 的路径，只作记录")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    merged = build(
        json.loads(args.facts.read_text(encoding="utf-8")),
        json.loads(args.policy.read_text(encoding="utf-8")),
        args.esi,
    )
    text = json.dumps(merged, ensure_ascii=False, indent=2) + "\n"
    if args.output is None:
        print(text, end="")
    else:
        args.output.write_text(text, encoding="utf-8")
        print(f"已写出 {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
