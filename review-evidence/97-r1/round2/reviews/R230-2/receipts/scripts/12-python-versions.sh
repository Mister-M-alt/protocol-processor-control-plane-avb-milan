#!/usr/bin/env bash
# The exact-head regression and gate under the other CPython versions already
# present on this host (uv-managed 3.11 and 3.12; system 3.14 is used elsewhere),
# with a German inherited LC_ALL. Nothing is installed.
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
for v in 3.11.15 3.12.13; do
  b="$HOME/.local/share/uv/python/cpython-$v-linux-x86_64-gnu/bin"
  $RUN --name "12-gate-python$v-DE_LC_ALL" --cwd "$S/head/tb/pp_top" ${SETTING[DE_LC_ALL]} \
    --set PATH="$b:/usr/local/bin:/usr/bin:/bin" --set R230_VLOG="$RC/verilator-invocations.jsonl" \
    --expect-rc 0 -- sh -c "python3 --version; make -j1 VERILATOR=$V8 fixture-guards"
  grep -E '^Python|^Ran|^OK|fixture guards:' "$RC/12-gate-python$v-DE_LC_ALL.log" | sed 's/^/   /'
done
echo "== runtime floor already set by pp_top make prerequisites (no __future__ annotations):"
grep -n "from __future__" "$S/head/hdl/aecp/ucode/gen_ucode.py" || echo "   gen_ucode.py: no __future__ import"
grep -n "def place(at: int, words: list\[int\])" "$S/head/hdl/aecp/ucode/gen_ucode.py"
echo "== features in the new test: parenthesized with (CPython PEG parser; documented 3.10), mock call.args/.kwargs (3.8)"
grep -n -E "with \(|\.args\[|\.kwargs" "$S/head/tb/pp_top/test_fixture_guards.py"
