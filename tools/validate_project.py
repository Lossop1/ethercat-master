#!/usr/bin/env python3
"""校验设备、拓扑、部署和受控供应商资料之间的工程契约。"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class Validation:
    errors: list[str]

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.errors.append(message)


def hex_value(value: str) -> int:
    return int(value.replace("#x", "0x"), 16)


def non_empty_string(value: object) -> bool:
    """只接受去除首尾空白后仍有内容的字符串。"""
    return isinstance(value, str) and bool(value.strip())


def validate_hex_value(
    check: Validation, value: object, field: str, maximum: int
) -> bool:
    """校验配置中的十六进制字符串及目标无符号整数范围。"""
    try:
        valid = isinstance(value, str) and value.startswith("0x")
        number = hex_value(value) if valid else -1
        valid = valid and 0 <= number <= maximum
    except ValueError:
        valid = False
    check.require(valid, f"{field} 必须是范围内的十六进制字符串")
    return valid


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} 必须包含 JSON 对象")
    return value


def load_documents(directory: Path, kind: str, check: Validation) -> list[dict[str, Any]]:
    """按文件名稳定排序加载一类配置，并把读取错误转为可汇总的校验错误。"""
    documents: list[dict[str, Any]] = []
    paths = sorted(directory.glob("*.json")) if directory.is_dir() else []
    check.require(bool(paths), f"未找到{kind}配置：{directory}")
    for path in paths:
        try:
            document = load_json(path)
            document["_source_path"] = path.as_posix()
            documents.append(document)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            check.errors.append(f"无法读取{kind}配置 {path}：{error}")
    return documents


def validate_pdo_set(check: Validation, pdo_set: dict[str, Any], profile_id: str) -> bool:
    """校验一个 PDO 方案的映射表、条目类型、字节对齐和声明长度。"""
    initial_error_count = len(check.errors)
    pdo_id = pdo_set.get("id")
    check.require(non_empty_string(pdo_id), f"设备 {profile_id} 的 PDO 方案缺少 id")
    validate_hex_value(
        check, pdo_set.get("module_ident"), f"设备 {profile_id} 的 module_ident", 0xFFFFFFFF
    )

    for direction in ("rx", "tx"):
        direction_config = pdo_set.get(direction)
        if not isinstance(direction_config, dict):
            check.errors.append(f"PDO 方案 {pdo_id} 缺少 {direction} 对象")
            continue
        byte_count = direction_config.get("bytes")
        byte_count_valid = (
            isinstance(byte_count, int)
            and not isinstance(byte_count, bool)
            and 0 < byte_count <= 0xFFFF
        )
        check.require(byte_count_valid, f"{pdo_id} {direction}.bytes 必须是正整数")
        mappings = direction_config.get("mappings")
        if not isinstance(mappings, list) or not mappings:
            check.errors.append(f"{pdo_id} {direction}.mappings 必须是非空数组")
            continue

        bit_sum = 0
        mappings_valid = True
        mapping_indices: list[int] = []
        for mapping_ordinal, mapping in enumerate(mappings, start=1):
            if not isinstance(mapping, dict):
                check.errors.append(
                    f"{pdo_id} {direction} 第 {mapping_ordinal} 张映射表必须是对象"
                )
                mappings_valid = False
                continue
            mapping_index_valid = validate_hex_value(
                check,
                mapping.get("index"),
                f"{pdo_id} {direction} 第 {mapping_ordinal} 张映射表的 index",
                0xFFFF,
            )
            if mapping_index_valid:
                mapping_indices.append(hex_value(mapping["index"]))
            entries = mapping.get("entries")
            if not isinstance(entries, list) or not entries:
                check.errors.append(
                    f"{pdo_id} {direction} 第 {mapping_ordinal} 张映射表的 entries 必须是非空数组"
                )
                mappings_valid = False
                continue
            for entry_ordinal, entry in enumerate(entries, start=1):
                if not isinstance(entry, dict):
                    check.errors.append(
                        f"{pdo_id} {direction} 第 {mapping_ordinal} 张映射表的第 "
                        f"{entry_ordinal} 个条目必须是对象"
                    )
                    mappings_valid = False
                    continue
                object_index_valid = validate_hex_value(
                    check,
                    entry.get("index"),
                    f"{pdo_id} {direction} 映射条目的 index",
                    0xFFFF,
                )
                subindex = entry.get("subindex")
                bits = entry.get("bits")
                data_type = entry.get("data_type")
                entry_name = entry.get("name")
                subindex_valid = (
                    isinstance(subindex, int)
                    and not isinstance(subindex, bool)
                    and 0 <= subindex <= 0xFF
                )
                bits_valid = (
                    isinstance(bits, int) and not isinstance(bits, bool) and 0 < bits <= 0xFF
                )
                data_type_valid = non_empty_string(data_type)
                name_valid = non_empty_string(entry_name)
                check.require(subindex_valid, f"{pdo_id} {direction} 条目的 subindex 超出范围")
                check.require(bits_valid, f"{pdo_id} {direction} 条目的 bits 必须为 1..255")
                check.require(data_type_valid, f"{pdo_id} {direction} 条目缺少 data_type")
                check.require(name_valid, f"{pdo_id} {direction} 条目缺少 name")
                if object_index_valid and subindex_valid and data_type_valid:
                    is_padding = hex_value(entry["index"]) == 0
                    check.require(
                        (is_padding and subindex == 0 and data_type == "PADDING")
                        or (not is_padding and data_type != "PADDING"),
                        f"{pdo_id} {direction} 的填充条目必须使用 0x0000:00/PADDING",
                    )
                mappings_valid = (
                    mappings_valid
                    and mapping_index_valid
                    and object_index_valid
                    and subindex_valid
                    and bits_valid
                    and data_type_valid
                    and name_valid
                )
                if bits_valid:
                    bit_sum += bits

        check.require(
            len(mapping_indices) == len(set(mapping_indices)),
            f"{pdo_id} {direction} 的映射表索引必须唯一",
        )
        if not mappings_valid or not byte_count_valid:
            continue
        check.require(bit_sum % 8 == 0, f"{pdo_id} {direction} 未按字节对齐")
        check.require(
            bit_sum // 8 == byte_count,
            f"{pdo_id} {direction} 字节数与条目不一致",
        )
    return len(check.errors) == initial_error_count


def esi_modules(root: ET.Element) -> dict[int, dict[str, Any]]:
    """提取 ESI 模块及其双向 PDO，供设备配置进行结构化对比。"""
    modules: dict[int, dict[str, Any]] = {}
    for module in root.findall("./Descriptions/Modules/Module"):
        module_type = module.find("Type")
        if module_type is None:
            continue
        module_ident = hex_value(module_type.attrib["ModuleIdent"])
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


def validate_catalog_against_esi(
    check: Validation,
    root_dir: Path,
    profile: dict[str, Any],
    require_vendor_artifacts: bool,
) -> None:
    esi_path = root_dir / profile["source"]["esi"]
    if not esi_path.is_file():
        check.require(not require_vendor_artifacts, f"所需受控 ESI 不存在：{esi_path}")
        return

    actual_hash = hashlib.sha256(esi_path.read_bytes()).hexdigest()
    check.require(
        actual_hash == profile["source"]["esi_sha256"].lower(),
        f"{profile['profile_id']} 的 ESI SHA-256 与设备配置不一致",
    )

    root = ET.parse(esi_path).getroot()
    vendor = root.find("./Vendor")
    device_type = root.find("./Descriptions/Devices/Device/Type")
    check.require(vendor is not None, "ESI 缺少 Vendor 部分")
    check.require(device_type is not None, "ESI 缺少 Device Type")
    if vendor is None or device_type is None:
        return

    identity = profile["identity"]
    check.require(
        hex_value(vendor.findtext("Id", default="0")) == hex_value(identity["vendor_id"]),
        "ESI Vendor ID 与设备目录不一致",
    )
    check.require(device_type.text == profile["model"], "ESI 型号与设备目录不一致")
    check.require(
        hex_value(device_type.attrib["ProductCode"]) == hex_value(identity["product_code"]),
        "ESI Product Code 与设备目录不一致",
    )
    check.require(
        hex_value(device_type.attrib["RevisionNo"]) == hex_value(identity["revision"]),
        "ESI Revision 与设备目录不一致",
    )

    modules = esi_modules(root)
    for pdo_set in profile["pdo_sets"]:
        module_ident = hex_value(pdo_set["module_ident"])
        check.require(module_ident in modules, f"ESI 缺少模块 0x{module_ident:08X}")
        if module_ident not in modules:
            continue
        for direction in ("rx", "tx"):
            expected = pdo_set[direction]
            actual = modules[module_ident][direction]
            expected_mappings = []
            for mapping in expected["mappings"]:
                expected_mappings.append(
                    {
                        "index": hex_value(mapping["index"]),
                        "entries": [
                            {
                                "index": hex_value(entry["index"]),
                                "subindex": int(entry["subindex"]),
                                "bits": int(entry["bits"]),
                                "data_type": entry["data_type"],
                            }
                            for entry in mapping["entries"]
                        ],
                    }
                )
            check.require(
                expected_mappings == actual,
                f"{pdo_set['id']} {direction} PDO 映射表、顺序、条目或数据类型与 ESI 不一致",
            )


def validate_profile(check: Validation, profile: dict[str, Any]) -> bool:
    """校验生成目录和 ESI 对比所需的完整设备配置结构。"""
    initial_error_count = len(check.errors)
    profile_id_value = profile.get("profile_id")
    profile_id = profile_id_value if non_empty_string(profile_id_value) else "<未知设备>"
    check.require(profile.get("schema_version") == 1, f"{profile_id} 使用不支持的设备配置版本")
    check.require(non_empty_string(profile_id_value), "设备配置缺少 profile_id")
    check.require(non_empty_string(profile.get("model")), f"设备 {profile_id} 缺少 model")

    identity = profile.get("identity")
    if not isinstance(identity, dict):
        check.errors.append(f"设备 {profile_id} 缺少 identity 对象")
    else:
        validate_hex_value(
            check, identity.get("vendor_id"), f"设备 {profile_id} 的 vendor_id", 0xFFFFFFFF
        )
        validate_hex_value(
            check, identity.get("product_code"), f"设备 {profile_id} 的 product_code", 0xFFFFFFFF
        )
        validate_hex_value(
            check, identity.get("revision"), f"设备 {profile_id} 的 revision", 0xFFFFFFFF
        )

    pdo_sets = profile.get("pdo_sets")
    pdo_ids: list[str] = []
    if not isinstance(pdo_sets, list) or not pdo_sets:
        check.errors.append(f"设备 {profile_id} 的 pdo_sets 必须是非空数组")
    else:
        for pdo_set in pdo_sets:
            if not isinstance(pdo_set, dict):
                check.errors.append(f"设备 {profile_id} 的 PDO 方案必须是对象")
                continue
            if non_empty_string(pdo_set.get("id")):
                pdo_ids.append(pdo_set["id"])
            validate_pdo_set(check, pdo_set, profile_id)
        check.require(len(pdo_ids) == len(set(pdo_ids)), f"设备 {profile_id} 的 PDO 方案 ID 必须唯一")
        check.require(
            profile.get("selected_pdo_set") in pdo_ids,
            f"设备 {profile_id} 选择的 PDO 方案不存在",
        )

    protocol = profile.get("protocol")
    if not isinstance(protocol, dict):
        check.errors.append(f"设备 {profile_id} 缺少 protocol 对象")
    else:
        check.require(
            isinstance(protocol.get("requires_distributed_clocks"), bool),
            f"设备 {profile_id} 的 requires_distributed_clocks 必须是布尔值",
        )

    conversion = profile.get("conversion")
    if not isinstance(conversion, dict):
        check.errors.append(f"设备 {profile_id} 缺少 conversion 对象")
    else:
        encoder_counts = conversion.get("encoder_counts_per_motor_revolution_default")
        check.require(
            isinstance(encoder_counts, int)
            and not isinstance(encoder_counts, bool)
            and 0 < encoder_counts <= 0xFFFFFFFF,
            f"设备 {profile_id} 的默认编码器计数必须是正整数",
        )

    source = profile.get("source")
    if not isinstance(source, dict):
        check.errors.append(f"设备 {profile_id} 缺少 source 对象")
    else:
        check.require(non_empty_string(source.get("esi")), f"设备 {profile_id} 缺少 ESI 路径")
        esi_hash = source.get("esi_sha256")
        hash_valid = isinstance(esi_hash, str) and len(esi_hash) == 64
        if hash_valid:
            try:
                int(esi_hash, 16)
            except ValueError:
                hash_valid = False
        check.require(hash_valid, f"设备 {profile_id} 的 ESI SHA-256 无效")

    return len(check.errors) == initial_error_count


def validate_profiles(
    check: Validation, profiles: list[dict[str, Any]]
) -> tuple[dict[str, dict[str, Any]], list[dict[str, Any]]]:
    """校验设备配置集合，并建立供拓扑引用的稳定 ID 索引。"""
    by_id: dict[str, dict[str, Any]] = {}
    valid_profiles: list[dict[str, Any]] = []
    for profile in profiles:
        structurally_valid = validate_profile(check, profile)
        profile_id_value = profile.get("profile_id")
        profile_id = profile_id_value if non_empty_string(profile_id_value) else ""
        is_unique = profile_id not in by_id
        check.require(not profile_id or is_unique, f"设备配置 ID 重复：{profile_id}")
        if profile_id and is_unique:
            by_id[profile_id] = profile
        if structurally_valid:
            valid_profiles.append(profile)
    return by_id, valid_profiles


def validate_topology(
    check: Validation, topology: dict[str, Any], profiles: dict[str, dict[str, Any]]
) -> None:
    """校验任意非空规模的逻辑拓扑，不引入产品轴数或部署网卡假设。"""
    topology_id_value = topology.get("topology_id")
    topology_id = topology_id_value if non_empty_string(topology_id_value) else "<未知拓扑>"
    check.require(topology.get("schema_version") == 1, f"{topology_id} 使用不支持的拓扑版本")
    slaves = topology.get("slaves")
    if not isinstance(slaves, list):
        check.errors.append(f"拓扑 {topology_id} 的 slaves 必须是数组")
        return
    check.require(bool(slaves), f"拓扑 {topology_id} 不能为空")

    positions: list[int] = []
    axis_ids: list[str] = []
    entries_valid = True
    for entry_index, slave in enumerate(slaves, start=1):
        if not isinstance(slave, dict):
            check.errors.append(f"拓扑 {topology_id} 的第 {entry_index} 个从站必须是对象")
            entries_valid = False
            continue
        position = slave.get("position")
        position_valid = (
            isinstance(position, int) and not isinstance(position, bool) and position > 0
        )
        axis_id = slave.get("axis_id")
        profile_id = slave.get("profile_id")
        check.require(position_valid, f"拓扑 {topology_id} 的从站位置必须是正整数")
        check.require(non_empty_string(axis_id), f"拓扑 {topology_id} 的轴 ID 不能为空")
        check.require(non_empty_string(profile_id), f"拓扑 {topology_id} 的 profile_id 不能为空")
        entries_valid = entries_valid and position_valid and non_empty_string(axis_id)
        entries_valid = entries_valid and non_empty_string(profile_id)
        if position_valid:
            positions.append(position)
        if non_empty_string(axis_id):
            axis_ids.append(axis_id)
        check.require(
            not non_empty_string(profile_id) or profile_id in profiles,
            f"拓扑 {topology_id} 的位置 {position} 使用未知设备配置：{profile_id}",
        )
    if entries_valid:
        check.require(len(positions) == len(set(positions)), f"拓扑 {topology_id} 的从站位置必须唯一")
        # 这里的 1 是 SOEM/探测报告采用的总线位置起点；终点始终由实际列表长度决定。
        check.require(
            positions == list(range(1, len(slaves) + 1)),
            f"拓扑 {topology_id} 的从站位置必须从 1 连续排列到实际长度",
        )
        check.require(len(axis_ids) == len(set(axis_ids)), f"拓扑 {topology_id} 的轴 ID 必须唯一")


def validate_topologies(
    check: Validation,
    topologies: list[dict[str, Any]],
    profiles: dict[str, dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    by_id: dict[str, dict[str, Any]] = {}
    for topology in topologies:
        topology_id_value = topology.get("topology_id")
        topology_id = topology_id_value if non_empty_string(topology_id_value) else ""
        check.require(bool(topology_id), "拓扑配置缺少 topology_id")
        check.require(not topology_id or topology_id not in by_id, f"拓扑 ID 重复：{topology_id}")
        validate_topology(check, topology, profiles)
        if topology_id and topology_id not in by_id:
            by_id[topology_id] = topology
    return by_id


def validate_deployments(
    check: Validation,
    deployments: list[dict[str, Any]],
    topologies: dict[str, dict[str, Any]],
) -> None:
    """校验物理部署引用，并阻止同一主机网口被多个部署声明占用。"""
    deployment_ids: set[str] = set()
    occupied_interfaces: set[tuple[str, str]] = set()
    for deployment in deployments:
        deployment_id_value = deployment.get("deployment_id")
        hostname_value = deployment.get("hostname")
        interface_value = deployment.get("ethercat_interface")
        topology_id_value = deployment.get("topology_id")
        deployment_id = deployment_id_value if non_empty_string(deployment_id_value) else ""
        hostname = hostname_value if non_empty_string(hostname_value) else ""
        interface = interface_value if non_empty_string(interface_value) else ""
        topology_id = topology_id_value if non_empty_string(topology_id_value) else ""
        management_interface = deployment.get("management_interface")

        check.require(deployment.get("schema_version") == 1, f"{deployment_id} 使用不支持的部署版本")
        check.require(bool(deployment_id), "部署配置缺少 deployment_id")
        check.require(
            not deployment_id or deployment_id not in deployment_ids,
            f"部署 ID 重复：{deployment_id}",
        )
        check.require(bool(hostname), f"部署 {deployment_id} 缺少 hostname")
        check.require(bool(interface), f"部署 {deployment_id} 缺少 ethercat_interface")
        check.require(bool(topology_id), f"部署 {deployment_id} 缺少 topology_id")
        check.require(topology_id in topologies, f"部署 {deployment_id} 引用了未知拓扑：{topology_id}")
        check.require(
            management_interface is None or non_empty_string(management_interface),
            f"部署 {deployment_id} 的 management_interface 必须是非空字符串",
        )
        check.require(
            not management_interface or management_interface != interface,
            f"部署 {deployment_id} 的 EtherCAT 与管理接口不能相同",
        )

        # 网口名只在部署层有意义；用主机和接口组成物理资源唯一键。
        occupancy = (hostname, interface)
        check.require(
            not hostname or not interface or occupancy not in occupied_interfaces,
            f"主机 {hostname} 的接口 {interface} 被多个部署重复占用",
        )
        if deployment_id:
            deployment_ids.add(deployment_id)
        if hostname and interface:
            occupied_interfaces.add(occupancy)


def validate_manifest(
    check: Validation,
    root_dir: Path,
    manifest_path: Path,
    require_vendor_artifacts: bool,
) -> tuple[int, int]:
    declared = 0
    installed = 0
    if not manifest_path.is_file():
        check.errors.append(f"供应商资料清单不存在：{manifest_path}")
        return declared, installed
    for line_number, raw_line in enumerate(
        manifest_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            expected_hash, relative_path = line.split("  ", 1)
        except ValueError:
            check.errors.append(f"资料清单第 {line_number} 行无效")
            continue
        declared += 1
        path = root_dir / relative_path
        check.require(
            path.is_file() or not require_vendor_artifacts,
            f"缺少所需受控资料：{relative_path}",
        )
        if path.is_file():
            installed += 1
            actual_hash = hashlib.sha256(path.read_bytes()).hexdigest()
            check.require(actual_hash == expected_hash, f"SHA-256 不一致：{relative_path}")
    return declared, installed


def run(root_dir: Path, require_vendor_artifacts: bool) -> tuple[list[str], int, int]:
    """执行全仓配置校验，返回全部错误以及受控资料的安装统计。"""
    check = Validation(errors=[])
    profiles = load_documents(root_dir / "config/devices", "设备", check)
    topologies = load_documents(root_dir / "config/topologies", "拓扑", check)
    deployments = load_documents(root_dir / "config/deployments", "部署", check)
    manifest_path = root_dir / "docs/vendor/SHA256SUMS"

    profiles_by_id, valid_profiles = validate_profiles(check, profiles)
    for profile in valid_profiles:
        try:
            validate_catalog_against_esi(check, root_dir, profile, require_vendor_artifacts)
        except (ET.ParseError, KeyError, OSError, TypeError, ValueError) as error:
            check.errors.append(f"设备 {profile['profile_id']} 的 ESI 无法校验：{error}")
    topologies_by_id = validate_topologies(check, topologies, profiles_by_id)
    validate_deployments(check, deployments, topologies_by_id)
    declared, installed = validate_manifest(
        check, root_dir, manifest_path, require_vendor_artifacts
    )
    return check.errors, declared, installed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument(
        "--require-vendor-artifacts",
        action="store_true",
        help="任何受控供应商资料未安装时返回失败",
    )
    args = parser.parse_args()
    errors, declared, installed = run(args.root.resolve(), args.require_vendor_artifacts)
    if errors:
        for error in errors:
            print(f"错误：{error}", file=sys.stderr)
        return 1
    if installed == declared:
        print("设备、ESI、拓扑、部署和受控供应商资料一致。")
    else:
        print(
            "项目元数据一致；受控供应商资料未全部安装"
            f"（已安装 {installed}/{declared}）。"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
