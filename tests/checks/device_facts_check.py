#!/usr/bin/env python3
"""设备档推导链的检查：ESI → 硬件事实 → 设备档（离线，不接总线）。

用法:
    python3 tests/checks/device_facts_check.py

为什么查这一条。这条链是"客户换自己的电机、不重编主站"的唯一入口，而它坏掉的样子
是不出声的：映射条目顺序被重排、位偏移整体错位，设备照样进 OP、反馈值照样有数，
只是数对不上。所以这里钉住五件事 —— 推导结果逐项固定、合成出的设备档字段位置正确、
**换算声明三项不全时必须被拒**、**模式集合缺声明时必须被拒且要走到运行期**、
**推不出来的时候必须非零退出**。猜着往下走的代价是拿错的映射跑真轴；换算给半份的
代价更隐蔽：折算出来的计数看着像真的；模式集合缺声明则是每一种模式都被安静地放行。

样本从哪来。这份检查自带的 ESI 是本文件里的字符串，不读仓库里任何 ESI：项目交付
时不含任何电机配置（连 ESI 都没有，客户把自己那份放进 esi/）。用合成样本的好处是
每条断言背后的输入就写在旁边，改了推导规则立刻看得出哪条断言该跟着改。
"""
import copy
import json
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools", "gen", "device"))
sys.path.insert(0, os.path.join(REPO, "tools", "gen"))

from derive_device_facts import derive  # noqa: E402
from esi_facts import extract_esi_facts, select_device  # noqa: E402
import validate_device_config  # noqa: E402
from validation_common import Validation  # noqa: E402


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

def _direct_device(product_code: str, revision: str, model: str) -> str:
    """一台能推导成功的非模块化设备：没有 <Module>，PDO 直挂 <Device>，靠对象字典的
    出厂默认分配（0x1C12 = 0x1600、0x1C13 = 0x1A00）与 PDO 上的 Sm 属性互相印证出
    唯一一套。抽成函数是为了让"一台"和"多台"两份样本用同一个设备定义 —— 多设备那份
    要是自己另抄一遍，两边就会各自漂。
    """
    return """  <Device>
    <Type ProductCode="#x{product}" RevisionNo="#x{revision}">{model}</Type>
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
""".format(
        product=product_code, revision=revision, model=model,
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


def _esi(devices: list[str]) -> str:
    return """<EtherCATInfo Version="1.0">
<Vendor><Id>#x00000001</Id><Name>检查用厂商</Name></Vendor>
<Descriptions>
<Devices>
%s</Devices>
</Descriptions>
</EtherCATInfo>""" % "".join(devices)


DIRECT_ESI = _esi([_direct_device("00000022", "00000001", "check-direct")])

# 一份 ESI 描述同一厂商的整个产品线是常态（厂商给的往往就是这一份）。顶层键只在设备数
# 为 1 时铺平，多设备时下游读顶层会撞 KeyError，所以这里钉住"要么挑、要么说清楚"。
MULTI_DEVICE_ESI = _esi([
    _direct_device("00000022", "00000001", "check-direct"),
    _direct_device("00000023", "00000002", "check-direct-large"),
])
MULTI_DEVICE_IDENTITIES = [("0x00000022", "0x00000001"), ("0x00000023", "0x00000002")]

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


def _run_capture(argv):
    done = subprocess.run(
        [sys.executable] + argv, capture_output=True, text=True, cwd=REPO,
        encoding="utf-8", errors="replace",
    )
    return done.returncode, done.stdout, done.stderr


def _run(argv):
    return _run_capture(argv)[0]


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
            "tools/gen/device/merge_device_profile.py",
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
        # 三项齐全：校验器只收完整的声明（见 check_conversion_declaration），
        # 样本自己也别拿半份当例子，否则读者会以为半份是能用的。
        policy["conversion"] = {
            "encoder_counts_per_motor_revolution_default": 65536,
            "gear_motor_revolutions_default": 10,
            "gear_shaft_revolutions_default": 1,
        }
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


def _conversion_errors(conversion):
    """把一份只带 conversion 的残缺档喂给配置校验器，只回换算相关的错误。

    档的其余部分是故意缺的 —— 这里问的是换算那一条，别的错误是样本本身的形状，
    所以按关键词过滤，不看总错误数。
    """
    # 设备名里不许出现 conversion：每条错误都带 "设备 <id>"，名字带这个词的话，
    # 其余字段缺失的错也会被下面的过滤捞进来。
    profile = {"schema_version": 2, "profile_id": "check.facts", "model": "check"}
    if conversion is not None:
        profile["conversion"] = conversion
    check = Validation(errors=[])
    validate_device_config.validate_profile(check, profile)
    return [error for error in check.errors if "conversion" in error or "polarity" in error]


def check_conversion_declaration(failures):
    """设备档的换算声明：三项一起给才算声明过；给半份、给零、给错方向都必须拒。

    运行期把这份声明当成一整份换算用（`emaster_position_scale_from_declaration`：
    三项齐全才认），缺任一项来源就留成"没有"，会话判运动无效、轴一步不动。所以这里
    问的是同一个问题：配置期能不能放一份运行期必然用不了的声明过去。能过去，失败就
    推到了运行期 —— 那时看到的是一句跟配置无关的错。

    这一条自己就带判别力：期望非零的那几例在一个"什么都收"的校验器上会一起红。
    """
    full = {
        "encoder_counts_per_motor_revolution_default": 131072,
        "gear_motor_revolutions_default": 19612,
        "gear_shaft_revolutions_default": 1000,
    }
    cases = {
        "三项齐全": (dict(full), 0),
        "只有编码器，减速比两段都没给": (
            {"encoder_counts_per_motor_revolution_default": 131072}, 1),
        "半份减速比：只有电机侧": (
            {"encoder_counts_per_motor_revolution_default": 131072,
             "gear_motor_revolutions_default": 19612}, 1),
        "半份减速比：只有负载侧": (
            {"encoder_counts_per_motor_revolution_default": 131072,
             "gear_shaft_revolutions_default": 1000}, 1),
        "编码器计数为零": (
            {"encoder_counts_per_motor_revolution_default": 0,
             "gear_motor_revolutions_default": 19612,
             "gear_shaft_revolutions_default": 1000}, 1),
        "减速比分母为零": (
            {"encoder_counts_per_motor_revolution_default": 131072,
             "gear_motor_revolutions_default": 19612,
             "gear_shaft_revolutions_default": 0}, 1),
        "方向值越界": (dict(full, polarity_default=2), 1),
    }
    for label, (conversion, expected) in cases.items():
        got = len(_conversion_errors(conversion))
        if got != expected:
            failures.append(
                "换算声明「%s」：校验器给了 %d 条换算错误，应 %d 条" % (label, got, expected)
            )
    # 整块不写是合法的：厂商资料没给换算，是句实话，不是"每转 0 个计数"。
    if _conversion_errors(None):
        failures.append("不写 conversion 却被判成错：缺省是合法的一句实话")


def _generate_catalog(scratch, profile):
    """把一份设备档喂给目录生成器，返回（退出码, 扁平文件文本）。

    每次先清掉上一次的产物：这条检查问的是"这一次生成了什么"，上一轮的残留会让
    "生成失败却留下旧产物"看起来像成功。
    """
    devices = os.path.join(scratch, "devices")
    if not os.path.isdir(devices):
        os.makedirs(devices)
    for name in os.listdir(devices):
        os.remove(os.path.join(devices, name))
    _write(os.path.join(devices, "device.json"), profile)
    flat = os.path.join(scratch, "catalog.flat")
    if os.path.exists(flat):
        os.remove(flat)
    code = _run(
        [
            "tools/gen/generate_catalog_apply.py",
            "--devices", devices,
            "--c-output", os.path.join(scratch, "catalog.c"),
            "--h-output", os.path.join(scratch, "generated_catalog.h"),
            "--flat-output", flat,
        ]
    )
    text = ""
    if os.path.exists(flat):
        with open(flat, encoding="utf-8") as handle:
            text = handle.read()
    return code, text


def check_mode_set_reaches_runtime(failures):
    """档里声明的模式集合必须一路走到运行期，缺声明必须硬失败。

    这个事实推不出来：ESI 的模块类型只有模块化设备才写得出，非模块化设备一个字都
    没有；而同一个取值在两家的说明书里指的不是同一件事（台架那台 MT 的通道 0x1601
    是 PVT 控制通道，良质的 0x1601 是精简 CSP）。所以运行期不按型号分支、也不猜
    通道号，只认档里这一份声明 —— 而它得先真的到得了运行期。

    在这里钉两头：
    ① 合成出的设备档经目录生成器后，扁平文件里带着模式集合，且升序去重 ——
       运行期才能拿它跟运行方案要跑的模式对账；少了这一项，运行期就只能放行一切。
    ② 缺声明、空集合、取值越界都必须被拒，且校验器要报出这一条。缺省成"什么都
       支持"会把设备带进它不认的模式；缺省成"什么都不支持"会让每一份配置都跑不
       起来。两种都不是实话，所以不给缺省。
    """
    with tempfile.TemporaryDirectory() as scratch:
        facts = derive(_facts_of(MODULAR_ESI))
        code, path = _merge(scratch, facts, _modular_policy(), name="modes.json")
        if code != 0:
            failures.append("模式集合的样本档合成失败，merge 返回 %d（应 0）" % code)
            return
        with open(path, encoding="utf-8") as handle:
            profile = json.load(handle)

        check = Validation(errors=[])
        validate_device_config.validate_profile(check, profile)
        if check.errors:
            failures.append("合成出的设备档没过校验器：%s" % check.errors[0])

        code, text = _generate_catalog(scratch, profile)
        if code != 0:
            failures.append("带模式集合的设备档生成目录失败（%d）" % code)
        else:
            if "profile.0.supported_modes.count\t3" not in text:
                failures.append("扁平文件没带出模式集合的条数")
            for ordinal, value in enumerate((8, 9, 10)):
                if "profile.0.supported_modes.%d\t%d" % (ordinal, value) not in text:
                    failures.append("扁平文件里的第 %d 项不是模式 %d" % (ordinal, value))

        def set_modes(broken, modes):
            broken["protocol"]["supported_modes"] = modes

        cases = {}
        missing = copy.deepcopy(profile)
        missing["protocol"].pop("supported_modes")
        cases["档里没有 supported_modes"] = missing

        empty = copy.deepcopy(profile)
        set_modes(empty, {})
        cases["supported_modes 是空对象"] = empty

        too_big = copy.deepcopy(profile)
        set_modes(too_big, {"csp": 200})
        cases["模式取值超出 int8"] = too_big

        for label, broken in cases.items():
            if _generate_catalog(scratch, broken)[0] == 0:
                failures.append("%s：目录生成器返回 0（应非零）" % label)
            check = Validation(errors=[])
            validate_device_config.validate_profile(check, broken)
            if not [error for error in check.errors if "supported_modes" in error or "模式" in error]:
                failures.append("%s：校验器没报这一条" % label)


def check_broken_esi(failures):
    """推不出来的 ESI 必须非零退出，且不写出产物。"""
    with tempfile.TemporaryDirectory() as scratch:
        for label, text in BROKEN_ESI.items():
            esi = _write(os.path.join(scratch, "broken.xml"), text)
            out = os.path.join(scratch, "facts.json")
            code = _run(
                ["tools/gen/device/derive_device_facts.py", esi, "--output", out]
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


def _derive_cli(esi_path, *extra):
    """跑一次 derive_device_facts.py，回（退出码, stdout, stderr）。"""
    return _run_capture(["tools/gen/device/derive_device_facts.py", esi_path, *extra])


def check_multi_device(failures):
    """一份 ESI 里有多台设备：不挑就列清单退出，挑了就得挑准。"""
    with tempfile.TemporaryDirectory() as scratch:
        single = _write(os.path.join(scratch, "single.xml"), DIRECT_ESI)
        multi = _write(os.path.join(scratch, "multi.xml"), MULTI_DEVICE_ESI)

        # 单设备这条路是客户日常走的那条：挑设备这一步必须透明，不能要求给 --select。
        code, out, err = _derive_cli(single)
        if code != 0:
            failures.append("单设备 ESI 直接推导失败（%d）：%s" % (code, err.strip()[:200]))
        else:
            identity = json.loads(out)["identity"]
            if identity["product_code"] != "0x00000022":
                failures.append("单设备 ESI 推导出的产品码变了：%s" % identity["product_code"])

        # 不挑：非零退出，且清单里两台都要看得见 —— 只报个 KeyError 等于让人自己去猜。
        code, out, err = _derive_cli(multi)
        if code == 0:
            failures.append("多设备 ESI 没挑设备却返回 0（应非零）")
        for product in ("0x00000022", "0x00000023"):
            if product not in err:
                failures.append("多设备 ESI 的清单里没列出 %s：%s" % (product, err.strip()[:200]))

        # 按序号挑第二台。
        code, out, err = _derive_cli(multi, "--select", "2")
        if code != 0:
            failures.append("--select 2 失败（%d）：%s" % (code, err.strip()[:200]))
        else:
            identity = json.loads(out)["identity"]
            if identity["product_code"] != "0x00000023":
                failures.append("--select 2 挑错了设备：%s" % identity["product_code"])
            if identity["revision"] != "0x00000002":
                failures.append("--select 2 的版本号不对：%s" % identity["revision"])
            # 挑完必须仍是"一台"的形状：下游一律读顶层，这里掉了顶层键就白挑。
            picked = select_device(_facts_of(MULTI_DEVICE_ESI), "2")
            if picked.get("device", {}).get("product_code") != "0x00000023" or "modules" not in picked:
                failures.append("select_device 挑完没给出与单设备 ESI 相同的形状：%s" % sorted(picked))

        # 按身份挑（厂商:产品:版本）、按型号挑：两种写法都要认。
        for selector, expected in (
            ("0x00000001:0x00000023:0x00000002", "0x00000023"),
            ("check-direct", "0x00000022"),
        ):
            code, out, err = _derive_cli(multi, "--select", selector)
            if code != 0:
                failures.append("--select %s 失败（%d）：%s" % (selector, code, err.strip()[:200]))
            elif json.loads(out)["identity"]["product_code"] != expected:
                failures.append("--select %s 挑错了设备" % selector)

        # 挑不中一律非零：越界、对不上的身份，都不能"随便给一台"。
        for selector in ("9", "0x00000001:0x00000099:0x00000001", "不存在的型号"):
            code, out, err = _derive_cli(multi, "--select", selector)
            if code == 0:
                failures.append("--select %s 挑不中却返回 0（应非零）" % selector)


def main():
    failures = []
    check_modular(failures)
    check_direct(failures)
    check_direct_merge(failures)
    check_optional_facts_round_trip(failures)
    check_conversion_declaration(failures)
    check_broken_esi(failures)
    check_supported_modes_conflict(failures)
    check_mode_set_reaches_runtime(failures)
    check_multi_device(failures)

    for failure in failures:
        print("不合格：%s" % failure)
    print("\n不合格 %d 项" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
