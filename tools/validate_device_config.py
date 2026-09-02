"""校验设备目录结构，并把目录事实与受控 ESI 逐项比较。"""

from __future__ import annotations

import hashlib
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

from validation_common import (
    EsiRuntimeConstraints,
    Validation,
    hex_value,
    non_empty_string,
    validate_hex_value,
)
from esi_facts import esi_modules, esi_runtime_constraints


def validate_pdo_set(check: Validation, pdo_set: dict[str, Any], profile_id: str) -> bool:
    """校验一个 PDO 方案的映射表、条目类型、字节对齐和声明长度。"""
    initial_error_count = len(check.errors)
    pdo_id = pdo_set.get("id")
    check.require(non_empty_string(pdo_id), f"设备 {profile_id} 的 PDO 方案缺少 id")
    validate_hex_value(
        check, pdo_set.get("module_ident"), f"设备 {profile_id} 的 module_ident", 0xFFFFFFFF
    )
    module_slot = pdo_set.get("module_slot")
    check.require(
        isinstance(module_slot, int) and not isinstance(module_slot, bool) and
        1 <= module_slot <= 0xFF,
        f"设备 {profile_id} 的 PDO 方案 {pdo_id} module_slot 必须为 1..255",
    )
    mode_initialization = pdo_set.get("mode_initialization")
    if mode_initialization is not None:
        check.require(
            isinstance(mode_initialization, dict) and
            mode_initialization.get("transition") == "safeop_to_op",
            f"设备 {profile_id} 的 PDO 方案 {pdo_id} mode_initialization 过渡无效",
        )
        if isinstance(mode_initialization, dict):
            validate_hex_value(
                check, mode_initialization.get("index"),
                f"设备 {profile_id} 的 PDO 方案 {pdo_id} mode_initialization.index",
                0xFFFF,
            )
            subindex = mode_initialization.get("subindex")
            value = mode_initialization.get("value")
            check.require(
                isinstance(subindex, int) and not isinstance(subindex, bool) and
                0 <= subindex <= 0xFF,
                f"设备 {profile_id} 的 PDO 方案 {pdo_id} mode_initialization.subindex 超出范围",
            )
            check.require(
                isinstance(value, int) and not isinstance(value, bool) and
                -128 <= value <= 127,
                f"设备 {profile_id} 的 PDO 方案 {pdo_id} mode_initialization.value 超出范围",
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


def validate_catalog_against_esi(
    check: Validation,
    root_dir: Path,
    profile: dict[str, Any],
    require_vendor_artifacts: bool,
) -> EsiRuntimeConstraints | None:
    """校验 ESI 身份、全部 PDO 方案和可用于运行方案的能力事实。"""
    esi_path = root_dir / profile["source"]["esi"]
    if not esi_path.is_file():
        check.require(not require_vendor_artifacts, f"所需受控 ESI 不存在：{esi_path}")
        return None

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
        return None

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
        check.require(
            pdo_set.get("module_slot") == modules[module_ident]["module_slot"],
            f"{pdo_set['id']} 的模块槽位与 ESI 不一致",
        )
        configured_init = pdo_set.get("mode_initialization")
        if isinstance(configured_init, dict):
            expected_init = {
                "transition": configured_init.get("transition"),
                "index": hex_value(configured_init.get("index", "0x0")),
                "subindex": configured_init.get("subindex"),
                "value": configured_init.get("value"),
            }
        else:
            expected_init = None
        check.require(
            expected_init == modules[module_ident]["mode_initialization"],
            f"{pdo_set['id']} 的模式初始化命令与 ESI 不一致",
        )
        for direction in ("rx", "tx"):
            actual = modules[module_ident][direction]
            expected_mappings = [
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
                for mapping in pdo_set[direction]["mappings"]
            ]
            check.require(
                expected_mappings == actual,
                f"{pdo_set['id']} {direction} PDO 映射表、顺序、条目或数据类型与 ESI 不一致",
            )

    constraints = esi_runtime_constraints(root)
    protocol = profile["protocol"]
    check.require(
        protocol["supports_pdo_assignment"] == constraints.supports_pdo_assignment,
        f"{profile['profile_id']} 的 PDO 分配能力与 ESI CoE/PdoAssign 不一致",
    )
    check.require(
        protocol["supports_pdo_configuration"] == constraints.supports_pdo_configuration,
        f"{profile['profile_id']} 的 PDO 配置能力与 ESI CoE/PdoConfig 不一致",
    )
    check.require(
        protocol["supports_distributed_clocks"] == constraints.supports_distributed_clocks,
        f"{profile['profile_id']} 的 DC 能力与 ESI 不一致",
    )
    return constraints


def validate_profile(check: Validation, profile: dict[str, Any]) -> bool:
    """校验生成目录和 ESI 对比所需的完整设备配置结构。"""
    initial_error_count = len(check.errors)
    profile_id_value = profile.get("profile_id")
    profile_id = profile_id_value if non_empty_string(profile_id_value) else "<未知设备>"
    check.require(profile.get("schema_version") == 2, f"{profile_id} 使用不支持的设备配置版本")
    check.require(non_empty_string(profile_id_value), "设备配置缺少 profile_id")
    check.require(non_empty_string(profile.get("model")), f"设备 {profile_id} 缺少 model")

    identity = profile.get("identity")
    if not isinstance(identity, dict):
        check.errors.append(f"设备 {profile_id} 缺少 identity 对象")
    else:
        for field in ("vendor_id", "product_code", "revision"):
            validate_hex_value(
                check, identity.get(field), f"设备 {profile_id} 的 {field}", 0xFFFFFFFF
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
        reference_id = profile.get("reference_pdo_set_id")
        check.require(
            non_empty_string(reference_id) and reference_id in pdo_ids,
            f"设备 {profile_id} 的 reference_pdo_set_id 不存在",
        )

    protocol = profile.get("protocol")
    if not isinstance(protocol, dict):
        check.errors.append(f"设备 {profile_id} 缺少 protocol 对象")
    else:
        for field in ("supports_pdo_assignment", "supports_pdo_configuration", "supports_distributed_clocks"):
            check.require(
                isinstance(protocol.get(field), bool),
                f"设备 {profile_id} 的 {field} 必须是布尔值",
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
