# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_pp_nvm_port suite — exit 0 = PASS.
VERILATOR ?= verilator
HDL        = ../../hdl/packet_engine
TOP        = KL_pp_nvm_port

# The suite runs at three legal values of the port's deadline, MEM_TIMEOUT_CYC_P.
# TMO is the bound every figure in the README is measured at; TMO_ALT is a
# second one, small and odd, where a harness wait written for 100 instead of
# derived from the bound would fail; TMO_MIN is the smallest the suite's fixed
# protocol delays leave legal. Each build hands its value to the C++ as
# NVM_PORT_TMO, the harness's TMO. The randomized harness, fuzz_main.cpp, is
# built at each bound in FUZZ_TMOS: the port's smallest legal bounds, which the
# suite cannot reach, and 37 beside the suite. The suite is built at the top's
# MAX_PAYLOAD_P; the randomized harness at FUZZ_MAXP, the parameter's largest
# legal value, which it asks the port for rather than reads from here.
TMO       = 100
TMO_ALT   = 37
TMO_MIN   = 20
FUZZ_TMOS = 1 2 3 37
MAXP      = 1024
FUZZ_MAXP = 65527

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/KL_pp_nvm_port.sv
CPP  = sim_main.cpp

# one build of the suite at deadline $(1) into $(2), then its run
suite = $(VERILATOR) $(VFLAGS) -GMAX_PAYLOAD_P=$(MAXP) -GMEM_TIMEOUT_CYC_P=$(1) -CFLAGS "-DNVM_PORT_TMO=$(1) -Wall -Wextra" \
            --Mdir $(2) $(SRCS) $(CPP) -o Vnvm_port_sim && ./$(2)/Vnvm_port_sim

# one build of the randomized harness at deadline $(1), then its run
fuzz_at = $(VERILATOR) $(VFLAGS) -GMAX_PAYLOAD_P=$(FUZZ_MAXP) -GMEM_TIMEOUT_CYC_P=$(1) -CFLAGS "-DNVM_PORT_TMO=$(1) -Wall -Wextra" \
              --Mdir obj_fz$(1) $(SRCS) fuzz_main.cpp -o Vnvm_port_fuzz && ./obj_fz$(1)/Vnvm_port_fuzz

BUILDS = $(words 1 2 3 $(FUZZ_TMOS))

# Each build prints its own tally and adds it to obj_dir/build_tally.txt; the
# last line printed is the sum, which is the line run_suites.sh reads.
run: elab
	@mkdir -p obj_dir && rm -f obj_dir/build_tally.txt
	$(call suite,$(TMO),obj_dir)
	$(call suite,$(TMO_ALT),obj_alt)
	$(call suite,$(TMO_MIN),obj_min)
	$(foreach t,$(FUZZ_TMOS),$(call fuzz_at,$(t)) && ) true
	@awk '{c += $$1; f += $$2} END {if (NR != $(BUILDS)) {print "FAIL: " NR " build tallies, expected $(BUILDS)"; exit 1} printf "%d checks: %d PASS, %d FAIL\n", c, c - f, f; exit (f != 0)}' \
	    obj_dir/build_tally.txt

# The build at TMO alone: what the figures gate measures every row against.
primary:
	@mkdir -p obj_dir
	$(call suite,$(TMO),obj_dir)

# The randomized harness alone, at every bound in FUZZ_TMOS.
fuzz:
	@mkdir -p obj_dir
	$(foreach t,$(FUZZ_TMOS),$(call fuzz_at,$(t)) && ) true

# the port's two elaboration guards (elab_bounds.sh): MEM_TIMEOUT_CYC_P refused
# at 0, 2^31 and 2^32 - 1 by name, built at 1 and 2^31 - 1; MAX_PAYLOAD_P
# refused at 65528, 65535 and 2^32 - 1 by name and bound, built at 1024 and 65527
elab:
	@VERILATOR=$(VERILATOR) ./elab_bounds.sh

clean:
	rm -rf obj_dir obj_alt obj_min obj_fz*

.PHONY: run primary fuzz elab clean

# Re-measure every figure the README quotes and refuse to agree with a drifted
# one. About four minutes; it prints its own derived build count, which is not
# repeated here because this number was wrong three rounds running. NOT part of `run` --
# too slow for the per-suite loop -- but it IS gated in CI, because "run it by
# hand after a change" is the discipline that let these figures go stale in four
# consecutive review rounds.
figures:
	@python3 $(CURDIR)/measure_figures.py --check

.PHONY: figures
