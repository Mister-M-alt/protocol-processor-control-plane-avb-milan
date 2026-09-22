#!/usr/bin/env bash
# Focused validation of the committed lane head (issue #95). Sequential, jobs capped at 8.
set -u
LANE=$CANDIDATE
OUT=/tmp/a157_val; rm -rf "$OUT"; mkdir -p "$OUT"
export MAKEFLAGS=-j8 VERILATOR_JOBS=8
V="verilator --build-jobs 8 --verilate-jobs 8"
cd "$LANE" || exit 1
sum() { echo "$*" | tee -a "$OUT/SUMMARY"; }
sum "HEAD $(git rev-parse HEAD)"
sum "status-before: $(git status --porcelain | wc -l) changed paths"
step() { # $1 name, rest = command
  local name=$1; shift
  local t0=$(date +%s)
  ( "$@" ) > "$OUT/$name.log" 2>&1
  local rc=$?
  sum "$name rc=$rc ${SECONDS_TAKEN:-}$(( $(date +%s)-t0 ))s"
}
suite() { # $1 suite dir
  ( cd "tb/$1" && make clean >/dev/null 2>&1; make VERILATOR="$V" )
}
step pp_top          suite pp_top
step srp_top         suite srp_top
step srp_encoder     suite srp_encoder
step timer_map       suite timer_map
step check_upc_map   python3 scripts/check_upc_map.py
step lint_hdl        ./scripts/lint_hdl.sh
step make_check      make -j1 check
step gen_matrix      python3 scripts/gen_matrix.py --check
step check_links     python3 scripts/check-links.py
step check_matrix    python3 scripts/check-matrix.py
step wavedrom_check  python3 scripts/render-wavedrom.py --check
step make_stale      make -j1 stale
step yosys           ./syn/yosys/run.sh
sum "status-after: $(git status --porcelain | wc -l) changed paths"
sum "DONE"
