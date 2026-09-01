"""项目配置校验器共享的数据结构和基础解析函数。"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class Validation:
    errors: list[str]

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.errors.append(message)


@dataclass(frozen=True)
class EsiRuntimeConstraints:
    """ESI 中可直接约束运行方案的同步和 PDO 配置事实。"""

    assign_activate_by_strategy: dict[str, int]
    default_sync_type_by_sm: dict[int, int]
    minimum_cycle_ns: int | None
    supports_pdo_configuration: bool
    supports_distributed_clocks: bool


def hex_value(value: str) -> int:
    return int(value.replace("#x", "0x"), 16)


def non_empty_string(value: object) -> bool:
    """只接受去除首尾空白后仍有内容的字符串。"""
    return isinstance(value, str) and bool(value.strip())


def validate_hex_value(
    check: Validation, value: object, field: str, maximum: int
) -> bool:
    """校验配置中的十六进制字符串及目标无符号整数范围。"""
    try:
        valid = isinstance(value, str) and value.startswith("0x")
        number = hex_value(value) if valid else -1
        valid = valid and 0 <= number <= maximum
    except ValueError:
        valid = False
    check.require(valid, f"{field} 必须是范围内的十六进制字符串")
    return valid


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} 必须包含 JSON 对象")
    return value


def load_documents(directory: Path, kind: str, check: Validation) -> list[dict[str, Any]]:
    """按文件名稳定排序加载一类配置，并把读取错误转为可汇总的校验错误。"""
    documents: list[dict[str, Any]] = []
    paths = sorted(directory.glob("*.json")) if directory.is_dir() else []
    check.require(bool(paths), f"未找到{kind}配置：{directory}")
    for path in paths:
        try:
            document = load_json(path)
            document["_source_path"] = path.as_posix()
            documents.append(document)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            check.errors.append(f"无法读取{kind}配置 {path}：{error}")
    return documents
