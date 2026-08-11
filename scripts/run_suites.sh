#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
# Run every Verilator suite under tb/ (glob — never a hand-kept list).
# Exit = number of failing suites; exit 90 if any PASSing suite's tally line
# is unreadable (a suite whose check count cannot be read is unproven).
set -u
cd "$(dirname "$0")/.."
fails=0; unreadable=0; total=0
for d in tb/*/; do
  [ -f "$d/Makefile" ] || continue
  name=$(basename "$d")
  log=$(mktemp)
  if (cd "$d" && make) >"$log" 2>&1; then
    tally=$(grep -Eo '[0-9]+ checks: [0-9]+ PASS, [0-9]+ FAIL' "$log" | tail -1)
    if [ -z "$tally" ]; then
      echo "UNREADABLE $name (passed but no tally line)"; unreadable=1
    else
      n=${tally%% *}; total=$((total + n))
      echo "PASS $name ($tally)"
    fi
  else
    echo "FAIL $name"; tail -5 "$log" | sed 's/^/    /'; fails=$((fails + 1))
  fi
  rm -f "$log"
done
echo "suites: $total checks total, $fails failing"
[ $fails -gt 0 ] && exit $fails
[ $unreadable -gt 0 ] && exit 90
exit 0
