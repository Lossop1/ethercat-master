#!/usr/bin/env python3
"""编排设备、运行方案、拓扑和部署配置校验。"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from validate_device_config import validate_profiles
from validate_runtime_config import (
    validate_deployments,
    validate_motion_profiles,
    validate_operations,
    validate_topologies,
)
from validation_common import Validation, load_documents


def run(config_dir: Path) -> tuple[list[str], list[str]]:
    """执行配置源的全套校验，返回 (错误, 告警)。

    告警是"这份配置合法，但它意味着某事"（例如停机判据掩码为 0，只能走标准判据）。
    它不影响退出码 —— 把合法配置报成错误只会让校验器被忽略。

    一族可以一个文件都没有：这个项目交付时不含任何电机配置，客户跑
    gen/emaster_config.py 现生成。空不等于错，所以不在这里要求"至少有一份"——
    真正的不一致（引用了不存在的 ID）仍由下面的引用校验逐个抓出来。
    """
    check = Validation(errors=[])
    profiles = load_documents(config_dir / "devices", "设备", check, required=False)
    topologies = load_documents(config_dir / "topologies", "拓扑", check, required=False)
    deployments = load_documents(config_dir / "deployments", "部署", check, required=False)
    operations = load_documents(
        config_dir / "operation_profiles", "运行方案", check, required=False
    )
    motions = load_documents(
        config_dir / "motion_profiles", "运动方案", check, required=False
    )
    profiles_by_id = validate_profiles(check, profiles)
    topologies_by_id = validate_topologies(check, topologies, profiles_by_id)
    operations_by_id = validate_operations(check, operations, profiles_by_id)
    motions_by_id = validate_motion_profiles(check, motions)
    validate_deployments(
        check, deployments, topologies_by_id, operations_by_id, motions_by_id
    )
    return check.errors, check.warnings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, help="配置源目录", default=None)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    config_dir = (
        args.config if args.config is not None else args.root / "config"
    ).resolve()
    errors, warnings = run(config_dir)
    for warning in warnings:
        print(f"告警：{warning}")
    if errors:
        for error in errors:
            print(f"错误：{error}", file=sys.stderr)
        return 1
    print("设备、运行方案、拓扑和部署配置一致。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
