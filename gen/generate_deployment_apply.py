#!/usr/bin/env python3
"""部署族的"填充代码"渲染层。

为什么它和别的族不一样
    部署是唯一跨族引用的一份：它按 ID 指向一个拓扑、一组运行方案、一个运动方案。
    所以它必须**最后**加载，而且解析 ID 时要去查别的族已经填好的数组 ——
    查的是那几族生成物导出的 count/at 访问函数，不是另抄一份 ID 表。
    这样"引用了不存在的 ID"在加载时就返回 false，不会留下一个空指针到运行期。

指针数组
    operation_profiles 是 const T *const *（指针本身也 const），运行期无法逐项赋值，
    所以声明成"指针可变、指向内容只读"，赋值处显式转型。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_config_model import (  # noqa: E402
    deployment_values,
    motion_values,
    operation_values,
    topology_values,
)


def read_str(key: str, target: str) -> list[str]:
    return [f'    if (!emaster_config_reader_string(reader, "{key}", &{target}))', "    {", "        return false;", "    }"]


def read_num(key: str, target: str, maximum: int | None = None, cast: str = "") -> list[str]:
    lines = [f'    if (!emaster_config_reader_u32(reader, "{key}", &number))', "    {", "        return false;", "    }"]
    if maximum is not None:
        lines.append(f"    if (number > {maximum}) {{ return false; }}")
    lines.append(f"    {target} = {'(' + cast + ')number' if cast else 'number'};")
    return lines


def read_signed(key: str, target: str) -> list[str]:
    return [
        f'    if (!emaster_config_reader_i32(reader, "{key}", &signed_number))',
        "    {",
        "        return false;",
        "    }",
        f"    {target} = signed_number;",
    ]


def read_flag(key: str, target: str) -> list[str]:
    return [f'    if (!emaster_config_reader_bool(reader, "{key}", &flag))', "    {", "        return false;", "    }", f"    {target} = flag;"]


def deployment_rows(deployments: list[dict[str, Any]]) -> list[tuple[str, str]]:
    rows: list[tuple[str, str]] = [("deployment.count", str(len(deployments)))]
    for di, d in enumerate(deployments):
        base = f"deployment.{di}"
        rows += [
            (f"{base}.id", d["id"]),
            (f"{base}.hostname", d["hostname"]),
            (f"{base}.ethercat_interface", d["interface"]),
            (f"{base}.management_interface", d["management"]),
            (f"{base}.run_report_path", d["run_report_path"]),
            (f"{base}.report_archive_keep", str(d["report_archive_keep"])),
            (f"{base}.error_recovery_policy_id", d["error_recovery_policy_id"]),
            (f"{base}.fault_policy", str(d["fault_policy"])),
            (f"{base}.realtime_priority_present", "true" if d["realtime_priority_present"] else "false"),
            (f"{base}.realtime_priority", str(d["realtime_priority"])),
            (f"{base}.realtime_cpu_core_present", "true" if d["realtime_cpu_core_present"] else "false"),
            (f"{base}.realtime_cpu_core", str(d["realtime_cpu_core"])),
            (f"{base}.realtime_required", "true" if d["realtime_required"] else "false"),
            (f"{base}.external_target_timeout_ms", str(d["external_target_timeout_ms"])),
            (f"{base}.external_target_torque_limit_per_mille", str(d["external_target_torque_limit_per_mille"])),
            (f"{base}.topology_id", d["topology"]),
            (f"{base}.operation_profile_ids.count", str(len(d["operation_profile_ids"]))),
        ]
        for ii, oid in enumerate(d["operation_profile_ids"]):
            rows.append((f"{base}.operation_profile_ids.{ii}", oid))
        motion_id = d["motion_profile_id"]
        rows.append((f"{base}.motion_profile_id_present", "true" if motion_id is not None else "false"))
        if motion_id is not None:
            rows.append((f"{base}.motion_profile_id", motion_id))
    return rows


# 运行期容量上限。条目数从文件读，不再由生成期的 #define 决定 ——
# 加一台机器、加一款电机不需要重新生成也不需要重编译。
# 超过上限时加载**响亮地失败**，不会截断。
DEPLOYMENT_CAPACITY = 64


def render(deployments: list[dict[str, Any]]) -> str:
    out: list[str] = [
        "/* 由 gen/generate_deployment_apply.py 生成，不要手改。 */",
        '#include "emaster/config/generated_deployment.h"',
        '#include "emaster/config/generated_apply.h"',
        '#include "emaster/config/generated_runtime.h"',
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "#include <stdio.h>",
        "#include <string.h>",
        "",
        f"#define EMASTER_DEPLOYMENT_CAPACITY {DEPLOYMENT_CAPACITY}U",
        "#define EMASTER_KEY_CAPACITY 192U",
        "",
        "static emaster_deployment_config_t deployments[EMASTER_DEPLOYMENT_CAPACITY];",
        "static size_t deployment_count = 0U;",
        "",
        "/*",
        " * 键名逐级拼接。每一级都用 snprintf 的返回值判截断：",
        " * 截断必须当失败。否则会拿一个被截短的前缀去查，查不到再报\"缺键\"——",
        " * 报错指向的地方和真正的原因（键太长）完全无关。",
        " */",
        "static bool key_push(char *buffer, size_t capacity, const char *suffix)",
        "{",
        "    size_t used = strlen(buffer);",
        "    int written;",
        "",
        "    if (used + 1U >= capacity)",
        "    {",
        "        return false;",
        "    }",
        "    written = snprintf(buffer + used, capacity - used, \".%s\", suffix);",
        "    return written > 0 && (size_t)written < capacity - used;",
        "}",
        "",
        "static bool key_push_index(char *buffer, size_t capacity, size_t index)",
        "{",
        "    size_t used = strlen(buffer);",
        "    int written;",
        "",
        "    if (used + 1U >= capacity)",
        "    {",
        "        return false;",
        "    }",
        "    written = snprintf(buffer + used, capacity - used, \".%zu\", index);",
        "    return written > 0 && (size_t)written < capacity - used;",
        "}",
        "",
        "/* 按 ID 在已加载的数组里查。查不到返回 NULL，由调用方拒绝加载。 */",
        "static const emaster_topology_config_t *find_topology(const char *id)",
        "{",
        "    size_t index;",
        "",
        "    for (index = 0U; index < emaster_topology_config_count(); ++index)",
        "    {",
        "        const emaster_topology_config_t *item = emaster_topology_config_at(index);",
        "        if (item != NULL && strcmp(item->topology_id, id) == 0)",
        "        {",
        "            return item;",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
        "static const emaster_operation_profile_t *find_operation(const char *id)",
        "{",
        "    size_t index;",
        "",
        "    for (index = 0U; index < emaster_operation_profile_count(); ++index)",
        "    {",
        "        const emaster_operation_profile_t *item = emaster_operation_profile_at(index);",
        "        if (item != NULL && strcmp(item->profile_id, id) == 0)",
        "        {",
        "            return item;",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
        "static const emaster_motion_profile_t *find_motion(const char *id)",
        "{",
        "    size_t index;",
        "",
        "    for (index = 0U; index < emaster_motion_profile_count(); ++index)",
        "    {",
        "        const emaster_motion_profile_t *item = emaster_motion_profile_at(index);",
        "        if (item != NULL && strcmp(item->motion_profile_id, id) == 0)",
        "        {",
        "            return item;",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
        "/* 运行方案指针数组按容量预留：指针本身要到加载时才知道指哪。 */",
        f"static const emaster_operation_profile_t *operation_pointers[{DEPLOYMENT_CAPACITY}U]"
        f"[{max((len(d['operation_profile_ids']) for d in deployments), default=1)}U];",
        "",
        "static bool load_deployment(const emaster_config_reader_t *reader, size_t ordinal,",
        "                            emaster_deployment_config_t *out)",
        "{",
        "    char key[EMASTER_KEY_CAPACITY];",
        "    char nested[EMASTER_KEY_CAPACITY];",
        "    size_t index;",
        "    size_t count;",
        "    uint32_t number = 0U;",
        "    int32_t signed_number = 0;",
        "    bool flag = false;",
        "    const char *text = NULL;",
        "",
        "    if (snprintf(key, sizeof(key), \"deployment.%zu\", ordinal) < 0)",
        "    {",
        "        return false;",
        "    }",
        "",
    ]

    def at(name: str) -> list[str]:
        """把键推进一级，然后读一个字段。"""
        lines = [f"    memcpy(nested, key, sizeof(nested));"]
        lines.append(f'    if (!key_push(nested, sizeof(nested), "{name}"))')
        lines.append("    {")
        lines.append("        return false;")
        lines.append("    }")
        return lines

    def read_str_field(name: str, target: str) -> list[str]:
        return at(name) + [
            f"    if (!emaster_config_reader_string(reader, nested, &{target}))",
            "    {",
            "        return false;",
            "    }",
        ]

    def read_num_field(name: str, target: str, maximum: int | None = None, cast: str = "") -> list[str]:
        lines = at(name) + [
            "    if (!emaster_config_reader_u32(reader, nested, &number))",
            "    {",
            "        return false;",
            "    }",
        ]
        if maximum is not None:
            lines.append(f"    if (number > {maximum}) {{ return false; }}")
        lines.append(f"    {target} = {'(' + cast + ')number' if cast else 'number'};")
        return lines

    def read_signed_field(name: str, target: str) -> list[str]:
        return at(name) + [
            "    if (!emaster_config_reader_i32(reader, nested, &signed_number))",
            "    {",
            "        return false;",
            "    }",
            f"    {target} = signed_number;",
        ]

    def read_flag_field(name: str, target: str) -> list[str]:
        return at(name) + [
            "    if (!emaster_config_reader_bool(reader, nested, &flag))",
            "    {",
            "        return false;",
            "    }",
            f"    {target} = flag;",
        ]

    out += read_str_field("id", "out->deployment_id")
    out += read_str_field("hostname", "out->hostname")
    out += read_str_field("ethercat_interface", "out->ethercat_interface")
    out += read_str_field("management_interface", "out->management_interface")
    out += read_str_field("run_report_path", "out->run_report_path")
    out += read_str_field("error_recovery_policy_id", "out->error_recovery_policy_id")
    out += read_num_field("report_archive_keep", "out->report_archive_keep")
    out += read_num_field("fault_policy", "out->fault_policy", 0xFF, "emaster_fault_policy_t")
    out += read_flag_field("realtime_priority_present", "out->has_realtime_priority")
    out += read_signed_field("realtime_priority", "out->realtime_priority")
    out += read_flag_field("realtime_cpu_core_present", "out->has_realtime_cpu_core")
    out += read_signed_field("realtime_cpu_core", "out->realtime_cpu_core")
    out += read_flag_field("realtime_required", "out->realtime_required")
    out += read_num_field("external_target_timeout_ms", "out->external_target_timeout_ms")
    out += read_num_field("external_target_torque_limit_per_mille",
                          "out->external_target_torque_limit_per_mille")
    out += read_str_field("topology_id", "text")
    out += [
        "    out->topology = find_topology(text);",
        "    if (out->topology == NULL)",
        "    {",
        '        emaster_config_reader_note_failure("引用的拓扑 ID 在该 flat 文件里不存在");',
        "        return false;",
        "    }",
    ]
    out += read_flag_field("motion_profile_id_present", "flag")
    out += [
        "    if (flag)",
        "    {",
    ]
    out += at("motion_profile_id")
    out += [
        "        if (!emaster_config_reader_string(reader, nested, &text))",
        "        {",
        "            return false;",
        "        }",
        "        out->motion_profile = find_motion(text);",
        "        if (out->motion_profile == NULL)",
        "        {",
        '            emaster_config_reader_note_failure("引用的运动方案 ID 在该 flat 文件里不存在");',
        "            return false;",
        "        }",
        "    }",
        "    else",
        "    {",
        "        out->motion_profile = NULL;",
        "    }",
    ]
    out += at("operation_profile_ids")
    out += [
        "    if (!key_push(nested, sizeof(nested), \"count\"))",
        "    {",
        "        return false;",
        "    }",
        "    if (!emaster_config_reader_size(reader, nested, &count))",
        "    {",
        "        return false;",
        "    }",
        f"    if (count > {max((len(d['operation_profile_ids']) for d in deployments), default=0)}U)",
        "    {",
        "        emaster_config_reader_note_failure(\"条目数超过编译期容量\");",
        "        return false;",
        "    }",
        "    for (index = 0U; index < count; ++index)",
        "    {",
        "        memcpy(nested, key, sizeof(nested));",
        '        if (!key_push(nested, sizeof(nested), "operation_profile_ids") ||',
        "            !key_push_index(nested, sizeof(nested), index))",
        "        {",
        "            return false;",
        "        }",
        "        if (!emaster_config_reader_string(reader, nested, &text))",
        "        {",
        "            return false;",
        "        }",
        "        operation_pointers[ordinal][index] = find_operation(text);",
        "        if (operation_pointers[ordinal][index] == NULL)",
        "        {",
        '            emaster_config_reader_note_failure("引用的运行方案 ID 在该 flat 文件里不存在");',
        "            return false;",
        "        }",
        "    }",
        "    if (count > 0U)",
        "    {",
        "        out->operation_profiles =",
        "            (const emaster_operation_profile_t *const *)operation_pointers[ordinal];",
        "        out->operation_profile_count = count;",
        "    }",
        "    else",
        "    {",
        "        out->operation_profiles = NULL;",
        "        out->operation_profile_count = 0U;",
        "    }",
        "    return true;",
        "}",
        "",
        "bool emaster_generated_deployment_apply(const emaster_config_reader_t *reader)",
        "{",
        "    size_t index;",
        "",
        '    if (!emaster_config_reader_size(reader, "deployment.count", &deployment_count))',
        "    {",
        "        return false;",
        "    }",
        "    if (deployment_count > (size_t)EMASTER_DEPLOYMENT_CAPACITY)",
        "    {",
        "        fprintf(stderr,",
        '                "部署条目 %zu 超过编译期容量 %u，请提高 EMASTER_DEPLOYMENT_CAPACITY\\n",',
        "                deployment_count, (unsigned)EMASTER_DEPLOYMENT_CAPACITY);",
        "        emaster_config_reader_note_failure(\"条目数超过编译期容量\");",
        "        return false;",
        "    }",
        "    for (index = 0U; index < deployment_count; ++index)",
        "    {",
        "        if (!load_deployment(reader, index, &deployments[index]))",
        "        {",
        "            return false;",
        "        }",
        "    }",
        "    return true;",
        "}",
        "",
        "size_t emaster_deployment_config_count(void)",
        "{",
        "    return deployment_count;",
        "}",
        "",
        "const emaster_deployment_config_t *emaster_deployment_config_at(size_t index)",
        "{",
        "    return index < deployment_count ? &deployments[index] : NULL;",
        "}",
        "",
        "const emaster_deployment_config_t *emaster_deployment_config_by_id(const char *deployment_id)",
        "{",
        "    size_t index;",
        "",
        "    if (deployment_id == NULL)",
        "    {",
        "        return NULL;",
        "    }",
        "    for (index = 0U; index < deployment_count; ++index)",
        "    {",
        "        if (strcmp(deployments[index].deployment_id, deployment_id) == 0)",
        "        {",
        "            return &deployments[index];",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
        "const emaster_operation_profile_t *emaster_deployment_operation_profile_by_id(",
        "    const emaster_deployment_config_t *deployment, const char *profile_id)",
        "{",
        "    size_t index;",
        "",
        "    if (deployment == NULL || profile_id == NULL || deployment->operation_profiles == NULL)",
        "    {",
        "        return NULL;",
        "    }",
        "    for (index = 0U; index < deployment->operation_profile_count; ++index)",
        "    {",
        "        const emaster_operation_profile_t *profile = deployment->operation_profiles[index];",
        "        if (profile != NULL && strcmp(profile->profile_id, profile_id) == 0)",
        "        {",
        "            return profile;",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    return "\n".join(out)


def render_header() -> str:
    return "\n".join([
        "/* 由 gen/generate_deployment_apply.py 生成，不要手改。 */",
        "#ifndef EMASTER_CONFIG_GENERATED_DEPLOYMENT_H",
        "#define EMASTER_CONFIG_GENERATED_DEPLOYMENT_H",
        "",
        '#include "emaster/config/config_reader.h"',
        '#include "emaster/config/runtime_config.h"',
        "",
        "#include <stdbool.h>",
        "#include <stddef.h>",
        "",
        "bool emaster_generated_deployment_apply(const emaster_config_reader_t *reader);",
        "size_t emaster_generated_deployment_count(void);",
        "const emaster_deployment_config_t *emaster_generated_deployment_at(size_t index);",
        "",
        "#endif /* EMASTER_CONFIG_GENERATED_DEPLOYMENT_H */",
        "",
    ])


def main() -> int:
    parser = argparse.ArgumentParser(description="部署 -> 填充代码 + 扁平文件")
    parser.add_argument("--deployments", type=Path, required=True)
    parser.add_argument("--topologies", type=Path, required=True)
    parser.add_argument("--operations", type=Path, required=True)
    parser.add_argument("--motions", type=Path, required=True)
    parser.add_argument("--c-output", type=Path, required=True)
    parser.add_argument("--h-output", type=Path, required=True)
    parser.add_argument("--flat-output", type=Path, required=True)
    args = parser.parse_args()

    def load(directory: Path) -> list[dict[str, Any]]:
        return [json.loads(p.read_text(encoding="utf-8")) for p in sorted(directory.glob("*.json"))]

    topologies = topology_values(load(args.topologies))
    operations = operation_values(load(args.operations))
    motions = motion_values(load(args.motions))
    deployments = deployment_values(load(args.deployments), topologies, operations, motions)
    head = "/* 由 gen/generate_deployment_apply.py 生成，不要手改。 */\n"
    args.c_output.write_text(render(deployments), encoding="utf-8", newline="\n")
    args.h_output.write_text(render_header(), encoding="utf-8", newline="\n")
    rows = deployment_rows(deployments)
    args.flat_output.write_text("".join(f"{k}\t{v}\n" for k, v in rows), encoding="utf-8", newline="\n")
    print(f"部署 {len(deployments)} 份，扁平记录 {len(rows)} 条")
    print(f"已写出 {args.c_output}")
    print(f"已写出 {args.h_output}")
    print(f"已写出 {args.flat_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
