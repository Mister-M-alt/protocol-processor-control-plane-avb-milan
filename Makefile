# SPDX-License-Identifier: CERN-OHL-W-2.0
# Diagram regeneration + documentation lint.
# Targets: diagrams (drawio -> svg), lint (mermaid+wavedrom blocks), stale (export freshness),
# ids (P-/T- IDs against F01.5/F08.1), figures (docs/diagrams/ file classes),
# reqtags (REQ rows against the tags in tb/).

DRAWIO      ?= drawio
DRAWIO_SRC  := $(wildcard docs/diagrams/src/*.drawio)
DRAWIO_SVG  := $(patsubst docs/diagrams/src/%.drawio,docs/diagrams/%.svg,$(DRAWIO_SRC))

.PHONY: all check diagrams wavedrom wavedrom-check lint links matrix reqtags modmatrix params ids figures stale
all: diagrams check

# everything the CI docs-gates job enforces: it runs this target (see
# docs/architecture/09_verification.md section 7)
check: lint wavedrom-check links matrix reqtags modmatrix params ids figures stale

diagrams: $(DRAWIO_SVG) wavedrom

# render every embedded ```wavedrom block to its committed SVG (GitHub cannot
# render WaveDrom natively; bootstraps .venv-wavedrom on first run)
wavedrom:
	@python3 scripts/render-wavedrom.py

wavedrom-check:
	@python3 scripts/render-wavedrom.py --check

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

# every REQ row whose Ver needs a check has a tagged check of that category in
# tb/ or a waiver naming its GAP (09 section 8.10, the static half; the CI
# suites job runs the executed half); the self-test plants each fault first
reqtags:
	@python3 scripts/check-req-tags.py --selftest
	@python3 scripts/check-req-tags.py

params:
	@python3 scripts/check-integrator-params.py

# every P- or T- ID used under docs/, hdl/ or tb/ has its F01.5 or F08.1 row;
# the self-test plants strays first, so a gate that cannot fail does not pass
ids:
	@python3 scripts/check-ids.py --selftest
	@python3 scripts/check-ids.py

# every file under docs/diagrams/ is draw.io, WaveDrom or a listed hand-authored SVG
figures:
	@python3 scripts/check-figures.py --selftest
	@python3 scripts/check-figures.py

# the module<->testbench matrix is GENERATED; drift and an untested module both fail
modmatrix:
	@python3 scripts/gen_matrix.py --check

# Staleness must be meaningful on a fresh checkout too: git does not preserve
# mtimes, so committed files are compared by last-commit time; the mtime test
# only applies when the SOURCE has uncommitted edits (where mtime is truth).
stale:
	@fail=0; \
	for src in $(DRAWIO_SRC); do \
	  svg="docs/diagrams/$$(basename $${src%.drawio}).svg"; \
	  if [ ! -f "$$svg" ]; then echo "STALE: $$svg (missing)"; fail=1; continue; fi; \
	  if git diff --quiet -- "$$src" 2>/dev/null; then \
	    st=$$(git log -1 --format=%ct -- "$$src"); \
	    gt=$$(git log -1 --format=%ct -- "$$svg"); \
	    if [ -n "$$st" ] && [ -n "$$gt" ] && [ "$$st" -gt "$$gt" ]; then \
	      echo "STALE: $$svg (source committed after the export)"; fail=1; \
	    fi; \
	  elif [ "$$src" -nt "$$svg" ]; then \
	    echo "STALE: $$svg (uncommitted source edit newer than the export)"; fail=1; \
	  fi; \
	done; exit $$fail
