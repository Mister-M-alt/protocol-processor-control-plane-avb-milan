<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# pp_top — the processor top, end-to-end wire truth

Builds `protocol_processor_top` (every landed module of the tree wired:
validator + replicated RX pools + normalizer + dispatch + ADP engine + ACMP
listener/talker + SRP engine behind `KL_mrp_strip` + TX pool/arbiter + the
ACMP Ethernet-prepend shim + timer/PRNG muxes + scoreboard, event router,
originator, trace ring, side port, NVM shadow + the AECP engine's D3 writer +
manager arbiter + port) under `pp_top_wrap`
and drives it ONLY through the top's external contract: one MAC byte stream
in, one MAC byte stream out, the side-port host face, the SRP service face,
the NVM device face and the descriptor-image memory master. Time is compressed to 1 ms = 100 clk (the 89-slot
deadline sweep still fits a ms tick), so every window measured below is the
REAL timer/PRNG path.

Expectations are independent C++ builders/parsers from the doc byte
offsets — F04.5 ADPDU, F05.13 Milan ACMPDU, 802.1Q §10.8/§35.2.2 MRPDU BNF,
Milan §4.3.3.2 Σ-slope — never DUT logic.

`make`: exit 0 = PASS. It builds the bench three times (sections DV and TB):
each executable prints its own build's tally, and the last line sums the three
into the one canonical tally.

## What it proves

- **D3: the saved-state writer's ownership (parent D3 contract sections 3,
  6.2 and 8.1).** `d3_phases.hpp` runs every case on a fresh model with the
  suite's descriptor image and an erased NVM device, through the top's own
  faces. **D3O1** a READ_DESCRIPTOR fed before `restore_go_i` waits in the
  AECP dispatch queue for 2,000 cycles with the writer owning the state bus
  in every one and the engine running nothing; after the boot the writer's
  terminal follows the listener admission gate's release, its ownership
  falls on that terminal, the engine takes the held command only after it,
  a validated image is proven without a LOCATE, and the command is answered
  byte-exact. **D3O2** an image whose magic the store refuses ends the
  restore CLOSED, cause 7, with ownership kept: a command then waits 30,000
  cycles unserved while the listener stays released. **D3O3** a descriptor
  memory that accepts and never answers ends CLOSED, cause 7, inside two of
  the store's 4,096-cycle watchdogs, never a hang. **D3O4** an image loaded
  after the store's boot walk failed is proven by the writer's LOCATE of
  ENTITY 0 (heal before answer), and the held command is answered from it.
  **D3O5** (the AECP hold admission, processor issue #131 ruling) in CLOSED,
  of `RX_SLOTS_P` + 2 = 6 READ_DESCRIPTORs the first is held and the other
  five are dropped at the slot gate and counted (snapshot word 37, read over
  the side port); a GET_RX_STATE after each is answered in exactly the 168
  cycles it takes with no AECP traffic, none of the six is answered, and the
  listener still answers in that time 2,000 ms later. **D3O6** the same
  during a restore whose next D3 grant waits 15,000 cycles: six AECP
  commands after the listener's release, then a GET_RX_STATE answered in the
  idle latency before the D3 terminal; at the terminal the held command is
  answered byte-exact, the five dropped ones never are, five drops are
  counted, and a command after the terminal is served with no drop. A mutant
  that drops nothing (the unbounded hold) fails all six checks. **D3O7**
  (R391-2 S1, taken) the resident count comes back down: in CLOSED the
  optional external drain (`aecp_txn_ready_i`, then `aecp_rxs_free_i` with
  the head record's slot, which the wrap exposes for this case alone) steals
  the held command, and of the next two AECP commands the first is held and
  only the second is dropped and counted.
  **D3S** the writer in service, on real AECP SETs over the device model.
  **D3S1** every persisted group at its first and last declared index
  (configuration, sampling rate, clock source, both stream-format
  directions, presentation offset) becomes exactly one ERASE and WRITE of
  its own record after the 500 ms debounce, byte-exact against an
  independent F07.8 frame builder (layout 2, CCITT-FALSE crc16), and no
  other record id moves. **D3S2** `d3_unflushed_o` rises the cycle after
  the store's accepting cycle, holds, and falls the cycle after the port's
  done of that WRITE. **D3S3** two changes in one window coalesce into one
  WRITE of the later value. **D3S4** a change while the WRITE is held at
  the device taints it: a second WRITE carries the change. **D3S5** a
  change accepted on the WRITE's done edge wins: the SET's latency to the
  store is measured, the device completion is placed so the two coincide
  (the premise is graded), and the record is rewritten. **D3S6** a done
  clears its own record only, by group and index: the other output offset
  and the clock source (index 0 of another group) changed during 0x50's
  WRITE are both written after it. **D3S7** SET_CONTROL on IDENTIFY raises
  no pending and moves no NVM operation. **D3S8** (DR2b) an identical
  rewrite after convergence moves nothing, and clock source 0 on the unset
  row, a row becoming valid at its reset value, is written. **D3S10** (DR2c)
  a record whose WRITEs all fail is attempted three times in all, each
  retry granted after the backoff and within one relatch of it (14 cycles
  here): the backoff is the top's own derivation, 500 ms of the wrap's
  `CLK_HZ_P`, ceil(1,000,001 / 2) = 500,001 cycles, with no override, so a
  product derivation of 5 ms or a count of 500 bench ticks fails it. While
  it waits the writer owns neither dispatch nor the state bus in any cycle,
  and a READ_DESCRIPTOR sent 1,000 cycles into it is answered byte-exact
  (2,364 cycles later) before the retry. Then the record is dropped with the
  reset-sticky `nvm_alarm_o`; no fourth attempt follows and a later
  successful write does not clear the alarm. **D3S11** a READ_DESCRIPTOR
  whose fetch the memory answers 3,000 cycles late is running when the
  debounce closes: the writer holds dispatch but not the state bus until
  it retires, then latches. **D3S9** grades every cycle of the phase: no
  command is taken while the writer owns and no latch overlaps a running
  program. The negative controls (each trigger deleted, taint ignored,
  clear winning the same edge, clear by group or by index alone,
  IDENTIFY made a change, validity ignored by the change qualifier, a
  latch that ignores the running program, a fourth attempt, an alarm
  forgiven by success, an unproven image continued, the hold released at
  the go, no dispatch hold) each fail their named check; they run from the
  mutation driver `d3_mutants.py` in this directory, which plants each in an
  extract of the tree (mutation record below).
  **D3R** the restore transaction over the device model, on fresh models.
  **D3R1** the nine rows D3S1 saves through real SETs come back across a
  power cycle. Before the save the volatile set is populated: the controller
  registers for notifications (proved by the one a second controller's change
  sends it), locks the entity and sets IDENTIFY to 255. When the D3 walk
  starts (the admission gate's release)
  every row of selectors 0 to 5 reads its reset value with its valid flag
  clear, read through taps rather than the bus the restore owns; the walk
  ends COMPLETE with exactly 9 applied, 0 refused and 18 blank of 27; each
  group's value and valid flag are restored and a real GET reads each
  back (GET_CONFIGURATION, GET_SAMPLING_RATE, GET_CLOCK_SOURCE, both
  GET_STREAM_FORMAT directions, GET_STREAM_INFO's latency through the
  integrator fold); an enable requested from reset advertises nothing
  before the combined terminal and ADP advertises after it; no restore
  write becomes a change (no pending, no device write, for two windows);
  and the volatile set is gone: IDENTIFY reads 0, the lock is free (the
  second controller's SET is accepted) and the registry is empty (that
  change notifies nobody).
  **D3R2** framed records a SET program would refuse keep their defaults
  and the walk goes on: configuration 5 of 2, rate 44100 (off the list),
  clock source 3 of 3, formats the integrator's judge refuses, an offset
  with bit 31 set; records whose frame fails (crc, layout version,
  another record's id, a u64 group carrying four bytes) are refused
  before any rule; the neighbour offset applies; COMPLETE with exactly 1
  applied and 10 refused. **D3R3** configuration 0, the rate list's second
  entry and clock source 0 are accepted. **D3R3b** the rate rule walks the
  AUDIO_UNIT's list as the SET program does, a lane (two entries) at a time
  and at most eight entries: over the suite's image re-packed with a
  ten-rate list, the eighth entry (read from the fourth lane) is restored
  and the ninth, listed but past the bound, is refused. **D3R4** a record read whole in
  pass 0 and erased at rest before pass 1 aborts (cause 5, the passes
  agree record by record) after an earlier record was applied, and the
  roll-back resets both stores: DEFAULTS (`restore_rb_o`), every row at its
  reset value, the image walked again and AECP running; with no descriptor
  debt owed the roll-back strobe still holds both stores in reset two cycles.
  **D3R4b** the other direction: a record erased when pass 0 reads it and
  framed at rest before pass 1 aborts too (cause 5) and is never applied. **D3R5** faults in pass 0 apply
  nothing and end done and failed on defaults with AECP running: a DEVICE
  error on a header (cause 2), a payload torn after two bytes (cause 1),
  and a header the device never answers, abandoned at the deadline to
  the arbiter's drain (cause 3); once the device answers the drained read
  a later SET persists. **D3R5b** the same containment in pass 1: its header
  READ of 0x50 never answered, the deadline aborts (cause 3) and rolls back
  to DEFAULTS, the READ goes to the drain, and once the device ends it a
  later SET persists with nothing left unflushed. **D3R6** an erased device restores blank and not
  failed, an unframed record is its default, and the one saved record
  whose every read ends in the device's error is a failure with cause 2,
  never blank. **D3R7** the rate
  rule's AUDIO_UNIT fetch answers an error beat: the restore aborts
  (cause 6), never a refused value, and rolls back. **D3R8** a READ granted 200 cycles
  inside the per-wait deadline completes; 200 cycles past it aborts (cause 3).
  **D3R8b** the integrator's format judge is a watched wait: stuck in pass 1,
  the per-wait deadline (not the aggregate) rolls the walk back to DEFAULTS,
  cause 3, within twice the deadline of the release.
  The deadline is the top's own derivation, never an override: 20 ms of the
  wrap's `CLK_HZ_P` (1,000,001 Hz) is ceil(20,000.02) = 20,001 clocks. **D3R9** a SET held since before the walk runs after
  the restore applied the saved value, is in force, and the next flush
  saves it. **D3R10** the rate rule's AUDIO_UNIT fetch answers late: at
  4,000 cycles (inside the store's 4,096-cycle watchdog) the restore
  completes; at 5,000 and 16,000 the watchdog's error aborts it (cause 6),
  both stores stay in reset while the guard still owes the abandoned burst
  (its debt survives the stores' reset) and leave it only after the late
  burst, and the re-walked image ends DEFAULTS; at 30,000 the debt outlasts
  the deadline and the restore ends CLOSED. **D3R11** a pass-1 roll-back
  after the binding walk restored sink 0 leaves it bound: the listener's
  GET_RX_STATE answers the restored talker and its record is not
  rewritten. **D3R12** a roll-back whose re-walk cannot prove the image
  (the memory falls silent) ends CLOSED: no done, AECP held, ADP never
  enabled. **D3R13** (DR3a, ratified as an enforced bound) a device that
  grants every request, the binding walk's included, 200 cycles inside the
  per-wait deadline trips no wait's deadline (the longest wait is graded
  under 20,001) and still ends the restore at the aggregate bound, the top's
  `NVM_RS_AGG_CYC_P` = 1,000 ms of the wrap's clock = 1,000,001 clocks from
  the one that took `restore_go_i`: with every record saved the bound falls
  in pass 0, DEFAULTS is registered by exactly that clock, cause 3, nothing
  applied, and the READ in hand is drained; once the device ends it a later
  SET persists. Over an erased device the bound falls in pass 1, which
  rolls back to DEFAULTS within one per-wait deadline of it. Without the
  counter the first walk runs to about 2.3 million clocks. **D3R14** (the
  clarification of DR3a on issue #131: an aggregate expiry never closes a
  provable image) a device slow per byte (each header-probe byte 2,100
  clocks apart, each payload byte 19,001: the reviewers' probe D1) with
  every sink's binding saved makes the binding walk alone outlast the bound
  while no wait trips its own deadline (longest graded under 20,001). At
  the bound's clock the binding walk takes its own per-wait path: it fails
  whole (`restore_cause_o` 3, no sink bound), its READ goes to the drain and
  the listener is released; the D3 walk then proves the image with no wait
  and no record READ and ends DEFAULTS, cause 3, within one per-wait
  deadline of the bound, with the image valid, AECP released (a
  READ_DESCRIPTOR answered byte-exact) and the enable released to ADP;
  once the device ends the drained READ a later SET persists. With the
  image refused (its magic flipped) the same boot ends CLOSED, cause 7.
  **D3R15** (R390-2 F3: the aggregate spans the roll-back) a pass-1 fault
  starts a roll-back shortly before the bound while every wait stays inside
  its deadline (the device's grants are steered so the fault's READ is
  granted on a chosen clock): the bound falls inside the roll-back and ends
  it CLOSED on the bound's own clock with the fault's cause, in its debt
  wait (the rate rule's AUDIO_UNIT fetch 16,000 cycles late: cause 6, the
  burst still owed at the bound) and in its re-LOCATE (a DEVICE error on
  pass 1's header READ of 0x02 100 cycles before the bound: cause 2, the
  store still walking the image at the bound). **D3R16** (R391-2 F1) the
  aggregate is inert after the terminal: a COMPLETE restore, a DEFAULTS
  one rolled back (cause 6) and a CLOSED one (a roll-back whose re-LOCATE
  meets a silent memory, cause 2), each followed by a SET where AECP runs,
  keep their verdicts, ownership and rows (the SET value included) two
  per-wait deadlines past the bound, with no roll-back strobe after the
  terminal. **D3R17** (R391-2 F1) it never fires with an event in hand: the
  device's grants are steered so the writer's arbiter grant of a pass-0
  READ lands on the bound's own clock; the aggregate waits for the next
  clock without an event, ends DEFAULTS (cause 3) two clocks later and
  abandons that READ to the drain, and once the device ends it a later SET
  persists with nothing unflushed. **D3R18** (R390-3 F1) an abort in the
  arbiter's issue cycle: the binding walk's registered READ strobe is out in
  its first `H_RS_STREAM` clock, where no byte can be in hand, and the case
  lands the fifth binding strobe on the first clock `agg_o` reads 1 (every
  binding and record saved, the headers at once, each payload byte 12,350
  clocks apart, the fourth record's done held to a clock placed from
  the done-to-strobe lag measured earlier in the same boot). That clock
  carries the strobe and the walk's abort with the arbiter still unowned;
  the arbiter drains the READ from the next clock, the walk fails whole
  (cause 3), the restore ends DEFAULTS, and once the device ends the
  drained READ the port is idle and a later SET persists. **D3R19** to
  **D3R21** (R390-3 F2, R391-3 F2) grade the aggregate's pre-proof variants,
  every case with every record and every binding saved. D3R19: the image
  absent at reset and loaded before `PP_CTRL[1]` (the product order of
  parent D3 section 8.1), so the D3 walk proves it with its own LOCATE.
  Past the bound, D3R14's device keeps the binding walk reading until the
  bound fails it, and the LOCATE then proves the image: DEFAULTS, cause 3,
  546 clocks after the bound, no D3 record READ. Inside the LOCATE, the
  binding walk completes with its last done placed so the bound falls
  midway through the LOCATE (271 of 542 clocks): the same DEFAULTS, image
  valid, AECP released. D3R20: the binding walk's last done placed so the
  image is proven on exactly the bound's own clock, in `W_IMG` (image valid
  at reset) and in `W_IMGLOC` (loaded late, the LOCATE's answer in hand):
  DEFAULTS on the next clock with no record READ. The done-to-proof lags
  these placements use come from two short boots with a fast device.
  D3R21: D3R14's walk with each payload byte `RS_TMO / 2` clocks apart and
  one byte placed so the binding manager holds it on the bound's own clock,
  the clock the aggregate fires: the walk fails whole at its next waiting
  clock (cause 3 registered two clocks after the bound) and the restore
  ends DEFAULTS. The restore's negative controls (each group's replay deleted,
  a value rule ignored, the passes allowed to disagree, a DEVICE error read
  as blank and an UNFRAMED one read as a device error, a descriptor error
  read as a refusal, no restore watchdog, restore writes counted as changes,
  ADP enabled without the restore, done without the D3 walk, blank ignoring
  the D3 walk, ownership taken only at the walk, no roll-back, either store
  left out of it, the roll-back ignoring the guard's debt, a CLOSED re-walk
  released) each fail their named check, from the same driver.
  `restore_done_o` is the COMBINED terminal: the binding walk's release
  alone (S4) frees the listener, never AECP or ADP.
  The dispatch hold runs from reset, so every section that resets and then
  issues AECP commands starts both walks first (`H::boot_to_aecp`, U10, U11
  and the internal-MAAP model's MP0). Focused reproduction:
  `./obj_dir/Vpp_top_sim --d3-only` after `make gsi-build`.
  `./obj_dir/Vpp_top_sim --dr3a` prints, and never grades, the restore
  durations and longest waits the parent contract's DR3a asks this lane to
  measure (blank and full restores at two memory latencies, the image walk,
  pass-0 and pass-1 faults, a debt-held roll-back, a silent device, CLOSED);
  it records no tally.
- **NW: accepted live name writes (issue #120).** The top's `aecp_name_wr_o`
  is sampled on every accepting clock edge. Independent byte comparisons
  predict one changed lane, all eight changed lanes and an unchanged name;
  byte-exact SET responses and GET readback check the actual stored value.
  A wrapper tap of the name RAM write enable outside boot loading checks
  cycle alignment independently of the exported wire. Pulse widths and
  ordering before the unchanged group-7 NVM mark are checked too.
  Boot loading, reads, idle time, lock refusal, invalid semantic indices,
  missing descriptors/configurations, unnamed descriptors and truncated
  SETs must produce no event. A delayed descriptor fetch must remain quiet
  until its eight writes are accepted. A fetch watchdog abort must remain
  quiet through late-response drain and recovery, with the old name intact.

  Focused reproduction: `make -C tb/pp_top name-writes`. The default full
  suite also runs these checks. Mutation reproduction:
  `python3 tb/pp_top/name_wr_mutant.py --output <log-directory>`.
  Builds use a temporary source copy; only logs enter the output directory.
  Golden and restored runs must pass. The mutant must compile, complete
  simulation and fail each named pulse-count check below; a build failure
  cannot count as detection.

  | Mutation | Required failing checks |
  |---|---|
  | Engine export driven by accepted `SET_NAME` command decode instead of the store's accepted live write | `NW EIGHT`, `NW LOCKED`, `NW ABORT`: accepted lane pulse count |

- **GI: processor-owned GET_STREAM_INFO input fields** (Milan 5.3.8.6/.8,
  5.4.5.2/Table 5.22). `gsi_internal.hpp` uses a fresh eight-sink processor
  with two STREAM_INPUT descriptors (ten after the second reset). The harness
  integrator folds the published binding/started view into STREAMING_WAIT,
  as F06.13 asks of an integrator, so that flag is graded too. Real BIND_RX, ADP discovery,
  PROBE_TX_RESPONSE and MSRP frames drive the listener and SRP state
  machines; no record is forced. Every transition is checked in the complete
  solicited and unsolicited response, including sequence, descriptor index,
  reserved bytes, failure code, full bridge ID and probing/ACMP byte.
  A new bind immediately probes (5.5.3.5.3); without a talker, the two
  unanswered probes and retry window reach PASSIVE (5.5.3.5.29). Discovery
  then drives ACTIVE, another double timeout reports ACMP status 7, retry
  clears that status, and successful probing reaches COMPLETED. Unbind
  reports DISABLED; every observed non-ACTIVE state carries ACMP status 0.
  Two settled sinks register different failure codes and 64-bit bridge IDs.
  Replacing Failed by Advertise and withdrawing Failed clear both fields;
  a changed FailureInformation refresh notifies, while an unchanged refresh
  stays quiet. On the wire, the fresh Failed registration declares Listener
  New (a control for the detector) and the changed refresh declares none:
  the processor's MSRP frames are parsed value by value for 1 s and 600 ms.
  Withdrawal produces the SRP and ensuing probing events, both
  checked. Reset over erased NVM clears live status. A hardware sink missing
  from the descriptor image answers a full zero error body in both response
  paths despite its real ACTIVE record. After the second reset, a re-bind
  from PRB_W_RESP to another talker with STREAMING_WAIT keeps ACTIVE/0 and
  must give exactly one unsolicited response carrying STREAMING_WAIT
  (REBIND-SW), then the new talker's double timeout and the unbind report.
  STREAM_INPUT 9 of the ten-input image, which a three-bit sink index would
  alias onto PASSIVE sink 1, answers zero internal fields (INDEX-GUARD;
  solicited only, because notifications are raised per hardware sink).
  External input selectors 5 and 7
  are stalled if requested, and a request counter must remain zero.
  Existing section G still checks the unchanged STREAM_OUTPUT gather.

  Latency-only Talker JoinIn refreshes (issue #113) are checked with sink 0
  registering Advertise and sink 1 Failed. The harness folds the processor's
  published per-sink accumulated latency into selector 3 with zero ingress
  delay. Every changed refresh must emit exactly one byte-exact unsolicited
  response carrying the new value; a solicited read must agree. An unchanged
  refresh emits none, and neither the other sink nor any other descriptor may
  be notified. The original distinct-value, zero and all-ones cases remain.
  A walking one on the Advertise sink then a walking zero on the Failed sink
  each cover all 32 bit positions (bit 0 is the least significant bit). Between
  walking values the latch returns to zero or all ones, respectively, so each
  of the 128 changed refreshes differs from its previous committed value in
  exactly one bit. Every changed value, including each return, is repeated
  unchanged. All 256 refreshes use the same response, solicited-read and
  other-sink checks. Before each bit, discovery and an unchanged, response-graded
  refresh of the other peer keep both streams live through the long sweep.
  These 64 maintenance refreshes must also stay silent. The added checks have
  a separate subtotal in the run log;
  the original GI and suite checks are retained. The processor's external ports are
  unchanged; the test wrapper exposes an existing output for the gather model.

  Focused reproduction: `make -C tb/pp_top gsi-internal`. The normal suite
  includes GI in its default build. The older registry/configuration tests
  retain a bound sink waiting passively for an absent peer, so independent
  probe-timeout notifications cannot masquerade as their command responses.

  Retained negative controls:
  `python3 tb/pp_top/gsi_mutants.py --output <log-directory>`.
  The runner builds in a temporary source copy, requires a clean golden run,
  and accepts only a completed simulation failing its named check. Compile
  failures do not count. It restores the sources and requires another clean
  run at the end.

  | Mutation | Required failing check |
  |---|---|
  | Latency trigger disconnected from `stri_events` | `GI LATENCY-CHANGE: exactly one unsolicited response` |
  | Latency comparison truncated to [7:0] | `GI LATENCY-WALK-ONE bit 8 step: exactly one unsolicited response` |
  | Latency comparison truncated to [15:0] | `GI LATENCY-WALK-ONE bit 16 step: exactly one unsolicited response` |
  | Latency comparison truncated to [31:16] | `GI LATENCY-WALK-ONE bit 0 step: exactly one unsolicited response` |
  | Latency comparison truncated to [30:0] | `GI LATENCY-WALK-ONE bit 31 step: exactly one unsolicited response` |
  | Latency comparison truncated to [31:30] | `GI LATENCY-WALK-ONE bit 0 step: exactly one unsolicited response` |
  | Latency bit 31 dropped by masking both operands | `GI LATENCY-WALK-ONE bit 31 step: exactly one unsolicited response` |
  | Failure code tied to zero | `GI FAILED-0 solicited: failure code` |
  | Failure bridge tied to zero | `GI FAILED-0 solicited: full failure bridge` |
  | pbsta tied to zero | `GI PASSIVE solicited: pbsta` |
  | acmpsta tied to zero | `GI TIMEOUT solicited: acmpsta` |
  | Adjacent sink selected for the internal read | `GI DISTINCT-0 solicited: full failure bridge` |
  | Original integrator path restored | `GI internal seam: selectors 5/7 never requested` |
  | Missing-descriptor guard removed | `GI MISSING solicited: pbsta` |
  | Committed-status notification removed | `GI PASSIVE unsolicited: complete Milan response` |
  | Re-bind started/stopped trigger removed (bind walks excluded again) | `GI REBIND-SW: exactly one unsolicited response` |
  | SRP FailureInformation-change strobe removed | `GI FAILED-REFRESH unsolicited: complete Milan response` |
  | FailureInformation change re-declares the Listener again | `GI FAILED-REFRESH wire: a changed FailureInformation sends no` |
  | Index guard removed before narrowing | `GI INDEX-GUARD solicited: pbsta sink 9` |
  | Bridge no longer gated on FAILED after the index mux | `GI ADVERTISE unsolicited: full failure bridge` |

- **A** **READ_DESCRIPTOR end to end** (06 §6.1, 07 §3.3) — the seam this
  suite used to stop at. A real AEM command on the MAC byte stream comes back
  as a byte-exact AECPDU carrying a descriptor that lives in MAIN MEMORY,
  fetched over the top's read-only memory master from a latency-injecting
  DRAM model (31-clock first-word latency, never zero):
  - **A1** ENTITY descriptor, whole 354-byte frame byte-exact against an
    independent IEEE §7.2.1 builder; command/response counters move once; and
    **exactly ONE memory burst per command** — the line buffer's whole purpose
    is that a descriptor costs one memory latency, not one per byte.
  - **A2** a bad `descriptor_index` and an unknown `descriptor_type` answer
    `NO_SUCH_DESCRIPTOR` with the 4-byte {type, index} stub of IEEE §7.4.5.
  - **A3** a bad `configuration_index` answers `BAD_ARGUMENTS` (06 §6.1), not
    `NO_SUCH_DESCRIPTOR`.
  - **A4** an 82-byte CLOCK_DOMAIN - a length that is NOT a multiple of 8, so
    `COPY_BUFFER` has to stop mid-lane; a whole-lane advance would put 6 bytes
    of the next descriptor on the wire and lie about `control_data_length`.
  - **A5/A6/A7** an unimplemented opcode answers `NOT_IMPLEMENTED` with the
    command ECHOED (F06.14 / IEEE §9.3.5.3.3) — never silence, never a
    malformed frame; IDENTIFY_NOTIFICATION as a COMMAND answers
    `BAD_ARGUMENTS` (IEEE §7.4.39.2 beats §9.3.5.3.3); a truncated
    READ_DESCRIPTOR answers `BAD_ARGUMENTS` rather than locating whatever
    followed the header.
  - **A5b** the NOT_IMPLEMENTED response is sized by ITS OWN command, swept
    over payloads of 0, 4, 8, 16 and 72 octets and two opcodes Table 7-140
    leaves unassigned: `control_data_length` is read off the wire (not
    compared to the builder, which would share any bug) and must be 12 + the
    command's payload, the echoed bytes are a non-zero pattern, and the frame
    is the padded 60 octets only where the payload is genuinely short. A5
    alone proves one 4-byte case, which a length stuck at 4 or a length held
    over from the previous command both survive. (An echo of ZEROS does not:
    A5 grades the echo byte for byte and catches it. An earlier revision of
    this list said otherwise.) A live Hive 4.3.1 session reported "Incorrect
    payload size" against exactly this class - see 06 §8.2 for who was right,
    which was us.
  - **A8/A9** a command addressed to another `entity_id` and an AECP RESPONSE
    arriving as input are both dropped and counted — answering a response is
    how a control plane builds a storm.
  - **A10/A11** three back-to-back commands each echo their own
    `sequence_id`; the snapshot window publishes the counters and image-valid.
  - **A12/A13** the descriptor-memory model accepts in-order queued requests
    while older bursts are owed. A fetch delayed beyond the store watchdog
    must not supply STREAM_OUTPUT bytes to a later STREAM_INPUT command;
    the integrated guard holds the next request and recovery is byte-exact.
    An unterminated burst keeps debt set and memory requests held while three
    more wire commands each receive `NO_SUCH_DESCRIPTOR` in bounded time.
    The standalone [guard suite](../desc_mem_guard/README.md) supplies the
    unguarded reproduction, hold-deleted mutant and independent reset checks.
    The bench observes `u_dut.u_desc_mem_guard.debt_o` hierarchically; debt
    routing through the product top and parent consumer is deferred to D3.
- **N** **GET_NAME and SET_NAME end to end** (Milan v1.2 5.4.2.11/.12):
  every named slot in the fixture answers with cdl 84 and the exact 64 bytes
  carried by its descriptor. The sweep includes both ENTITY semantic indices,
  all other named descriptor types at index 0, and negative cases for an
  unnamed descriptor, an invalid semantic index, a missing descriptor, and a
  truncated command. Successful SET_NAME is followed by GET_NAME and
  READ_DESCRIPTOR for both offset-4 `object_name` and the ENTITY group name at
  offset 180. A foreign controller under lock receives `ENTITY_LOCKED` with
  the old current name, and the stored value remains unchanged.
- **M** **MVU GET_MILAN_INFO end to end** (Milan v1.2 §5.4.4.1) — the command a
  Milan controller sends FIRST, before a single descriptor, and the one whose
  answer decides whether it treats this device as a PAAD-AE at all. It is not
  an AEM opcode: §5.4.3.2 puts a 48-bit `protocol_id` at @22..@27 and the MVU
  `command_type` at @28..@29, so the field the 03 §4 record calls `opcode`
  holds the head of the protocol_id and nothing that names the command.
  - **M1/M2** the Figure 5.4 response byte-exact (44-byte AECPDU, cdl 32,
    message_type VENDOR_UNIQUE_RESPONSE, protocol_id intact), and the three
    fields decoded OFF THE WIRE: `protocol_version` 1 (§4.2.4),
    `features_flags` 0 and `certification_version` 0. The last two are checked
    by name because Table 5.20's REDUNDANCY would claim Milan §8 on a
    single-interface PAAD and TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING would claim
    map changes while a Stream Output is running, which the root integrator
    deliberately refuses.
  - **M3** a FOREIGN vendor-unique protocol (same Avnu OUI-36, protocol id
    0x101) comes back echoed with MVU status 1. This proves the whole 48 bits
    are compared: nothing above @26 tells the two protocols apart.
  - **M4** pins the [October waiver](../../docs/architecture/06_aecp_engine.md#69-mvu-commands)
    for SET/GET_SYSTEM_UNIQUE_ID and SET/GET_MEDIA_CLOCK_REFERENCE_INFO
    (0x0001–0x0004), plus reserved command type 0x0005 for generic refusal.
    Each complete command receives VENDOR_UNIQUE_RESPONSE, status 1
    NOT_IMPLEMENTED, and its own bytes and cdl echoed exactly. The SET
    payloads contain a nonzero ID or priority/name; the short GET payloads
    remain short. In command order, response AECPDU lengths are 40/32/104/32
    bytes, cdl 28/20/92/20, and untagged frame lengths excluding FCS are
    60/60/118/60 bytes. The reserved type uses cdl 20. M1/M2 separately pin
    features_flags = 0; Table 5.20 has no support bit for either waived pair.
  - **M4L** takes LOCK_ENTITY with one controller, then sends both complete,
    nonzero waived SETs from another controller. Each must still receive its
    byte-exact NOT_IMPLEMENTED echo, with no extra AECP frame during a 20 ms
    observation. The lock grant
    and the holder's subsequent unlock are also checked byte-exact.
  - **M5** the r field is compared and the reserved field is not — §5.4.3.2.2
    requires r = 0 and gives the receiver no leave to ignore it, while
    §5.4.4.1's reserved field is explicitly "ignored by the receiver". So r = 1
    is echoed, and a junk reserved field still gets the real answer with a
    reserved field of 0.
  - **M6/M7** a truncated MVU command is echoed rather than answered from bytes
    nobody read; and a READ_DESCRIPTOR after the MVU traffic is still
    byte-exact, because Hive enumerating is worth more than the gap this closes.
- **Audio-map edit transaction**: ADD/REMOVE_AUDIO_MAPPINGS cover atomic
  validation and commit, duplicate-safe removal, static-port refusal,
  running-output refusal, state-changing success notifications with
  idempotent ADD kept silent,
  normalized Figure 7-71 responses, reserved-field clearing, timeout behavior
  after the phase-1 reservation point, and live scoreboard ownership. R19a
  parks MAP_CFG at the output streaming recheck, injects a state-changing
  source-1 PROBE_TX, proves the scoreboard hold is nonzero and the ACMP response
  and declaration edge are absent, then releases the map and grades both
  transactions in order. R21 grades the `aecp_nvm_stb_o` /
  `aecp_nvm_mark_o` export (issue #90) on this face and the name store: a
  committed ADD carries mark 6 and a committed SET_NAME mark 7, one strobe
  each, and the GET between them carries none — a mark has no wire shape, so
  the pin is the only place any of this is visible. A mark is a completion
  notification: R21 proves the notification, never persistence (section D3
  grades the scalar records; maps and names are later stages).
- **R** boot restore over a blank NVM device: all 8 BINDING regions read,
  the walk's terminal without `restore_fail`. The loop waits on the binding
  manager's own terminal (`dbg_walk_done_o`), not on `restore_done_o`, so every
  later section keeps the clock it was tuned against (section T's note).
- **S0/S1** quiescence + snapshot identity, and `restore_done_o` has followed
  both walks' terminals (the top's level waits for the listener admission
  gate's release, issue #92, and for the D3 walk; BW4 grades it cycle by
  cycle; S0 reads it after its 20 ms so section R's clock is unchanged); SRP bring-up: the FIRST MSRP
  frame is the Domain default declaration `New {6,3,2}`, byte-exact.
- **BW** (runs last, behind resets of its own) a read-only command in the
  boot window, at the top (issue #92): sink 0 is bound to a talker of the
  section's own and committed (a verified F07.8 record in the device model);
  across a reset one `GET_RX_STATE` arrives before `restore_go_i` and one once
  region 0x20 has been read and stored, with the device slowed so the walk
  stays open. Both are held while the walk runs, both answer the restored
  binding byte-exact after it, nothing is written to region 0x20, and the
  next reset restores the same binding. **BW3** (issue #93) the device stops
  granting the walk's next read after region 0x20 was stored: the walk fails
  whole at its read deadline (the top derives `NVM_RS_TMO_CYC_P` from the
  wrap's `CLK_HZ_P`: 20 ms of 1,000,001 Hz, 20,001 clocks),
  cause 3 and nothing preloaded, and the GET held in the window answers the
  vendor default at the listener's release, while `entity_enable_i` is
  still low and before the restore is done: the D3 walk's first read meets
  the port the arbiter drains and ends at its own deadline on defaults
  (`rs_cause_o` 3), within two deadlines of the silence. Released, the device
  serves the abandoned read, which the arbiter drains. Every debounce and
  flush then runs out before anything else happens, and region 0x20 must
  still hold the saved record the failed walk had stored: the held GET's
  write-back restated the listener's default and changes nothing (issue #92
  after #93's causes). A BIND after it commits only once that read has
  ended, a verified F07.8 record, and the next reset restores it. **BW4**
  (issue #93 S4) grades the top's `restore_done_o` and `restore_busy_o` in
  every cycle of every walk the run made (section R's blank boot, BW0-BW3,
  the boot after BW3, and one of its own whose last offer is sink 7's saved
  binding, seeded in the device model): from a walk's first busy cycle to the
  next reset one of the two reads 1; `restore_done_o` never reads 1 while
  the admission gate still owns the listener; and no preload record write or
  discovery arm of a walk lands in a cycle at or after its `restore_done_o`
  rose. The walks' terminals lead the release by at least one cycle each, so
  the composition is exercised, not assumed.
- **S2** `DECLARE_TALKER` (svc face) → Σ-slope admission equals the
  independent Milan model (sum, granted, admitted, no over-limit) → Talker
  Advertise `New` AND MVRP VID `New` byte-exact on the MAC stream.
- **S3** first, before its flush: no ADPDU reached the queue between S0's link
  rise and this enable (a queue read only, so no later clock moves; section AD0
  holds the same state past the whole T-ADP-DELAY span). Then entity enable →
  82 B ENTITY_AVAILABLE byte-exact (aidx 0) inside the T-ADP-DELAY-START
  window; re-advertise with aidx 1 at the T-ADP-ADV 5 s + 0-4 s anti-storm
  cadence.
- **S4** ENTITY_DISCOVER in → delayed byte-exact response at the running
  available_index; zero front-end drops.
- **S5** host face: ctrl scratch RW, status flags, firmware-window error
  when disabled, snapshot reads clean.
- **S6** BIND_RX in → byte-exact BIND_RX_RESPONSE; talker ENTITY_AVAILABLE
  in → discovery event through the router (trace-ring record checked via
  the host face) → byte-exact PROBE_TX_COMMAND inside the T-ACMP-DELAY
  window, Ethernet header prepended by the top (lane 2 shim).
- **S7** TX interleave: ADP response + ACMP GET_RX_STATE response + SRP
  Talker Advertise pushed together — each frame byte-exact and whole, all
  three arbiter lanes take grants.
- **S8** certified two-class Domain arrival (FirstValue {5,2,5}, nov 2)
  adopts {3,5} and re-declares `Lv{6,3,2}+New{6,3,5}` byte-exact; listener
  READY end-to-end (class-D snapshot + Listener Ready `New` byte-exact +
  TK_ATTR_REGISTERED trace record).
- **S9** the S6 binding commits through the debounced NVM shadow: framed
  F07.8 record (magic 0x1722) carrying the bound talker EID at the device
  face, and the `nvm_unflushed_o` export (issue #90) sampled every cycle
  across it — the sink reads unflushed while the change waits, and 0 once
  the commit reports done. That is the binding manager's pending only; the
  D3 records' is `d3_unflushed_o` (D3S2), and an integrator's pending is
  their OR.
- **S10** the `maap` face (02 §4.2), which the top publishes because 01 §3
  puts address allocation in the integrating fabric. Run in two halves. With
  NO allocator (`maap_req_ready_i` 0 for the whole run above): the port is
  seen OFFERING requests, nothing is accepted, `acmp_declaring_o` is 0, and —
  the regression — a GET_TX_STATE_COMMAND is still answered byte-exact, plus
  a PROBE_TX answered byte-exact TALKER_DEST_MAC_FAILED. Before the accept
  window existed, one unaccepted allocation parked the single talker walker
  forever and neither answer ever came. After the allocator becomes available,
  one retry period plus a bounded
  sweep acquires all eight enabled sources without another probe (issue #128).
  The address supplied by the allocator BFM is tracked per source;
  `acmp_declaring_o[0]` is observed rising 0 -> 1,
  the granted address is what the next GET_TX_STATE_RESPONSE carries, and the
  same address appears as the dest MAC of the Talker Advertise on the MSRP
  wire — MAAP -> DA gate -> ACMP answer -> SRP declaration, end to end.
- **V** **GET_AVB_INFO / GET_AS_PATH and their Table 5.22 triggers**: both
  solicited responses are byte-exact over the integrator-owned gather face;
  missing descriptors and truncated commands fail with the required status;
  a GM identity publish raises the independent AVB-info and AS-path strobes
  and produces both pushes; a PathTrace-tail publish produces only
  `GET_AS_PATH`; another AVB-info-word change produces only `GET_AVB_INFO`;
  and `gm_change_i` alone produces only `GET_AVB_INFO`, because the same ADP
  duty also covers a domain-only change whose path sequence did not move.
- **DV** **the Domain default is the top's `SRP_DOM_DEF_VID_P`** (issue #95;
  10 §6.1 F10.2, F01.5 `P-SRP-DOM-DEF-VID`), in both builds on a fresh model:
  the reset value, the LINK_UP declaration byte-exact with all 16 bits of
  SRclassVID, the GET_TX_STATE stream VLAN, a bridge's Domain still adopted
  over it, the LINK_DOWN revert and the LINK_UP re-declaration. See section DV.
- **AD** **the boot gate, and the ADPDU across SET_CONFIGURATION** (issues #41
  and #40; REQ-ADP-006 and REQ-ADP-005, Milan §5.6.1 and the §5.6.2 note, IEEE
  §6.2.2.18), on a fresh processor of its own (the GI pattern:
  its own model, erased NVM, nothing bound), so the main run's clock is
  untouched. Its image declares two configurations with default 1, and
  `current_cfg_i` says 1. AD0 (issue #41; REQ-ADP-006, Milan §5.6.1) first:
  the link rises with `entity_enable_i` low and stays up for 4200 ms, bounces
  for 20 ms and stays up for another 4200 ms, each window longer than the whole
  T-ADP-DELAY span, with nothing flushing the ADP queue. The queue stays empty,
  the advertise SM reads DOWN at every 100 ms sample (snapshot word 31) and the
  flags word reads {seeded, link, !enable}. S3 also checks, before its flush,
  that nothing reached the queue between S0's link rise and S3's enable, but
  that span is shorter than T-ADP-DELAY, so AD0 is the check that can see a
  gate defect (the record is `gate-enable-dropped-top` in `tb/adp_engine`'s
  campaign). AD1: the first ENTITY_AVAILABLE carries 1 and
  GET_CONFIGURATION agrees. AD1b: SET_CLOCK_SOURCE writes another row of the
  store and the next advert still carries 1. AD2: SET_CONFIGURATION(0), then
  GET, the ENTITY descriptor and the next ENTITY_AVAILABLE all say 0, and that
  advert equals the one before it in every wire byte but available_index
  (50..53, +1) and current_configuration_index (64..65). AD3: the same back
  to 1. AD4: `current_cfg_i` moves to 0 and the next advert still carries 1,
  the set configuration. From AD5 on each arm resets the processor as a power
  cycle and re-runs both restore walks (PR #132's D3 writer), and grades the
  first ENTITY_AVAILABLE after the enable byte-exact, GET_CONFIGURATION, the
  ENTITY descriptor, and the valid flag the ADPDU reads against the store's own
  in every clock from the reset. AD5: SET_CONFIGURATION(0) is saved to record
  0x00, the power cycle carries the device, the restore writes the row, and all
  three views say 0 from the first advert. AD6: the restore applies that record
  and then aborts in pass 1 (record 0x50 read whole in pass 0 and erased before
  pass 1, D3R4's disagreement), the roll-back resets both stores, and all three
  views fall back to the image default 1. AD7 (review R406-1 F-1): a SUCCESS
  SET_CONFIGURATION(0), then a reset with nothing to restore (an erased device):
  the valid flag clears with the row, and all three views carry the image default
  1. `make adp-config` runs this section alone; the
  default run includes it. The mutation record is `tb/adp_engine`'s campaign
  (`make -C tb/adp_engine mutants`), which runs this section against each
  patch.
- **DL** **the AECP transaction deadline** (issue #81, GAP-07; 03 §6 rule (e),
  08 §4; IEEE 1722.1-2021 §9.3.2.6, Milan v1.2 §5.4.3.4), on a fresh processor
  of its own. The normalizer stamps each AECP transaction's deadline
  T-BUDGET-AECP-WC after its reception, the top reads it at admission, and a
  command still executing when it passes is preempted into the forced FAIL_SAFE
  response, which must be on the wire inside T-AECP-RESP. In this timebase the
  deadline is 10,000 clocks and the response line 24,000. Every stall is a face
  answering slowly but inside its 4,096-clock watchdog, the case no watchdog
  catches. **DL1** a GET_COUNTERS whose counter store answers each quadlet
  1,000 clocks late (33 quadlets, about 330 ms) is answered ENTITY_MISBEHAVING,
  header only, byte-exact: the kill rises at the deadline, the first byte lands
  after it and inside T-AECP-RESP, the program stopped at an op boundary (fewer
  than 33 quadlets asked), the µCPU was redirected once, and the scoreboard
  honoured the kill once, in the clock the forced response was handed to its
  lane, the killed hold's bit set in every clock from the expiry to that hand-off
  and clear after it. **DL2** (no partial commit) a SET_NAME whose descriptor
  fetch is 3,000 clocks late writes its name lanes before the deadline and
  builds its nine-lane body against a response memory taking 1,500 clocks per
  lane write, so the deadline passes inside the body: it answers its own
  SUCCESS byte-exact (late in this timebase; each remaining op is
  watchdog-bounded, well under a millisecond at P-CLK-HZ), the µCPU is never
  redirected, its NVM mark lands once and GET_NAME reads the name back. **DL3**
  a GET_MILAN_INFO queued behind DL1's stall waits past its own deadline (the
  deadline counts from reception) and is preempted at its first op boundary:
  it answers MVU NOT_IMPLEMENTED with the command echoed (Milan Table 5.19 has
  no status 10), byte-exact, past T-BUDGET-AECP-WC and inside T-AECP-RESP of
  its own reception. **DL4** a GET_DYNAMIC_INFO of twelve GET_VIDEO_FORMAT
  records against a response memory taking 3,900 clocks per lane write (about
  700 ms of copying) is voided past the deadline: ENTITY_MISBEHAVING, empty,
  inside T-AECP-RESP. **DL5** an AECP RESPONSE arriving as input, queued behind
  a stall and admitted past its deadline, is dropped and owed nothing: no answer,
  one kill honoured (the stall's), every hold free, and a later
  GET_CONFIGURATION is answered. **DL6** an ADD_AUDIO_MAPPINGS whose edit face
  holds each validation request 3,000 clocks is still validating at the
  deadline and is never preempted: its own SUCCESS, both records committed.
  **DL7** every RX slot is free afterwards and READ_DESCRIPTOR(ENTITY) is
  byte-exact. **DL8** (REQ-MVU-005, Milan Table 5.19) a GET_MILAN_INFO whose
  response memory fails, by a read error, a write error or a tied-off master,
  answers MVU NOT_IMPLEMENTED with the command echoed, byte-exact, and the void
  is counted; one padded to 540 payload bytes echoes a 578-byte frame, which the
  engine sized into the oversize slot; a GET_CONFIGURATION under the read error
  still answers ENTITY_MISBEHAVING header only; afterwards every RX slot is free
  and GET_MILAN_INFO answers SUCCESS. **DL9** (IEEE 1722.1-2021 Table 9-2:
  status 10 is AEM's alone) an ADDRESS_ACCESS, an AVC, an HDCP_APM and an
  EXTENDED command each answer NOT_IMPLEMENTED with the command echoed idle,
  and the same frame, byte-exact, when queued behind DL1's stall past their
  deadline and preempted, inside T-AECP-RESP of their reception. **DL10**
  REGISTER_UNSOLICITED_NOTIFICATION and LOCK_ENTITY commit on the registry face
  in their first op, which the µCPU cannot see as an effect, so they are never
  preempted: each, queued behind DL1's stall past its own deadline, answers its
  own SUCCESS byte-exact with no redirect, the lock is then held, and after the
  unlock a SET_NAME by another controller is pushed to the registered one.
  **DL11** a kill the scoreboard honours ends the AECP owner, so the RX-slot
  return after it releases nothing (DL1 checks this at the kill too): across
  the section every normal release names a live hold (`dbg_sb_rel_o`,
  `dbg_sb_rel_id_o`). The boot-held command's exemption (rule (d)) is D3O6's 150 ms
  hold. `make deadline` runs this section alone; the default run includes it.
  The taps are `dbg_aecp_dl_kill_o`, `dbg_aecp_dl_queued_o`,
  `dbg_sb_kill_ack_o`, `dbg_aecp_sb_id_o`, `dbg_sb_holds_o` and
  `dbg_ucpu_pre_o`; the mutation record is below (`aecp_mutants.py`).
- **TB** **response budgets under the 08 §4 worst-case stimuli** (issue #57,
  REQ-MVU-005, Milan v1.2 §5.4.3.4; issue #81 acceptance 3), on a fresh
  processor of its own in the third build, whose timebase is the nominal clock's
  own (1 ms = 1,000 clocks): the deadline (100 ms, 100,000 clocks there) never
  cuts a measurement, and the section ends by proving it never fired. Latency is
  MAC command byte 0 to MAC response byte 0, graded like B4/B4b against its line
  at P-CLK-HZ = 100 MHz: T-AECP-RESP 24,000,000 clocks and T-BUDGET-AECP-WC
  10,000,000 for AECP, T-BUDGET-ACMP-RESP 5,000,000 for ACMP. The image's
  SIGNAL_MULTIPLEXER fills the whole 576-byte line buffer here. **TB1**
  GET_MILAN_INFO and GET_SYSTEM_UNIQUE_ID (waived, so NOT_IMPLEMENTED with the
  command echoed), byte-exact, at the suite latency and at 143 clocks per
  access. **TB2** READ_DESCRIPTOR of the 576-byte descriptor (a 618-byte frame
  in the oversize TX slot) byte-exact, and a GET_DYNAMIC_INFO carrying all
  thirteen §7.4.76.2 getters (SUCCESS, cdl within 524), at both latencies.
  **TB3** sixteen controllers register (every registry row) and a
  SET_CLOCK_SOURCE fans fifteen unsolicited responses through the engine; each
  of the three commands above, asked with such a fan-out in flight at 143
  clocks, is answered as it is idle and follows the fan-out's last frame by at
  most one job plus its own idle latency: KL_aecp_notify holds the command
  path while a class drains, so the 08 §4 "≤ 1 frame-time gap" row is not what
  the RTL does (a recorded finding, 08 §4). **TB4** GET_MILAN_INFO against a
  response memory taking 4,000 clocks per access, short of its 4,096 watchdog.
  **TB5** GET_RX_STATE and GET_TX_STATE idle, and beside the oversize
  READ_DESCRIPTOR and a fan-out: unchanged, and never later than idle by more
  than one frame on the wire. The histogram it prints is recorded in 08 §4.
  `make budget` runs this build alone; `make` runs all three.
- **HZ** **the nine F03.7 hazard classes at the scoreboard** (issue #84, GAP-10;
  03 §6), on a fresh processor of its own. The expectation is an independent
  transcription of F03.7 and F06.14: the class in F03.7 row order and the key a
  descriptor's `{type[5:0], index[9:0]}`. The taps are the scoreboard's
  admission port (`dbg_sb_class_o`, `dbg_sb_key_o`, `dbg_sb_acc_aecp_o`,
  `dbg_sb_acc_acmp_o`, and `dbg_sb_ref_aecp_o`/`dbg_sb_ref_acmp_o`, the head it
  was asked about and refused), the two owners' holds and the pending drain
  (`dbg_sb_barrier_o`). **HZ1** 33 AECP transactions (every class, the GETs'
  descriptor keys, READ_DESCRIPTOR, GET_DYNAMIC_INFO, MVU, an AECP response as
  input and a command for another entity_id) and 6 ACMP ones each present their
  class and key. An ACMP transaction is held in flight by stalling the MAC until
  four GET_RX_STATE answers fill the standard TX slots, so the next ACMP command
  is admitted and keeps its key until the MAC restarts. The same stall holds an
  AECP command, whose response waits for a standard slot: the talker returns its
  RX slot once it has read a frame, so a talker transaction keeps its key a few
  clocks only and every talker-side pair is graded with the AECP command held.
  Every "waits" check requires the scoreboard to have refused the head at least
  once while the other holds, and a MAAP allocator answers, so the talker is
  free to take commands (without one it spends each retry round in its MAAP
  request wait). **HZ2** SET_CONFIGURATION latches the drain while an UNBIND_RX
  holds, is admitted once that key frees, keeps a later ACMP head behind it, and
  answers SUCCESS. **HZ3** with ACMP preferred by the round-robin (an AECP frame
  for another entity admitted beside the held step) and a GET_RX_STATE queued
  behind the pending barrier, both are answered, the barrier first: without the
  barrier's priority the admission port wedges for good. **HZ4** LOCK_ENTITY
  waits for a held UNBIND_RX and the unlock runs beside a held GET_RX_STATE.
  **HZ5** START_STREAMING waits for the held sink's key and runs beside another
  sink's; **HZ6** GET_STREAM_INFO likewise, and beside a GET_RX_STATE of the same
  sink (two reads); **HZ7** ADD_AUDIO_MAPPINGS waits for any stream step.
  **HZ8** SET_SAMPLING_RATE, SET_NAME, REGISTER_UNSOLICITED_NOTIFICATION,
  SET_CONTROL and READ_DESCRIPTOR each run beside a held stream step, and the
  REGISTER beside a held GET_RX_STATE: REGISTRY_OP is the one class with no
  reachable conflict at this top. **HZ9** (NAME_WR) a SET_NAME on STREAM_INPUT 1
  waits for a held GET_RX_STATE of sink 1, then answers SUCCESS and GET_NAME
  reads the name back; on STREAM_INPUT 0 it runs beside that read, and on
  STREAM_INPUT 1 beside an UNBIND_RX of sink 1; held, a SET_NAME on
  STREAM_OUTPUT 1 holds back a GET_TX_STATE of source 1 and not one of source 2,
  and one on STREAM_INPUT 1 a GET_RX_STATE of sink 1. **HZ10** STOP_STREAMING on
  STREAM_INPUT 1 waits for a held GET_RX_STATE of sink 1; held on STREAM_OUTPUT
  1 it holds back a GET_TX_STATE and a DISCONNECT_TX of source 1 and not a
  DISCONNECT_TX of source 2; a held GET_STREAM_INFO on STREAM_OUTPUT 1 holds
  back the DISCONNECT_TX and not the GET_TX_STATE. **HZ11** a held
  SET_CONFIGURATION holds back a GET_TX_STATE; a held LOCK_ENTITY or
  ADD_AUDIO_MAPPINGS holds back a DISCONNECT_TX and not a GET_TX_STATE.
  **HZ12** SET_SAMPLING_RATE, SET_CONTROL and ADD_AUDIO_MAPPINGS naming a stream
  descriptor (which none may legally name) each wait for a held GET_RX_STATE of
  sink 1 when they name STREAM_INPUT 1 and are then refused NOT_SUPPORTED, run
  beside it when they name STREAM_INPUT 0, and, held naming STREAM_OUTPUT 1,
  hold back a GET_TX_STATE of source 1. `make hazards` runs this section alone;
  the default run includes it.

## Snapshot window map (side port 0x20000, implemented by the top)

The map moved out of this file. It is a product contract, not a testbench note, and it is
now maintained word by word and bit by bit in the
[operator guide](../../docs/guides/operator.md#5-the-snapshot-window-word-by-word), with
the window list in [07 §5.5](../../docs/architecture/07_memory_maps.md). This suite reads
words 0, 3, 32, 33 and 34 as part of scenarios S5 and A11, and words 35 and 36 in B6
through B9.

Trace window 0x40000: record = 4 words, lane 0 = now_ms, lane 1 =
{source, flags, payload} (event-router consumer glue).

## Mutation record (backup / sed / run / restore)

| # | what was broken | result |
|---|---|---|
| M1 | `KL_mrp_strip` strips 13 bytes instead of 14 (`body_w` compare 4'd14→4'd13) | 9 FAIL — S8 Domain adoption, listener READY, class-D: the SRP RX seam is load-bearing |
| M2 | ACMP prepend shim EtherType 0x22F0→0x22F1 | 8 FAIL — S6/S7 every ACMP wire check: the prepended header is what the wire sees |
| M3 | steer prefetch reads the addressed EID at PDU offset 27 instead of 28 | 16 FAIL — S6/S7/S9 the listener silently ignores mis-addressed heads (and the binding never commits): the target_eid rewrite is the real multicast discriminator |
| M4 | `KL_acmp_talker` S_EV_MAAP loses its timeout exit (the deadlock restored) | 5 FAIL — S10: with no allocator the talker walker never consumes another command, so neither ACMP answer reaches the wire |
| M5 | the top re-ties `.maap_req_ready_i (1'b0)` on the talker instance | 6 FAIL — S10: no grant, no gate, no declared DA on the SRP wire. The port is load-bearing, not decoration |
| M6 | `KL_aecp_dyn_state` drops the SEL_CFG write (`cfg_r` assignment removed) | 8 FAIL -- W17j, W18c, W18d3, W18f, W18g, W19a, W19g3, W19h: the configuration overlay is observed end to end. The W22 block passes under this mutation by construction: its echo is command-sourced and its expected 0 equals both the reset value and the image default. W22 exists for two other properties: a SUCCESS-arm refusal predicate stuck after W21u's unbind (W22a) and a lost later write against W18's residue of 1 (W22d) |
| M7 | the engine's SET_STREAM_FORMAT running route forced dead (`run_this_w` arm to `1'b0`) | 2 FAIL -- W23h + W23h2: the per-descriptor STREAM_IS_RUNNING refusal against a REALLY bound sink, and the write its absence lets through |
| M8 | the verdict requirement dropped (E_SFMTI/E_SFMTO `MOVE r4, 3` to `MOVE r4, 0`) | 8 FAIL -- both refusal rows (W23c/W23c2 unsupported, W23d shrink) AND the success rows (W23a/W23a2/W23b, W23h/W23h2 -- the latter as collateral: their expected bodies ride W23a.s write through the fold): CHECK_ARG.s comparator is load-bearing in both directions, not a tautology |
| M9 | the engine's SET_STREAM_INFO flag gate forced open (the SIF_ACC_LAT compare to `1'b0`) | 3 FAIL -- W24d/W24e, and W24f collaterally because the extra-flag command's write now lands: nothing is partially applied is a checked property |
| M10 | E_SINFO's WRITE_ST replaced with NOP | 4 FAIL -- W24a2/W24b (the published row and the folded GET), W24d/W24f (rows that assert the value survived refusals). W24a's byte-exact echo PASSES under this mutation -- the echo cannot see a dropped write, which is exactly why the face checks exist |
| M11 | the engine's SET_STREAM_INFO running route forced dead | 2 FAIL -- W25b + W25b2 against a REALLY streaming output (Advertise + registered Listener on the wire) |
| M12 | restored the pre-fix `.ev_asp_i (gm_change_i || gsi_asp_chg_i)` wiring | 1 FAIL at 1,269 checks — V6i: `gm_change_i` alone emits the forbidden `GET_AS_PATH`; the positive simultaneous-strobe arm stays live |
| M13 | tied `.ev_asp_i` to zero | 5 FAIL at 1,269 checks — V6/V6c/V6d lose both GM-entry and tail-only `GET_AS_PATH`; V6f/V6h fail collaterally because the missing frame shifts the per-controller sequence IDs |
| M14 | E_SCLKS+7 (ROM word 1191, the image read that supplies the CURRENT index while the clock-source row is unset) replaced with NOP, in a review copy of `gen_ucode.py`; the ROM is swapped, the tracked generator is not touched | 1 FAIL at 1,300 checks -- W10i2: the refusal on the unset row answers the zero r6 preload instead of the image's 1. W10i (BAD_ARGUMENTS at cdl 20), W10i4 (GET still reads the image) and every W10j effect count PASS under this mutation, which is why W10i exists: no set-row arm (W10f) can see this word |
| M15 | the ROM `gen_ucode.py` generated at 2faa5af8, the last commit before the range check (31 words differ, 1185 to 1215) | 20 FAIL at 1,300 checks -- W10e and W10f for both refused values, W10h, W10i/W10i2/W10i4, and the effect grades of W10j: the refusal of 3 is stored and marked (W10j2, W10j4, W10j5), the refusal of 65535 is stored, marked, enqueued AND announced at the second controller (W10j2 to W10j6), and the accepted SET that follows carries the residue in its counts and in its notification sequence (W10j8, W10j10 to W10j12) |
| M16 | E_SCLKS+19 (ROM word 1203, the BUILD_FLD that puts the stored index, r12, at @28 of the success response) rewritten to build r6, the CURRENT index read before the write, in a review copy of `gen_ucode.py`; the same shape as the refusal tail nine words later, so a refactor that shares that tail produces it. The ROM is swapped, the tracked generator is not touched | 2 FAIL at 1,300 checks -- W10j7b: the accepted SET from the unset row answers 0, the index it replaced, not the 1 it stored; W10b: the SET on the set row answers 1, not the 2 it stored. Every other check PASSES, W10j8 included: the engine rebuilds the unsolicited copy from the stored row through GET_CLOCK_SOURCE's program, not from the response, so only the response body can see this word, and only when the SET changes the index; which is why W10j stores 1 and W10 then stores 2 |
| M17 | `KL_acmp_talker` keys REGISTERING_FAILED on the retired private code 3 again (`rf_live_w` compares `srp_pkg::srp_decl_e'(2'd3)`, the pre-fix `LSN_ASKING_FAILED_C`), in a copy of the tree | 5 FAIL at 1,316 checks -- T2 and T3 for Asking Failed (flags 0x0000) and for Ready Failed (flags 0x0040), and T5. The same five failures are the reproduction of issue #46 at `2ccb427`, before the fix |
| M18 | `srp_pkg::srp_decl_e` swaps ASKING_FAILED and READY_FAILED (1 and 3), in a copy of the tree | the same 5 FAIL at 1,316 checks, and nothing else in this suite: the decoder publishes the wire code and the top's streaming reduction tests bit 1. The SRP engine's own readers of the package move too (the talker FSM's ACTIVE term, the listener FSM's declaration), but this suite never grades them against an Asking Failed; `srp_stream_fsms` (4) and `srp_top` (1) do, and fail under the same mutation, as does `acmp_talker` (3) |
| M19 | THE LIST CHECK REMOVED: E_SSRATE+13 (ROM word 1165) `BRANCH E_SSRWALK` rewritten to `BRANCH SSR_ACCEPT`, so no rate is ever looked up, in a review copy of `gen_ucode.py`; the ROM is swapped, the tracked generator is not touched | 28 FAIL at 1,361 checks, the same 28 as M24: W9i/W9i2 (44100 answered SUCCESS on the unset row and read back), W9j/W9j2 (192000 stored on the set row), W11e (collateral: W9j's 192000 is still in the row), W9k2-W9k6 for all three refused values (each stored, marked and announced to the second controller), W9k8 and W9k10-W9k12 (the accepted SET carries the residue in its counts and its notification sequence), W9l2, W9l5, W9l6, W9m5 |
| M20 | the count bound removed: the walk's two `BR_STATUS` per lane that leave on `count == k` replaced with NOP (E_SSRWALK+1, +7, +11, +17, +21, +27, +31, +37) | 10 FAIL at 1,361 checks -- W9k2-W9k6 for the zero rate, which the walk now "finds" in the lane past the 152-byte descriptor, and the residue it leaves in W9k8 and W9k10-W9k12; W9l2: with the count patched to 1, entry 1's 96000 is accepted |
| M21 | the offset check removed: E_SSRATE+12 (ROM word 1164, the `CHECK_ARG` of `sampling_rates_offset` against 144) replaced with NOP | 1 FAIL at 1,361 checks -- W9l6: with the offset patched to 148 the walk still reads the list at 144 and accepts 48000. Nothing else can see this word, which is why W9l patches the offset |
| M22 | the refusal tail builds r12, the REJECTED rate, at @28 instead of r6, the current one (E_SSRATE+17, ROM word 1169) | 9 FAIL at 1,361 checks -- every refusal that is graded byte-exact: W9i, W9j, W9k2 three times, W9l2, W9l5, W9l6, W9m5. Status, GET and every effect count PASS under it |
| M23 | the unset row's image read removed: E_SSRATE+6 (ROM word 1158, `SHIFT_R` of the image's current_sampling_rate into r6) replaced with NOP, so r6 keeps its zero preload | 1 FAIL at 1,361 checks -- W9i: the refusal on the unset row carries 0 instead of the image's 96000. Every set-row arm reads the dynamic row instead and PASSES, which is why W9i runs before W9 |
| M24 | the ROM `gen_ucode.py` generates at `6a9a124`, the last commit before the list check (the issue #51 reproduction with the final harness) | 28 FAIL at 1,361 checks, the M19 list |
| M25 | the top's `.DOM_DEF_VID_P (SRP_DOM_DEF_VID_P)` binding line removed | fixture build 13 FAIL of 20: every value check of DV1 to DV6. The default build PASSES all 1,391, which is why the fixture build exists |
| M26 | the binding misbound to a literal, `.DOM_DEF_VID_P (16'd2)` | fixture build 13 FAIL of 20, the M25 set |
| M27 | the binding truncated to 12 bits, `.DOM_DEF_VID_P (16'(SRP_DOM_DEF_VID_P[11:0]))` | fixture build 4 FAIL of 20: DV2 (byte-exact and the decoded SRclassVID), DV4 and DV6, the checks that read the 16-bit wire field. Every 12-bit face PASSES |
| M28 | the value bound to the wrong child parameter, `.DOM_DEF_PRIO_P (SRP_DOM_DEF_VID_P[7:0])`, the VID left unbound | default build 16 FAIL of 1,391 (S1 twice, S2, S7, S8 and 11 in DV); fixture build 13 FAIL of 20 |
| M29 | the top's own default changed from 2 to 3 | default build 18 FAIL of 1,391 (S1 twice, S8, T0, MP3 and 13 in DV). The fixture build PASSES 20 of 20, because it overrides the default |
| M30 | the wrap's fixture override removed (test infrastructure) | fixture build 13 FAIL of 20: the build cannot pass on the top's or the child's own default |
| M31 | control, not a defect: `KL_srp_top`'s own `DOM_DEF_VID_P` default changed from 2 to 7, binding intact | both builds PASS, 1,391 and 20: the child's default is no longer a source |
| M32 | the listener admission gate deleted (`KL_pp_acmp_lsn_admit` never owns: `own_r` resets to 0), the top's wiring before issue #92 | default build 5 FAIL of 1,401: BW1 four times (the GET before the walk is answered at once; the one inside the window is taken, answers sink 0 unbound and its write-back rewrites region 0x20 unbound) and BW2 (the next reset restores nothing). Every other section PASSES |
| M33 | the binding walk's read deadline deleted (`KL_acmp_nvm_shadow` `rs_tmo_w` tied 0) | default build 2 FAIL of 1,407: BW3 (the walk never ends in 80,000 clocks, and the held GET is never answered). Every other section PASSES |
| M34 | the abandoned read never drained (`KL_pp_nvm_mgr_arb` never sets `drain_r`) | default build 2 FAIL of 1,407: BW3 (the late read is handed to the binding manager, which takes no byte outside its walk, so the read never ends: the BIND is never committed and the next reset restores nothing). Every other section PASSES |
| M35 | the binding manager's capture compare back to its form before the unbound rule (`KL_acmp_nvm_shadow` `c1_diff_w` true on any field difference, so two unbound records can differ) | default build 2 FAIL of 1,414: BW3 (the GET served after the failed walk rewrites region 0x20 unbound, byte 8 `03` to `00`, before any BIND; the BIND then writes region 0x20 a second time). Every other section PASSES |
| M36 | the top's `restore_done_o` without the release (`= nvm_walk_done_w`) | default build 2 FAIL of 1,414: BW4 (8 cycles of `restore_done_o` while the gate owned the listener, one in each of the six earlier walks and two in BW4's own; and in BW4's own walk the last preload's discovery arm lands in the cycle `restore_done_o` rose). Every other section PASSES |
| M37 | the top's `restore_busy_o` without the gap term (`= nvm_walk_busy_w`) | default build 1 FAIL of 1,414: BW4 (8 cycles read neither busy nor done). Every other section PASSES |

Every row but the M31 control bites; originals restored; suite back to green. The M1-M6 counts were
taken when the suite stood at 86 checks (scenario A and section B have since
been added) and the M7-M11 counts at 1,139, so re-run a mutation before quoting
its blast radius. M12-M13 were measured at 1,269 checks, M14-M16 at 1,300,
M17-M18 at 1,316, M19-M24 at 1,361. M14 to M16 and M19 to M24
are ROM swaps (`ucode.hex` is read at simulation start), so they need no
rebuild: generate the review ROM elsewhere, copy it over `ucode.hex`, run
`./obj_dir/Vpp_top_sim`, and put the generated ROM back. Only its sha256 against
the generator's output proves the ROM is the pinned one again: a copied ROM is
newer than `gen_ucode.py`, so `make` alone leaves it in place, and `git status`
cannot see it (`ucode.hex` is ignored); `rm ucode.hex && make` regenerates it.
M25 to M31 were measured on 2026-09-22 at 1,391 checks in the default build and
20 in the fixture build, each in its own scratch copy of `hdl/` and `tb/pp_top/`,
so there was nothing to restore. M25 to M30 bite; M31, a control, stays green as
it must. M32 was measured on 2026-09-23 at 1,401 checks in the default build and
again on 2026-09-24 at 1,407 (the same five), M33 and M34 on 2026-09-24 at 1,407.
M32 to M37 were measured on 2026-09-24 at 1,414, each in its own extract of the
tree: M32 then fails 8 (BW1 four times, BW2, BW3's saved-record check, and BW4
twice, because a gate that never owns makes no gap and sees no preload), M33
and M34 the same 2 as before.

### D3 negative controls (issue #131): `d3_mutants.py`

`python3 d3_mutants.py --output DIR [--jobs N]` plants each control below in its own
extract of `hdl/`, `tb/common/` and this directory, builds `gsi-build`, runs
`--d3-only`, and counts the mutant KILLED only when the run completes with its
tally, exits non-zero and every named check fails; a golden extract runs first and
must pass. The same driver runs the binding manager's three DR2c controls, the
arbiter's issue-cycle control and its seven own-contract controls (N11) in `tb/acmp_nvm`
and the validator's admission control in `tb/rx_validator` (their READMEs record
them). At the lane head all 83 are KILLED and the three goldens PASS;
the last column is how many checks each one failed there. Since the AECP
deadline kill (issue #81, section DL), `hold_released_at_go` and
`dispatch_not_held` each fail D3O6 as well (17 and 6). Both let the held
command run during the slowed restore, and it is no longer exempt from its
deadline: rule (d) exempts only a command held until the terminal. With the
kill tied off, the counts are 16 and 5 again.

| Mutant | Defect planted | Named checks, each failing | Failing checks |
|---|---|---|---|
| `hold_released_at_go` | the writer's ownership ends at the walk's go instead of its terminal | `D3O1: released at` | 17 |
| `dispatch_not_held` | the engine's three dispatch gates ignore the writer's ownership | `D3O1: without the walk the writer owns every cycle` | 6 |
| `own_taken_at_the_walk` | ownership and the bus taken only once the walk starts, not from reset | `D3R9: the held SET` | 10 |
| `image_unproven_continues` | an unprovable image (the LOCATE's error) no longer aborts | `D3O2: CLOSED at`, `D3O3: CLOSED` | 9 |
| `latch_ignores_program` | the service latch does not wait for a running program | `D3S9` | 3 |
| `TRG_cfg` | configuration trigger deleted | `D3S1 cfg` | 3 |
| `TRG_rate` | sampling-rate trigger deleted | `D3S1 rate` | 3 |
| `TRG_clks` | clock-source trigger deleted | `D3S1 clks` | 5 |
| `TRG_fmti` | input-format trigger deleted | `D3S1 fmti` | 4 |
| `TRG_fmto` | output-format trigger deleted | `D3S1 fmto` | 4 |
| `TRG_ptof` | presentation-offset trigger deleted | `D3S1 ptof` | 24 |
| `taint_ignored` | a change after the latch no longer taints the write | `D3S4 taint` | 1 |
| `clear_wins_same_edge` | the done's clear outranks a change on the same edge | `D3S5 same edge` | 1 |
| `clear_by_group` | the done clears every record of the group | `D3S6 group` | 8 |
| `clear_by_index` | the done clears every record of the same index | `D3S6 index` | 14 |
| `identify_is_a_change` | IDENTIFY (selector 7) made a persisted change | `D3S7` | 1 |
| `unchanged_compare_ignores_validity` | the change qualifier ignores the valid flag | `D3S8 validity` | 1 |
| `RPL_cfg` | configuration replay deleted | `D3R1 cfg` | 6 |
| `RPL_rate` | sampling-rate replay deleted | `D3R1 rate` | 6 |
| `RPL_clks` | clock-source replay deleted | `D3R1 clks` | 3 |
| `RPL_fmti` | input-format replay deleted | `D3R1 fmti` | 2 |
| `RPL_fmto` | output-format replay deleted | `D3R1 fmto` | 2 |
| `RPL_ptof` | presentation-offset replay deleted | `D3R1 ptof` | 6 |
| `rule_ignored` | a SET-rule refusal applied anyway | `D3R2: COMPLETE` | 3 |
| `passes_may_disagree` | the pass agreement removed | `D3R4:`, `D3R4b` | 3 |
| `device_error_reads_as_blank` | a DEVICE error read as a blank record | `D3R5 device error on the header`, `D3R6: the one saved record` | 5 |
| `unframed_reads_as_device_error` | an UNFRAMED record read as a device error | `D3R6: an erased device restores blank` | 38 |
| `desc_error_is_a_refusal` | a rule's descriptor error read as a refusal | `D3R7` | 6 |
| `no_restore_watchdog` | the per-wait deadline removed | `D3R8: a READ granted`, `D3R8b` | 7 |
| `restore_writes_are_changes` | the snoop taps the shared bus, so restore writes are changes | `D3R1: no restore write is a change` | 1 |
| `enable_not_released_by_restore` | ADP enabled by the request alone | `D3R1: the enable requested from reset` | 4 |
| `done_without_d3` | restore done without the D3 walk | `D3R1: the enable requested from reset`, `D3R1: COMPLETE` | 42 |
| `blank_ignores_d3` | restore blank ignores the D3 walk | `D3R1: COMPLETE` | 1 |
| `store_not_cleared` | the sampling-rate row and its valid flag not reset | `D3R1: every row at its reset value` | 17 |
| `valid_not_cleared` | the sampling-rate valid flag not reset | `D3R1: every row at its reset value` | 17 |
| `quarantine_released_by_time` | the arbiter ends a drain after 1,000 cycles | `D3R5: once the device ends the drained read a later SET persists` | 10 |
| `no_rollback` | a pass-1 abort ends DEFAULTS without the roll-back | `D3R4:` | 17 |
| `dyn_not_rolled_back` | the dynamic-state store left out of the roll-back | `D3R4:` | 4 |
| `store_not_rolled_back` | the descriptor store left out of the roll-back | `D3R10 5000` | 3 |
| `rollback_ignores_debt` | the roll-back ignores the guard's debt | `D3R10 16000` | 3 |
| `closed_releases_the_entity` | a roll-back that cannot re-prove the image ends DEFAULTS | `D3R12` | 2 |
| `no_backoff_d3` | the writer's DR2c backoff removed | `D3S10 timing` | 2 |
| `fourth_attempt` | the writer allows a fourth attempt | `D3S10 count` | 2 |
| `alarm_forgiven_by_success` | a later success clears the writer's alarm | `D3S10 revocation` | 1 |
| `backoff_derivation` | the top derives the backoff as CLK_HZ_P / 200 (5 ms) | `D3S10 timing` | 2 |
| `backoff_holds_dispatch` | BACKOFF holds dispatch and the bus | `D3S10 backoff` | 1 |
| `disagree_one_direction` | the agreement aborts whole-then-blank only | `D3R4b` | 1 |
| `rollback_one_cycle` | the roll-back strobe lasts one cycle without debt | `D3R4 strobe` | 1 |
| `pass1_read_not_drained` | a pass-1 deadline no longer abandons its READ to the drain | `D3R5b: once the device ends the drained pass-1 READ` | 1 |
| `judge_wait_unwatched` | the format judge's wait is not watched | `D3R8b` | 1 |
| `rate_walk_stuck_on_first_lane` | the rate walk never leaves the list's first lane | `D3R3b entry 7` | 1 |
| `rate_walk_unbounded` | the rate walk's eight-entry bound dropped | `D3R3b entry 8` | 1 |
| `no_aggregate_deadline` | the aggregate deadline removed | `D3R13 pass 0: DEFAULTS at clock`, `D3R13 pass 1` | 16 |
| `aggregate_mirrored` | the aggregate a mirrored 100,000,000 instead of CLK_HZ_P | `D3R13 pass 0: DEFAULTS at clock` | 19 |
| `aggregate_from_the_walk` | the aggregate counts from the binding walk's end | `D3R13 pass 0: DEFAULTS at clock` | 21 |
| `per_wait_floor` | the per-wait derivation rounds down | `D3R8 deadline` | 1 |
| `agg_closes_before_proof` | the aggregate aborts before the image is proven (the pre-walk close) | `D3R14 image valid: DEFAULTS` | 11 |
| `binding_walk_ignores_aggregate` | the binding walk ignores the aggregate and reads on | `D3R14 image valid: the binding walk`, `D3R14 image refused: the binding walk` | 10 |
| `proof_reads_records_past_bound` | the image proven past the bound starts pass 0 | `D3R14 image valid: DEFAULTS` | 6 |
| `agg_not_in_rollback` | the aggregate count paused in the roll-back and its re-LOCATE (R390-2's own edit) | `D3R15 debt wait: CLOSED`, `D3R15 re-LOCATE: CLOSED` | 4 |
| `agg_not_stopped_at_terminal` | the aggregate keeps counting after the terminal (R391-2's own edit) | `D3R16 COMPLETE`, `D3R16 DEFAULTS`, `D3R16 CLOSED` | 3 |
| `agg_fires_with_event_in_hand` | the aggregate fires with a grant, byte or answer in hand (R391-2's own edit) | `D3R17: the writer's grant`, `D3R17: once the device ends` | 2 |
| `drain_misses_issue_cycle` | the head's arbiter: the drain armed only for a READ already owned, so an abort in the issue cycle is lost (R390-3 F1) | `D3R18: the READ abandoned in its issue cycle`, `D3R18: once the device ends` | 2 |
| `agg_closes_during_proof` | the aggregate also aborts in the image proof's LOCATE (R390-3's own edit) | `D3R19 inside the LOCATE: DEFAULTS` | 2 |
| `proof_past_needs_fire` | the proof past the bound keyed on the fired level, not the bound reached (R390-3's own edit) | `D3R20 W_IMG: DEFAULTS`, `D3R20 W_IMGLOC: DEFAULTS` | 2 |
| `proof_default_only_from_img` | the proof's DEFAULTS taken only in `W_IMG`, never from the LOCATE (R391-3's own edit) | `D3R19 past the bound: DEFAULTS`, `D3R19 inside the LOCATE: DEFAULTS`, `D3R20 W_IMGLOC: DEFAULTS` | 3 |
| `proof_past_bound_needs_fired` | `agg_past_w` is the fired level, so a proof on the bound's own clock reads on (R391-3's own edit) | `D3R20 W_IMG: DEFAULTS`, `D3R20 W_IMGLOC: DEFAULTS` | 2 |
| `agg_o_pulse` | `agg_o` a one-clock pulse on the expiry instead of a level (R391-3's own edit) | `D3R21: the binding walk fails whole` | 3 |
| `aecp_hold_unbounded` | the admission gate never drops (the unbounded hold) | `D3O5: in CLOSED each GET_RX_STATE`, `D3O6: during the slowed walk` | 7 |
| `held_drop_uncounted` | a held drop not counted | `D3O5: one AECP command held`, `D3O6: at the terminal` | 5 |
| `resident_never_returned` | the resident count never comes back down (R391-2's own edit) | `D3O7: the returned slot frees the share` | 1 |

### AECP deadline and hazard-class controls (lane C5a): `aecp_mutants.py`

`make aecp-mutants` (`python3 aecp_mutants.py --output DIR [--only a,b]`)
applies each reviewed patch in `mutations/` to a scratch copy of `hdl/`,
`tb/common/`, `tb/ucpu/` and this directory with `git apply`, runs one suite
target there, and counts the arm KILLED only when the simulation completed with
its tally, failed, and printed its named check. It reads logs only, never
production source. A positive control of every (suite, target) pair runs first
and must pass. Counts below were taken on 2026-09-30 with Verilator 5.052.

| Arm | Suite, target | What is broken | Failing checks |
|---|---|---|---|
| `dl-kill-tied-off` | pp_top `deadline` | the kill face tied off again, and the engine's `dl_kill_i` with it (the wiring before issue #81) | 16: every DL1 check (the stall answers its own SUCCESS after 33,505 clocks), DL3 (both answered for real, the MVU one SUCCESS), DL4 (no answer inside the wait), DL5 |
| `dl-released-before-queued` | pp_top `deadline` | `kill_resp_queued_i` tied 1, so the key is released at the expiry | 3: DL1 (the kill honoured 3,410 clocks before the forced response's hand-off, the hold free for them), DL5 (the dropped frame's kill honoured too) |
| `dl-armed-at-admission` | pp_top `deadline` | the deadline re-armed at every admission instead of read from the record | 2: DL3 (the queued GET_MILAN_INFO runs and answers SUCCESS, one redirect) |
| `dl-boot-hold-not-exempt` | pp_top `d3` | rule (d)'s exception removed: the command held through the boot keeps its stamped deadline | 1: D3O6 (the command held 150 ms is answered by the forced response, not byte-exact) |
| `dl-gdi-runs-on` | pp_top `deadline` | the GET_DYNAMIC_INFO voids removed | 4: DL4 (the batch keeps copying, no answer inside the wait), DL5 |
| `dl-edit-preempted` | pp_top `deadline` | the edit, registry and lock commands preempted like the rest | 6: DL6 (ENTITY_MISBEHAVING mid-validation, no record committed), DL10 x4 |
| `dl-registry-preempted` | pp_top `deadline` | REGISTER/DEREGISTER preempted like the rest | 2: DL10 (the queued REGISTER answers status 10 and never registers) |
| `dl-lock-preempted` | pp_top `deadline` | LOCK_ENTITY preempted like the rest | 2: DL10 (the queued LOCK_ENTITY answers status 10 and the lock is not held) |
| `dl-kill-ack-keeps-owner` | pp_top `deadline` | an honoured kill no longer ends the AECP owner | 2: DL1 (the RX-slot return releases the freed id again), DL11 (19 such releases in the section) |
| `dl-preempt-after-effect-top` | pp_top `deadline` | the µCPU redirects after an effect op | 2: DL2 (the SET_NAME that wrote its name answers ENTITY_MISBEHAVING) |
| `dl-mvu-forced-status-10` | pp_top `deadline` | the forced answer left at status 10 for every message type | 5: DL3 (status 10, which Milan Table 5.19 reserves), DL9 x4 |
| `dl-non-aem-forced-status-10` | pp_top `deadline` | the forced NOT_IMPLEMENTED answer for MVU alone (the round-1 rule) | 4: DL9, ADDRESS_ACCESS, AVC, HDCP_APM and EXTENDED each answered status 10 |
| `mvu-fault-status-10` | pp_top `deadline` | the MVU fault answer removed: a voided MVU response is rebuilt as status 10, header only | 4: DL8 under every fault (status 10, 60 bytes) |
| `mvu-echo-slot-std` | pp_top `deadline` | an MVU response's slot sized for its built answer only | 1: DL8 (the 578-byte echo clipped to the 576-byte standard slot) |
| `dl-preempt-after-effect` | ucpu `run` | the same µCPU patch, at the unit | 4: P19c (status 10 after the write, commit and mark, the notification lost), P19d |
| `ucpu-preempt-cuts-a-wait` | ucpu `run` | the redirect taken while an op waits on its face | 2: P19e (the waiting locate abandoned, no read answered) |
| `ucpu-preempt-keeps-the-body` | ucpu `run` | the cursor not returned to 12 | 1: P19f (length 36 with a partly built counters block) |
| `ucpu-preempt-repeats` | ucpu `run` | the redirect not limited to once per dispatch | 18: P19a to P19h (E_DLKILL redirected into itself, never sends) |
| `dlkill-always-misbehaving` | ucpu `run` | E_DLKILL overwrites a refusal already chosen | 1: P19d (ENTITY_LOCKED became status 10) |
| `mvu-silent` | pp_top `budget` | GET_MILAN_INFO retires without its SEND_RESPONSE | 12: TB1, TB3 and TB4, every GET_MILAN_INFO unanswered |
| `fanout-never-ends` | pp_top `budget` | the notification walk never ends its class, so the command-path hold never drops | 23: TB3 to TB5, nothing answered once the fan-out starts |
| `acmp-waits-for-aecp` | pp_top `budget` | READ_DESCRIPTOR classified CFG_BARRIER, so ACMP waits behind unrelated AECP work | 2: TB5 (GET_RX_STATE 13,144 clocks beside the READ_DESCRIPTOR, GET_TX_STATE 977 during the fan-out) |
| `hz-stub-restored` | pp_top `hazards` | the dispatch-ROM stub the classifier replaced (ACMP STREAM_CFG and the audio-map pair MAP_CFG, keyed by protocol; everything else RO) | 74: HZ1 (34 rows), HZ2 x3, HZ3 x2, HZ4 x3, HZ5, HZ6 and every keyed pair of HZ9 to HZ12 |
| `hz-setcfg-not-barrier` | pp_top `hazards` | SET_CONFIGURATION classified RO_SNAPSHOT | 7: HZ1, HZ2 x2, HZ3 x2, HZ11a x2 |
| `hz-setcfg-not-barrier-talker` | pp_top `hazards` | the same patch, named on the talker | the same 7; named HZ11a |
| `hz-lock-not-lockop` | pp_top `hazards` | LOCK_ENTITY classified RO_SNAPSHOT | 7: HZ1 x2, HZ4 x3, HZ11b x2 |
| `hz-lock-not-lockop-talker` | pp_top `hazards` | the same patch, named on the talker | the same 7; named HZ11b |
| `hz-stream-key-none` | pp_top `hazards` | AECP STREAM_CFG keyed by nothing | 12: HZ1 x4, HZ5 x2, HZ10a x2, HZ10b x2, HZ10c x2 |
| `hz-stream-key-none-vs-read`, `-talker-read`, `-talker-step` | pp_top `hazards` | the same patch, named on STREAM_CFG against an ACMP read, against the talker's read and against its step | the same 12; named HZ10a, HZ10b, HZ10c |
| `hz-reads-keyed-none` | pp_top `hazards` | the descriptor GETs keyed by nothing | 13: HZ1 x9, HZ6 x2, HZ10e x2 |
| `hz-reads-keyed-none-talker` | pp_top `hazards` | the same patch, named on the talker's step | the same 13; named HZ10e |
| `hz-talker-keyed-as-listener` | pp_top `hazards` | the talker's transactions keyed as STREAM_INPUT | 18: HZ1 x4, HZ9d x2, HZ10b x2, HZ10c x2, HZ10e x2, HZ12a x2, HZ12b x2, HZ12c x2 |
| `hz-clock-as-ro` | pp_top `hazards` | CLOCK_CFG classified RO_SNAPSHOT | 6: HZ1 x2, HZ12a x4 |
| `hz-clock-as-lock` | pp_top `hazards` | CLOCK_CFG classified LOCK_OP (over-serialized) | 3: HZ1 x2, HZ8 |
| `hz-clock-key-none`, `-talker` | pp_top `hazards` | CLOCK_CFG keyed by nothing | 6: HZ1 x2, HZ12a x4; named on STREAM_INPUT 1 and on the talker |
| `hz-name-as-ro` | pp_top `hazards` | NAME_WR classified RO_SNAPSHOT | 8: HZ1, HZ9a x2, HZ9c, HZ9d x2, HZ9f x2 |
| `hz-name-as-stream` | pp_top `hazards` | NAME_WR classified STREAM_CFG (over-serialized) | 2: HZ1, HZ9c (it waits for an UNBIND_RX of its sink) |
| `hz-name-key-none`, `-talker`, `-held` | pp_top `hazards` | NAME_WR keyed by nothing | 7: HZ1, HZ9a x2, HZ9d x2, HZ9f x2; named HZ9a, HZ9d, HZ9f |
| `hz-registry-as-ro` | pp_top `hazards` | REGISTRY_OP classified RO_SNAPSHOT | 2: HZ1 |
| `hz-identify-as-ro` | pp_top `hazards` | IDENTIFY classified RO_SNAPSHOT | 5: HZ1, HZ12b x4 |
| `hz-identify-key-none`, `-talker` | pp_top `hazards` | IDENTIFY keyed by nothing | 5: HZ1, HZ12b x4; named on STREAM_INPUT 1 and on the talker |
| `hz-map-as-ro`, `-talker` | pp_top `hazards` | MAP_CFG classified RO_SNAPSHOT, so the cross-lock is lost | 10: HZ1 x2, HZ7 x2, HZ11d x2, HZ12c x4; named HZ7 and HZ11d |
| `hz-map-key-none`, `-talker` | pp_top `hazards` | MAP_CFG keyed by nothing (the class-wide cross-lock still holds) | 6: HZ1 x2, HZ12c x4; named on STREAM_INPUT 1 and on the talker |
| `hz-acmp-reads-as-steps` | pp_top `hazards` | ACMP GET_RX/TX_STATE and GET_TX_CONNECTION classified STREAM_CFG | 23: HZ1 x3, HZ4, HZ6 (two reads), and every read of HZ9 to HZ12 |
| `hz-barrier-no-priority` | pp_top `hazards` | the pending barrier's priority removed from the round-robin | 127: HZ3, then every later arm (the admission port stays wedged) |
| `hz-foreign-target-classified` | pp_top `hazards` | a command for another entity_id classified by its opcode | 1: HZ1 (CFG_BARRIER for a frame the engine drops) |
| `hz-response-classified` | pp_top `hazards` | an AECP response arriving as input classified by its opcode | 1: HZ1 |

## Recorded seams and honest limits

- The validator's V9 pass-through has NO msrp/mvrp select — `KL_mrp_strip`
  derives it from the EtherType bytes it strips (V9 already enforced the
  DA/EtherType pairing).
- The validator's F03.4 `target_eid` for ACMP is the @4 stream_id; both
  ACMP engines discriminate on the ADDRESSED entity id — the top's steer
  prefetch rewrites the head from the slot bytes (doc conflict reported;
  03 §4 says only "as applicable").
- S6 allows the probe to RACE the trace record: a short bind-armed
  T-ACMP-DELAY draw can put the probe on the wire around the discovery
  walk, so the trace check polls a bounded 500 ms window instead of
  reading once.
- MSRP byte-exact checks run under `la_guard()`: the PRNG-drawn 10-15 s
  LeaveAll would otherwise fold the expected vector into an LA PDU. The
  MVRP byte-exact check runs early (before the first LeaveAll) because an
  idle MVRP participant latches an expired LeaveAll until its next tx
  opportunity — there is no clean later window by construction.
- The AECP pop face is still exposed and still tied `ready = 0` here: the
  AECP head is now drained by `KL_aecp_engine` INSIDE the top, and the port
  is an additional, optional consumer (see its banner).
- Scenario A and section D3 touch the descriptor memory. The image is
  loaded into the DRAM model BEFORE reset, exactly as software does before
  `restore_go_i` (and so before `entity_enable`); the "software has not
  loaded it" and "no bridge at all" arms live in the `desc_store` suite,
  which owns that face, and their effect on the restore (CLOSED, or a late
  load healed by the restore's LOCATE) is D3O2 to D3O4.
- The `A` expectations are byte builders from the IEEE §9.3.1 AECPDU and
  §7.2 descriptor field offsets plus the documented image layout — nothing in
  them comes from the DUT or from `gen_desc_image.py`'s output.
- The S10 allocator is a harness model of the 02 §4.2 op semantics only
  (accept a request, answer once, hand back an address). It proves the FACE
  and the address flow through this processor — never MAAP itself: the
  probe/defend/announce state machine of IEEE 1722 Annex B lives in the
  integrating fabric, outside this repo.
- The NVM device model is an erased flash (every byte 0xFF) that keeps what
  is written to it: the first boot reads blank, so a record failing the F07.8
  magic/layout check is SKIPPED by the shadow, which is the documented
  no-saved-binding path; section BW's resets read back what it committed.
  Torn-stream and device-error restore aborts, and the deadline at its
  boundary, are covered by the `acmp_nvm` suite; BW3 grades the deadline at the
  top once.
- The wrap exposes observe-only cross-module taps (`dbg_*`) used during
  bring-up; the checks themselves read only wire frames + the host face,
  except where a section names its tap: R waits on `dbg_walk_done_o`, and
  BW4 grades the top's restore pins against the admission gate's release and
  the listener's preload write and A4 arm (`dbg_lsn_released_o`,
  `dbg_lsn_preload_o`, `dbg_lsn_arm_o`). Section D3 grades the writer's
  ownership and verdicts (`dbg_d3_*`), the engine's command-in-flight level
  (`dbg_aecp_busy_o`) and the dispatch queue's head (`dbg_aecp_head_o`).

## Section B — the response buffer lives in main memory (03 §7.1)

The 592-byte AECP response buffer is no longer fabric state; it is
`KL_aecp_resp_buf` over the `resp_mem_*` master, and the model behind that
master injects **non-zero latency on both channels by default** (23 clocks
read, 17 write). B1 demands one read burst and exactly the lane writes the
write pattern implies; B2 compares the payload on the wire against the model's
own memory image, not against the DUT's account of it; B3 proves a byte whose
write strobe is 0 is never modified; B4/B4b measure the whole path — MAC
command byte 0 to MAC response byte 0 — and check it against the IEEE §9.2.1.1
100 ms budget (10,000,000 clocks at `P-CLK-HZ`), once at the suite's latency
and once at the reference SoC's measured ~1424 ns (143 clocks) per access;
B5 proves an echoed payload costs the response memory **nothing** (it comes
straight out of the RX slot); B6–B9 tie the master off, fail its writes and
fail its reads, and demand a well-formed 60-byte `ENTITY_MISBEHAVING` answer
plus the counters and the snapshot window that name the fault; B10 demands the
slot pools back afterwards; B11 proves a four-times-slower bridge only costs
time.

## Mutation-proven 2026-08-13 (scenario A)

| Break | Went red |
|---|---|
| `COPY_BUFFER` advances by the whole 8-byte lane instead of the residual | **10** red |
| response buffer places fields little-endian instead of big-endian | **8** red |
| unimplemented opcodes fall through to the READ_DESCRIPTOR µprogram | **1** red |
| the frame builder ignores whether the payload byte has arrived from memory yet | **58** red |

## Mutation-proven 2026-08-14 (A5b, response sizing)

| Break | Went red |
|---|---|
| `control_data_length` pinned at 16 (12 + 4) instead of 12 + payload | **23** red, **8** of them A5b |
| the echoed payload capped at 8 octets, with `control_data_length` following it down | **5** red, **all** A5b |

The second one is the result that justifies the block. It produces an
INTERNALLY CONSISTENT frame (the length field matches the bytes actually
emitted, and it still pads to 60), so every pre-existing check stays green:
before A5b the suite could not tell a response sized by its command from one
sized by something else, which is precisely what a controller complains about.
The first row also leaves A5 itself green, because 16 is the right answer for
the one 4-byte payload A5 sends.

## Mutation-proven 2026-08-14 (M, GET_MILAN_INFO)

| Break | Went red |
|---|---|
| `MILAN_PROTOCOL_VERSION` 1 -> 2 in `gen_ucode.py` | **3** red (M1, M2, M5b) |
| `MILAN_FEATURES_FLAGS` 0 -> 0x2, claiming a feature this build cannot serve | **3** red (M1, M2, M5b) |
| the protocol_id tail compare (@26..@27) dropped from the sub-decode | **1** red, M3 |
| the MVU `command_type` compare dropped from the sub-decode | **2** red, M4 and M5 |
| the Figure 5.3 length guard dropped (`pld_cmd_r >= 8` -> `>= 0`) | **1** red, M6 |

Every one of these produces a WELL-FORMED frame of the right length — the first
two are a correct Figure 5.4 response carrying a false claim, and the last three
answer SUCCESS to a command that was never GET_MILAN_INFO. None of them is
visible to a check that only counts bytes, which is why M2 decodes the three
fields by name and M3 uses a protocol id that differs from MVU's in its last
16 bits alone.

The `COPY_BUFFER` one is the interesting result: it goes red HERE and stays
green in `tb/ucpu` (0 checks red there), because that suite's µprogram only copies a
whole number of 8-byte lanes. A descriptor whose length is not a multiple of 8
is a thing only the end-to-end suite sees.

## October MVU waiver response mutation (2026-09-25)

Issues #55/#56/#77 use the waiver in
[06 §6.9](../../docs/architecture/06_aecp_engine.md#69-mvu-commands).
All counts in this section were re-measured on 2026-09-25 at merged head
`b51bc3893b06f4d39be49726c1b8f4ed6c65573d`, before the M4L lock regression
was added. At that head, the unmodified `make -C tb/pp_top run` passed
**7,660 checks**: 7,640 in the default build, including M1/M2 and all five
M4 cases, plus 20 in the domain-default fixture build. These are measurements
of that commit, not live suite totals; re-run the suite and both mutations
before quoting counts for any later head.

Each mutation changes only a disposable copy of generated `tb/pp_top/ucode.hex`.
Copy `ltn_rom.hex` alongside it, create an `obj_dir` for the tally, then run the
built `tb/pp_top/obj_dir/Vpp_top_sim` binary with the ROM-copy directory as its
working directory. The tracked generator and RTL stay unchanged. ROM indices below
are zero-based; the normal suite bank uses the unmodified source-checkout ROM.

| Generated-ROM change | Result |
|---|---|
| E_NOTIMPL, word 560: `c00000000001` → `c00000000000` (`SET_STATUS NOT_IMPLEMENTED` → `SET_STATUS SUCCESS`), with body and length untouched | exit 1; 197 of 7,640 default-build checks fail. M4 contributes 10: both the status and byte-exact echo assertion for each type 0x0001–0x0005. All five frame-length and cdl checks still pass |
| E_MVUINFO+5, word 741: `230000000000` → `230000000003` (`MOVE r6, 0` → `MOVE r6, 3`), asserting both Table 5.20 flags | exit 1; 3 of 7,640 default-build checks fail: M1, M2 features_flags, and M5b |

Both mutants were rejected. Each ran against a disposable generated-ROM copy;
the source checkout's original ROM remained byte-for-byte unchanged.

## Section K — GET_COUNTERS (06 §6.6; IEEE §7.4.42, Milan §5.4.2.25)

The harness plays the **integrator's counter store**, never the DUT's: it decides
what a quadlet means and which of them exist, and the suite then demands the
processor carry that answer onto the wire unchanged. Two masks on purpose —
`0x00000FFF` for an AAF sink that keeps the tv-bit tallies, Milan v1.2 Table
5.16's `0x00000F3F` for a CRF Media Clock Input that does not, and Milan Table
5.17's compact `0x0000001F` for a Stream Output. A processor that substituted a
mask of its own would therefore be caught. The store holds every beat for two
cycles by default, because a face that answers in the same cycle never exercises
the hold.

K1 demands the byte-exact 174-byte frame (Figure 7-67's block runs to byte 156,
so the AECPDU is 160 and `control_data_length` 148 — a short one is what Hive
4.3.1 reports as "Incorrect payload size"); K2 demands the Milan mandatory set
la_avdecc gates the badge on; K3 asks for STREAM_INPUT **1** and demands a
different object's answer, which is the whole point of reading `descriptor_index`
from @26 rather than @30, and demands zero bytes behind a clear mask bit; K4
demands SUCCESS with an EMPTY mask and a full-size block for ENTITY; K5 makes a
truncated command `BAD_ARGUMENTS`; K6 runs the same command at zero hold and at
an 11-cycle hold per quadlet and demands identical bytes; K7 wedges the store
outright and demands a bounded `ENTITY_MISBEHAVING` **and a working
READ_DESCRIPTOR immediately afterwards**; K8 demands the store be asked for the
mask and then quadlets 0..31 in order, consecutive repeats folded away — a
repeat under back-pressure is free, an index that MOVES under it is a lost beat.

| Break | Went red |
|---|---|
| `descriptor_index` read at the READ_DESCRIPTOR offset instead of §7.4.42.1's @26 | **3** red |
| the block stops after 12 quadlets instead of 32 | **9** red |
| a voided response is still sealed with its intended payload length | **2** red |
| the counters-face watchdog never fires | **5** red |
| `counters_valid` is a constant `0xFFF` instead of what the store returns | **6** red |

The last one is the one worth keeping: it is the advertised-zero lie in its
purest form — a full mask over a block the fabric never fills — and it must not
be able to pass.

## Section W8: GET_DYNAMIC_INFO

The suite sends `0x004B` through the complete MAC, RX slot, dispatch, AECP
engine, response memory, and TX path. Its expected bytes are built from the
standard's record layout and the harness models, not from standalone DUT
responses.

W8 covers implemented getters including full GET_NAME records in one byte-exact
aggregate, a missing
descriptor that changes only one record status, whole-command `BAD_ARGUMENTS`
for a forbidden `GET_AUDIO_MAP` with proof that no earlier record reached the
descriptor store, silent overflow omission followed by successful processing
of a later record, the Milan 56-byte `GET_STREAM_INFO` body, and
record-level `NOT_SUPPORTED` with exact command-data copy for a permitted but
unimplemented getter. It also covers an empty batch, truncated and
overrunning records, per-record `BAD_ARGUMENTS` for a non-SUCCESS command
status, preservation of the full 16-bit record command discriminator, every
member of the exact 13-command whitelist, retention at the exact cdl 524
response boundary, and rejection of an oversized cdl 525 command before record
processing. The batch-only falsifiers use distinct overflow targets,
non-zero unsupported data, a non-zero image configuration, full-body
wrong-target refusals, and sampling-rate image hits both before another record
and at the end of the aggregate. The final hit also verifies that the word
after its four-byte body remains untouched.

## Section U10: controller availability monitor

U10 verifies the full registered-controller liveness path through the real
timer, PRNG, frame builder, inflight tracker, TX slot pool, serializer, and RX
validator. It checks the independent 30 to 60 second draw, exact one-time retry,
targeted deregistration after silence, any-status response rearm, sequence
advancement across row reuse, and valid-command cancellation.

The cancellation case interrupts a CONTROLLER_AVAILABLE frame during allocation
or construction, then requires a solicited response to pass through the same TX
writer, the writer lock to return idle, and all five TX slots to be free. The
response-match cases inject both a colliding MAC fold and a correct MAC carrying
the wrong target Entity ID. Neither may suppress the exact retry.

The queue-delay case stalls a solicited response in the serializer, waits for a
CONTROLLER_AVAILABLE handle to queue behind it for longer than two response
budgets, and proves that no attempt timeout starts before serializer acceptance.
A valid command then cancels the queued exchange. The stale handle must drain,
both solicited responses must resume, and all five TX slots must return free.

U10 resets the DUT over an erased NVM device and starts both restore walks
before its first command (`H::boot_to_aecp`): the D3 writer holds AECP
dispatch from reset to its restore terminal.

## Section U11: non-head cancellation

U11 boots the same way after its reset.

U11 stalls the serializer until two independent controller probes occupy the
originator queue. It cancels the controller owning the second handle and
requires the queue to compact immediately, before the released physical slot
can be reused. Cleanup commands then prove that no queued or inflight exchange
survives and that all five shared TX slots return free.

## Section T: GET_TX_STATE against a registered Listener (issue #46)

Milan §5.5.4.3 sets REGISTERING_FAILED (0x0040) in a GET_TX_STATE_RESPONSE iff
the talker is registering a Listener Asking Failed attribute for the stream.
`acmp_talker` grades that flag against a code its own harness drives; this is
the one place the code comes from the SRP engine registering a real inbound
MRPDU, so it is the one place the talker and the engine can disagree about
what the code means. They did: the talker keyed on 3, which the engine
publishes for Ready Failed (`srp_pkg::srp_decl_e`, 02 F02.10).

T0 re-pings source 0 and requires its PROBE_TX answer and its Talker Advertise
back. T1 to T4 then register, by MRPDU exactly as W17b does, Asking Failed,
Ready Failed and Ready in turn: each arm checks the code the engine published
(snapshot word 13), grades the GET_TX_STATE_RESPONSE byte-exact and its flags
word alone (0x0040, 0, 0), and the Asking Failed arm asks source 1 too, which
must answer flags 0. T5 to T7 prove the flag is read live in both directions:
Asking Failed after Ready sets it again, and the Listener leaving clears it.

It runs on the main DUT after U11, not beside W17b, on purpose. S10's
PROBE_TX is the only ping source 0 gets, and W21 to W25 lean on that
T-SRP-DAFRESH window still being open: the same arms placed after W17b took
about 1.7 s of simulated time and moved the window's lapse into W21t2 and
W25pre, which then failed for a reason that has nothing to do with them. At
the end of the run two response fields have moved since S10, and neither is
read from the talker: the DA is the one the bench's own allocator model last
granted source 0 (`maap_src_da`), and the VID is the SR-class level on
snapshot word 10, because the S8 Domain registration has aged out under the
processor's own LeaveAll and the bench never re-declares it.

## Section W9i-W9m: SET_SAMPLING_RATE against the AUDIO_UNIT list (issue #51)

Milan §5.4.2.13 / IEEE §7.4.21.1 (06 §6.4): SET_SAMPLING_RATE accepts only a
rate the located AUDIO_UNIT's `sampling_rates` list holds. Any other rate is
`BAD_ARGUMENTS` carrying the CURRENT rate, with nothing stored, marked or
notified. Before the check, `E_SSRATE` stored any 32-bit rate, answered
`SUCCESS` and announced it. The fixture lists 48000 and 96000 and its
`current_sampling_rate` is 96000.

- **W9i** (before W9, on the unset row) refuses 44100 byte-exact carrying the
  image's 96000, and GET still reads 96000. This is the reproduction. **W9c**
  adds the GET_DYNAMIC_INFO member's read of W9's 48000 (W8q graded the
  member's image arm). **W9j** refuses 192000 on the set row carrying the
  stored 48000.
- **W9k** registers a second controller and refuses 44100, 0x2000BB80 (48000
  with pull 1: the list stores whole words, so a pulled rate is another rate)
  and 0 (what a lane past the 152-byte descriptor reads). Each refusal is
  graded at the dynamic store's write counter, the NVM-mark and notify
  strobes, and the second controller's queue. The accepted 96000 (list entry
  1) moves each counter once and sends one unsolicited SET_SAMPLING_RATE.
- **W9l** patches the image in place: count 1 refuses entry 1, a replaced
  entry is accepted and the rate it replaced refused, and offset 148 refuses
  everything (the walk reads the list at 144 only, 07 §3.1 L10).
- **W9m** proves the lock outranks the list check: a foreign controller is
  `ENTITY_LOCKED` for a listed and an unlisted rate alike and moves nothing.
  The body of that refusal is issue #53's decision and is not graded here.

W9k, W9l and W9m run LAST on the main DUT, after section T, for section T's
reason: their empty notification windows cost about 1.3 s of simulated time,
and placed beside W9 they moved source 0's T-SRP-DAFRESH lapse into W25pre
(5 FAIL, W25pre to W25b2, in the first placement). U10 and U11 reset the DUT,
so the phase deregisters both bench controllers and sets the listed 48000
before it starts.

## Section DV: the Domain default is the top's parameter (issue #95)

`KL_srp_top` has a 16-bit `DOM_DEF_VID_P` with its own default of 2, and the top
used to leave it unbound, so nothing outside the processor could drive the
Domain default. The top now declares `SRP_DOM_DEF_VID_P` (`P-SRP-DOM-DEF-VID`,
default 2) and binds it to the child explicitly. The Domain FSM reads it three
times: as its reset value, in the declaration on every LINK_UP, and in the
revert on LINK_DOWN (10 §6.1 F10.2).

A build that only runs the product value cannot see that binding. The child's
own default is also 2, so a dropped or misbound connection produces exactly the
frames the default build expects (M25: 0 failures there). The Makefile therefore
builds the bench a second time, and a third for section TB's timebase:

| Build | Override | Runs | Expects |
|---|---|---|---|
| `obj_dir/Vpp_top_sim` | none: the top's own default | every section, DV and DL among them | 2 (Milan §4.2.7.2.1) |
| `obj_vid/Vpp_top_vid` | `SRP_DOM_DEF_VID_P = 0x5A3C` (`SRP_VID_FIXTURE`) | DV alone | 0x5A3C |
| `obj_tim/Vpp_top_tim` | the wrap's timebase at the nominal clock's rate (`PP_TOP_TIM_REAL`: 1 ms = 1,000 clocks) | TB alone (`make budget`) | the product default |

The fixture is a verification value, not a product profile: Milan §4.2.7.2.1
fixes a shipping build at 2. It is chosen so that each plausible fault gives a
value of its own: a missing binding or a literal (2), an 8- or 12-bit truncation
(0x003C, 0x0A3C), a byte swap (0x3C5A), or routing it to the priority parameter
(priority 0x3C, read as 4 on the 3-bit face). Its low 12 bits are a legal VID,
0xA3C, and that is what the 12-bit faces carry: the class-D port, snapshot word
10, GET_DOMAIN and the ACMP talker. Only the MSRP wire field shows the top
nibble, which is why M27 fails the wire checks alone. The C++ expectation is
compiled from the same Makefile variable the wrap receives, never read back from
the DUT, and the wrap overrides nothing in the first build.

The fixture branch refuses compilation unless its 16-bit wire value and its
low 12-bit class-D value both differ from product default 2 (issue #97).
`SRP_VID_FIXTURE=0002` fails both assertions; `SRP_VID_FIXTURE=1002` fails the
class-D assertion even though its wire value differs. Each diagnostic names
the fixture, product default and affected width. These are verification-bench
assertions only. The default build has no fixture override and no distinctness
assertion.

`make` also runs `make fixture-guards`, a focused compile check of the actual
`sim_main.cpp` against generated model headers in a disposable directory. It
requires the no-override and pinned `5A3C` cases to compile, `0002` to fail with
both diagnostics and `1002` to fail with only the class-D diagnostic. An
unrelated compiler error fails the check. Run `make fixture-guards` alone for
this coverage without simulation; it does not change the sources or reuse
`obj_dir`/`obj_vid`. This check does not replace the two executable builds or
the missing-binding and child-default controls in the mutation record.

The compiler subprocess uses `LC_ALL=C` so diagnostic matching is independent
of the caller's language; all other environment inputs and compiler arguments
are preserved. `make fixture-guards-test` checks this isolation with mocked
subprocesses, without requiring non-English compiler catalogs, and is required
by `fixture-guards`. Removing the locale override makes this regression fail.

DV runs on a fresh model in both builds, after reset and with the link down:

- **DV1** the reset value on the class-D ports, snapshot word 10 and GET_DOMAIN;
  nothing is declared before the link.
- **DV2** LINK_UP: the first MSRP frame is `New {6, 3, default}` byte-exact, and
  the decoded SRclassVID is all 16 bits of the parameter.
- **DV3** GET_TX_STATE answers the default as `stream_vlan_id`. F05.11 leaves
  that field undefined while a source is not declaring; this talker answers the
  SR-class VID either way, as S10 and MP3 grade.
- **DV4** a bridge's certified two-class Domain `{5, 2, 5}` (NumberOfValues 2)
  is still adopted over the parameter: class-D `{3, 5}`, ADOPTED, one
  DOMAIN_CHANGE, and `Lv {6, 3, default}` + `New {6, 3, 5}` byte-exact.
- **DV5** LINK_DOWN restores the default and DEFAULTS with one DOMAIN_CHANGE,
  and nothing is declared for the 500 ms the link stays down.
- **DV6** LINK_UP declares `New {6, 3, default}` again, byte-exact.

Every edge is aligned into a clean slot of the 200 ms join cadence
(`sync_join`): the cadence timers are armed at reset, and a periodic re-join
drained into the same MRPDU would change the frame. The phase costs about 3.4 s
of simulated time on its own model, so the main DUT's timeline is untouched.
M25 to M31 in the mutation record are its evidence.

Issue #128 also updates MP3 to wait the acquisition bound before its first
probe, which must now succeed byte-exact with the internal claim's base address.
S10 retains a failed probe while the allocator is absent, then proves recovery
without another probe. The standalone talker retry suite carries the detailed
pacing, fairness, late-response and block-change mutation matrix.
