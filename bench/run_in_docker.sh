#!/usr/bin/env bash
# Builds the original v1 code (BASELINE_REF, with only the libfuse-3.14 compile fix applied) and
# the working tree, then benchmarks both against a plain directory, interleaved trial by trial.
#
#   docker build -t versionfs-dev docker/
#   docker run --rm --privileged --device /dev/fuse -v "$PWD":/repo versionfs-dev \
#     bash /repo/bench/run_in_docker.sh disk
#   docker run --rm --privileged --device /dev/fuse --tmpfs /tmp:rw,size=4g -v "$PWD":/repo \
#     versionfs-dev bash /repo/bench/run_in_docker.sh tmpfs
set -euo pipefail
name=${1:?usage: run_in_docker.sh <result-name>}
baseline_ref=${BASELINE_REF:-9e619d7}
git config --global --add safe.directory /repo
mkdir -p /src/base && git -C /repo archive "$baseline_ref" | tar -x -C /src/base
sed -i "s/FUSE_FILL_DIR_DEFAULTS/static_cast<fuse_fill_dir_flags>(0)/" /src/base/src/fuse_ops.cpp
cmake -S /src/base -B /bb -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build /bb -j >/dev/null 2>&1
cp -r /repo /src/v && rm -rf /src/v/build*
cmake -S /src/v -B /lb -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build /lb -j >/dev/null 2>&1
python3 /src/v/scripts/bench.py --trials "${TRIALS:-7}" \
  --variant baseline=/bb/versionfs,/bb/versionfsctl \
  --variant v2=/lb/versionfs,/lb/versionfsctl \
  --json "/repo/bench/results/$name.json" 2>&1 | grep -v "max threads\|invalid command"
