<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# acmp_talker — KL_acmp_talker suite

Proves the ACMP stateless talker responder + per-source DA-gate
(`hdl/acmp/KL_acmp_talker.sv`) against
[05 §6bis](../../docs/architecture/05_acmp_engine.md) (F05.11 decision tree +
F05.12 DA-gate) and the 08 §2/§5 timer contract: `make` = build + run, exit 0 =
PASS, 631 checks. `make lint` runs the repo's zero-warning gate (no width
waivers).

The C++ harness is an independent model, never DUT logic: every expected
response is spelled out field-by-field from the F05.11 tables; the harness
plays the RX-slot RAM (sync read + committed length), the maap face
(single-outstanding ALLOC/RELEASE + conflict events), the PRNG (it supplies
the T-MRP-LEAVEALL draw, so the 2x LEAVEALL2 deadline is checked exactly), and
the timer face (arms are checked for slot/owner/absolute-deadline; expiries
are injected, which is also how the backoff is time-compressed).

Covered: boot walk (8 sources allocate DAs in order, nothing declares
unprobed); PROBE_TX success with every field checked incl. the flag law
(FAST_CONNECT+STREAMING_WAIT echoed, junk bits masked, REGISTERING_FAILED
FORCED 0 — the trap the pipewire reference inverted); GET_TX_STATE with
listener fields zeroed and REGISTERING_FAILED read LIVE from the srp face
(`ASKING_FAILED`) — the two tables checked back-to-back on the same source so
their deliberate difference is the check; TALKER_UNKNOWN_ID both verbs;
silently-ignored wrong-interface probe (retired + slot freed, no ping);
DISCONNECT_TX always-SUCCESS no-op; GET_TX_CONNECTION NOT_SUPPORTED; the V3
truncated-PDU rule (flags beyond a 44-byte PDU read as 0); freshness expiry
(withdraw only once fresh AND listener are both gone; DA kept); the MAAP
conflict flow (withdraw -> kind-3 draw -> arm now+2xdraw -> DEST_MAC_FAILED
while backed off -> re-alloc -> re-declare with the NEW DA); the PCP-change
flow (same backoff but the DA is KEPT: probe during backoff still answers
SUCCESS, re-declare with the SAME DA); the compressed backoff exit that
re-arms DAFRESH to the ABSOLUTE remaining window; conflict while DA_OK
(re-alloc, no backoff — doc-gap decision recorded in the RTL banner); source
disable (timer cancel + RELEASE_DA + unknown-id afterwards); per-source
independence (src2's answer unchanged by src0/1/3 churn); and the stateless
property twice (identical query around interleaved traffic = byte-identical
response, compared as whole structs).

Two things the port `declaring_o` and the maap face own, checked on the PORTS:

- **The gate IS the grant.** `declaring_o` is sampled every cycle and its
  EDGES are logged, so the level is proven MOVING (0 -> 1 at B1/I4/J7, 1 -> 0
  at D2/J1) rather than read once at its reset value. A REFUSED `ALLOC_DA`
  leaves it shut even with a Listener registered — i.e. the refusal, not the
  gate predicate, is what holds it — and the following grant opens it.
- **An absent maap DEGRADES, it does not wedge (section J).** With
  `maap_req_ready_i` tied 0 the request is offered for exactly
  P-MAAP-ACCEPT-CYC = 1024 cycles and then abandoned; the source lands in the
  refused-alloc state (PROBE_TX answers TALKER_DEST_MAC_FAILED, still pinging
  freshness), every following command is still consumed and answered, each
  new stimulus re-offers, and when maap returns the source recovers to
  DECLARING. This is the regression test for the deadlock the single walker
  had: S_EV_MAAP was the only state with no exit but ready, and the same
  walker serves every source and every talker command.
- **An UNANSWERED accept must not strand every source (section K).** The
  other half of the face, and the quieter failure: `maap_busy_r` is a single
  GLOBAL tracker, so one request the allocator accepts and never answers
  stops allocation for *every* source — nothing reaches `GS_DA_OK`, no DA
  gate opens, and there is no SRP `DECLARE_TALKER` either. It never wedges:
  transaction dispatch outranks the pending-init flag, so PROBE_TX and
  GET_TX_STATE keep answering and every liveness signal stays healthy while
  no stream can start. Section K accepts src 6's `ALLOC_DA` and goes quiet,
  then proves in order: src 2 cannot even OFFER a request (K2 — the defect);
  the waiting source still answers TALKER_DEST_MAC_FAILED and arms freshness
  (K3); nothing is re-offered one millisecond before P-MAAP-RSP-MS (K4); at
  the bound src 2 allocates again (K5 — the regression); src 6's LATE
  response, carrying a poison address, is swallowed and installs nothing on
  the source that is now tracked (K6); src 2's own response then completes
  normally with its own DA (K7); and the abandoned source recovers on its
  next stimulus with a fresh DA, never the poison one (K8).

Known limits: the harness drives `now_ms_i` directly and injects expiries, so
it cannot catch a prescaler-level defect — that is `tb/timer_service`'s job.
And P-MAAP-RSP-MS is checked as a *boundary* (nothing at bound-1, abandon at
bound); the suite pins the number the RTL declares but cannot prove that
number is the right one — that argument is the IEEE 1722-2016 Annex B
derivation at `MAAP_RSP_MS_P`, and only a real MAAP shim on silicon can
falsify it.

Mutation-proven 2026-08-11 (backup/sed/run/restore):
- M1 PROBE flag law inverted (RF ored live into the PROBE response, the
  pipewire bug): fails 1 of 480 (`B4 flags got 004a want 000a`).
- M2 backoff halved (`now + draw` instead of `now + 2*draw`): fails 4 of 480
  (E1/F2 leaveall2 arm deadlines).
- M3 GET_TX_STATE RF no longer live (forced 0): fails 2 of 480 (B3, B5).

Mutation-proven 2026-08-12 (the maap-degrade round):
- M4 the S_EV_MAAP timeout exit removed (`maap_req_ready_i || maap_tmo_w` ->
  `maap_req_ready_i`, i.e. the deadlock restored): fails 27 (the whole J
  section — the walker never consumes another command).
- M6 the ALLOC refusal ignored (`!maap_rel_r && maap_rsp_ok_i` ->
  `!maap_rel_r`, a refusal treated as a grant of DA 0): fails 10 (I3/I4/J7 —
  the gate opens on a refusal and every later DA is off by one grant).

Mutation-proven 2026-08-12 (the unanswered-accept round, 723 checks):
- M7 the response bound never fires (`maap_rsp_tmo_w` -> `1'b0`, i.e. the
  defect restored): fails 11 (K5 onward — allocation never resumes, and the
  late response is then taken as live).
- M8 the stale-credit swallow removed (`maap_swallow_w` -> `1'b0`): fails 6
  — and the failure text is the hazard itself, `dest_mac got deadbeefcafe`:
  the abandoned request's answer installs its address on a DIFFERENT source.
- M9 the bound shortened to 5 s: fails 1 (K4 — the request is abandoned
  before the window the Annex B walk needs).
