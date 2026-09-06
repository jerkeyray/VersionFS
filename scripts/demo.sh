#!/usr/bin/env bash
set -euo pipefail

build_dir=${1:-build}
daemon="$build_dir/versionfs"
ctl="$build_dir/versionfsctl"
root=$(mktemp -d /tmp/versionfs-demo-XXXXXX)
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
for _ in $(seq 1 50); do [[ -S "$source_dir/.versionfs/control.sock" ]] && break; sleep 0.1; done

printf 'draft one\n' > "$mount_dir/project.txt"
printf 'draft two\n' > "$mount_dir/project.txt"
echo "Version history before rename:"
"$ctl" --source "$source_dir" history /project.txt
mv "$mount_dir/project.txt" "$mount_dir/final.txt"
rm "$mount_dir/final.txt"
echo "Recoverable deletions:"
"$ctl" --source "$source_dir" deleted
version=$("$ctl" --source "$source_dir" history /final.txt | awk 'NR == 2 {print $2}')
"$ctl" --source "$source_dir" restore /final.txt "$version"
echo "Restored content: $(cat "$mount_dir/final.txt")"
"$ctl" --source "$source_dir" verify
