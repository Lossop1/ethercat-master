#!/usr/bin/env python3
"""把拓扑和部署配置生成为只读 C 目录，不在运行时解析 JSON。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def load_documents(paths: list[Path], kind: str) -> list[dict[str, Any]]:
    """按传入顺序读取配置，并检查生成目录所需的最小结构。"""
    documents: list[dict[str, Any]] = []
    for path in paths:
        document = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            raise ValueError(f"{kind}配置 {path} 必须是 JSON 对象")
        documents.append(document)
    if not documents:
        raise ValueError(f"至少需要一个{kind}配置")
    return documents


def required_string(document: dict[str, Any], field: str, kind: str) -> str:
    """读取非空字符串；配置错误必须在构建期失败，不能生成不完整目录。"""
    value = document.get(field)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{kind}缺少非空字段 {field}")
    return value


def c_string(value: str) -> str:
    """使用 JSON 转义规则生成 C 字符串字面量，避免手工处理引号和反斜杠。"""
    return json.dumps(value, ensure_ascii=True)


def topology_values(documents: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """提取并稳定排序拓扑，保留任意规模的从站列表。"""
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
            if not isinstance(position, int) or isinstance(position, bool) or position <= 0:
                raise ValueError(f"拓扑 {topology_id} 的从站位置必须是正整数")
            axis_id = required_string(slave, "axis_id", f"拓扑 {topology_id} 的从站")
            profile_id = required_string(slave, "profile_id", f"拓扑 {topology_id} 的从站")
            if position in positions or axis_id in axis_ids:
                raise ValueError(f"拓扑 {topology_id} 的位置或轴 ID 重复")
            positions.add(position)
            axis_ids.add(axis_id)
            entries.append((position, axis_id, profile_id))
        expected_positions = set(range(1, len(entries) + 1))
        if positions != expected_positions:
            raise ValueError(f"拓扑 {topology_id} 的位置必须从 1 连续排列")
        entries.sort(key=lambda item: item[0])
        values.append({"id": topology_id, "status": status, "slaves": entries})
    return sorted(values, key=lambda item: item["id"])


def deployment_values(
    documents: list[dict[str, Any]], topology_ids: set[str]
) -> list[dict[str, str]]:
    """提取部署绑定；网卡名称只能从部署配置进入生成目录。"""
    values = []
    seen: set[str] = set()
    for document in documents:
        deployment_id = required_string(document, "deployment_id", "部署配置")
        if deployment_id in seen:
            raise ValueError(f"部署 ID 重复：{deployment_id}")
        seen.add(deployment_id)
        topology_id = required_string(document, "topology_id", f"部署 {deployment_id}")
        if topology_id not in topology_ids:
            raise ValueError(f"部署 {deployment_id} 引用了未知拓扑：{topology_id}")
        management = document.get("management_interface")
        if management is not None and (not isinstance(management, str) or not management.strip()):
            raise ValueError(f"部署 {deployment_id} 的 management_interface 必须是非空字符串")
        values.append(
            {
                "id": deployment_id,
                "hostname": required_string(document, "hostname", f"部署 {deployment_id}"),
                "interface": required_string(
                    document, "ethercat_interface", f"部署 {deployment_id}"
                ),
                "management": management or "",
                "topology": topology_id,
            }
        )
    return sorted(values, key=lambda item: item["id"])


def generate(topology_documents: list[dict[str, Any]], deployment_documents: list[dict[str, Any]]) -> str:
    """生成拓扑、部署数组和只读查找函数。"""
    topologies = topology_values(topology_documents)
    topology_ids = {item["id"] for item in topologies}
    deployments = deployment_values(deployment_documents, topology_ids)

    topology_arrays = []
    topology_initializers = []
    for ordinal, topology in enumerate(topologies):
        entries = ",\n".join(
            f'    {{UINT16_C({position}), {c_string(axis_id)}, {c_string(profile_id)}}}'
            for position, axis_id, profile_id in topology["slaves"]
        )
        topology_arrays.append(
            f"static const emaster_topology_slave_config_t topology_{ordinal}_slaves[] = {{\n"
            f"{entries}\n}};"
        )
        topology_initializers.append(
            "    {"
            + c_string(topology["id"])
            + ", "
            + c_string(topology["status"])
            + f", topology_{ordinal}_slaves, "
            + f"sizeof(topology_{ordinal}_slaves) / sizeof(topology_{ordinal}_slaves[0])"
            + "}"
        )

    deployment_initializers = []
    for deployment in deployments:
        topology_ordinal = next(
            index for index, topology in enumerate(topologies) if topology["id"] == deployment["topology"]
        )
        deployment_initializers.append(
            "    {"
            + c_string(deployment["id"])
            + ", "
            + c_string(deployment["hostname"])
            + ", "
            + c_string(deployment["interface"])
            + ", "
            + c_string(deployment["management"])
            + f", &topologies[{topology_ordinal}]"
            + "}"
        )

    return f"""/* 由 tools/generate_runtime_config.py 生成，禁止手工修改。 */
#include "emaster/config/runtime_config.h"

#include <stddef.h>
#include <string.h>

{"\n\n".join(topology_arrays)}

static const emaster_topology_config_t topologies[] = {{
{",\n".join(topology_initializers)},
}};

static const emaster_deployment_config_t deployments[] = {{
{",\n".join(deployment_initializers)},
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

"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology-input", required=True, type=Path, nargs="+")
    parser.add_argument("--deployment-input", required=True, type=Path, nargs="+")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    topologies = load_documents(args.topology_input, "拓扑")
    deployments = load_documents(args.deployment_input, "部署")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(topologies, deployments), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
