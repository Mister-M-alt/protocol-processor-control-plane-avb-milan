# SPDX-License-Identifier: CERN-OHL-W-2.0
# Elaboration-guard suite - exit 0 = PASS. No model is built: every case is one
# elaboration. Each guard in hdl/ must be refused, as a $fatal at its own line,
# by Verilator lint and, when sv2v and yosys are on PATH, by Yosys; its nearest
# passing case must elaborate clean; and no severity task in hdl/ may be missing
# from guards.py (README).
VERILATOR ?= verilator
JOBS ?= 4
MUTANT_OUTPUT ?= /tmp/elab-guard-mutants

run:
	python3 -B guards.py --verilator "$(VERILATOR)" --jobs $(JOBS)

# The two Vivado legs, xelab and synth_design -rtl, for whoever has Vivado on
# PATH; slow (one synthesis elaboration per case), so not part of `run`
vivado:
	python3 -B guards.py --verilator "$(VERILATOR)" --jobs $(JOBS) \
	    --frontend xsim --frontend synth

# The bench's own proof: every guard's $error, $warning and initial-block form,
# and one defect per other check, each planted in scratch and failed by name
mutants:
	python3 -B mutants.py --verilator "$(VERILATOR)" --output "$(MUTANT_OUTPUT)" \
	    --jobs $(JOBS)

.PHONY: run vivado mutants
