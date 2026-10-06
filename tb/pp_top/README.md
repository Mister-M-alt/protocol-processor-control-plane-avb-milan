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

`make`: exit 0 = PASS. It builds the bench seven times (sections DV, ID, AX, TB,
TD and IF): each executable prints its own build's tally, and the last line sums
the seven into the one canonical tally.

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
  ends COMPLETE with exactly 9 applied, 0 refused and 50 blank of 59 (the 27
  dynamic-state records and the 32 name records of the top's
  `DESC_NAME_ENTRIES_P` default); each
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
  SET persists. Over an erased device, each grant spaced so pass 0's 59
  reads end before the bound (1,000,001 / (8 + 59 + 29) clocks apart, inside
  the per-wait deadline), the bound falls in pass 1, which rolls back to
  DEFAULTS within one per-wait deadline of it. Without the
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
  **D3C** (issue #141, milan-fpga #629) the clock-source selection over a
  ten-source domain, the 8x8 shape's INTERNAL 0, CRF 1 and AAF input k at
  2 + k (07 §3.1 L6), on a fresh model over the suite's image re-packed with a
  ten-source identity list. The suite's own list has three sources; neither
  image carries a CLOCK_SOURCE descriptor, because neither path reads one.
  **D3C1** SET_CLOCK_SOURCE(9), AAF input 7, answers SUCCESS byte-exact
  carrying 9; a registered second controller receives exactly one unsolicited
  SET_CLOCK_SOURCE carrying 9, sequence 0; the store write, NVM_MARK and
  NOTIFY_ENQ strobes each move once; and GET_CLOCK_SOURCE, the row and the
  top's exported `aecp_clk_src_index_o` read 9. **D3C2** SET_CLOCK_SOURCE(10),
  the count itself, answers BAD_ARGUMENTS byte-exact carrying the 9 in force
  (IEEE 1722.1-2021 §7.2.32, Table 7-141, §7.4.23.1). Nothing is written,
  marked, enqueued or announced; GET, the row and the export still read 9;
  and nothing is pending and the device sees no operation in the two windows
  after it. **D3C3** the D3S1/D3R1 pair for an AAF index (REQ-AEM-013): the
  accepted 9 is saved as exactly one ERASE and one WRITE of record 0x0A,
  byte-exact, with no other record moving. Across a power cycle the walk
  starts from cleared rows and applies it (1 applied, 0 refused, 58 blank of
  59), and the row, GET and the export read 9. **D3C4** the same saved record
  over an image whose list is shorter, at its count (nine sources) and above
  it (the suite's three): refused, COMPLETE with 0 applied and 1 refused, the
  row unset and GET reading the image's 0. The arms run in the order D3C1,
  D3C3's save, D3C2, D3C3's restore, D3C4, so the refusal is graded against
  a row already saved. Their negative controls (the SET bound fixed at three
  or made inclusive, the row narrowed to two bits, the restore rule's count or
  index narrowed to three bits, the restore bound made inclusive) each fail
  their named check: the two SET arms from `aecp_dispatch_mutants.py`, the
  rest from `d3_mutants.py`. Run in the whole default build, the SET bound
  fixed at three fails D3C's eleven checks and nothing else, and the row
  narrowed to two bits D3C's ten: no other check sets an index or reads a
  count past the suite's three. **D3C5** (issue #52) a record 0x0A that
  cannot be restored keeps the image's index 0, over the suite's three-source
  image: erased, corrupt (index 2 with a crc that is not the crc of its bytes),
  and read torn (the device ends pass 0's payload READ after one byte). The
  first two end COMPLETE, with 0 and 1 records refused. The
  torn read ends DEFAULTS, cause 1. In each the row stays unset, and
  GET_CLOCK_SOURCE and `aecp_clk_src_index_o` read 0. **D3C6** (issue #52)
  the saved index is in force before the entity is enabled. With
  `entity_enable_i` requested from reset and record 0x0A carrying 2, the top
  exports 2 with the row valid in every cycle the ADP engine's enable is high,
  the first included, and GET_CLOCK_SOURCE then reads 2.
  **D3N** (issues #61 and #83; REQ-PER-001, REQ-AEM-011; Milan v1.2 §5.3.13)
  the user names, one record per name-table entry, `0x80` + ordinal, the
  64-byte entry verbatim. The suite's image names 11 of the store's 32
  ordinals. Five of them are set with real SET_NAMEs: both ENTITY names (the
  group name a full 64 bytes with no NUL), CLOCK_DOMAIN 0's set to the EMPTY
  name, the IDENTIFY CONTROL's name and the last ordinal, 10. **D3N1** each is
  saved as exactly one ERASE and one WRITE of its record after the debounce,
  byte-exact, with no other record moving and nothing left unflushed.
  **D3N2** a SET_NAME naming what the entry holds writes no lane: no
  `aecp_name_wr_o` pulse, nothing pending and no device operation for two
  windows. **D3N3** across a power cycle every saved entry holds the image's
  name on the clock the D3 walk proves the image (the store's walk at reset
  may still be running at the admission gate's release, which is why the
  proof waits for it). The walk ends COMPLETE with exactly the five applied;
  the entries and GET_NAME read each byte-exact, the empty and the full name
  included, and READ_DESCRIPTOR serves both ENTITY names. The restore's name
  writes pulse no `aecp_name_wr_o`, and none becomes a change: nothing
  pending and no device write for two windows. **D3N4** SET_NAME's rule and
  the frame: a framed record for ordinal 20, past the image's names, is
  refused by the rule (the store's region 0xA count). Ordinal 3's corrupt
  crc and ordinal 4's 8-byte payload are refused by the frame, ordinal 5's
  applies, and GET_NAME reads the image's names for 3 and 4. **D3N5** a pass-1
  abort after a name was applied (ordinal 10's record erased between the
  passes, cause 5) rolls the descriptor store back, and the entry, GET_NAME
  and READ_DESCRIPTOR carry the image's entity_name. **D3N6** a SET_NAME
  while the device holds the record's WRITE request taints that WRITE: two
  WRITEs, the first carrying the latched name, the record ending with the
  second. **D3N7** an image loaded after the store's boot walk (D3O4's order)
  is walked at the writer's LOCATE before any record is read, so the
  restored name is not overwritten: GET_NAME and READ_DESCRIPTOR read it.
  **D3K** (issues #61 and #83; 09 §3 NVM, "every record type cut ≥ once")
  every D3 record type is cut by a real `rst_n` with the device carried:
  configuration, sampling rate, clock source, both stream formats,
  presentation offset and user name. The device model keeps each WRITE byte
  as it takes it, so a cut leaves a real torn record. The device holds A,
  which the first boot restores; a real SET of B starts the ERASE and WRITE.
  The cut falls on the ERASE's grant, on the WRITE's grant, after the
  8-byte header, one byte short of the record, and at a byte drawn from the
  fixed seed `0xD3C0FFEE`. The boot after it never fails. A cut before the
  ERASE completed keeps A. An erased record or a torn header reads UNFRAMED
  (blank), and a torn payload fails its crc16 (refused): each keeps the
  image's value with the valid flag clear. The device also holds sink 0's
  binding at the cut, and that boot restores it, probing PASSIVE
  (PRB_W_AVAIL). Once UNBIND_RX frees the sink, a later SET of B persists
  over whatever the cut left. The binding manager's own `rst_n` cuts are
  `tb/acmp_nvm` R1 and R2, and the port's are `tb/nvm_port` T25. These are
  the fixed cuts; section D3KR (below) is the seeded-random campaign.
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
- **D3V: the volatile set across a power cycle** (issues #59 and #62;
  REQ-NOT-005, REQ-PER-002; Milan v1.2 §5.3.4.1, §5.3.4.2, §5.3.12). This is a
  fresh model of its own, run by `--volatile-only` (`make volatile`) and in the
  default run. Its watch outlasts the controller monitor, so it stays out of
  `--d3-only`. **D3V1** (premise) the device holds sink 0's binding record and
  the first boot restores it. One controller registers, a second registers
  TIME_LIMITED (IEEE 1722.1-2021 §7.4.37.2), and a third controller's change
  notifies each of them at sequence_id 0. The first controller locks the
  entity (the second is refused ENTITY_LOCKED) and sets IDENTIFY to 255. All of
  this happens inside the wrap's 400 ms TIME_LIMITED and lock windows. A power
  cycle follows: `rst_n` with the device carried, then both walks from
  `restore_go_i`. **D3V2** the binding preload still arrives: sink 0 is bound
  and GET_RX_STATE answers the saved talker. **D3V3** IDENTIFY reads 0 in every
  cycle from `restore_go_i` on, and GET_CONTROL reads 0. **D3V4**
  `aecp_lock_held_o` is 0 in every cycle from `restore_go_i` on. **D3V5** a
  third controller's change notifies neither former controller. **D3V6** for
  66,000 ms after it (the monitor's longest 60 s draw, with U10's margin) no
  frame of any kind reaches either former controller: no CONTROLLER_AVAILABLE,
  no TIME_LIMITED expiry DEREGISTER and no notification. **D3V7** LOCK_ENTITY
  from the second controller answers SUCCESS and takes the lock, and its
  UNLOCK frees it. **D3V8** sixteen controllers new to the entity all register
  (Milan §5.3.4.2's sixteen). **D3V9** one of them deregisters and the first
  former controller registers again. Its first notification carries
  sequence_id 0, byte-exact (Milan §5.4.2.21: zero when a new entry is
  created), not the 1 its row would have reached before the cycle. The
  negative controls delete the registry's valid bits, the lock and IDENTIFY
  from their reset branches (`KL_aecp_notify.sv` `valid_r`, `lk_held_r`;
  `KL_aecp_dyn_state.sv` `ident_r`), from `d3_mutants.py` (mutation record
  below).
- **D3KR: the reset cut as a standing seeded-random campaign** (issue #83
  acceptance 3 and the manager's ruling on it, #83 comment 5967611704; 09 §3
  NVM, "cut at randomized commit points"). This is a fresh model of its own,
  run by `--cuts-only` (`make cuts`) and in the default run, and kept out of
  `--d3-only` so that no D3 control's run pays for it. Every record type both
  producers write is cut: D3K's seven and the binding manager's sink record
  `0x20`. A calibration commit of each type first writes B whole and measures
  the clocks from its ERASE's grant to its WRITE's done (cfg 15, rate 17, clks
  15, fmti 21, fmto 21, ptof 17, name 77, bind 33). Then for each of 32 standing
  seeds (`0xD3C0FFEE` + k × `0x9E3779B9`) and each type: the device holds A, the
  first boot restores it, and a live change to B starts the record's commit (D3K's
  SET; for the binding a BIND_RX of sink 0 to another talker, which a sink in
  PRB_W_AVAIL saves, Milan v1.2 §5.5.3.5.6). `rst_n` falls with the device
  carried at a clock drawn from the seed and the record id (xorshift32), from
  the ERASE's grant to two clocks past the WRITE's done. The outcome is read
  from the bytes the device holds at the cut, never from the RTL: A whole comes
  back, B whole comes back, and any other bytes (erased, a torn header, a torn
  payload) must frame no record, the oracle's own premise, and keep the default:
  the image's value, or an unbound sink. Per cut: the premise (A restored, B
  accepted, the ERASE granted); the restore never fails and the group holds the
  oracle's value; for a D3 type, sink 0's saved binding is restored and probes
  PASSIVE (PRB_W_AVAIL), as D3K's; and a later change to B persists over whatever
  the cut left (for a D3 type after UNBIND_RX frees the sink). Every check names
  its seed and its cut clock, and `./obj_dir/Vpp_top_sim --cut-seed S` reruns one
  seed alone. The 32 seeds reach, per type, A whole / erased / a torn header / a
  torn payload / B whole: cfg 3/7/13/1/8, rate 5/3/8/8/8, clks 5/5/9/5/8, fmti
  5/2/11/8/6, fmto 4/3/9/9/7, ptof 3/4/12/4/9, name 0/2/2/27/1, bind 3/2/7/18/2
  (the name's A-whole window is two of 77 clocks; D3K's ERASE cut covers it).
  1,000 checks, about 83 s. The negative controls (mutation record below)
  compare no crc in the binding manager's walk or in the D3 writer's frame, so
  a torn record is restored instead of the default.
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
  `python3 tb/pp_top/gsi_mutants.py --output <log-directory> [--jobs N] [--only NAME ...]`.
  The runner builds every variant in a temporary source copy of its own,
  requires a clean golden run first, and accepts only a completed simulation
  failing its named check. Compile failures do not count. It requires another
  clean run, of an unmutated copy, at the end. `--jobs N` (default 4, the
  meaning and default of `d3_mutants.py`) builds up to N copies at once after
  the golden, and the results are printed in the table's order whatever order
  they finish in. Measured 2026-10-02 at `85da751` with Verilator 5.050, each
  run pinned to 4 of the host's 16 CPUs: `--jobs 1` took 1,108 s and `--jobs 8`
  713 s, and the golden, the 20 variants and the restored run gave the same
  verdict and the same failing checks in both.

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
    over payloads of 0, 4, 8, 12, 16 and 72 octets, two opcodes Table 7-140
    leaves unassigned, and (issue #74, REQ-FWX-001) the six the row names as
    outer AEM commands at their own IEEE 1722.1-2021 command lengths: REBOOT
    (0x002A, 4), START_OPERATION (0x0037, 12), ABORT_OPERATION (0x0038, 8),
    OPERATION_STATUS sent as a command (0x0039, 8), SET_MEMORY_OBJECT_LENGTH
    (0x0047, 12) and GET_MEMORY_OBJECT_LENGTH (0x0048, 4, a GET_DYNAMIC_INFO
    whitelist member and still no outer command). `control_data_length` is read off the wire (not
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
  - **M9** (issues #83 and #76) every opcode the engine decodes, sent on every
    message type whose @22..@23 is not a command_type (2, 4, 6, 8, 10, 12, 14),
    is answered NOT_IMPLEMENTED with the command echoed whole: the `aem_w`
    message-type guard of each dispatch arm. The list `kOpcodes` is held to
    exactly the engine's `OP_*_C` set by `scripts/check_m9_opcodes.py`, which
    `run_suites.sh` runs (with its self-test) before any suite; issue #76
    found it at 23 of 30. Arms that re-dispatch on payload content get real
    bodies (GET_AUDIO_MAP and the ADD/REMOVE pair a STREAM_PORT_INPUT), and
    the three writers that joined with #76 (SET_STREAM_FORMAT, SET_STREAM_INFO,
    SET_NAME) are sent once more with a whole command body, so an unguarded arm
    reaches its write: **M9b3-M9b5** read STREAM_OUTPUT 1's published format
    and presentation-offset rows and CLOCK_DOMAIN 0's name back unmoved, as
    **M9b/M9b2** do the sampling rate. `make aecp-dispatch` runs A5b and M9
    alone on a booted model (the main run holds both in its own timeline), then
    section AX.
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
  grades the scalar and name records; maps are the integrator's, 07 §5.1).
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
  1. AD8 (issue #63): record 0x00 carries configuration 0 with a crc that is not
  the crc of its bytes. The frame refuses it, the walk ends COMPLETE with one
  record refused and the row unset, and all three views carry the image default
  1. AD9 (issue #63): the same record uncorrupted, but the device ends pass 0's
  payload READ of it after one byte. The walk fails whole, cause 1, nothing
  applied, and all three views carry 1. `make adp-config` runs
  this section alone; the default run includes it. The mutation record is
  `tb/adp_engine`'s campaign (`make -C tb/adp_engine mutants`), which runs this
  section against each patch. AD7 to AD9's restore controls are D3 writer
  defects, so `d3_mutants.py` plants them and runs this section (`--adp-only`).
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
  processor of its own in the fifth build, whose timebase is the nominal clock's
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
  `make budget` runs this build alone; `make` runs all six.
- **TD** **T-LOCK-UNLOCK and T-NOTIF-TIMELIMITED at the top's own defaults**
  (issue #81 acceptance 4; IEEE 1722.1-2021 §7.4.2 and §7.4.37.2, Milan v1.2
  §5.4.2.2), on a fresh processor of its own in the sixth build. Every other
  build overrides both timeouts to 400 ms in the wrap, so U5 and L6d grade the
  mechanism, never the 60,000 and 300,000 ms defaults. The sixth build drops the
  two overrides and keeps the first build's prescaler (1 ms = 100 clocks), so
  the real counts elapse: 30 million clocks for the registration. A controller
  registers TIME_LIMITED and another locks the entity; the registered one
  answers every CONTROLLER_AVAILABLE the monitor sends it (Milan §5.4.5.3), so
  only the timer under test can end its registration. **TD1** the auto-unlock
  notification (LOCK_ENTITY, u = 1, locked_id 0, byte-exact but for the entry's
  sequence_id) reaches the registered controller no sooner than 60,000 ms after
  the LOCK_ENTITY was fed and at most 20 ms later. **TD2** the expiry
  DEREGISTER (u = 1, byte-exact likewise) reaches it no sooner than 300,000 ms
  after the REGISTER and at most 20 ms later. The lower bound is exact, since
  the timer is armed from the ms its program runs in; the 20 ms covers the run
  to the arm, the sweep and the notification's build and serialization. The
  section prints both elapsed times. `make timer-defaults` runs this build
  alone.
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
  class and key; GET_DYNAMIC_INFO presents MAP_CFG with the no-descriptor key. An ACMP transaction is held in flight by stalling the MAC until
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
  reachable conflict at this top. The one accepted over-serialization: a
  GET_DYNAMIC_INFO naming no stream (one GET_CONFIGURATION record) waits for the
  held stream step, because the batch takes MAP_CFG's class-wide cross-lock.
  That check runs after HZ12, so every earlier arm keeps its clock. **HZ9** (NAME_WR) a SET_NAME on STREAM_INPUT 1
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
  hold back a GET_TX_STATE of source 1. **HZ13** (R419-2 F5) a
  GET_DYNAMIC_INFO carrying a GET_STREAM_INFO record of STREAM_INPUT 1 waits
  for a held UNBIND_RX of sink 1, as HZ6's stand-alone GET_STREAM_INFO does;
  held itself, it holds back an UNBIND_RX of sink 1; and it runs beside a held
  GET_RX_STATE of sink 1, two reads. `make hazards` runs this section alone;
  the default run includes it.
- **AQ: the timer arm-port queues against a model of their contract (issue
  #639).** At every clock edge of the main harness, over every section the full
  default run drives on it, the arm port and the drop counter equal an
  independent eight-FIFO model fed from the engine faces. Then a drive from the
  bench forces the faces through every state the rings add over a one-deep
  queue (full queues, drops, a saturated counter, resets with arms queued),
  under the same model.
  [Section AQ](#section-aq-the-timer-arm-port-queues-issue-639) has the rule,
  the drive, the coverage and the controls.

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
`--d3-only` (the controls of AD7 to AD9 run `--adp-only`, those of D3V
`--volatile-only` and those of D3KR `--cuts-only`, each with a golden of its own), and counts the mutant KILLED only when the run completes with its
tally, exits non-zero and every named check fails; a golden extract runs first and
must pass. The same driver runs the binding manager's three DR2c controls, the
arbiter's issue-cycle control and its seven own-contract controls (N11) in `tb/acmp_nvm`
and the validator's admission control in `tb/rx_validator` (their READMEs record
them). At the head of lane P1 (issues #52, #59, #61, #62, #63, #83) all 110 are
KILLED and the six goldens PASS (`--d3-only`, `--adp-only`, `--volatile-only`,
`--cuts-only`, `tb/acmp_nvm`, `tb/rx_validator`); the last column is how many
checks each one failed there. The two `cut_` controls run the seeded-random cut
campaign (section D3KR): every check they fail names its seed, and a torn
configuration, rate, clock-source or format record stays refused by its value
rule, so their named checks are the binding's, the offset's and the name's. The
counts the next paragraph quotes are those lanes' own: since the
name stage and the cut section (D3N, D3K) every walk reads 59 records, so a control
that breaks the walk or a frame fails more checks than it did then. Since the AECP
deadline kill (issue #81, section DL), `hold_released_at_go` and
`dispatch_not_held` each fail D3O6 as well (17 and 6). Both let the held
command run during the slowed restore, and it is no longer exempt from its
deadline: rule (d) exempts only a command held until the terminal. With the
kill tied off, the counts are 16 and 5 again. Since the clock-source arms
(issue #141, section D3C), five controls fail D3C checks as well:
`TRG_clks` (10; 5 before) D3C3's save, its two restore checks and D3C4's two
walks, which then find no record; `RPL_clks` (5; 3 before) D3C3's two restore
checks; `rule_ignored` (7; 3 before) D3C4's four; `unframed_reads_as_device_error`
(42; 38 before) D3C3's two restore checks and D3C4's two walks; and
`done_without_d3` (45; 42 before) the walk check of D3C3's restore and of each
D3C4 arm. The last four rows are D3C's own controls.

| Mutant | Defect planted | Named checks, each failing | Failing checks |
|---|---|---|---|
| `hold_released_at_go` | the writer's ownership ends at the walk's go instead of its terminal | `D3O1: released at` | 19 |
| `dispatch_not_held` | the engine's three dispatch gates ignore the writer's ownership | `D3O1: without the walk the writer owns every cycle` | 6 |
| `own_taken_at_the_walk` | ownership and the bus taken only once the walk starts, not from reset | `D3R9: the held SET` | 12 |
| `image_unproven_continues` | an unprovable image (the LOCATE's error) no longer aborts | `D3O2: CLOSED at`, `D3O3: CLOSED` | 9 |
| `latch_ignores_program` | the service latch does not wait for a running program | `D3S9` | 3 |
| `TRG_cfg` | configuration trigger deleted | `D3S1 cfg` | 17 |
| `TRG_rate` | sampling-rate trigger deleted | `D3S1 rate` | 17 |
| `TRG_clks` | clock-source trigger deleted | `D3S1 clks` | 24 |
| `TRG_fmti` | input-format trigger deleted | `D3S1 fmti` | 18 |
| `TRG_fmto` | output-format trigger deleted | `D3S1 fmto` | 18 |
| `TRG_ptof` | presentation-offset trigger deleted | `D3S1 ptof` | 38 |
| `taint_ignored` | a change after the latch no longer taints the write | `D3S4 taint` | 2 |
| `clear_wins_same_edge` | the done's clear outranks a change on the same edge | `D3S5 same edge` | 1 |
| `clear_by_group` | the done clears every record of the group | `D3S6 group` | 18 |
| `clear_by_index` | the done clears every record of the same index | `D3S6 index` | 14 |
| `identify_is_a_change` | IDENTIFY (selector 7) made a persisted change | `D3S7` | 2 |
| `unchanged_compare_ignores_validity` | the change qualifier ignores the valid flag | `D3S8 validity` | 5 |
| `RPL_cfg` | configuration replay deleted | `D3R1 cfg` | 12 |
| `RPL_rate` | sampling-rate replay deleted | `D3R1 rate` | 12 |
| `RPL_clks` | clock-source replay deleted | `D3R1 clks` | 12 |
| `RPL_fmti` | input-format replay deleted | `D3R1 fmti` | 8 |
| `RPL_fmto` | output-format replay deleted | `D3R1 fmto` | 8 |
| `RPL_ptof` | presentation-offset replay deleted | `D3R1 ptof` | 12 |
| `rule_ignored` | a SET-rule refusal applied anyway | `D3R2: COMPLETE` | 8 |
| `passes_may_disagree` | the pass agreement removed | `D3R4:`, `D3R4b` | 5 |
| `device_error_reads_as_blank` | a DEVICE error read as a blank record | `D3R5 device error on the header`, `D3R6: the one saved record` | 5 |
| `unframed_reads_as_device_error` | an UNFRAMED record read as a device error | `D3R6: an erased device restores blank` | 132 |
| `desc_error_is_a_refusal` | a rule's descriptor error read as a refusal | `D3R7` | 6 |
| `no_restore_watchdog` | the per-wait deadline removed | `D3R8: a READ granted`, `D3R8b` | 7 |
| `restore_writes_are_changes` | the snoop taps the shared bus, so restore writes are changes | `D3R1: no restore write is a change` | 1 |
| `enable_not_released_by_restore` | ADP enabled by the request alone | `D3R1: the enable requested from reset`, `D3C6` | 5 |
| `done_without_d3` | restore done without the D3 walk | `D3R1: the enable requested from reset`, `D3R1: COMPLETE` | 124 |
| `blank_ignores_d3` | restore blank ignores the D3 walk | `D3R1: COMPLETE` | 2 |
| `store_not_cleared` | the sampling-rate row and its valid flag not reset | `D3R1: every row at its reset value` | 25 |
| `valid_not_cleared` | the sampling-rate valid flag not reset | `D3R1: every row at its reset value` | 21 |
| `quarantine_released_by_time` | the arbiter ends a drain after 1,000 cycles | `D3R5: once the device ends the drained read a later SET persists` | 10 |
| `no_rollback` | a pass-1 abort ends DEFAULTS without the roll-back | `D3R4:` | 19 |
| `dyn_not_rolled_back` | the dynamic-state store left out of the roll-back | `D3R4:` | 4 |
| `store_not_rolled_back` | the descriptor store left out of the roll-back | `D3R10 5000` | 4 |
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
| `clks_row_two_bits` | the clock-source row stores only the index's low two bits (issue #141) | `D3C1 readback`, `D3C3 save` | 10 |
| `clks_restore_count_narrowed` | the restore rule reads only the low three bits of `clock_sources_count` | `D3C3 restore` | 2 |
| `clks_restore_index_narrowed` | the restore rule compares only the low three bits of the saved index | `D3C4 at the count`, `D3C4 above the count` | 4 |
| `clks_restore_bound_inclusive` | the restore rule accepts an index equal to the count (`<=` for `<`) | `D3C4 at the count` | 4 |
| `TRG_name` | the name trigger deleted (issues #61, #83) | `D3N1 ordinal` | 27 |
| `RPL_name` | the name replay deleted: every framed name refused | `D3N3 ordinal` | 17 |
| `name_rule_ignored` | the name rule accepts any ordinal | `D3N4: COMPLETE` | 1 |
| `name_empty_refused` | an EMPTY name (lane 0 zero) refused on restore | `D3N3: COMPLETE`, `D3N3 ordinal 2` | 2 |
| `name_record_id_shifted` | name records at `0x81` + ordinal | `D3N1 ordinal` | 25 |
| `name_entry_shifted` | a restored name written to the next entry | `D3N3 ordinal` | 14 |
| `name_lanes_partial` | the write-back ends after seven lanes | `D3N3 ordinal 1` | 2 |
| `name_taint_ignored` | a name change after its latch does not taint the write | `D3N6 taint` | 1 |
| `name_restore_pulses` | the exported `aecp_name_wr_o` not gated off the writer's restore | `D3N3: the restore's name writes` | 1 |
| `name_restore_is_a_change` | the name snoop not gated off the writer's restore | `D3N3: the restore's name writes` | 1 |
| `names_before_the_image` | the image proof skipped, so names land before the store's walk | `D3N7` | 18 |
| `frame_crc_ignored` | the frame's crc compare removed for every group (issues #61, #83) | `D3K ptof cut at byte 11 of 12: the restore`, `D3K name cut at byte 71 of 72: the restore` | 8 |
| `clks_crc_ignored` | the frame's crc compare bypassed for the clock-source group (issue #52) | `D3C5 corrupt` | 1 |
| `torn_read_not_an_abort` | a torn read is not an abort (issues #52, #63) | `D3C5 torn` | 2 |
| `blank_applies_zero` | a blank record applies a zero value with its valid flag (issues #52, #63) | `D3C5 blank` | 26 |
| `cfg_crc_ignored` | the frame's crc compare bypassed for the configuration record; run `--adp-only` (issue #63) | `AD8: the first ENTITY_AVAILABLE` | 4 |
| `torn_read_not_an_abort_cfg` | `torn_read_not_an_abort`, run `--adp-only` | `AD9: the torn read` | 1 |
| `blank_applies_zero_cfg` | `blank_applies_zero`, run `--adp-only` | `AD7: the first ENTITY_AVAILABLE` | 7 |
| `registry_survives_reset` | `valid_r <= '0` deleted from `KL_aecp_notify`'s reset branch; run `--volatile-only` (issue #59) | `D3V5`, `D3V8`, `D3V9` | 4 |
| `lock_survives_reset` | `lk_held_r <= 1'b0` deleted from the same reset branch; run `--volatile-only` (issue #62) | `D3V4`, `D3V7` | 4 |
| `identify_survives_reset` | `ident_r` deleted from `KL_aecp_dyn_state`'s reset branch; run `--volatile-only` (issue #62) | `D3V3` | 1 |
| `cut_binding_crc_ignored` | the binding manager's crc compare removed from its record check, so a torn binding record is restored; run `--cuts-only` (issue #83, section D3KR) | `D3KR bind seed` (every seed whose cut left a torn binding record) | 20 |
| `cut_frame_crc_ignored` | `frame_crc_ignored`, run `--cuts-only`: a torn D3 record the value rule would take is restored (issue #83, section D3KR) | `D3KR ptof seed`, `D3KR name seed` | 30 |

### AECP deadline and hazard-class controls (lane C5a): `aecp_mutants.py`

`make aecp-mutants` (`python3 aecp_mutants.py --output DIR [--only a,b] [--jobs N]`)
applies each reviewed patch in `mutations/` to a scratch copy of `hdl/`,
`tb/common/`, `tb/ucpu/` and this directory, one copy per arm, with `git apply`,
runs one suite target there, and counts the arm KILLED only when the simulation
completed with its tally, failed, and printed its named check. It reads logs
only, never production source. A positive control of every (suite, target) pair
runs first, each in its own copy, and must pass. `--jobs N` (default 4, the
meaning and default of `d3_mutants.py`; the make target runs the default) builds
and runs up to N copies at once, and the results are printed in the table's
order whatever order they finish in. Counts below were taken on 2026-10-01 with
Verilator 5.050, the CI pin: 5 controls PASS and 55 arms KILLED. Measured
2026-10-02 at `85da751` with Verilator 5.050, each run pinned to 4 of the host's
16 CPUs: `--jobs 1` took 1,366 s and `--jobs 8` 1,079 s, and every control and
arm gave the same verdict and the same failing checks in both, the counts below.
Re-run 2026-10-03 at the head of lane P1 (issues #61, #83), the 27 `hazards` arms
only, because that lane made HZ9 let its name saves drain: the control PASS and all
27 KILLED. Three rows carry that run's counts, because under them a misplaced HZ9
SET_NAME's save holds dispatch behind HZ11's held LOCK_ENTITY.
Re-run in full 2026-10-04 at the head of the #81/#84 closeout lane, `--jobs 3`:
6 controls PASS (`timer-defaults` is the sixth) and 61 arms KILLED. The six arms
from `td-lock-default-59s` to `hz-gdi-as-barrier` are that lane's. Three counts
moved, each only by the checks it added: `hz-stub-restored` (HZ1's
GET_DYNAMIC_INFO row now wants MAP_CFG, and HZ8's batch, HZ13a and HZ13c are
new), `hz-acmp-reads-as-steps` (HZ13b) and `hz-barrier-no-priority` (twelve new
HZ checks behind the wedge). Every other arm failed the same checks as at the
lane's base, line for line.

| Arm | Suite, target | What is broken | Failing checks |
|---|---|---|---|
| `dl-kill-tied-off` | pp_top `deadline` | the kill face tied off again, and the engine's `dl_kill_i` with it (the wiring before issue #81) | 28: every DL1 check (the stall answers its own SUCCESS after 33,505 clocks), DL3 (both answered for real, the MVU one SUCCESS), DL4 (no answer inside the wait), DL5, DL9 x8 (each stall and each queued command answered for real), DL10 x4 |
| `dl-released-before-queued` | pp_top `deadline` | `kill_resp_queued_i` tied 1, so the key is released at the expiry | 3: DL1 (the kill honoured 3,410 clocks before the forced response's hand-off, the hold free for them), DL5 (the dropped frame's kill honoured too) |
| `dl-armed-at-admission` | pp_top `deadline` | the deadline re-armed at every admission instead of read from the record | 6: DL3 (the queued GET_MILAN_INFO runs and answers SUCCESS, one redirect), DL9 x4 |
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
| `dl-preempt-after-effect` | ucpu `run` | the same µCPU patch, at the unit | 4: P20c (status 10 after the write, commit and mark, the notification lost), P20d |
| `ucpu-preempt-cuts-a-wait` | ucpu `run` | the redirect taken while an op waits on its face | 2: P20e (the waiting locate abandoned, no read answered) |
| `ucpu-preempt-keeps-the-body` | ucpu `run` | the cursor not returned to 12 | 1: P20f (length 36 with a partly built counters block) |
| `ucpu-preempt-repeats` | ucpu `run` | the redirect not limited to once per dispatch | 18: P20a to P20h (E_DLKILL redirected into itself, never sends) |
| `dlkill-always-misbehaving` | ucpu `run` | E_DLKILL overwrites a refusal already chosen | 1: P20d (ENTITY_LOCKED became status 10) |
| `td-lock-default-59s` | pp_top `timer-defaults` | the top's `LOCK_TIMEOUT_MS_P` default 59,000 instead of 60,000 | 1: TD1 (the auto-unlock 59,003 ms after the LOCK_ENTITY) |
| `td-tl-default-301s` | pp_top `timer-defaults` | the top's `REG_TL_TIMEOUT_MS_P` default 301,000 instead of 300,000 | 1: TD2 (no expiry DEREGISTER by 300,020 ms; 6 monitor probes answered) |
| `mvu-silent` | pp_top `budget` | GET_MILAN_INFO retires without its SEND_RESPONSE | 12: TB1, TB3 and TB4, every GET_MILAN_INFO unanswered |
| `fanout-never-ends` | pp_top `budget` | the notification walk never ends its class, so the command-path hold never drops | 23: TB3 to TB5, nothing answered once the fan-out starts |
| `acmp-waits-for-aecp` | pp_top `budget` | READ_DESCRIPTOR classified CFG_BARRIER, so ACMP waits behind unrelated AECP work | 2: TB5 (GET_RX_STATE 13,144 clocks beside the READ_DESCRIPTOR, GET_TX_STATE 977 during the fan-out) |
| `hz-stub-restored` | pp_top `hazards` | the dispatch-ROM stub the classifier replaced (ACMP STREAM_CFG and the audio-map pair MAP_CFG, keyed by protocol; everything else RO) | 81: HZ1 (35 rows), HZ2 x3, HZ3 x2, HZ4 x3, HZ5, HZ6, every keyed pair of HZ9 to HZ12, HZ8's GET_DYNAMIC_INFO x2, HZ13a x2 and HZ13c x2 |
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
| `hz-name-as-ro` | pp_top `hazards` | NAME_WR classified RO_SNAPSHOT | 9: HZ1, HZ9a x2, HZ9c, HZ9d x2, HZ9f x2, HZ11b (since the name stage: a misplaced HZ9 SET_NAME's save holds dispatch behind HZ11's held LOCK_ENTITY) |
| `hz-name-as-stream` | pp_top `hazards` | NAME_WR classified STREAM_CFG (over-serialized) | 2: HZ1, HZ9c (it waits for an UNBIND_RX of its sink) |
| `hz-name-key-none`, `-talker`, `-held` | pp_top `hazards` | NAME_WR keyed by nothing | 8: HZ1, HZ9a x2, HZ9d x2, HZ9f x2, HZ11b (as above); named HZ9a, HZ9d, HZ9f |
| `hz-registry-as-ro` | pp_top `hazards` | REGISTRY_OP classified RO_SNAPSHOT | 2: HZ1 |
| `hz-identify-as-ro` | pp_top `hazards` | IDENTIFY classified RO_SNAPSHOT | 5: HZ1, HZ12b x4 |
| `hz-identify-key-none`, `-talker` | pp_top `hazards` | IDENTIFY keyed by nothing | 5: HZ1, HZ12b x4; named on STREAM_INPUT 1 and on the talker |
| `hz-map-as-ro`, `-talker` | pp_top `hazards` | MAP_CFG classified RO_SNAPSHOT, so the cross-lock is lost | 10: HZ1 x2, HZ7 x2, HZ11d x2, HZ12c x4; named HZ7 and HZ11d |
| `hz-map-key-none`, `-talker` | pp_top `hazards` | MAP_CFG keyed by nothing (the class-wide cross-lock still holds) | 6: HZ1 x2, HZ12c x4; named on STREAM_INPUT 1 and on the talker |
| `hz-acmp-reads-as-steps` | pp_top `hazards` | ACMP GET_RX/TX_STATE and GET_TX_CONNECTION classified STREAM_CFG | 27: HZ1 x3, HZ4, HZ6 (two reads), every read of HZ9 to HZ12, HZ11b's held LOCK_ENTITY, its premise and both arms (as above), and HZ13b |
| `hz-barrier-no-priority` | pp_top `hazards` | the pending barrier's priority removed from the round-robin | 139: HZ3, then every later arm (the admission port stays wedged) |
| `hz-foreign-target-classified` | pp_top `hazards` | a command for another entity_id classified by its opcode | 1: HZ1 (CFG_BARRIER for a frame the engine drops) |
| `hz-response-classified` | pp_top `hazards` | an AECP response arriving as input classified by its opcode | 1: HZ1 |
| `hz-gdi-key-none`, `-held`, `-no-stream` | pp_top `hazards` | GET_DYNAMIC_INFO back to RO_SNAPSHOT with the NONE key, the classification before R419-2 F5 was fixed | 7: HZ1, HZ8's GET_DYNAMIC_INFO x2, HZ13a x2 (the batch admitted beside the held UNBIND_RX, refused 0 clocks), HZ13c x2; named HZ13a, HZ13c and HZ8 |
| `hz-gdi-as-barrier` | pp_top `hazards` | GET_DYNAMIC_INFO classified CFG_BARRIER (over-serialized against reads too) | 2: HZ1, HZ13b (the batch waits for a GET_RX_STATE) |

### AECP dispatch and response negative controls: `aecp_dispatch_mutants.py`

`make aecp-dispatch-mutants [AECP_DISPATCH_MUTANT_OUTPUT=DIR]` (or
`python3 aecp_dispatch_mutants.py --output DIR [--only ARM,...] [--jobs N]`)
plants each arm below, an explicit patch in `aecp_dispatch_mutations/`, into a
scratch copy of `hdl/`, `tb/common/` and this directory, one copy per arm, with
`git apply`; the driver reads only simulation and lint logs.
The HDL workflow runs the whole campaign through the make target, at the
default `--jobs`. Every generated ROM and model directory is deleted before
each build, so a microcode arm cannot leave its ROM behind. A positive control
of each make target runs first, each in its own copy, and must pass, and an arm
is KILLED only when its run completes with the build's tally (or the line
guards' summary), exits non-zero and prints the named check. `--jobs N`
(default 4, the meaning and default of `d3_mutants.py`) builds and runs up to N
copies at once, and the results are printed in the table's order whatever order
they finish in. Measured 2026-10-02 at `85da751` with Verilator 5.050, each run
pinned to 4 of the host's 16 CPUs: `--jobs 1` took 1,059 s and `--jobs 8`
814 s, and every control and arm gave the same verdict and the same failing
checks in both. Re-run 2026-10-03 with the three NSD arms (issue #37) at `--jobs 6`,
330 s: every control and every earlier arm gave the same verdict, and one count moved,
`lk-sclks-miss-lock-nop`'s, which NSD3 now fails beside LK4; the counts below are that
run's. `aecp-dispatch` runs A5b and M9 on a booted model,
then section AX on its own processor (`--aecp-dispatch-only`); `aecp-line`
runs section AX in the line build; `line-guards` lints the top across the line
range; `d3` runs section D3, whose D3C arms grade SET_CLOCK_SOURCE over a
ten-source domain (`--d3-only`). The last column is how many checks each arm
failed at the lane head.

| Arm | Defect planted | Named check | Failing checks |
|---|---|---|---|
| `m9-guard-set-stream-format` | `ssfmt_w` loses its `aem_w` message-type guard (issue #76) | `M9: mt=4 word 0008` | 9 |
| `m9-guard-set-stream-info` | `ssinfo_w` loses its guard | `M9: mt=4 word 000E` | 9 |
| `m9-guard-set-name` | `sname_w` loses its guard | `M9: mt=4 word 0010` | 9 |
| `m9-guard-get-name` | `gname_w` loses its guard | `M9: mt=4 word 0011` | 7 |
| `m9-guard-add-mappings` | the ADD term of `amap_edit_w` loses its guard | `M9: mt=4 word 002C` | 7 |
| `m9-guard-remove-mappings` | the REMOVE term of `amap_edit_w`, and `amap_remove_w`, lose their guard | `M9: mt=4 word 002D` | 7 |
| `m9-guard-dynamic-info` | `gdi_w` loses its guard | `M9: mt=4 word 004B` | 7 |
| `a5b-reboot-success-arm` | the pop decode answers REBOOT (0x002A) SUCCESS with the command echoed (issue #74) | `A5b: REBOOT (7.4.43, Figure 7-68): the response is not the echoed command` | 1 |
| `lk-ssrate-lock-nop` | E_SSRATE's CHECK_LOCK (E_SSRATE+9) replaced with NOP (issue #53) | `LK1 unset rate row, ...: ENTITY_LOCKED byte-exact` | 9 |
| `lk-ssrate-miss-lock-nop` | E_SSRATE's locate-miss CHECK_LOCK (SSR_REFUSE) replaced with NOP | `LK4 foreign SET_SAMPLING_RATE on AUDIO_UNIT 3 (absent): ENTITY_LOCKED byte-exact` | 1 |
| `lk-sclks-lock-nop` | E_SCLKS's CHECK_LOCK (E_SCLKS+9) replaced with NOP | `LK1 unset clock-source row, ...: ENTITY_LOCKED byte-exact` | 9 |
| `lk-sclks-miss-lock-nop` | E_SCLKS's locate-miss CHECK_LOCK (E_SCLKSRF) replaced with NOP | `LK4 foreign SET_CLOCK_SOURCE on CLOCK_DOMAIN 3 (absent): ENTITY_LOCKED byte-exact` | 2 |
| `lk-sctrl-lock-nop` | E_SCTRL's CHECK_LOCK (E_SCTRL+4) replaced with NOP | `LK1 IDENTIFY at its reset 0, ...: ENTITY_LOCKED byte-exact` | 12 |
| `lk-sctrl-miss-lock-nop` | E_SCTRL's locate-miss CHECK_LOCK (E_SCTRL+21) replaced with NOP | `LK4 foreign SET_CONTROL on CONTROL 3 (absent): ENTITY_LOCKED byte-exact` | 1 |
| `lk-prefix-zero-body` | the microcode generator as it stood at the lane base (`0451d83d`): the lock checked first and refused through the zero-bodied E_LOCKED4/E_LOCKED1 stubs, the issue #53 reproduction | `LK3 foreign SET_SAMPLING_RATE(96000) carries the stored 48000: ENTITY_LOCKED byte-exact` | 7 |
| `sctrl-badarg-zero-body` | E_SCTRL's out-of-range arm branches to the zero-bodied E_BADARG1 again, as at the lane base (R416-1 F1) | `LK3b IDENTIFY at 255, the holder's SET_CONTROL(128) carries 255: BAD_ARGUMENTS byte-exact` | 2 |
| `ov-oversize-never` | the engine's `txs_oversize_o` forced to 0 (issue #50's acceptance 2) | `OV1 AUDIO_MAP 0 (576 B, the whole line: cdl 592, frame 618): the 576-byte descriptor, byte-exact` | 18 |
| `ov-oversize-at-576` | the engine asks for the oversize slot at a 576-byte frame (`>=` for `>`) | `OV4 CLOCK_DOMAIN 0 (534 B: frame 576, the standard slot's own size): one TX-slot grant` | 4 |
| `ov-top-oversize-dropped` | the top's `pool_oversize_w` tied to 0, so the request never reaches the pool | `OV1 AUDIO_MAP 0 (576 B, the whole line: cdl 592, frame 618): the 576-byte descriptor, byte-exact` | 18 |
| `pg-append-524` | E_GAMAP's record APPEND loses its Δ8 flag and stops at cdl 524 again, the issue #50 reproduction | `PG2 a 63-mapping page: SUCCESS above cdl 524 (528), a standard slot: number_of_mappings` | 14 |
| `pg-cap-dropped` | E_GAMAP's page cap keeps the count and the SUCCESS (its SET_MASKED and SET_STATUS NO_RESOURCES replaced with NOP) | `PG7 a 72-mapping page: NO_RESOURCES, no record claimed: number_of_mappings` | 10 |
| `pg-cap-off-by-one` | the page cap compares against 73 instead of 72, so a 72-mapping page is answered SUCCESS | `PG7 a 72-mapping page: NO_RESOURCES, no record claimed: byte-exact` | 4 |
| `rd-base-no-overlay` | the engine's issue #82 re-dispatch removed, so AUDIO_UNIT, CLOCK_DOMAIN and STREAM reads are the image's, the issue reproduction | `RD1 AUDIO_UNIT 0 after SET_SAMPLING_RATE(48000): READ_DESCRIPTOR byte-exact` | 9 |
| `rd-au-image-only` | E_RDESCAU always branches to the image (its unset test a BRANCH) | `RD1 AUDIO_UNIT 0 after SET_SAMPLING_RATE(48000): READ_DESCRIPTOR byte-exact` | 2 |
| `rd-au-unset-overlays` | E_RDESCAU's unset test removed, so an unset row overlays its zero | `RD0 AUDIO_UNIT 0, rate unset: READ_DESCRIPTOR byte-exact` | 1 |
| `rd-cd-image-only` | E_RDESCCD always branches to the image | `RD1 CLOCK_DOMAIN 0 after SET_CLOCK_SOURCE(1): READ_DESCRIPTOR byte-exact` | 3 |
| `rd-str-unset-overlays` | the STREAM programs' unset test removed, so an unset row overlays its zero | `RD0 STREAM_INPUT 0, format unset: READ_DESCRIPTOR byte-exact` | 4 |
| `rd-so-reads-input-row` | E_RDESCSO reads the STREAM_INPUT row (`SEL_FMTIN`) instead of its own | `RD1 STREAM_OUTPUT 1 after SET_STREAM_FORMAT: READ_DESCRIPTOR byte-exact` | 4 |
| `rd-cfg-any` | the configuration-0 guard of the re-dispatch dropped | `RD2 configuration 1's CLOCK_DOMAIN 0 keeps its image bytes: the 534-byte descriptor, byte-exact` | 2 |
| `rd-tail-uncut` | the µCPU's COPY_BUFFER TAIL copies the whole length from its start (no subtract) | `RD1 AUDIO_UNIT 0 after SET_SAMPLING_RATE(48000): READ_DESCRIPTOR byte-exact` | 9 |
| `rd-str-short-guard-nop` | E_RDESCSF's too-short guard replaced with NOP, so a STREAM short of the lane takes the overlay with a wrapped TAIL count (R416-1 S1) | `RD4 STREAM_OUTPUT 1 of 80 bytes with a set row: READ_DESCRIPTOR byte-exact` | 1 |
| `line-floor-rounded` | the engine's line floor judged on the buffer rounded up to 16, as at 54c1e2b1 (R417-1 F1), so a 568-byte line elaborates (`line-guards`) | `line guard 568` | 1 |
| `line-ceiling-dropped` | the engine's line ceiling removed, so 1016 is refused only by the µCPU's own cap, which does not name `DESC_LINE_BYTES_P` (R416-1 F2; `line-guards`) | `line guard 1016` | 1 |
| `line-buffer-fixed-592` | the response buffer fixed at the default line's 592 bytes instead of `16 + LINE_BYTES_P` (`aecp-line`) | `OV1 AUDIO_MAP 0 (584 B, the whole line: cdl 600, frame 626): the 584-byte descriptor, byte-exact` | 7 |
| `rb-rounded-buffer-no-page-cap` | the buffer rounded up to 16 as at 54c1e2b1 AND E_GAMAP's page cap dropped (`pg-cap-dropped`'s two NOPs), so a page fills the rounded 608 bytes (`aecp-line`) | `RB no response byte written at or past RESP_BASE_P + 16 + DESC_LINE_BYTES_P` | 10 |
| `sclks-bound-three` | E_SCLKS's bound loaded as the constant 3, the suite list's count, instead of the located domain's `clock_sources_count` (issue #141; `d3`) | `D3C1: SET_CLOCK_SOURCE(9) over the ten-source domain answers SUCCESS` | 11 |
| `sclks-bound-inclusive` | E_SCLKS's range check made inclusive, `count >= index` in place of `index < count` (issue #141; `d3`) | `D3C2: SET_CLOCK_SOURCE(10), the count, answers BAD_ARGUMENTS` | 6 |
| `sclks-miss-target-next-word` | E_SCLKS's locate-miss branch (E_SCLKS + 3) aimed one word on, at SCLKS_EMIT, past E_SCLKSRF's CHECK_LOCK (issue #37) | `NSD3 foreign SET_CLOCK_SOURCE(2) on CLOCK_DOMAIN 1 (absent), the lock first: ENTITY_LOCKED byte-exact` | 2 |
| `sclks-miss-preload-dropped` | E_SCLKS's r6 preload (E_SCLKS + 1) replaced with NOP, so a miss carries whatever r6 the last program left | `NSD1 a second controller's SET_CLOCK_SOURCE(2) on CLOCK_DOMAIN 1 (absent), zero body: NO_SUCH_DESCRIPTOR byte-exact` | 2 |
| `sclks-miss-branch-dropped` | E_SCLKS's locate-miss branch replaced with NOP, so a miss runs on into the main path | `NSD1 a second controller's SET_CLOCK_SOURCE(2) on CLOCK_DOMAIN 1 (absent), zero body: NO_SUCH_DESCRIPTOR byte-exact` | 2 |

Each guard arm fails its opcode's row on all seven message types; the three
writers also fail their whole-body row and the read-back that proves the write
landed (M9b3, M9b4, M9b5). The REBOOT arm answers at the right length and cdl,
so only the byte-exact echo can see it, and it does. Each lock arm's
main-path NOP lets the foreign SET write: its byte-exact check, the holder's
unsolicited frame, the effect counters and the next holder SET's notification
count all fail (SET_CONTROL's also LK3b and LK3c, which then carry the 0 LK3
wrote). The reproduction fails exactly the five LK1/LK3 bodies that are not
zero (status 3 and cdl are right; LK1's SET_CONTROL at IDENTIFY's reset 0
passes, because zero is its value in force) and, since the base's out-of-range
arm was the same zero-bodied stub, LK3b and LK3c, the two checks the
`sctrl-badarg-zero-body` arm fails. With no oversize request (from the
engine or through the top) the three oversize READ_DESCRIPTORs leave truncated to a
576-byte standard slot: byte-exact, wire length, grant and slot 4 fail on each, and
OV5 with them; the request at 576 moves OV4, and RD2's read of the same 534-byte
descriptor, into slot 4. The page-cap arms
fail the pages they reach: the Δ8 flag removed, PG2 to PG6 (62 records, the full
count); the cap dropped, PG7 to PG9 (the count kept, 71 records or none carried);
the cap one late, PG7 alone. The overlay arms fail the reads they reach: without
the re-dispatch, or with the TAIL count uncut (the response runs past the descriptor),
the five RD1 reads after a SET and the four RD3 reads after the restore; each
image-only program its own type's RD1 and RD3 reads; the unset test removed, the
unset rows it serves (RD0's rate; RD0's two streams and the never-set STREAM_OUTPUT 0
of RD1 and RD3); the output program on the input row, both STREAM_OUTPUT reads of RD1
and of RD3; the configuration guard dropped, RD2 and OV4; the STREAM guard removed,
RD4, whose 80-byte descriptor (a TAIL count of 80 less 88, wrapped) gets no response
inside the bound at all. The line arms: the floor
on the rounded buffer lets 568 lint clean, and without the engine's ceiling 1016 is
refused only by the µCPU's `RESP_D8_CAP_BYTES_P` message; a buffer fixed at 592
truncates OV1 and OV5's 584-byte descriptor at the line build (byte-exact, cdl and
wire length each) and leaves RB 8 bytes short of the reservation. RB's own arm
needs two sites, because at a legal line every response is bounded twice: the
page cap and the store's line bound keep each writer inside `16 + line`, and the
buffer, exactly that reservation, drops a byte past it. The rounding alone changes
no write at any legal line, and `pg-cap-dropped` alone is fenced at the
reservation (a 176-record page carries 72 records, 600 bytes, at 584). Together
the 176-record page carries 73, and RB counts the 8 bytes past 600, besides
PG7 to PG9.
The two clock-source arms run the `d3` target. With the bound fixed at three,
SET_CLOCK_SOURCE(9) is refused, and every D3C check built on it fails: D3C1's
four, D3C2's answer and read-back, both of D3C3's restore and its save, and
D3C4's two walks, whose record is then blank. With the bound inclusive, the
count 10 is stored, marked, announced and saved: D3C2's four checks fail, and
D3C3's restore, whose saved 10 the restore rule refuses.
The three NSD arms (issue #37) run the `aecp-dispatch` target. The branch one word
on skips the lock on a miss: NSD3 and LK4 are answered NO_SUCH_DESCRIPTOR, not
ENTITY_LOCKED. With the preload dropped, NSD1 and NSD3 carry the 2 NSD0 left in r6,
and LK4 and LK5 pass, because the r6 they inherit is already 0: NSD is the one arm
that grades the preload. With the branch dropped, a miss falls into the main path
and NSD1 and LK5 are answered BAD_ARGUMENTS. The other one-word move, the branch
aimed at E_SCLKSRF - 1, is measured equivalent: word 1143 is unplaced fill, which
`gen_ucode.py` lays as a NOP there, so the program runs on into E_SCLKSRF's
CHECK_LOCK, and every check passes. It is not an arm, because the driver counts
only kills.

### GET_COUNTERS face controls (lane C7, issues #44 and #79): `ctr_mutants.py`

`make -C tb/pp_top ctr-mutants` (`python3 ctr_mutants.py --output DIR [--only a,b]
[--jobs N]`; the make target writes its logs to `CTR_MUTANT_OUTPUT`, default
`/tmp/ctr-mutants`). Each arm is a reviewed patch in `ctr_mutations/`, applied
with `git apply` to a scratch copy of `hdl/`, `tb/common/` and this directory, one
copy per arm; each runs `make counters` (K9 to K17 on their fresh model), after a
positive control in its own copy that must pass, and is KILLED only when that
run completed, failed, and printed its named check. `--jobs N` (default 4, the
meaning and default of `d3_mutants.py`, through `tb/common/mutant_pool.py`; the
make target runs the default) builds and runs up to N copies at once, and the
results are printed in the table's order whatever order they finish in.
Fifteen arms break the processor; the last two break the harness's integrator
store, the half the processor cannot hold, to show the checks refuse a store that
breaks the [integrator guide §7.1](../../docs/guides/integrator.md#counters-face)
contract. Recorded 2026-10-03 on this lane's tree with K17 and its four arms:
control PASS, 17 of 17 KILLED. K17 adds one or two failing checks to eight of the
first thirteen arms' counts (its checks see their broken push or counts); no other
check moved. Measured 2026-10-03 at `274b424` with Verilator 5.050, each run pinned
to 4 of the host's 16 CPUs: `--jobs 1` took 940 s and `--jobs 8` 699 s, and the two
printed the same record byte for byte: every control and arm the same verdict and
the same failing checks, the counts below.
Re-run 2026-10-04 at `main` `07b1469d` and at the #148 head (`--jobs 2`): control
PASS, 17 of 17 KILLED at both. `ctr-notify-one-window` fails one check more at the
head, the record #148 moves here (its row). In the arms that fail K14's late push,
that push now leaves 1,018 ms after the first, not 1,000: the window runs from the
first round's last send, its second controller's job, about 18 ms after the first.

| Arm | What is broken | Failing checks |
|---|---|---|
| `ctr-avb-not-supported` | the type gate refuses AVB_INTERFACE NOT_SUPPORTED (`KL_aecp_engine`) | 9: K9 (named), K10 x4, K11 x3, K12 |
| `ctr-ckd-not-supported` | the type gate refuses CLOCK_DOMAIN | 3: K16 x3 (named) |
| `ctr-index-from-type` | `ctr_desc_index_o` driven from the descriptor type | 18: K9 (named), K10 x4, K11 x3, K13, K14, K15 x3, K16 x4, K17 |
| `ctr-block-beats-swapped` | quadlets 4 to 7 and 8 to 11 swapped in the block (`gen_ucode.py` beat order) | 9: K11 x3 (named: offset 20), K13, K14, K15 x3, K17 |
| `ctr-locate-ignored` | the locate miss falls through to the face (`E_GCTRS`) | 1: K12 (named) |
| `ctr-notify-avb-dropped` | the notification block ignores an AVB_INTERFACE strobe (`KL_aecp_notify`) | 6: K13 (named), K14 x2, K15 x2, K17 |
| `ctr-notify-avb-as-clock` | an AVB_INTERFACE strobe marks the CLOCK_DOMAIN slot | 8: K13 (named), K14 x2, K15 x2, K16, K17 x2 (a CLOCK_DOMAIN push left pending inside K17's first wait, and no AVB_INTERFACE 0 push) |
| `ctr-notify-ckd-dropped` | the notification block ignores a CLOCK_DOMAIN strobe | 1: K16 (named) |
| `ctr-notify-one-window` | one emission starts every descriptor's one-second window | 4: K15 x2 (named), K16, K17. 3 at `main` `07b1469d`, without K15's second: since #148 AVB_INTERFACE 0's own stamp follows its round's jobs, so STREAM_INPUT 0's window opens first, and its selection, which here restarts every window, holds the interface a further second, past K15's 1,300 ms and into K17 (2 pushes, not 1) |
| `ctr-notify-no-window` | the one-second limit removed | 5: K14 x3 (named), K15 x2 |
| `ctr-change-type-from-index` | the strobe's type taken from `ctr_change_desc_index_i` (`protocol_processor_top`) | 8: K13 (named), K14 x2, K15 x3, K16, K17 |
| `ctr-notify-avb-any-index` | the notification block takes an AVB_INTERFACE strobe of any index onto AVB_INTERFACE 0's slot | 1: K17 (named: AVB_INTERFACE 1) |
| `ctr-notify-ckd-any-index` | the notification block takes a CLOCK_DOMAIN strobe of any index onto CLOCK_DOMAIN 0's slot | 1: K17 (named: CLOCK_DOMAIN 1) |
| `ctr-notify-stri-past-shape` | the Stream Input range check removed: STREAM_INPUT 8 lands on STREAM_OUTPUT 0's slot | 1: K17 (named: STREAM_INPUT 8) |
| `ctr-notify-stro-past-shape` | the Stream Output range check removed: STREAM_OUTPUT 8 lands on AVB_INTERFACE 0's slot | 1: K17 (named: STREAM_OUTPUT 8) |
| `store-counts-domain-strobes` | the harness store counts every `gm_change_i`, a domain-only one included | 7: K11 x2 (named), K13, K14, K15 x2, K17 |
| `store-link-detector-resets-up` | the harness store's link edge detector resets up, so the boot's link-up is never counted | 11: K9 (named), K10 x4, K11, K13, K14, K15 x2, K17 |

## Recorded seams and honest limits

- ST2b reads GET_COUNTERS rounds on the wire. Until issue #148,
  `KL_aecp_notify` stamped a round's one-second limit when it selected the
  round, so a solicited answer that left just before a round's first frame
  delayed that frame, and the next round, on time, then left up to one job
  (about 4 ms here) under a second after it. At main `ddb3119d`, starting ST's
  churn 30 to 95 clocks later than it starts failed ST2b (99,590 to 99,914
  clocks), and at `main` `07b1469d` 27 to 98 clocks later fell below the bound
  at row 0. The stamp now follows the clock while a round's job waits for the
  engine and the TX slot, so the next round starts a second after the previous
  round's last send, and section CS grades every row at two of those starts.
  ST keeps its phase, one clock before a millisecond tick, after the D3 writer
  saves ST1's names, so its record stays comparable.
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
- The fixture image (`load_descriptor_image` in `sim_main.cpp`) is built in
  C++ and never passes through `gen_desc_image.py`'s `build()`, so the
  packer's semantic model lint (07 §3.1) does not judge it. It is not a Milan
  model: it carries what the AECP commands under test read, and no
  CONFIGURATION or CLOCK_SOURCE descriptor, a STREAM_INPUT `buffer_length` of
  192 and no CLASS_A flag. The lint's positive Milan model is
  `hdl/aecp/desc/milan_min.json`.
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

AVB_INTERFACE 0 (Milan Table 5.13, `0x00000023`) and CLOCK_DOMAIN 0 (Table 5.15,
`0x00000003`) are **live**, kept the way the
[integrator guide §7.1](../../docs/guides/integrator.md#counters-face) asks, from
the inputs the harness itself drives into the processor: LINK_UP and LINK_DOWN
count the edges of `link_up_i`, GPTP_GM_CHANGED counts the `gm_change_i` strobes
that publish a different `gm_id_i` (a domain-only strobe counts nothing), and
LOCKED and UNLOCKED count the edges of a media-lock level the processor never
sees. Each edge detector resets inactive and every count resets with the
harness's reset, so each pair keeps its Milan invariant by construction. Every
section that reads these objects (K4c, K4d, ST) builds its expectation from the
same store at the moment it checks.

K1 demands the byte-exact 174-byte frame (Figure 7-67's block runs to byte 156,
so the AECPDU is 160 and `control_data_length` 148 — a short one is what Hive
4.3.1 reports as "Incorrect payload size"); K2 demands the Milan mandatory set
la_avdecc gates the badge on; K3 asks for STREAM_INPUT **1** and demands a
different object's answer, which is the whole point of reading `descriptor_index`
from @26 rather than @30, and demands zero bytes behind a clear mask bit; K4
demands NOT_SUPPORTED with an EMPTY mask and a full-size block for ENTITY, K4b
the Stream Outputs' block, K4c and K4d the live AVB_INTERFACE and CLOCK_DOMAIN
blocks byte-exact with their masks, K4e and K4f NO_SUCH_DESCRIPTOR for an index
the image lacks, the face never asked; K5 makes a
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

### K9 to K17 — the AVB_INTERFACE and CLOCK_DOMAIN counters on the wire (issues #44, #79)

`counters_phases.hpp`, on a fresh model of its own (`--counters-only`,
`make counters`), with the notification sections' bench. The processor keeps no
counter (owner decision 2026-09-19); these checks grade what it does with the
integrator's: carry its counts onto the wire for the right object, and push them
when the integrator says they moved. Every expected frame is written in the
section from IEEE 1722.1-2021 Figure 7-67 and Tables 7-152 to 7-155, never read
back from the store or the DUT.

| Check | Property |
|---|---|
| K9 | after boot AVB_INTERFACE 0 answers byte-exact: mask `0x23`, LINK_UP 1 (the boot's link-up), LINK_DOWN 0, GPTP_GM_CHANGED 0 |
| K10 | four link flaps, sampled in the state each grades: LINK_UP/LINK_DOWN 1/1, 2/1, 2/2, 3/2, and LINK_UP = LINK_DOWN when down, LINK_DOWN + 1 when up (Milan Table 5.1), read off the wire |
| K11 | five grandmaster changes count five; a domain-only `gm_change_i` counts nothing; the distinct counts 3, 2, 5 byte-exact at block offsets 0, 4 and 20 with every other quadlet zero (issue #44 acceptance 3) |
| K12 | AVB_INTERFACE 1, which the image lacks, answers NO_SUCH_DESCRIPTOR with the zero body, and the face is never asked about it |
| K13 | a link flap and one `ctr_change_i` for AVB_INTERFACE 0 push one unsolicited GET_COUNTERS to each of two registered controllers within 300 ms, byte-exact at each entry's own sequence_id, carrying the counts of that moment (Milan Table 5.22) |
| K14 | two more changes inside that second (a link-up, a grandmaster change) push nothing for 850 ms after the first push, then exactly one push per controller, 900 ms or more after it, with the latest counts, and nothing after it |
| K15 | a change a second after the last push goes out at once and opens a new window; inside it a STREAM_INPUT 0 strobe is pushed at once while the interface's next change waits for its own second, then goes out |
| K16 | CLOCK_DOMAIN 0 over three lock edges: LOCKED/UNLOCKED 1/0, 1/1, 2/1 byte-exact with mask `0x03` and the Table 5.7 invariant, and its strobe pushes it to both controllers |
| K17 | a `ctr_change_i` for an object the notification block keeps no slot for (AVB_INTERFACE 1, CLOCK_DOMAIN 1, STREAM_INPUT 8 and STREAM_OUTPUT 8, past the default shape), each alone, pushes nothing to either controller for 1.5 s, past every window K13 to K16 left open; then a link-down and AVB_INTERFACE 0's own strobe push it at once, byte-exact with LINK_DOWN 5, and nothing else |

The negative controls are `ctr_mutants.py` (below).

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
  The body of that refusal is issue #53's decision, graded byte-exact in
  section AX (LK).

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
builds the bench a second time; the third build is section ID's identify build,
the fourth section AX's line build, the fifth section TB's timebase, and the
sixth section TD's timer defaults:

| Build | Override | Runs | Expects |
|---|---|---|---|
| `obj_dir/Vpp_top_sim` | none: the top's own default | every section, DV, AX, DL, D3, D3V and D3KR among them, then lane C6's ID0, NP, ST and RN, and lane C7's K9 to K17 last | 2 (Milan §4.2.7.2.1) |
| `obj_vid/Vpp_top_vid` | `SRP_DOM_DEF_VID_P = 0x5A3C` (`SRP_VID_FIXTURE`) | DV alone | 0x5A3C |
| `obj_idn/Vpp_top_idn` | `EN_IDENTIFY_NOTIF_P = 1` (`PP_TOP_EN_IDENT`) | ID alone | 2 |
| `obj_line/Vpp_top_line` | `DESC_LINE_BYTES_P = 584` (`LINE_FIXTURE`) | AX alone | 2 |
| `obj_tim/Vpp_top_tim` | the wrap's timebase at the nominal clock's rate (`PP_TOP_TIM_REAL`: 1 ms = 1,000 clocks) | TB alone (`make budget`) | the product default |
| `obj_tdf/Vpp_top_tdf` | the wrap's two 400 ms timeout overrides dropped (`PP_TOP_TIM_DEFAULTS`), so the top's own `REG_TL_TIMEOUT_MS_P` and `LOCK_TIMEOUT_MS_P` stand | TD alone (`make timer-defaults`) | the product default |

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

## Section MP: the internal MAAP engine (11; IEEE 1722-2016 Annex B)

A second model runs the processor with `cfg_maap_internal_i = 1` from reset.
MP0 starts both restore walks over an erased device, so AECP is released
(section D3). MP1 grades the whole acquisition on the MAC stream, MP2 a
byte-exact DEFEND, MP3 the talker granted from the internal claim, MP4 a
yield to a conflicting ANNOUNCE, MP5 the DA-qualified subtype gate, and MP6
the descriptor path while MAAP runs.

**MP4** (issue #68, B.3.6.4): the announcer `F2:11:22:33:44:01` is
octet-reversed lower than `OWN_MAC` (`0A:0B:0C:0D:0E:0F`) but forward higher,
and a premise check says so. Only the reversed compare_MAC yields to it, so
the `tb/maap` campaign's `compare-mac-forward` arm fails MP4 (5 FAIL of 34).

**MP7** (issue #67, B.2.3): every other MAAP frame in this bench carries
maap_version 1. MP7 feeds a conflicting PROBE with maap_version 2 (B.2.3.2),
then one with maap_version 0 (B.2.3.4), through the real validator and
dispatch against the DEFEND-state claim. Each gets a byte-exact DEFEND that
carries our version 1, and the claim is kept.

`make maap-internal` builds the bench and runs the MP section alone (34
checks). The `tb/maap` mutation campaign uses it: its
`validator-maap-version-1-only` arm fails MP7 (4 FAIL of 34).

## Section AC: the ACMP listener path end to end (issues #45, #47, #48)

AC runs on a fresh model with the suite's descriptor image and an erased NVM
device, after both restore walks, with the link up and the entity enabled, so
the main DUT's timeline is untouched. `./obj_dir/Vpp_top_sim --acmp-only`
(after `make gsi-build`) runs it alone and prints its own `ACMP:` tally. Sink 1
is the one bound, never sink 0, so a stage that loses the sink index cannot
pass by accident. The legs run in wire order on one binding:

- **AI** (issue #47, REQ-ACMP-012, Milan §5.5.3.1). **AI1** BIND_RX for sink 1
  is answered byte-exact and PROBE_TX #1 (sequence_id 0, FAST_CONNECT,
  listener_unique_id 1) follows byte-exact. **AI2** a BIND_RX_RESPONSE (7, a
  response type of IEEE 1722.1-2021 Table 8-2, Table 8.1 in IEEE 1722.1-2013)
  and a reserved type (14) arrive shaped as the perfect answer to that probe:
  the steer delivers both to the listener, and neither raises an ACMP frame,
  both pass the front end with no drop counted, all four RX slots are free
  and no scoreboard hold is left.
  **AI3** the exact duplicate of probe #1 follows at T-ACMP-CMD: the sink never
  left PRB_W_RESP, which a listener that took either frame as a probe response
  would have done (it settles and cancels the timer).
- **AL** (issue #45, REQ-ACMP-001, Milan §5.5.2.2 and 03 V3; IEEE 1722.1-2021
  §8.2.1.6 sets cdl 84 for the 96-B form of Figure 8-1). Each long command
  carries its 40-byte IP tail filled with a pattern. **AL1** a 96-B UNBIND_RX
  from PRB_W_RESP2 is answered by the 56-B cdl-44 UNBIND_RX_RESPONSE,
  byte-exact. **AL2** a 96-B BIND_RX with AI1's fields and sequence_id gets
  the same response as AI1's 56-B BIND_RX, byte for byte, and PROBE_TX #2
  (sequence_id 1), regenerated from the record, is byte-exact, so the long
  command's talker and controller fields reached the binding and not only the
  echo. **AL3** a PROBE_TX to our talker, 56-B then 96-B with one
  sequence_id, gets one byte-exact answer twice (TALKER_DEST_MAC_FAILED: no
  allocator is wired on this model, as S10 (c) on the main DUT). **AL4** no
  96-B frame was dropped or counted at the front end (`rx_length` 0) and all
  four RX slots are free.
- **AS** (issue #48, REQ-ACMP-016, Milan §5.5.3.5.18 / .36 / .42 / .45 /
  .48 and §5.3.8.5 / .9): the listener's A15 reaches the SRP listener
  matcher only through the top's service stage (`st_ls_r`: DECLARE_LISTENER,
  op 2, state READY, the sink index, stream_id, DA and VID) and comes back as
  TK_ATTR_REGISTERED through the event router. The wrap exports the class-D
  bound view (`acmp_bound_eid_o`, `acmp_bound_sid_o`, `acmp_bound_dmac_o`,
  `acmp_bound_vlan_o`) for this leg. **AS1** PROBE_TX #2 goes unanswered: its
  exact duplicate follows, T-ACMP-RETRY finds no talker and re-probes
  nothing, and GET_RX_STATE answers the probing form (Table 5.37: bound
  values, stream fields 0) byte-exact; the talker's ENTITY_AVAILABLE then
  drives discovery and T-ACMP-DELAY to PROBE_TX #3 byte-exact, which the
  bench answers with a PROBE_TX_RESPONSE SUCCESS carrying {stream_id, DA,
  VLAN}. **AS2** `acmp_bound_o`/`eid`/`sid`/`dmac`/`vlan_o[1]` carry exactly
  the response's stream and every other sink's view stays clear;
  GET_RX_STATE answers the settled form (Table 5.38) byte-exact; nothing is
  declared or registered yet. **AS3** three near misses (Talker Advertise
  with the DA, the VLAN or the stream_id off by one) put no Listener vector on
  the wire, register nothing on the class-D face and route no
  TK_ATTR_REGISTERED{1}; GET_RX_STATE is unchanged. **AS4** the matching
  Talker Advertise yields Listener Ready New byte-exact, alone in its MRPDU,
  class-D READY and ADVERTISE for sink 1, exactly one TK_ATTR_REGISTERED{1}
  in the trace, and GET_RX_STATE in Table 5.39's form (REGISTERING_FAILED 0,
  the same bytes as Table 5.38's). **AS5** the state: 11.5 s after the
  settle, with the bench re-joining its Advertise every second as a live
  talker would, there is no re-probe and no Listener Lv, the declaration and
  registration hold, and GET_RX_STATE and the bound view still carry the
  stream, which a sink left in SETTLED_NO_RSV cannot do past T-ACMP-NOTK
  (§5.5.3.5.36 tears SRP down and re-probes the discovered talker). **AS6**
  UNBIND_RX from SETTLED_RSV_OK: UNBIND_RX_RESPONSE byte-exact, the Listener
  Lv (FourPackedType Ready, 802.1Q §35.2.2.7.2) byte-exact on the wire, the
  bound view cleared, no declaration or match held, GET_RX_STATE unbound
  byte-exact, nothing probes the sink from the unbind on (no ACMP frame
  between the UNBIND_RX_RESPONSE and the GET_RX_STATE, whose answer is the
  next ACMP frame, and none in the 1.5 s after it), and no RX slot or
  scoreboard hold is left.

Every byte-exact MRPDU check is aligned into a clean slot of the join
cadence (`sync_join`) inside a LeaveAll-free window: the phase waits for the
next MSRP LeaveAll when fewer than the needed milliseconds of the shortest
T-MRP-LEAVEALL (10 s) remain, re-joining the talker meanwhile. The phase costs
about 19 s of simulated time on its own model (it prints the figure).

### ACMP negative controls: `acmp_mutants.py`

`python3 acmp_mutants.py --output DIR [--jobs N] [--only NAME ...]` plants each
control in its own extract of `hdl/`, `tb/common/` and the grading suite's
directory, and counts it KILLED only when the run completes with its tally,
exits non-zero and every named check fails; a golden extract of each suite in
use runs first and must pass. Here it builds `gsi-build` and runs
`--acmp-only`; the same driver runs the listener controls in `tb/acmp_listener`
and the validator control in `tb/rx_validator` (recorded in those READMEs).
Measured 2026-09-30 at the lane head, each in its own extract: all 19 KILLED
(14 here, 4 in `tb/acmp_listener`, 1 in `tb/rx_validator`) and the three
goldens PASS. Issue #639 adds fourteen controls: nine here, run with
`--arm-queue-only` (section AQ records them), and five in `tb/acmp_listener`.
A golden now runs per suite and run mode, so `tb/pp_top` has two. Re-run
2026-10-04 at the issue #639 head of its second round: all 33 KILLED, the four
goldens PASS, and every arm below at the count recorded:

| Mutant | Defect planted | Named checks, each failing | Failing checks |
|---|---|---|---|
| `msg_ok_forced` | `txn_msg_ok_w` forced to 1 in the listener | `AI3: the sink never left PRB_W_RESP` | 1 of 43 |
| `cdl_not_44_rejected` | the validator accepts ACMP only at cdl 44 (`v1_pass_w` also requires it for subtype 0xFC) | `AL1: a 96-B UNBIND_RX`, `AL2: a 96-B BIND_RX`, `AL3: the 96-B PROBE_TX`, `AL4: no 96-B frame was dropped` | 19 of 43 (the dropped long BIND_RX leaves AS without its binding) |
| `st_ls_settle_as_withdraw` | `st_ls_r` issues the settle as WITHDRAW_LISTENER (op 3) | `AS4: the matching Talker Advertise yields Listener Ready New`, `AS4: class-D`, `AS5: 11.5 s after the settle` | 7 of 43 |
| `st_ls_teardown_as_declare` | `st_ls_r` issues the teardown as DECLARE_LISTENER (op 2) | `AS6: the Listener attribute is withdrawn on the wire` | 1 of 43 |
| `st_ls_sid_da_swapped` | `st_ls_r` carries the DA as the stream_id and the stream_id as the DA | `AS4: the matching ...`, `AS4: class-D`, `AS5: 11.5 s ...` | 7 of 43 |
| `st_ls_state_none` | `st_ls_r` declares state NONE (the teardown code) instead of READY | `AS4: the matching ...`, `AS4: class-D`, `AS5: 11.5 s ...` | 7 of 43 |
| `st_ls_vid_dropped` | `st_ls_r` carries VID 0 | `AS4: the matching ...`, `AS4: class-D`, `AS5: 11.5 s ...` | 7 of 43 |
| `st_ls_index_zero` | `st_ls_r` carries sink 0 whatever the sink (the wire frame is the same: only the per-sink faces and the state show it) | `AS4: class-D`, `AS4: exactly one TK_ATTR_REGISTERED{1}`, `AS5: 11.5 s ...` | 6 of 43 |
| `st_ls_teardown_lost` | the teardown strobe never reaches `st_ls_r` | `AS6: the Listener attribute is withdrawn on the wire`, `AS6: no Listener declared and no match held` | 2 of 43 |
| `bound_view_not_latched` | the A15 latch of the bound view removed | `AS2: acmp_bound_o/eid/sid/dmac/vlan_o[1] carry the settled stream`, `AS5: the bound view still carries the settled stream` | 2 of 43 |
| `bound_dmac_from_sid` | the bound DA latched from the stream_id | `AS2: acmp_bound_o/eid/sid/dmac/vlan_o[1] ...` | 2 of 43 |
| `bound_view_not_cleared` | the bound stream identity left behind on the unbind (A9) | `AS6: the bound view is cleared with the binding` | 1 of 43 |
| `matcher_da_ignored` | the SRP listener matcher ignores the DA | `AS3: near misses (DA, VLAN, stream_id) put no Listener declaration`, `AS3: near misses register nothing`, `AS3: no TK_ATTR_REGISTERED{1}` | 5 of 43 |
| `matcher_vid_ignored` | the SRP listener matcher ignores the VLAN | the same three AS3 checks | 5 of 43 |

## Section AQ: the timer arm-port queues (issue #639)

The top feeds the timer service's one arm port from eight engine arm faces
through a 4-deep queue per face, each a 4-entry ring in distributed RAM since
issue #639. AQ checks that mux against an independent model of the contract its
banner states: eight FIFOs of four; one arm leaves per clock, from the first
non-empty face in drain order (listener, talker, ADP, SRP, originator, MAAP,
notify, notify monitor); an arm offered to a face still full after this clock's
departure is dropped; and the drop counter (snapshot word 24, bits 31:16)
rises by one per clock however many faces drop in it, saturating. That last
rule is the landed counter's: every face's increment reads the same old value,
so two faces overrunning in one clock count once. The model records it as it
is; it is not a behaviour this suite asks for.

The wrap exports the eight faces from their own nets (`dbg_aq_vld_o`,
`dbg_aq_arm_o`), the arm port (`dbg_aq_port_valid_o`, `dbg_aq_port_o`) and the
counter (`dbg_aq_drop_o`). Every harness model runs the model at every clock
edge from its first reset (`ArmQueueModel`, in `H::step`), and the full default
run grades the main harness's at its end. **AQ1** the model saw the port at
work. **AQ2** after every edge the arm port, its valid and the drop counter
equal the model's. `./obj_dir/Vpp_top_sim --arm-queue-only` (after `make
gsi-build`) runs the main section list alone, then AQ, and prints its own `AQ:`
tally.

The first coverage line AQ prints is the reach of traffic driven from outside
the top. Measured at the issue #639 head: 71,258,305 edges and 8,270 arms in the
`--arm-queue-only` run, 28 clocks with two faces holding arms, 4,288 pushes onto
a face whose one arm left in the same clock, and none onto a face still holding
one: no face ever held two arms, so no queue filled and nothing was dropped.

So AQ ends with a drive of its own (`drive_arm_faces`), the last stimulus of the
run. From the clock the bench raises `dbg_aq_drive_i`, the wrap forces the eight
faces' own nets to the bench's arms (`dbg_aq_drv_*_i`) and holds the timer
service's arm input idle. The mux and the face taps read the bench's arms, the
port taps still read the mux's own arm port, and no engine sees an arm the drive
queues. Nothing is released. A fixed-seed draw offers arms at per-face rates redrawn
every 64 clocks, light or heavy as the draw falls, with a reset of one to four
clocks after every eighth block, taken with arms queued and still offered. Then
every face is offered at heavy rates until the drop counter has held 0xFFFF
through 4,096 dropping clocks, the faces are reset full, and the draw runs
again. Every arm offered carries its own serial number as its deadline, so an
arm written to the wrong entry, overwritten, lost or issued twice differs from
the model.

- **AQ3** after every edge of the drive, the arm port, its valid and the drop
  counter equal the model's.
- **AQ4** the drive reached every state the rings add over a one-deep queue: a
  write at head + 0, + 1, + 2 and + 3; a full face's write onto its head as the
  head leaves; two faces dropping in one clock; the counter held at 0xFFFF; a
  reset with arms queued; and arms offered in reset. AQ4 reads only the model,
  which the forced faces feed, so it does not depend on the RTL: it fails only
  when the drive stops reaching a state.

The drive's coverage line (`AQ drive:`), measured at the head of the issue's
second round, in the `--arm-queue-only` run and the full default run alike:

| Measure | Count |
|---|---:|
| edges out of reset / arms issued | 90,172 / 88,003 |
| pushes onto a face holding 0, 1, 2, 3 arms (the write at head + that many) | 18,778 / 48,239 / 3,636 / 5,594 |
| pushes onto a full face as its head left (the write lands on the leaving head) | 12,481 |
| arms refused / drop clocks | 352,294 / 80,719 |
| drop clocks with two or more faces dropping | 78,930 |
| drop clocks with the counter held at 0xFFFF | 4,096 |
| resets with arms queued / arms offered in reset | 36 / 366 |

AQ3 and AQ4 pass on `main`'s RTL (the shift queue) with the same coverage, so
the drive grades behaviour issue #639 kept.

The controls below are planted by `acmp_mutants.py` (issue #639 group), each
built with `gsi-build` and run with `--arm-queue-only`. The last four are the
review's controls of the full-queue path (R462-1's `write_refused`,
`wr_wrap_hi`, `full_pop_refuses` and `drop_skip_sat`, the same edits). Each of
those four passed this suite before the drive (AQ 2 of 2 checks): traffic never
reaches the lines they plant.

| Mutant | Defect planted | Named checks, each failing | Failing checks in the run |
|---|---|---|---|
| `armq_read_tail` | the port reads the ring at head + count instead of the head | `AQ2`, `AQ3` | 72 |
| `armq_head_stuck` | a pop never advances the head index | `AQ2`, `AQ3` | 3 |
| `armq_ring_of_three` | the head index wraps after entry 2 | `AQ2`, `AQ3` | 12 |
| `armq_write_at_head` | a push writes at the head index, ignoring the count | `AQ2`, `AQ3` | 3 |
| `armq_write_at_mid` | a push writes at head + the count after the pop (the shift queue's append index, not a ring's) | `AQ2`, `AQ3` | 3 |
| `armq_write_refused` | the ring is written on the offer, not on its acceptance, so a refused push overwrites the head of a full queue | `AQ3` | 1 |
| `armq_write_wrap_hi` | a push writes at head + 2 when the queue holds three or more | `AQ3` | 1 |
| `armq_full_pop_refuses` | a full queue that pops refuses the push of the same clock | `AQ3` | 1 |
| `armq_drop_skip_sat` | the drop counter wraps instead of saturating | `AQ3` | 1 |

Measured 2026-10-04 at the head of the issue's second round, each KILLED. In the
first five, the other failing checks are the main section's own (S2's MVRP VID
New is among them in every case).

## Section AX: AECP dispatch and response (issues #53, #50, #82)

A fresh processor of its own (the AD pattern), after AD in the default build and
by `make aecp-dispatch` (with A5b and M9) for the mutation driver, so the main
run's clock is untouched. Its image is the main run's with CLOCK_DOMAIN 0's
`clock_source_index` defaulting to 2, so an unset row's current index is not also
the zero a stub would carry.

- **LK** the ENTITY_LOCKED arm of SET_SAMPLING_RATE, SET_CLOCK_SOURCE and
  SET_CONTROL (Milan 5.4.2.13/.15/.17). The bench (the lock holder) registers for
  unsolicited notifications and re-locks before each foreign command: the wrap
  compresses the 60 s lock window to 400 ms, and an expiry would itself notify
  the holder. A second controller's SET is then answered ENTITY_LOCKED at the
  response form's cdl (20, 20, 17), byte-exact, carrying the value in force
  (06 section 6.8, IEEE 1722.1-2021 7.4.21.1/7.4.23.1/7.4.25.1): **LK1** on the
  unset rows (the image's 96000 and clock source 2; IDENTIFY's reset 0), **LK3**
  on rows the holder set in **LK2** (48000, 1, 255), each followed by a GET that
  still reads it. Every refusal writes, marks and notifies nothing (the dynamic
  store's accepted-write counter, OP_NVM_MARK and OP_NOTIFY_ENQ, and no
  unsolicited frame at the registered holder). SET_CONTROL's out-of-range
  refusal shares that tail (R416-1 F1): while IDENTIFY holds 255, where a zero
  body and the value in force differ (W13 runs at IDENTIFY's reset 0), 128 is
  answered BAD_ARGUMENTS at cdl 17 carrying 255, byte-exact and with no effect,
  to the holder under its own lock (**LK3b**) and, the lock released, to a
  second controller (**LK3c**, where a notification would reach the registered
  holder); GET and the face still read 255. **LK4**: the lock outranks a
  locate miss (AUDIO_UNIT, CLOCK_DOMAIN and CONTROL 3), zero body; **LK5**: the
  holder asking the same is answered NO_SUCH_DESCRIPTOR; **LK6**: the holder,
  still holding the lock, is served, one store write and one notification per
  changing SET, and each GET reads what it stored.
- **NSD** SET_CLOCK_SOURCE's NO_SUCH_DESCRIPTOR branch (issue #37; Milan 5.4.2.15,
  IEEE 1722.1-2021 7.4.23.1): E_SCLKS + 3 sends a locate miss to E_SCLKSRF, the
  lock check and then the refusal tail every SET_CLOCK_SOURCE refusal shares,
  which carries r6 at @28; on a miss r6 must still be E_SCLKS + 1's zero preload.
  The µCPU loads only r12 to r15 per dispatch, so **NSD0** first has the holder
  change the index from 2 to 1, which leaves the replaced 2 in r6. **NSD1**: a
  second controller's SET_CLOCK_SOURCE(2) on CLOCK_DOMAIN 1, which the image
  lacks, is answered NO_SUCH_DESCRIPTOR at cdl 20 with a zero body, byte-exact,
  with no store write, NVM mark, notification or unsolicited frame at the
  registered holder; **NSD2**: GET_CLOCK_SOURCE still reads the stored 1;
  **NSD3**: under the bench's lock the same command is answered ENTITY_LOCKED,
  zero body, the lock outranking the miss (06 section 6.4).
- **OV** a response above cdl 524 through the oversize TX slot (Milan 5.4.1; issue
  #50, #82's oversize path). The image's configuration 1 holds four descriptors on
  either side of the 576-byte standard slot (frame = 42 + length): AUDIO_MAPs of 576
  bytes (the whole line: cdl 592, frame 618; 584 bytes, cdl 600 and frame 626, in
  the line build), 536 (cdl 552, frame 578) and 528 (cdl 544, frame 570), and a
  534-byte CLOCK_DOMAIN (229 clock sources: frame 576, the standard slot's own size). Each READ_DESCRIPTOR is graded byte-exact against the
  descriptor's own bytes, with cdl off the wire (above 524) and the wire length, and
  the wrap's taps of the engine's grant, its oversize request, the serializer's start
  and slot 4's state show that **OV1**, **OV2** and **OV5** (the 576-byte one again,
  so slot 4 is reusable) took one grant of slot 4 with the request raised, left
  through slot 4 and freed it, while **OV3** and **OV4** used a standard slot with the
  request clear. Configuration 0, the main run's image, is unchanged.
- **RD** READ_DESCRIPTOR carries the value the SET stored, which the GET returns
  (issue #82). Each arm grades READ_DESCRIPTOR of configuration 0's descriptor
  byte-exact against its image bytes with the expected value in the field: AUDIO_UNIT 0
  `current_sampling_rate` @136, CLOCK_DOMAIN 0 `clock_source_index` @70, and
  STREAM_INPUT 0 / STREAM_OUTPUT 0 and 1 `current_format` @74. **RD0**, right after
  boot, before any SET: the rate and clock source are the image's (96000, 2), which the
  GETs read, and each stream is its image exactly (its GET reads the face, whose model
  here does not match the image before a SET). **RD1**, after the other sections:
  SET_SAMPLING_RATE(48000), SET_CLOCK_SOURCE(0) then (1), and SET_STREAM_FORMAT(2ch) on
  STREAM_INPUT 0 and STREAM_OUTPUT 1, each followed by its GET and the READ_DESCRIPTOR
  that must carry the GET's value; STREAM_OUTPUT 0, never set, is still its image.
  **RD2**: configuration 1's CLOCK_DOMAIN 0 keeps its image bytes (index 0) while
  configuration 0's row holds 1. **RD3** (R417-1 S2): once RD1's rows reach the NVM
  device, a power cycle carries it and both restore walks write the rows back; with
  no SET since the reset the GETs read 48000, clock source 1 and the 2ch format, and
  each READ_DESCRIPTOR carries them, STREAM_OUTPUT 0 still its image. **RD4** (R416-1
  S1): the index map gives configuration 0's STREAM_OUTPUTs 80 bytes, short of
  `current_format`'s second lane (88), a power cycle walks it again and the holder
  sets STREAM_OUTPUT 1's format; READ_DESCRIPTOR serves its 80 image bytes whole.
  The AUDIO_UNIT and CLOCK_DOMAIN programs' guards cannot be reached this way: a rate
  or clock source, set or restored, is judged against the descriptor's own list or
  count, which a descriptor short of the lane does not hold, while a stream format is
  judged by the integrator's face. At the lane base every RD1 read after a SET fails
  (the image's defaults); `rd-base-no-overlay` below restores exactly that.
- **PG** the GET_AUDIO_MAP page (issue #50). The audio-map face serves STREAM_PORT_INPUT
  1's page 0 at M mappings, every record distinct, and each response is graded
  byte-exact, its `number_of_mappings` against the records its cdl carries, and its TX
  slot as in OV. **PG1** to **PG6** (62, 63, 64, 65, 66 and 71 mappings) are served
  whole with SUCCESS: cdl 24 + 8M passes 524 from 63, the frame (50 + 8M) takes the
  oversize slot from 66, and 71 is the cap (cdl 592, frame 618). **PG7** to **PG9** (72,
  176 and 256, whose count's low byte is 0) answer NO_RESOURCES with
  `number_of_mappings` 0 and no record, and **PG10** serves the 3-mapping page whole
  again. At the lane base PG2 to PG6 fail: the records stopped at cdl 524, 62 of
  them, while `number_of_mappings` still named every one (the `pg-append-524` arm
  below restores exactly that).
- **RB** every response write stays inside the reservation (R417-1 F1). The
  integrator reserves `16 + DESC_LINE_BYTES_P` bytes at `RESP_BASE_P` and nothing
  else writes there (integrator guide section 5, 07 section 3.3.2), and the
  engine's response buffer is exactly that. The response-memory model counts
  every strobed byte it is asked to write outside the reservation instead of
  dropping it unseen, and after the other arms RB demands none, that the top
  elaborated the line the bench reserves for (the wrap's `dbg_desc_line_bytes_o`),
  and that OV1's whole-line descriptor reached the reservation's last byte.

The fourth build, `obj_line` (`make aecp-line` alone), runs section AX at
`DESC_LINE_BYTES_P = 584` (`LINE_FIXTURE`), a legal line that is not the default and
whose 600-byte reservation is not a multiple of 16: OV1 reads a 584-byte descriptor
(cdl 600, frame 626) and RB bounds every write by 600. `make line-guards` (run by
`make`) lints the real top, with `scripts/lint_hdl.sh`'s flags, at 576, 584 and
1008, which must lint clean, and at 568, 1016 and 580, which the engine must refuse
by name (`DESC_LINE_BYTES_P=568 is below 576`, `...=1016 is above 1008`, `...=580 is
not a multiple of 8`). Verilator reports an elaboration `$error` as a warning that a
`-Wno-fatal` build carries past, so the verdict is a lint's, which tolerates none.

## Lane C6: notifications and identify (issues #54, #58, #80, #86)

`notify_phases.hpp` holds eight sections, each on a fresh processor of its own
(the section AD pattern: its own model, the suite's descriptor image, erased NVM,
both restore walks, link up and enable), so the main run's clock is untouched.
Every AECP and ACMP frame is logged with the clock its last byte left on, so
spacing and latency are read off the wire. Every expectation is built from the
clause byte offsets; a per-entry `sequence_id` is modelled from the wire alone, as
the count of unsolicited frames that controller was sent before (Milan §5.4.5.1).

`--identify-only` runs ID0 (default build), `--notify-only` runs NP, ST and RN,
`--spacing-only` runs CS, and `--domain-notify-only` runs DN; `make identify` builds and
runs the third build and ID0, and `make timer-defaults` the sixth build, which runs TD
alone.

### The third build: `P-EN-IDENTIFY-NOTIFICATION`

| Build | Override | Runs |
|---|---|---|
| `obj_dir/Vpp_top_sim` | none: `EN_IDENTIFY_NOTIF_P` = 0, the default | every section, ID0 among them |
| `obj_vid/Vpp_top_vid` | `SRP_DOM_DEF_VID_P` (section DV) | DV alone |
| `obj_idn/Vpp_top_idn` | `EN_IDENTIFY_NOTIF_P` = 1 (`PP_TOP_EN_IDENT`) | ID alone |
| `obj_line/Vpp_top_line` | `DESC_LINE_BYTES_P` (section AX) | AX alone |
| `obj_tim/Vpp_top_tim` | the wrap's timebase at the nominal clock's rate (section TB) | TB alone |
| `obj_tdf/Vpp_top_tdf` | the wrap's two timeout overrides dropped (section TD) | TD alone |

The wrap adds `identify_button_i` and sets the parameter only under
`PP_TOP_EN_IDENT`, so the first build grades the top's own default.

### Section ID: IDENTIFY_NOTIFICATION origination (#54, REQ-AEM-026)

IEEE 1722.1-2021 §7.4.39.1 (Figure 7-61), §7.5.1, §7.5.1.2.1 and Figure 7-142;
Milan §5.4.5.4. The expected frame is `aecp_frame(91-E0-F0-01-00-01, own MAC,
AEM_RESPONSE, SUCCESS, entity_id, 90-E0-F0-FF-FE-01-00-01, seq, 0x0026, {CONTROL,
IDIX})` with u = 1: 42 bytes padded to 60, cdl 16. Every frame is due
T-IDENT-BURST after the previous one left (its last byte to the MAC), so spacing
is graded at least the T- value (150 ms = 15,000 clocks, 1 s = 100,000 clocks) and
at most `SLACK` = 400 clocks more: one tick of the ms timebase (each deadline
counts from the next ms boundary after a departure), the sweep's walk to the
identify slots (at most 91 cycles) and the job's build and serialization (under
200 clocks here). The largest gap measured in the section is 15,309 clocks.

- **ID1** one press (40 ms): exactly three frames, each byte-exact at sequence_id
  0, gaps of 15,264 and 15,300 clocks, and nothing else on the wire; no controller
  is registered.
- **ID2** held 2.5 s: three bursts, sequence_id 1, 2, 3 (one per burst), each
  spaced as ID1, each burst 100,000 to 100,400 clocks after the previous one's
  first frame (Figure 7-142's timeout; 100,265 and 100,300 measured), and none
  after the release.
- **ID3** a release and a press inside a burst: the running burst is not cut
  (sequence_id 4, three frames), the new press's burst (sequence_id 5) starts
  T-IDENT-BURST after its third frame left (15,301 clocks), and the second release
  stops it before its timeout.
- **ID4** the controller-to-entity forms start nothing: IDENTIFY_NOTIFICATION as a
  command still answers BAD_ARGUMENTS byte-exact (§7.4.39.2, the A6 contract in
  this build), and SET_CONTROL IDENTIFY 255 and back to 0 sends no frame to the
  multicast address (Milan §5.4.5.4).
- **ID5** fifteen controllers registered and a SET_NAME fan-out running: the press
  delays the burst by at most two fan-out frames (the job in flight and the one
  the engine already took), the burst keeps its bytes and spacing, all fifteen
  rows receive their notification byte-exact, and the next fan-out is at
  sequence_id 1 on every row: the identify job touched none. **ID5i-ID5l** (the
  shape of review R421-1's probe P1): a fan-out fed 550, 300 and 50 ms-ticks before
  frame 2 is due (frame 1 + 14,450, 14,700 and 14,950 clocks) holds the engine, so
  frame 2 leaves late (gap 1->2 up to 16,147 clocks, ID5l: some fan-out did delay
  it); frame 3 is due T-IDENT-BURST after frame 2 left, so no gap is short (the
  smallest of the six is 15,240; a deadline chained from the first frame leaves
  14,109).
- **ID6** a press before the restore (AECP held from reset): nothing leaves while
  held; at the release the burst goes out at sequence_id 0 (the reset restarted
  identifySequenceID, §7.5.1) and keeps its spacing. The hold is before the first
  frame; ID7 grades a stall between frames.
- **ID7** a TX stall mid-burst (the MAC's `tx_ready_i` low, as a full MAC FIFO
  holds it), sequence_ids 1 to 5 after ID6's reset. The engine retires a job at
  its lane grant, so a stall can hold a frame the engine has already let go:
  - **ID7-ID7e** 400 ms inside frame 1: frame 1 leaves 40,167 clocks after the
    press, and frame 2 follows it by 15,270 (not 63, as from the retirement);
  - **ID7f-ID7i** 400 ms after frame 1 (review R420-1's probe): gap 1->2 40,093,
    gap 2->3 15,264 (not 164, as from t0);
  - **ID7j-ID7m** 250 ms after frame 2: gaps 15,290 and 25,093;
  - **ID7n-ID7t** held through 900 ms after frame 1, so Figure 7-142's timeout
    passes before the third frame leaves: gaps 90,060 and 15,273, then the next
    burst T-IDENT-BURST after the third frame (15,301, not at once), spaced as
    ID1 and still at least T-IDENT-REARM after the first frame.

  On the round-1 RTL (every frame from t0, the next burst at once) the section
  fails seven checks: ID3f (106 clocks), ID5k (14,109), ID7d (63), ID7e (207),
  ID7i (164), ID7q (63) and ID7r (10,111).
- **ID8** no press is lost to a burst or to the T-IDENT-BURST gap after it
  (reviews R420-2 F1 and R421-2 F1; Figure 7-142 answers `identifyButtonPressed`
  in WAITING, and the gap only delays the answer). Each arm starts from a burst
  of its own (a 30 ms press). ID8 and ID9 run after ID5 and before ID6's reset,
  at sequence_ids 10 to 25 and 26 to 27, so section ID still ends with ID7. The
  gap's end is read off the timer bus: the wrap taps the IDENT-BURST
  singleton's arm (with its ms deadline) and its expiry (`dbg_ident_gap_*`,
  observe-only references into the top, like the other taps), so a press can be
  placed against it to the clock:
  - **ID8-ID8e** a 30 ms press made 2 ms after the third frame left, over long
    before the gap ends: one burst, byte-exact, its first frame 15,301 clocks
    after the third (the expiry came 63 clocks into its deadline's ms, and the
    burst's first frame left 166 clocks after it);
  - **ID8f-ID8i** a 200 ms press made at the same point, still held when the gap
    ends: one burst, starting exactly as the latched press did (166);
  - **ID8j-ID8n** a 30 ms press whose synchronised level the sequencer first
    samples one edge before the edge that samples the expiry, on it (k = 0: the
    gap's last clock), and one and two edges after it. The expiry is predicted
    from the arm's deadline and ID8's 63 (ID8j checks the prediction held), and
    each press sends exactly one burst. The first three start on the same edge
    as the latched press (166); the last starts one clock later (167), so the
    measure resolves a single clock;
  - **ID8o-ID8r** a release and a new press inside a burst, still held when its
    third frame leaves and let go 30 ms later, inside the gap: one more burst
    at the gap's end (166);
  - **ID8s-ID8v** a release and a new 30 ms press between frames 1 and 2, let go
    long before the burst ends: one more burst at the gap's end (166).

  On the round-2 RTL (the WAITING start reads the button level with no latch)
  the section fails 29 checks. Three are the lost presses: ID8 (3 frames in all,
  want 6), ID8o and ID8s (each 3, want 6). The other twenty-six follow from them:
  every later burst's identifySequenceID is short by the bursts lost before it
  (ID8g, ID8l, ID9b, ID9f), and ID8 never measures the start the later arms
  compare against (ID8i, ID8n). ID8f and the presses at the gap's end pass
  there: a press still held when the gap ends needs no latch.
- **ID9** a MAC stall on a frame's last byte (review R421-2 S1): the MAC holds
  `tx_ready_i` low on the eof beat itself (the bench's `stall_tx_at_eof` hook,
  armed once the identify frame is part-way out), so the frame is presented
  whole and its last byte not taken for 400 ms. **ID9-ID9d** frame 1's: it
  leaves 40,001 clocks after the stall began, and frame 2 follows its last byte
  by 15,232 clocks (63 when the departure is taken at the eof beat's
  presentation, without `ready`); **ID9e-ID9h** frame 2's: frame 3 follows by
  15,299.

The one-tick margins of the schedule (each deadline counts from the next ms
boundary, so a gap is at least the T- value rather than up to a tick short) are
below this bench's resolution: one tick is 100 clocks here, no longer than a
frame's build and serialization. Section FT of `tb/aecp_notify` grades them at
the full timebase (reviews R420-2 S1 and R421-2 S2).

### Section ID0: the default build (the parameter at 0)

The button held across two T-IDENT-REARM periods and pressed again: no AECP frame
at all, nothing to 91-E0-F0-01-00-01. Beside it, every other section of the
default build is the unchanged pre-lane suite. The netlists say the same: yosys
`equiv_*` proves `KL_aecp_notify` at 0 equivalent to main's, and the engine's own
logic at 0 equivalent to main's plus the SET_STREAM_INFO fix below (issue #80's
lane record carries the scripts).

### Section NP: a wire-level push per command class (#58)

The requester A and a second controller B are registered. Each step graded: the
solicited response byte-exact, exactly one unsolicited response of that command
at B, byte-exact (u = 1, B's DA and entity_id, B's own sequence_id, the command's
response body per IEEE §7.5.2), and none at the requester (Milan §5.4.5.2):
NP1/NP1b SET_CONFIGURATION to 1 and back to 0, NP2 SET_STREAM_FORMAT, NP3
SET_STREAM_INFO, NP4/NP4b SET_CONTROL IDENTIFY 255 and 0, NP5
SET_SAMPLING_RATE(48000), NP6 STOP_STREAMING and NP7 START_STREAMING on a sink
bound with STREAMING_WAIT clear, NP8 a SET from B reaching A at the count of
what A was actually sent, and NP9/NP9b unchanged SETs pushing nothing.

**Found by NP3 and fixed.** On the landed RTL the unsolicited SET_STREAM_INFO
carried Milan's 56-byte GET_STREAM_INFO body (cdl 68, the GET program's
integrator words) under the SET_STREAM_INFO command type. IEEE §7.4.15.1 gives
the SET_STREAM_INFO response Figure 7-40's complete 84-byte body (cdl 96), and
Milan §5.4.2.10 replaces the format of the GET response alone. The job now runs
`E_SINFOUNS`: {type, index}, MSRP_ACC_LAT_VALID alone and the offset in force,
zeros elsewhere, the same bytes as the successful solicited answer
(`stream_info_get_body` in the record below puts the old mapping back).

### Section ST: STORM (#80 acceptance 2)

- **ST1** sixteen controllers registered in three waves with a change after each
  of the first two, so the rows carry sequence_ids 2, 1 and 0: one SET_NAME from
  row 15 reaches the other fifteen byte-exact, each at its own sequence_id, and
  the requester receives only its answer.
- **ST2** every served descriptor of five (STREAM_INPUT 0 and 1, STREAM_OUTPUT 0,
  AVB_INTERFACE 0, CLOCK_DOMAIN 0) changes every 100 ms for 3.5 s: each emits at
  least three and at most one round per second (4 in 5 s, the closest two 115,077
  clocks apart: since issue #148 a round's last send starts the next second, and a
  sixteen-row round takes about 15,000 clocks here; at `main` `07b1469d`, before it,
  5 or 4, the closest 99,994: the limiter reads one tick), and every GET_COUNTERS
  frame is byte-exact at its row's own sequence_id. The one-tick tolerance is a recorded
  decision (review R420-1 S4, retained): the limiter predates this lane and counts
  Milan Table 5.22's "once per second" on the 1 ms timebase like every T- value (08
  §3: 1 ms resolution), so two rounds are at least 1,000 ticks apart: in core
  clocks, at most one tick short of a second.
- **ST3** a GET_CONFIGURATION and an ACMP GET_RX_STATE every 700 ms of the churn
  are all answered, within T-BUDGET-AECP-WC and T-BUDGET-ACMP-RESP counted in
  clocks at the wrap's nominal clock (clk_ms(100), clk_ms(50)): worst 70,681 and
  485 clocks (70,681 and 249 at `main` `07b1469d`: the rounds' phase moved with
  issue #148). The ms timebase is compressed a thousandfold while the engine's
  work is counted in real clocks, so the notifications a real second allows are
  packed into 100,000 clocks here; the bound is a hundred times stricter than the
  F01.5 default clock's.

### Section RN: RND (#80 acceptance 3)

Seed 0xC6A46301, 720 xorshift32 steps from twenty controllers: REGISTER (30 %),
DEREGISTER (7 %), LOCK (13 %), UNLOCK (8 %), SET_CONTROL (24 %),
SET_CLOCK_SOURCE (10 %) and GET_CONTROL (8 %) against the model in
`RndPhase::Model`: the list (capacity 16, a refresh keeps its entry's
sequence_id, a new entry starts at 0), the lock (one holder, keep-alive, the
400 ms compressed expiry) and the pushes (every registered controller but the
requester; an automatic unlock excludes nobody). Each step compares the solicited
response and the set of unsolicited frames it caused byte for byte, and counts any
other frame as a divergence. The lock never sits in the ambiguous window around
its expiry: 250 ms after its last LOCK the holder refreshes it or the bench waits
600 ms for the automatic unlock. A lock-refused SET is graded on status and cdl
only (its body is issue #53's). Result: 2,203 frame comparisons, zero divergence;
24 NO_RESOURCES, 117 lock denials, 10 lock takes, 7 automatic unlocks, 239
lock-refused SETs, 375 pushes, in 10.6 s of compressed time (under the controller
monitor's 30 s floor, so no CONTROLLER_AVAILABLE is due).

### Section CS: counter spacing from the previous round's send (#148)

Milan Table 5.22 (`T-CTR-NOTIF`, 08 F08.1) allows one GET_COUNTERS notification per
descriptor per second. A round whose job waits for the TX slot, behind a solicited
answer that leaves just before it, must still leave a second after the previous
round's send. Each check runs ST's churn (the five descriptors at 10 Hz for 3.5 s,
a GET_CONFIGURATION and a GET_RX_STATE every 700 ms, then 1.5 s quiet) on a fresh
processor with all sixteen rows registered, and grades every row's rounds, not only
row 0's as ST2b does: each controller's GET_COUNTERS frames of one descriptor are at
least 1,000 ms less the one tick the limiter reads apart (ST2, 99,900 clocks), and
each row receives at least three rounds of each descriptor.

- **CS1** (premise, each run) the sixteen controllers register.
- **CS2a** the churn starts at ST's phase, one clock before a ms tick.
- **CS2b** it starts 30 clocks later.
- **CS2c** it starts 95 clocks later.

The two shifted starts are the "shifted timing" of #148: while `KL_aecp_notify`
stamped a round when it selected it, a start 27 to 98 clocks after ST's phase (of
every 100) narrowed a gap below the bound at `main` `07b1469d`, 30 to 95 at
`ddb3119d` (the README's earlier limit). At `main` CS2a passes (99,994 clocks), CS2b
fails (99,854, row 0, STREAM_OUTPUT 0) and CS2c fails (99,590, row 0, STREAM_INPUT
1). At this head the closest gap is 115,077, 115,070 and 115,077 clocks: the next
round starts a second after the previous round's last send, and a sixteen-row round
takes about 15,000 clocks here.

### Section DN: the Domain and link-edge GET_AVB_INFO notification (#42)

Milan §5.3.6.2 and Table 5.22 report a change of the Class A Domain (§4.2.7.2.1: its
priority and VID) and of the link state to every registered controller as an
unsolicited GET_AVB_INFO. The top ORs four triggers into `KL_aecp_notify`'s `ev_avb_i`:
`gm_change_i` and `gsi_avb_chg_i`, which section V grades (V6 to V6i), and the two this
section grades, the SRP Domain machine's DOMAIN_CHANGE (`srp_evt_domain_change_w`) and
the edge of `link_up_i`. One controller A is registered, and a bridge feeds its Class A
Domain in S8's certified two-class shape (FirstValue {5, 2, VID}, NumberOfValues 2).
Each notifying step is graded twice: exactly one frame reaches A in the 1.2 s after the
stimulus, and it is a u = 1 GET_AVB_INFO (so no GET_AS_PATH and nothing else); and it is
byte-exact: SUCCESS to A's DA and entity_id at the entry's `sequence_id`, AVB_INTERFACE
0, cdl 40 and V1's body. The body is the integrator's face answer (06 §6.10), so the
check grades the trigger and the frame, not the words.

- **DN0** (premise) A registers.
- **DN4b-DN4e** the link edge, at DEFAULTS: link down sends one GET_AVB_INFO,
  byte-exact at `sequence_id` 0, and link up one more at 1 (issue #42 acceptance 4).
  **DN4** (premise) neither edge raises DOMAIN_CHANGE: `KL_srp_domain` strobes its
  LINK_DOWN revert only from ADOPTED, so the link term is the only trigger.
- **DN1** (premise) the bridge's {3, 5} is adopted with one DOMAIN_CHANGE. **DN1b,
  DN1c** one GET_AVB_INFO, byte-exact at `sequence_id` 2 (acceptance 1).
- **DN3** the bridge declares the adopted {3, 5} again, as its periodic re-declaration
  does: no DOMAIN_CHANGE, and nothing reaches A (acceptance 2).
- **DN2** (premise) the bridge declares the default {3, 2}, which is then in force
  with one DOMAIN_CHANGE. **DN2b, DN2c** one more GET_AVB_INFO, byte-exact at
  `sequence_id` 3. The strobe comes from the adoption arm (`KL_srp_domain.sv:184`): a
  received Domain that differs from the operating one is adopted, even when it carries
  the default's values, so `srp_domain_adopted_o` stays 1 (the section's `[i]` line).
  The revert at `:157` runs on LINK_DOWN alone.
- **DN3b** the default declared again: nothing.

The section's `[i]` lines time each notification from its stimulus: the link edge, or
the return of `feed()`, which clocks four idle cycles after the MRPDU's last byte. Each
link-edge notification leaves 466 clocks after the edge. Each Domain notification
leaves 495 clocks after `feed()` returns, so 499 after the MRPDU's last byte.
The LINK_DOWN revert at `:157` is not graded on its own here. It strobes on the
same edge that raises the link term of this OR, so at `ev_avb_i` it cannot be told
apart from the link term, and removing `srp_evt_domain_change_w` still leaves a
notification on that edge. DV5 grades the revert's DOMAIN_CHANGE.

**Spacing.** The only notification rate limit 06 §7 defines is the GET_COUNTERS one:
`T-CTR-NOTIF`, one second per descriptor from the previous round's last send.
GET_AVB_INFO coalesces into one pending bit and has no limit. The section still waits
out that second after each notification: `space_out` holds a stimulus until 1,000 ms
(100,000 clocks) after the latest notification to A left. It does not count the
REGISTER response, which is a solicited answer and not a notification, so the first
stimulus (link down) comes at the bench's clock 100,000, 96,536 clocks (965 ms) after
that response. Each window is 1.2 s, longer than the spacing, so every later stimulus
follows the previous window directly, at least 1,000 ms after the latest notification.
No count depends on coalescing or on a limiter. The section takes 8,167 ms of the
timebase after the registration. That is under the controller monitor's 30 s floor, so
no CONTROLLER_AVAILABLE reaches A.

### Mutation record: `notify_mutants.py`

`python3 tb/pp_top/notify_mutants.py --output DIR [--jobs N] [--only NAME ...]`
(`--jobs` default 1) plants each control in a
private copy (the `d3_mutants.py` rules: exact edits, goldens first, KILLED only
with a completed run, a non-zero exit and every named check failing). Results at
the lane head, 40 of 40 KILLED (the four `ident_*` controls after
`ident_t0_at_request` are round 2's, and the six after them round 3's).
Re-run 2026-10-03 at lane P1's merge of `main` `f4167536`: the goldens PASS, 40 of 40
KILLED, every count as below. `ident_burst_from_t0`'s count moved with lane P1, and
`main` alone still fails 20. Issue #232 adds the four `ix_*` controls of the
registry's identity index, graded by `tb/aecp_notify` section IX, for 44 of 44,
and PR #153's review adds three more, graded by `tb/aecp_notify` sections IX
and TS, for 47 of 47. Issue #148 adds six counter-spacing controls, graded by
section CS and by `tb/aecp_notify` section TW, for 53 of 53. Re-run 2026-10-04 at
`main` `07b1469d` (47 of 47) and at the #148 head (53 of 53): the goldens PASS, and
46 of the 47 earlier controls fail the same checks at both. `counter_limit_500ms` is
the one record #148 moves, and its named check is now ST2b alone (its row). Issue #158
adds three controls, graded by `tb/aecp_notify` section DR, for 56 of 56. Re-run
2026-10-05 at `main` `054d01c7` (53 of 53) and at the #158 head (56 of 56): the
goldens PASS, and 50 of the 53 earlier controls fail the same checks at both. The
three `tb/aecp_notify` TW controls also fail DR3 since #158 (their rows). Issue #42
adds nine controls, graded by section DN on a run of its own (`--domain-notify-only`),
for 65 of 65. Re-run 2026-10-06 at `main` `e6a759de` (56 of 56) and at the #42 head
(65 of 65): the goldens PASS, and the 56 earlier controls fail the same checks at both.
Issue #69, on its branch without #42,
adds nine controls, eight graded by `tb/aecp_notify`'s third build (sections PT and CK)
and one by this suite's seventh (section IF), for 65 of 65. Re-run 2026-10-06 at
`main` `e6a759de` (56 of 56) and at the #69 head (65 of 65): the goldens PASS, and all
56 earlier controls fail the same checks at both (a control that fails the first
`tb/aecp_notify` build stops `make` before the third). Issue #69's second round
(review R512-1) adds twelve controls, ten graded by `tb/aecp_notify`'s third build
(sections PD and CA) and two by this suite's seventh (IF3, IF3b), for 77 of 77. Re-run
2026-10-06 at the first round's head `cb730a2f` (65 of 65) and at the second round's
head (77 of 77): the goldens PASS, and 63 of the 65 earlier controls fail the same
checks at both; `port_not_compared` and `port_not_latched` also fail checks of the new
sections (their rows):

| Mutant | Planted in | Failing checks |
|---|---|---|
| `ident_two_frames` | a burst of two | 35, ID1 first |
| `ident_seq_per_frame` | identifySequenceID per frame | 82, ID1b first |
| `ident_no_rearm` | IDENT-REARM never armed | 58, ID2 first |
| `ident_rearm_from_third_frame` | re-arm at t0 + 1.3 s | 58, ID2 and ID2d among them |
| `ident_burst_100ms` | T-IDENT-BURST 100 ms | 39, ID1c first |
| `ident_t0_at_request` | t0 at the press, not the first frame's departure | 58, ID2d (burst 3) among them |
| `ident_burst_from_t0` | frames 2 and 3 due t0 + 150 and t0 + 300 ms (round 1's schedule) | 21, ID3f, ID5k, ID6d, ID7i, ID7q and ID7r among them (20 before lane P1: its name stage lengthens the D3 walk that holds ID6's burst, which moves the burst's phase, and frame 3, still due at t0 + 300 ms, then leaves 14,979 clocks after frame 2) |
| `ident_departure_is_retirement` | the departure taken at the engine's retirement (the lane grant) | 9, ID7d and ID7q among them |
| `ident_departure_unwired` | `uns_tx_busy_i` tied 0 at the top | 9, ID7d and ID7q among them |
| `ident_next_burst_at_once` | the next burst not held for T-IDENT-BURST after a third frame (both starts) | 20, ID3f, ID7r and ID8c among them |
| `ident_wait_ignores_gap` | the WAITING start alone not held for the gap (`&& !gap_r` dropped there; the latch stays) | 18, ID8c and ID8h among them |
| `ident_press_not_latched` | a press seen in WAITING while the gap runs not latched | 41, ID8 first |
| `ident_burst_press_not_latched` | a new press after a release inside a burst not latched | 8, ID8o and ID8s among them |
| `ident_departure_ignores_ready` | the departure taken at the eof beat's presentation (`&& arb_tx_ready_w` dropped at the top) | 2: ID9d, ID9h |
| `ident_burst_deadline_one_tick_short` | the IDENT-BURST deadline from the departure's own ms (`+ 1` dropped) | 1: `tb/aecp_notify` FT2 |
| `ident_t0_same_ms` | t0 the departure's own ms, not the next boundary | 1: `tb/aecp_notify` FT4 |
| `ident_cut_on_release` | a release ends the burst | 39, ID1 first |
| `ident_release_ignored` | a release never returns to WAITING | 45, ID1 first |
| `ident_unicast_da` | the job's DA is the registry tuple's | 25, ID1 first |
| `ident_face_taken_mid_job` | the uns face taken while a registry job is presented | 3: ID5b, ID5c, ID5f |
| `ident_built_at_default` | the sequencer built at the default | 2: ID0, ID0b |
| `enq_dropped_configuration` | E_SCFG's NOTIFY_ENQ a NOP | 2: NP1, NP1b |
| `enq_dropped_stream_info` | E_SINFO's NOTIFY_ENQ a NOP | 1: NP3 |
| `class_4_mapped_to_rate` | class 4 to PP_UNS_SRATE_C (`KL_aecp_notify` pick) | 4: NP4, NP4b, NP8, RN |
| `class_9_mapped_to_control` | class 9 to PP_UNS_CTRL_C | 2: NP6, NP7 |
| `requester_not_excluded` | command pushes exclude nobody | 12, NP1 first |
| `entry_seq_not_advanced` | the row's sequence_id never moves | 13, NP1b first |
| `stream_info_get_body` | the landed SET_STREAM_INFO mapping | 1: NP3 |
| `counter_limit_500ms` | the GET_COUNTERS limiter at 500 ms | 7: ST2b x5 (named), ST3, ST3b. 12 at `main` `07b1469d`, with ST2 x5 named too: since #148 a round's second runs from its last send, so half a second leaves rounds 75,504 clocks apart, six in ST2's five seconds, inside its count bound |
| `fan_out_skips_row_0` | the walk skips row 0 | 9, ST1b among them |
| `refresh_resets_seq` | a refresh re-zeroes the entry | 1: RN |
| `foreign_unlock_allowed` | a foreign UNLOCK succeeds | 1: RN |
| `lock_taker_notified` | the lock taker is notified too | 1: RN |
| `deregister_keeps_row` | DEREGISTER leaves the row | 1: RN |
| `registry_holds_15` | the last row is never claimed | 2: ST1, RN |
| `set_control_ignores_lock` | E_SCTRL's CHECK_LOCK a NOP | 1: RN |
| `inflight_highest_free_id` | `KL_pp_originator` allocates the highest free entry | 15, `tb/originator` R among them |
| `inflight_match_ignores_seq` | the response CAM ignores sequence_id | 7, R among them |
| `inflight_cancel_keeps_timer` | a cancellation leaves its timer armed | 5, R among them |
| `inflight_shared_seq` | one sequence counter for every owner | 10, R among them |
| `ix_old_identity_kept` | a row write never clears the old identity | 1: `tb/aecp_notify` IX1 |
| `ix_new_identity_unset` | a row write never sets the new identity | 3: `tb/aecp_notify` IX3, IX4, IX6b |
| `ix_last_chunk_ignored` | the match ignores the last 6-bit chunk | 1: `tb/aecp_notify` IX2 |
| `ix_rewrite_unmatched` | the two rewrite cycles read the index, not the compare | 3: `tb/aecp_notify` IX4, IX6, IX6b |
| `override_set_only` | the compare covers only the rewrite's second cycle; the row write's own cycle reads the index | 2: `tb/aecp_notify` IX6, IX6b |
| `own_compare_new_row` | the rewrite's compare reads the incoming row, not what `rows_r` holds | 1: `tb/aecp_notify` IX5 |
| `stamp_read_without_valid` | a counter stamp is read without its valid bit, `ctr_sent_r` | 1: `tb/aecp_notify` TS3 |
| `counter_spacing_from_selection` | the stamp no longer follows a waiting job: the one-second limit restarts at the round's selection (`main`'s rule) | 2: CS2b, CS2c |
| `counter_spacing_from_selection_tw` | the same edit, graded in `tb/aecp_notify` | 3: `tb/aecp_notify` TW1, TW2, DR3 (2 before #158) |
| `counter_stamp_at_send_only` | the stamp written at the job's send alone, not while it waits | 2: `tb/aecp_notify` TW2, DR3 (1 before #158) |
| `counter_stamp_first_job_only` | the stamp follows only the round's first job (row 0) | 3: `tb/aecp_notify` TW1, TW2, DR3 (2 before #158) |
| `counter_limit_500ms_cs` | `counter_limit_500ms`'s edit, graded by section CS | 3: CS2a, CS2b, CS2c (75,504 clocks at each start) |
| `registry_holds_15_cs` | `registry_holds_15`'s edit, graded by section CS | 6: CS1 x3 (15 of 16 register), CS2a, CS2b, CS2c (row 15 receives no round) |
| `dereg_mid_round_no_hold` | a DEREGISTER drained between two jobs of a round no longer waits for the round's boundary (`main`'s rule) | 3: `tb/aecp_notify` DR1, DR2, DR3 |
| `dereg_pending_stops_follow` | the counter stamp stops following while a DEREGISTER is pending (review R477-1 S2 on PR #159) | 1: `tb/aecp_notify` DR3 |
| `dereg_lost_at_round_end` | the round's end drops the held DEREGISTER | 2: `tb/aecp_notify` DR1b, DR2b |
| `avb_domain_term_dropped` | `srp_evt_domain_change_w` removed from `ev_avb_i` (issue #42 acceptance 3) | 4: DN1b, DN1c, DN2b, DN2c |
| `avb_link_term_dropped` | the `link_up_i` edge removed from `ev_avb_i` | 4: DN4b, DN4c, DN4d, DN4e |
| `asp_takes_domain` | `ev_asp_i` takes DOMAIN_CHANGE too | 2: DN1b, DN2b (a GET_AS_PATH follows each GET_AVB_INFO) |
| `avb_notify_not_interface` | the GET_AVB_INFO job names CLOCK_DOMAIN 0, not AVB_INTERFACE 0 | 4: DN4c, DN4e, DN1c, DN2c (SUCCESS, cdl 32: the face's answer for another descriptor is empty) |
| `domain_same_readopted` | `KL_srp_domain` adopts an identical declaration (the `!=` dropped) | 2: DN3, DN3b |
| `adoption_no_strobe` | the adoption arm raises no DOMAIN_CHANGE | 6: DN1, DN1b, DN1c, DN2, DN2b, DN2c |
| `revert_strobes_at_defaults` | the LINK_DOWN revert strobes from DEFAULTS too (`if (adopted_r)` dropped) | 1: DN4 |
| `registry_never_claims` | a REGISTER never claims a free row | 9: DN0, DN4b-DN4e, DN1b, DN1c, DN2b, DN2c |
| `restore_never_done` | `restore_done_o` tied 0 | 1: the bench's boot premise ("notify bench: blank NVM, both restore walks reach done") |
| `port_not_compared` | the registry walk matches {eid, mac} without the port (#69) | 7: `tb/aecp_notify` PT2, PT4, PT3, PT5, PT6, CA1, CA2 (5 before round 2: PT2 to PT7 but PT5) |
| `port_not_latched` | the op's port latched as 0 | 12: `tb/aecp_notify` PT2, PT4, PT3, PT5, PT6, PD1 to PD3, CA1 to CA4 (5 before round 2) |
| `port_not_stored` | a claimed or refreshed row stores port 0 | 4: `tb/aecp_notify` PT3, PT5, PT6, PT7 |
| `avb_counter_row_dropped` | AVB_INTERFACE 1's change sets no slot | 2: `tb/aecp_notify` CK1, CK3 |
| `avb_counter_row_collapsed` | AVB_INTERFACE 1's change sets AVB_INTERFACE 0's slot | 3: `tb/aecp_notify` CK1, CK2, CK3 |
| `avb_counter_named_clock` | AVB_INTERFACE 1's slot named CLOCK_DOMAIN 0 | 2: `tb/aecp_notify` CK1, CK3 |
| `avb_counter_any_index` | the map takes any AVB_INTERFACE index into index 0's slot | 5: `tb/aecp_notify` CK1 to CK5 |
| `avb_counter_name_overlaps_clock` | the slot naming starts at interface 0 | 1: `tb/aecp_notify` CK5 |
| `rgy_port_tied_zero` | the top ties `u_notify`'s `rgy_port_i` to 0 | 2: IF3, IF3b (seventh build) |
| `depth_shared` | a REGISTER claims any free row, not one of its own port (#69 round 2) | 6: `tb/aecp_notify` PD1, PD2, PD3, CA1, CA2, CA1b |
| `depth_not_keyed` | the registry holds `N_CTRL_P` rows in all | 8: `tb/aecp_notify` PD1 to PD3, CA1, CA2, CA1b, CA3, CA4 |
| `registry_tag_port_bits` | a TIME_LIMITED arm's owner tag takes the row's port bits | 1: `tb/aecp_notify` PD2 |
| `monitor_tag_port_bits` | a monitor arm's owner tag takes the row's port bits | 1: `tb/aecp_notify` PD2 |
| `expiry_port_dropped` | an expiry is decoded to the port-0 row of its tag's index | 4: `tb/aecp_notify` PD3, CA1, CA3, CA4 |
| `cancel_one_per_command` | a cancel not sent in its cycle is dropped (review R512-1 F1, probe P3) | 2: `tb/aecp_notify` CA1, CA1b |
| `report_fail_ignores_probe` | a failure is taken for its owner's last row, live probe or not | 2: `tb/aecp_notify` CA2, CA3 |
| `report_rsp_ignores_probe` | a response is taken likewise | 1: `tb/aecp_notify` CA2 |
| `owner_turns_dropped` | a probe no longer waits while its CA owner is held | 2: `tb/aecp_notify` CA3, CA4 |
| `settle_dropped` | no settle after a cancel | 1: `tb/aecp_notify` CA4 |
| `rgy_port_from_latest_frame` | the top takes `u_notify`'s `rgy_port_i` from the latest received frame's interface, `hdr_if_r` (review R512-1 F2, probe P1) | 2: IF3, IF3b (seventh build) |
| `dereg_matches_other_port` | a DEREGISTER matches the other port's entry (review R512-1 F2, probe P2) | 1: IF3b (seventh build) |

RN and `tb/originator` R are the suites whose mutation records #80 and #86 ask for:
every RND control is killed by the divergence check alone.

**Retained controls, which survive by design** (review R420-1 S1; re-run at the round-2
head with the reviewer's own driver, all three SURVIVED with every section-ID check
passing). They are kept as defensive structure and are not graded:

| Control | Planted | Why no check kills it |
|---|---|---|
| `x_ident_arm_ignores_core_arm` | the identify arm stops yielding to the registry machine's arm sites (`&& !core_arm_w` dropped) | a collision needs a registry op to arm (N_APPLY, or N_IDLE with a new op) in the very cycle the identify arm is owed, the one after a frame's departure; a directed check would have to sweep a command's arrival cycle by cycle |
| `x_ident_rearm_single_generation` | the REARM generation never flips | a stale REARM can only fire in the cycles between a first frame's departure and the new REARM arm landing (a few cycles), and the departure itself clears `fired_r` |
| `x_ident_no_sync_second_flop` | the synchroniser loses its second flop | CDC hygiene: metastability is invisible to a two-state simulation |

## Section IF: two AVB interfaces (issue #69)

The seventh build (`make interfaces` alone, `obj_if2`) defines `PP_TOP_IF2`: the wrap
sets the top's `N_AVB_IF_P` to 2 and connects `rx_if_index_i`, which every other
build leaves unconnected, as a one-interface integration does. `interface_phases.hpp`
runs section IF alone on a NotifyBench processor (its own model, the suite's image,
erased NVM, both restore walks, link up and enable), and reads each interface's
advertise state through the wrap's `dbg_adp_adv_state_o`. A frame's interface is
driven with its bytes alone: from the clock after its last byte the port names the
other interface, which the contract allows, so a top that read it any later takes the
wrong one.

- **IF1** each advertise machine sends its own ENTITY_AVAILABLE inside
  T-ADP-DELAY-START, byte-exact with interface_index 0 and 1 and available_index 0.
- **IF2** (twice, interface 1 then 0) with both machines WAITING, ENTITY_DISCOVER
  received on one interface restarts that interface's machine alone, which
  advertises inside T-ADP-DELAY: the frame's interface rides the header beat to ADP.
- **IF3** the registry port comes from the command's interface, not the latest
  frame's. C registers on interface 0 and D's lock reaches it at sequence_id 0.
  With the MAC TX held, D's unlock queues a push to C that cannot leave, so the
  notification block holds the engine's command path; C's REGISTER comes in on
  interface 1 and an ENTITY_DISCOVER for another entity on interface 0 after it,
  and then the TX resumes. The unlock reaches C once, at 1, before the REGISTER's
  response, and the REGISTER, run after the interface-0 frame, makes interface 1's
  entry: the next lock reaches C at 0 and 2, the unlock at 1 and 3.
  **IF3b** with D's next lock held the same way, C's DEREGISTER comes in on
  interface 1 and a frame on interface 0 after it: the lock reaches both entries,
  at 2 and 4, before the DEREGISTER runs, and the unlock then reaches C once, at
  interface 0's 5. The entry removed was interface 1's, whose sequence_id was 3.

`make if-guards` (run by `make`, like `line-guards`) lints the real top with
`scripts/lint_hdl.sh`'s flags at `N_AVB_IF_P` 1 and 2, which must lint clean with no
warning at all, and at 0 and 3, which the top must refuse by name
(`N_AVB_IF_P=0 is outside 1 to 2`, `...=3 is outside 1 to 2`). It is the seam's
lint-only elaboration at two interfaces, on every run of the suite.

The controls that collapse the top's interface count, index or latch, or move its
range guard (`if-top-count-collapsed`, `if-top-count-collapsed-lint`,
`if-top-ingress-collapsed`, `if-top-ingress-live`, `if-top-range-unguarded`,
`if-top-range-floor-off-by-one`, `if-top-range-floor-dropped`) are arms of `tb/adp_engine`'s
campaign, whose README carries their record; `rgy_port_tied_zero`,
`rgy_port_from_latest_frame` and `dereg_matches_other_port` are in `notify_mutants.py`,
recorded in the table above.
