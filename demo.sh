#!/usr/bin/env bash
# One-command visual demo of VersionFS. Needs Docker (Docker Desktop on macOS).
#
#   ./demo.sh
#
# Builds the Linux image on first use, picks a free port, builds VersionFS inside the
# container, runs the paced demo and opens the live view in your browser.
set -euo pipefail
cd "$(dirname "$0")"

die() { printf 'demo: %s\n' "$1" >&2; exit 1; }
command -v docker >/dev/null || die "Docker is not installed."
docker info >/dev/null 2>&1 || die "Docker is not running. Start Docker Desktop and try again."

if ! docker image inspect versionfs-dev >/dev/null 2>&1; then
  echo "Building the Linux image (first run only, about a minute)..."
  docker build -q -t versionfs-dev docker/ >/dev/null
fi

port_busy() {
  if command -v lsof >/dev/null; then lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1
  else (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; fi
}
port=${PORT:-8080}
while port_busy "$port"; do port=$((port + 1)); done
url="http://localhost:$port"

# Open the browser once the live view answers.
(
  for _ in $(seq 1 180); do
    if curl -fs "$url/api/state" >/dev/null 2>&1; then
      [[ -n "${NO_OPEN:-}" ]] && exit 0
      if command -v open >/dev/null; then open "$url"; elif command -v xdg-open >/dev/null; then xdg-open "$url"; fi
      exit 0
    fi
    sleep 1
  done
) &
opener=$!
trap 'kill "$opener" 2>/dev/null || true' EXIT

echo "Live view: $url (opens automatically)"
tty=-i; [[ -t 0 && -t 1 ]] && tty=-it
docker run --rm $tty --privileged --device /dev/fuse -p "$port:8080" -v "$PWD":/src:ro \
  -e AUTO -e STEP_DELAY -e HOLD -e DEMO_URL="$url" versionfs-dev bash -c '
  echo "Building VersionFS..."
  cmake -S /src -B /tmp/build -DCMAKE_BUILD_TYPE=Release >/dev/null &&
  cmake --build /tmp/build -j >/dev/null 2>&1 || { echo "build failed"; exit 1; }
  exec bash /src/scripts/visual_demo.sh /tmp/build'
