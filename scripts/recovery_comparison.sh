#!/usr/bin/env bash
# Recovery comparison: one agent session, one human working alongside it, three ways to undo
# the agent. Each strategy starts from an identical project and is scored on the same checks.
#
#   git        - `git checkout -- . && git clean -fd`, the usual "throw away the agent's changes"
#   snapshot   - copy the tree when the session starts, put the copy back afterwards
#                (what volume snapshots, sandbox discard, and checkpoint/rewind tools amount to)
#   versionfs  - `versionfsctl undo <session>`: revert only what the session's processes did
#
# Usage: recovery_comparison.sh /path/to/versionfs /path/to/versionfsctl
set -euo pipefail

daemon=${1:?usage: recovery_comparison.sh /path/to/versionfs /path/to/versionfsctl}
ctl=${2:?usage: recovery_comparison.sh /path/to/versionfs /path/to/versionfsctl}
root=$(mktemp -d /tmp/versionfs-recovery-XXXXXX)
pid=""
cleanup() {
  if [[ -n "$pid" ]]; then fusermount3 -u "$root/mount" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  rm -rf "$root"
}
trap cleanup EXIT

# A small project under git, with an ignored secret that git has never seen.
make_project() {
  local dir=$1
  mkdir -p "$dir"
  cd "$dir"
  git init -q && git config user.email t@example.com && git config user.name test
  printf 'int version = 1;\n' > main.c
  printf 'keep me\n' > notes.md
  printf 'draft\n' > shared.txt
  printf '.env\nbuild/\n' > .gitignore
  git add -A && git commit -qm initial
  printf 'API_KEY=secret\n' > .env
  cd - >/dev/null
}

# The agent works in two steps; the human acts in between (signalled with a flag file).
AGENT='cd "$1"
sed -i s/1/2/ main.c
printf "agent rewrite\n" > shared.txt
while [ ! -e "$2" ]; do sleep 0.05; done
rm .env
mkdir -p build && printf obj > build/out.o
mv notes.md NOTES.md
ln -s main.c current.c'

human() {
  printf 'human fix\n' > "$1/shared.txt"
  printf 'todo: review agent\n' > "$1/human-notes.txt"
  touch "$2"
}

check() {  # check <dir> -> prints 8 pass/fail columns
  local d=$1 out=()
  [[ $(cat "$d/main.c" 2>/dev/null) == "int version = 1;" ]] && out+=(yes) || out+=(no)
  [[ $(cat "$d/.env" 2>/dev/null) == "API_KEY=secret" ]] && out+=(yes) || out+=(no)
  [[ $(cat "$d/notes.md" 2>/dev/null) == "keep me" && ! -e "$d/NOTES.md" ]] && out+=(yes) || out+=(no)
  [[ ! -e "$d/build" ]] && out+=(yes) || out+=(no)
  [[ ! -L "$d/current.c" ]] && out+=(yes) || out+=(no)
  [[ $(cat "$d/shared.txt" 2>/dev/null) == "human fix" ]] && out+=(yes) || out+=(no)
  [[ $(cat "$d/human-notes.txt" 2>/dev/null) == "todo: review agent" ]] && out+=(yes) || out+=(no)
  local score=0
  for v in "${out[@]}"; do [[ $v == yes ]] && score=$((score + 1)); done
  printf '%s\t%s\t%s/7\n' "$2" "$(IFS=$'\t'; echo "${out[*]}")" "$score"
}

run_plain() {  # run_plain <dir>: agent + human on an ordinary directory
  local dir=$1 flag="$root/flag-$RANDOM"
  bash -c "$AGENT" _ "$dir" "$flag" &
  local agent=$!
  sleep 0.3; human "$dir" "$flag"; wait "$agent"
}

printf 'strategy\tagent edit undone\tdeleted secret back\trename undone\tbuild output removed\tsymlink removed\thuman edit kept\thuman file kept\tscore\n'

# git reset
make_project "$root/git"
run_plain "$root/git"
(cd "$root/git" && git checkout -q -- . && git clean -qfd)
check "$root/git" "git checkout+clean"

# time-based snapshot rollback
make_project "$root/snap"
cp -a "$root/snap" "$root/snap.at-session-start"
run_plain "$root/snap"
rm -rf "$root/snap" && mv "$root/snap.at-session-start" "$root/snap"
check "$root/snap" "snapshot rollback"

# VersionFS session undo
mkdir -p "$root/source" "$root/mount"
"$daemon" mount --source "$root/source" --mount "$root/mount" --foreground 2>/dev/null &
pid=$!
for _ in $(seq 1 50); do [[ -S "$root/source/.versionfs/control.sock" ]] && break; sleep 0.1; done
make_project "$root/mount/project"
flag="$root/flag-vfs"
"$ctl" --source "$root/source" run --label agent -- bash -c "$AGENT" _ "$root/mount/project" "$flag" 2>/dev/null &
agent=$!
sleep 0.3; human "$root/mount/project" "$flag"; wait "$agent"
"$ctl" --source "$root/source" undo agent >"$root/undo.log"
check "$root/mount/project" "versionfs undo"
echo
echo "versionfs undo output:"
sed 's/^/  /' "$root/undo.log"
