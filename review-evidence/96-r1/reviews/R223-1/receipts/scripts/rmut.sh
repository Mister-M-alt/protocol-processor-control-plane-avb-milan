#!/usr/bin/env bash
# R223-1 scratch mutation runner (review-owned; never touches a tracked tree).
# usage: rmut.sh ID MODE FILE OLD NEW
#   MODE: fixture | default | both
#   FILE: path relative to the repo root (hdl/... or tb/pp_top/...)
#   OLD/NEW: literal text; OLD must occur exactly once in FILE.
# Copies hdl/, tb/common/ and the three tb/pp_top inputs of the head scratch
# copy into mut/ID, applies the one edit, proves exactly one file differs, then
# runs the build/run commands `make -n run` prints for that copy (so the flags
# cannot drift from the Makefile), with Verilator capped at 8 jobs.
set -euo pipefail
id=$1; mode=$2; file=$3; old=$4; new=$5
root=/tmp/r223-96-r1
src=$root/head
dst=$root/mut/$id
V=$root/bin/verilator8
rm -rf "$dst"; mkdir -p "$dst/tb/pp_top"
cp -a "$src/hdl" "$dst/hdl"
cp -a "$src/tb/common" "$dst/tb/common"
cp -a "$src/tb/pp_top/Makefile" "$src/tb/pp_top/pp_top_wrap.sv" \
      "$src/tb/pp_top/sim_main.cpp" "$dst/tb/pp_top/"
if [ "$old" != "-" ]; then
python3 - "$dst/$file" "$old" "$new" <<'EOF'
import sys
p, old, new = sys.argv[1:4]
s = open(p).read()
n = s.count(old)
if n != 1:
    sys.exit(f"MUTATION REFUSED: expected exactly one match, got {n}")
t = s.replace(old, new)
if t == s:
    sys.exit("MUTATION REFUSED: no-op")
open(p, 'w').write(t)
EOF
fi
# exactly one differing input (or zero for the unmutated control "-")
ndiff=0
while IFS= read -r f; do
  rel=${f#"$dst/"}
  if ! cmp -s "$f" "$src/$rel"; then ndiff=$((ndiff+1)); echo "DIFFERS: $rel"; fi
done < <(find "$dst/hdl" "$dst/tb/common" "$dst/tb/pp_top" -type f)
if [ "$old" = "-" ]; then want=0; else want=1; fi
[ "$ndiff" -eq "$want" ] || { echo "EXPECTED $want differing file(s), got $ndiff"; exit 2; }
if [ "$old" != "-" ]; then
  (cd "$dst" && diff -u "$src/$file" "$file" > "$dst/mutation.diff" || true)
else
  echo "control: no edit" > "$dst/mutation.diff"
fi
cd "$dst/tb/pp_top"
make -s VERILATOR="$V" ltn_rom.hex ucode.hex >/dev/null
mapfile -t cmds < <(make -n run VERILATOR="$V" | sed -e ':a' -e '/\\$/N; s/\\\n//; ta')
# cmds: [0] rm tally, [1] default build, [2] default run, [3] fixture build,
#       [4] fixture run, [5] awk tally
printf '%s\n' "${cmds[@]}" > "$dst/commands.txt"
[ "${#cmds[@]}" -eq 6 ] || { echo "UNEXPECTED make -n shape (${#cmds[@]} lines)"; exit 3; }
eval "${cmds[0]}"
mkdir -p obj_dir
if [ "$mode" = default ] || [ "$mode" = both ]; then
  eval "${cmds[1]}" > "$dst/build_default.log" 2>&1
  set +e; eval "${cmds[2]}" > "$dst/run_default.log" 2>&1; echo "rc=$?" >> "$dst/run_default.log"; set -e
  grep -E '^\[build|^FAIL' "$dst/run_default.log" | sed "s/^/[$id default] /"
  tail -1 "$dst/run_default.log" | sed "s/^/[$id default] /"
fi
if [ "$mode" = fixture ] || [ "$mode" = both ]; then
  eval "${cmds[3]}" > "$dst/build_fixture.log" 2>&1
  set +e; eval "${cmds[4]}" > "$dst/run_fixture.log" 2>&1; echo "rc=$?" >> "$dst/run_fixture.log"; set -e
  grep -E '^\[build|^FAIL' "$dst/run_fixture.log" | sed "s/^/[$id fixture] /"
  tail -1 "$dst/run_fixture.log" | sed "s/^/[$id fixture] /"
fi
# drop the build products, keep the logs, the diff and the commands
rm -rf "$dst/tb/pp_top/obj_dir" "$dst/tb/pp_top/obj_vid"
