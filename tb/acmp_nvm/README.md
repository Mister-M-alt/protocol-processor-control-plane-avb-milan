<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# acmp_nvm — KL_acmp_nvm_shadow persistence suite

Proves the ACMP binding NVM shadow (`hdl/acmp/KL_acmp_nvm_shadow.sv`,
[05 §5](../../docs/architecture/05_acmp_engine.md) ≈20 B/sink shadow +
[07 §5](../../docs/architecture/07_memory_maps.md) F07.8/F07.9 +
[02 §8](../../docs/architecture/02_interfaces.md) F02.8): `make` = build + run,
exit 0 = PASS, 349 checks. `-GDEB_TICKS_P=50` pins the debounce window the C++
timing mirrors (tick_i is held high, so window = 50 cycles), and
`-GRS_TMO_CYC_P=3000` the walk's read deadline (`T-NVM-RS-DEADLINE`) that group N
places its boundaries against.

The wrap compiles the shadow together with the REAL `KL_pp_nvm_port` (class-F
manager face) behind the REAL `KL_pp_nvm_mgr_arb` (the shadow is its manager 0;
manager 1 is a harness face, as the platform's saved-state writer will be), the
REAL `KL_pp_acmp_listener` (capture from its record write
port, boot replay into its `pre_*` preload face) and the REAL
`KL_pp_acmp_lsn_admit` between the listener's four work faces and their
producers, wired as `protocol_processor_top` wires them — face compatibility is
proven by elaboration, not transcription. The harness plays the producers
(dispatch queue head, event router, AECP START/STOP request, timer expiry), each
holding a presented request until its producer-side handshake, and the
listener's RX-slot and TX-slot pools. The harness plays the physical NVM
behind the port's device face (region store, grant/completion delays,
per-byte stalls, targeted error injection) and independently re-implements
the record contract: crc16 CCITT-FALSE, big-endian 16-bit header fields, and
the 20-byte BINDING payload `{flags[valid,started,sw], rsv, talker_uid,
talker_eid, ctlr_eid}` transcribed from the docs, never from the RTL.
Injected F07.6 record images (volatile fields loaded with junk) stand in for
executor write-backs: settle, unbind, started-change, and volatile-only
churn.

Covered: write-through on CHANGE only (volatile-field write-backs — probe
bookkeeping, GET_RX-style — cost zero NVM traffic); T-NVM-DEBOUNCE
coalescing (three changes in one window → one ERASE+WRITE burst of two
records, byte-exact against the model, then quiescence); unbind rewrites the
record with a valid=0 payload; bounded commit retry (recovered error never
alarms) then the sticky side-port alarm with the engine still serviceable;
`restore_blank_o` separating a walk that validated records from one that
read blank or unframed media (`restore_done_o` is set on BOTH, which is
why the pin exists) and following the image rather than the history when
a tear discards records already taken;
boot replay driving `pre_*` for exactly the valid sinks in ascending order —
fields checked at the accept AND against the PRB_W_AVAIL record the listener
then writes (started/sw/eids exact, discovery armed per A4, replay
write-backs compare-equal so nothing re-dirties); per-record vendor defaults
that never abort (empty region, bad crc, wrong layout_version, wrong
payload_length); a TORN mid-record read-back (device error after 5 payload
bytes) aborting the WHOLE restore — `restore_fail_o`, not one preload
driven, the image discarded, the walk stopped, live captures still commit
after; change-during-restore ordering both ways (capture before AND after
its sink's record was walked: the capture wins, its sink is never preloaded,
the live value is flushed back); change-during-flush (the taint path: a
capture landing mid-serialization keeps dirty and the burst re-serializes —
NVM converges to the newest value); and group X, the contract
`protocol_processor_top` exports as `nvm_unflushed_o` (issue #90) — the bit
rises on the ACCEPTED change and on that sink only, holds through the
debounce and the burst, falls on the commit's manager-face `done` (graded on
the cycle, which is why the wrap publishes `dbg_port_done_o`: the device
face's own done is two cycles earlier), and on retry exhaustion falls on the
SAME cycle `alarm_o` rises, never silently.

Group L — the boot window and the listener's admission (issue #92, and the
S4 admission of issue #93). Each case boots on the saved bindings of the FIRST
(0) and the LAST (7) sink and grades what the listener TOOK and ANSWERED and
what the device ends up holding, never a timeout: the walk completes, both
saved bindings are preloaded, each in the cycle it is offered, and written
bound as saved; the release follows the last preload's record write and
discovery arm, within four cycles of the manager's terminal; while the gate
owns the faces the listener visits only `X_INIT`, `X_IDLE` and `X_PRELOAD`,
takes nothing at a work face and raises no side effect (timer arm, PRNG draw,
TX allocation, RX free, settle, teardown, disarm, notify, START/STOP
completion); every request is popped by its producer in exactly the cycle the
listener takes it, at or after the release; no sink is marked touched.
- **L00** control, no traffic in the window; after the release both sinks
  answer `GET_RX_STATE` with their restored binding; a reset restores both.
- **L05a-f** a read-only `GET_RX_STATE` in the window: of the first sink
  between its store and its preload, of the later sink likewise, of the first
  sink at the later sink's store, of the later sink at the first sink's store,
  five consecutive GETs of both sinks, and one of the later sink as the first
  sink's preload is offered. Every reply is the restored binding byte-exact
  (Milan Table 5.37), nothing is written to NVM, both saved records stay
  byte-exact, and a reset restores both.
- **L05s** the same GET, of either sink, presented at EVERY cycle from the
  reset's release through `restore_go_i`, the whole walk and its release, one
  boot per cycle.
- **L09** live changes stay ordered: a BIND of the first sink to another
  talker held from reset lands after the restore, answers SUCCESS, is
  committed once and restored by the next reset; an UNBIND of the later sink
  presented at the first sink's store commits a `valid = 0` record and the
  next reset restores only the first sink; a queue of GET, BIND, GET, UNBIND,
  GET at the first sink's store is answered in queue order, each reply on the
  state its predecessor left, and NVM ends at the queue's last word.
- **L10** a GET presented in the gate's last owned cycle, its first released
  cycle and the one after, placed against the release a control boot
  measured: taken once, in the release cycle or its own.
- **L20-L22** reset boundaries: a GET held in the window across a reset
  (taken once, after the NEXT walk's release); a power cut inside the preload
  phase writes nothing and the next boot restores both; a BIND held across a
  reset lands once after the second walk and persists.
- **L01-L03b** the talker-event face (issue #93, S4): a talker-event level
  held from reset for ever (R217 R3-F1's: a droppable event the listener
  acknowledges and drops, re-raised at once), a finite level from reset, the
  level raised against the later sink once the first sink's preload is taken,
  and a queued `EVT_TK_DISCOVERED` of the first sink at the same point. The
  walk and every preload are unaffected, the router's acknowledge equals the
  listener's take in every cycle and none comes before the release, the
  discovered event is taken exactly once, and both restored bindings answer
  after the release.
- **L04** `GET_RX_STATE` polled from reset, alternating the two sinks: four
  answers after the release, in queue order, each the restored binding.
- **L06-L06c** the START/STOP face: a STOP of the first sink held from reset,
  a START then a STOP of it back to back, a START of the later sink against its
  preload. Nothing is captured and no completion fires while the gate owns the
  faces; each request completes once, in order, after the release; NVM holds
  the started state the requests left and a reset restores it.
- **L07, L07b** the expiry bus: an expiry of the first sink's owner every cycle
  from reset, and of the later sink's owner every cycle from the first
  preload on. None reaches the listener while owned, every one is counted
  (`dbg_exp_drop_o` equals the number presented), and the bus reaches the
  listener again after the release.

Group N — how the walk ends (issue #93, S1 and S3). A read that ends with
nothing forwarded is an empty record only when the port names the err
UNFRAMED (or the read ends in a clean done); a DEVICE err there fails the whole
walk. The read phase is bounded by `RS_TMO_CYC_P` cycles without progress, and
an expiry abandons an issued read to the arbiter, which drains it. Every failed
walk is graded whole: `restore_done_o` and `restore_fail_o` with the named
`restore_cause_o`, no preload offered and no restored binding kept, blank, the
release within four cycles, no work while owned, nothing written.
- **N1a-d** cause 2: a device error at the first record's header read, inside
  a middle record's header, the last record's header read ended short at five
  bytes, a device error after the first header and before its done. The walk
  stops at the failing record and the released listener answers the vendor
  default.
- **N2a-b** an erased middle record (eight 0xFF) and one whose magic is
  corrupt: that record's default, and the walk completes past it with both
  saved bindings (A2, F4 and G2 grade cause 0, 0 and 1 on their own paths).
- **N3** a middle record's header read granted and never answered: cause 3
  within the deadline of the silence, the read abandoned exactly once; the
  listener answers on the defaults, the port stays quarantined, and a later
  change stays pending, never written.
- **N4** the port held by manager 1's read, which the device never answers:
  the walk waits for the port idle, issues no read of its own, abandons
  nothing, and fails at its deadline with cause 3.
- **N5** the boundary, placed against the lag a control boot measures between
  the device's first header byte and the walk's: the walk's first byte in the
  last cycle before the expiry completes the walk (the stall count reads
  `RS_TMO_CYC_P - 1`); one cycle later fails it with cause 3, the late read's
  bytes move in the drain and none reaches a manager, the next operation is
  issued only after the drain ended, and a later change persists. **N5c**: the
  device ends the abandoned read 2,000 cycles after the deadline, with a live
  change waiting the whole time: the change stays pending while the port is
  drained and is issued only after the device ended the read; a reset
  restores all three bindings.
- **N6** the arbiter's grant-cycle rule: manager 1's read presented at every
  offset from the go to a control walk's terminal (200 boots). Every walk
  completes as saved and every manager-1 read is granted inside the walk and
  completes.
- **N7a-b** the talker-event level of L01 held from reset while the read
  phase runs to its deadline (S3 against S4): the walk's first byte in the
  last cycle before the expiry completes the walk with every preload taken in
  the cycle it is offered; a record read the device never answers fails the
  walk with cause 3, the level reaches the listener only after the release,
  and the listener answers the vendor default.
- **N8a-c** a failed walk rejects the image, not the media (issue #92 after
  #93's causes). Sinks 0, 3 and 7 are saved; the walk stores sink 0, then
  fails at sink 3 with cause 1 (its payload torn after three bytes), 2 (a
  device error at its header read) or 3 (its read silent, the device ending
  it 2,000 cycles after the abort); sink 7 is never read. A GET_RX_STATE of
  sink 0 held from reset through the window, and one GET of each saved sink
  polled afterwards, are answered on the vendor default in order; in N8b an
  UNBIND of sink 3, which the failed walk left unbound, is too. Nothing is
  written or left pending, every saved record is still in the device
  byte-exact, and a reset on a healthy device restores all three bindings.
- **N9a-c** an unwired device face, as the integrator guide's tie-off rules
  state it: a face that never grants fails the walk at its deadline with
  cause 3, its one request abandoned and the port left quarantined; a face
  that answers every READ with err fails it at the first read with cause 2;
  either way the released listener answers the vendor default and nothing is
  written. A face that answers every READ as erased media (every region
  0xFF) ends the walk done, not failed and blank, one header read per sink,
  nothing preloaded or written: the same two levels as a full restore.

Pinned wiring: `make pinned` builds the same bench with the gate left out
(`ACMP_NVM_PINNED_WIRING`, the producers wired straight to the listener as the
top was before issue #92) and exits non-zero. It is the reproduction of the
recorded L05 control: 107 of 349 checks fail, among them L05a's "sink 0
holds 000000000000000000000000" (the listener's unbound record flushed over
the saved binding), the unbound reply and the reset that no longer restores
it; L05s finds 193 presentation cycles of sink 0 and 202 of sink 7 answered
unbound, and L01's talker-event level leaves both preloads untaken and both
records flushed unbound. Its failures are assertions on completed scenarios.

Known limits (honest): the BINDING record id allocation (`REC_ID_BASE_P` =
0x20) and the exact payload byte layout are design decisions of the shadow's
banner — 07 §5.2 names the BINDING[i] record but pins neither; the suite's
model transcribes the banner contract. Captures arriving between reset and
`restore_go_i` (other than the all-zero init sweeps) are a boot-sequencing
violation (07 §5.3 restores before entity_enable) and are not defended
beyond the restore span. crc polynomial (CCITT-FALSE) matches the
`tb/nvm_port` manager BFM so both suites pin one on-media format.

Mutation-proven 2026-08-11 (backup → sed → run → restore → green):
- **M1** debounce window collapsed to one tick (`deb_cnt_r <= 32'd1`):
  fails 2 of 73 (B4 one-burst coalescing, B5 op-pair shape).
- **M2** torn read-back no longer sets the abort flag (`fail_r <= 1'b0`):
  fails 1 of 73 (G2 restore_fail).
- **M3** serializer streams ctlr EID in the talker EID byte lanes:
  fails 8 of 73 (every byte-exact store check, B6/B7/E2/E7/G7/H9/H10/I2).
- **M4** change detection dropped (`c1_wr_w = c1_v_r`): fails 4 of 73
  (C2 volatile churn, D3 op count, F16/F17 replay must not re-dirty).
- **M5** atomic reject keeps the restored valid bits: fails 1 of 73 (G4).

Mutation-proven 2026-08-13 for the blank arm:
- **M6** `restore_blank_o` hard-wired to `1'b0`: fails 2 of 76 (A2b empty
  NVM, G4b atomic reject), and 1 more in the consumer suite
  (milan-fpga `tb/verilator/pp_shadow`, `PP_STAT[7]`).

Mutation-proven 2026-09-23 for the listener admission (issue #92), each on
`KL_pp_acmp_lsn_admit` at 190 checks:
- **LG01** the gate deleted (`own_r` resets to 0): fails 58 of 190, the
  pinned-wiring set above.
- **LG02** the transaction's valid admitted while owned, its ready masked:
  fails 52 of 190 (the listener takes the head every cycle it is idle while
  the producer never pops: 561 takes, no release in L05a).
- **LG03** the transaction's ready passed while owned, its valid masked:
  fails 38 of 190 (the producer pops a head the listener never took: the
  command is lost, no reply).
- **LRdone** the release no longer waits for the walk's terminal: fails 58
  of 190, the LG01 set.
- The `pre_valid`, `busy` and `arm` release terms are NOT graded here: the
  real manager raises its terminal one cycle after its last preload was taken,
  when all three are already clear. `tb/lsn_admit` grades each on its own.

Mutation-proven 2026-09-24 for issue #93, first at 332 checks and measured
again at 349 once N8 and N9 were added (receipts under the lane's output
directory, one log per mutant); the counts below are at 349:
- **LG01** the gate deleted: fails 107 of 349, every L case and, through the
  early release, N2-N9a as well. **LRdone** fails 108.
- **LG02** / **LG03** the transaction's valid admitted, or its ready passed:
  fail 71 and 45 of 349.
- **LG02t** the talker event's valid admitted while owned: fails 19 of 349
  (L01-L03b, N7a-b: the listener takes the level every idle cycle, 80,501
  takes in L01, and the walk never ends). **LG03t** its ready passed: fails 7
  (the router's acknowledge no longer equals a take, and L03b's event is
  lost).
- **LG04** expiries admitted: fails 5 of 349 (L07, L07b: the expiries reach
  the listener in the window, hold L07's first preload offer untaken for
  19,811 cycles, and their write-backs flush a saved binding unbound).
- **LG05** START/STOP admitted: fails 12 of 349 (L06-L06c: the request is
  captured and completed while the gate owns the faces, before its sink's
  preload, so NVM does not end at the state the request left and the next
  reset does not restore it).
- **B01** no read deadline: fails 20 of 349 (N3, N4, N5b, N5c, N7b, N8c,
  N9a: the walk never ends). **BA1** the abort never raised: fails 15 (N3,
  N5b, N5c, N7b, N9a: the late read reaches the manager and wedges the port).
- **B02** a zero-byte err read as blank whatever its cause (processor issue
  20's defect, both of its edits): fails 12 of 349, N1a-d, N8b and N9b.
  **B04** an UNFRAMED err failing the walk: fails 120 of 342, A2, F4 (the
  blank first boot) and N9c among them.
- **BC1** the walk's cause collapsed to torn: fails 13 of 349 (N1a-d, N3, N4,
  N5b, N5c, N7b, N8b-c, N9a-b).
- **B03** the abandoned read never drained (`KL_pp_nvm_mgr_arb`): fails 10 of
  349 (N3, N5b, N5c, N9a: the port never serves again).
- **A01** the arbiter's grant-cycle busy term deleted: fails 1 of 349, N6
  (92 of 200 offsets lose the walk's request and end at the deadline).

Mutation-proven 2026-09-24 for the failed walk's saved records (issue #92
after #93's causes, PR #109 review), at 349 checks:
- **F1-compare** the capture compare back to its form before the unbound
  rule (`c1_diff_w` true on any field difference, so two unbound records can
  differ): fails 9 of 349, N8a-c, three each. After every cause the GET held
  through the window writes sink 0's record unbound (1 erase and 1 write,
  `sink 0 holds 000000000000000000000000`), and the next healthy reset
  restores two bindings of three. The whole suite is otherwise green: no
  other case depended on unbound records differing.

Mutation-proven 2026-09-20 for the unflushed export:
- **M7** `dbg_dirty_o` hard-wired to `'0` — the pin issue #90 exports:
  fails 12 of 86, X1, X1b, X2, X3, X3b, X4 and X5b by name (B2 and the
  four byte-exact store checks go with them, because the suite waits on the
  same pin to know a burst drained).
