#!/usr/bin/env bash
# The new fixture-guards-test regression: caller independence, no real process
# creation, mutant sensitivity, equivalent-refactor tolerance, real-gate
# consequence of each defect mutant, and failure propagation through `make`.
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
H="$S/head/tb/pp_top"
for k in EN DE_LC_ALL MIXED; do
  $RUN --name "06a-regression-$k" --cwd "$H" ${SETTING[$k]} --expect-rc 0 -- make -j1 fixture-guards-test
done
# No compiler, Verilator or other tool reachable on PATH; python3 by absolute path.
$RUN --name 06b-regression-audit-noPATH --cwd "$H" ${SETTING[MIXED]} --set PATH=/nonexistent \
  --expect-rc 0 -- /usr/bin/python3 -B "$RC/scripts/06-audit-unittest.py"
# Mutants: each in its own directory holding the mutant gate and the head test.
M="$S/regression-mutants"; rm -rf "$M"; mkdir -p "$M"
: > "$RC/06c-mutants.diff"
for n in N1-no-env-argument N2-no-lc-all-override N3-lang-instead-of-lc-all \
         N4-lc-messages-instead-of-lc-all N5-drop-inherited-environment \
         N6-mutate-caller-environment N7-verilator-also-normalised \
         N8-global-lc-all-before-verilator E1-dict-merge E2-dict-kwargs C1-lenient-verdict; do
  mkdir -p "$M/$n"; cp "$H/test_fixture_guards.py" "$M/$n/"
  { echo "### $n"; python3 "$RC/scripts/06-mutants.py" "$n" "$H/fixture_guards.py" "$M/$n/fixture_guards.py"; } >> "$RC/06c-mutants.diff"
  exp=1; case "$n" in E*|C1*) exp=0;; esac
  $RUN --name "06c-regression-$n" --cwd "$M/$n" --expect-rc $exp -- python3 -B -m unittest -v test_fixture_guards.py
done
# Real-gate consequence of the defect mutants under an inherited German LC_ALL,
# bypassing the regression (gate command exactly as make builds it).
T="$S/mutant-tree"; rm -rf "$T"; mkdir -p "$T"
git -C $VALIDATION_STORAGE/reviews/r230-100-r2 archive 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e | tar -x -C "$T"
orig=$(sha256sum < "$T/tb/pp_top/fixture_guards.py")
$RUN --name "06d-gate-direct-head-DE_LC_ALL" --cwd "$T/tb/pp_top" ${SETTING[DE_LC_ALL]} \
  --expect-rc 0 -- bash "$RC/scripts/gate-direct.sh" "$V8"
for n in N1-no-env-argument N2-no-lc-all-override N3-lang-instead-of-lc-all N4-lc-messages-instead-of-lc-all; do
  cp "$M/$n/fixture_guards.py" "$T/tb/pp_top/fixture_guards.py"
  $RUN --name "06d-gate-direct-$n-DE_LC_ALL" --cwd "$T/tb/pp_top" ${SETTING[DE_LC_ALL]} \
    --expect-rc 1 -- bash "$RC/scripts/gate-direct.sh" "$V8"
done
# The regression failing stops the default target before the gate and both builds.
cp "$M/N2-no-lc-all-override/fixture_guards.py" "$T/tb/pp_top/fixture_guards.py"
$RUN --name "06e-make-default-target-N2-j8" --cwd "$T/tb/pp_top" ${SETTING[DE_LANGUAGE]} \
  --set R230_VLOG="$RC/verilator-invocations.jsonl" --expect-rc 2 -- make -j8 VERILATOR="$V8"
echo "after 06e: obj_dir exists=$([ -e "$T/tb/pp_top/obj_dir" ] && echo yes || echo no) obj_vid exists=$([ -e "$T/tb/pp_top/obj_vid" ] && echo yes || echo no)"
git -C $VALIDATION_STORAGE/reviews/r230-100-r2 show 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e:tb/pp_top/fixture_guards.py > "$T/tb/pp_top/fixture_guards.py"
rm -f "$T/tb/pp_top/ltn_rom.hex" "$T/tb/pp_top/ucode.hex"
[ "$(sha256sum < "$T/tb/pp_top/fixture_guards.py")" = "$orig" ] && echo "mutant tree gate restored to head bytes"
echo "== summary"
for f in "$RC"/06*.json; do
  python3 -c "import json,sys; r=json.load(open(sys.argv[1])); print(f\"{r['name']:<52} rc={r['rc']} expected={r['expected_rc']} as_expected={r['as_expected']}\")" "$f"
done
echo "== audit-hook result"; grep -E 'tests run|RESULT' "$RC/06b-regression-audit-noPATH.log"
echo "== failing assertion per N mutant"
for n in N1 N2 N3 N4 N5 N6 N7 N8; do
  f=$(ls "$RC"/06c-regression-$n-*.log); echo "-- $(basename "$f" .log)"
  grep -E '^(FAIL|ERROR):|AssertionError|Error:' "$f" | head -4 | cut -c1-200 | sed 's/^/   /'
  grep -E '^(FAILED|OK)' "$f" | sed 's/^/   /'
done
echo "== real gate verdict lines (06d)"
for f in "$RC"/06d-*.log; do echo "-- $(basename "$f" .log)"; grep -E '^(FAIL:|fixture guards:)|error:' "$f" | head -3 | cut -c1-180 | sed 's/^/   /'; done
echo "== 06e tail"; tail -4 "$RC/06e-make-default-target-N2-j8.log" | cut -c1-200
