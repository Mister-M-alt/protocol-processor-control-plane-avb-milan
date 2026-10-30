<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# dyn_state — KL_aecp_dyn_state suite

Proves the AECP dynamic-state store (`hdl/aecp/KL_aecp_dyn_state.sv`), the one home of
the run-time values a controller may change: current configuration, sampling rate,
clock source, stream formats, presentation-time offset and IDENTIFY
([07 §3.4](../../docs/architecture/07_memory_maps.md#34-dynamic-overlay), Milan v1.2
§5.3.5.1, §5.3.7.1, §5.3.7.6, §5.3.8.1, §5.3.11.1, §5.3.12, §5.4.2.5). A Stream Input's
started state is not here: it belongs to the ACMP binding record (Milan §5.3.8.7), and
group F2 below holds its old selector retired.

`make` builds and runs the shapes of the [Makefile](Makefile)'s `run` and sums them: exit 0 =
PASS, tally `118 checks: 118 PASS, 0 FAIL` (62 at 2x2, 56 at the shipping 1x1). Each build appends
its own count to `shape_tally.txt`, which the last line of the run adds up.

Every check drives the real state port with the handshake the µCPU drives (request held
until ready or read-valid), so a store that only works when poked in a convenient order
fails here.

| Group | What it proves |
|---|---|
| A | out of reset every field reads invalid and zero, IDENTIFY is 0 and nothing is dirty |
| B | a write is kept and its valid flag (region 0x2) goes to 1, the bit every GET program branches on to fall back to the descriptor image |
| C | settings are per descriptor: two Stream Inputs keep two formats, and one output's offset leaves the other's invalid |
| D | an index the shape does not have is refused and counted (`dbg_oob_o`), and stores nothing |
| E | the dirty output marks the persisted set only: IDENTIFY, which is volatile, never sets it |
| F | the live faces the fabric reads (current configuration, clock source, offsets, formats and their valid bits) follow the store |
| F2 | selector 6 is retired: a write to it is refused and counted, and cannot become a second source of any field |
| G | a read never changes the store or its write count |
| H | the change qualifier the D3 writer triggers on is set by a write that changes a value and never by one that repeats it, by IDENTIFY, by an out-of-shape index or by the retired selector |

Not covered here: the µprograms that use the store (graded end to end in
[`tb/pp_top`](../pp_top/README.md)) and its save and restore through the D3 writer
([09 §8.2](../../docs/architecture/09_verification.md#82-saved-state-the-scalar-and-name-stages-evidence-issues-131-61-83-170)).
