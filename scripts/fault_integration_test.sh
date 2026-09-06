#!/usr/bin/env bash
set -euo pipefail

daemon=${1:?usage: fault_integration_test.sh /path/to/versionfs /path/to/versionfsctl}
ctl=${2:?usage: fault_integration_test.sh /path/to/versionfs /path/to/versionfsctl}
root=$(mktemp -d /tmp/versionfs-fault-XXXXXX)
pid=""

cleanup() {
  if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    fusermount3 -u "$mount_dir" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$root"
}
trap cleanup EXIT

start_daemon() {
  local point=${1:-}
  rm -f "$source_dir/.versionfs/control.sock"
  if [[ -n "$point" ]]; then
    VERSIONFS_CRASH_AT="$point" "$daemon" mount --source "$source_dir" --mount "$mount_dir" --foreground &
  else
    "$daemon" mount --source "$source_dir" --mount "$mount_dir" --foreground &
  fi
  pid=$!
  for _ in $(seq 1 50); do
    "$ctl" --source "$source_dir" status >/dev/null 2>&1 && return
    sleep 0.1
  done
  echo "daemon did not become ready" >&2
  exit 1
}

stop_daemon() {
  fusermount3 -u "$mount_dir"
  wait "$pid"
  pid=""
}

for point in after-temp-sync after-object-publish after-metadata-commit before-live-write; do
  source_dir="$root/$point/source"
  mount_dir="$root/$point/mount"
  mkdir -p "$source_dir" "$mount_dir"

  # Establish a live file. The subsequent write will need to preserve it.
  start_daemon
  printf 'base\n' > "$mount_dir/report.txt"
  stop_daemon

  start_daemon "$point"
  printf 'candidate\n' > "$mount_dir/report.txt" || true
  wait "$pid" || [[ $? -eq 86 ]]
  pid=""

  start_daemon
  [[ $(cat "$mount_dir/report.txt") == "base" ]]
  "$ctl" --source "$source_dir" verify
  if [[ "$point" == after-metadata-commit || "$point" == before-live-write ]]; then
    version=$("$ctl" --source "$source_dir" history /report.txt | awk 'NR == 2 {print $2}')
    [[ -n "$version" ]]
    [[ $("$ctl" --source "$source_dir" show /report.txt "$version") == "base" ]]
  fi
  stop_daemon
done

echo "fault-injection recovery tests passed"
