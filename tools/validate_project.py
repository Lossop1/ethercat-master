#!/usr/bin/env python3
"""Validate the phase-one catalog, ESI, topology, and vendor manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class Validation:
    errors: list[str]

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.errors.append(message)


def hex_value(value: str) -> int:
    return int(value.replace("#x", "0x"), 16)


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def validate_pdo_set(check: Validation, pdo_set: dict[str, Any]) -> None:
    for direction in ("rx", "tx"):
        pdo = pdo_set[direction]
        bit_sum = sum(int(entry["bits"]) for entry in pdo["entries"])
        check.require(bit_sum % 8 == 0, f"{pdo_set['id']} {direction} is not byte aligned")
        check.require(
            bit_sum // 8 == int(pdo["bytes"]),
            f"{pdo_set['id']} {direction} byte count does not match entries",
        )


def esi_modules(root: ET.Element) -> dict[int, dict[str, Any]]:
    modules: dict[int, dict[str, Any]] = {}
    for module in root.findall("./Descriptions/Modules/Module"):
        module_type = module.find("Type")
        if module_type is None:
            continue
        module_ident = hex_value(module_type.attrib["ModuleIdent"])
        result: dict[str, Any] = {}
        for xml_tag, direction in (("RxPdo", "rx"), ("TxPdo", "tx")):
            pdo = module.find(xml_tag)
            if pdo is None:
                continue
            entries = []
            for entry in pdo.findall("Entry"):
                index = entry.findtext("Index", default="0")
                entries.append(
                    {
                        "index": hex_value(index),
                        "subindex": int(entry.findtext("SubIndex", default="0")),
                        "bits": int(entry.findtext("BitLen", default="0")),
                    }
                )
            result[direction] = {
                "index": hex_value(pdo.findtext("Index", default="0")),
                "entries": entries,
            }
        modules[module_ident] = result
    return modules


def validate_catalog_against_esi(
    check: Validation,
    root_dir: Path,
    profile: dict[str, Any],
    require_vendor_artifacts: bool,
) -> None:
    esi_path = root_dir / profile["source"]["esi"]
    if not esi_path.is_file():
        check.require(
            not require_vendor_artifacts,
            f"required controlled ESI does not exist: {esi_path}",
        )
        return

    actual_hash = hashlib.sha256(esi_path.read_bytes()).hexdigest()
    check.require(
        actual_hash == profile["source"]["esi_sha256"].lower(),
        "ESI SHA-256 does not match the slave profile",
    )

    root = ET.parse(esi_path).getroot()
    vendor = root.find("./Vendor")
    device_type = root.find("./Descriptions/Devices/Device/Type")
    check.require(vendor is not None, "ESI Vendor section is missing")
    check.require(device_type is not None, "ESI Device Type is missing")
    if vendor is None or device_type is None:
        return

    identity = profile["identity"]
    check.require(hex_value(vendor.findtext("Id", default="0")) == hex_value(identity["vendor_id"]),
                  "ESI Vendor ID does not match catalog")
    check.require(device_type.text == profile["model"], "ESI model does not match catalog")
    check.require(hex_value(device_type.attrib["ProductCode"]) == hex_value(identity["product_code"]),
                  "ESI Product Code does not match catalog")
    check.require(hex_value(device_type.attrib["RevisionNo"]) == hex_value(identity["revision"]),
                  "ESI Revision does not match catalog")

    modules = esi_modules(root)
    for pdo_set in profile["pdo_sets"]:
        validate_pdo_set(check, pdo_set)
        module_ident = hex_value(pdo_set["module_ident"])
        check.require(module_ident in modules, f"ESI module 0x{module_ident:08X} is missing")
        if module_ident not in modules:
            continue
        for direction in ("rx", "tx"):
            expected = pdo_set[direction]
            actual = modules[module_ident][direction]
            expected_entries = [
                {
                    "index": hex_value(entry["index"]),
                    "subindex": int(entry["subindex"]),
                    "bits": int(entry["bits"]),
                }
                for entry in expected["entries"]
            ]
            check.require(
                hex_value(expected["index"]) == actual["index"],
                f"{pdo_set['id']} {direction} PDO index differs from ESI",
            )
            check.require(
                expected_entries == actual["entries"],
                f"{pdo_set['id']} {direction} PDO entries differ from ESI",
            )


def validate_topology(
    check: Validation, topology: dict[str, Any], profiles: dict[str, dict[str, Any]]
) -> None:
    slaves = topology["slaves"]
    positions = [int(slave["position"]) for slave in slaves]
    axis_ids = [str(slave["axis_id"]) for slave in slaves]
    check.require(len(slaves) == 12, "phase-one robot topology must contain 12 positions")
    check.require(positions == list(range(1, 13)), "slave positions must be contiguous from 1 to 12")
    check.require(len(axis_ids) == len(set(axis_ids)), "axis IDs must be unique")
    for slave in slaves:
        check.require(
            slave["profile_id"] in profiles,
            f"unknown profile for position {slave['position']}: {slave['profile_id']}",
        )
    check.require(
        topology["safety"]["hardware_enable_allowed"] is False,
        "phase one must not allow hardware enable",
    )
    check.require(
        topology["safety"]["maximum_state_phase_1"] == "PRE-OP",
        "phase-one topology must cap the state at PRE-OP",
    )


def validate_manifest(
    check: Validation,
    root_dir: Path,
    manifest_path: Path,
    require_vendor_artifacts: bool,
) -> tuple[int, int]:
    declared = 0
    installed = 0
    for line_number, raw_line in enumerate(
        manifest_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            expected_hash, relative_path = line.split("  ", 1)
        except ValueError:
            check.errors.append(f"invalid manifest line {line_number}")
            continue
        declared += 1
        path = root_dir / relative_path
        check.require(
            path.is_file() or not require_vendor_artifacts,
            f"required controlled artifact is missing: {relative_path}",
        )
        if path.is_file():
            installed += 1
            actual_hash = hashlib.sha256(path.read_bytes()).hexdigest()
            check.require(actual_hash == expected_hash, f"SHA-256 mismatch: {relative_path}")
    return declared, installed


def run(root_dir: Path, require_vendor_artifacts: bool) -> tuple[list[str], int, int]:
    check = Validation(errors=[])
    profile_path = root_dir / "config/slaves/cyberbeast_isvd90rc_300b_100_70.json"
    topology_path = root_dir / "config/topology/robot_12_axis.json"
    manifest_path = root_dir / "docs/vendor/SHA256SUMS"

    profile = load_json(profile_path)
    topology = load_json(topology_path)
    profiles = {profile["profile_id"]: profile}
    check.require(profile.get("schema_version") == 1, "unsupported slave profile schema")
    check.require(topology.get("schema_version") == 1, "unsupported topology schema")
    validate_catalog_against_esi(check, root_dir, profile, require_vendor_artifacts)
    validate_topology(check, topology, profiles)
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
        help="fail if any controlled vendor input is not installed",
    )
    args = parser.parse_args()
    errors, declared, installed = run(
        args.root.resolve(), args.require_vendor_artifacts
    )
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    if installed == declared:
        print("Project catalog, ESI, topology, and controlled vendor inputs are consistent.")
    else:
        print(
            "Project metadata is consistent; controlled vendor inputs are not installed "
            f"({installed}/{declared} present)."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
