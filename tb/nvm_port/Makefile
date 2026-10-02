# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_pp_nvm_port suite — exit 0 = PASS.
VERILATOR ?= verilator
HDL        = ../../hdl/packet_engine
TOP        = KL_pp_nvm_port

# The suite runs at two legal values of the port's deadline, MEM_TIMEOUT_CYC_P.
# TMO is the bound every figure in the README is measured at; TMO_ALT is a
# second one, small and odd, where a harness wait written for 100 instead of
# derived from the bound would fail. Each build hands its value to the C++ as
# NVM_PORT_TMO, the harness's TMO.
TMO     = 100
TMO_ALT = 37

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -GMAX_PAYLOAD_P=1024 \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/KL_pp_nvm_port.sv
CPP  = sim_main.cpp

# one build of the suite at deadline $(1) into $(2), then its run
suite = $(VERILATOR) $(VFLAGS) -GMEM_TIMEOUT_CYC_P=$(1) -CFLAGS "-DNVM_PORT_TMO=$(1) -Wall -Wextra" \
            --Mdir $(2) $(SRCS) $(CPP) -o Vnvm_port_sim && ./$(2)/Vnvm_port_sim

# Each build prints its own tally and adds it to obj_dir/build_tally.txt; the
# last line printed is the sum, which is the line run_suites.sh reads.
run: elab
	@mkdir -p obj_dir && rm -f obj_dir/build_tally.txt
	$(call suite,$(TMO),obj_dir)
	$(call suite,$(TMO_ALT),obj_alt)
	@awk '{c += $$1; f += $$2} END {if (NR != 2) {print "FAIL: " NR " build tallies, expected 2"; exit 1} printf "%d checks: %d PASS, %d FAIL\n", c, c - f, f; exit (f != 0)}' \
	    obj_dir/build_tally.txt

# The build at TMO alone: what the figures gate measures every row against.
primary:
	@mkdir -p obj_dir
	$(call suite,$(TMO),obj_dir)

# MEM_TIMEOUT_CYC_P's elaboration guard: refused at 0, 2^31 and 2^32 - 1 by
# name, built at 1 and 2^31 - 1 (elab_bounds.sh)
elab:
	@VERILATOR=$(VERILATOR) ./elab_bounds.sh

clean:
	rm -rf obj_dir obj_alt

.PHONY: run primary elab clean

# Re-measure every figure the README quotes and refuse to agree with a drifted
# one. About four minutes; it prints its own derived build count, which is not
# repeated here because this number was wrong three rounds running. NOT part of `run` --
# too slow for the per-suite loop -- but it IS gated in CI, because "run it by
# hand after a change" is the discipline that let these figures go stale in four
# consecutive review rounds.
figures:
	@python3 $(CURDIR)/measure_figures.py --check

.PHONY: figures
