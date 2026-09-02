#!/usr/bin/env python3
"""编排设备、ESI、运行方案、拓扑和部署配置校验。"""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from validate_device_config import validate_catalog_against_esi, validate_profiles
from validate_runtime_config import (
    validate_deployments,
    validate_operations,
    validate_topologies,
)
from validation_common import EsiRuntimeConstraints, Validation, load_documents


def run(root_dir: Path) -> list[str]:
    """执行全仓配置校验并返回全部错误。"""
    check = Validation(errors=[])
    profiles = load_documents(root_dir / "config/devices", "设备", check)
    topologies = load_documents(root_dir / "config/topologies", "拓扑", check)
    deployments = load_documents(root_dir / "config/deployments", "部署", check)
    operations = load_documents(root_dir / "config/operation_profiles", "运行方案", check)
    profiles_by_id, valid_profiles = validate_profiles(check, profiles)
    constraints_by_profile: dict[str, EsiRuntimeConstraints] = {}
    for profile in valid_profiles:
        try:
            constraints = validate_catalog_against_esi(check, root_dir, profile)
            if constraints is not None:
                constraints_by_profile[profile["profile_id"]] = constraints
        except (ET.ParseError, KeyError, OSError, TypeError, ValueError) as error:
            check.errors.append(f"设备 {profile['profile_id']} 的 ESI 无法校验：{error}")
    topologies_by_id = validate_topologies(check, topologies, profiles_by_id)
    operations_by_id = validate_operations(
        check, operations, profiles_by_id, constraints_by_profile
    )
    validate_deployments(check, deployments, topologies_by_id, operations_by_id)
    return check.errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = run(args.root.resolve())
    if errors:
        for error in errors:
            print(f"错误：{error}", file=sys.stderr)
        return 1
    print("设备、ESI、运行方案、拓扑和部署配置一致。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
