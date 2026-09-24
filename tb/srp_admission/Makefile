# SPDX-License-Identifier: CERN-OHL-W-2.0
VERILATOR ?= verilator
N ?= 8

run:
	$(VERILATOR) --cc --exe --build -j 8 --top-module srp_admission_wrap \
	  -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
	  -GN_SOURCES_P=$(N) --Mdir obj_$(N) \
	  -CFLAGS "-std=c++17 -O2 -Wall -Wextra -DTB_SOURCES=$(N)" \
	  ../../hdl/srp/KL_srp_admission.sv srp_admission_wrap.sv sim_main.cpp \
	  -o Vsrp_admission_sim
	./obj_$(N)/Vsrp_admission_sim

shapes:
	$(MAKE) run N=1
	$(MAKE) run N=2
	$(MAKE) run N=3
	$(MAKE) run N=5
	$(MAKE) run N=8

clean:
	rm -rf obj_1 obj_2 obj_3 obj_5 obj_8

.PHONY: run shapes clean
