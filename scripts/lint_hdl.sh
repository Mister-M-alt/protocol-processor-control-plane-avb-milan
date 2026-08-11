#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
# Verilator --lint-only over every module under hdl/, each elaborated as a top
# WITH the whole tree visible (hierarchical modules need their children).
# ZERO tolerance — greenfield; a waiver is a justified lint_off pragma in RTL.
set -u
cd "$(dirname "$0")/.."
pkgs=$(find hdl -name '*_pkg.sv' | sort)
all=$(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort)
rc=0
for f in $all; do
  top=$(grep -oEm1 '^\s*module\s+\w+' "$f" | awk '{print $2}')
  [ -n "$top" ] || continue
  out=$(verilator --lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
        -Wno-UNUSEDPARAM --top-module "$top" $pkgs $all 2>&1)
  if echo "$out" | grep -qE '%(Warning|Error)'; then
    echo "LINT FAIL $top ($f)"; echo "$out" | grep -E '%(Warning|Error)' | head -5
    rc=1
  else
    echo "LINT OK  $top"
  fi
done
exit $rc
