"""校验设备目录自身的结构和字段关系。"""

from __future__ import annotations

from typing import Any

from validation_common import (
    Validation,
    hex_value,
    non_empty_string,
    validate_hex_value,
)


ASSIGNMENT_STYLES = ("module_slot", "direct_1c12")


def validate_pdo_set(check: Validation, pdo_set: dict[str, Any], profile_id: str) -> bool:
    """校验一个 PDO 方案的映射表、条目类型、字节对齐和声明长度。"""
    initial_error_count = len(check.errors)
    pdo_id = pdo_set.get("id")
    check.require(non_empty_string(pdo_id), f"设备 {profile_id} 的 PDO 方案缺少 id")
    style = pdo_set.get("assignment_style", "module_slot")
    check.require(
        style in ASSIGNMENT_STYLES,
        f"设备 {profile_id} 的 PDO 方案 {pdo_id} assignment_style 无效：{style!r}",
    )
    module_slot = pdo_set.get("module_slot")
    if style == "direct_1c12":
        # 直连分配没有模块槽位这回事：两个字段都必须留空，不能留个摆设让人以为有用。
        check.require(
            pdo_set.get("module_ident") is None and module_slot == 0,
            f"设备 {profile_id} 的 PDO 方案 {pdo_id} 是直连分配，"
            "module_ident 必须为 null 且 module_slot 必须为 0",
        )
    else:
        validate_hex_value(
            check, pdo_set.get("module_ident"), f"设备 {profile_id} 的 module_ident", 0xFFFFFFFF
        )
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


SLOW_TELEMETRY_TYPES = ("u8", "i8", "u16", "i16", "u32", "i32")
SLOW_TELEMETRY_SEMANTICS = (
    "none",
    "actual_current",
    "error_code",
    "bus_voltage",
    "mosfet_temperature",
    "motor_temperature",
    "actual_velocity",
    "target_velocity",
    "extended_error_code",
    "servo_error_code",
    "target_position_readback",
    "max_torque",
    "max_motor_speed",
)


def validate_slow_telemetry(
    check: Validation, profile: dict[str, Any], profile_id: str
) -> None:
    """校验可选的慢速遥测对象表；缺省表示该设备不声明任何供应商遥测对象。"""
    entries = profile.get("slow_telemetry")
    if entries is None:
        return
    if not isinstance(entries, list) or not entries:
        check.errors.append(f"设备 {profile_id} 的 slow_telemetry 必须是非空数组")
        return
    names: set[str] = set()
    addresses: set[tuple[int, int]] = set()
    for ordinal, entry in enumerate(entries, start=1):
        if not isinstance(entry, dict):
            check.errors.append(
                f"设备 {profile_id} 的 slow_telemetry 第 {ordinal} 条必须是对象"
            )
            continue
        name = entry.get("name")
        index_valid = validate_hex_value(
            check,
            entry.get("index"),
            f"设备 {profile_id} 的 slow_telemetry 第 {ordinal} 条 index",
            0xFFFF,
        )
        subindex = entry.get("subindex")
        subindex_valid = (
            isinstance(subindex, int)
            and not isinstance(subindex, bool)
            and 0 <= subindex <= 0xFF
        )
        check.require(
            non_empty_string(name) and name not in names,
            f"设备 {profile_id} 的 slow_telemetry 第 {ordinal} 条名称无效或重复",
        )
        check.require(
            subindex_valid,
            f"设备 {profile_id} 的 slow_telemetry 第 {ordinal} 条 subindex 超出范围",
        )
        check.require(
            entry.get("type") in SLOW_TELEMETRY_TYPES,
            f"设备 {profile_id} 的 slow_telemetry 第 {ordinal} 条类型无效",
        )
        check.require(
            entry.get("semantic") in SLOW_TELEMETRY_SEMANTICS,
            f"设备 {profile_id} 的 slow_telemetry 第 {ordinal} 条语义无效",
        )
        check.require(
            isinstance(entry.get("unit"), str),
            f"设备 {profile_id} 的 slow_telemetry 第 {ordinal} 条 unit 必须是字符串",
        )
        if non_empty_string(name):
            names.add(name)
        if index_valid and subindex_valid:
            address = (hex_value(entry["index"]), subindex)
            check.require(
                address not in addresses,
                f"设备 {profile_id} 的 slow_telemetry 第 {ordinal} 条地址重复",
            )
            addresses.add(address)


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

        # 模式能力必须由档声明：ESI 的模块类型只有模块化设备才写得出，非模块化设备
        # 一个字都没有。缺省成"什么都支持"会把设备带进它不认的模式；缺省成"什么都不
        # 支持"会让每一份配置都跑不起来。两种都不是实话，所以不给缺省。
        modes = protocol.get("supported_modes")
        modes_valid = isinstance(modes, dict) and bool(modes)
        check.require(
            modes_valid,
            f"设备 {profile_id} 的 supported_modes 必须是非空对象（模式名 → 0x6060 取值）；"
            "模式能力推不出来，必须由人声明",
        )
        if modes_valid:
            for name, value in sorted(modes.items()):
                check.require(
                    non_empty_string(name),
                    f"设备 {profile_id} 的 supported_modes 键必须是非空字符串",
                )
                check.require(
                    isinstance(value, int)
                    and not isinstance(value, bool)
                    and -128 <= value <= 127,
                    f"设备 {profile_id} 的模式 {name} 取值必须是 -128..127 的整数",
                )

        # ESI 声明的 AssignActivate：DC 与 SM 各一个。这个映射里没有哪种同步方式，
        # 就表示厂商没写它 —— 于是不能生成要求它的运行方案（生成器也会拒绝），
        # 而不是替厂商编一个"标准上一般是这样"的值填上。
        assign = protocol.get("assign_activate_by_strategy")
        if not isinstance(assign, dict):
            check.errors.append(
                f"设备 {profile_id} 的 assign_activate_by_strategy 必须是对象；"
                "缺了它就无法确定同步怎么使能，只能由人填"
            )
        else:
            for strategy, value in sorted(assign.items()):
                check.require(
                    strategy in ("dc", "sm"),
                    f"设备 {profile_id} 的 assign_activate_by_strategy 键必须是 dc 或 sm：{strategy!r}",
                )
                validate_hex_value(
                    check, value, f"设备 {profile_id} 的 {strategy} AssignActivate", 0xFFFFFFFF
                )

        # 掩码为 0 合法且含义明确："这台设备没有可靠的设备侧停机判据"。但它的后果
        # 只有读运行期代码才看得见 —— 停机时只按标准 CiA 402 状态判据（未使能/未就绪）
        # 判定"能不能安全停"。对状态字不符合标准的驱动器这不安全：台架上实测过
        # 某型号**真使能时 0x6041 也读到 0x1250**，而 0x1250 & 0x006F == SWITCH_ON_DISABLED，
        # 于是主站会在驱动器仍出力时报"全轴已安全"。合法，可以放过，但必须说出来。
        mask_text = protocol.get("safe_stop_status_mask")
        try:
            mask_is_zero = isinstance(mask_text, str) and hex_value(mask_text) == 0
        except ValueError:
            mask_is_zero = False
        check.warn(
            not mask_is_zero,
            f"设备 {profile_id} 的停机判据是空的（safe_stop_status_mask=0），"
            "停机只按标准 CiA 402 状态判据。真使能时 0x6041 读 0x1250 这类驱动器会被误判为"
            "已安全。台架量准 0x6041 后填实。",
        )

    # 缺省合法：厂商资料没给换算，是句实话。给了就必须完整 —— 一半的换算折算出来的
    # 计数看着像真的。三项齐全的声明在运行期压过设备自报的 608F/6091：自报是驱动器的
    # 说法，这一份是人对着机器写下的判据。声明缺项就退回自报；两边都没有（MT 的对象
    # 字典里就没有这两条）则该轴换算不可用。谁生效记在报告的来源字段里，自报值照写。
    def positive_count(value: Any) -> bool:
        return (
            isinstance(value, int)
            and not isinstance(value, bool)
            and 0 < value <= 0xFFFFFFFF
        )

    conversion = profile.get("conversion")
    if conversion is not None:
        check.require(
            isinstance(conversion, dict),
            f"设备 {profile_id} 的 conversion 必须是对象",
        )
        if isinstance(conversion, dict):
            # 三项是一件事：编码器每转计数、减速比电机侧与负载侧。少一项就不是换算，
            # 是一半的换算 —— 而按半份算出来的角度不会报错，只会把轴按错的系数动起来。
            #
            # 为什么"少给减速比"也在这里拒，而不是留给运行期。运行期把这一整份声明灌
            # 进一个换算（relative_position.h 的 emaster_position_scale_from_declaration：
            # 三项齐全才认），缺任一项来源就留成"没有"，会话判 MOVEMENT_INVALID，轴一步
            # 不动。让这种档过了配置校验，失败就推到了运行期，那时看到的是一句跟配置
            # 无关的错。直驱（无减速）就写 1:1，别省。
            missing = [
                key
                for key in (
                    "encoder_counts_per_motor_revolution_default",
                    "gear_motor_revolutions_default",
                    "gear_shaft_revolutions_default",
                )
                if not positive_count(conversion.get(key))
            ]
            if missing:
                check.errors.append(
                    f"设备 {profile_id} 的 conversion 三项必须一起给，且都是正整数"
                    f"（编码器每转计数、减速比电机侧与负载侧）；缺 "
                    + "、".join(missing)
                )
            # -1 表示没声明，所以缺省与 607E 的合法取值 0/1 分得开。
            polarity = conversion.get("polarity_default")
            check.require(
                polarity is None
                or (
                    isinstance(polarity, int)
                    and not isinstance(polarity, bool)
                    and polarity in (0, 1)
                ),
                f"设备 {profile_id} 的 polarity_default 只能是 0 或 1",
            )

    validate_slow_telemetry(check, profile, profile_id)

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
