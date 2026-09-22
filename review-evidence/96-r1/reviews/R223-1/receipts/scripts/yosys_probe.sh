#!/usr/bin/env bash
# R223-1 portability probe: sv2v + Yosys elaborate protocol_processor_top (the
# donor's syn/yosys/run.sh tops list does not include it) at head with the
# default and with the verification fixture, and at base, and print the
# derived KL_srp_top / KL_srp_domain modules with the parameter values Yosys
# bound. Single-threaded; scratch only.
set -euo pipefail
R=/tmp/r223-96-r1
for tree in head base; do
  w=$R/yosys/$tree; mkdir -p "$w"
  ( cd "$R/$tree" && sv2v $(find hdl -name '*_pkg.sv' | sort) $(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort) > "$w/all.v" )
  ( cd "$R/$tree/hdl/aecp/ucode" && python3 gen_ucode.py -o "$w/ucode.hex" >/dev/null )
  ( cd "$R/$tree/hdl/acmp/rom" && python3 gen_ltn_rom.py -o "$w/ltn_rom.hex" >/dev/null )
  echo "== $tree: sv2v lines $(wc -l < "$w/all.v")"
  grep -n 'SRP_DOM_DEF_VID_P\|DOM_DEF_VID_P\|DEF_VID_P' "$w/all.v" | cut -c1-160 || true
done
cd $R/yosys/head
echo "== head, top default"
yosys -p "read_verilog all.v; hierarchy -check -top protocol_processor_top; ls" 2>&1 \
  | grep -E 'KL_srp_top|KL_srp_domain|^ERROR|Error|Warning: .*DOM_DEF' | sort -u
echo "== head, -chparam SRP_DOM_DEF_VID_P 23100 (0x5A3C)"
yosys -p "read_verilog all.v; hierarchy -check -top protocol_processor_top -chparam SRP_DOM_DEF_VID_P 23100; ls" 2>&1 \
  | grep -E 'KL_srp_top|KL_srp_domain|^ERROR|Error' | sort -u
cd $R/yosys/base
echo "== base, top default"
yosys -p "read_verilog all.v; hierarchy -check -top protocol_processor_top; ls" 2>&1 \
  | grep -E 'KL_srp_top|KL_srp_domain|^ERROR|Error' | sort -u
echo "== base, -chparam SRP_DOM_DEF_VID_P 23100 (must be refused: no such top parameter)"
yosys -p "read_verilog all.v; hierarchy -check -top protocol_processor_top -chparam SRP_DOM_DEF_VID_P 23100; ls" 2>&1 \
  | grep -E 'KL_srp_top|KL_srp_domain|^ERROR|Error|chparam|SRP_DOM' | sort -u || true
echo "YOSYS PROBE DONE"
