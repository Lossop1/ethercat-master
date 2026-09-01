#!/usr/bin/env python3
"""以 JSON 输出单个 ESI 的事实报告，不修改项目文件。"""

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from esi_facts import load_esi_facts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    args = parser.parse_args()
    try:
        report = load_esi_facts(args.input)
    except (OSError, ET.ParseError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    json.dump(report, sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
