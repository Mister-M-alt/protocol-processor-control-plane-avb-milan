# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_pp_acmp_lsn_admit suite, exit 0 = PASS.
VERILATOR ?= verilator
HDL        = ../../hdl/acmp
TOP        = KL_pp_acmp_lsn_admit

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/KL_pp_acmp_lsn_admit.sv
CPP  = sim_main.cpp

run:
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vlsn_admit_sim
	./obj_dir/Vlsn_admit_sim

clean:
	rm -rf obj_dir

.PHONY: run clean
