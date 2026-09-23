# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_acmp_nvm_shadow suite — exit 0 = PASS.
#
# `pinned` is NOT part of the suite's run: it builds the wrap without the
# listener admission gate (the top's wiring before issue #92) and runs the
# same bench, whose L group must then report the recorded L05 binding loss.
# Its non-zero exit is the expected result (README, "Pinned wiring").
VERILATOR ?= verilator
HDL        = ../../hdl
TOP        = acmp_nvm_wrap

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -GDEB_TICKS_P=50 \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/common/pp_pkg.sv $(HDL)/acmp/pp_acmp_pkg.sv \
       $(HDL)/acmp/KL_pp_acmp_listener.sv \
       $(HDL)/acmp/KL_pp_acmp_lsn_admit.sv \
       $(HDL)/acmp/KL_acmp_nvm_shadow.sv \
       $(HDL)/packet_engine/KL_pp_nvm_port.sv \
       acmp_nvm_wrap.sv
CPP  = sim_main.cpp

run: ltn_rom.hex
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vacmp_nvm_sim
	./obj_dir/Vacmp_nvm_sim

pinned: ltn_rom.hex
	$(VERILATOR) $(VFLAGS) +define+ACMP_NVM_PINNED_WIRING \
	    -CFLAGS "-DACMP_NVM_PINNED_WIRING" --Mdir obj_pinned \
	    $(SRCS) $(CPP) -o Vacmp_nvm_pinned
	./obj_pinned/Vacmp_nvm_pinned

ltn_rom.hex: $(HDL)/acmp/rom/gen_ltn_rom.py
	python3 $< -o $@

clean:
	rm -rf obj_dir obj_pinned ltn_rom.hex

.PHONY: run pinned clean
