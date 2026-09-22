#!/usr/bin/env bash
# Invocation used for receipt 05 (in-process caller isolation, MIXED caller).
# The first attempt stopped inside the harness before the gate ran: its argv
# extraction matched the `python3 -B -m unittest ... test_fixture_guards.py`
# line of `make -n`. The harness now selects the line that starts with
# `python3 fixture_guards.py`; the receipt is from the corrected harness.
set -u
source "$(dirname "$0")/settings.sh"
$RUN --name 05-inprocess-isolation-MIXED --cwd "$S/head/tb/pp_top" ${SETTING[MIXED]} \
  --set R230_MARKER=keep-me --set R230_VLOG="$RC/verilator-invocations.jsonl" \
  --expect-rc 0 -- python3 -B "$RC/scripts/05-inprocess-isolation.py"
