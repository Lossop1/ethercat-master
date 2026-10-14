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
    tools/gen/emaster_config.py 现生成。空不等于错，所以不在这里要求"至少有一份"——
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
    _warn_about_orphan_motions(check, deployments, motions_by_id)
    return check.errors, check.warnings


def _warn_about_orphan_motions(
    check: Validation, deployments: list[dict], motions_by_id: dict[str, dict]
) -> None:
    """没人引用的运动方案要说出来。

    它和"引用了不存在的 ID"是两回事，甚至更阴：那份方案一路进得了
    `config-runtime/runtime.flat`（`motion.0.*` 都在），部署那一行却写
    `motion_profile_id_present=false`。主站照常起、照常空转，编进去的每轴换算与
    限幅一次都用不上，而校验、构建、启动三处全都不出声——现场只会看到"轴没怎么动"。

    所以这里按"合法但它意味着某事"报成告警，点名是哪几份、怎么挂上去。不报成错误：
    留一份还没挂的草稿方案是正当用法。
    """
    if not motions_by_id:
        return
    attached = {
        deployment.get("motion_profile_id")
        for deployment in deployments
        if isinstance(deployment, dict)
    }
    for motion_id in sorted(motions_by_id):
        if motion_id in attached:
            continue
        check.warnings.append(
            f"运动方案 {motion_id} 没有被任何部署引用，主站不会加载它："
            f"init-site 上加 --motion {motion_id} --force 重跑一次才能生效"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, help="配置源目录", default=None)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
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
