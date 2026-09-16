#!/usr/bin/env python3
"""从报告里只抠出 cycle_trace，避开 99% 体积的 accesses。

直接 json.load 整份 265 MB 报告会把它展开成一两个 GB 的 Python 对象；
这里先按字节找 "cycle_trace": 的偏移，再从这个位置 raw_decode，
内存里只留一份 bytes 和解析出来的那个小对象。

用法: python3 scripts/analysis/ring_dump.py <report.json> [n]
"""
import json
import sys

KEY = b'"cycle_trace":'


def main():
    path = sys.argv[1]
    tail = int(sys.argv[2]) if len(sys.argv) > 2 else 24

    with open(path, "rb") as handle:
        blob = handle.read()
    at = blob.find(KEY)
    if at < 0:
        print("报告里没有 cycle_trace 段")
        return 1
    del blob
    # raw_decode 需要 str；只解码剩余部分，前面 99% 的内容不再解码。
    with open(path, "rb") as handle:
        handle.seek(at + len(KEY))
        text = handle.read().decode("utf-8", "replace")
    trace, _ = json.JSONDecoder().raw_decode(text)
    del text

    cols = ["exchange", "wkc", "frame_interval_ns", "send_lateness_ns",
            "sync0_margin_ns", "send_duration_ns", "receive_duration_ns",
            "mailbox_duration_ns"]

    def dump(title, samples, limit):
        print(f"== {title} 共 {len(samples)} 条" + (f"，取最后 {limit} 条" if limit and len(samples) > limit else ""))
        print(" ".join(c[:12].rjust(12) for c in cols))
        for s in (samples[-limit:] if limit else samples):
            print(" ".join(str(s.get(c, "-"))[:12].rjust(12) for c in cols))

    mismatch = trace.get("mismatch", {})
    print(f"capacity={trace.get('capacity')} "
          f"mismatch.present={mismatch.get('present')} "
          f"mismatch.exchange={mismatch.get('exchange')} "
          f"mismatch.wkc={mismatch.get('wkc')}")
    dump("live", trace.get("live", {}).get("samples", []), tail)
    dump("mismatch(冻结环)", mismatch.get("samples", []), 0)

    ring = mismatch.get("samples", [])
    if ring:
        first = ring[0].get("exchange")
        last = ring[-1].get("exchange")
        print(f"冻结环覆盖 exchange {first} .. {last}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
