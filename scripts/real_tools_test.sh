#!/usr/bin/env bash
# Real-tools session trial: real git, pip, npm, a C compiler, an atomic-save "editor", rm -rf and
# a background process all run inside one supervised session on a cloned real project, while a
# "human" edits files mid-session. The session is then undone and the whole tree (including .git)
# is compared with the expected result: pre-session state plus the human's edits.
#
# Usage: real_tools_test.sh /path/to/versionfs /path/to/versionfsctl [report.md]
# Needs network access once (clone + package downloads); installs inside the session are offline.
set -euo pipefail

daemon=${1:?usage: real_tools_test.sh /path/to/versionfs /path/to/versionfsctl [report.md]}
ctl=${2:?usage: real_tools_test.sh /path/to/versionfs /path/to/versionfsctl [report.md]}
report=${3:-}
repo_url=${REPO_URL:-https://github.com/pallets/click.git}
repo_ref=${REPO_REF:-8.1.7}
root=$(mktemp -d /tmp/versionfs-realtools-XXXXXX)
pid=""
cleanup() {
  cd /
  if [[ -n "$pid" ]]; then fusermount3 -u "$root/mount" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  rm -rf "$root"
}
trap cleanup EXIT
now() { date +%s.%N; }
elapsed() { python3 -c "print(f'{($2 - $1):.2f}')"; }

# Prefetch packages so the session's installs are offline and timing is not network-bound.
mkdir -p "$root/wheels" "$root/npmcache"
pip download -q -d "$root/wheels" requests==2.32.3
npm cache add --cache "$root/npmcache" lodash@4.17.21 >/dev/null 2>&1

# The "agent": real tools doing typical agent work. It pauses at a flag so the human can act.
AGENT='set -e
cd "$1"
python3 -m venv .venv
.venv/bin/pip install -q --no-index --find-links "$3" requests==2.32.3
printf "{\"name\":\"demo\",\"version\":\"1.0.0\",\"dependencies\":{\"lodash\":\"4.17.21\"}}\n" > package.json
npm install --offline --cache "$4" --no-audit --no-fund --silent
git checkout -q -b agent-work
sed -i "s/def echo(/def echo_message(/" src/click/utils.py
git -c user.email=agent@example.com -c user.name=agent commit -qam "agent: rename echo"
python3 - <<PY
import os
text = open("README.rst").read().replace("Click", "CLICK")
with open(".README.rst.tmp", "w") as f: f.write(text)
os.replace(".README.rst.tmp", "README.rst")   # editor-style atomic save
PY
touch "$2.ready"                                  # let the human act now ...
while [ ! -e "$2" ]; do sleep 0.05; done          # ... and wait until they have
rm -rf docs
printf "int main(void){return 0;}\n" > tool.c && cc -o tool tool.c
python3 -m compileall -q src >/dev/null
( setsid bash -c "sleep 0.5; printf late > late.log" & )   # outlives its parent
sleep 1.5'

human() {  # human <project> <flag>: edit a file the agent also changed, edit another, add a new one
  [[ -n "${2:-}" ]] && while [ ! -e "$2.ready" ]; do sleep 0.05; done
  printf 'Human edit to the README.\n' > "$1/README.rst"
  printf 'Human changelog entry.\n' >> "$1/CHANGES.rst"
  printf 'my notes\n' > "$1/human-notes.md"
  [[ -n "${2:-}" ]] && touch "$2"
  return 0
}

git_view() {  # what git itself reports about a repository
  git -C "$1" rev-parse HEAD
  git -C "$1" for-each-ref --format='%(refname) %(objectname)'
  git -C "$1" status --porcelain=v1 --untracked-files=all
}

tree_listing() {  # names, types and modes of everything (including .git)
  (cd "$1" && find . -printf '%y %m %p\n' | sort)
}

# Direct run on a plain directory (timing reference).
git clone -q --depth 1 -b "$repo_ref" "$repo_url" "$root/pristine" 2>/dev/null
cp -a "$root/pristine" "$root/direct"
t0=$(now)
bash -c "$AGENT" _ "$root/direct" "$root/direct.flag" "$root/wheels" "$root/npmcache" &
agent=$!; human "$root/direct" "$root/direct.flag"; wait "$agent"
direct_s=$(elapsed "$t0" "$(now)")

# VersionFS run.
mkdir -p "$root/source" "$root/mount"
"$daemon" mount --source "$root/source" --mount "$root/mount" --foreground 2>/dev/null &
pid=$!
for _ in $(seq 1 50); do [[ -S "$root/source/.versionfs/control.sock" ]] && break; sleep 0.1; done
project="$root/mount/project"
git clone -q --depth 1 -b "$repo_ref" "$repo_url" "$project" 2>/dev/null
cp -a "$project" "$root/expected"
human "$root/expected" ""          # expected end state = pre-session + the human's edits
files_before=$(find "$project" | wc -l)

t0=$(now)
"$ctl" --source "$root/source" run --label tools -- \
  bash -c "$AGENT" _ "$project" "$root/vfs.flag" "$root/wheels" "$root/npmcache" 2>/dev/null &
agent=$!; human "$project" "$root/vfs.flag"; wait "$agent"
vfs_s=$(elapsed "$t0" "$(now)")
sleep 1   # let the background writer finish
files_during=$(find "$project" | wc -l)

changes=$("$ctl" --source "$root/source" changes tools)
paths=$(($(wc -l <<<"$changes") - 1))
events=$("$ctl" --source "$root/source" log --session tools --limit 10000000 | tail -n +2)
processes=$(awk -F'\t' '{split($2, a, ":"); sub(/\(.*/, "", a[2]); print a[2]}' <<<"$events" | sort | uniq -c | sort -rn)
grep -q '/project/late.log' <<<"$changes"     # background process attributed (subreaper)

plan=$("$ctl" --source "$root/source" undo tools --dry-run)
t0=$(now)
undo=$("$ctl" --source "$root/source" undo tools)
undo_s=$(elapsed "$t0" "$(now)")
summary_plan=$(tail -1 <<<"$plan"); summary_undo=$(tail -1 <<<"$undo")

# Verdict: contents, names, types and modes must equal the expected tree.
content_ok=yes; listing_ok=yes; git_ok=yes
diff -r --no-dereference "$root/expected" "$project" >"$root/diff.txt" 2>&1 || content_ok=no
diff <(tree_listing "$root/expected") <(tree_listing "$project") >"$root/listing.txt" || listing_ok=no
diff <(git_view "$root/expected") <(git_view "$project") >"$root/git.txt" || git_ok=no
git -C "$project" rev-parse -q --verify refs/heads/agent-work >/dev/null && git_ok=no
conflicts=$(grep '^conflict:' <<<"$undo" || true)
status=$("$ctl" --source "$root/source" status)
stored=$(awk -F'\t' '$1 == "stored-bytes" {printf "%.1f MiB", $2 / 1048576}' <<<"$status")

result=$(cat <<EOF
## Real-tools session trial

Project: \`$repo_url\` at \`$repo_ref\` ($files_before paths before the session, $files_during after).
Run as uid $(id -u) ($(id -un)), kernel $(uname -r), libfuse $(pkg-config --modversion fuse3 2>/dev/null || echo ?).

Inside one \`versionfsctl run\` session: \`python3 -m venv\` + \`pip install requests\`, \`npm install lodash\`,
\`git checkout -b\` + \`sed -i\` + \`git commit\`, an atomic-save README rewrite, \`rm -rf docs\`, a \`cc\` compile,
\`python -m compileall\`, and a double-forked background writer. Mid-session a human rewrote the README
(which the agent had also changed), appended to CHANGES.rst and created human-notes.md.

| Measure | Result |
| --- | --- |
| Session wall time, plain directory | ${direct_s} s |
| Session wall time, VersionFS | ${vfs_s} s |
| Paths touched by the session | $paths |
| Provenance events recorded | $(wc -l <<<"$events") |
| Snapshot storage after session + undo | $stored |
| Dry-run plan | \`$summary_plan\` |
| Undo | \`$summary_undo\` in ${undo_s} s |
| Tree contents equal expected (incl. \`.git\`) | **$content_ok** |
| Names, types and modes equal expected | **$listing_ok** |
| git reports the same HEAD, refs and status; agent branch gone | **$git_ok** |
| Background (double-forked) writer attributed and undone | **yes** |

Conflicts reported (human edits kept):

\`\`\`
${conflicts:-none}
\`\`\`

Processes the session's changes were attributed to (events per process):

\`\`\`
$processes
\`\`\`
EOF
)
echo "$result"
if [[ "$content_ok" != yes || "$listing_ok" != yes ]]; then
  echo; echo "content diff:"; head -40 "$root/diff.txt"; echo "listing diff:"; head -40 "$root/listing.txt"
fi
[[ "$git_ok" == yes ]] || { echo "git view diff:"; head -40 "$root/git.txt"; }
[[ -n "$report" ]] && printf '%s\n' "$result" > "$report"
[[ "$content_ok" == yes && "$listing_ok" == yes && "$git_ok" == yes ]]
