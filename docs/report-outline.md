# Suggested OS Class Report Outline

Use this outline as a concise companion to the implementation. Replace bracketed items with your own environment, screenshots, and benchmark results.

## 1. Problem and objective

Explain why ordinary filesystems overwrite or delete previous state, then state the VersionFS objective: provide transparent pre-change versions in userspace without changing the kernel or backing filesystem.

## 2. Architecture

Include the component diagram from [design.md](design.md). Explain the request path from application to Linux VFS, the FUSE driver, the daemon, the repository manager, SQLite, immutable objects, and the backing directory.

## 3. Design decisions

Discuss the high-level FUSE API, SQLite WAL, stable UUIDs across rename, full-file immutable snapshots, repository mutation locking, and the private Unix control socket. For each, state the correctness benefit and the trade-off.

## 4. Consistency and crash recovery

Include the snapshot sequence diagram. State the ordering invariant: object sync, atomic publication, metadata commit, then live mutation. Describe the four fault-injection tests and include observed output.

## 5. Evaluation

Use the benchmark table in [testing.md](testing.md). Compare direct and VersionFS writes, explain the first-overwrite penalty, and state the test host configuration.

## 6. Limitations and future work

State the v1 limits honestly: full-copy storage growth, global mutation serialization, no hard-link identity or xattrs/ACLs, sparse-file limitations, no multi-file transactions, and no direct-backing-store modification support. Propose deduplication, retention policies, garbage collection, and richer POSIX coverage as future work.

## 7. Demonstration script

Demonstrate mount, create, overwrite, list history, rename, delete, list deleted entries, restore, verify, and unmount. `scripts/demo.sh` provides the same flow for a repeatable recording.
