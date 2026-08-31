# EtherCAT Master

This repository is a clean-room engineering baseline for a 12-axis robot EtherCAT master.
It uses SOEM as the EtherCAT protocol implementation and keeps device knowledge, real-time
execution, CiA 402 control, safety policy, and application APIs in separate modules.

## Current status

Phase 1 is in progress. Hardware enable and motion are not implemented. The provisional topology
caps all diagnostic activity at PRE-OP and sets `hardware_enable_allowed` to `false`.

The repository currently provides:

- a pinned SOEM v2.0.0 submodule;
- a generated, typed slave catalog checked against the vendor ESI;
- a provisional 12-position topology;
- offline catalog, ESI, topology, and vendor-artifact validation;
- a Linux-only PRE-OP fingerprint tool that performs SDO reads only;
- the first architecture, safety, timing, and acceptance contracts.

Vendor originals under `docs/lz-joint/` are intentionally not distributed in this public
repository. Their expected paths and SHA-256 values are recorded in `docs/vendor/SHA256SUMS`.
When the local artifacts are installed, project validation checks their bytes and the catalog
against the ESI. A clean public checkout validates metadata only and reports that the controlled
inputs are absent.

## Build

Linux is the target platform:

```sh
git submodule update --init --recursive
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

For an offline host build without SOEM hardware tools:

```sh
cmake -S . -B build/host-debug -DBUILD_TESTING=ON -DEMASTER_BUILD_HARDWARE_TOOLS=OFF
cmake --build build/host-debug
ctest --test-dir build/host-debug --output-on-failure
```

Require all controlled vendor inputs during an engineering release check:

```sh
python3 tools/validate_project.py --root . --require-vendor-artifacts
```

## Hardware fingerprint

`emaster-fingerprint` is intentionally not a passive network observer. EtherCAT discovery sends
frames and `ecx_config_init()` requests PRE-OP. The tool requires an explicit acknowledgement,
does not map PDOs, configure distributed clocks, request SAFE-OP/OP, or write SDOs, and attempts
to restore INIT before exit.

```sh
sudo build/linux-debug/apps/fingerprint/emaster-fingerprint --list-interfaces
sudo build/linux-debug/apps/fingerprint/emaster-fingerprint \
  --interface <dedicated-interface> \
  --output fingerprint.json \
  --acknowledge-preop
```

Do not run the probe on a management, lidar, or production network interface. Follow
[the fingerprint procedure](docs/testing/hardware-fingerprint.md) before using it.

## Repository layout

- `config/`: source-of-truth slave profiles and topology.
- `include/emaster/`: stable project interfaces; no SOEM types are allowed here.
- `src/core/`: platform-independent domain logic.
- `src/soem/`: the only layer allowed to call SOEM.
- `apps/`: bounded operator and diagnostic tools.
- `tests/`: offline tests.
- `docs/`: requirements, decisions, vendor sources, and test procedures.
- `external/SOEM/`: pinned upstream dependency.

## Licensing

SOEM 2.0.0 is dual-licensed under GPLv3 or a commercial license. The licensing model for this
project has not been selected. Production or closed-source distribution is blocked until that
decision is recorded.
