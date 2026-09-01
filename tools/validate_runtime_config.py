"""校验运行方案、逻辑拓扑和物理部署之间的引用契约。"""

from __future__ import annotations

from typing import Any

from validation_common import (
    EsiRuntimeConstraints,
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
    esi_constraints: dict[str, EsiRuntimeConstraints],
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
    constraints = (
        esi_constraints.get(device_profile_id)
        if non_empty_string(device_profile_id)
        else None
    )
    validate_sync_config(check, operation_id, status, strategy, sync, constraints)
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
    return len(check.errors) == initial_error_count


def validate_sync_config(
    check: Validation,
    operation_id: str,
    status: object,
    strategy: object,
    sync: object,
    constraints: EsiRuntimeConstraints | None,
) -> None:
    """校验可选同步参数；参数存在时必须满足 ESI 明确给出的约束。"""
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
            validate_hex_value(check, value, f"运行方案 {operation_id} 的 {field}", maximum)

    assign_activate = sync.get("assign_activate")
    assign_activate_valid = isinstance(assign_activate, str) and assign_activate.startswith("0x")
    if assign_activate_valid and constraints is not None and strategy in ("sm", "dc"):
        expected_assign_activate = constraints.assign_activate_by_strategy.get(strategy)
        check.require(
            expected_assign_activate is not None
            and hex_value(assign_activate) == expected_assign_activate,
            f"运行方案 {operation_id} 的 assign_activate 与 ESI {strategy} 模式不一致",
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
    if (
        isinstance(cycle_ns, int)
        and not isinstance(cycle_ns, bool)
        and constraints is not None
        and constraints.minimum_cycle_ns is not None
    ):
        check.require(
            cycle_ns >= constraints.minimum_cycle_ns,
            f"运行方案 {operation_id} 的 cycle_ns 小于 ESI 最小周期 "
            f"{constraints.minimum_cycle_ns} ns",
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
            check.require(
                sync.get("sync0_shift_ns") is not None,
                f"已批准 DC 运行方案 {operation_id} 缺少 sync0_shift_ns",
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
    esi_constraints: dict[str, EsiRuntimeConstraints],
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
        validate_operation_profile(check, operation, profiles, esi_constraints)
        if operation_id and operation_id not in by_id:
            by_id[operation_id] = operation
    return by_id


def validate_deployments(
    check: Validation,
    deployments: list[dict[str, Any]],
    topologies: dict[str, dict[str, Any]],
    operations: dict[str, dict[str, Any]],
) -> None:
    """校验物理部署引用、候选运行方案及同一网口的唯一占用。"""
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
