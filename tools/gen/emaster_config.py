#!/usr/bin/env python3
"""客户入口：从厂商 ESI 到主站能读的一套运行期配置。

三个子命令，对应拿到产品之后实际要做的三件事：

    add-device  放一份厂商 ESI，产出设备档
    init-site   给出现场参数（网卡、主机名、从站数），产出拓扑、运行方案和部署
    build       把配置源编成主站 --config-dir 要的四族扁平文件

为什么要有这条命令
    四族生成器原来只挂在构建上：加一款电机必须重新构建主站。客户拿到二进制之后
    改不了任何东西。这条命令把"生成扁平文件"从构建里拿出来，客户改完配置跑一次、
    重启主站即可，不需要重新编译 —— 生成出来的 C 与配置内容无关，所以二进制不必重编。

推不出来的字段怎么处理
    硬件事实（身份、PDO 映射、模式集合）从 ESI 推导。工程决策（选哪套 PDO、停机
    判据、换算、遥测）给保守的默认值，并在退出前逐条列出来，说明哪些还没在台架上
    确认过。**不编造设备事实**：厂商资料没给的就留空，而不是填一个像模像样的数。
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import subprocess
import sys
import unicodedata
from pathlib import Path
from typing import Any

GEN_DIR = Path(__file__).resolve().parent
REPO = GEN_DIR.parent.parent
sys.path.insert(0, str(GEN_DIR))
sys.path.insert(0, str(GEN_DIR / "device"))

import merge_device_profile  # noqa: E402
import validate_project  # noqa: E402
from derive_device_facts import derive, module_mode_ids  # noqa: E402
from esi_facts import load_esi_facts, select_device  # noqa: E402

# CiA 402 各模式必须有的过程数据字段。这是标准定的，不是这台设备定的。
# 生成运行方案时按这几条去设备档的 PDO 里核对，缺一条就说明这套映射跑不了这个模式。
# 每个模式的三件事：Rx 侧要哪些字段、Tx 侧要哪些字段、目标通道收的那个数是什么量。
# 第三项进运行方案的 `quantity`，运行期据此决定判据与断供行为 —— 不按模式号分支。
MODE_FIELDS: dict[str, tuple[tuple[str, ...], tuple[str, ...], str]] = {
    "csp": (
        ("control_word", "target_position", "mode_of_operation"),
        ("status_word", "actual_position", "mode_display"),
        "position",
    ),
    "csv": (
        ("control_word", "target_velocity", "mode_of_operation"),
        ("status_word", "actual_velocity", "mode_display"),
        "velocity",
    ),
    "cst": (
        ("control_word", "target_torque", "mode_of_operation"),
        ("status_word", "actual_torque", "mode_display"),
        "torque",
    ),
    # 回零没有周期目标：Rx 侧只要控制字与模式，Tx 侧要实际位置（参考点搜寻的结果）。
    "homing": (
        ("control_word", "mode_of_operation"),
        ("status_word", "actual_position", "mode_display"),
        "none",
    ),
    # MIT 复合控制律：驱动器按 T = Kp·(P目标−P实际) + Kd·(V前馈−V实际) + T前馈 算力矩，
    # 主站每拍送的是这五项。目标通道收的那个数是**位置**——Kp/Kd 与两个前馈是控制律的
    # 参数，不是外部设点，所以它们不在这里列，由运行方案宣布为固定值（两个增益必给：
    # 出厂值未知时主站每拍往映射里写的 0 会把驱动器自己的增益顶掉，轴就是松的）。
    "mit": (
        ("control_word", "target_position", "mit_kp", "mit_kd", "mode_of_operation"),
        ("status_word", "actual_position", "mode_display"),
        "position",
    ),
}

# 一个模块支持多种模式时先挑哪个。CSP 是位置主轴的核心模式，其余按使用频率排。
# 回零（homing）不在这张表里：它是特殊用途的一次性模式，不该被自动挑中。
# MIT 排最后：它要额外给两个增益，不是"什么都不填就能跑"的模式。
MODE_PREFERENCE = ("csp", "csv", "cst", "mit")

# ESI 里观察不到模式集合时的兜底：CiA 402 标准三件套的模式号。
# 兜底里不含 homing：它是不是支持只有设备自己知道，不能替客户猜。
# 也不含 mit：它不是 CiA 402 的标准模式，只有厂商在 ESI 里声明了才成立。
STANDARD_MODE_VALUES = {"csp": 8, "csv": 9, "cst": 10}

# 台架实测的 DC 同步参数（1 ms 周期的取值）。周期变了按比例缩放，见 _sync_block。
DC_SM2_SYNC_TYPE = "0x0002"
DC_SM3_SYNC_TYPE = "0x0022"
SYNC0_SHIFT_NUMERATOR = 4
PHASE_NUMERATOR = 20
DC_STARTUP_CYCLES = 1000

DEFAULT_CYCLE_NS = 1_000_000
DEFAULT_PRIORITY = 80


def _slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def _width(text: str) -> int:
    """终端里的显示宽度。中日韩字符占两列，按字符数补空格会对不齐。"""
    return sum(2 if unicodedata.east_asian_width(char) in "WF" else 1 for char in text)


def _print_block(title: str, rows: list[tuple[str, str]]) -> None:
    """印一块"标签 值"的表。列宽按显示宽度算。"""
    width = max(_width(label) for label, _ in rows)
    print(f"\n{title}")
    for label, text in rows:
        print(f"  {label}{' ' * (width - _width(label))}  {text}")


def _print_notes(notes: list[tuple[str, str]]) -> None:
    """还没定下来的事，编号印出来。每条都能直接照做。空就不印。"""
    if not notes:
        return
    width = max(_width(label) for label, _ in notes)
    print(f"\n需要你确认 {len(notes)} 项")
    for number, (label, text) in enumerate(notes, start=1):
        print(f"  {number:>2}  {label}{' ' * (width - _width(label))}  {text}")


def _print_next(*lines: str) -> None:
    print("\n下一步")
    for line in lines:
        print(f"  {line}")


def _write_json(path: Path, document: dict[str, Any]) -> None:
    """只写文件，不打印 —— 输出由调用方汇总成一块，免得一屏全是"已写出"。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _profile_id(identity: dict[str, Any], model: str | None) -> str:
    """设备身份三元组 → 稳定且可读的 profile_id。

    格式 <厂商>.<型号>.rev<修订号>，修订号取十六进制解析后的十进制 ——
    同一款电机的不同修订版是两个不同的 profile_id。

    每一段都必须非空。厂商名和型号整个是中文是很常见的，slug 之后就是空串 ——
    直接拼会得到 ".型号.rev1" 这种以点开头的 ID，而且两家中文名的厂商会撞在一起。
    空的段落退回厂商 ID（ESI 里必有、且唯一），型号段退回 device。
    """
    vendor_slug = _slug(identity.get("vendor_name") or "")
    if not vendor_slug:
        vendor_id = str(identity.get("vendor_id", "")).lower().replace("#x", "0x")
        vendor_slug = "vendor-" + (re.sub(r"[^a-z0-9]+", "", vendor_id) or "unknown")
    model_slug = _slug(model or "") or "device"
    revision = int(str(identity.get("revision", "0x0")), 16)
    return f"{vendor_slug}.{model_slug}.rev{revision}"


def _file_stem(profile_id: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", profile_id.lower()).strip("_")


def _resolve_esi(argument: str) -> Path:
    """ESI 可以给文件，也可以给目录；给目录时里面必须只有一份 .xml。

    **只认 .xml。** EDS（CANopen）不是 ESI，拿它进到这里，后面 `ElementTree` 会抛一个
    裸的回溯，读的人得从栈里猜出"原来是不支持这种文件"——所以要在这里拦下来说清楚。
    """
    path = Path(argument)
    if path.is_dir():
        candidates = sorted(path.glob("*.xml"))
        if len(candidates) == 1:
            return candidates[0]
        others = sorted(path.glob("*.eds"))
        if not candidates and others:
            raise SystemExit(
                f"{path} 里只有 .eds，没有 .xml。EDS 是 CANopen 的设备描述，本项目读不了；"
                "向厂商要 EtherCAT 的 ESI（XML）"
            )
        raise SystemExit(
            f"{path} 里有 {len(candidates)} 份 .xml，无法确定用哪一份；直接给出文件路径"
        )
    if not path.is_file():
        raise SystemExit(f"找不到 ESI：{path}")
    if path.suffix.lower() != ".xml":
        hint = "EDS 是 CANopen 的设备描述，本项目读不了；向厂商要 EtherCAT 的 ESI（XML）" \
            if path.suffix.lower() == ".eds" else "本项目只读 ESI（XML）"
        raise SystemExit(f"{path} 不是 ESI（后缀 {path.suffix or '无'}）。{hint}")
    return path


def _pdo_set_draft(
    facts: dict[str, Any],
    derived: dict[str, Any],
    supported_modes: dict[str, int],
    notes: list[tuple[str, str]],
) -> list[dict[str, Any]]:
    """按 ESI 里的模块（或设备级声明）起草 PDO 方案列表。

    supported_modes 是这份新策略要写的模式集合，由调用方按"ESI 观察到的、没有就按
    标准三种兜底"定好 —— 起草几条方案、给什么提示都看它。
    """
    modules = facts.get("modules") or []
    if not modules:
        # 非模块化设备只有出厂那一套映射，策略没有可挑的余地。但"一套映射"不等于
        # "一个模式"：模式由 0x6060 定，同一套映射换一个模式值就是另一条方案。
        # 所以这里也要说清楚怎么加——否则客户改 --mode 之后会撞在"没有这套 PDO
        # 方案"上，而那句话不会告诉他该改哪儿。
        modes = supported_modes
        runnable = [mode for mode in MODE_PREFERENCE if mode in modes]
        if len(runnable) > 1:
            notes.append(
                (
                    "模式",
                    "非模块化设备只有出厂那一套映射，但它能跑 "
                    + "、".join(f"{mode}={modes[mode]}" for mode in runnable)
                    + "。要跑 csp 以外的模式，在策略档的 pdo_sets 里照抄一条，"
                    "只改 id 与 mode_value，再重跑本命令。",
                )
            )
        if len(derived["pdo_sets"]) != 1:
            raise SystemExit("设备级推导给出的 PDO 方案不是一套，无法起草策略")
        return [
            {
                "id": "direct",
                "assignment_style": "direct_1c12",
                "module_ident": None,
                "module_slot": 0,
                "mode_value": STANDARD_MODE_VALUES["csp"],
            }
        ]

    draft: list[dict[str, Any]] = []
    for module in modules:
        modes = module_mode_ids(module)
        # 常用模式优先；一个模块只写得出 homing 时（纯回零通道）也要起草得出来，
        # 所以退回到它认得的全部模式里排第一个。
        usable = [mode for mode in MODE_PREFERENCE if mode in modes] or sorted(modes)
        if not usable:
            raise SystemExit(
                f"模块 {module['module_ident']}（{module.get('type')}）没写出任何"
                f"本工具认识的模式，无法起草 PDO 方案"
            )
        declared = _declared_mode_value(module)
        entry: dict[str, Any] = {
            "id": f"{module['module_ident'][2:]}-{usable[0]}",
            "module_ident": module["module_ident"],
            "module_slot": 1,
        }
        # 模块自带的 CoE 启动命令已经把模式钉死了，那是厂商的声明 —— 不抄进策略档。
        # 只有多模式模块（一条启动命令都没有）才由人在这里选跑哪种。
        if declared is None:
            entry["mode_value"] = modes[usable[0]]
        draft.append(entry)
        if len(usable) > 1:
            singled = [
                f"{mode} 用 {_dedicated_module_ident(modules, mode)}"
                for mode in usable[1:]
                if _dedicated_module_ident(modules, mode) is not None
            ]
            hint = (
                "厂商为这些模式单独声明了模块（" + "、".join(singled) + "），优先用那些；"
                if singled
                else ""
            )
            notes.append(
                (
                    "模式",
                    f"模块 {module['module_ident']} 还支持 {'、'.join(usable[1:])}。"
                    f"{hint}没有专门模块的，在策略档照抄一条、只改 mode_value，"
                    "模式仍由运行方案选。",
                )
            )
    return draft


def add_device(args: argparse.Namespace) -> int:
    config = Path(args.config)
    esi_path = _resolve_esi(args.esi)
    facts = select_device(load_esi_facts(esi_path), getattr(args, "select", None))
    derived = derive(facts)
    profile_id = _profile_id(derived["identity"], derived["model"])
    stem = _file_stem(profile_id)

    notes: list[tuple[str, str]] = []
    policy_path = config / "device_policies" / f"{stem}.json"
    if policy_path.is_file():
        policy = _read_json(policy_path)
        policy_state = "已存在，按它合成"
    else:
        observed_modes = derived["protocol"]["supported_modes"]
        if not observed_modes:
            notes.append(
                (
                    "模式",
                    "ESI 里没有模式信息，暂按 CiA 402 标准三种填了 csp、csv、cst。"
                    "台架确认这一台支持哪几种。",
                )
            )
            observed_modes = dict(STANDARD_MODE_VALUES)
        modes = _pdo_set_draft(facts, derived, observed_modes, notes)
        policy = {
            "schema_version": 1,
            "profile_id": profile_id,
            "reference_pdo_set_id": modes[0]["id"],
            "safe_stop_status_mask": "0x0000",
            "safe_stop_status_value": "0x0000",
            "supported_modes": observed_modes,
            "pdo_sets": modes,
        }
        notes.append(
            (
                "停机判据",
                "当前为空，按 CiA 402 标准状态判据停机。台架实测后填 safe_stop_status_mask；"
                "填错没人看得出来，它只影响主站自己的判定。",
            )
        )
        notes.append(
            (
                "慢速遥测",
                "未声明，报告里温度、母线电压、电流那几栏会是空的。"
                "要就照 config/README.md 补 slow_telemetry。",
            )
        )
        if not derived["protocol"]["assign_activate_by_strategy"].get("dc"):
            notes.append(
                (
                    "同步",
                    "这份 ESI 没声明 DC（缺 <Dc><OpMode>），init-site 会拒绝生成运行方案。"
                    "补 ESI，不要在这里编同步参数。",
                )
            )
        _write_json(policy_path, policy)
        policy_state = "新建，改了要重跑本命令"

    merged = merge_device_profile.build(
        derived, policy, esi_path.as_posix()
    )
    _write_json(config / "devices" / f"{stem}.json", merged)

    identity = merged["identity"]
    _print_block(
        f"设备档 {profile_id}",
        [
            (
                "身份",
                f"{identity['vendor_id']} / {identity['product_code']}"
                f" / rev {int(str(identity['revision']), 16)}",
            ),
            ("PDO", f"{len(merged['pdo_sets'])} 套"),
            ("模式", " ".join(merged["protocol"]["supported_modes"])),
            ("策略档", f"{policy_path}（{policy_state}）"),
            ("设备档", str(config / "devices" / f"{stem}.json")),
        ],
    )
    _print_notes(notes)
    _print_next(
        "build/apps/diagnose_slave/emaster-scan <网卡> > scan.txt    # 只读；环上有几台、各是什么",
        f"python3 tools/gen/emaster_config.py init-site --device {profile_id} "
        f"--interface <网卡> --from-scan scan.txt",
        "台数和逐台身份都从扫描来，不用手填 --slaves。",
    )
    return 0


def _mode_rank(mode_id: str) -> int:
    """模式在运行方案里的排列顺序：常用的按 MODE_PREFERENCE，其余（含 homing）排最后。"""
    return (
        MODE_PREFERENCE.index(mode_id)
        if mode_id in MODE_PREFERENCE
        else len(MODE_PREFERENCE)
    )


def _declared_mode_value(module: dict[str, Any]) -> int | None:
    """模块自带的 CoE 启动命令里那个模式值；模块没声明就返回 None。

    单模式模块靠这条命令把模式钉死，那是厂商的声明，不该再抄进策略档。
    """
    for command in module.get("startup_commands") or []:
        if command.get("index") == "0x6060":
            return int(command["value"])
    return None


def _dedicated_module_ident(modules: list[dict[str, Any]], mode: str) -> str | None:
    """只声明了这一个模式的模块 —— 厂商为它单独留的那一份映射。"""
    for module in modules:
        if list(module_mode_ids(module)) == [mode]:
            return str(module["module_ident"])
    return None


def _mode_fields(pdo_set: dict[str, Any], direction: str) -> set[str]:
    names: set[str] = set()
    for mapping in pdo_set.get(direction, {}).get("mappings", []):
        for entry in mapping.get("entries", []):
            names.add(entry["name"])
    return names


def _sync_block(device: dict[str, Any], cycle_ns: int) -> dict[str, Any]:
    """DC 同步参数。相位与 Sync0 偏移按周期成比例，周期改了不必重测。

    AssignActivate 不在这里猜：它是厂商在 ESI 里写的（设备档的
    assign_activate_by_strategy["dc"]）。厂商没写 dc 这一种，就说明这台设备
    的 ESI 没声明 DC 支持 —— 编一个"标准上一般是这样"的值填上，会安静地把
    从站的同步配成另一套，主机这边只看到 DC 对不上，查起来毫无头绪。
    """
    assign = (device.get("protocol") or {}).get("assign_activate_by_strategy") or {}
    dc_assign = assign.get("dc")
    if dc_assign is None:
        raise SystemExit(
            f"设备 {device['profile_id']} 的 ESI 没有声明 DC 同步（设备档里 "
            "assign_activate_by_strategy 没有 dc 一项），而 init-site 只会生成 DC 方案，"
            "生成了也过不了校验。\n"
            "该做的是把 ESI 补全，不是在这里编同步参数：\n"
            "  · 设备实际支持 DC：把 ESI 里 <Dc><OpMode><Name>DC</Name> 那一段补上，"
            "重新跑 add-device 推导设备档；\n"
            "  · 设备不支持 DC：同步参数（AssignActivate、sm2/sm3_sync_type、相位与偏移）"
            "只能按台架实测写，请照 config/README.md『运行方案』一节手写一份。"
        )
    return {
        "strategy": "dc",
        "assign_activate": dc_assign,
        "cycle_ns": cycle_ns,
        "process_data_phase_ns": cycle_ns // PHASE_NUMERATOR,
        "dc_startup_cycles": DC_STARTUP_CYCLES,
        "sync0_shift_ns": cycle_ns // SYNC0_SHIFT_NUMERATOR,
        "sm2_sync_type": DC_SM2_SYNC_TYPE,
        "sm3_sync_type": DC_SM3_SYNC_TYPE,
    }


def _mit_gain_writes(device: dict[str, Any]) -> list[dict[str, Any]]:
    """MIT 的两个增益，从设备档的策略段读出来，写成方案里的固定值声明。

    为什么必须由策略给、不能由工具编一个默认值：这两个增益在 RxPDO 里，主站每拍
    写进去的就是驱动器会用的值；工具编一个数，客户拿到的就是一份"看着能跑、刚度
    是猜的"的配置。为什么不能不给：不给就是每拍写 0 —— 没有刚度也没有阻尼，轴是
    松的（比拒开更危险）。所以缺了就当场拒绝，并说清该往哪写。
    """
    gains = device.get("mit_gains")
    values: dict[str, int] = {}
    for key, label in (("kp", "Kp"), ("kd", "Kd")):
        value = gains.get(key) if isinstance(gains, dict) else None
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise SystemExit(
                f"设备 {device['profile_id']} 声明支持 MIT 模式，但设备档里没有可用的 "
                f"mit_gains.{key}（{label}）。\n"
                "这两个增益由主站逐拍写进过程数据，值是驱动器实际用的值，必须由人给：\n"
                "  · 在 config/device_policies/<这份档对应的策略>.json 里加一段\n"
                '    "mit_gains": {"kp": <非负整数>, "kd": <非负整数>}，\n'
                "    再重跑 add-device 合成设备档；\n"
                "  · 取值照台架实测（先在 PRE-OP 只读这两个对象看驱动器里是多少），\n"
                "    不要照 CAN 协议那两个 mit_max_* 上限值 —— 那是对 12 位定点数的量程，\n"
                "    不是这台设备 EtherCAT 侧该用的增益。"
            )
        values[key] = value
    # 0x2013 是厂商的 MIT 参数组（ESI 里的 "Mit Parameters"）：1 号 Kp、2 号 Kd。
    return [
        {"index": "0x2013", "subindex": 1, "type": "i32", "value": values["kp"]},
        {"index": "0x2013", "subindex": 2, "type": "i32", "value": values["kd"]},
    ]


def _modes_block(
    device: dict[str, Any], pdo_set: dict[str, Any], wanted: str
) -> list[dict[str, Any]]:
    """这套 PDO 方案能表达的模式，按 CiA 402 要求的字段逐个核对。"""
    supported = device["protocol"]["supported_modes"]
    rx_names = _mode_fields(pdo_set, "rx")
    tx_names = _mode_fields(pdo_set, "tx")
    modes: list[dict[str, Any]] = []
    for mode_id, value in sorted(supported.items(), key=lambda item: _mode_rank(item[0])):
        layout = MODE_FIELDS.get(mode_id)
        if layout is None:
            continue
        rx_fields, tx_fields, quantity = layout
        missing = [name for name in rx_fields + tx_fields if name not in rx_names | tx_names]
        if missing:
            continue
        mode: dict[str, Any] = {
            "id": mode_id,
            "value": value,
            "quantity": quantity,
            "required_rx_fields": list(rx_fields),
            "required_tx_fields": list(tx_fields),
            "mode_display_policy": "required",
        }
        if mode_id == "mit":
            mode["safeop_to_op_sdo_writes"] = _mit_gain_writes(device)
        modes.append(mode)
    if not any(mode["id"] == wanted for mode in modes):
        raise SystemExit(
            f"PDO 方案 {pdo_set['id']} 表达不了模式 {wanted}："
            "缺字段，或者这个模式的字段布局本工具不认识（只认 "
            f"{'、'.join(sorted(MODE_FIELDS))}）"
        )
    return modes


def _find_device(config: Path, profile_id: str) -> dict[str, Any]:
    for path in sorted((config / "devices").glob("*.json")):
        document = _read_json(path)
        if document.get("profile_id") == profile_id:
            return document
    raise SystemExit(f"{config / 'devices'} 里没有 profile_id 为 {profile_id} 的设备档")


def _pdo_set_mode_value(pdo_set: dict[str, Any]) -> int | None:
    """这套方案进 OP 前把 0x6060 写成几。没声明就是不固定模式。"""
    initialization = pdo_set.get("mode_initialization")
    if isinstance(initialization, dict):
        return initialization.get("value")
    return None


def _pdo_set_expresses(device: dict[str, Any], pdo_set: dict[str, Any], mode: str) -> bool:
    """这套方案的字段能不能表达这个模式。判据与 _modes_block 用的是同一张表。"""
    layout = MODE_FIELDS.get(mode)
    if layout is None or mode not in device["protocol"]["supported_modes"]:
        return False
    rx_fields, tx_fields, _quantity = layout
    names = _mode_fields(pdo_set, "rx") | _mode_fields(pdo_set, "tx")
    return all(name in names for name in rx_fields + tx_fields)


def _pick_pdo_set(
    device: dict[str, Any], wanted_id: str | None, wanted_mode: str
) -> dict[str, Any]:
    """挑出这份运行方案要用的 PDO 方案。

    一台设备可以给同一套映射写多条方案，彼此只差 mode_value —— 同一套映射换一个
    模式值就是另一个模式。所以"跑 CSV 该用哪条"不能只看参考方案：先看它表达不表达
    得了这个模式，再看哪条的 mode_value 与模式号对得上。挑不出来时退回参考方案，
    由 _modes_block 报"这套方案表达不了这个模式"，那句话里已经说了缺什么。
    """
    if wanted_id is not None:
        for pdo_set in device["pdo_sets"]:
            if pdo_set["id"] == wanted_id:
                return pdo_set
        raise SystemExit(
            f"设备 {device['profile_id']} 没有 PDO 方案 {wanted_id}；"
            f"可选：{', '.join(item['id'] for item in device['pdo_sets'])}"
        )

    reference = next(
        (item for item in device["pdo_sets"] if item["id"] == device["reference_pdo_set_id"]),
        None,
    )
    if reference is None:
        raise SystemExit(
            f"设备 {device['profile_id']} 的参考 PDO 方案 "
            f"{device['reference_pdo_set_id']} 不在 pdo_sets 里"
        )
    candidates = [
        item for item in device["pdo_sets"] if _pdo_set_expresses(device, item, wanted_mode)
    ]
    # 固定模式值对得上的那条最准：字段能表达只是必要条件，模式值才是"进 OP 之后
    # 驱动器收到的是哪个模式"。参考方案排在其后。
    wanted_value = device["protocol"]["supported_modes"].get(wanted_mode)
    for item in candidates:
        if _pdo_set_mode_value(item) == wanted_value:
            return item
    if reference in candidates:
        return reference
    return candidates[0] if candidates else reference


def _check_pdo_set_mode(device: dict[str, Any], pdo_set: dict[str, Any], mode: str) -> None:
    """方案固定的模式值必须就是这次要跑的模式。

    落在这里而不是留到 build：两条命令之间夹着一次重跑 add-device，而 build 的
    报错只说"模式值与所选 PDO 模块初始化值不一致"，不告诉人改哪儿。
    """
    declared = _pdo_set_mode_value(pdo_set)
    modes = device["protocol"]["supported_modes"]
    if declared is None or mode not in modes or declared == modes[mode]:
        return
    raise SystemExit(
        f"PDO 方案 {pdo_set['id']} 声明进 OP 前把模式写成 {declared}，"
        f"而 --mode {mode} 是 {modes[mode]}。\n"
        f"要在同一套映射上跑 {mode}：在设备策略档的 pdo_sets 里照抄一条 "
        f"{pdo_set['id']}，改 id 与 mode_value 为 {modes[mode]}，重跑 add-device，"
        f"再带上 --pdo-set <新 id> 重跑本命令。"
    )


def _refuse_if_exists(paths: list[Path], force: bool) -> None:
    existing = [str(path) for path in paths if path.exists()]
    if existing and not force:
        raise SystemExit(
            "这些文件已存在，不覆盖（确要重写加 --force）：\n  " + "\n  ".join(existing)
        )


def _cycle_text(cycle_ns: int) -> str:
    return f"{cycle_ns // 1_000_000}ms" if cycle_ns % 1_000_000 == 0 else f"{cycle_ns}ns"


def _operation_id(device: dict[str, Any], mode: str, cycle_ns: int) -> str:
    return f"{_slug(device['model'])}.dc-{mode}-{_cycle_text(cycle_ns)}"


def _read_scan(path: Path) -> tuple[int, list[dict[str, str]]]:
    """读 emaster-scan 的输出，返回台数与逐台身份。

    输出是 key=value 的纯文本：一条 `slaves=N`，之后每台一条 `slave=…` 行。
    只取本命令用得上的一部分，其余原样不管 —— 扫描输出还会加字段，那不该
    让这里失效。
    """
    if not path.is_file():
        raise SystemExit(f"找不到扫描输出 {path}\n先跑：emaster-scan <网卡> > {path}")
    line = re.compile(
        r"slave=(?P<position>\d+) name=(?P<name>.*?)"
        r" vendor=0x(?P<vendor>[0-9A-Fa-f]+)"
        r" product=0x(?P<product>[0-9A-Fa-f]+)"
        r" revision=0x(?P<revision>[0-9A-Fa-f]+)"
    )
    total: int | None = None
    scanned: list[dict[str, str]] = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        raw = raw.strip()
        if raw.startswith("slaves="):
            total = int(raw.split("=", 1)[1])
            continue
        match = line.match(raw)
        if match:
            scanned.append(match.groupdict())
    if total is None or not scanned:
        raise SystemExit(
            f"{path} 不像 emaster-scan 的输出：没有 slaves= 行，或者没有 slave= 行。\n"
            f"先跑：emaster-scan <网卡> > {path}"
        )
    if total != len(scanned):
        raise SystemExit(
            f"{path} 自相矛盾：slaves={total}，逐台行却只有 {len(scanned)} 条"
        )
    return total, scanned


def _check_scan_identity(device: dict[str, Any], scanned: list[dict[str, str]]) -> None:
    """拿扫到的逐台身份跟设备档对。对不上要点名到台。

    只报"身份不一致"等于没说：环上有几台、哪一台不对、差在型号还是版本，
    都是现场要的。这里在读配置的时候就把话说清楚。
    """
    expected = device["identity"]
    wanted = (
        int(str(expected["vendor_id"]), 16),
        int(str(expected["product_code"]), 16),
        int(str(expected["revision"]), 16),
    )
    for entry in scanned:
        got = (
            int(entry["vendor"], 16),
            int(entry["product"], 16),
            int(entry["revision"], 16),
        )
        if got != wanted:
            raise SystemExit(
                f"第 {entry['position']} 台不是 {device['profile_id']}。\n"
                f"  总线上：{entry['name']} "
                f"0x{got[0]:08X} / 0x{got[1]:08X} / rev {got[2]}\n"
                f"  设备档：0x{wanted[0]:08X} / 0x{wanted[1]:08X} / rev {wanted[2]}\n"
                f"换电机就重新跑 add-device，接错线就查线。"
            )


def init_site(args: argparse.Namespace) -> int:
    config = Path(args.config)
    device = _find_device(config, args.device)
    pdo_set = _pick_pdo_set(device, args.pdo_set, args.mode)
    _check_pdo_set_mode(device, pdo_set, args.mode)
    modes = _modes_block(device, pdo_set, args.mode)

    host = args.hostname or socket.gethostname()
    host_slug = _slug(host)
    if args.from_scan:
        scan_path = Path(args.from_scan)
        axis_count, scanned = _read_scan(scan_path)
        _check_scan_identity(device, scanned)
        axis_note = (
            "轴数",
            f"{axis_count} 台，来自扫描 {scan_path}，逐台身份已核对。"
            f"环序到物理关节的对应仍未确认。",
        )
    else:
        axis_count = args.slaves
        axis_note = (
            "轴数",
            f"按 --slaves {axis_count} 填的，没跟实物比过。"
            f"接上真机跑 emaster-scan 核对；环序到物理关节的对应也仍未确认。",
        )
    topology_id = f"{host_slug}-{axis_count}-axis"
    operation_id = _operation_id(device, args.mode, args.cycle_ns)
    deployment_id = f"{topology_id}-idle"

    topology_path = config / "topologies" / f"{topology_id}.json"
    operation_path = config / "operation_profiles" / f"{operation_id}.json"
    deployment_path = config / "deployments" / f"{deployment_id}.json"
    _refuse_if_exists([topology_path, operation_path, deployment_path], args.force)

    # 挂运动方案这件事必须给人说清楚，因为它挂不上时是安静的：方案照旧进
    # runtime.flat，部署那一行写 motion_profile_id_present=false，主站照常起、
    # 照常空转，只是那份方案永远不生效。所以这里两件事都做——建目录，让
    # "写每轴换算与限幅"那一步有地方落笔；挂不上就当场拒绝，而不是留一份
    # 引用了不存在 ID 的部署等 build 去炸。
    (config / "motion_profiles").mkdir(parents=True, exist_ok=True)
    if args.motion is not None and not (
        config / "motion_profiles" / f"{args.motion}.json"
    ).is_file():
        raise SystemExit(
            f"没有 config/motion_profiles/{args.motion}.json，部署挂不到它上面。\n"
            f"先写这份运动方案，再带上 --motion {args.motion} --force 重跑本命令：\n"
            f"  · 每轴的 axis_id 见 config/topologies/{topology_id}.json；\n"
            f"  · required_mode_id 必须是 {args.mode}（运行方案选的就是它）；\n"
            f"  · 轴集合必须覆盖拓扑里的全部轴。\n"
            "字段语义见 config/README.md『运行方案』一节。"
        )

    topology = {
        "schema_version": 1,
        "topology_id": topology_id,
        "status": "provisional",
        "slaves": [
            {
                "position": position,
                "axis_id": f"axis_{position:02d}",
                "joint_assignment": None,
                "profile_id": device["profile_id"],
            }
            for position in range(1, axis_count + 1)
        ],
    }
    operation = {
        "schema_version": 1,
        "operation_profile_id": operation_id,
        "status": "approved",
        "device_profile_id": device["profile_id"],
        "pdo_set_id": pdo_set["id"],
        "selected_mode_id": args.mode,
        "sync": _sync_block(device, args.cycle_ns),
        "modes": modes,
    }
    deployment = {
        "schema_version": 1,
        "deployment_id": deployment_id,
        "hostname": host,
        "topology_id": topology_id,
        "ethercat_interface": args.interface,
        "management_interface": None,
        "operation_profile_ids": [operation_id],
        "motion_profile_id": args.motion,
        "run_report_path": f"runtime/reports/{deployment_id}-latest.json",
        "error_recovery_policy_id": "default",
        "external_target_timeout_ms": 200,
        "realtime": {
            "scheduler_priority": DEFAULT_PRIORITY,
            "cpu_core": None,
            "required": False,
        },
    }
    _write_json(topology_path, topology)
    _write_json(operation_path, operation)
    _write_json(deployment_path, deployment)

    _print_block(
        "已生成",
        [
            ("拓扑", f"{topology_id}     {axis_count} 轴，状态 provisional"),
            (
                "运行方案",
                f"{operation_id}     {args.mode} / {_cycle_text(args.cycle_ns)} / DC",
            ),
            ("部署", f"{deployment_id}     {args.interface}"),
            (
                "运动方案",
                f"{args.motion}     挂在部署上，主站按它跑固定轨迹"
                if args.motion is not None
                else "未挂 —— 只空转；要动就由客户端提交外部目标",
            ),
        ],
    )
    notes = [
        axis_note,
        (
            "诊断项",
            "运行方案里没有 safeop_to_op_sdo_writes 和 final_sdo_reads，"
            "那两张表要按型号在台架挑。",
        ),
        (
            "实时",
            "只给了调度优先级，没绑核。要绑核补 realtime.cpu_core，"
            "计时类实验还要把 realtime.required 改成 true。",
        ),
    ]
    if any(mode["id"] == args.mode and mode["quantity"] == "torque" for mode in modes):
        # 力矩模式没有位置与速度保护，方案里那一档力矩上限就是唯一的限幅。
        # 生成器不替客户编一个数：额定力矩是型号的事，编一个像模像样的值比留空更危险。
        notes.append(
            (
                "力矩上限",
                f"{args.mode} 不做位置与速度保护，方案里要写明力矩上限：在 "
                f"{operation_path.name} 的 modes 里给 {args.mode} 加一条 "
                "safeop_to_op_sdo_writes，对象 0x6072，取驱动器额定力矩里的一档。",
            )
        )
    _print_notes(notes)
    _print_next(
        "python3 tools/gen/emaster_config.py build",
        "生成主站读的四族扁平文件。",
    )
    return 0


def build(args: argparse.Namespace) -> int:
    """校验配置源，再产出四族扁平文件。"""
    config = Path(args.config).resolve()
    out = Path(args.out).resolve()
    errors, warnings = validate_project.run(config)
    for warning in warnings:
        print(f"告警：{warning}")
    if errors:
        for error in errors:
            print(f"错误：{error}", file=sys.stderr)
        print("配置源不合规，没有产出任何文件。", file=sys.stderr)
        return 1
    out.mkdir(parents=True, exist_ok=True)

    steps = [
        (
            "tools/gen/generate_config_apply.py",
            ["--topologies", config / "topologies"],
            "topologies.flat",
        ),
        (
            "tools/gen/generate_catalog_apply.py",
            ["--devices", config / "devices"],
            "catalog.flat",
        ),
        (
            "tools/gen/generate_runtime_apply.py",
            ["--operations", config / "operation_profiles",
             "--motions", config / "motion_profiles"],
            "runtime.flat",
        ),
        (
            "tools/gen/generate_deployment_apply.py",
            ["--deployments", config / "deployments",
             "--topologies", config / "topologies",
             "--operations", config / "operation_profiles",
             "--motions", config / "motion_profiles"],
            "deployments.flat",
        ),
    ]
    for script, inputs, flat_name in steps:
        command = [
            sys.executable,
            str(REPO / script),
            *[str(item) for item in inputs],
            "--flat-output",
            str(out / flat_name),
        ]
        # 生成器输出的是 UTF-8 中文。Windows 上 subprocess 默认按本地代码页解码，
        # 会把这一层变成 UnicodeDecodeError —— 明明生成成功却报解码失败。
        done = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=REPO,
        )
        if done.returncode != 0:
            sys.stderr.write(done.stdout)
            sys.stderr.write(done.stderr)
            print(f"{script} 失败，退出码 {done.returncode}", file=sys.stderr)
            return done.returncode
    _print_block("已写出", [(name, str(out / name)) for _, _, name in steps])
    _print_next(
        f"sudo bash -c 'ulimit -r unlimited; exec env EMASTER_OBSERVATION=1 "
        f"./build/apps/master/emaster-master --config-dir {out}'",
        "起主站。同机有多份部署档时再加 --deployment <部署 ID>。",
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="从 ESI 生成主站运行期配置")
    sub = parser.add_subparsers(dest="command", required=True)

    add = sub.add_parser("add-device", help="厂商 ESI → 设备档（策略档照样给出）")
    add.add_argument("esi", help="ESI 文件（XML），或只装一份 .xml 的目录")
    add.add_argument("--config", default="config", help="配置源目录，缺省 config")
    add.add_argument(
        "--select",
        help="ESI 里有多台设备时挑一台：清单里的序号（从 1 起），或 厂商ID:产品码:版本",
    )
    add.set_defaults(handler=add_device)

    site = sub.add_parser("init-site", help="现场参数 → 拓扑、运行方案和部署")
    site.add_argument("--device", required=True, help="设备档的 profile_id")
    site.add_argument("--interface", required=True, help="只跑 EtherCAT 的物理网卡")
    site.add_argument("--hostname", help="运行主站的主机名，缺省取本机")
    site.add_argument("--slaves", type=int, default=1, help="从站数，缺省 1")
    site.add_argument(
        "--from-scan",
        help="emaster-scan 的输出文件；给了就用它的台数和身份，不用 --slaves",
    )
    site.add_argument("--cycle-ns", type=int, default=DEFAULT_CYCLE_NS, help="周期，缺省 1 ms")
    site.add_argument("--mode", default="csp", help="运行模式，缺省 csp")
    site.add_argument("--pdo-set", help="用哪套 PDO 方案，缺省设备档的参照方案")
    site.add_argument(
        "--motion",
        help="部署要跑的那份运动方案的 ID（config/motion_profiles/<ID>.json）；"
        "不给就只有空转或外部目标",
    )
    site.add_argument("--force", action="store_true", help="已存在时覆盖")
    site.add_argument("--config", default="config", help="配置源目录，缺省 config")
    site.set_defaults(handler=init_site)

    builder = sub.add_parser("build", help="配置源 → 四族扁平文件")
    builder.add_argument("--config", default="config", help="配置源目录，缺省 config")
    builder.add_argument(
        "--out", default="config-runtime", help="扁平文件输出目录，缺省 config-runtime"
    )
    builder.set_defaults(handler=build)

    args = parser.parse_args()
    if getattr(args, "slaves", 1) < 1:
        raise SystemExit("--slaves 至少要 1")
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
