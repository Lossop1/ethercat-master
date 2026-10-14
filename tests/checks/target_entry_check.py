#!/usr/bin/env python3
"""命令口的外部目标入口不许挂在设点开关上（离线，不接总线）。

用法:
    python3 tests/checks/target_entry_check.py
    python3 tests/checks/target_entry_check.py --mutate   # 证明这条判据抓得住

为什么查这一条。主站有两条送目标的路：命令口的文本动词 `set_external_target`（控制台、
图形界面、各台架脚本走的都是它），以及设点口 `-sp.sock` 那条高频通道。后者由
`EMASTER_SETPOINT` 开关按需建，README 的"配置模型"一节也是这么写的——**开关只管
设点口**。

2026-10-14 的 bb6356d 把两者绑在了一起：外部目标源的绑定也压到了 `EMASTER_SETPOINT`
上。于是默认关着的新开关悄悄废掉了已经交付的入口——命令口每一条目标都被拒
（"No external target buffer configured"），而各脚本要看的那些指标全都照样成立：

  · 命令流"全程连续"、以 0 退出（目标被拒也算一次成功往返，只是轴没动）；
  · 界面自检退出码 0、接管成功；
  · 周期照常推进、掉出 OP 计数为零。

当场没人看得出来：同一天在 MT 真机上，一整轮 6000 条目标一条都没生效，脚本判"通过"。
所以这里把关系钉死：绑定只看"部署有没有引用固定运动方案"，绝不再看那个开关；而设点口
自己该怎么受开关管还怎么管。

判据朝两个方向各查一次，缺一不可：只查"绑定里没有这个开关"的话，把绑定整段删掉也过；
只查"设点口仍受开关管"的话，绑定挂回去也过。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN_C = ROOT / "apps" / "master" / "main.c"

BIND_LINE = "callbacks.position_target_source = position_target_source;"
SERVER_LINE = "emaster_setpoint_server_create("

# 建设点口的那道门。按正则取而不是按字面量取：门下除了开关还有别的条件
# （有轴没有周期目标时这条通道也不建，见 mode 6 回零），字面量一改就找不着了。
GUARD_PATTERN = re.compile(r"if\s*\([^)]*setpoint_channel_enabled\(\)[^)]*\)")


def condition_above(text: str, anchor: str) -> str | None:
    """取某个语句上方最近的那个 if (...) 的条件。

    这段代码里绑定语句紧跟在 `if (...) {` 之后，中间没有别的语句，所以"往上找最近的
    if"取到的就是它的条件。
    """
    index = text.find(anchor)
    if index < 0:
        return None
    prefix = text[:index]
    matches = list(re.finditer(r"if\s*\(([^)]*)\)", prefix))
    if not matches:
        return None
    return matches[-1].group(1)


def verdict(text: str) -> list[str]:
    """返回不合格项；空表 = 通过。"""
    failures: list[str] = []

    binding = condition_above(text, BIND_LINE)
    if binding is None:
        failures.append("main.c 里找不到绑定外部目标源的那一段（%s）" % BIND_LINE)
    else:
        if "plan.motion_profile == NULL" not in binding:
            failures.append(
                "绑定的条件里没有 plan.motion_profile == NULL，取到的是：%s" % binding.strip()
            )
        if "setpoint_channel_enabled" in binding:
            failures.append(
                "绑定挂到了 EMASTER_SETPOINT 上（条件：%s）。开关只管设点口，"
                "命令口的 set_external_target 不许受它管" % binding.strip()
            )
        if "plan_has_cyclic_targets" not in binding:
            failures.append(
                "绑定的条件里没有 plan_has_cyclic_targets（条件：%s）。回零（模式 6）"
                "没有周期目标，那种部署建起这条通道只会得到"
                "\"设点口照常收、计数照常涨、轴不动\"" % binding.strip()
            )

    # 设点口这边不取"最近的 if"：建它的那句在开关那道门里又套了一层
    # `if (轴数上限 && 路径推得出来)`，最近的 if 是里面那层。改判"开关那道门在不在"，
    # 以及建口的那句在它后面——门上的开关拆掉，这个条件就没有了。
    guard = GUARD_PATTERN.search(text)
    server_at = text.find(SERVER_LINE)
    if server_at < 0:
        failures.append("main.c 里找不到建设点口的那一段（%s）" % SERVER_LINE)
    elif guard is None or guard.end() > server_at:
        failures.append(
            "设点口不再由 EMASTER_SETPOINT 按需建。开关的用途就是把这条通道默认关着"
        )

    return failures


def mutate_rebind(text: str) -> str:
    """把开关塞回绑定条件里——正是 bb6356d 干过的事。"""
    return text.replace(
        "if (plan.motion_profile == NULL && plan_has_cyclic_targets) {",
        "if (setpoint_channel_enabled() && plan.motion_profile == NULL && "
        "plan_has_cyclic_targets) {",
        1,
    )


def mutate_unguard(text: str) -> str:
    """把设点口的开关拆掉：通道不再问开关，开关就架空了。"""
    return GUARD_PATTERN.sub("if (plan_has_cyclic_targets)", text, count=1)


def mutate_drop_cyclic(text: str) -> str:
    """把"有轴没有周期目标就不建通道"这一条拿掉：回零部署又会拿到一条空通道。"""
    return text.replace(
        "if (plan.motion_profile == NULL && plan_has_cyclic_targets) {",
        "if (plan.motion_profile == NULL) {",
        1,
    )


MUTATIONS = (
    ("开关塞回绑定条件", mutate_rebind),
    ("拆掉设点口的开关", mutate_unguard),
    ("拿掉周期目标这一条", mutate_drop_cyclic),
)


def main() -> int:
    mutate = "--mutate" in sys.argv[1:]
    if not MAIN_C.exists():
        print("找不到 %s" % MAIN_C, file=sys.stderr)
        return 2

    text = MAIN_C.read_text(encoding="utf-8")

    if mutate:
        caught = 0
        for label, apply in MUTATIONS:
            mutated = apply(text)
            if mutated == text:
                print("变异没生效：%s（源码不是预期的写法，这条自检已经失去意义）" % label,
                      file=sys.stderr)
                return 1
            if verdict(mutated):
                print("ok    变异被抓住：%s之后判红" % label)
                caught += 1
            else:
                print("FAIL  变异没被抓住：%s之后仍然判过" % label)
        print()
        if caught != len(MUTATIONS):
            print("通过 %d 项，失败 %d 项" % (caught, len(MUTATIONS) - caught))
            return 1
        print("通过 %d 项，失败 0 项" % caught)
        return 0

    failures = verdict(text)
    for failure in failures:
        print("FAIL  %s" % failure)
    if failures:
        print()
        print("通过 0 项，失败 %d 项" % len(failures))
        return 1

    print("ok    外部目标源的绑定只看部署有没有固定运动方案、有没有周期目标")
    print("ok    设点口仍由 EMASTER_SETPOINT 按需建，开关没有被架空")
    print()
    print("通过 2 项，失败 0 项")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
