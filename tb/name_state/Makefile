# SPDX-License-Identifier: CERN-OHL-W-2.0
VERILATOR ?= verilator
OUTPUT ?= /tmp/name-state-mutants

.PHONY: run mutants
run:
	python3 run.py --verilator "$(VERILATOR)"

mutants:
	python3 mutants.py --verilator "$(VERILATOR)" --jobs 2 --output "$(OUTPUT)"
