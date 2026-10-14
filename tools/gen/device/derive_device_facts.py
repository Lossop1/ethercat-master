#!/usr/bin/env python3
"""从 ESI 事实推导设备档里的"硬件事实"部分。

为什么要这个脚本
    设备档原来是把 ESI 里的硬件事实手抄一遍来的。抄写这一步就是交付的断点：
    第三方换电机时必须重抄，而且不知道该抄哪些。硬件事实在 ESI 里是全的，
    tools/gen/device/esi_facts.py 也已经能把它读出来，缺的只是拼成设备档的形状。

本脚本产出什么
    只产出"换一台电机就会变、而人不需要做任何决定"的字段：
    身份、协议能力、PDO 集合的选择方式 / 字节数 / 映射条目。
    它不产出任何工程决策 —— 选哪套 PDO、停机判据用哪个掩码、读哪些遥测、
    换算期望值 —— 那些由人写在策略档里，本脚本一个字都不碰。

语义名从哪来
    src/cia402/process_image.c:141 按字符串名把运行方案要的字段和设备档
    的 PDO 条目对起来。那些名字不是厂商起的，是 CiA 402 / CiA 301 规定的对象
    索引决定的，所以可以由索引查表得到。下表属于标准，不属于任何台架。

生效 PDO 怎么选
    ESI 用 <Module> 声明 PDO 的设备是模块化的，一个模块一套方案，运行期靠
    0xF030 选；直接把 PDO 挂在 <Device> 下的设备没有模块，只能靠厂默认分配
    与 Sm 属性互相印证出唯一一套。两条规则不必都成立，但必须一致。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from esi_facts import load_esi_facts, select_device  # noqa: E402

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
# homing 是模式 6：ESI 里的写法是 "homing - axis"。它没有周期目标，程序映像只写
# 控制字与模式本身——这一点由运行期按模式判，不是在这里。
STANDARD_MODE_VALUES: dict[str, int] = {"csp": 8, "csv": 9, "cst": 10, "homing": 6}


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


def _selected_direction(pdos: list[dict[str, Any]]) -> dict[str, Any]:
    """把若干张生效映射表按分配顺序拼成一个方向，字节数是它们的和。"""
    byte_lengths = []
    for pdo in pdos:
        if pdo["byte_length"] is None:
            raise SystemExit(f"PDO {pdo['index']} 的位宽不是整字节，无法推导字节数")
        byte_lengths.append(int(pdo["byte_length"]))
    return {
        "bytes": sum(byte_lengths),
        "mappings": [{"index": pdo["index"], "entries": _entries(pdo)} for pdo in pdos],
    }


def _device_pdo_set(facts: dict[str, Any]) -> dict[str, Any]:
    """非模块化设备：ESI 没有模块，只能从设备级声明里选唯一一套生效 PDO。

    两条互相独立的规则都要给出同一套，才认为这份 ESI 足以确定映射：
      1. 对象字典里 0x1C12/0x1C13 的出厂默认分配 —— 设备出厂就这样接的；
      2. PDO 上的 Sm 属性 —— 只有真正绑到过程数据同步管理器上的映射才带 Sm。
    只有一条能用、或两条给出不同集合，都直接失败：这时任何一套都是猜的，
    猜错的后果是位偏移全错，跑起来才发现。
    """
    declared = facts.get("default_pdo_assignment") or {}
    by_default = {
        direction: [str(index) for index in declared.get(direction) or []]
        for direction in ("rx", "tx")
    }
    by_sm = {
        direction: [
            pdo["index"]
            for pdo in facts["direct_pdos"][direction]
            if pdo.get("sync_manager") == sync_manager
        ]
        for direction, sync_manager in (("rx", 2), ("tx", 3))
    }
    if by_default != by_sm or not all(by_default.values()):
        raise SystemExit(
            "无法确定设备级生效 PDO —— 两条规则必须非空且一致。\n"
            f"  出厂默认分配 1C12/1C13：{by_default}\n"
            f"  PDO 的 Sm 属性：        {by_sm}\n"
            "这份 ESI 既没用 <Module> 声明 PDO，又没有能互相印证的默认分配，"
            "不足以确定映射；不要按其中一条猜着往下走。"
        )

    selected: dict[str, Any] = {}
    for direction in ("rx", "tx"):
        by_index = {pdo["index"]: pdo for pdo in facts["direct_pdos"][direction]}
        missing = [index for index in by_default[direction] if index not in by_index]
        if missing:
            raise SystemExit(
                f"{direction} 的默认分配引用了 ESI 里没有的 PDO：{', '.join(missing)}"
            )
        selected[direction] = _selected_direction(
            [by_index[index] for index in by_default[direction]]
        )
    return {
        "assignment_style": "direct_1c12",
        "module_ident": None,
        "rx": selected["rx"],
        "tx": selected["tx"],
    }


def module_mode_ids(module: dict[str, Any]) -> dict[str, int]:
    """一个模块的类型串里写了哪几种模式。

    ESI 里的写法是 "csv,csp,cst - axis"：逗号分隔，末尾那个还带 " - axis"
    后缀。只按逗号切会把 "cst - axis" 整个当成一个记号，"cst" 就漏掉了
    —— 第一版就是这么错的，实测推导出的模式集合少了 cst。
    """
    modes: dict[str, int] = {}
    kind = (module.get("type") or "").lower().replace("-", ",").replace("/", ",")
    for token in kind.split(","):
        token = token.strip()
        if token in STANDARD_MODE_VALUES:
            modes[token] = STANDARD_MODE_VALUES[token]
    return modes


def _modes(modules: list[dict[str, Any]]) -> dict[str, int]:
    modes: dict[str, int] = {}
    for module in modules:
        modes.update(module_mode_ids(module))
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
            # ESI 给的是"这套同步怎么使能"的硬事实：DC 与 SM 各自的 AssignActivate。
            # 留在设备档里，是为了让生成运行方案的那一步用厂商写的数，而不是用
            # "标准上一般是 0x0300"这种看着像事实的猜测 —— 写错这个数会安静地把
            # 从站的同步配置配成另一套，主机这边只会看到 DC 对不上。
            "assign_activate_by_strategy": facts["synchronization"][
                "assign_activate_by_strategy"
            ],
            "supported_modes": _modes(modules),
        },
        # 模块化设备由 0xF030 选模块，一套模块一套映射；非模块化设备没有模块，
        # 退回设备级声明，且必须能唯一确定 —— 见 _device_pdo_set。
        "pdo_sets": [
            {
                "assignment_style": "module_slot",
                "module_ident": module["module_ident"],
                "rx": _direction(module, "rx"),
                "tx": _direction(module, "tx"),
            }
            for module in modules
        ]
        or [_device_pdo_set(facts)],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="从 ESI 推导设备档的硬件事实部分")
    parser.add_argument("esi", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--select",
        help="ESI 里有多台设备时挑一台：清单里的序号（从 1 起），或 厂商ID:产品码:版本",
    )
    args = parser.parse_args()

    facts = select_device(load_esi_facts(args.esi), args.select)
    text = json.dumps(derive(facts), ensure_ascii=False, indent=2)
    if args.output is None:
        print(text)
    else:
        args.output.write_text(text + "\n", encoding="utf-8")
        print(f"已写出 {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
