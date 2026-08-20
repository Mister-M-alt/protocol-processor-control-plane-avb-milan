<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# nvm_port — KL_pp_nvm_port class-F suite

Proves the class-F NVM port (`hdl/packet_engine/KL_pp_nvm_port.sv`,
[02 §8](../../docs/architecture/02_interfaces.md) F02.8 +
[07 §5](../../docs/architecture/07_memory_maps.md) F07.8): `make` = build + run,
exit 0 = PASS, 83 checks. `-GMAX_PAYLOAD_P=1024` pins the geometry the C++
constants mirror.

The harness plays BOTH neighbors, independently of the RTL: a **manager BFM**
that frames records per 07 §5.2 (magic 0x1722, layout_version, record_id,
payload_length, crc16 — 16-bit fields big-endian on the stream; crc computed by
the harness, opaque to the DUT) and streams them with configurable stalls, and
a **device model** implementing the region port (req/gnt with delay, op
READ/WRITE/ERASE_REGION, region + offset + len, stalling byte phases,
busy/done/err with completion delays, error injection at any command index or
data byte, a seeded byte store, and a log of every accepted command).

Covered: the commit envelope ERASE_REGION → WRITE(0, 8+plen) with the device
store byte-exact against the manager's framed record and the erase visible past
it; the restore envelope READ(0,8) → READ(8,plen) with the returned stream
byte-exact; zero-payload records both ways (restore then issues only the header
probe); stall torture on all four byte interfaces at once (gnt and done
delayed); back-to-back ops with req re-asserted the cycle after the pulse;
req-while-busy ignored (single outstanding, F02.8); busy/done/err sequencing —
busy high mid-op, LOW at the pulse, done and err mutually exclusive and exactly
once per op; a device error during ERASE surfacing as exactly one manager-face
err with the WRITE never issued, plus recovery on retry; errors mid WRITE-data
and mid READ-payload; refusals per 07 §5.2 with zero (further) device traffic —
bad magic on commit, oversize payload_length on commit, bad magic and oversize
length in the stored record on restore (nothing forwarded to the manager); and
the port serviceable again after every refusal.

Known limits (honest): the CRC16 is carried, never checked — that is the
manager's job per 07 §5.3, so a corrupt-crc record passes this port by design
and must be caught by the NVM-manager suite (P4). The byte order of the 16-bit
header fields on the stream is a design decision of the port (network order),
not pinned by the doc.

Power-cut coverage (issue #70, added 2026-08-20). A commit is ERASE(region)
then WRITE(0, 8+plen), so a cut inside the WRITE leaves the region erased plus a
partial record: the record being written is gone **and so is whatever it
replaced**. That is a property of writing a slot in place, and it is why the
flash map reserves A/B slots — the suite pins what the port DOES guarantee
rather than a survival this layout cannot give:

- **T15** a torn commit reports `err` and never `done`, with busy low at the
  pulse; the torn image is neither the old record nor the new one (so the cut is
  real, not a no-op); it never restores as a VALID record — either the port
  refuses it at the header or the bytes it forwards fail the manager's CRC,
  which the suite computes itself; and the port is serviceable afterwards.
- **T16** the property #70 actually needs: a torn commit of one record leaves
  **every other record** untouched. All 8 regions are snapshotted in FULL
  (every one of `REG_BYTES`, not just the record-sized prefix) before the cut
  and compared after, so a clobber landing two regions over, or past the end of
  a record, is caught rather than only the byte range the test happens to use;
  the neighbour still restores byte-exactly afterwards. The isolation claim is
  guarded against vacuity by first pinning that the torn commit really did
  reach the device — otherwise "nothing else moved" would also hold for a port
  that never issued anything. Getting that guard right took three attempts and
  the first two were themselves vacuous, which is worth recording: "the
  region's bytes changed" is satisfied by the ERASE alone, and "some byte is
  not 0xFF" is satisfied by whatever an earlier phase left behind — region 1
  still holds T5b's record from `sim_main.cpp:375` eleven phases later, so that
  spelling passed even under a mutant where the port wedged and issued no
  device traffic at all. What actually defends the claim is the pair of checks
  that READ THE DUT: the op log must show ERASE then the WRITE for this record,
  and the region's erase count must have moved. Both are driven by `dev_req_o`,
  and a port issuing nothing can fake neither. The byte comparison beside them
  is deliberately NOT load-bearing, and the code says so: `torn[0..4]` is
  byte-identical to the T5b residue prefix, so all its separating power sits in
  `store[1][5] == 0xFF`, and it reddens under a device model that leaves the
  last page half-programmed even with the RTL untouched. It is kept as a
  model-consistency check on offset and data, not as the anti-vacuity guard.
- **T17** the cut a NOR device actually produces. T15 and T16 cut while bytes
  are still moving, but a real program failure is not reported then: the device
  latches the bytes, starts the program cycle, and raises its error when that
  cycle ends — after the last byte, busy still high. That is the port's
  `S_WWAIT` arm, the widest window in a commit, and nothing else in the suite
  enters it. The phase pins what the port owes there — `err` and never `done`,
  busy released and the port idle afterwards, and the port usable for the next
  commit. It also restores the region afterwards: what the array holds is the
  model's choice, but whether the port still SERVES that region after taking
  `S_WWAIT`'s error exit is the port's own property, and T17 otherwise
  exercised only the write side. The restore is compared against what the
  ARRAY holds rather than against the record that was being written: comparing
  against the record pins the device model, and a model rolling its last bytes
  back to 0xFF -- the half-programmed page this section names -- reddens it
  with no RTL change at all. The length is pinned separately, because a bare
  slice compare passes vacuously on an empty forward.
- **T18** the same argument on the RESTORE side. A NOR read fails the way a
  program does: an ECC or timeout error surfaces when the read cycle ENDS, not
  mid-stream. `S_RPWAIT` is the exact mirror of the arm T17 closed, `S_RHWAIT`
  is that window on the header probe, and `S_RHCOLL` is an error during the
  header collect — which sits on the boot restore walk, the one path where a
  torn image is actually consumed. All three survived the suite before T18.
  What the array holds afterwards is deliberately NOT pinned: this device model
  writes every byte before failing, while real NOR may leave the last page
  half-programmed, and the port cannot distinguish those.

Mutation-proven 2026-08-11 (backup → sed → run → restore → green):
- **M1** commit skips the ERASE (`S_WEREQ` target rewritten to `S_WWREQ`):
  fails 11 of 55 (op-log shape, erase pulse/visibility, erase-error path).
- **M2** magic gate dropped from `hdr_ok_w`: fails 5 of 55 (both bad-magic
  refusals and the nothing-forwarded check).
- **M3** payload pump off-by-one (`bcnt_r == plen_r` for `plen_r - 1`, both
  directions): fails 38 of 55 (every data-phase op times out or mismatches).
- **M4** (2026-08-20) the write phase swallows the device error (`S_WDPUMP`'s
  `if (dev_err_i)` forced false): **fails 22 of 83**. A torn commit then looks
  clean, which is exactly the false success #70 exists to remove. Six checks
  survive, enumerated by RUNNING the mutation rather than reasoning about it,
  because earlier versions of this file twice published a survivor list written
  from what M4 ought to do: `T15 seed record committed` (it precedes the cut);
  `T15 busy raised then low at the err pulse` and `T17 busy raised then low at
  the err pulse` (a wedged port holds busy high and never pulses, so neither
  pair is contradicted); `T15 the torn image is neither the old record nor the
  new one`; and T16's two isolation checks, `erased no other region` and `no
  other region's bytes moved`, which pass VACUOUSLY because a wedged port
  issues no further device traffic and so nothing else can move. That vacuity
  is what the T16 guard exists to answer.
- **M5** (2026-08-20) the completion window swallows the device error
  (`S_WWAIT`'s `if (dev_err_i)` forced false): **fails 10 of 83**, and survives
  the whole suite without T17. The port waits for a `done` a failed device will
  never send, so the commit never answers at all: `run_op` returns -1 after
  100,000 cycles. `busy_seen && busy_ok` does NOT catch this, and the comment
  at that check says so: a wedged port holds busy high, so `busy_seen` is true
  and no pulse arrives to contradict `busy_ok`.
- **Probes** (mutations of the TEST, not the RTL). Arming T16's tear as
  `arm_err(1, -1)`, so the WRITE fails before its first byte moves, fails 1 of
  83. Replacing the torn commit with a bare `rc = 1` and no device traffic at
  all — the port the T16 prose names as the threat — fails 3 of 83. Under the
  previous guard that second probe failed only ONE check, the erase count,
  while the payload guard passed on residue from an earlier phase.
- **Model probe**: changing only the DEVICE MODEL to roll the last 4 bytes back
  to 0xFF on a completion-window failure — the half-programmed page a real NOR
  may leave — must not redden a check about the PORT. The T17 restore check
  did exactly that before it was made array-relative. The T16 byte comparison
  still does, which is why it is documented above as a model-consistency check
  rather than counted toward the anti-vacuity guarantee.

### Device-error arm coverage

`KL_pp_nvm_port.sv` has twelve `if (dev_err_i)` arms. Each was forced to
`1'b0` in turn and the suite re-run, so this table is measured, not argued:

| line | state | checks failed |
|---|---|---|
| 185 | `S_WEREQ`  | **0 — uncovered** |
| 194 | `S_WEWAIT` | 37 |
| 204 | `S_WWREQ`  | **0 — uncovered** |
| 214 | `S_WHPUMP` | 32 |
| 227 | `S_WDPUMP` | 21 |
| 237 | `S_WWAIT`  | 9 |
| 248 | `S_RHREQ`  | **0 — uncovered** |
| 258 | `S_RHCOLL` | 5 |
| 276 | `S_RHWAIT` | 4 |
| 303 | `S_RPREQ`  | **0 — uncovered** |
| 313 | `S_RPPUMP` | 31 |
| 323 | `S_RPWAIT` | 3 |

**Eight of twelve are covered; four are not.** The survivors are the four
`*REQ` arms, where the device asserts an error before its request is granted.
Reaching them needs an `err_at_req` mode the device model does not have. That
is written down here rather than left to be rediscovered, because a phase list
that reads as complete is worse than one that names its gaps. The other
outstanding item is the same: `09_verification.md:56` sets the bar as "cut at
randomized commit points ... every record type cut >= once", and this suite
uses fixed cut points on both sides, so the randomized half is still owed.

### On checks that cannot fail alone

Three added checks are implied by a neighbour: `T16 the neighbour's stored
bytes are untouched` is subsumed by `no other region's bytes moved` now that
the latter compares all of `REG_BYTES`; `T15 a torn record never restores as a
valid one` is weaker than the CRC-branch pin following it; and `T17 the port is
idle after the late failure` is structurally implied by `rc == 1` plus the next
commit succeeding. That last one states F02.8's busy envelope -- busy low once
the terminating pulse has passed -- on the signal that carries it, so it sits
on the specification side of the rule below rather than restating the
implementation. They are kept deliberately, because a weak specification
claim beside a strong implementation pin records WHAT is required separately
from HOW the port happens to satisfy it today, and the two drift apart.

The rule this suite follows, stated once: a check may restate a stronger
neighbour when it states the SPECIFICATION, but a check that only restates the
same implementation fact in other words is removed. That is why `rc != -1` was
dropped from T17 while the three above were kept.
