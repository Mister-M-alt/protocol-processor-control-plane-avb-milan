#!/usr/bin/env bash
# [A165] donor issue #98 follow-up: identity, source, hash and scope receipts.
# Run from the lane clone. Read-only: nothing here edits the clone or the original packet.
set -u
BASE=8452f564294300a82d56eed464276576f65f4d58      # main
OLD=88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf       # figure commit (first round)
HEAD_EXPECTED=bc997e7c00e50e3d59b97987950bfbf6cc442182
DOC=docs/architecture/10_srp_engine.md
NOTE=tb/srp_encoder/README.md
ORIG=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor98-author
run() { printf '\n$ %s\n' "$*"; "$@"; printf '# exit: %s\n' "$?"; }
fence() { git show "$1:$DOC" | awk '/<a id="fig-10-domsm"><\/a>/{a=1} a && /^```mermaid$/{f=1; next} f && /^```$/{exit} f{print}'; }

echo "## 1. identity"
run git rev-parse HEAD HEAD^{tree} "$OLD" "$OLD^{tree}" "$BASE" "$BASE^{tree}"
run git rev-parse HEAD^
run git status --porcelain=v2 --branch
run git rev-list --count "$OLD..HEAD"
run git rev-list --count "$BASE..HEAD"
run git log --format='%H %P%n  tree %T%n  author %an <%ae> %aI%n  committer %cn <%ce> %cI%n  subject %s' "$BASE..HEAD"
[ "$(git rev-parse HEAD)" = "$HEAD_EXPECTED" ] && echo "HEAD matches expected $HEAD_EXPECTED" || echo "HEAD DIFFERS from expected $HEAD_EXPECTED"
[ "$(git rev-parse HEAD^)" = "$OLD" ] && echo "HEAD^ is the figure commit $OLD" || echo "HEAD^ DIFFERS from $OLD"

echo; echo "## 2. commit message: one line, no trailers"
run git log -1 --format=%B HEAD
printf 'message lines (non-empty): '; git log -1 --format=%B HEAD | grep -c .
printf 'trailers parsed: ['; git log -1 --format=%B HEAD | git interpret-trailers --parse | tr '\n' ';'; echo ']'

echo; echo "## 3. changed paths, modes and blobs ($OLD..HEAD), and diff --check"
run git diff --name-status "$OLD" HEAD
run git diff --numstat "$OLD" HEAD
run git diff --summary "$OLD" HEAD
run git diff-tree -r "$OLD" HEAD
run git diff --check "$OLD" HEAD
run git rev-parse "$OLD:$NOTE" "HEAD:$NOTE"
printf 'sha256 old  %s: ' "$NOTE"; git show "$OLD:$NOTE" | sha256sum
printf 'sha256 head %s: ' "$NOTE"; git show "HEAD:$NOTE" | sha256sum
echo "-- combined head against main (what full validation and review will see)"
run git diff --name-status "$BASE" HEAD
run git diff --numstat "$BASE" HEAD
run git diff --check "$BASE" HEAD

echo; echo "## 4. the whole change (one hunk, $NOTE:75 only)"
run git diff -U0 "$OLD" HEAD
printf 'added lines with U+2013/U+2014: '; git diff -U0 "$OLD" HEAD | grep -P '^\+(?!\+\+)' | grep -cP '[\x{2013}\x{2014}]'
printf 'added lines with any non-ASCII: '; git diff -U0 "$OLD" HEAD | grep -P '^\+(?!\+\+)' | grep -cP '[^\x00-\x7F]'
printf 'added line length: '; git show "HEAD:$NOTE" | awk 'NR==75 {print length($0)}'
echo "-- note at $OLD ($NOTE:69-77)"
git show "$OLD:$NOTE" | awk 'NR>=69 && NR<=77 {printf "%4d  %s\n", NR, $0}'
echo "-- note at HEAD ($NOTE:69-77)"
git show "HEAD:$NOTE" | awk 'NR>=69 && NR<=77 {printf "%4d  %s\n", NR, $0}'
printf 'interpretation lines :76-77 byte-identical old/head: '
if cmp -s <(git show "$OLD:$NOTE" | sed -n '76,77p') <(git show "HEAD:$NOTE" | sed -n '76,77p'); then echo yes; else echo NO; fi
printf 'every line except :75 byte-identical old/head: '
if cmp -s <(git show "$OLD:$NOTE" | sed '75d') <(git show "HEAD:$NOTE" | sed '75d'); then echo yes; else echo NO; fi

echo; echo "## 5. stale quotation gone; the new label is not duplicated"
echo "-- the old label anywhere in the tree (git grep exit 1 = no match)"
run git grep -n -F 'LINK_DOWN then LINK_UP' "$OLD"
run git grep -n -F 'LINK_DOWN then LINK_UP' HEAD
run git grep -n -F 'back to defaults' HEAD
echo "-- the new label's literal text: its only home is the F10.2 fence"
run git grep -n -F 'restore defaults, declared again on the next LINK_UP' HEAD
run git grep -n -F 'declared again on the next' HEAD
echo "-- the note no longer says the edge 'reads' a label"
run git grep -n -E 'revert edge reads' HEAD

echo; echo "## 6. F10.2 source unchanged since the rendered commit"
run git rev-parse "$BASE:$DOC" "$OLD:$DOC" "HEAD:$DOC"
printf 'sha256 old  %s: ' "$DOC"; git show "$OLD:$DOC" | sha256sum
printf 'sha256 head %s: ' "$DOC"; git show "HEAD:$DOC" | sha256sum
run git diff --quiet "$OLD" HEAD -- "$DOC"
echo "-- Mermaid fence extracted by anchor fig-10-domsm (fence body, as rendered)"
printf 'fence sha256 base %s: ' "${BASE:0:7}"; fence "$BASE" | sha256sum
printf 'fence sha256 old  %s: ' "${OLD:0:7}"; fence "$OLD" | sha256sum
printf 'fence sha256 head %s: ' "$(git rev-parse --short=7 HEAD)"; fence HEAD | sha256sum
printf 'head fence byte-identical to the rendered %s: ' "render/F10.2-head-88a4eb4.mmd"
if cmp -s <(fence HEAD) "$ORIG/render/F10.2-head-88a4eb4.mmd"; then echo yes; else echo NO; fi
printf 'base fence byte-identical to the rendered %s: ' "render/F10.2-base-8452f56.mmd"
if cmp -s <(fence "$BASE") "$ORIG/render/F10.2-base-8452f56.mmd"; then echo yes; else echo NO; fi
echo "-- the original packet's recorded hashes for the render files (its SHA256SUMS)"
grep -E ' render/' "$ORIG/SHA256SUMS"
echo "-- F10.2 at HEAD ($DOC:198-207)"
git show "HEAD:$DOC" | awk 'NR>=198 && NR<=207 {printf "%4d  %s\n", NR, $0}'

echo; echo "## 7. renderer, master and executable bytes identical to $OLD (exit 0 = identical)"
run git diff --quiet "$OLD" HEAD -- . ":(exclude)$NOTE"
run git diff --quiet "$OLD" HEAD -- ':(exclude)*.md'
printf 'mode+blob of every tracked path except the note identical old/head: '
if diff <(git ls-tree -r "$OLD" | grep -v -P "\t\Q$NOTE\E\$") <(git ls-tree -r HEAD | grep -v -P "\t\Q$NOTE\E\$") >/dev/null; then echo yes; else echo NO; fi
printf 'tracked file list identical old/head: '
if diff <(git ls-tree -r --name-only "$OLD") <(git ls-tree -r --name-only HEAD) >/dev/null; then echo yes; else echo NO; fi
printf 'tracked paths at head: '; git ls-tree -r --name-only HEAD | wc -l
echo "-- path sets"
for p in hdl syn scripts Makefile .github .gitignore README.md IEEE_1722_1_Hardware_Protocol_Processor.md docs \
         docs/diagrams docs/diagrams/src docs/diagrams/wavedrom "$DOC"; do
  git diff --quiet "$OLD" HEAD -- "$p"; printf '  %-48s exit %s\n' "$p" "$?"
done
git diff --quiet "$OLD" HEAD -- tb ":(exclude)$NOTE"; printf '  %-48s exit %s\n' "tb except $NOTE" "$?"
echo "-- renderer and gate scripts: mode and blob at old and head"
for f in Makefile scripts/lint-diagrams.sh scripts/render-wavedrom.py scripts/check-links.py scripts/check-matrix.py scripts/gen_matrix.py; do
  o=$(git ls-tree "$OLD" -- "$f" | awk '{print $1, $3}'); h=$(git ls-tree HEAD -- "$f" | awk '{print $1, $3}')
  [ "$o" = "$h" ] && s=same || s=DIFFERENT; printf '  %-30s old %s  head %s  %s\n' "$f" "$o" "$h" "$s"
done
echo "-- every executable (100755) file: blob at old and head"
git ls-tree -r HEAD | awk '$1=="100755" {print $4}' | while read -r f; do
  o=$(git rev-parse "$OLD:$f"); h=$(git rev-parse "HEAD:$f")
  [ "$o" = "$h" ] && s=same || s=DIFFERENT; printf '  %-30s %s %s  %s\n' "$f" "$o" "$h" "$s"
done
printf 'executable files at old/head: %s/%s\n' "$(git ls-tree -r "$OLD" | awk '$1=="100755"' | wc -l)" "$(git ls-tree -r HEAD | awk '$1=="100755"' | wc -l)"
printf 'mode of the note at old/head: %s/%s\n' "$(git ls-tree "$OLD" -- "$NOTE" | awk '{print $1}')" "$(git ls-tree HEAD -- "$NOTE" | awk '{print $1}')"

echo; echo "## 8. generated/exported assets"
run git diff --stat "$OLD" HEAD -- docs/diagrams
printf 'committed diagram assets at head: '; git ls-tree -r --name-only HEAD -- docs/diagrams | grep -cE '\.(svg|png|drawio)$'

echo; echo "## 9. RTL and evidence behind the note (unchanged blobs, head line text)"
for f in hdl/srp/KL_srp_domain.sv tb/srp_encoder/sim_main.cpp tb/pp_top/sim_main.cpp; do
  printf '  %-30s old %s head %s\n' "$f" "$(git rev-parse "$OLD:$f")" "$(git rev-parse "HEAD:$f")"
done
echo "-- hdl/srp/KL_srp_domain.sv:109-110 (edge detects), :154-171 (LINK_DOWN / LINK_UP branches)"
git show HEAD:hdl/srp/KL_srp_domain.sv | awk '(NR>=109 && NR<=110) || (NR>=154 && NR<=171) {printf "%4d  %s\n", NR, $0}'
echo "-- tb/srp_encoder/sim_main.cpp:691-707 (D9)"
git show HEAD:tb/srp_encoder/sim_main.cpp | awk 'NR>=691 && NR<=707 {printf "%4d  %s\n", NR, $0}'
echo "-- the same suite's own summary of D9 ($NOTE:42-43)"
git show "HEAD:$NOTE" | awk 'NR>=42 && NR<=43 {printf "%4d  %s\n", NR, $0}'

echo; echo "## 10. remote: nothing pushed"
run git ls-remote origin refs/heads/main refs/heads/98-clarify-srp-domain-events

echo; echo "## 11. original packet intact (its own SHA256SUMS, absolute paths)"
sed "s|  |  $ORIG/|" "$ORIG/SHA256SUMS" | sha256sum -c --strict --quiet; printf '# exit: %s\n' "$?"
printf 'files listed: '; wc -l < "$ORIG/SHA256SUMS"
