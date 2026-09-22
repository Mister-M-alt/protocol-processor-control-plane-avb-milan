#!/usr/bin/env bash
# Legacy (non-UTF-8) caller locales. German/French single-byte encodings: gate at
# prior and exact head. EUC-JP as a boundary: gate at both heads, and the base
# tree's own Python steps (pp_top's ltn_rom.hex prerequisite, docs gates) under
# the same locales, to separate the gate's behaviour from pre-existing limits.
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
for spec in "de_DE ISO-8859-1 de_DE.ISO-8859-1" "fr_FR@euro ISO-8859-15 fr_FR.ISO-8859-15@euro" \
            "ja_JP EUC-JP ja_JP.EUC-JP"; do
  set -- $spec
  localedef -i "$1" -f "$2" "$S/locales/$3" >/dev/null 2>&1; echo "localedef $3 rc=$?"
done
declare -A L=([DE_LATIN1]=de_DE.ISO-8859-1 [FR_LATIN9]=fr_FR.ISO-8859-15@euro [JA_EUCJP]=ja_JP.EUC-JP)
for k in DE_LATIN1 FR_LATIN9 JA_EUCJP; do
  for w in prior head; do
    $RUN --name "13-$w-$k" --cwd "$S/$w/tb/pp_top" --set LANG="${L[$k]}" \
      --set R230_VLOG="$RC/verilator-invocations.jsonl" -- make -j1 VERILATOR="$V8" fixture-guards
    grep -a -E '^(fixture guards:|FAIL:)|UnicodeDecodeError' "$RC/13-$w-$k.log" | head -2 | cut -c1-170 | sed 's/^/   /'
  done
done
B="$S/base-export"; rm -rf "$B"; mkdir -p "$B"
git -C $VALIDATION_STORAGE/reviews/r230-100-r2 archive 8452f564294300a82d56eed464276576f65f4d58 | tar -x -C "$B"
for k in JA_EUCJP DE_LATIN1 FR_LATIN9; do
  rm -f "$B/tb/pp_top/ltn_rom.hex"
  $RUN --name "13-base-make-ltn_rom-$k" --cwd "$B/tb/pp_top" --set LANG="${L[$k]}" -- make -j1 ltn_rom.hex
  grep -a -m1 -E 'Error|wrote' "$RC/13-base-make-ltn_rom-$k.log" | cut -c1-150 | sed 's/^/   /'
done
rm -f "$B/tb/pp_top/ltn_rom.hex"
$RUN --name 13-base-make-ltn_rom-DE_UTF8 --cwd "$B/tb/pp_top" --set LANG=de_DE.UTF-8 -- make -j1 ltn_rom.hex
grep -a -m1 -E 'Error|wrote' "$RC/13-base-make-ltn_rom-DE_UTF8.log" | sed 's/^/   /'
rm -f "$B/tb/pp_top/ltn_rom.hex"
for c in "check_upc_map:python3 -B scripts/check_upc_map.py" "check_links:python3 -B scripts/check-links.py" \
         "check_matrix:python3 -B scripts/check-matrix.py" "gen_matrix:python3 -B scripts/gen_matrix.py --check" \
         "gen_ucode:python3 -B hdl/aecp/ucode/gen_ucode.py -o $S/tmp/u.hex" \
         "gen_ltn_rom:python3 -B hdl/acmp/rom/gen_ltn_rom.py -o $S/tmp/l.hex"; do
  n=${c%%:*}
  $RUN --name "13-base-JA_EUCJP-$n" --cwd "$B" --set LANG=ja_JP.EUC-JP -- sh -c "${c#*:}"
  grep -a -E 'Error|OK|PASS|wrote' "$RC/13-base-JA_EUCJP-$n.log" | tail -1 | cut -c1-150 | sed 's/^/   /'
done
rm -f "$S/tmp/u.hex" "$S/tmp/l.hex"
rm -rf "$B"
