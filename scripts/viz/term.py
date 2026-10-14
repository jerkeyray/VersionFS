#!/usr/bin/env python3
"""Readable terminal tables for the visual demo, built from the live view's /api/state.

Usage: term.py touched|plan|final [project-dir]
"""
import json
import os
import sys
import urllib.request

BOLD, DIM, INV, OFF = "\033[1m", "\033[2m", "\033[7m", "\033[0m"
INDENT = "    "


def state():
    with urllib.request.urlopen("http://127.0.0.1:8080/api/state", timeout=5) as response:
        return json.load(response)


def who(w):
    if not w:
        return ""
    if w["kind"] == "agent":
        return f"{INV} {w['text']} {OFF}"
    if w["kind"] == "undo":
        return f"{DIM}undo{OFF}"
    return f"{BOLD}{w['text']}{OFF}"


def width(text):  # printable width, ignoring ANSI codes
    out, skip = 0, False
    for ch in text:
        if ch == "\033":
            skip = True
        elif skip and ch == "m":
            skip = False
        elif not skip:
            out += 1
    return out


def table(rows, header):
    widths = [max(width(str(r[i])) for r in rows + [header]) for i in range(len(header))]
    pad = lambda i, c: str(c) + ("" if i == len(header) - 1 else " " * (widths[i] - width(str(c))))
    line = lambda r: INDENT + "   ".join(pad(i, c) for i, c in enumerate(r))
    print(DIM + line(header) + OFF)
    print(DIM + INDENT + "─" * (sum(widths) + 3 * (len(widths) - 1)) + OFF)
    for r in rows:
        print(line(r))


def name(f):
    return f["name"] + (" →" if f["link"] else "") if f["present"] else f"{DIM}{f['name']} (deleted){OFF}"


def main():
    mode = sys.argv[1]
    project = sys.argv[2] if len(sys.argv) > 2 else ""
    files = state()["files"]
    if mode == "touched":
        agent = [f for f in files if f["who"] and f["who"]["kind"] == "agent"]
        mine = [f for f in files if f["who"] and f["who"]["kind"] == "outside" and f["verb"] != "created"]
        table([[name(f), f["verb"], who(f["who"])] for f in agent + mine], ["file", "change", "by"])
        print(f"\n{INDENT}{len(agent)} files touched by the agent, "
              f"{len(mine)} changed by the developer in the meantime.")
    elif mode == "plan":
        rows = []
        for f in files:
            if not f["plan"]:
                if f["who"] and f["who"]["kind"] == "outside" and f["verb"] != "created":
                    rows.append([name(f), f"{DIM}untouched (developer's change){OFF}"])
                continue
            plan = f["plan"]
            if plan.startswith("kept"):
                plan = f"{BOLD}kept: the developer changed it too (conflict){OFF}"
            rows.append([name(f), plan])
        table(rows, ["file", "undo will"])
    elif mode == "final":
        rows = []
        for f in files:
            if not f["who"]:
                continue
            if f["who"]["kind"] == "undo":
                result = f"{f['verb']} by undo"
            elif f["who"]["kind"] == "outside":
                result = f"{BOLD}kept{OFF} (developer's version)"
            else:
                result = f"{f['verb']} by {f['who']['text']}"
            content = ""
            path = os.path.join(project, f["name"])
            if f["present"] and project and os.path.isfile(path) and not os.path.islink(path):
                with open(path, errors="replace") as handle:
                    content = DIM + handle.read().strip().replace("\n", " ")[:42] + OFF
            rows.append([name(f), result, content])
        table(rows, ["file", "result", "content"])


if __name__ == "__main__":
    main()
