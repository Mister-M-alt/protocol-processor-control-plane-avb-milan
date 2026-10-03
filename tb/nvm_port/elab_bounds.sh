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
#   The payload bound's refusal is a FATAL, not just a failing lint. Verilator
#   reports `$fatal`, `$error` and `$warning` alike as warnings, each of which
#   fails -Wall, so the refusal's own line must carry %Warning-USERFATAL (or
#   %Error). Where sv2v and yosys are on PATH the class is also graded where it
#   decides the outcome: yosys elaborates 65527 and refuses 65528 by running
#   the lowered `$fatal`'s `$finish`, and it elaborates through a module-scope
#   `$error` or `$warning`. The deadline guard is an `$error`, like every other
#   module-scope guard in hdl/, and is graded by name only.
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
refused() { # $1 = parameter, $2 = the classes its refusal may carry (ERE),
            # $3 = the refusal's text after "$1=<value> ", then values
  local p=$1 cls=$2 why=$3 v out line; shift 3
  for v in "$@"; do
    # This elaboration MUST fail; its status is consumed here so errexit does
    # not end the script before the verdict it was run for.
    if out=$(elaborate "$p" "$v"); then
      echo "GUARD FAIL $p=$v elaborated; the guard did not fire"
      rc=1
    elif ! line=$(grep -m1 -F "$p=$v $why" <<<"$out"); then
      echo "GUARD FAIL $p=$v failed without naming the parameter and its bound"
      grep -E '%(Warning|Error)' <<<"$out" | head -5 || true
      rc=1
    elif ! grep -qE "^%($cls)[:-]" <<<"$line"; then
      echo "GUARD FAIL $p=$v refused as ${line%%:*}, not as %($cls)"
      rc=1
    else
      echo "GUARD OK   $p=$v (refused by name as ${line%%:*}, as it must be)"
    fi
  done
}

yosys_at() { # $1 = the sv2v output, $2 = parameter, $3 = its value
  yosys -q -p "read_verilog -defer $1; chparam -set $2 $3 KL_pp_nvm_port; hierarchy -check -top KL_pp_nvm_port; proc; opt_clean" 2>&1
}
# sv2v + yosys, when both are installed: $1 = parameter, $2 = a legal value
# that must elaborate, $3 = the first value its `$fatal` must refuse.
fatal_in_yosys() {
  local p=$1 ok=$2 bad=$3 work out
  if ! command -v sv2v >/dev/null 2>&1 || ! command -v yosys >/dev/null 2>&1; then
    echo "YOSYS SKIP $p (sv2v or yosys is not on PATH; the class above still holds)"
    return 0
  fi
  work=$(mktemp -d)
  if ! out=$(sv2v "$SRC" 2>&1 > "$work/port.v"); then
    echo "YOSYS FAIL sv2v refused $SRC"
    head -3 <<<"$out"
    rc=1
  elif ! out=$(yosys_at "$work/port.v" "$p" "$ok"); then
    echo "YOSYS FAIL $p=$ok did not elaborate"
    grep -m3 'ERROR' <<<"$out" || true
    rc=1
  elif out=$(yosys_at "$work/port.v" "$p" "$bad"); then
    echo "YOSYS FAIL $p=$bad elaborated; the guard is not a \$fatal"
    rc=1
  elif ! grep -qF '$finish' <<<"$out"; then
    echo "YOSYS FAIL $p=$bad failed, but not at the guard's \$finish"
    grep -m3 'ERROR' <<<"$out" || true
    rc=1
  else
    echo "YOSYS OK   $p=$ok elaborates, $p=$bad stops at the guard's \$finish"
  fi
  rm -rf "$work"
}

legal MEM_TIMEOUT_CYC_P 1 2147483647
refused MEM_TIMEOUT_CYC_P 'Warning|Error' "is outside 1 to 2147483647" 0 2147483648 4294967295
legal MAX_PAYLOAD_P 1024 65527
refused MAX_PAYLOAD_P 'Warning-USERFATAL|Error' "is above 65527" 65528 65535 4294967295
fatal_in_yosys MAX_PAYLOAD_P 65527 65528
exit "$rc"
