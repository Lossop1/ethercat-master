#!/usr/bin/env python3
"""模式声明的目标量：断供与拒收按它分（离线，不接总线；需要已构建的主站）。

用法:
    python3 tests/checks/mode_target_check.py [构建目录，缺省 build/linux-debug]

为什么查这一条。外部目标通道收的是一个数，而这个数是什么量由模式决定：位置模式下它是
位置计数、速度模式下是计数每秒、力矩模式下是千分比。三件事随之不同 —— 写进哪个 PDO
字段、反馈从哪一路读、以及**断供之后写什么**：

  position  断供保持在原位；判"目标离实际位置太远就拒收"。
  velocity  断供写 0（保持上一条速度 = 轴一直跑）；不判距离。
  torque    断供写 0（保持上一条力矩 = 轴一直出力）；不判距离。

按模式号认会错：同一个取值在不同厂商的说明书里指的不是同一件事（台架那台 MT 的通道
0x1601 是 PVT 控制通道，良质的 0x1601 是精简 CSP）。所以主站读的是运行方案里声明的量。

拦不住的样子是不出声的：CSV 部署客户端一断，轴按最后一条速度一直转下去，报告里每个
字段都正常。这条检查要钉的就是"判据来自声明、声明缺了不许放行"。

四种形态逐个跑一遍真主站：
  ① 声明 position —— 不许出现非位置模式的告警；
  ② 声明 velocity —— 必须出现，且点名是哪个模式；
  ③ 把声明改成 position（与本套 PDO 的字段对不上）—— 告警必须消失（证明读的是声明，
     不是模式号）；
  ④ 把声明删掉 —— 主站必须拒绝启动（缺声明是硬失败，不是静默按模式号兜底）。

接口名刻意不存在。计划建好之后主站才去开网卡，所以这四种形态一个报文都不会发出去；
这条检查在任何机器上跑都碰不到总线，也就不会碰到轴。

退出码：0 = 全部符合；1 = 有不符合项；2 = 内部错误。
"""

import json
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GENERATOR = os.path.join(REPO, "tools", "gen", "emaster_config.py")

# 样本 ESI 与扫描结果都写在本文件里，不读仓库里任何设备配置：交付树里本来就没有。
# 两份只差模块类型与映射的对象：一份位置（607A/6064），一份速度（60FF/606C）。
ESI_TEMPLATE = """<EtherCATInfo Version="1.0">
<Vendor><Id>#x00000001</Id><Name>check-vendor</Name></Vendor>
<Descriptions>
<Modules>
  <Module>
    <Type ModuleIdent="#x{mident}">{mode} - axis</Type>
    <RxPdo><Index>#x1600</Index>
<Entry><Index>#x6040</Index><BitLen>16</BitLen><DataType>UINT</DataType></Entry>
<Entry><Index>#x{target}</Index><BitLen>32</BitLen><DataType>INT32</DataType></Entry>
<Entry><Index>#x6060</Index><BitLen>8</BitLen><DataType>INT8</DataType></Entry>
</RxPdo>
    <TxPdo><Index>#x1A00</Index>
<Entry><Index>#x6041</Index><BitLen>16</BitLen><DataType>UINT</DataType></Entry>
<Entry><Index>#x{feedback}</Index><BitLen>32</BitLen><DataType>INT32</DataType></Entry>
<Entry><Index>#x6061</Index><BitLen>8</BitLen><DataType>INT8</DataType></Entry>
</TxPdo>
  </Module>
</Modules>
<Devices>
  <Device>
    <Type ProductCode="#x00000011" RevisionNo="#x00000001">check-mode</Type>
    <Mailbox><CoE PdoAssign="true" PdoConfig="true"/></Mailbox>
    <Dc>
      <OpMode><Name>DC</Name><Desc>DC-Synchron</Desc><AssignActivate>#x0300</AssignActivate></OpMode>
    </Dc>
  </Device>
</Devices>
</Descriptions>
</EtherCATInfo>
"""

SCAN = """interface=eth0
slaves=1
slave=1 name=check-mode vendor=0x00000001 product=0x00000011 revision=0x00000001 state=8
"""

# 位置形态与速度形态各自的模块标识、目标对象、反馈对象。
FORMS = {
    "csp": {"mident": "00119800", "target": "607A", "feedback": "6064"},
    "csv": {"mident": "00119900", "target": "60FF", "feedback": "606C"},
}

ABSENT_INTERFACE = "emaster-check-absent0"

NOT_POSITION_MARKER = "Non-position mode"
QUANTITY_KEY = "operation.0.modes.0.quantity"


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def _tokens(path):
    """扁平文件的一行是 `键<TAB>值`。返回 键 -> 值 的字典。"""
    pairs = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            key, separator, value = line.rstrip("\n").partition("\t")
            if separator:
                pairs[key] = value
    return pairs


def _edit(path, replacements):
    """按整键替换若干行；值为 None 表示删掉那一行。"""
    with open(path, encoding="utf-8") as handle:
        lines = handle.readlines()
    out = []
    for line in lines:
        key, separator, _ = line.rstrip("\n").partition("\t")
        if separator and key in replacements:
            value = replacements[key]
            if value is not None:
                out.append("%s\t%s\n" % (key, value))
            continue
        out.append(line)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.writelines(out)


def _generate(work, argv):
    done = subprocess.run(
        [sys.executable, GENERATOR, *argv],
        cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def _master(binary, work, deployment):
    """跑一次主站，返回 (退出码, 输出)。超时按 None 记 —— 卡住不该被当成通过。"""
    try:
        done = subprocess.run(
            [binary, "--deployment", deployment, "--config-dir", "config-runtime"],
            cwd=work, capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=60,
        )
    except subprocess.TimeoutExpired:
        return None, "<超时>"
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def _not_position_line(text):
    for line in text.splitlines():
        if NOT_POSITION_MARKER in line:
            return line
    return None


def _build_site(work, mode, failures):
    """一种形态：造 ESI、扫一份、生成整套配置。返回部署 ID 或 None。"""
    form = FORMS[mode]
    _write(os.path.join(work, "esi", "mymotor", "sample.xml"),
           ESI_TEMPLATE.format(mode=mode, **form))
    _write(os.path.join(work, "scan.txt"), SCAN)

    code, output = _generate(work, ["add-device", "esi/mymotor/sample.xml"])
    if code != 0:
        failures.append("%s 形态：add-device 失败（%d）：%s" % (mode, code, output[-400:]))
        return None
    devices = os.path.join(work, "config", "devices")
    names = sorted(name for name in os.listdir(devices) if name.endswith(".json"))
    with open(os.path.join(devices, names[0]), encoding="utf-8") as handle:
        profile_id = json.load(handle)["profile_id"]

    for argv, label in (
        (["init-site", "--device", profile_id, "--interface", ABSENT_INTERFACE,
          "--from-scan", "scan.txt", "--mode", mode], "init-site"),
        (["build"], "build"),
    ):
        code, output = _generate(work, argv)
        if code != 0:
            failures.append("%s 形态：%s 失败（%d）：%s" % (mode, label, code, output[-400:]))
            return None
    return _tokens(os.path.join(work, "config-runtime", "deployments.flat"))["deployment.0.id"]


def check_position(binary, work, deployment, failures):
    """① 位置：不许出现非位置模式的告警。"""
    code, output = _master(binary, work, deployment)
    if code is None:
        failures.append("① 位置形态：主站卡住没退出")
        return
    line = _not_position_line(output)
    if line is not None:
        failures.append("① 声明 position，主站却按非位置模式处理：%s" % line)


def check_velocity(binary, work, deployment, failures):
    """② 速度：必须告警，且点名模式；③ ④ 在此基础上改声明。"""
    code, output = _master(binary, work, deployment)
    if code is None:
        failures.append("② 速度形态：主站卡住没退出")
        return
    line = _not_position_line(output)
    if line is None:
        failures.append("② 声明 velocity，主站没按非位置模式处理（客户端断供后轴会一直跑）")
        return
    if "csv" not in line:
        failures.append("② 告警没点名是哪个模式：%s" % line)

    runtime = os.path.join(work, "config-runtime", "runtime.flat")
    with open(runtime, encoding="utf-8") as handle:
        original = handle.read()
    if QUANTITY_KEY not in _tokens(runtime):
        failures.append("② 运行方案的扁平文件里没有 %s：声明没进到运行期" % QUANTITY_KEY)
        return

    # ③ 声明改成 position：告警必须消失 —— 证明主站读的是声明，不是模式号（9 没变）。
    _edit(runtime, {QUANTITY_KEY: "EMASTER_MODE_QUANTITY_POSITION"})
    code, output = _master(binary, work, deployment)
    if code is None:
        failures.append("③ 改声明后主站卡住没退出")
    elif _not_position_line(output) is not None:
        failures.append("③ 声明已改成 position，主站仍按模式号判成非位置：%s"
                        % _not_position_line(output))
    elif "错误：无法从" in output:
        failures.append("③ 声明与 PDO 字段对不上时主站没走到计划阶段就退出了")

    # ④ 删掉声明：必须拒绝启动，不能按模式号兜底。
    _edit(runtime, {QUANTITY_KEY: None})
    code, output = _master(binary, work, deployment)
    if code is None:
        failures.append("④ 删掉声明后主站卡住没退出")
    elif code == 0:
        failures.append("④ 删掉模式声明的量，主站照常启动")
    elif "无法从" not in output:
        failures.append("④ 缺声明的失败原因不是配置装载：%s" % output.strip().splitlines()[-1:])

    with open(runtime, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(original)


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build/linux-debug"
    binary = os.path.abspath(os.path.join(REPO, build_dir, "apps", "master", "emaster-master"))
    if not os.path.isfile(binary):
        print("不合格：找不到主站二进制 %s（先构建，或把构建目录作为第一个参数）" % binary)
        return 1

    failures = []
    root = tempfile.mkdtemp(prefix="emaster-mode-target-")
    try:
        for mode, runner in (("csp", check_position), ("csv", check_velocity)):
            work = os.path.join(root, mode)
            os.makedirs(work)
            deployment = _build_site(work, mode, failures)
            if deployment is None:
                continue
            runner(binary, work, deployment, failures)
    finally:
        import shutil
        shutil.rmtree(root, ignore_errors=True)

    for failure in failures:
        print("不合格：%s" % failure)
    print("\n不合格 %d 项" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
