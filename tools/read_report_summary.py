#!/usr/bin/env python3
import json, sys

path = "/home/orangepi/ethercat-master/runtime/reports/orangepi-dual-with-motion-latest.json"
with open(path) as f:
    d = json.load(f)

r = d.get("result", {})
motion = d.get("motion", {})
axes = d.get("axes", [])

print("=== result ===")
for k in ("status_code", "cycle_count", "cycle_deadline_missed", "fault_latched",
          "motion_started", "motion_completed", "wkc_error_count"):
    print(f"  {k}: {r.get(k)}")

print("=== motion profile ===")
print(f"  profile_id: {motion.get('profile_id')}")
print(f"  duration_ms: {motion.get('duration_ms')}")
print(f"  settle_ms: {motion.get('settle_ms')}")

print("=== first_cycle_failure ===")
fcf = d.get("first_cycle_failure", {})
for k, v in fcf.items():
    if k != "last_dc_time_ns":
        print(f"  {k}: {v}")

print("=== first_runtime_failure ===")
# coordinator_status: -1=协调器未调用过, 0=通过, 其余=拒绝整帧（见 coordinator.h）
frf = d.get("first_runtime_failure")
if frf is None:
    print("  (无运行时故障)")
else:
    for k in ("status_code", "cycle_count", "exchange", "coordinator_status",
              "axis_count"):
        print(f"  {k}: {frf.get(k)}")
    for i, ax in enumerate(frf.get("axes", [])):
        print(f"  axis {i+1}: status_word={ax.get('status_word')} "
              f"control_word={ax.get('control_word')} "
              f"state={ax.get('observed_state')} known={ax.get('state_known')} "
              f"fault={ax.get('fault_present')}")

print("=== axes ===")
for i, ax in enumerate(axes):
    print(f"  axis {i+1}:")
    for k in ("axis_id", "initial_actual_position", "motion_final_position",
              "motion_completion_actual_position", "motion_actual_delta_counts",
              "motion_final_error_counts", "max_following_error_counts",
              "max_observed_following_error_counts", "motion_direction_match"):
        print(f"    {k}: {ax.get(k)}")
