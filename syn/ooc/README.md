<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# syn/ooc — the µCPU area experiment

The gate experiment of
[`docs/10_RESOURCE_AND_EFFORT.md` §6](../../docs/10_RESOURCE_AND_EFFORT.md):
out-of-context Vivado synthesis of the µCPU skeleton at the reference
platform's ship part, using the same instrument as every anchor in that
document (post-synthesis hierarchical utilization). This directory is
deliberately device-specific — it prices this architecture against one real
die; the RTL itself stays vendor-neutral (`hdl/README.md` rule 1).

```sh
cd <workdir>
python3 <repo>/hdl/aecp/ucode/gen_ucode.py -o ucode.hex   # ROM image for $readmemh
vivado -mode batch -source <repo>/syn/ooc/ucpu_ooc.tcl -nojournal -log ooc.log
```

## Result of record — 2026-08-11, Vivado 2026.1, xc7a100t-fgg484-2

| Metric | Value |
|---|---|
| Slice LUTs | **1,042** (910 logic + 132 as distributed RAM) |
| Registers | 478 |
| Block RAM | 3 × RAMB36 (the 2048 × 48 µcode ROM) |
| DSP | 0 |
| WNS at 100 MHz (`P-CLK-HZ`), OOC | **+2.263 ns** (0 failing endpoints) |

Sanity held: the register file inferred as distributed RAM (not the +894-LUT
flop-mirror failure mode), the ROM as block RAM (its contents cannot be
constant-folded into the decode), and the only synthesis warnings are the two
constant-1 strobe bits that are true by construction. The functional suite
(`tb/ucpu/`, 37 checks, mutation-proven) ran green on the same RTL and the
same ROM image before synthesis.

The number is a **skeleton** measurement: dispatch handshake, hazard-key
extraction and the deadline/abort arm will grow it. The bracket it collapses
(+1,200…+2,500 ESTIMATE) survives a 2× growth allowance.
