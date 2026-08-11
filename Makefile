# SPDX-License-Identifier: CERN-OHL-W-2.0
# Diagram regeneration + documentation lint.
# Targets: diagrams (drawio -> svg), lint (mermaid+wavedrom blocks), stale (export freshness).

DRAWIO      ?= drawio
DRAWIO_SRC  := $(wildcard docs/diagrams/src/*.drawio)
DRAWIO_SVG  := $(patsubst docs/diagrams/src/%.drawio,docs/diagrams/%.svg,$(DRAWIO_SRC))

.PHONY: all check diagrams lint links matrix stale
all: diagrams check

# everything CI should enforce (see docs/architecture/09_verification.md section 7)
check: lint links matrix stale

diagrams: $(DRAWIO_SVG)

docs/diagrams/%.svg: docs/diagrams/src/%.drawio
	@$(DRAWIO) -x -f svg --crop -o $@ $< 2>/dev/null \
	  || xvfb-run -a $(DRAWIO) --no-sandbox -x -f svg --crop -o $@ $<
	@echo "exported $@"

lint:
	@./scripts/lint-diagrams.sh

links:
	@python3 scripts/check-links.py

matrix:
	@python3 scripts/check-matrix.py

stale:
	@fail=0; \
	for src in $(DRAWIO_SRC); do \
	  svg="docs/diagrams/$$(basename $${src%.drawio}).svg"; \
	  if [ ! -f "$$svg" ] || [ "$$src" -nt "$$svg" ]; then \
	    echo "STALE: $$svg (regenerate from $$src)"; fail=1; \
	  fi; \
	done; exit $$fail
