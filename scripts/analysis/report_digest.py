#!/usr/bin/env python3
"""报告摘要：打印除 accesses 以外的全部内容，accesses 只做聚合。"""
import json
import sys
from collections import Counter


def main():
    path = sys.argv[1]
    with open(path) as handle:
        data = json.load(handle)

    accesses = data.pop("accesses", [])
    print(json.dumps(data, ensure_ascii=False, indent=1))

    print("\n=== accesses 聚合 ===")
    print(f"条数={len(accesses)}")
    if not accesses:
        return
    print("phase 分布:", Counter(a.get("phase") for a in accesses).most_common())
    print("transport 分布:", Counter(a.get("transport") for a in accesses).most_common())
    print("顺序:", [a.get("order") for a in accesses[:10]])
    print("首条:", json.dumps(accesses[0], ensure_ascii=False))
    print("末条:", json.dumps(accesses[-1], ensure_ascii=False))
    ex_first = [a.get("first_exchange") for a in accesses]
    ex_last = [a.get("last_exchange") for a in accesses]
    print(f"交换号范围: {min(ex_first)} .. {max(ex_last)}")
    print("sample_count 分布 top5:", Counter(a.get("sample_count") for a in accesses).most_common(5))


if __name__ == "__main__":
    main()
