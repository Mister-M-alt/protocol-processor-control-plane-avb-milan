#!/usr/bin/env bash
# Full default `make` (regression + gate + default and 5A3C builds) at the exact
# head under a German inherited LC_ALL and a French LANG. Serial outer make;
# Verilator through verilator8. Outputs are ignored files; `make clean` after.
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
H="$S/head"
echo "head clone status before: $(git -C "$H" status --porcelain --ignored | wc -l) lines"
for k in DE_LC_ALL FR_LANG; do
  $RUN --name "08-head-make-$k" --cwd "$H/tb/pp_top" ${SETTING[$k]} \
    --set R230_VLOG="$RC/verilator-invocations.jsonl" --expect-rc 0 -- make -j1 VERILATOR="$V8"
  cp "$H/tb/pp_top/obj_dir/build_tally.txt" "$RC/08-head-make-$k.build_tally.txt"
  git -C "$H" status --porcelain --ignored > "$RC/08-head-make-$k.status.txt"
  make -s -C "$H/tb/pp_top" clean
  echo "== $k"
  grep -E '^(Ran|OK|fixture guard|\[build|[0-9]+ checks:)|FAIL' "$RC/08-head-make-$k.log" | cut -c1-170 | sed 's/^/   /'
  echo "   build_tally.txt: $(tr '\n' ' ' < "$RC/08-head-make-$k.build_tally.txt")"
  echo "   last line: $(tail -1 "$RC/08-head-make-$k.log")"
  echo "   run_suites.sh tally regex: $(grep -Eo '[0-9]+ checks: [0-9]+ PASS, [0-9]+ FAIL' "$RC/08-head-make-$k.log" | tail -1)"
  echo "   status after make (before clean):"; sed 's/^/     /' "$RC/08-head-make-$k.status.txt"
done
echo "head clone status after clean: $(git -C "$H" status --porcelain --ignored | wc -l) lines"
