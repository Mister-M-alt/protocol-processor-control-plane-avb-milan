# SPDX-License-Identifier: CERN-OHL-W-2.0
# KL_aecp_desc_store suite — exit 0 = PASS.
VERILATOR ?= verilator
HDL        = ../../hdl
TOP        = KL_aecp_desc_store

# MEM_TIMEOUT_CYC_P is compressed from its 4096 default so the "no bridge at
# all" arm runs in a few hundred clocks instead of tens of thousands; the BFM's
# 24-clock first-word latency stays well inside it.
VFLAGS = --cc --exe --build -j 0 \
         --top-module $(TOP) \
         -GMEM_TIMEOUT_CYC_P=64 \
         -GNAME_ENTRIES_P=256 \
         -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
         -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM \
         -CFLAGS "-std=c++17 -O2 -I$(CURDIR) -Wall -Wextra"

SRCS = $(HDL)/aecp/KL_aecp_desc_store.sv
CPP  = sim_main.cpp

run: generator-check image.bin
	$(VERILATOR) $(VFLAGS) $(SRCS) $(CPP) -o Vdesc_store_sim
	./obj_dir/Vdesc_store_sim

# the generator's own worked example — proves generator and RTL agree on the
# documented format, not merely on each other. It is a layout vector, not a
# Milan model, so it packs with the semantic lint off (milan_min.json is the
# lint's positive case; generator-check packs it).
image.bin: $(HDL)/aecp/desc/gen_desc_image.py $(HDL)/aecp/desc/model_lint.py \
           $(HDL)/aecp/desc/model_rules.py $(HDL)/aecp/desc/example_milan_8.json
	python3 -B $(HDL)/aecp/desc/gen_desc_image.py --no-lint \
	  -i $(HDL)/aecp/desc/example_milan_8.json -o $@ -m image.map >/dev/null

# the packer's self-test gate: layout refusals, the semantic lint (one
# negative case per check, derived from milan_min.json), waivers, the ADP
# report and the recorded model digest
generator-check:
	python3 -B test_gen_desc_image.py

# the gate's check-suppression proof: each lint check, its findings dropped
# alone, must make generator-check fail (README, "Mutation proof")
lint-suppression:
	python3 -B lint_suppression.py --jobs 8

clean:
	rm -rf obj_dir image.bin image.map

.PHONY: run generator-check lint-suppression clean
