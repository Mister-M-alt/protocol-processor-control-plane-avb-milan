<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# nvm_port — KL_pp_nvm_port class-F suite

Proves the class-F NVM port (`hdl/packet_engine/KL_pp_nvm_port.sv`,
[02 §8](../../docs/architecture/02_interfaces.md) F02.8 +
[07 §5](../../docs/architecture/07_memory_maps.md) F07.8): `make` = build + run,
exit 0 = PASS, 90 checks. `-GMAX_PAYLOAD_P=1024` pins the geometry the C++
constants mirror.

The harness plays BOTH neighbors, independently of the RTL: a **manager BFM**
that frames records per 07 §5.2 (magic 0x1722, layout_version, record_id,
payload_length, crc16 — 16-bit fields big-endian on the stream; crc computed by
the harness, opaque to the DUT) and streams them with configurable stalls, and
a **device model** implementing the region port (req/gnt with delay, op
READ/WRITE/ERASE_REGION, region + offset + len, stalling byte phases,
busy/done/err with completion delays, error injection at any command index or
data byte, a seeded byte store, and a log of every accepted command (`ops`), and a log of every byte the port
put on the device write bus (`sent`), which is what the rule below points a
check at when the assertion must not read the array).

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
then WRITE(0, 8+plen). The destructive window is the WHOLE commit, not just the
WRITE: a cut at `S_WWREQ` errs with zero bytes on the bus and `erase_count`
already advanced, so **`err` never means "the saved set is unchanged"**. A cut
inside the WRITE leaves the region erased plus a
partial record: the record being written is gone, and on the backends that
erase in place **so is whatever it replaced**. That is a property of writing a
slot in place. The flash map's A/B slots are related but do NOT cover it: they
are per record SET, whole-image with read-back-then-promote, and this port has
no slot notion at all. What covers a torn SINGLE record is the crc16, and it IS
implemented — in the manager, not here: `hdl/acmp/KL_acmp_nvm_shadow.sv`
serialises it (`:402-403`), accumulates it (`:627`) and gates the record on it
(`:316`), with the per-record vendor-default policy at `:54-55`. It is
instantiated at `hdl/top/protocol_processor_top.sv:2238` and pinned by
`tb/acmp_nvm`. An earlier revision of this file said nothing implemented it,
which was wrong and is the third time this file has asserted an absence without
checking for the presence. It is NOT
universal: measured with the RTL byte-identical, a backend that both answers
ERASE lazily and buffers writes to the page leaves the old record intact. So
the suite pins what the port DOES guarantee, and where a claim about the array
follows a device `err` it is either moved onto the bus or conditioned on what
the array actually holds. Claims that follow a `done` are asserted directly and
need no condition, which is most of them. Counted in the unit the claim is
about, CHECK sites whose condition depends on the array: **16 sites, of which 2
condition on it and 14 do not**. Counting this needs care and got it wrong once:
`T16 no other region's bytes moved` reaches the array through `other_moved`, a
local hoisted 35 lines above its CHECK, so every text sweep for `store` missed
it -- and that site is the #70 isolation claim itself, the most important member
of the set. See "Where a check may read from" for the rule and its one known exception,
`T1 erase visible past the record`, which asserts on the array after a device
`done` and reddens under a lazy-erase backend:

- **T15** a torn commit reports `err` and never `done`, with busy low at the
  pulse; the cut is proven real on the BUS (ERASE then a WRITE that stopped 12
  bytes in, checked against the write-handshake log) rather than by reading the
  array, which is the backend's business; unless the old record survived the
  tear, it never restores as a VALID record — either the port refuses it at the
  header or the bytes it forwards fail the manager's CRC,
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
  still holds T5b's record (the `f5b` commit in T5) eleven phases later, so that
  spelling passed even under a mutant where the port wedged and issued no
  device traffic at all. What actually defends the claim is the pair of checks
  that READ THE DUT: the op log must show ERASE then the WRITE for this record,
  and the region's erase count must have moved. Both are driven by `dev_req_o`,
  and a port issuing nothing can fake neither. The byte comparison beside them
  reads the write-handshake log `sent`, NOT the region, so it pins what the
  port put on the bus and carries no model dependency. An earlier spelling
  compared the region's bytes and was doubly wrong: `torn[0..4]` is
  byte-identical to the T5b residue prefix, so its separating power sat
  entirely in `store[1][5] == 0xFF`, and it reddened under a half-page model
  with the RTL untouched.
- **T17** the cut a NOR device actually produces. T15 and T16 cut while bytes
  are still moving, but a real program failure is not reported then: the device
  latches the bytes, starts the program cycle, and raises its error when that
  cycle ends — after the last byte, busy still high. That is the port's
  `S_WWAIT` arm, the widest window in a commit, and nothing else in the suite
  enters it. The phase pins what the port owes there — `err` and never `done`,
  busy released and the port idle afterwards, and the port usable for the next
  commit. It also exercises the READ side, because whether the port still
  serves a region after taking `S_WWAIT`'s error exit is its own property and
  T17 otherwise covered only the write side. It does NOT restore the torn
  region, because what that region holds is the backend's choice: it commits
  a good record
  first, which ends in `done` so every model agrees what the array now holds,
  and only then reads it back, pinning the device ops the way T2 does so a
  fabricated restore cannot pass. Two earlier spellings compared against the
  record being written and against the array, and both pinned the model.
- **T18** the same argument on the RESTORE side. A NOR read fails the way a
  program does: an ECC or timeout error surfaces when the read cycle ENDS, not
  mid-stream. `S_RPWAIT` is the exact mirror of the arm T17 closed, `S_RHWAIT`
  is that window on the header probe, and `S_RHCOLL` is an error during the
  header collect — which sits on the boot restore walk, the one path where a
  torn image is actually consumed. All three survived the suite before T18.

Mutation-proven (backup → sed → run → restore → green). **These figures are
against the 90-check suite; earlier revisions of this file carried 55-era
numbers for M1 to M3 long after the suite grew.**
- **M1** commit skips the ERASE: the transition INTO `S_WEREQ` (`:175`)
  rewritten to `S_WWREQ`, so no ERASE is ever issued. **Fails 19 of 90**
  (op-log shape, erase pulse/visibility, erase-error path).
  The description used to read "`S_WEREQ` target rewritten", which is ambiguous
  and the two readings differ enormously: rewriting what `S_WEREQ` itself
  transitions to (`:189`), so the ERASE is REQUESTED but never awaited, **fails
  only 1 of 90**. That sibling is a real coverage gap and is recorded as one
  rather than hidden by the ambiguity.
- **M2** magic gate dropped from `hdr_ok_w`: **fails 5 of 90** (both bad-magic
  refusals and the nothing-forwarded check). Note this drops BOTH magic bytes;
  the low byte alone is uncovered, see the gap list below.
- **M3** payload pump off-by-one (`bcnt_r == plen_r` for `plen_r - 1`, both
  directions): **fails 68 of 90** (every data-phase op times out or mismatches).
- **M4** (2026-08-20) the write phase swallows the device error (`S_WDPUMP`'s
  `if (dev_err_i)` forced false): **fails 29 of 90**. A torn commit then looks
  clean, which is exactly the false success #70 exists to remove. Six checks
  survive, enumerated by RUNNING the mutation rather than reasoning about it,
  because earlier versions of this file twice published a survivor list written
  from what M4 ought to do: `T15 seed record committed` (it precedes the cut);
  `T15 busy raised then low at the err pulse` and `T17 busy raised then low at
  the err pulse` (a wedged port holds busy high and never pulses, so neither
  pair is contradicted); `T15 the cut was real: ERASE then a WRITE stopped 12
  bytes in`; and T16's two isolation checks, `erased no other region` and `no
  other region's bytes moved`, which pass VACUOUSLY because a wedged port
  issues no further device traffic and so nothing else can move. That vacuity
  is what the T16 guard exists to answer.
- **M5** (2026-08-20) the completion window swallows the device error
  (`S_WWAIT`'s `if (dev_err_i)` forced false): **fails 16 of 90**, and survives
  the whole suite without T17. The port waits for a `done` a failed device will
  never send, so the commit never answers at all: `run_op` returns -1 after
  100,000 cycles. `busy_seen && busy_ok` does NOT catch this, and the comment
  at that check says so: a wedged port holds busy high, so `busy_seen` is true
  and no pulse arrives to contradict `busy_ok`.
- **Probes** (mutations of the TEST, not the RTL). Arming T16's tear as
  `arm_err(1, -1)`, so the WRITE fails before its first byte moves, fails 1 of
  90. Replacing the torn commit with a bare `rc = 1` and no device traffic at
  all — the port the T16 prose names as the threat — fails 3 of 90. Under the
  previous guard that second probe failed only ONE check, the erase count,
  while the payload guard passed on residue from an earlier phase.
- **Model probe**: changing only the DEVICE MODEL to roll the last 4 bytes back
  to 0xFF on a completion-window failure — the half-programmed page a real NOR
  may leave — must not redden a check about the PORT. The T17 restore check
  did exactly that, and so did the T16 byte comparison. Neither does now, by
  two different routes: T16's moved onto the bus, T17's moved to assert after
  a `done` where the array is known. The half-page model is 90 PASS, 0 FAIL.
  See the matrix below for every pre-fix form against every model.

### Device-error arm coverage

`KL_pp_nvm_port.sv` has twelve `if (dev_err_i)` arms. Each was forced to
`1'b0` in turn and the suite re-run, so this table is measured, not argued —
and it is now **checked by a script rather than by hand**: `measure_figures.py`
re-runs every arm, cross-checks the arm COUNT against the RTL, and re-measures
**every figure in this file**: eleven mutations and probes, seven device-model
result rows, and all thirty cells of the pre-fix matrix. CI runs it.

The covered set is DERIVED, not asserted. Every `N of M` and `N PASS, N FAIL`
here is a claim by default, satisfied only by a measurement or by an explicit
waiver whose reason the script prints on a clean run. A new figure is a hard
error until it is measured, so it cannot be added silently -- and it cannot be
deleted to silence the gate either, because deleting it makes the measurement
that owns it fail instead.

It took four rounds to get the gate itself honest, and the failures belong in
the record because they are the same failure four times. Version one checked
only denominators and result-row sums, so seven of eight falsifications walked
past it -- including reverting a numerator to the exact stale value the gate had
been written after finding. Version two added a table of named mutations and
still missed M3, the very numerator that had gone stale, because nothing
required the table to cover this file's own claims. Version three coupled the
two but matched ONE PHRASE, `fails N of M`, so three figures already present
were invisible to it. Version three also declared the thirty matrix cells
unmeasurable because their check forms no longer exist in the tree -- a cost
choice dressed as an impossibility, contradicted by this file's own sentence
that re-deriving the matrix is re-running it. Each version closed a narrower
class than it claimed. Inverting the default is what ended that, and it paid for
itself on its first run by finding that the coincident-completion figure could
not be re-derived at all: its recipe had never been committed. The number was
sound; it was unverifiable rather than wrong, so the recipe was committed rather
than the figure retracted.

Run `make -C tb/nvm_port figures` after any change to this suite. It takes 36
Verilator builds, about four minutes, which is the price of figures that four
review rounds found stale. Two of those rounds found the arm TABLE stale; the
other two found stale numerators elsewhere in the file, which is why the gate
covers every figure rather than the table alone.
The arm table specifically has been stale twice. Splitting one T17 check moved every row reaching T17;
later, replacing an array-negative check with a bus check moved every row whose
mutant WEDGES BEFORE T15, because the old check survived those mutants and the
new one does not. The second time a spot-check missed it: M4 and M5 were
re-measured and both were genuinely unchanged, since under those the bytes were
really sent before the swallowed error. **Checking only the figures you changed
is the wrong sample.** Any change to the suite invalidates every number here and
the whole table has to be re-swept.

| line | state | checks failed |
|---|---|---|
| 185 | `S_WEREQ`  | **0 — uncovered** |
| 194 | `S_WEWAIT` | 46 |
| 204 | `S_WWREQ`  | **0 — uncovered** |
| 214 | `S_WHPUMP` | 41 |
| 227 | `S_WDPUMP` | 29 |
| 237 | `S_WWAIT`  | 16 |
| 248 | `S_RHREQ`  | **0 — uncovered** |
| 258 | `S_RHCOLL` | 9 |
| 276 | `S_RHWAIT` | 6 |
| 303 | `S_RPREQ`  | **0 — uncovered** |
| 313 | `S_RPPUMP` | 40 |
| 323 | `S_RPWAIT` | 3 |

**Eight of twelve are covered; four are not.** The survivors are the four
`*REQ` arms, where the device asserts an error before its request is granted.
Reaching them needs an `err_at_req` mode the device model does not have. That
is written down here rather than left to be rediscovered, because a phase list
that reads as complete is worse than one that names its gaps. The other
outstanding item is the same: `09_verification.md:56` sets the bar as "cut at
randomized commit points ... every record type cut >= once", and this suite
uses fixed cut points on both sides, so the randomized half is still owed.

### Reachability pins, and why they exist

T17 and T18 name the windows they cover, and until a review probed them nothing
CHECKED that they reached those windows. Both were measured green under probes
that removed the thing being tested:

- moving T17's cut off the completion window, so `S_WWAIT` is never entered:
  **90/90 green** before, now fails on `T17 the cut was in the completion
  window: every byte sent first`.
- replacing T18's three device errors with a bad stored magic, so ZERO device
  errors occur anywhere: **90/90 green** before. The op log alone could not tell
  them apart, because the port issues the header READ BEFORE validating it, so
  a refusal and a device error produce the same two ops. Distinguishing them
  needed a count of DEVICE-raised errors, separate from the port's own `err`
  pulses; `dev_errs` is that counter and each arm now pins it.
- replacing a T18 arm with a bare `r = 1`, port never touched: now fails 3.

The general shape: a phase that names a window is not the same as a phase that
proves it entered one, and the evidence for the difference lived only in the
"Device-error arm coverage" table, which this file says has gone stale twice.
That table, and every other figure here, is now re-measured by
`make -C tb/nvm_port figures` and gated in CI.

### On checks that cannot fail alone

TWO added checks are implied by a neighbour:

- `T16 the neighbour's stored bytes are untouched` is subsumed by `no other
  region's bytes moved`, now that the latter compares all of `REG_BYTES`.
- `T17 the port is idle after the late failure` is structurally implied by
  `rc == 1` plus the next commit succeeding. It is kept because it states
  F02.8's busy envelope -- busy low once the terminating pulse has passed --
  on the signal that carries it, so it sits on the specification side of the
  rule below rather than restating the implementation.

Both are kept deliberately, because a weak specification claim beside a strong
implementation pin records WHAT is required separately from HOW the port
happens to satisfy it today, and the two drift apart.

The rule this suite follows, stated once: a check may restate a stronger
neighbour when it states the SPECIFICATION, but a check that only restates the
same implementation fact in other words is removed. That is why `rc != -1` was
dropped from T17 while the two above were kept.

**A third check was listed here and the claim was FALSE**, so it is recorded
rather than quietly deleted. `T15 unless the old record survived, a torn image
never restores as valid` was called weaker than the header-agreement check
beside it, on the evidence of twelve arms and four models showing no
divergence. Measured directly by moving T15's tear into the completion window:
that check FAILS while the header check PASSES, 2 of 90. It can fail alone, so
it is not a member of this section at all. The error is the same shape as the
corollary retracted below -- a general claim generalised from the states that
happened to be tried -- and this file has now made it twice, because "no
divergence across the cases I ran" is not "cannot diverge".


### Where a check may read from

FIVE checks in this file were written against what the flash ARRAY held and
had to be rewritten, because what the array holds is the device MODEL's choice,
not the port's behaviour. FIVE device models and one combination were run, RTL
byte-identical. Four vary what the array RETAINS; the fifth, coincident
completion, varies the HANDSHAKE instead -- the device raises `dev_done_i` on
the same edge that moves a pump's final byte, which `KL_pp_nvm_port.sv:151-153`
says the sticky `done_seen_r` latch exists for. It is a contract freedom rather
than a broken peer, and the port handles it. Its whole interest is that
deleting that latch is INVISIBLE without it:
this one, which keeps every accepted byte; a half-page model, which drops the
last four on a failure; a page-buffered NOR, which keeps none until the program
cycle ends with `done`; and a lazy-erase backend, which answers ERASE with
`done` without rewriting the array at all. None is invented. The port's own
header names the last one ("backends without erase semantics answer ERASE with
done at once"). `02 SS8` does list host filesystem via `mgmt` among the
permitted backings in general, but NOT for these records: the parent's
`sw/litex/milan_soc.py` places the binding records in a journal slot that is
deliberately RAW, no filesystem,
for exactly this reason. The lazy-erase freedom is real and is what the models
below exercise; the host-filesystem framing was too wide and is withdrawn.

The rule:

> **After a device `done`, assert on the array only for what the port itself
> put there. After a device `err`, assert on what the port SENT or REQUESTED,
> never on what the array retained.**

The `done` clause carries that qualifier because a device SIDE EFFECT is not
the port's behaviour either: `T1 erase visible past the record` asserts after a
`done` and still reddens under lazy erase, since whether an ERASE rewrites the
array is the backend's business. T1 is pre-existing and left as it is, named
here as the one known remaining member rather than quietly fixed. It is the
exception the power-cut preamble points at.

**A negative claim about the array is NOT automatically safe.** An earlier
version of this section said it was, and that was false: under lazy erase
COMBINED WITH page buffering the old record survives a torn commit intact, so
`the torn image is neither the old record nor the new one` reddens for a device
doing exactly what #70 asks. The two freedoms are needed TOGETHER, measured by
printing `old_intact` after T15's tear with the RTL byte-identical:

| model | old record intact |
|---|---|
| pristine | 0 |
| lazy erase alone | 0 |
| lazy erase + page buffering | **1** |

Under lazy erase alone the write still lands and the old record dies with it.
A check that goes red when the device gets the requirement RIGHT is the wrong
check. Where the property genuinely is about the array, CONDITION it on what
the array holds rather than asserting it, the way T15's header check does.

But a condition must be LIVE, or it hides a check as effectively as it rescues
one: a guard always true under the model CI runs silently disables everything
behind it. Check both arms independently. Fix B's disjunction was verified that
way -- under the pristine model `old_intact` fails alone while
`refused || crc_rejects` passes alone, and under lazy erase PLUS page buffering
the reverse -- lazy erase alone gives the same result as pristine -- so it
is conditioned AND still doing work in CI.

Applied here:

- **T16** reads `sent`, the write-bus handshake log, instead of the region's
  bytes. Identical discriminating power, zero model dependency.
- **T17** commits a good record first, so the array is known under every model
  because that commit ended in `done`, and only then exercises the read side,
  pinning the device ops the way T2 does so a fabricated restore cannot pass.
- **T15** had THREE members. Its branch pin surfaced under the page-buffered
  model; the other two needed lazy erase AND page buffering together:
  - its branch pin no longer records WHICH branch fired, because which one
    fires legitimately differs by device -- this model keeps the bytes so the
    CRC rejects, a page-buffered NOR discards them so the port rightly refuses
    at the header. It pins that the branch the port took AGREES with what the
    array holds, which is the port's behaviour under any model.
  - "the cut was real" is stated on the bus: ERASE then a WRITE that stopped
    12 bytes in, checked against `sent`.
  - the #70 property is CONDITIONED rather than asserted -- unless the old
    record survived, a torn image never restores as valid.

| model | result |
|---|---|
| pristine | **90 PASS, 0 FAIL** |
| half-page | **90 PASS, 0 FAIL** |
| page-buffered NOR | **90 PASS, 0 FAIL** |
| lazy erase | 89 PASS, 1 FAIL, only the pre-existing `T1` |
| lazy erase + page-buffered | 89 PASS, 1 FAIL, only `T1` |
| coincident completion | **90 PASS, 0 FAIL** |

A device model variant is the cheapest way to find a check that tests the
harness rather than the DUT, and it belongs in the standing mutation set. Each
addition found members the previous ones could not. Rather than say which,
here is the measurement: every pre-fix form re-injected, every model run, RTL
byte-identical throughout. An attribution sentence drifts from the runs; this
cannot, because re-deriving it is re-running it.

| pre-fix form | pristine | half-page | page-buf | lazy | lazy+pb |
|---|---|---|---|---|---|
| T16 byte comparison | pass | **FAIL** | FAIL | FAIL | FAIL |
| T17 restore vs the record | pass | **FAIL** | FAIL | pass | FAIL |
| T17 restore vs the array | pass | pass | **FAIL** | pass | pass |
| T15 branch pin | pass | pass | **FAIL** | pass | FAIL |
| T15 cut was real | pass | pass | pass | pass | **FAIL** |
| T15 #70 property, unconditioned | pass | pass | pass | pass | **FAIL** |

Bold marks first discovery, reading left to right. By MEMBER that partitions as
**half-page 2, page-buffered 1, the combination 2 = the five** named above; the
six rows exceed the five members because T17's restore appears twice, which is
the most instructive line in the table:

- **T17's restore was found twice, by two different models.** Half-page killed
  the record-relative spelling. The array-relative fix written in response
  SURVIVES half-page -- the very model that prompted it -- and page-buffered
  killed it anyway. Passing the model that found the bug is not evidence the
  fix is model-independent.

Every row passes under pristine, which is why all six survived until a model
beyond the shipping one was tried. Trying one model is how a family of five
looked like a family of one; trying one freedom at a time is how the last two
stayed hidden after three models had been run.


### What this suite does NOT establish

Recorded so the phase list does not read as closing #70:

- **#70's desk-proof sentence is "leave the PREVIOUS SAVED SET intact", and this
  layer cannot meet it.** T16 pins "every OTHER record", which is honest and is
  what the port can guarantee, but it is weaker precisely on the record being
  rewritten. Covering that is the crc16's job, and the crc16 exists — in
  `KL_acmp_nvm_shadow`, on the synthesized path. What does not exist is A/B
  promotion. So the gap is narrower than "nothing covers it": the manager can
  reject a torn record, it just cannot recover the one it replaced.
- **A restore failure is one signal for three situations**: a refused region, a
  torn header, and a tear after a complete header has already been forwarded.
  T18 pins that each reports `err`; it does not distinguish them, and the
  specification requires the manager to. Tracked as issue #20; issue #16 stated
  this as a port-face defect and was closed, because the manager already makes
  the distinction — what survives is narrower and is #20.
- **NONE of the twelve `dev_err_i` arms can fire on any bitstream that exists.**
  The parent answers this port's device face with a blank-flash responder that
  ties `nvm_dev_err_i` to `1'b0` (`milan-fpga hdl/milan/KL_pp_shadow.sv:1010`).
  That is the largest limitation here and it was omitted from this list: the
  whole error-handling half of the port, and every row of the "Device-error arm
  coverage" table, is unreachable until a real backend lands. Note the responder is not inert - it
  drives `nvm_dev_done_i` and answers every command - so what makes issues #14
  and #15 unfirable is that it is WELL BEHAVED, not that error is tied off. The
  port's own `nvm_err_o` is live and fires on every restore on the shipping
  build.
- **The device model is well-behaved by construction**, so the port's tolerance
  of a badly-behaved one is untested. The five variants in "Where a check may read
  from" -- pristine, half-page, page-buffered NOR, lazy erase, and lazy erase
  plus page buffering -- all vary what the array RETAINS; none varies the handshake. Issues #14 and #15 are both defects
  this suite is structurally blind to for that reason.
- **No phase asserts `rst_n` after init.** "Power cut" means "the device raised
  `err`" throughout this file. A real reset mid-commit is the path a power cut
  actually takes, and it is not modelled: after one, the port forwards a 32-byte
  torn image with 23 bytes erased as a well-formed record. That is #70's
  "half-record that restores as garbage", reached the way it actually happens.
  Tracked as issue #18.
- **Four RTL mechanisms have zero coverage**, found by mutation and tracked in
  issue #19: the low magic byte (`hdr_r[1]`, since both magic tests corrupt only
  the high byte), the payload bound's upper edge (`<=` to `<` is green, so the
  largest legal record can be silently refused), the sticky `done_seen_r` latch
  (deleting it two ways is green; under a coincident-completion model the same
  mutations fail 68 of 90), and the short-read defence at `:268`, whose failure
  mode is a hang on the boot restore walk and which no `dev_err_i` mutation can
  reach because it guards `dev_done_i`.
- **The `err` clause of the rule has THREE known exceptions**, and T1 is not
  among them: T1 asserts after a `done` and reddens only under lazy erase, so
  it is a `done`-clause exception. The three are T16's isolation checks, and a coarse-erase
  model where a sector spans regions reddens all three with the RTL untouched.
  The erase-count check does not redden, which is the asymmetry the rule exists
  to describe.
