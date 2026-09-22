#!/usr/bin/env bash
# Conformance at the exact head: the normative sources for the fixture policy,
# the two guards and the DV4 guard as committed, and a width value matrix of
# the real sim_main.cpp (-fsyntax-only, LC_ALL=C) against one generated model.
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
H="$S/head"
cd "$H" || exit 1
echo "== sources"
sed -n '166p' docs/architecture/01_overview.md
sed -n '66p' docs/guides/integrator.md
sed -n '198p;366,369p' docs/architecture/10_srp_engine.md
sed -n '410p;422p' docs/00_MILAN_COMPLIANCE_REVIEW.md | cut -c1-300
echo "== bench guards (tb/pp_top/sim_main.cpp:75-87, 8257-8262)"
sed -n '75,87p;8257,8262p' tb/pp_top/sim_main.cpp
echo "== value matrix"
M="$S/value-model"; rm -rf "$M"; mkdir -p "$M"
cd "$H/tb/pp_top" || exit 1
vflags=$(make -n fixture-guards VERILATOR=x | sed -e ':a' -e '/\\$/N; s/\\\n//; ta' \
         | grep '^python3 fixture_guards.py' | sed 's/.* -- //')
# shellcheck disable=SC2086
eval "$V8 $vflags --Mdir $M" > "$RC/14-value-model-verilate.log" 2>&1; echo "verilate rc=$?"
root=$(verilator --getenv VERILATOR_ROOT)
for v in none 5A3C 5a3c 0003 0000 0FFF 1005 0002 2 10002 1002 F002; do
  d=""; [ "$v" = none ] || d="-DPP_TOP_SRP_DOM_DEF_VID=0x$v"
  out=$(LC_ALL=C g++ -std=c++17 -Wall -Wextra -fsyntax-only -I"$M" -I"$root/include" \
        -I"$root/include/vltstd" $d sim_main.cpp 2>&1); rc=$?
  w=$(grep -c "error: static assertion failed: SRP VID fixture must differ from product default 2 in the 16-bit wire value" <<<"$out")
  c=$(grep -c "error: static assertion failed: SRP VID fixture must differ from product default 2 in the 12-bit class-D value" <<<"$out")
  dv4=$(grep -c "error: static assertion failed: DV4 must see the VID move" <<<"$out")
  other=$(grep "error:" <<<"$out" | grep -vc "static assertion failed: \(SRP VID fixture must differ\|DV4 must see\)")
  printf '   %-6s rc=%d wire=%d class-D=%d DV4=%d other-errors=%d\n' "$v" "$rc" "$w" "$c" "$dv4" "$other"
done
rm -rf "$M"
echo "== head clone status --porcelain --ignored: $(git -C "$H" status --porcelain --ignored | wc -l) lines"
