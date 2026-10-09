<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# acmp_listener — KL_pp_acmp_listener MTXW suite

Proves the ROM-driven Milan listener-SM executor
(`hdl/acmp/KL_pp_acmp_listener.sv`) against the full F05.3 transition matrix of
[05 §6.3](../../docs/architecture/05_acmp_engine.md): `make` = generate the
ROM + build + run, exit 0 = PASS, 3167 checks.

**The MTXW walk** ([09 §3](../../docs/architecture/09_verification.md)):
every one of the 112 cells (14 events x 8 states) is driven against an
**independent C++ matrix model transcribed from the doc table without
`gen_ltn_rom.py`** — two transcriptions of F05.3 that must agree through the
DUT's behavior; that is the point of the walk. Each sink is brought to the
cell's column state through legitimate stimulus only (no state backdoor),
then the row's physical event is applied and *every* observable compared
against the model: the full 384-bit F07.6 record write-back (shadowed from
the write port), committed 56-byte Milan ACMPDUs byte-exact (BIND/UNBIND/
GET_RX_STATE responses per F05.14, PROBE_TX per A5/A13), timer arm/cancel
ops with exact F08.1 deadlines (T-ACMP-CMD 200 ms, RETRY 4 s, NOTK 10 s,
DELAY = the scripted PRNG draw), the A15/A8/A4/A9/NVM/notify strobes with
payloads, and the RX-slot free.

Every `—`/`ign` cell is **proven inert** (state unchanged, no strobes, no
frames, no timer ops, no notification). Timer-row `—` cells split honestly:
states with no armed SM timer (UNB/PWA/SOK, 12 cells) get a spurious expiry
injected and full inertness checked; the 15 cells whose T-ID aliases the
shared per-sink slot (08 §5) are proven impossible by construction — the
suite checks the deadline armed on entry equals the state's *own* T-ID, so
the foreign expiry cannot exist — plus the model transcription must mark
them `—`. The `ign (note)` cells additionally prove the tk_disc/tk_reg
bookkeeping still tracks (the dagger conditionals depend on it).

Behavior checks beyond the walk: exact-duplicate probe (A13 bytes == A5
bytes, same `probe_seq`, `retried` set), double timeout -> `acmpsta` 7, both
arms of all three dagger cells, the **REF-BUG guard** (TalkerFailed rise in
SETTLED_RSV_OK takes no invented arc — the Table 5.30 'x' the doc pins),
REGISTERING_FAILED visible in GET_RX_STATE, LISTENER_UNKNOWN_ID for the
three commands + silent drop for probe responses, foreign-EID and
foreign-protocol silence, probe-response guard mismatch silence, the A1
lock gate (foreign holder blocked, holder passes, GET_RX_STATE unaffected),
boot preload -> PRB_W_AVAIL with discovery armed (07 §5.3), the
A11-swallows-stale-expiry race (a pending T-ACMP-CMD expiry must not tear
down a settlement that just landed), and cross-sink record isolation.

The harness emulates the four **landed** faces cycle-exactly: KL_pp_rx_slots
(sync read, one-cycle latency), KL_pp_tx_slots (request sampled, grant
pulsed the following cycle), KL_pp_timer_service (arm/cancel bus, owner-
tagged expiries), KL_pp_prng (busy then a one-cycle valid draw).

Mutation-proven 2026-08-11 (backup/sed/run/restore):
- **M1** `gen_ltn_rom.py`: drop A8 from the `BIND_NEW x SOK` cell -> 3 of
  2458 FAIL (missing teardown strobe, stale settled params survive rebind).
- **M2** RTL: invert the dagger tk condition (`!f_tk_disc` -> `f_tk_disc`)
  -> 21 FAIL (all three cond cells take the wrong arm in both directions).
- **M3** RTL: A11 no longer clears the pending-expiry bit -> 1 FAIL (the
  stale T-ACMP-CMD expiry surfaces after settlement and tears it down to
  PRB_W_AVAIL). Honest note: the first version of this scenario watched a
  20-tick window — shorter than the ~24-cycle retirement of the second work
  item — and the mutant survived vacuously; the window is now 60 ticks of
  required silence with the collector cleared. A surviving mutant proved
  the check wrong, not the design right.
- **M4** RTL: same-bind classification disabled (every BIND treated as
  new/different source) -> 389 FAIL (the whole BIND_SAME row re-probes and
  churns SRP instead of the v1.2 A6 short-circuit).

Messages outside the listener set, and the probe-response guard per term
(issue #47, REQ-ACMP-012, Milan §5.5.3.1). **B13** drives every ACMP
message type the listener does not own — 3, 5, 7, 9, 11, 13 (the responses
of IEEE 1722.1-2021 Table 8-2, Table 8.1 in IEEE 1722.1-2013, that are not
PROBE_TX_RESPONSE) and 14, 15 (reserved) — with the own listener_entity_id
and a valid listener_unique_id, in PRB_W_RESP with status SUCCESS and in
PRB_W_RESP2 with TALKER_NO_BANDWIDTH. Each is shaped as the perfect answer
to the outstanding probe (all four guard terms equal to the saved probe,
stream fields set), so only its message type keeps it out; each must be
fully inert: no frame, no record write, no timer op, no action strobe, no
notify, the record unchanged, and exactly one RX-slot free of the slot it
arrived in. **B14** grades the
§5.5.3.5.18 / .25 step-1 guard term by term: a response with the wrong
controller_entity_id, talker_entity_id or talker_unique_id is ignored in
both probing states exactly as B8's wrong sequence_id is, and the unaltered
response then settles, so a guard that rejected everything cannot pass.

Mutation-proven 2026-09-29 by `tb/pp_top/acmp_mutants.py`, each mutant in its
own extract of the tree, KILLED only when every named check fails:

| Mutant | Defect planted | Named checks | Result |
|---|---|---|---|
| `msg_ok_forced` | `txn_msg_ok_w` forced to 1: every message type is classified, the others through the PROBE_TX_RESPONSE default arm | B13 msg 7 / 14 in PWR, msg 3 / 15 in PW2 | 93 of 2988 FAIL: all 16 B13 arms settle (SUCCESS) or back off (NO_BANDWIDTH) |
| `guard_ctlr_dropped` | the controller_entity_id term of `probe_match_w` tied true | B14 wrong controller_entity_id in PWR / PW2 | 50 of 2984 FAIL |
| `guard_talker_eid_dropped` | the talker_entity_id term tied true | B14 wrong talker_entity_id in PWR / PW2 | 40 of 2984 FAIL |
| `guard_talker_uid_dropped` | the talker_unique_id term tied true | B14 wrong talker_unique_id in PWR / PW2 | 30 of 2984 FAIL |

The same `msg_ok_forced` edit also fails `tb/pp_top` AI3 (the top-level leg,
recorded there).

Record storage (issue #639). The records live in distributed RAM with no read
register: the walk reads a record asynchronously in X_LATCH or X_STRT_AP, the
state after its read-issue state, and nothing writes a record in between. The
array has no reset, so the X_INIT sweep is its only one. **RS** grades that
sweep through the RAM rather than the write port this suite shadows: with
records bound at the end of the walk, a reset is taken, and a GET_RX_STATE on
every sink must then answer the unbound record, and the record its walk
writes back must equal the model's unbound one (123 checks, which is why the
tally moved from 2988).

Mutation-proven 2026-10-04 by `tb/pp_top/acmp_mutants.py` (issue #639 group),
each mutant in its own extract, KILLED only when every named check fails. The
four controls above fail the same counts as recorded, now of 3111 and 3107
checks.

| Mutant | Defect planted | Named checks | Result |
|---|---|---|---|
| `rec_read_sink_zero` | the walk reads sink 0's record whatever the sink | B12 parked sink 7 untouched; F05.3 BIND_SAME x PWA sm_state | 645 of 3080 FAIL |
| `rec_read_in_idle` | the record read through a register sampled in X_IDLE, the cycle before the read-issue state (the stale form of a removed read register) | RV8(setup) state sync | 1 of 3111 FAIL |
| `rec_started_unstored` | the RAM never stores `f_started` (record bit 12) | S1d STOP through the request face; F05.3 GETRX x PWA flags | 91 of 3111 FAIL |
| `rec_settled_vlan_unstored` | the RAM never stores `settled_vlan` bit 1 (record bit 305) | F05.3 GETRX x SOK settled; B12 | 25 of 3111 FAIL |
| `rec_sweep_misaddressed` | the X_INIT sweep writes the walk's sink instead of each record in turn | RS binding | 37 of 3111 FAIL; before RS it survived (0 of 2988) |

Re-bind started/stopped trigger (RV8, issues #43/#49): a BIND_NEW onto a bound
sink with STREAMING_WAIT flipped (A2 without A10) raises `act_strt_chg_o`
exactly once, one cycle after its record write for that sink (the edge the
top's pbsta/acmpsta compare registers on, so the two OR into one
notification); a re-bind that keeps STREAMING_WAIT raises none. Mutation-proven
2026-09-24 in a scratch copy: excluding walks that run A2 from the trigger
again → 2 of 2544 FAIL (RV8b, RV8d).

## ACMP response fields (issue 168)

The matrix expectations follow Milan v1.2 Table 5.36 (successful UNBIND clears
both talker fields), 5.5.3.5.30 step 2 and 5.5.3.5.10 (the discovered retry and subsequent
delay expiry retain ACMP status),
and IEEE 1722.1-2021 Table 8-3 (lock refusal status 16). VLAN settlement and
readback retain all 16 received bits under Milan 5.3.8.9 and Table 5.38.

`field_cases.hpp` adds 56 checks through the transport harness. Its probe
identity comes from the emitted command. Same-talker rebind updates the binding
controller while responses and the byte-identical retry still use the sent
probe's controller (5.5.3.5.16 step 1, .17 step 2 and .18 step 1).
The private record RAM shares its stream-ID word between pending-probe controller
and settled stream-ID lifetimes. The published record reports zero stream-ID
while unsettled, and the binding controller remains independently current.

The issue 168 controls in `tb/pp_top/acmp_mutants.py` require these witnesses:

| Mutant | Required failing check |
|---|---|
| `unbind_talker_echo` | LD1 zero talker fields |
| `retry_status_cleared` | LD2 retained error at the retry-delay transition |
| `retry_probe_status_cleared` | LD2 retained error when the delay expires (5.5.3.5.10) |
| `lock_status_13` | LD3 status 16 for both BIND and UNBIND |
| `lock_gate_bypassed` | LD3 binding unchanged after both refusals |
| `settled_vlan_truncated` | Full VLAN in GET_RX_STATE |
| `settlement_vlan_truncated` | Full VLAN on the settlement action |
| `probe_guard_current_controller` | Accept the sent controller and reject its replacement |
| `probe_retry_current_controller` | Byte-identical retry after rebind |

Every control must build, finish the suite and fail its named assertion;
compilation failure is not a killed defect. Historical mutation counts above
remain the results at their stated dates.

`field_reset_blocked` prevents the initialization sweep from completing and
must fail the new transport/setup checks as well as the named frame-presence
checks. This grades the setup itself rather than treating a skipped field
comparison as coverage.

### Round 2 response selection

The outstanding-probe controller comparison selects between equality results.
Other states retain the binding-controller comparison and transaction classification.
Unbind leaves the private controller word for its next writer; published
unsettled records and GET_RX_STATE still return a zero stream ID.
The response builder selects the controller octet before its source.
These changes retain Milan 5.5.3.5.16/.17/.18 and 5.3.8.9 behaviour. Existing
clause checks and the required failures of every planted control are retained.
