# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_adp_engine suite — exit 0 = PASS.
#
# Two builds of one top. The first overrides nothing and runs the shipping
# shape (one interface, eight sinks, sim_main.cpp); the second sets
# P-N-AVB-INTERFACES to 2 and runs section IF, per-interface advertising
# (sim_if2.cpp, README). Each binary prints its own build's count; the last
# line is the one canonical tally, summed over both builds.
VERILATOR ?= verilator
MUTANT_OUTPUT ?= /tmp/adp-mutants
JOBS       ?= 4
HDL        = ../../hdl
TOP        = tb_adp_top
.DEFAULT_GOAL := run

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/common/pp_pkg.sv $(HDL)/adp/pp_adp_pkg.sv \
       $(HDL)/common/KL_pp_prng.sv $(HDL)/adp/KL_adp_engine.sv \
       tb_adp_top.sv
CPP  = sim_main.cpp
IF2_VFLAGS = -GN_IF_P=2 --Mdir obj_if2

# the second build alone (section IF), for the ADP mutation campaign
interfaces:
	@mkdir -p obj_dir
	$(VERILATOR) $(VFLAGS) $(IF2_VFLAGS) $(SRCS) sim_if2.cpp -o Vadp_if2
	./obj_if2/Vadp_if2

run:
	@rm -f obj_dir/build_tally.txt
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vadp_sim
	./obj_dir/Vadp_sim
	$(VERILATOR) $(VFLAGS) $(IF2_VFLAGS) $(SRCS) sim_if2.cpp -o Vadp_if2
	./obj_if2/Vadp_if2
	@awk '{c += $$1; f += $$2} END {if (NR != 2) {print "FAIL: " NR " build tallies, expected 2"; exit 1} printf "%d checks: %d PASS, %d FAIL\n", c, c - f, f}' \
	    obj_dir/build_tally.txt

mutants:
	python3 mutants.py --output "$(MUTANT_OUTPUT)" --jobs $(JOBS)

clean:
	rm -rf obj_dir obj_if2

.PHONY: run interfaces mutants clean
