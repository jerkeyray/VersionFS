#!/usr/bin/env bash
set -euo pipefail

daemon=${1:?usage: integration_test.sh /path/to/versionfs /path/to/versionfsctl}
ctl=${2:?usage: integration_test.sh /path/to/versionfs /path/to/versionfsctl}
root=$(mktemp -d /tmp/versionfs-integration-XXXXXX)
source_dir="$root/source"
mount_dir="$root/mount"
mkdir -p "$source_dir" "$mount_dir"
pid=""

cleanup() {
  if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    fusermount3 -u "$mount_dir" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$root"
}
trap cleanup EXIT

"$daemon" mount --source "$source_dir" --mount "$mount_dir" --foreground &
pid=$!
for _ in $(seq 1 50); do
  [[ -S "$source_dir/.versionfs/control.sock" ]] && break
  sleep 0.1
done
[[ -S "$source_dir/.versionfs/control.sock" ]]

printf 'first\n' > "$mount_dir/report.txt"
printf 'second\n' > "$mount_dir/report.txt"
history=$("$ctl" --source "$source_dir" history /report.txt)
version=$(printf '%s\n' "$history" | awk 'NR == 2 {print $2}')
[[ -n "$version" ]]
mv "$mount_dir/report.txt" "$mount_dir/archive.txt"
rm "$mount_dir/archive.txt"
"$ctl" --source "$source_dir" restore /archive.txt "$version"
[[ $(cat "$mount_dir/archive.txt") == "first" ]]
test ! -e "$mount_dir/.versionfs"
mkdir "$mount_dir/documents"
ln -s ../archive.txt "$mount_dir/documents/latest"
[[ $(cat "$mount_dir/documents/latest") == "first" ]]
chmod 0600 "$mount_dir/archive.txt"
metadata_history=$("$ctl" --source "$source_dir" history /archive.txt)
[[ $(printf '%s\n' "$metadata_history" | awk 'END {print NR}') -ge 3 ]]

# A restore must not race an active writable FUSE file handle.
printf 'base\n' > "$mount_dir/active.txt"
printf 'changed\n' > "$mount_dir/active.txt"
active_version=$("$ctl" --source "$source_dir" history /active.txt | awk 'NR == 2 {print $2}')
exec 3>> "$mount_dir/active.txt"
if "$ctl" --source "$source_dir" restore /active.txt "$active_version"; then
  echo "restore unexpectedly succeeded with an active writer" >&2
  exit 1
fi
exec 3>&-
"$ctl" --source "$source_dir" restore /active.txt "$active_version"
[[ $(cat "$mount_dir/active.txt") == "base" ]]

# Two write-open sessions must serialize their pre-change snapshots without corruption.
printf 'seed\n' > "$mount_dir/concurrent.txt"
(printf 'A\n' >> "$mount_dir/concurrent.txt") & first_writer=$!
(printf 'B\n' >> "$mount_dir/concurrent.txt") & second_writer=$!
wait "$first_writer"
wait "$second_writer"
concurrent_history=$("$ctl" --source "$source_dir" history /concurrent.txt)
[[ $(printf '%s\n' "$concurrent_history" | awk 'END {print NR}') -ge 3 ]]

# Regression: writes through an O_WRONLY handle without O_TRUNC (appends, in-place patches)
# used to fail with EIO because the snapshot tried to read from a write-only descriptor.
printf 'one\n' > "$mount_dir/append.txt"
printf 'two\n' >> "$mount_dir/append.txt"
[[ $(cat "$mount_dir/append.txt") == $'one\ntwo' ]]
append_version=$("$ctl" --source "$source_dir" history /append.txt | awk 'NR == 2 {print $2}')
[[ $("$ctl" --source "$source_dir" show /append.txt "$append_version") == "one" ]]

# Regression: fstat() on an open file arrives with a NULL path and used to return ENOENT,
# which broke git. Run a real commit and an fstat-after-attribute-timeout on the mount.
python3 - "$mount_dir/fstat.txt" <<'PY'
import os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o644)
os.write(fd, b"hello")
time.sleep(1.2)  # let the kernel's cached attributes expire so it asks the daemon
assert os.fstat(fd).st_size == 5
os.close(fd)
PY
git -C "$mount_dir" init -q repo
printf 'tracked\n' > "$mount_dir/repo/file.txt"
git -C "$mount_dir/repo" add -A
git -C "$mount_dir/repo" -c user.email=t@example.com -c user.name=t commit -qm "commit on VersionFS"
git -C "$mount_dir/repo" log --oneline | grep -q "commit on VersionFS"

# Regression: renaming /a_b must not move the history of the look-alike /aXb (SQL LIKE wildcard).
mkdir "$mount_dir/a_b" "$mount_dir/aXb"
printf '1\n' > "$mount_dir/aXb/x"
printf '2\n' > "$mount_dir/aXb/x"
mv "$mount_dir/a_b" "$mount_dir/renamed_dir"
"$ctl" --source "$source_dir" history /aXb/x | grep -q truncate

# Supervised session: an "agent" edits with sed -i, deletes an untracked secret, generates build
# output, renames a file and adds a symlink, while a human edits a shared file mid-session.
project="$mount_dir/project"
mkdir "$project"
printf 'int version = 1;\n' > "$project/main.c"
printf 'API_KEY=secret\n' > "$project/.env"
printf 'keep me\n' > "$project/notes.md"
printf 'draft\n' > "$project/shared.txt"
"$ctl" --source "$source_dir" run --label agent -- bash -c '
  cd "$1"
  sed -i s/1/2/ main.c
  printf "agent rewrite\n" > shared.txt
  sleep 1.5
  rm .env
  mkdir build && printf obj > build/out.o
  mv notes.md NOTES.md
  ln -s main.c current.c
' _ "$project" &
agent=$!
sleep 0.7
printf 'human fix\n' > "$project/shared.txt"   # the human keeps working in parallel
wait "$agent"
[[ $(cat "$project/main.c") == "int version = 2;" ]]
"$ctl" --source "$source_dir" sessions | grep -q 'agent	ended'
"$ctl" --source "$source_dir" changes agent | grep -q '/project/notes.md -> /project/NOTES.md'
"$ctl" --source "$source_dir" history /project/main.c | grep -q 'agent:sed'
"$ctl" --source "$source_dir" log /project/shared.txt | grep -q 'agent:bash'
plan=$("$ctl" --source "$source_dir" undo agent --dry-run)
grep -q 'conflict: /project/shared.txt' <<<"$plan"
[[ $(cat "$project/main.c") == "int version = 2;" ]]   # dry run changes nothing

# A double-forked background process is still attributed to its session (subreaper).
"$ctl" --source "$source_dir" run --label daemonized -- bash -c \
  '( setsid bash -c "sleep 0.3; printf late > \"$1/late.txt\"" & ) ; sleep 0.8' _ "$mount_dir" 2>/dev/null
"$ctl" --source "$source_dir" changes daemonized | grep -q '/late.txt'

# A process inside a session may not rewrite history, so an agent cannot erase its undo trail.
if "$ctl" --source "$source_dir" run -- "$ctl" --source "$source_dir" purge /project/main.c --all; then
  echo "purge from inside a session unexpectedly succeeded" >&2
  exit 1
fi

undo_output=$("$ctl" --source "$source_dir" undo agent)
# The dry-run plan must predict the real undo exactly.
[[ $(grep -o '[0-9]* steps, [0-9]* conflicts' <<<"$plan") == $(grep -o '[0-9]* steps, [0-9]* conflicts' <<<"$undo_output") ]]
[[ $(cat "$project/main.c") == "int version = 1;" ]]
[[ $(cat "$project/.env") == "API_KEY=secret" ]]
[[ $(cat "$project/notes.md") == "keep me" ]]
[[ $(cat "$project/shared.txt") == "human fix" ]]       # human work survives the undo
test ! -e "$project/NOTES.md"
test ! -e "$project/build"
test ! -L "$project/current.c"
"$ctl" --source "$source_dir" sessions | grep -q 'agent	undone'
if "$ctl" --source "$source_dir" undo agent 2>/dev/null; then
  echo "second undo unexpectedly succeeded" >&2
  exit 1
fi

"$ctl" --source "$source_dir" verify
echo "mounted integration test passed"
