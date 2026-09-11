import json, sys

with open("/home/orangepi/ethercat-master/runtime/reports/orangepi-current-bench-idle-latest.json") as f:
    r = json.load(f)

t = r["axes"][0]["timing"]
dist = t.get("distribution")
if dist is None:
    print("FAIL: distribution key missing")
    sys.exit(1)

print("distribution keys:", list(dist.keys()))
for metric in ["send_duration_ns", "round_trip_ns", "send_lateness_ns",
               "dc_arrival_phase_ns", "phase_error_ns", "sync0_margin_ns"]:
    d = dist.get(metric, {})
    print(f"  {metric}: samples={d.get('samples')}, p50={d.get('p50')}, p99={d.get('p99')}, overflow={d.get('overflow')}")
print("OK")
