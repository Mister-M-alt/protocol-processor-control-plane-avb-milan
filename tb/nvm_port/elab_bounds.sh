#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
#
# KL_pp_nvm_port's MEM_TIMEOUT_CYC_P guard (processor issue #15). Two claims:
#
#   LEGAL values elaborate with zero warnings under -Wall, at both ends of the
#   range: 1, the smallest, and 2^31 - 1, the largest the counter is sized
#   for. The suite itself runs at 100 (the Makefile's -G), so these two are
#   the only evidence the extremes build.
#
#   ILLEGAL values stop the build with an error naming the parameter: 0,
#   which would make every owed cycle a deadline, 2^31, the first the guard
#   refuses, and 2^32 - 1, where a 32-bit + 1 wraps to 0 and the counter
#   width would collapse before any guard could speak.
#
# Exit 0 = both claims hold. Same shape as tb/timer_map/shape_elab.sh, and
# for the same reasons: the verdict greps read a here-string, never a pipe.
set -euo pipefail
cd "$(dirname "$0")" || exit 1
VERILATOR=${VERILATOR:-verilator}
SRC=../../hdl/packet_engine/KL_pp_nvm_port.sv
VFLAGS="--lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM"

elaborate() { # $1 = MEM_TIMEOUT_CYC_P
  $VERILATOR $VFLAGS -GMEM_TIMEOUT_CYC_P="$1" "$SRC" 2>&1  # shellcheck disable=SC2086 # deliberate word split of the flag list
}

rc=0
for v in 1 2147483647; do
  if ! out=$(elaborate "$v") || grep -qE '%(Warning|Error)' <<<"$out"; then
    echo "ELAB FAIL  MEM_TIMEOUT_CYC_P=$v (a legal value must elaborate clean)"
    grep -E '%(Warning|Error)' <<<"$out" | head -5 || true
    rc=1
  else
    echo "ELAB OK    MEM_TIMEOUT_CYC_P=$v"
  fi
done
for v in 0 2147483648 4294967295; do
  # This elaboration MUST fail; its status is consumed here so errexit does
  # not end the script before the verdict it was run for.
  if out=$(elaborate "$v"); then
    echo "GUARD FAIL MEM_TIMEOUT_CYC_P=$v elaborated; the guard did not fire"
    rc=1
  elif grep -q "MEM_TIMEOUT_CYC_P=$v is outside 1 to 2147483647" <<<"$out"; then
    echo "GUARD OK   MEM_TIMEOUT_CYC_P=$v (refused by name, as it must be)"
  else
    echo "GUARD FAIL MEM_TIMEOUT_CYC_P=$v failed without naming the parameter"
    grep -E '%(Warning|Error)' <<<"$out" | head -5 || true
    rc=1
  fi
done
exit "$rc"
