#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
#
# Elaborate the REAL protocol_processor_top across shapes. Two claims:
#
#   LEGAL shapes must elaborate with ZERO warnings under -Wall. This is the
#   only thing that can prove the F08.4 map and every index width actually
#   TRACK P-N-STREAM-IN / P-N-STREAM-OUT: a simulation of the 8x8 shape sees
#   nothing, because 8x8 is the one shape a literal map is right at.
#
#   OVER-LARGE shapes must FAIL, loudly, at elaboration. The 8-bit expiry
#   bus's owner-tag allocation (pp_pkg PP_OWN_*) is fixed, so a shape past
#   it has no correct map; the guard in the top turns that into a build stop
#   instead of two engines sharing an owner tag in silence.
#
# Exit 0 = both claims hold.
set -u
cd "$(dirname "$0")"
VERILATOR=${VERILATOR:-verilator}
HDL=../../hdl
SRCS=$( (find $HDL -name '*_pkg.sv' | sort; find $HDL -name '*.sv' ! -name '*_pkg.sv' | sort) )
VFLAGS="--lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM"

# SI SO — the F01.5 default, the board's 1-stream shipping shape, the 9x9
# shape the reference platform's generated header asks for, and asymmetric
# shapes (a map derived from the WRONG count still looks right when SI == SO)
LEGAL="1:1 1:8 8:1 2:2 4:4 8:8 9:9 9:2 3:12 16:16"
# past the owner-tag space: SO 17 overruns the talker tag base, SI 33 the
# listener tag base (ADP publishes its slot AS its owner tag)
OVER="17:17 16:17 33:8"

rc=0
for pair in $LEGAL; do
  si=${pair%%:*}; so=${pair##*:}
  out=$($VERILATOR $VFLAGS --top-module protocol_processor_top \
        -GN_STREAM_IN_P="$si" -GN_STREAM_OUT_P="$so" $SRCS 2>&1)
  if [ $? -ne 0 ] || echo "$out" | grep -qE '%(Warning|Error)'; then
    echo "SHAPE FAIL SI=$si SO=$so (legal shape must elaborate clean)"
    echo "$out" | grep -E '%(Warning|Error)' | head -5
    rc=1
  else
    echo "SHAPE OK   SI=$si SO=$so"
  fi
done

for pair in $OVER; do
  si=${pair%%:*}; so=${pair##*:}
  out=$($VERILATOR $VFLAGS --top-module protocol_processor_top \
        -GN_STREAM_IN_P="$si" -GN_STREAM_OUT_P="$so" $SRCS 2>&1)
  if echo "$out" | grep -q 'F08.4: owner tags OVERLAP'; then
    echo "GUARD OK   SI=$si SO=$so (elaboration refused, as it must)"
  else
    echo "GUARD FAIL SI=$si SO=$so — the guard did NOT fire. An over-large"
    echo "           shape would alias owner tags in silence."
    rc=1
  fi
done
exit $rc
