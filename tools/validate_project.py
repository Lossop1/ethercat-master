#!/usr/bin/env python3
"""编排设备、ESI、运行方案、拓扑、部署和供应商资料校验。"""

from __future__ import annotations

import argparse
import hashlib
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


def validate_manifest(
    check: Validation,
    root_dir: Path,
    manifest_path: Path,
    require_vendor_artifacts: bool,
) -> tuple[int, int]:
    """校验受控资料清单中已经安装的文件，不把缺失的可选资料当作元数据错误。"""
    declared = 0
    installed = 0
    if not manifest_path.is_file():
        check.errors.append(f"供应商资料清单不存在：{manifest_path}")
        return declared, installed
    for line_number, raw_line in enumerate(
        manifest_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            expected_hash, relative_path = line.split("  ", 1)
        except ValueError:
            check.errors.append(f"资料清单第 {line_number} 行无效")
            continue
        declared += 1
        path = root_dir / relative_path
        check.require(
            path.is_file() or not require_vendor_artifacts,
            f"缺少所需受控资料：{relative_path}",
        )
        if path.is_file():
            installed += 1
            actual_hash = hashlib.sha256(path.read_bytes()).hexdigest()
            check.require(actual_hash == expected_hash, f"SHA-256 不一致：{relative_path}")
    return declared, installed


def run(root_dir: Path, require_vendor_artifacts: bool) -> tuple[list[str], int, int]:
    """执行全仓配置校验，返回全部错误以及受控资料的安装统计。"""
    check = Validation(errors=[])
    profiles = load_documents(root_dir / "config/devices", "设备", check)
    topologies = load_documents(root_dir / "config/topologies", "拓扑", check)
    deployments = load_documents(root_dir / "config/deployments", "部署", check)
    operations = load_documents(root_dir / "config/operation_profiles", "运行方案", check)
    manifest_path = root_dir / "docs/vendor/SHA256SUMS"

    profiles_by_id, valid_profiles = validate_profiles(check, profiles)
    constraints_by_profile: dict[str, EsiRuntimeConstraints] = {}
    for profile in valid_profiles:
        try:
            constraints = validate_catalog_against_esi(
                check, root_dir, profile, require_vendor_artifacts
            )
            if constraints is not None:
                constraints_by_profile[profile["profile_id"]] = constraints
        except (ET.ParseError, KeyError, OSError, TypeError, ValueError) as error:
            check.errors.append(f"设备 {profile['profile_id']} 的 ESI 无法校验：{error}")
    topologies_by_id = validate_topologies(check, topologies, profiles_by_id)
    operations_by_id = validate_operations(
        check, operations, profiles_by_id, constraints_by_profile
    )
    validate_deployments(check, deployments, topologies_by_id, operations_by_id)
    declared, installed = validate_manifest(
        check, root_dir, manifest_path, require_vendor_artifacts
    )
    return check.errors, declared, installed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument(
        "--require-vendor-artifacts",
        action="store_true",
        help="任何受控供应商资料未安装时返回失败",
    )
    args = parser.parse_args()
    errors, declared, installed = run(args.root.resolve(), args.require_vendor_artifacts)
    if errors:
        for error in errors:
            print(f"错误：{error}", file=sys.stderr)
        return 1
    if installed == declared:
        print("设备、ESI、运行方案、拓扑、部署和受控供应商资料一致。")
    else:
        print(
            "项目元数据一致；受控供应商资料未全部安装"
            f"（已安装 {installed}/{declared}）。"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
