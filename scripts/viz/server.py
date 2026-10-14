#!/usr/bin/env python3
"""Live, read-only web view of a VersionFS repository for demos.

Reads <source>/.versionfs/metadata.db (SQLite WAL allows a concurrent reader) and the backing
directory, and turns the provenance log into plain-language activity and per-file status.
While a session has ended but is not yet undone, it also shows what `undo` would do per file.

Usage: server.py --source <backing-dir> --ctl <path/to/versionfsctl> [--port 8080]
                 [--step-file F] [--outside-label developer]
"""
import argparse
import json
import os
import sqlite3
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
NOISE = {"chmod", "chown", "utimens", "rename-overwrite"}  # metadata, or part of a save
VERBS = {"create": "created", "write": "edited", "truncate": "edited", "delete": "deleted",
         "delete-symlink": "deleted", "mkdir": "created folder", "rmdir": "removed folder",
         "symlink": "created link", "restore": "restored", "undo-create": "removed",
         "undo-delete": "restored", "undo-rename": "moved back", "rename": "renamed",
         "rename-dir": "renamed", "rename-other": "renamed"}


class Config:
    source = ctl = step_file = None
    outside = "outside session"
    plan_cache = {}


def connect():
    db = os.path.join(Config.source, ".versionfs", "metadata.db")
    return sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=2)


def short(path, top):
    path = path.lstrip("/")
    return path[len(top) + 1:] if top and path.startswith(top + "/") else path


def actor(label, process):
    if label:
        return {"kind": "agent", "text": f"{label} · {process}"}
    if process == "versionfsctl":
        return {"kind": "undo", "text": "undo"}
    return {"kind": "outside", "text": Config.outside}


def activity(rows):
    """Turn raw provenance events into a short list of human-readable actions."""
    hidden, edited_by_rename = set(), set()
    for i, (eid, _t, label, pid, process, op, path, path2) in enumerate(rows):
        # `sed -i` and editors save by writing a temp file and renaming it over the original:
        # show that as one "edited <file>", not create + rename-overwrite + rename.
        if op == "rename" and path2:
            created = [r for r in rows[:i] if r[3] == pid and r[5] == "create" and r[6] == path]
            replaced = [r for r in rows[:i] if r[3] == pid and r[5] == "rename-overwrite" and r[6] == path2]
            if created and replaced:
                hidden.add(created[-1][0])
                edited_by_rename.add(eid)
        # Undo removing a file and then restoring the original at the same path (the file `sed -i`
        # put in place of main.c, say) reads as one "restored".
        if op == "undo-create" and any(r[3] == pid and r[5] == "undo-delete" and r[6] == path for r in rows[i + 1:]):
            hidden.add(eid)
        # An undo snapshot followed by removing the same file is one step: "removed".
        if op == "undo" and any(r[0] > eid and r[5] == "undo-create" and r[6] == path for r in rows[i + 1:i + 3]):
            hidden.add(eid)
    items = []
    for eid, t, label, pid, process, op, path, path2 in rows:
        if eid in hidden or op in NOISE:
            continue
        who = actor(label, process)
        if eid in edited_by_rename:
            verb, target, extra = "edited", path2, ""
        elif op == "undo":
            verb, target, extra = "restored", path, ""
        elif op in ("rename", "rename-dir", "rename-other", "undo-rename") and path2:
            verb, target, extra = VERBS[op], path2, path
        else:
            verb, target, extra = VERBS.get(op, op), path, ""
        items.append({"id": eid, "t": t, "who": who, "verb": verb, "path": target, "from": extra})
    return items


def plan_for(session):
    key = (session["id"], session["events"])
    if key not in Config.plan_cache:
        out = subprocess.run([Config.ctl, "--source", Config.source, "undo", session["id"], "--dry-run"],
                             capture_output=True, text=True, timeout=10)
        actions = {}
        for line in out.stdout.splitlines():
            if line.startswith("conflict: "):
                actions[line[len("conflict: "):].split(":", 1)[0]] = "kept: changed by both"
            elif line.startswith("would "):
                words = line[len("would "):].split()
                if words[:2] == ["restore", "deleted"]:
                    actions[words[2]] = "will be restored"
                elif words[0] == "restore":
                    actions[words[1]] = "will be restored"
                elif words[0] == "remove":
                    actions[words[-1]] = "will be removed"
                elif words[0] == "move":
                    actions[words[1]] = "will be renamed back"
                    actions[words[-1]] = "will be restored"
        Config.plan_cache[key] = actions
    return Config.plan_cache[key]


def state():
    with connect() as db:
        sessions = [{"id": r[0], "label": r[1], "status": r[2], "events": r[3]} for r in db.execute(
            "SELECT s.id,s.label,s.status,(SELECT COUNT(*) FROM events e WHERE e.session_id=s.id) "
            "FROM sessions s ORDER BY s.started_ns")]
        rows = db.execute(
            "SELECT e.id,e.time_ns,COALESCE(s.label,''),e.pid,e.process,e.op,e.path,COALESCE(e.path2,'') "
            "FROM events e LEFT JOIN sessions s ON s.id=e.session_id ORDER BY e.id").fetchall()
        deleted = {r[0] for r in db.execute("SELECT last_path FROM files WHERE deleted=1")}

    items = activity(rows)
    tops = {p.lstrip("/").split("/")[0] for r in rows for p in (r[6],) if "/" in p.lstrip("/")}
    top = tops.pop() if len(tops) == 1 else ""

    latest = {}
    for item in items:
        latest[item["path"]] = item
        if item["from"] and item["verb"] in ("renamed", "moved back"):
            latest[item["from"]] = {"who": item["who"], "verb": "renamed away"}

    session = sessions[-1] if sessions else None
    plan = plan_for(session) if session and session["status"] == "ended" else {}

    files = []
    for base, dirs, names in os.walk(Config.source):
        dirs[:] = sorted(d for d in dirs if d != ".versionfs")
        rel = os.path.relpath(base, Config.source)
        for name in sorted(names):
            full = os.path.join(base, name)
            path = "/" + os.path.normpath(os.path.join(rel, name)).lstrip("./")
            files.append({"path": path, "present": True, "link": os.path.islink(full)})
    live = {f["path"] for f in files}
    files += [{"path": p, "present": False, "link": False} for p in sorted(deleted - live)
              if p in latest and latest[p]["verb"] in ("deleted", "removed", "renamed away")]
    for f in files:
        last = latest.get(f["path"])
        f["name"] = short(f["path"], top)
        f["who"] = last["who"] if last else None
        f["verb"] = last["verb"] if last else ""
        f["plan"] = plan.get(f["path"], "")
    files.sort(key=lambda f: (not f["present"], f["name"]))

    # Consecutive identical actions by the same actor read as one line ("created a, b and c").
    feed = []
    for i in items:
        entry = {"t": i["t"], "who": i["who"], "verb": i["verb"], "paths": [short(i["path"], top)],
                 "from": short(i["from"], top) if i["from"] else ""}
        prev = feed[-1] if feed else None
        if (prev and not entry["from"] and not prev["from"] and prev["verb"] == entry["verb"]
                and prev["who"] == entry["who"] and entry["t"] - prev["t"] < 3e9):
            prev["paths"] += entry["paths"]
        else:
            feed.append(entry)
    feed = feed[::-1][:40]
    step = ""
    if Config.step_file and os.path.exists(Config.step_file):
        with open(Config.step_file) as f:
            step = f.read().strip()
    return {"step": step, "folder": top,
            "session": {"label": session["label"], "status": session["status"]} if session else None,
            "files": files, "feed": feed}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body, kind="application/json"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        try:
            if url.path == "/":
                with open(os.path.join(HERE, "index.html"), "rb") as f:
                    self.send(200, f.read(), "text/html; charset=utf-8")
            elif url.path == "/api/state":
                self.send(200, json.dumps(state()))
            else:
                self.send(404, "{}")
        except (sqlite3.Error, OSError, subprocess.SubprocessError) as error:
            self.send(503, json.dumps({"error": str(error)}))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--ctl", required=True)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--step-file", help="text file holding the demo's current step, shown as a banner")
    parser.add_argument("--outside-label", default="outside session",
                        help="how to name changes made outside any session")
    args = parser.parse_args()
    Config.source = os.path.abspath(args.source)
    Config.ctl = os.path.abspath(args.ctl)
    Config.step_file = args.step_file
    Config.outside = args.outside_label
    print(f"VersionFS live view on http://localhost:{args.port}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
