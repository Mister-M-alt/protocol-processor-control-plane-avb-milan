#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
# Device portability: sv2v + yosys must elaborate every top (hdl/README rule 2).
# The tops array is the authoritative list, and the census below is what makes
# that sentence true rather than aspirational: six modules were absent from it
# and no run ever elaborated them, `protocol_processor_top` among them (#25).
set -eu
cd "$(dirname "$0")/../.."

# ONE ENTRY PER MODULE UNDER hdl/. The census after it fails the gate when that
# stops holding, so a new module cannot be added without a top.
tops=(KL_aecp_ucpu KL_aecp_desc_store KL_aecp_dyn_state KL_aecp_resp_buf KL_aecp_engine KL_aecp_notify KL_aecp_ca_originator KL_pp_timer_service KL_pp_prng KL_pp_rx_slots KL_pp_tx_slots KL_pp_release_merge
      KL_pp_rx_validator KL_pp_normalizer KL_pp_dispatch KL_pp_scoreboard
      KL_pp_event_router KL_pp_originator KL_pp_tx_arbiter KL_pp_trace_ring
      KL_pp_side_port KL_pp_nvm_port KL_adp_engine KL_pp_acmp_listener
      KL_acmp_talker KL_pp_maap KL_srp_decoder KL_srp_encoder KL_srp_domain
      KL_srp_vlan KL_srp_talker_fsm KL_srp_listener_fsm
      KL_acmp_nvm_shadow KL_mrp_strip KL_pp_dispatch_fifo KL_srp_admission
      KL_srp_top protocol_processor_top)

# THE ARRAY IS ONLY AUTHORITATIVE IF SOMETHING ENFORCES IT. Declared modules are
# read from the sources, never from a second hand-written list, so the two
# cannot drift the way they already had: the union of what `hierarchy -top`
# reached over the previous 32 entries was exactly those 32, so the six missing
# modules were not covered transitively either.
declared="$(grep -rhoE '^[[:space:]]*module[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' \
              hdl --include='*.sv' | awk '{print $2}' | sort -u)"
missing="$(comm -23 <(printf '%s\n' "$declared") \
                    <(printf '%s\n' "${tops[@]}" | sort -u))"
if [ -n "$missing" ]; then
  echo "modules declared under hdl/ with no entry in the tops array:" >&2
  printf '  %s\n' $missing >&2
  echo "extend tops (hdl/README rule 2) - the array is the authoritative list" >&2
  exit 1
fi
stale="$(comm -13 <(printf '%s\n' "$declared") \
                  <(printf '%s\n' "${tops[@]}" | sort -u))"
if [ -n "$stale" ]; then
  echo "tops array names modules that no longer exist under hdl/:" >&2
  printf '  %s\n' $stale >&2
  exit 1
fi

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
sv2v $(find hdl -name '*_pkg.sv' | sort) $(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort) > "$work/all.v"
( cd hdl/aecp/ucode && python3 gen_ucode.py -o "$work/ucode.hex" >/dev/null )
( cd hdl/acmp/rom && python3 gen_ltn_rom.py -o "$work/ltn_rom.hex" >/dev/null 2>&1 || python3 gen_ltn_rom.py > /dev/null; cp ltn_rom.hex "$work/" 2>/dev/null || true )
cd "$work"

# `-defer` AND A POOL, WHICH ARE THE SAME FIX. This script lowers the whole
# tree into one all.v and then reads it once per top; every read but the
# selected top's was elaboration nobody asked for. Measured here: 39.65s as it
# stood, 10.31s with -defer, 1.25s with -defer in a 16-way pool - and 5.68s for
# the 38 tops above, against 39.65s for the 32 it used to run.
#
# -defer DEFERS ELABORATION, NOT PARSING, so it costs no front-end coverage:
# a syntax error, a negative-width vector, an undefined submodule and a $fatal
# injected into a module no top instantiates are each caught with and without
# it (#25). The parse is still the thing that reads every module; the tops
# array above is what elaborates each one.
jobs="${YOSYS_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
[ "$jobs" -ge 1 ] 2>/dev/null || jobs=4
[ "$jobs" -le 16 ] || jobs=16

elaborate_one() {
  local t="$1"
  if yosys -q -p "read_verilog -defer all.v; hierarchy -check -top $t; proc; opt_clean" \
       > "$t.yos.log" 2>&1; then
    printf 'OK\n' > "$t.status"
  else
    printf 'FAIL\n' > "$t.status"
    return 1
  fi
}
export -f elaborate_one

# The pool's own output interleaves, so it is discarded and the verdict is
# printed below IN INVENTORY ORDER from the per-top status files. A gate whose
# report changes order between runs cannot be diffed between runs.
printf '%s\n' "${tops[@]}" \
  | xargs -P "$jobs" -I{} bash -c 'elaborate_one "$@"' _ {} >/dev/null 2>&1 || true

fail=0
for t in "${tops[@]}"; do
  if [ -f "$t.status" ] && [ "$(cat "$t.status")" = OK ]; then
    echo "YOSYS OK  $t"
  else
    # A missing status file is a worker that died without recording a verdict,
    # which is a failure and not a pass: absence of evidence is not a green.
    echo "YOSYS FAIL $t: $(grep -oE 'ERROR:.*' "$t.yos.log" 2>/dev/null | head -1)"
    fail=1
  fi
done
[ "$fail" -eq 0 ] || exit 1

# Elaboration alone does not prove that inferred memories map onto the target
# FPGA. KL_aecp_engine contains the largest mixed-control RAM in this block, so
# carry it through the complete Xilinx memory-mapping flow as a regression gate.
# Six RAMB36 is the current engine shape: five existing stores plus exactly
# one staging store. Assert both facts so losing inference and expanding byte
# lanes fail in opposite directions.
yosys -q -p "read_verilog all.v; hierarchy -check -top KL_aecp_engine; synth_xilinx -family xc7 -flatten -top KL_aecp_engine; select -module KL_aecp_engine; select -assert-count 1 c:amap_stage_r* t:RAMB36E1 %i; select -assert-count 6 t:RAMB36E1" \
  && echo "YOSYS XILINX OK  KL_aecp_engine" \
  || { echo "YOSYS XILINX FAIL KL_aecp_engine"; exit 1; }
