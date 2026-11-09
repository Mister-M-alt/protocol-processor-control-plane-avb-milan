<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# srp_encoder — KL_srp_encoder + KL_srp_domain + KL_srp_vlan suite

Exit 0 = PASS. `make` builds `srp_tb_wrap` (encoder wired to a **real**
`KL_pp_tx_slots` pool; Domain and VLAN singletons stand-alone) and runs
1646 checks against an **independent** C++ packer written from 802.1Q
§10.8.1/§35.2.2 and Milan §4.2.7 — never from the RTL — and an independent
byte-level MRPDU parser. Every captured frame crossed the actual slot RAM
through its serialize port.

## What it proves

**Encoder (docs/architecture/10 §3 + §7)** — E1..E12:
- byte-exact MRPDU emission for single- and multi-attribute PDUs, MSRP and
  MVRP (MVRP has **no** AttributeListLength field);
- `AttributeListLength` counts the VectorAttributes **plus** the
  AttributeList EndMark (802.1Q §35.2.2.6) — checked both byte-exact and at
  the hand-computed offset (a 4-byte Domain vector lists as 9, not 7);
- dual EndMark always explicit, so the EndMark precedes any MAC padding
  (Milan §4.2.7.1.3) even on a 30-byte frame;
- sorted-run detection: consecutive `{stream_id, DA}` (Talker, both fields
  stepping), `stream_id` (Listener), `{SRclassID, priority}` (Domain) and
  VID (MVRP) sequences fold into one VectorAttribute with NumberOfValues up
  to the table depth (12); a same-type non-successor (stream_id steps, DA
  frozen) starts a new vector in the same message;
- ThreePacked radix-6 and FourPacked radix-4 packing including tail padding
  (NoV=3 and NoV=5 lanes hand-computed);
- cadence aggregation: N pushed events sit silently, then ONE T-MRP-JOIN
  tick produces ONE alloc + ONE commit + ONE frame (the functional advance
  over the reference platform's one-frame-per-event vectors);
- LeaveAllEvent injection per application, taken by that application's next
  drain and consumed by it (E7, E7b);
- per-participant independence (an MSRP tick never drains the MVRP table),
  simultaneous ticks serialize MSRP-then-MVRP, full-table backpressure on
  the push handshake, unknown-type drop strobe.

**LeaveAll per Attribute Type (10 §6.5; 802.1Q-2014 §10.8.2.6, §10.7.5.20
NOTE, §10.8.2.8 f/g, §10.8.2.10.1 NOTE)** — L1..L6:
- L1: all 15 combinations of declared MSRP types (non-empty subsets of
  {Talker Advertise, Talker Failed, Listener, Domain}, push order rotated).
  Each LeaveAll MRPDU is byte-exact against the packer. The independent
  parser confirms that every type carries LeaveAllEvent exactly once, on its
  first vector. A declared type gets no LeaveAll-only vector, and each
  undeclared type gets one after every declared message.
- The same drain is re-sent without LeaveAll and compared. The declared
  messages differ only by one LeaveAllEvent bit per declared type, and the
  frame grows by exactly the LeaveAll-only messages. With every type declared
  (L1[15]) it grows by **0** octets: nothing is added.
- L2: the NumberOfValues-0 vectors byte-for-byte. A Domain-only drain closes
  with exactly the three LeaveAll-only messages the bench switch itself sends
  after its own Domain message. Those bytes are transcribed from the
  milan-fpga #117 Run B tap capture, switch port `3c:c0:c6:fe:02:11` at
  10.429430 s. The Domain one, which the switch never needs, is
  hand-computed.
- L3: one type spread over two vectors of one message and over a later
  second message carries LeaveAll only on its first vector.
- L4: MVRP has one Attribute Type, so its LeaveAll MRPDU is unchanged. The
  first VID vector is flagged and nothing is appended. Without LeaveAll the
  frame differs by that one bit, and the MSRP lane is untouched.
- L5: a LeaveAll requested while a drain is already writing is carried
  **whole** by the next MRPDU. It is never split across two MRPDUs and never
  dropped. The base encoder consumed it at commit, so it was lost.
- L6: a LeaveAll on a drain's start cycle is taken by that drain, once.

**Domain (10 §6.1, F10.2)** — D1..D9: declare defaults {class A id 6,
priority 3, VID 2} at startup/LINK_UP; adopt a differing received Class A
Domain as withdraw-old (Lv, at once) + declare (New at the next tx!) + one
DOMAIN_CHANGE strobe + class-D levels; identical parameters are no change; the
certified two-class bridge shape (FirstValue {5, 2, VID}, NoV=2) surfaces Class
A as value 1 with priority 3 by the §3 range rule; non-covering vectors
ignored; periodic/LeaveAll re-join the ADOPTED declaration and never revert;
revert happens on LINK_DOWN only, LINK_UP re-declares the defaults. Domain TX
is independent of gPTP state structurally — the module has no gPTP port.

The declared value's Applicant follows 802.1Q-2018 Table 10-3 (issue #216):
every message is the one its state gives at a tx! opportunity, which the
harness gives as `h.dom_tx()` (the round's `tx_tick_i`, and with
`leaveall_own_i` for the own sLA's txLA! row) and waits on until
`dom_tx_done_o`. D1: LINK_UP (Begin!, New!) leaves VN and sends nothing; four
tx! then send New, New again (AN), JoinMt (AA, the Registrar MT, note 8) and
nothing (QA). D3: a received LeaveAll (rLA!: QA → VP) owes two re-joins, from
VP and then AA. D8: the adopting declaration is registered, so its second New
takes AN to QA (note 8), and `periodic!` then owes a JoinIn.

**Domain re-join against its Registrar (issue #184; 802.1Q-2018 §10.7.6.3,
Table 10-4; Milan v1.2 §4.2.7.2.2)** — DR1..DR11. The expected codes come from
§10.7.6.3, not from the RTL: JoinIn only while the Registrar of the declared
value is IN, JoinMt in MT or LV. Each re-join is the Applicant's sJ at a tx!.
- DR1: no peer: the periodic re-join is JoinMt.
- DR2: the peer declares the same Domain: registered, not adopted; JoinIn.
- DR3a, DR3: a received LeaveAll (IN → LV, QA → VP) followed by the peer's
  Domain declaration (LV → IN), as a bridge's LeaveAll MRPDU carries them. The
  re-join waits for the tx! (DR3a), and is then JoinIn (DR3).
- DR4: the own LeaveAll from QA, with its sLA on the round's own tx! clock (the
  txLA! row: QA sends sJ where tx! sends nothing): LV, JoinMt.
- DR5: LV re-joins JoinMt until the peer declares again, then JoinIn.
- DR6: the peer's Lv takes IN to MT at once: the very next re-join is JoinMt.
- DR7: an Lv of another Domain value leaves the registration IN.
- DR8: a registering event in the LeaveAll's own clock keeps IN.
- DR9: an adoption carries the adopting declaration's registration: Lv, New,
  New (AN to QA), then the periodic re-join JoinIn of the adopted value.
- DR10: an adoption held behind a busy queue, withdrawn (Lv) before it
  applies, is declared but not registered: New, New, then JoinMt.
- DR11: LINK_DOWN and LINK_UP clear the registration: New, New, then JoinMt.

D2, D3, D8 and B1 grade the same rule on their own walks: no registration
gives JoinMt (D2, D3); the adopting declaration registers, so D8's periodic
re-join is JoinIn and its LeaveAll re-join JoinMt; B1's Domain is registered
since DA4, so JoinIn.

**The Domain's tx! (issue #216)** — DT1..DT3: a tx! whose message the encoder
has not taken is not done, and is done once after it (DT1); a tx! with nothing
to send is done within four clocks (DT2); a tx! that finds the two-deep queue
full is owed until its message fits, then served (DT3). `KL_srp_top`'s round
waits for `tx_done_o` before its MSRP drain.

**The received messages of the declared value (issue #216; Table 10-3 with
operPointToPointMAC TRUE)** — DA1..DA5: rJoinIn! in AA leaves nothing to send
(QA, DA1); rMt! in QA owes the re-join (DA2); rIn! in AA, point-to-point, leaves
nothing to send (DA3, note 5); rJoinMt! in QA owes the re-join and registers
(DA4); a message of another Domain value moves nothing (DA5).

**VLAN (10 §6.2, corrected F10.3)** — V1..V11: per-VID refcount keyed by each
user's OWN stream VID; each VID's Applicant follows Table 10-3 and its messages
go at an MVRP tx!, which the harness gives as `h.vlan_tx()` and waits on until
`vlan_tx_done_o` (issue #216). V1: the first user's New! leaves VN, and four
tx! send New, New, JoinMt (the Registrar MT) and nothing. Two users of one VID
produce one declaration (V2); V3: `periodic!` owes a JoinMt; a Domain VID change
moves nothing — a new user brings the new VID and two VIDs are briefly live
while the old one stays frozen until its LAST user leaves (V4, V6); a received
LeaveAll and the own LeaveAll's txLA! move every VID (V5); the last user's Lv
waits for its tx! (V6); unknown-VID leave and table overflow are error strobes,
never wire events (V7, V8); a VID retired and joined again is New (V9), and one
joined again while its Lv is still owed sends New and no Lv (V10); V11: a
withdrawn VID's txLA! sends nothing (LO), the next tx! the Mt it owes, then
nothing.

**Each VID's Registrar (issue #216; Table 10-4, MVRP as printed)** — VR0..VR11:
VR1 a received JoinIn registers (JoinIn re-join); VR2 a received Lv takes IN to
LV (JoinMt); VR3 a received value of another VID moves nothing; VR4 In and Mt
register nothing, and the Mt owes the re-join; VR5 JoinMt registers; VR6 a
received LeaveAll takes IN to LV; VR7 a registering value in the LeaveAll's own
clock wins; VR8 the own LeaveAll's tx! ages IN to LV (JoinMt); VR9 rJoinIn! and
rIn! in AA leave nothing to send; VR10 note 8: a VID declared with its Registrar
IN goes from AN to QA, no JoinMt; VR11 a VID allocated again starts MT.

**MVRP LeaveAll with nothing pending (issue #216; 802.1Q-2018 §10.7.9, Table
10-5, §10.8.2.8 f)** — L7: an MVRP tick with nothing pending and no LeaveAll
emits nothing; with a LeaveAll it emits the LeaveAll-only MVRPDU, byte-exact
(one VID message, NumberOfValues 0, a zero FirstValue); on a tick shared with
an MSRP drain the MSRP drain goes first and the deferred LeaveAll-only MVRPDU
follows.

**Bridge** — B1: the harness plays the not-yet-landed event router, feeding
both FSMs' own re-join declarations through the encoder into byte-exact
frames end-to-end.

**One entry per attribute value (issue #214, 10 §3)** — PV1..PV20: a
second push for a value already pending rewrites its entry, so the frame
carries the value once, byte-exact against the independent packer given the
one expected message. The expected messages come from IEEE 802.1Q-2022
Table 10-3 and §10.7.6, never from the RTL: a later New or Lv replaces; a
later join keeps a pending New and otherwise replaces with the newer
Registrar reading; a later In or Mt replaces only an In or Mt (PV1-PV5). A
Listener entry takes the newer declaration type, except under an In or Mt
that did not replace (PV6). The entry keeps its place, so a value pushed
again after its successor still folds into one vector of two (PV7). A Talker
Advertise, a Talker Failed and a Listener of one StreamID stay three values
(PV8). MVRP: a VID's New and two re-join walks, and three walks over two
VIDs (PV9). PV10 offers an absent value until its search has passed entry
0, then a value that entry 0 holds, as the intake's re-arbitration does: it
merges into entry 0. PV11 merges the txLA! re-join into the `periodic!` one
inside a prepared own LeaveAll round. PV12 bridges the Domain and VLAN
participants into the encoder with two or three tx! before one drain (issue
#216: each Applicant makes one message per tx!): the Domain's New from VN and
from AN, a `periodic!` re-join and a received LeaveAll's re-join, and a VID's
New, New and its own LeaveAll's re-join each hand the value over twice or three
times, and each frame carries it once.

PV13 holds an offer of one application, with that table non-empty, from the
join tick to the end of the other application's frame, in both directions: a
search takes only its own application's read port, so the drain reads its
entries in order, and the offer merges or appends once in its own table. The
offer repeats back to back, so each search also starts clean after an accept.
PV14 and PV15 grade the whole identity: two StreamIDs that differ in one bit, each
of the 64 in turn and each Talker and Listener type, and VID 2 and a VID that
differs from it in one bit, each of the twelve in turn, stay two values. PV16: two Talker Advertise
or two Talker Failed values of one StreamID, every later field changed, carry the
newer one (802.1Q-2022 35.2.2.8). PV17: MVRP JoinIn then Lv and New then Lv
carry Lv, and Lv then New carries New (Table 10-3 Lv!, New!). PV18: an In or Mt
that replaces an In or Mt brings its own declaration type. PV19: an unknown
AttributeType of either application, offered while that table holds a value, is
taken and dropped on its first clock. PV20: an event whose search ends on the
clock its application's drain starts waits, and rides the next MRPDU; the drain
counts its entries on that clock and clears the count when it commits, so an
event taken from then to the commit would be lost. Three starts are graded, the
MSRP drain and the MVRP drain from E_IDLE and the prepared own LeaveAll round from
E_COLLECT, and the offer drops on the clock it is taken, as a producer's does, so a
push lost on any clock of the drain is missing from the next frame.

## Mutation ledger (each planted, seen to bite, then restored)

| # | Mutation (sed on the RTL) | Suite response |
|---|---|---|
| M1 | `KL_srp_encoder.sv`: AttributeListLength computed − 4 instead of − 2 (EndMark excluded) | 14 FAIL — every MSRP byte-exact check + the explicit listlen-offset check (`got 0007`) |
| M2 | `KL_srp_encoder.sv`: `ext_w` forced 0 (sorted-run detection disabled) | 12+ FAIL — E4/E5/E10/E12 lengths and NoV fields (every value emitted as its own vector) |
| M3 | `KL_srp_vlan.sv`: leave branch takes `found_v_r` alone (Lv on EVERY leave, refcount ignored) | 2 FAIL — "V6 a remaining user keeps the VID declared", "V6 still two live entries" |
| M4 | `KL_srp_domain.sv`: `surf_prio_w = rxdom_prio_i` (FirstValue equality instead of the §3 range rule) | 2 FAIL — both D6 two-class-shape checks |

Domain re-join (issue #184), through [the campaign](../srp_top/mutants.py)
(`tb/srp_top/mutations/`); each arm must fail its named check. The counts are this
suite's failing checks, measured at the issue #216 head; the four re-join arms are
re-expressed there, because the Domain's re-join became its Applicant's sJ at tx!, and
the eight Registrar arms keep their edits, regenerated where their lines moved:

| Mutation (`KL_srp_domain.sv`) | Named check | Suite response |
|---|---|---|
| `domain-rejoin-always-joinin`: every sJ JoinIn (the base's re-join) | DR1 no peer | 14 FAIL — D1, D2, D3 (two), D8, DR1, DR4, DR5, DR6, DR10, DR11, DT1, DT2, DT3 |
| `domain-rejoin-always-joinmt`: every sJ JoinMt | DR2, DR5 | 12 FAIL — D8, DR2, DR3, DR5, DR7, DR8, DR9, DR10, DA2, DA4, PV12, B1 |
| `domain-rejoin-at-request`: a received LeaveAll serves a tx! at once | DR3a | 6 FAIL — D3, D8, DR3a, DR3, DR8, PV12 |
| `domain-rx-leaveall-dropped`: rLA! never reaches the Applicant | DR3 | 5 FAIL — D3 (two), D8, DR3, PV12 |
| `domain-leaveall-keeps-in`: a LeaveAll leaves IN | DR4 | 3 FAIL — D8, DR4, DR5 |
| `domain-lv-ignored`: a received Lv leaves IN | DR6 | 1 FAIL — DR6 |
| `domain-lv-any-value`: an Lv of any value clears IN | DR7 | 1 FAIL — DR7 |
| `domain-leaveall-after-receive`: the LeaveAll wins its clock | DR8 | 1 FAIL — DR8 |
| `domain-adoption-drops-registrar`: an adoption clears the registration | DR9 | 3 FAIL — D8, DR9, DR10 |
| `domain-capture-unregistered`: the captured adoption is not registered | DR9 | 3 FAIL — D8, DR9, DR10 |
| `domain-pending-lv-ignored`: an Lv of the pending adoption is lost | DR10 | 1 FAIL — DR10 |
| `domain-link-keeps-registrar`: LINK_DOWN and LINK_UP keep IN | DR11 | 4 FAIL — DR11, DT1, DT2, DT3 |

Issue #216, the Domain's Applicant and the VID participant, through the same campaign;
each arm must fail its named check, and the counts are this suite's:

| Mutation | Named check | Suite response |
|---|---|---|
| `domain-one-new`: LINK_UP enters AN, one New (the base) | D1 four tx! | 3 FAIL — D1 (two), DR11 |
| `domain-txla-row-ignored`: the own LeaveAll's tx! takes the tx! row | DR4 own LeaveAll | 1 FAIL — DR4 |
| `domain-tx-done-at-serve`: the tx! done when served, before its message is taken | DT1 | 1 FAIL — DT1 |
| `domain-tx-owed-dropped`: a tx! that finds the queue full is dropped | DT3 | 1 FAIL — DT3 |
| `domain-applicant-deaf`: received messages never reach the Applicant | DA1 | 2 FAIL — DA1, DA3 |
| `domain-applicant-any-value`: a message of any Class A value moves the Applicant | DA5 | 1 FAIL — DA5 |
| `vlan-one-new` (`KL_srp_vlan.sv`): a first user's New! enters AN, one New (the base) | V1 four tx! | 7 FAIL — V1 (two), V5 (two), VR0, VR10, VR11 |
| `vlan-joinin-registrar-mt`: every sJ JoinIn, the Registrar MT (the base) | V3 | 13 FAIL — V1, V3, V5 (two), V11, VR0, VR2, VR3, VR4, VR6, VR8, VR11, B1 |
| `vlan-registrar-never-in`: no received value registers | VR1 | 4 FAIL — VR1, VR5, VR7, VR10 |
| `vlan-rlv-ignored`: a received Lv leaves IN | VR2 | 3 FAIL — VR2, VR3, VR4 |
| `vlan-rx-any-vid`: a received value hits every entry | VR3 | 3 FAIL — VR3, VR4, VR10 |
| `vlan-s-registers`: a received In or Mt registers | VR4 | 1 FAIL — VR4 |
| `vlan-leaveall-keeps-in`: a received LeaveAll leaves IN | VR6 | 1 FAIL — VR6 |
| `vlan-rx-loses-to-leaveall`: the LeaveAll wins its clock | VR7 | 1 FAIL — VR7 |
| `vlan-txla-keeps-in`: the own LeaveAll's tx! leaves IN | VR8 | 1 FAIL — VR8 |
| `vlan-applicant-deaf`: received values never reach the Applicant | VR9 | 5 FAIL — VR2, VR3, VR4, VR5, VR9 |
| `vlan-note8-ignores-registrar`: tx! from AN goes to AA whatever the Registrar | VR10 | 1 FAIL — VR10 |
| `vlan-registrar-kept-on-allocation`: a new entry keeps the Registrar it held | VR11 | 1 FAIL — VR11 |
| `vlan-leave-abandoned`: the last user's leave discards the entry, its Lv never sent | V6 last user out -> Lv VID 2 | 4 FAIL — V6, V9 (two), V11 |
| `vlan-rejoin-of-leaving-allocates`: a first user while the Lv is owed allocates a second entry | V10 | 7 FAIL — V10, W4, W5 (two), B1 (three) |
| `vlan-walk-frees-lo`: an entry in LO is discarded before its s | V11 | 1 FAIL — V11 |
| `mvrp-leaveall-needs-content` (`KL_srp_encoder.sv`): an MVRP drain needs a pending VID (the base) | L7 the LeaveAll-only MVRPDU is captured | 6 FAIL — L7, each of its two drains |

LeaveAll per Attribute Type (issue #106). The planted files, logs and a
restore-hash check are kept in the review packet (`mutate.py`):

| # | Property it breaks | Mutation (`KL_srp_encoder.sv`) | Suite response |
|---|---|---|---|
| M0 | all of them | the base encoder (main `fbc1f715`) against this suite | 146 FAIL — E7 and every L1 combination, including L1[15] (`1 octets differ, 4 types declared`), L2, L3, L5 |
| M5 | every registered type flagged on its first vector | `la3_w` back to "the PDU's first vector only" | 128 FAIL — E7 `the Listener message carries its own LeaveAllEvent`, L1 `type t carries LeaveAll once ... (0 flags)` |
| M6 | a LeaveAll-only vector for each undeclared type | `la_need_w = 4'b0000` (no append) | 115 FAIL — L1 `type t (not declared) has 0 LeaveAll-only vectors`, `LeaveAll adds exactly 70 octets (got 51, plain 51)`, L2 |
| M7 | the LeaveAll-only vector is NumberOfValues 0 | NoV written as 1 for it | 81 FAIL — L1 `one well-formed MRPDU, list lengths agree`, byte-exact, L2 |
| M7b | its FirstValue is present and zero | FirstValue written all-ones | 50 FAIL — L1 `type t LeaveAll-only vector: LeaveAll, NoV 0, zero FirstValue ...`, L2 |
| M8 | nothing added when every type is declared | declared types never marked seen (always-append encoder) | 128 FAIL — L1[15] `LeaveAll adds exactly 0 octets (got 228, plain 125)`, `type t (declared) has 1 LeaveAll-only vectors` |
| M9 | the MVRP LeaveAll unchanged | `LA_TYPES_MVRP_C = 4'b0000` | 3 FAIL — L4 byte-exact, `LeaveAll on the first VID vector only`, `differ by the one flag bit` |
| M10 | a mid-drain LeaveAll goes whole to the next MRPDU | consumed at commit again (the base encoder's point) | 2 FAIL — `L5 next PDU: length got 60 exp 121` + byte-exact |
| M11 | a start-cycle LeaveAll is taken once | its pending latch no longer gated by the drain start | 2 FAIL — `L6 next PDU: length got 121 exp 30` + byte-exact |
| M12 | a start-cycle LeaveAll is taken by that drain | the drain's snapshot ignores the same-cycle request | 2 FAIL — `L6 start-cycle LeaveAll: length got 30 exp 121` + byte-exact |

The issue #106 restore control was `556 checks: 556 PASS, 0 FAIL`, with
`scripts/lint_hdl.sh` clean. The current suite adds the six O8 arbitration
checks below: `562 checks: 562 PASS, 0 FAIL`.

## Notes / interpretations recorded

- F10.2 says "adopt received FirstValue"; an endpoint declares only Class A,
  so what is adopted and re-declared is the **surfaced** Class A tuple
  {6, prio_first + (6 − SRclassID_first), VID} per 10 §3 — for a NoV=1
  class-6 vector the two readings are identical.
- The F10.2 revert edge (ADOPTED to DEFAULTS) fires on LINK_DOWN: the
  class-D levels revert at LINK_DOWN (nothing is declared on a dead link);
  LINK_UP performs the default declaration.
- A pending MSRP LeaveAll on the per-application interface with an empty
  table rides the next PDU that carries attributes; the integrated MSRP round
  uses the prepared form instead, which drains a LeaveAll with nothing pending.
  A pending MVRP LeaveAll starts an MVRP drain alone, as the LeaveAll-only
  MVRPDU (issue #216, L7): the MVRP participant's LeaveAll goes out whether or
  not it declares anything (802.1Q-2018 §10.7.9). With the Domain declared
  (always, while the link is up) the MSRP LeaveAll MRPDU carries its txLA! sJ.
- The LeaveAll-only messages follow the drained ones in ascending
  AttributeType order. 802.1Q does not order Messages. This is the order the
  bench switch uses.
- The TX request face (`txreq_valid_o` held until accept + committed slot
  handle) matches the since-landed `KL_pp_tx_arbiter` requester contract
  (`req_valid_i[i]` + committed `tx_slot_i`, grant completes).

The integrated MSRP own round uses the encoder's preparation handshake:
acceptance with a reserved slot emits `sLA`, then the pending table accepts the induced
applicant responses before the join drain. O8 drives preparation concurrently
with an MVRP join tick and a second VID push. Preparation leaves the MVRP intake
open; the deferred join emits both VIDs without another tick. With VID 2 pending,
the push of VID 3 is taken on the clock after the join clock: the value search of
issue #214 reads VID 2's entry first (one clock per pending entry, 10 §3). Before
issue #214, O8 required it on the join clock itself. The
`r-mvrp-start-during-prepare` arm in the integrated mutation campaign removes
the arbitration guard and must fail O8. The other unit cases keep preparation
low to exercise the original per-application injection interface;
[srp_top](../srp_top/README.md) exercises the preparation, cancellation,
allocation/TX stalls, empty canceled reservations and full-table paths through
real shared services. A canceled empty reservation waits for content or can be
reused by a later own action; it does not emit an empty MRPDU.

## Join transmitted (issue #65, Milan v1.2 4.3.2)

W1..W5 grade the two faces the talker's streaming licence reads. W1: the
encoder's `tx_mvrp_o` strobes once per MVRP MRPDU the arbiter accepts, never for
MSRP. W2..W5: KL_srp_vlan's per-entry `vid_sent_o` (with `vid_val_o`) rises only
at a transmission after the entry's New or re-join was handed to the encoder (by
its tx! walk, issue #216); a
New still held by encoder backpressure does not count; a second user keeps it;
the last user's leave retires it; a re-join of the VID waits for its own New;
periodic re-joins never un-send an entry. Mutation-proven 2026-09-29 through
[the campaign](../srp_top/mutants.py):

| Mutation | Suite response |
|---|---|
| `tx-strobe-any-app`: the strobe fires for MSRP too | 1 FAIL — W1 |
| `join-sent-at-handover`: sent at the hand-over, not at the transmission | 2 FAIL — W2, W3 (issue #216 head; the hand-over is the walk's) |
| `count-up-unsends`: a second user resets the sent state | 2 FAIL — W4 (issue #216 head) |

## One entry per attribute value (issue #214)

Mutation-proven through [the campaign](../srp_top/mutants.py)
(`tb/srp_top/mutations/`); each arm must fail its named check, and the
counts are the failing checks of this suite:

| Mutation (`KL_srp_encoder.sv`) | Named check | Suite response |
|---|---|---|
| `second-queueing`: the search never finds the pending entry, so every push appends (the base's second queueing) | PV1 | 68 FAIL — every PV row except PV8, PV14, PV15, PV19 and PV20 (unchanged at the issue #216 head) |
| `join-displaces-new`: a later join replaces a pending New | PV2 | 4 FAIL — PV2, PV6, PV9, PV12 (one, at the issue #216 head) |
| `leave-does-not-replace`: a later Lv keeps what was pending | PV4 | 4 FAIL — PV4, PV17 (two) |
| `s-displaces-declaration`: a later In or Mt replaces anything | PV5 | 3 FAIL — PV5 (two), PV6 |
| `listener-type-oldest`: a merged Listener entry keeps its first declaration type | PV6 | 4 FAIL — PV6 (two), PV18 (two) |
| `merge-ignores-type`: the AttributeType is left out of the identity | PV8 | 2 FAIL — PV8 |
| `search-no-restart`: the search goes on when the offered event changes | PV10 | 3 FAIL — PV10 |
| `msrp-read-taken-by-any-search`: an MVRP search takes the MSRP read port too | PV13 | 3 FAIL — PV13, each search beside an MSRP drain |
| `mvrp-read-taken-by-any-search`: an MSRP search takes the MVRP read port too | PV13 | 2 FAIL — PV13, each search beside an MVRP drain |
| `search-survives-accept`: the search goes on after its accept, so a value offered again on the next clock starts past entry 0 | PV13 | 8 FAIL — PV13 |
| `identity-drops-top-octet`: the StreamID's top octet is left out of the identity | PV14 | 48 FAIL — PV14, every bit of octet 0 of each type |
| `identity-vid-low-octet-only`: only the VID's low octet is the identity | PV15 | 8 FAIL — PV15, bits 8 to 11 |
| `merge-keeps-older-value`: a merged MSRP entry keeps its first FirstValue | PV16 | 2 FAIL — PV16 |
| `mvrp-merge-never-written`: an MVRP merge writes the append address, so the pending entry never changes | PV17 | 3 FAIL — PV17, each row |
| `mvrp-lv-keeps-joinin`: an MVRP Lv over a JoinIn keeps the JoinIn | PV17 | 1 FAIL — JoinIn then Lv |
| `mvrp-lv-keeps-new`: an MVRP Lv over a New keeps the New | PV17 | 1 FAIL — New then Lv |
| `mvrp-new-keeps-lv`: an MVRP New over an Lv keeps the Lv | PV17 | 1 FAIL — Lv then New |
| `listener-type-s-over-s-kept`: an In or Mt that replaces keeps the older declaration type | PV18 | 2 FAIL — PV18 |
| `unknown-type-held-while-pending`: an unknown type waits for the table to empty | PV19 | 3 FAIL — PV19 |
| `push-on-drain-start`: an event is taken on the clock its drain starts, outside its count, and is lost | PV20 | 10 FAIL — PV20, all three starts |
| `push-on-prepared-start`: only the prepared own LeaveAll round's term of the guard is gone, so an event taken on the clock that round starts is lost | PV20 | 4 FAIL — PV20, the prepared round |
| `push-taken-in-alloc`: a drain does not own its table while it waits for its slot, so an event taken then is lost | PV20 | 4 FAIL — PV20, both applications |

Against the base RTL this suite reads `1604 checks: 1519 PASS, 85 FAIL`: every PV
row except PV8, PV14, PV15 and PV19 (the base appends each push, so each frame
carries the value twice or more, 7 octets more per Domain vector and 5 per VID;
PV20 fails because the base takes the offer before the start clock, into the drain
it starts, which leaves no next frame), and O8, whose VID 3 the base takes on the
join clock.
