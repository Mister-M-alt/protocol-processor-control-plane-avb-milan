<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# acmp_talker — KL_acmp_talker suite

Proves the ACMP stateless talker responder + per-source DA-gate
(`hdl/acmp/KL_acmp_talker.sv`) against
[05 §6bis](../../docs/architecture/05_acmp_engine.md) (F05.11 decision tree +
F05.12 DA-gate) and the 08 §2/§5 timer contract: `make` = build + run, exit 0 =
PASS, 480 checks. `make lint` runs the repo's zero-warning gate (no width
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

Known limit: the harness drives `now_ms_i` directly and injects expiries, so
it cannot catch a prescaler-level defect — that is `tb/timer_service`'s job.

Mutation-proven 2026-08-11 (backup/sed/run/restore):
- M1 PROBE flag law inverted (RF ored live into the PROBE response, the
  pipewire bug): fails 1 of 480 (`B4 flags got 004a want 000a`).
- M2 backoff halved (`now + draw` instead of `now + 2*draw`): fails 4 of 480
  (E1/F2 leaveall2 arm deadlines).
- M3 GET_TX_STATE RF no longer live (forced 0): fails 2 of 480 (B3, B5).
