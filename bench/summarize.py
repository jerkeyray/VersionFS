#!/usr/bin/env python3
"""Render benchmark JSON files (from scripts/bench.py) as Markdown tables for the report.

Usage: summarize.py [--spread] bench/results/disk.json [bench/results/tmpfs.json ...]
"""
import json
import sys

MiB = 1024 * 1024

DESCRIPTIONS = {
    "seq_new_64M": "Sequential 64 MiB write, new file",
    "overwrite_64M": "O_TRUNC rewrite of 64 MiB file",
    "inplace_patch_64M": "4 KiB in-place patch of 64 MiB file",
    "small_files_1000": "Create 1,000 x 4 KiB files",
    "parallel_4x64M": "4 threads x 64 MiB, separate files",
    "small_writes_10k": "10,000 x 4 KiB write() calls",
    "resave_10x4M": "Re-save unchanged 4 MiB doc 10x + 10 touch",
    "agent_session": "Agent session (sed -i x200, rm, mkdir, mv, append)",
}


def ms(entry, spread=False):
    if entry.get("error"):
        return "fails (EIO)" if "Errno 5" in str(entry["error"]) else "n/a"
    text = f"{entry['median_s'] * 1000:.1f}"
    if spread:
        trials = entry["trials_s"]
        text += f" ({min(trials) * 1000:.0f}-{max(trials) * 1000:.0f})"
    return text


def ratio(entry, direct):
    if entry.get("error") or direct["median_s"] < 0.001:  # ratios to sub-millisecond baselines mislead
        return "-"
    return f"{entry['median_s'] / direct['median_s']:.1f}x"


def render(path, spread):
    data = json.load(open(path))
    host = data["host"]
    print(f"### {path}\n")
    print(f"Host: {host['cpus']} vCPU {host['cpu'] or 'unknown CPU'}, {host['memory']}, kernel {host['kernel']}, "
          f"libfuse {host['libfuse']}, /tmp on {host['fs']}; median of {data['trials']} interleaved trials.\n")
    print("| Workload | direct (ms) | v1 baseline (ms) | v2 (ms) | v2 / direct | stored v1 | stored v2 |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    for name, row in data["workloads"].items():
        d, b, v = row["direct"], row["baseline"], row["v2"]
        stored = lambda e: "-" if e.get("error") else f"{e['stored_bytes'] / MiB:.1f} MiB"
        print(f"| {DESCRIPTIONS.get(name, name)} | {ms(d, spread)} | {ms(b, spread)} | {ms(v, spread)} | {ratio(v, d)} | "
              f"{stored(b)} | {stored(v)} |")
    agent = data["workloads"].get("agent_session", {}).get("v2", {})
    if "undo_median_s" in agent:
        print(f"\nSession undo of the agent workload: {agent['undo_median_s'] * 1000:.1f} ms median; "
              f"tree restored byte-for-byte in every trial: {agent['undo_restored_tree']}.")
    print()


# --spread adds (min-max) to each cell; use it for noisy hosts.
spread = "--spread" in sys.argv
for argument in sys.argv[1:]:
    if argument != "--spread":
        render(argument, spread)
