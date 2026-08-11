<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Resource & Effort — implementing this architecture on the reference platform

**What this is.** A quantified answer to two questions: *how much resource can this
architecture save*, and *how much effort does implementing it add* — measured against
the reference platform, [milan-fpga](https://github.com/kebag-logic/milan-fpga)
(Artix-7 xc7a100t, ALINX AX7101): a shipping, silicon-validated Milan v1.2 PAAD whose
existing IEEE 1722.1 control plane is the incumbent this architecture would replace.
The intended consumption model is the one [`../hdl/README.md`](../hdl/README.md)
defines: RTL authored in this repository, pinned into the consumer as a git submodule.

**Method.** Block-by-block inventory of this architecture mapped onto the consumer's
module tree; resource deltas anchored to the consumer's measured area records
(`docs/design/AREA_BUDGET.md` and `docs/design/AREA_80_CAMPAIGN.md` in that repo,
Vivado post-synthesis hierarchical utilization plus placed-design censuses); effort
mined from the consumer's own git history (its 1722.1 plane is the direct comparable);
then an independent adversarial verification pass over every number. Figures that
exist only in the consumer's internal build-campaign records (not in its tracked
documents) are marked *(internal measurement)*. Every figure for a block that does not
yet exist is an **ESTIMATE** and names its measured anchor. Date of record: 2026-08-11.

---

## 1. The two answers

| Scenario | Net LUT | BRAM | Slices | Effort (person-days*) | Verdict |
|---|---|---|---|---|---|
| **A — adopt as written** (bolt this architecture's shared infrastructure onto the consumer, keep its engines) | **≈ +3,000** (cost) | ~0 | ~0 | ~140–350 | Do not. Shares what is already shared; unshippable density |
| **B — full replacement** (P0–P5: build everything here, swap behind a build parameter) | **−2,750 … +4,550**, central ≈ **+800** (cost) | +206…+284 kbit | ~0 | **325–807** | Not worth it for area: central lands *past* the conformance cliff, in a density bin that has closed 0 of 7 attempts |
| **C — retargeted µ-coded engine** (aim the µCPU at the emit-engine mass, keep everything else) | **−350 … −3,400**, central ≈ **−1,900** (saving) | +103…+136 kbit | ~0 | **131–321** | The only version that saves. Unproven until a 1–2-day out-of-context synthesis of the µCPU skeleton |
| **D — consumer-side fixes only** (registry→16, FIFO 1→4 KiB, wire-form fixes, tier-0 deletes, constant-table serialisation) | **−1,030** (measured-anchored) | ~0 | ~0 | **12–25** | Best return per person-day by an order of magnitude. Needs no submodule and none of this architecture |

\* A "person-day" is an **AI-agent-assisted lane-day** at the velocity mined from the
consumer's history (single developer + agent lanes, §8) — *not* a human-team day.

**In slices the answer is ~0 in every scenario** — see §2. **Timing-closure risk is a
separate, non-linear cost** in every scenario that changes the shipped netlist, and it
is never included in a person-day figure (§8).

The recommendation that falls out (§10): take **D** now in the consumer; run the
**1–2-day µCPU skeleton experiment** that decides whether **C** is real; do **P0**
(fix this spec's own defects, §9) regardless, because this document set is being read
as a specification today; and do not pursue **A** or **B** for resources — the
architecture's value is conformance correctness, not area.

---

## 2. Measurement discipline (read before quoting any number)

The reference platform's area records enforce rules that this analysis inherits.
Violating them has repeatedly produced confident, wrong conclusions there.

1. **The currency is LUT, then packing density (LUT/slice), then the timing
   lottery — never slices.** The consumer's die holds slice occupancy at 15,847 of
   15,850 while LUT sits at 83.84 %: slices are saturated and barely respond.
   Deleting a block frees only the slices it owns *exclusively*; a placed-design
   census found the consumer's entire 1722.1 plane (19,370 cells across 3,517
   slices) owns just 1,603 exclusively, and a whole-plane deletion A/B measured
   −298 slices for −9,622 LUT *(internal measurements)*. Net slice delta for any
   block-scale change: **~0, bounded −20…+20, against a ±65 σ rebuild noise
   floor.** No slice figure below is quoted as a benefit or a cost.
2. **Baseline (the consumer's shipping build):** 53,156 / 63,400 LUT = 83.84 %,
   density 3.35 LUT/slice. Its conformance-timing cliff sits at **84.77 %**
   (53,745 LUT): headroom **+589 LUT**. Closure rate by density bin over its
   123-build campaign *(internal measurements)*: 3.3–3.4 → 28 %, 3.4–3.5 → **0 of
   7**, 3.5–3.6 → 10 % (n = 20); the highest density that ever closed is 3.586.
   Because slice count moves up to ±237 for bit-identical netlists on placer
   directive alone, every density below carries a **±0.015 band**, and a bin
   verdict within ~0.05 of a boundary is not decidable. "0 of 7" has a ~35 %
   upper 95 % bound (rule of three) — read it as *never observed*, not *impossible*.
3. **FF is at 42 % and BRAM at 67 % (44 free tiles) on that die.** LUT→BRAM
   restructuring is the only real resource lever; a change that only saves flip-flops
   buys nothing. Every LUT→BRAM move costs a **read latency**, and the consumer's
   precedent (`crf_rx`) is that the shifted cycle must be pinned by a suite check
   before the change is accepted.
4. **Instrument rule:** Yosys `-flatten` reads ~2× high on LUT against Vivado
   (measured factor on the consumer's `KL_pcm_lpf`: 864 vs 428–445 placed; FF agree
   exactly). Area claims for new blocks come from Vivado out-of-context synthesis
   or they are labeled ESTIMATE.

---

## 3. What this architecture would replace — and what is already there

The block-by-block inventory against the consumer's tree produced one structural
result that drives everything else:

**Six of this architecture's eight "shared infrastructure" items already exist as
shared, exactly once, in the consumer.** One 1 s tick generator already fans to all
engines; ADP has *no* RX port there at all — ENTITY_DISCOVER detection lives inside
the shared AECP ingress (`hdl/ieee17221/aecp/KL_aecp_ingress.sv`), which is why the
consumer's standalone ADP parser is an orphan (testbench and matrix row, never
instantiated); the shared common parser already *is* the normalized transaction of
[03 §4](architecture/03_packet_engine.md) (target/controller EID, sequence_id, u, cdl
on beat 3); the single-writer state-RAM arbiter rule of
[07 §2](architecture/07_memory_maps.md) is already implemented and argued the same
way inside its descriptor store; one RX AXIS trunk already feeds every 1722.1
consumer as a non-intrusive tap; one TX trunk already merges every control and stream
source. Generalizing all of that recovers ~0.9 %.

**The consumer's entire shared front end — ingress, validator, parser, descriptor
store, dynamic-field mux, lock, timers, persistence, patch — costs ≈ 417 ± 208 LUT
combined** (derived by subtraction between two instrument-mismatched measurements;
corroborated to 2.2 % by an independent decomposition — *internal measurements*; the
±208 is why it is not load-bearing anywhere below). There is no saving available in
sharing infrastructure, because the infrastructure already costs almost nothing.

**The mass is elsewhere, and this architecture's sharing list does not name it:**
the consumer's AECP response builder + descriptor accessor are a combined **5,785
LUT** *(internal measurement; the tracked block table reads 5,300 + 1,290, which are
**not** additive — the 1,290 is a hierarchy-attribution artifact of
`-flatten_hierarchy rebuilt`)*. Its own area records characterise what remains in
that block after one RAM diet as "emit-engine source selection and the batch rlen
tables" — which is precisely a response-size ROM plus a wide descriptor field mux,
i.e. exactly what [06 §8](architecture/06_aecp_engine.md)'s dispatch ROM,
response-size ROM and µcode already specify. Pointed *at* that block (scenario C)
they attack the right structure; layered *above* it (scenario A) they only add.

The one genuine residual duplication this architecture would remove: ACMP is the one
protocol not sharing the parser there today — both ACMP context blocks carry a
private raw-beat ACMPDU capture off the RX trunk. That credit books in P3.

Two structural agreements worth recording, because they were reached independently:
the consumer's ACMP is already "one shared state machine + one context RAM serving
all listener sinks" — the same compute-model decision
[05 §4](architecture/05_acmp_engine.md) records (its replication escape hatch
included), reached before this document set existed. And its stateless talker
responder matches [05 §5](architecture/05_acmp_engine.md) exactly, including both
documented reference-implementation traps.

---

## 4. Scenario A — adoption as written: ≈ +3,000 LUT, do not

Bolting this architecture's shared infrastructure onto the consumer while keeping its
engines pays for the scoreboard, event router, trace ring, NVM port, side-port,
dispatch queues and profile ROMs — all new — and recovers ~0.9 % from sharing.
Net ≈ **+3,000 LUT** (settled by the inventory above, not re-derived): 56,156 LUT
= 88.57 %, density 3.54 ± 0.015 → the 10 %-closure bin, **2,411 LUT past the
conformance cliff** on a die with 589 LUT of headroom. Roughly 140–350 person-days
to build an unshippable bitstream. There is no version of this worth doing.

---

## 5. Scenario B — full replacement, phased (P0–P5)

Every phase below assumes the integration strategy of §10: the new plane goes in
behind a **default-OFF elaboration-time parameter** (consumer's default build
byte-identical, provably), because the phases are **not independently shippable** —
every removal P1 justifies lives inside blocks only P3/P4 delete.

| Phase | Scope | Net LUT (EST) | BRAM kbit | Effort (pd) |
|---|---|---|---|---|
| **P0** — fix this spec + its gates | §9 in full; no RTL | 0 (exact) | 0 | 5–9 |
| **P0b** — stand up hdl/tb scaffolding | suites, lint ratchet, matrix gate, portability tops, CI (§8) | 0 | 0 | 10–20 |
| **P1** — shared packet engine | [03](architecture/03_packet_engine.md) + side-port + NVM port + timer service: ~18 modules; the genuinely-new mass is scoreboard, event router, trace ring, NVM port, dispatch queues | **+1,100…+3,100**, pt +2,000 | +85…+110 † | 80–230 |
| **P2** — ADP engine | [04](architecture/04_adp_engine.md); consumer's ADP is nearly all wire-format (whole plane ≈ 200 LUT) | **+80…+480**, pt ≈ +280 | ~0 | 19–45 |
| **P3** — ACMP engine | [05](architecture/05_acmp_engine.md); first net saving: deletes both consumer ACMP context blocks (−3,420 measured, their LUTRAM *included*), adds ROM-driven executor (+1,600…+2,200; the consumer measured +1,738 when this SM moved into fabric) + talker responder + BACKOFF arc (+800…+1,000) | **−1,020…−220**, pt ≈ −620 | +18…+36 | 46–107 |
| **P4** — AECP µCPU + toolchain | [06](architecture/06_aecp_engine.md) + [07](architecture/07_memory_maps.md) + the F09.1 single-source model, assembler, ROM builder, golden model. Removals −6,202 (5,785 mass + front-end residue share); additions +3,150…+7,000, dominated by the µCPU (§6) | **−3,050…+800**, pt ≈ −1,130 | +103…+136 used (+166…+203 allocated at tile granularity) | 122–306 |
| **P5** — consumer integration | submodule pin, parameter, side-port→CSR bridge, adapter shims, suites, silicon soak | +150…+400 (ON), **0 OFF** | — | 43–90 |

† P1's BRAM includes a trace ring **ASSUMED at 256 × 128 b** — this spec states no
trace-ring geometry (§9, missing width #4).

**Totals (parameter ON, all corrected):** net LUT **−2,742 … +4,558**, central
**≈ +800** — i.e. 53,964 LUT = 85.12 %, density 3.41 ± 0.015: the 0-of-7 bin at
nominal, reaching the 28 % bin only at the directive band's favourable edge, and
**~220 LUT past the conformance cliff either way**. The optimistic end (50,414 =
79.52 %, density 3.18) is genuinely good; the pessimistic end (57,714 = 91.03 %,
density 3.64) is above the highest density that has ever closed. The spread is
owned by two unmeasured numbers: how fat P1's new infrastructure lands, and the
µCPU's size (§6). Effort: **325–807 person-days**; at the mined 3–5 concurrent
lanes, 9–38 calendar weeks, **plus** the closure lottery (§8).

**Verdict: not worth it for area.** The honest central case spends 325+ person-days
to land *past* the cliff in a bin never observed to close. What survives of B is its
conformance content — which scenario D plus P0 deliver for a fraction of the cost.

Phase risks that must not be lost (each verified against the consumer's records):
the ACMP sink record at 384 b is 21 % wider than the shipping 317 b and **must land
in BRAM** — as distributed RAM it grows the board's second-largest LUTRAM array
(448 LUT ≈ 112 SLICEMs, measured) by a fifth; ACMP is the consumer's
highest-rework engine (churn 1.85×, 45 % of written lines later deleted — price
against churn); the plain-IEEE profile ROM column is pure cost with no Milan return
(a product decision, not an architecture default); and the ADP cadence /
`available_index` semantics differ from the consumer's controller-validated
behaviour — adjudicate against live controllers (la_avdecc, Hive) *before* landing,
not after (§9).

---

## 6. Scenario C — the retargeted ceiling: what *can* be saved

**The design-space answer.** Keep the consumer's ingress/parser/stores/lock/timers/
persistence (the ≈ 417-LUT front end), keep its ADP and ACMP engines, and point this
architecture's µCPU + dispatch ROM + response-size ROM **at the 5,785-LUT emit
engine** — the block its own records call "largely a wide field mux over descriptor
data … whatever part is a pure function of a descriptor index is a ROM".

- **Displaceable fraction (EST):** 3,800…5,000 of the 5,785. What survives in any
  compute model: registry state, the eleven notification pending-class vectors (a
  queue, not a mux), the SET write-back/commit path, lock, identify, AXIS beat
  machinery, and the command staging buffer (which moves to BRAM, it does not
  disappear).
- **Replacement engine (EST):** µCPU datapath +1,200…+2,500; ROM read paths
  +100…+250; registry + notification queue as record tables +400…+950. Bracket
  anchors, both measured on the consumer: a complete protocol SM with timers and
  PDU build costs 621 LUT (`KL_maap`) — the µCPU cannot plausibly be smaller; a
  table walker with wide parallel compares costs 2,938 LUT (`KL_lwsrp_walker`) —
  structurally the closest analog to a micro-sequencer, the µCPU should not exceed
  it. (PicoRV32-class ≈ 1,500 LUT is an *external public analog*, not a measurement,
  and not directly comparable — this µCPU needs a 16 × 64-bit operand file.)
- **The regfile warning:** the 16 × 64 operand file must land in LUTRAM/BRAM, never
  flops — as a flop array with a 16:1 × 64-bit read mux it alone consumes most of
  the µCPU budget. The consumer measured this exact failure mode at **+894 LUT**
  when a counter mirror landed as flops, because Vivado does not read-replicate a
  multi-column array into LUTRAM.
- **Net ceiling: −350 … −3,400 LUT, central ≈ −1,900** — 51,256 LUT = 80.85 %,
  density 3.23: below every bin in the closure table, cliff margin +2,489. **Every
  point in the band improves closure odds; none worsens them.** That is the
  qualitative difference from scenarios A/B. Absolute upper reference: deleting the
  plane to software entirely was priced at 6,206 LUT + 9 RAMB36 on the consumer —
  the ceiling of ceilings, foreclosed there by an explicit product decision that
  AECP stays in fabric.
- **BRAM:** +103…+136 kbit used ≈ +7–10 BRAM18 (µcode ROM 2048 × 32–48 b is the
  dominant and *assumed-width* term), minus ~1.5 tiles freed with the ingress FIFO:
  BRAM goes ~67 % → ~70 %. The static descriptor image and the L2 descriptor
  directory are **already in BRAM** on the consumer — do not book them, and do not
  re-propose moving the accessor's L1 tables (32 LUT6, measured; a previously
  refuted theory).
- **Effort: 131–321 person-days** (P0 + scaffolding + retargeted P4 + narrowed P5),
  ≈ 4–15 calendar weeks at 3–5 lanes, plus the closure lottery.

**The gate experiment — do this first.** The entire band above hangs on one
unmeasured number the spec never states: the µCPU's LUT cost (its µop width is
unstated, so nothing is derivable). **1–2 person-days** buys the answer: write the
µCPU skeleton (µPC, µcode-ROM read port, 16 × 64 regfile as LUTRAM/BRAM, 32-bit ALU,
64-bit move path, decode for the µISA, cursors, status) and synthesize it
**out of context in Vivado** at the consumer's ship shape; read
`report_utilization -hierarchical`. Yosys cannot be the instrument (§2.4). If the
skeleton lands ≤ ~1,800 LUT, scenario C is real; if it lands fat (> 2,500), C
collapses toward zero and the correct decision is scenario D plus nothing.

**A separate, measured, spec-independent packing win discovered on the way:** the
consumer's three 1722.1-plane LUTRAM arrays (ACMP context RAM 448 LUT ≈ 112
SLICEMs, two fetch-word buffers 88 LUT, AECP staging buffer 86 LUT — all in its
tracked SLICEM census) total 622 LUTRAM LUTs ≈ 156 SLICEMs for 8,101 bits. Moved to
BRAM — up to three BRAM18, one per owning module; a shared tile would need an
unbooked port arbiter — the precedent A/B on the shipping recipe freed 196 SLICEMs
and −1,051 slice-LUTs for +1.5 tiles. Distributed RAM pins whole SLICEMs and
cannot LUT-combine, so it costs packing far beyond its LUT count. Each move owes a
pinned read-latency suite check. **This needs no µCPU, no submodule, and none of
this architecture.**

---

## 7. Latency: micro-coding is safe — the risk is elsewhere

The µCPU's serialization is *not* a conformance risk, and this deserves to be stated
as strongly as the area numbers. Per [08 §4](architecture/08_timing.md): a typical
program of ~25 µops at 100 MHz is 250 ns at 1 cycle/µop, 2.5 µs at a deliberately
pessimistic 10 cycles/µop; the pathological 13-element GET_DYNAMIC_INFO batch is
32.5 µs. Against T-BUDGET-AECP-TYP ≤ 20 ms, -WC ≤ 100 ms and the 240 ms
shall-respond line, that is **615× to 8,000× margin** — the µCPU could be a
thousand times slower than the hardwired FSM and still meet every budget. The
consumer's records reach the same verdict from the opposite direction: its "100 ms
razor" places AECP outside the fabric-necessary band entirely.

One correction to the obvious next thought: ACMP's case for staying out of the µCPU
is **not** a tighter budget. Its hard limit (200 ms) is only 1.2× tighter than
AECP's 240 ms, and its 4× design margin is *looser* than AECP's 2.4× worst case.
The reasons are the ones [05 §4](architecture/05_acmp_engine.md) gives — the SM is
Milan-normative and closed, so microcode buys nothing — plus contention: ACMP on
the µCPU would consume exactly the serialization slack AECP's margin pays for.

Where latency **is** at risk, in order:

1. **The ingress blackout — the one that bites.** The consumer's 1 KiB drop-on-full
   ingress FIFO is an IEEE 1722.1 §9.3.2.6 violation *mechanism*, and its worst
   burst source is the entity itself: the response builder holds ingress `tready`
   low ~1,050 cycles ≈ 16 frames ≈ exactly the FIFO, during SET-command replay to
   registered controllers. Growing the registry 4→16 multiplies the blackout 5×,
   and a serialized µCPU makes the stall window *longer*. **Therefore the 4-slot RX
   slot RAM ([03 §2](architecture/03_packet_engine.md), one BRAM18) is a hard
   dependency of the µCPU, not an optional extra.** Any build that takes the µCPU
   without it must take the consumer's +8-LUT FIFO fix (1 KiB→4 KiB) first.
2. **Notification fan-out.** 16 controllers × eleven trigger classes: the
   [03 §8](architecture/03_packet_engine.md) pacing contract (≥ 1 TX slot per
   frame-time held for solicited traffic) is what protects the 8,000× margin above.
3. **The per-frame plane must not move.** Counter *accumulation* is per-frame and
   stays fabric; the µCPU serves and latches counters, it is never in the
   accumulation path.

---

## 8. The effort model and what it is based on

Mined from the consumer's git history (author dates; deduplicated; spans quoted over
the real delivery windows, stub-commit artifacts excluded). The direct analog is its
own 1722.1 control plane:

| Comparable | Modules | RTL LOC | TB LOC | Checks | Commits | Window | Active days |
|---|---|---|---|---|---|---|---|
| **1722.1 plane (AECP+ACMP+ADP)** | 19 + 3 pkg | 13,024 | 17,650 | 4,355 | 206 | 40 days | 57 (summed) |
| AECP chain | 12 + pkg | 9,081 | 11,490 | 2,773 | 147 | 31 days | 27 |
| ACMP SMs | 4 + pkg | 2,859 | 4,105 | 843 | 38 | 29 days | 18 |
| ADP | 3 + pkg | 1,084 | 2,055 | 739 | 21 | 39 days | 12 |
| lwSRP engine (cleanest greenfield) | 11 + pkg | 3,949 | 3,898 | 933 | 40 | 27 days | 16 |
| CSR plane (the seam) | 1 file | 2,693 | 2,329 | 851 | 126 | 41 days | 32 |
| Fuzz/cosim layer | — | — | 3,330 | 3,378 | 8 | 16 days | 5 |

Planning rates derived from the four RTL engines: **RTL 120–200 LOC/day, TB 143–236
LOC/day** (calendar/active bounds); TB:RTL ratio 1.0 (datapath) → 1.3 (command
engine) → 1.44 (state machines) → 1.9 (pure wire format); **rework multiplier 1.4
planning / 1.85 measured worst** (ACMP); **CSR tax ~1 CSR commit per protocol
commit**. Convention tax measured over 220 protocol-plane commits: 72 % also touch
testbenches, 54 % docs, 35 % generated configs; median 11 files per commit.

Four caveats that bound every figure above:

- **Unit.** Person-days are AI-agent-assisted lane-days; the consumer ran 3.5–5.8
  effective parallel lanes (its 1722.1 plane prices at 140–232 person-days and was
  delivered in 40 calendar days). Do not quote these as human-team velocities.
- **The scaffolding was already paid for.** Those velocities were measured in a repo
  with 62 Verilator suites, a ratcheted lint gate, a zero-tolerance untested-module
  matrix gate, 50 portability tops, 45 BDD features, an 8.4 k-LOC builder gate and
  a live bench. This repository has `hdl/README.md` and no RTL. P0b (10–20
  person-days) is the honest floor for standing that up, and the
  [consumption contract](../hdl/README.md) adds a second CI system on top.
- **The toolchain is the likely schedule miss.** The F09.1 single-source model +
  assembler + ROM builder + golden model is a *second builder*: the consumer's
  equivalent is 4,265 LOC with a test harness nearly as large (4,181 LOC). Priced
  at the mined general rates: 28–77 person-days. Everything downstream — ROMs,
  golden model, vectors, doc tables — generates from it; if it slips, nothing else
  can be verified.
- **Timing closure is not a person-day cost and is never folded in.** The
  consumer's campaign closes 34 % of placed builds; its ship build closed at
  WNS +0.002 ns with three free slices. Delivering RTL is not shipping a bitstream.

---

## 9. Defects in this document set that this analysis must feed back (P0)

Found while grounding the analysis; each verified against the cited file. These are
P0 because the architecture is already being read as a specification, and because
its own gates cannot currently catch any of them.

1. **NC-1 (blocker).** [06 §6.7](architecture/06_aecp_engine.md)'s GET_DYNAMIC_INFO
   allowed-set names 7 opcodes; IEEE 1722.1-2021 §7.4.76.2 enumerates **13**, and
   the gate is *list membership*, not implementation (§7.4.76.1 requires
   NOT_SUPPORTED sub-statuses for unimplemented members, never BAD_ARGUMENTS for
   the batch). The consumer's `batch_legal()` already does all 13 — port from it,
   not from this spec.
2. **NC-2 (blocker).** [03 §6](architecture/03_packet_engine.md) rule (e): the
   deadline kill retires a command with **no response** — exactly what IEEE 1722.1
   §9.3.2.6 forbids — *after* this architecture (correctly) removed IN_PROGRESS,
   the only legal extension. This is a redesign, not an edit: the µISA needs a
   forced-respond disposition (a fail-safe SET_STATUS + BUILD_HEADER +
   SEND_RESPONSE arm off the deadline event), and 03 §6 needs "a deadline expiry
   emits, never drops".
3. **NC-3.** IDENTIFY_NOTIFICATION received as a command must yield
   `NOT_IMPLEMENTED` (IEEE 1722.1 §9.3.5.3.3), not `BAD_ARGUMENTS`.
4. **NC-4.** Milan §5.4.2.26/.27/.28 — `NOT_SUPPORTED` on dynamic-mapping
   commands addressed to a mapped Stream Port — is absent from
   [06 §6.5](architecture/06_aecp_engine.md).
5. **F09.4 needs corrections, not deletions.** The two
   [09 §3](architecture/09_verification.md) cases that encode NC-1 and NC-3 as
   *expected* results must be rewritten (all 13 §7.4.76.2 opcodes accepted inside a
   batch; IDENTIFY_NOTIFICATION → NOT_IMPLEMENTED with a correctly-sized echo) and
   a positive GDI-acceptance case added. Net TOL case count goes up.
6. **The gates must be able to catch 1–5.** `scripts/check-matrix.py` never reads
   the Mand or Cov columns, and 4 of 17 GAPs carry zero REQ rows, so the release
   gate never reaches them. Add both checks; add REQ rows for the empty GAPs.
   Install `@mermaid-js/mermaid-cli` so `make lint` stops passing vacuously when
   `mmdc` is absent.
7. **Four missing widths** (the only sizing statements the architecture's central
   bet needs, none stated): the **µop width** (P-UCODE-ROM-DEPTH = 2048 × *unstated*
   is the single most consequential missing number in the set), the dispatch-ROM
   entry width, the ACMP transition-ROM entry width, and the trace-ring record
   width × depth ([02 §7](architecture/02_interfaces.md) gives only an address
   window).
8. **µISA count consistency.** The [06 §8](architecture/06_aecp_engine.md) table
   lists 29 operations across its 8 groups; the prose says "~24". Decode width is a
   first-order µCPU term — state one number.
9. **F05.3 is 8 states × 14 events = 112 cells** (not 13 events, an easy miscount —
   the MTXW coverage target and the transition-ROM sizing, 112 × ~32 b ≈ 3,584 b
   per profile column, both depend on it).
10. **Interop adjudications to record before P2 ever lands.** The consumer's
    controller-validated behaviour diverges from [04](architecture/04_adp_engine.md)
    on advertise cadence (it uses the faster of Milan Table 5.50 and IEEE Fig 6-2:
    MIN(5, MAX(1, valid_time/2))) and on `available_index` (increments on every
    ADPDU, never resets — justified there by observed la_avdecc/Hive behaviour).
    Whichever way each is adjudicated, prove it against both controllers first.
    Also: [03 §3](architecture/03_packet_engine.md) V3's acceptance of the 96-B
    IEEE ACMPDU form is *broader* than the consumer's (which rightly rejects
    short/nonstandard forms) — widening acceptance is a wire-behaviour change
    needing its own TOL cases.
11. **Where this spec is right and the consumer is wrong, keep the spec.** Two
    found by this analysis: the consumer's ACMP anti-storm PRNG is a 16-bit LFSR
    with a fixed seed (period 65,535 — every device on a segment draws the
    identical sequence; draw range 1–1024 ms vs the intended 0–1000 ms), failing
    this architecture's ≥ 2³¹−1 sequence-length requirement — the requirement
    stands. And the consumer ships a 4-entry controller registry against Milan
    §5.3.4.2's ≥ 16 (with a dead 16-wide constant seven lines above it);
    [01 §7](architecture/01_overview.md) P-N-CONTROLLERS = 16 is correct.
    Conversely, **do not port** into this spec's implementation: its single-region
    dynamic overlay (the consumer's split — live-HW fields overlaid at read, AECP
    settables stored — is cheaper: no second RAM, no merge in the read path), and
    its per-record crc16 (weaker than the consumer's whole-image CRC-32 atomic
    reject, whose torn-slot rejection is structural).

---

## 10. Recommendation

**Do now, in this order:**

1. **Scenario D in the consumer** (12–25 person-days, −1,030 LUT, every live
   conformance fix, better closure odds). It saves more than scenario B's central
   estimate and needs none of this architecture.
2. **The 1–2-day out-of-context µCPU skeleton synthesis** (§6). It converts
   scenario C from a bet into a decision, and it gates 131–321 person-days.
3. **P0** (§9) — regardless of whether any RTL is ever written here. This
   architecture is more correct than the consumer's gateware in over a dozen
   adjudicated places; that correctness, not area, is its present value, and it
   currently ships with defects its own gates cannot see.
4. If (2) lands lean: **scenario C**, built in this repository per
   [`../hdl/README.md`](../hdl/README.md), consumed behind a **default-OFF**
   elaboration parameter, integrated all-at-once — never P1 or P1+P2 alone, which
   land in the 0-of-7 density bin.

**Do not:** adopt the shared-infrastructure layer as written (scenario A); land any
phase incrementally on the shipping build; port this spec's GDI list, its
IDENTIFY_NOTIFICATION status, its overlay structure, or its per-record CRC into an
implementation; or quote a slice delta for any block-scale change, in any scenario.
