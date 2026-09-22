#!/usr/bin/env bash
# R223-1: the donor lint gate's own flags on protocol_processor_top (head), at
# the top's default and with the verification fixture applied by -G, plus
# KL_srp_top at head. Scratch copy only; Verilator capped by bin/verilator8.
set -uo pipefail
R=/tmp/r223-96-r1
cd $R/head
pkgs=$(find hdl -name '*_pkg.sv' | sort)
all=$(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort)
for g in "" "-GSRP_DOM_DEF_VID_P=16'h5A3C"; do
  echo "== lint protocol_processor_top ${g:-<default>}"
  out=$($R/bin/verilator8 --lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
        -Wno-UNUSEDPARAM --top-module protocol_processor_top $g $pkgs $all 2>&1); rc=$?
  echo "rc=$rc warnings=$(grep -cE '%(Warning|Error)' <<<"$out")"
  grep -E '%(Warning|Error)' <<<"$out" | head -5
done
echo "== lint KL_srp_top"
out=$($R/bin/verilator8 --lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
      -Wno-UNUSEDPARAM --top-module KL_srp_top $pkgs $all 2>&1); rc=$?
echo "rc=$rc warnings=$(grep -cE '%(Warning|Error)' <<<"$out")"
echo "LINT PROBE DONE"
