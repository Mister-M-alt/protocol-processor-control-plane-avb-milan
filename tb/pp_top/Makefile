# SPDX-License-Identifier: CERN-OHL-W-2.0
# protocol_processor_top end-to-end suite — exit 0 = PASS.
#
# Five builds of one bench. The first overrides nothing and runs every
# section; the second overrides P-SRP-DOM-DEF-VID with SRP_VID_FIXTURE and
# runs section DV alone (README). The SRP engine's own default is 2 as well,
# so only a value the child cannot produce shows a dropped or misbound
# top-level connection. The third sets P-EN-IDENTIFY-NOTIFICATION to 1 and
# runs section ID alone; the first grades the default 0 (section ID0). The
# fourth overrides DESC_LINE_BYTES_P with LINE_FIXTURE and runs section AX
# alone, so its response writes are bounded by a reservation that is not the
# default's. The fifth runs the timebase at the nominal clock's rate and
# section TB alone. Each binary prints its own build's count; the last line is
# the one canonical tally, summed over the five builds.
VERILATOR ?= verilator
HDL        = ../../hdl
TOP        = pp_top_wrap
# verification only: Milan 4.2.7.2.1 fixes the product value at 2. README
# section DV says why this pattern
SRP_VID_FIXTURE = 5A3C
# verification only: a legal non-default line (576..1008 in steps of 8) whose
# 16 + line reservation is not a multiple of 16 (README section AX, RB)
LINE_FIXTURE = 584
MUTANT_OUTPUT ?= /tmp/aecp-mutants
AECP_DISPATCH_MUTANT_OUTPUT ?= /tmp/aecp-dispatch-mutants
CTR_MUTANT_OUTPUT ?= /tmp/ctr-mutants
.DEFAULT_GOAL := run

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

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
       $(HDL)/packet_engine/KL_pp_nvm_mgr_arb.sv \
       $(HDL)/aecp/ucpu_pkg.sv $(HDL)/aecp/KL_aecp_ucpu.sv \
       $(HDL)/aecp/KL_aecp_desc_mem_guard.sv \
       $(HDL)/aecp/KL_aecp_desc_store.sv $(HDL)/aecp/KL_aecp_dyn_state.sv $(HDL)/aecp/KL_aecp_engine.sv $(HDL)/aecp/KL_aecp_resp_buf.sv \
       $(HDL)/aecp/KL_aecp_nvm_writer.sv \
       $(HDL)/aecp/KL_aecp_notify.sv $(HDL)/aecp/KL_aecp_ca_originator.sv \
       $(HDL)/adp/KL_adp_engine.sv \
       $(HDL)/acmp/KL_pp_acmp_listener.sv $(HDL)/acmp/KL_acmp_talker.sv \
       $(HDL)/acmp/KL_pp_acmp_lsn_admit.sv \
       $(HDL)/acmp/KL_acmp_nvm_shadow.sv \
       $(HDL)/maap/KL_pp_maap.sv \
       $(HDL)/srp/KL_srp_decoder.sv $(HDL)/srp/KL_srp_domain.sv \
       $(HDL)/srp/KL_srp_vlan.sv $(HDL)/srp/KL_srp_admission.sv \
       $(HDL)/srp/KL_srp_talker_fsm.sv $(HDL)/srp/KL_srp_listener_fsm.sv \
       $(HDL)/srp/KL_srp_encoder.sv $(HDL)/srp/KL_srp_top.sv \
       $(HDL)/top/KL_mrp_strip.sv $(HDL)/top/protocol_processor_top.sv \
       pp_top_wrap.sv
CPP  = sim_main.cpp
LINE_VFLAGS = "+define+PP_TOP_DESC_LINE_BYTES=$(LINE_FIXTURE)" \
              -CFLAGS "-DPP_TOP_DESC_LINE_BYTES=$(LINE_FIXTURE) -Wall -Wextra" \
              --Mdir obj_line

gsi-build: ltn_rom.hex ucode.hex
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vpp_top_sim

name-writes: gsi-build
	./obj_dir/Vpp_top_sim --name-writes-only

gsi-internal: gsi-build
	./obj_dir/Vpp_top_sim --gsi-internal-only

# the MP section alone (the tb/maap mutation campaign's pp_top arms)
maap-internal: gsi-build
	./obj_dir/Vpp_top_sim --maap-internal-only

adp-config: gsi-build
	./obj_dir/Vpp_top_sim --adp-only

# the third build alone (section ID), for the identify mutation campaign
identify-build: ltn_rom.hex ucode.hex
	@mkdir -p obj_dir
	$(VERILATOR) $(VFLAGS) "+define+PP_TOP_EN_IDENT" \
	    -CFLAGS "-DPP_TOP_EN_IDENT -Wall -Wextra" \
	    --Mdir obj_idn $(SRCS) $(CPP) -o Vpp_top_idn

identify: identify-build gsi-build
	./obj_idn/Vpp_top_idn
	./obj_dir/Vpp_top_sim --identify-only

# A5b and M9 alone on a booted model (the main run holds both in its own
# timeline): the fast target aecp_dispatch_mutants.py plants its arms against
aecp-dispatch: gsi-build
	./obj_dir/Vpp_top_sim --aecp-dispatch-only

# section AX alone in the line build (aecp_dispatch_mutants.py's line arms); its
# tally goes where the full run's builds put theirs
line-build: ltn_rom.hex ucode.hex
	$(VERILATOR) $(VFLAGS) $(LINE_VFLAGS) $(SRCS) $(CPP) -o Vpp_top_line

aecp-line: line-build
	@mkdir -p obj_dir
	./obj_line/Vpp_top_line

# the AECP dispatch-and-response mutation campaign (README section
# "AECP dispatch and response negative controls"); every arm is a patch in
# aecp_dispatch_mutations/, planted in a scratch copy
aecp-dispatch-mutants:
	python3 aecp_dispatch_mutants.py --output "$(AECP_DISPATCH_MUTANT_OUTPUT)" \
	    --verilator "$(VERILATOR)"

deadline: gsi-build
	./obj_dir/Vpp_top_sim --deadline-only

d3: gsi-build
	./obj_dir/Vpp_top_sim --d3-only

hazards: gsi-build
	./obj_dir/Vpp_top_sim --hazards-only

# section K's AVB_INTERFACE and CLOCK_DOMAIN half (K9 to K17) alone, on its
# fresh model: the target ctr_mutants.py plants its arms against
counters: gsi-build
	./obj_dir/Vpp_top_sim --counters-only

# lane C7's campaign (issues #44, #79): reviewed patches in ctr_mutations/,
# each applied to a scratch copy, each required to fail its named check
ctr-mutants:
	python3 ctr_mutants.py --output "$(CTR_MUTANT_OUTPUT)"

# lane C5a's campaign (issues #81, #57, #84): reviewed patches in mutations/,
# each applied to a scratch copy, each required to fail its named check
aecp-mutants:
	python3 aecp_mutants.py --output "$(MUTANT_OUTPUT)"

run: fixture-guards line-guards ltn_rom.hex ucode.hex
	@rm -f obj_dir/build_tally.txt
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vpp_top_sim
	./obj_dir/Vpp_top_sim
	$(VERILATOR) $(VFLAGS) "+define+PP_TOP_SRP_DOM_DEF_VID=16'h$(SRP_VID_FIXTURE)" \
	    -CFLAGS "-DPP_TOP_SRP_DOM_DEF_VID=0x$(SRP_VID_FIXTURE) -Wall -Wextra" \
	    --Mdir obj_vid $(SRCS) $(CPP) -o Vpp_top_vid
	./obj_vid/Vpp_top_vid
	$(VERILATOR) $(VFLAGS) "+define+PP_TOP_EN_IDENT" \
	    -CFLAGS "-DPP_TOP_EN_IDENT -Wall -Wextra" \
	    --Mdir obj_idn $(SRCS) $(CPP) -o Vpp_top_idn
	./obj_idn/Vpp_top_idn
	$(VERILATOR) $(VFLAGS) $(LINE_VFLAGS) $(SRCS) $(CPP) -o Vpp_top_line
	./obj_line/Vpp_top_line
	$(MAKE) budget
	@awk '{c += $$1; f += $$2} END {if (NR != 5) {print "FAIL: " NR " build tallies, expected 5"; exit 1} printf "%d checks: %d PASS, %d FAIL\n", c, c - f, f}' \
	    obj_dir/build_tally.txt

# The fifth build (section TB): the wrap's timebase at the nominal clock's
# own rate, 1 ms = 1,000 clocks, so no budget measurement is cut by the
# compressed AECP deadline. Its tally lands beside the other four.
budget-build: ltn_rom.hex ucode.hex
	$(VERILATOR) $(VFLAGS) "+define+PP_TOP_TIM_REAL" \
	    -CFLAGS "-DPP_TOP_TIM_REAL -Wall -Wextra" \
	    --Mdir obj_tim $(SRCS) $(CPP) -o Vpp_top_tim

budget: budget-build
	@mkdir -p obj_dir
	./obj_tim/Vpp_top_tim

# Compile the actual bench with four fixture settings in a temporary model dir.
# No simulation or source mutation; bad fixtures must fail for the named guards.
fixture-guards: fixture-guards-test
	@python3 fixture_guards.py --verilator "$(VERILATOR)" --cxx "$(CXX)" -- \
	    $(filter-out --exe --build,$(VFLAGS)) $(SRCS)

fixture-guards-test:
	@python3 -B -m unittest -v test_fixture_guards.py

# Lint the real top at the edges of the legal DESC_LINE_BYTES_P range and one
# step outside each; the refusals must name the parameter (README section AX)
line-guards:
	@python3 line_guards.py --verilator "$(VERILATOR)" -- $(filter-out pp_top_wrap.sv,$(SRCS))

ltn_rom.hex: $(HDL)/acmp/rom/gen_ltn_rom.py
	python3 $< -o $@

ucode.hex: $(HDL)/aecp/ucode/gen_ucode.py
	python3 $< -o $@

clean:
	rm -rf obj_dir obj_vid obj_idn obj_line obj_tim ltn_rom.hex ucode.hex

.PHONY: name-writes run gsi-build gsi-internal maap-internal adp-config aecp-dispatch \
        line-build aecp-line aecp-dispatch-mutants identify-build identify \
        deadline d3 hazards counters budget budget-build \
        aecp-mutants ctr-mutants fixture-guards fixture-guards-test line-guards clean
