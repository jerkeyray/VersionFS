#!/usr/bin/env bash
# Paced, visual demo, run inside the Linux container (./demo.sh at the repo root does all of this).
# An agent session and a developer edit the same project while the live view shows who changed
# what; then the agent's session is undone and the developer's work is kept.
#
# Usage: visual_demo.sh <build-dir>
# Advance with the Next button on the live page (or Enter in the terminal).
# AUTO=1 advances by itself (STEP_DELAY seconds, default 3);
# HOLD=N keeps the live view up for N seconds at the end.
set -euo pipefail

build=${1:-build}
ctl="$build/versionfsctl"
here=$(cd "$(dirname "$0")" && pwd)
root=/tmp/versionfs-visual-demo
src="$root/source"
mnt="$root/mount"
project="$mnt/project"
step_file="$root/step.txt"
next_file="$root/next"        # created by the live page's Next button
wait_file="$root/waiting"     # present while the demo waits for Next
total=5
url=${DEMO_URL:-http://localhost:8080}

cleanup() {
  cd /
  [[ -n "${viz:-}" ]] && kill "$viz" 2>/dev/null || true
  fusermount3 -u "$mnt" 2>/dev/null || true
  [[ -n "${daemon:-}" ]] && wait "$daemon" 2>/dev/null || true
}
trap cleanup EXIT

B=$'\e[1m'; D=$'\e[2m'; O=$'\e[0m'
rule() { printf '%s%s%s\n' "$D" "──────────────────────────────────────────────────────────────────────" "$O"; }
banner() { printf '%s' "$1" > "$step_file"; }
step() {  # step <n> <title> <one-line explanation>
  printf '\n'; rule
  printf ' %sSTEP %s/%s%s  %s%s%s\n' "$D" "$1" "$total" "$O" "$B" "$2" "$O"
  printf ' %s%s%s\n' "$D" "$3" "$O"
  rule
  banner "$1/$total|$2"
}
dev() { printf '    %s DEVELOPER %s  %s\n' "$B" "$O" "$1"; banner "2/$total|$2"; }
say() { printf '    %s\n' "$1"; }
cmd() { printf '\n    %s$ versionfsctl %s%s\n' "$D" "$*" "$O"; }
pause() {  # wait for the page's Next button or Enter in the terminal
  if [[ -n "${AUTO:-}" ]]; then sleep "${STEP_DELAY:-3}"; return; fi
  printf '\n    %s▸ Next: click Next on the page, or press Enter here%s\n' "$D" "$O"
  rm -f "$next_file"; touch "$wait_file"
  while [[ ! -e "$next_file" ]]; do
    if [[ -t 0 ]]; then read -r -t 0.3 _ && break; else sleep 0.3; fi
  done
  rm -f "$next_file" "$wait_file"
}

rm -rf "$root"
mkdir -p "$src" "$mnt"
"$build/versionfs" mount --source "$src" --mount "$mnt" --foreground 2>/dev/null &
daemon=$!
for _ in $(seq 1 50); do [[ -S "$src/.versionfs/control.sock" ]] && break; sleep 0.1; done
banner "Starting"
printf '%s\n' "Setup" "Agent works" "Agent's changes" "Preview undo" "Undo" > "$root/steps.txt"
python3 "$here/viz/server.py" --source "$src" --ctl "$ctl" --port 8080 --step-file "$step_file" \
  --outside-label developer >/dev/null 2>&1 &
viz=$!
for _ in $(seq 1 50); do python3 -c 'import urllib.request;urllib.request.urlopen("http://127.0.0.1:8080/api/state")' 2>/dev/null && break; sleep 0.1; done

clear 2>/dev/null || true
printf '\n %sVersionFS demo%s  %sundo one actor, keep everyone else%s\n' "$B" "$O" "$D" "$O"
printf ' %sLive view:%s %s\n' "$D" "$O" "$url"
printf ' %sA developer and an AI agent edit the same folder. VersionFS records which process\n made every change, then undoes only the agent.%s\n' "$D" "$O"

step 1 "The developer creates a small project" "A normal folder mounted through VersionFS; programs need no changes."
mkdir "$project"
printf 'int version = 1;\n' > "$project/main.c"
printf 'int add(int a, int b) { return a + b - 1; }\n' > "$project/utils.c"
printf 'meeting notes\n' > "$project/notes.md"
printf 'API_KEY=secret\n' > "$project/.env"
printf 'draft\n' > "$project/shared.txt"
for f in main.c utils.c notes.md .env shared.txt; do
  printf '    %-11s %s%s%s\n' "$f" "$D" "$(cat "$project/$f")" "$O"
done
pause

step 2 "An agent works while the developer keeps editing" \
     "The agent runs inside a session: everything it starts is attributed to it."
cmd run --label agent -- ./agent.sh
"$ctl" --source "$src" run --label agent -- bash -c '
  a() { printf "    \e[7m AGENT \e[0m      \$ %s\n" "$*"; }
  cd "$1"
  sleep 1; a "sed -i s/1/2/ main.c";               sed -i s/1/2/ main.c
  sleep 2; a "echo \"agent rewrite\" > shared.txt";  printf "agent rewrite\n" > shared.txt
  sleep 2; a "rm .env";                            rm .env
  sleep 2; a "cc app.c -o build/app";              mkdir build && echo "int main(void){return 0;}" | cc -x c - -o build/app
  sleep 2; a "mv notes.md NOTES.md";               mv notes.md NOTES.md
  sleep 2; a "ln -s main.c current.c";             ln -s main.c current.c
' _ "$project" 2>/dev/null &
agent=$!
sleep 4
dev "fixes the bug in utils.c" "Meanwhile the developer fixes the bug in utils.c"
printf 'int add(int a, int b) { return a + b; }\n' > "$project/utils.c"
sleep 3
dev "rewrites shared.txt (the agent changed it too)" "The developer also rewrites shared.txt, which the agent changed"
printf 'developer version\n' > "$project/shared.txt"
wait "$agent"
banner "2/$total|The agent has finished: sed, rm, cc, mv and ln, all recorded"
say "${D}agent finished${O}"
pause

step 3 "Which changes belong to the agent?" "Every change is tagged with the process that made it."
cmd changes agent
printf '\n'
python3 "$here/viz/term.py" touched
pause

step 4 "Preview the undo" "Only the agent's changes are reverted. A file both of them changed is kept."
cmd undo agent --dry-run
printf '\n'
python3 "$here/viz/term.py" plan
pause

step 5 "Undo the agent; keep the developer's work" "Everything undo overwrites is saved first, so it can be undone too."
cmd undo agent
summary=$("$ctl" --source "$src" undo agent | tail -1)
say "${D}${summary}${O}"
printf '\n'
python3 "$here/viz/term.py" final "$project"
printf '\n'; rule
printf ' %sDone.%s The agent'"'"'s changes are reverted; the developer'"'"'s utils.c fix and shared.txt are kept.\n' "$B" "$O"
rule
banner "Done|The agent's changes are reverted; the developer's utils.c fix and shared.txt are kept"

if [[ -n "${HOLD:-}" ]]; then sleep "$HOLD"; else pause; fi
