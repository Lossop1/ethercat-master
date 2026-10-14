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
    --facts   由 tools/gen/device/derive_device_facts.py 从 ESI 产出
    --policy  人写的策略档

事实与策略的边界：facts 说什么硬件就是什么，policy 只做选择。
所以两者撞车时以 facts 为准，并报错要求人去改策略 —— 见 _supported_modes。
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

# 生效 PDO 怎么选：模块化设备先写 0xF030 选模块，非模块化设备直接写 1C12/1C13。
# 缺省取模块化 —— 既有设备档都是这类，不写这个键时行为逐字节不变。
ASSIGNMENT_STYLE_MODULE_SLOT = "module_slot"


def _pdo_sets(
    facts: dict[str, Any], policy: dict[str, Any]
) -> list[dict[str, Any]]:
    """按策略挑出要写进设备档的 PDO 方案，硬件部分一律取自 facts。"""
    by_ident = {
        pdo["module_ident"]: pdo
        for pdo in facts["pdo_sets"]
        if pdo["assignment_style"] == "module_slot"
    }
    direct = [pdo for pdo in facts["pdo_sets"] if pdo["assignment_style"] == "direct_1c12"]

    pdo_sets: list[dict[str, Any]] = []
    for entry in policy["pdo_sets"]:
        if entry.get("assignment_style", ASSIGNMENT_STYLE_MODULE_SLOT) == "direct_1c12":
            # 非模块化设备只有出厂那一套映射，策略没有可挑的余地，
            # 所以这里不写 module_ident / module_slot —— 它们对这类设备没有含义。
            if len(direct) != 1:
                raise SystemExit(
                    f"策略按直连方式引用 PDO，ESI 却给出 {len(direct)} 套设备级映射；应为 1 套"
                )
            hardware = direct[0]
            ident = None
            module_slot = 0
        else:
            ident = entry["module_ident"]
            if ident not in by_ident:
                raise SystemExit(
                    f"策略引用了 ESI 里没有的 PDO 模块：{ident}"
                    f"（ESI 里有：{', '.join(sorted(by_ident))}）"
                )
            hardware = by_ident[ident]
            module_slot = entry["module_slot"]
        pdo_sets.append(
            {
                "id": entry["id"],
                "assignment_style": entry.get(
                    "assignment_style", ASSIGNMENT_STYLE_MODULE_SLOT
                ),
                "module_ident": ident,
                "module_slot": module_slot,
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
    return pdo_sets


def _supported_modes(facts: dict[str, Any], policy: dict[str, Any]) -> dict[str, int]:
    """模式能力由人声明，但 ESI 声明了模块类型时以 ESI 为准，两者必须一致。

    ESI 的模块类型是"这台设备支持哪些模式"的直接证据；人写的那份是转述。
    转述和证据对不上就是有一方错了，不能挑一个信。
    非模块化 ESI 不写模块类型，观察不到 —— 这时只有人写的那份，是已知的唯一来源。
    """
    if "supported_modes" not in policy:
        raise SystemExit("策略档缺 supported_modes：模式能力推不出来，必须由人声明")
    declared = {str(key): int(value) for key, value in policy["supported_modes"].items()}
    observed = facts["protocol"]["supported_modes"]
    if observed and observed != declared:
        raise SystemExit(
            f"策略声明支持的模式 {declared} 与 ESI 模块类型给出的 {observed} 不一致"
        )
    return declared


def build(facts: dict[str, Any], policy: dict[str, Any], esi_path: str) -> dict[str, Any]:
    pdo_sets = _pdo_sets(facts, policy)
    supported_modes = _supported_modes(facts, policy)

    merged: dict[str, Any] = {
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
            "assign_activate_by_strategy": facts["protocol"]["assign_activate_by_strategy"],
            "safe_stop_status_mask": policy["safe_stop_status_mask"],
            "safe_stop_status_value": policy["safe_stop_status_value"],
            "supported_modes": supported_modes,
        },
        "reference_pdo_set_id": policy["reference_pdo_set_id"],
        "pdo_sets": pdo_sets,
    }
    # 这两项只在策略给了内容时才写进去。缺省在这里的含义是"厂商资料没给"，
    # 是句实话；编一个默认值填上就成了假的设备事实。
    if policy.get("conversion"):
        merged["conversion"] = policy["conversion"]
    if policy.get("slow_telemetry"):
        merged["slow_telemetry"] = policy["slow_telemetry"]
    merged["source"] = {"esi": esi_path}
    return merged


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
