# SPDX-License-Identifier: CERN-OHL-W-2.0
# AECP registry monitor lifecycle suite, exit 0 = PASS.
#
# Two builds of one bench. The first overrides nothing and runs the registry
# monitor lifecycle; the second sets P-EN-IDENTIFY-NOTIFICATION to 1 and runs
# section FT, the identify schedule at the full timebase (README). Each binary
# prints its own build's count; the last line is the one canonical tally,
# summed over both builds.
VERILATOR ?= verilator
HDL        = ../../hdl
TOP        = KL_aecp_notify
.DEFAULT_GOAL := run

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -GN_CTRL_P=2 -GN_STREAM_IN_P=1 -GN_STREAM_OUT_P=1 \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/common/pp_pkg.sv $(HDL)/aecp/KL_aecp_notify.sv
CPP  = sim_main.cpp

# the second build alone (section FT), for the identify mutation campaign
identify-build:
	@mkdir -p obj_dir
	$(VERILATOR) $(VFLAGS) -GEN_IDENTIFY_NOTIF_P=1 \
	    -CFLAGS "-DAECP_NOTIFY_IDENT -Wall -Wextra" \
	    --Mdir obj_idn $(SRCS) $(CPP) -o Vaecp_notify_idn

identify: identify-build
	./obj_idn/Vaecp_notify_idn

run:
	@rm -f obj_dir/build_tally.txt
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vaecp_notify_sim
	./obj_dir/Vaecp_notify_sim
	$(VERILATOR) $(VFLAGS) -GEN_IDENTIFY_NOTIF_P=1 \
	    -CFLAGS "-DAECP_NOTIFY_IDENT -Wall -Wextra" \
	    --Mdir obj_idn $(SRCS) $(CPP) -o Vaecp_notify_idn
	./obj_idn/Vaecp_notify_idn
	@awk '{c += $$1; f += $$2} END {if (NR != 2) {print "FAIL: " NR " build tallies, expected 2"; exit 1} printf "%d checks: %d PASS, %d FAIL\n", c, c - f, f}' \
	    obj_dir/build_tally.txt

clean:
	rm -rf obj_dir obj_idn

.PHONY: run identify-build identify clean
