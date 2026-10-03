<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# acmp_talker — KL_acmp_talker suite

Proves the ACMP stateless talker responder + per-source DA-gate
(`hdl/acmp/KL_acmp_talker.sv`) against
[05 §6bis](../../docs/architecture/05_acmp_engine.md) (F05.11 decision tree +
F05.12 DA-gate) and the 08 §2/§5 timer contract: `make` = build + run, exit 0 =
PASS, 1342 checks. `make lint` runs the repo's zero-warning gate (no width
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
their deliberate difference is the check — and clear for a registered Ready
Failed (B6) and Ready (B7); TALKER_UNKNOWN_ID both verbs;
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

The harness drives `srp_lsn_reg_state_i` in the SRP engine's own code,
`srp_pkg::srp_decl_e` (1 Asking Failed, 2 Ready, 3 Ready Failed,
[02 F02.10](../../docs/architecture/02_interfaces.md#fig-02-statusdict)), written
down as the contract rather than read from the package. So this suite and
`pp_top`'s section T, where the code comes from a real MRPDU through the SRP
engine, grade the same code, and a package that moves a code fails both.

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
  freshness), every following command is still consumed and answered, a later retry round re-offers, and when maap returns the source recovers to
  DECLARING. This is the regression test for the deadlock the single walker
  had: S_EV_MAAP was the only state with no exit but ready, and the same
  walker serves every source and every talker command.
- **An UNANSWERED accept must not strand every source (section K).** The
  other half of the face, and the quieter failure: `maap_busy_r` is a single
  GLOBAL tracker, so one request the allocator accepts and never answers
  stops allocation for *every* source — nothing reaches `GS_DA_OK`, no DA
  gate opens, and there is no SRP `DECLARE_TALKER` either. It never wedges:
  commands alternate with pending events, so PROBE_TX and
  GET_TX_STATE keep answering and every liveness signal stays healthy while
  no stream can start. Section K accepts src 6's `ALLOC_DA` and goes quiet,
  then proves in order: src 2 cannot even OFFER a request (K2 — the defect);
  the waiting source still answers TALKER_DEST_MAC_FAILED and arms freshness
  (K3); nothing is re-offered one millisecond before P-MAAP-RSP-MS (K4); at
  the bound src 2 allocates again (K5 — the regression); src 6's LATE
  response, carrying a poison address, is swallowed and installs nothing on
  the source that is now tracked (K6); src 2's own response then completes
  normally with its own DA (K7); and the abandoned source recovers on a
  paced retry with a fresh DA, never the poison one (K8).
- **A RELEASE_DA is OWED, never attempted (section L).** An `ALLOC_DA` that
  cannot reach the face is safe to drop, because the stimulus that asked for
  it comes again; a `RELEASE_DA` has no such stimulus, and `EVC_OFF` wipes the
  record naming the address in the same cycle it asks. So a release skipped
  because the single-outstanding face happened to be busy left the address
  allocated forever with nothing left to notice, and BUSY is the normal
  state for seconds at a time, because an `ALLOC_DA` maps onto a real MAAP
  claim walk (IEEE Std 1722-2016 Table B.7 driven by Table B.8: three probe
  intervals of up to 600 ms each per attempt, B.3.4.2). Annex B permits the
  delay (B.3.5.2 attaches no deadline to `Release!` and the machine sits
  legally in DEFEND until it arrives) but not the loss: footnote c to
  Table B.7 makes the range free only once `Release!` has reached INITIAL.
  Section L parks the face on an unanswered claim walk and proves, in order:
  the teardown itself is never delayed by it (WITHDRAW_TALKER, timer cancel
  and the TALKER_UNKNOWN_ID answer all land at once, L1); nothing is even
  offered while the face is busy (L1); the release survives the busy window
  and goes out when the face frees (L2); it is offered exactly once and a
  taken release is never re-offered (L2/L3); and three teardowns behind one
  busy face yield three releases, lowest index first, and no fourth (L4).
  K0 removes two sources in the SAME cycle, which is the race in its
  shortest form, and J6/J7 cover the other drop path: a release abandoned at
  P-MAAP-ACCEPT-CYC is re-offered by the engine (nothing else would), and
  when the allocator returns the owed release is taken BEFORE the rejoined
  source's new allocation.

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

Mutation-proven 2026-08-13 (the owed-release round, 807 checks):
- M10 the release is never BOOKED (`ev_relset_w = rec_w.da_valid` -> `1'b0`,
  i.e. the recorded defect restored): fails 38 (K0 and all of L: the second
  and third teardowns behind a busy face hand nothing back).
- M11 the debt discharged on the OFFER instead of the ACCEPT
  (`maap_accept_w && mreq_rel_r` -> `(state_r == S_EV_MAAP) && mreq_rel_r`):
  fails 50. The P-MAAP-ACCEPT-CYC abandon silently eats the release again.
- M12 `EVC_REL` demoted below `EVC_INIT` in the dispatcher: fails 47 (J7 and
  L4: a source that leaves and rejoins releases the address its rejoin was
  just granted).

Mutation-proven 2026-09-19 (the lstn_reg_state encoding round, 839 checks,
issue #46; run in a copy of the tree, the tracked files never edited):
- M13 the retired private code restored: `rf_live_w` compares
  `srp_pkg::srp_decl_e'(2'd3)`, the pre-fix `LSN_ASKING_FAILED_C`. Fails 3:
  B3 (`flags got 0000 want 0040`), B5, and B6 (`flags got 0040 want 0000`),
  the Ready Failed arm that did not exist before this round.
- M14 `srp_pkg::srp_decl_e` swaps ASKING_FAILED and READY_FAILED (1 and 3):
  fails the same 3, because the harness writes the codes as the contract and
  never reads the package. The same mutation fails `pp_top` (5), and
  `srp_stream_fsms` (4) and `srp_top` (1) through the SRP engine's own
  readers of the package.

## Refused startup allocation recovery (issue #128)

Section R adds 503 checks across seventeen independent fresh boots/scenario groups.
The allocation BFM can supply a stable base-plus-source block, refuse out-of-range
sources, refuse all requests, withhold ready or delay responses. All assertions
observe ports; no internal state is forced. The parent adapter remains a separate
integration regression in the consumer lane.

| Case | Contract |
|---|---|
| R1 | All eight startup refusals recover automatically at 100 ms, never before; first probes succeed with the correct tuple; acquisition alone creates no declaration or timer arm; freshness still expires |
| R2 | Repeated probes and listener changes share one attempt/source/round; disabled sources stay quiet, including listener events; every source recovers on re-enable |
| R3 | Absent ready and continuously presented commands: every source gets a turn, each offer ends at 1024 cycles, response gap stays within 1088 cycles |
| R4 | Successful grants invalidated by disable/re-enable or conflict are never installed; their releases precede fresh allocation |
| R5 | Response timeout and time wrap, stale response swallowed before the retry response, silent-accept capacity saturates and resumes safely after draining |
| R6 | Retry rounds leave conflict and PCP backoff/timer arms untouched; only conflict reallocates, while PCP retains its address |
| R7 | Count growth, shrink and block move: every source's mapping is checked; out-of-block probes fail without a retry storm |
| R8 | Retry deadline on both sides of the 32-bit millisecond wrap |
| R9 | Six consecutive probes meet the command bound while an accepted allocation is silent and another source has INIT pending |
| R10 | DA_OK conflict and refused-source disable/re-enable restart allocation inside the current round |
| R11 | Continuous commands do not starve source disable/withdrawal or immediate/owed releases |
| R12 | Probe, listener change and full backoff expiry each allocate between retry ticks; no round can mask a missing demand arc |
| R13 | Disable and conflict swept over the response edge and following three edges never publish an obsolete grant and always release it |
| R14 | Cancellation during allocation dispatch/read or at its action edge suppresses an obsolete request |
| R15 | Re-enable at the release offer acquires immediately; release does not charge the allocation pacing bit |
| R16 | Conflict, PCP, freshness and listener events act with GET_TX_STATE continuously valid on another source; gate state is sampled before commands stop, and responses keep flowing |
| R17 | A never-ready allocator gets one offer per source per round; three repeated probes per source cannot re-offer inside either of two rounds, and the next round visits all sources |

The acquisition bound and its assumptions are in
[05 §6bis](../../docs/architecture/05_acmp_engine.md#6bis-talker-side-stateless-responder).
R3 deliberately compresses a millisecond into a clock so an unaccepted request
spans several retry rounds and exercises rotating arbitration under saturation.
R1 and R8 hold time fixed to prove the exact pacing boundary. R17 holds time
fixed throughout each probe group and counts offers rather than accepted requests,
so an abandoned offer cannot escape the same-round limit.

Run the new mutations with:

```sh
python3 tb/acmp_talker/retry_mutants.py --logs /tmp/acmp-retry-mutants [--jobs N] [--only NAME ...]
```

The runner copies only build inputs into a temporary directory of each case's
own (the baseline, every mutant and a final unmutated run, `restored`), requires
a simulation tally (a compiler error cannot kill a mutant), checks nonzero exits
and named assertion failures, and requires the baseline and `restored` to pass
with rc 0. The baseline runs first; `--jobs N` (default 4, the meaning and
default of `tb/pp_top/d3_mutants.py`) then runs up to N cases at once, and the
results are printed in the table's order whatever order they finish in.
Measured 2026-10-02 at `85da751` with Verilator 5.050, each run pinned to 4 of
the host's 16 CPUs: `--jobs 1` took 408 s and `--jobs 8` 248 s. The baseline,
every mutant and `restored` gave the same verdict and the same failures in both
runs, and `coverage.txt` was identical.
It records a failing witness by file and line for every new assertion site in
`coverage.txt`. The complete table includes both review mutant sets. Equivalent single-term
variants are controls and never count as killed witnesses; their construction
arguments are recorded in `EQUIVALENT_MUTATIONS`. `PERFORMANCE_MUTATIONS`
records the enable mask that avoids no-op walker visits: it preserves the
contract but can change cycle traces, so it is not an equivalence claim.
The redundant enable-edge
clear was removed, with its reason in `REMOVED_EQUIVALENTS`. Simulation verdicts
use DUT cycles only, with no host-time deadline. Final measured results follow.

Measured on the expanded suite: 62 defect mutants killed, seven equivalent
controls retained baseline behavior, and one performance control retained the
contract bounds; baseline and restored runs pass 1342 checks.
All 59 assertion sites in `retry_cases.hpp` have a killed witness. Counts below
include named failures anywhere in the committed suite; `coverage.txt` separately
requires a witness for each retry assertion site.

| Mutant | Result | Named failing assertion |
|---|---|---|
| `no_round` | killed, 22 failures | R1 automatic bounded acquisition |
| `early_round` | killed, 2 failures | R1 no retry before 100 ms |
| `late_round` | killed, 22 failures | R1 automatic bounded acquisition |
| `retry_wrap` | killed, 2 failures | R8 wrap retry bound minus one |
| `no_pacing` | killed, 40 failures | R2 demand cannot bypass retry pacing |
| `fixed_priority` | killed, 1 failures | R3 every source gets an attempt under continuous commands |
| `command_monopoly` | killed, 9 failures | R3 every source gets an attempt under continuous commands |
| `retry_monopoly` | killed, 60 failures | R3 absent allocator src0 consumed |
| `disabled_alloc` | killed, 2 failures | R2 disabled listener event cannot allocate |
| `no_accept_bound` | killed, 197 failures | R3 every source gets an attempt under continuous commands |
| `short_accept_bound` | killed, 2 failures | R3 absent request respects accept bound |
| `refusal_is_grant` | killed, 62 failures | R1 automatic bounded acquisition |
| `obsolete_grant` | killed, 19 failures | R4 obsolete grant rejected src0 status/tuple |
| `obsolete_release_lost` | killed, 11 failures | R4 obsolete successful allocation released before retry |
| `no_response_bound` | killed, 16 failures | R5 timeout retries automatically across wrap |
| `response_wrap` | killed, 1 failures | R5 no premature response timeout across wrap |
| `no_stale_swallow` | killed, 9 failures | R5 stale timeout response src0 status/tuple |
| `no_stale_capacity` | killed, 2 failures | R5 silent accepts stop at stale-credit capacity |
| `no_stale_drain` | killed, 16 failures | R5 retry response src0 status/tuple |
| `backoff_bypass` | killed, 2 failures | R6 retries leave backoff and timer untouched |
| `reallocate_owned` | killed, 119 failures | R1 owned addresses are never reallocated |
| `half_backoff` | killed, 6 failures | R6 full two-LeaveAll backoff armed |
| `fresh_forever` | killed, 94 failures | R1 freshness expiry still withdraws every source |
| `declare_without_demand` | killed, 158 failures | R1 acquired addresses neither declare nor borrow timer slots |
| `source_alias` | killed, 172 failures | R1 first probe after bound src0 status/tuple |
| `gate_without_ownership` | killed, 56 failures | R1 acquired addresses neither declare nor borrow timer slots |
| `no_requests` | killed, 215 failures | R1 one refused startup attempt per source |
| `no_command_ready` | killed, 693 failures | R1 first probe after bound src0 consumed |
| `busy_tracker_blocks_commands` | killed, 62 failures | R4 obsolete grant rejected src0 consumed |
| `no_conflict_wait_clear` | killed, 3 failures | R10 conflict immediately restarts acquisition |
| `wait_on_release` | killed, 48 failures | R15 release does not consume re-enabled lifetime allocation attempt |
| `tick_no_rearm` | killed, 43 failures | R2 one attempt per enabled source per round |
| `no_rotate_advance` | killed, 1 failures | R3 every source gets an attempt under continuous commands |
| `no_sticky_gp_window` | equivalent control, rc 0 | See construction argument below |
| `grant_kill_reg_only` | killed, 3 failures | R13 cancellation kind0 edge3 never publishes obsolete grant |
| `accept_kill_zero` | equivalent control, rc 0 | See construction argument below |
| `kill_no_pending_conflict` | equivalent control, rc 0 | See construction argument below |
| `kill_no_live_conflict` | killed, 2 failures | R13 cancellation kind0 edge3 never publishes obsolete grant |
| `kill_no_disable` | killed, 1 failures | R13 cancellation kind1 edge3 never publishes obsolete grant |
| `init_elig_no_avail` | killed, 15 failures | R9 consecutive command during silent allocation src1 consumed |
| `no_turn_restore` | killed, 57 failures | R3 commands never starve behind retries (gap=0) |
| `elig_drop_rel` | killed, 1 failures | R11 owed release serviced under continuous commands |
| `elig_drop_off` | killed, 4 failures | R11 disable withdraws under continuous commands |
| `ready_ignores_turn` | killed, 89 failures | B4 re-ping: timer arm missing |
| `init_ignores_off_conflict` | killed, 2 failures | R14 cancellation kind0 prevents obsolete allocation offer |
| `retry_period_200` | killed, 22 failures | R1 automatic bounded acquisition |
| `tick_ge_to_gt` | killed, 22 failures | R1 automatic bounded acquisition |
| `kill_no_conflict_at_all` | killed, 10 failures | R4 obsolete grant rejected src0 status/tuple |
| `kill_no_disable_at_all` | killed, 9 failures | R4 obsolete grant rejected src0 status/tuple |
| `kill_sticky_off` | killed, 10 failures | R4 obsolete grant rejected src0 status/tuple |
| `no_probe_initset` | killed, 1 failures | R12 demand arc 0 allocates inside round |
| `no_lsn_initset` | killed, 1 failures | R12 demand arc 1 allocates inside round |
| `no_conflict_daok_initset` | killed, 39 failures | R10 conflict immediately restarts acquisition |
| `no_backoff_exit_initset` | killed, 1 failures | R12 demand arc 2 allocates inside round |
| `no_enable_wait_clear` | killed, 2 failures | R10 re-enable immediately restarts acquisition |
| `eligible_only_init` | killed, 8 failures | R11 disable withdraws under continuous commands |
| `accept_kill_no_disable` | equivalent control, rc 0 | See construction argument below |
| `init_no_enable_check` | killed, 1 failures | R14 cancellation kind2 prevents obsolete allocation offer |
| `eligible_ignores_init` | killed, 1 failures | R3 every source gets an attempt under continuous commands |
| `tick_keeps_wait` | killed, 33 failures | R1 automatic bounded acquisition |
| `no_get_tx_state_response` | killed, 131 failures | R16 conflict commands keep flowing (0) |
| `elig_drop_conflict` | killed, 1 failures | R16 conflict event served under continuous commands (gates 0x02 want 0x00) |
| `elig_drop_pcp` | killed, 1 failures | R16 pcp event served under continuous commands (gates 0x02 want 0x00) |
| `elig_drop_tmr` | killed, 1 failures | R16 freshness event served under continuous commands (gates 0x02 want 0x00) |
| `elig_drop_lsn` | killed, 1 failures | R16 listener event served under continuous commands (gates 0x02 want 0x06) |
| `init_busy_else_removed` | equivalent control, rc 0 | See construction argument below |
| `kill_w_no_off` | equivalent control, rc 0 | See construction argument below |
| `init_ready_no_en` | performance control, rc 0 | Action-side enable guard retains the bounds |
| `wait_only_on_accept` | killed, 4 failures | R17 absent same-round probes cannot re-offer |
| `sticky_kill_ignores_accept` | equivalent control, rc 0 | See construction argument below |

Equivalent controls are checked for a clean baseline result, not counted as kills.

- `no_sticky_gp_window`: Until GRANT consumes gp_valid, OFF/CONFLICT cannot dispatch; their pending bits retain every cancellation through the grant action.
- `accept_kill_zero`: Cancellation on accept is pending on the next busy edge; a response cannot be consumed as a grant before that edge latches the kill.
- `accept_kill_no_disable`: An accept-edge disable sets OFF, which persists and latches the kill on the next busy edge before grant consumption.
- `kill_no_pending_conflict`: A conflict after accept latches the live kill; an older pending conflict is captured by the accept-side kill expression.
- `no_reenable_wait_clear`: Removed !en_q_r: reset clears wait and each disabled edge already clears it through !cfg_src_en_i; no enable-edge clear is needed.

- `sticky_kill_ignores_accept`: An accept requires availability: busy and gp_valid are both clear, so the sticky-kill condition is false on that edge.

- `init_busy_else_removed`: INIT dispatch requires availability; no other request can become accepted or create a grant before this walk reaches its action.

- `kill_w_no_off`: Disable is captured live while busy or holding a grant; an older OFF is captured at accept, so its pending copy adds no cancellation.

The three controls above and the identity control each show no output divergence
in 16 seeds of 3,000,000 cycles against the original RTL. A known live-conflict
cancellation defect diverges in 12 of those seeds. The earlier four equivalence
controls and the removed enable-edge term also have 16-seed lockstep receipts
from round 2; the production logic is unchanged in this round. These are finite
simulation checks supporting the construction arguments, not formal proofs.

The separate `init_ready_no_en` performance control (review m28) can dispatch
extra no-op visits, but the action-side enable check still prevents allocation
for a disabled source. It must pass the documented suite bounds and is never
counted as an equivalent trace or a killed defect.
