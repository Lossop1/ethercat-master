#!/usr/bin/env python3
"""验证设备、拓扑和部署分层配置的成功与失效关闭行为。"""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.validate_project import run


class ProjectFixture:
    """创建最小完整工程输入，单个测试只修改与其断言有关的数据。"""

    def __init__(self, slave_count: int = 1) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        for relative in (
            "config/devices",
            "config/topologies",
            "config/deployments",
            "docs/vendor",
        ):
            (self.root / relative).mkdir(parents=True)

        source_profile = (
            REPOSITORY_ROOT
            / "config/devices/cyberbeast_isvd90rc_300b_100_70.json"
        )
        self.profile = json.loads(source_profile.read_text(encoding="utf-8"))
        self.write_json("config/devices/device.json", self.profile)
        (self.root / "docs/vendor/SHA256SUMS").write_text(
            "# 测试夹具不安装受控资料\n", encoding="utf-8"
        )

        self.topology = self.make_topology(slave_count)
        self.deployment = {
            "schema_version": 1,
            "deployment_id": "test-deployment",
            "hostname": "test-host",
            "topology_id": self.topology["topology_id"],
            "ethercat_interface": "test-ethercat0",
            "management_interface": "test-management0",
        }
        self.flush()

    def close(self) -> None:
        self.temporary.cleanup()

    def write_json(self, relative: str, document: dict[str, Any]) -> None:
        """统一使用 UTF-8 和稳定缩进写入测试配置，便于失败时人工检查。"""
        (self.root / relative).write_text(
            json.dumps(document, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    def make_topology(self, slave_count: int) -> dict[str, Any]:
        return {
            "schema_version": 1,
            "topology_id": "test-topology",
            "status": "test",
            "slaves": [
                {
                    "position": index,
                    "axis_id": f"axis_{index}",
                    "joint_assignment": None,
                    "profile_id": self.profile["profile_id"],
                }
                for index in range(1, slave_count + 1)
            ],
        }

    def flush(self) -> None:
        self.write_json("config/topologies/topology.json", self.topology)
        self.write_json("config/deployments/deployment.json", self.deployment)

    def errors(self) -> list[str]:
        self.flush()
        errors, _, _ = run(self.root, require_vendor_artifacts=False)
        return errors


class ProjectValidationTests(unittest.TestCase):
    """重点证明通用校验器不感知产品轴数、台架数量或具体网卡名。"""

    def fixture(self, slave_count: int = 1) -> ProjectFixture:
        fixture = ProjectFixture(slave_count)
        self.addCleanup(fixture.close)
        return fixture

    def test_accepts_different_non_empty_topology_sizes(self) -> None:
        for slave_count in (1, 2, 5, 12, 13):
            with self.subTest(slave_count=slave_count):
                fixture = self.fixture(slave_count)
                self.assertEqual(fixture.errors(), [])

    def test_discovers_every_device_configuration(self) -> None:
        fixture = self.fixture()
        second = copy.deepcopy(fixture.profile)
        second["profile_id"] = "example.second.rev1"
        second["identity"]["product_code"] = "0x00000002"
        fixture.write_json("config/devices/second.json", second)
        fixture.topology["slaves"][0]["profile_id"] = second["profile_id"]
        self.assertEqual(fixture.errors(), [])

    def test_rejects_empty_topology(self) -> None:
        fixture = self.fixture(0)
        self.assertTrue(any("不能为空" in error for error in fixture.errors()))

    def test_rejects_duplicate_position_and_axis_id(self) -> None:
        fixture = self.fixture(2)
        fixture.topology["slaves"][1]["position"] = 1
        fixture.topology["slaves"][1]["axis_id"] = "axis_1"
        errors = fixture.errors()
        self.assertTrue(any("从站位置必须唯一" in error for error in errors))
        self.assertTrue(any("轴 ID 必须唯一" in error for error in errors))

    def test_rejects_non_contiguous_positions(self) -> None:
        fixture = self.fixture(2)
        fixture.topology["slaves"][1]["position"] = 3
        self.assertTrue(any("必须从 1 连续排列到实际长度" in error for error in fixture.errors()))

    def test_rejects_unknown_device_profile(self) -> None:
        fixture = self.fixture()
        fixture.topology["slaves"][0]["profile_id"] = "unknown.profile"
        self.assertTrue(any("未知设备配置" in error for error in fixture.errors()))

    def test_reports_malformed_topology_entry_without_crashing(self) -> None:
        fixture = self.fixture()
        fixture.topology["slaves"][0] = {"position": "first"}
        errors = fixture.errors()
        self.assertTrue(any("位置必须是正整数" in error for error in errors))
        self.assertTrue(any("轴 ID 不能为空" in error for error in errors))

    def test_reports_malformed_device_without_crashing(self) -> None:
        fixture = self.fixture()
        del fixture.profile["identity"]
        fixture.write_json("config/devices/device.json", fixture.profile)
        self.assertTrue(any("缺少 identity" in error for error in fixture.errors()))

    def test_rejects_deployment_with_unknown_topology(self) -> None:
        fixture = self.fixture()
        fixture.deployment["topology_id"] = "unknown-topology"
        self.assertTrue(any("引用了未知拓扑" in error for error in fixture.errors()))

    def test_rejects_duplicate_interface_occupancy(self) -> None:
        fixture = self.fixture()
        second = copy.deepcopy(fixture.deployment)
        second["deployment_id"] = "second-deployment"
        fixture.write_json("config/deployments/second.json", second)
        self.assertTrue(any("重复占用" in error for error in fixture.errors()))

    def test_rejects_management_interface_reuse(self) -> None:
        fixture = self.fixture()
        fixture.deployment["management_interface"] = fixture.deployment[
            "ethercat_interface"
        ]
        self.assertTrue(any("不能相同" in error for error in fixture.errors()))

    def test_rejects_non_string_deployment_fields(self) -> None:
        fixture = self.fixture()
        fixture.deployment["hostname"] = 123
        fixture.deployment["ethercat_interface"] = False
        errors = fixture.errors()
        self.assertTrue(any("缺少 hostname" in error for error in errors))
        self.assertTrue(any("缺少 ethercat_interface" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
