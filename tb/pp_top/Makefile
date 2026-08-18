# SPDX-License-Identifier: CERN-OHL-W-2.0
# protocol_processor_top end-to-end suite — exit 0 = PASS.
VERILATOR ?= verilator
HDL        = ../../hdl
TOP        = pp_top_wrap

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR)"

SRCS = $(HDL)/common/pp_pkg.sv $(HDL)/srp/srp_pkg.sv \
       $(HDL)/acmp/pp_acmp_pkg.sv $(HDL)/adp/pp_adp_pkg.sv \
       $(HDL)/common/KL_pp_prng.sv $(HDL)/common/KL_pp_timer_service.sv \
       $(HDL)/packet_engine/KL_pp_rx_validator.sv \
       $(HDL)/packet_engine/KL_pp_rx_slots.sv \
       $(HDL)/packet_engine/KL_pp_normalizer.sv \
       $(HDL)/packet_engine/KL_pp_dispatch.sv \
       $(HDL)/packet_engine/KL_pp_tx_slots.sv \
       $(HDL)/packet_engine/KL_pp_release_merge.sv \
       $(HDL)/packet_engine/KL_pp_tx_arbiter.sv \
       $(HDL)/packet_engine/KL_pp_scoreboard.sv \
       $(HDL)/packet_engine/KL_pp_event_router.sv \
       $(HDL)/packet_engine/KL_pp_originator.sv \
       $(HDL)/packet_engine/KL_pp_trace_ring.sv \
       $(HDL)/packet_engine/KL_pp_side_port.sv \
       $(HDL)/packet_engine/KL_pp_nvm_port.sv \
       $(HDL)/aecp/ucpu_pkg.sv $(HDL)/aecp/KL_aecp_ucpu.sv \
       $(HDL)/aecp/KL_aecp_desc_store.sv $(HDL)/aecp/KL_aecp_dyn_state.sv $(HDL)/aecp/KL_aecp_engine.sv $(HDL)/aecp/KL_aecp_resp_buf.sv \
       $(HDL)/aecp/KL_aecp_notify.sv $(HDL)/aecp/KL_aecp_ca_originator.sv \
       $(HDL)/adp/KL_adp_engine.sv \
       $(HDL)/acmp/KL_pp_acmp_listener.sv $(HDL)/acmp/KL_acmp_talker.sv \
       $(HDL)/acmp/KL_acmp_nvm_shadow.sv \
       $(HDL)/maap/KL_pp_maap.sv \
       $(HDL)/srp/KL_srp_decoder.sv $(HDL)/srp/KL_srp_domain.sv \
       $(HDL)/srp/KL_srp_vlan.sv $(HDL)/srp/KL_srp_admission.sv \
       $(HDL)/srp/KL_srp_talker_fsm.sv $(HDL)/srp/KL_srp_listener_fsm.sv \
       $(HDL)/srp/KL_srp_encoder.sv $(HDL)/srp/KL_srp_top.sv \
       $(HDL)/top/KL_mrp_strip.sv $(HDL)/top/protocol_processor_top.sv \
       pp_top_wrap.sv
CPP  = sim_main.cpp

run: ltn_rom.hex ucode.hex
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vpp_top_sim
	./obj_dir/Vpp_top_sim

ltn_rom.hex: $(HDL)/acmp/rom/gen_ltn_rom.py
	python3 $< -o $@

ucode.hex: $(HDL)/aecp/ucode/gen_ucode.py
	python3 $< -o $@

clean:
	rm -rf obj_dir ltn_rom.hex ucode.hex

.PHONY: run clean
