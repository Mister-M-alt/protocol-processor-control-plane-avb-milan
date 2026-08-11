<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# ucpu — KL_aecp_ucpu skeleton suite

Proves the µCPU skeleton (`hdl/aecp/KL_aecp_ucpu.sv`) actually executes the
[06 §8](../../docs/architecture/06_aecp_engine.md) µISA before anyone quotes its
area: `make` = build + run, exit 0 = PASS, 37 checks.

The C++ harness is an independent model, never DUT logic: it implements the
state port (2-cycle read latency, locate mapping, forced miss), the gather port
(2-cycle latency, selector-keyed data), a reluctant TX (3 stall cycles before
`tx_ready`), and the lock context — then checks the exact response-buffer bytes,
length, status and send count each µprogram must produce.

Covered per program (µcode from `hdl/aecp/ucode/gen_ucode.py`, shared with
synthesis): the GET_SAMPLING_RATE exemplar success + locate-miss fail path;
RAW interlock and branch flush (poison ops must never reach the buffer); masked
merge; 2-beat qword fields; the ITER_OPEN/APPEND/ITER_NEXT loop shape of
GET_DYNAMIC_INFO; CHECK_ARG → BAD_ARGUMENTS; CHECK_LOCK → LOCKED vs SUCCESS;
GATHER_EXT and the 4-beat READ_COUNTERS burst; re-dispatch readiness.

Mutation-proven 2026-08-11: disabling the RAW interlock fails 11 of 37;
removing the branch flush fails 2 of 37.
