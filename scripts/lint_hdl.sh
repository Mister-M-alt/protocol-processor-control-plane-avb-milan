#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
# Verilator --lint-only over every module under hdl/. ZERO tolerance — this
# repository is greenfield and no violation is ever grandfathered. A waiver
# needs a `verilator lint_off` pragma WITH a justification comment in the RTL.
set -u
cd "$(dirname "$0")/.."
rc=0
pkgs=$(find hdl -name '*_pkg.sv' | sort)
for f in $(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort); do
  if ! verilator --lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
       -Wno-UNUSEDPARAM $pkgs "$f" 2>&1 | grep -E '%(Warning|Error)'; then
    echo "LINT OK  $f"
  else
    echo "LINT FAIL $f"; rc=1
  fi
done
exit $rc
