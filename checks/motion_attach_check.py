#!/usr/bin/env python3
"""运动方案挂载链（离线，不接总线）。

用法:
    python3 checks/motion_attach_check.py

为什么查这一条。六族里只有运动方案是"写了却不生效也一声不响"的：方案自己一路进得了
`config-runtime/runtime.flat`（`motion.0.*` 都在），可部署那一行写
`motion_profile_id_present=false`，主站于是照常起、照常空转，编进去的每轴换算与限幅
一次都用不上 —— 校验、构建、启动三处全都不出声。现场只看到"轴没怎么动"，而报告里
每个字段都正常，是回头最难查的一类。

所以本检查走一遍真实 CLI（add-device / init-site / build），钉住四件事：

  1. `init-site` 建出 `config/motion_profiles/` —— README 让客户编辑的就是这个目录，
     不建的话那一步根本走不下去；
  2. `--motion <ID>` 真的把方案挂上：部署档里写进了 ID，扁平文件里 present=true；
  3. 挂一份还没写出来的方案时当场非零退出，而不是留一份引用了不存在 ID 的部署；
  4. **写了方案却没有部署引用它时，build 必须出声** —— 点名是哪一份、怎么挂。

判别力从哪来：第 4 条不能只查"告警出现过"，那样告警永远出现也算过。所以第 2 条同时
断言**挂上之后那句告警必须消失**。两个方向一起判，才说明这条告警真的在跟着配置走。

退出码：0 = 全部符合；1 = 有不符合项；2 = 内部错误。
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GENERATOR = os.path.join(REPO, "gen", "emaster_config.py")

# 样本 ESI 写在本文件里，不读仓库里任何 ESI：交付时仓库不含电机配置。
# 一套模块、一个 csp，字段给全 CiA 402 要求的那六项 —— 缺 mode_of_operation
# 或 mode_display 时生成器会拒绝出运行方案，那样这条检查就测不到挂载了。
ESI = """<EtherCATInfo Version="1.0">
<Vendor><Id>#x00000001</Id><Name>check-vendor</Name></Vendor>
<Descriptions>
<Modules>
  <Module>
    <Type ModuleIdent="#x00119800">csp - axis</Type>
    <RxPdo><Index>#x1600</Index>
<Entry><Index>#x6040</Index><BitLen>16</BitLen><DataType>UINT</DataType></Entry>
<Entry><Index>#x607A</Index><BitLen>32</BitLen><DataType>INT32</DataType></Entry>
<Entry><Index>#x6060</Index><BitLen>8</BitLen><DataType>INT8</DataType></Entry>
</RxPdo>
    <TxPdo><Index>#x1A00</Index>
<Entry><Index>#x6041</Index><BitLen>16</BitLen><DataType>UINT</DataType></Entry>
<Entry><Index>#x6064</Index><BitLen>32</BitLen><DataType>INT32</DataType></Entry>
<Entry><Index>#x6061</Index><BitLen>8</BitLen><DataType>INT8</DataType></Entry>
</TxPdo>
  </Module>
</Modules>
<Devices>
  <Device>
    <Type ProductCode="#x00000011" RevisionNo="#x00000001">check-motion</Type>
    <Mailbox><CoE PdoAssign="true" PdoConfig="true"/></Mailbox>
    <Dc>
      <OpMode><Name>DC</Name><Desc>DC-Synchron</Desc><AssignActivate>#x0300</AssignActivate></OpMode>
    </Dc>
  </Device>
</Devices>
</Descriptions>
</EtherCATInfo>
"""

MOTION_ID = "demo"

SCAN = """interface=eth0
slaves=2
slave=1 name=check-motion vendor=0x00000001 product=0x00000011 revision=0x00000001 state=8
slave=2 name=check-motion vendor=0x00000001 product=0x00000011 revision=0x00000001 state=8
"""

# 轴 ID 是 init-site 按总线位置生成的，这里跟着写死：这份配置是检查自己造的，
# 两头一起改才不会对不上。
MOTION = {
    "schema_version": 1,
    "motion_profile_id": MOTION_ID,
    "status": "approved",
    "required_mode_id": "csp",
    "trajectory": "relative_linear_position",
    "coordinate_frame": "output_shaft",
    "duration_ms": 2000,
    "settle_ms": 200,
    "axes": [
        {
            "axis_id": f"axis_{position:02d}",
            "relative_angle_millidegrees": 30000,
            "max_following_error_millidegrees": 2000,
            "max_step_millidegrees": 500,
            "expected_position_scale": {
                "encoder_increments": 16384,
                "encoder_motor_revolutions": 1,
                "gear_motor_revolutions": 28,
                "gear_shaft_revolutions": 1,
            },
        }
        for position in (1, 2)
    ],
}

ORPHAN_MARK = "没有被任何部署引用"


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def _run(work, argv):
    return subprocess.run(
        [sys.executable, GENERATOR, *argv],
        cwd=work,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def _flat(work, name):
    path = os.path.join(work, "config-runtime", name)
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def _tokens(text):
    """扁平文件的一行是 `键<TAB>值`。返回 键 -> 值 的字典。"""
    pairs = {}
    for line in text.splitlines():
        if "\t" not in line:
            continue
        key, _, value = line.partition("\t")
        pairs[key] = value
    return pairs


def check(failures):
    work = tempfile.mkdtemp(prefix="emaster-motion-")
    try:
        _write(os.path.join(work, "esi", "mymotor", "sample.xml"), ESI)
        _write(os.path.join(work, "scan.txt"), SCAN)

        done = _run(work, ["add-device", "esi/mymotor/sample.xml"])
        if done.returncode != 0:
            failures.append(
                "add-device 失败（%d）：%s" % (done.returncode, done.stderr[-400:])
            )
            return
        # 设备档 ID 的拼法由生成器定（厂商名 + 型号 + 修订），这里读回来用，
        # 不跟着抄一遍 —— 抄了就是同一件事记两处，改名时只改一处的那种坏。
        devices_dir = os.path.join(work, "config", "devices")
        names = sorted(
            name for name in os.listdir(devices_dir) if name.endswith(".json")
        )
        if len(names) != 1:
            failures.append("应当恰好生成一份设备档，实际 %s" % names)
            return
        with open(os.path.join(devices_dir, names[0]), encoding="utf-8") as handle:
            profile_id = json.load(handle)["profile_id"]

        # ── 1. init-site 要建出运动方案目录 ─────────────────────────────
        site = [
            "init-site",
            "--device",
            profile_id,
            "--interface",
            "eth0",
            "--from-scan",
            "scan.txt",
        ]
        done = _run(work, site)
        if done.returncode != 0:
            failures.append(
                "init-site 失败（%d）：%s" % (done.returncode, done.stderr[-400:])
            )
            return
        if not os.path.isdir(os.path.join(work, "config", "motion_profiles")):
            failures.append(
                "init-site 没有建出 config/motion_profiles/：README 第 3 节第 6 步"
                "让客户编辑的目录必须由这里建出来"
            )

        # ── 3. 挂一份还没写的方案：当场拒绝 ─────────────────────────────
        dangling = _run(work, site + ["--motion", MOTION_ID, "--force"])
        if dangling.returncode == 0:
            failures.append(
                "挂一份还不存在的运动方案时 init-site 退出了 0：这会在配置源里留下一份"
                "引用了不存在 ID 的部署"
            )
        elif f"motion_profiles/{MOTION_ID}.json" not in dangling.stdout + dangling.stderr:
            failures.append("init-site 拒绝了，但没说清缺的是哪份文件")

        _write(
            os.path.join(work, "config", "motion_profiles", f"{MOTION_ID}.json"),
            json.dumps(MOTION, ensure_ascii=False, indent=2) + "\n",
        )

        # ── 4. 写了方案却没有部署引用它：build 必须出声 ──────────────────
        done = _run(work, ["build"])
        if done.returncode != 0:
            failures.append("build 失败（%d）：%s" % (done.returncode, done.stderr[-400:]))
            return
        if ORPHAN_MARK not in done.stdout:
            failures.append(
                "运动方案没有被任何部署引用，build 却什么都没说："
                "客户写完方案跑构建，拿到的是一个永远不会生效的配置"
            )
        elif MOTION_ID not in done.stdout.split(ORPHAN_MARK)[1]:
            failures.append("build 报了没有被引用的方案，但没点名是哪一份")

        # ── 2. 挂上：present=true，且那句告警必须消失 ────────────────────
        attached = _run(work, site + ["--motion", MOTION_ID, "--force"])
        if attached.returncode != 0:
            failures.append(
                "带 --motion 重跑 init-site 失败（%d）：%s"
                % (attached.returncode, attached.stderr[-400:])
            )
            return
        # 部署档名带主机名，检查不能写死它 —— 换台机器跑就找不到文件了。
        deployments_dir = os.path.join(work, "config", "deployments")
        names = sorted(
            name for name in os.listdir(deployments_dir) if name.endswith(".json")
        ) if os.path.isdir(deployments_dir) else []
        if len(names) != 1:
            failures.append("应当恰好生成一份部署档，实际 %s" % names)
            return
        with open(os.path.join(deployments_dir, names[0]), encoding="utf-8") as handle:
            deployment = json.load(handle)
        if deployment.get("motion_profile_id") != MOTION_ID:
            failures.append(
                "部署档里的 motion_profile_id 是 %r，应为 %r"
                % (deployment.get("motion_profile_id"), MOTION_ID)
            )

        done = _run(work, ["build"])
        if done.returncode != 0:
            failures.append(
                "挂上方案后 build 失败（%d）：%s" % (done.returncode, done.stderr[-400:])
            )
            return
        if ORPHAN_MARK in done.stdout:
            failures.append(
                "方案已经挂到部署上了，build 还在报它没有被引用："
                "这句告警不看配置，属于恒真的检查"
            )
        tokens = _tokens(_flat(work, "deployments.flat"))
        if tokens.get("deployment.0.motion_profile_id_present") != "true":
            failures.append(
                "部署扁平文件的 motion_profile_id_present 不是 true：%r"
                % tokens.get("deployment.0.motion_profile_id_present")
            )
        if tokens.get("deployment.0.motion_profile_id") != MOTION_ID:
            failures.append(
                "部署扁平文件里的运动方案 ID 是 %r，应为 %r"
                % (tokens.get("deployment.0.motion_profile_id"), MOTION_ID)
            )
        # 方案本身要真的编进去了 —— 挂载链断在"方案没进 runtime.flat"这一环时，
        # 主站会在加载部署那一刻才失败，而那已经不是挂载的问题了。
        if _tokens(_flat(work, "runtime.flat")).get("motion.0.id") != MOTION_ID:
            failures.append("runtime.flat 里没有这份运动方案：挂上了也没有东西可加载")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    failures = []
    check(failures)
    if failures:
        for line in failures:
            print("  [不符] %s" % line)
        print("运动方案挂载链：%d 项不符" % len(failures))
        return 1
    print("运动方案挂载链：全部通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
