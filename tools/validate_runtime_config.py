"""校验运行方案、逻辑拓扑和物理部署之间的引用契约。"""

from __future__ import annotations

from typing import Any

from validation_common import (
    Validation,
    hex_value,
    non_empty_string,
    validate_hex_value,
)


def pdo_field_names(pdo_set: dict[str, Any], direction: str) -> set[str]:
    """提取一个 PDO 方案中可供模式引用的稳定字段名。"""
    names: set[str] = set()
    direction_config = pdo_set.get(direction)
    if not isinstance(direction_config, dict):
        return names
    mappings = direction_config.get("mappings")
    if not isinstance(mappings, list):
        return names
    for mapping in mappings:
        if not isinstance(mapping, dict) or not isinstance(mapping.get("entries"), list):
            continue
        for entry in mapping["entries"]:
            if isinstance(entry, dict) and non_empty_string(entry.get("name")):
                names.add(entry["name"])
    return names


def validate_operation_profile(
    check: Validation,
    operation: dict[str, Any],
    profiles: dict[str, dict[str, Any]],
) -> bool:
    """校验运行方案引用、同步策略和模式字段，不替运行者补默认参数。"""
    initial_error_count = len(check.errors)
    operation_id_value = operation.get("operation_profile_id")
    operation_id = (
        operation_id_value if non_empty_string(operation_id_value) else "<未知运行方案>"
    )
    check.require(
        operation.get("schema_version") == 1,
        f"{operation_id} 使用不支持的运行方案版本",
    )
    check.require(
        non_empty_string(operation_id_value),
        "运行方案配置缺少 operation_profile_id",
    )
    status = operation.get("status")
    check.require(
        status in ("draft", "approved"),
        f"运行方案 {operation_id} 的 status 必须是 draft 或 approved",
    )

    device_profile_id = operation.get("device_profile_id")
    check.require(
        non_empty_string(device_profile_id),
        f"运行方案 {operation_id} 缺少 device_profile_id",
    )
    device = profiles.get(device_profile_id) if non_empty_string(device_profile_id) else None
    check.require(
        device is not None,
        f"运行方案 {operation_id} 引用了未知设备配置：{device_profile_id}",
    )

    pdo_set_id = operation.get("pdo_set_id")
    pdo_set: dict[str, Any] | None = None
    if device is not None:
        pdo_sets = device.get("pdo_sets")
        if isinstance(pdo_sets, list):
            pdo_set = next(
                (
                    item
                    for item in pdo_sets
                    if isinstance(item, dict) and item.get("id") == pdo_set_id
                ),
                None,
            )
    check.require(
        non_empty_string(pdo_set_id) and pdo_set is not None,
        f"运行方案 {operation_id} 的 pdo_set_id 不存在：{pdo_set_id}",
    )

    sync = operation.get("sync")
    strategy = sync.get("strategy") if isinstance(sync, dict) else None
    check.require(
        strategy in ("sm", "dc"),
        f"运行方案 {operation_id} 的同步策略必须是 sm 或 dc",
    )
    if strategy == "dc" and device is not None:
        supports_dc = device.get("protocol", {}).get("supports_distributed_clocks")
        check.require(
            supports_dc is True,
            f"运行方案 {operation_id} 要求 DC，但设备目录未声明支持",
        )
    validate_sync_config(check, operation_id, status, strategy, sync)
    validate_modes(check, operation_id, operation.get("modes"), device, pdo_set)
    selected_mode_id = operation.get("selected_mode_id")
    selected_mode_valid = selected_mode_id is None or non_empty_string(selected_mode_id)
    check.require(
        selected_mode_valid,
        f"运行方案 {operation_id} 的 selected_mode_id 必须是非空字符串或 null",
    )
    modes = operation.get("modes")
    mode_ids = {
        mode.get("id")
        for mode in modes
        if isinstance(mode, dict) and non_empty_string(mode.get("id"))
    } if isinstance(modes, list) else set()
    check.require(
        selected_mode_id is None or selected_mode_id in mode_ids,
        f"运行方案 {operation_id} 的 selected_mode_id 不在 modes 中：{selected_mode_id}",
    )
    if status == "approved":
        check.require(
            selected_mode_id is not None,
            f"已批准运行方案 {operation_id} 缺少 selected_mode_id",
        )
    mode_initialization = pdo_set.get("mode_initialization") if pdo_set is not None else None
    if isinstance(mode_initialization, dict) and selected_mode_id is not None and isinstance(modes, list):
        selected_mode = next(
            (mode for mode in modes if isinstance(mode, dict) and mode.get("id") == selected_mode_id),
            None,
        )
        check.require(
            selected_mode is not None and
            selected_mode.get("value") == mode_initialization.get("value"),
            f"运行方案 {operation_id} 的模式值与所选 PDO 模块初始化值不一致",
        )
    return len(check.errors) == initial_error_count


def validate_sync_config(
    check: Validation,
    operation_id: str,
    status: object,
    strategy: object,
    sync: object,
) -> None:
    """校验运行方案显式给出的同步参数。"""
    if not isinstance(sync, dict):
        check.errors.append(f"运行方案 {operation_id} 缺少 sync 对象")
        return

    for field, maximum in (
        ("assign_activate", 0xFFFFFFFF),
        ("sm2_sync_type", 0xFFFF),
        ("sm3_sync_type", 0xFFFF),
    ):
        value = sync.get(field)
        if value is not None:
            validate_hex_value(
                check, value, f"运行方案 {operation_id} 的 {field}", maximum
            )

    cycle_ns = sync.get("cycle_ns")
    cycle_valid = (
        cycle_ns is None
        or (
            isinstance(cycle_ns, int)
            and not isinstance(cycle_ns, bool)
            and 0 < cycle_ns <= 0xFFFFFFFF
        )
    )
    check.require(cycle_valid, f"运行方案 {operation_id} 的 cycle_ns 必须是正整数或 null")

    process_data_phase_ns = sync.get("process_data_phase_ns")
    phase_valid = (
        process_data_phase_ns is None
        or (
            isinstance(process_data_phase_ns, int)
            and not isinstance(process_data_phase_ns, bool)
            and 0 <= process_data_phase_ns <= 0xFFFFFFFF
        )
    )
    check.require(
        phase_valid,
        f"运行方案 {operation_id} 的 process_data_phase_ns 必须是非负整数或 null",
    )
    if (
        cycle_valid
        and isinstance(cycle_ns, int)
        and isinstance(process_data_phase_ns, int)
        and not isinstance(process_data_phase_ns, bool)
    ):
        check.require(
            process_data_phase_ns < cycle_ns,
            f"运行方案 {operation_id} 的 process_data_phase_ns 必须小于 cycle_ns",
        )

    dc_startup_cycles = sync.get("dc_startup_cycles")
    check.require(
        dc_startup_cycles is None
        or (
            isinstance(dc_startup_cycles, int)
            and not isinstance(dc_startup_cycles, bool)
            and 0 < dc_startup_cycles <= 0xFFFFFFFF
        ),
        f"运行方案 {operation_id} 的 dc_startup_cycles 必须是正整数或 null",
    )

    shift = sync.get("sync0_shift_ns")
    check.require(
        shift is None
        or (
            isinstance(shift, int)
            and not isinstance(shift, bool)
            and -(1 << 31) <= shift < (1 << 31)
        ),
        f"运行方案 {operation_id} 的 sync0_shift_ns 必须是 32 位有符号整数或 null",
    )
    if status == "approved":
        for field in ("assign_activate", "cycle_ns", "sm2_sync_type", "sm3_sync_type"):
            check.require(
                sync.get(field) is not None,
                f"已批准运行方案 {operation_id} 缺少同步参数 {field}",
            )
        if strategy == "dc":
            for field in (
                "sync0_shift_ns",
                "process_data_phase_ns",
                "dc_startup_cycles",
            ):
                check.require(
                    sync.get(field) is not None,
                    f"已批准 DC 运行方案 {operation_id} 缺少 {field}",
                )


def validate_modes(
    check: Validation,
    operation_id: str,
    modes: object,
    device: dict[str, Any] | None,
    pdo_set: dict[str, Any] | None,
) -> None:
    """校验模式值及其要求的 PDO 字段。"""
    check.require(
        isinstance(modes, list) and bool(modes),
        f"运行方案 {operation_id} 的 modes 必须是非空数组",
    )
    if not isinstance(modes, list):
        return

    mode_ids: set[str] = set()
    supported_modes = (
        device.get("protocol", {}).get("supported_modes", {}) if device is not None else {}
    )
    rx_fields = pdo_field_names(pdo_set, "rx") if pdo_set is not None else set()
    tx_fields = pdo_field_names(pdo_set, "tx") if pdo_set is not None else set()
    for mode in modes:
        if not isinstance(mode, dict):
            check.errors.append(f"运行方案 {operation_id} 的模式条目必须是对象")
            continue
        mode_id = mode.get("id")
        mode_label = mode_id if non_empty_string(mode_id) else "<未知模式>"
        mode_value = mode.get("value")
        mode_valid = (
            non_empty_string(mode_id)
            and mode_id not in mode_ids
            and isinstance(mode_value, int)
            and not isinstance(mode_value, bool)
            and -128 <= mode_value <= 127
        )
        check.require(mode_valid, f"运行方案 {operation_id} 的模式 {mode_label} 无效或重复")
        if non_empty_string(mode_id):
            mode_ids.add(mode_id)
            if isinstance(supported_modes, dict) and mode_id in supported_modes:
                check.require(
                    mode_value == supported_modes[mode_id],
                    f"运行方案 {operation_id} 的模式 {mode_id} 数值与设备目录不一致",
                )
            else:
                check.errors.append(f"运行方案 {operation_id} 的模式 {mode_id} 未在设备目录声明")
        for field_name, available in (
            ("required_rx_fields", rx_fields),
            ("required_tx_fields", tx_fields),
        ):
            fields = mode.get(field_name)
            fields_valid = isinstance(fields, list) and all(
                non_empty_string(field) for field in fields
            )
            check.require(
                fields_valid,
                f"运行方案 {operation_id} 的模式 {mode_label} 的 {field_name} 必须是字符串数组",
            )
            if fields_valid:
                check.require(
                    len(fields) == len(set(fields)),
                    f"运行方案 {operation_id} 的模式 {mode_label} 的 {field_name} 不能重复",
                )
                for field in fields:
                    check.require(
                        field in available,
                        f"运行方案 {operation_id} 的模式 {mode_label} 引用了不存在的 PDO 字段：{field}",
                    )
        check.require(
            mode.get("mode_display_policy") in ("required", "diagnostic"),
            f"运行方案 {operation_id} 的模式 {mode_label} 必须明确声明 "
            "mode_display_policy 为 required 或 diagnostic",
        )
        sdo_writes = mode.get("safeop_to_op_sdo_writes", [])
        check.require(
            isinstance(sdo_writes, list),
            f"运行方案 {operation_id} 的模式 {mode_label} 的 safeop_to_op_sdo_writes 必须是数组",
        )
        if isinstance(sdo_writes, list):
            addresses: set[tuple[int, int]] = set()
            for command in sdo_writes:
                if not isinstance(command, dict):
                    check.errors.append(
                        f"运行方案 {operation_id} 的模式 {mode_label} 包含无效 SDO 写入"
                    )
                    continue
                index_valid = validate_hex_value(
                    check, command.get("index"),
                    f"运行方案 {operation_id} 的模式 {mode_label} SDO index", 0xFFFF,
                )
                subindex = command.get("subindex")
                subindex_valid = (
                    isinstance(subindex, int) and not isinstance(subindex, bool) and
                    0 <= subindex <= 0xFF
                )
                check.require(
                    subindex_valid,
                    f"运行方案 {operation_id} 的模式 {mode_label} SDO subindex 超出范围",
                )
                value_type = command.get("type")
                limits = {
                    "u8": (0, (1 << 8) - 1),
                    "i8": (-(1 << 7), (1 << 7) - 1),
                    "u16": (0, (1 << 16) - 1),
                    "i16": (-(1 << 15), (1 << 15) - 1),
                    "u32": (0, (1 << 32) - 1),
                    "i32": (-(1 << 31), (1 << 31) - 1),
                }
                check.require(
                    value_type in limits,
                    f"运行方案 {operation_id} 的模式 {mode_label} SDO 类型无效",
                )
                configured_value = command.get("value")
                try:
                    if isinstance(configured_value, str) and configured_value.startswith("0x"):
                        parsed_value = hex_value(configured_value)
                    elif isinstance(configured_value, int) and not isinstance(configured_value, bool):
                        parsed_value = configured_value
                    else:
                        raise ValueError
                    value_valid = value_type in limits and limits[value_type][0] <= parsed_value <= limits[value_type][1]
                except ValueError:
                    value_valid = False
                check.require(
                    value_valid,
                    f"运行方案 {operation_id} 的模式 {mode_label} SDO value 与类型不匹配",
                )
                if index_valid and subindex_valid:
                    address = (hex_value(command["index"]), subindex)
                    check.require(
                        address not in addresses,
                        f"运行方案 {operation_id} 的模式 {mode_label} SDO 地址重复",
                    )
                    addresses.add(address)
        final_reads = mode.get("final_sdo_reads", [])
        check.require(
            isinstance(final_reads, list),
            f"运行方案 {operation_id} 的模式 {mode_label} 的 final_sdo_reads 必须是数组",
        )
        if isinstance(final_reads, list):
            names: set[str] = set()
            addresses: set[tuple[int, int]] = set()
            for read in final_reads:
                if not isinstance(read, dict):
                    check.errors.append(
                        f"运行方案 {operation_id} 的模式 {mode_label} 包含无效诊断读取"
                    )
                    continue
                name = read.get("name")
                index_valid = validate_hex_value(
                    check, read.get("index"),
                    f"运行方案 {operation_id} 的模式 {mode_label} 诊断读取 index",
                    0xFFFF,
                )
                subindex = read.get("subindex")
                subindex_valid = (
                    isinstance(subindex, int) and not isinstance(subindex, bool)
                    and 0 <= subindex <= 0xFF
                )
                check.require(
                    non_empty_string(name) and name not in names,
                    f"运行方案 {operation_id} 的模式 {mode_label} 诊断读取名称无效或重复",
                )
                check.require(
                    subindex_valid,
                    f"运行方案 {operation_id} 的模式 {mode_label} 诊断读取 subindex 超出范围",
                )
                check.require(
                    read.get("type") in ("u8", "i8", "u16", "i16", "u32", "i32"),
                    f"运行方案 {operation_id} 的模式 {mode_label} 诊断读取类型无效",
                )
                if non_empty_string(name):
                    names.add(name)
                if index_valid and subindex_valid:
                    address = (hex_value(read["index"]), subindex)
                    check.require(
                        address not in addresses,
                        f"运行方案 {operation_id} 的模式 {mode_label} 诊断读取地址重复",
                    )
                    addresses.add(address)


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
    """校验拓扑集合并建立稳定 ID 索引。"""
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


def validate_operations(
    check: Validation,
    operations: list[dict[str, Any]],
    profiles: dict[str, dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    """校验运行方案集合并建立稳定 ID 索引。"""
    by_id: dict[str, dict[str, Any]] = {}
    for operation in operations:
        operation_id_value = operation.get("operation_profile_id")
        operation_id = operation_id_value if non_empty_string(operation_id_value) else ""
        check.require(
            not operation_id or operation_id not in by_id,
            f"运行方案 ID 重复：{operation_id}",
        )
        validate_operation_profile(check, operation, profiles)
        if operation_id and operation_id not in by_id:
            by_id[operation_id] = operation
    return by_id


def validate_motion_profiles(
    check: Validation, motions: list[dict[str, Any]]
) -> dict[str, dict[str, Any]]:
    """校验独立运动方案，不给任何角度、时间或误差边界补默认值。"""
    by_id: dict[str, dict[str, Any]] = {}
    for motion in motions:
        profile_id_value = motion.get("motion_profile_id")
        profile_id = profile_id_value if non_empty_string(profile_id_value) else ""
        check.require(motion.get("schema_version") == 1, f"{profile_id} 使用不支持的运动方案版本")
        check.require(bool(profile_id), "运动方案配置缺少 motion_profile_id")
        check.require(
            not profile_id or profile_id not in by_id,
            f"运动方案 ID 重复：{profile_id}",
        )
        check.require(
            motion.get("status") in ("draft", "approved"),
            f"运动方案 {profile_id} 的 status 必须是 draft 或 approved",
        )
        check.require(
            non_empty_string(motion.get("required_mode_id")),
            f"运动方案 {profile_id} 缺少 required_mode_id",
        )
        check.require(
            motion.get("trajectory") == "relative_linear_position",
            f"运动方案 {profile_id} 只支持 relative_linear_position",
        )
        check.require(
            motion.get("coordinate_frame") in ("motor_rotor", "output_shaft"),
            f"运动方案 {profile_id} 的坐标系必须是 motor_rotor 或 output_shaft",
        )
        duration_ms = motion.get("duration_ms")
        settle_ms = motion.get("settle_ms")
        check.require(
            isinstance(duration_ms, int)
            and not isinstance(duration_ms, bool)
            and 0 < duration_ms <= 0xFFFFFFFF,
            f"运动方案 {profile_id} 的 duration_ms 必须是正整数",
        )
        check.require(
            isinstance(settle_ms, int)
            and not isinstance(settle_ms, bool)
            and 0 <= settle_ms <= 0xFFFFFFFF,
            f"运动方案 {profile_id} 的 settle_ms 必须是非负整数",
        )
        axes = motion.get("axes")
        check.require(
            isinstance(axes, list) and bool(axes),
            f"运动方案 {profile_id} 的 axes 必须是非空数组",
        )
        axis_ids: set[str] = set()
        if isinstance(axes, list):
            for axis in axes:
                if not isinstance(axis, dict):
                    check.errors.append(f"运动方案 {profile_id} 的轴条目必须是对象")
                    continue
                axis_id_value = axis.get("axis_id")
                axis_id = axis_id_value if non_empty_string(axis_id_value) else ""
                relative_angle = axis.get("relative_angle_millidegrees")
                following_error = axis.get("max_following_error_millidegrees")
                expected_scale = axis.get("expected_position_scale")
                check.require(bool(axis_id), f"运动方案 {profile_id} 的轴 ID 不能为空")
                check.require(
                    not axis_id or axis_id not in axis_ids,
                    f"运动方案 {profile_id} 的轴 ID 重复：{axis_id}",
                )
                check.require(
                    isinstance(relative_angle, int)
                    and not isinstance(relative_angle, bool)
                    and -(1 << 31) <= relative_angle < (1 << 31),
                    f"运动方案 {profile_id} 的轴 {axis_id} 相对角度必须是 32 位整数",
                )
                check.require(
                    isinstance(following_error, int)
                    and not isinstance(following_error, bool)
                    and 0 < following_error <= 0xFFFFFFFF,
                    f"运动方案 {profile_id} 的轴 {axis_id} 跟随误差边界必须是正整数",
                )
                check.require(
                    isinstance(expected_scale, dict),
                    f"运动方案 {profile_id} 的轴 {axis_id} 缺少 expected_position_scale",
                )
                if isinstance(expected_scale, dict):
                    for field in (
                        "encoder_increments",
                        "encoder_motor_revolutions",
                        "gear_motor_revolutions",
                        "gear_shaft_revolutions",
                    ):
                        value = expected_scale.get(field)
                        check.require(
                            isinstance(value, int)
                            and not isinstance(value, bool)
                            and 0 < value <= 0xFFFFFFFF,
                            f"运动方案 {profile_id} 的轴 {axis_id} 换算前提 "
                            f"{field} 必须是正整数",
                        )
                if axis_id:
                    axis_ids.add(axis_id)
        if profile_id and profile_id not in by_id:
            by_id[profile_id] = motion
    return by_id


def validate_deployments(
    check: Validation,
    deployments: list[dict[str, Any]],
    topologies: dict[str, dict[str, Any]],
    operations: dict[str, dict[str, Any]],
    motions: dict[str, dict[str, Any]],
) -> None:
    """校验物理部署引用、候选运行方案及审计报告路径的唯一占用。"""
    deployment_ids: set[str] = set()
    occupied_report_paths: set[str] = set()
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
        run_report_path = deployment.get("run_report_path")

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
            non_empty_string(run_report_path),
            f"部署 {deployment_id} 的 run_report_path 必须是非空字符串",
        )
        check.require(
            not management_interface or management_interface != interface,
            f"部署 {deployment_id} 的 EtherCAT 与管理接口不能相同",
        )

        operation_profile_ids = deployment.get("operation_profile_ids", [])
        operation_list_valid = isinstance(operation_profile_ids, list) and all(
            non_empty_string(item) for item in operation_profile_ids
        )
        check.require(
            operation_list_valid,
            f"部署 {deployment_id} 的 operation_profile_ids 必须是字符串数组",
        )
        if operation_list_valid:
            check.require(
                len(operation_profile_ids) == len(set(operation_profile_ids)),
                f"部署 {deployment_id} 的运行方案 ID 不能重复",
            )
            for operation_id in operation_profile_ids:
                operation = operations.get(operation_id)
                check.require(
                    operation is not None,
                    f"部署 {deployment_id} 引用了未知运行方案：{operation_id}",
                )
                if operation is not None:
                    check.require(
                        operation.get("status") == "approved",
                        f"部署 {deployment_id} 只能引用已批准运行方案：{operation_id}",
                    )

            # 非空集合表示启用过程数据会话，必须为拓扑中的每种设备恰好选择一个方案。
            topology = topologies.get(topology_id)
            topology_profile_ids = {
                slave.get("profile_id")
                for slave in topology.get("slaves", [])
                if isinstance(slave, dict) and non_empty_string(slave.get("profile_id"))
            } if isinstance(topology, dict) else set()
            selected_profile_ids = [
                operations[operation_id].get("device_profile_id")
                for operation_id in operation_profile_ids
                if operation_id in operations
            ]
            if operation_profile_ids:
                check.require(
                    len(selected_profile_ids) == len(operation_profile_ids)
                    and len(selected_profile_ids) == len(set(selected_profile_ids)),
                    f"部署 {deployment_id} 对同一设备配置只能启用一个运行方案",
                )
                check.require(
                    set(selected_profile_ids) == topology_profile_ids,
                    f"部署 {deployment_id} 的运行方案必须完整覆盖拓扑中的设备配置",
                )
                cycle_values = {
                    operations[operation_id].get("sync", {}).get("cycle_ns")
                    for operation_id in operation_profile_ids
                    if operation_id in operations
                    and isinstance(operations[operation_id].get("sync"), dict)
                }
                check.require(
                    len(cycle_values) == 1 and None not in cycle_values,
                    f"部署 {deployment_id} 启用的运行方案必须使用相同且已确认的周期",
                )

        motion_profile_id = deployment.get("motion_profile_id")
        check.require(
            motion_profile_id is None or non_empty_string(motion_profile_id),
            f"部署 {deployment_id} 的 motion_profile_id 必须是非空字符串或 null",
        )
        if non_empty_string(motion_profile_id):
            motion = motions.get(motion_profile_id)
            check.require(
                motion is not None,
                f"部署 {deployment_id} 引用了未知运动方案：{motion_profile_id}",
            )
            if motion is not None:
                check.require(
                    motion.get("status") == "approved",
                    f"部署 {deployment_id} 只能引用已批准运动方案：{motion_profile_id}",
                )
                selected_modes = {
                    operations[operation_id].get("selected_mode_id")
                    for operation_id in operation_profile_ids
                    if operation_id in operations
                }
                check.require(
                    selected_modes == {motion.get("required_mode_id")},
                    f"部署 {deployment_id} 的运行模式与运动方案要求不一致",
                )
                topology = topologies.get(topology_id)
                topology_axis_ids = {
                    slave.get("axis_id")
                    for slave in topology.get("slaves", [])
                    if isinstance(slave, dict) and non_empty_string(slave.get("axis_id"))
                } if isinstance(topology, dict) else set()
                motion_axes = motion.get("axes")
                motion_axis_ids = {
                    axis.get("axis_id")
                    for axis in motion_axes
                    if isinstance(axis, dict) and non_empty_string(axis.get("axis_id"))
                } if isinstance(motion_axes, list) else set()
                check.require(
                    motion_axis_ids == topology_axis_ids,
                    f"部署 {deployment_id} 的运动方案必须完整覆盖拓扑中的全部轴",
                )

        # 同一主机和 EtherCAT 网口允许出现多条部署记录：它们是互斥的启动方案，
        # 一次只能运行一个，物理独占由运行时抢占网卡自然保证，不是静态可判定的冲突。
        # 静态可判定的风险是不同方案的审计报告互相覆盖，因此约束报告路径唯一。
        report_path = run_report_path if non_empty_string(run_report_path) else ""
        check.require(
            not report_path or report_path not in occupied_report_paths,
            f"部署 {deployment_id} 的 run_report_path 与其它部署重复：{report_path}",
        )
        if deployment_id:
            deployment_ids.add(deployment_id)
        if report_path:
            occupied_report_paths.add(report_path)
