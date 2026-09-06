# VersionFS

VersionFS is a Linux FUSE3 filesystem that presents a normal directory while preserving the state of a regular file immediately before it changes. Applications use familiar commands—`cat`, editors, `cp`, `mv`, and `rm`—through the mountpoint. Version history, deletion recovery, integrity checks, and restore operations are exposed through a separate CLI.

It is designed as an operating-systems course project: the code makes the kernel/userspace filesystem boundary, persistence protocol, metadata model, locking, and recovery behavior explicit and inspectable.

## What it demonstrates

| OS concept | Where VersionFS demonstrates it |
| --- | --- |
| Filesystem architecture | Linux VFS sends requests to a userspace FUSE daemon, which delegates to a backing filesystem. |
| File operations | FUSE callbacks implement open, read, write, create, truncate, rename, delete, directory, symlink, and metadata operations. |
| Metadata and storage | SQLite stores stable file identities and version metadata; immutable objects store file contents. |
| Consistency | A mutation is serialized with snapshot publication, so history is durable before the live file changes. |
| Crash recovery | Temporary objects are cleaned at startup; SQLite WAL and checksum verification protect committed history. |
| Concurrency | Writable handles are tracked; restoration is rejected while a target has active writers. |

## Architecture at a glance

```text
application → Linux VFS → FUSE kernel driver → VersionFS daemon
                                             ├─ live backing directory
                                             └─ .versionfs/
                                                ├─ metadata.db       (SQLite WAL)
                                                ├─ objects/          (immutable snapshots)
                                                ├─ tmp/              (staging files)
                                                └─ control.sock      (private CLI channel)
```

The `.versionfs` directory exists in the backing store but is deliberately hidden from the mountpoint. See [the design document](docs/design.md) for the data model and recovery protocol.

## Requirements

Use Ubuntu 24.04 or another Linux system with FUSE support. macOS is suitable for building the portable storage core, but the mounted daemon is Linux-only.

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config fuse3 libfuse3-dev libsqlite3-dev libssl-dev
```

Confirm that the current user can access FUSE:

```bash
test -r /dev/fuse && echo "FUSE is available"
```

If this fails, enable FUSE for the VM/container or add the user to the distribution's FUSE access group and start a new login session.

## Build

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The build produces:

- `build/versionfs` — the filesystem daemon.
- `build/versionfsctl` — history and restore CLI.

## Five-minute demo

Terminal A mounts a private backing directory. The backing directory and mountpoint must be different, non-overlapping directories.

```bash
mkdir -p /tmp/versionfs-source /tmp/versionfs-mount
./build/versionfs mount \
  --source /tmp/versionfs-source \
  --mount /tmp/versionfs-mount \
  --foreground
```

Leave Terminal A running. In Terminal B, create a file and overwrite it in a second write-open session:

```bash
printf 'draft one\n' > /tmp/versionfs-mount/report.txt
printf 'draft two\n' > /tmp/versionfs-mount/report.txt

./build/versionfsctl --source /tmp/versionfs-source history /report.txt
```

The history output lists a version ID for the original `draft one` state. Copy that ID and restore it:

```bash
./build/versionfsctl --source /tmp/versionfs-source restore /report.txt <version-id>
cat /tmp/versionfs-mount/report.txt
```

Deletion is recoverable too:

```bash
rm /tmp/versionfs-mount/report.txt
./build/versionfsctl --source /tmp/versionfs-source deleted
./build/versionfsctl --source /tmp/versionfs-source restore /report.txt <version-id>
```

Unmount when finished:

```bash
fusermount3 -u /tmp/versionfs-mount
```

For an automated version of this flow, run `bash scripts/demo.sh build`.

## CLI reference

All paths passed to `versionfsctl` are mount-relative paths beginning with `/`.

| Command | Purpose |
| --- | --- |
| `history <path>` | List preserved versions, events, sizes, and timestamps. |
| `show <path> <version-id>` | Stream a historical file's raw content to standard output. |
| `restore <path> <version-id> [--to <path>]` | Restore at the original path or create a restored copy elsewhere. |
| `deleted` | List deleted files that can still be recovered. |
| `status` | Show tracked-file, version, and object-store counts. |
| `verify [path]` | Recompute SHA-256 checksums for stored objects. |
| `purge <path> <version-id\|--all>` | Permanently delete selected history. |

## Versioning semantics

- A newly created file does not receive a meaningless empty version.
- The first mutation through each writable handle preserves the pre-change state once. A second independent write-open session creates another version.
- `O_TRUNC`, explicit truncation, `chmod`, `chown`, timestamp changes, deletion, and rename-overwrite preserve the prior state before changing it.
- Renaming a file moves its stable identity and history to the new path. Renaming over another file preserves the destination first.
- Deleting a regular file creates a final version and a tombstone. Restoring a deleted file reactivates its original identity unless `--to` creates a copy.
- `purge` is permanent; VersionFS keeps history indefinitely by default.

## Testing, recovery, and benchmarks

```bash
# Mounted integration test: FUSE operations, rename/delete, symlinks,
# metadata snapshots, concurrent writers, restore, and verification.
bash scripts/integration_test.sh ./build/versionfs ./build/versionfsctl

# Separate fault-injection build and crash-recovery test.
cmake -S . -B build-faults -DVERSIONFS_ENABLE_FAULT_INJECTION=ON
cmake --build build-faults --parallel
bash scripts/fault_integration_test.sh ./build-faults/versionfs ./build-faults/versionfsctl
```

The fault hooks are compiled out of normal builds. [Testing and evaluation](docs/testing.md) explains expected recovery outcomes and how to collect benchmark results.

## Scope and limitations

VersionFS deliberately uses full-file snapshots. That makes history and recovery straightforward to demonstrate, but means the first modification of a large file costs an additional full copy and consumes equivalent snapshot storage.

v1 does not provide hard-link identity, extended attributes/ACLs, sparse-file extent preservation, cross-filesystem atomic restore, multi-file transactions, or external modification detection. The backing directory must be treated as private while mounted; changes made there bypass VersionFS.

## Repository map

- `src/fuse_ops.cpp` — FUSE adapter and control-service lifecycle.
- `src/repository.cpp` — metadata, snapshots, restoration, recovery, and integrity checks.
- `src/control.cpp` — length-prefixed Unix-domain socket protocol.
- `tests/unit_tests.cpp` — storage and path unit tests.
- `scripts/` — demo, integration, fault-injection, and benchmark runners.
- `docs/` — design, test/evaluation, and report-outline material.
