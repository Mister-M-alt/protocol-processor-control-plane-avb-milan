<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# syn/ooc — the µCPU area experiment

`protocol_processor_ooc.tcl` applies the same post-synthesis instrument to
the complete processor, with its default stream shape and all top-level
ports present. Run it from an empty build directory; an optional first Tcl
argument selects a different source tree for a baseline measurement, and an
optional second and third select `N_STREAM_IN_P` and `N_STREAM_OUT_P` (for
example `-tclargs <tree> 1 1`, the shipping one-stream shape). It
generates both ROM images in that build directory and reports hierarchical
and total utilization. It uses the reference part and clock from
`ucpu_ooc.tcl`; these are area measurements, not routed timing or hardware
qualification. Keep baseline and candidate reports beside the change's
validation evidence and compare the same recipe on both trees.

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
| Slice LUTs | **1,068** (936 logic + 132 as distributed RAM) |
| Registers | 491 |
| Block RAM | 3 × RAMB36 (the 2048 × 48 µcode ROM) |
| DSP | 0 |
| WNS at 100 MHz (`P-CLK-HZ`), OOC | **+2.541 ns** (0 failing endpoints) |

Sanity held: the register file inferred as distributed RAM (not the +894-LUT
flop-mirror failure mode), the ROM as block RAM (its contents cannot be
constant-folded into the decode), and the only synthesis warnings are the two
constant-1 strobe bits that are true by construction. The functional suite
(`tb/ucpu/`, mutation-proven; 92 checks at that date, and it has grown since —
run the suite rather than trusting this number) ran green on the same RTL and
the same ROM image before synthesis.

## Re-measured 2026-08-13 — after the flow-controlled response buffer

The response-buffer face gained `rb_ready` when the buffer moved into main
memory ([03 §7.1](../../docs/architecture/03_packet_engine.md)), so the number
above was re-taken on the same instrument, same part, same ROM image:

| Metric | 2026-08-11 | 2026-08-13 |
|---|---|---|
| Slice LUTs | 1,068 (936 + 132 distributed RAM) | **1,070** (938 + 132) |
| Registers | 491 | 491 |
| Block RAM | 3 × RAMB36 | 3 × RAMB36 |
| WNS at 100 MHz, OOC | +2.541 ns | **+2.586 ns** |

Two LUTs for a stall arm the whole µISA needed anyway. The reason it is that
cheap is the one design decision in `KL_aecp_resp_buf`: its `wr_ready_o` is a
REGISTER, so nothing in the µCPU's stall path reaches into the buffer's address
arithmetic and back. Driven combinationally from the write address instead, the
same feature cost ~1 ns of WNS on the whole processor.

The number is a **skeleton** measurement: dispatch handshake, hazard-key
extraction and the deadline/abort arm will grow it. The bracket it collapses
(+1,200…+2,500 ESTIMATE) survives a 2× growth allowance.

## Descriptor-memory guard — issue #94

`desc_mem_guard_ooc.tcl` uses the same out-of-context synthesis, reference part,
clock constraint and utilization reports as the µCPU flow above:

```sh
cd <empty-output-directory>
vivado -mode batch -source <repo>/syn/ooc/desc_mem_guard_ooc.tcl -nojournal -log ooc.log
```

Measured 2026-09-24 with Vivado 2026.1, `xc7a100tfgg484-2`: **4 Slice LUTs,
1 flip-flop, no RAM or DSP**, against the
[immutable T9 estimate](https://github.com/kebag-logic/milan-fpga/blob/c1ee27d81c4a1e98f9584e979b73a88acfe238b3/design-evidence/500-materialization/tickets/T9-processor-descriptor-memory-response-isolation.md)
of 5 LUTs and 1 flip-flop. The guard was synthesized as its own top with all
ports present. This is post-synthesis area, not routed timing or hardware proof.

## SRP admission freshness — issue #112

`srp_ooc.tcl` measures the complete `KL_srp_top` at its default shape
(eight sources, eight sinks), with all ports present, using the same part,
OOC synthesis, clock and reports:

```sh
cd <empty-output-directory>
vivado -mode batch -source <repo>/syn/ooc/srp_ooc.tcl -nojournal -log ooc.log
```

For a base/head comparison, run this identical recipe against each source
tree. `util_hier.rpt` includes the admission block as well as the enclosing
engine, so the internal invalidation wiring is included in the total.

Measured 2026-09-24 with Vivado 2026.1, `xc7a100tfgg484-2`, 10 ns clock. Base
is `939c143`. Round 1 is `73a5478`, where a pending source counted as
absent. Round 2 holds every verdict while a declaration is pending.

| Complete `KL_srp_top` | Base | Round 1 | Round 2 |
|---|---:|---:|---:|
| Slice LUTs | 7566 | 7334 | **7603** (+37 on base) |
| Flip-flops | 10485 | 10464 | **10502** (+17 on base) |
| LUT as memory / RAMB18 / RAMB36 / DSP | 194 / 1 / 0 / 2 | same | same |
| `u_admission` LUTs / flip-flops | 782 / 862 | 503 / 841 | 749 / 879 |
| WNS, OOC (worst path in the unmodified `u_decoder`) | +1.302 ns | +1.083 ns | +1.083 ns |

The engine total is the area result. A hierarchy row is not an isolated
estimate, because whole-engine optimization remaps unmodified blocks too.
Round 1's drop came from that remapping, not from removed function: from
base to round 1, `u_vlan` fell 361 → 101 LUTs, `u_talker` rose 1667 → 1893
and `u_encoder` rose 1181 → 1252, and these stay put in round 2. The
admission flip-flops, listed per register bank, account for every
difference:

- Round 2 − base = +17. The validity pipeline adds 10 (`valid_q1_r`,
  `valid_q2_r`, 8 × `slope_valid_r`) and `pend_acc_r` adds 1. Synthesis adds
  5 fanout replicas of `aidx_r`. Base had also trimmed `wgrant_r[7]`.
- Round 1 − base = −21. The validity pipeline adds 10 and `wgrant_r[7]` is
  kept (+1). Round 1 also trimmed `wgslope_r[7]` (−32), which is always zero
  because index 7 publishes straight from the candidate.

Round 1's admission LUT primitives packed two to a LUT (712 primitives in
503 LUTs). Base and round 2 map to wider LUTs: 950 in 782, and 944 in 749.
This is post-synthesis area, not routed timing or hardware proof.

## GET_STREAM_INFO input selectors — issues #43/#49

Measured 2026-09-24 with Vivado 2026.1, `xc7a100tfgg484-2`, this recipe on the
base `939c1433` and on the change's final RTL, both shapes. Post-synthesis
area, not routed timing or hardware proof:

| Shape | Resource | Base | Change | Delta |
|---|---|---:|---:|---:|
| 8 in / 8 out | Slice LUTs | 28,092 | 28,326 | +234 |
| 8 in / 8 out | Registers | 30,354 | 31,064 | +710 |
| 1 in / 1 out | Slice LUTs | 20,978 | 21,245 | +267 |
| 1 in / 1 out | Registers | 23,446 | 23,674 | +228 |

Block RAM is unchanged at both shapes. The registers are owned state kept
alive by the new readers: the per-sink 64-bit FailureInformation bridge latch
in the SRP listener, the decoder's FailureInformation capture (+128 at either
shape) and the eight-bit committed pbsta/acmpsta view per sink. The bridge is
gated once after the top's index mux and the change is compared on the hit
sink only, which took the SRP listener from +808 to +455 LUTs at 8x8 against
the first revision; removing that revision's selector-0 sample-and-hold removed
81 top-level registers at 8x8 (78 at 1x1). LUT moves of a few tens in modules
this change does not touch are synthesis variance.
