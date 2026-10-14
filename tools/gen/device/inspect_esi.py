#!/usr/bin/env python3
"""以 JSON 输出单个 ESI 的事实报告，不修改项目文件。"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from esi_facts import load_esi_facts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    args = parser.parse_args()
    # 读不了、不是 XML、是 XML 但不是 ESI，三种情形 load_esi_facts 都换成了一句说得
    # 清的话再退出，所以这里不再接异常——接了也只是把同一句话再打印一遍。
    report = load_esi_facts(args.input)
    json.dump(report, sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
