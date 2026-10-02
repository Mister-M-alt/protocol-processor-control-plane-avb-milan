# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_pp_nvm_port suite — exit 0 = PASS.
VERILATOR ?= verilator
HDL        = ../../hdl/packet_engine
TOP        = KL_pp_nvm_port

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -GMAX_PAYLOAD_P=1024 -GMEM_TIMEOUT_CYC_P=100 \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/KL_pp_nvm_port.sv
CPP  = sim_main.cpp

run: elab
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vnvm_port_sim
	./obj_dir/Vnvm_port_sim

# MEM_TIMEOUT_CYC_P's elaboration guard: refused at 0, 2^31 and 2^32 - 1 by
# name, built at 1 and 2^31 - 1 (elab_bounds.sh)
elab:
	@VERILATOR=$(VERILATOR) ./elab_bounds.sh

clean:
	rm -rf obj_dir

.PHONY: run elab clean

# Re-measure every figure the README quotes and refuse to agree with a drifted
# one. About four minutes; it prints its own derived build count, which is not
# repeated here because this number was wrong three rounds running. NOT part of `run` --
# too slow for the per-suite loop -- but it IS gated in CI, because "run it by
# hand after a change" is the discipline that let these figures go stale in four
# consecutive review rounds.
figures:
	@python3 $(CURDIR)/measure_figures.py --check

.PHONY: figures
