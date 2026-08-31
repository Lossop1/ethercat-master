#!/usr/bin/env python3
"""验证设备目录生成器对多型号输入的公开契约。"""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.generate_slave_catalog import generate


class CatalogGeneratorTests(unittest.TestCase):
    """使用真实设备配置形状验证集合生成，不依赖某个固定目录数量。"""

    @classmethod
    def setUpClass(cls) -> None:
        profile_path = (
            REPOSITORY_ROOT
            / "config/devices/cyberbeast_isvd90rc_300b_100_70.json"
        )
        cls.profile = json.loads(profile_path.read_text(encoding="utf-8"))

    def test_generates_all_profiles_in_stable_order(self) -> None:
        second = copy.deepcopy(self.profile)
        second["profile_id"] = "example.second.rev1"
        second["model"] = "EXAMPLE-SECOND"
        second["identity"]["product_code"] = "0x00000002"

        output = generate([second, self.profile])

        self.assertEqual(output.count(".profile_id ="), 2)
        self.assertLess(
            output.index("cyberbeast.isvd90rc-300b-100-70.rev1"),
            output.index("example.second.rev1"),
        )

    def test_rejects_empty_input(self) -> None:
        with self.assertRaisesRegex(ValueError, "至少需要一个"):
            generate([])

    def test_rejects_duplicate_profile_id(self) -> None:
        duplicate = copy.deepcopy(self.profile)
        with self.assertRaisesRegex(ValueError, "profile_id 必须唯一"):
            generate([self.profile, duplicate])


if __name__ == "__main__":
    unittest.main()
