#!/usr/bin/env python3
"""定位报告体积：按 JSON 路径列出各字段的序列化字节数，只打印大的。"""
import json
import sys


def size(obj):
    return len(json.dumps(obj))


def walk(obj, prefix, limit):
    if isinstance(obj, dict):
        for key, value in obj.items():
            walk(value, f"{prefix}.{key}", limit)
        return
    if isinstance(obj, list):
        total = size(obj)
        if total > limit:
            print(f"{prefix}  [list len={len(obj)}] bytes={total}")
            if obj:
                walk(obj[0], f"{prefix}[0]", limit)
        return
    total = size(obj)
    if total > limit:
        print(f"{prefix}  bytes={total} head={str(obj)[:100]!r}")


def main():
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
    with open(path) as handle:
        data = json.load(handle)
    print(f"顶层键：{list(data.keys())}")
    walk(data, "", limit)


if __name__ == "__main__":
    main()
