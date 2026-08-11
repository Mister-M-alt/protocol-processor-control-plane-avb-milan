#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
# Device portability: sv2v + yosys must elaborate every top (hdl/README rule 2).
# The tops array is the authoritative list — extend it with every new top.
set -eu
cd "$(dirname "$0")/../.."
tops=(KL_aecp_ucpu KL_pp_timer_service KL_pp_prng KL_pp_rx_slots KL_pp_tx_slots
      KL_pp_rx_validator KL_pp_normalizer KL_pp_dispatch KL_pp_scoreboard
      KL_pp_event_router KL_pp_originator KL_pp_tx_arbiter KL_pp_trace_ring
      KL_pp_side_port KL_pp_nvm_port KL_adp_engine KL_acmp_listener
      KL_acmp_talker KL_srp_decoder KL_srp_encoder KL_srp_domain KL_srp_vlan
      KL_srp_talker_fsm KL_srp_listener_fsm)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
sv2v $(find hdl -name '*_pkg.sv' | sort) $(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort) > "$work/all.v"
( cd hdl/aecp/ucode && python3 gen_ucode.py -o "$work/ucode.hex" >/dev/null )
( cd hdl/acmp/rom && python3 gen_ltn_rom.py -o "$work/ltn_rom.hex" >/dev/null 2>&1 || python3 gen_ltn_rom.py > /dev/null; cp ltn_rom.hex "$work/" 2>/dev/null || true )
cd "$work"
for t in "${tops[@]}"; do
  yosys -q -p "read_verilog all.v; hierarchy -check -top $t; proc; opt_clean" \
    && echo "YOSYS OK  $t" || { echo "YOSYS FAIL $t"; exit 1; }
done
