#!/usr/bin/env bash
# Paced, visual demo, run inside the Linux container (./demo.sh at the repo root does all of this).
# An agent session and a developer edit the same project while the live view shows who changed
# what; then the agent's session is undone and the developer's work is kept.
#
# Usage: visual_demo.sh <build-dir>
# Press Enter to advance. AUTO=1 advances by itself (STEP_DELAY seconds, default 3);
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
total=5

cleanup() {
  cd /
  [[ -n "${viz:-}" ]] && kill "$viz" 2>/dev/null || true
  fusermount3 -u "$mnt" 2>/dev/null || true
  [[ -n "${daemon:-}" ]] && wait "$daemon" 2>/dev/null || true
}
trap cleanup EXIT

bold=$'\e[1m'; dim=$'\e[2m'; off=$'\e[0m'
banner() { printf '%s' "$1" > "$step_file"; }
step() {  # step <n> <text>: terminal heading + browser banner
  printf '\n%s[%s/%s] %s%s\n' "$bold" "$1" "$total" "$2" "$off"
  banner "$1/$total|$2"
}
note() { printf '      %s\n' "$1"; banner "$2|$1"; }
pause() {
  if [[ -n "${AUTO:-}" ]]; then sleep "${STEP_DELAY:-3}"; else read -r -p "${dim}      press Enter to continue${off} " _; fi
}
run() { printf '%s      $ versionfsctl %s%s\n' "$dim" "$*" "$off"; "$ctl" --source "$src" "$@" | sed 's/^/      /'; }

rm -rf "$root"
mkdir -p "$src" "$mnt"
"$build/versionfs" mount --source "$src" --mount "$mnt" --foreground 2>/dev/null &
daemon=$!
for _ in $(seq 1 50); do [[ -S "$src/.versionfs/control.sock" ]] && break; sleep 0.1; done
banner "Starting"
python3 "$here/viz/server.py" --source "$src" --ctl "$ctl" --port 8080 --step-file "$step_file" --outside-label developer >/dev/null 2>&1 &
viz=$!

step 1 "The developer creates a small project"
mkdir "$project"
printf 'int version = 1;\n' > "$project/main.c"
printf 'int add(int a, int b) { return a + b - 1; }\n' > "$project/utils.c"
printf 'meeting notes\n' > "$project/notes.md"
printf 'API_KEY=secret\n' > "$project/.env"
printf 'draft\n' > "$project/shared.txt"
printf '      %s\n' "$(cd "$project" && ls -A | tr '\n' ' ')"
pause

step 2 "An agent works in a session while the developer keeps editing"
"$ctl" --source "$src" run --label agent -- bash -c '
  cd "$1"
  sleep 1;  sed -i s/1/2/ main.c
  sleep 2;  printf "agent rewrite\n" > shared.txt
  sleep 2;  rm .env
  sleep 2;  mkdir build && echo "int main(void){return 0;}" | cc -x c - -o build/app
  sleep 2;  mv notes.md NOTES.md
  sleep 2;  ln -s main.c current.c
' _ "$project" 2>/dev/null &
agent=$!
sleep 4
note "Meanwhile the developer fixes the bug in utils.c" "2/$total"
printf 'int add(int a, int b) { return a + b; }\n' > "$project/utils.c"
sleep 3
note "The developer also rewrites shared.txt, which the agent changed" "2/$total"
printf 'developer version\n' > "$project/shared.txt"
wait "$agent"
note "The agent has finished: sed, rm, cc, mv and ln, all recorded" "2/$total"
pause

step 3 "Which changes belong to the agent?"
run changes agent
pause

step 4 "Preview the undo: shared.txt is a conflict and will be kept"
run undo agent --dry-run
pause

step 5 "Undo the agent; keep the developer's work"
run undo agent
printf '\n'
printf '      %-11s %s\n' "main.c" "$(cat "$project/main.c")" "utils.c" "$(cat "$project/utils.c")" \
                        "shared.txt" "$(cat "$project/shared.txt")" ".env" "$(cat "$project/.env")"
printf '      %s\n' "files: $(cd "$project" && ls -A | tr '\n' ' ')"
banner "Done|The agent's changes are reverted; the developer's utils.c fix and shared.txt are kept"

if [[ -n "${HOLD:-}" ]]; then sleep "$HOLD"; else
  read -r -p "${dim}      press Enter to finish${off} " _
fi
