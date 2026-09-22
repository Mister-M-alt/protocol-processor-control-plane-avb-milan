#!/usr/bin/env bash
# Run only the fixture gate command exactly as the Makefile builds it (make -n,
# continuation lines joined), skipping its fixture-guards-test prerequisite.
# Usage (in a tb/pp_top copy): gate-direct.sh <verilator wrapper>
set -eu
cmd=$(make -n fixture-guards VERILATOR="$1" | sed -e ':a' -e '/\\$/N; s/\\\n//; ta' \
      | grep '^python3 fixture_guards.py')
echo "gate command: ${cmd:0:160}..."
exec sh -c "$cmd"
