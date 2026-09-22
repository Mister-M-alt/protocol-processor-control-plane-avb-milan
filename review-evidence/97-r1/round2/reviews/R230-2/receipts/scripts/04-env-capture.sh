#!/usr/bin/env bash
# Real processes, no mocks: `make fixture-guards` at the exact head with the
# compiler replaced by cxx-record (runs /usr/bin/g++ unchanged, records argv,
# complete environment, output bytes, rc) and Verilator by verilator8 (dumps
# its complete environment). Extra caller inputs are added to prove they reach
# the compiler unchanged. Analysis: scripts/04-analyze.py.
set -u
source "$(dirname "$0")/settings.sh"
V8="$RC/scripts/verilator8"
mkdir -p "$S/empty-include"
for k in EN FR_LANGUAGE DE_LC_ALL MIXED; do
  d="$S/capture/$k"; rm -rf "$d"; mkdir -p "$d/cxx" "$d/venv"
  $RUN --name "04-capture-$k" --cwd "$S/head/tb/pp_top" ${SETTING[$k]} \
    --set R230_MARKER=keep-me --set CPATH="$S/empty-include" \
    --set CPLUS_INCLUDE_PATH="$S/empty-include" --set SOURCE_DATE_EPOCH=0 \
    --set R230_CXX_DIR="$d/cxx" --set R230_ENV_DUMP_DIR="$d/venv" \
    --set R230_VLOG="$RC/verilator-invocations.jsonl" \
    --expect-rc 0 -- make -j1 VERILATOR="$V8" CXX="$RC/scripts/cxx-record" fixture-guards
done
python3 "$RC/scripts/04-analyze.py"
