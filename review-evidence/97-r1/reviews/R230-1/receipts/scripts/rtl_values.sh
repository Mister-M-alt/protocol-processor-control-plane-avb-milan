#!/usr/bin/env bash
# R230 RTL-lens probe: the product top still elaborates, warning-free under the
# donor's zero-tolerance lint flags, at its default and at the two values the
# bench now refuses. rtl_values.sh REPO_EXPORT VERILATOR
set -uo pipefail
cd "$1" || exit 1
V=$2
pkgs=$(find hdl -name '*_pkg.sv' | sort)
all=$(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort)
rc=0
for g in "" "16'h0002" "16'h1002" "16'h5A3C"; do
  gflag=(); [ -n "$g" ] && gflag=("-GSRP_DOM_DEF_VID_P=$g")
  # shellcheck disable=SC2086
  out=$("$V" --lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
        --top-module protocol_processor_top "${gflag[@]}" $pkgs $all 2>&1); st=$?
  n=$(grep -cE '%(Warning|Error)' <<<"$out")
  echo "protocol_processor_top ${g:-default}: verilator exit $st, $n warning/error lines"
  [ "$st" -eq 0 ] && [ "$n" -eq 0 ] || { rc=1; grep -E '%(Warning|Error)' <<<"$out" | head -5; }
done
echo "grep of hdl/ for any assertion on the VID parameter:"
grep -rnE 'assert|\$error|\$fatal' hdl/top/protocol_processor_top.sv hdl/srp/KL_srp_top.sv hdl/srp/KL_srp_domain.sv \
  | grep -iE 'VID' || echo "  none"
exit "$rc"
