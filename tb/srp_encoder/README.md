<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# srp_encoder — KL_srp_encoder + KL_srp_domain + KL_srp_vlan suite

Exit 0 = PASS. `make` builds `srp_tb_wrap` (encoder wired to a **real**
`KL_pp_tx_slots` pool; Domain and VLAN singletons stand-alone) and runs
556 checks against an **independent** C++ packer written from 802.1Q
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
priority 3, VID 2} at startup/LINK_UP (New); adopt a differing received
Class A Domain as withdraw-old (Lv) + re-declare (New) + one DOMAIN_CHANGE
strobe + class-D levels; identical parameters are no change; the certified
two-class bridge shape (FirstValue {5, 2, VID}, NoV=2) surfaces Class A as
value 1 with priority 3 by the §3 range rule; non-covering vectors ignored;
periodic/LeaveAll re-join the ADOPTED declaration and never revert; revert
happens on LINK_DOWN only, LINK_UP re-declares the defaults. Domain TX is
independent of gPTP state structurally — the module has no gPTP port.

**VLAN (10 §6.2, corrected F10.3)** — V1..V9: per-VID refcount keyed by
each user's OWN stream VID: two users of one VID produce exactly one join
(New, then JoinIn on the cadence); a Domain VID change moves nothing — a
new user brings the new VID and two VIDs are briefly live while the old one
stays frozen until its LAST user leaves (Lv); LeaveAll/periodic re-join
every VID with users; unknown-VID leave and table overflow are error
strobes, never wire events.

**Bridge** — B1: the harness plays the not-yet-landed event router, feeding
both FSMs' own re-join declarations through the encoder into byte-exact
frames end-to-end.

## Mutation ledger (each planted, seen to bite, then restored)

| # | Mutation (sed on the RTL) | Suite response |
|---|---|---|
| M1 | `KL_srp_encoder.sv`: AttributeListLength computed − 4 instead of − 2 (EndMark excluded) | 14 FAIL — every MSRP byte-exact check + the explicit listlen-offset check (`got 0007`) |
| M2 | `KL_srp_encoder.sv`: `ext_w` forced 0 (sorted-run detection disabled) | 12+ FAIL — E4/E5/E10/E12 lengths and NoV fields (every value emitted as its own vector) |
| M3 | `KL_srp_vlan.sv`: leave branch takes `found_v_r` alone (Lv on EVERY leave, refcount ignored) | 2 FAIL — "V6 a remaining user keeps the VID declared", "V6 still two live entries" |
| M4 | `KL_srp_domain.sv`: `surf_prio_w = rxdom_prio_i` (FirstValue equality instead of the §3 range rule) | 2 FAIL — both D6 two-class-shape checks |

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

Restored tree: `556 checks: 556 PASS, 0 FAIL`, `scripts/lint_hdl.sh` clean.

## Notes / interpretations recorded

- F10.2 says "adopt received FirstValue"; an endpoint declares only Class A,
  so what is adopted and re-declared is the **surfaced** Class A tuple
  {6, prio_first + (6 − SRclassID_first), VID} per 10 §3 — for a NoV=1
  class-6 vector the two readings are identical.
- The F10.2 revert edge (ADOPTED to DEFAULTS) fires on LINK_DOWN: the
  class-D levels revert at LINK_DOWN (nothing is declared on a dead link);
  LINK_UP performs the default declaration.
- A pending LeaveAll with an empty table rides the next PDU that carries
  attributes; Domain and VLAN re-declare on the LeaveAll cycle, so that PDU
  is the immediate next one in practice. With the Domain declared (always,
  while the link is up) the MSRP LeaveAll MRPDU therefore never consists of
  LeaveAll-only vectors alone.
- The LeaveAll-only messages follow the drained ones in ascending
  AttributeType order. 802.1Q does not order Messages. This is the order the
  bench switch uses.
- The TX request face (`txreq_valid_o` held until accept + committed slot
  handle) matches the since-landed `KL_pp_tx_arbiter` requester contract
  (`req_valid_i[i]` + committed `tx_slot_i`, grant completes).
