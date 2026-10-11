# SPDX-License-Identifier: CERN-OHL-W-2.0
# tb/aecp_cmds - KL_aecp_engine's decode and dispatch, observed, held to the
# AECP command table (issue #73) — exit 0 = PASS.
#
# `run` (the suite) builds the engine alone, sweeps it (sim_main.cpp) into
# obj_dir/observed.json, and has scripts/check_aecp_commands.py hold that to
# the table and to the recorded decode.json beside this file; the gate prints
# the tally. `record` rewrites decode.json from a fresh sweep. `mutants` plants
# each standing control of mutants.py in a scratch copy and requires its named
# failure (README).
VERILATOR ?= verilator
HDL        = ../../hdl
TOP        = aecp_cmds_wrap
GATE       = ../../scripts/check_aecp_commands.py
MUTANT_OUTPUT ?= /tmp/aecp-cmds-mutants
.DEFAULT_GOAL := run

VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM -Wno-PINCONNECTEMPTY \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/common/pp_pkg.sv $(HDL)/aecp/ucpu_pkg.sv \
       $(HDL)/aecp/KL_aecp_ucpu.sv $(HDL)/aecp/KL_aecp_desc_store.sv \
       $(HDL)/aecp/KL_aecp_dyn_state.sv $(HDL)/aecp/KL_aecp_nvm_writer.sv \
       $(HDL)/aecp/KL_aecp_resp_buf.sv $(HDL)/aecp/KL_aecp_engine.sv \
       aecp_cmds_wrap.sv
CPP  = sim_main.cpp

run: obj_dir/observed.json
	python3 -B $(GATE) --observed obj_dir/observed.json

record: obj_dir/observed.json
	python3 -B $(GATE) --observed obj_dir/observed.json --write-record

obj_dir/observed.json: build
	./obj_dir/Vaecp_cmds obj_dir/observed.json

build: ucode.hex image.bin
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vaecp_cmds

# the table is a generator input (status codes, the MVU command_type, the
# dispatch entries), so it is a prerequisite beside the generator itself
ucode.hex: $(HDL)/aecp/ucode/gen_ucode.py $(HDL)/aecp/ucode/aecp_commands.json
	python3 -B $< -o $@

# the semantic lint's positive model: the D3 restore must prove an image
# before the engine takes a command
image.bin: $(HDL)/aecp/desc/gen_desc_image.py $(HDL)/aecp/desc/model_lint.py \
           $(HDL)/aecp/desc/model_rules.py $(HDL)/aecp/desc/milan_min.json
	python3 -B $< -i $(HDL)/aecp/desc/milan_min.json -o $@ >/dev/null

mutants:
	python3 -B mutants.py --output "$(MUTANT_OUTPUT)" --verilator "$(VERILATOR)"

clean:
	rm -rf obj_dir ucode.hex image.bin

.PHONY: run record build mutants clean
