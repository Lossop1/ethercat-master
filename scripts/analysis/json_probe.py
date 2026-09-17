#!/usr/bin/env python3
"""一次性脚本：从超大单行 JSON 里按字段名取一小段窗口。

报告能到 300 MB 且是一整行，grep 的 -o/-m1 在这种文件上等于把整行读进内存再匹配，
在台架那台机器上会卡住十几分钟。这个脚本流式读、命中即停，只要几百毫秒。

用法：python3 scripts/analysis/json_probe.py <文件> <字段名>[:窗口字节] [字段名[:窗口字节] ...]
     字段名写成 all:<字段名>:<窗口字节> 时打印每一次出现（默认只打第一次）。

报告是 331 MB 的**一整行** JSON，grep 的 -o/-m1 会把整行读进内存再匹配，在台架上
要卡十几分钟；这个脚本流式读、命中位置即可停，取一两个字段是秒级。
"""
import sys

path = sys.argv[1]
specs = []
for argument in sys.argv[2:]:
    every = argument.startswith("all:")
    if every:
        argument = argument[len("all:"):]
    name, _, size = argument.partition(":")
    specs.append((('"' + name + '":').encode(), int(size) if size else 700, every))

chunk_size = 1 << 20
overlap = max(len(needle) for needle, _, _ in specs)
with open(path, "rb") as handle:
    offset = 0
    tail = b""
    pending = list(specs)
    while pending:
        chunk = handle.read(chunk_size)
        if not chunk:
            break
        haystack = tail + chunk
        base = offset - len(tail)
        for needle, window, every in list(pending):
            start = 0
            while True:
                found = haystack.find(needle, start)
                if found < 0:
                    break
                print(f"[{needle.decode()} offset {base + found}]")
                sys.stdout.write(haystack[found:found + window].decode("utf-8", "replace"))
                print("\n")
                if not every:
                    pending.remove((needle, window, every))
                    break
                start = found + 1
        tail = haystack[-overlap:]
        offset += len(chunk)
