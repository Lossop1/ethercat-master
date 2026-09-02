"""校验设备目录自身的结构和字段关系。"""

from __future__ import annotations

from typing import Any

from validation_common import (
    Validation,
    hex_value,
    non_empty_string,
    validate_hex_value,
)


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


def validate_profile(check: Validation, profile: dict[str, Any]) -> bool:
    """校验生成设备目录所需的完整配置结构。"""
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

    return len(check.errors) == initial_error_count


def validate_profiles(
    check: Validation, profiles: list[dict[str, Any]]
) -> dict[str, dict[str, Any]]:
    """校验设备配置集合，并建立供拓扑引用的稳定 ID 索引。"""
    by_id: dict[str, dict[str, Any]] = {}
    for profile in profiles:
        validate_profile(check, profile)
        profile_id_value = profile.get("profile_id")
        profile_id = profile_id_value if non_empty_string(profile_id_value) else ""
        is_unique = profile_id not in by_id
        check.require(not profile_id or is_unique, f"设备配置 ID 重复：{profile_id}")
        if profile_id and is_unique:
            by_id[profile_id] = profile
    return by_id
