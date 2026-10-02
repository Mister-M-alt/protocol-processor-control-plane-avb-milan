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
`make` = build + run, exit 0 = PASS, **2200 checks**.

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
  the wire, byte-exact, at the first T-MRP-JOIN drain; class-D defaults.
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
  Domain JoinIn + Listener Ready; registrations stay published through LV and a
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
    next T-MRP-JOIN, well before any periodic re-join); neither the
    Listener registration nor sink 0's Advertise ages.
  - **(c)** a Listener-only LeaveAll, never re-declared: Ready stays
    published for T-MRP-LEAVE (LV), then ages to MT and ACTIVE drops; sink
    0's Advertise is untouched. The Domain row's negative: no Domain JoinIn
    follows it before the next periodic re-join, nor, each in its own clean
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
| M: supersession | Each of the four MSRP types before preparation and while allocation is blocked cancels the pending own action. Rejoined registrars are never re-aged and no own flags appear. MVRP cannot cancel it. Since issue #108 an MSRP peer restarts the MSRP timer (a fresh 10-15 s deadline from the peer) and an MVRP peer leaves it unchanged (M4). Six peer/acceptance phases cover -2 through +3 clocks: the peer wins before/on acceptance, and cannot retract an accepted action. A peer from -12 to +1 clocks around real timer expiry leaves no own action: on or after the expiry it drops the new intent, and before it the restart makes that expiry stale (M10; before #108 the -1 case still acted). The sweep crosses rLA!'s draw request, the PRNG's rejection retries and the arm of the restarted deadline, and M9 grades each placement against the calibrated expiry clock, because once the restarted deadline is armed the superseded one never fires. A quiet canceled round waits for real content; a later own action can reuse its reserved slot. |
| N: congestion | Eight sources and eight sinks exceed the 12-entry table. A previous TX request stalls across several cadence ticks; Leaves still clear IN. After acceptance, a held TX request never repeats sLA. Interim and final drains preserve all live declarations and all four type flags, and queued ticks complete. Another case fills the table before acceptance and proves that this initial full condition also drains. Allocation held across two timer expiries produces just one accepted own action. An asymmetric eight-source/one-sink walk preserves the completed walker across blocked cadence ticks. |

The existing periodic-declaration and six-cycle/no-storm assertions retain their
limits. The measured six-cycle counts were 4, 3, 3, 3, 3, 2 (18 total, maximum 4).
Since issue #108 each peer LeaveAll restarts the timer, no own burst lands in
F4, and the counts are 4, 3, 3, 3, 2, 2 (pinned 17 total, maximum 4).
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
`armdelay`, `restart`, `timers` or `join` selects one group for a focused run; plain `make` runs
every existing and new check. The stream-FSM suite independently walks every
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
| `round-completion-lost` | congestion | 1 | N13 |
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


## Round 2: pending-action guards

The default suite includes 73 additional checks; no DUT event is forced.
Reset edges are excluded from the event history because synchronous reset can
interrupt a combinational preparation acceptance without performing an action.

| Check | Stimulus and required behavior |
|---|---|
| O1 | A peer Domain LeaveAll while preparation waits behind a held encoder TX request cancels the eventual action and its wire flags. |
| O2 | After cancellation, a second real timer expiry while allocation remains blocked survives the canceled acceptance and produces exactly one own action/frame. |
| O3 | Peer Domain LeaveAll decoded at -1/0/+1 clocks around the join opportunity cancels before acceptance; both registrar planes remain IN. |
| O4 | With the link down, reuse a retained empty reservation; peer decode at -1/0/+1 around reuse acceptance permits wire flags only when the local action preceded the peer. The exact acceptance clock is checked separately. |
| O5 | Talker Advertise and Failed Leaves on sinks 0 and 7 decode at -1/0/+1 around acceptance. Before/on the edge they clear IN; afterward they retain LV. |
| O6 | Several cadence ticks during blocked preparation produce exactly one immediate follow-up walk after acceptance, before another cadence tick. |
| O7 | A canceled Domain-only round holds its empty reservation for periodic content; with the link up it drains within Periodic plus Join (measured 649 ms, test bound 1200 ms). |
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
| P4 | MVRP Active: right after the own MVRP expiry, a peer MVRP LeaveAll drops the pending flag, the VID re-join still goes out, and the timer restarts. Control without the peer: the flag rides the next drain. |
| P5 | MVRP Active with no VID held (no drain carries the flag): a peer MVRP LeaveAll makes it Passive, so the first VID New carries no own flag. Control: the flag rides that New. |
| P6 | The MVRP lane from -12 to +1 clocks around the real MVRP expiry: no own flag in any case, and a restarted timer (before the expiry the superseded deadline is stale, or never fires once the restarted one is armed). |
| P7 | The MVRP equal edge: after the own MVRP expiry the flag waits for the next MVRP join tick whose drain has content. A peer MVRP LeaveAll at -1 or 0 clocks of that tick drops it (Passive; the rider skips the flag on the peer's edge), at +1 the tick was tx! and the accepted own flag goes out. The timer restarts in all three. |
| P8 (`RUN_ARGS=armdelay`) | The committed form of the reviewer's delayed-arm probe. The wrapper delays the engine's timer arms by `arm_delay_i` clocks, as the processor top's queued arm port does. At delays 3, 4, 8 and 16, one reset scenario per offset puts a peer MVRP LeaveAll around the own MVRP expiry, then a peer MSRP (Domain) LeaveAll around the own MSRP expiry, each from -(delay + 12) to +2 clocks: no offset produces an own LeaveAll of either application, every peer lands at its offset and restarts its application's timer. |

The MSRP twin of P6 is M10. Mutation-proven 2026-09-29 through `mutants.py`
(checked-in patches, each built in a scratch RTL copy):

| Deliberate breakage | Group | Failing checks | Named failing assertions |
|---|---|---:|---|
| `expiry-only-redraw` (MSRP peer does not restart the timer) | restart | 15 | P1,P3 |
| `mvrp-expiry-only-redraw` (MVRP peer does not restart the timer) | restart | 6 | P2,P3,P6 |
| `stale-expiry-honoured` (MSRP superseded deadline acts) | peer | 1 | M10 |
| `mvrp-stale-expiry-honoured` (MVRP superseded deadline acts) | restart | 1 | P6 |
| `mvrp-passive-lost` (MVRP peer keeps the own flag) | restart | 4 | P4,P5,P6 |
| `mvrp-flag-at-expiry` (flag handed to the encoder at expiry, the old shape) | restart | 6 | P4,P5,P6 |

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
| Q2 | periodictimer 900-1500 ms: a quiet declared Talker Advertise re-joins (JoinMt) on each periodic!, and so do the Domain and the MVRP VID (JoinIn); at least five of each before the first LeaveAll | 9 x 1000 ms each |
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
| R4 | DECLARE_LISTENER sink 0 on VID 7, which no source holds: byte-exact MVRP VID 7 New, then JoinIn every 900-1500 ms (3 x 1000 ms measured), and WITHDRAW_LISTENER of its last user: byte-exact MVRP VID 7 Lv, no membership left. |

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
| `join-sent-at-handover` (VLAN marks a VID sent when the encoder takes its New) | srp_top join | 3 | R1,R3 |
| `join-sent-at-handover` | srp_encoder | 3 | W2,W3,W4 |
| `count-up-unsends` (a second user resets the VID's sent state) | srp_top join | 3 | R2,R3 |
| `count-up-unsends` | srp_encoder | 2 | W4 |
| `tx-strobe-any-app` (the transmission strobe also fires for MSRP) | srp_encoder | 1 | W1 |
| `listener-lane-cut` (`vu_sel_ls_w` forced 0, the #65 acceptance arm) | srp_top join | 3 | R4 |
