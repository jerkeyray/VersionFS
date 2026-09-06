#!/usr/bin/env bash
set -euo pipefail

mount_dir=${1:?usage: benchmark.sh <mounted-versionfs-directory>}
direct_dir=${2:?usage: benchmark.sh <mounted-versionfs-directory> <direct-backing-directory>}
size_mb=${3:-64}

echo "Direct backing write (${size_mb} MiB)"
/usr/bin/time -p dd if=/dev/zero of="$direct_dir/benchmark.bin" bs=1M count="$size_mb" conv=fsync status=none
echo "VersionFS first write (${size_mb} MiB, includes snapshot if file exists)"
/usr/bin/time -p dd if=/dev/zero of="$mount_dir/benchmark.bin" bs=1M count="$size_mb" conv=fsync status=none
echo "VersionFS subsequent write in a new open session (${size_mb} MiB)"
/usr/bin/time -p dd if=/dev/zero of="$mount_dir/benchmark.bin" bs=1M count="$size_mb" conv=fsync status=none
