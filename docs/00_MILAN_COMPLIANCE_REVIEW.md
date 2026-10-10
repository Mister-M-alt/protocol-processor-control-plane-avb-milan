<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Milan v1.2 Compliance Review — `IEEE_1722_1_Hardware_Protocol_Processor.md`

Review of the original concept document against the requirements of a **non-redundant
Milan v1.2 PAAD** built on IEEE 1722.1-2021. Companion architecture: [`architecture/`](architecture/01_overview.md).

## 1. Executive summary

The reviewed document proposes a sound **execution architecture** — shared packet
infrastructure, protocol-graded compute (tiny ADP FSM / medium ACMP engine / microcoded
AECP), hazard scoreboard, deadline engine, control-plane-only boundary. Those ideas
survive into the new architecture. It is, however, **not yet a Milan architecture**: it
models ACMP with IEEE semantics that Milan replaces, models ADP from a controller's
perspective, has no path for the ~40 % of PAAD behavior that the entity must *originate*
(probes, liveness checks, notifications), never defines the external-engine interfaces
that most mandatory commands depend on, and contains no requirement inventory to size
anything against.

Top findings: [GAP-02](#gap-02) (ACMP model), [GAP-16](#gap-16) (ADP model),
[GAP-17](#gap-17) (no self-originated traffic), [GAP-04](#gap-04) (external "Hardware
API" undefined), [GAP-01](#gap-01) (no clause-mapped requirement inventory).
Every finding is dispositioned into the new architecture — see [§7](#7-disposition-of-findings-f002).

## 2. Scope and compliance target

Target: **non-redundant PAAD**, Milan Specification Consolidated **v1.2** (Final,
2023-11-30). Milan chapter 8 (seamless redundancy) is wholly excluded; the architecture
keeps its structural seams parameterized ([01 §7](architecture/01_overview.md)).

Pinned base revisions (Milan §2):

| Standard | Revision |
|---|---|
| ATDECC | IEEE Std 1722.1-**2021** |
| AVTP | IEEE Std 1722-**2016** |
| gPTP | IEEE Std 802.1AS-**2011** + Cor1-2013 + Cor2-2015 (not -2020) |
| Bridging/SRP | IEEE Std 802.1Q-**2014** |
| AVB profile | IEEE Std 802.1BA-2011 |

Precedence rule: **where Milan differs from IEEE 1722.1 ACMP (or elsewhere), Milan takes
precedence** (Milan §5.5.2.1). The master delta list is
[F01.4](architecture/01_overview.md#fig-01-deltas).

Milan v1.2 contains **no PICS / conformance annex**; requirements are inline
shall/should/may prose. The compliance matrix in [§6](#6-compliance-matrix-f001) is therefore
built from clause extraction, not from a published proforma.

## 3. Methodology

- Sources: the reviewed document; Milan v1.2 consolidated PDF (printed page = PDF − 7);
  IEEE 1722.1-2021 PDF. Every requirement row carries its clause.
- Procedure: extract requirements → map onto the reviewed document (Covered / Partial /
  Absent / Incorrect) → derive findings → disposition each finding into the new
  architecture → attach a verification category.
- Severity scale: **Blocker** (architecture cannot express the required behavior) ·
  **Major** (required subsystem/behavior missing, structure permits adding) ·
  **Minor** (scoping/policy gap) · **Info** (documentation/convention risk).

> ⚠ Bit tables in Milan and IEEE 1722.1 are **MSB-first** (bit 31 ⇔ mask `0x00000001`).
> The **hex mask column is authoritative**; never derive shifts from bit-number columns.

## 4. What the document got right

| Strength (original §) | Retained as |
|---|---|
| Shared packet engine + normalized transaction (§2, §4, §5) | [03 §2–§5](architecture/03_packet_engine.md), record extended with `origin` |
| Compute gradation: ADP tiny / ACMP medium / AECP µcoded (§1, §3, §23) | [04](architecture/04_adp_engine.md)/[05](architecture/05_acmp_engine.md)/[06](architecture/06_aecp_engine.md); ACMP refined to table-driven record executor |
| 4-stage pipeline, variable-latency EXECUTE (§5) | [03 §4](architecture/03_packet_engine.md) with three amendments |
| Command scoreboard / "deterministic safe parallelism" (§13) | [03 §6](architecture/03_packet_engine.md), classes re-grounded ([F03.7](architecture/03_packet_engine.md#fig-03-hazards)) |
| Deadline engine, deadline from command reception (§20) | [08](architecture/08_timing.md) timer service + PRNG |
| TX arbiter merging all sources (§16) | [03 §8](architecture/03_packet_engine.md) |
| Control-plane-only boundary (§21) | [01 §2](architecture/01_overview.md); the "Hardware API" is now four concrete adapters ([02 §4](architecture/02_interfaces.md)) |
| 16-controller table (§15) | Controller registry + separate lock manager ([06 §7](architecture/06_aecp_engine.md)) |
| Configurable response buffer (§19) | TX slot classes incl. full-frame oversize slot ([03 §7](architecture/03_packet_engine.md)) |
| µcode extensibility; single-source command model & toolchain vision (§9, §26) | [06 §8](architecture/06_aecp_engine.md), [09 §1](architecture/09_verification.md) |
| Implementation options analysis A–D (§22) | Rationale retained in [01 §3](architecture/01_overview.md) |

## 5. Gap findings

#### <a id="gap-01"></a>GAP-01 [Blocker] — No clause-mapped requirement inventory
The document names protocols, not requirements: no mandatory AEM/MVU command list, no
descriptor set, no per-command Milan behavior (direction rules, status codes, response
sizes). Nothing can be sized (µcode ROM, RAMs, timers) or verified against it.
Evidence: Milan mandates ~24 AEM commands with per-command deviations (Milan §5.4.2.1–.29),
Milan-extended GET_STREAM_INFO 80-byte response (§5.4.2.10, Fig 5.1), direction
prohibitions (SET_STREAM_INFO input → `NOT_SUPPORTED`, START/STOP_STREAMING output →
`NOT_SUPPORTED`; §5.4.2.9/.19/.20), SET_STREAM_INFO all-or-nothing sub-flag rule
(§5.4.2.9), GET_DYNAMIC_INFO execution rules (IEEE §7.4.76), correctly-sized
`NOT_IMPLEMENTED` responses for **all** opcodes (IEEE §9.3.5.3.3).
**Disposition**: command master table [F06.14](architecture/06_aecp_engine.md#fig-06-cmdtable);
matrix §6 below is the inventory.

#### <a id="gap-02"></a>GAP-02 [Blocker] — ACMP modeled with IEEE semantics Milan replaces
Original §7 keeps CONNECT/DISCONNECT_TX as talker state operations with talker-side
connection records. Milan (ch. 5.5): the talker is **stateless** (PROBE_TX is a pure
query; DISCONNECT_TX validates the source and returns TALKER_UNKNOWN_ID if invalid,
otherwise SUCCESS, with no state change in either case (§5.5.4.2 step 1,
Tables 5.44/5.45 govern the §5.5.2.7 overview); GET_TX_CONNECTION →
`NOT_SUPPORTED`); all connection intelligence is the **listener's 8-state
binding/probing state machine** per Stream Input, driven by ADP discovery and SRP
registration events; messages are renamed (BIND_RX/UNBIND_RX/PROBE_TX); the ACMPDU is
truncated to 56 bytes; **all five command timeouts are 200 ms** (Table 5.26); binding
persists across power cycles (§5.3.8.2). "Milan as additive profile" (original §17)
understates this: Milan *replaces* ACMP semantics (§5.5.2.1).
**Disposition**: [05](architecture/05_acmp_engine.md) — stateless talker rules + listener
SM package (F05.2–F05.6).

#### <a id="gap-03"></a>GAP-03 [Major] — Milan Vendor Unique protocol absent
"Milan MVU" appears only as a label. Missing: MVU framing (protocol_id
`00-1B-C5-0A-C1-00`, 15-bit command_type, padding excluded from cdl; §5.4.3.2),
mandatory **GET_MILAN_INFO** (protocol_version = 1, features flags, certification_version;
§5.4.4.1), recommended SET/GET_SYSTEM_UNIQUE_ID (§5.4.4.2/.3) and
SET/GET_MEDIA_CLOCK_REFERENCE_INFO (§5.4.4.4/.5), MVU timing 250 ms / respond ≤ 240 ms
(§5.4.3.4).
**Current disposition (October release)**: the MVU sub-decoder and one command
group, GET_MILAN_INFO, are implemented. The two recommended pairs are deliberately
not implemented under the
[2026-09-23 owner decision](https://github.com/kebag-logic/milan-fpga/issues/510#issuecomment-5789766089).
Milan v1.2 §5.4.4.2–§5.4.4.5 (printed pp. 58–61) and §7.6 (printed p. 115)
each state: “Note: Support for this feature is a recommendation for Milan compliant PAADs.
This recommendation will become a requirement in a future revision of this specification.”
(Spelling normalized.)
Implementation is deferred to
[P4](https://github.com/kebag-logic/milan-fpga/issues/416) if the conformance lab
requires it or a targeted Milan revision makes it mandatory.
[06 §6.9](architecture/06_aecp_engine.md#69-mvu-commands) records
the waiver and command-length `NOT_IMPLEMENTED` responses for 0x0001–0x0004;
[`tb/pp_top`](../tb/pp_top/README.md) M4 grades all four byte-exact.
[F01.5](architecture/01_overview.md#fig-01-params) marks the phantom enable
parameters reserved with no RTL consumer. M1/M2 grade GET_MILAN_INFO's zero
features_flags; Table 5.20 has no SUID or MCR support bits. The separate timing
requirement remains open in
[#57](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/57).

#### <a id="gap-04"></a>GAP-04 [Blocker] — External-engine "Hardware API" never defined
Original §21 draws arrows to AVTP/gPTP/TSN engines but defines no interface. Mandatory
behavior depends on those interfaces: GET_AVB_INFO / GET_AS_PATH (gPTP data; §5.4.2.23/.24),
GET_STREAM_INFO (SRP declaration/registration state, failure code+bridge, accumulated
latency; §5.4.2.10), GET_COUNTERS (link/gPTP/media-clock/stream events; §5.4.2.25),
talker DA-validity gate (MAAP + 15 s probe freshness **or** listener registered;
§4.3.3.1), MAAP-conflict recovery (withdraw → 2×LeaveAll → new DA; Table 5.3),
MSRP domain adopt/re-declare (§4.2.7.2.1), unsolicited notifications on all of the above
(Table 5.22).
**Disposition**: interface classes in [02](architecture/02_interfaces.md); the `srp` and
`maap` class-B faces; gPTP, AVTP and media clocking served by class-D levels and the
`gsi_*` and `ctr_*` read faces, with the landed shape of each in 02 §1 and §4.3 to §4.6
(processor issue #78: correcting the text, since Milan requires the wire behaviour these
faces serve, not an adapter API); status dictionary
[F02.10](architecture/02_interfaces.md#fig-02-statusdict), each row naming its landed port
or word; the `srp` contract is served in scope by the SRP engine
([10](architecture/10_srp_engine.md), §6.9 REQ-SRP rows, §8 item 9).

#### <a id="gap-05"></a>GAP-05 [Major] — Counters/diagnostics subsystem absent
GET_COUNTERS is mandatory for every AVB_INTERFACE, CLOCK_DOMAIN, STREAM_INPUT and
STREAM_OUTPUT of the current configuration (§5.4.2.25), with defined 32-bit wrapping
counters, invariant pairs (LINK_UP/DOWN, LOCKED/UNLOCKED, START/STOP), ≤ 1 s observation
intervals, and reset rules (input bank cleared on not-bound→bound; three output counters
cleared on stream start). Milan's STREAM_OUTPUT mask layout **differs from IEEE**
(Milan Table 5.17: MEDIA_RESET `0x4`, TIMESTAMP_UNCERTAIN `0x8`, FRAMES_TX `0x10`) —
Milan takes precedence (Δ-tagged).
**Disposition** (owner decision 2026-09-19, processor issues #44 and #79): the
**integrator** keeps every counter, behind the `ctr_*` read face; the processor owns the
GET_COUNTERS command and its Table 5.22 push
([06 §6.6](architecture/06_aecp_engine.md#sec-06-counters),
[02 §4.6](architecture/02_interfaces.md#sec-02-ctr)) and keeps no bank
([07 F07.10](architecture/07_memory_maps.md#fig-07-ctrmap)). The face contract per
descriptor type (masks, quadlets, what counts, wrap, reset rules, the change strobe) is
the [integrator guide §7.1](guides/integrator.md#counters-face).

#### <a id="gap-06"></a>GAP-06 [Major] — Notification/registry/lock machinery underspecified
Original §15/§16 sketch a table and a TX mux. Required: registry tuples {controller EID,
MAC, **port**, per-controller next sequence_id} with no-duplicate rule, ≥ 16 per AVB
interface, cleared on power cycle (§5.3.4.2); fan-out to all registered controllers
**excluding the requester**, per-entry DA/EID/seq on the recorded port (§5.4.5.1);
trigger set = every successful state-changing command + non-ATDECC changes while
unlocked + async triggers of Table 5.22 with **≤ 1 notification per descriptor per
second** for counters; departing-controller detection (random 30–60 s monitor,
CONTROLLER_AVAILABLE probe + one retry, removal + targeted DEREGISTER; §5.4.5.3);
optional registration-overflow probing before mandatory `NO_RESOURCES` (§5.4.2.21); TIME_LIMITED
registrations with 300 s expiry (IEEE §7.4.37.2). The original conflates lock ownership
with this table: the **lock manager** is a separate object (ENTITY scope only, UNLOCK
flag, 60 s auto-unlock → notification; §5.4.2.2). Identify machinery (CONTROL 0/255,
multicast notification 3× @150 ms; §5.3.12, §5.4.5.4, IEEE §7.5.1) is absent.
**Disposition**: [06 §7](architecture/06_aecp_engine.md); records in [07 §4](architecture/07_memory_maps.md).

**Landed 2026-08-18**: registry (16 rows, duplicates refreshed, NO_RESOURCES on
overflow), TIME_LIMITED 300 s expiry with targeted u=1 DEREGISTER, the per-entry
sequence emission walk, successful state-changing command triggers with requester
exclusion and no-op suppression, the per-descriptor GET_COUNTERS limiter, and the
§5.4.5.3 random monitor with CONTROLLER_AVAILABLE, one retry, any-status rearm, and
targeted removal. The optional overflow eviction sweep is not attempted.

**Landed 2026-09-30 (lane C6, issues #54, #58, #80)**: the identify machinery is
complete in both directions. SET/GET_CONTROL on the IDENTIFY control (0/255, volatile)
was already landed (`tb/pp_top` W12/W13). IDENTIFY_NOTIFICATION origination is landed
behind `P-EN-IDENTIFY-NOTIFICATION` (default 0, zero area): `identify_button_i`,
2FF-synchronised in the core and debounced by the integrator, starts three u = 1
frames to 91-E0-F0-01-00-01 at `T-IDENT-BURST` through the shared timer service,
identifySequenceID per burst, and a `T-IDENT-REARM` re-arm while held (`tb/pp_top`
section ID, third build; section ID0 at the default). Every notifying command class
has a byte-exact wire push at a second controller (section NP), which found and fixed
the unsolicited SET_STREAM_INFO body (IEEE §7.4.15.1, Figure 7-40). The RND and STORM
suites this finding names exist (sections RN and ST). Non-ATDECC (MGMT-origin)
changes are not supported by this build ([06 §7](architecture/06_aecp_engine.md) trigger
class 2). **Identify work remaining:** none in this repository; a product adopts it with
a debounced button and the parameter at 1.

#### <a id="gap-07"></a>GAP-07 [Major] — Timing model incomplete
Original §20 has one AECP number (250 ms, with a 187.5 ms internal target). The real
constant set is ~22 entries: ADP valid_time 10 → advertise every 5 s, delay 0–4 s
(0–2 s at startup — two distinct constants), ACMP 200 ms ×5 with exact-duplicate retry
then 4 s backoff, listener delay 0–1 s, TMR_NO_TK 10 s, AEM/MVU 250/240 ms, monitor
30–60 s, lock 60 s, TIME_LIMITED 300 s, identify 3×150 ms, observation ≤ 1 s, DA
freshness 15 s, MAAP backoff 2×LeaveAll. Randomized draws require an IEEE-seeded PRNG.
The 187.5 ms figure is not spec-derived; budgets replace it.
**Disposition**: master table [F08.1](architecture/08_timing.md#fig-08-constants), timer
service + PRNG [08 §3](architecture/08_timing.md), budgets [08 §4](architecture/08_timing.md).

#### <a id="gap-08"></a>GAP-08 [Major] -- Entity-model memory architecture incomplete
"Descriptor RAM" is named but not designed. Needed: static descriptor image + dynamic
overlay split; **IEEE 1722.1-2021 Table 7-8** STREAM descriptor layout (formats_offset =
138, N ≤ 46 formats (the §7.2 508-octet descriptor maximum), redundancy tail emitted with R = 0 even when non-redundant, per
Milan §5.3.3.4 which binds the descriptor to [ATDECC, Clause 7.2.6] and leaves Annex C
Table C.1 a **may**); name table for all
named descriptors; per-configuration index maps; READ_DESCRIPTOR assembly
incl. the 4-byte failure stub (IEEE §7.4.5). Response buffering must anticipate Milan
**oversize responses** (no cdl cap for six commands; §5.4.1) — a full-Ethernet-frame TX
slot, not `MAX_AECP_RESPONSE_SIZE` guesswork.
The live audio-map transaction is implemented: the engine stages a full page,
the root validates every row, and commit is all-or-nothing. Persisting and
restoring mappings is the integrator's (GAP-09; 07 §5.1): it saves a port's set
from the accepted phase-5 commit beat and restores it.
The static descriptor image, per-configuration index map, writable name table,
and coherent READ_DESCRIPTOR name patching are implemented. A name update pulses
the accepted name-lane write (`aecp_name_wr_o`), which the saved-state contract
names as the name group's persistence trigger; the group-7 mark is a completion
notification, never a persistence trigger. The D3 writer saves a changed name and
replays it after power loss, once the image walk has run (GAP-09).
After a SET, READ_DESCRIPTOR serves the configuration, sampling rate, clock source and
stream format the SET stored, which the GET reads, not the image's defaults (issue #82). No stream descriptor
is assembled here: the consumer's image carries each one whole in the Table 7-8 layout,
and the model rules are the consumer's (07 §3.1); the packer's model lint (L1–L12) checks
them by default as defence in depth (07 §3.1, 09 §8.5). The oversize path is graded
end to end up to the response buffer's cdl 592.
**Disposition**: [07 §3](architecture/07_memory_maps.md), TX slots [03 §7](architecture/03_packet_engine.md).

#### <a id="gap-09"></a>GAP-09 [Major] — Persistence requirements absent
Milan mandates non-volatile storage of eleven state groups: sampling rate (§5.3.5.1),
stream formats in/out (§5.3.7.1/§5.3.8.1), presentation-time offset (§5.3.7.6), bound
state + binding parameters (§5.3.8.2/.3), started/stopped (§5.3.8.7), input and output
mappings (§5.3.10.1/§5.3.9.1), current clock source (§5.3.11.1), all user names
(§5.3.13). Explicitly volatile: lock state, controller registry, identify (reset to 0).
Boot must restore **before** entity enable, and a restored binding starts the listener
SM in `PRB_W_AVAIL` (§5.5.3.5.2). Persistence of the current configuration index is
*not* stated by Milan — recorded as a design decision (§8 item 1) and retained.
**Disposition**: two record producers + records + flows [07 §5](architecture/07_memory_maps.md),
under the parent saved-state (D3) contract: its live-write triggers, replay and
volatile exclusions. **Status by stage**, each with executable evidence at the head
that lands it: bound state, binding parameters and started/stopped by the binding
manager (`tb/acmp_nvm`, `tb/pp_top` BW); configuration index, sampling rate, clock
source, both stream formats and presentation offset by the D3 writer, restored in two
agreeing passes before AECP and ADP are released (processor issue #131; `tb/pp_top`
D3); every user name by the D3 writer, written back after the image walk (issues #61
and #83; `tb/pp_top` D3N, and every D3 record type cut mid-commit by `rst_n` in D3K).
Channel mappings are **the integrator's** by the manager's ruling on issue #83
(2026-10-03), which amends the D3 contract's map stage: the map plane is the
integrator's, so it writes records `0x60`/`0x70` from the phase-5 commit beat,
restores them against the restored formats and resets them on a D3 roll-back
([07 §5.1](architecture/07_memory_maps.md#51-persisted-vs-volatile-normative-set-req-per-001002)).
The processor keeps the ATDECC side of the maps
(GET_AUDIO_MAP, ADD/REMOVE_AUDIO_MAPPINGS), so they stay ATDECC-authoritative. The
seeded-random cut campaign (`tb/pp_top` D3KR) cuts every record type both producers
write, the binding included, at 32 standing seeds each. No processor-only evidence
closes the physical saved-state acceptance, which is the integrating platform's.

#### <a id="gap-10"></a>GAP-10 [Major] — Reusability substance missing
"Reusable FPGA IP" is claimed without the artifacts that make IP reusable: no clock/
reset/CDC strategy, no bus-agnostic interface contracts (signal tables + handshakes), no
parameter table, no profile mechanism, ungrounded scoreboard classes.
**Disposition**: [02 §2](architecture/02_interfaces.md) (CDC), interface classes A–F
(02), parameters [F01.5](architecture/01_overview.md#fig-01-params), profile-ROM strategy
[01 §7](architecture/01_overview.md), hazard classes [03 §6](architecture/03_packet_engine.md).

#### <a id="gap-11"></a>GAP-11 [Minor] — No verification/compliance strategy
One sentence on co-simulation (§26). Needed: requirement↔test traceability, listener-SM
matrix walking, malformed/tolerance suite, timing verification with compressed timers,
notification-storm and NVM power-cut tests, doc-sync regression.
**Disposition**: [09](architecture/09_verification.md); Verification column in §6.

**Landed 2026-10-10 (issue #72)**: requirement-to-test traceability. Each check that
verifies a row carries the row's tag in its suite's source, and `scripts/check-req-tags.py`
fails a row whose Ver category has neither a tagged check of that category nor a waiver
naming its GAP; `make check` runs its static half and the CI suites job its executed half
(09 §8.10). The Cov column of §6 records each row's state.

#### <a id="gap-12"></a>GAP-12 [Minor] — Non-redundant scoping not explicit
The document never states the redundancy position. For a non-redundant PAAD: Milan ch. 8
excluded; GET_MILAN_INFO `REDUNDANCY` flag = 0; STREAM descriptors emitted with R = 0 and
Annex C Table C.1 declined (§5.3.3.4 makes it a **may** absent a redundant pair);
per-interface keying kept parameterized as the redundancy seam; gPTP-as-media-clock
(§7.5) is *only* legal non-redundant single-AVB_INTERFACE.
**Disposition**: [01 §1/§7](architecture/01_overview.md).

#### <a id="gap-13"></a>GAP-13 [Minor] — Firmware/REBOOT scope ambiguity
Milan v1.2 mandates **no** firmware-update mechanism, no REBOOT, no MEMORY_OBJECT
operations, no AEM checksum (verified by full-text search). These are IEEE-optional
(Annex D informative). Leaving them implied bloats the control plane.
**Disposition**: optional side-port assist behind `P-EN-FIRMWARE-ASSIST`
([02 §7](architecture/02_interfaces.md)); EFU_MODE capability = 0.

#### <a id="gap-14"></a>GAP-14 [Info] — All diagrams ASCII
Violates the project requirement that every figure be an editable artifact.
**Disposition**: Mermaid/WaveDrom/draw.io per [docs/README.md](README.md) §3; regeneration
+ lint via `Makefile`.

#### <a id="gap-15"></a>GAP-15 [Info] — Spec-literalism traps unaddressed
Traps that silently break interop if unknown: MSB-first bit tables (masks authoritative);
PDU length = cdl + 12 (IEEE figures printing +8 are an erratum) and padding excluded
from cdl → parser trusts cdl; mandatory short-PDU tolerance (2013 ACMPDU, 2013
ENTITY_AVAILABLE response, REGISTER_UNSOLICITED without flags); ACQUIRE_ENTITY must be
implemented but **never succeed** (→ `NOT_SUPPORTED`, §5.4.2.1) so no acquire/contention
machinery; IN_PROGRESS is incompatible with GET_DYNAMIC_INFO (IEEE §7.4.76.2) → policy:
**never emit IN_PROGRESS**, always respond ≤ 240 ms; AECP unicast vs ACMP/ADP multicast
`91-E0-F0-01-00-00` and identify multicast `91-E0-F0-01-00-01`.
**Disposition**: conventions ([docs/README.md](README.md) §4), parser rules
[F03.6](architecture/03_packet_engine.md#fig-03-valrules), policy notes in 06.

#### <a id="gap-16"></a>GAP-16 [Blocker] — ADP engine modeled from the controller side
Original §6 designs an "entity table + timer/aging" — that is what a *controller* keeps.
A PAAD needs: an **advertise SM per AVB interface** (DOWN/WAITING/DELAY; 5 s advertise;
0–4 s random delay, 0–2 s at startup; re-advertise on **GM change**; ENTITY_DEPARTING
only on shutdown, never on link-down; §5.6.3), ADP gating (start only when ready to
accept AECP + bind/probe; §5.6.1), `available_index` TX semantics (increment after each
ENTITY_AVAILABLE; reset on departing/power-up; IEEE §6.2.2.15), and a **per-bound-sink
talker-discovery SM** (GM-id + domain match, `available_index` restart detection,
TMR_NO_ADP aging; §5.6.4) feeding the ACMP listener SM.
**Disposition**: [04](architecture/04_adp_engine.md) (F04.2/F04.3).

#### <a id="gap-17"></a>GAP-17 [Blocker] — Purely reactive pipeline
The RX→process→TX pipeline has no path for traffic the entity must **originate**:
periodic/triggered ADP, listener PROBE_TX commands (with exact-duplicate retry),
CONTROLLER_AVAILABLE liveness probes (with one-retry inflight tracking), unsolicited
responses to ≥ 16 controllers, IDENTIFY_NOTIFICATION multicasts. These need an
originator with an inflight table so returning responses route back to their owners.
**Disposition**: originator + inflight [03 §5/§8](architecture/03_packet_engine.md);
pipeline origins {RX, TIMER, SELF, MGMT} [03 §4](architecture/03_packet_engine.md).

**Landed shape, made normative 2026-09-30 (lane C6, issue #86)**: dispatch is RX-only;
timer expiries reach their owners on the timer service's expiry bus; AECP unsolicited
responses, IDENTIFY_NOTIFICATION among them (behind `P-EN-IDENTIFY-NOTIFICATION`), are
engine-internal SELF jobs; CONTROLLER_AVAILABLE is the one command PDU through the
originator's inflight table and central retry; PROBE_TX and its exact-duplicate retry
stay in the ACMP listener (`PROBE_SLOTS_P = 0` at the top); ADP writes its own frames.
**MGMT is not supported by this build**: no non-ATDECC path changes command-settable
state. [03 §4/§5](architecture/03_packet_engine.md#5-origins-originator-and-event-router)
state it; the normalizer's TIMER/SELF/MGMT ports stay tied off with that disposition.

## 6. Compliance matrix (F00.1)

<a id="fig-00-matrix"></a>
Column key — **Cov**: original-document coverage (C = Covered, P = Partial, A = Absent,
I = Incorrect), then the row's verification state, which `scripts/check-req-tags.py` holds
to the suites (09 §8.10): **traced** (a check of the row's own Ver category carries the
row's tag, and runs), **waived GAP-nn** (09 §8.10 waives the row against one of its own
findings, with the reason), **no check** (Ver lint or —, which call for no tagged check).
**Mand**: shall / should / may / rec (Milan "recommendation, future
requirement") / design (architecture-imposed) / — (informative). **Arch**: element of the new architecture. **Doc**: architecture document
section. **Ver**: verification category per [09 §3](architecture/09_verification.md)
(DIR directed · MTXW matrix walker · TOL malformed/tolerance · TIM timing · RND
randomized multi-controller · STORM notification storm · NVM power-cut restore ·
lint = CI gate, [09 §7](architecture/09_verification.md) · — = no dynamic
verification).

### 6.1 Discovery (ADP)

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-ADP-001 | Milan §5.6.2 | valid_time = 10 (20 s); advertise every 5 s | shall | A; traced | [GAP-07](#gap-07) | advertise SM | 04 §6.1 | TIM |
| REQ-ADP-002 | Milan §5.6.2 | entity_capabilities: AEM, VU, CLASS_A, GPTP =1; PERSISTENT_ACQUIRE, GENERAL_CONTROLLER_IGNORE, ENTITY_NOT_READY, ACMP_ACQUIRE_WITH_AEM =0; AEM_IDENTIFY_CONTROL_INDEX_VALID, AEM_INTERFACE_INDEX_VALID =1 | shall | A; traced | [GAP-16](#gap-16) | ADPDU sourcing table | 04 §3 | DIR |
| REQ-ADP-003 | Milan §5.6.2, §5.3.1 | entity_model_id valid EUI-64 (≠0, ≠all-1s); changes when static model changes | shall | A; traced | [GAP-01](#gap-01) | config/ID regs + packer model lint L9 (validity, the driven value, the recorded digest) | 07 §3.1, 09 §8.5 | DIR |
| REQ-ADP-004 | Milan §5.3.3.1 | talker_stream_sources / listener_stream_sinks = max across **all** configurations | shall | A; traced | [GAP-16](#gap-16) | ADPDU sourcing; packer model lint L11 (ENTITY counts and the driven values) | 04 §3, 07 §3.1, 09 §8.5 | DIR |
| REQ-ADP-005 | Milan §5.6.2 note | ADPDU fields independent of current configuration | shall | A; traced | [GAP-16](#gap-16) | ADPDU sourcing | 04 §3 | DIR |
| REQ-ADP-006 | Milan §5.6.1 | Start ADP only when ready to accept AECP commands and bind/probe requests | shall | A; traced | [GAP-16](#gap-16) | boot sequencer gate | 01 §5, 04 §6.1 | DIR |
| REQ-ADP-007 | Milan §5.6.3 | Advertise SM per AVB interface: DOWN/WAITING/DELAY; events DISCOVER(0/own), TMR, LINK, GM_CHANGE, SHUTDOWN | shall | I; traced | [GAP-16](#gap-16) | F04.2 | 04 §6.1 | MTXW |
| REQ-ADP-008 | Milan §5.6.3.5.2/.3 | Random delay 0–2 s at startup-link-up; 0–4 s otherwise | shall | A; traced | [GAP-07](#gap-07) | T-ADP-DELAY-START / T-ADP-DELAY | 08 §2 | TIM |
| REQ-ADP-009 | Milan §5.6.3.5.7 | GM change ⇒ re-advertise (via DELAY) | shall | A; traced | [GAP-16](#gap-16) | GPTP adapter event | 04 §6.1 | DIR |
| REQ-ADP-010 | Milan §5.6.3.5.6/.8/.11 | ENTITY_DEPARTING only on SHUTDOWN; never on link-down | shall | A; traced | [GAP-16](#gap-16) | F04.2 | 04 §6.1 | DIR |
| REQ-ADP-011 | IEEE §6.2.2.15 | available_index: 0 at init; ++ after each ENTITY_AVAILABLE tx; 0 on DEPARTING/power-up | shall | A; traced | [GAP-16](#gap-16) | available_index mgr | 04 §5 | DIR |
| REQ-ADP-012 | Milan §5.6.4 | Talker-discovery SM per bound Stream Input (not a general entity table) | shall | I; traced | [GAP-16](#gap-16) | F04.3 | 04 §6.2 | MTXW |
| REQ-ADP-013 | Milan §5.6.4.5.1/.2 | Ignore ENTITY_AVAILABLE whose gptp_grandmaster_id or domain ≠ local port state; available_index ≤ last ⇒ talker-restart handling | shall | A; traced | [GAP-16](#gap-16) | F04.3 guards | 04 §6.2 | DIR |
| REQ-ADP-014 | Milan §5.6.4.5.1 | TMR_NO_ADP from received valid_time; expiry ⇒ EVT_TK_DEPARTED | shall | A; traced | [GAP-07](#gap-07) | T-ADP-NOADP | 04 §6.2, 08 §2 | TIM |

### 6.2 Connection management (ACMP)

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-ACMP-001 | Milan §5.5.2.2 | Send truncated 56-B ACMPDU; accept 56-B and longer | shall | A; traced | [GAP-15](#gap-15) | parser + builder | 03 §3, 05 §3 | TOL |
| REQ-ACMP-002 | Milan §5.5.2.2 | BIND_RX/UNBIND_RX/PROBE_TX usage; unique_id = STREAM_OUTPUT/INPUT descriptor index of current config; CL_ENTRIES_VALID = 0 | shall | I; traced | [GAP-02](#gap-02) | F05.13 + rules | 05 §3 | DIR |
| REQ-ACMP-003 | Milan Table 5.26 | All five command timeouts = 200 ms | shall | A; traced | [GAP-07](#gap-07) | T-ACMP-CMD | 08 §2 | TIM |
| REQ-ACMP-004 | Milan §5.5.2.7, §5.5.4 | Talker stateless: no bound/settled listener state; SRP (never ACMP) tells the talker about listeners | shall | I; traced | [GAP-02](#gap-02) | stateless responder | 05 §6bis | DIR |
| REQ-ACMP-005 | Milan §5.5.4.1 | PROBE_TX responses: TALKER_UNKNOWN_ID / ignore-or-INCOMPATIBLE_REQUEST (wrong interface) / TALKER_DEST_MAC_FAILED / SUCCESS{cc=0, echo flags, stream params} | shall | A; traced | [GAP-02](#gap-02) | F05.11 | 05 §6bis | DIR |
| REQ-ACMP-006 | Milan §5.5.4.1 | Talker ignores STREAMING_WAIT; streams whenever bandwidth is reserved | shall | A; traced | [GAP-02](#gap-02) | F05.11 note | 05 §6bis | DIR |
| REQ-ACMP-007 | Milan §5.5.4.2 step 1, Tables 5.44/5.45; §5.5.4.4 | DISCONNECT_TX validates the source: invalid → TALKER_UNKNOWN_ID, valid → SUCCESS; neither changes state. GET_TX_CONNECTION → NOT_SUPPORTED | shall | I; traced | [GAP-02](#gap-02) | responder rules | 05 §6bis | DIR |
| REQ-ACMP-008 | Milan §5.5.4.3 | GET_TX_STATE: REGISTERING_FAILED = registering Listener Asking Failed; stream fields = declared values | shall | A; traced | [GAP-02](#gap-02) | responder rules | 05 §6bis | DIR |
| REQ-ACMP-009 | Milan §4.3.3.1 | Talker DA valid ⇔ MAAP-allocated ∧ (PROBE_TX ≤ 15 s ∨ matching listener attr registered) | shall | A; traced | [GAP-04](#gap-04) | F05.12, SRP adapter | 05 §6bis, 02 §4 | TIM |
| REQ-ACMP-010 | Milan Table 5.3 | MAAP conflict / PCP change ⇒ withdraw attr, wait 2×LeaveAll, new DA, re-declare | shall | A; traced | [GAP-04](#gap-04) | F05.12 | 05 §6bis | DIR |
| REQ-ACMP-011 | Milan §5.5.3.2/.5 | Listener SM: 8 states with normative transition set | shall | I; traced | [GAP-02](#gap-02) | F05.2–F05.6 | 05 §6 | MTXW |
| REQ-ACMP-012 | Milan §5.5.3.1 | Dispatch: LISTENER_UNKNOWN_ID on bad unique_id; silently ignore bad PROBE_TX_RESPONSE; ignore all other ACMP | shall | A; traced | [GAP-02](#gap-02) | RX classifier | 05 §4 | TOL |
| REQ-ACMP-013 | Milan §5.5.3.5.3 | Canonical bind: lock check, NVM store, response SUCCESS{cc=1}, start discovery, PROBE_TX{FAST_CONNECT=1}, save copy | shall | A; traced | [GAP-02](#gap-02) | A-ID actions | 05 §6.3 | MTXW |
| REQ-ACMP-014 | Milan §5.5.3.5.6/… (v1.2) | Re-bind same talker+source ⇒ update controller EID + STREAMING_WAIT only | shall | A; traced | [GAP-02](#gap-02) | matrix cells | 05 §6.3 | MTXW |
| REQ-ACMP-015 | Milan §5.5.3.5.16/.23 | Probe retry = exact duplicate once (200 ms), then 4 s backoff with acmpsta = LISTENER_TALKER_TIMEOUT (7) | shall | A; traced | [GAP-07](#gap-07) | F05.4 | 05 §6.4 | TIM |
| REQ-ACMP-016 | Milan §5.5.3.5.18/.36/.42/.48 | Settle: latch {stream_id, DA, VLAN}; SRP reservation; TMR_NO_TK 10 s; register/unregister events on exact match | shall | A; traced | [GAP-02](#gap-02) | F05.5 | 05 §6.5 | MTXW |
| REQ-ACMP-017 | Milan §5.3.8.9 | Settled SRP params must equal last PROBE_TX_RESPONSE; divergent talker attr ignored → re-probe | shall | A; traced | [GAP-02](#gap-02) | F05.5 guard | 05 §6.5 | DIR |
| REQ-ACMP-018 | Milan §5.3.8.5 | Declare Listener Ready iff matching Talker Advertise registered; Asking Failed optional | shall | A; traced | [GAP-04](#gap-04) | SRP adapter ops | 02 §4 | DIR |
| REQ-ACMP-019 | Milan §5.5.3.5.41/.47 | Talker departure while settled ⇒ note only (keep reservation) | shall | A; traced | [GAP-02](#gap-02) | matrix cells | 05 §6.3 | MTXW |
| REQ-ACMP-020 | Milan §5.5.2.4/.5 | BIND/UNBIND from non-lock-owner ⇒ CONTROLLER_NOT_AUTHORIZED | shall | C; traced | [GAP-06](#gap-06) | listener A1 on the notify lock (landed) | 06 §6.8 | DIR |
| REQ-ACMP-021 | Milan §5.3.8.2/.3, §5.5.3.5.2 | Binding (talker EID, source idx, controller EID, started) persists; boot with saved binding → PRB_W_AVAIL | shall | A; traced | [GAP-09](#gap-09) | NVM records | 07 §5 | NVM |
| REQ-ACMP-022 | Milan §5.5.3.5.4 etc. | GET_RX_STATE always SUCCESS; three content forms (unbound/probing/settled) | shall | A; traced | [GAP-02](#gap-02) | F05.14 | 05 §6.7 | DIR |
| REQ-ACMP-023 | Milan §5.3.8.6 | pbsta (3-bit) / acmpsta (5-bit; valid only while PROBING_ACTIVE) exposure | shall | A; traced | [GAP-02](#gap-02) | sink record | 05 §5, 07 §4 | DIR |

### 6.3 Control (AECP/AEM)

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-AEM-001 | Milan §5.4.1 | Responses may exceed the 524-octet cdl cap (READ_DESCRIPTOR, GET_AVB_INFO, GET_AS_PATH, GET_AUDIO_MAP, ADD/REMOVE_AUDIO_MAPPINGS) up to a max Ethernet frame | may | A; traced | [GAP-08](#gap-08) | oversize TX slot, up to the response buffer's cdl 592: READ_DESCRIPTOR and GET_AUDIO_MAP (06 §3; issue #50) | 03 §7 | DIR |
| REQ-AEM-002 | Milan §5.4.2.1 | ACQUIRE_ENTITY implemented but never SUCCESS; respond NOT_SUPPORTED | shall | C; traced | [GAP-15](#gap-15) | E_NSUPPE echo (landed) | 06 §6.8 | DIR |
| REQ-AEM-003 | Milan §5.4.2.2 | LOCK_ENTITY: UNLOCK flag; ENTITY descriptor only; 60 s auto-unlock ⇒ unsolicited | shall | C; traced | [GAP-06](#gap-06) | KL_aecp_notify lock (landed) | 06 §6.8 | TIM |
| REQ-AEM-004 | Milan §5.4.2.3 / IEEE §7.4.3 | ENTITY_AVAILABLE: 2021 response with flags + acquired/locked IDs | shall | A; traced | [GAP-01](#gap-01) | F06.14 row | 06 §6 | DIR |
| REQ-AEM-005 | Milan §5.4.2.21/§5.4.5.3 | Entity originates CONTROLLER_AVAILABLE for the departing-controller monitor with one retry; overflow probing remains optional | shall | A; traced | [GAP-17](#gap-17) | originator + inflight (landed for monitor) | 03 §5, 06 §7 | RND |
| REQ-AEM-006 | Milan §5.4.2.4 / IEEE §7.4.5 | READ_DESCRIPTOR: allowed while locked/acquired; 4-byte stub on failure | shall | P; traced | [GAP-01](#gap-01) | model store assembly | 07 §3 | DIR |
| REQ-AEM-007 | Milan §5.4.2.5 | SET_CONFIGURATION rejected with STREAM_IS_RUNNING if any input bound or output streaming; lock-protected | shall | A; traced | [GAP-01](#gap-01) | CFG_BARRIER + guard | 03 §6, 06 §6.4 | DIR |
| REQ-AEM-008 | Milan §5.4.2.7 | SET_STREAM_FORMAT: STREAM_IS_RUNNING / BAD_ARGUMENTS (mapping refs channel absent in new format) | shall | A; traced | [GAP-01](#gap-01) | validation chain | 06 §6.4 | DIR |
| REQ-AEM-009 | Milan §5.4.2.9 | SET_STREAM_INFO: OUTPUT only (INPUT → NOT_SUPPORTED); MSRP_ACC_LAT_VALID sets presentation offset 0..0x7FFFFFFF ns; any unsupported sub-flag ⇒ whole command NOT_SUPPORTED | shall | A; traced | [GAP-01](#gap-01) | F06.14 row | 06 §6.3 | DIR |
| REQ-AEM-010 | Milan §5.4.2.10 | GET_STREAM_INFO: Milan 80-B extended response (flags_ex, pbsta, acmpsta); renamed flags; full validity matrix | shall | C; traced | [GAP-01](#gap-01) | E_GSTRI + gsi face (landed; validity matrix = integrator serving the face) | 06 §6.2 | DIR |
| REQ-AEM-011 | Milan §5.4.2.11/.12 | SET/GET_NAME for all names of implemented descriptors; persisted | shall | C; traced | live commands and coherent descriptor overlay implemented; persisted and restored after the image walk by the D3 writer, records `0x80`+ordinal ([GAP-09](#gap-09); #61/#83, `tb/pp_top` D3N1 to D3N7, D3K) | name table + accepted name-lane write as the persistence trigger (the mark is completion only) | 06 §6.2.1, 07 §3/§5 | DIR |
| REQ-AEM-012 | Milan §5.4.2.13/.14 | SET/GET_SAMPLING_RATE per Audio Unit; may NOT_SUPPORTED when mappings mismatch and no SRC ("UNSUPPORTED" in spec text is a typo) | shall | A; traced | [GAP-01](#gap-01) | validation chain | 06 §6.4 | DIR |
| REQ-AEM-013 | Milan §5.4.2.15/.16 | SET/GET_CLOCK_SOURCE per Clock Domain; persisted | shall | A; traced | [GAP-09](#gap-09); persisted and restored by the D3 writer (#131, `tb/pp_top` D3S1/D3R1); over a ten-source domain, an AAF index set, refused past the list, saved and restored (#141, `tb/pp_top` D3C1 to D3C4); a blank, corrupt or torn record keeps the image's index, and the saved index is exported in every cycle the ADP enable is high (#52, D3C5, D3C6) | CLOCK_CFG class; D3 record `0x0A`+domain | 06 §6, 07 §5 | NVM |
| REQ-AEM-014 | Milan §5.4.2.17/.18, §5.3.12 | SET/GET_CONTROL for Identify (0 / 255; reset default 0) | shall | A; traced | [GAP-06](#gap-06) | identify handler | 06 §7 | DIR |
| REQ-AEM-015 | Milan §5.4.2.19/.20 | START/STOP_STREAMING: INPUT only (OUTPUT → NOT_SUPPORTED); bound+stopped→started semantics; persisted | shall | A; traced | [GAP-01](#gap-01) | F06.14 rows | 06 §6.3 | DIR |
| REQ-AEM-016 | Milan §5.4.2.21, §5.3.4.2 | REGISTER_UNSOLICITED: tuple {EID, MAC, port}, no duplicates, ≥16/interface, seq init 0; overflow ⇒ NO_RESOURCES (eviction probe is a MAY — not attempted); never deregister a responder | shall | C; traced | [GAP-06](#gap-06) | KL_aecp_notify registry (landed): P-N-CONTROLLERS rows per AVB interface, at P-N-AVB-INTERFACES 1 and 2 (#69) | 06 §7, 07 §4 | RND |
| REQ-AEM-017 | IEEE §7.4.37.2 | TIME_LIMITED flag: 300 s expiry ⇒ auto-DEREGISTER unsolicited to that controller; accept 2013 no-flags form | shall | C; traced | [GAP-06](#gap-06) | KL_aecp_notify TL timers (landed) | 06 §7 | TIM |
| REQ-AEM-018 | Milan §5.4.2.25 | GET_COUNTERS for every AVB_INTERFACE/CLOCK_DOMAIN/STREAM_IN/STREAM_OUT of current config; Milan mask set takes precedence over IEEE for STREAM_OUTPUT | shall | P; traced | [GAP-05](#gap-05) | E_GCTRS locate-first + type gate (landed); the integrator serves every bank behind the `ctr_*` face, Milan masks per type in the [integrator guide §7.1](guides/integrator.md#counters-face) | 06 §6.6, integrator guide §7.1 | DIR |
| REQ-AEM-019 | Milan Tables 5.1/5.4/5.6/5.7 | Counter semantics: invariant pairs; ≤1 s observation intervals; input bank reset on not-bound→bound; output MEDIA_RESET/TS_UNCERTAIN/FRAMES_TX reset on stream start | shall | A; waived GAP-05 | [GAP-05](#gap-05) | the integrator's banks behind the `ctr_*` face (owner decision 2026-09-19); every rule is the face contract of the [integrator guide §7.1](guides/integrator.md#counters-face) | integrator guide §7.1, 06 §6.6 | DIR |
| REQ-AEM-020 | Milan §5.4.2.26 | GET_AUDIO_MAP: fixed partition, subsets ≤176 channels, number_of_maps = N always | shall | C; traced | [GAP-08](#gap-08) | E_GAMAP + E_GAMAPO, both port directions off the integrator's map stores (landed); a subset of up to `P-MAP-SUBSET-CH-MAX` = 71 served whole, above it `NO_RESOURCES` (issue #50) | 06 §6.5, 07 §3 | DIR |
| REQ-AEM-021 | Milan §5.4.2.27/.28 | ADD/REMOVE_AUDIO_MAPPINGS: all-or-nothing BAD_ARGUMENTS; input conflict rules; REMOVE ignores duplicates; streaming-output changes gated by TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING; input maps changeable any time | shall | C; traced | live transaction implemented; persistence is the integrator's ([GAP-09](#gap-09), 07 §5.1; issue #83 ruling) | staged `MAP_VALID` transaction plus root projector; phase-5 commit beat as the persistence trigger | 06 §6.5 | DIR |
| REQ-AEM-022 | Milan §5.4.2.29 / IEEE §7.4.76 | GET_DYNAMIC_INFO: fixed-size-GET whitelist (else BAD_ARGUMENTS, nothing processed); per-element status; skip-on-overflow; incompatible with IN_PROGRESS | shall | P; traced | [GAP-15](#gap-15) | GDI iterator | 06 §6.7 | DIR |
| REQ-AEM-023 | IEEE §9.3.5.3.3 | Correctly-sized NOT_IMPLEMENTED response for every unimplemented opcode | shall | A; traced | [GAP-01](#gap-01) | response-size ROM | 06 §6 | TOL |
| REQ-AEM-024 | IEEE §9.3.2.6 | AEM: respond ≤240 ms (250 ms controller timeout); policy: never IN_PROGRESS | shall | P; traced | [GAP-07](#gap-07) | deadline engine | 08 §4 | TIM |
| REQ-AEM-025 | Milan §5.4.2.22 + Table 5.22 | DEREGISTER on auto-removal sent unsolicited to that controller only | shall | A; waived GAP-06 | [GAP-06](#gap-06) | KL_aecp_notify targeted holder (landed) | 06 §7 | RND |
| REQ-AEM-026 | IEEE §7.4.39, Milan §5.4.5.4 | IDENTIFY_NOTIFICATION: unsolicited-only (command ⇒ BAD_ARGUMENTS); multicast DA 91-E0-F0-01-00-01; 3× @150 ms; 1 s re-arm | should | A; traced | [GAP-06](#gap-06) | KL_aecp_notify identify sequencer behind P-EN-IDENTIFY-NOTIFICATION (landed; default 0); pp_top ID (third build), ID0, A6 | 06 §7 | TIM |

### 6.4 Milan Vendor Unique (MVU)

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-MVU-001 | Milan §5.4.3.2 | MVU framing: protocol_id 00-1B-C5-0A-C1-00; r=0; 15-bit command_type; padding excluded from cdl | shall | A; traced | [GAP-03](#gap-03) | MVU sub-decoder | 06 §6.9 | DIR |
| REQ-MVU-002 | Milan §5.4.4.1, §4.2.4 | GET_MILAN_INFO: protocol_version = 1; features (REDUNDANCY=0; TALKER_DYNAMIC_MAPPINGS optional); certification_version | shall | A; traced | [GAP-03](#gap-03) | F06.11 | 06 §6.9 | DIR |
| REQ-MVU-003 | Milan v1.2 §5.4.4.2/.3 | SET/GET_SYSTEM_UNIQUE_ID: recommended; October release waiver, not implemented (owner decision in GAP-03); revisit at P4 if the lab requires it or a targeted Milan revision makes it mandatory | rec | A; waived GAP-03 | [GAP-03](#gap-03) | NOT_IMPLEMENTED command echo; pp_top M4 verifies fallback only | 06 §6.9 | DIR |
| REQ-MVU-004 | Milan v1.2 §5.4.4.4/.5, §7.6 | SET/GET_MEDIA_CLOCK_REFERENCE_INFO: recommended; October release waiver, not implemented (owner decision in GAP-03); revisit at P4 if the lab requires it or a targeted Milan revision makes it mandatory | rec | A; waived GAP-03 | [GAP-03](#gap-03) | NOT_IMPLEMENTED command echo; pp_top M4 verifies fallback only | 06 §6.9 | DIR |
| REQ-MVU-005 | Milan §5.4.3.3/.4 | MVU status {SUCCESS, NOT_IMPLEMENTED}; 250 ms timeout / respond ≤240 ms | shall | A; traced | [GAP-03](#gap-03) | deadline engine; NOT_IMPLEMENTED echo for a voided MVU response | 08 §2, 06 §6.9 | TIM |

### 6.5 Notifications and registry

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-NOT-001 | Milan §5.4.5.1 | Fan-out: one message per registered controller excluding requester; per-entry DA/EID/seq on recorded port; seq +1 after handing to stack | shall | A; traced | [GAP-06](#gap-06) | KL_aecp_notify row walk (landed; pp_top ST fans out to all 16 rows) | 06 §7 | STORM |
| REQ-NOT-002 | Milan §5.4.5.2 | Triggers: every successful state-changing command; equivalent non-ATDECC changes while unlocked | shall | A; traced | [GAP-06](#gap-06) | command effect queue and observed fabric triggers (landed; every notifying class pushed byte-exact, pp_top NP). MGMT-origin (non-ATDECC) changes are **not supported by this build**: nothing outside ATDECC changes command-settable state (06 §7 trigger class 2) | 06 §7 | RND |
| REQ-NOT-003 | Milan Table 5.22 | Async triggers: GET_STREAM_INFO/GET_AVB_INFO/GET_AS_PATH field changes; GET_COUNTERS ≤1/descriptor/s; LOCK auto-unlock; auto-DEREGISTER | shall | P; traced | [GAP-06](#gap-06) | KL_aecp_notify observed trigger set and per-descriptor limiter (landed); committed per-sink accumulated-latency changes notify, unchanged refreshes stay silent (#113, pp_top GI response tests: walking one and walking zero isolate every comparator bit; six truncation mutants require named failures) | 06 §7 | STORM |
| REQ-NOT-004 | Milan §5.4.5.3 | Departing-controller detection: per-controller random 30–60 s; CONTROLLER_AVAILABLE + retry; any-status reply re-arms; silence ⇒ remove + targeted DEREGISTER | shall | A; traced | [GAP-06](#gap-06) | registry monitor plus originator (landed) | 06 §7 | TIM |
| REQ-NOT-005 | Milan §5.3.4.2 | Registry cleared by power cycle | shall | A; traced | [GAP-09](#gap-09); graded across a power cycle (#59, `tb/pp_top` D3V: two registrations, one TIME_LIMITED, notify nobody after it, no CONTROLLER_AVAILABLE or expiry DEREGISTER reaches them, sixteen new rows register, a returning controller restarts at sequence_id 0) | volatile policy: no record; the reset clears every row (`KL_aecp_notify` `valid_r`) | 07 §5 | NVM |

### 6.6 Entity model

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-MDL-001 | Milan §5.3.2 | Descriptor subset + cardinalities (AVB_INTERFACE ≥1, CLOCK_DOMAIN ≥1, CLOCK_SOURCE ≥1/domain); exactly one parent per descriptor | shall | A; traced | [GAP-08](#gap-08) | consumer's model (07 §3.1 ownership); packer model lint L1, and L12 for each descriptor's §7.2 extent (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-002 | Milan §5.3.3.4 | STREAM: buffer_length ≥ 2 126 000 ns; CLASS_A flag; no CRF+AAF mix in one format list; current_format ∈ list | shall | A; traced | [GAP-08](#gap-08) | consumer's model (07 §3.1 ownership); packer model lint L4 (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-003 | Milan §5.3.3.4 → IEEE 1722.1-2021 §7.2.6 | STREAM descriptors in Table 7-8 layout: formats_offset 138, N ≤ 46 (the 508-octet descriptor maximum of §7.2), `timing` at 136, redundancy tail `redundant_offset` = 138+8N with R = 0. Milan Annex C Table C.1 (formats at 136, no `timing`) is a **may** for any Stream and a shall only for a redundant pair; this PAAD declares none, so it is not emitted | shall (layout) | A; traced | [GAP-08](#gap-08) | consumer's model, served verbatim (07 §3.1 ownership; the image carries the Table 7-8 layout, 07 §3.2); packer model lint L4 stream-layout, which accepts Table 7-8 and the Milan §5.3.3.4 Annex C Table C.1 layout, redundancy tail included, and format-count (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-004 | Milan §5.3.3.5 | Same AVB_INTERFACE index for the same physical port in all configurations | shall | A; traced | [GAP-12](#gap-12) | consumer's model (07 §3.1 ownership); packer model lint L5 (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-005 | Milan v1.2 §5.3.3.6; IEEE 1722.1-2021 §7.2.32, Table 7-141 | CLOCK_SOURCE construction, §5.3.3.6's set as a minimum: one INPUT_STREAM per CRF-capable input (or the single AAF input when no CRF input exists); ≥1 INTERNAL if any output; gPTP-as-MC only non-redundant single-interface. One INPUT_STREAM source per AAF input may sit beside the CRF input's, in the order INTERNAL 0, CRF 1, AAF input k at 2 + k (milan-fpga D1). An index the CLOCK_DOMAIN's `clock_sources` list does not hold is BAD_ARGUMENTS (IEEE 1722.1-2021 §7.2.32, Table 7-141; Milan v1.2 §5.4.2.15/.16 add no argument rule) | shall | A; traced | [GAP-08](#gap-08) | consumer's model (07 §3.1 ownership); the processor's range check over L6's identity list; packer model lint L6, which accepts that set and reads no order (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-006 | Milan §5.3.3.7 | STREAM_PORT_INPUT has **no** static AUDIO_MAP (dynamic input mappings mandatory) | shall | A; traced | [GAP-08](#gap-08) | consumer's model (07 §3.1 ownership); packer model lint L7 (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-007 | Milan §5.3.3.8 | AUDIO_CLUSTER channel_count = 1 | shall | A; traced | [GAP-08](#gap-08) | consumer's model (07 §3.1 ownership); packer model lint L7 (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-008 | Milan §5.3.3.9 | ≤1 static mapping per output stream channel across all AUDIO_MAPs | shall | A; traced | [GAP-08](#gap-08) | consumer's model (07 §3.1 ownership); packer model lint L7 (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-009 | Milan §5.3.3.10 | Primary IDENTIFY CONTROL exists in all configurations at the same index | shall | A; traced | [GAP-06](#gap-06) | consumer's model (07 §3.1 ownership); packer model lint L8, with the IEEE §7.3.5.2 IDENTIFY format (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-010 | Milan §6.3/§6.4 | Talker ≥1 Stream Output and Listener ≥1 Stream Input advertising Base formats (AAF PCM32, 48/96/192 k, {1,2,4,6,8} ch); rate-completeness and configuration-uniformity rules | shall | A; traced | [GAP-01](#gap-01) | consumer's model (07 §3.1 ownership); packer model lint L3 (defence in depth) | 07 §3.1, 09 §8.5 | DIR |
| REQ-MDL-011 | Milan §7.3 | CRF media-clock stream format 0x041060010000BB80 (every CRF format listed, so the current one too); Class A | shall (if CRF) | A; traced | [GAP-01](#gap-01) | consumer's model (07 §3.1 ownership); packer model lint L3 crf-format, L4 class-a (defence in depth) | 07 §3.1, 09 §8.5 | DIR |

### 6.7 Persistence

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-PER-001 | Milan §5.3.5.1, §5.3.7.1/.6, §5.3.8.1/.2/.3/.7, §5.3.9.1, §5.3.10.1, §5.3.11.1, §5.3.13 | Persist: sampling rate; stream formats in/out; presentation offset; bound state + binding params; started/stopped; output + input mappings; clock source; all user names | shall | A; traced | [GAP-09](#gap-09); bindings, started/stopped and the scalar groups implemented (#131), the user names (#61/#83, `tb/pp_top` D3N; every D3 record type cut mid-commit, D3K, and every record type at 32 seeded-random cut points, D3KR); the maps assigned to the integrator (07 §5.1, issue #83 ruling) | binding manager + D3 writer; maps the integrator's (07 §5.1, §5.2 inventory) | 07 §5 | NVM |
| REQ-PER-002 | Milan §5.3.4.1/.2, §5.3.12 | Volatile: lock state; controller registry; identify = 0 after reset | shall | A; traced | [GAP-09](#gap-09); graded across a power cycle that restores a saved binding (#62, `tb/pp_top` D3V: the lock free to a second controller, the registry empty, IDENTIFY 0 from the restore on, each reset arm's deletion killed) | volatile policy: no record; IDENTIFY (selector 7) excluded at the D3 trigger | 07 §5 | NVM |
| REQ-PER-003 | (unstated) | Current configuration index persistence — Milan silent; design decision: persist | — | A; traced | [GAP-09](#gap-09); implemented (#131); restored to the ADPDU, GET_CONFIGURATION and the ENTITY descriptor, and a blank, corrupt or torn record keeps the image default (#63, `tb/pp_top` AD5 to AD9) | design decision §8 item 1, retained; D3 record `0x00` | 07 §5 | NVM |

### 6.8 Base-protocol crossings (control-plane visible)

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-NET-001 | Milan §5.3.6.1 | Track per interface: gPTP GM ID, path sequence, domain, propagation delay (GET_AVB_INFO/GET_AS_PATH + notifications) | shall | P; traced | [GAP-04](#gap-04) | E_GAVB/E_GASP + gsi face (supports live propagation delay, depth-8 path data, and independent AVB/path change strobes; the consumer owns their sources) | 02 §4.3, 06 §6.10 | DIR |
| REQ-NET-002 | Milan §5.3.6.2, §4.2.7.2.1 | Track MSRP domain params (Class A priority 3, default VID 2); adopt + re-declare on differing Domain declaration; notify on change | shall | A; traced | [GAP-04](#gap-04) | `srp` contract; SRP engine | 02 §4, 10 §6.1 | DIR |
| REQ-NET-003 | Milan §5.3.7.2–.4, §5.3.8.8/.9 | Track SRP talker declaration + listener registration states, failure code + bridge ID, accumulated latency | shall | A; traced | [GAP-04](#gap-04) | `srp` contract; SRP engine | 02 §4, 10 §6.3/.4 | DIR |
| REQ-NET-004 | Milan Tables 5.1/5.13 | LINK_UP/LINK_DOWN counter invariant; GPTP_GM_CHANGED counter | shall | A; waived GAP-05 | [GAP-05](#gap-05) | the integrator's AVB_INTERFACE bank behind the `ctr_*` face (owner decision 2026-09-19): `link_up_i` edges and grandmaster identity changes, per the [integrator guide §7.1](guides/integrator.md#counters-face) | integrator guide §7.1, 02 §4.6 | DIR |
| REQ-NET-005 | Milan §4.4.2.2 | Listener discards AVTPDUs not matching configured input format (enforced in AVTP engine; control plane configures) | shall | A; waived GAP-04 | [GAP-04](#gap-04) | the integrator's AVTP datapath, armed from the published bound view and input formats (`acmp_bound_*`, `aecp_fmt_in_o`) | 02 §4.4 | DIR |

### 6.9 SRP endpoint engine (in scope by owner decision — §8 item 9)

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-SRP-001 | Milan Table 4.3 | MRP timers: joinTime 200 ms (180–240), LeaveTime 5000 ms (4500–7500), leavealltimer 10–15 s, periodictimer 1000 ms (900–1500) | shall | A; traced | [GAP-04](#gap-04) | T-MRP-* | 10 §9, 08 §2 | TIM |
| REQ-SRP-002 | Milan §4.2.7.1.2 | Malformed MRPDU: may process up to the bad field, then discard the rest of that vector-attribute list and all subsequent messages in the PDU | shall | A; traced | [GAP-04](#gap-04) | decoder tolerance | 10 §3 | TOL |
| REQ-SRP-003 | Milan §4.2.7.1.3 | EndMark transmitted as explicit 0x0000 when padding follows | shall | A; traced | [GAP-04](#gap-04) | vector encoder | 10 §3 | DIR |
| REQ-SRP-004 | Milan §4.2.7.2.1 | Class A Domain: priority 3 / default VID 2 at startup and link-up; adopt received params + re-declare on differing declaration; Domain TX independent of gPTP port state | shall | A; traced | [GAP-04](#gap-04) | Domain FSM F10.2 | 10 §6.1 | DIR |
| REQ-SRP-005 | Milan §4.2.7.2.2 | Registrar `IN → MT` immediately on rLv (no leavetimer) outside the LeaveAll cycle (Δ13) | shall | A; traced | [GAP-04](#gap-04) | registrar rule F10.9 | 10 §6.5 | MTXW |
| REQ-SRP-006 | Milan §4.2.7.3, §4.3.2, §4.4.1 | MVRP: talker joins the VLAN before sending any stream frames; listener declares the VID of its settled sinks | shall | A; traced | [GAP-04](#gap-04) | VLAN FSM F10.3 | 10 §6.2 | DIR |

### 6.9b MAAP engine (in scope as the opt-in internal allocator — §8 item 11)

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-MAAP-001 | 1722-2016 B.1 + Table B.9 | Dynamic-pool addresses only via MAAP; the block drawn uniformly from 91:E0:F0:00:00:00..91:E0:F0:00:FD:FF with the whole block inside the pool | shall | A; waived GAP-04 | [GAP-04](#gap-04) | generate_address + fit clamp | [11](architecture/11_maap_engine.md) §6 | RND |
| REQ-MAAP-002 | 1722-2016 B.2 (Figure B.1, Tables B.1/B.10) | PDU byte layout with cdl 16 and stream_id 0; PROBE/ANNOUNCE to 91:E0:F0:00:FF:00, DEFEND unicast to the probe's SA; higher maap_version with a known type interpreted, reserved types ignored | shall | A; traced | [GAP-04](#gap-04) | frame builder + DA-qualified validator demux | [11](architecture/11_maap_engine.md) §3 | DIR |
| REQ-MAAP-003 | 1722-2016 Table B.7 + Table B.8 | The initial PROBE plus MAAP_PROBE_RETRANSMITS = 3 retransmits, the first ANNOUNCE immediately at probeCount!, the claim valid only in DEFEND | shall | A; traced | [GAP-04](#gap-04) | walker | [11](architecture/11_maap_engine.md) §6 | MTXW |
| REQ-MAAP-004 | 1722-2016 B.3.4 | probe_timer strictly inside (500, 600) ms and announce_timer strictly inside (30, 32) s, drawn fresh at every start | shall | A; traced | [GAP-04](#gap-04) | T-MAAP-* via PRNG kinds 5/6 | 08 §2, [11](architecture/11_maap_engine.md) §8 | TIM |
| REQ-MAAP-005 | 1722-2016 B.3.5.5–.7 + Table B.7 + B.3.6.4 | The conflict matrix: rProbe! defended in DEFEND without tie-break; compare_MAC (octet-wise reversed, TRUE = no action) in PROBE/rProbe! and DEFEND/rDefend!+rAnnounce!; every yield re-randomizes | shall | A; traced | [GAP-04](#gap-04) | row decode | [11](architecture/11_maap_engine.md) §6 | MTXW |
| REQ-MAAP-006 | 1722-2016 B.3.6.6 + B.2.7/B.2.8 | DEFEND echoes the probe's requested_*; conflict_start = first allocated conflicting address, conflict_count from it; both fields 0 in PROBE/ANNOUNCE | shall | A; traced | [GAP-04](#gap-04) | defend fields | [11](architecture/11_maap_engine.md) §3 | DIR |
| REQ-MAAP-007 | 1722-2016 B.3.5.2 + Table B.7 (Release! row, footnote c) + B.3.2 | Release! is a local event: stop timers, INITIAL, no PDU generated after the fall (a frame an earlier entry already requested may drain) | shall | A; traced | [GAP-04](#gap-04) | engage-fall arc | [11](architecture/11_maap_engine.md) §6 | DIR |

### 6.10 Non-redundant scoping

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-SCP-001 | Milan §4.2.5, §8.1 | Redundancy optional; non-redundant PAAD excludes all of ch. 8 | may | A; waived GAP-12 | [GAP-12](#gap-12) | scoping stmt | 01 §1 | DIR |
| REQ-SCP-002 | Milan Table 5.20 | GET_MILAN_INFO REDUNDANCY flag = 0 | shall | A; traced | [GAP-12](#gap-12) | F06.11 | 06 §6.9 | DIR |
| REQ-SCP-003 | Milan §5.3.4.2, §8 | Keep per-interface keying (registry, ADP, counters) parameterized as the redundancy seam | design | A; traced | [GAP-12](#gap-12) | P-N-AVB-INTERFACES, `N_AVB_IF_P` on the top (1 or 2; #69). **Keyed** at 2: an ADP advertise machine per interface (`KL_adp_engine` `N_IF_P`); the received frame's interface, `rx_if_index_i` read with its last byte, as every transaction's `interface_index`; the registry row's port, so REGISTER and DEREGISTER match {Entity ID, MAC, port}, and the registry depth, P-N-CONTROLLERS rows per interface, so a two-interface build holds Milan §5.3.4.2's minimum on each interface (`KL_aecp_notify` `N_IF_P`; a row's index is its CA owner and owner-tag entry, so the rows of one index take turns at the availability probe); the Table 5.22 GET_COUNTERS slots, one per AVB_INTERFACE; GET_COUNTERS, GET_AVB_INFO and GET_AS_PATH reads, by descriptor_index through the integrator's `ctr_*` and `gsi_*` faces. **Not keyed**, each one per top: the MAC trunks (no frame carries an egress interface out); the class-D levels `link_up_i`, `gm_change_i`, `gm_id_i`, `gptp_domain_i`, read by every advertise machine; the SRP engine (one participant set); the availability monitor, which matches {Entity ID, MAC} on any interface, so one command supersedes the live probe of every row of its controller and each is cancelled; the GET_AVB_INFO and GET_AS_PATH pushes (AVB_INTERFACE 0); the side-port snapshot and `adp_next_avail_index_o` (interface 0). The processor's own counters (snapshot words 4 to 9, 24 to 29, 32 to 37) count the shared trunk and engines; the Milan Table 5.1 AVB_INTERFACE counters are the integrator's (GAP-05) | 01 §7 | DIR |

### 6.11 Reusability, verification and process requirements

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-REU-001 | project charter | Bus-agnostic interface contracts: every external contract is a signal table with a handshake class (A–F) | design | A; no check | [GAP-10](#gap-10) | interface classes | 02 §1–§8 | — |
| REQ-REU-002 | project charter | One parameter master table with defaults and consumers; derived values referenced by ID, never copied | design | A; no check | [GAP-10](#gap-10) | F01.5 + single-source rules; `make check` `ids` fails any P-/T- ID without its F01.5/F08.1 row | 01 §7, docs/README §2, 09 §7 | lint |
| REQ-REU-003 | project charter | One core clock domain; MAC boundaries cross via dual-clock FIFOs with frame-atomic handoff | design | A; no check | [GAP-10](#gap-10) | clocking contract; the dual-clock MAC FIFOs are the integrator's, outside the top, and the RX one presents only complete, FCS-good frames (the byte face has no err/abort) | 02 §2 rule 2, 02 §3; integrator guide §1, §3 | — |
| REQ-VER-001 | project charter | Requirement↔verification traceability: every REQ row names its Ver category or an explicit non-dynamic marker; release gate = the categories of 09 §3 + the CI gates of 09 §7 | design | A; no check | [GAP-11](#gap-11) | verification plan; the REQ tags in `tb/`, held by `scripts/check-req-tags.py` | 09 §3, 09 §7, 09 §8.10 | — |
| REQ-VER-002 | project charter | Single-source generation: ROMs, golden model, stimulus vectors and doc tables all derive from one command model | design | A; no check | [GAP-11](#gap-11) | F09.1 | 09 §1 | lint |
| REQ-FWX-001 | IEEE §9.3.5.3.3 (outside Milan's mandatory command set; IEEE Annex D is informative) | No firmware update, REBOOT, MEMORY_OBJECT operations or AEM checksum; those opcodes take the unknown-opcode path → `NOT_IMPLEMENTED` echo | shall | A; traced | [GAP-13](#gap-13) | dispatch default | 06 §6 | DIR |
| REQ-DOC-001 | project charter | Every figure an editable artifact (Mermaid/WaveDrom/draw.io) with committed exports, regeneration and staleness gates | design | A; no check | [GAP-14](#gap-14) | Makefile gates, run whole by the CI docs-gates job (`make check`); hand-authored SVG is a listed class held by `make figures` | docs/README §3, 09 §7 | lint |

## 7. Disposition of findings (F00.2)

<a id="fig-00-disposition"></a>

| GAP | Severity | Resolution | Addressed in | Verified by | Open residue |
|---|---|---|---|---|---|
| [GAP-01](#gap-01) | Blocker | Full command/descriptor inventory + per-command rules | [F06.14](architecture/06_aecp_engine.md#fig-06-cmdtable), §6 matrix | DIR/TOL | [#76](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/76) |
| [GAP-02](#gap-02) | Blocker | Milan-native ACMP: stateless talker + listener SM package | [05](architecture/05_acmp_engine.md) | MTXW | none found |
| [GAP-03](#gap-03) | Major | MVU sub-decoder + one implemented group (GET_MILAN_INFO); SUID/MCR pairs waived for October by the linked owner decision; reserved enable names have no RTL consumer | [06 §6.9](architecture/06_aecp_engine.md#69-mvu-commands), [F01.5](architecture/01_overview.md#fig-01-params) | DIR: pp_top M1/M2 feature fields and M4 four refusals; no implementation claim for the waived pairs | [#55](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/55), [#56](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/56), [#77](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/77) resolved by waiver; timing and the voided-response status: [#57](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/57), graded by the deadline engine and `tb/pp_top` sections DL (DL3 and DL8 the MVU answers) and TB ([09 §8.3](architecture/09_verification.md#83-the-aecp-deadline-and-the-hazard-classes-issues-81-57-84)) |
| [GAP-04](#gap-04) | Blocker | Interface classes A–F; SRP and MAAP class-B faces; gPTP, AVTP and media clocking as class-D levels and the `gsi_*`/`ctr_*` read faces, the landed shape of each written down (02 §1, §4.3 to §4.6, §5, F02.10); status dictionary; in-scope SRP engine | [02](architecture/02_interfaces.md), [10](architecture/10_srp_engine.md) | DIR/MTXW/TOL/TIM | [#78](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/78) for the interface text; the Domain (F10.2) and VLAN (F10.3) machines are graded by directed cases, not a cell walk |
| [GAP-05](#gap-05) | Major | **Decision (owner, 2026-09-19): the counters live in the integrator**, behind the documented `ctr_*` face with the Milan-precedence masks per descriptor type; the processor owns the GET_COUNTERS command (locate-first, type gate, fixed body) and the Table 5.22 push from `ctr_change_i`, and keeps no bank (F07.10) | [integrator guide §7.1](guides/integrator.md#counters-face), [02 §4.6](architecture/02_interfaces.md#sec-02-ctr), [06 §6.6](architecture/06_aecp_engine.md#sec-06-counters) | DIR: `tb/pp_top` K1 to K8 (the command and the face), U9 (the push), K9 to K17 (the integrator's AVB_INTERFACE and CLOCK_DOMAIN counts on the wire, their invariants, the push and its slot rule; [09 §8.7](architecture/09_verification.md#87-the-counters-face-issues-44-79)) | none in this repository: the counters and their rules are the integrator's to keep and grade ([#79](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/79), [#44](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/44)) |
| [GAP-06](#gap-06) | Major | Registry + monitor + fan-out + lock manager + identify (IDENTIFY_NOTIFICATION behind `P-EN-IDENTIFY-NOTIFICATION`) | [06 §7](architecture/06_aecp_engine.md) | RND/STORM/TIM: `tb/pp_top` RN, ST, ID, NP | [#80](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/80) (lane C6); the eviction sweep is a MAY, not attempted |
| [GAP-07](#gap-07) | Major | Master T-ID table, timer service, PRNG, budgets | [08](architecture/08_timing.md) | TIM | [#81](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/81) |
| [GAP-08](#gap-08) | Major | Entity-model store (image + current-value overlay on READ_DESCRIPTOR), consumer-built Table 7-8 streams and model rules (07 §3.1), oversize TX slot up to the response buffer | [07 §3](architecture/07_memory_maps.md), [03 §7](architecture/03_packet_engine.md) | DIR: tb/pp_top AX RD (READ_DESCRIPTOR after each SET and after the D3 restore) and OV/PG/RB (above cdl 524, TX slot 4, inside the response reservation) | [#82](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/82); the model rules are the consumer's, and the packer's lint L1–L12 is defence in depth with one negative case per check (07 §3.1, 09 §8.5) |
| [GAP-09](#gap-09) | Major | NVM manager, records, commit/restore flows | [07 §5](architecture/07_memory_maps.md) | NVM | [#83](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/83) |
| [GAP-10](#gap-10) | Major | CDC/reset, interface contracts, parameter table, profiles, grounded hazards | [01](architecture/01_overview.md)/[02](architecture/02_interfaces.md)/[03](architecture/03_packet_engine.md) | DIR | [#84](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/84) |
| [GAP-11](#gap-11) | Minor | Verification strategy + traceability | [09](architecture/09_verification.md) | lint: `scripts/check-req-tags.py`, static in `make check` and executed in the CI suites job (09 §8.10) | [#72](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/72) |
| [GAP-12](#gap-12) | Minor | Explicit non-redundant scope + parameterized seams | [01 §1/§7](architecture/01_overview.md) | DIR | [#69](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/69) |
| [GAP-13](#gap-13) | Minor | Firmware assist optional behind side-port flag | [02 §7](architecture/02_interfaces.md) | DIR | none found |
| [GAP-14](#gap-14) | Info | All figures Mermaid/WaveDrom/draw.io or listed hand-authored SVG + lint | [docs/README.md](README.md), `Makefile` | lint | [#75](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/75) |
| [GAP-15](#gap-15) | Info | Conventions + parser rules + status policies | [docs/README.md](README.md) §4, [03 §3](architecture/03_packet_engine.md) | TOL | none found |
| [GAP-16](#gap-16) | Blocker | ADP advertise SM + talker-discovery SM (entity table dropped) | [04](architecture/04_adp_engine.md) | MTXW | [#85](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/85) |
| [GAP-17](#gap-17) | Blocker | Originator + inflight table (CONTROLLER_AVAILABLE); the landed origins: RX-only dispatch, expiry bus, engine-internal SELF jobs; MGMT not supported | [03 §5](architecture/03_packet_engine.md) | RND: `tb/originator` R (the inflight model) | [#86](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/86) (lane C6) |

The **Open residue** column records the audit of 2026-09-18 at main `6a878f6`: each finding was
checked against what `hdl/` and `tb/` carry, not against this table. A linked issue tracks what is
still unimplemented or ungraded for that finding and lists the requirement tickets under it;
"none found" means the resolution is implemented and a suite of the named category grades it.
GAP-03 carries the later October-release waiver above; it does not re-grade the
original-document Cov column. REQ-MVU-005's timing evidence is the deadline engine and
`tb/pp_top` sections DL and TB (issue #57).

## 8. Residual risks and open decisions

| # | Item | Decision taken | Recorded in |
|---|---|---|---|
| 1 | Current configuration index persistence unstated by Milan | **Persist** (least surprise across power cycles) | 07 §5 |
| 2 | AEM status for lock violation unspecified by Milan | Use IEEE `ENTITY_LOCKED` (3) | 06 §6.8 |
| 3 | Milan §5.4.2.13 references "UNSUPPORTED" status | Read as `NOT_SUPPORTED` (11) — no such code exists | 06 §6.4 |
| 4 | `system_unique_id` / `user_mcr_prio` / media-clock-domain-name persistence unstated | Earlier persistence plan deferred with the commands under the October waiver; no processor storage or persistence for these fields; P4 if required by the lab | [06 §6.9](architecture/06_aecp_engine.md#69-mvu-commands), 07 §5 |
| 5 | Dual ADP startup delay (0–2 s) vs link-up (0–4 s) — easy single-constant bug | Two distinct T-IDs | 08 §2 |
| 6 | talker/listener_capabilities bits unconstrained by Milan | Set per IEEE Table 6-3/6-4 (IMPLEMENTED + AUDIO/MEDIA_CLOCK as per product) | 04 §3 |
| 7 | IN_PROGRESS vs GET_DYNAMIC_INFO | Never emit IN_PROGRESS; hard ≤240 ms response budget | 06 §5, 08 §4 |
| 8 | Milan-vs-IEEE STREAM_OUTPUT counter masks | Milan masks in Milan profile (Δ-tagged); IEEE masks in plain-IEEE profile ROM | 06 §6.6 |
| 9 | SRP location (originally out of scope per the reviewed doc's §21) | Owner decision 2026-08-11: SRP endpoint (MSRP/MVRP participant) moved **in scope** as doc 10 — 1 Domain FSM, 1 VLAN FSM (Class A, single VID), N + M stream FSMs; MAAP stays external; external-stack alternative retained (`P-EN-SRP-ENGINE`) | [10](architecture/10_srp_engine.md), §6.9 |
| 11 | MAAP location (item 9 recorded "MAAP stays external") | The seam grew an internal server: `KL_pp_maap` ([11](architecture/11_maap_engine.md)) answers the 02 §4.2 contract when the quasi-static `cfg_maap_internal_i` = 1 and publishes the claim for the fabric; the DEFAULT stays 0 = external, byte-identical to the landed integrations, so item 9's shipping wiring is unchanged until the integrator flips the input | [11](architecture/11_maap_engine.md), §6.9b |
| 10 | Implementation strategy for the reference platform | Owner decision 2026-08-11: full implementation (scenario B of [docs/10](10_RESOURCE_AND_EFFORT.md)) proceeds in this repository; the reference platform cuts over by **direct substitution at parity** — the superseded planes are deleted, never parameterized (git history preserves them); the area verdict is accepted with eyes open, the architecture/conformance value is the goal | [docs/10](10_RESOURCE_AND_EFFORT.md) §5/§10 |
