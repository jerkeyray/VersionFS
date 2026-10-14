# Reproducible Linux + FUSE environment for building, testing, and benchmarking VersionFS.
# Run with: docker run --rm --privileged --device /dev/fuse -v "$PWD":/src versionfs-dev <cmd>
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config fuse3 libfuse3-dev libsqlite3-dev libssl-dev \
      python3 git ca-certificates time \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
