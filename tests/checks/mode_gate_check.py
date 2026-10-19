#!/usr/bin/env python3
"""运行期校核设备档声明的模式集合（离线，不接总线；需要已构建的主站）。

用法:
    python3 tests/checks/mode_gate_check.py [构建目录，缺省 build/linux-debug]

为什么查这一条。模式能力是两个独立的事实：设备档声明它认哪几个 0x6060 取值，主站
另外有自己的一套过程映像能力。这两份事实分别来自两个运行时文件 —— catalog.flat 与
runtime.flat，主站启动时各自独立装载，生成链只管得住"生成那一刻"两者一致。谁手工改
过一份、或者换了一份设备档却没重新生成运行方案，运行期就只有这条门能拦住。

拦不住的样子是不出声的：主站照常起、照常进 OP，把设备带进它不认的模式 —— 或者在
主站还没实现的模式上不动。两种都不会有任何报错，只有轴不动，而报告里每个字段都正常。

模式号还跨厂商歧义：同一个通道 0x1601 在台架那台 MT 上是 PVT 控制通道、在良质上是
精简 CSP。所以这里同时钉住第三种形态 —— 设备声明了、主站驱动不了时，话必须说成
"缺口在主站这一侧"，不许说成设备的问题。

四种形态逐个跑一遍真主站：
  ① 两份文件一致 —— 不许出现模式相关的中止；
  ② 运行方案要的模式设备档没声明 —— 中止并点名轴与模式；
  ③ 设备声明了而主站驱动不了 —— 中止并说明缺的是主站；
  ④ 改回一致 —— 那句话必须消失（①与④是同一件事的两端：光有②③的话，一条"永远
     中止"的坏门也照样过）。

退出码：0 = 全部符合；1 = 有不符合项；2 = 内部错误。
"""

import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GENERATOR = os.path.join(REPO, "tools", "gen", "emaster_config.py")

# 样本 ESI 与扫描结果都写在本文件里，不读仓库里任何设备配置：交付树里本来就没有。
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

# 接口名刻意不存在。计划建好之后主站才去开网卡，所以这四种形态一个报文都不会发出去；
# 这条检查在任何机器上跑都碰不到总线，也就不会碰到轴。
ABSENT_INTERFACE = "emaster-check-absent0"


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
    """按整键替换若干行；replacements 里值为 None 表示删掉那一行。"""
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


def _append(path, key, value):
    """在文件末尾补一行。键按名字查，所以补在哪儿都一样。"""
    with open(path, "a", encoding="utf-8", newline="\n") as handle:
        handle.write("%s\t%s\n" % (key, value))


def _generate(work, argv):
    done = subprocess.run(
        [sys.executable, GENERATOR, *argv],
        cwd=work, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def _master(binary, work, deployment):
    """跑一次主站，返回 (退出码, 输出)。超时按 2 记 —— 卡住不该被当成通过。"""
    try:
        done = subprocess.run(
            [binary, "--deployment", deployment, "--config-dir", "config-runtime"],
            cwd=work, capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=60,
        )
    except subprocess.TimeoutExpired:
        return None, "<超时>"
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def _mode_line(text):
    """中止原因里那句模式相关的话；没有就返回 None。"""
    for line in text.splitlines():
        if "模式集合里没有它" in line or "还不能驱动它" in line:
            return line
    return None


def check(work, binary, failures):
    _write(os.path.join(work, "esi", "mymotor", "sample.xml"), ESI)
    _write(os.path.join(work, "scan.txt"), SCAN)

    code, output = _generate(work, ["add-device", "esi/mymotor/sample.xml"])
    if code != 0:
        failures.append("add-device 失败（%d）：%s" % (code, output[-400:]))
        return
    devices = os.path.join(work, "config", "devices")
    names = sorted(name for name in os.listdir(devices) if name.endswith(".json"))
    with open(os.path.join(devices, names[0]), encoding="utf-8") as handle:
        profile_id = json.load(handle)["profile_id"]

    code, output = _generate(work, [
        "init-site", "--device", profile_id,
        "--interface", ABSENT_INTERFACE, "--from-scan", "scan.txt",
    ])
    if code != 0:
        failures.append("init-site 失败（%d）：%s" % (code, output[-400:]))
        return
    code, output = _generate(work, ["build"])
    if code != 0:
        failures.append("build 失败（%d）：%s" % (code, output[-400:]))
        return

    runtime_dir = os.path.join(work, "config-runtime")
    catalog = os.path.join(runtime_dir, "catalog.flat")
    runtime = os.path.join(runtime_dir, "runtime.flat")
    deployment = _tokens(os.path.join(runtime_dir, "deployments.flat"))["deployment.0.id"]

    catalog_tokens = _tokens(catalog)
    runtime_tokens = _tokens(runtime)
    mode_value_key = "operation.0.modes.0.value"
    declared = sorted(
        int(value)
        for key, value in catalog_tokens.items()
        if re.fullmatch(r"profile\.0\.supported_modes\.\d+", key)
    )
    if catalog_tokens.get("profile.0.supported_modes.count") != str(len(declared)):
        failures.append("设备档声明的模式条数与逐项不一致")
    original_mode = int(runtime_tokens[mode_value_key])
    if original_mode not in declared:
        failures.append("客户入口生成的两份文件自己对不上：运行方案要 %d，设备档声明 %s"
                        % (original_mode, declared))
        return

    other_mode = next(
        (value for value in (8, 9, 10, 5) if value != original_mode), 9
    )

    # ① 一致：不许出现模式相关的中止。
    code, output = _master(binary, work, deployment)
    if code is None:
        failures.append("① 一致时主站卡住没退出")
    elif _mode_line(output) is not None:
        failures.append("① 两份文件一致，主站却报了模式问题：%s" % _mode_line(output))

    # ② 运行方案要的模式，设备档没声明。
    _edit(runtime, {mode_value_key: str(other_mode)})
    code, output = _master(binary, work, deployment)
    line = _mode_line(output)
    if line is None:
        failures.append("② 运行方案要的模式设备档没声明，主站没中止")
    else:
        if code == 0:
            failures.append("② 模式对不上，主站却退出 0")
        if "模式集合里没有它" not in line:
            failures.append("② 中止原因说成了别的：%s" % line)
        if "轴 1" not in line:
            failures.append("② 中止原因没点名是哪根轴：%s" % line)

    # ③ 设备声明了，主站驱动不了。设备档里补一条主站还没实现的模式（PP = 1），
    #    运行方案跟着改成它 —— 设备说明书里声明了、主站执行层还没做的形态。
    #    不要用 5：MIT 已进执行层，拿它当"驱动不了"的样本会让这条检查恒绿。
    #    这条键原本不存在，_edit 替换不到，得新补一行。
    _edit(catalog, {"profile.0.supported_modes.count": str(len(declared) + 1)})
    _append(catalog, "profile.0.supported_modes.%d" % len(declared), "1")
    _edit(runtime, {mode_value_key: "1"})
    code, output = _master(binary, work, deployment)
    line = _mode_line(output)
    if line is None:
        failures.append("③ 设备声明了、主站驱动不了，主站没中止")
    elif "还不能驱动它" not in line:
        failures.append("③ 中止原因说成了别的（会被当成设备的问题）：%s" % line)

    # ④ 改回一致：那句话必须消失。
    _edit(catalog, {
        "profile.0.supported_modes.count": str(len(declared)),
        "profile.0.supported_modes.%d" % len(declared): None,
    })
    _edit(runtime, {mode_value_key: str(original_mode)})
    code, output = _master(binary, work, deployment)
    if code is None:
        failures.append("④ 改回一致后主站卡住没退出")
    elif _mode_line(output) is not None:
        failures.append("④ 改回一致，模式那句话没消失：%s" % _mode_line(output))


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build/linux-debug"
    binary = os.path.abspath(os.path.join(REPO, build_dir, "apps", "master", "emaster-master"))
    if not os.path.isfile(binary):
        print("不合格：找不到主站二进制 %s（先构建，或把构建目录作为第一个参数）" % binary)
        return 1

    failures = []
    work = tempfile.mkdtemp(prefix="emaster-mode-gate-")
    try:
        check(work, binary, failures)
    finally:
        import shutil
        shutil.rmtree(work, ignore_errors=True)

    for failure in failures:
        print("不合格：%s" % failure)
    print("\n不合格 %d 项" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
