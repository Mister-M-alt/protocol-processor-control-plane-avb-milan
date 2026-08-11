<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# ucpu — KL_aecp_ucpu skeleton suite

Proves the µCPU skeleton (`hdl/aecp/KL_aecp_ucpu.sv`) actually executes the
[06 §8](../../docs/architecture/06_aecp_engine.md) µISA before anyone quotes its
area: `make` = build + run, exit 0 = PASS, 92 checks.

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

Covered additionally: the SET_SAMPLING_RATE exemplar with its full effect
chain (state write-back strobes, COMMIT, NVM_MARK, NOTIFY_ENQ — and their
suppression under a foreign lock), the name region select, COPY_BUFFER lanes,
MAP_VALIDATE both ways, the 524-byte cap with §7.4.76.1 skip-on-overflow (64
of 70 elements fit, the rest skip, iteration continues), Table 7-141 status
codes on the wire header, write-strobe formats B/W/Q, truncating moves, 64-bit
compares, the unknown-opcode NOT_IMPLEMENTED path, the ACQUIRE_ENTITY Milan Δ7
exemplar, and the §9.3.2.6 FAIL_SAFE arm preserving the best current status.

Mutation-proven 2026-08-11: RAW interlock off fails 11 of 92; branch flush
removed fails 2; the 524 cap widened fails 4; the COMMIT strobe killed fails 1.
(One earlier candidate mutation — advance-gating on a never-stalling op — was
behavior-equivalent and was replaced: an equivalent mutant proves nothing.)
