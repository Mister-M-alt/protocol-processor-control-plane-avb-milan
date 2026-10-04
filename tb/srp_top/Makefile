# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_srp_top end-to-end engine suite — exit 0 = PASS.
VERILATOR ?= verilator
MUTANT_OUTPUT ?= /tmp/srp-leaveall-mutants
HDL        = ../../hdl
TOP        = srp_top_wrap

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

ENGINE = $(HDL)/common/pp_pkg.sv $(HDL)/srp/srp_pkg.sv \
         $(HDL)/common/KL_pp_prng.sv $(HDL)/common/KL_pp_timer_service.sv \
         $(HDL)/packet_engine/KL_pp_tx_slots.sv \
         $(HDL)/srp/KL_srp_decoder.sv $(HDL)/srp/KL_srp_domain.sv \
         $(HDL)/srp/KL_srp_vlan.sv $(HDL)/srp/KL_srp_talker_fsm.sv \
         $(HDL)/srp/KL_srp_listener_fsm.sv $(HDL)/srp/KL_srp_admission.sv \
         $(HDL)/srp/KL_srp_encoder.sv $(HDL)/srp/KL_srp_top.sv
SRCS = $(ENGINE) srp_top_wrap.sv
CPP  = sim_main.cpp

# The timer-arm FIFO arms (store_main.cpp, issue #230) at four shapes,
# sources x sinks. srp_store_wrap forces one state, test-only (its banner),
# which Verilator reports as a blocking write beside the block's nonblocking
# ones and as a second driver, and leaves the class-C/D faces it does not read
# unconnected; BLKANDNBLK, MULTIDRIVEN and PINCONNECTEMPTY are waived for that
# build alone.
STORE_SHAPES = 1x1 2x2 3x5 9x9
STORE_M      = $(word 1,$(subst x, ,$(SHAPE)))
STORE_N      = $(word 2,$(subst x, ,$(SHAPE)))
SFLAGS = --cc --exe --build -j 0 \
         --top-module srp_store_wrap \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -Wno-BLKANDNBLK -Wno-MULTIDRIVEN -Wno-PINCONNECTEMPTY \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

# `make` runs the timer-arm FIFO arms at every shape, then the end-to-end
# suite, whose tally is the last line (the one run_suites.sh reads). Every
# part runs even after a failing one; the exit status is 1 if any failed.
# RUN_ARGS=storage runs the FIFO arms alone; any other RUN_ARGS is a suite
# group (sim_main.cpp) and runs the suite alone.
all:
	@rc=0; \
	if [ -z "$(RUN_ARGS)" ] || [ "$(RUN_ARGS)" = storage ]; then \
	  for s in $(STORE_SHAPES); do $(MAKE) --no-print-directory storage SHAPE=$$s || rc=1; done; \
	fi; \
	if [ "$(RUN_ARGS)" != storage ]; then $(MAKE) --no-print-directory run || rc=1; fi; \
	exit $$rc

run:
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vsrp_top_sim
	./obj_dir/Vsrp_top_sim $(RUN_ARGS)

# one FIFO-arm shape: make storage SHAPE=<sources>x<sinks>
storage:
	$(VERILATOR) $(SFLAGS) -GN_SOURCES_P=$(STORE_M) -GN_SINKS_P=$(STORE_N) \
	  -CFLAGS "-DTB_SOURCES=$(STORE_M) -DTB_SINKS=$(STORE_N)" --Mdir obj_store_$(SHAPE) \
	  $(ENGINE) srp_store_wrap.sv store_main.cpp -o Vsrp_store_sim
	./obj_store_$(SHAPE)/Vsrp_store_sim

mutants:
	python3 mutants.py --output "$(MUTANT_OUTPUT)"

clean:
	rm -rf obj_dir $(addprefix obj_store_,$(STORE_SHAPES))

.PHONY: all run storage mutants clean
