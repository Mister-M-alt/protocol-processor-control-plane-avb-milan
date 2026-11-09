<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# srp_top — KL_srp_admission + KL_srp_top end-to-end engine suite

Proves the assembled SRP engine (`hdl/srp/KL_srp_top.sv` wiring
KL_srp_decoder → {Domain, VLAN, talker/listener FSMs} → KL_srp_encoder,
with `hdl/srp/KL_srp_admission.sv` feeding the talker plane) against
[10 §1/§2/§3/§6/§7/§9](../../docs/architecture/10_srp_engine.md) and the
[02 §4.1](../../docs/architecture/02_interfaces.md) `srp` contract —
**through the REAL shared blocks**: frames are pulled byte-by-byte off a
real `KL_pp_tx_slots` serialize face (the C++ side plays the 03 §8 TX
arbiter), cadence and registrar-leave timers run on a real
`KL_pp_timer_service` (time-compressed: 1 ms = 40 clk, 32 slots), and the
T-MRP-LEAVEALL draws come from a real `KL_pp_prng` (kind 3, 10–15 s).
`make` = build + run, exit 0 = PASS, **8703 checks**. `make` first runs the
timer-arm FIFO arms at four shapes
([below](#timer-arm-fifos-at-four-shapes-issue-230)), then this suite, whose
tally is the last line.

Expectations are independent: an MRPDU builder/parser written here from
802.1Q §10.8.1.2 / §35.2.2, a Σ-slope model transcribing the Milan v1.2
§4.3.3.2 recipe from the silicon-measured reference
(`milan-fpga/hdl/ieee8021q/srp/KL_lwsrp_bw_gate.sv`: F = MaxFrameSize+22,
min-68 clamp, W = F+20, slope = W·MaxIntervalFrames·8000·8) with the
greedy stream-index-order walk against the 75 % port-rate ceiling, and
the Table 10-3 fresh-declaration ladder (New, New, JoinMt) cited from the
standard — never DUT logic.

Covered end to end:

- **Bring-up**: link-up → Domain default declaration `{6, 3, 2}` New on
  the wire, byte-exact, at the first T-MRP-JOIN drain, New again at the
  next, then JoinMt (802.1Q-2018 Table 10-3, issue #216), each byte-exact;
  class-D defaults.
- **Class-B service port** (02 §4.1 template): GET_DOMAIN {prio, VID}
  (class A only — class 5 FAILs), unknown op UNSUPPORTED, out-of-range
  indices FAIL; single-outstanding request/response contract throughout.
- **DECLARE_TALKER → admission → wire**: granted_slope_bps / sr_admitted /
  Σ / over_limit against the independent model; Talker Advertise New
  byte-exact (25 B FirstValue: stream_id, DA, VID, TSpec, prio/rank from
  the Domain + cfg, accumulated latency), the declaration ladder, and the
  MVRP VID New the membership join emits — all through the real slot pool.
- **Over-ceiling refusal (10 §6.3)**: a TSpec that pushes Σ past 75 % is
  refused → granted 0, over_limit, `tk_decl_state` FAILED, fail code 1 +
  own-MAC system id in class-D, and **Talker Failed code 1 byte-exact on
  the wire** (34 B FirstValue, §35.2.2.8.7 end-station MAC). Withdrawal of
  the blocking stream re-admits it: Advertise New replaces the Failed in
  place, over_limit clears.
- **Listener end-to-end**: DECLARE_LISTENER arms the exact matcher; a fed
  Talker Advertise MRPDU registers (TK_ATTR_REGISTERED, acc_latency
  latch) and the engine answers with a byte-exact Listener Ready New
  (FourPacked lane); the in-place Advertise↔Failed swap both ways
  (REGISTERED strobe, no unregister, failure info latched/gated,
  declaration follows Ready↔AskingFailed).
- **Certified two-class Domain arrival**: FirstValue `{5, 2, VID 5}`,
  NumberOfValues 2 — class A arrives as value 1 (802.1Q §35.2.2.9) —
  adopts (DOMAIN_CHANGE, class-D update, GET_DOMAIN agrees) and
  re-declares end-to-end: one Domain message, Lv `{6,3,2}` + New
  `{6,3,5}`, byte-exact.
- **LeaveAll per application (10 §6.5)**: the own MSRP leavealltimer
  (PRNG-drawn) emits a LeaveAllEvent PDU that flags every MSRP Attribute
  Type once, on its first vector (a NumberOfValues-0 vector for a type the
  cycle declares nothing of; 802.1Q §10.8.2.6), and whose cycle re-declares
  the Domain + Listener Ready. The Domain re-join is JoinMt: the own sLA ages
  our Domain registration to LV and no peer re-declares the Domain there
  (issue #184, 802.1Q-2018 §10.7.6.3). Registrations stay published through LV and a
  peer re-join keeps them (no unregister). A received MSRP LeaveAll that
  flags every MSRP type, as a conformant peer's does (F2), ages the
  registrar over a real 5 s T-MRP-LEAVE to TK_ATTR_UNREGISTERED and the
  Listener Lv reaches the wire — while MVRP membership is untouched.
  The MVRP participant runs its own LeaveAll (flagged PDU, VID re-join,
  never an Lv flap). The F4 peer cadence flags every type the same way.
- **Received LeaveAll routed per Attribute Type, end to end (F5; 10 §6.5,
  802.1Q-2014 §10.7.5.20 NOTE)**: each step starts right after an own MSRP
  LeaveAll, so the next own cycle cannot land in its 5.4 s window, and the
  peer answers it with Listener Ready on source 1 and Talker Advertise
  toward sink 0.
  - **(a)** the bench switch's LeaveAll MRPDU from Run B (LeaveAll in every
    message, Listener JoinMt/Ready first, Talker Advertise/Failed as
    NumberOfValues-0 vectors; this bench's stream_id and Domain VID): the
    Listener registration stays IN past T-MRP-LEAVE and source 1 stays
    ACTIVE, while the Talker Advertise lane ages sink 0's un-re-declared
    Advertise to TK_ATTR_UNREGISTERED.
  - **(b)** a Domain-only LeaveAll: our Domain re-declares (JoinIn at the
    next T-MRP-JOIN, well before any periodic re-join; the flagged vector
    re-declares the peer's Domain, so our registration is IN again at our
    tx!); neither the
    Listener registration nor sink 0's Advertise ages.
  - **(c)** a Listener-only LeaveAll, never re-declared: Ready stays
    published for T-MRP-LEAVE (LV), then ages to MT and ACTIVE drops; sink
    0's Advertise is untouched. The Domain row's negative: no Domain re-join,
    JoinIn or JoinMt, follows it before the next periodic re-join, nor, each in its own clean
    slot, a Talker Advertise LeaveAll (sink 0's Advertise re-declared in the
    flagged vector) or a Talker Failed-only LeaveAll.
- **Admission sweep**: 30 randomized declare/withdraw rounds across all 8
  sources vs the model — admitted vector, per-source granted slopes, Σ
  and over_limit, exercising the greedy order and capacity reuse.
- **Current-declaration grant (H, issue #112)**: sources 0, 1 and 7 at all
  eight slope-sampling phases. Cold refusal, 224 → 20000-byte growth,
  20000 → 224-byte shrink, and identical re-declaration are observed on
  every cycle from real service-gate acceptance. The refused source never
  grants; every admitted slope belongs to the current declaration; the
  grant is low on acceptance and only returns at round completion.
  Shrink/identical latency is 8/16/24 clocks from acceptance, printed per
  case. An actual Listener Ready PDU is paused before its packed events
  and completed just after acceptance, exercising optimistic ACTIVE before
  the real grant. The suite checks the three-published-round window, the
  ACTIVE equation, zero slope while unadmitted, and settled sum/refusal.
  The window length is printed per case: 25, 33 or 41 clocks, 17 past the
  round that publishes the verdict. No state or verdict is forced. The
  [unit suite](../srp_admission/README.md) covers smaller shapes, rapid
  changes, both TSpec fields and failing mutants.
- **A pending re-declaration frees no capacity (I, issue #112 round 2)**:
  source 0 (69.952 Mb/s) is admitted and ACTIVE. Source 1 or 7
  (15.488 Mb/s) is refused by the 75 Mb/s ceiling and declared Talker
  Failed, with a real Listener Ready. Source 0 then re-declares:
  - identically, or shrunk to 60.288 Mb/s. The other source stays refused.
    On every clock from acceptance it never grants, is never ACTIVE and
    never declares Advertise. Across two T-MRP-JOIN periods, no Talker
    Advertise vector for its stream reaches the wire.
  - shrunk to 17.024 Mb/s, the control that frees capacity. The other
    source's first grant is the same clock as source 0's re-grant, its
    Advertise follows, and its Advertise reaches the wire.

  Each case runs at all eight sampling phases, 48 runs in all. The
  identical re-declaration is also accepted 1 to 40 clocks before a
  T-MRP-JOIN tick (read-only `dbg_join_tick_o` probe). This is the placement
  where round 1's rule put a Talker Advertise on the wire for k = 1–5, 7
  and 8. Every recorded round must publish the independent greedy model,
  and between rounds a grant only retires with its own declaration.
- **The optimistic window outlives a held verdict (J)**: source 7, then
  source 0, both admissible, with 0 to 20 idle clocks between the two
  requests, at all eight phases.
  While source 0 is pending, source 7's verdict is held, and a window that
  aged on discarded rounds could close first. Neither source ever declares
  Failed. A verdict still pending at source 0's acceptance publishes in the
  same clock as source 0's.

Known limits (recorded honestly): the C++ side emulates the TX arbiter
and the processor-top header strip (both out of scope here — 03 §8 / 03
§5 own them); the PRNG face assumes this engine is the only draw client
(true in the wrap; the full processor routes draws); the byte-exact
checks align actions into clean 200 ms slots because T-MRP-PERIODIC
re-joins legitimately aggregate into the same MRPDU at boundary ticks —
aggregation itself is checked structurally by the parser, and the ladder
is covered by set-checks wherever bundling can occur.

Mutation-proven 2026-08-11 (backup/sed/run/restore):

| Mutation | Result |
|---|---|
| Admission ceiling widened to the full port rate (75 % term removed) | 33 of 230 FAIL (refusal phase C: admitted/over_limit/FAILED/wire Failed; the whole G sweep) |
| granted_slope_bps published ungated (raw slope instead of admitted-gated) | 31 of 230 FAIL (C "granted 0 while refused" + every G slope compare) |
| Admission verdict into the talker FSMs tied 1 (top wiring cut) | 5 of 230 FAIL (C: FAILED state, fail code/bridge, Talker Failed byte-exact; C2 swap-back) |

All three restored; suite back to 230/230 PASS.

Receive-side LeaveAll routing, mutation-proven 2026-09-23 (issue #106; each
arm planted, run, restored under a SHA-256 check). F2 and F4 flag every type
and pass under both the previous and the new routing; F5 separates them:

| Mutation | Result |
|---|---|
| The previous receive side (decoder, both FSMs, KL_srp_top, srp_pkg at the first #106 commit) | 5 of 252 FAIL (F5a, F5b, F5c) |
| Decoder strobes every lane at every flagged VectorHeader | 5 of 252 FAIL (F5a, F5b, F5c) |
| KL_srp_top broadcasts every lane to every plane | 5 of 252 FAIL (F5a, F5b, F5c) |
| Domain participant deaf to the Domain lane | 1 of 252 FAIL (F5b Domain re-declaration) |

The F5c Domain negative (PR #107 correction round 1), mutation-proven
2026-09-23 in a `git archive` export; the arm passed all 252 checks before it:

| Mutation | Result |
|---|---|
| Domain participant takes every MSRP lane (R270-1 X2, R271-1 R4) | 1 of 253 FAIL (F5c no Domain re-declaration) |

Its extension to the two talker lanes (PR #107 correction round 2),
mutation-proven 2026-09-24 in `git archive` exports with the reviewer's plants
verbatim; both arms passed all 253 checks before it:

| Mutation | Result |
|---|---|
| Domain participant also takes the Talker Advertise lane (R271-2 K13) | 1 of 255 FAIL (F5c Talker Advertise LeaveAll) |
| Domain participant also takes the Talker Failed lane (R271-2 K14) | 1 of 255 FAIL (F5c Talker Failed-only LeaveAll) |

FailureInformation change end to end (section D, issues #43/#49): after the
Advertise→Failed swap has drained, a Failed JoinIn with a changed code and
bridge raises `evt_tk_fail_chg_o` once and no EVT_TK_REGISTERED, latches the
new values, and puts NO Listener New on the wire for 800 ms; an unchanged
refresh strobes nothing. Mutation-proven 2026-09-24 in a scratch copy: folding
the change back into the registration indication → 3 of 259 FAIL (the strobe
count, the wire silence, and the unchanged-refresh count).

## Own LeaveAll at transmit acceptance (issue #127)

Timer expiry is only pending intent. The encoder reserves a transmit slot and
issues one `sLA` action, which ages both registrar arrays and starts both
applicant walks with `txLA!`. The tests observe real timer, decoder, allocation,
encoder and serialization handshakes; no DUT state or timer event is forced.
The wrapper can delay the slot pool's allocation request, and the BFM can delay
TX request acceptance. Read-only probes identify the action and decoded events.
The BFM waits until the clock after the serializer's final byte before requesting
another slot, matching the real serializer's return to idle.

| Group | Cases and oracle |
|---|---|
| K: expiry phase | Ready and Ready Failed on sources 0, 3 and 7, with three sources present. Leaves at -50, -1, +1, +40 and +80 ms from the real expiry clear IN and remain stopped for two seconds. At +120 and +199 ms, accepted sLA has legitimately entered LV, so rLv retains registration. Advertise and Failed sink Leaves follow the same phase sweep. |
| K: controls | Mismatched SID and malformed Listener Leave preserve IN; reset clears a blocked preparation; real LeaveTime expires both planes without renewal; timely rejoin preserves healthy registrations across the old deadline. |
| L: acceptance edge | Slot allocation is delayed while a real Listener Leave is decoded. Fourteen release positions for each Ready parameter cover decoded events from -8 through +5 clocks relative to sLA, including -1, 0 and +1. Receive priority clears IN before/on the edge; after it, LV retains registration. All unaffected registrars age once. |
| M: supersession | Each of the four MSRP types before preparation and while allocation is blocked cancels the pending own action. Rejoined registrars are never re-aged and no own flags appear. MVRP cannot cancel it. Since issue #108 an MSRP peer restarts the MSRP timer (a fresh 10-15 s deadline from the peer) and an MVRP peer leaves it unchanged (M4). Six peer/acceptance phases cover -2 through +3 clocks: the peer wins before/on acceptance, and cannot retract an accepted action. A peer from -12 to +1 clocks around real timer expiry leaves no own action: on or after the expiry it drops the new intent, and before it the restart makes that expiry stale (M10; before #108 the -1 case still acted). The sweep crosses rLA!'s draw request, the PRNG's rejection retries and the arm of the restarted deadline, and M9 grades each placement against the calibrated expiry clock, because once the restarted deadline is armed the superseded one never fires. A quiet canceled round waits for real content; a later own action can reuse its reserved slot. Since issue #216 the quiet case (M11) feeds a peer JoinIn of the Domain before the release (AA to QA), so the canceled round's own tx! has nothing to send and the round is empty, as the arm `empty-canceled-pdu` needs. |
| N: congestion | Eight sources and eight sinks exceed the 12-entry table. A previous TX request stalls across several cadence ticks; Leaves still clear IN. After acceptance, a held TX request never repeats sLA. Interim and final drains preserve all live declarations and all four type flags, and queued ticks complete. Another case fills the table before acceptance and proves that this initial full condition also drains. Allocation held across two timer expiries produces just one accepted own action. An asymmetric eight-source/one-sink walk preserves the completed walker across blocked cadence ticks. |

The existing periodic-declaration and six-cycle/no-storm assertions retain their
limits. The measured six-cycle counts were 4, 3, 3, 3, 3, 2 (18 total, maximum 4).
Since issue #108 each peer LeaveAll restarts the timer, no own burst lands in
F4, and the counts were 4, 3, 3, 3, 2, 2 (pinned 17 total, maximum 4). Since
issue #216 a received LeaveAll takes the Domain's Applicant from QA to VP, so the
Domain re-joins on two tx! (VP, then AA), as the Listener Ready already did, and
the counts are 4, 3, 3, 3, 3, 3 (pinned 19 total, maximum 4).
The accepted action precedes serialization; a Leave after that legitimate LV
entry may still retain registration for LeaveTime. This change does not impose
a stricter LV + rLv policy. The receive-timer restart is issue #108, below.

Run the mutation campaign with:

```sh
make -C tb/srp_top mutants MUTANT_OUTPUT=/tmp/srp-leaveall-mutants
```

The `mutants` target runs the complete campaign and is invoked by the HDL
workflow. `MUTANT_OUTPUT` selects the receipt directory; the default is shown
above. Plain `make` still runs the positive suite. The patch files retain their
unified-diff context verbatim; `.gitattributes` exempts only those files from
blank-at-end-of-line and blank-at-end-of-file whitespace checks.

`python3 tb/srp_top/mutants.py --output DIR [--only a,b] [--jobs N]` runs the
campaign directly: `--jobs N` (default 4, the meaning and default of
`tb/pp_top/d3_mutants.py`; the `mutants` target runs the default) builds and
runs up to N copies at once, and the results are printed in the declared order
whatever order they finish in. A label graded on two suites keeps one receipt,
the later row's, as a serial run leaves it. Measured 2026-10-02 at `85da751`
with Verilator 5.050, each run pinned to 4 of the host's 16 CPUs: `--jobs 1`
took 2,258 s and `--jobs 8` 1,099 s. All 89 runs (11 controls, 78 arm runs)
gave the same verdict and the same failing checks in both, and both ended
`90 checks: 90 PASS, 0 FAIL` with assertion coverage 65/65.

Each control and deliberate RTL defect builds in a temporary source copy of its
own. A mutation counts only when a simulation finishes with a nonzero result and
its named assertion fails; compilation failures and timeouts do not count.
The driver applies checked-in unified patches with `git apply --check` in a
fresh scratch RTL copy; it never reads DUT source to derive an oracle. Only
simulation logs are read. Original source integrity is recorded in the review
packet. `srp_top` stops at 300,000,000 DUT clocks (raised from 200,000,000
when the P8 arm-delay sweep brought the complete run to 185,012,669); the
encoder and stream-FSM
walks use finite cycle bounds. Budget exits and missing tallies are unproven,
not kills. `RUN_ARGS=phases`, `edge`, `peer`, `congestion`, `guards`,
`armdelay`, `restart`, `timers`, `join`, `lvleave`, `lvcoll`, `domjoin`, `share`, `link` or `onevec` selects one group for a focused run,
`RUN_ARGS=storage` the timer-arm FIFO arms; plain `make` runs every existing and
new check. The stream-FSM suite independently walks every
applicant state with same-edge sLA/join acceptance.

Round 1 measurement: all five positive controls pass; all 36
mutation runs finish with the required named assertion failing. Restoring the
timer-expiry registrar pulse causes 81 failures. The union of killed assertions
covers all 41 new integration check families (K1–K12, L1–L4, M1–M12, N1–N13).
Both same-edge applicant tables are also mutation-proven. The complete campaign
returns 0 (42/42 checks), and the mutated RTL source hashes remain unchanged.
Build errors and timeouts are never counted as kills.

| Deliberate breakage | Group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `bad-listener-length` | phases | 1 | K12 |
| `peer-restarts-timer` | peer | 8 | M4 |
| `repeated-action` | congestion | 5 | N10, N12, N13, N4, N8 |
| `expiry-event` | phases | 81 | K1, K11, K12, K2, K4, K5, K9 |
| `before-slot` | edge | 30 | L1, L4 |
| `talker-no-own` | phases | 25 | K2, K4, K8 |
| `listener-no-own` | phases | 13 | K11, K6 |
| `talker-lost-txla` | stream FSMs | 12 | same-edge Table 10-3 states/messages |
| `listener-lost-txla` | stream FSMs | 12 | same-edge Table 10-3 states/messages |
| `talker-strict-lv` | phases | 24 | K2, K4 |
| `listener-strict-lv` | phases | 13 | K11, K6 |
| `listener-sid-ignored` | phases | 33 | K12, K3, K5, K8 |
| `talker-no-renewal` | phases | 1 | K7 |
| `listener-no-renewal` | phases | 1 | K7 |
| `talker-no-expiry` | phases | 1 | K8 |
| `listener-no-expiry` | phases | 1 | K8 |
| `pending-peer-ignored` | peer | 14 | M1, M10, M2, M3 |
| `encoder-peer-ignored` | peer | 20 | M1, M11, M12, M2, M3, M6, M7 |
| `mvrp-supersedes-msrp` | peer | 6 | M1, M2, M3 |
| `reset-retains-intent` | phases | 1 | K10 |
| `full-drain-missing` | congestion | 7 | N10, N13, N3, N4, N5, N6, N8 |
| `already-full-missed` | congestion | 1 | N10 |
| `round-completion-lost` | congestion | 6 | N13, N3, N4, N5, N6, N8 |
| `own-flags-missing` | congestion | 4 | N10, N12, N5, N6 |
| `expiry-congestion` | congestion | 5 | N1, N11, N12, N2, N9 |
| `missing-registrar-edge` | edge | 38 | L2, L3 |
| `empty-canceled-pdu` | peer | 1 | M11 |
| `reserved-slot-not-reused` | peer | 1 | M12 |
| `renewal-congestion` | congestion | 1 | N7 |
| `leaveall-expiry-lost` | peer | 18 | M1, M10, M12, M2, M3, M5, M8, M9 |
| `expiry-outranks-peer` | peer | 1 | M10 |
| `repeated-expiry-queued` | congestion | 1 | N12 |
| `receive-priority-lost` | edge | 2 | L2 |
| `action-omitted` | congestion | 6 | N10, N12, N13, N3, N4, N8 |
| `preparation-before-slot` | peer | 22 | M1, M10, M11, M12, M5, M6, M8 |
| `table-one-short` | congestion | 8 | N10, N13, N3, N4, N5, N6, N8, N9 |

Issue #214 moved one count of this table: the value search makes a push wait one clock per
pending entry, so the walks of a congested round take longer and a join tick more often lands
inside the round. `round-completion-lost`, which clears both walks' completion bits on that
tick, now also fails N3, N4, N5, N6 and N8 (1 failing check before, N13).


## Round 2: pending-action guards

The default suite includes 73 additional checks; no DUT event is forced.
Reset edges are excluded from the event history because synchronous reset can
interrupt a combinational preparation acceptance without performing an action.

| Check | Stimulus and required behavior |
|---|---|
| O1 | A peer Domain LeaveAll while preparation waits behind a held encoder TX request cancels the eventual action and its wire flags. The held request is the Domain's JoinMt, which a peer Mt of the Domain before the last T-MRP-JOIN tick ahead of the deadline makes that tick send (issue #216: the Domain sends at tx! only). |
| O2 | After cancellation, a second real timer expiry while allocation remains blocked survives the canceled acceptance and produces exactly one own action/frame. |
| O3 | Peer Domain LeaveAll decoded at -1/0/+1 clocks around the join opportunity cancels before acceptance; both registrar planes remain IN. |
| O4 | With the link down, reuse a retained empty reservation; peer decode at -1/0/+1 around reuse acceptance permits wire flags only when the local action preceded the peer. The exact acceptance clock is checked separately. |
| O5 | Talker Advertise and Failed Leaves on sinks 0 and 7 decode at -1/0/+1 around acceptance. Before/on the edge they clear IN; afterward they retain LV. |
| O6 | Several cadence ticks during blocked preparation produce exactly one immediate follow-up walk after acceptance, before another cadence tick. |
| O7 | A canceled Domain-only round holds its empty reservation for periodic content; with the link up it drains within Periodic plus Join (test bound 1200 ms). A peer JoinIn of the Domain before the release (AA to QA, issue #216) leaves the canceled round's own tx! nothing to send. |
| O8 (encoder suite) | An MVRP join tick and VID push coincide with MSRP preparation. MVRP intake remains open, and the deferred drain emits both VIDs without another tick. |

The reserved handle cannot be released through the current TX interface, which
has no abort operation. It holds one of the shared standard slots and blocks
this encoder's MVRP drains. The link-up bound assumes eventual TX acceptance.
With the link down it waits for link-up content or the next unsuperseded own
action; repeated peer cancellation or external backpressure can extend this
without a finite bound. See [10 section 6.5](../../docs/architecture/10_srp_engine.md#65-leaveall-and-the-δ13-registrar-deviation).

The round-2 campaign reproduces the 36 measurements above and adds all 19
reviewer arms (including aliases for the same edit) plus an empty-reservation
drain arm. All seven controls pass, all 56 arms are killed by their required
named assertions, and all 49 integration assertion families K1–O8 are covered:
`64 checks: 64 PASS, 0 FAIL`, driver rc 0. Stream-FSM control is 1215/1215;
encoder control is 562/562. The required new reviewer edits are measured below.

| Deliberate breakage | Group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `my-expiry-pulse` | phases | 62 | K11,K12,K2,K4,K5 |
| `my-join-edge-peer` | guards | 2 | O3,O4 |
| `my-cancel-eats-new-intent` | guards | 1 | O2 |
| `my-edge-cancel-lost` | peer | 2 | M6,M7 |
| `my-la-outranks-leave` | edge | 2 | L2 |
| `my-drop-join-during-wait` | guards | 1 | O6 |
| `my-no-walk-at-sLA` | congestion | 5 | N10,N12,N13,N5,N6 |
| `my-reuse-flags-ignore-cancel` | guards | 1 | O4 |
| `r-expiry-pulse-restored` | phases | 62 | K11,K12,K2,K4,K5 |
| `r-cancel-latch-dropped` | guards | 1 | O1 |
| `r-cancel-consumes-new-intent` | guards | 1 | O2 |
| `r-join-start-guard-dropped` | guards | 2 | O3,O4 |
| `r-sink-receive-priority-lost` | guards | 2 | O5 |
| `r-join-coalesce-dropped` | guards | 1 | O6 |
| `r-la-only-emission-lost` | peer | 1 | M12 |
| `r-collect-blocks-pushes` | congestion | 4 | N12,N13,N5,N6 |
| `r-accept-edge-drain-lost` | congestion | 1 | N10 |
| `r-reuse-without-action` | peer | 1 | M12 |
| `r-mvrp-start-during-prepare` | srp_encoder | 5 | B1-vlan,O8 |
| `canceled-content-never-drains` | guards | 1 | O7 |

## Received LeaveAll restarts the leavealltimer (issue #108)

802.1Q-2014 Table 10-5 maps rLA! to "Start leavealltimer, Passive" in both
states, and 10.6 says why: a LeaveAll received from another participant
restarts the timer "without generating a message". The LeaveAll machine is per
application (10.7.5.20 NOTE), so a lane of any of its Attribute Types is its
rLA! (10.7.5.20 b)2)): any MSRP lane for MSRP, the VID lane for MVRP, never
across ([10 section 6.5](../../docs/architecture/10_srp_engine.md#fig-10-leaveall)).
`RUN_ARGS=restart` runs the group. Read-only probes add the MVRP deadline,
pending flag, expiry, flag hand-off, decoded MVRP lane and draw requests; no
DUT state is forced.

| Check | Stimulus and required behavior |
|---|---|
| P1 | F4's bridge shape (LeaveAll on every MSRP type, riding its re-declarations) every second for 20 cycles, past the 15 s ceiling of any draw: no own MSRP action and no flagged own MRPDU; every peer restarts the deadline to 10-15 s after it; MVRP keeps its own cycle; registrations stay IN. After the last peer the own LeaveAll follows 10-15 s later (measured +11092 ms). |
| P2 | The same for MVRP (peer VID 2 JoinIn flagged): no own MVRP flag in 20 cycles; MSRP keeps its own cycle; no VID Lv. |
| P3 | Each lane (MVRP, then MSRP types 1-4) 5 s before the earlier own deadline: only its application's deadline is redrawn from the peer. The superseded deadline passes silently; the restarted one fires once, 10 s or more after the peer (measured 13.2-14.0 s). |
| P4 | MVRP Active: right after the own MVRP expiry, a peer MVRP LeaveAll drops the pending flag, the VID re-join (sJ: JoinIn after the peer's JoinIn) still goes out, and the timer restarts. Control without the peer: the flag rides the next drain, with the VID's sJ (JoinMt). |
| P5 | MVRP Active with no VID held (issue #216): the next MVRP tx! is the sLA, and its drain is the LeaveAll-only MVRPDU, byte-exact, within 240 ms of the expiry. A peer MVRP LeaveAll before that tx! makes it Passive: no own LeaveAll. Either way the first VID New after it carries no own flag. Before issue #216 the flag waited for a drain with content, so with no VID held no LeaveAll went out. |
| P6 | The MVRP lane from -12 to +1 clocks around the real MVRP expiry: no own flag in any case, and a restarted timer (before the expiry the superseded deadline is stale, or never fires once the restarted one is armed). |
| P7 | The MVRP equal edge: after the own MVRP expiry the flag waits for the next MVRP join tick, the tx! whose VID walk is followed by the drain that carries it (issue #216). A peer MVRP LeaveAll at -1 or 0 clocks of that tick drops it (Passive; the rider skips the flag on the peer's edge), at +1 the tick was tx! and the accepted own flag goes out. The timer restarts in all three. |
| P8 (`RUN_ARGS=armdelay`) | The committed form of the reviewer's delayed-arm probe. The wrapper delays the engine's timer arms by `arm_delay_i` clocks, as the processor top's queued arm port does. At delays 3, 4, 8 and 16, one reset scenario per offset puts a peer MVRP LeaveAll around the own MVRP expiry, then a peer MSRP (Domain) LeaveAll around the own MSRP expiry, each from -(delay + 12) to +2 clocks: no offset produces an own LeaveAll of either application, every peer lands at its offset and restarts its application's timer. |

The MSRP twin of P6 is M10. Mutation-proven 2026-09-29 through `mutants.py`
(checked-in patches, each built in a scratch RTL copy):

| Deliberate breakage | Group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `expiry-only-redraw` (MSRP peer does not restart the timer) | restart | 15 | P1,P3 |
| `mvrp-expiry-only-redraw` (MVRP peer does not restart the timer) | restart | 6 | P2,P3,P6 |
| `stale-expiry-honoured` (MSRP superseded deadline acts) | peer | 1 | M10 |
| `mvrp-stale-expiry-honoured` (MVRP superseded deadline acts) | restart | 1 | P6 |
| `mvrp-passive-lost` (MVRP peer keeps the own flag) | restart | 5 | P4,P5,P6,P7 |
| `mvrp-flag-at-expiry` (flag handed to the encoder at expiry, the old shape) | restart | 11 | P4,P5,P6,P7 |
| `r-flag-ignores-edge-peer` (a peer MVRP LeaveAll on the tx! edge does not win) | restart | 1 | P7 |

The last three counts are measured at the issue #216 head, where the MVRP flag goes
to the encoder at the round's done, after the VID walk; `mvrp-flag-at-expiry` and
`r-flag-ignores-edge-peer` were re-expressed there because the lines they edit moved.

The round-1 arm `peer-restarts-timer` planted exactly the behavior #108
requires, so it is retired. Five arms whose context the change moved
(`expiry-outranks-peer`, `mvrp-supersedes-msrp`, `pending-peer-ignored`,
`reset-retains-intent`, `my-expiry-pulse`) are regenerated with the same edit and
re-measured: 1 (M10), 8 (M1-M4), 15 (M1, M2, M3, M10), 1 (K10) and 62 (K2, K4,
K5, K11, K12) failing checks.

### A stale expiry whatever the arm-path latency

The first guard treated a LeaveAll-slot expiry as stale until the restarted
deadline's arm was *issued*. Here that arm reaches the timer service on the
same clock, but the processor top queues it behind other faces, and with more
than two clocks of arm latency the superseded deadline could still fire after
the issue and act: one own LeaveAll right after the peer's. The guard now holds
while a draw is outstanding or `now_ms` is before the deadline the draw produced
([10 section 6.5](../../docs/architecture/10_srp_engine.md#fig-10-leaveall)).
M10 and P6 were widened from -1/0/+1 to -12..+1 clocks, P7 is the MVRP equal
edge and P8 the arm-delay sweep (table above). Mutation-proven 2026-09-29 through
`mutants.py`:

| Deliberate breakage | Group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `rearm-at-issue` (the issue-time guard restored: stale only until the arm is issued) | armdelay | 8 | P8 |
| `r-rearm-no-inflight` (no stale window while the draw is in flight) | peer / restart | 3 / 6 | M10 / P6 |
| `r-rearm-no-deadline` (no stale window between the draw and the restarted deadline) | peer / armdelay | 3 / 8 | M10 / P8 |
| `r-flag-ignores-edge-peer` (the MVRP flag rides a join tick that meets a peer MVRP LeaveAll) | restart | 1 | P7 |

`rearm-at-issue` leaves M10 and P6 green: on the direct arm path the issue-time
guard holds. Under it P8 counts an own MSRP LeaveAll at 4, 4, 7 and 15 offsets
for delays 3, 4, 8 and 16 (peer -11..-8, -11..-8, -14..-8 and -22..-8 clocks
before the expiry), and an own MVRP LeaveAll at 2, 2, 6 and 14. At delay 0, M10
needs the guard from -7 to -1 clocks (the draw request at -1, the draw in flight
to -4, the restarted deadline to -7), and P6 from -7 to -1 (the MVRP draw in
flight to -7). `draw-kind-0` is
regenerated with the same edit (its context moved) and still fails 6 checks (Q1-Q4).
The widened sweeps and P7 re-measure these arms: `mvrp-expiry-only-redraw` 17
(P2, P3, P6), `stale-expiry-honoured` 7 (M10), `mvrp-stale-expiry-honoured` 7 (P6),
`mvrp-passive-lost` 6 (P4-P7), `mvrp-flag-at-expiry` 11 (P4-P7),
`pending-peer-ignored` 26 (M1, M2, M3, M10), `leaveall-expiry-lost` 16 (M1, M2,
M3, M5, M8, M9, M12) and `preparation-before-slot` 21 (M1, M5, M6, M8, M11, M12).

## MRP timers graded against Milan v1.2 Table 4.3 (issue #64, REQ-SRP-001)

Every captured MRPDU carries `now_ms_o` at its last byte (`archive_ms`), and the
BFM clock that accepted its request (`archive_accept`). `RUN_ARGS=timers` runs
one bring-up from reset: Begin! arms both leavealltimers at reset release, a
talker declaration on VID 2 in a clean slot, then 52 s with no peer LeaveAll,
then one peer LeaveAll per application. Only MRPDU spacing is graded, never a
DUT register.

| Check | Timer (Table 4.3 range) | Measured |
|---|---|---|
| Q1 | joinTime 180-240 ms: the Table 10-3 ladder New, New, JoinMt of one fresh declaration is one MRPDU per T-MRP-JOIN tick | 200, 200 ms |
| Q2 | periodictimer 900-1500 ms: a quiet declared Talker Advertise re-joins (JoinMt) on each periodic!, and so do the Domain (JoinMt: no peer Domain is registered, issue #184) and the MVRP VID (JoinMt: no peer VID is registered, issue #216); at least five of each before the first LeaveAll | 9 x 1000 ms each |
| Q3 | leavealltimer 10-15 s: the first own MSRP and MVRP LeaveAll no earlier than 10 s after arming, and consecutive own LeaveAlls 10-15 s apart, at least three per application | first +14206 / +10601 ms; MSRP 10000-12800 ms, MVRP 11000-13200 ms |
| Q4 | issue #108 on the wire: a peer LeaveAll 5 s after the latest own one restarts the timer, so the next own LeaveAll of that application comes 10-15 s after the peer's | MSRP +11000 ms, MVRP +14495 ms |

The own MSRP LeaveAll rides the first join opportunity after the expiry, so
its MRPDUs sit on the T-MRP-JOIN grid; with a 10-15 s draw their spacing stays
inside 10-15 s (the Q3 minimum is exactly one 10 s draw). Milan's own tolerance
is wider (9.5-15.5 s).

Mutation-proven 2026-09-29 through `mutants.py` (checked-in patches):

| Deliberate breakage | Group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `join-ms-400` (`JOIN_MS_P = 400`) | timers | 2 | Q1,Q2 |
| `periodic-ms-3000` (`PERIODIC_MS_P = 3000`) | timers | 1 | Q2 |
| `draw-kind-0` (`draw_kind_o = 3'd0`, the 0-1 s range) | timers | 6 | Q1,Q2,Q3,Q4 |

## Domain re-join follows the peer's Domain (issue #184)

802.1Q-2018 §10.7.6.3: the re-join (sJ) is JoinIn only while the Registrar of the
declared Domain is IN, and JoinMt in MT or LV. Milan v1.2 §4.2.7.2.2: rLv! takes that
Registrar from IN to MT at once ([10 §6.1](../../docs/architecture/10_srp_engine.md#sec-10-domain-rejoin)).
`RUN_ARGS=domjoin` runs the group, also part of the default run, on a fresh model
from reset. Every step ends before the first own LeaveAll can fire (10 s from reset).
Each step takes the first Domain re-join of the wire, JoinIn or JoinMt, through the
real decoder, cadence and encoder.

| Check | What it requires |
|---|---|
| DJ1 | nothing declares a Domain: our periodic re-join is JoinMt |
| DJ2 | the peer declares our Domain {6, 3, 2} (JoinIn): our next re-join is JoinIn |
| DJ3 | the peer's LeaveAll MRPDU flags the Domain type on a vector re-declaring its Domain (JoinMt), and flags the other three types with NumberOfValues-0 vectors: our rLA! re-join, at the next T-MRP-JOIN, is JoinIn; and again with the peer's Domain JoinIn (issue #216), which leaves VP alone (note 4), so rLA! alone owes the re-join |
| DJ4 | the peer withdraws its Domain (Lv): our next re-join is JoinMt |

The unit half is [srp_encoder](../srp_encoder/README.md) DR1 to DR11. Mutation-proven
through `mutants.py` (checked-in patches; the `srp_encoder` rows are listed there):

| Deliberate breakage | Group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `domain-rejoin-always-joinin` (the base: every re-join JoinIn) | domjoin | 2 | DJ1, DJ4 |
| `domain-rejoin-always-joinin` | timers | 1 | Q2 |
| `domain-rejoin-always-joinmt` (every re-join JoinMt) | domjoin | 3 | DJ2, DJ3 (both) |
| `domain-lv-ignored` (a received Lv leaves the Registrar IN) | domjoin | 1 | DJ4 |
| `top-domain-lv-unwired` (`KL_srp_top` ties the Domain's Lv strobe to 0) | domjoin | 1 | DJ4 |
| `domain-rx-leaveall-dropped` (rLA! never reaches the Domain's Applicant) | domjoin | 1 | DJ3 (the JoinIn case) |
| `top-domain-tx-every-clock` (`KL_srp_top` ties the Domain's tx! input to 1) | domjoin | 5 | DJ1, DJ2, DJ3, DJ4 |
| `top-domain-tx-unwired` (`KL_srp_top` ties the Domain's tx! input to 0: the Domain never sends, and the MSRP drain, which waits for its tx!, never starts) | domjoin | 5 | DJ1, DJ2, DJ3, DJ4 |

These counts are measured at the issue #216 head, where the four Domain re-join arms are
re-expressed on the Applicant (its sJ at tx!) and a tx! input tied to 1 runs the ladder
in three clocks.

The assertion-coverage check also requires DJ1-DJ4.

## MVRP join before the stream (issue #65, REQ-SRP-006)

Milan v1.2 4.3.2: a Talker PAAD "shall join the relevant VLAN via MVRP prior to
sending any Stream frames"; 4.4.1: a Listener PAAD "shall declare an MVRP VID
attribute for each VLAN used by its settled sinks". ACTIVE, the streaming
licence, now also requires the source VID's MVRP declaration to have left: the
encoder strobes every MVRP MRPDU the TX arbiter accepts, and KL_srp_vlan marks
which live VIDs it carried ([10 section 6.2](../../docs/architecture/10_srp_engine.md#sec-10-join-before-stream)).
An MRPDU counts as transmitted at the clock the BFM accepted its request.
`RUN_ARGS=join` runs the group.

| Check | Stimulus and required behavior |
|---|---|
| R1 | DECLARE_TALKER on VID 2, then a Listener Ready at 1 ms, before the first join tick: registered, admitted, not ACTIVE. ACTIVE rises 1 clock after the BFM accepts the VID 2 New (measured at 200 ms), within one join-paced MRPDU. The VID New is accepted ahead of the first Talker Advertise, so a Ready that answers the Advertise never waits. |
| R2 | A second source on VID 2, already joined: ACTIVE as soon as its Ready registers. |
| R3 | Source 0 withdrawn and re-declared on VID 7 with a new Ready: no ACTIVE until VID 7's own New is accepted; VID 2 stays declared for source 1 (no Lv). |
| R4 | DECLARE_LISTENER sink 0 on VID 7, which no source holds: byte-exact MVRP VID 7 New, then (after the Table 10-3 ladder, issue #216) JoinMt every 900-1500 ms (3 x 1000 ms measured), and WITHDRAW_LISTENER of its last user: byte-exact MVRP VID 7 Lv, no membership left. |

H and I (issue #112) registered a Listener Ready about 2 ms after reset,
before VID 2's MVRP join could leave: exactly the #65 case, so 184 of their
checks fail against the gated licence (96 H, 88 I). Each case now waits 250 ms
after its first declaration and asserts that VID 2's join was captured (24 + 88
new checks); every LATENCY, CROSS and WINDOW measurement line is byte-identical
to the one before the change.

Mutation-proven 2026-09-29 through `mutants.py` (checked-in patches):

| Deliberate breakage | Suite / group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `licence-ignores-join` (talker ACTIVE without the VID term) | srp_top join | 3 | R1,R3 |
| `licence-ignores-join` | srp_stream_fsms | 3 | the three VID-term checks |
| `join-sent-at-handover` (VLAN marks a VID sent when the encoder takes its New) | srp_top join | 2 | R1,R3 |
| `join-sent-at-handover` | srp_encoder | 2 | W2,W3 |
| `count-up-unsends` (a second user resets the VID's sent state) | srp_top join | 2 | R2,R3 |
| `vlan-leave-abandoned` (the last user's leave discards the VID, its Lv never sent; issue #216) | srp_top join | 1 | R4 |

The `join-sent-at-handover` and `count-up-unsends` counts are measured at the issue #216
head, where the VID walk hands each declaration over at its tx! and both arms were
re-expressed on its lines.
| `count-up-unsends` | srp_encoder | 2 | W4 |
| `tx-strobe-any-app` (the transmission strobe also fires for MSRP) | srp_encoder | 1 | W1 |
| `listener-lane-cut` (`vu_sel_ls_w` forced 0, the #65 acceptance arm) | srp_top join | 3 | R4 |

## A withdrawal that meets an LV registrar (issue #134)

The bench case of milan-fpga #608 (cycle 22): the DUT's own LeaveAll crossed the wire
1.390 ms before the bridge's `Lv` for a stream whose listener had withdrawn. The Listener
registrar for that stream was already LV, so the talker streamed on through the 2 s hold.
802.1Q-2014 Table 10-4 puts rLv!, rLA! and txLA! in one row: in IN it starts the leavetimer
and enters LV, and in LV it is `-x-`. leavetimer! in LV is Lv and MT. Milan v1.2 4.2.7.2.2
(Δ13) replaces only the IN cell. So the `Lv` changes nothing, and the registration and
ACTIVE end when the leave timer expires, including a decoded `Lv` on that same clock.
The expiry is applied first; a same-clock New or Join instead finishes IN with the
new registration, as Table 10-4 at MT requires
([10 section 6.5](../../docs/architecture/10_srp_engine.md#sec-10-lv-withdrawal)).
`RUN_ARGS=lvleave` runs the group.

Each case starts from `leaveall_setup`: sources 0, 3 and 7 declared, each registering a
Listener Ready (or ReadyFailed) and ACTIVE, and sinks 0 and 7 registered. Eight cases: an
own and a peer LeaveAll, Ready and ReadyFailed, target source 0 and 7.

- **Own LeaveAll.** The real leavealltimer expires and the accepted `sLA` ages every
  registrar. 1.390 ms (56 clocks) after the LeaveAll MRPDU's last byte, the bridge answers
  with one MRPDU: JoinIn for every other registration, and `Lv` for the target.
- **Peer LeaveAll.** The bridge's LeaveAll on every MSRP type, Listener first, re-joining
  every registration but the target's. 1.390 ms later, its `Lv` for the target.

In both, the bridge repeats the target's `Lv` 2.5 s after the LeaveAll. The harness counts
every edge of ACTIVE, as the integrator's STREAM_START and STREAM_STOP counters do
([02 section 4.6](../../docs/architecture/02_interfaces.md)), and every `LISTENER_REG_CHANGE`
strobe, on every clock. No DUT state is forced; `dbg_t_reg_o` is read only.

| Check | Stimulus and required behavior |
|---|---|
| S1 | The LeaveAll (one `sLA`, or the peer's Listener lane with no own action) has put the target's registrar in LV, and both `Lv`s find it LV and leave it LV. Up to the last ms before the leave timer can expire, the registration is published (Ready or ReadyFailed), ACTIVE is high, and the case has seen no ACTIVE edge and no `LISTENER_REG_CHANGE` |
| S2 | The leave timer's expiry closes them: ACTIVE falls and `LISTENER_REG_CHANGE` strobes 5000 or 5001 ms after the LeaveAll, and the registrar is MT with `lstn_reg_state` 0. The 1 ms of slack is the arm's deadline, T-MRP-LEAVE from the clock the arm issues, which can fall in the next ms |
| S3 | 2 s after that: exactly one STREAM_STOP and no STREAM_START over the case, and one `LISTENER_REG_CHANGE` |

The leave timer is `KL_srp_top`'s `LEAVE_MS_P`, 5000 ms (F08.1 T-MRP-LEAVE, unchanged by the
processor top). Milan v1.2 Table 4.3 LeaveTime: default 5000 ms, tolerance +50 %/-10 %, so
4500 to 7500 ms. Each case prints one `LV_LEAVE` line. Measured at the head: the own
LeaveAll at 14200 ms, the first `Lv` decoded by 14216 ms, the second by 16700 ms, the close at
19200 ms; the peer LeaveAll at 705 ms, the `Lv`s by 711 and 3205 ms, the close at 5705 ms. The
leave time is 5000 ms in all eight cases, one STREAM_STOP and no STREAM_START in each.

Mutation-proven 2026-10-05 with the checked-in patches, each planted separately and
run through the complete default `make -C tb/srp_top` (8656 checks). `lv-second-lv-ends`
fails only the 16 S1/S2 checks. `lv-never-ends` fails the 16 S2/S3 checks plus 3016 SC2
checks in `lvcoll`, for 3032 failures:

| Deliberate breakage | Group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `lv-second-lv-ends` (the registrar lets one `Lv` in LV pass and ends the registration on the second) | lvleave | 16 | S1,S2 |
| `lv-never-ends` (an `Lv` in LV stops the leave timer, so the registration never ends) | lvleave | 16 | S2,S3 |
| `lv-never-ends` (the same planted patch, in the same default run) | lvcoll | 3016 | SC2 |

The older one-line forms of the same two defects, `talker-strict-lv` (an `Lv` in LV ends the
registration at once) and `talker-no-expiry` (no leave-timer expiry reaches the talker
registrar), are campaign rows of the phases group. Run through this group they fail 16 checks
each, S1,S2 and S2,S3, closing after 14 or 5 ms and never.

### Same-clock leave expiry (round 2)

`RUN_ARGS=lvcoll` is also part of the default run. It contains 6,416 byte-stream
cases: k = 0..400, both planes, own/peer LeaveAll, Ready/ReadyFailed, index 0/7.
Sink 0 holds Advertise and sink 7 holds Failed. Both target registrars age;
only the selected plane receives the `Lv` near expiry. The listener plane has no
ACTIVE face: its matching TK_UNREGISTERED is graded alongside the source's
STREAM_STOP in every case.

A no-`Lv` calibration measures each plane's close clock. Each offset restores
identical DUT and BFM state saved 401 clocks before that clock, then starts a real
MRPDU k clocks before it. The shared prefix uses the real own/peer LeaveAll and
5000 ms timer. All clocks around decoding, expiry and publication execute, followed
by a 20 ms observation window. The run counter includes every executed replay
clock; the snapshot lives outside the tree and is removed after the sweep.

| Check | Required result | Planted fault that fails it |
|---|---|---|
| SC1 (`srp_stream_fsms`) | 128 simultaneous-event cases match Table 10-4, expiry first, including indications and timer operations | `lv-expiry-masked` 16 failures; `lv-expiry-last` 48; `lv-expiry-dropped` 80 SC1 failures |
| SC2 | Every k ends with both registrars MT, both registrations absent, ACTIVE low, one STREAM_STOP and one TK_UNREGISTERED | `lv-expiry-masked` restores both original planes: 16 failures, one collision in each combination |
| SC3 | Exactly one decoded `Lv` in each sweep shares the calibrated expiry clock | `lv-sweep-misses-collision` delays every frame 401 clocks: all 16 checks fail while SC2 stays green |

The restoration mutant is a checked-in reverse patch of the complete RTL change.
The two original mutants and S1-S3 remain. The two other matrix faults move expiry
after reception (so a registering event wrongly ends MT) and discard expiry
(so Lv/LA/In/Mt wrongly leave LV). Every new check fails under at least one fault.
The full campaign requires SC1-SC3 as well as the original assertion coverage.

Table 10-4 results after LV / leavetimer!: MT + Lv, then:

| Same-clock event | Final state | Published result |
|---|---|---|
| rLv | MT | Registration closes once |
| rLA / own txLA | MT | Registration closes once; no replacement leave ARM |
| rIn / rMt | MT | Registration closes once |
| rNew | IN | Received value registered; obsolete timer canceled |
| rJoinIn / rJoinMt | IN | Received value registered; obsolete timer canceled |

Both planes publish the final result of the composed transitions. A registering
event preserves the published registration; it emits no intermediate withdrawal
or ACTIVE edge. The listener retains its existing type-change, latency and
FailureInformation notifications for the received value. Separate REGISTERED and
UNREGISTERED pulses would enter independent event-router queues with no ordering
guarantee, so renewal must not publish a transient withdrawal.

## Timer-arm FIFOs at four shapes (issue #230)

Each stream FSM's timer ops reach the one merged arm port through a FIFO of its
own: 32 words of distributed RAM since issue #230
([10 §5.1](../../docs/architecture/10_srp_engine.md)), issued round-robin, one
word every two clocks. `store_main.cpp` checks them on the real engine and its
real services (`srp_store_wrap.sv`, time-compressed as above) at sources/sinks
1/1, 2/2, 3/5 and 9/9. A scoreboard reads both FSMs' arm faces (what each FIFO
is offered) and the merged face (what leaves) through read-only probes, and
holds the FIFO contract: every op an FSM offers leaves the merged face exactly
once, in that FSM's order and unmodified, except an op offered while its FIFO
holds 32 words, which the full guard refuses.

The first arm brings every source and sink up with a registration from the
peer and waits for an own LeaveAll. Its sLA ages both registrar arrays in one
clock, so both FSMs offer an op in the same clock (checked). On a LeaveAll clock
where the round-robin serves the listener FIFO first, a re-declaration of
source 0 is accepted, and its gate op cancels the leave timer that LeaveAll just
started. The talker FIFO therefore holds two words when the merged face selects
it (checked). On a LeaveAll where the round-robin points at the talker FIFO, the
peer re-joins all but source 0, whose leave timer runs out instead. That makes
an odd number of issues, so the next LeaveAll finds the listener FIFO first. At
every shape the second LeaveAll is the one.

The second arm reaches the full guard, which no port sequence reaches: the issue
drains a word every two clocks, and the registrar walks offer at most one op per
source or sink. `srp_store_wrap` holds the merged issue in TM_SEL while
`tm_stall_i` is high: **the one forced state in these arms**, and only in this
arm. Peer LeaveAll MRPDUs then offer each FIFO two ops per stream (the LeaveAll
ages, the re-join cancels) until each has been offered 36. Then the stall is
released.

| Check | What it proves |
|---|---|
| TF1 | the talker FIFO issues every op it accepted once, in its order and unmodified: no word lost, reordered, altered, duplicated or invented, and every queued word drains |
| TF2 | the same for the listener FIFO |
| TF3 | no op is refused while the issue runs unforced |
| TF4 | held, the talker FIFO accepts exactly 32 ops and refuses the rest; released, it issues those 32 in order, unmodified |
| TF5 | the same for the listener FIFO |

Preconditions are checks too: every registrar IN before each arm, an own
LeaveAll within 16 s, both FSMs offering in one clock, the re-declaration on a
LeaveAll clock, a two-word talker FIFO at a selection, more than 32 offers per
FIFO while held, and no FSM op issued while held. `make RUN_ARGS=storage` runs
these arms alone. Every part runs even after a failing one, and every failing
check names its shape (`[sources/sinks]`). Measured at the head, 15 checks at
each shape, all PASS:

| Shape | First arm: offered (= issued), talker / listener | Clocks with both offering | Two-word selections, talker / listener | Held arm: MRPDUs, offered, refused |
|---|---|---:|---|---|
| 1/1 | 3 / 4 | 2 | 1 / 0 | 18; 36 / 36; 4 / 4 |
| 2/2 | 7 / 8 | 4 | 2 / 1 | 9; 36 / 36; 4 / 4 |
| 3/5 | 11 / 20 | 7 | 4 / 7 | 6; 36 / 60; 4 / 28 |
| 9/9 | 35 / 36 | 18 | 19 / 15 | 2; 36 / 36; 4 / 4 |

The build waives three Verilator warnings for this wrap alone (the Makefile
says which and why). Verilator 5.050 faults on a hierarchical reference to
`KL_srp_top`'s enum item `TM_SEL`, so the wrap names its encoding (1'b0).

## Issue #230 storage controls

The probes of both #230 reviews (R458-1 `lockstep/probes.py` and R458-3's two
ready-handshake probes, R459-1 `make_controls.py` and its stall probe) and the
lane's own lockstep controls are killed controls of `mutants.py`, one
checked-in patch per distinct edit: 35 patches, where a probe and a control with
the same edit share one. The new arms carry no group of the suite above:
`storage` runs the FIFO arms, `walk` the
[walk-record arms](../srp_stream_fsms/README.md#walk-records-at-both-elaboration-arms-issue-230),
and the slope controls run the [admission suite](../srp_admission/README.md) at
its five shapes. The assertion-coverage check now also requires TF1-TF5 and
WK1-WK13, and RB1-RB9 ([issue #188](#issue-188-the-owed-listener-leave)), SH1-SH24 and
SW1-SW7 ([issue #193](#issue-193-sinks-on-one-stream-share-one-declaration)), TP1-TP9
([issue #202](#issue-202-the-owed-talker-leave)), and PS1-PS24 and LK1
([issue #212](#issue-212-the-pre-settle-registration)).

Failing checks per shape, from the campaign's receipts. "n/e": the edited arm
is not elaborated at that shape. "equivalent": with one source or one sink the
edit changes nothing (it names source or sink 0, the only one). "equivalent in
simulation": at one sink the parked control index (WK6) is the face's only
out-of-range value, and Verilator reads a single 64-bit packed element at an
out-of-range index as element 0, so the edit reads sink 0 there too; the 48-bit
DA of `wid-flops-da-of-gate-source` does not alias, and that edit is caught at
1/1.

| Control | From | Edit | Named failing checks | 1/1 | 2/2 | 3/5 | 9/9 | Caught at |
|---|---|---|---|---:|---:|---:|---:|---|
| `tf-heads-swapped` | A523, R458-1 | each FIFO head reads the other FIFO's memory | TF1, TF2 | 2 | 2 | 2 | 2 | 4 of 4 |
| `tf-head-at-write-pointer` | A523 | talker head read at `wptr - 1`, the newest word | TF1, TF4 | 2 | 2 | 2 | 2 | 4 of 4 |
| `tf-listener-push-dropped` | A523 | listener push lost when both FSMs push | TF2 | 1 | 1 | 1 | 1 | 4 of 4 |
| `tf-ls-written-at-tk-pointer` | R458-1 | listener word written at the talker's write pointer | TF2, TF5 | 2 | 2 | 2 | 2 | 4 of 4 |
| `tf-tk-head-read-ahead` | R458-1, R459-1 (`tf-tk-head-reads-next`) | talker head read at `rptr + 1` | TF1, TF4 | 4 | 4 | 3 | 2 | 4 of 4 |
| `tf-ls-head-reads-tk-ram` | R459-1 | listener head reads the talker memory | TF1, TF2, TF3, TF4, TF5 | 4 | 4 | 4 | 5 | 4 of 4 |
| `tf-tk-write-at-rptr` | R459-1 | talker word written at the read pointer | TF1, TF4 | 2 | 2 | 2 | 2 | 4 of 4 |
| `tf-full-guard-31` | R458-1 | talker full guard at 31 words | TF4 | 1 | 1 | 1 | 1 | 4 of 4 |
| `tf-tk-write-ignores-full` | R459-1 (stall probe) | talker memory written while full | TF4 | 1 | 1 | 1 | 1 | 4 of 4 |
| `tf-ls-write-ignores-full` | R459-1 | listener memory written while full | TF5 | 1 | 1 | 1 | 1 | 4 of 4 |
| `walk-record-written-on-close` | A523, R459-1 (`wtsp-written-on-close`) | walk records also written by a gate close | WK3 | 1 | 1 | 1 | 1 | 4 of 4 |
| `wtsp-first-open-only` | A523, R458-1 | walk TSpec written only by a source's first open | WK2, WK3, WK4, WK9 | 4 | 6 | 8 | 20 | 4 of 4 |
| `wtsp-read-at-gate-source` | A523, R458-1 | walk TSpec read at the gate source | WK1, WK2, WK4, WK9 | 1 | 6 | 13 | 49 | 4 of 4 |
| `wtsp-read-at-source-0` | R459-1 | walk TSpec read at source 0 | WK1, WK2, WK3, WK4, WK5, WK9 | 0 (equivalent) | 8 | 14 | 50 | 3 of 4 |
| `wtsp-latency-field-shifted` | A523 | latency read one bit off (`wtsp_w[32:1]`) | WK1, WK2, WK3, WK4, WK5, WK9 | 8 | 14 | 20 | 56 | 4 of 4 |
| `wtsp-latency-shifted` | R458-1 | latency shifted left one bit | WK1, WK2, WK3, WK4, WK5, WK9 | 8 | 14 | 20 | 56 | 4 of 4 |
| `wtsp-rank-dropped` | R458-1 | rank bit published as 0 | WK1, WK2, WK3, WK4, WK5, WK9 | 4 | 8 | 9 | 28 | 4 of 4 |
| `wtsp-prio-rank-swapped` | R459-1 | priority and rank bits rotated | WK1, WK2, WK3, WK4, WK5, WK9 | 7 | 13 | 19 | 48 | 4 of 4 |
| `wid-ram-first-open-only` | A523, R458-1 | RAM {stream_id, DA, VLAN} written only by the first open | WK2, WK3, WK4, WK9 | 0 (n/e) | 0 (n/e) | 8 | 20 | 2 of 4 |
| `wid-ram-read-at-gate-source` | A523, R458-1 | RAM {stream_id, DA, VLAN} read at the gate source | WK1, WK2, WK4, WK9 | 0 (n/e) | 0 (n/e) | 13 | 49 | 2 of 4 |
| `wid-ram-read-neighbour` | R459-1 | RAM {stream_id, DA, VLAN} read at the previous source | WK1, WK2, WK3, WK4, WK5, WK9 | 0 (n/e) | 0 (n/e) | 20 | 56 | 2 of 4 |
| `wid-flops-da-of-gate-source` | A523, R458-1 | flop arm reads the gate source's DA | WK1, WK2, WK4, WK9 | 1 | 6 | 0 (n/e) | 0 (n/e) | 2 of 4 |
| `wid-flops-sid-of-source-0` | R458-1 | flop arm reads source 0's stream_id | WK1, WK2, WK3, WK4, WK5, WK9 | 0 (equivalent) | 8 | 0 (n/e) | 0 (n/e) | 1 of 4 |
| `wid-flops-vid-of-source-0` | R459-1 | flop arm reads source 0's VLAN | WK1, WK2, WK3, WK4, WK5, WK9 | 0 (equivalent) | 8 | 0 (n/e) | 0 (n/e) | 1 of 4 |
| `talker-vid-unreset` | A523 | the matcher VLAN loses its reset (a stored value read without its valid bit) | WK5 | 1 | 1 | 1 | 1 | 4 of 4 |
| `wsid-ram-first-settle-only` | A523, R458-1 | RAM stream_id written only by the first settle | WK8, WK10, WK12 (since issue #188's second round) | 0 (n/e) | 0 (n/e) | 11 | 19 | 2 of 4 |
| `wsid-ram-written-on-teardown` | A523, R458-1, R459-1 | RAM stream_id also written by a teardown | WK7 | 0 (n/e) | 0 (n/e) | 1 | 1 | 2 of 4 |
| `wsid-flops-of-control-sink` | A523, R458-1 | flop arm reads the control sink's stream_id | WK6, WK8, WK10, WK12 (since issue #188's second round) | 0 (equivalent in simulation) | 6 | 0 (n/e) | 0 (n/e) | 1 of 4 |
| `wsid-flops-read-sink-0` | R459-1 | flop arm reads sink 0's stream_id | WK6, WK7, WK8, WK10, WK12 (since issue #188's second round) | 0 (equivalent) | 7 | 0 (n/e) | 0 (n/e) | 1 of 4 |
| `wtsp-write-ignores-ready` | R458-3 (`r3-wtsp-write-ignores-ready`) | walk records written by a gate open not yet taken (`gate_valid_i` for `gate_acc_w`) | WK9 | 1 | 1 | 1 | 1 | 4 of 4 |
| `wsid-write-ignores-ready` | R458-3 (`r3-wsid-write-ignores-ready`) | RAM stream_id written by a settle not yet taken (`ctl_valid_i` for `ctl_acc_w`) | WK10, WK11 (since issue #188: the early word is also what the taken settle parks) | 0 (n/e) | 0 (n/e) | 2 | 2 | 2 of 4 |

The slope controls, at the admission suite's five shapes. The suite stops at
its first failing shape, so the campaign sees N = 2; the counts at 3, 5 and 8
were measured one shape at a time in scratch copies:

| Control | From | Edit | N = 1 | N = 2 | N = 3 | N = 5 | N = 8 | Caught at |
|---|---|---|---:|---:|---:|---:|---:|---|
| `slope-stored-at-stage-2-index` | A523, R458-1, R459-1 (`slope-store-at-stage-1-index`) | slope written at `cidx_q1_r`, not `cidx_q2_r` | 0 (equivalent) | 1,333 of 12,615 | 4,109 of 41,012 | 12,760 of 201,073 | 38,919 of 991,231 | 4 of 5 |
| `slope-stored-at-source-0` | R458-1 | every slope written to source 0 | 0 (equivalent) | 1,521 of 12,615 | 3,915 of 41,012 | 12,415 of 201,073 | 39,056 of 991,231 | 4 of 5 |
| `slope-store-source-0-only` | A523 | only source 0's slope stored | 0 (equivalent) | 1,201 of 12,615 | 3,163 of 41,012 | 10,281 of 201,073 | 33,958 of 991,231 | 4 of 5 |
| `slope-read-source-0` | R459-1 | the admission walk reads source 0's slope | 0 (equivalent) | 578 of 12,615 | 1,683 of 41,009 | 6,125 of 201,068 | 22,202 of 991,223 | 4 of 5 |

"A523" is the lane's round-1 lockstep controls, and "R458-1", "R459-1" and "R458-3"
are the reviews' probes, by their names there. Every control is caught at every shape
where its arm is elaborated and the edit is not equivalent by construction, except
`wsid-flops-of-control-sink` at 1/1: there Verilator 5.050 reads the out-of-range 64-bit
element as element 0 (equivalent in simulation, above), as it would any 64-bit stream_id
read at an idle face at one context; 2/2 catches those edits. The
three edits R459-1 planted as equivalent leave every committed suite passing, as
an equivalent edit must: `tf-tk-same-entry-bypass` (the head bypasses the
memory when the write and read pointers meet), and `wid-threshold-ram-from-1`
and `wid-threshold-flops-always` (either walk arm at every shape).

## Issue #188: the owed Listener Leave

An A15 settle no longer forces the sink's applicant to VO ([10 §6.4](../../docs/architecture/10_srp_engine.md#sec-10-owed-leave)).
The [stream-FSM suite](../srp_stream_fsms/README.md) grades it in section M (RB1 the
rebind, RB2 the re-probe, RB3 the re-probe that re-registers before tx!; while the old
stream_id is parked, RB4 a second rebind onto a third stream, RB5 a rebind back to it,
RB6 an LO applicant, RB7 a txLA! walk and RB8 a received Listener value; RB9 a sink with
no record since reset, which owes nothing) and, for the leave copy at both elaboration
arms, in WK11 and WK12. One patch per arm, each required to fail its named check:

| Control | Edit | Named failing checks |
|---|---|---|
| `listener-rebind-abandons-leave` | a settle on another stream forces the owing applicant to VO (the behaviour before the issue) | RB1 |
| `listener-declaration-not-held` | the new stream's declaration request is granted while the old one is parked, so it reaches the parked applicant | RB1 |
| `listener-park-never-retires` | the parked applicant never takes the settled stream after its Lv | RB1 |
| `listener-reprobe-abandons-leave` | a settle on the same stream forces the owing applicant to VO | RB2 |
| `listener-reprobe-parks-own-stream` | a settle on the same stream parks it as if it were another | RB3 |
| `listener-csid-ram-ignores-park` | RAM arm: the settle compares and parks the walk copy's stream_id, not the parked one (also WK12, below) | RB4 |
| `listener-rebind-back-keeps-park` | a settle back onto the parked stream keeps the park | RB5 |
| `listener-lo-not-owed` | an LO applicant is not counted as owing (only LA is) | RB6 |
| `listener-park-retires-on-txla` | a txLA! walk retires the park although the applicant stays LO | RB7 |
| `listener-parked-applies-rx` | received Listener values are applied to a parked sink | RB8 |
| `listener-unrecorded-applicant-owes` | a sink with no record since reset owes what its applicant's state says (the behaviour of the first #188 change) | RB9 |

The leave-copy arms, failing checks per shape as in the #230 table:

| Control | Edit | Named failing checks | 1/1 | 2/2 | 3/5 | 9/9 | Caught at |
|---|---|---|---:|---:|---:|---:|---|
| `walk-ignores-leave-copy` | the walk publishes the settled stream_id while the old one is parked | WK11 | 1 | 2 | 5 | 9 | 4 of 4 |
| `lsid-ram-read-sink-0` | RAM leave copy read at sink 0 | WK11 | 0 (n/e) | 0 (n/e) | 4 | 8 | 2 of 4 |
| `lsid-flops-read-sink-0` | flop leave copy read at sink 0 | WK11 | 0 (equivalent) | 1 | 0 (n/e) | 0 (n/e) | 1 of 4 |
| `lsid-ram-written-with-new-stream` | RAM leave copy written with the settled stream_id | WK11 | 0 (n/e) | 0 (n/e) | 5 | 9 | 2 of 4 |
| `listener-csid-ram-ignores-park` | RAM arm: the settle compares and parks the walk copy's stream_id, not the parked one (also RB4 in the suite) | WK12 | 0 (n/e) | 0 (n/e) | 5 | 9 | 2 of 4 |
| `listener-csid-flops-ignores-park` | flop arm: the settle compares and parks the matcher's stream_id, not the parked one | WK12 | 1 | 2 | 0 (n/e) | 0 (n/e) | 2 of 4 |

The four WK11 controls also fail WK12, at the same shapes and with the same counts. WK12's
own count lines and its precondition are graded at every shape too: the walk run of
`listener-rebind-abandons-leave` must fail `WK12: the listener walk sends` (no Leave goes
out), that of `listener-park-never-retires` must fail `WK12: push` (since issue #212 the
walk that sends the parked Leave declares the third stream too, and with the park never
retiring each such declaration carries the parked stream_id; before it, the third stream
was never declared and the arm named `WK12: the listener walk pushes`), and
`listener-aa-tx-stays-aa` (AA + tx! stays AA, so a declared applicant never goes quiet)
must fail `WK12 precondition`.

Two reviewer probes of this area are accepted as likely equivalent and have no control.
The retire of the park keeping the old FourPacked parameter shows only as the parameter
of an Empty the sink sends before its next declaration (a received LeaveAll takes its VO
applicant to LO), and an Empty registers nothing. The same-value test without the
record-valid term changes nothing: it matters only when something is owed, and a sink
with no record owes nothing (RB9).

The `tb/pp_top` AS7 leg grades the same behaviour end to end, with two controls of
`tb/pp_top/acmp_mutants.py`.

## Issue #212: the pre-settle registration

The SRP listener registers every Talker Advertise/Failed value of the port, up to one per
sink, and an A15 settle on one starts its sink's registrar in that registration's state;
an A8 leaves the sink's registrar registering its value, unpublished, and an A15 on the
same {stream_id, DA, VLAN} keeps it; LINK_DOWN (Flush!) and LINK_UP (Begin!) take the port
registrars and every sink registrar, settled or not, to MT, a settled sink's withdrawal
published as at its leavetimer!; the walk that sends a parked
Leave also declares the settled stream
([10 §6.4](../../docs/architecture/10_srp_engine.md#sec-10-pre-settle); 802.1Q-2022 §10.7,
§10.7.8, §35.2.7, Table 10-3, Table 10-4; Milan v1.2 §5.3.8.5, §5.3.8.9, §5.5.3.5.18 step
4). The [stream-FSM suite](../srp_stream_fsms/README.md) grades it in section PS (PS1 to
PS24) and in RB1, RB4, RB6 and RB7, and this suite's `link` group grades the link input:

| Check | What it requires |
|---|---|
| LK1 | a Talker Advertise fed before a sink settles registers at the settle (one TK_ATTR_REGISTERED, ADVERTISE, Listener Ready within 800 ms); after `link_up_i` falls and rises again, the same Advertise fed before them is not applied: no TK_ATTR_REGISTERED, NONE, no Listener value for it in 800 ms |

One patch per arm, each required to fail its named check; the count is the arm's failing
checks in the suite run:

| Control | Edit | Named failing check | Failing checks |
|---|---|---|---:|
| `pt-seed-dropped` | a settle never takes the port's registration | PS1 (acceptance 1), PS5 (acceptance 2) | 26 |
| `presettle-removed` | no seed, no kept registration and no declaration in the parked Leave's walk (the behaviour before the issue) | PS3 (acceptance 2) | 37 |
| `pt-never-holds` | the port registrars never register a value | PS2 | 26 |
| `seed-no-registered-strobe` | a seeding or keeping settle raises no EVT_TK_REGISTERED | `PS1: a Talker Advertise` | 26 |
| `seed-type-advertise` | a settle always starts an Advertise registration | PS2 | 8 |
| `seed-drops-failure-information` | a settle starts the registration without its failure code and system id | PS4 | 5 |
| `park-retire-declares-next-walk` | the walk that sends a parked Leave leaves the settled stream to the next walk (the behaviour before the issue) | PS4, RB1 | 5 |
| `pt-rlv-ignored` | the port registrars ignore rLv (no Δ13) | PS6 (acceptance 3) | 6 |
| `pt-leavetimer-never-expires` | a port registrar in LV never ends | PS7 (acceptance 3) | 4 |
| `pt-leaveall-ignored` | the port registrars never age on a LeaveAll | PS7 | 11 |
| `pt-leaveall-any-lane` | a LeaveAll on any MSRP lane ages every port registrar | `PS7: a LeaveAll on the other lanes` | 1 |
| `seed-lv-fresh-deadline` | an LV settle arms a fresh T-MRP-LEAVE, not the registration's deadline | PS8 | 3 |
| `seed-lv-unguarded` | an LV settle leaves its slot unguarded until the arm issues | `PS15: the leave slot` | 1 |
| `seed-from-current-state` | a settle takes the port registrar's state before its own clock's events | PS9 | 2 |
| `pt-match-ignores-da` | the port registrars match without the DA | PS10 | 1 |
| `pt-vid-high-bits-ignored` | a Talker whose VLAN field is above 12 bits is held under its low 12 | PS10 | 1 |
| `pt-allocation-overwrites-entry-0` | a value nobody holds overwrites entry 0 even when another is MT | PS11 | 6 |
| `park-retire-ignores-group` | the walk declares the settled stream although another sink declares it | PS13 | 1 |
| `park-retire-without-hold` | the walk sends the parked Leave on the clock a registrar indication changes the waiting request | PS14 | 2 |
| `seed-keeps-expiry-strobe` | the old registration's expiry on a seeding settle's clock still strobes EVT_TK_UNREGISTERED | `PS15: the old registration` | 1 |
| `pt-alloc-any-event` | a value no entry holds is allocated IN on any Talker event, rLv, rIn and rMt too | PS16 (acceptance 3) | 9 |
| `pt-fcode-first-only` | a port registrar keeps the FailureInformation of its allocation or of its first Failed | `PS17: a held Talker Failed` (acceptance 2) | 1 |
| `pt-lat-on-alloc-only` | a port registrar keeps its allocation's latency | `PS17: a held Talker Advertise whose latency` | 3 |
| `pt-type-on-alloc-only` | a port registrar keeps its allocation's type | `PS17: a held Talker Advertise that turns Failed` | 1 |
| `seed-on-teardown` | an A8 offered a held value's triple seeds the registrar as a settle does | PS18 | 2 |
| `pt-rlv-on-lv-ends` | rLv on an LV port registrar ends it (Table 10-4 `-x-`) | PS19 | 2 |
| `rebind-same-not-kept` | an A15 on the triple the sink's registrar registers starts it afresh | `PS20: every port registrar held by other values, sink 0 registered on its stream` (acceptance 2) | 5 |
| `keep-ignores-da` | the kept registration is taken on the same stream_id and VLAN, any DA | `PS20: a re-bind to the same stream_id on another DA` | 1 |
| `keep-ignores-vlan` | the kept registration is taken on the same stream_id and DA, any VLAN | `PS20: a re-bind to the same stream_id on another VLAN` | 1 |
| `keep-declares-seed-type` | a keeping settle requests the declaration of the port registrars' type | `PS20: every port registrar held by other values, sink 0 registered on a Talker Failed` | 2 |
| `keep-latency-strobe-dropped` | a keeping settle raises no latency strobe | `PS20: every port registrar held by other values, sink 0 registered on its stream` | 2 |
| `unarmed-registrar-deaf` | a sink's registrar takes no event while the sink is unbound | `PS21: sink 0 unbound from a stream only its registrar holds, the talker withdraws` (acceptance 3) | 2 |
| `keep-ignores-expiry` | a re-bind on the clock of the kept registration's leavetimer! keeps it | `PS21: a re-bind on the clock of the kept registration` | 1 |
| `unarmed-indicates` | a registration while unbound requests the declaration | PS22 | 1 |
| `unarmed-fail-chg` | a FailureInformation change while unbound strobes | PS22 | 1 |
| `unarmed-unregistered` | an unregistration while unbound strobes | PS22 | 1 |
| `unarmed-latency-strobe` | a latency change while unbound strobes | PS22 | 1 |
| `unarmed-publishes-latency` | `acc_latency` publishes the kept latency while unbound | PS22 | 2 |
| `unarmed-dbg-registrar` | the debug registrar reads the kept state while unbound | PS22 | 2 |
| `pt-link-flush-dropped` | a link change leaves the port registrars | `PS23: a Talker Advertise registered before the link` | 6 |
| `link-begin-dropped` | only LINK_DOWN takes a registrar to MT, LINK_UP leaves it | `PS23: LINK_UP (Begin!)` | 1 |
| `unarmed-link-flush-dropped` | a link change leaves the registrar of a sink not settled | `PS23: sink 0 unbound` | 1 |
| `keep-on-link-clock` | a re-bind on the clock of a link change keeps the registration | `PS23: a re-bind to that stream on the clock the link goes down` | 1 |
| `top-listener-link-unwired` | `KL_srp_top` ties the listener's `link_up_i` to 1 | LK1 | 1 |
| `pt-expiry-sign-bit` | the 13-bit leave deadline compared by its sign bit, which ends an LV entry at once | PS8 | 6 |
| `seed-deadline-rebuild-drops-now` | a seeded LV arm's deadline without `now_ms_i`'s upper bits | PS8 | 4 |
| `settled-link-flush-dropped` | a settled sink's registrar takes no link change (the exception before this row) | `PS24: a sink settled through a link change` | 11 |
| `settled-link-unpublished` | a link change ends a settled sink's registration without EVT_TK_UNREGISTERED | `PS24: sink 0 settled and registered` | 5 |
| `link-withdraw-dropped` | a link change ends a settled sink's registration but leaves its declaration | `PS24: sink 0 settled and registered` | 6 |
| `link-clock-indicates` | a Talker on the clock of a link change requests the declaration | `PS24: a Talker Advertise on the clock the link goes down` | 1 |
| `settled-link-keeps-lv-slot` | a link change ends an LV registration but leaves its leave slot armed | `PS24: sink 0 settled on a Talker Advertise a LeaveAll ages` | 1 |

The assertion-coverage check requires PS1-PS24 and LK1. For acceptance 3, removing the
pre-settle path would apply nothing and so leave its rows passing; they are killed by arms
that break the path's withdrawal handling instead (rLv, leavetimer!, LeaveAll, allocation,
the unbound registrar's events, the link change). The change moved the context of patches
that keep their edits: `listener-retire-skips-group-mark`, `lv-expiry-masked` (issue
#134), and issue #212's own arms where the settle block, the leave deadline and the link
branch moved. The link branch now goes first in the sink registrar's events, so two edits
follow it: `r-sink-receive-priority-lost` puts the LeaveAll ahead of a reception as
before, now after the link change; and `unarmed-link-flush-dropped`, the link branch
serving every sink, keeps it from the unbound ones. The issue #188 arms named on RB1, RB4, RB6 and RB7
keep their edits and their named checks, which now require the old stream's Lv and the
new stream's New in one walk. On the processor, AS7, AS8 and AS9 of
[`tb/pp_top`](../pp_top/README.md) grade it end to end, with two controls of
`tb/pp_top/acmp_mutants.py`.

## Issue #202: the owed Talker Leave

A talker gate re-open no longer takes the owing applicant over for another stream_id
([10 §6.3](../../docs/architecture/10_srp_engine.md#sec-10-talker-owed-leave)). The
[stream-FSM suite](../srp_stream_fsms/README.md) grades it in section TP (TP1 the re-open
under another stream_id, TP2 the same stream_id, TP3 back to the parked one, TP4 an LO
applicant, TP5 a source with no record since reset; while parked, TP6 a close, TP7 a txLA!
walk, TP8 a received Talker value and TP9 the Listener tracker) and, for the leave copy at
every shape, in WK13. One patch per arm, each required to fail its named check (failing
checks of the suite run in brackets):

| Control | Edit | Named failing checks |
|---|---|---|
| `talker-reopen-abandons-leave` | a re-open under another stream_id takes the owing applicant over (LA + Join! is AA, the behaviour before the issue) | TP1 (9), WK13 |
| `talker-park-never-retires` | the park never ends, so the waiting Join! never comes | TP1 (3) |
| `talker-reopen-parks-own-stream` | a re-open on the stream_id the applicant holds parks it as if it were another | TP2 (2) |
| `talker-rebind-back-keeps-park` | the re-open compares with the matcher's stream_id, not the parked one | TP3 (1) |
| `talker-lo-not-owed` | an LO applicant is not counted as owing (only LA is) | TP4 (2) |
| `talker-unrecorded-applicant-owes` | a source with no record since reset owes what its applicant's state says | TP5 (1) |
| `talker-close-keeps-join` | a close while parked keeps the waiting Join! | TP6 (1) |
| `talker-park-retires-on-txla` | a txLA! walk ends the park although the applicant stays LO | TP7 (1) |
| `talker-parked-applies-rx` | received Talker values reach a parked applicant | TP8 (1) |
| `talker-park-keeps-matcher` | a parking re-open keeps the matcher on the old stream_id | TP9 (1) |
| `walk-ignores-talker-leave-copy` | the walk publishes the record's FirstValue while a Leave is parked | TP1 (5), WK13 |

The leave-copy arms, failing checks per shape as in the #230 table:

| Control | Edit | Named failing checks | 1/1 | 2/2 | 3/5 | 9/9 | Caught at |
|---|---|---|---:|---:|---:|---:|---|
| `talker-reopen-abandons-leave` | as above | WK13 | 2 | 3 | 4 | 10 | 4 of 4 |
| `walk-ignores-talker-leave-copy` | as above | WK13 | 1 | 2 | 3 | 9 | 4 of 4 |
| `lwid-read-source-0` | the walk reads source 0's parked {stream_id, DA, VLAN} | WK13 | 0 (equivalent) | 1 | 2 | 8 | 3 of 4 |
| `lwtsp-read-at-gate-source` | the walk reads the parked TSpec at the gate source | WK13 | 0 (equivalent) | 1 | 2 | 8 | 3 of 4 |
| `lwid-written-from-source-0` | the leave copy takes source 0's stream_id | WK13 | 0 (equivalent) | 1 | 2 | 8 | 3 of 4 |
| `talker-leave-copy-rewritten-while-parked` | a re-open while parked writes the leave copy again, with the value never declared | WK13 | 1 | 2 | 3 | 9 | 4 of 4 |

"Equivalent": with one source the edit names source 0, the only one. The `tb/pp_top` MP9
leg grades the same behaviour on the wire, through the in-scope MAAP engine, with the
`moved_open_skips_leave` control of `tb/pp_top/acmp_mutants.py`.

Measured 2026-10-26 at ffc76f1741e3: 207 arm runs KILLED by their named checks, 18
controls PASS, assertion coverage 142 of 142, `226 checks: 226 PASS, 0 FAIL`, driver rc 0.

## Issue #193: sinks on one stream share one declaration

Two or more sinks settled on one stream_id share one Listener applicant, the owner's: the
lowest-index sink declaring it ([10 §6.4](../../docs/architecture/10_srp_engine.md#sec-10-shared-stream);
802.1Q-2018 §10.7.7, §35.1.2.2, the §35.2.6 NOTE, Table 35-14). `RUN_ARGS=share` runs
the group, also part of the default run, on fresh models from reset, each before the
first own LeaveAll. The frames are the wire bytes of the real slot pool.

| Check | What it requires |
|---|---|
| SW1 | two sinks settled on one {stream_id, DA, VLAN} and registered by one Talker Advertise: the first Listener frame is the independent builder's one-declaration frame, byte-exact (one VectorAttribute, New, Ready), both sinks report Ready, and the next three frames carrying the value carry it once each |
| SW2 | a Milan v1.2 §4.2.7.2.2 peer Registrar model, fed every MSRP frame since the Advertise (rLv! takes IN to MT at once; a LeaveAll takes IN to LV, which ends MT after 5000 ms unless the value registers again): after the owner's unbind it is still IN, never deregistered, no Lv went out, the declaration was re-joined and the rider still reports Ready; after the rider's unbind exactly one Lv went out and the Registrar deregistered once |
| SW3 | Ready (sink 0, VLAN 2) with AskingFailed (sink 1, VLAN 3, its talker Failed) on one stream_id: one New, ReadyFailed (FourPacked 3), byte-exact; each sink reports its own type |
| SW4-SW7 | a sink alone on the stream re-binds to another (SW4, SW5 at once; SW6, SW7 withdrawn first) and parks its owed Leave (issue #188); the other sink then settles on the stream and the talker's next Advertise registers it before the next tx! (parker 1 and joiner 0 in SW4 and SW6, the reverse in SW5 and SW7): the first Listener frame is the builder's one-declaration frame, byte-exact (one VectorAttribute, New, Ready, no Lv), the next three frames carrying the value carry it once each, the peer Registrar model above, fed every MSRP frame since the first Advertise, stays IN with no deregistration and no Lv, and the joiner reports Ready |

The stream-FSM half is section SH of [that suite](../srp_stream_fsms/README.md): SH1-SH9
check, tick by tick against the suite's Table 10-3 model of one applicant, the owner and
the riders, both unbind orders, three sinks, a rider's rebind, the type sum, a handover in
the middle of a tx! walk, an owner's A8 and A15 on consecutive clocks, and an owner's A8
with the rider's A15 on the next clock. SH10-SH16 check a parked Leave (issue #188) that
meets a sink on its stream: the four forms of SW4-SW7, tick by tick, with a peer Registrar
fed the pushes; no sink declaring, so the parked Lv goes out once (SH14); a declaration
after the walk sent the parked Lv, which waits for the next tick (SH15); and a sink whose
own Leave goes first, then takes the parked applicant over (SH16). SH17 and SH23 check two
parks: a parked sink re-bound again keeps the pairs of the value it holds, and two parked
Leaves each keep their own group through an own LeaveAll. SH18-SH22 check that each
applicant has one transmit per walk, of the walk's kind, whichever slot holds it: a handover
after the walk passed the taker, from a slot it had not reached, gets that walk's txLA! (a
QA applicant's JoinMt, SH18; an LA applicant's LO, so Empty and not Lv next, SH20) or tx!
(an AN applicant's New, SH19), because the walk goes round again before it ends; a rider
declaring after the last declarer's Lv waits for the next tick (SH21); and a rider settling
after its stream's transmit lets the walk end (SH22). SH24 checks that the walk's done
strobe follows every push of the walk, the second pass's included, for a taker at sink 4 and
a handover at clock 1 to 12 of a tx! or an own LeaveAll's walk: `KL_srp_top` drains the
encoder on the strobe, so the cadence's MRPDU carries them. One patch per arm, each required
to fail its named check:

| Control | Edit | Suite, group | Named failing checks |
|---|---|---|---|
| `listener-settle-compare-dropped` | the settle never finds another sink on its stream_id, so every sink is a group of its own and declares the value itself (the behaviour before the issue, on the wire) | srp_top `share`; stream-FSM suite | SW2, SW3 (3); SH1-SH9, SH16, SH18-SH21, SH24, PS12, PS13, PS20, PS21 (45) |
| `listener-rider-not-quiet` | a rider takes events: a received LeaveAll takes its VO to LO, which sends an Empty for the shared value | stream-FSM suite | SH14, and SH1, SH2, SH4, SH6, SH18, SH19, SH21, SH24 (15) |
| `listener-teardown-applies-lv` | an A8 applies Lv! to its own slot, as before the issue: the owner's unbind leaves LA, and the handover declares it again with New | stream-FSM suite | SH2, SH4, SH7, SH8 (9) |
| `listener-handover-drops-state` | the handover gives the taker a VO applicant, which declares anew (New) | stream-FSM suite | SH2, SH4, SH7, SH8, SH18, SH19, SH24, PS13 (15) |
| `listener-withdraw-ignores-riders` | a sink's group counts only its own declaration, so the owner's unbind withdraws the stream (Lv) while the rider still wants it | stream-FSM suite; srp_top `share` | SH2, SH4, SH6, SH7, SH10-SH13, SH16 (15); SW2, SW4-SW7 (6) |
| `listener-teardown-keeps-declaring` | an A8 leaves the sink's own type set, so the last unbind never withdraws | stream-FSM suite; srp_top `share` | SH2-SH17, SH21, SH23, RB1, RB4, RB7, PS3-PS5, PS13, PS14, PS20-PS23 and the Table 10-3 walks that reach LA through an A8 (84); SW2, SW4, SW6 (3) |
| `listener-group-order-reversed` | the election runs from the highest index: the highest-index declaring sink owns the applicant | stream-FSM suite | SH1, SH3, SH4, SH5, SH8, SH9, SH18, SH19, SH24, PS12 (14) |
| `listener-rider-settle-hands-over` | any member's A15 hands over, a rider's VO included, so the owner's applicant becomes VO and declares anew | stream-FSM suite | SH5, SH9 (2) |
| `listener-type-not-combined` | the declared type is the last declaring sink's, not the sum | stream-FSM suite; srp_top `share` | SH6 (6); SW3 (1) |
| `listener-handover-forgets-walk-mark` | the handover drops the walk mark, so a handover in the middle of a tx! walk gives the applicant a second tx! (a second New) | stream-FSM suite | SH7, SH24 (3) |
| `listener-settle-parks-shared-stream` | an A15 of the sink holding the applicant parks it, with its Lv owed, although a rider still declares | stream-FSM suite | SH8, SH20 (2) |
| `listener-group-op-during-settle` | a group operation does not wait out a settle: the owner's applicant is handed to a rider in the clock that settles the rider on another stream | stream-FSM suite | SH9 (1) |
| `listener-parked-outside-groups` | a parked applicant is in no group (the behaviour before this change): a sink settled on its stream declares the value itself beside the parked Lv | stream-FSM suite; srp_top `share` | SH10-SH16 (7); SW4, SW6 (2) |
| `listener-settle-skips-leave-copies` | a settle never compares its stream_id with the leave copies, so a sink settling on a parked stream does not join its group | stream-FSM suite; srp_top `share` | SH10-SH15 (6); SW4, SW6 (2) |
| `listener-shares-nothing-encoder-appends` (issue #214) | `listener-settle-compare-dropped` and the encoder's `second-queueing` together: every sink declares the value itself, and the encoder appends each push | srp_top `share` | SW1, SW2, SW3, OV0 (6) |
| `listener-merge-keeps-park` | the takeover leaves the parked sink parked, so its new stream is never declared | stream-FSM suite | SH10-SH13 (4) |
| `listener-retire-skips-group-mark` | a walk transmit marks only its own slot, so a declaration later in the walk, after a retired park's Lv or the last declarer's, sends New in the same walk | stream-FSM suite | SH15, SH21 (2) |
| `listener-park-drops-matcher-pairs` | a park does not copy its matcher pairs, so a sink already settled on the parked stream does not join its group when its own park clears | stream-FSM suite | SH16 (1) |
| `listener-repark-takes-matcher-pairs` | a sink parked again takes the pairs of the stream it re-bound to first, so the held value's Lv is lost and that stream gets a second New | stream-FSM suite | SH17 (1) |
| `listener-two-parks-grouped` | two parked applicants are paired by the leave-copy map, so the one holding the value the other's matcher is on goes quiet through an own LeaveAll's walk and sends Lv, not Empty, next | stream-FSM suite | SH23 (1) |
| `listener-walk-ends-owing-a-transmit` | the walk ends with a slot unmarked (the behaviour before this change), so an applicant handed over behind the walk, from a slot it had not reached, has no transmit in that walk | stream-FSM suite | SH16, SH18, SH19, SH20, SH24, and since issue #212 RB1, RB4, RB6, RB7 and PS4, whose one-walk re-bind takes the walk's second pass (12) |
| `listener-walk-marks-only-served-slots` | the walk marks only the slots it serves, so a rider that settles after its stream's transmit stays unmarked and the walk goes round without end | stream-FSM suite | SH22 (1) |
| `listener-walk-marks-group-only-on-retire` | the walk marks a group only when a park retires (the form before this change), so a rider declaring after the last declarer's Lv sends New in the same walk | stream-FSM suite | SH21 (1) |
| `listener-walk-done-before-second-pass` | the walk's done strobe comes at the end of its first pass, the second pass kept, so the second pass's push follows the strobe and misses that cadence's MRPDU | stream-FSM suite | SH24, and since issue #212 SH15 (3) |

The counts are failing checks per run, re-measured at issue #212's head: its rows add
failures under six of these arms, at main 35, 14, 65, 13, 7 and 2, and its later rows
five more under `listener-settle-compare-dropped` (PS20, PS21) and ten more under
`listener-teardown-keeps-declaring` (PS20 to PS23). The assertion-coverage
check requires SH1-SH24 and SW1-SW7. Since issue #214 the encoder keeps one pending entry per value, so a sink that
declares a shared stream's value beside its group's applicant no longer puts a second Listener
vector on the wire: SW1 passes under `listener-settle-compare-dropped`, and SW5 and SW7 under
`listener-parked-outside-groups` and `listener-settle-skips-leave-copies`. Those arms are
still killed in the stream-FSM suite and, end to end, by the Leaves of SW2, SW3, SW4 and SW6:
`listener-settle-compare-dropped` is required to fail SW2 and `listener-settle-skips-leave-copies`
SW4 (SW1 and SW7 before). SW1 is required of `listener-shares-nothing-encoder-appends`, which
removes both layers. Issue #193 moved the context of nine issue #188 patches: `listener-lo-not-owed`,
`listener-lost-txla`, `listener-park-never-retires`, `listener-park-retires-on-txla`,
`listener-rebind-abandons-leave`, `listener-reprobe-abandons-leave`,
`listener-reprobe-parks-own-stream` and `listener-unrecorded-applicant-owes` keep their
edits (those of `listener-lo-not-owed` and `listener-park-retires-on-txla` on lines that now
also carry the handover and walk-mark terms), and `listener-declaration-not-held` drops the
park term of the declaration grant (a request no longer drives the applicant; granted while
parked, it reaches the parked applicant as the group's New!). Each still fails its RB check.
The parked-Leave change moved the context of one patch, `listener-settle-compare-dropped`,
whose edit is unchanged. The one-transmit-per-walk change moved the context of
`listener-park-never-retires` and `listener-retire-skips-group-mark`, whose edits are
unchanged (the latter's now takes every group mark away, so it also fails SH21). The walk
marks also changed one control's named check: with `listener-rider-not-quiet` (every rider
takes events) the rider above the owner in SH1 still turns VO to LO on the received LeaveAll,
but the owner's transmit now marks it, so its Empty cannot go out while the owner declares.
The control is required to fail SH14, which grades the same rule with the rider below the
parked holder; SH1's received-LeaveAll check also requires the rider to stay VO, so it sees
the edit with the rider above the owner too, and the control fails 15 checks in all.

## One attribute value per MRPDU (issue #214)

IEEE 802.1Q-2022 §10.7.7 keeps one Applicant per attribute value, and a value inside two
VectorAttributes of one MRPDU is two events for its state machine (§10.7.5.13 c),
§10.8.2.10.1). The encoder now holds one pending entry per value
([10 §3](../../docs/architecture/10_srp_engine.md#sec-10-one-vector)). `RUN_ARGS=onevec`
runs the group, also part of the default run. Each row places a second reason to send a
value in the transmission window of the first, and reads every MRPDU of the row with
`doubled_values()`. That function expands each VectorAttribute's range by the type's own
increment rule (§35.2.2.8, §35.2.2.9, §11.2.3.1.7), skips Listener lanes declared Ignore
(§35.2.2.7.2), and reports a value of one type that two VectorAttributes carry. Read-only
probes add the `periodic!` strobe and the encoder's drain ticks, which bound each
application's transmission window.

| Check | Stimulus and required behaviour |
|---|---|
| OV0 | every MRPDU of the whole run, in every group above, carries each value in at most one VectorAttribute |
| OV1 | source 0 declared on VID 2 with a peer Ready, sink 0 Ready on a peer Advertise, the peer declaring our Domain. The peer's LeaveAll MRPDU of each application (every type flagged, its values re-declared in the flagged vectors) every 1030 ms for 35 s, so it slides 30 ms per cycle across `periodic!` and across the T-MRP-JOIN tx!: no MRPDU carries Domain {6, 3, 2} twice; at least one window holds a `periodic!` |
| OV2 | the same run: no MRPDU carries VID 2 twice |
| OV3 | the same run: no MRPDU carries the Talker Advertise, the Listener or any other value twice |
| OV4 | the own MSRP LeaveAll in each tenth of the `periodic!` second: a peer LeaveAll restarts the leavealltimer with a fresh draw (Table 10-5 rLA!) until the drawn deadline lands in the tenth. All ten fire, at least one with a `periodic!` in its window, and no MRPDU carries a value twice |
| OV5 | the own MVRP LeaveAll the same way: its LeaveAll MRPDU carries VID 2 once, and no MRPDU carries a value twice |
| OV6 | LINK_UP in each tenth: the first MRPDU carrying Domain {6, 3, 2} carries it New, and none carries it twice |
| OV7 | a differing Domain {6, 5, 5} adopted in each tenth: Lv {6, 3, 2} and New {6, 5, 5}, and no value twice |
| OV8 | DECLARE_TALKER on a new VID in each tenth: the VID goes New, the Talker Advertise is declared, no value twice |
| OV9 | a Listener declared, through the peer's Advertise, in each tenth: New, no value twice |
| OV10 | the transmitter held 250 to 1050 ms from two phases, across join rounds and `periodic!`, then released: no value twice |

Against the base RTL the default run reads `8683 checks: 8675 PASS, 8 FAIL`: OV0 to OV7.
Every existing check passes, and 69 of 17,650 MRPDUs of the run carry a value twice. They
are Domain {6, 3, 2} JoinIn twice after a received LeaveAll, as in the capture, and VID 2
up to eight times in one MVRP MRPDU when several LeaveAll walks fall in one window. They
also include New and JoinMt after LINK_UP, New and JoinIn of an adopted Domain, and, in
the N9 stimulus, adopted Domain values each New and Lv in one MRPDU.
OV8 to OV10 pass at the base too: the stream applicants transmit only at tx!, and no
join request of theirs shares a window with a second one.

N9 filled the pending table with eight adoptions in one window, which the base queued as
sixteen entries. Each adoption's Lv now takes the entry of the New before it, so k
adoptions hold k + 1 entries, and N9 adopts twelve Domain values to fill the table.

Mutation-proven through `mutants.py` (checked-in patches; the `srp_encoder` rows are
listed there):

| Deliberate breakage | Group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `second-queueing` (the search never finds the pending entry: the base's second queueing) | onevec | 8 | OV0 to OV7 |
| `join-displaces-new` (a later join replaces a pending New) | onevec | 2 | OV6, OV7 |
| `listener-shares-nothing-encoder-appends` (every sink declares a shared stream itself, and the encoder appends: both layers that keep one Listener vector per stream removed) | share | 6 | SW1, SW2, SW3, OV0 |

The assertion-coverage check also requires OV1 to OV7.

Eight checked-in patches carried the line `assign ev_ready_o = ...` as context, which this
change rewrites. Each was cut again as the same edit against the new source:
`action-omitted`, `before-slot`, `encoder-peer-ignored`, `my-edge-cancel-lost`,
`preparation-before-slot`, `repeated-action`, `reserved-slot-not-reused`,
`r-reuse-without-action`.

## Each Applicant against Table 10-3, and LeaveAll with nothing declared (issue #216)

IEEE 802.1Q-2018 Table 10-3: a declaration made with New! enters VN; tx! from VN sends New
(AN), tx! from AN sends New again (QA with the Registrar IN, AA otherwise, note 8), tx! from AA
sends sJ (QA). 10.7.6.3: sJ is JoinIn only with the Registrar IN. 10.7.9 and Table 10-5: a full
participant's LeaveAll state machine sends a LeaveAll at each leavealltimer! expiry, declaring
or not; Milan v1.2 Table 4.3 sets that timer to 10 to 15 s, graded with its 0.5 s tolerance.
`RUN_ARGS=applicant` runs the group, also part of the default run. Each row reads the wire:
the messages of one attribute value from every MRPDU of its application, through the real
decoder, cadence and encoder ([10 §6.1](../../docs/architecture/10_srp_engine.md#sec-10-domain-applicant),
[§6.2](../../docs/architecture/10_srp_engine.md#sec-10-vid-applicant),
[§6.5](../../docs/architecture/10_srp_engine.md#fig-10-leaveall)).

| Check | Stimulus and required behavior |
|---|---|
| AP1 | LINK_UP with no peer: the Domain {6, 3, 2} sends New, New, then JoinMt on three consecutive transmit opportunities (180-240 ms apart), the first New within 240 ms of LINK_UP, and no Domain message is a JoinIn |
| AP2 | DECLARE_TALKER on VID 2 with no peer: VID 2 sends New, New, then JoinMt on three consecutive MVRP transmit opportunities, the first within 240 ms of the declaration |
| AP3 | VID 2 with its Registrar MT sends JoinMt, never JoinIn; after the peer's MVRP LeaveAll riding its JoinIn of VID 2 (the Registrar IN) every VID 2 Join is JoinIn; after the peer's Lv of VID 2 (LV, then MT) every one is JoinMt |
| AP4 | the Talker Advertise of that DECLARE_TALKER, and a sink's Listener Ready declared on a registered Talker Advertise: each New, New, then JoinMt, the first New at the first tx! |
| AP5 | from Begin! (reset) with no VID ever declared: an MVRP LeaveAll within 15.5 s and again 9.5 to 15.5 s after each, each the LeaveAll-only MVRPDU byte-exact; the MSRP LeaveAll the same, each MRPDU carrying the Domain's JoinMt (txLA! from QA) |
| AP6 | the Talker, the Listener and VID 2 declared, then each withdrawn: an MVRP LeaveAll within 15.5 s of VID 2's Lv and again 9.5 to 15.5 s after each, each LeaveAll-only byte-exact; the MSRP LeaveAll the same, with the Domain's JoinMt |
| AP7 | eleven adoptions leave eleven Lv entries pending, so the Domain's New at the next tx! is accepted after a value search of eleven entries, later than the stream walks finish: that tx!'s MRPDU carries the eleven Lvs and the New |

Row 0 of the bring-up, the first three Domain MRPDUs byte-exact (New, New, JoinMt), grades the
Domain's ladder the same way, and `tb/pp_top` S1 does on the processor.

Against the base RTL (`044cb25f`) the group reads `18 checks: 9 PASS, 9 FAIL`: AP1 (`New@202
JoinMt@1002`), AP2 and both AP3 rows (`New@401 JoinIn@1201`), both AP5 and both AP6 rows (no
MVRP LeaveAll at all), and AP7. The Talker and Listener ladders (AP4), AP3 with the Registrar
IN, and the MSRP LeaveAll rows of AP5 and AP6 pass at the base: the stream Applicants already
followed Table 10-3, and the MSRP prepared LeaveAll round already had a LeaveAll-only form.

Mutation-proven through `mutants.py` (checked-in patches); each arm must fail its named
check, and the counts are this group's failing checks:

| Deliberate breakage | Failing checks | Named failing assertions |
|---|---:|---|
| `domain-one-new` (`KL_srp_domain.sv`: LINK_UP enters AN, one New, as the base sent it) | 1 | AP1 |
| `vlan-one-new` (`KL_srp_vlan.sv`: a first user's New! enters AN, one New) | 1 | AP2 |
| `vlan-joinin-registrar-mt` (every VID sJ JoinIn, the base's re-join) | 3 | AP2, AP3 |
| `vlan-registrar-never-in` (no received value registers) | 1 | AP3 (the Registrar IN) |
| `vlan-rlv-ignored` (a received Lv leaves the Registrar IN) | 1 | AP3 (after the peer's Lv) |
| `talker-one-new` (`KL_srp_talker_fsm.sv`: tx! from VN goes to AA) | 1 | AP4 (the Talker Advertise) |
| `listener-one-new` (`KL_srp_listener_fsm.sv`: the same) | 1 | AP4 (the Listener Ready) |
| `mvrp-leaveall-needs-content` (`KL_srp_encoder.sv`: an MVRP drain needs a pending VID, as at the base) | 4 | AP5, AP6 |
| `top-mvrp-la-flag-dropped` (`KL_srp_top.sv`: the MVRP round's LeaveAll never reaches the encoder) | 4 | AP5, AP6 |
| `domain-txla-row-ignored` (the Domain's own LeaveAll tx! takes the tx! row: QA sends nothing) | 2 | AP5, AP6 (the Domain's JoinMt) |
| `top-mvrp-drain-at-tick` (the MVRP drain at the tick, before the VID walk: each message one T-MRP-JOIN late) | 1 | AP2 |
| `top-round-ignores-domain` (the MSRP drain does not wait for the Domain's tx!) | 1 | AP7 |

The assertion-coverage check also requires AP1-AP7. The other rows that pinned the earlier
behaviour changed with it, each recorded in its section: 0 (the ladder), O1 and O7 (their
preconditions), N9 (k adoptions hold k entries), F3, F4, P4, P5, P7, Q2, R4, DJ3 and SW1's
setup. The encoder-level rows and arms are in the [srp_encoder README](../srp_encoder/README.md).
