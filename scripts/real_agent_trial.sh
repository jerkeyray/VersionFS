#!/usr/bin/env bash
# Real-agent trial: run an actual coding agent (Claude Code by default) inside a VersionFS session
# on a real repository, make "human" edits while it works, then undo the session and check that
# the tree is back to its pre-session state plus the human's edits.
#
# Run inside the versionfs-agent image (non-root user, Claude Code preinstalled):
#   docker build -t versionfs-dev docker/
#   docker build -t versionfs-agent -f docker/Dockerfile.agent docker/
#   docker run --rm -it --privileged --device /dev/fuse -e ANTHROPIC_API_KEY \
#     -v "$PWD":/repo versionfs-agent bash /repo/scripts/real_agent_trial.sh
# Without an API key: start the container with `bash`, run `claude` once to log in, exit it,
# then run the script from the same shell.
#
# Environment overrides:
#   AGENT_CMD    command run in the project directory (default: Claude Code, non-interactive)
#   TASK         prompt given to the agent
#   HUMAN_DELAY  seconds after the agent starts before the human edits (default 20)
#   OUT          markdown report path (default /repo/bench/results/real_agent_trial.md)
set -euo pipefail

src=${SRC:-/repo}
out=${OUT:-$src/bench/results/real_agent_trial.md}
human_delay=${HUMAN_DELAY:-20}
repo_url=${REPO_URL:-https://github.com/pallets/click.git}
repo_ref=${REPO_REF:-8.1.7}
export TASK=${TASK:-"You are working in the click Python repository in the current directory. Do all of the following without asking questions:
1. Add a function slugify(text) to src/click/utils.py that lowercases text and replaces each run of non-alphanumeric characters with a single '-', trimming '-' from both ends.
2. Create tests/test_slugify.py with a few pytest tests for it.
3. Create a virtual environment in .venv, install pytest and this package (editable) into it, and run your new tests until they pass.
4. The examples/ directory is obsolete: delete it.
5. Commit all of your changes on a new git branch named agent-work."}
agent_cmd=${AGENT_CMD:-'claude -p "$TASK" --dangerously-skip-permissions'}

root=$(mktemp -d /tmp/versionfs-agent-XXXXXX)
pid=""
cleanup() {
  cd /
  if [[ -n "$pid" ]]; then fusermount3 -u "$root/mount" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  rm -rf "$root"
}
trap cleanup EXIT
now() { date +%s.%N; }
elapsed() { python3 -c "print(f'{($2 - $1):.1f}')"; }

echo "== building VersionFS"
cp -r "$src" "$root/src" && rm -rf "$root/src"/build*
cmake -S "$root/src" -B "$root/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$root/build" -j >/dev/null
daemon="$root/build/versionfs"; ctl="$root/build/versionfsctl"

echo "== mounting and cloning $repo_url@$repo_ref"
mkdir -p "$root/source" "$root/mount"
"$daemon" mount --source "$root/source" --mount "$root/mount" --foreground 2>/dev/null &
pid=$!
for _ in $(seq 1 50); do [[ -S "$root/source/.versionfs/control.sock" ]] && break; sleep 0.1; done
project="$root/mount/project"
git clone -q --depth 1 -b "$repo_ref" "$repo_url" "$project" 2>/dev/null
cp -a "$project" "$root/expected"

human() {  # the person keeps working while the agent runs
  printf '\nHuman changelog entry written during the agent session.\n' >> "$1/CHANGES.rst"
  printf 'my notes, written while the agent was working\n' > "$1/human-notes.md"
}
human "$root/expected"   # expected end state = pre-session + human edits

echo "== running the agent under a VersionFS session (human edits after ${human_delay}s)"
t0=$(now)
(cd "$project" && "$ctl" --source "$root/source" run --label agent -- bash -c "$agent_cmd") \
  >"$root/transcript.txt" 2>&1 &
agent=$!
for _ in $(seq 1 $((human_delay * 10))); do kill -0 "$agent" 2>/dev/null || break; sleep 0.1; done
human "$project"
human_during=$(kill -0 "$agent" 2>/dev/null && echo yes || echo "no (agent finished first)")
agent_status=0; wait "$agent" || agent_status=$?
agent_s=$(elapsed "$t0" "$(now)")
sleep 1

changed_vs_expected=$(diff -rq --no-dereference "$root/expected" "$project" 2>/dev/null | wc -l || true)
changes=$("$ctl" --source "$root/source" changes agent)
paths=$(($(wc -l <<<"$changes") - 1))
events=$("$ctl" --source "$root/source" log --session agent --limit 10000000 | tail -n +2)
processes=$(awk -F'\t' '{split($2, a, ":"); sub(/\(.*/, "", a[2]); print a[2]}' <<<"$events" | sort | uniq -c | sort -rn | head -15)
agent_commits=$(git -C "$project" log --oneline agent-work 2>/dev/null | head -5 || echo "(no agent-work branch)")
sample=$(grep -v '^session ' <<<"$changes" | grep -v '\.venv/\|__pycache__\|\.git/' | head -25)

echo "== undoing the session"
plan=$("$ctl" --source "$root/source" undo agent --dry-run)
t0=$(now)
undo=$("$ctl" --source "$root/source" undo agent)
undo_s=$(elapsed "$t0" "$(now)")
conflicts=$(grep '^conflict:' <<<"$undo" || true)

# Files reported as conflicts are intentionally left as they are; compare everything else.
excluded=$(sed -n 's|^conflict: '"$root"'/mount/project/\([^:]*\):.*|\1|p; s|^conflict: /project/\([^:]*\):.*|\1|p' <<<"$undo" | sort -u)
diff_out=$(diff -r --no-dereference "$root/expected" "$project" 2>&1 || true)
for f in $excluded; do diff_out=$(grep -v "$f" <<<"$diff_out" || true); done
restored=$([[ -z "$diff_out" ]] && echo yes || echo no)
human_kept=$([[ -f "$project/human-notes.md" ]] && grep -q 'Human changelog entry' "$project/CHANGES.rst" && echo yes || echo no)
branch_gone=$(git -C "$project" rev-parse -q --verify refs/heads/agent-work >/dev/null && echo no || echo yes)

result=$(cat <<EOF
## Real-agent trial

Agent: \`${agent_cmd}\`$([[ -z "${AGENT_CMD:-}" ]] && echo " ($(claude --version 2>/dev/null))"), exit status $agent_status.
Project: \`$repo_url\` at \`$repo_ref\`. Run as $(id -un) (uid $(id -u)), kernel $(uname -r).

Task given to the agent:

\`\`\`
$TASK
\`\`\`

| Measure | Result |
| --- | --- |
| Agent run time | ${agent_s} s |
| Human edited while the agent was running | $human_during |
| Paths the agent touched (VersionFS session) | $paths |
| Paths differing from expected before undo | $changed_vs_expected |
| Dry-run plan | \`$(tail -1 <<<"$plan")\` |
| Undo | \`$(tail -1 <<<"$undo")\` in ${undo_s} s |
| Tree equals pre-session + human edits (conflicts excluded) | **$restored** |
| Human's edits kept | **$human_kept** |
| Agent's git branch gone | **$branch_gone** |

Agent commits (before undo):

\`\`\`
$agent_commits
\`\`\`

What the agent did, per path (excluding .venv, caches and .git internals):

\`\`\`
$sample
\`\`\`

Processes the agent's changes were attributed to (events per process):

\`\`\`
$processes
\`\`\`

Conflicts:

\`\`\`
${conflicts:-none}
\`\`\`
EOF
)
echo "$result"
[[ -z "$diff_out" ]] || { echo; echo "remaining differences:"; head -40 <<<"$diff_out"; }
echo; echo "agent transcript (last 30 lines):"; tail -30 "$root/transcript.txt"
mkdir -p "$(dirname "$out")"
printf '%s\n\n<details><summary>Agent transcript</summary>\n\n```\n%s\n```\n</details>\n' "$result" "$(tail -200 "$root/transcript.txt")" > "$out"
echo; echo "report written to $out"
