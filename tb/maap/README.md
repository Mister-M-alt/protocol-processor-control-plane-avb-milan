<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# maap — KL_pp_maap (IEEE 1722-2016 Annex B) unit suite

The MAAP engine against **independent** expectations: frame bytes from the
Figure B.1 offsets, the walk from Table B.7, timer bounds from B.3.4, the
compare_MAC rule from B.3.6.4 — never DUT logic. The engine runs with its
real services (KL_pp_prng kinds 5/6/7, KL_pp_timer_service compressed to
1 ms = 10 clk, KL_pp_tx_slots); the C++ side plays the TX arbiter's lane and
the talker's allocator face; records are injected pre-parsed exactly as the
validator's MAAP lanes deliver them.

What it proves (U0..U22):

- cold start: `generate_address` inside the Table B.9 pool with the block
  fit, then 4 byte-exact PROBEs (1 + `MAAP_PROBE_RETRANSMITS`) at spacings
  measured inside the exclusive (500, 600) ms probe bounds, the first
  ANNOUNCE back-to-back with the fourth (probeCount!), the claim valid only
  from DEFEND, and the (30, 32) s announce cadence;
- the full Table B.7 conflict matrix: rProbe! defended in DEFEND with the
  byte-exact B.3.6.6 overlap fields and **no** tie-break, ignored or yielded
  in PROBE by compare_MAC; rDefend!/rAnnounce! yielding unconditionally in
  PROBE and by compare_MAC in DEFEND; footnote-b non-overlap and empty-range
  ignores; reserved message types ignored (B.2.2); maap_version tolerance
  (B.2.3.2);
- yield mechanics: re-randomized range (never the seed), fresh 4-probe walk,
  re-address counter, and the per-source conflict fan-out lowest-first;
- the allocator seam under the shim contract: accept-and-answer at the
  accepting edge, refuse while probing or parked, grant base + s in DEFEND,
  refuse s >= count (with the count change fanning out conflicts), RELEASE
  acknowledged as a no-op;
- the engage arcs: Release! (engage fall) tears down with **no** PDU
  (footnote c), PortOperational! (link bounce) restarts with a fresh range,
  and footnote-a seeding probes the provisioned offset first.

The Table B.9 fit (U17, U17b, U18; issue #66, REQ-MAAP-001, B.1 and
B.3.6.1):

- **U17** the reject arm. At the widest `cfg_count_i` (255) the whole block
  fits only at offsets up to `0xFE00 - 255 = 0xFD01`. A kind-7 stub in
  `maap_wrap.sv` hands the engine `0xFDFF`, then `0xFD02`, then `0xFD01`: the
  first two overhang `91:E0:F0:00:FD:FF` and are redrawn (three draws
  consumed), and the byte-exact PROBE names `91:E0:F0:00:FD:01`, count 255,
  whose block ends exactly at the top of the pool. The real PRNG would reject
  only about one draw in 256 at this count, and one in 9,300 at the suite's
  count of 8, so without the stub the arm is never driven.
- **U17b** a Release! (link loss) inside that redraw loop, at each of four
  consecutive cycles of its request/answer rhythm, must leave no draw
  behind: each following PortOperational! reaches its first PROBE (Table
  B.7, B.3.5.9). This test found the defect fixed with it (below).
- **U18** a seeded walk whose offset (`0xFFFF`, inside the Table B.10
  reserved range) lies past `0xFE00 - count` probes the clamped offset
  `0xFDF8` byte-exact without drawing an address (footnote a), claims the
  block, and grants its last source `91:E0:F0:00:FD:FF`.

The kind-7 stub is the wrapper's one piece of harness logic: while the suite
scripts values, a completed kind-7 draw hands the engine the scripted value
instead of the PRNG's; kinds 5 and 6 always pass through. Every other
scenario runs on the real PRNG.

The Table B.7 conflict walk with a discriminating compare_MAC (issue #68,
REQ-MAAP-005, B.3.5.5 to B.3.5.7, Table B.7, B.3.6.4). Every tie-break
scenario uses a peer MAC whose forward and octet-reversed orders against
`OWN_MAC` (`02:AA:BB:CC:DD:EE`) **disagree**, and asserts that premise, so a
forward compare decides each one the wrong way:
`WIN_MAC` `00:11:22:33:44:FF` is forward-lower but reversed-higher
(compare_MAC TRUE, we win), and `LOSE_MAC` `F2:11:22:33:44:01` is
forward-higher but reversed-lower (compare_MAC FALSE, we lose).

| Cell | We win (`WIN_MAC`: no action) | We lose (`LOSE_MAC`: yield, fresh range) |
|---|---|---|
| PROBE / rProbe! | U9: the walk continues and claims the contested range | U19: yield, re-address counted, next PROBE on a fresh range |
| DEFEND / rAnnounce! | U7: the claim stands | U8: yield, 8 per-source conflicts, a fresh 4-probe walk |
| DEFEND / rDefend! | U20: the claim stands, nothing sent | U21: yield, 8 per-source conflicts, a fresh range |

The no-tie-break cells use a peer we beat in **both** orders
(`F2:FF:EE:DD:CC:FF`), so a tie-break inserted in either direction would
keep the range: PROBE / rDefend! (U10) and PROBE / rAnnounce! (U22) still
yield. DEFEND / rProbe! (U5, U5b) is defended with no tie-break. Every yield
is graded by one helper: one re-address, the claim not valid, and the next
frame a byte-exact PROBE of a range inside the pool that is not the
contested one.

Run: `make` (exit 0 = PASS).

## RTL fix found by U17b (issue #66)

`KL_pp_maap.sv` `W_ADDR`: an engage fall (Release! or link loss) while a
kind-7 draw was in flight parked the walker in `W_OFF` with its draw mark
still set. The PRNG's answer arrived unread, so the next walk's `W_IVAL`
waited forever for an answer already given. Every later PortOperational!
then stalled before ReserveAddress!, with no PROBE and no claim until reset
(Table B.7 PortOperational! from INITIAL; B.3.5.9). In the top the stale
answer is steered to the MAAP engine too, so the processor wedged the same
way. The exit now clears the mark. Before the fix U17b fails phases 1 to 3
and U18 (6 FAIL of 89); after it the suite passes.

## Mutation campaign

`make mutants` (optionally `MUTANT_OUTPUT=<dir>` for the receipts; default
`/tmp/maap-mutants`) plants each reviewed patch in `mutations/` into a
scratch copy of `hdl/` with `git apply`. An arm may run on more than one
suite (`tb/rx_validator`, and `tb/pp_top`'s `maap-internal` target, the MP
section alone). It first runs every suite target the arms use unmutated,
then requires each arm's simulation to finish red with a `FAIL:` line
carrying the arm's own named check. A build failure, a
missing tally or a clean run is UNPROVEN, never a kill. The driver reads
only simulation logs; no expectation comes from RTL text.

| Arm (patch) | Planted defect | Suite | Named failures (of the suite's checks) |
|---|---|---|---|
| `fit-compare-forced-true` | the fit compare at `KL_pp_maap.sv:594` (issue #66's `:587` before the fix above) forced true: an overhanging draw is accepted | maap | U17 x4 (1 draw instead of 3; claim `…:FD:FF`; block ends `…:FE:FD`; PROBE bytes): 4 FAIL of 114 |
| `fit-compare-off-by-one` | the fit compare `<=` becomes `<`: the last fitting offset is refused | maap | U17 x3 (no PROBE, the boundary draw refused 1,745 times) and U17b x4: 7 FAIL of 113 |
| `seed-clamp-removed` | the footnote-a seed clamp removed: the provisioned offset is probed as given | maap | U18 x4 (claim `…:FF:FF`; PROBE bytes; claim; last-source grant): 4 FAIL of 114 |
| `release-keeps-draw-mark` | the fix above removed | maap | U17b phases 1 to 3, U18 x3, then U19 to U22 x16 behind the wedge: 22 FAIL of 110 |
| `validator-maap-version-1-only` | `KL_pp_rx_validator.sv` gains a `maap_version == 1` acceptance rule (issue #67, B.2.3.2/B.2.3.4) | rx_validator | F28a/F28b/F28c: versions 2, 0 and 31 counted `rx_version` and aborted: 47 FAIL of 453 |
| (same arm) | | pp_top `maap-internal` | MP7 x4: neither the version-2 nor the version-0 PROBE is defended: 4 FAIL of 33 |
| `compare-mac-forward` | `cmp_mac_true_w = own_mac_i < rxm_sa_r`: compare_MAC in forward octet order (issue #68, B.3.6.4) | maap | U7, U8, U9 x2, U10, U19 x2, U20, U21: 9 FAIL of 112 |
| (same arm) | | pp_top `maap-internal` | MP4 x5: the rev-lower, forward-higher ANNOUNCE is ignored, nothing withdrawn: 5 FAIL of 33 |
| `probe-rprobe-never-yields` | PROBE / rProbe! never yields | maap | U19 x2: 2 FAIL of 114 |
| `defend-rdefend-ignored` | DEFEND / rDefend! ignored (only rAnnounce! can yield in DEFEND) | maap | U21 x4, then U22 x4 behind it: 8 FAIL of 112 |
| `defend-rdefend-no-tiebreak` | DEFEND / rDefend! always yields (no compare_MAC) | maap | U20, then U21 (not in DEFEND): 2 FAIL of 114 |
| `probe-rannounce-tiebreak` | PROBE / rDefend! and rAnnounce! gain a compare_MAC | maap | U10 x3, U22 x2: 5 FAIL of 114 |
| `yield-reuses-range` | a yield re-probes the contested range instead of running generate_address | maap | U8, U10, U15, U19, U21, U22: 6 FAIL of 114 |

The last run: 3 controls PASS and 13 of 13 arm runs KILLED
(`16 checks: 16 PASS, 0 FAIL`).
