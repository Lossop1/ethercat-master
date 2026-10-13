#!/usr/bin/env python3
"""设备档推导链的检查：ESI → 硬件事实 → 设备档（离线，不接总线）。

用法:
    python3 checks/device_facts_check.py

为什么查这一条。这条链是"客户换自己的电机、不重编主站"的唯一入口，而它坏掉的样子
是不出声的：映射条目顺序被重排、位偏移整体错位，设备照样进 OP、反馈值照样有数，
只是数对不上。所以这里钉住三件事 —— 推导结果逐项固定、合成出的设备档字段位置正确、
**推不出来的时候必须非零退出**。猜着往下走的代价是拿错的映射跑真轴。

样本从哪来。这份检查自带的 ESI 是本文件里的字符串，不读仓库里任何 ESI：项目交付
时不含任何电机配置（连 ESI 都没有，客户把自己那份放进 esi/）。用合成样本的好处是
每条断言背后的输入就写在旁边，改了推导规则立刻看得出哪条断言该跟着改。
"""
import json
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "gen", "device"))

from derive_device_facts import derive  # noqa: E402
from esi_facts import extract_esi_facts  # noqa: E402


def _entry(index: str, bits: int, data_type: str = "UINT") -> str:
    return (
        f"<Entry><Index>{index}</Index><BitLen>{bits}</BitLen>"
        f"<DataType>{data_type}</DataType></Entry>"
    )


def _pdo(index: str, entries: list[str], sm: int | None = None) -> str:
    attribute = "" if sm is None else f' Sm="{sm}"'
    return (
        f"<PdoHolder{attribute}><Index>{index}</Index>"
        f"{''.join(entries)}</PdoHolder>"
    )


def _wrap(tag: str, pdo: str) -> str:
    return pdo.replace("<PdoHolder", f"<{tag}").replace("</PdoHolder>", f"</{tag}>")


# 模块化 ESI：PDO 挂在 <Module> 下，靠 0xF030 选模块。三套映射的宽度各不相同，
# 宽度算错就是整帧位偏移错位，所以这里逐套钉住字节数。
MODULAR_ESI = """<EtherCATInfo Version="1.0">
<Vendor><Id>#x00000001</Id><Name>检查用厂商</Name></Vendor>
<Descriptions>
<Modules>
  <Module>
    <Type ModuleIdent="#x00119800">csv,csp,cst - axis</Type>
    {rx_a}
    {tx_a}
  </Module>
  <Module>
    <Type ModuleIdent="#x00219800">csp - axis</Type>
    {rx_b}
    {tx_b}
  </Module>
  <Module>
    <Type ModuleIdent="#x00319800">cst - axis</Type>
    {rx_c}
    {tx_c}
  </Module>
</Modules>
<Devices>
  <Device>
    <Type ProductCode="#x00000011" RevisionNo="#x00000001">check-modular</Type>
    <Mailbox><CoE PdoAssign="true" PdoConfig="true"/></Mailbox>
    <Dc>
      <OpMode><Name>DC</Name><Desc>DC-Synchron</Desc><AssignActivate>#x0300</AssignActivate></OpMode>
      <OpMode><Name>Synchron</Name><Desc>SM-Synchron</Desc><AssignActivate>#x0000</AssignActivate></OpMode>
    </Dc>
  </Device>
</Devices>
</Descriptions>
</EtherCATInfo>""".format(
    rx_a=_wrap("RxPdo", _pdo("#x1600", [
        _entry("#x6040", 16), _entry("#x607A", 32, "INT32"),
        _entry("#x60FF", 32, "INT32"), _entry("#x6071", 16, "INT16"),
    ])),
    tx_a=_wrap("TxPdo", _pdo("#x1A00", [
        _entry("#x6041", 16), _entry("#x6064", 32, "INT32"),
        _entry("#x606C", 32, "INT32"), _entry("#x6077", 16, "INT16"),
    ])),
    rx_b=_wrap("RxPdo", _pdo("#x1600", [
        _entry("#x6040", 16), _entry("#x607A", 32, "INT32"),
    ])),
    tx_b=_wrap("TxPdo", _pdo("#x1A00", [
        _entry("#x6041", 16), _entry("#x6064", 32, "INT32"),
    ])),
    rx_c=_wrap("RxPdo", _pdo("#x1600", [
        _entry("#x6040", 16), _entry("#x6071", 16, "INT16"),
    ])),
    tx_c=_wrap("TxPdo", _pdo("#x1A00", [
        _entry("#x6041", 16), _entry("#x6077", 16, "INT16"),
    ])),
)

MODULAR_EXPECTED = [
    ("0x00119800", 12, 12),
    ("0x00219800", 6, 6),
    ("0x00319800", 4, 4),
]
MODULAR_RX_A_ORDER = ["0x6040", "0x607A", "0x60FF", "0x6071"]

# 非模块化 ESI：没有 <Module>，PDO 直挂 <Device>，靠对象字典的出厂默认分配
# （0x1C12 = 0x1600、0x1C13 = 0x1A00）与 PDO 上的 Sm 属性互相印证出唯一一套。
DIRECT_ESI = """<EtherCATInfo Version="1.0">
<Vendor><Id>#x00000001</Id><Name>检查用厂商</Name></Vendor>
<Descriptions>
<Devices>
  <Device>
    <Type ProductCode="#x00000022" RevisionNo="#x00000001">check-direct</Type>
    <Mailbox><CoE PdoAssign="true" PdoConfig="true"/></Mailbox>
    <Profile><Dictionary><Objects>
      <Object><Index>#x1C12</Index><Info>
        <SubItem><Info><DefaultData>01</DefaultData></Info></SubItem>
        <SubItem><Info><DefaultData>0016</DefaultData></Info></SubItem>
      </Info></Object>
      <Object><Index>#x1C13</Index><Info>
        <SubItem><Info><DefaultData>01</DefaultData></Info></SubItem>
        <SubItem><Info><DefaultData>001A</DefaultData></Info></SubItem>
      </Info></Object>
    </Objects></Dictionary></Profile>
    {rx}
    {tx}
  </Device>
</Devices>
</Descriptions>
</EtherCATInfo>""".format(
    rx=_wrap("RxPdo", _pdo("#x1600", [
        _entry("#x6040", 16), _entry("#x607A", 32, "INT32"),
        _entry("#x60FF", 32, "INT32"), _entry("#x6071", 16, "INT16"),
        _entry("#x6072", 16, "INT16"), _entry("#x6060", 8, "SINT"),
        _entry("#x2FFD", 16),
    ], sm=2)),
    tx=_wrap("TxPdo", _pdo("#x1A00", [
        _entry("#x6041", 16), _entry("#x6064", 32, "INT32"),
        _entry("#x606C", 32, "INT32"), _entry("#x6077", 16, "INT16"),
        _entry("#x603F", 16), _entry("#x6061", 8, "SINT"),
        _entry("#x2FFE", 16),
    ], sm=3)),
)

DIRECT_EXPECTED_RX = ["0x6040", "0x607A", "0x60FF", "0x6071", "0x6072", "0x6060", "0x2FFD"]
DIRECT_EXPECTED_TX = ["0x6041", "0x6064", "0x606C", "0x6077", "0x603F", "0x6061", "0x2FFE"]

# 推不出来的两种 ESI。都不该有产物。
BROKEN_ESI = {
    "既无模块也无默认分配": """<EtherCATInfo><Vendor><Id>1</Id><Name>t</Name></Vendor>
<Descriptions><Devices><Device><Type ProductCode="#x1" RevisionNo="#x1">t</Type>
<RxPdo><Index>#x1600</Index><Entry><Index>#x6040</Index><BitLen>16</BitLen>
<DataType>UINT</DataType></Entry></RxPdo>
<TxPdo><Index>#x1A00</Index><Entry><Index>#x6041</Index><BitLen>16</BitLen>
<DataType>UINT</DataType></Entry></TxPdo>
</Device></Devices></Descriptions></EtherCATInfo>""",
    # 默认分配说是 0x1600，而带 Sm="2" 的那张映射是 0x1601：两条规则给出不同集合。
    "默认分配与 Sm 互相矛盾": """<EtherCATInfo><Vendor><Id>1</Id><Name>t</Name></Vendor>
<Descriptions><Devices><Device><Type ProductCode="#x1" RevisionNo="#x1">t</Type>
<Profile><Dictionary><Objects>
<Object><Index>#x1C12</Index><Info>
<SubItem><Info><DefaultData>01</DefaultData></Info></SubItem>
<SubItem><Info><DefaultData>0016</DefaultData></Info></SubItem>
</Info></Object>
<Object><Index>#x1C13</Index><Info>
<SubItem><Info><DefaultData>01</DefaultData></Info></SubItem>
<SubItem><Info><DefaultData>001A</DefaultData></Info></SubItem>
</Info></Object>
</Objects></Dictionary></Profile>
<RxPdo Sm="2"><Index>#x1601</Index><Entry><Index>#x6040</Index><BitLen>16</BitLen>
<DataType>UINT</DataType></Entry></RxPdo>
<TxPdo Sm="3"><Index>#x1A00</Index><Entry><Index>#x6041</Index><BitLen>16</BitLen>
<DataType>UINT</DataType></Entry></TxPdo>
</Device></Devices></Descriptions></EtherCATInfo>""",
}


def _facts_of(text):
    return extract_esi_facts(ET.fromstring(text))


def _run(argv):
    done = subprocess.run(
        [sys.executable] + argv, capture_output=True, text=True, cwd=REPO,
        encoding="utf-8", errors="replace",
    )
    return done.returncode


def _write(path, payload):
    with open(path, "w", encoding="utf-8") as handle:
        if isinstance(payload, str):
            handle.write(payload)
        else:
            json.dump(payload, handle, ensure_ascii=False)
    return path


def check_modular(failures):
    facts = derive(_facts_of(MODULAR_ESI))
    got = [
        (pdo["module_ident"], pdo["rx"]["bytes"], pdo["tx"]["bytes"])
        for pdo in facts["pdo_sets"]
    ]
    if got != MODULAR_EXPECTED:
        failures.append("模块化 ESI 推导结果变了：%s（应为 %s）" % (got, MODULAR_EXPECTED))
    if facts["protocol"]["supported_modes"] != {"csp": 8, "csv": 9, "cst": 10}:
        failures.append("模块化 ESI 的模式集合变了：%s" % facts["protocol"]["supported_modes"])
    style = {pdo["assignment_style"] for pdo in facts["pdo_sets"]}
    if style != {"module_slot"}:
        failures.append("模块化 ESI 的分配方式变了：%s" % sorted(style))
    # 同步怎么使能是 ESI 给的硬事实，运行方案的 assign_activate 直接取这里。
    # 取不到就得由人写，不许拿"标准上一般是 0x0300"顶上。
    assign = facts["protocol"]["assign_activate_by_strategy"]
    if assign != {"dc": "0x00000300", "sm": "0x00000000"}:
        failures.append("模块化 ESI 的 AssignActivate 事实变了：%s" % assign)
    # 条目顺序就是位偏移的累加顺序，重排一次整帧就对不上。
    first = facts["pdo_sets"][0]["rx"]["mappings"][0]["entries"]
    order = [entry["index"] for entry in first]
    if order != MODULAR_RX_A_ORDER:
        failures.append("模块化 ESI 的条目顺序变了：%s" % order)


def check_direct(failures):
    facts = derive(_facts_of(DIRECT_ESI))
    pdo_sets = facts["pdo_sets"]
    if len(pdo_sets) != 1:
        failures.append("非模块化 ESI 应推出恰好一套 PDO，实际 %d 套" % len(pdo_sets))
        return
    pdo = pdo_sets[0]
    if pdo["assignment_style"] != "direct_1c12" or pdo["module_ident"] is not None:
        failures.append(
            "非模块化 ESI 的分配方式应为 direct_1c12 且无模块标识：%s"
            % json.dumps(pdo, ensure_ascii=False)[:120]
        )
    # 这份样本没有 <Dc> 段：厂商没声明同步方式，事实就是空映射 ——
    # 不是"补上标准值"，补上就成了假的设备事实。
    if facts["protocol"]["assign_activate_by_strategy"] != {}:
        failures.append(
            "没有 <Dc> 的 ESI 不该推出发明出来的 AssignActivate：%s"
            % facts["protocol"]["assign_activate_by_strategy"]
        )
    for direction, expected in (("rx", DIRECT_EXPECTED_RX), ("tx", DIRECT_EXPECTED_TX)):
        mappings = pdo[direction]["mappings"]
        if [m["index"] for m in mappings] != ["0x1600" if direction == "rx" else "0x1A00"]:
            failures.append("非模块化 ESI 的 %s 映射表不是出厂那套" % direction)
            continue
        got = [entry["index"] for entry in mappings[0]["entries"]]
        if got != expected:
            failures.append("非模块化 ESI 的 %s 条目顺序变了：%s" % (direction, got))
        if pdo[direction]["bytes"] * 8 != sum(e["bits"] for e in mappings[0]["entries"]):
            failures.append("非模块化 ESI 的 %s 字节数与条目位宽不一致" % direction)


def _modular_policy():
    """一份能过的最小模块化策略：三套映射，槽位 1..3。"""
    return {
        "schema_version": 1,
        "profile_id": "check.modular.rev1",
        "reference_pdo_set_id": "00119800-csp",
        "safe_stop_status_mask": "0x0004",
        "safe_stop_status_value": "0x0000",
        "supported_modes": {"csp": 8, "csv": 9, "cst": 10},
        "pdo_sets": [
            {
                "id": "%s-%s" % (ident[2:], mode),
                "module_ident": ident,
                "module_slot": slot,
                "mode_value": value,
            }
            for slot, (ident, mode, value) in enumerate(
                [
                    ("0x00119800", "csp", 8),
                    ("0x00219800", "csv", 9),
                    ("0x00319800", "cst", 10),
                ],
                start=1,
            )
        ],
    }


def _direct_policy():
    """一份能过的最小直连策略。不带 conversion / slow_telemetry —— 厂商资料没给。"""
    return {
        "schema_version": 1,
        "profile_id": "check.direct.rev1",
        "reference_pdo_set_id": "factory_default",
        "safe_stop_status_mask": "0x0004",
        "safe_stop_status_value": "0x0000",
        "supported_modes": {"csp": 8},
        "pdo_sets": [
            {
                "id": "factory_default",
                "assignment_style": "direct_1c12",
                "mode_value": 8,
            }
        ],
    }


def _merge(scratch, facts, policy, name="profile.json"):
    facts_path = _write(os.path.join(scratch, "facts.json"), facts)
    policy_path = _write(os.path.join(scratch, "policy_%s" % name), policy)
    profile_path = os.path.join(scratch, name)
    code = _run(
        [
            "gen/device/merge_device_profile.py",
            "--facts", facts_path,
            "--policy", policy_path,
            "--output", profile_path,
        ]
    )
    return code, profile_path


def check_direct_merge(failures):
    """非模块化设备合成出的设备档：不写模块，PDO 硬件部分逐字取自事实。"""
    with tempfile.TemporaryDirectory() as scratch:
        facts = derive(_facts_of(DIRECT_ESI))
        code, path = _merge(scratch, facts, _direct_policy())
        if code != 0:
            failures.append("非模块化设备合成失败，merge 返回 %d（应 0）" % code)
            return
        with open(path, encoding="utf-8") as handle:
            profile = json.load(handle)
        merged = profile["pdo_sets"][0]
        if merged["assignment_style"] != "direct_1c12":
            failures.append("合成出的设备档丢了分配方式：%s" % merged["assignment_style"])
        if merged["module_ident"] is not None or merged["module_slot"] != 0:
            failures.append(
                "直连设备不应带模块标识或槽位：ident=%s slot=%s"
                % (merged["module_ident"], merged["module_slot"])
            )
        hardware = facts["pdo_sets"][0]
        if merged["rx"] != hardware["rx"] or merged["tx"] != hardware["tx"]:
            failures.append("合成出的 PDO 映射与推导出的事实不一致")
        if profile["protocol"]["supported_modes"] != {"csp": 8}:
            failures.append("合成出的模式集合不是策略声明的那份")
        # 同步事实要原样过桥：它是 init-site 生成运行方案时唯一能用的来源，
        # 丢在合成这一步，客户就会拿到一份没有 assign_activate 的运行方案。
        if profile["protocol"].get("assign_activate_by_strategy") != facts["protocol"][
            "assign_activate_by_strategy"
        ]:
            failures.append("合成出的设备档丢了 ESI 声明的 AssignActivate")
        # 策略没给的两项就不该出现在设备档里：缺省的含义是"厂商资料没给"，
        # 补一个默认值填上等于替厂商编了一条它没说过的事实。
        for key in ("conversion", "slow_telemetry"):
            if key in profile:
                failures.append("策略没给 %s，设备档里却出现了" % key)


def check_optional_facts_round_trip(failures):
    """策略给了就必须原样带进设备档 —— 上面那条只在缺省时成立。"""
    with tempfile.TemporaryDirectory() as scratch:
        facts = derive(_facts_of(DIRECT_ESI))
        policy = _direct_policy()
        policy["conversion"] = {"encoder_counts_per_motor_revolution_default": 65536}
        policy["slow_telemetry"] = ["0x6079", "0x200B"]
        code, path = _merge(scratch, facts, policy)
        if code != 0:
            failures.append("带可选字段的策略合成失败，merge 返回 %d（应 0）" % code)
            return
        with open(path, encoding="utf-8") as handle:
            profile = json.load(handle)
        if profile.get("conversion") != policy["conversion"]:
            failures.append("策略给的 conversion 没原样带进设备档")
        if profile.get("slow_telemetry") != policy["slow_telemetry"]:
            failures.append("策略给的 slow_telemetry 没原样带进设备档")


def check_broken_esi(failures):
    """推不出来的 ESI 必须非零退出，且不写出产物。"""
    with tempfile.TemporaryDirectory() as scratch:
        for label, text in BROKEN_ESI.items():
            esi = _write(os.path.join(scratch, "broken.xml"), text)
            out = os.path.join(scratch, "facts.json")
            code = _run(
                ["gen/device/derive_device_facts.py", esi, "--output", out]
            )
            if code == 0:
                failures.append("%s：derive 返回 0（应非零）" % label)
            elif os.path.exists(out):
                failures.append("%s：derive 失败却写出了产物" % label)


def check_supported_modes_conflict(failures):
    """策略声明的模式与 ESI 模块类型不一致时必须失败；一致时必须成功。"""
    with tempfile.TemporaryDirectory() as scratch:
        facts = derive(_facts_of(MODULAR_ESI))

        code, _ = _merge(scratch, facts, _modular_policy(), name="ok.json")
        if code != 0:
            failures.append("模式集合与 ESI 一致的策略，merge 却返回 %d（应 0）" % code)

        narrowed = _modular_policy()
        narrowed["supported_modes"] = {"csp": 8}
        code, _ = _merge(scratch, facts, narrowed, name="narrowed.json")
        if code == 0:
            failures.append("策略缩水了模式集合，merge 却返回 0（应非零）")

        missing = _modular_policy()
        missing.pop("supported_modes")
        code, _ = _merge(scratch, facts, missing, name="missing.json")
        if code == 0:
            failures.append("策略缺 supported_modes，merge 却返回 0（应非零）")

        # 策略引用 ESI 里没有的模块标识：应该报错，而不是拿别的模块顶上。
        alien = _modular_policy()
        alien["pdo_sets"][0]["module_ident"] = "0x00DEAD00"
        code, _ = _merge(scratch, facts, alien, name="alien.json")
        if code == 0:
            failures.append("策略引用了不存在的模块，merge 却返回 0（应非零）")


def main():
    failures = []
    check_modular(failures)
    check_direct(failures)
    check_direct_merge(failures)
    check_optional_facts_round_trip(failures)
    check_broken_esi(failures)
    check_supported_modes_conflict(failures)

    for failure in failures:
        print("不合格：%s" % failure)
    print("\n不合格 %d 项" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
