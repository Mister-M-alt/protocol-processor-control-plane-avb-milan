#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
# Device portability: sv2v + yosys must elaborate every top (hdl/README rule 2).
# The tops array is the authoritative list — extend it with every new top.
set -eu
cd "$(dirname "$0")/../.."
tops=(KL_aecp_ucpu)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
sv2v hdl/aecp/ucpu_pkg.sv $(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort) > "$work/all.v"
( cd hdl/aecp/ucode && python3 gen_ucode.py -o "$work/ucode.hex" >/dev/null )
cd "$work"
for t in "${tops[@]}"; do
  yosys -q -p "read_verilog all.v; hierarchy -check -top $t; proc; opt_clean" \
    && echo "YOSYS OK  $t" || { echo "YOSYS FAIL $t"; exit 1; }
done
