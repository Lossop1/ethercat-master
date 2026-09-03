#!/usr/bin/env python3
"""把运行配置模型生成为编译期只读 C 目录。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from runtime_config_model import (
    deployment_values,
    load_documents,
    motion_values,
    operation_values,
    required_string,
    topology_values,
)


def c_string(value: str) -> str:
    """使用 JSON 转义规则生成可移植的 C 字符串字面量。"""
    return json.dumps(value, ensure_ascii=True)


def c_int32(value: int) -> str:
    """生成可移植的 32 位有符号整数常量，包括无法直接取正值的 INT32_MIN。"""
    if value == -(1 << 31):
        return "(-INT32_C(2147483647) - INT32_C(1))"
    if value < 0:
        return f"(-INT32_C({-value}))"
    return f"INT32_C({value})"


def operation_declarations(operations: list[dict[str, Any]]) -> list[str]:
    """生成各运行模式使用的 PDO 字段名数组和模式数组。"""
    declarations: list[str] = []
    for ordinal, operation in enumerate(operations):
        modes_name = f"operation_{ordinal}_modes"
        mode_initializers = []
        for mode_ordinal, mode in enumerate(operation["modes"]):
            rx_name = f"operation_{ordinal}_mode_{mode_ordinal}_rx_fields"
            tx_name = f"operation_{ordinal}_mode_{mode_ordinal}_tx_fields"
            sdo_name = f"operation_{ordinal}_mode_{mode_ordinal}_safeop_to_op_sdos"
            read_name = f"operation_{ordinal}_mode_{mode_ordinal}_final_sdo_reads"
            if mode["rx_fields"]:
                declarations.append(
                    f"static const char *const {rx_name}[] = {{\n"
                    + ",\n".join(
                        f"    {c_string(field)}" for field in mode["rx_fields"]
                    )
                    + "\n};"
                )
                rx_pointer = rx_name
                rx_count = f"sizeof({rx_name}) / sizeof({rx_name}[0])"
            else:
                rx_pointer = "NULL"
                rx_count = "0U"

            if mode["tx_fields"]:
                declarations.append(
                    f"static const char *const {tx_name}[] = {{\n"
                    + ",\n".join(
                        f"    {c_string(field)}" for field in mode["tx_fields"]
                    )
                    + "\n};"
                )
                tx_pointer = tx_name
                tx_count = f"sizeof({tx_name}) / sizeof({tx_name}[0])"
            else:
                tx_pointer = "NULL"
                tx_count = "0U"

            if mode["safeop_to_op_sdo_writes"]:
                declarations.append(
                    f"static const emaster_sdo_write_config_t {sdo_name}[] = {{\n"
                    + ",\n".join(
                        "    {"
                        f"UINT16_C(0x{command['index']:04X}), "
                        f"UINT8_C({command['subindex']}), EMASTER_CONFIG_SDO_VALUE_U16, "
                        f"UINT16_C(0x{command['value']:04X})"
                        "}"
                        for command in mode["safeop_to_op_sdo_writes"]
                    )
                    + "\n};"
                )
                sdo_pointer = sdo_name
                sdo_count = f"sizeof({sdo_name}) / sizeof({sdo_name}[0])"
            else:
                sdo_pointer = "NULL"
                sdo_count = "0U"

            if mode["final_sdo_reads"]:
                declarations.append(
                    f"static const emaster_sdo_read_config_t {read_name}[] = {{\n"
                    + ",\n".join(
                        "    {"
                        f"{c_string(read['name'])}, "
                        f"UINT16_C(0x{read['index']:04X}), "
                        f"UINT8_C({read['subindex']}), "
                        f"EMASTER_CONFIG_SDO_VALUE_{read['type'].upper()}"
                        "}"
                        for read in mode["final_sdo_reads"]
                    )
                    + "\n};"
                )
                read_pointer = read_name
                read_count = f"sizeof({read_name}) / sizeof({read_name}[0])"
            else:
                read_pointer = "NULL"
                read_count = "0U"

            mode_initializers.append(
                f"    {{{c_string(mode['id'])}, INT8_C({mode['value']}), "
                f"{rx_pointer}, {rx_count}, {tx_pointer}, {tx_count}, "
                f"EMASTER_MODE_DISPLAY_{mode['mode_display_policy'].upper()}, "
                f"{sdo_pointer}, {sdo_count}, {read_pointer}, {read_count}}}"
            )
        if mode_initializers:
            rendered_modes = ",\n".join(mode_initializers)
            declarations.append(
                f"static const emaster_operation_mode_t {modes_name}[] = {{\n"
                f"{rendered_modes}\n}};"
            )
    return declarations


def operation_initializers(operations: list[dict[str, Any]]) -> str:
    """生成运行方案结构体初始化器，显式保留参数存在标志。"""
    rendered = []
    for ordinal, operation in enumerate(operations):
        mode_count = (
            f"sizeof(operation_{ordinal}_modes) / "
            f"sizeof(operation_{ordinal}_modes[0])"
            if operation["modes"]
            else "0U"
        )
        selected_mode_id = (
            c_string(operation["selected_mode_id"])
            if operation["selected_mode_id"] is not None
            else "NULL"
        )
        rendered.append(
            f"""    {{
        .profile_id = {c_string(operation['id'])},
        .approval = EMASTER_OPERATION_PROFILE_{operation['status'].upper()},
        .device_profile_id = {c_string(operation['device_profile_id'])},
        .pdo_set_id = {c_string(operation['pdo_set_id'])},
        .sync_strategy = EMASTER_SYNC_STRATEGY_{operation['strategy'].upper()},
        .has_assign_activate = {'true' if operation['assign_present'] else 'false'},
        .assign_activate = UINT32_C(0x{operation['assign_activate']:08X}),
        .has_cycle_ns = {'true' if operation['cycle_present'] else 'false'},
        .cycle_ns = UINT32_C({operation['cycle_ns']}),
        .has_process_data_phase_ns = {'true' if operation['phase_present'] else 'false'},
        .process_data_phase_ns = UINT32_C({operation['process_data_phase_ns']}),
        .has_dc_startup_cycles = {'true' if operation['startup_present'] else 'false'},
        .dc_startup_cycles = UINT32_C({operation['dc_startup_cycles']}),
        .has_sync0_shift_ns = {'true' if operation['shift_present'] else 'false'},
        .sync0_shift_ns = {c_int32(operation['shift_ns'])},
        .has_sm2_sync_type = {'true' if operation['sm2_present'] else 'false'},
        .sm2_sync_type = UINT16_C(0x{operation['sm2_type']:04X}),
        .has_sm3_sync_type = {'true' if operation['sm3_present'] else 'false'},
        .sm3_sync_type = UINT16_C(0x{operation['sm3_type']:04X}),
        .selected_mode_id = {selected_mode_id},
        .modes = {f"operation_{ordinal}_modes" if operation['modes'] else 'NULL'},
        .mode_count = {mode_count},
    }}"""
        )
    return ",\n".join(rendered)


def motion_declarations(motions: list[dict[str, Any]]) -> tuple[str, str]:
    """生成运动方案的每轴参数数组和只读目录。"""
    declarations: list[str] = []
    initializers: list[str] = []
    for ordinal, motion in enumerate(motions):
        axes_name = f"motion_{ordinal}_axes"
        declarations.append(
            f"static const emaster_motion_axis_config_t {axes_name}[] = {{\n"
            + ",\n".join(
                "    {"
                f"{c_string(axis['axis_id'])}, "
                f"{c_int32(axis['relative_angle_millidegrees'])}, "
                f"UINT32_C({axis['max_following_error_millidegrees']}), "
                f"UINT32_C({axis['expected_position_scale']['encoder_increments']}), "
                f"UINT32_C({axis['expected_position_scale']['encoder_motor_revolutions']}), "
                f"UINT32_C({axis['expected_position_scale']['gear_motor_revolutions']}), "
                f"UINT32_C({axis['expected_position_scale']['gear_shaft_revolutions']})"
                "}"
                for axis in motion["axes"]
            )
            + "\n};"
        )
        initializers.append(
            "    {"
            f"{c_string(motion['id'])}, "
            f"EMASTER_MOTION_PROFILE_{motion['status'].upper()}, "
            f"{c_string(motion['required_mode_id'])}, "
            f"EMASTER_MOTION_TRAJECTORY_{motion['trajectory'].upper()}, "
            f"EMASTER_MOTION_COORDINATE_{motion['coordinate'].upper()}, "
            f"UINT32_C({motion['duration_ms']}), UINT32_C({motion['settle_ms']}), "
            f"{axes_name}, sizeof({axes_name}) / sizeof({axes_name}[0])"
            "}"
        )
    if motions:
        array = (
            "static const emaster_motion_profile_t motion_profiles[] = {\n"
            + ",\n".join(initializers)
            + "\n};"
        )
    else:
        array = "static const emaster_motion_profile_t motion_profiles[1] = {{0}};"
    return "\n\n".join(declarations), array


def render_topologies(
    topologies: list[dict[str, Any]],
) -> tuple[str, str]:
    """渲染从站数组以及引用这些数组的拓扑初始化器。"""
    arrays = []
    initializers = []
    for ordinal, topology in enumerate(topologies):
        entries = ",\n".join(
            f"    {{UINT16_C({position}), {c_string(axis_id)}, "
            f"{c_string(profile_id)}}}"
            for position, axis_id, profile_id in topology["slaves"]
        )
        arrays.append(
            "static const emaster_topology_slave_config_t "
            f"topology_{ordinal}_slaves[] = {{\n{entries}\n}};"
        )
        initializers.append(
            "    {"
            + c_string(topology["id"])
            + ", "
            + c_string(topology["status"])
            + f", topology_{ordinal}_slaves, "
            + f"sizeof(topology_{ordinal}_slaves) / "
            + f"sizeof(topology_{ordinal}_slaves[0])"
            + "}"
        )
    return "\n\n".join(arrays), ",\n".join(initializers)


def render_deployments(
    deployments: list[dict[str, Any]],
    topologies: list[dict[str, Any]],
    operations: list[dict[str, Any]],
    motions: list[dict[str, Any]],
) -> tuple[str, str]:
    """渲染部署允许使用的方案指针数组和部署初始化器。"""
    topology_ordinals = {
        topology["id"]: ordinal for ordinal, topology in enumerate(topologies)
    }
    operation_ordinals = {
        operation["id"]: ordinal for ordinal, operation in enumerate(operations)
    }
    motion_ordinals = {motion["id"]: ordinal for ordinal, motion in enumerate(motions)}
    pointer_arrays: list[str] = []
    initializers = []
    for ordinal, deployment in enumerate(deployments):
        operation_ids = deployment["operation_profile_ids"]
        if operation_ids:
            pointers = ",\n".join(
                f"    &operation_profiles[{operation_ordinals[operation_id]}]"
                for operation_id in operation_ids
            )
            pointer_arrays.append(
                "static const emaster_operation_profile_t "
                f"*deployment_{ordinal}_operations[] = {{\n{pointers}\n}};"
            )
            operation_pointer = f"deployment_{ordinal}_operations"
            operation_count = (
                f"sizeof(deployment_{ordinal}_operations) / "
                f"sizeof(deployment_{ordinal}_operations[0])"
            )
        else:
            operation_pointer = "NULL"
            operation_count = "0U"

        topology_ordinal = topology_ordinals[deployment["topology"]]
        motion_pointer = (
            f"&motion_profiles[{motion_ordinals[deployment['motion_profile_id']]}]"
            if deployment["motion_profile_id"] is not None
            else "NULL"
        )
        initializers.append(
            "    {"
            + c_string(deployment["id"])
            + ", "
            + c_string(deployment["hostname"])
            + ", "
            + c_string(deployment["interface"])
            + ", "
            + c_string(deployment["management"])
            + f", &topologies[{topology_ordinal}], "
            + f"{operation_pointer}, {operation_count}, {motion_pointer}, "
            + c_string(deployment["run_report_path"])
            + "}"
        )
    return "\n\n".join(pointer_arrays), ",\n".join(initializers)


def generate(
    topology_documents: list[dict[str, Any]],
    deployment_documents: list[dict[str, Any]],
    operation_documents: list[dict[str, Any]],
    motion_documents: list[dict[str, Any]],
    device_documents: list[dict[str, Any]] | None = None,
) -> str:
    """解析各层配置，并生成只读目录及其查找函数。"""
    topologies = topology_values(topology_documents)
    profiles = None
    if device_documents is not None:
        profiles = {
            required_string(document, "profile_id", "设备配置"): document
            for document in device_documents
        }
    operations = operation_values(operation_documents, profiles)
    motions = motion_values(motion_documents)
    deployments = deployment_values(
        deployment_documents,
        topologies,
        operations,
        motions,
    )

    topology_arrays, topology_initializers = render_topologies(topologies)
    operation_declaration_text = "\n\n".join(operation_declarations(operations))
    motion_declaration_text, motion_array = motion_declarations(motions)
    operation_array = (
        "static const emaster_operation_profile_t operation_profiles[] = {\n"
        + operation_initializers(operations)
        + "\n};"
    )
    operation_pointers, deployment_initializers = render_deployments(
        deployments, topologies, operations, motions
    )
    return f"""/* 由 tools/generate_runtime_config.py 生成，禁止手工修改。 */
#include "emaster/config/runtime_config.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

{topology_arrays}

{operation_declaration_text}

{operation_array}

{motion_declaration_text}

{motion_array}

static const emaster_topology_config_t topologies[] = {{
{topology_initializers},
}};

{operation_pointers}

static const emaster_deployment_config_t deployments[] = {{
{deployment_initializers},
}};

size_t emaster_topology_config_count(void)
{{
    return sizeof(topologies) / sizeof(topologies[0]);
}}

const emaster_topology_config_t *emaster_topology_config_at(size_t index)
{{
    return index < emaster_topology_config_count() ? &topologies[index] : NULL;
}}

const emaster_topology_config_t *emaster_topology_config_by_id(const char *topology_id)
{{
    size_t index;
    if (topology_id == NULL)
    {{
        return NULL;
    }}
    for (index = 0U; index < emaster_topology_config_count(); ++index)
    {{
        if (strcmp(topologies[index].topology_id, topology_id) == 0)
        {{
            return &topologies[index];
        }}
    }}
    return NULL;
}}

size_t emaster_operation_profile_count(void)
{{
    return sizeof(operation_profiles) / sizeof(operation_profiles[0]);
}}

const emaster_operation_profile_t *emaster_operation_profile_at(size_t index)
{{
    return index < emaster_operation_profile_count() ? &operation_profiles[index] : NULL;
}}

const emaster_operation_profile_t *emaster_operation_profile_by_id(const char *profile_id)
{{
    size_t index;
    if (profile_id == NULL)
    {{
        return NULL;
    }}
    for (index = 0U; index < emaster_operation_profile_count(); ++index)
    {{
        if (strcmp(operation_profiles[index].profile_id, profile_id) == 0)
        {{
            return &operation_profiles[index];
        }}
    }}
    return NULL;
}}

size_t emaster_motion_profile_count(void)
{{
    return {len(motions)}U;
}}

const emaster_motion_profile_t *emaster_motion_profile_at(size_t index)
{{
    return index < emaster_motion_profile_count() ? &motion_profiles[index] : NULL;
}}

const emaster_motion_profile_t *emaster_motion_profile_by_id(const char *profile_id)
{{
    size_t index;
    if (profile_id == NULL)
    {{
        return NULL;
    }}
    for (index = 0U; index < emaster_motion_profile_count(); ++index)
    {{
        if (strcmp(motion_profiles[index].motion_profile_id, profile_id) == 0)
        {{
            return &motion_profiles[index];
        }}
    }}
    return NULL;
}}

size_t emaster_deployment_config_count(void)
{{
    return sizeof(deployments) / sizeof(deployments[0]);
}}

const emaster_deployment_config_t *emaster_deployment_config_at(size_t index)
{{
    return index < emaster_deployment_config_count() ? &deployments[index] : NULL;
}}

const emaster_deployment_config_t *emaster_deployment_config_by_id(const char *deployment_id)
{{
    size_t index;
    if (deployment_id == NULL)
    {{
        return NULL;
    }}
    for (index = 0U; index < emaster_deployment_config_count(); ++index)
    {{
        if (strcmp(deployments[index].deployment_id, deployment_id) == 0)
        {{
            return &deployments[index];
        }}
    }}
    return NULL;
}}

const emaster_operation_profile_t *emaster_deployment_operation_profile_by_id(
    const emaster_deployment_config_t *deployment, const char *profile_id)
{{
    size_t index;
    if (deployment == NULL || profile_id == NULL || deployment->operation_profiles == NULL)
    {{
        return NULL;
    }}
    for (index = 0U; index < deployment->operation_profile_count; ++index)
    {{
        const emaster_operation_profile_t *profile = deployment->operation_profiles[index];
        if (profile != NULL && strcmp(profile->profile_id, profile_id) == 0)
        {{
            return profile;
        }}
    }}
    return NULL;
}}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology-input", required=True, type=Path, nargs="+")
    parser.add_argument("--deployment-input", required=True, type=Path, nargs="+")
    parser.add_argument("--operation-input", type=Path, nargs="*", default=[])
    parser.add_argument("--motion-input", type=Path, nargs="*", default=[])
    parser.add_argument("--device-input", type=Path, nargs="*", default=[])
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    topologies = load_documents(args.topology_input, "拓扑")
    deployments = load_documents(args.deployment_input, "部署")
    operations = load_documents(args.operation_input, "运行方案")
    motions = load_documents(args.motion_input, "运动方案")
    devices = load_documents(args.device_input, "设备") if args.device_input else None
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        generate(topologies, deployments, operations, motions, devices),
        encoding="utf-8",
        newline="\n",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
