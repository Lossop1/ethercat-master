# Environment Findings

## 2026-08-31 initial engineering inspection

- Target: Orange Pi 6 Plus, Ubuntu 24.04.3, aarch64.
- Kernel: `6.6.89-cix`, `CONFIG_PREEMPT=y`, not PREEMPT_RT, `CONFIG_HZ=250`.
- The `orangepi` account has no real-time scheduling allowance (`ulimit -r` was `0`).
- CPU and NIC IRQ isolation were not configured.
- Both relevant NICs use the Realtek `r8126` driver.
- `enp97s0` is the management interface and must not be used for EtherCAT.
- `enp49s0` has lidar/PTP configuration ownership and is not currently available as a dedicated
  EtherCAT interface.
- No third dedicated EtherCAT interface has been identified.
- The board clock reported `2026-08-19T18:48:23+08:00` while the engineering workspace date was
  `2026-08-31`. Clock synchronization must be corrected before creating traceable hardware records.

These are blocking baseline findings, not settings changed by this project. No kernel, service,
network, scheduler, or clock configuration was modified during inspection or build verification.
