#!/usr/bin/env bash
# RTL lens at the exact head: product-top zero-tolerance lint at four VID values
# (default, 0002, 1002, 5A3C), no RTL assertion on the VID, the three RTL
# defaults the guard's reference value stands for, and the real fixture build
# recipe for 0002/1002 (Verilator accepts the override; only the bench refuses).
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
H="$S/head"
cd "$H" || exit 1
pkgs=$(find hdl -name '*_pkg.sv' | sort | tr '\n' ' ')
all=$(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort | tr '\n' ' ')
for v in default 16\'h0002 16\'h1002 16\'h5A3C; do
  g=""; [ "$v" = default ] || g="-GSRP_DOM_DEF_VID_P=$v"
  n="10-lint-top-${v//\'/}"
  # shellcheck disable=SC2086
  $RUN --name "$n" --cwd "$H" --set R230_VLOG="$RC/verilator-invocations.jsonl" --expect-rc 0 -- \
    "$V8" --lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
    --top-module protocol_processor_top $g $pkgs $all
  echo "   $n: %Warning/%Error lines = $(grep -cE '%(Warning|Error)' "$RC/$n.log")"
done
echo "== VID parameter defaults at head"
grep -n "DOM_DEF_VID_P\s*=\|DEF_VID_P\s*=" hdl/top/protocol_processor_top.sv hdl/srp/KL_srp_top.sv hdl/srp/KL_srp_domain.sv
echo "== assertions mentioning a VID parameter in those RTL files (expect none)"
grep -n -i -E "assert|\\\$error|\\\$fatal" hdl/top/protocol_processor_top.sv hdl/srp/KL_srp_top.sv hdl/srp/KL_srp_domain.sv | grep -i vid || echo "   none"
echo "== real fixture build recipe, 0002 and 1002 (EN caller)"
cd "$H/tb/pp_top" || exit 1
for f in 0002 1002; do
  rm -rf obj_vid
  cmd=$(make -n run VERILATOR="$V8" SRP_VID_FIXTURE=$f | sed -e ':a' -e '/\\$/N; s/\\\n//; ta' | grep -- '--Mdir obj_vid')
  echo "$cmd" > "$RC/10-fixture-build-$f.cmd.txt"
  $RUN --name "10-fixture-build-$f" --cwd "$H/tb/pp_top" --set R230_VLOG="$RC/verilator-invocations.jsonl" \
    --expect-rc 2 -- sh -c "$cmd"
  echo "   verilation report present: $(grep -c 'V e r i l a t i o n   R e p o r t' "$RC/10-fixture-build-$f.log")"
  echo "   model header generated: $([ -f obj_vid/Vpp_top_wrap.h ] && echo yes || echo no); fixture executable: $([ -e obj_vid/Vpp_top_vid ] && echo yes || echo no)"
  echo "   failing objects: $(grep -oE 'Error [0-9]+|\*\*\* \[[^]]+\]' "$RC/10-fixture-build-$f.log" | sort | uniq -c | tr '\n' ';')"
  grep -E 'error:' "$RC/10-fixture-build-$f.log" | sed 's/^/   /' | cut -c1-180
  rm -rf obj_vid
done
cd "$H" && echo "== head clone status --porcelain --ignored: $(git status --porcelain --ignored | wc -l) lines"
