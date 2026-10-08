# SPDX-License-Identifier: CERN-OHL-W-2.0
# AECP registry monitor lifecycle suite, exit 0 = PASS.
#
# Three builds of one bench. The first overrides nothing and runs the registry
# monitor lifecycle; the second sets P-EN-IDENTIFY-NOTIFICATION to 1 and runs
# section FT, the identify schedule at the full timebase; the third sets
# P-N-AVB-INTERFACES to 2 and runs sections PT and CK, the registry port and
# the per-interface counter rows (README). Each binary prints its own build's
# count; the default suite adds SC1 as a fourth run and sums all four.
VERILATOR ?= verilator
HDL        = ../../hdl
TOP        = KL_aecp_notify
.DEFAULT_GOAL := check

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -GN_CTRL_P=2 -GN_STREAM_IN_P=1 -GN_STREAM_OUT_P=1 \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/common/pp_pkg.sv $(HDL)/aecp/KL_aecp_notify.sv
CPP  = sim_main.cpp
IF2_VFLAGS = -GN_IF_P=2 -CFLAGS "-DAECP_NOTIFY_IF2 -Wall -Wextra" --Mdir obj_if2

# the second build alone (section FT), for the identify mutation campaign
identify-build:
	@mkdir -p obj_dir
	$(VERILATOR) $(VFLAGS) -GEN_IDENTIFY_NOTIF_P=1 \
	    -CFLAGS "-DAECP_NOTIFY_IDENT -Wall -Wextra" \
	    --Mdir obj_idn $(SRCS) $(CPP) -o Vaecp_notify_idn

identify: identify-build
	./obj_idn/Vaecp_notify_idn

# the third build alone (sections PT and CK), for the notify mutation campaign
interfaces-build:
	@mkdir -p obj_dir
	$(VERILATOR) $(VFLAGS) $(IF2_VFLAGS) $(SRCS) $(CPP) -o Vaecp_notify_if2

interfaces: interfaces-build
	./obj_if2/Vaecp_notify_if2

run:
	@rm -f obj_dir/build_tally.txt
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vaecp_notify_sim
	./obj_dir/Vaecp_notify_sim
	$(VERILATOR) $(VFLAGS) -GEN_IDENTIFY_NOTIF_P=1 \
	    -CFLAGS "-DAECP_NOTIFY_IDENT -Wall -Wextra" \
	    --Mdir obj_idn $(SRCS) $(CPP) -o Vaecp_notify_idn
	./obj_idn/Vaecp_notify_idn
	$(VERILATOR) $(VFLAGS) $(IF2_VFLAGS) $(SRCS) $(CPP) -o Vaecp_notify_if2
	./obj_if2/Vaecp_notify_if2
	@awk '{c += $$1; f += $$2} END {if (NR != 3) {print "FAIL: " NR " build tallies, expected 3"; exit 1} printf "%d checks: %d PASS, %d FAIL\n", c, c - f, f}' \
	    obj_dir/build_tally.txt

# The existing run target retains its three mutation records. The default
# suite also runs SC1 on the same one-interface binary and sums all four runs.
collision:
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vaecp_notify_sim
	./obj_dir/Vaecp_notify_sim --cancel-collision-only

check:
	$(MAKE) run
	./obj_dir/Vaecp_notify_sim --cancel-collision-only
	@awk '{c += $$1; f += $$2} END {if (NR != 4) {print "FAIL: " NR " run tallies, expected 4"; exit 1} printf "%d checks: %d PASS, %d FAIL\n", c, c - f, f}' \
	    obj_dir/build_tally.txt

clean:
	rm -rf obj_dir obj_idn obj_if2

.PHONY: check collision run identify-build identify interfaces-build interfaces clean
