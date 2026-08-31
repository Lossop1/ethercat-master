#!/usr/bin/env python3
"""使用不依赖硬件的合成指纹报告验证 JSON 输出。"""

from __future__ import annotations

import argparse
import json
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    args = parser.parse_args()

    completed = subprocess.run(
        [args.executable], check=True, capture_output=True, text=True
    )
    document = json.loads(completed.stdout)
    assert document["schema_version"] == 1
    assert document["interface"] == "test0"
    assert document["slave_count"] == 1
    assert document["slaves"][0]["target_profile_match"] is True
    reads = document["slaves"][0]["sdo_reads"]
    assert reads[0]["value"] == 'drive "A"\n'
    assert reads[1]["value"] == 4294967295
    assert reads[2]["ok"] is False and "value" not in reads[2]
    assert document["behavior"]["highest_requested_state"] == "PRE-OP"
    assert document["behavior"]["restore_init_succeeded"] is True
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
