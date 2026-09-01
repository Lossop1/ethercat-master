"""提取 ESI 中可审查的设备事实，不把事实转换成运行方案。"""

from __future__ import annotations

import hashlib
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

from validation_common import EsiRuntimeConstraints, hex_value


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


def esi_modules(root: ET.Element) -> dict[int, dict[str, Any]]:
    """提取 ESI 模块及其双向 PDO，供设备配置进行结构化对比。"""
    modules: dict[int, dict[str, Any]] = {}
    for module in root.findall("./Descriptions/Modules/Module"):
        module_type = module.find("Type")
        if module_type is None:
            continue
        module_ident = hex_value(module_type.attrib["ModuleIdent"])
        if module_ident in modules:
            raise ValueError(f"ESI 模块标识重复：{_hex_text(module_ident, 8)}")
        result: dict[str, Any] = {}
        for xml_tag, direction in (("RxPdo", "rx"), ("TxPdo", "tx")):
            mappings = []
            for pdo in module.findall(xml_tag):
                entries = []
                for entry in pdo.findall("Entry"):
                    object_index = hex_value(entry.findtext("Index", default="0"))
                    entries.append(
                        {
                            "index": object_index,
                            "subindex": int(entry.findtext("SubIndex", default="0")),
                            "bits": int(entry.findtext("BitLen", default="0")),
                            "data_type": (
                                "PADDING"
                                if object_index == 0
                                else entry.findtext("DataType", default="")
                            ),
                        }
                    )
                mappings.append(
                    {
                        "index": hex_value(pdo.findtext("Index", default="0")),
                        "entries": entries,
                    }
                )
            result[direction] = mappings
        modules[module_ident] = result
    return modules


def little_endian_default_data(value: str) -> int:
    """把 ESI DefaultData 的字节序列解释为 EtherCAT 小端无符号整数。"""
    try:
        return int.from_bytes(bytes.fromhex(value), byteorder="little", signed=False)
    except ValueError as error:
        raise ValueError(f"ESI DefaultData 不是有效字节序列：{value!r}") from error


def _device_runtime_constraints(device: ET.Element) -> EsiRuntimeConstraints:
    """提取一个 Device 明确声明的 PDO 配置能力、同步模式和最小周期。"""
    coe = device.find("Mailbox/CoE")
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
        supports_pdo_configuration=supports_pdo_configuration,
        supports_distributed_clocks="dc" in assign_activate_by_strategy,
    )


def esi_runtime_constraints(root: ET.Element) -> EsiRuntimeConstraints:
    """为现有单型号设备目录校验提取第一项 Device 的约束。"""
    device = root.find("./Descriptions/Devices/Device")
    if device is None:
        raise ValueError("ESI 缺少 Device")
    return _device_runtime_constraints(device)


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
            }
        )
    return modules


def _device_facts(vendor: ET.Element, device: ET.Element) -> dict[str, Any]:
    """提取一个 Device 的身份、协议能力、同步约束和直接 PDO 声明。"""
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
            "supports_pdo_configuration": constraints.supports_pdo_configuration,
            "supports_distributed_clocks": constraints.supports_distributed_clocks,
        },
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

    report: dict[str, Any] = {
        "report_schema_version": 1,
        "esi_version": root.attrib.get("Version"),
        "source": source,
        "devices": [_device_facts(vendor, device) for device in devices],
        "modules": _module_facts(root),
    }
    if len(report["devices"]) == 1:
        # 单设备 ESI 保留便捷字段；多设备 ESI 以 devices 数组为唯一来源。
        report.update(report["devices"][0])
    return report


def load_esi_facts(path: Path) -> dict[str, Any]:
    """读取 ESI 文件并生成带 SHA-256 的只读事实报告。"""
    content = path.read_bytes()
    root = ET.fromstring(content)
    return extract_esi_facts(
        root,
        source_path=path.as_posix(),
        source_sha256=hashlib.sha256(content).hexdigest(),
    )
