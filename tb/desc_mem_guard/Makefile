# SPDX-License-Identifier: CERN-OHL-W-2.0
VERILATOR ?= verilator
HDL = ../../hdl
TOP = desc_mem_guard_wrap
OBJ_DIR ?= obj_dir
GUARD_SRC ?= $(HDL)/aecp/KL_aecp_desc_mem_guard.sv
DEFINES ?=
ARGS ?=
SRCS = $(HDL)/aecp/KL_aecp_desc_store.sv $(GUARD_SRC) wrap.sv

run:
	$(VERILATOR) --cc --exe --build -j 2 --top-module $(TOP) \
	  --Mdir $(OBJ_DIR) $(DEFINES) -Wall -Wno-DECLFILENAME \
	  -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
	  -CFLAGS "-std=c++17 -O2 -Wall -Wextra" $(SRCS) sim_main.cpp -o Vguard_sim
	./$(OBJ_DIR)/Vguard_sim $(ARGS)

baseline:
	$(MAKE) run GUARD_SRC= DEFINES=+define+DESC_GUARD_BASELINE \
	  OBJ_DIR=obj_baseline ARGS=--late-only

clean:
	rm -rf obj_dir obj_baseline obj_mutant

.PHONY: run baseline clean
