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
            if strategy == "dc" and not shift_present:
                missing_parameters.append("sync0_shift_ns")
            if missing_parameters:
                raise ValueError(
                    f"已批准运行方案 {operation_id} 缺少同步参数："
                    f"{', '.join(missing_parameters)}"
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


def deployment_values(
    documents: list[dict[str, Any]],
    topologies: list[dict[str, Any]],
    operations: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """解析部署绑定，并确保部署只引用已批准的运行方案。"""
    values = []
    seen: set[str] = set()
    topology_by_id = {topology["id"]: topology for topology in topologies}
    operation_by_id = {operation["id"]: operation for operation in operations}
    operation_ids = set(operation_by_id)
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
                "topology": topology_id,
                "operation_profile_ids": operation_profile_ids,
            }
        )
    return sorted(values, key=lambda item: item["id"])
