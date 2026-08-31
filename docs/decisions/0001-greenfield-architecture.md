# ADR 0001: Greenfield Master Architecture

Status: Accepted

Date: 2026-08-31

## Decision

Develop the production master as a new layered project. The previous `/home/orangepi/ethercat/E_master`
programs are experimental evidence only and are not a source-code base for the new runtime.

SOEM, vendor ESI/manuals, verified identities, PDO layouts, and recorded hardware observations may
be reused. A fact enters the new runtime only through the reviewed catalog, configuration, or test
evidence.

## Rationale

The previous project combines device configuration, PDO access, lifecycle, diagnostics, timing,
CiA 402 control, and test behavior in large single-file programs. Its new and legacy entry points
also disagree on synchronization configuration. Incremental refactoring would preserve ambiguous
behavior and make safety claims difficult to establish.

## Consequences

Initial progress is intentionally slower than copying the existing program. In return, dependencies
are directional, hardware behavior is gated, device assumptions are traceable, and most logic can
be tested without physical EtherCAT hardware.
