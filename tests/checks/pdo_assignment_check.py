#!/usr/bin/env python3
"""生效 PDO 两种分配方式的生成/校验契约（离线，不接总线）。

用法:
    python3 tests/checks/pdo_assignment_check.py

为什么查这一条。设备档里多了一个 `assignment_style`，它决定运行期要不要往 0xF030
写模块标识。写错的后果不是报错，而是**对没有这个对象的设备发一条 0xF030**：SDO 中止，
整个 PDO 分配失败，主站进不了 OP。反过来，模块化设备漏了这一步会安静地沿用出厂分配 ——
看着能跑，跑的却是另一套映射。

所以这里钉住四件事：直连档生成出的扁平文件带对了符号、模块槽位真的落到 0；
模块化档不受影响；两种自相矛盾的写法都必须被拒（直连却声明模块标识、模块槽位为 0）。

退出码：0 = 全部符合；1 = 有不符合项；2 = 内部错误。
"""

import json
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools", "gen"))

import validate_device_config  # noqa: E402
from validation_common import Validation  # noqa: E402

DIRECT_SYMBOL = "EMASTER_PDO_ASSIGNMENT_DIRECT"
MODULE_SLOT_SYMBOL = "EMASTER_PDO_ASSIGNMENT_MODULE_SLOT"

# 样本档写在本文件里，不读仓库里任何设备档：交付时仓库不含电机配置。
# 只要一份"形状对"的档就够 —— 这份检查问的是分配方式字段的契约，
# 不是某款电机的映射对不对（那由 tests/checks/device_facts_check.py 钉）。
_ENTRIES_RX = [
    {"index": "0x6040", "subindex": 0, "bits": 16, "data_type": "UINT", "name": "control_word"},
    {"index": "0x607A", "subindex": 0, "bits": 32, "data_type": "INT32", "name": "target_position"},
]
_ENTRIES_TX = [
    {"index": "0x6041", "subindex": 0, "bits": 16, "data_type": "UINT", "name": "status_word"},
    {"index": "0x6064", "subindex": 0, "bits": 32, "data_type": "INT32", "name": "actual_position"},
]


def _template():
    """一份模块化设备档：PDO 挂在模块上，靠写模块槽位选。"""
    return {
        "schema_version": 2,
        "profile_id": "check.modular.rev1",
        "model": "check-modular",
        "identity": {
            "vendor_id": "0x00000001",
            "product_code": "0x00000011",
            "revision": "0x00000001",
        },
        "protocol": {
            "supports_pdo_assignment": True,
            "supports_pdo_configuration": True,
            "supports_distributed_clocks": True,
            "assign_activate_by_strategy": {"dc": "0x00000300"},
            "safe_stop_status_mask": "0x0004",
            "safe_stop_status_value": "0x0000",
        },
        "reference_pdo_set_id": "mod-csp",
        "pdo_sets": [
            {
                "id": "mod-csp",
                "module_ident": "0x00119800",
                "module_slot": 1,
                "mode_initialization": {
                    "transition": "safeop_to_op",
                    "index": "0x6060",
                    "subindex": 0,
                    "value": 8,
                },
                "rx": {"bytes": 6, "mappings": [{"index": "0x1600", "entries": _ENTRIES_RX}]},
                "tx": {"bytes": 6, "mappings": [{"index": "0x1A00", "entries": _ENTRIES_TX}]},
            }
        ],
    }


def _direct_profile():
    """同一份档改成直连形态：一套 PDO、无模块标识、无设备侧停机判据。"""
    profile = _template()
    pdo = profile["pdo_sets"][0]
    pdo["module_ident"] = None
    pdo["module_slot"] = 0
    pdo["assignment_style"] = "direct_1c12"
    profile["reference_pdo_set_id"] = pdo["id"]
    profile["protocol"]["safe_stop_status_mask"] = "0x0000"
    profile["protocol"]["safe_stop_status_value"] = "0x0000"
    return profile


def _generate(failures, label, profile):
    """跑一次设备目录生成器，返回（退出码, 扁平文件文本）。"""
    with tempfile.TemporaryDirectory() as scratch:
        devices = os.path.join(scratch, "devices")
        os.makedirs(devices)
        with open(os.path.join(devices, "device.json"), "w", encoding="utf-8") as handle:
            json.dump(profile, handle, ensure_ascii=False)
        flat = os.path.join(scratch, "catalog.flat")
        done = subprocess.run(
            [
                sys.executable, "tools/gen/generate_catalog_apply.py",
                "--devices", devices,
                "--c-output", os.path.join(scratch, "catalog.c"),
                "--h-output", os.path.join(scratch, "generated_catalog.h"),
                "--flat-output", flat,
            ],
            capture_output=True, text=True, cwd=REPO,
            encoding="utf-8", errors="replace",
        )
        text = ""
        if os.path.exists(flat):
            with open(flat, encoding="utf-8") as handle:
                text = handle.read()
        if done.returncode != 0 and label is not None:
            failures.append("%s：生成器返回 %d，期望 0" % (label, done.returncode))
        return done.returncode, text


def check_direct_generates(failures):
    code, text = _generate(failures, "直连档", _direct_profile())
    if code != 0:
        return
    for expected in (
        "profile.0.pdo_sets.0.assignment_style\t" + DIRECT_SYMBOL,
        "profile.0.pdo_sets.0.module_slot\t0",
        "profile.0.pdo_sets.0.module_ident\t0x00000000",
        "profile.0.safe_stop_status_mask\t0",
    ):
        if expected not in text:
            failures.append("直连档的扁平文件缺少 %r" % expected)


def check_modular_unchanged(failures):
    code, text = _generate(failures, "模块化档", _template())
    if code != 0:
        return
    if ("profile.0.pdo_sets.0.assignment_style\t" + MODULE_SLOT_SYMBOL) not in text:
        failures.append("模块化档没有被标成 module_slot：缺省值没兜住")
    if "safe_stop_status_mask\t4" not in text:
        failures.append("模块化档的停机掩码被改了")


def _validator_errors(profile):
    check = Validation(errors=[])
    validate_device_config.validate_profile(check, profile)
    return check.errors


def check_contradictions_rejected(failures):
    """自相矛盾的写法必须被配置链的某一环拒掉，不能一路走到运行期。

    不指定是哪一环：拒在生成器（形状不对，生不出 C）和拒在校验器（字段越界）都算数，
    这里问的是同一件事 —— 不许有一份坏配置从这条链上过去。
    """
    cases = {}
    direct_with_ident = _direct_profile()
    direct_with_ident["pdo_sets"][0]["module_ident"] = "0x00119800"
    cases["直连档声明了 module_ident"] = direct_with_ident

    direct_with_slot = _direct_profile()
    direct_with_slot["pdo_sets"][0]["module_slot"] = 1
    cases["直连档声明了非零 module_slot"] = direct_with_slot

    zero_slot_modular = _template()
    zero_slot_modular["pdo_sets"][0]["module_slot"] = 0
    cases["模块化档的 module_slot 为 0"] = zero_slot_modular

    mask_with_nonzero_value = _template()
    mask_with_nonzero_value["protocol"]["safe_stop_status_mask"] = "0x0000"
    mask_with_nonzero_value["protocol"]["safe_stop_status_value"] = "0x0004"
    cases["空掩码配非零期望值"] = mask_with_nonzero_value

    bad_style = _direct_profile()
    bad_style["pdo_sets"][0]["assignment_style"] = "module"
    cases["未知的 assignment_style"] = bad_style

    for label, profile in cases.items():
        if _generate(None, None, profile)[0] != 0:
            continue
        if not _validator_errors(profile):
            failures.append("%s：生成器与校验器都放过了它" % label)


def check_validator_accepts_direct(failures):
    """校验器与生成器对直连档的判断必须一致，否则配置链两边打架。"""
    for error in _validator_errors(_direct_profile()):
        failures.append("校验器拒绝了合法的直连档：%s" % error)


def main():
    failures = []
    check_direct_generates(failures)
    check_modular_unchanged(failures)
    check_contradictions_rejected(failures)
    check_validator_accepts_direct(failures)

    for failure in failures:
        print("不合格：%s" % failure)
    print("\n不合格 %d 项" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
