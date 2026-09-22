#!/usr/bin/env bash
# [A165] donor issue #98: identity, source, hash and scope receipts.
# Run from the lane clone. Read-only: nothing here edits the clone.
set -u
BASE=8452f564294300a82d56eed464276576f65f4d58
HEAD_EXPECTED=88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf
DOC=docs/architecture/10_srp_engine.md
run() { printf '\n$ %s\n' "$*"; "$@"; printf '# exit: %s\n' "$?"; }

echo "## 1. identity"
run git rev-parse HEAD HEAD^{tree} "$BASE" "$BASE^{tree}"
run git rev-parse HEAD^
run git status --porcelain=v2 --branch
run git ls-remote origin refs/heads/main refs/heads/98-clarify-srp-domain-events
run git rev-list --count "$BASE..HEAD"
run git log --format='%H %P%n  tree %T%n  author %an <%ae> %aI%n  committer %cn <%ce> %cI' "$BASE..HEAD"
[ "$(git rev-parse HEAD)" = "$HEAD_EXPECTED" ] && echo "HEAD matches expected $HEAD_EXPECTED" || echo "HEAD DIFFERS from expected $HEAD_EXPECTED"

echo; echo "## 2. commit message: one line, no trailers"
run git log -1 --format=%B HEAD
printf 'message lines (non-empty): '; git log -1 --format=%B HEAD | grep -c .
printf 'trailers parsed: ['; git log -1 --format=%B HEAD | git interpret-trailers --parse | tr '\n' ';'; echo ']'

echo; echo "## 3. changed paths, modes and blobs"
run git diff --name-status "$BASE" HEAD
run git diff --numstat "$BASE" HEAD
run git diff --summary "$BASE" HEAD
run git diff-tree -r "$BASE" HEAD
run git diff --check "$BASE" HEAD
run git rev-parse "$BASE:$DOC" "HEAD:$DOC"
printf 'sha256 base %s: ' "$DOC"; git show "$BASE:$DOC" | sha256sum
printf 'sha256 head %s: ' "$DOC"; git show "HEAD:$DOC" | sha256sum

echo; echo "## 4. the whole change (one hunk, line 205 only)"
run git diff -U0 "$BASE" HEAD
printf 'added lines with U+2013/U+2014: '; git diff -U0 "$BASE" HEAD | grep -P '^\+(?!\+\+)' | grep -cP '[\x{2013}\x{2014}]'
printf 'added lines with any non-ASCII: '; git diff -U0 "$BASE" HEAD | grep -P '^\+(?!\+\+)' | grep -cP '[^\x00-\x7F]'

echo; echo "## 5. F10.2 fence at head (file lines 198-207)"
git show "HEAD:$DOC" | awk 'NR>=198 && NR<=207 {printf "%4d  %s\n", NR, $0}'

echo; echo "## 6. unchanged scope: each path set compared base..head (exit 0 = identical)"
for p in hdl tb syn scripts Makefile .github .gitignore README.md IEEE_1722_1_Hardware_Protocol_Processor.md \
         docs/diagrams docs/README.md docs/guides docs/traceability docs/00_MILAN_COMPLIANCE_REVIEW.md \
         docs/10_RESOURCE_AND_EFFORT.md docs/architecture/01_overview.md docs/architecture/02_interfaces.md \
         docs/architecture/03_packet_engine.md docs/architecture/04_adp_engine.md docs/architecture/05_acmp_engine.md \
         docs/architecture/06_aecp_engine.md docs/architecture/07_memory_maps.md docs/architecture/08_timing.md \
         docs/architecture/09_verification.md docs/architecture/11_maap_engine.md; do
  git diff --quiet "$BASE" HEAD -- "$p"; printf '  %-48s exit %s\n' "$p" "$?"
done
printf 'tracked file list identical base/head: '
if diff <(git ls-tree -r --name-only "$BASE") <(git ls-tree -r --name-only HEAD) >/dev/null; then echo yes; else echo NO; fi

echo; echo "## 7. generated/exported assets (none represents F10.2; all unchanged)"
run git diff --stat "$BASE" HEAD -- docs/diagrams
printf 'committed diagram assets at head: '; git ls-tree -r --name-only HEAD -- docs/diagrams | grep -cE '\.(svg|png|drawio)$'
printf 'assets containing F10.2/Domain FSM terms (fig-10-domsm|F10\\.2|LINK_DOWN|LINK_UP|DEFAULTS|ADOPTED|back to defaults|DOMAIN_CHANGE): '
n=0; for f in $(git ls-tree -r --name-only HEAD -- docs/diagrams | grep -E '\.(svg|drawio)$'); do
  if git show "HEAD:$f" | grep -qE 'fig-10-domsm|F10\.2|LINK_DOWN|LINK_UP|DEFAULTS|ADOPTED|back to defaults|DOMAIN_CHANGE'; then echo "HIT $f"; n=$((n+1)); fi
done; echo "$n"
printf 'PNG assets whose strings contain those terms: '
n=0; for f in $(git ls-tree -r --name-only HEAD -- docs/diagrams | grep -E '\.png$'); do
  if git show "HEAD:$f" | strings | grep -qE 'fig-10-domsm|F10\.2|LINK_DOWN|back to defaults'; then echo "HIT $f"; n=$((n+1)); fi
done; echo "$n"
run git grep -n -E 'fig-10-domsm|LINK_DOWN then LINK_UP|back to defaults' HEAD

echo; echo "## 8. RTL and evidence references (unchanged blobs, head line text)"
for f in hdl/srp/KL_srp_domain.sv tb/pp_top/sim_main.cpp tb/pp_top/README.md tb/srp_encoder/sim_main.cpp tb/srp_encoder/README.md \
         docs/architecture/01_overview.md docs/guides/integrator.md; do
  printf '  %-34s base %s head %s\n' "$f" "$(git rev-parse "$BASE:$f")" "$(git rev-parse "HEAD:$f")"
done
echo "-- hdl/srp/KL_srp_domain.sv:109-110 (edge detects), :113-119 (reset), :154-171 (LINK_DOWN / LINK_UP branches)"
git show HEAD:hdl/srp/KL_srp_domain.sv | awk '(NR>=109 && NR<=110) || (NR>=113 && NR<=119) || (NR>=154 && NR<=171) {printf "%4d  %s\n", NR, $0}'
echo "-- tb/pp_top/sim_main.cpp:8369-8400 (DV5, DV6)"
git show HEAD:tb/pp_top/sim_main.cpp | awk 'NR>=8369 && NR<=8400 {printf "%5d  %s\n", NR, $0}'
echo "-- tb/pp_top/README.md:535-537"
git show HEAD:tb/pp_top/README.md | awk 'NR>=535 && NR<=537 {printf "%4d  %s\n", NR, $0}'
echo "-- tb/srp_encoder/sim_main.cpp:691-707 (D9)"
git show HEAD:tb/srp_encoder/sim_main.cpp | awk 'NR>=691 && NR<=707 {printf "%4d  %s\n", NR, $0}'
echo "-- consistent prose already at base: 10:48, 10:366-368, 01:166, integrator:66, integrator:260, 00:422"
git show "HEAD:$DOC" | awk 'NR==48 || (NR>=366 && NR<=368) {printf "  10:%d  %s\n", NR, $0}'
git show HEAD:docs/architecture/01_overview.md | awk 'NR==166 {printf "  01:%d  %s\n", NR, $0}'
git show HEAD:docs/guides/integrator.md | awk 'NR==66 || NR==260 {printf "  integrator:%d  %s\n", NR, $0}'
git show HEAD:docs/00_MILAN_COMPLIANCE_REVIEW.md | awk 'NR==422 {printf "  00:%d  %s\n", NR, $0}'
echo "-- observed, unchanged: tb/srp_encoder/README.md:75-77 quotes the pre-change label"
git show HEAD:tb/srp_encoder/README.md | awk 'NR>=75 && NR<=77 {printf "%4d  %s\n", NR, $0}'
