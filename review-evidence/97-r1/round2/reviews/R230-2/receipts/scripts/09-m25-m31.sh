#!/usr/bin/env bash
# M25 (top-to-child VID binding removed) and M31 (child default 2 -> 7, binding
# intact) at the exact head, each in its own disposable export, with A161's
# published round-1 patches (evidence 290d5a2, review-evidence/97-r1/author).
# Serial outer make; Verilator through verilator8. Caller settings localized.
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
R=$VALIDATION_STORAGE/reviews/r230-100-r2
EV=290d5a2ceb23a5cb4eac41257ac331d63babeaf3
for m in M25:06-missing-binding:DE_LANGUAGE:2 M31:07-child-default:FR_LC_MESSAGES:0; do
  IFS=: read -r name patch k exp <<< "$m"
  T="$S/$name-tree"; rm -rf "$T"; mkdir -p "$T"
  git -C "$R" archive 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e | tar -x -C "$T"
  git -C "$R" show "$EV:review-evidence/97-r1/author/receipts/$patch.patch" > "$RC/09-$name.patch"
  (cd "$T" && patch -p1 --no-backup-if-mismatch < "$RC/09-$name.patch") > "$RC/09-$name.apply.txt" 2>&1
  echo "$name apply rc=$? ($(cat "$RC/09-$name.apply.txt"))"
  diff -r -q "$R/hdl" "$T/hdl" | sed "s|$S|<scratch>|; s|$R|<review>|" | sed 's/^/   changed: /'
  $RUN --name "09-$name-$k" --cwd "$T/tb/pp_top" ${SETTING[$k]} \
    --set R230_VLOG="$RC/verilator-invocations.jsonl" --expect-rc "$exp" -- make -j1 VERILATOR="$V8"
  cp "$T/tb/pp_top/obj_dir/build_tally.txt" "$RC/09-$name.build_tally.txt" 2>/dev/null
  echo "== $name"
  grep -E '^(fixture guards:|\[build|[0-9]+ checks:)|FAIL' "$RC/09-$name-$k.log" | grep -v 'Fehler\|Error [0-9]' | cut -c1-170 | sed 's/^/   /' | head -30
  echo "   build_tally.txt: $(tr '\n' ' ' < "$RC/09-$name.build_tally.txt" 2>/dev/null)"
  echo "   last line: $(tail -1 "$RC/09-$name-$k.log")"
  rm -rf "$T"
done
