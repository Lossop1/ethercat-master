# ADR 0002: Pin SOEM and Block Production License Assumptions

Status: Partially accepted; product license unresolved

Date: 2026-08-31

## Decision

Use upstream SOEM `v2.0.0` at commit `304d1c05eab77dc0d426f1a5cf09c8cc7dc03713` as a Git
submodule. Only the SOEM adapter layer may include its headers or call its API.

The repository does not yet choose a license for the new master. SOEM 2.0.0 is offered under GPLv3
or a commercial license. Closed-source or commercial distribution is blocked until project owners
choose a compatible licensing path and record it here.

## Consequences

Builds are tied to a reviewable upstream revision. Updating SOEM requires an explicit dependency
change, build/test evidence, and an impact review of the adapter. The legacy `SOEM-master.zip` is
not a build input and is ignored locally.
