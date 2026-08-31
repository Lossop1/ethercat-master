# Hardware Fingerprint Procedure

## Purpose

Capture reproducible identity and configuration facts from each physical slave without PDO
mapping, DC configuration, SAFE-OP, OP, CiA 402 enable, or motion commands.

## Preconditions

1. Motor power and mechanics are controlled under the approved bench procedure.
2. STO/E-stop behavior is verified independently of this software.
3. The selected interface is a dedicated EtherCAT port with no management, lidar, PTP, or IP role.
4. The operator has recorded cable order and physical joint labels.
5. The exact Git revision and clean/dirty status are recorded.
6. The host UTC clock is synchronized and its time source is recorded.

## Capture

Build the Debug preset, list interfaces, and verify the intended interface by MAC address and
physical link. Then run:

```sh
sudo build/linux-debug/apps/fingerprint/emaster-fingerprint \
  --interface <dedicated-interface> \
  --output records/fingerprints/<date>-<setup>.json \
  --acknowledge-preop
```

The acknowledgement means the operator understands that discovery sends EtherCAT frames and
requests PRE-OP. It does not authorize any higher state.

## Review

The resulting JSON must be valid and `restore_init_succeeded` must be `true`. Review every slave's:

- cable position and SII/CoE identity;
- model, hardware version, software version, and serial number;
- active `1C12/1C13` PDO assignments;
- `1C32/1C33` synchronization types and counters;
- encoder resolution, gear ratio, rated current, and rated torque;
- supported CiA 402 modes;
- target-profile match.

Unexpected or unreadable values are blockers. Do not "fix" a drive by writing a value during the
capture session.

## Record protection

Fingerprint records are test evidence. Do not edit them. Store the original file, its SHA-256,
the test record, software revision, and configuration hashes together.
