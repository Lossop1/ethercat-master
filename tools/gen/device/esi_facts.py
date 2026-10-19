"""提取 ESI 中可审查的设备事实，不把事实转换成运行方案。"""

from __future__ import annotations

import hashlib
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

# validation_common 是校验与设备档两条链共用的解析基础，放在上一层 tools/gen/。
# 本文件被 derive_device_facts.py 以模块方式导入，所以这里自己保证可导入。
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from validation_common import EsiRuntimeConstraints, hex_value  # noqa: E402


def _integer_text(value: str, field: str) -> int:
    """解析 ESI 中可能使用十进制或 #x/0x 十六进制表示的整数。"""
    text = value.strip()
    try:
        if text.lower().startswith(("#x", "0x")):
            return hex_value(text)
        return int(text, 10)
    except ValueError as error:
        raise ValueError(f"ESI 字段 {field} 不是有效整数：{value!r}") from error


def _hex_text(value: int, width: int) -> str:
    """将 ESI 数值格式化为稳定的十六进制字符串，便于审查和比较。"""
    return f"0x{value:0{width}X}"


def _optional_text(element: ET.Element | None, path: str) -> str | None:
    """读取可选文本并去除首尾空白；缺失或空文本保留为 null。"""
    if element is None:
        return None
    value = element.findtext(path)
    if value is None or not value.strip():
        return None
    return value.strip()


def _bool_attribute(element: ET.Element | None, name: str, default: bool = False) -> bool:
    """严格解析 ESI 布尔属性；未声明时使用调用者给出的保守值。"""
    if element is None:
        return default
    value = element.attrib.get(name)
    if value is None:
        return default
    normalized = value.strip().lower()
    if normalized in ("1", "true", "yes"):
        return True
    if normalized in ("0", "false", "no"):
        return False
    raise ValueError(f"ESI 属性 {element.tag}@{name} 不是有效布尔值：{value!r}")


def little_endian_default_data(value: str) -> int:
    """把 ESI DefaultData 的字节序列解释为 EtherCAT 小端无符号整数。"""
    try:
        return int.from_bytes(bytes.fromhex(value), byteorder="little", signed=False)
    except ValueError as error:
        raise ValueError(f"ESI DefaultData 不是有效字节序列：{value!r}") from error


# 出厂默认 PDO 分配由这两个对象管：0x1C12 管 RxPdo（输出），0x1C13 管 TxPdo（输入）。
PDO_ASSIGNMENT_OBJECTS: dict[str, str] = {"#x1c12": "rx", "#x1c13": "tx"}


def _default_pdo_assignment(device: ET.Element) -> dict[str, list[str]]:
    """读对象字典里设备出厂就接好的 PDO 分配。

    子项 0 是条目数 N，其后 N 个子项的 DefaultData 是小端 u16 的 PDO 索引。
    空列表表示这份 ESI 没写默认分配 —— 模块化设备就是这样，由 0xF030 在运行期
    选模块，而不是靠默认分配。两种情况的区别由调用者按"有没有模块"判断。
    """
    assignment: dict[str, list[str]] = {"rx": [], "tx": []}
    for object_config in device.findall("Profile/Dictionary/Objects/Object"):
        index_text = object_config.findtext("Index")
        if index_text is None:
            continue
        direction = PDO_ASSIGNMENT_OBJECTS.get(index_text.strip().lower())
        if direction is None:
            continue
        defaults = [
            little_endian_default_data(value.strip())
            for value in (
                subitem.findtext("./Info/DefaultData")
                for subitem in object_config.findall("./Info/SubItem")
            )
            if value is not None and value.strip()
        ]
        if not defaults:
            continue
        count = defaults[0]
        if count + 1 > len(defaults):
            raise ValueError(
                f"ESI {index_text} 声明了 {count} 项默认分配，"
                f"却只有 {len(defaults) - 1} 个带默认值的子项"
            )
        assignment[direction] = [_hex_text(value, 4) for value in defaults[1 : count + 1]]
    return assignment


def _device_runtime_constraints(device: ET.Element) -> EsiRuntimeConstraints:
    """提取一个 Device 明确声明的 PDO 配置能力、同步模式和最小周期。"""
    coe = device.find("Mailbox/CoE")
    supports_pdo_assignment = (
        coe is not None and coe.attrib.get("PdoAssign", "false").lower() == "true"
    )
    supports_pdo_configuration = (
        coe is not None and coe.attrib.get("PdoConfig", "false").lower() == "true"
    )
    dc = device.find("Dc")
    assign_activate_by_strategy: dict[str, int] = {}
    if dc is not None:
        for operation_mode in dc.findall("OpMode"):
            name = operation_mode.findtext("Name", default="").strip().lower()
            description = operation_mode.findtext("Desc", default="").strip().lower()
            assign_text = operation_mode.findtext("AssignActivate")
            if assign_text is None:
                continue
            strategy = None
            if name == "dc" or description.startswith("dc-"):
                strategy = "dc"
            elif name == "synchron" or description.startswith("sm-"):
                strategy = "sm"
            if strategy is not None:
                assign_activate_by_strategy[strategy] = hex_value(assign_text)

    default_sync_type_by_sm: dict[int, int] = {}
    minimum_cycles: list[int] = []
    for object_config in device.findall("Profile/Dictionary/Objects/Object"):
        object_index = object_config.findtext("Index")
        if object_index is None:
            continue
        object_index_value = hex_value(object_index)
        if object_index_value not in (0x1C32, 0x1C33):
            continue
        sm_number = 2 if object_index_value == 0x1C32 else 3
        for subitem in object_config.findall("./Info/SubItem"):
            item_name = subitem.findtext("Name")
            default_data = subitem.findtext("./Info/DefaultData")
            if item_name == "Synchronization Type" and default_data:
                default_sync_type_by_sm[sm_number] = little_endian_default_data(default_data)
            elif item_name == "Minimum Cycle Time" and default_data:
                minimum_cycles.append(little_endian_default_data(default_data))

    return EsiRuntimeConstraints(
        assign_activate_by_strategy=assign_activate_by_strategy,
        default_sync_type_by_sm=default_sync_type_by_sm,
        minimum_cycle_ns=max(minimum_cycles) if minimum_cycles else None,
        supports_pdo_assignment=supports_pdo_assignment,
        supports_pdo_configuration=supports_pdo_configuration,
        supports_distributed_clocks="dc" in assign_activate_by_strategy,
    )


def _access(element: ET.Element) -> str | None:
    """读写权限在 ESI 里有两处写法：对象直接挂 Flags，某些工具放进 Info/Flags。"""
    return _optional_text(element, "Flags/Access") or _optional_text(
        element, "Info/Flags/Access"
    )


def _data_type_facts(root: ET.Element) -> dict[str, dict[str, Any]]:
    """读出 ESI 声明的复合数据类型（如 DT200B），供对象字典解析子索引。

    厂商对象常写成"一个对象 + 一份 DataType"：0x200B Monitor Parameters 自身
    只有一个 1152 位的类型名，真正能读的是 DT200B 里 :1 MOSFET 温度、
    :2 电机温度这些子索引。没有这一步，对象字典里就只剩一个不可直接读的整体。
    """
    types: dict[str, dict[str, Any]] = {}
    for data_type in root.findall(".//DataTypes/DataType"):
        name = _optional_text(data_type, "Name")
        if name is None:
            continue
        subitems: list[dict[str, Any]] = []
        for subitem in data_type.findall("SubItem"):
            subindex_text = subitem.findtext("SubIdx")
            if subindex_text is None:
                continue
            subitems.append(
                {
                    "subindex": _integer_text(subindex_text, "DataType/SubItem/SubIdx"),
                    "name": _optional_text(subitem, "Name"),
                    "type": _optional_text(subitem, "Type"),
                    "bits": _integer_text(
                        subitem.findtext("BitSize", default="0"),
                        "DataType/SubItem/BitSize",
                    ),
                    "access": _access(subitem),
                }
            )
        types[name] = {
            "bits": _integer_text(
                data_type.findtext("BitSize", default="0"), "DataType/BitSize"
            ),
            "subitems": subitems,
        }
    return types


def _object_dictionary(
    device: ET.Element, data_types: dict[str, dict[str, Any]]
) -> list[dict[str, Any]]:
    """设备对象字典的只读事实：索引、名称、类型、位宽、权限、可读子索引。

    这是生成器判断"这台设备能读哪些量"的唯一依据。ESI 也是厂商资料，
    名字和索引由厂商写下；我们只照抄，不按型号写死对象号。
    """
    objects: list[dict[str, Any]] = []
    for object_config in device.findall("Profile/Dictionary/Objects/Object"):
        index_text = object_config.findtext("Index")
        if index_text is None:
            continue
        type_name = _optional_text(object_config, "Type")
        resolved = data_types.get(type_name) if type_name is not None else None
        subitems = list(resolved["subitems"]) if resolved is not None else []
        if not subitems:
            # 少数 ESI 把子索引直接写在对象里。这种写法通常不带 SubIdx，
            # 顺序就是子索引，0 号是条目数。
            for ordinal, subitem in enumerate(object_config.findall("./Info/SubItem")):
                subitems.append(
                    {
                        "subindex": ordinal,
                        "name": _optional_text(subitem, "Name"),
                        "type": _optional_text(subitem, "Type"),
                        "bits": _integer_text(
                            subitem.findtext("BitSize", default="0"),
                            "Object/Info/SubItem/BitSize",
                        ),
                        "access": _access(subitem),
                    }
                )
        objects.append(
            {
                "index": _hex_text(_integer_text(index_text, "Object/Index"), 4),
                "name": _optional_text(object_config, "Name"),
                "type": type_name,
                "bits": _integer_text(
                    object_config.findtext("BitSize", default="0"), "Object/BitSize"
                ),
                "access": _access(object_config),
                "subitems": subitems,
            }
        )
    return objects


def _entry_fact(entry: ET.Element) -> dict[str, Any]:
    """提取单个 PDO 条目的原始描述，不为缺失名称臆造稳定字段名。"""
    index_text = entry.findtext("Index", default="0")
    object_index = _integer_text(index_text, "Entry/Index")
    subindex = _integer_text(entry.findtext("SubIndex", default="0"), "Entry/SubIndex")
    bits = _integer_text(entry.findtext("BitLen", default="0"), "Entry/BitLen")
    data_type = _optional_text(entry, "DataType")
    if object_index == 0:
        data_type = "PADDING"
    return {
        "index": _hex_text(object_index, 4),
        "subindex": subindex,
        "bits": bits,
        "name": _optional_text(entry, "Name"),
        "comment": _optional_text(entry, "Comment"),
        "data_type": data_type,
        "depends_on_slot": _bool_attribute(entry, "DependOnSlot"),
    }


def _pdo_fact(pdo: ET.Element) -> dict[str, Any]:
    """提取一个 PDO 的索引、同步管理器、固定性和有序条目。"""
    index = _integer_text(pdo.findtext("Index", default="0"), "PDO/Index")
    entries = [_entry_fact(entry) for entry in pdo.findall("Entry")]
    bit_length = sum(int(entry["bits"]) for entry in entries)
    return {
        "index": _hex_text(index, 4),
        "name": _optional_text(pdo, "Name"),
        "fixed": _bool_attribute(pdo, "Fixed"),
        "sync_manager": (
            _integer_text(pdo.attrib["Sm"], "PDO/Sm") if "Sm" in pdo.attrib else None
        ),
        "bit_length": bit_length,
        "byte_length": bit_length // 8 if bit_length % 8 == 0 else None,
        "entries": entries,
    }


def _module_startup_commands(module: ET.Element) -> list[dict[str, Any]]:
    """模块自带的 CoE 启动命令：在某次状态过渡上把某个对象写成某个值。

    ESI 里就是 <Mailbox><CoE><InitCmd>。厂商用它把"这个模块跑哪种模式"钉死：
    单模式模块带着一条 6060 的启动命令，多模式模块一条都没有。这是模块自己的
    声明，不是我们的选择，所以按原样取出来交给下游。
    """
    commands: list[dict[str, Any]] = []
    for command in module.findall("Mailbox/CoE/InitCmd"):
        index = _optional_text(command, "Index")
        data = _optional_text(command, "Data")
        if index is None or data is None:
            continue
        subindex = _optional_text(command, "SubIndex")
        commands.append(
            {
                "transition": _optional_text(command, "Transition"),
                "index": _hex_text(_integer_text(index, "InitCmd/Index"), 4),
                "subindex": int(subindex, 0) if subindex is not None else 0,
                # Data 是十六进制字节串。启动命令里我们只用到单字节的模式值。
                "data": data,
                "value": int(data, 16),
            }
        )
    return commands


def _module_facts(root: ET.Element) -> list[dict[str, Any]]:
    """提取 ESI 中全部模块，保留声明顺序和每个模块的完整 PDO 选项。"""
    modules: list[dict[str, Any]] = []
    for module in root.findall("./Descriptions/Modules/Module"):
        module_type = module.find("Type")
        if module_type is None or "ModuleIdent" not in module_type.attrib:
            continue
        module_ident = _integer_text(module_type.attrib["ModuleIdent"], "Module/Type@ModuleIdent")
        modules.append(
            {
                "module_ident": _hex_text(module_ident, 8),
                "type": (module_type.text or "").strip() or None,
                "name": _optional_text(module, "Name"),
                "profile_numbers": [
                    _integer_text(value.text or "0", "Module/Profile/ProfileNo")
                    for value in module.findall("Profile/ProfileNo")
                ],
                "rx": [_pdo_fact(pdo) for pdo in module.findall("RxPdo")],
                "tx": [_pdo_fact(pdo) for pdo in module.findall("TxPdo")],
                "startup_commands": _module_startup_commands(module),
            }
        )
    return modules


def _device_facts(
    vendor: ET.Element,
    device: ET.Element,
    data_types: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """提取一个 Device 的身份、协议能力、同步约束、对象字典和直接 PDO 声明。"""
    device_type = device.find("Type")
    if device_type is None:
        raise ValueError("ESI Device 缺少 Type")
    constraints = _device_runtime_constraints(device)
    coe = device.find("Mailbox/CoE")
    return {
        "device": {
            "vendor_name": _optional_text(vendor, "Name"),
            "vendor_id": _hex_text(
                _integer_text(vendor.findtext("Id", default="0"), "Vendor/Id"), 8
            ),
            "model": (device_type.text or "").strip() or None,
            "device_name": _optional_text(device, "Name"),
            "product_code": _hex_text(
                _integer_text(
                    device_type.attrib.get("ProductCode", "0"),
                    "Device/Type@ProductCode",
                ),
                8,
            ),
            "revision": _hex_text(
                _integer_text(
                    device_type.attrib.get("RevisionNo", "0"),
                    "Device/Type@RevisionNo",
                ),
                8,
            ),
            "physics": device.attrib.get("Physics"),
            "group_type": _optional_text(device, "GroupType"),
        },
        "protocol": {
            "coe": {
                "sdo_info": _bool_attribute(coe, "SdoInfo"),
                "pdo_assign": _bool_attribute(coe, "PdoAssign"),
                "pdo_config": _bool_attribute(coe, "PdoConfig"),
                "complete_access": _bool_attribute(coe, "CompleteAccess"),
                "segmented_sdo": _bool_attribute(coe, "SegmentedSdo"),
            }
        },
        "synchronization": {
            "assign_activate_by_strategy": {
                strategy: _hex_text(value, 8)
                for strategy, value in constraints.assign_activate_by_strategy.items()
            },
            "default_sync_type_by_sm": {
                str(sm): _hex_text(value, 4)
                for sm, value in constraints.default_sync_type_by_sm.items()
            },
            "minimum_cycle_ns": constraints.minimum_cycle_ns,
            "supports_distributed_clocks": constraints.supports_distributed_clocks,
        },
        "capabilities": {
            "supports_pdo_assignment": constraints.supports_pdo_assignment,
            "supports_pdo_configuration": constraints.supports_pdo_configuration,
            "supports_distributed_clocks": constraints.supports_distributed_clocks,
        },
        "default_pdo_assignment": _default_pdo_assignment(device),
        "object_dictionary": _object_dictionary(device, data_types or {}),
        "direct_pdos": {
            direction: [_pdo_fact(pdo) for pdo in device.findall(tag)]
            for direction, tag in (("rx", "RxPdo"), ("tx", "TxPdo"))
        },
    }


def extract_esi_facts(
    root: ET.Element,
    source_path: str | None = None,
    source_sha256: str | None = None,
) -> dict[str, Any]:
    """生成只读 ESI 事实报告；不包含 profile_id、运行模式或换算默认值。"""
    vendor = root.find("./Vendor")
    devices = root.findall("./Descriptions/Devices/Device")
    if vendor is None or not devices:
        raise ValueError("ESI 缺少 Vendor 或 Device")
    source: dict[str, str] = {}
    if source_path is not None:
        source["path"] = source_path
    if source_sha256 is not None:
        source["sha256"] = source_sha256

    data_types = _data_type_facts(root)
    report: dict[str, Any] = {
        "report_schema_version": 1,
        "esi_version": root.attrib.get("Version"),
        "source": source,
        "devices": [
            _device_facts(vendor, device, data_types) for device in devices
        ],
        "modules": _module_facts(root),
    }
    if len(report["devices"]) == 1:
        # 单设备 ESI 保留便捷字段；多设备 ESI 以 devices 数组为唯一来源。
        report.update(report["devices"][0])
    return report


def device_label(index: int, entry: dict[str, Any]) -> str:
    """一台设备在"请挑一台"清单里的那一行。序号从 1 起，与 --select 收的值一致。"""
    device = entry.get("device") or {}
    return "%d. %s  厂商 %s  产品 %s  版本 %s" % (
        index + 1,
        device.get("model") or device.get("device_name") or "（无名）",
        device.get("vendor_id"),
        device.get("product_code"),
        device.get("revision"),
    )


def select_device(facts: dict[str, Any], selector: str | None = None) -> dict[str, Any]:
    """把一份 ESI 事实报告收敛到**一台**设备上，形状与单设备 ESI 相同。

    一份 ESI 可以描述同一个厂商的整个产品线。单设备时 `extract_esi_facts` 会把那台
    的字段铺到顶层，多设备时只有 `devices` 数组——下游一律读顶层，于是多设备 ESI 会在
    一个 `KeyError: 'device'` 上炸掉，读的人从回溯里看不出"原来是这份 ESI 里有很多台，
    得挑一台"。

    `selector` 收两种写法：序号（照清单里那个，从 1 起），或
    `厂商ID:产品码:版本`（大小写与 `0x` 前缀都不计较）。挑不中一律 SystemExit。
    """
    devices = facts.get("devices") or []
    if not devices:
        raise SystemExit("这份 ESI 里没有 Device")
    if len(devices) == 1:
        if "device" not in facts:
            return {**facts, **devices[0]}
        return facts

    if selector is None:
        listing = "\n".join(device_label(index, entry) for index, entry in enumerate(devices))
        raise SystemExit(
            f"这份 ESI 里有 {len(devices)} 台设备，用 --select 指定一台：\n{listing}"
        )

    if selector.isdigit():
        index = int(selector) - 1
        if 0 <= index < len(devices):
            return {**facts, **devices[index]}
        raise SystemExit(f"--select {selector} 超出范围：这份 ESI 有 {len(devices)} 台")

    wanted = selector.lower().replace("0x", "")
    for entry in devices:
        device = entry.get("device") or {}
        identity = "%s:%s:%s" % (
            str(device.get("vendor_id", "")).lower().replace("0x", ""),
            str(device.get("product_code", "")).lower().replace("0x", ""),
            str(device.get("revision", "")).lower().replace("0x", ""),
        )
        if wanted in (identity, str(device.get("model", "")).lower()):
            return {**facts, **entry}
    listing = "\n".join(device_label(index, entry) for index, entry in enumerate(devices))
    raise SystemExit(f"--select {selector} 对不上这份 ESI 里的任何一台：\n{listing}")


def load_esi_facts(path: Path) -> dict[str, Any]:
    """读取 ESI 文件并生成带 SHA-256 的只读事实报告。

    解析失败一律换成一句说得清的 SystemExit。`ElementTree` 的异常只有行列号和一句
    "not well-formed"，读的人从中看不出是文件没给对还是内容真坏了。三种情形分开说：
    文件不是 XML、XML 但不是 ESI、XML 但不完整。
    """
    try:
        content = path.read_bytes()
    except OSError as error:
        raise SystemExit(f"读不了 {path}：{error}") from None
    try:
        root = ET.fromstring(content)
    except ET.ParseError as error:
        suffix = path.suffix.lower()
        hint = ("EDS 是 CANopen 的设备描述，本项目读不了；向厂商要 EtherCAT 的 ESI（XML）"
                if suffix == ".eds" else "本工具只读 ESI（XML）")
        if suffix != ".xml":
            raise SystemExit(f"{path} 不是 ESI（后缀 {suffix or '无'}）。{hint}") from None
        raise SystemExit(f"{path} 不是一份能读的 XML：{error}") from None

    try:
        facts = extract_esi_facts(
            root,
            source_path=path.as_posix(),
            source_sha256=hashlib.sha256(content).hexdigest(),
        )
    except ValueError as error:
        raise SystemExit(f"{path} 是一份 XML，但不是 ESI：{error}") from None
    return facts
