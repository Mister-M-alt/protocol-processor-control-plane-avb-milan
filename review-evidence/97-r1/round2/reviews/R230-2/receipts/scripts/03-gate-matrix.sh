#!/usr/bin/env bash
# The real `make fixture-guards` at the prior head (1eb20dc, export) and at the
# exact head (5c45845, clone) under every caller setting. Serial outer make;
# Verilator through verilator8 (all job controls pinned at 8).
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
export_vlog="--set R230_VLOG=$RC/verilator-invocations.jsonl"
for k in $ORDER; do
  exp_prior=2; [ "$k" = EN ] && exp_prior=0
  $RUN --name "03-prior-$k" --cwd "$S/prior/tb/pp_top" ${SETTING[$k]} $export_vlog \
    --expect-rc $exp_prior -- make -j1 VERILATOR="$V8" fixture-guards
  $RUN --name "03-head-$k" --cwd "$S/head/tb/pp_top" ${SETTING[$k]} $export_vlog \
    --expect-rc 0 -- make -j1 VERILATOR="$V8" fixture-guards
done
echo "== per-run outcome lines"
for k in $ORDER; do
  for w in prior head; do
    f="$RC/03-$w-$k.log"
    echo "-- 03-$w-$k rc=$(python3 -c "import json;print(json.load(open('${f%.log}.json'))['rc'])")"
    grep -E '^(fixture guard|fixture guards:|FAIL:)|Ran [0-9]+ test|^OK$|error:' "$f" | sed 's/^/   /'
  done
done
echo "== leftover gate temp dirs in scratch TMPDIR:"; ls -d "$S"/tmp/pp-top-vid-guards-* 2>/dev/null || echo "   none"
echo "== head clone status --porcelain --ignored:"; git -C "$S/head" status --porcelain --ignored | sed 's/^/   /'; echo "   (end)"
