# Architecture Baseline

## Dependency direction

```text
application / robot controller
             |
        command API
             |
 multi-axis coordination and safety
             |
      CiA 402 axis model
             |
       cyclic engine
             |
       SOEM adapter
             |
 dedicated NIC and EtherCAT slaves
```

Dependencies point downward only. Application and axis code must not include SOEM headers, use
SOEM types, access PDO byte offsets, or issue mailbox requests.

## Module responsibilities

- **Core model**: typed identities, profiles, commands, feedback, and state; no OS or SOEM calls.
- **Slave catalog**: validated device identity, PDO sets, capabilities, and conversion sources.
- **SOEM adapter**: raw socket ownership, discovery, AL transitions, mailbox access, and SOEM error
  translation.
- **Cyclic engine**: the sole owner of cyclic PDO exchange and DC timing.
- **Axis model**: one independent CiA 402 state machine per physical axis.
- **Safety coordinator**: whole-robot interlocks, command validity, limits, and stop policy.
- **Supervisor**: non-real-time lifecycle, diagnostics, controlled recovery, and records.
- **API adapter**: transport-specific integration without changing internal command semantics.

## Concurrency contract

The future cyclic thread is the only thread allowed to exchange PDOs. It must not allocate memory,
perform file/console I/O, issue SDO requests, scan state, sleep on an unbounded primitive, or call
an API with an undocumented worst-case duration.

Mailbox and lifecycle operations run outside the cyclic loop. SOEM access must be serialized by
the adapter; a supervisor may not call SOEM concurrently with the cyclic engine. Commands and
feedback cross thread boundaries as complete, sequence-numbered snapshots. Partial per-axis
updates are forbidden.

## Configuration contract

JSON files in `config/` are engineering inputs. Build-generated C data is immutable at runtime.
No safety-relevant identity, PDO length, unit conversion, or limit may be introduced as an
unreviewed literal in application code.

The external control interface uses SI units and explicit coordinate frames. Raw counts are
confined to the device/PDO boundary. Transport selection (for example shared memory or ROS 2) is
open, but it must preserve atomic 12-axis frames, monotonic sequence numbers, command deadlines,
mode, validity, and per-axis status.

## Planned source ownership

Later phases may add `cyclic`, `cia402`, `safety`, `supervisor`, and `api` modules. They are not
stubbed in Phase 1 because interfaces will be introduced only with executable requirements and
tests. This prevents placeholder abstractions from becoming accidental architecture.
