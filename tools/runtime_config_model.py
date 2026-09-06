"""运行配置生成器使用的结构化模型和解析规则。"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def load_documents(paths: list[Path], kind: str) -> list[dict[str, Any]]:
    """按传入顺序读取 JSON 对象，输入错误直接使构建失败。"""
    documents: list[dict[str, Any]] = []
    for path in paths:
        document = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            raise ValueError(f"{kind}配置 {path} 必须是 JSON 对象")
        documents.append(document)
    return documents


def required_string(document: dict[str, Any], field: str, kind: str) -> str:
    """读取非空字符串，避免无效配置进入生成后的只读目录。"""
    value = document.get(field)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{kind}缺少非空字段 {field}")
    return value


def parse_unsigned(value: object, field: str, maximum: int) -> int:
    """解析十六进制无符号参数，并限制到目标 C 类型的范围。"""
    if not isinstance(value, str) or not value.startswith("0x"):
        raise ValueError(f"{field} 必须是十六进制字符串")
    number = int(value, 16)
    if not 0 <= number <= maximum:
        raise ValueError(f"{field} 超出范围")
    return number


def optional_unsigned(value: object, field: str, maximum: int) -> tuple[bool, int]:
    """保留可选无符号参数的“未配置”状态，不擅自补默认值。"""
    if value is None:
        return False, 0
    return True, parse_unsigned(value, field, maximum)


def optional_signed(value: object, field: str) -> tuple[bool, int]:
    """解析可选的 32 位有符号参数，同时返回存在标志。"""
    if value is None:
        return False, 0
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not -(1 << 31) <= value < (1 << 31)
    ):
        raise ValueError(f"{field} 必须是 32 位有符号整数或 null")
    return True, value


def optional_positive_integer(
    value: object, field: str, maximum: int
) -> tuple[bool, int]:
    """解析可选正整数，同时返回存在标志。"""
    if value is None:
        return False, 0
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not 0 < value <= maximum
    ):
        raise ValueError(f"{field} 必须是正整数或 null")
    return True, value


def topology_values(documents: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """提取并稳定排序拓扑，保留由配置决定的任意从站数量。"""
    values = []
    seen: set[str] = set()
    for document in documents:
        topology_id = required_string(document, "topology_id", "拓扑配置")
        if topology_id in seen:
            raise ValueError(f"拓扑 ID 重复：{topology_id}")
        seen.add(topology_id)
        status = required_string(document, "status", f"拓扑 {topology_id}")
        slaves = document.get("slaves")
        if not isinstance(slaves, list) or not slaves:
            raise ValueError(f"拓扑 {topology_id} 的 slaves 必须是非空数组")

        entries = []
        positions: set[int] = set()
        axis_ids: set[str] = set()
        for slave in slaves:
            if not isinstance(slave, dict):
                raise ValueError(f"拓扑 {topology_id} 的从站条目必须是对象")
            position = slave.get("position")
            if (
                not isinstance(position, int)
                or isinstance(position, bool)
                or position <= 0
            ):
                raise ValueError(f"拓扑 {topology_id} 的从站位置必须是正整数")
            axis_id = required_string(slave, "axis_id", f"拓扑 {topology_id} 的从站")
            profile_id = required_string(
                slave, "profile_id", f"拓扑 {topology_id} 的从站"
            )
            if position in positions or axis_id in axis_ids:
                raise ValueError(f"拓扑 {topology_id} 的位置或轴 ID 重复")
            positions.add(position)
            axis_ids.add(axis_id)
            entries.append((position, axis_id, profile_id))

        # 总线位置从 1 开始，但拓扑规模完全取自配置列表，不绑定具体轴数。
        if positions != set(range(1, len(entries) + 1)):
            raise ValueError(f"拓扑 {topology_id} 的位置必须从 1 连续排列")
        entries.sort(key=lambda item: item[0])
        values.append({"id": topology_id, "status": status, "slaves": entries})
    return sorted(values, key=lambda item: item["id"])


def operation_values(
    documents: list[dict[str, Any]],
    profiles: dict[str, dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    """解析运行方案，并校验其设备、PDO 与模式字段引用。"""
    values = []
    seen: set[str] = set()
    for document in documents:
        operation_id = required_string(document, "operation_profile_id", "运行方案")
        if operation_id in seen:
            raise ValueError(f"运行方案 ID 重复：{operation_id}")
        seen.add(operation_id)
        status = required_string(document, "status", f"运行方案 {operation_id}")
        if status not in ("draft", "approved"):
            raise ValueError(
                f"运行方案 {operation_id} 的 status 必须是 draft 或 approved"
            )
        device_profile_id = required_string(
            document, "device_profile_id", f"运行方案 {operation_id}"
        )
        pdo_set_id = required_string(
            document, "pdo_set_id", f"运行方案 {operation_id}"
        )

        device = profiles.get(device_profile_id) if profiles is not None else None
        if profiles is not None and device is None:
            raise ValueError(
                f"运行方案 {operation_id} 引用了未知设备配置：{device_profile_id}"
            )
        pdo_set = None
        if device is not None and isinstance(device.get("pdo_sets"), list):
            pdo_set = next(
                (
                    item
                    for item in device["pdo_sets"]
                    if isinstance(item, dict) and item.get("id") == pdo_set_id
                ),
                None,
            )
        if profiles is not None and pdo_set is None:
            raise ValueError(
                f"运行方案 {operation_id} 的 pdo_set_id 不存在：{pdo_set_id}"
            )

        sync = document.get("sync")
        if not isinstance(sync, dict):
            raise ValueError(f"运行方案 {operation_id} 缺少 sync 对象")
        strategy = required_string(sync, "strategy", f"运行方案 {operation_id} 的 sync")
        if strategy not in ("sm", "dc"):
            raise ValueError(f"运行方案 {operation_id} 的同步策略必须是 sm 或 dc")
        assign_present, assign_activate = optional_unsigned(
            sync.get("assign_activate"),
            f"运行方案 {operation_id} 的 assign_activate",
            0xFFFFFFFF,
        )
        cycle_present, cycle_ns = optional_positive_integer(
            sync.get("cycle_ns"),
            f"运行方案 {operation_id} 的 cycle_ns",
            0xFFFFFFFF,
        )
        phase_present, process_data_phase_ns = optional_positive_integer(
            sync.get("process_data_phase_ns"),
            f"运行方案 {operation_id} 的 process_data_phase_ns",
            0xFFFFFFFF,
        )
        startup_present, dc_startup_cycles = optional_positive_integer(
            sync.get("dc_startup_cycles"),
            f"运行方案 {operation_id} 的 dc_startup_cycles",
            0xFFFFFFFF,
        )
        shift_present, shift_ns = optional_signed(
            sync.get("sync0_shift_ns"),
            f"运行方案 {operation_id} 的 sync0_shift_ns",
        )
        sm2_present, sm2_type = optional_unsigned(
            sync.get("sm2_sync_type"),
            f"运行方案 {operation_id} 的 sm2_sync_type",
            0xFFFF,
        )
        sm3_present, sm3_type = optional_unsigned(
            sync.get("sm3_sync_type"),
            f"运行方案 {operation_id} 的 sm3_sync_type",
            0xFFFF,
        )

        # 草案允许参数为 null；只有显式批准的方案才具备运行资格。
        if status == "approved":
            required_sync_parameters = {
                "assign_activate": assign_present,
                "cycle_ns": cycle_present,
                "sm2_sync_type": sm2_present,
                "sm3_sync_type": sm3_present,
            }
            missing_parameters = [
                field
                for field, present in required_sync_parameters.items()
                if not present
            ]
            if strategy == "dc":
                if not shift_present:
                    missing_parameters.append("sync0_shift_ns")
                if not phase_present:
                    missing_parameters.append("process_data_phase_ns")
                if not startup_present:
                    missing_parameters.append("dc_startup_cycles")
            if missing_parameters:
                raise ValueError(
                    f"已批准运行方案 {operation_id} 缺少同步参数："
                    f"{', '.join(missing_parameters)}"
                )
        if (
            strategy == "dc"
            and cycle_present
            and phase_present
            and process_data_phase_ns >= cycle_ns
        ):
            raise ValueError(
                f"运行方案 {operation_id} 的 process_data_phase_ns 必须小于 cycle_ns"
            )

        modes = document.get("modes", [])
        if not isinstance(modes, list):
            raise ValueError(f"运行方案 {operation_id} 的 modes 必须是数组")
        mode_values = []
        mode_ids: set[str] = set()
        for mode in modes:
            if not isinstance(mode, dict):
                raise ValueError(f"运行方案 {operation_id} 的模式条目必须是对象")
            mode_id = required_string(
                mode, "id", f"运行方案 {operation_id} 的模式"
            )
            mode_value = mode.get("value")
            if (
                mode_id in mode_ids
                or not isinstance(mode_value, int)
                or isinstance(mode_value, bool)
            ):
                raise ValueError(f"运行方案 {operation_id} 的模式 ID 或数值无效")
            if not -128 <= mode_value <= 127:
                raise ValueError(
                    f"运行方案 {operation_id} 的模式值必须是有符号 8 位整数"
                )

            required_rx_fields = mode.get("required_rx_fields", [])
            required_tx_fields = mode.get("required_tx_fields", [])
            for field_name, field_values in (
                ("required_rx_fields", required_rx_fields),
                ("required_tx_fields", required_tx_fields),
            ):
                if not isinstance(field_values, list) or any(
                    not isinstance(field, str) or not field.strip()
                    for field in field_values
                ):
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} 的 "
                        f"{field_name} 必须是字符串数组"
                    )
                if len(field_values) != len(set(field_values)):
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} 的 "
                        f"{field_name} 不能重复"
                    )
                if pdo_set is not None:
                    direction = "rx" if field_name == "required_rx_fields" else "tx"
                    available_fields = {
                        entry.get("name")
                        for mapping in pdo_set.get(direction, {}).get("mappings", [])
                        if isinstance(mapping, dict)
                        for entry in mapping.get("entries", [])
                        if isinstance(entry, dict)
                    }
                    unknown_fields = set(field_values) - available_fields
                    if unknown_fields:
                        raise ValueError(
                            f"运行方案 {operation_id} 的模式 {mode_id} 引用了"
                            f"不存在的 PDO 字段：{', '.join(sorted(unknown_fields))}"
                        )

            mode_display_policy = mode.get("mode_display_policy")
            if mode_display_policy not in ("required", "diagnostic"):
                raise ValueError(
                    f"运行方案 {operation_id} 的模式 {mode_id} 必须明确声明 "
                    "mode_display_policy 为 required 或 diagnostic"
                )

            sdo_writes_value = mode.get("safeop_to_op_sdo_writes", [])
            if not isinstance(sdo_writes_value, list):
                raise ValueError(
                    f"运行方案 {operation_id} 的模式 {mode_id} 的 "
                    "safeop_to_op_sdo_writes 必须是数组"
                )
            sdo_writes = []
            sdo_addresses: set[tuple[int, int]] = set()
            for command in sdo_writes_value:
                if not isinstance(command, dict):
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} 包含无效 SDO 写入"
                    )
                index = parse_unsigned(
                    command.get("index"),
                    f"运行方案 {operation_id} 的模式 {mode_id} SDO index",
                    0xFFFF,
                )
                subindex = command.get("subindex")
                if (
                    not isinstance(subindex, int)
                    or isinstance(subindex, bool)
                    or not 0 <= subindex <= 0xFF
                ):
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} SDO subindex 超出范围"
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
                if value_type not in limits:
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} SDO 类型无效"
                    )
                configured_value = command.get("value")
                if isinstance(configured_value, str) and configured_value.startswith("0x"):
                    value = int(configured_value, 16)
                elif isinstance(configured_value, int) and not isinstance(configured_value, bool):
                    value = configured_value
                else:
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} SDO value 必须是整数或十六进制字符串"
                    )
                minimum, maximum = limits[value_type]
                if not minimum <= value <= maximum:
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} SDO value 超出 {value_type} 范围"
                    )
                address = (index, subindex)
                if address in sdo_addresses:
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} SDO 地址重复"
                    )
                sdo_addresses.add(address)
                sdo_writes.append(
                    {"index": index, "subindex": subindex, "type": value_type, "value": value}
                )

            final_reads_value = mode.get("final_sdo_reads", [])
            if not isinstance(final_reads_value, list):
                raise ValueError(
                    f"运行方案 {operation_id} 的模式 {mode_id} 的 "
                    "final_sdo_reads 必须是数组"
                )
            final_reads = []
            final_read_names: set[str] = set()
            final_read_addresses: set[tuple[int, int]] = set()
            for read in final_reads_value:
                if not isinstance(read, dict):
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} 包含无效诊断读取"
                    )
                name = required_string(
                    read, "name",
                    f"运行方案 {operation_id} 的模式 {mode_id} 诊断读取",
                )
                index = parse_unsigned(
                    read.get("index"),
                    f"运行方案 {operation_id} 的模式 {mode_id} 诊断读取 index",
                    0xFFFF,
                )
                subindex = read.get("subindex")
                value_type = read.get("type")
                if (
                    not isinstance(subindex, int)
                    or isinstance(subindex, bool)
                    or not 0 <= subindex <= 0xFF
                ):
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} "
                        "诊断读取 subindex 超出范围"
                    )
                if value_type not in ("u8", "i8", "u16", "i16", "u32", "i32"):
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} "
                        f"诊断读取 {name} 的类型无效"
                    )
                address = (index, subindex)
                if name in final_read_names or address in final_read_addresses:
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} "
                        "诊断读取名称或地址重复"
                    )
                final_read_names.add(name)
                final_read_addresses.add(address)
                final_reads.append(
                    {
                        "name": name,
                        "index": index,
                        "subindex": subindex,
                        "type": value_type,
                    }
                )

            if device is not None:
                supported_modes = device.get("protocol", {}).get(
                    "supported_modes", {}
                )
                if isinstance(supported_modes, dict) and mode_id not in supported_modes:
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} 未在设备目录声明"
                    )
                if (
                    isinstance(supported_modes, dict)
                    and mode_id in supported_modes
                    and mode_value != supported_modes[mode_id]
                ):
                    raise ValueError(
                        f"运行方案 {operation_id} 的模式 {mode_id} 数值与设备目录不一致"
                    )
            mode_ids.add(mode_id)
            mode_values.append(
                {
                    "id": mode_id,
                    "value": mode_value,
                    "rx_fields": required_rx_fields,
                    "tx_fields": required_tx_fields,
                    "mode_display_policy": mode_display_policy,
                    "safeop_to_op_sdo_writes": sdo_writes,
                    "final_sdo_reads": final_reads,
                }
            )

        selected_mode_id = document.get("selected_mode_id")
        if selected_mode_id is not None and (
            not isinstance(selected_mode_id, str) or not selected_mode_id.strip()
        ):
            raise ValueError(
                f"运行方案 {operation_id} 的 selected_mode_id 必须是非空字符串或 null"
            )
        if selected_mode_id is not None and selected_mode_id not in mode_ids:
            raise ValueError(
                f"运行方案 {operation_id} 的 selected_mode_id 不在 modes 中："
                f"{selected_mode_id}"
            )
        if status == "approved" and selected_mode_id is None:
            raise ValueError(
                f"已批准运行方案 {operation_id} 缺少 selected_mode_id"
            )
        mode_initialization = pdo_set.get("mode_initialization") if pdo_set is not None else None
        if isinstance(mode_initialization, dict) and selected_mode_id is not None:
            selected_mode = next(
                (mode for mode in mode_values if mode["id"] == selected_mode_id),
                None,
            )
            if selected_mode is None or selected_mode["value"] != mode_initialization.get("value"):
                raise ValueError(
                    f"运行方案 {operation_id} 的模式值与所选 PDO 模块初始化值不一致"
                )

        values.append(
            {
                "id": operation_id,
                "status": status,
                "device_profile_id": device_profile_id,
                "pdo_set_id": pdo_set_id,
                "strategy": strategy,
                "assign_present": assign_present,
                "assign_activate": assign_activate,
                "cycle_present": cycle_present,
                "cycle_ns": cycle_ns,
                "phase_present": phase_present,
                "process_data_phase_ns": process_data_phase_ns,
                "startup_present": startup_present,
                "dc_startup_cycles": dc_startup_cycles,
                "shift_present": shift_present,
                "shift_ns": shift_ns,
                "sm2_present": sm2_present,
                "sm2_type": sm2_type,
                "sm3_present": sm3_present,
                "sm3_type": sm3_type,
                "selected_mode_id": selected_mode_id,
                "modes": mode_values,
            }
        )
    return sorted(values, key=lambda item: item["id"])


def motion_values(documents: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """解析独立运动方案，所有轨迹和边界参数都必须由配置显式给出。"""
    values: list[dict[str, Any]] = []
    seen: set[str] = set()
    for document in documents:
        profile_id = required_string(document, "motion_profile_id", "运动方案")
        if profile_id in seen:
            raise ValueError(f"运动方案 ID 重复：{profile_id}")
        seen.add(profile_id)
        status = required_string(document, "status", f"运动方案 {profile_id}")
        if status not in ("draft", "approved"):
            raise ValueError(f"运动方案 {profile_id} 的 status 必须是 draft 或 approved")
        required_mode_id = required_string(
            document, "required_mode_id", f"运动方案 {profile_id}"
        )
        trajectory = required_string(document, "trajectory", f"运动方案 {profile_id}")
        if trajectory != "relative_linear_position":
            raise ValueError(f"运动方案 {profile_id} 使用了不支持的轨迹：{trajectory}")
        coordinate = required_string(document, "coordinate_frame", f"运动方案 {profile_id}")
        if coordinate not in ("motor_rotor", "output_shaft"):
            raise ValueError(
                f"运动方案 {profile_id} 的 coordinate_frame 必须是 motor_rotor 或 output_shaft"
            )

        duration_ms = document.get("duration_ms")
        settle_ms = document.get("settle_ms")
        if (
            not isinstance(duration_ms, int)
            or isinstance(duration_ms, bool)
            or not 0 < duration_ms <= 0xFFFFFFFF
        ):
            raise ValueError(f"运动方案 {profile_id} 的 duration_ms 必须是正整数")
        if (
            not isinstance(settle_ms, int)
            or isinstance(settle_ms, bool)
            or not 0 <= settle_ms <= 0xFFFFFFFF
        ):
            raise ValueError(f"运动方案 {profile_id} 的 settle_ms 必须是非负整数")

        axes = document.get("axes")
        if not isinstance(axes, list) or not axes:
            raise ValueError(f"运动方案 {profile_id} 的 axes 必须是非空数组")
        axis_values: list[dict[str, Any]] = []
        axis_ids: set[str] = set()
        for axis in axes:
            if not isinstance(axis, dict):
                raise ValueError(f"运动方案 {profile_id} 的轴条目必须是对象")
            axis_id = required_string(axis, "axis_id", f"运动方案 {profile_id} 的轴")
            relative_angle = axis.get("relative_angle_millidegrees")
            following_error = axis.get("max_following_error_millidegrees")
            expected_scale = axis.get("expected_position_scale")
            if axis_id in axis_ids:
                raise ValueError(f"运动方案 {profile_id} 的轴 ID 重复：{axis_id}")
            if (
                not isinstance(relative_angle, int)
                or isinstance(relative_angle, bool)
                or not -(1 << 31) <= relative_angle < (1 << 31)
            ):
                raise ValueError(
                    f"运动方案 {profile_id} 的轴 {axis_id} 相对角度必须是 32 位整数"
                )
            if (
                not isinstance(following_error, int)
                or isinstance(following_error, bool)
                or not 0 < following_error <= 0xFFFFFFFF
            ):
                raise ValueError(
                    f"运动方案 {profile_id} 的轴 {axis_id} 跟随误差边界必须是正整数"
                )
            if not isinstance(expected_scale, dict):
                raise ValueError(
                    f"运动方案 {profile_id} 的轴 {axis_id} 缺少 expected_position_scale"
                )
            scale_values: dict[str, int] = {}
            for field in (
                "encoder_increments",
                "encoder_motor_revolutions",
                "gear_motor_revolutions",
                "gear_shaft_revolutions",
            ):
                value = expected_scale.get(field)
                if (
                    not isinstance(value, int)
                    or isinstance(value, bool)
                    or not 0 < value <= 0xFFFFFFFF
                ):
                    raise ValueError(
                        f"运动方案 {profile_id} 的轴 {axis_id} 换算前提 {field} "
                        "必须是正整数"
                    )
                scale_values[field] = value
            axis_ids.add(axis_id)
            axis_values.append(
                {
                    "axis_id": axis_id,
                    "relative_angle_millidegrees": relative_angle,
                    "max_following_error_millidegrees": following_error,
                    "expected_position_scale": scale_values,
                }
            )
        values.append(
            {
                "id": profile_id,
                "status": status,
                "required_mode_id": required_mode_id,
                "trajectory": trajectory,
                "coordinate": coordinate,
                "duration_ms": duration_ms,
                "settle_ms": settle_ms,
                "axes": sorted(axis_values, key=lambda item: item["axis_id"]),
            }
        )
    return sorted(values, key=lambda item: item["id"])


def deployment_values(
    documents: list[dict[str, Any]],
    topologies: list[dict[str, Any]],
    operations: list[dict[str, Any]],
    motions: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """解析部署绑定，并确保部署只引用已批准且完整覆盖拓扑的方案。"""
    values = []
    seen: set[str] = set()
    topology_by_id = {topology["id"]: topology for topology in topologies}
    operation_by_id = {operation["id"]: operation for operation in operations}
    operation_ids = set(operation_by_id)
    motion_by_id = {motion["id"]: motion for motion in motions}
    for document in documents:
        deployment_id = required_string(document, "deployment_id", "部署配置")
        if deployment_id in seen:
            raise ValueError(f"部署 ID 重复：{deployment_id}")
        seen.add(deployment_id)
        topology_id = required_string(
            document, "topology_id", f"部署 {deployment_id}"
        )
        if topology_id not in topology_by_id:
            raise ValueError(f"部署 {deployment_id} 引用了未知拓扑：{topology_id}")

        management = document.get("management_interface")
        if management is not None and (
            not isinstance(management, str) or not management.strip()
        ):
            raise ValueError(
                f"部署 {deployment_id} 的 management_interface 必须是非空字符串"
            )
        operation_profile_ids = document.get("operation_profile_ids", [])
        if not isinstance(operation_profile_ids, list) or any(
            not isinstance(item, str) or not item.strip()
            for item in operation_profile_ids
        ):
            raise ValueError(
                f"部署 {deployment_id} 的 operation_profile_ids 必须是字符串数组"
            )
        if len(operation_profile_ids) != len(set(operation_profile_ids)):
            raise ValueError(f"部署 {deployment_id} 的运行方案 ID 不能重复")
        unknown = set(operation_profile_ids) - operation_ids
        if unknown:
            raise ValueError(
                f"部署 {deployment_id} 引用了未知运行方案："
                f"{', '.join(sorted(unknown))}"
            )
        unapproved = [
            operation_id
            for operation_id in operation_profile_ids
            if operation_by_id[operation_id]["status"] != "approved"
        ]
        if unapproved:
            raise ValueError(
                f"部署 {deployment_id} 只能引用已批准运行方案："
                f"{', '.join(unapproved)}"
            )

        if operation_profile_ids:
            selected_profile_ids = [
                operation_by_id[operation_id]["device_profile_id"]
                for operation_id in operation_profile_ids
            ]
            if len(selected_profile_ids) != len(set(selected_profile_ids)):
                raise ValueError(
                    f"部署 {deployment_id} 对同一设备配置只能启用一个运行方案"
                )
            topology_profile_ids = {
                profile_id
                for _, _, profile_id in topology_by_id[topology_id]["slaves"]
            }
            if set(selected_profile_ids) != topology_profile_ids:
                raise ValueError(
                    f"部署 {deployment_id} 的运行方案必须完整覆盖拓扑中的设备配置"
                )
            cycle_values = {
                operation_by_id[operation_id]["cycle_ns"]
                for operation_id in operation_profile_ids
            }
            if len(cycle_values) != 1:
                raise ValueError(
                    f"部署 {deployment_id} 启用的运行方案必须使用相同周期"
                )
            dc_phase_values = {
                operation_by_id[operation_id]["process_data_phase_ns"]
                for operation_id in operation_profile_ids
                if operation_by_id[operation_id]["strategy"] == "dc"
            }
            if len(dc_phase_values) > 1:
                raise ValueError(
                    f"部署 {deployment_id} 启用的 DC 运行方案必须使用相同过程帧相位"
                )

        motion_profile_id = document.get("motion_profile_id")
        if motion_profile_id is not None and (
            not isinstance(motion_profile_id, str) or not motion_profile_id.strip()
        ):
            raise ValueError(
                f"部署 {deployment_id} 的 motion_profile_id 必须是非空字符串或 null"
            )
        if motion_profile_id is not None:
            motion = motion_by_id.get(motion_profile_id)
            if motion is None:
                raise ValueError(f"部署 {deployment_id} 引用了未知运动方案：{motion_profile_id}")
            if motion["status"] != "approved":
                raise ValueError(f"部署 {deployment_id} 只能引用已批准运动方案：{motion_profile_id}")
            selected_modes = {
                operation_by_id[operation_id]["selected_mode_id"]
                for operation_id in operation_profile_ids
            }
            if selected_modes != {motion["required_mode_id"]}:
                raise ValueError(
                    f"部署 {deployment_id} 的运行模式与运动方案要求不一致"
                )
            topology_axis_ids = {
                axis_id for _, axis_id, _ in topology_by_id[topology_id]["slaves"]
            }
            motion_axis_ids = {axis["axis_id"] for axis in motion["axes"]}
            if motion_axis_ids != topology_axis_ids:
                raise ValueError(
                    f"部署 {deployment_id} 的运动方案必须完整覆盖拓扑中的全部轴"
                )

        values.append(
            {
                "id": deployment_id,
                "hostname": required_string(
                    document, "hostname", f"部署 {deployment_id}"
                ),
                "interface": required_string(
                    document, "ethercat_interface", f"部署 {deployment_id}"
                ),
                "management": management or "",
                "run_report_path": required_string(
                    document, "run_report_path", f"部署 {deployment_id}"
                ),
                "topology": topology_id,
                "operation_profile_ids": operation_profile_ids,
                "motion_profile_id": motion_profile_id,
            }
        )
    return sorted(values, key=lambda item: item["id"])
