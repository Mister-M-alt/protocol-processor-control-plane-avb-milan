<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# srp_decoder — KL_srp_decoder MRPDU vector-decode suite

Proves the SRP MRPDU vector decoder (`hdl/srp/KL_srp_decoder.sv`) against
[10 §3](../../docs/architecture/10_srp_engine.md) (F10.6/F10.7/F10.8, the
vector-value-k paragraph, the dual-EndMark framing rule and the Milan
§4.2.7.1.2 tolerance rules, tested per F09.4) and the per-type LeaveAll rule
of 10 §6.5: `make` = build + run, exit 0 = PASS, 150 checks.

Every MRPDU is hand-built **byte-exact** in the harness and every expected
event is an explicit hand-computed constant — the C++ never re-implements
the decoder. The stream is driven through the validator's rule-V9
pass-through face (`mrp_valid/data/last` + MSRP/MVRP select), honoring
`mrp_ready_o` (the decoder paces packed-byte drains at one event per cycle).

Covered:

- **THE +k regression pin**: the certified-bench Domain shape — 802.1Q
  §35.2.2.9's own worked example, `NumberOfValues = 2` from FirstValue
  `{SRclassID 5, prio 2, VID}` — class A surfaced as value 1 with priority
  3 and `evt_class_a_o` set, VID unchanged.
- Multi-value Talker Advertise range reconstruction: {unique_id, DA}
  increment together per value, TSpec/PCP/rank/accumulated_latency ride
  every value; Talker Failed FailureInformation (system id + code 1).
- Listener pairing through the sync-read three-packed RAM: all four
  four-packed declaration codes paired with their three-packed events,
  stream_id +k, no DA.
- MVRP VID decode (no AttributeListLength field), VID +k per value.
- **Dual-EndMark framing truth**: a PDU ending after the AttributeList
  EndMark alone is malformed — the already-emitted prefix stands.
- **AttributeListLength is counted, never trusted**: a lying declared
  length is flagged (`listlen_bad_o`, counted value 9 exposed) while
  EndMark framing carries the walk into the next message; PDU still ok.
- Tolerance per F09.4: truncation mid-FirstValue (no events, malformed),
  truncation mid-vector (3-value prefix emitted, malformed), bad
  AttributeLength (nothing emitted, next PDU decodes cleanly), a bad
  attribute mid-PDU (good first message emitted, fully-valid third message
  proven discarded), out-of-alphabet three-packed digit (> 215).
- **Per-application LeaveAll**: an MSRP LeaveAll never strobes `la_mvrp_o`
  and vice versa; LeaveAll with `NumberOfValues = 0` consumes its FirstValue
  and emits no events.
- **Per-Attribute-Type LeaveAll lanes** (10 §6.5, 802.1Q-2014 §10.7.5.20
  NOTE and b)2), §10.8.2.6): `la_msrp_o[type − 1]`, each lane once per
  MRPDU, at the first VectorHeader of its type that carries LeaveAllEvent.
  The harness records every lane strobe and every value event in one
  timeline and checks it in full:
  - **P — the bench switch's own LeaveAll MRPDU**, transcribed byte for
    byte from the Run B capture (`tap-runB.pcap`, switch port, 47.029619 s,
    FCS dropped, 109 B): `[Listener LA JoinMt/Ready] [Domain LA n=2]
    [TalkerAdvertise LA n=0] [TalkerFailed LA n=0]` gives exactly
    `L3 E3 L4 E4 E4 L1 L2` — each lane once, ahead of its own type's events,
    and nothing after the Listener re-declaration reaches the Listener lane.
  - **Q — Domain-only LeaveAll** behind a Listener re-declaration: the
    Domain lane alone; the Listener and talker lanes never fire.
  - **R — Listener-only LeaveAll** (NumberOfValues 0, zero FirstValue,
    list = AttributeLength + 4): the Listener lane alone, no events.
  - **S — once per MRPDU**: two flagged Listener vectors plus a flagged
    second Listener message fire the lane once, ahead of all three
    re-declarations; the next MRPDU fires it again; MVRP likewise.
- Explicit-EndMark-then-padding (min-frame pad bytes inert, one done).

Mutation-proven 2026-08-11 (backup/sed/run/restore):

| Mutation | Result |
|---|---|
| Domain `SRclassPriority` +k increment removed | 2 of 113 FAIL (A "+k prio 3", F prefix) |
| FourPacked extraction order flipped (MS-first → LS-first) | 5 of 113 FAIL (D/G/J/O declaration codes) |
| Talker/Listener DA +k increment removed | 4 of 113 FAIL (B DA range checks) |

All three restored; suite back to 113/113 PASS.

Receive-side LeaveAll routing, mutation-proven 2026-09-23 (issue #106; each
arm planted, run, restored under a SHA-256 check):

| Mutation | Result |
|---|---|
| The previous decoder (one application-wide strobe per flagged VectorHeader) | 19 of 150 FAIL (L, P, Q, R, S) |
| Every lane strobed at every flagged VectorHeader | 21 of 150 FAIL (L, P, Q, R, S) |
| Once-per-MRPDU gate removed | 8 of 150 FAIL (S, both MRPDUs and MVRP) |
| Gate never re-armed at the next MRPDU | 18 of 150 FAIL (P, Q, R, S) |
| Lane = AttributeType instead of AttributeType − 1 | 11 of 150 FAIL (L, Q, R, S; P by strobe order alone) |
