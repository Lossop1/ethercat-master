#!/usr/bin/env python3
"""将中文消息资源生成只读 C 文本表。"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


MESSAGE_KEYS = [
    "command_interfaces",
    "command_deployments",
    "command_capture",
    "command_prepare_dc",
    "usage",
    "interface_line",
    "preop_confirmation_token",
    "preop_confirm_required",
    "preop_confirm_prompt",
    "deployment_line",
    "message_deployment_unavailable",
    "message_output_exists",
    "message_sdo_plan_failed",
    "message_preop_not_confirmed",
    "message_timestamp_failed",
    "message_empty_report",
    "message_topology_or_pdo_mismatch",
    "message_output_path_too_long",
    "message_output_create_failed",
    "message_output_open_failed",
    "message_output_write_failed",
    "message_output_flush_failed",
    "message_output_publish_failed",
    "message_diagnostic_report_saved",
    "message_incomplete_report_rejected",
    "probe_invalid_argument",
    "probe_interface_enumeration_failed",
    "probe_interface_open_failed",
    "probe_no_slaves",
    "probe_too_many_slaves",
    "probe_preop_not_reached",
    "probe_sdo_read_failed",
    "probe_pdo_discovery_failed",
    "probe_out_of_memory",
    "probe_restore_init_failed",
    "probe_ok",
    "probe_unknown",
    "probe_status_error",
    "probe_status_warning",
    "probe_status_interface_error",
    "probe_status_interface_warning",
    "pdo_failure",
    "dc_prepare_confirmation_token",
    "dc_prepare_usage",
    "dc_prepare_confirm_required",
    "dc_prepare_confirm_prompt",
    "dc_prepare_plan_failed",
    "dc_prepare_success",
    "dc_prepare_failed",
    "dc_prepare_axis_line",
    "dc_prepare_match",
    "dc_prepare_mismatch",
    "dc_prepare_dc_unavailable",
    "dc_prepare_sdo_write_failed",
    "dc_prepare_sdo_readback_failed",
    "dc_prepare_dc_config_failed",
    "dc_prepare_sync0_readback_failed",
]

# 每条格式消息允许的 printf 转换说明符。资源文本可修改，参数契约不可漂移。
MESSAGE_FORMATS = {
    "usage": ("s", "s", "s", "s", "s", "s"),
    "dc_prepare_usage": ("s", "s"),
    "interface_line": ("s", "s"),
    "preop_confirm_prompt": ("s", "s", "s", "s"),
    "deployment_line": ("s", "s", "s", "s"),
    "message_output_exists": ("s",),
    "message_output_create_failed": ("s",),
    "message_output_open_failed": ("s",),
    "message_output_flush_failed": ("s",),
    "message_output_publish_failed": ("s",),
    "probe_status_error": ("s",),
    "probe_status_warning": ("s",),
    "probe_status_interface_error": ("s", "s"),
    "probe_status_interface_warning": ("s", "s"),
    "pdo_failure": ("u", "u", "X", "X"),
    "dc_prepare_confirm_prompt": ("s", "s", "s", "u", "s"),
    "dc_prepare_failed": ("s",),
    "dc_prepare_axis_line": ("u", "s", "s", "s", "s", "s", "s", "s"),
}

PRINTF_CONVERSION = re.compile(
    r"%(?:[1-9][0-9]*\$)?[-+ #0]*(?:[0-9]+|\*)?(?:\.(?:[0-9]+|\*))?"
    r"(?:hh|h|ll|l|j|z|t|L)?([diuoxXfFeEgGaAcspn%])"
)


def c_string(value: str) -> str:
    """用 JSON 转义生成 C 字符串，并保留中文 UTF-8 文本。"""
    return json.dumps(value, ensure_ascii=False)


def format_conversions(value: str, key: str) -> tuple[str, ...]:
    """完整解析格式串；除合法转换和 %% 外，不允许出现未解释的百分号。"""
    conversions: list[str] = []
    cursor = 0
    while cursor < len(value):
        percent = value.find("%", cursor)
        if percent < 0:
            break
        if percent + 1 < len(value) and value[percent + 1] == "%":
            cursor = percent + 2
            continue
        match = PRINTF_CONVERSION.match(value, percent)
        if match is None or match.group(1) in ("%", "n"):
            raise ValueError(f"消息 {key} 包含无效或不安全的格式说明符")
        conversions.append(match.group(1))
        cursor = match.end()
    return tuple(conversions)


def generate(document: dict[str, object]) -> str:
    """校验资源键集合并生成按枚举顺序排列的 C 文本表。"""
    messages = document.get("messages")
    if document.get("schema_version") != 1 or not isinstance(messages, dict):
        raise ValueError("消息资源版本或 messages 对象无效")
    missing = [key for key in MESSAGE_KEYS if not isinstance(messages.get(key), str)]
    if missing:
        raise ValueError(f"消息资源缺少键：{', '.join(missing)}")
    extra = sorted(set(messages) - set(MESSAGE_KEYS))
    if extra:
        raise ValueError(f"消息资源包含未知键：{', '.join(extra)}")
    for key in ("command_interfaces", "command_deployments", "command_capture", "command_prepare_dc"):
        if re.fullmatch(r"[a-z][a-z0-9-]*", messages[key]) is None:
            raise ValueError(f"消息 {key} 必须是单个小写命令词")
    if not messages["preop_confirmation_token"]:
        raise ValueError("PRE-OP 确认令牌不能为空")
    for key in MESSAGE_KEYS:
        value = messages[key]
        conversions = format_conversions(value, key)
        expected = MESSAGE_FORMATS.get(key, ())
        if conversions != expected:
            raise ValueError(
                f"消息 {key} 的格式参数应为 {expected}，实际为 {conversions}"
            )
    rendered = ",\n".join(
        f"    [EMASTER_TEXT_{key.upper()}] = {c_string(messages[key])}"
        for key in MESSAGE_KEYS
    )
    return f"""/* 由 tools/generate_messages.py 生成，禁止手工修改。 */
#include "emaster/messages.h"

static const char *const messages[EMASTER_TEXT_COUNT] = {{
{rendered}
}};

const char *emaster_text(emaster_text_id_t id)
{{
    return id >= EMASTER_TEXT_USAGE && id < EMASTER_TEXT_COUNT ? messages[id] : "";
}}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    document = json.loads(args.input.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError("消息资源必须是 JSON 对象")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(document), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
