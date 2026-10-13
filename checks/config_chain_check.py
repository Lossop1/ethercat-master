#!/usr/bin/env python3
"""配置引用链闭合检查（离线，不接总线）。

用法:
    python3 checks/config_chain_check.py            # 检查 config/ 下的六族

配置源还在 JSON 这一层（`gen/emaster_config.py build` 才把它们变成运行时读的扁平文件），
所以一条断掉的引用链不会在运行时优雅降级，而是变成主站启动时的某一句笼统报错。
这个检查把它提前到生成之前。生成器自己也校验，这里查的是同一件事的入口早提醒。

交付时六族都是空的，本检查在空树上通过（0 份配置）—— 空不是错。

查什么：

  1. 唯一性 —— 每一类配置里的 id 不得重复。重复时"按 id 找文件"会命中先遇到的那一个，
     效果是**改了一份配置而实际生效的是另一份**，而且没有任何提示。
  2. 引用闭合 —— 部署档引用的拓扑/运动方案/操作方案/错误恢复策略必须存在；
     拓扑里每台从站引用的设备档必须存在。这是主站启动必经的链。
  3. 换算自洽 —— 运动方案每轴的 expected_position_scale 里编码器分辨率与减速比必须为正。
     零或负会让 counts 换算变成 0 或负数，而主站只在会话失败时笼统地报"位置换算无效"。
  4. 设备档最低完整性 —— 身份三件套与慢速遥测清单必须存在；后者是观测线程的输入。

不查什么（有意）：

  · 不查"配置与实车是否一致"。那需要接总线读真实身份与换算，是台架侧的事（见 docs/test-plan.md 的 F1）。本工具只保证配置内部自洽。
  · 不查数值是否合理（例如减速比 25 与 28 哪个对）。那不是文件能回答的问题。

**提示与问题是两回事。** 缺 motion_profile_id 是合法配置（空闲档就不需要运动方案），
但它意味着"该部署档不能用于运动类轮次"——因为护栏与角度换算都来自运动方案。所以它
以提示形式列出，不影响退出码。把合法配置报成错误，只会让这个检查被忽略。

退出码：0 = 没有问题；1 = 有问题；2 = 用法或读文件出错。
"""

import glob
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 类别 -> (目录, id 字段)。顺序即报错顺序，按"从入口往里"排。
KINDS = (
    ("部署档", "deployments", "deployment_id"),
    ("拓扑", "topologies", "topology_id"),
    ("运动方案", "motion_profiles", "motion_profile_id"),
    ("操作方案", "operation_profiles", "operation_profile_id"),
    ("错误恢复策略", "error_recovery_policies", "policy_id"),
    ("设备档", "devices", "profile_id"),
)


def load_all(kinds_dir, id_field):
    """读一类配置，返回 (id -> path, [问题])。读不了的 JSON 也要报出来。"""
    found = {}
    problems = []
    for path in sorted(glob.glob(os.path.join(REPO, "config", kinds_dir, "*.json"))):
        rel = os.path.relpath(path, REPO)
        try:
            with open(path, encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError) as error:
            problems.append("%s 读不了或不是合法 JSON：%s" % (rel, error))
            continue
        identifier = data.get(id_field)
        if not identifier:
            problems.append("%s 没有 %s 字段" % (rel, id_field))
            continue
        if identifier in found:
            # 重复的 id 不会报错，只会让"按 id 找文件"命中先遇到的那一个 ——
            # 于是改了一份配置而生效的是另一份。
            problems.append("id 重复：%s 在 %s 与 %s 里都出现"
                            % (identifier, os.path.relpath(found[identifier], REPO), rel))
            continue
        found[identifier] = path
    return found, problems


def main():
    if len(sys.argv) > 2:
        print(__doc__)
        return 2

    tables = {}
    problems = []
    notes = []
    for label, kinds_dir, id_field in KINDS:
        tables[label], found_problems = load_all(kinds_dir, id_field)
        problems.extend(found_problems)

    def need(label, identifier, where):
        if not identifier:
            problems.append("%s 没有引用%s" % (where, label))
        elif identifier not in tables[label]:
            problems.append("%s 引用的%s %s 不存在" % (where, label, identifier))

    # 1. 部署档 -> 其余四类
    for identifier, path in sorted(tables["部署档"].items()):
        rel = os.path.relpath(path, REPO)
        try:
            with open(path, encoding="utf-8") as handle:
                deployment = json.load(handle)
        except (OSError, ValueError):
            continue  # 读不了上面已经报过
        need("拓扑", deployment.get("topology_id"), rel)
        if deployment.get("motion_profile_id"):
            need("运动方案", deployment.get("motion_profile_id"), rel)
        else:
            # 合法：空闲档不需要运动方案。但它跑不了运动类轮次，说清楚而不是判它错。
            notes.append("%s 没有运动方案 —— 只能空转，跑不了运动类轮次" % rel)
        need("错误恢复策略", deployment.get("error_recovery_policy_id"), rel)
        for operation in deployment.get("operation_profile_ids") or []:
            need("操作方案", operation, rel)

    # 2. 拓扑 -> 设备档
    for identifier, path in sorted(tables["拓扑"].items()):
        rel = os.path.relpath(path, REPO)
        try:
            with open(path, encoding="utf-8") as handle:
                topology = json.load(handle)
        except (OSError, ValueError):
            continue
        slaves = topology.get("slaves") or []
        if not slaves:
            problems.append("%s 里一台从站都没有" % rel)
        for slave in slaves:
            need("设备档", slave.get("profile_id"),
                 "%s 的第 %s 号从站" % (rel, slave.get("position")))

    # 3. 运动方案 -> 换算自洽
    for identifier, path in sorted(tables["运动方案"].items()):
        rel = os.path.relpath(path, REPO)
        try:
            with open(path, encoding="utf-8") as handle:
                profile = json.load(handle)
        except (OSError, ValueError):
            continue
        axes = profile.get("axes") or []
        if not axes:
            problems.append("%s 里一根轴都没有" % rel)
        for axis in axes:
            scale = axis.get("expected_position_scale") or {}
            where = "%s 的 %s" % (rel, axis.get("axis_id"))
            for key in ("encoder_increments", "encoder_motor_revolutions",
                        "gear_motor_revolutions", "gear_shaft_revolutions"):
                value = scale.get(key)
                if value is None:
                    problems.append("%s 缺 %s" % (where, key))
                elif not isinstance(value, (int, float)) or value <= 0:
                    # 零或负会让 counts 换算变成 0 或负数，而主站只会笼统地报
                    # "运动方案、位置换算或目标范围无效"。
                    problems.append("%s 的 %s 不是正数（%r）" % (where, key, value))

    # 4. 设备档 -> 最低完整性
    for identifier, path in sorted(tables["设备档"].items()):
        rel = os.path.relpath(path, REPO)
        try:
            with open(path, encoding="utf-8") as handle:
                device = json.load(handle)
        except (OSError, ValueError):
            continue
        identity = device.get("identity") or {}
        for key in ("vendor_id", "product_code", "revision"):
            if not identity.get(key):
                problems.append("%s 的身份缺 %s" % (rel, key))
        telemetry = device.get("slow_telemetry")
        if not telemetry:
            # 慢速遥测清单是观测线程的输入，缺了它对运行没有影响，
            # 但会让所有"运行时读到了什么"的问题都无解 —— 所以在这里拦。
            problems.append("%s 没有 slow_telemetry 清单" % rel)

    for line in notes:
        print("提示：%s" % line)

    if problems:
        print("配置引用链检查：发现 %d 个问题" % len(problems))
        for line in problems:
            print("  %s" % line)
        return 1
    total = sum(len(table) for table in tables.values())
    print("配置引用链检查通过：%d 份配置，引用全部闭合（%d 条提示）" % (total, len(notes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
