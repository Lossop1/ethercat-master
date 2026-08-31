# Real-Time Baseline and Provisional Acceptance Metrics

These metrics define the engineering target for the 1 ms, 12-axis master. They must be confirmed
against robot-control requirements before Phase 2 approval and measured on the final hardware.

## Host baseline

- PREEMPT_RT kernel with documented kernel and configuration hashes.
- `SCHED_FIFO` permission for the service account and locked process memory.
- Dedicated CPU for the cyclic thread and intentional IRQ affinity for the EtherCAT NIC.
- A dedicated EtherCAT NIC with no IP address, route, PTP, lidar, or NetworkManager service.
- NIC interrupt moderation and offload settings recorded and intentionally configured.
- CPU frequency policy, thermal limits, and background service load recorded.

## Provisional 1 ms criteria

| Metric | Gate |
|---|---:|
| Nominal period | 1,000,000 ns |
| Wake-up lateness, p99.9 | <= 20 us |
| Wake-up lateness, p99.999 | <= 50 us |
| Worst observed wake-up lateness | <= 100 us |
| Cyclic execution time, p99.999 | <= 250 us |
| Worst observed cyclic execution time | <= 400 us |
| Missed 1 ms deadlines | 0 during an 8-hour gate run |
| Valid process-data WKC | Exact expected value on every steady-state cycle |
| Lost/late frame policy | Fault on first invalid cycle; no silent continuation |

The supplier requires the SM2 process-data event to precede Sync0 and recommends a Sync0 shift of
one quarter cycle. The current proposal sends near +50 us and places Sync0 near +250 us. Phase 2
must measure the actual frame-arrival margin and retain at least 100 us of worst-case guard time.

The vendor's quoted minimum cycles (`41.1 us` for one slave through `46.5 us` for five slaves) are
laboratory device capabilities, not proof that the Orange Pi host or a 12-axis network meets these
criteria.

## Required evidence

Record raw histograms and extrema, not only averages. A timing report must bind results to the
kernel, CPU/IRQ affinity, NIC driver/firmware, offload settings, topology, slave firmware, build
revision, compiler flags, duration, and background load. Clock source and measurement method must
be stated.
