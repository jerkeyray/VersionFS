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
"$ctl" --source "$source_dir" verify
echo "mounted integration test passed"
