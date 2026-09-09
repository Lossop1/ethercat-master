#!/usr/bin/env python3
"""生成错误恢复策略配置的 C 代码。"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def c_string(value: str) -> str:
    """使用 JSON 转义规则生成可移植的 C 字符串字面量。"""
    return json.dumps(value, ensure_ascii=True)


def load_policy(path: Path) -> dict[str, Any]:
    """加载并验证错误恢复策略 JSON 文件。"""
    with path.open("r", encoding="utf-8") as f:
        policy = json.load(f)

    # 基本验证
    assert policy.get("schema_version") == 1, f"Unsupported schema_version in {path}"
    assert "policy_id" in policy, f"Missing policy_id in {path}"

    return policy


def generate_policy_struct(policy: dict[str, Any], ordinal: int) -> str:
    """生成单个策略的 C 结构体初始化代码。"""
    wkc = policy.get("wkc_recovery", {})
    deadline = policy.get("deadline_recovery", {})
    al_state = policy.get("al_state_recovery", {})
    cia402 = policy.get("cia402_fault_recovery", {})

    # 生成可恢复错误码数组
    error_codes = cia402.get("recoverable_error_codes", [])
    if error_codes:
        codes_array_name = f"policy_{ordinal}_cia402_error_codes"
        codes_decl = f"static const uint16_t {codes_array_name}[] = {{\n"
        codes_decl += ",\n".join(f"    UINT16_C({code})" for code in error_codes)
        codes_decl += "\n};"
        codes_pointer = codes_array_name
        codes_count = f"sizeof({codes_array_name}) / sizeof({codes_array_name}[0])"
    else:
        codes_decl = ""
        codes_pointer = "NULL"
        codes_count = "0U"

    struct = f"""    {{
        .policy_id = {c_string(policy["policy_id"])},
        .wkc_recovery = {{
            .enabled = {str(wkc.get("enabled", False)).lower()},
            .consecutive_error_threshold = UINT32_C({wkc.get("consecutive_error_threshold", 1)}),
            .total_error_threshold = UINT32_C({wkc.get("total_error_threshold", 1)})
        }},
        .deadline_recovery = {{
            .enabled = {str(deadline.get("enabled", False)).lower()},
            .consecutive_error_threshold = UINT32_C({deadline.get("consecutive_error_threshold", 1)})
        }},
        .al_state_recovery = {{
            .enabled = {str(al_state.get("enabled", False)).lower()},
            .check_interval_cycles = UINT32_C({al_state.get("check_interval_cycles", 100)}),
            .max_recovery_attempts = UINT32_C({al_state.get("max_recovery_attempts", 3)})
        }},
        .cia402_fault_recovery = {{
            .enabled = {str(cia402.get("enabled", False)).lower()},
            .max_reset_attempts = UINT32_C({cia402.get("max_reset_attempts", 1)}),
            .recoverable_error_codes = {codes_pointer},
            .recoverable_error_code_count = {codes_count}
        }}
    }}"""

    return codes_decl, struct


def generate_config(policies: list[dict[str, Any]]) -> str:
    """生成完整的错误恢复配置 C 代码。"""
    lines = [
        "/* 自动生成的错误恢复策略配置 - 不要手动编辑 */",
        "",
        '#include "emaster/config/error_recovery_config.h"',
        "",
        "#include <stddef.h>",
        "#include <string.h>",
        "",
    ]

    # 生成错误码数组
    array_decls = []
    struct_initializers = []
    for ordinal, policy in enumerate(policies):
        codes_decl, struct = generate_policy_struct(policy, ordinal)
        if codes_decl:
            array_decls.append(codes_decl)
        struct_initializers.append(struct)

    if array_decls:
        lines.extend(array_decls)
        lines.append("")

    # 生成策略数组
    lines.append("static const emaster_error_recovery_policy_t policies[] = {")
    lines.append(",\n".join(struct_initializers))
    lines.append("};")
    lines.append("")

    # 生成查询函数
    lines.extend([
        "size_t emaster_error_recovery_policy_count(void) {",
        "    return sizeof(policies) / sizeof(policies[0]);",
        "}",
        "",
        "const emaster_error_recovery_policy_t *emaster_error_recovery_policy_at(size_t index) {",
        "    if (index >= emaster_error_recovery_policy_count()) {",
        "        return NULL;",
        "    }",
        "    return &policies[index];",
        "}",
        "",
        "const emaster_error_recovery_policy_t *emaster_error_recovery_policy_by_id(const char *policy_id) {",
        "    size_t count, i;",
        "    if (policy_id == NULL) {",
        "        return NULL;",
        "    }",
        "    count = emaster_error_recovery_policy_count();",
        "    for (i = 0; i < count; ++i) {",
        "        if (strcmp(policies[i].policy_id, policy_id) == 0) {",
        "            return &policies[i];",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
    ])

    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="生成错误恢复策略配置的 C 代码")
    parser.add_argument("config_dir", type=Path, help="错误恢复策略配置目录")
    parser.add_argument("output", type=Path, help="输出 C 文件路径")
    args = parser.parse_args()

    # 加载所有策略文件
    policies = []
    for policy_file in sorted(args.config_dir.glob("*.json")):
        try:
            policy = load_policy(policy_file)
            policies.append(policy)
            print(f"Loaded policy: {policy['policy_id']} from {policy_file.name}")
        except Exception as e:
            print(f"Error loading {policy_file}: {e}")
            raise

    if not policies:
        print(f"Warning: No policy files found in {args.config_dir}")

    # 生成 C 代码
    code = generate_config(policies)

    # 写入输出文件
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(code, encoding="utf-8")
    print(f"Generated {args.output} with {len(policies)} policies")


if __name__ == "__main__":
    main()
