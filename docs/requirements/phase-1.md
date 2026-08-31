# Phase 1 Requirements: Trusted Non-Motion Baseline

Status: In progress

Owner: Project engineering

Target completion gate: approval for Phase 2 zero-output OP testing

## Objective

Create a reproducible, traceable, and fail-closed EtherCAT master foundation. Phase 1 must produce
reliable facts about every physical slave and freeze the architecture, safety boundary, timing
metrics, and upper-layer semantics. It must not enable a drive or command motion.

## Required deliverables

1. A Git repository with reproducible CMake builds and pinned dependencies.
2. A machine-readable slave catalog validated against a checksum-pinned ESI.
3. A 12-position topology with explicit physical axis assignment and serial numbers.
4. One fingerprint per physical slave, captured from the intended EtherCAT network.
5. Resolution of the SM3 synchronization-type conflict (`0x0002` versus `0x0022`).
6. A dedicated EtherCAT NIC decision and documented host/network baseline.
7. Frozen module boundaries and thread ownership rules.
8. Frozen safety states and failure reactions through PRE-OP.
9. Frozen command/feedback semantics independent of transport technology.
10. Offline tests, static analysis, and a controlled hardware-test record.

## Functional scope

Phase 1 software may:

- enumerate local network interfaces;
- discover EtherCAT slaves with an explicit operator acknowledgement;
- request at most PRE-OP;
- read SII identity and selected CoE objects;
- compare fingerprints with the catalog and topology;
- restore INIT after a diagnostic session;
- validate artifacts without hardware.

Phase 1 software must not:

- map or exchange process data in the fingerprint workflow;
- configure distributed clocks on hardware;
- request SAFE-OP or OP;
- write an SDO;
- execute a CiA 402 transition or set modes `8`, `9`, or `10`;
- send a non-zero position, velocity, torque, or control word;
- persist a drive parameter;
- contain a compile-time or command-line bypass for hardware enable.

## Fail-closed rules

- Unknown topology, identity, revision, PDO mapping, unit conversion, or synchronization behavior
  is a blocking error, not a warning.
- A missing or stale command must never reuse an unbounded previous command.
- Vendor defaults are evidence, not physical-device truth. Safety-relevant conversion and limits
  must come from a captured fingerprint or an approved per-axis configuration.
- Any tool that can emit EtherCAT frames must state its maximum requested AL state and require an
  explicit interface selection.

## Completion criteria

Phase 1 is complete only when all of the following are true:

- clean Debug and RelWithDebInfo builds pass on the Orange Pi;
- all offline tests and static analysis pass;
- all 12 physical slots have approved joint assignments and unique captured identities;
- the intended EtherCAT NIC is dedicated and has no conflicting service or IP role;
- the `1C32/1C33` values are confirmed on the physical firmware and approved;
- encoder resolution, gear ratio, rated torque/current, firmware, PDO assignment, and supported
  modes are captured for every axis;
- preliminary timing tests demonstrate that the host can meet the metrics in
  `docs/realtime/baseline.md`;
- safety and test owners approve entry to Phase 2.

Until then, Phase 2 and all drive-enable work remain blocked.
