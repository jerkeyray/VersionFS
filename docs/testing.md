# Testing and Evaluation Guide

Use this guide to separate tests that run without a mount from tests that require Linux with `/dev/fuse`. Record commands, host details, and results for the final report.

## Test ladder

| Level | Command | What it establishes |
| --- | --- | --- |
| Unit | `ctest --test-dir build --output-on-failure` | Paths, metadata, snapshots, history, deleted restore, and checksums work in isolation. |
| Mounted integration | `bash scripts/integration_test.sh ./build/versionfs ./build/versionfsctl` | Normal FUSE operations work end-to-end. |
| Crash recovery | `bash scripts/fault_integration_test.sh ...` | Recovery ordering survives deliberate daemon termination. |
| Benchmark | `bash scripts/benchmark.sh <mount> <source> 64` | Measures the cost of FUSE and full-file snapshots. |

## Unit and mounted tests

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash scripts/integration_test.sh ./build/versionfs ./build/versionfsctl
```

The mounted script uses disposable `/tmp` directories and validates create, overwrite, read, history lookup, rename, delete, restore, hidden metadata, symlink traversal, metadata snapshots, active-writer restore rejection, concurrent writers, and checksum verification.

## Crash recovery

Fault injection is opt-in and compiled out of normal builds:

```bash
cmake -S . -B build-faults -DVERSIONFS_ENABLE_FAULT_INJECTION=ON
cmake --build build-faults --parallel
bash scripts/fault_integration_test.sh ./build-faults/versionfs ./build-faults/versionfsctl
```

The test expects daemon status `86`, remounts the same backing store, then validates the live file and metadata.

| Fault point | Simulated interruption | Required result after remount |
| --- | --- | --- |
| `after-temp-sync` | Temporary object written but not published. | Temporary data is removed; live file remains unchanged. |
| `after-object-publish` | Object exists but metadata has not committed. | Live file remains unchanged; an unreferenced object may remain for audit. |
| `after-metadata-commit` | Version record is durable; live write has not started. | Live file remains unchanged; the preserved version is readable and valid. |
| `before-live-write` | Snapshot completed immediately before `pwrite`. | Live file remains unchanged; the preserved version is readable and valid. |

## Benchmark procedure

Mount VersionFS, then run:

```bash
bash scripts/benchmark.sh /tmp/versionfs-mount /tmp/versionfs-source 64
```

Run one warm-up plus five measured trials. Report the median and record CPU model, RAM, storage type, Linux/kernel version, libfuse version, compiler version, and filesystem type.

| Case | File size | Trial 1 | Trial 2 | Trial 3 | Trial 4 | Trial 5 | Median |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Direct backing write | 64 MiB |  |  |  |  |  |  |
| VersionFS first overwrite | 64 MiB |  |  |  |  |  |  |
| VersionFS later overwrite | 64 MiB |  |  |  |  |  |  |

The expected first-overwrite penalty is a full read/copy/sync of the old file before the new data is allowed. Explain this trade-off rather than treating it as an unexpected result.

## Submission checklist

- Save output from unit, mounted integration, and fault-recovery tests.
- Capture a screenshot of `versionfsctl history`, deletion, and restore.
- Add benchmark results and a short overhead analysis.
- State the limits from [design.md](design.md): full-copy storage cost, global mutation serialization, and no multi-file transactions.
