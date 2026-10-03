#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
#
# KL_pp_nvm_port's two elaboration guards: MEM_TIMEOUT_CYC_P (processor issue
# #15) and MAX_PAYLOAD_P (processor issue #17). Two claims for each:
#
#   LEGAL values elaborate with zero warnings under -Wall, at both ends of the
#   range. For the deadline: 1, the smallest, and 2^31 - 1, the largest the
#   counter is sized for; the suite itself runs at 100 (the Makefile's -G), so
#   these two are the only evidence the extremes build. For the payload bound:
#   65527, the largest whose 8 + payload_length fits dev_len_o's 16 bits, and
#   1024, the default the top builds.
#
#   ILLEGAL values stop the build with an error naming the parameter. For the
#   deadline: 0, which would make every owed cycle a deadline, 2^31, the first
#   the guard refuses, and 2^32 - 1, where a 32-bit + 1 wraps to 0 and the
#   counter width would collapse before any guard could speak. For the payload
#   bound: 65528, the first the guard refuses, 65535, the WRITE of 7 bytes that
#   then pumps 65543, and 2^32 - 1, which the 16-bit MAXP_C would truncate to
#   65535. Its message must name the bound, 65527, as well as the parameter.
#
# Exit 0 = every claim holds. Same shape as tb/timer_map/shape_elab.sh, and
# for the same reasons: the verdict greps read a here-string, never a pipe.
set -euo pipefail
cd "$(dirname "$0")" || exit 1
VERILATOR=${VERILATOR:-verilator}
SRC=../../hdl/packet_engine/KL_pp_nvm_port.sv
VFLAGS="--lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM"

elaborate() { # $1 = parameter, $2 = its value
  $VERILATOR $VFLAGS -G"$1"="$2" "$SRC" 2>&1  # shellcheck disable=SC2086 # deliberate word split of the flag list
}

rc=0
legal() { # $1 = parameter, then its legal values
  local p=$1 v out; shift
  for v in "$@"; do
    if ! out=$(elaborate "$p" "$v") || grep -qE '%(Warning|Error)' <<<"$out"; then
      echo "ELAB FAIL  $p=$v (a legal value must elaborate clean)"
      grep -E '%(Warning|Error)' <<<"$out" | head -5 || true
      rc=1
    else
      echo "ELAB OK    $p=$v"
    fi
  done
}
refused() { # $1 = parameter, $2 = the refusal's text after "$1=<value> ", then values
  local p=$1 why=$2 v out; shift 2
  for v in "$@"; do
    # This elaboration MUST fail; its status is consumed here so errexit does
    # not end the script before the verdict it was run for.
    if out=$(elaborate "$p" "$v"); then
      echo "GUARD FAIL $p=$v elaborated; the guard did not fire"
      rc=1
    elif grep -qF "$p=$v $why" <<<"$out"; then
      echo "GUARD OK   $p=$v (refused by name, as it must be)"
    else
      echo "GUARD FAIL $p=$v failed without naming the parameter and its bound"
      grep -E '%(Warning|Error)' <<<"$out" | head -5 || true
      rc=1
    fi
  done
}

legal MEM_TIMEOUT_CYC_P 1 2147483647
refused MEM_TIMEOUT_CYC_P "is outside 1 to 2147483647" 0 2147483648 4294967295
legal MAX_PAYLOAD_P 1024 65527
refused MAX_PAYLOAD_P "is above 65527" 65528 65535 4294967295
exit "$rc"
