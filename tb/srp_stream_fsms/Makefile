# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_srp_talker_fsm + KL_srp_listener_fsm suite — exit 0 = PASS.
VERILATOR ?= verilator
HDL        = ../../hdl
TOP        = srp_stream_fsms_wrap

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/common/pp_pkg.sv $(HDL)/srp/srp_pkg.sv \
       $(HDL)/srp/KL_srp_talker_fsm.sv $(HDL)/srp/KL_srp_listener_fsm.sv \
       srp_stream_fsms_wrap.sv
CPP  = sim_main.cpp

# The walk-record arms (walk_main.cpp, issue #230) at four shapes, sources x
# sinks: one and two contexts elaborate the walk's flop arm, three and nine
# its distributed-RAM arm. Every unreset memory starts at random contents.
# srp_walk_wrap leaves the faces these arms do not read unconnected, so
# PINCONNECTEMPTY is waived for that build alone.
WALK_SHAPES = 1x1 2x2 3x5 9x9
WALK_SRCS   = $(HDL)/common/pp_pkg.sv $(HDL)/srp/srp_pkg.sv \
              $(HDL)/srp/KL_srp_talker_fsm.sv $(HDL)/srp/KL_srp_listener_fsm.sv \
              srp_walk_wrap.sv
WALK_M      = $(word 1,$(subst x, ,$(SHAPE)))
WALK_N      = $(word 2,$(subst x, ,$(SHAPE)))
WFLAGS = --cc --exe --build -j 0 \
         --top-module srp_walk_wrap --x-initial unique \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM -Wno-PINCONNECTEMPTY \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

# `make` runs the walk-record arms at every shape, then the eight-stream
# suite, whose tally is the last line (the one run_suites.sh reads). Every
# part runs even after a failing one; the exit status is 1 if any failed.
# RUN_ARGS=walk runs the walk-record arms alone, RUN_ARGS=suite the suite.
all:
	@rc=0; \
	if [ "$(RUN_ARGS)" != suite ]; then \
	  for s in $(WALK_SHAPES); do $(MAKE) --no-print-directory walk SHAPE=$$s || rc=1; done; \
	fi; \
	if [ "$(RUN_ARGS)" != walk ]; then $(MAKE) --no-print-directory run || rc=1; fi; \
	exit $$rc

run:
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vsrp_stream_fsms_sim
	./obj_dir/Vsrp_stream_fsms_sim

# one walk-record shape: make walk SHAPE=<sources>x<sinks>
walk:
	$(VERILATOR) $(WFLAGS) -GN_SOURCES_P=$(WALK_M) -GN_SINKS_P=$(WALK_N) \
	  -CFLAGS "-DTB_SOURCES=$(WALK_M) -DTB_SINKS=$(WALK_N)" --Mdir obj_walk_$(SHAPE) \
	  $(WALK_SRCS) walk_main.cpp -o Vsrp_walk_sim
	./obj_walk_$(SHAPE)/Vsrp_walk_sim +verilator+rand+reset+2 +verilator+seed+230

clean:
	rm -rf obj_dir $(addprefix obj_walk_,$(WALK_SHAPES))

.PHONY: all run walk clean
