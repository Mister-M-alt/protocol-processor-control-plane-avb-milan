#!/usr/bin/env bash
# Mutation runner for issue #95 (scratch copies only; the lane is never edited).
# usage: a157_mutate.sh <id> <which: fixture|both> <file> <sed-expr> [<file2> <sed-expr2>]
set -uo pipefail
LANE=$CANDIDATE
id=$1; which=$2; shift 2
root=$MUTATION_STORAGE/$id
rm -rf "$root"; mkdir -p "$root/tb"
cp -r "$LANE/hdl" "$root/hdl"
cp -r "$LANE/tb/common" "$root/tb/common"
mkdir -p "$root/tb/pp_top"
cp "$LANE"/tb/pp_top/{Makefile,README.md,pp_top_wrap.sv,sim_main.cpp} "$root/tb/pp_top/"
find "$root" -name '__pycache__' -prune -exec rm -rf {} +
while [ $# -ge 2 ]; do
  f=$1; expr=$2; shift 2
  before=$(md5sum "$root/$f" | cut -d' ' -f1)
  sed -i "$expr" "$root/$f"
  n=$(diff "$LANE/$f" "$root/$f" | grep -c '^[<>]')
  after=$(md5sum "$root/$f" | cut -d' ' -f1)
  if [ "$before" = "$after" ]; then echo "MUTATION DID NOT APPLY: $f :: $expr"; exit 3; fi
  echo "== $id: $f changed ($n diff lines)"; diff "$LANE/$f" "$root/$f"
done
cd "$root/tb/pp_top" || exit 4
export MAKEFLAGS=-j8 VERILATOR_JOBS=8
V="verilator --build-jobs 8 --verilate-jobs 8"
make -s VERILATOR="$V" ltn_rom.hex ucode.hex >/dev/null 2>&1
cmds=$(make -n VERILATOR="$V" run | sed -e ':a' -e '/\\$/N; s/\\\n *//; ta')
mkdir -p obj_dir; rm -f obj_dir/build_tally.txt
run_build() { # $1 = obj_dir|obj_vid
  local b=$1 vcmd bin
  vcmd=$(grep -E '^verilator ' <<<"$cmds" | grep -- "$( [ "$b" = obj_vid ] && echo '--Mdir obj_vid' || echo ' -o Vpp_top_sim')")
  bin=$( [ "$b" = obj_vid ] && echo ./obj_vid/Vpp_top_vid || echo ./obj_dir/Vpp_top_sim )
  eval "$vcmd" > "build_$b.log" 2>&1 || { echo "BUILD FAILED ($b)"; tail -5 "build_$b.log"; return; }
  "$bin" > "run_$b.log" 2>&1; echo "   $b exit=$?"
  grep -E '^\[build' "run_$b.log" | sed 's/^/   /'
  grep -E '^FAIL' "run_$b.log" | sed 's/^/   /'
}
[ "$which" = both ] && run_build obj_dir
run_build obj_vid
