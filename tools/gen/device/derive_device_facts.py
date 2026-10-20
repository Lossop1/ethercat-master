#!/usr/bin/env python3
"""从 ESI 事实推导设备档里的"硬件事实"部分。

为什么要这个脚本
    设备档原来是把 ESI 里的硬件事实手抄一遍来的。抄写这一步就是交付的断点：
    第三方换电机时必须重抄，而且不知道该抄哪些。硬件事实在 ESI 里是全的，
    tools/gen/device/esi_facts.py 也已经能把它读出来，缺的只是拼成设备档的形状。

本脚本产出什么
    只产出"换一台电机就会变、而人不需要做任何决定"的字段：
    身份、协议能力、PDO 集合的选择方式 / 字节数 / 映射条目，
    以及从对象字典里能直接认出的慢速遥测对象。
    它不产出工程决策 —— 选哪套 PDO、停机判据用哪个掩码、换算期望值 ——
    那些由人写在策略档里，本脚本一个字都不碰。

慢速遥测从哪来
    ESI 的对象字典说了这台设备有哪些对象、叫什么、多大、能不能读。
    CiA 402 标准索引（0x6078 电流、0x6079 母线电压、0x603F 错误码等）
    的含义由标准定；厂商私有区里只有名字能说明用途，所以只接受与下表
    完全同名的对象（"Voltage"、"Motor Temperature"……），不做模糊匹配。
    一个语义只取一条：标准索引优先于私有对象，再按索引与子索引排序。

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
import re
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

# ESI 里 module 的 type 字符串 → 模式号。同模块类型串联的是同一族设备，取值由厂商定。
# homing 是模式 6：ESI 里的写法是 "homing - axis"。它没有周期目标，程序映像只写
# 控制字与模式本身——这一点由运行期按模式判，不是在这里。
# mit 是模式 5：厂商的 MIT 复合控制律（位置环 + 速度环 + 前馈力矩），不是 CiA 402
# 定的模式号，是这台家族自己在 ESI 里声明的（见模块 "Mit - axis"）。
STANDARD_MODE_VALUES: dict[str, int] = {"csp": 8, "csv": 9, "cst": 10, "homing": 6, "mit": 5}

# 同一个 MIT 增益，不同厂商在 ESI 里叫法不同：良质写 "MitKp"/"MitKd"（0x2013:1/2），
# MT 写 "PVT_KP"/"PVT_KD"（0x2000/0x2001）。运行方案按 mit_kp/mit_kd 这两个名字
# 找它们，所以把 PVT 的叫法折到同一组名字上。对象索引不在这里定：由生成器从
# 实际 PDO 映射里取，MT 与良质的索引本就不同。
FIELD_NAME_ALIASES: dict[str, str] = {"pvt_kp": "mit_kp", "pvt_kd": "mit_kd"}

# 标准索引 → 慢速遥测语义。全部来自 CiA 301/402，换任何合规驱动器含义不变。
# 0x607A 是驱动器自己持有的目标位置：读它用于区分"值没到驱动器"与
# "到了驱动器但不执行"，它不是反馈。0x6072/0x6080 是配置型读数，
# 不在周期遥测里，故不列。
STANDARD_SLOW_TELEMETRY: dict[str, str] = {
    "0x603f": "error_code",
    "0x606c": "actual_velocity",
    "0x6078": "actual_current",
    "0x6079": "bus_voltage",
    "0x607a": "target_position_readback",
    "0x60ff": "target_velocity",
}

# 厂商私有对象没有标准含义，只能按 ESI 的 <Name> 认。键是规范化后的名字：
# 小写、非字母数字折成单个空格。只做完全匹配 —— "Max Bus Voltage" 是保护阈值，
# 不是母线电压实测值，模糊匹配会把它读成电压。
VENDOR_SLOW_TELEMETRY: dict[str, str] = {
    "voltage": "bus_voltage",
    "bus voltage": "bus_voltage",
    "bus voltage value": "bus_voltage",
    "dc link voltage": "bus_voltage",
    "dc link circuit voltage": "bus_voltage",
    "drive temperature": "mosfet_temperature",
    "mosfet temperature": "mosfet_temperature",
    "mosfet temperature value": "mosfet_temperature",
    "igbt junction temperature": "mosfet_temperature",
    "motor temperature": "motor_temperature",
    "motor temperature value": "motor_temperature",
    "extended servo error code": "extended_error_code",
    "servo error code": "servo_error_code",
}

# 输出顺序固定，便于比对两份设备档的差别。
SLOW_TELEMETRY_ORDER = (
    "actual_current",
    "bus_voltage",
    "mosfet_temperature",
    "motor_temperature",
    "actual_velocity",
    "target_velocity",
    "error_code",
    "extended_error_code",
    "servo_error_code",
    "target_position_readback",
)

_SIGNED_TYPES = {"SINT", "INT", "DINT", "LINT"}
_TELEMETRY_TYPES = {8: "u8", 16: "u16", 32: "u32"}
# 能读的权限。没有 Access 的条目通常是 DataType 里给 PDO 映射起名字的
# 子项（0x1A02 的 :6 "Motor Temperature" 就是映射描述），不是测量对象。
_READABLE_ACCESS = {"ro", "rw", "rwr", "rww"}

# 过程数据映射对象本身不是测量量。0x1A02 的子项名字与真实对象一模一样，
# 不排除就会把映射表条目当成温度读回来。
_PDO_MAPPING_RANGES = ((0x1600, 0x17FF), (0x1A00, 0x1BFF))
_PDO_ASSIGNMENT_INDEXES = {0x1C12, 0x1C13}


def _is_pdo_mapping_object(index: int) -> bool:
    return index in _PDO_ASSIGNMENT_INDEXES or any(
        low <= index <= high for low, high in _PDO_MAPPING_RANGES
    )


def _normalized_name(text: str | None) -> str:
    if not isinstance(text, str):
        return ""
    return re.sub(r"[^a-z0-9]+", " ", text.strip().lower()).strip()


def _telemetry_type(type_name: str | None, bits: int) -> str | None:
    """ESI 类型 + 位宽 → 设备档声明的读取宽度。认不出就不进清单。"""
    kind = (type_name or "").strip().upper()
    base = _TELEMETRY_TYPES.get(bits)
    if base is None:
        return None
    if base.startswith("u") and kind in _SIGNED_TYPES:
        return "i" + base[1:]
    return base


def _slow_telemetry_name(object_name: str | None, fallback: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "_", (object_name or "").strip().lower()).strip("_")
    return slug or fallback


def _slow_telemetry_entries(facts: dict[str, Any]) -> list[dict[str, Any]]:
    """从对象字典里挑出能周期读的对象，拼成设备档的 slow_telemetry。

    只保留"语义 + 地址"唯一的两级去重结果之一：标准索引优先（它的含义
    有标准背书），同一优先级按索引和子索引取最小的那条。名字在表内唯一，
    重名时补上索引后缀 —— 运行期按名字把读数放进会话槽位，重名会串值。
    """
    chosen: dict[str, tuple[tuple[int, int, int], dict[str, Any]]] = {}
    for object_config in facts.get("object_dictionary") or []:
        index = str(object_config.get("index", "")).lower()
        if _is_pdo_mapping_object(int(index, 0)):
            continue
        candidates: list[dict[str, Any]] = []
        if object_config.get("subitems"):
            for subitem in object_config["subitems"]:
                candidates.append(
                    {
                        "index": index,
                        "subindex": int(subitem.get("subindex", 0)),
                        "name": subitem.get("name"),
                        "type": subitem.get("type"),
                        "bits": int(subitem.get("bits", 0)),
                        "access": subitem.get("access"),
                    }
                )
        else:
            candidates.append(
                {
                    "index": index,
                    "subindex": 0,
                    "name": object_config.get("name"),
                    "type": object_config.get("type"),
                    "bits": int(object_config.get("bits", 0)),
                    "access": object_config.get("access"),
                }
            )

        for candidate in candidates:
            access = str(candidate["access"] or "").strip().lower()
            # 标准索引缺 Access 时按可读处理：对象含义由标准定，权限没写
            # 只是这份 ESI 少写了一条。写明了不可读（wo）则一律不读。
            if access not in _READABLE_ACCESS and not (
                access == "" and index in STANDARD_SLOW_TELEMETRY
            ):
                continue
            if index in STANDARD_SLOW_TELEMETRY:
                semantic = STANDARD_SLOW_TELEMETRY[index]
                standard = 0
            else:
                semantic = VENDOR_SLOW_TELEMETRY.get(
                    _normalized_name(candidate["name"])
                )
                standard = 1
            if semantic is None:
                continue
            telemetry_type = _telemetry_type(candidate["type"], candidate["bits"])
            if telemetry_type is None:
                # 位宽不是 8/16/32：主站按这四种宽度读，别的宽度读不了。
                continue
            rank = (standard, int(index, 0), candidate["subindex"])
            previous = chosen.get(semantic)
            if previous is not None and previous[0] <= rank:
                continue
            chosen[semantic] = (
                rank,
                {
                    "name": _slow_telemetry_name(
                        candidate["name"],
                        f"object_{index[2:]}_{candidate['subindex']}",
                    ),
                    "index": index,
                    "subindex": candidate["subindex"],
                    "type": telemetry_type,
                    "semantic": semantic,
                    # ESI 不写刻度与单位。空字符串表示厂商没声明，
                    # 报告按原始读数写，不编一个像"V"或"°C"的单位。
                    "unit": "",
                },
            )

    entries: list[dict[str, Any]] = []
    seen_names: set[str] = set()
    for semantic in SLOW_TELEMETRY_ORDER:
        if semantic not in chosen:
            continue
        entry = dict(chosen[semantic][1])
        name = entry["name"]
        if name in seen_names:
            name = f"{name}_{entry['index'][2:]}_{entry['subindex']}"
            entry["name"] = name
        seen_names.add(name)
        entries.append(entry)
    return entries


def _vendor_field_name(entry: dict[str, Any]) -> str:
    """厂商自定义索引的字段名，取 ESI 自己给这个条目起的名字。

    标准表只覆盖 CiA 301/402 规定的索引。厂商区（如 0x2000 起的 MIT 参数）没有
    标准名可用，而"按索引猜"在这里会撞车：0x2013:1 与 0x2013:2 是同一个索引的两个
    子索引，按索引起名两者都会叫 unknown_2013，同一份映射里出现两个同名字段。
    ESI 的 <Entry><Name> 是厂商为这个条目写下的名字（0x2013:1 是 "MitKp"），把它
    转成小写下划线形式，风格与标准表一致，运行方案也能直接引用。
    """
    raw = entry.get("name")
    if isinstance(raw, str) and raw.strip():
        # MitKp -> Mit_Kp -> mit_kp；"Mit kp" 走同一条路。
        spaced = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", raw.strip())
        slug = re.sub(r"[^a-z0-9]+", "_", spaced.lower()).strip("_")
        if slug:
            return FIELD_NAME_ALIASES.get(slug, slug)
    index = entry["index"].lower()
    # 名字取不到时才退回索引加子索引 —— 至少不会与别的子索引重名。
    return f"unknown_{index[2:]}_{int(entry.get('subindex', 0))}"


def _field_name(entry: dict[str, Any]) -> str:
    key = entry["index"].lower()
    if key in STANDARD_FIELD_NAMES:
        return STANDARD_FIELD_NAMES[key]
    return _vendor_field_name(entry)


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
                "name": _field_name(entry),
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


def _declares_homing(objects: list[dict[str, Any]]) -> bool:
    """对象字典里有 0x6098 与 0x6099:1，就是厂商声明了回零模式 6。

    回零没有独立的 PDO 模块：同一套过程数据在 0x6060 写 6 就切到回零。所以不能
    只从模块名认模式，要看对象字典。这两个对象是 CiA 402 规定的回零参数入口，
    厂商把它们写进 ESI 就是可用声明 —— 没有它们，写了模式 6 也无处给方法与速度。
    """
    for entry in objects:
        if entry["index"].lower() != "0x6099":
            continue
        if any(item.get("subindex") == 1 for item in entry.get("subitems") or []):
            break
    else:
        return False
    return any(entry["index"].lower() == "0x6098" for entry in objects)


def _modes(
    modules: list[dict[str, Any]], objects: list[dict[str, Any]]
) -> dict[str, int]:
    modes: dict[str, int] = {}
    for module in modules:
        modes.update(module_mode_ids(module))
    if "homing" not in modes and _declares_homing(objects):
        modes["homing"] = STANDARD_MODE_VALUES["homing"]
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
            "supported_modes": _modes(modules, facts.get("object_dictionary") or []),
        },
        # 对象字典里能直接认出的遥测对象。策略档缺这一项时 add-device
        # 用这份起草；已有策略档仍以策略档为准。
        "slow_telemetry": _slow_telemetry_entries(facts),
        # 模块化设备由 0xF030 选模块，一套模块一套映射；非模块化设备没有模块，
        # 退回设备级声明，且必须能唯一确定 —— 见 _device_pdo_set。
        "pdo_sets": [
            {
                "assignment_style": "module_slot",
                "module_ident": module["module_ident"],
                "rx": _direction(module, "rx"),
                "tx": _direction(module, "tx"),
                # 模块自带的 CoE 启动命令：厂商在这里声明"这个模块跑哪种模式"。
                # 单模式模块带一条，多模式模块一条都没有 —— 后者模式由运行方案选。
                "startup_commands": module.get("startup_commands") or [],
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
