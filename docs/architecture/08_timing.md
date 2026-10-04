<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# 08 — Timing, Timers, Deadlines

## 1. Role

Single source of truth for **every time constant** (T-IDs), the timer hardware that
implements them, and the response-latency budgets. No other document states a value.

## 2. Master constant table

<a id="fig-08-constants"></a>**F08.1 — All T-IDs** (profile column: value in a
plain-IEEE build where different; blank = same).

| T-ID | Value | Owner | Meaning | Clause | IEEE profile |
|---|---|---|---|---|---|
| T-ADP-ADV | 5 s fixed | ADP | re-advertise period (valid_time = 10 ⇒ 20 s validity) | Milan §5.6.2/.3 | valid_time/2 model |
| T-ADP-DELAY | random 0–4 s | ADP | pre-advertise anti-storm (LINK_UP, DISCOVER, TMR, GM_CHANGE) | Milan §5.6.3.5.3–.7 | randomDeviceDelay (ms-scale) |
| T-ADP-DELAY-START | random 0–2 s | ADP | startup-link-up variant — distinct constant | Milan §5.6.3.5.2 | — |
| T-ADP-NOADP | rx valid_time (20 s typ.) | ADP | bound-talker aging | Milan §5.6.4.5.1 | |
| T-ACMP-CMD | 200 ms | ACMP/originator | all five ACMP command timeouts, 2 attempts | Milan Table 5.26 | 2000/4500/500/200 ms |
| T-ACMP-DELAY | random 0–1 s | ACMP | listener pre-probe anti-storm | Milan Table 5.29 | — |
| T-ACMP-RETRY | 4 s | ACMP | probe backoff after failure/error | Milan §5.5.3.5.23 | — |
| T-ACMP-NOTK | 10 s | ACMP | settled: max wait for matching talker attribute | Milan §5.5.3.5.18 | — |
| T-AECP-RESP | 240 ms | AECP | hard respond budget (never IN_PROGRESS) | IEEE §9.3.2.6; Milan §5.4.3.4 | |
| T-AECP-TIMEOUT | 250 ms | originator | inflight timeout for originated AECP commands (+1 retry) | IEEE §9.3.2.6 | |
| T-AECP-INPROG | 120 ms | — | IN_PROGRESS cadence — **unused by policy** | IEEE §9.3.2.6 | |
| T-NOTIF-MONITOR | random 30–60 s | registry | per-controller departing detection | Milan §5.4.5.3 | — |
| T-NOTIF-TIMELIMITED | 300 s | registry | TIME_LIMITED registration expiry (controllers re-register at 100 s) | IEEE §7.4.37.2 | |
| T-LOCK-UNLOCK | 60 s | lock mgr | auto-unlock + notification | Milan §5.4.2.2 | |
| T-IDENT-BURST | 150 ms ×3 | identify | IDENTIFY_NOTIFICATION triple: each later frame due T-IDENT-BURST after the first ms boundary following its IDENT-BURST arm, which the previous frame's departure (its last byte to the MAC) makes on its next clock or later, so never less than T-IDENT-BURST after that departure; the next burst's first frame no sooner after the third, so a late frame or a TX stall delays the rest and never shortens a gap; a press made while a burst or that gap runs is latched and its burst starts when the gap ends | IEEE §7.5.1, §7.5.1.2.1 | |
| T-IDENT-REARM | 1 s | identify | re-arm while button held: Figure 7-142's timeout, from the burst's first frame | IEEE §7.5.1.2.1, §7.5.1.3 | |
| T-CTR-OBSERVE | ≤ 1 s tick | integrator's counters (`ctr_*` face) | observation-interval commit of the interval counters; the processor keeps no tick for it ([06 §6.6](06_aecp_engine.md#sec-06-counters)) | Milan §5.3.7.7, §5.3.8.10 | |
| T-CTR-NOTIF | 1 s | notif engine | ≥ 1 s between GET_COUNTERS notifications per descriptor | Milan Table 5.22 | |
| T-ACMP-DA-RETRY | 100 ms | talker DA gate | allocation retry round for enabled NO_DA sources; one attempt/source/round | implementation policy, [05 §6bis](05_acmp_engine.md#6bis-talker-side-stateless-responder) | |
| T-SRP-DAFRESH | 15 s | talker DA gate | PROBE_TX freshness window for DA validity | Milan §4.3.3.1 | — |
| T-SRP-LEAVEALL2 | 2 × T-MRP-LEAVEALL ≈ 20–30 s | talker DA gate | backoff after MAAP conflict / PCP change | Milan Table 5.3 | — |
| T-MAAP-PROBE | random, strictly 500 ms < T < 600 ms | MAAP engine (11) | probe_timer — a fresh draw at every start | 1722-2016 B.3.4.2, Table B.8 | |
| T-MAAP-ANNOUNCE | random, strictly 30 s < T < 32 s | MAAP engine (11) | announce_timer — a fresh draw at every start | 1722-2016 B.3.4.1, Table B.8 | |
| T-MRP-JOIN | 200 ms (180–240) | SRP engine (10) | MRP joinTime — join tx cadence + vector aggregation window | Milan Table 4.3 | |
| T-MRP-LEAVE | 5000 ms (4500–7500) | SRP engine (10) | MRP LeaveTime — registrar LV expiry during LeaveAll (Δ13 removes the rLv path) | Milan Table 4.3 | 600–1000 ms (802.1Q Table 10-7) — coupled to Δ13: change both or neither |
| T-MRP-LEAVEALL | random 10–15 s | SRP engine (10) | leavealltimer per participant — a fresh draw at every start: Begin! (reset), its own expiry, and a received LeaveAll of that participant (802.1Q Table 10-5 rLA!, [10 §6.5](10_srp_engine.md#fig-10-leaveall)) | Milan Table 4.3 | |
| T-MRP-PERIODIC | 1000 ms (900–1500) | SRP engine (10) | periodictimer — periodic re-join transmissions | Milan Table 4.3 | |
| T-NVM-DEBOUNCE | 500 ms: 500 ticks of the 1 ms `tick_ms` (`DEB_TICKS_P` in the binding manager, `DEB_MS_P` in the D3 writer) | NVM mgr, D3 writer | the producer's first-dirty window: the first change opens it, its close arms one burst that drains every dirty record (the integrator's own firmware window, 1,000 ms, is separate and not this processor's) | parent DR2a ruling | a tick counter in each producer, not a timer-service slot |
| T-NVM-RS-DEADLINE | `P-NVM-RS-TMO-CYC` clocks without progress (20 ms, ceil(`P-CLK-HZ` × 20 / 1000); **ratified**, DR3a) | NVM mgr, D3 writer | per restore wait: the binding walk's read phase, and every wait of the D3 walk (reads, descriptor reads, the judge, the roll-back's debt wait, the re-LOCATE). Expiry fails that walk and abandons an issued read to the drain ([07 §5.3](07_memory_maps.md#fig-07-nvmflow)) | parent DR3a ruling (persistence that wedges must not hold the entity) | a clock counter in each walk, not a timer-service slot |
| T-NVM-RS-AGGREGATE | `P-NVM-RS-AGG-CYC` clocks from the accepted restore start (1,000 ms, ceil(`P-CLK-HZ` × 1000 / 1000) = `P-CLK-HZ`; **ratified** as an enforced bound, DR3a) | D3 writer | the whole restore: from `restore_go_i` through the binding walk, both D3 passes and the roll-back to the terminal. At the bound the phase the restore is in takes the path a stalled wait takes in it (cause 3), once, in the first clock whose wait has no event in hand, and a provable image is never closed: a binding walk still reading fails whole and releases the listener, and the D3 walk then proves the image with no record read and ends DEFAULTS (CLOSED only if the image cannot be proven, cause 7); in pass 0 DEFAULTS; in pass 1 the roll-back; during the roll-back (its debt wait or re-LOCATE) CLOSED ([07 §5.3](07_memory_maps.md#fig-07-nvmflow)). A device that answers every wait just inside `T-NVM-RS-DEADLINE` ends here, within one such deadline of it plus a few clocks, or within two plus a few clocks when a roll-back follows | parent DR3a ratification (a slow but live device must not hold AECP and the enable) | a clock counter in the D3 writer, not a timer-service slot |
| T-NVM-PORT-DEADLINE | `P-NVM-MEM-TMO-CYC` owed clocks in which the device owes the NVM port an event and presents none, counted from its last event (1,000 ms, `P-CLK-HZ` clocks, **derived**: 20 × the parent backend's longest legal stall, its 50 ms mutating-grant hold, and equal to T-NVM-RS-AGGREGATE) | NVM port | every wait of the port on its device: a grant to a request, a byte it presents or is ready for, the terminal of a command whose data phase is over and not already latched. A cycle the port or the manager holds the operation pauses the count and never restarts it, so no manager handshake pattern holds a silent device off; every event of the device restarts it. Expiry ends the operation with one `err`, cause DEADLINE, and keeps an abandoned command owed until the device ends it or a reset ([02 §8](02_interfaces.md#sec-02-nvm-deadline)) | processor issue #15 and its ruling (a silent device must reach the retry and alarm path, never wedge the port) | a clock counter in the port (`MEM_TIMEOUT_CYC_P`, class E's watchdog idiom), not a timer-service slot |
| T-NVM-RETRY-BACKOFF | `P-NVM-RETRY-BACKOFF-CYC` clocks (500 ms) from a failed record write | NVM mgr, D3 writer | DR2c: at most three attempts per record (`RETRY_MAX_P` = 2 retries), this wait before each retry, then the reset-sticky `nvm_alarm_o` | parent DR2c ruling | a clock counter in each producer, not a timer-service slot |
| T-TX-AGING | 10 ms (design) | TX arbiter | starvation promotion | design | |
| T-BUDGET-ACMP-RESP | ≤ 50 ms (design) | budgets | see §4 | design | |
| T-BUDGET-AECP-TYP / -WC | ≤ 20 ms / ≤ 100 ms (design) | budgets | see §4 | design | |

The original document's single 187.5 ms target is **superseded** by §4
([GAP-07](../00_MILAN_COMPLIANCE_REVIEW.md#gap-07)).

<a id="sec-08-nvm"></a>**The saved-state times, separated** (parent
[D3 contract](https://github.com/kebag-logic/milan-fpga/blob/7a7582f0/docs/design/SAVED_STATE_MATERIALIZATION.md) §5.1, §6.1, §6.3 and its §15.1 rulings). The clocks
below govern persistence; none stands for another.

| Time | Where it runs | Value and unit | Status |
|---|---|---|---|
| producer debounce | each record producer (binding manager, D3 writer) | `T-NVM-DEBOUNCE`, 500 ms of `tick_ms` | ruled (DR2a); measured acceptance-to-durable time is published per writer lane |
| firmware debounce | the integrator's snapshot writer | 1,000 ms first-dirty window | ruled (DR2a); not this processor's |
| producer retry | each record producer | at most **three attempts** per record (the first write and `RETRY_MAX_P` = 2 retries), each retry `P-NVM-RETRY-BACKOFF-CYC` = ceil(`P-CLK-HZ` / 2) clocks (500 ms) after the failed attempt's error; 50,000,000 clocks at 100 MHz, 25,000,000 at 50 MHz | ruled (DR2c) |
| firmware retry | the integrator's snapshot writer | at most three transaction attempts per unchanged captured work set, 1,000 ms apart | ruled (DR2c); not this processor's |
| per-wait restore deadline | each restore walk | `T-NVM-RS-DEADLINE`, 20 ms = ceil(`P-CLK-HZ` × 20 / 1000) clocks; 2,000,000 at 100 MHz, 1,000,000 at 50 MHz | **ratified** (DR3a) |
| aggregate restore deadline | from the accepted restore start (`restore_go_i`, the integrator's `PP_CTRL[1]`) to COMPLETE, DEFAULTS or CLOSED, roll-back included | `T-NVM-RS-AGGREGATE`, 1,000 ms = `P-CLK-HZ` clocks; 100,000,000 at 100 MHz, 50,000,000 at 50 MHz | **ratified** as an enforced bound (DR3a): a counter in the D3 writer, not a measured budget |
| device-face deadline | the NVM port, every wait on its device | `T-NVM-PORT-DEADLINE`, `P-NVM-MEM-TMO-CYC` = `P-CLK-HZ` clocks (1,000 ms); 100,000,000 at 100 MHz, 50,000,000 at 50 MHz | **derived** (processor issue #15 ruling): 20 × the parent backend's 50 ms mutating-grant hold, its longest legal stall; equal to the aggregate restore deadline, and 50 × the per-wait one, so the walks still fail first |
| media deadlines | the integrator's device and flash | the device's own | outside this processor; the port's deadline bounds each event the device owes, never the media's own work |

One conversion and one rounding rule serve every clock count above: t ms of the core clock
is ceil(`P-CLK-HZ` × t / 1000) clocks, which the top writes reduced (ceil(`P-CLK-HZ` / 50),
ceil(`P-CLK-HZ` / 2), `P-CLK-HZ`) so it never multiplies or overflows. The processor lane
measured both DR3a numbers (healthy restores end within 2,661 clocks, the longest healthy
wait is 853) and the manager ratified them
([parent #70](https://github.com/kebag-logic/milan-fpga/issues/70#issuecomment-5873060660)):
20 ms per wait, and 1,000 ms in all as an **enforced** bound, because each wait alone lets a
device that answers every wait just inside 20 ms stretch the D3 walk over some 970 waits.

**One alarm.** `nvm_alarm_o` is the only reset-sticky persistence alarm, and only a
producer's **own** write-attempt exhaustion raises it: the third failed WRITE of one
record by the binding manager or the D3 writer. A producer's record is retired at the
untainted `done` of its window write: from then the integrator's backend owns it
(parent D3 §7.1), and no flash-slot outcome feeds back to the producer. A firmware
transaction that exhausts its three attempts is therefore not a producer failure and
raises no alarm here: it is the [FASTCONNECT §9.2](https://github.com/kebag-logic/milan-fpga/blob/7a7582f0/docs/design/SAVED_STATE_FASTCONNECT.md#92-when-it-sets-when-it-is-revoked-and-when-the-loss-is-forgiven)
verdict loss, reported by the integrator's status without an ACK of the failed slot.
No later success and no heartbeat clears `nvm_alarm_o`; only reset does.

**The quarantine is the port's, and it is bounded** (processor issue #15). A device that
never ends an abandoned read, or never answers at all, is answered by the port's deadline:
one `err`, cause DEADLINE, which ends the arbiter's drain
([02 §8.2](02_interfaces.md#82-two-record-managers-one-port)), and the abandoned command
stays owed: the port requests nothing over it until the device's own terminal or a reset,
so nothing is released on time. Every later change then meets the owed command and is
answered DEADLINE: three attempts, `T-NVM-RETRY-BACKOFF` apart, then `nvm_alarm_o`, which
drops the change's pending bit. A WRITE the port abandoned on a device that waits for its
next byte for ever is contained the same way until reset: padding it would close a record
whose bytes the manager never supplied, and an abort would be a new interface. Issue #15's
reusable service, as ruled: `busy` falls at the `err`, and every later request is answered
within the bound, served once the device has ended the abandoned command.

## 3. Timer hardware

<a id="fig-08-timerhw"></a>**F08.2 — Timebase, pools, PRNG**

```mermaid
flowchart LR
  clk["core clock P-CLK-HZ"] --> ps["prescaler -> 1 µs tick -> 1 ms tick"]
  ps --> ramsweep["deadline RAM sweep @1 ms: P-TIMER-SLOTS x {armed, owner tag, deadline_ms}"]
  ramsweep --> evb["expiry event bus -> event router (owner-tagged)"]
  prng["PRNG 64-bit (LFSR/xoshiro class)"] --> draw["range draw: 0-1 s / 0-2 s / 0-4 s / 10-15 s (T-MRP-LEAVEALL) / 30-60 s / 501-599 ms + 30.001-31.999 s + pool offset (MAAP kinds 5-7)"]
  seed["seed = entity_id XOR free-running counter latched at first link-up"] --> prng
  draw --> ramsweep
```

- One arm/cancel port serves every engine. The top queues each engine's arm face
  four deep (eight faces, each a ring in distributed RAM since issue #639) and drains
  one arm per clock in a fixed priority, listener first and notification last; per
  face order is kept, and an arm offered to a full queue is dropped and counted
  (snapshot word 24).
- All protocol timers use 1 ms resolution (smallest constant 150 ms; randomized draws
  quantize to 1 ms). Deadlines are absolute ms timestamps compared on sweep — arming is
  O(1), expiry detection bounded by `P-TIMER-SLOTS` per ms.
- PRNG seeding follows IEEE §6.2.4.2.2 practice (MAC/EID + time source, sequence
  length ≥ 2³²−1); range reduction by rejection so draws are unbiased.
- Verification hook: a **time-compression factor** on the prescaler (sim-only) scales
  every constant uniformly ([09 §3](09_verification.md), TIM).

## 4. Deadline budgets

<a id="fig-08-budget"></a>**F08.3 — AECP/MVU response window (schematic, ms axis)**

![fig-08-budget](../diagrams/wavedrom/fig-08-budget.svg)

<details>
<summary>WaveDrom source (editable)</summary>

```wavedrom
{"signal": [
  {"name": "command rx",      "wave": "10.........", "node": ".a........."},
  {"name": "execute + build", "wave": "01....0....", "node": "......b...."},
  {"name": "response on wire","wave": "0.....10...", "node": ""},
  {"name": "design targets",  "wave": "x=....=....", "data": ["t0", "TYP <= 20 ms / WC <= 100 ms"]},
  {"name": "shall respond",   "wave": "x........=.", "data": ["240 ms (T-AECP-RESP)"]},
  {"name": "ctrl timeout",    "wave": "x.........=", "data": ["250 ms (+1 retry)"]}
],
 "edge": ["a~>b execution window"],
 "head": {"text": "t0 = completion of command reception; no IN_PROGRESS extension exists in this design"}}
```

</details>

| Protocol | Hard limit | Design budget | Rationale |
|---|---|---|---|
| ACMP responses | initiator times out at `T-ACMP-CMD` (200 ms, 2 attempts) | **T-BUDGET-ACMP-RESP ≤ 50 ms** | leaves ≥ 150 ms network + initiator margin inside a single attempt |
| AECP/MVU responses | respond ≤ `T-AECP-RESP` (240 ms) | **≤ 20 ms typical; ≤ 100 ms worst-case** (oversize READ_DESCRIPTOR, full GET_DYNAMIC_INFO batch, 16-way fan-out contention) | 2.4× margin at worst case |
| ADP DISCOVER response | within the delay window | `T-ADP-DELAY` draw | anti-storm by design |
| Unsolicited fan-out | no protocol deadline | ≤ 1 frame-time gap injection (not met as written: the finding below) | never starves solicited traffic ([03 §8](03_packet_engine.md)) |

**Realized for AECP (issue #81).** The normalizer stamps every transaction's
`deadline` as its reception plus the class budget (`T-BUDGET-AECP-WC` for AEM,
MVU and AA). `protocol_processor_top` reads it at the AECP admission and
compares it with the ms timebase every clock: one register and one comparator,
because the engine is single-issue, and no timer-service slot, so F08.4 is
unchanged. A command still executing when it passes is preempted into the forced
FAIL_SAFE response ([03 §6](03_packet_engine.md) rule (e)). The redirect waits
for the op in progress, whose wait is watchdog-bounded: the longest is a
descriptor burst of at most 72 beats of `DESC_MEM_TMO_CYC_P` = 4,096 clocks,
about 2.95 ms at `P-CLK-HZ`, and `E_DLKILL` plus FAIL_SAFE then cost about
twenty clocks and one response-buffer lane write. The forced response is
therefore queued a few milliseconds after the 100 ms line, well inside the
140 ms rule (e) keeps before `T-AECP-RESP`. A program already past its first
effect answers for itself, and each op it has left is bounded the same way. The
one AECP record held through the boot restore is re-armed at its admission
(rule (d)). ACMP's stamped deadline has no kill consumer: the budget above is a
design target the TIM suite asserts ([09 §8.3](09_verification.md#83-the-aecp-deadline-and-the-hazard-classes-issues-81-57-84)).

**Measured (issues #57 and #81, `tb/pp_top` section TB).** MAC command byte 0
to MAC response byte 0, in the suite's fifth build, whose timebase is the nominal
clock's own so the deadline never cuts a measurement, at the reference SoC's 143
clocks per memory access unless stated:

| Stimulus | Clocks | Share of its line at `P-CLK-HZ` |
|---|---|---|
| GET_MILAN_INFO / an unimplemented MVU command | 746 / 169 | 0.003 % of `T-AECP-RESP` |
| oversize READ_DESCRIPTOR (576-byte descriptor, 618-byte frame) | 12,587 | 0.052 % |
| GET_DYNAMIC_INFO with all thirteen §7.4.76.2 getters, once each (not a batch that fills the 524-octet response) | 12,852 | 0.054 % |
| the same three behind a 15-frame notification fan-out | 12,709 / 24,550 / 24,681 | at most 0.103 % |
| GET_MILAN_INFO, response memory at 4,000 clocks per access | 16,174 | 0.067 % |
| ACMP GET_RX_STATE / GET_TX_STATE, idle and under that load | 172 / 154, unchanged | 0.003 % of `T-BUDGET-ACMP-RESP` |

The budget table's "Unsolicited fan-out" row states a design target the RTL
does not meet as written: `KL_aecp_notify` holds the AECP command path while a
notification class drains, so its event queue stays lossless, and a solicited
command that arrives during a fan-out waits for that class's last frame, then
runs. The wait is bounded by the queue, and it is measured above; it is
recorded as a finding, not changed here.

`T-BUDGET-ACMP-RESP` is measured beside AECP work an ACMP transaction does not
conflict with. One that conflicts with an AECP hold under F03.7 waits at
admission for that hold to end ([03 §6](03_packet_engine.md)). The holds it can
meet are a SET_CONFIGURATION barrier, a LOCK_ENTITY against a stream step, an
ADD/REMOVE_AUDIO_MAPPINGS against any stream step (the `MAP_CFG` class-wide
cross-lock), and an AECP command on the same stream key. The AECP command
bounds each hold in one of two ways:

- A command the deadline preempts holds until its forced response at the
  latest: up to `T-BUDGET-AECP-WC` from its reception, plus the op in progress
  and the forced response, a few milliseconds (above).
- LOCK_ENTITY and ADD/REMOVE_AUDIO_MAPPINGS are never preempted, because their
  change rides a gather face ([06 §8.1](06_aecp_engine.md)). Past their
  deadline they run the rest of their own program to its END. The engine's
  shared gather watchdog bounds each face wait in it, at `DESC_MEM_TMO_CYC_P`
  = 4,096 clocks at this top. LOCK_ENTITY makes two registry-face waits, about
  82 µs at `P-CLK-HZ`. A mapping edit of N mappings makes at most 2N + 3
  edit-face waits. N is at most 63, because the engine refuses a cdl past 524,
  so that is at most 528,384 clocks, about 5.3 ms. REGISTER and DEREGISTER,
  also never preempted, meet no ACMP transaction.

Such an ACMP answer can therefore come later than the 50 ms design budget,
though inside `T-ACMP-CMD` of the first attempt. Section HZ grades that the
ACMP transaction waits; how long it waits is not measured.

## 5. Timer allocation and sizing

<a id="fig-08-alloc"></a>**F08.4 — Ownership × multiplicity → `P-TIMER-SLOTS`**

| T-ID | Instances | Count |
|---|---|---|
| T-ADP-ADV / T-ADP-DELAY(-START) | per interface (one shared slot — SM is in exactly one timed state) | 1 × IF |
| T-ADP-NOADP | per sink | 1 × SI |
| T-ACMP-{CMD, DELAY, RETRY, NOTK} | per sink (one shared SM slot — states are exclusive) | 1 × SI |
| T-SRP-DAFRESH / T-SRP-LEAVEALL2 | per source (shared slot) | 1 × SO |
| T-NOTIF-MONITOR + T-NOTIF-TIMELIMITED | per registry entry | 2 × CTRL × IF |
| T-AECP-TIMEOUT (CA inflight) | pool | P-CA-POOL |
| T-LOCK-UNLOCK, T-IDENT-BURST, T-IDENT-REARM, T-CTR-OBSERVE, T-NVM-DEBOUNCE | singletons, in that order (`KL_aecp_notify` owns the first three; T-IDENT-BURST and T-IDENT-REARM are armed only with P-EN-IDENTIFY-NOTIFICATION, owner tags 0xB1 and 0xB2/0xB3, two REARM generations; the T-CTR-OBSERVE and T-NVM-DEBOUNCE slots stay reserved and unused: see below) | 5 |
| T-MAAP-PROBE + T-MAAP-ANNOUNCE | one SM per entity (one block claim, [11](11_maap_engine.md)) | 2 |
| T-MRP-{JOIN, LEAVEALL} × 2 participants + T-MRP-PERIODIC + registrar-leave pool (T-MRP-LEAVE, active only during LeaveAll: SI + SO stream registrars + the Domain and MVRP VID registrars) | per interface, when `P-EN-SRP-ENGINE` | (7 + SI + SO) × IF |

`P-TIMER-SLOTS = IF + SI + SI + SO + 2·CTRL·IF + P-CA-POOL + 5 + 2 [+ (7 + SI + SO)·IF with the SRP engine]`
(+`T-CTR-NOTIF` implemented as a per-descriptor last-sent timestamp, not a timer slot).
The persistence times take no timer-service slot: each record producer counts
`T-NVM-DEBOUNCE` in `tick_ms` ticks and `T-NVM-RETRY-BACKOFF` in clocks, and each restore
walk counts `T-NVM-RS-DEADLINE` in clocks, all in counters of their own (two producers,
two walks). The T-NVM-DEBOUNCE singleton keeps its reserved slot so no later base moves,
and so does the T-CTR-OBSERVE singleton: the observation interval is the integrator's,
which keeps the counters ([06 §6.6](06_aecp_engine.md#sec-06-counters)).
The MAAP pair was **appended after the singletons** so every earlier base — the ones
landed engines' default parameters already point at — stays put; only `base_end` and
the SRP block moved, by exactly 2. Baseline example (1 IF, 8 + 8 streams, 16
controllers, CA pool 4): `1 + 8 + 8 + 8 + 32 + 4 + 5 + 2 = 68`, plus the SRP
engine's `7 + 8 + 8 = 23` → **91** slots — one 91 × 40-bit deadline RAM (3,640 bits:
still the same single RAMB18 the 66-slot baseline used).

### 5.1 The map: order is the contract, spacing is the shape

The **row order** above is normative — engines are parameterized with a base slot
and index it as `base + instance`, so moving a group renumbers everything after it.
The **sizes** are not: each group's extent depends on `SI` / `SO` / `IF` / `CTRL`, so
every base is the running sum of the extents before it, computed once in
`pp_pkg::pp_timer_map()` and read from there by `protocol_processor_top`. Written as
literals the map is correct at exactly one shape: at `SI = SO = 9` the 8-stream
literals put ACMP listener sink 8 on the talker base and SRP talker 8 on the SRP
listener base. The first is a **lost** deadline (the ACMP engines filter expiries by
owner tag, and theirs differ); the second is a **misdelivered** one (ADP and SRP
filter by slot, with no owner discrimination). Neither raises an error or moves a
counter. The 02 §5 event-router source map has the same shape-dependence and the same
cure (`pp_pkg::pp_evr_map()`), with no owner tag at all to fall back on.

The expiry bus carries `{slot, owner}` in a **fixed** 8-bit owner space
(`pp_pkg PP_OWN_*`: listener `0x20`, SRP talker `0x40`, ACMP talker `0x50`, SRP
listener `0x60`, SRP cadence `0x80`, MAAP `0x90`; ADP publishes its slot *as* its
owner tag). That
space does not scale with the shape, so it is not re-spaced — it is **bounded**: an
elaboration guard in the top refuses to build a shape whose owner ranges would
overlap. The current allocation admits up to 16 sources and 31 sinks. Both maps and
the guard are graded by the `timer_map` suite ([09 §3](09_verification.md)).

## 6. Cross-references

Consumed by every engine (§9 sections of [04](04_adp_engine.md)/[05](05_acmp_engine.md)/
[06](06_aecp_engine.md)); CDC/tick generation contract in [02 §2](02_interfaces.md);
TIM verification category in [09 §3](09_verification.md). Covers REQ-ADP-001/008/014,
REQ-ACMP-003/015, REQ-AEM-024, REQ-MVU-005, REQ-NOT-004 timing aspects.
