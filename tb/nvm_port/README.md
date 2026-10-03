<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# nvm_port — KL_pp_nvm_port class-F suite

Proves the class-F NVM port (`hdl/packet_engine/KL_pp_nvm_port.sv`,
[02 §8](../../docs/architecture/02_interfaces.md) F02.8 +
[07 §5](../../docs/architecture/07_memory_maps.md) F07.8): `make` = build + run,
exit 0 = PASS, 393 checks in each of three builds. The Makefile's `MAXP`,
`-GMAX_PAYLOAD_P=1024`, pins the geometry the C++ constant `MAXP` mirrors. The device-face deadline is built
three times: `-GMEM_TIMEOUT_CYC_P=100`, the bound every figure in this file is
measured at, 37, a small odd bound, and 20, the smallest the suite's fixed
protocol delays leave legal; the Makefile hands each build's value to the C++
as `TMO`. Every harness wait that meets the deadline is derived from `TMO`, and
every cut or poke inside an operation is named on the bus (see "Three bounds"
below), so every build grades the same contract. `make` then builds the
randomized harness, `fuzz_main.cpp`, at bounds 1, 2, 3 and 37: the port's
smallest legal bounds, which the suite cannot reach, and one beside it, each
at `MAX_PAYLOAD_P` = 65,527, the parameter's largest legal value (see "The
randomized harness"). Each build prints its own tally, and the last line
is their sum. `make` first runs `elab_bounds.sh`, the port's two elaboration
guards: the deadline parameter refused by name at 0, 2^31 and 2^32 - 1, built
clean at 1 and 2^31 - 1; `MAX_PAYLOAD_P` (issue #17) refused by name and by its
bound, 65,527, at 65,528, 65,535 and 2^32 - 1, built clean at 1,024 and 65,527.
That refusal must be a `$fatal`: its line must carry `%Warning-USERFATAL` (or
`%Error`), since Verilator reports `$error` and `$warning` as failing warnings
too, and where sv2v and yosys are installed yosys must build 65,527 and stop at
65,528, which it does only for a `$fatal`.

The harness plays BOTH neighbors, independently of the RTL: a **manager BFM**
that frames records per 07 §5.2 (magic 0x1722, layout_version, record_id,
payload_length, crc16 — 16-bit fields big-endian on the stream; crc computed by
the harness, opaque to the DUT) and streams them with configurable stalls, and
a **device model** implementing the region port (req/gnt with delay, op
READ/WRITE/ERASE_REGION, region + offset + len, stalling byte phases,
busy/done/err with completion delays, error injection at any command index or
data byte, a seeded byte store, and a log of every accepted command (`ops`), and a log of every byte the port
put on the device write bus (`sent`), which is what the rule below points a
check at when the assertion must not read the array). It can also misbehave on
the HANDSHAKE, which is the half the five array-flavoured variants never
reached: it will pulse a `done` that belongs to no command at all, it will
answer an ERASE on the grant cycle, and it will answer any command on the same
edge that moves that command's final byte (see "Completion ownership" below);
it will end a command short, withhold any one event it owes for a set number of
cycles or for ever, and keep a command running across the port's reset. Its
grant is REGISTERED, as the parent's backend's is: it takes the request on the
edge that samples it and grants a cycle later, whether or not the request is
still up. Four of these behaviours are standing models, left on for a whole run
(see "The handshake models").

Covered: the commit envelope ERASE_REGION → WRITE(0, 8+plen) with the device
store byte-exact against the manager's framed record and, on a backend with
erase semantics, the erase visible past it; the restore envelope READ(0,8) → READ(8,plen) with the returned stream
byte-exact; zero-payload records both ways (restore then issues only the header
probe); stall torture on all four byte interfaces at once (gnt and done
delayed); back-to-back ops with req re-asserted the cycle after the pulse;
req-while-busy ignored (single outstanding, F02.8); busy/done/err sequencing —
busy high mid-op, LOW at the pulse, done and err mutually exclusive and exactly
once per op; a device error during ERASE surfacing as exactly one manager-face
err with the WRITE never issued, plus recovery on retry; errors mid WRITE-data
and mid READ-payload; refusals per 07 §5.2 with zero (further) device traffic —
bad magic on commit, oversize payload_length on commit, bad magic and oversize
length in the stored record on restore (nothing forwarded to the manager); the
port serviceable again after every refusal; and completion ownership — a `done`
that belongs to no command ignored in every state that owns none, on both the
commit and the restore path, and the two completions the port must take: one
riding its own grant, one riding a command's final byte; the device-face
deadline in each of the twelve states in which the device owes the port an
event, at its boundary, and the owed command it leaves behind (T24); a command
the device ends short, in every data phase (T27); a reset at six stages of a
commit, and of the port alone (T25); the low magic byte and the payload
bound's legal edge (T26); and the two ways the deadline's count holds without
charging the device, each at the edge of the bound (T30). Every operation of
the run is also graded on what it was owed, whatever the device model (the RW
checks, "The handshake models"), and the randomized harness grades the
deadline against random devices and managers at the smallest legal bounds
and at the payload bound's largest.

Known limits (honest): the CRC16 is carried, never checked — that is the
manager's job per 07 §5.3, so a corrupt-crc record passes this port by design
and must be caught by the NVM-manager suite (P4). The byte order of the 16-bit
header fields on the stream is a design decision of the port (network order),
not pinned by the doc.

### Torn commits: a device `err` (T15-T18) and a reset (T25)

Issue #70, added 2026-08-20; split by issue #18. Two different events tear a
commit, and this file used to call both "power cut". **T15-T18 model a DEVICE
that fails mid-commit and says so with `err`**: the port's own state machine
runs on, and every phase ends in the port's `err`. **T25 models the event a
power cut actually is**, `rst_n` asserted with the commit in flight, which the
port cannot report at all: it resets to idle and forgets the command (see "A
reset mid-commit"). A commit is ERASE(region)
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
serialises it (`:482-483`), accumulates it (`:775`) and gates the record on it
(`rrec_ok_w`, `:391-395`), with the per-record vendor-default policy at
`:90-92`; the D3 writer gates its records the same way (`frame_ok_w`,
`hdl/aecp/KL_aecp_nvm_writer.sv:482-485`). The shadow is
instantiated at `hdl/top/protocol_processor_top.sv:2726` and pinned by
`tb/acmp_nvm`. An earlier revision of this file said nothing implemented it,
which was wrong and is the third time this file has asserted an absence without
checking for the presence. It is NOT
universal: measured with the RTL byte-identical, a backend that both answers
ERASE lazily and buffers writes to the page leaves the old record intact. So
the suite pins what the port DOES guarantee, and where a claim about the array
follows a device `err` it is either moved onto the bus or conditioned on what
the array actually holds. Claims that follow a `done` are asserted directly and
need no condition, which is most of them. Counted in the unit the claim is
about, CHECK sites whose condition depends on the array: **18 sites, of which 3
condition on it, 1 conditions on the backend's erase semantics instead (T1's
erased tail) and 14 do not** (T24's setup asserts after a `done`; T25's
header agreement conditions, as T15's does). Counting this needs care and got it wrong once:
`T16 no other region's bytes moved` reaches the array through `other_moved`, a
local hoisted 35 lines above its CHECK, so every text sweep for `store` missed
it -- and that site is the #70 isolation claim itself, the most important member
of the set. See "Where a check may read from" for the rule. Its one known
exception was `T1 erase visible past the record`, which asserted on the array
after a device `done` and reddened under a lazy-erase backend; it now asserts
the erased tail only on a backend with erase semantics, and the bus carries
the port's half. The torn commits:

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
- **T25** the power cut itself: `rst_n` mid-commit at six stages, with the
  device reset too, then the torn record graded exactly as T15 grades its own;
  and two cuts of the port alone. See "A reset mid-commit".

### Terminal cause (issue #93, S1)

`nvm_err_cause_o` names what an err was: **1 DEVICE** for every error the device
raised, in any state, and for every command it ended short (refusal (d), T27);
**2 UNFRAMED** only for a header this port refused, one the device delivered
whole or one the manager streamed on a commit; **3 DEADLINE** only for an
operation the deadline ended (T24): the device said nothing; 0 with done and
on every cycle without err. Before it, all
of those ended in the same err with nothing forwarded, so a manager restoring a
record could not tell a failing device from a record that is not there.
UNFRAMED does not prove erased media, and nothing here claims it does. T23 runs
after the end-of-run idle check has graded every earlier phase, and from a
reset of its own, so a mutation that wedged the port earlier cannot fail it as
collateral; T24, T25 and T27 after it each start from a reset of their own for
the same reason, and the RW checks close the run. It grades: the header read refused at the grant (`S_RHREQ`'s
error arm, which no phase reached before), a device error inside the header, a
header read ended short at five bytes, a device error after the header and before
its done, a device error in the payload and on a commit's ERASE, all DEVICE; an
erased header (eight 0xFF), a corrupted magic and a payload_length over the bound,
each delivered whole, and a commit whose own header fails the gate, all UNFRAMED;
and a done reading 0, with no cycle of the whole run reading a cause without err.

### Completion ownership (issue #14)

`done_seen_r` is sticky because a device `done` may land on the same edge a pump
moves its last byte; the wait states consume it a cycle later. The set used to
sit OUTSIDE the state case, so the flag was armed in EVERY state — including the
states where this port has no device command outstanding at all. A `done`
arriving while a commit's header was still being collected was then consumed by
`S_WEWAIT` as the ERASE's, and the WRITE went out into a region the backend was
still erasing, reported to the manager as `done` rather than `err`.

Nothing here could see it, and the reason belongs beside the model rules below:
**a harness whose device model is well behaved cannot exercise the port's
tolerance of one that is not.** This model only ever completed a command it had
accepted, so "sticky per command" and "sticky globally" were the same port to
it. The array-flavoured variants vary a different axis and none of them reaches
this.

What the port owes is now stated in its own header as refusal (c) and gated on
it (`dev_cmd_owned_w`, `KL_pp_nvm_port.sv:244-255`): a completion is this port's
from the cycle its grant is observed — that cycle may carry the completion —
until the wait state consumes it, and ownership retires at the terminal state,
at the next accept, and at reset. `S_WHDR`, an ungranted `*REQ`, `S_RHFWD`,
`S_FIN` and `S_IDLE` own nothing. Note where that contract comes from:
[02 §8](../../docs/architecture/02_interfaces.md) leaves the device face free,
so nothing above this module says whether a grant may carry its own completion.
The port's header is the decision and T21 is the measurement of it, not a
licence the architecture handed down.

`arm_unsolicited_done(after_ops, in_req)` is the device the harness could not
play before: it pulses one `done` for no command at all, in a window named by
what the BUS shows — how many commands the backend has accepted, and whether a
request is up that it has not granted — so no check reads DUT state, and an
arming that never found its window reports zero pulses rather than passing
quietly.

| phase | the stray `done` lands in | reached by the withdrawn `S_WHDR` clear | the counter that fails when ownership admits that state |
|---|---|---|---|
| T19a | `S_WHDR`: collecting the header, nothing issued | yes | `req_while_owed` |
| T19b | `S_WEREQ`: ERASE requested, ungranted | no | `req_while_owed` |
| T19c | `S_WWREQ`: WRITE requested, ungranted | no | `pulse_while_owed` |
| T20a | `S_RHREQ`: header READ requested, ungranted | no | `fwd_while_owed` |
| T20b | `S_RHFWD`: after the header read, before the payload read | no | `pulse_while_owed` |
| T20c | `S_RPREQ`: payload READ requested, ungranted | no | `pulse_while_owed` |

The last column is measured the same way as everything else here: ownership was
widened by exactly one state term, one build each, and the counter named is the
one that goes non-zero. It is in the table because a phase that names a state
and cannot fail for it is the defect this suite has now made twice -- and the
`S_RHREQ` row was that defect until the third counter landed.

T21 and T22 are the other edge of the same window, and between them they are why
it opens at the grant and stays open through the pumps. T21: a backend without
erase semantics -- the one the port's own header names -- answering ERASE with
`done` on the grant cycle. That completion IS the port's, and a fix narrowed to
"strictly after the grant" wedges the commit in `S_WEWAIT` against a device
doing nothing wrong. T22: a backend answering on the same edge that moves a
command's final byte, which is the coincidence the sticky flag was written for.
A cycle later the wait state reads `done_seen_r` rather than `dev_done_i`, so a
window narrowed to the grant handshake alone drops that completion and waits
forever for one the backend already sent. T22 runs it on the WRITE's payload
pump, on a header-only WRITE whose last byte moves in `S_WHPUMP`, and on both
reads of a restore, which is the four states of the window that can move a final
byte.

**Why the checks are on the bus.** A stray `done` consumed as somebody else's
does not corrupt the stream: the bytes still move, the record still lands, the
restore still comes back byte-exact. What changes is WHEN — and against a model
that will not grant a second command while one is in flight, the damage shows as
a port running one command ahead of its backend. So the three discriminating
counters count exactly that: cycles where the port requested a new device command
while the backend still owed a completion, cycles where it answered the manager
inside the same window, and cycles where it handed the manager a byte out of its
own header buffer inside it. None reads the array, so all three hold under
every model here. Against a backend that grants while busy — which this model
deliberately is not — the same one-command-ahead port is the WRITE landing in a
still-erasing region, which is the failure the ticket describes.

The third counter is not decoration, and the reason is the interesting part of
this suite's history. The first two are read a long way downstream of the
defect: a completion taken in an ungranted `S_RHREQ` is consumed by `S_RHWAIT`
the moment the eighth header byte lands, but the next REQUEST and the manager's
PULSE come a whole header forward later, so whether the backend is still owing a
completion by then is a property of its own delay rather than of the port. At
the delay T20 runs, both were back to zero and T20a passed under a mutant that
did exactly what the row says it must not: measured before this counter existed,
admitting an ungranted `S_RHREQ` into the window left the whole suite green,
T20a included. The forward counter fires
two cycles after the flag is consumed instead, and it is causal rather than
tuned -- the port may hand up a buffered header only once the read that filled
the buffer has completed, on any backend, at any delay.

The sticky latch is no longer invisible on the pristine model either: deleting the set
line now reddens 23 of 393 here, in T21, every arm of T22, T30's two latched
terminals and T27's two READs ended on their own grant. A completion the wait
state cannot read live is lost, so the port reaches its deadline where it owed
done, and then holds as owed a command the device has already ended, which
takes the rest of T27 and two RW checks with it; under a coincident-completion
model the same mutations fail 267 of 393.
Narrowing ownership the other way, by dropping the grant term, reddens exactly
the same checks, measured: T21 is the first phase that needs a completion the
wait state cannot read live.

**Both halves of the window are pinned now, and one half was not.**
`dev_cmd_owned_w` is the grant handshake plus eight state terms. The grant term
was defended from the start, by T21. The eight state terms were not defended by
anything: every backend the suite ran completed some cycles after the last byte,
a completion that lands with the port already in a wait state is read live from
`dev_done_i`, and the sticky flag is then never consulted at all, so the whole
state half could be deleted with the suite green. The coincident-completion
model in this file already reddened that mutation heavily, which says the terms
are load-bearing rather than dead; what was missing was a standing phase pairing
them with the completion semantics the port's own header allows. T22 is that
phase, and these are the measurements:

- ownership narrowed to the grant handshake alone **fails 61 of 393**: T22a,
  T22b and T22c lose the completion that rode their last byte, and every T24,
  T28 and T30 arm whose command the window no longer covers fails too (below).
- dropping the single `S_RHCOLL` term **fails 28 of 393**: the restore whose
  header read is answered on its own eighth byte loses that completion and
  never issues the payload read, so T22c's own witness reports one coincidence
  where it requires two, and T30b's restore, answered the same way, ends at
  its deadline; and T24's `S_RHCOLL` arm and T28g, whose deadline no longer
  marks the abandoned READ owed, so the next restore is issued over it.
- dropping the single `S_WWAIT` term **fails 3 of 393**: T24's boundary arm in
  `S_WWAIT`, a done on the very cycle the count reaches `TMO`, which is
  progress only for a command the port owns; and T28c with RW3, because a
  deadline in `S_WWAIT` then leaves the WRITE owed to nobody and the next
  commit is requested over it. Before the deadline this term could not be
  reached at all; see the list below.
- admitting an ungranted `S_RHREQ` into the window **fails 29 of 393**: T20a's
  forward counter, as before (this mutant was green until that counter
  existed), and now T24, T28 and T30e, because a deadline in that state then
  marks owed a command the device never took.

Since the deadline, the window answers two more questions than whose a `done`
is: whether a `done` is PROGRESS for the deadline, and whether a deadline
abandoned a command the device still owes. So every one of its terms is
load-bearing now, through T24, as the last two rows measure for one term each.

**What this does NOT establish.** Nothing here says the defect was reachable on
any bitstream. What answers this port's device face lives in the integrating
parent, which this repository does not contain and this suite does not model;
what is proved is the port's contract. Beyond that, here is exactly which part
of the fix each check reaches and which parts nothing here reaches, enumerated
rather than summarised because "one line of the fix" was the previous summary
and it was too small:

- **the four wait-state terms** of `dev_cmd_owned_w`: `S_WEWAIT`, `S_WWAIT`,
  `S_RHWAIT`, `S_RPWAIT`. Before the deadline each was dropped on its own with
  the suite green, a property of the FSM rather than of this suite: a wait
  state consumes `dev_done_i` on the cycle it arrives, so the sticky set beside
  it can never be read. The deadline reads the window too, and there they are
  not dead: a done in a wait state is progress only for a command the port
  owns, and a deadline there marks the command owed only if the port owns it.
  T24 grades both in every wait state; the `S_WWAIT` row above is that
  measurement for one of them, and the row that once recorded its equivalence.
- **the four pump and collect terms** are the opposite case and are pinned:
  `S_WHPUMP` and `S_WDPUMP` by T22a and T22b, `S_RHCOLL` and `S_RPPUMP` by
  T22c.
- **both retirement clears.** Clearing `done_seen_r` in `S_FIN` retires
  ownership at the terminal boundary, and the accept path in `S_IDLE` clears it
  again before the next op can read it. Removing either leaves the suite green,
  under every device model here: no observer can separate a flag cleared twice
  from a flag cleared once. Removing BOTH used to leave it green as well, and
  since refusal (d) it does not, measured: a flag left over from one operation
  is read by the next one's first data phase as a short command, and T23e is
  the first check to fail. They are kept because the invariant they state,
  that the flag is never set outside the window, is what the ticket asks for.
- **the grant-carried completion is a legal end for an ERASE only.** T21
  measures it on the ERASE a backend without erase semantics answers at once,
  which is the case the port's header names. For a READ or a WRITE a
  completion on its own grant ends a command with bytes still to move, a
  zero-byte short command, which refusal (d) answers with one err, cause
  DEVICE, as soon as the data phase reads the latched flag (T27b, c and f).
  Before refusal (d), `S_RHCOLL`'s defence tested the live `dev_done_i` only,
  so the READ case wedged the port waiting for bytes that never came: issue
  #15's wedge, reached by a device that did answer.

Mutation-proven (backup → sed → run → restore → green). **These figures are
against the 393-check suite; earlier revisions of this file carried figures of
an older suite long after it grew.**

**How a lost terminal reads since the deadline.** Before issue #15 a mutation
that swallowed a device error or lost a completion WEDGED the port: `run_op`
gave up after 100,000 cycles and every phase after it died with it. Now the
deadline answers that operation, as DEADLINE where the device had said DEVICE
or done, and the port then holds the command it abandoned as OWED, although the
device already ended it, so the operations after it end DEADLINE too until a
device terminal or a reset clears the owed state (T19a's unsolicited `done` is
the first such terminal in the run; T23, T24, T25 and T27 each start from a
reset). The cause checks (T7-T18 each name DEVICE now) and RW4/RW5 are what
fail first; the rest of each count is that containment.

- **C1** (2026-09-23) the cause register deleted (`nvm_err_cause_o` tied 0):
  **fails 86 of 393**, every check that reads a cause: T7-T9, T15-T18, T23,
  T26a-b, the DEADLINE checks of T24, T28, T29 and T30, and T27.
- **C2** the cause collapsed to DEVICE (the refusal records DEVICE): **fails 7 of 393**, the UNFRAMED cases of T23 and T26, and T25c, whose refused branch must read UNFRAMED.
- **C3** the cause collapsed to UNFRAMED (every active state records UNFRAMED): **fails 23 of 393**, every DEVICE case: T7-T9, T15-T18, T23a-f, T27, RW5 and RW6.
- **C4** the cause published without its err gate: **fails 1 of 393**, T23k.
- **M1** commit skips the ERASE: the transition INTO `S_WEREQ` (`:376`)
  rewritten to `S_WWREQ`, so no ERASE is ever issued. **Fails 71 of 393**
  (op-log shape, erase pulse/visibility, erase-error path, and the T24, T25,
  T28 and T30 arms that name the ERASE).
  The description used to read "`S_WEREQ` target rewritten", which is ambiguous
  and the two readings differ enormously: rewriting what `S_WEREQ` itself
  transitions to (`:390`), so the ERASE is REQUESTED but never awaited, **fails
  36 of 393**. That sibling was a real coverage gap at the older suite's size,
  recorded as one rather than hidden by the ambiguity; the unsolicited,
  coincident, deadline and reset phases have since closed it, because a WRITE
  requested into a device still erasing is a request while owed.
- **M2** magic gate dropped from `hdr_ok_w`: **fails 12 of 393** (both bad-magic
  refusals, the nothing-forwarded check, T23's UNFRAMED cases and T26's low
  byte). This drops BOTH magic bytes; the low byte alone is M2-lo, below.
- **M3** payload pump off-by-one (`bcnt_r == plen_r` for `plen_r - 1`, both
  directions): **fails 146 of 393** (every data-phase op mismatches or ends at
  the deadline).
- **M4** (2026-08-20) the write phase swallows the device error (`S_WDPUMP`'s
  `if (dev_err_i)` forced false): **fails 28 of 393**. Before the deadline a
  torn commit then never answered at all; now it answers DEADLINE where the
  device said DEVICE, which T15's cause check names first. The port then holds
  the WRITE as owed, so T15-T18 end DEADLINE with no command issued, until
  T19a's unsolicited `done` ends the owed state; RW4 and RW5 close the count.
  Among the survivors, by running it: `T15 torn commit reports err, never done`
  and its busy pair (the deadline does pulse err, with busy low), the cut checks
  that precede the swallowed error, and T16's two isolation checks, which pass
  VACUOUSLY because a contained port issues no further device traffic and so
  nothing else can move. That vacuity is what the T16 guard exists to answer,
  and it fails here: `the torn commit really did erase its own region`.
- **M5** (2026-08-20) the completion window swallows the device error
  (`S_WWAIT`'s `if (dev_err_i)` forced false): **fails 16 of 393**. The port
  waits for a `done` a failed device will never send; before the deadline the
  commit never answered at all (`run_op` gave up after 100,000 cycles), and
  `busy_seen && busy_ok` did not catch that, as the comment at that check says.
  Now the deadline answers it: T17's cause check fails first (DEADLINE where
  the device said DEVICE), then T17's next commit and T18, which the owed WRITE
  answers DEADLINE, and RW4 and RW5.
- **M6** (2026-09-07) the completion latch armed in every state: the ownership
  gate removed from the set, putting the flag back the way issue #14 found it.
  **fails 58 of 393**: every arm of the unsolicited-completion table above, and
  from there a port running one command ahead of its backend, which requests
  into a device still busy until its deadline answers DEADLINE (RW3, RW4), so
  T21, T22, T24, T28 and T30e fall as collateral. Before those phases the same mutation was
  green. The same edit plus the withdrawn one-line clear in `S_WHDR`, the fix
  the ticket proposed first and then retracted, **fails 55 of 393**: it closes
  the header-collection arm alone, `S_WEREQ` stays exposed on the commit side,
  and a restore never enters `S_WHDR` at all. It is measured rather than argued
  because "that would not have been enough" is exactly the shape of claim this
  file has had to retract before.
- **Probes** (mutations of the TEST, not the RTL). Arming T16's tear as
  `arm_err(1, -1)`, so the WRITE fails before its first byte moves, fails 1 of
  393. Replacing the torn commit with a bare `rc = 1` and no device traffic at
  all — the port the T16 prose names as the threat — fails 4 of 393, the cause
  check among them. Under the
  previous guard that second probe failed only ONE check, the erase count,
  while the payload guard passed on residue from an earlier phase.
- **Model probe**: changing only the DEVICE MODEL to roll the last 4 bytes back
  to 0xFF on a completion-window failure — the half-programmed page a real NOR
  may leave — must not redden a check about the PORT. The T17 restore check
  did exactly that, and so did the T16 byte comparison. Neither does now, by
  two different routes: T16's moved onto the bus, T17's moved to assert after
  a `done` where the array is known. The half-page model is 393 PASS, 0 FAIL.
  See the matrix below for every pre-fix form against every model.

### The deadline (issue #15)

The port's banner states the device face's one timing obligation: the device
presents each event it OWES the port within `MEM_TIMEOUT_CYC_P` owed clocks of
the previous one. It owes one in every cycle the port waits on it (a grant
while a request is up, a write byte the port presents, a read byte the port is
ready for, the terminal of a command whose data phase is over, unless that
terminal is already latched) and nothing while the port or the manager holds
the operation. Every grant, byte and terminal restarts the count; a cycle that
owes nothing PAUSES it, so no handshake pattern of the manager's holds a silent
device off; the (`MEM_TIMEOUT_CYC_P` + 1)-th owed cycle without its event ends
the operation with one err, cause DEADLINE. A deadline
ends the operation, never the device's command: a command the device accepted
and has not ended stays OWED, the port requests nothing over it, drains the
bytes an owed READ still owes and no more, and takes the device's next done or
err as its end. T24, T28 and T29 grade all of it at `TMO` = 100, and again at
37 and at 20, every check on the bus, and T30 pins the two holds at the edge of
the bound:

- **the twelve owed states.** In each of the four requests, four waits and
  four data phases the device withholds the event it owes for exactly `TMO`
  cycles, which the port must tolerate (done, byte-exact), and for `TMO` + 1,
  which it must refuse: one err, cause DEADLINE, never done, busy low at the
  pulse, exactly `TMO` + 2 cycles after the last handshake on either face, and
  no device request while the backend owed one. The device then takes up where
  it stopped, one cycle too late, which is the owed command: a request the
  backend took on the very edge the deadline withdrew it is granted a cycle
  late and owed; an owed READ's late bytes are drained and none forwarded; a
  late terminal ends the owed state at once. After each arm the next commit and
  restore are served byte-exact, after a reset where the abandoned command is a
  WRITE (below).
- **the late grant, both ways.** A late grant that carries its own terminal,
  a done (T21's rule) or an err, leaves nothing owed, and the next commit's
  ERASE is requested at once; a request withdrawn before the backend decided is
  never granted, and nothing is owed.
- **a slow device is not a silent one.** A backend that answers every owed
  event `TMO` cycles late commits and restores byte-exact over more than forty
  deadlines' worth of cycles, and a manager stalling three deadlines on every
  byte, in both directions, is never charged to the device.
- **issue #15's criterion 2, both branches.** Busy is low at the err pulse and
  the next request is accepted. It is SERVED once the device has ended the
  abandoned command: a payload READ resumed three fifths of a deadline after
  the verdict, the
  next restore waiting for its end, issuing nothing over it, then served
  byte-exact from scratch. It is answered with one err DEADLINE, no command and
  nothing forwarded while the device stays silent: an ERASE whose done comes
  three deadlines late, after which the next request is served. An owed READ
  is still drained after a later deadline, and dones for no command during an
  ungranted request do not hold the deadline off.
- **the abandoned WRITE is contained** (the ruling on #15). A WRITE whose device
  stops taking bytes, its busy reading idle throughout, ends DEADLINE; two
  commits and two restores after it each end DEADLINE with no command and no
  byte; the device's own err ends it and the port serves again. A WRITE
  abandoned inside its header is contained the same way until a reset of port
  and device.
- **the owed command's end, seen from a request waiting on it** (T28). T24
  ends owed commands on an idle port; here the next operation already waits
  in its request state when the device ends the abandoned command, and that
  end is the abandoned command's, credited to no operation. (a) An owed
  payload READ ended by the device's err while a restore waits in `S_RHREQ`,
  and (b) an owed ERASE ended by err while a commit waits in `S_WEREQ`: each
  waiting request is then served byte-exact, never handed the err. (c) A WRITE
  abandoned in its completion window stays owed: the next commit requests
  nothing over it and ends DEADLINE, and is served once the device's done
  ends the WRITE. The owed command's own events restart the count of the
  request waiting on it: (d) an owed READ drained one byte every `TMO` / 2
  cycles holds a restore for more than ten deadlines, which is then served,
  never DEADLINE; (e) an owed ERASE's done three fifths of a deadline into a
  restore's wait, then a grant three fifths of a deadline after that done:
  served. Each one is the check a defect planted in review got past the
  suite without; D18-D22 below are those defects. (f) The drain takes the
  bytes the owed READ still owes and no more: a payload READ abandoned before
  its 11th byte, whose device then presents its bytes one every `TMO` / 2
  cycles past its length for ever, a broken backend, is drained of exactly
  the 30 it still owed, and the restore waiting on it ends DEADLINE `TMO` + 2
  cycles after the last of them, as against a silent device; the device's
  own done then ends the READ and the next restore is served. Taken as
  progress, those bytes held the restore off for ever (D27). (g)-(k) pin the
  bound's other branches the same way, each against a device presenting
  bytes past what is owed, one every `TMO` / 2 cycles, and each arm from a
  reset of its own: (g) a header READ abandoned in its collection after 3
  and after 7 of its 8 bytes is drained of the 5 and the 1 it still owes;
  (h) a READ abandoned in its wait state, the header's and the payload's,
  every byte moved, of none; (i) a READ the backend granted on the edge its
  deadline withdrew the request, of its whole length, the header's 8 and the
  payload's 40; (k) an owed WRITE granted that way, whose device presents
  read bytes for it, of none, since a WRITE owes no read byte. In each the
  restore waiting on it ends DEADLINE `TMO` + 2 cycles after the last byte
  owed (or its own accept), no command taken, and the device's own
  terminal then ends the command and the next operation is served. (j) The
  count is as wide as a READ's length: a 600-byte payload READ abandoned
  before its 11th byte, owing 590, and a payload READ of the largest legal
  length, `MAX_PAYLOAD_P` = 1,024, owing all of it, abandoned before its
  first byte and granted late, are each ended by the device at a legal pace
  while a restore waits on it, and that restore is served byte-exact, the
  device having delivered what the READ still owed and then the whole
  record. The device presents one byte every `TMO` / 2 cycles until the
  restore is issued, so the late-granted READ still owes almost all of its
  length then at every bound. Counted in 8 bits, the drain stops where the
  count wraps, the device can never move its next byte, and every request
  ends DEADLINE until reset (Z10b and B10 below); the randomized harness
  takes the same property to the parameter's largest legal value, 65,527.
- **a manager's strobe never holds a silent device off** (T29). The count
  pauses on a cycle that owes nothing and restarts only at the device's own
  events. A manager that drops `rready` one cycle in every `TMO` / 2 while
  the device never presents the payload READ's 11th byte, and one that drops
  `wvalid` likewise while the device never takes the WRITE's 21st: each ends
  in one err, cause DEADLINE, exactly `TMO` + 2 cycles after the device's last
  byte plus the cycles the manager held, which the check counts and requires
  to be more than none. The round-1 count, cleared on every such cycle,
  answered neither operation at all (D24).
- **a paused or latched cycle is never charged** (T30). A wait state whose
  terminal is already latched owes nothing: (a) an ERASE answered on its own
  grant, the backend without erase semantics the banner names, then the WRITE
  granted `TMO` cycles after its request, and (b) a header READ answered on
  its eighth byte, then the payload READ granted `TMO` cycles after its
  request, are each served byte-exact. Charged, the one latched cycle is
  carried by the pause into the next request, which is then refused a cycle
  early. Those are the two wait states that can carry it: `S_WWAIT` and
  `S_RPWAIT` leave for `S_FIN` and `S_IDLE`, which zeroes the count, so the
  term's member in each is equivalent (Q3 and Q4 below). A cycle that owes
  nothing is never a verdict, not even with the count at its bound: (c) the
  payload READ's 11th byte, presented `TMO` cycles late on the one cycle the
  manager drops `rready`, and (d) the WRITE's 21st byte, wanted `TMO` cycles
  late on the one cycle the manager drops `wvalid`, are each taken the next
  cycle and served byte-exact; (e) the payload byte one owed cycle later ends
  DEADLINE, `TMO` + 3 cycles after the last byte, the held one included. Each
  arm starts from a reset of its own, and no silence is armed in (a) to (d),
  so RW4 grades them as well. Round 2's suite passed both defects the reviews
  planted here (Q1 and Q9 below).

Mutations, each a row of the figures gate:

- **D1** the verdict forced false, no deadline at all: **fails 91 of 393**,
  every refused arm and branch of T24, T28, T29 and T30, with RW1 recording
  the wedge that returns and RW3 a request over a command still owed.
- **D2** the verdict one cycle early: **fails 54 of 393**, every tolerated arm,
  the slow device, the timing of T28f-k and T29, every arm of T30 and RW4. **D3**
  one cycle late: **fails 54 of 393**, every refused arm's timing and the late
  grants it no longer meets.
- **D4** progress no longer restarts the count: **fails 57 of 393**; a device
  that is slow but moving is refused, from T4 on.
- **D5** a stalled manager charged to the device in the commit's payload pump:
  **fails 5 of 393**, and **D6** in the restore's: **fails 5 of 393**; T24's
  manager-stall arms, the T29 and T30 arms in that pump (their held cycles
  counted, so the verdict comes early) and RW4.
- **D7** a done for no command counted as progress: **fails 9 of 393**; the
  strays during an ungranted request hold the deadline off for ever, and RW1
  records it.
- **D8** the verdict named DEVICE: **fails 56 of 393**, every DEADLINE cause
  check.
- **D9** a deadline leaves nothing owed (timeout to idle): **fails 42 of 393**;
  the next request is issued over an abandoned command (RW3).
- **D10** the late registered grant ignored: **fails 28 of 393**, the next
  request issued into the command the backend took late, and T28i and T28j,
  whose late-granted READs are then drained by nothing; **D11** a late grant
  whose terminal rode it made owed anyway, a done or an err: **fails 4 of
  393**, two checks for each; **D31** the err alone (review probe X20):
  **fails 2 of 393**, the T24 arm of the late grant that carries an err: the
  next commit then waits on a command the device has already ended, and ends
  DEADLINE.
- **D12** the owed state released at the next deadline, by time alone:
  **fails 4 of 393**; **D13** released when the device's busy reads idle:
  **fails 3 of 393**; **D14** a request issued over an owed command:
  **fails 8 of 393**.
- **D15** the owed READ not drained: **fails 36 of 393**; **D16** a later
  deadline overwriting the owed command's kind, which stops the drain:
  **fails 4 of 393**, T24's three and T28k, whose owed WRITE the waiting
  restore's deadline relabels a READ, so read bytes are then taken for it.
- **D17** the owed command's terminal latched as a completion of the next
  operation: **fails 14 of 393**.
- **D18** `S_RHREQ` no longer blocked by the owed command, so the owed
  command's err is taken as the waiting restore's (review probes W15, X24):
  **fails 1 of 393**, T28a. **D19** the same in `S_WEREQ`, the waiting
  commit's (W15b): **fails 1 of 393**, T28b.
- **D20** a byte drained from an owed READ is not progress (W16, X18):
  **fails 8 of 393**, T28d, where the restore behind the slow drain ends
  DEADLINE while the device is moving, T28j, whose long drains it ends the
  same way, and T28f, T28g and T28i, whose restores end so before the drain
  has taken what the READ owed.
- **D21** the owed command's done is not progress (X17): **fails 1 of 393**,
  T28e.
- **D22** a deadline in the WRITE's completion window leaves nothing owed
  (X12): **fails 2 of 393**, T28c and RW3; the next commit's ERASE is
  requested into a device still programming.
- **D23** the same guard dropped from `S_WWREQ` and `S_RPREQ` together:
  **fails 0 of 393**, and cannot fail. A command becomes owed only as an
  operation ends, at its deadline or in the `S_FIN` cycle after it, so the
  next operation meets it in its first request state, `S_WEREQ` or `S_RHREQ`,
  which waits until the device ends it; nothing sets it again before the
  operation ends. The two later guards are kept as the banner's rule written
  where the request is, not as coverage.
- **D24** the count cleared, not paused, on a cycle that owes nothing (the
  round-1 watchdog): **fails 4 of 393**, T29a and T29b, whose operations a
  manager dropping its strobe keeps unanswered for ever, RW1, and T30e, whose
  late byte the cleared count lets in.
- **D25** the count not zeroed between operations: **fails 10 of 393**; the
  count a deadline left at its bound ends the next request on its first owed
  cycle.
- **D26** a wait state that consumes a latched done still owing it: **fails
  3 of 393**, T30a and T30b with RW4. Round 2's suite measured it at none
  under the pristine model, where the one latched done a wait state consumed
  before a request, T21's ERASE answered on its grant, was followed by a WRITE
  grant far inside the deadline; T30 puts both grants at the bound. D26 under
  the coincident model **fails 11 of 393**: those three, T28i and T28j, and
  T24's `S_RPREQ` arm, where the header READ's done rides its eighth byte, `S_RHWAIT`
  consumes it, and without the term that cycle counts against the payload
  READ's request, whose grant `TMO` cycles late is then refused. Paused rather
  than cleared, a stale owed cycle is carried across the header's forward;
  this term is why the pause leaves the coincident model green.
- **D27** the drain unbounded, as round 2 had it: every byte an owed READ
  presents is taken and is progress, past its length too: **fails 15 of
  393**, T28f-i and RW1: the restore behind a device presenting bytes past
  the length is never answered. What the READ still owes taken as its whole
  length, not less the bytes that moved before the deadline, one branch at a
  time (round 3's D28 edited both at once, and its payload half alone failed
  it): **D28a** in the header's collection **fails 2 of 393**, T28g at both
  of its bytes; **D28b** in the payload's pump **fails 1 of 393**, T28f.
  **D29** a drained byte not counted against what is owed: **fails 11 of
  393**, T28f, T28g, T28i and RW1. **D30** what is owed taken again at a
  later deadline, which stops the drain of a READ still owing bytes: **fails
  9 of 393**, T24's owed READ outliving a later deadline, and T28f-i.

The round-2 reviews planted their own defects in the pause logic, and two of
them passed every check of round 2's suite: Q1, the latched-terminal term
dropped from `S_WEWAIT`, and Q9, a verdict on a cycle that owes nothing. T30
is what fails them now. Each plant is a row of the figures gate with the
reviewer's own edit text, R436-2's Q1-Q10 and R437-2's Y1-Y16, so a pair that
is one defect spelled twice measures the same:

| plant | the defect | fails | named by |
|---|---|---|---|
| Q1 | `S_WEWAIT`'s latched cycle still owes | fails 2 of 393 | T30a, RW4 |
| Q2 | `S_RHWAIT`'s latched cycle still owes | fails 2 of 393 | T30b, RW4 |
| Q3 | `S_WWAIT`'s latched cycle still owes | fails 0 of 393 | equivalent: it goes to `S_FIN`, then `S_IDLE`, which zeroes the count |
| Q4 | `S_RPWAIT`'s latched cycle still owes | fails 0 of 393 | equivalent, as Q3 |
| Q5 | the count runs on cycles that owe nothing | fails 21 of 393 | T24, T28i, T28j, T29a-b, T30a-b, RW4 |
| Q6 | a dropped `rready` clears the count | fails 3 of 393 | T29a, T30e, RW1 |
| Q7 | a dropped `wvalid` clears the count | fails 2 of 393 | T29b, RW1 |
| Q8 | `S_WHDR` and `S_RHFWD` clear the count | fails 0 of 393 | equivalent: the count is zero in both, entered from `S_IDLE` or from a terminal |
| Q9 | a verdict on any busy cycle, one that owes nothing included | fails 4 of 393 | T30c-e, RW4 |
| Q10 | the count zeroed in `S_FIN`, not `S_IDLE` | fails 0 of 393 | equivalent: `S_FIN` always goes to `S_IDLE`, which owes nothing |
| Y1 | Q1, R437-2's spelling | fails 2 of 393 | T30a, RW4 |
| Y2 | Q2, R437-2's spelling | fails 2 of 393 | T30b, RW4 |
| Y3 | Q3, R437-2's spelling | fails 0 of 393 | equivalent, as Q3 |
| Y4 | Q4, R437-2's spelling | fails 0 of 393 | equivalent, as Q3 |
| Y5 | the count runs on every cycle but `S_FIN` | fails 21 of 393 | T24, T28i, T28j, T29a-b, T30a-b, RW4 |
| Y6 | `S_RHFWD` owes | fails 11 of 393 | T24, T28i, T28j, T30b, RW4 |
| Y7 | `S_WHDR` owes | fails 10 of 393 | T24, RW4 |
| Y8 | a request waiting on an owed command owes nothing | fails 26 of 393 | T24, T28c, T28f-i, T28k, RW1 |
| Y9 | the WRITE pump owes only once the device is ready | fails 9 of 393 | T24, T29b, RW1 |
| Y10 | the READ pump owes only once a byte is presented | fails 19 of 393 | T24, T28a, T28d, T28f, T28j, T29a, T30e, RW1 |
| Y11 | a verdict on a paused pump cycle | fails 4 of 393 | T30c-e, RW4 |
| Y12 | a manager handshake counted as progress | fails 0 of 393 | equivalent: in a pump it is the device's own byte, and in `S_WHDR` and `S_RHFWD` the count is already zero |
| Y13 | Q6, R437-2's spelling | fails 3 of 393 | T29a, T30e, RW1 |
| Y14 | Q7, R437-2's spelling | fails 2 of 393 | T29b, RW1 |
| Y15 | the count one bit short | fails 91 of 393 | T24, T28, T29, T30e, RW1, RW3 |
| Y16 | Q9, R437-2's spelling | fails 4 of 393 | T30c-e, RW4 |

The round-3 reviews planted their own defects in the drain's bound, R436-3's
Z1-Z12 and Z10b and R437-3's B1-B13 (B6b beside B6, no B11), and nine of them
passed every check of round 3's suite and randomized harness: Z1, Z3, Z8 and
Z10b, and B6, B6b, B7, B8 and B10, the same defects spelled twice and B7. That
suite pinned the payload pump's branch alone. T28g-k are what fail them now,
one branch each, and the randomized harness fails Z1, Z3, Z8 and Z10b too
(below). B12, which R437-3 measured equivalent against every contract-legal
device, fails T28k, whose device presents read bytes for an owed WRITE. Each
plant is a row of the figures gate with the reviewer's own edit text:

| plant | the defect | fails | named by |
|---|---|---|---|
| Z1 | a header READ abandoned in `S_RHCOLL` owes its whole 8 | fails 2 of 393 | T28g |
| Z2 | a payload READ abandoned in `S_RPPUMP` owes its whole length | fails 1 of 393 | T28f |
| Z3 | a READ abandoned in either READ wait state owes 8 | fails 2 of 393 | T28h |
| Z4 | a READ granted late owes nothing | fails 22 of 393 | T24, T28i, T28j |
| Z5 | the payload pump's branch one over | fails 1 of 393 | T28f |
| Z6 | the payload pump's branch one short | fails 20 of 393 | T24, T28d-f, T28j, T30e |
| Z7 | the header collection's branch one short | fails 19 of 393 | T24, T28g |
| Z8 | the header collection's branch one over | fails 2 of 393 | T28g |
| Z9 | the count decremented on any byte while owed, so it wraps | fails 15 of 393 | T28f-i, RW1 |
| Z10 | the count 8 bits wide | fails 3 of 393 | T28j; `make run` stops first at `elab_bounds.sh`, whose width lint it fails |
| Z11 | `dev_rready_o` raised for the whole owed READ, the count kept | fails 15 of 393 | T28f-i, RW1 |
| Z10b | Z10 spelled lint-clean | fails 3 of 393 | T28j |
| Z12 | every other branch owes its length less one, a wait state 65,535 | fails 27 of 393 | T24, T28h-j, RW1 |
| B1 | Z6, R437-3's spelling | fails 20 of 393 | T24, T28d-f, T28j, T30e |
| B2 | Z7, R437-3's spelling | fails 19 of 393 | T24, T28g |
| B3 | a READ granted late owes one byte fewer | fails 22 of 393 | T24, T28i, T28j |
| B4 | Z4, R437-3's spelling | fails 22 of 393 | T24, T28i, T28j |
| B5 | Z5, R437-3's spelling | fails 1 of 393 | T28f |
| B6 | Z8, R437-3's spelling | fails 2 of 393 | T28g |
| B6b | Z1, R437-3's spelling | fails 2 of 393 | T28g |
| B7 | a READ granted late owes one byte more | fails 2 of 393 | T28i |
| B8 | Z3, spelled `HDR_LEN_C` | fails 2 of 393 | T28h |
| B9 | Z9, R437-3's spelling | fails 15 of 393 | T28f-i, RW1 |
| B10 | Z10, R437-3's spelling | fails 3 of 393 | T28j; `make run` stops first at the width lint |
| B12 | the drain not limited to an owed READ | fails 1 of 393 | T28k |
| B13 | what is owed taken only where the deadline abandons an owned command, never a late grant's | fails 22 of 393 | T24, T28i, T28j |

### Three bounds (issue #15)

Every wait in the harness that meets the deadline is a function of `TMO`, and
every cut or poke inside an operation is named on the bus, so the suite is
evidence at any bound from its smallest, 20, up; the Makefile builds it at
three. Until its waits were derived they were numbers written for 100: built
at 37 the suite failed four checks and at 1,000 three, on the harness, not
the port. Until T6's poke was named on the bus it came a fixed 2 * `TMO` / 5
cycles after the accept, and under the coincident model, where the WRITE ends
on its last byte, the commit was over by then from `TMO` = 500 up: an idle
port took the poke as a restore, and the operations after it wedged behind
it. The poke now comes once the backend has taken the ERASE, inside the
commit at every bound. The coincident model at `TMO` = 4096 **fails 0 of
393**; the same with round 2's T6 **fails 16 of 393**, T6 and T7-T9 after it,
with RW1. The waits, each the same number at 100 as before:

- the silences and late events T24, T28 and T29 arm: `TMO`, `TMO` + 1,
  `TMO` + 2, `TMO` + 3 * `TMO` / 5 (the served branch), 3 * `TMO` (the
  DEADLINE branch, the owed READ, the WRITE's completion window);
- a slow device's every event `TMO` late, a manager's three-deadline stall,
  the drain `TMO` / 2 a byte, the late done and grant 3 * `TMO` / 5 each, the
  strays and the strobe drops 3 * `TMO` / 10 and `TMO` / 2 apart;
- the waits a cut is staged in (T25): 2 * `TMO` / 5, and their midpoints;
- the cycles ticked past a pulse to catch a second, 3 * `TMO` / 10; the
  windows after a reset in which the port must stay silent, `TMO` / 2 and
  3 * `TMO` / 5; and `run_op`'s guard, a thousand deadlines, past the
  longest legal operation here (the manager stalling three deadlines on
  every byte of a record).

The fixed protocol delays left are the ones no deadline meets: grants and
completions of 1 to 9 cycles (T4, T6, T19-T21) and a device's 4-cycle settle.
The harness refuses to build below `TMO` = 20, where those and the derived
fractions stop meaning what their phases say, so 20 is the third build. The
second is 37: small, odd, and the one that failed before. The bounds below 20
are the randomized harness's. The figures gate measures every row at 100, the
coincident model again at 4096, and requires all three builds green, and the
randomized harness at every bound it is built at.

### The randomized harness (issue #15)

`fuzz_main.cpp` is the round-2 review's randomized probe, folded in as a
standing check: its own device model, its own manager and its own count of
owed cycles, written from the port's banner rather than from the RTL, and
independent of `sim_main.cpp`. `make` builds it at bounds 1, 2 and 3, the
port's smallest legal bounds, which the suite cannot reach, and at 37 beside
the suite, each at `MAX_PAYLOAD_P` = 65,527, the parameter's largest legal
value (the Makefile's `FUZZ_MAXP`). The harness does not read that value: it
asks the port, by a binary search over stored headers whose payload_length
the port either takes or refuses UNFRAMED, and prints what it found, so a
build at any other value grades that one. Each build runs three seeds, six
hundred operations per mode and seed, and grades each property as ONE named
check, its message carrying the counts, so its tally does not move with what
the seeds draw:

- **legal** (FZ1-FZ4). The device presents every event it owes within the
  bound, biased to the exact bound: terminals riding final bytes and ERASE
  grants, dones for no command where nothing is owned, now and then an err in
  place of any one event. The manager drops its strobe at random,
  periodically, or for up to five deadlines. Every operation is answered once
  with busy low, never DEADLINE; an err comes only where the device erred, and
  is named DEVICE; every done is byte-exact.
- **silent** (FZ5, FZ6). The device withholds one owed event for ever while
  the manager drops its strobe: one err DEADLINE, never done, on exactly the
  (`TMO` + 1)-th owed cycle since the device's last event.
- **resume** (FZ7, FZ8). A command abandoned by a deadline at any of its
  bytes or terminals (never a grant, and never a WRITE's byte, which is
  contained), which the device ends `r` cycles into the next request's wait
  and then keeps legal pace: served if `r` <= `TMO`, DEADLINE if not, and
  both branches taken. Half its records are long, up to 1,024 bytes, and
  once per seed a READ of the largest payload the port accepts, stored whole
  and abandoned before its first payload byte, owes every byte of it: the
  restore behind it, which the device ends within the bound, is served.
- **babble** (FZ9). A READ abandoned by a deadline at any of its bytes or at
  its terminal, the header's or the payload's, half its records long, whose
  device then presents its bytes one every `TMO` / 2 cycles past its length
  for ever and never ends it, a broken backend: the request waiting on it
  ends one err DEADLINE, and the port took exactly the bytes the READ still
  owed, none where it owed none.
- **reach** (FZ10). The abandonments covered every branch of what a READ
  owes: FZ9's at header bytes, at payload bytes and at terminals, and FZ7's
  served requests behind READs owing 256 bytes or more, and behind the
  largest in every seed.

Q1 under the randomized harness at bound 3 **fails 4 of 10**: FZ2, FZ3, FZ6
and FZ7. Q9 under the randomized harness at bound 3 **fails 5 of 10**: those
four, and FZ10, its restore behind the largest READ refused in two seeds of
three. A false DEADLINE against a legal device fails FZ2 first. D27 under the
randomized harness at bound 3 **fails 1 of 10**, FZ9: no request behind the
babbling READ is ever answered. The round-3 plants that passed the harness
fail it now at every bound it is built at; at bound 3, Z1 under the
randomized harness **fails 1 of 10**, Z3 under the randomized harness **fails
1 of 10** and Z8 under the randomized harness **fails 1 of 10**, each FZ9, and
Z10b under the randomized harness **fails 3 of 10**, FZ7, FZ9 and FZ10: a READ
owing 256 bytes or more is drained short and its device then stalls. The
plants in the late grant's branch (Z4, B3, B4, B7 and B13) and B12 pass it,
because its device grants a request only while it is up and presents no read
byte for a WRITE; the suite fails each of them.

### Short commands (refusal (d))

A command the device ends before its final byte moves, by a done on an edge
that moves no final byte or one that rode the grant of a command with bytes
still to move, is a DEVICE error at once in all four data phases. The header
read always refused it; a payload read ended short, or any READ completed on
its own grant, left the port waiting for bytes that would never come. T27: a
payload READ ended after ten of its bytes, a header READ and a payload READ
completed on their own grant, a WRITE ended in its header pump, in its payload
pump and on its own grant. Each: one err, cause DEVICE, never done, the cycle
after a live completion or two after a latched one, nothing left owed, and the
next operation served at once. The legal coincidence, a completion on the final
byte's own edge, is T22's.

- **S1** the rule off in the header pump: **fails 7 of 393**; **S2** in the
  payload pump: **fails 5 of 393**; **S3** in the payload read: **fails 15 of
  393**; **S4** the header read reading only the live done, not the latched one
  (the rule as it stood before): **fails 13 of 393**. Each fails its own T27 arm
  first; the port then reaches its deadline and holds as owed a command the
  device already ended, which takes the later T27 arms with it.

### A reset mid-commit (issue #18)

T25 asserts `rst_n` mid-commit at six stages named on the bus, never by DUT
state: (a) the header still streaming in, nothing issued; (b) the ERASE
requested and not granted; (c) the ERASE granted and not done; (d) the
WRITE's header pump, five bytes sent; (e) its payload pump, nine bytes of the
record sent, which is the reviewer's case on #18, a 32-byte record cut so that
23 bytes stay erased; (f) its completion window, every byte sent. The device
is reset with the port (both lose power). After the release the port is idle
and silent, no pulse, no request, no byte; a neighbour restores byte-exact;
the next commit of the same record is byte-exact; and the torn record never
restores as a valid one: either the port refuses it at the header (cause
UNFRAMED) or forwards it whole and the suite's own crc16 rejects it, unless
the old record survived, or, at (f), the new one did, and the branch the port
took agrees with the stored header. Where nothing reached the device, (a) and
(b), the old record restores byte-exact. Two more cuts reset the port alone,
an SoC reset its backend does not share, in the ERASE's and the WRITE's
completion windows: the device's late completion reaches a port that owns
nothing and is discarded (refusal (c)), and the port serves once the device
has ended its command.

**Who refuses the torn image that restores as well-formed: the manager, by the
crc16.** The port gates only magic and payload_length, so that it can delimit
the stream; it forwards a torn record whose header survived WHOLE, by design,
because integrity is in-band and validated by the manager (07 §5.3), never by
this port (its banner). Under the pristine model (e) is exactly that case: the
header and one payload byte survive, the port forwards all 32 bytes, and the
crc16 rejects them. In the processor that crc16 is `KL_acmp_nvm_shadow`'s
`rrec_ok_w` and `KL_aecp_nvm_writer`'s `frame_ok_w` (cited above), which apply
that record's vendor default (F07.9); `tb/acmp_nvm` grades a reset in the
middle of the binding manager's flush the same way (its group R).

### The handshake models (issue #21)

Four standing variants vary the HANDSHAKE, beside the four that vary what the
array retains, each one arming of the harness's own backend that
`measure_figures.py` leaves on for the whole run. Each is justified against
the port's banner, and the line the rest of this file draws is kept: a model
of a contract FREEDOM must leave every check green, and a model of a BROKEN
backend is graded on what the port owes it, not on service.

| model | what the device does | the port's clause | class |
|---|---|---|---|
| unsolicited completion | one done per operation for no command, in a window the bus names: a commit's header still streaming in, or a restore's being handed up | refusal (c): a completion seen while the port owns no command is discarded | freedom |
| coincident completion | done on the edge that moves a command's final byte | the latch comment, refusal (c)'s ownership window | freedom |
| short read | every other READ it accepts ends with done after three eighths of its bytes, rounded down (three of a header's eight) | refusal (d) | broken backend |
| silent | every command granted, then nothing more: no byte, no terminal | the deadline, and the owed command | broken backend |

The two freedoms leave all 393 checks green (the model table above). The two
broken backends fail service, the restores a short device cannot complete and
everything a silent one never answers, and are graded on the RW checks, which
close the run and must pass under every model, which the figures gate enforces
by name: **RW1** every operation answered, none past `run_op`'s guard; **RW2**
none answered twice; **RW3** no device command requested while the backend
still owed one; **RW4** no DEADLINE where the device was not silent; **RW5**
every device error answered DEVICE; **RW6** every READ the device ended short
answered with one err, cause DEVICE, never done; **RW7** under the silent
model every operation ended in one err, never done and never DEVICE; RW8 and
RW9 witness that the unsolicited and short-read models fired.

Issue #21's four mutations, each under the model that names its device:

- M6 under the unsolicited model **fails 277 of 393** (M6 alone above).
- the latch deleted, under the coincident model: the `done_seen_r` row above.
- M8 under the short-read model **fails 170 of 393** (M8 alone below).
- D1 under the silent model **fails 335 of 393**, RW1, RW3 and RW7 among them:
  the wedge issue #15 named, back.

### Issue #19's four mechanisms

Each mutation now fails a check whose message names its mechanism:

- **the low magic byte.** T26a commits a record framed 0x17FF, refused with one
  err, cause UNFRAMED, and no device traffic; T26b restores a stored record
  whose byte 1 is wrong, refused with nothing forwarded. **M2-lo**, the low
  byte's compare forced true, **fails 5 of 393**.
- **the payload bound at both edges.** T26c and T26d commit and restore a record
  whose payload_length is exactly `MAX_PAYLOAD_P`, byte-exact; T11 and T13 keep
  the `MAX_PAYLOAD_P + 1` refusals. **M7**, the bound weakened from `<=` to
  `<`, **fails 8 of 393**: T26c and T26d, and T28j, whose READs of the
  largest legal length the weakened bound refuses.
- **the sticky `done_seen_r` latch.** T21 and T22 name it; deleting its set line
  and the coincident-model row are measured above.
- **the short-read defence.** T23c names it, and T27 extends it to every data
  phase. **M8**, the header read's defence off, **fails 22 of 393**: T23c, then
  the port reaches its deadline and holds as owed a read the device ended,
  which takes the rest of T23 and T27's READ arms with it.

### Device-error arm coverage

`KL_pp_nvm_port.sv` has twelve `if (dev_err_i)` arms. Each was forced to
`1'b0` in turn and the suite re-run, so this table is measured, not argued —
and it is now **checked by a script rather than by hand**: `measure_figures.py`
re-runs every arm, cross-checks the arm COUNT against the RTL, and re-measures
**every figure in this file**: one hundred and twenty-six mutations and probes, ten device-model
result rows, and all thirty cells of the pre-fix matrix; and under every device
model it requires the RW checks to pass by name. CI runs it.

The covered set is DERIVED, not asserted. Every `N of M` and `N PASS, N FAIL`
here is a claim by default, satisfied only by a measurement or by an explicit
waiver whose reason and absorbed count the script prints on a clean run. A new
figure is a hard error until it is measured, so it cannot be added silently --
and it cannot be deleted to silence the gate either, because deleting it makes
the measurement that owns it fail instead.

**What that does NOT establish, stated plainly because an earlier version of
this paragraph claimed otherwise.** The default is inverted over the SHAPE of a
figure, and shapes are recognised, not resolved. An earlier version argued the
vocabulary could not grow behind the gate because English number words are a
closed class. The argument is true and beside the point: the closed class is
number words, the open class is ways of writing a ratio, and closing one axis
leaves the other. Ten of thirteen phrasings still evaded, two of them using
digits only -- `fails 22 of the 393 checks` and `fails 22 out of 393`. Both are
caught now, and `| Mx | 22 |`, `reddens 22 checks`, `a fifth of the suite`,
`68/90ths` and `24%` are not. A real closure would mean treating every bare
integer as a claim: measured, 292 numbers in this file fall outside every claim
and waiver, so the waiver list would be larger than what it protects. This is a
strong default that catches every phrasing anyone has written here. It is not a
proof that none can be written, and the difference is the whole subject of this
section.

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

Run `make -C tb/nvm_port figures` after any change to this suite. About seven
minutes on sixteen cores; it prints its own derived build count. That is the price of figures that four
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
| 376 | `S_WEREQ`  | **0 — uncovered** |
| 385 | `S_WEWAIT` | 51 |
| 395 | `S_WWREQ`  | **0 — uncovered** |
| 405 | `S_WHPUMP` | 41 |
| 422 | `S_WDPUMP` | 28 |
| 437 | `S_WWAIT`  | 16 |
| 448 | `S_RHREQ`  | 3 |
| 458 | `S_RHCOLL` | 20 |
| 477 | `S_RHWAIT` | 15 |
| 504 | `S_RPREQ`  | **0 — uncovered** |
| 514 | `S_RPPUMP` | 47 |
| 529 | `S_RPWAIT` | 4 |

**Nine of twelve are covered; three are not.** Since the deadline, a swallowed
error no longer wedges the port: it reaches its deadline, answers DEADLINE, and
then holds the command as owed (see "How a lost terminal reads" above), so each
count is the cause check of the phase that cut there plus that containment. The
survivors are three of the
four `*REQ` arms, where the device asserts an error before its request is
granted. T23a added that mode to the device model (`gnt_err`: err in place of
the grant) for the header READ, which is how `S_RHREQ` came to be reached; the
ERASE, the WRITE and the payload READ have not been armed with it. That
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
  the whole suite **green** before, now fails on `T17 the cut was in the
  completion window: every byte sent first`.
- replacing T18's three device errors with a bad stored magic, so ZERO device
  errors occur anywhere: the whole suite **green** before. The op log alone could not tell
  them apart, because the port issues the header READ BEFORE validating it, so
  a refusal and a device error produce the same two ops. Distinguishing them
  needed a count of DEVICE-raised errors, separate from the port's own `err`
  pulses; `dev_errs` is that counter and each arm now pins it.
- replacing a T18 arm with a bare `r = 1`, port never touched: now fails 4.

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
that check FAILS while the header check PASSES, 2 of 393. It can fail alone, so
it is not a member of this section at all. The error is the same shape as the
corollary retracted below -- a general claim generalised from the states that
happened to be tried -- and this file has now made it twice, because "no
divergence across the cases I ran" is not "cannot diverge".


### Where a check may read from

FIVE checks in this file were written against what the flash ARRAY held and
had to be rewritten, because what the array holds is the device MODEL's choice,
not the port's behaviour. EIGHT device models and one combination are run, RTL
byte-identical. Four vary what the array RETAINS; four vary the HANDSHAKE
instead, and "The handshake models" above has the other three. The first of
those is coincident completion: the device raises `dev_done_i` on
the same edge that moves a pump's final byte, which `KL_pp_nvm_port.sv:350-354`
says the sticky `done_seen_r` latch exists for. It is a contract freedom rather
than a broken peer, and the port handles it. Its whole interest was that
deleting that latch was INVISIBLE without it, which stopped being true when
T21 and T22 landed -- it still catches far more of that deletion than the
pristine model does, and it was the first model here to vary the handshake for
a whole run. It is not a separate
implementation any more either: the behaviour is the harness's own armed
backend (`present_coincident_completion`), which T22 arms for one commit and
one restore, and this model is the same arming left on for the whole run. That
matters for what the model can be trusted to say -- a model spliced in from the
gate is a second implementation of the thing under test, and this one is not. The four array-flavoured models:
the pristine model, which keeps every accepted byte; a half-page model, which drops the
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
the port's behaviour either: `T1 erase visible past the record` asserted after
a `done` and reddened under lazy erase, since whether an ERASE rewrites the
array is the backend's business. It was the one known remaining member, and
issue #21 asks for every original check green under every model the port must
tolerate, so it is no longer an exception: the erased tail is asserted only on
a backend with erase semantics (the harness's `lazy_erase` switch, which both
lazy-erase rows set), and T1's bus checks, the whole-region ERASE requested
once and before the WRITE, carry the port's half under every model.

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
| pristine | **393 PASS, 0 FAIL** |
| half-page | **393 PASS, 0 FAIL** |
| page-buffered NOR | **393 PASS, 0 FAIL** |
| lazy erase | **393 PASS, 0 FAIL** |
| lazy erase + page-buffered | **393 PASS, 0 FAIL** |
| coincident completion | **393 PASS, 0 FAIL** |
| unsolicited completion | **393 PASS, 0 FAIL** |
| short read | 297 PASS, 96 FAIL, service only: every RW check passes |
| silent | 122 PASS, 271 FAIL, service only: every RW check passes |

The page-buffered model discards its buffer when it takes a new command, as a
real NOR's buffer does; before the deadline no WRITE was ever left unfinished
without an err, so that edit had nothing to do.

A device model variant is the cheapest way to find a check that tests the
harness rather than the DUT, and it belongs in the standing mutation set. Each
addition found members the previous ones could not. Rather than say which,
here is the measurement: every pre-fix form re-injected, every model run, RTL
byte-identical throughout. An attribution sentence drifts from the runs; this
cannot, because re-deriving it is re-running it.

Re-running is not enough on its own, though, and the reason is worth stating.
Two people reconstructing these forms from the old commits share whatever
transcription error both make, and all thirty cells agree either way. That is
not hypothetical: one reconstruction merged two T17 checks into one conjunction
and moved the array capture from after the restore to before it, and every cell
still agreed. So each row's predicates are additionally pinned VERBATIM against
the revision they were recovered from -- git is the third party, and it costs no
builds. Every row carries at least one pin, which the gate enforces: the first
version of that pin had five predicates and all five were T17, the one row where
a defect had already been found, leaving the other four rows unpinned.

Two further rules close what those two share. **Every condition line of every
injected CHECK must be pinned**, not merely one line per row: T16's form spans
two lines, and deleting the second left the row nominally compliant while
re-opening the exact escape the pin existed to close. The rule is read off the
artifact rather than set as a threshold, because the forms carry one, three and
three checks and any count would be arbitrary. And **no injected line may appear
more times than in its source revision**, which catches a structural change that
moves no cell: prepend a second restore and the predicates stay verbatim while
every cell stays byte-identical, so neither of the other two mechanisms sees it,
but the duplicate IS the structure. An earlier draft of this paragraph called
that case unclosable. It is not, and the version that was tried first --
diffing the injected span against the git hunk -- fails for a different reason
worth recording: T17's form merges removals from two revisions and T15's from
three, so it false-positives unless given a splice budget of three or more.

**What still gets through**, narrowed to two nameable cases rather than left as
a general excuse: an invented line that happens to exist elsewhere in the
pre-fix file, and an ANCHOR change that moves no cell. The anchor half is the
hard one; moving an anchor after the good commit flips ten cells, so the cell
comparison is a real backstop there, but it is a backstop rather than a pin.
The merged T17 form that started all this was caught only because it also
altered predicate text, which was luck rather than coverage.

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
- **A restore failure is told apart by its cause and its byte count, and the
  telling is the manager's.** This suite pins the port's half: the cause names
  a device that failed or ended short (DEVICE), a record that is not one
  (UNFRAMED) and a device that said nothing (DEADLINE), and a tear after a
  complete header shows as bytes already forwarded. Whether a zero-byte DEVICE
  or DEADLINE `err` is a dying flash rather than a blank region is the binding
  manager's verdict, graded in `tb/acmp_nvm` (N1, N2, N9, N12; issue #20).
- **NONE of the twelve `dev_err_i` arms can fire on any bitstream that exists.**
  The parent answers this port's device face with a blank-flash responder that
  ties `nvm_dev_err_i` to `1'b0` (`milan-fpga hdl/milan/KL_pp_shadow.sv:1010`).
  That is the largest limitation here and it was omitted from this list: the
  whole error-handling half of the port, and every row of the "Device-error arm
  coverage" table, is unreachable until a real backend lands. Note the responder is not inert - it
  drives `nvm_dev_done_i` and answers every command - so what makes issues #14
  and #15 unfirable is that it is WELL BEHAVED, not that error is tied off. The
  port's own `nvm_err_o` is live and fires on every restore on the shipping
  build. **That paragraph describes the parent at the revision it was written
  against, and this repository cannot re-derive it**: the device face is
  answered outside this tree, the responder there has since been reported
  changed, and nothing here re-checks it. Read it as a statement about a parent
  revision, not as a live reachability result either way — the phases below
  prove a contract, not a bitstream. The deadline is the one exception the port
  makes reachable on its own: a backend that stops answering now ends in the
  port's `err` with cause DEADLINE, whatever the backend's error tie-off.
- **The device models are models.** The four that vary the handshake (see
  "The handshake models") are drawn from the port's own contract and from the
  parent's backend as this repository describes it; no real flash part was
  run. The deadline's default (1,000 ms, `NVM_MEM_TMO_CYC_P = CLK_HZ_P` at the
  top) is a derivation from that backend's longest legal stall, not a
  measurement of a device, and this suite runs at `TMO` = 100 and 37 cycles.
- **A contained WRITE is not recovered.** A WRITE the port abandons on a device
  that then waits for its next byte for ever keeps the port answering
  DEADLINE until the device ends it or a reset (T24). That is the ruled
  behaviour: padding the WRITE would close a record whose bytes the manager
  never supplied, and an abort would be an interface change.
- **The RANDOMIZED cut points `09_verification.md:56` asks for are still owed**
  on both sides: T15-T18 and T25 cut at fixed points named on the bus.
- **The `err` clause of the rule has THREE known exceptions**, and T1 was
  never among them: it asserted after a `done` and reddened only under lazy
  erase, a `done`-clause exception, now conditioned on erase semantics. The three are T16's isolation checks, and a coarse-erase
  model where a sector spans regions reddens all three with the RTL untouched.
  The erase-count check does not redden, which is the asymmetry the rule exists
  to describe.
