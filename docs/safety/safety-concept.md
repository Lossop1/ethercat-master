# Phase 1 Safety Concept

This software is not a certified functional-safety system. Electrical STO, emergency stop,
mechanical restraint, power isolation, and safe test-area controls remain external requirements.

## Software safety boundary

Phase 1 has no drive-enable feature. Its highest permitted AL state is PRE-OP. The fingerprint
tool reads SII and CoE data, then attempts to restore INIT. It does not map PDOs or issue SDO
writes.

The following invariants apply to all future phases:

- startup output is fully initialized before any process-data exchange;
- hardware enable is a lifecycle state, not a compile-time switch;
- topology and identity must match before a network may progress;
- command frames carry sequence and expiry information;
- communication loss, expired commands, invalid feedback, and process exit have defined stop
  reactions;
- recovery is bounded, recorded, and cannot silently reset a drive fault;
- one-axis and whole-robot stop policies are explicit and testable;
- logging and diagnostics never execute in the real-time path.

## Test authorization

Each hardware test record must identify the operator, observer, physical setup, power limits,
mechanical restraint, STO/E-stop verification, network interface, software revision, configuration
hash, ESI hash, and all slave serial numbers.

Permission to run a PRE-OP fingerprint does not authorize SAFE-OP, OP, CiA 402 enable, or motion.
Each later boundary requires a separate procedure and approval.
