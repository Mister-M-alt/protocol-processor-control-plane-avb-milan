#!/usr/bin/env bash
# sim_main.cpp guard/unrelated-error mutants through the full `make fixture-guards`
# (regression + gate) at the exact head, under localized caller settings.
# Sequential, one disposable export; sim_main.cpp restored after each mutant.
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
T="$S/mutant-tree"; P="$T/tb/pp_top"
git -C $VALIDATION_STORAGE/reviews/r230-100-r2 show 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e:tb/pp_top/sim_main.cpp > "$S/sim_main.head.cpp"
orig=$(sha256sum < "$S/sim_main.head.cpp")
[ "$(sha256sum < "$P/sim_main.cpp")" = "$orig" ] || { echo "mutant tree not at head bytes"; exit 1; }
[ "$(sha256sum < "$P/fixture_guards.py")" = "$(git -C $VALIDATION_STORAGE/reviews/r230-100-r2 show 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e:tb/pp_top/fixture_guards.py | sha256sum)" ] || { echo "gate not at head bytes"; exit 1; }
: > "$RC/07-sim-mutants.diff"
run_mutant() {  # name setting
  local n=$1 k=$2
  { echo "### $n"; python3 "$RC/scripts/07-sim-mutants.py" "$n" "$S/sim_main.head.cpp" "$P/sim_main.cpp"; } >> "$RC/07-sim-mutants.diff.tmp"
  $RUN --name "07-$n-$k" --cwd "$P" ${SETTING[$k]} --set R230_VLOG="$RC/verilator-invocations.jsonl" \
    --expect-rc 2 -- make -j1 VERILATOR="$V8" fixture-guards
  cp "$S/sim_main.head.cpp" "$P/sim_main.cpp"
}
: > "$RC/07-sim-mutants.diff.tmp"
for n in G1-wire-guard-removed G2-class-d-guard-removed G3-class-d-mask-widened \
         G4-diagnostics-swapped G5-guards-outside-fixture-branch G6-unrelated-error-all-fixtures \
         G7-unrelated-error-0002-alongside G8-unrelated-error-default-branch G9-extra-static-assertion-0002; do
  run_mutant "$n" DE_LC_ALL
done
for k in FR_LANG MIXED; do
  for n in G1-wire-guard-removed G2-class-d-guard-removed G7-unrelated-error-0002-alongside; do
    run_mutant "$n" "$k"
  done
done
# Keep one diff per mutant in the receipt.
python3 - "$RC/07-sim-mutants.diff.tmp" "$RC/07-sim-mutants.diff" <<'EOF'
import sys
blocks, seen, cur = [], set(), None
for line in open(sys.argv[1]):
    if line.startswith("### "):
        cur = line[4:].strip(); keep = cur not in seen; seen.add(cur)
    if keep:
        blocks.append(line)
open(sys.argv[2], "w").writelines(blocks)
EOF
rm -f "$RC/07-sim-mutants.diff.tmp"
[ "$(sha256sum < "$P/sim_main.cpp")" = "$orig" ] && echo "mutant tree sim_main.cpp restored to head bytes"
echo "== verdict per mutant (regression line, gate verdict line, compiler error lines)"
for f in "$RC"/07-G*.log; do
  j="${f%.log}.json"
  echo "-- $(basename "$f" .log) rc=$(python3 -c "import json;print(json.load(open('$j'))['rc'])")"
  grep -E '^OK$|^FAILED' "$f" | sed 's/^/   regression: /'
  grep -E '^(fixture guard|FAIL:)' "$f" | cut -c1-170 | sed 's/^/   /'
  grep -E 'error:' "$f" | grep -v '^make' | cut -c1-170 | sed 's/^/      /'
done
echo "== leftover gate temp dirs:"; ls -d "$S"/tmp/pp-top-vid-guards-* 2>/dev/null || echo "   none"
