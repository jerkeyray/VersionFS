#!/usr/bin/env python3
"""VersionFS benchmark harness.

Mounts a fresh VersionFS instance per workload, runs one warm-up plus N measured
trials, and reports the median wall-clock time. The same workload is also run
against a plain directory on the same filesystem ("direct") so the overhead of
FUSE + versioning can be separated from the cost of the underlying storage.

Usage: bench.py --variant NAME=<versionfs>,<versionfsctl> [--variant ...] [--trials N]
                [--json out.json] [--only w1,w2]

Every workload also runs on a plain directory ("direct"), and all variants are interleaved
trial by trial so host noise affects them equally.
"""
import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time

MiB = 1024 * 1024


def write_file(path, size, block=MiB, flags=os.O_WRONLY | os.O_CREAT | os.O_TRUNC, fsync=True):
    fd = os.open(path, flags, 0o644)
    try:
        chunk = b"x" * block
        remaining = size
        while remaining > 0:
            n = os.write(fd, chunk[: min(block, remaining)])
            remaining -= n
        if fsync:
            os.fsync(fd)
    finally:
        os.close(fd)


class Mount:
    def __init__(self, daemon, ctl):
        self.daemon, self.ctl = daemon, ctl
        self.root = tempfile.mkdtemp(prefix="vfs-bench-", dir="/tmp")
        self.source = os.path.join(self.root, "source")
        self.mount = os.path.join(self.root, "mount")
        os.makedirs(self.source)
        os.makedirs(self.mount)
        self.extra = {}
        self.proc = subprocess.Popen([daemon, "mount", "--source", self.source, "--mount", self.mount,
                                      "--foreground"], stderr=subprocess.DEVNULL)
        sock = os.path.join(self.source, ".versionfs", "control.sock")
        for _ in range(100):
            if os.path.exists(sock) and os.path.ismount(self.mount):
                return
            time.sleep(0.05)
        raise RuntimeError("daemon did not start")

    def ctl_run(self, *args, check=True):
        return subprocess.run([self.ctl, "--source", self.source, *args], capture_output=True,
                              text=True, check=check)

    def stored_bytes(self):
        total = 0
        objects = os.path.join(self.source, ".versionfs", "objects")
        for name in os.listdir(objects):
            total += os.path.getsize(os.path.join(objects, name))
        return total

    def close(self):
        subprocess.run(["fusermount3", "-u", self.mount], check=False)
        self.proc.wait(timeout=30)
        shutil.rmtree(self.root, ignore_errors=True)


class Direct:
    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="vfs-direct-", dir="/tmp")
        self.mount = self.root
        self.extra = {}

    def stored_bytes(self):
        return 0

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)


# --- workloads: setup(dir) runs untimed, run(dir) is timed -------------------------------

def w_seq_new(d):
    """Sequential 64 MiB write to a new file (no snapshot needed)."""
    write_file(os.path.join(d, "new.bin"), 64 * MiB)


def w_overwrite_setup(d):
    write_file(os.path.join(d, "big.bin"), 64 * MiB)


def w_overwrite(d):
    """O_TRUNC rewrite of an existing 64 MiB file (forces a full pre-change snapshot)."""
    write_file(os.path.join(d, "big.bin"), 64 * MiB)


def w_inplace_setup(d):
    write_file(os.path.join(d, "db.bin"), 64 * MiB)


def w_inplace(d):
    """Open an existing 64 MiB file O_WRONLY (no truncate) and patch 4 KiB, like a DB/log append."""
    fd = os.open(os.path.join(d, "db.bin"), os.O_WRONLY)
    try:
        os.pwrite(fd, b"y" * 4096, 32 * MiB)
        os.fsync(fd)
    finally:
        os.close(fd)


def w_small_files(d):
    """Create 1,000 files of 4 KiB each (metadata-heavy, like unpacking a source tree)."""
    sub = os.path.join(d, "tree")
    os.mkdir(sub)
    for i in range(1000):
        write_file(os.path.join(sub, f"f{i:04d}.c"), 4096, block=4096, fsync=False)


def w_parallel(d):
    """Four threads, each writing a new 64 MiB file in 128 KiB chunks."""
    threads = [threading.Thread(target=write_file, args=(os.path.join(d, f"p{i}.bin"), 64 * MiB, 128 * 1024))
               for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def w_small_writes(d):
    """10,000 sequential 4 KiB write() calls to one new file (per-write overhead)."""
    write_file(os.path.join(d, "log.bin"), 10000 * 4096, block=4096, fsync=False)


def w_resave_setup(d):
    write_file(os.path.join(d, "doc.txt"), 4 * MiB)


def w_resave(d):
    """Editor-style: save an unchanged 4 MiB document 10 times, plus 10 `touch` calls."""
    p = os.path.join(d, "doc.txt")
    for _ in range(10):
        write_file(p, 4 * MiB, fsync=False)
    for _ in range(10):
        os.utime(p)


AGENT_SCRIPT = r"""
cd "$1"
for f in src/*.c; do sed -i 's/value/answer/' "$f"; done      # refactor 200 files in place
rm -f src/mod01*.c                                                # delete 10 files
mkdir -p build && for i in $(seq 1 50); do printf 'obj%d' $i > build/o$i.o; done
for i in 2 3 4 5 6; do mv src/mod00$i.c src/renamed$i.c; done  # rename 5 files
printf 'patched\n' >> README.md                                   # append to an existing file
"""


def tree_digest(d):
    import hashlib
    digest = hashlib.sha256()
    for base, dirs, files in sorted(os.walk(d)):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            digest.update(os.path.relpath(path, d).encode())
            with open(path, "rb") as f:
                digest.update(f.read())
    return digest.hexdigest()


def w_agent_setup(d, target):
    os.mkdir(os.path.join(d, "src"))
    for i in range(200):
        with open(os.path.join(d, "src", f"mod{i:03d}.c"), "w") as f:
            f.write(f"int value_{i} = {i};\n" * 300)  # ~6 KiB per file
    with open(os.path.join(d, "README.md"), "w") as f:
        f.write("project\n")
    target.extra["before"] = tree_digest(d)


def w_agent(d, target):
    """An agent-like session: sed -i over 200 files, deletes, build output, renames, an append.
    On VersionFS it runs under `versionfsctl run`, so every effect is attributed to a session."""
    if isinstance(target, Mount):
        subprocess.run([target.ctl, "--source", target.source, "run", "--label", "bench", "--", "bash", "-c",
                        AGENT_SCRIPT, "_", d], check=True, stderr=subprocess.DEVNULL)
        start = time.perf_counter()
        target.ctl_run("undo", "bench")
        target.extra["undo_s"] = time.perf_counter() - start
        target.extra["undo_restored_tree"] = tree_digest(d) == target.extra["before"]
    else:
        subprocess.run(["bash", "-c", AGENT_SCRIPT, "_", d], check=True)


WORKLOADS = {
    "seq_new_64M": (None, w_seq_new),
    "overwrite_64M": (w_overwrite_setup, w_overwrite),
    "inplace_patch_64M": (w_inplace_setup, w_inplace),
    "small_files_1000": (None, w_small_files),
    "parallel_4x64M": (None, w_parallel),
    "small_writes_10k": (None, w_small_writes),
    "resave_10x4M": (w_resave_setup, w_resave),
    "agent_session": (w_agent_setup, w_agent),
}

# Workloads that need the mount/direct object rather than just a directory.
TARGET_AWARE = {"agent_session"}


def measure(kind, daemon, ctl, name):
    setup, run = WORKLOADS[name]
    aware = name in TARGET_AWARE
    target = Mount(daemon, ctl) if daemon else Direct()
    try:
        if setup:
            setup(target.mount, target) if aware else setup(target.mount)
        start = time.perf_counter()
        try:
            run(target.mount, target) if aware else run(target.mount)
            ok = True
        except (OSError, subprocess.CalledProcessError) as error:
            ok = str(error)
        elapsed = time.perf_counter() - start - target.extra.get("undo_s", 0)
        return elapsed, ok, target.stored_bytes(), dict(target.extra)
    finally:
        target.close()


def host_info():
    def cmd(*a):
        try:
            return subprocess.run(a, capture_output=True, text=True).stdout.strip()
        except OSError:
            return ""
    cpu = ""
    try:
        for line in open("/proc/cpuinfo"):
            if line.lower().startswith(("model name", "cpu model")):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    mem = ""
    try:
        mem = open("/proc/meminfo").readline().split(":", 1)[1].strip()
    except OSError:
        pass
    return {"kernel": platform.release(), "cpus": os.cpu_count(), "cpu": cpu, "memory": mem,
            "libfuse": cmd("pkg-config", "--modversion", "fuse3"),
            "fs": cmd("stat", "-f", "-c", "%T", "/tmp")}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--variant", action="append", required=True,
                        help="name=/path/to/versionfs,/path/to/versionfsctl (repeatable)")
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--json")
    parser.add_argument("--only")
    args = parser.parse_args()
    variants = {"direct": (None, None)}
    for spec in args.variant:
        name, paths = spec.split("=", 1)
        variants[name] = tuple(paths.split(","))
    names = args.only.split(",") if args.only else list(WORKLOADS)
    results = {"host": host_info(), "trials": args.trials, "variants": list(variants), "workloads": {}}
    for name in names:
        samples = {kind: [] for kind in variants}
        # Interleave variants trial by trial so drift in the host (page cache, VM I/O) is shared
        # evenly instead of biasing whichever variant happened to run last. Round 0 is warm-up.
        for round_ in range(args.trials + 1):
            for kind, (daemon, ctl) in variants.items():
                sample = measure(kind, daemon, ctl, name)
                if round_:
                    samples[kind].append(sample)
        results["workloads"][name] = {}
        for kind, runs in samples.items():
            times = [r[0] for r in runs]
            errors = next((r[1] for r in runs if r[1] is not True), None)
            entry = {"median_s": statistics.median(times), "trials_s": times, "stored_bytes": runs[-1][2],
                     "error": errors}
            undo = [r[3]["undo_s"] for r in runs if "undo_s" in r[3]]
            if undo:
                entry["undo_median_s"] = statistics.median(undo)
                entry["undo_restored_tree"] = all(r[3].get("undo_restored_tree") for r in runs)
            results["workloads"][name][kind] = entry
            status = f"FAILED ({errors[:40]})" if errors else f"{entry['median_s'] * 1000:9.1f} ms"
            if undo:
                status += f"  undo={entry['undo_median_s'] * 1000:.1f} ms exact={entry['undo_restored_tree']}"
            print(f"{name:20s} {kind:10s} {status}  stored={entry['stored_bytes'] / MiB:.2f} MiB", flush=True)
    if args.json:
        with open(args.json, "w") as out:
            json.dump(results, out, indent=2)


if __name__ == "__main__":
    sys.exit(main())
