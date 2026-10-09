#!/usr/bin/env python3
"""把配置 JSON 拍平成「每行一条 key<TAB>value」的扁平文本（原型）。

为什么拍平
    见 include/emaster/config/config_reader.h 的说明：C 侧不引 JSON 解析器，
    由生成器把结构拍平。拍平之后，C 侧只需要一个按 key 取值的读取器，
    而字段名与结构体字段的对应写在生成的填充代码里 —— 由编译器检查。

约定
    · 对象用 '.' 连接，数组用下标：deployment.0.hostname
    · 长度另给一条 <路径>.count，便于 C 侧先分配再逐项填
    · 字符串直接写原值。当前配置里不含制表符与换行，所以不做转义；
      真要支持，就在这里加转义、在读取器里加反转义。
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def flatten(node: Any, prefix: str, out: list[tuple[str, str]]) -> None:
    if isinstance(node, dict):
        for key, value in node.items():
            flatten(value, f"{prefix}.{key}" if prefix else str(key), out)
        return
    if isinstance(node, list):
        out.append((f"{prefix}.count", str(len(node))))
        for index, value in enumerate(node):
            flatten(value, f"{prefix}.{index}", out)
        return
    if node is None:
        return  # 缺失就是缺失，不写一条 NULL 出来
    if isinstance(node, bool):
        out.append((prefix, "true" if node else "false"))
        return
    out.append((prefix, str(node)))


def main() -> int:
    parser = argparse.ArgumentParser(description="配置 JSON -> 扁平文本")
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    rows: list[tuple[str, str]] = []
    flatten(json.loads(args.input.read_text(encoding="utf-8")), "", rows)
    for key, value in rows:
        if "\t" in value or "\n" in value:
            raise SystemExit(f"值里含制表符或换行，当前格式不支持：{key}")
    args.output.write_text(
        "".join(f"{key}\t{value}\n" for key, value in rows),
        encoding="utf-8",
        newline="\n",
    )
    print(f"已写出 {args.output}（{len(rows)} 条）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
