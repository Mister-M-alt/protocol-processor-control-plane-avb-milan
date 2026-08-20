# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_pp_nvm_port suite — exit 0 = PASS.
VERILATOR ?= verilator
HDL        = ../../hdl/packet_engine
TOP        = KL_pp_nvm_port

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -GMAX_PAYLOAD_P=1024 \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR)"

SRCS = $(HDL)/KL_pp_nvm_port.sv
CPP  = sim_main.cpp

run:
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vnvm_port_sim
	./obj_dir/Vnvm_port_sim

clean:
	rm -rf obj_dir

.PHONY: run clean

# Re-measure every figure the README quotes and refuse to agree with a drifted
# one. NOT part of `run`: it rebuilds the suite once per arm plus once per named
# mutation, about seventeen builds. Run it after ANY change to this suite --
# four review rounds found the figures stale, and each time the rows that had
# been corrected were the ones somebody thought to check.
figures:
	@python3 $(CURDIR)/measure_figures.py --check

.PHONY: figures
