<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# SRP admission freshness

`make` runs the 1, 2, 3, 5 and 8-source shapes; the eight-source tally is
printed last. `make run N=<n>` runs one shape. The wrapper captures
request/TSpec changes at the same edge as `KL_srp_top` and strobes the
admission invalidation input. Its only internal probe reads the slope
sampling index to cover every phase.

The independent oracle computes exact 64-bit slopes from Milan §4.3.3.2:
`(max(MaxFrameSize + 22, 68) + 20) * MaxIntervalFrames * 64000`, followed
by greedy source-order admission against the 75% ceiling. On every clock,
any grant must carry the current declaration's exact slope, a refused
TSpec must have no grant, and an unadmitted source must publish zero slope.
Every published round must carry the sum of its granted slopes. Settled
grants, sum and refusal are checked against the independent model.
Declaration acceptance must retain unrelated published grants and hold the
previous aggregate while discarding the partial round.

The grant is also judged in its Σ context on every clock:

- The live sum of granted slopes never exceeds the ceiling.
- Every published round (`round_done_o`) equals the greedy walk over
  **all** current declarations: grants, granted slopes, sum and refusal.
- Between publications, a grant only retires with its own declaration, no
  grant or granted slope changes otherwise, and the sum and refusal hold.

Every source and sampling phase covers cold refusal, grow from 224 to
20000 bytes, shrink back, identical re-declaration, withdrawal for just one
cycle, rapid changes to both TSpec fields at each pipeline stage, and
saturating overflow followed by an admissible minimum frame. A competing
source set checks greedy priority and capacity reuse. The corresponding
service-port/Listener-PDU integration is in [srp_top](../srp_top/README.md).

Cross-source cases (issue #112 round 2), for every sampling phase at two or
more sources:

- Source 0 is admitted at 699.968 Mb/s and source N-1 is refused at
  130.688 Mb/s. Source 0 then re-declares identically, shrinks to
  642.688 Mb/s, grows to 731.968 Mb/s, or re-declares twice 5 clocks apart.
  Source N-1 stays refused before and after. It is watched on every clock
  for `refused source never granted while a lower source re-declares`.
- Source 0 shrinks to 224 bytes or withdraws. Source N-1 must then be
  admitted, in a published round that already carries source 0's new state.
- With three or more sources, source 0 (400 Mb/s) and the middle source
  (299.968 Mb/s) are admitted, and source N-1 (99.968 Mb/s) is refused. The
  middle source re-declares. Source N-1 stays refused, and source 0's grant
  holds throughout.

Latency is counted from the declaration capture edge (cycle 0) to the
first grant. With no further changes, the measured cycle sets are below.
The cross-source re-declarations stay within the same sets.

| Sources | Grant latency, cycles |
|---|---|
| 1 | 4 |
| 2 | 4, 6 |
| 3 | 6, 9 |
| 5 | 5, 10, 15 |
| 8 | 8, 16, 24 |

The general bound and the optimistic-window/accounting contract are
documented in
[10 §6.3](../../docs/architecture/10_srp_engine.md#sec-10-admission-freshness),
and the cross-source rule in
[10 §6.3](../../docs/architecture/10_srp_engine.md#sec-10-admission-cross-source).
The unit bench does not model the Listener registrar or parent licences.

Mutation command (logs go to a caller-selected directory):

```sh
python3 tb/srp_admission/mutants.py --output /tmp/srp-admission-mutants
```

The campaign builds clean controls and three mutants in a temporary tree.
Each one runs through this suite at two and at eight sources, and through
[srp_top](../srp_top/README.md). A mutant counts as killed only if it fails
its named check. A build failure does not count. The temporary tree is
deleted afterwards.

| Mutant | What it restores | Named check (unit / srp_top) |
|---|---|---|
| stale-evaluation | evaluation of the old cached slope: the fit/refusal validity terms and the pending-round discard both removed | `refused current TSpec never pulses a grant` / `H: grow has no grant pulse` |
| pending-absent | round 1's rule, where a pending source counts as absent and its round publishes | `refused source never granted while a lower source re-declares` / `I: refused source … never grants` |
| discarded-round-strobes | `round_done_o` also strobes for a discarded round, so the optimistic window ages while a verdict is held | `round publishes the greedy walk over every current declaration` / `J: no Failed while …` |

Removing only the fit/refusal validity terms is an equivalent mutant. A
round that visits a pending source is discarded, so it never publishes. The
terms are kept as a defensive guard and commented in the RTL.

Measured 2026-09-24: the controls pass (12615 checks at two sources, 991231
at eight, 1527 in srp_top). Failing checks per mutant, at two sources /
eight sources / srp_top:

- stale-evaluation: 402 / 5473 / 105
- pending-absent: 175 / 1067 / 205. Of these, the named cross-source check
  fails 44 / 368 times, and srp_top's refused-source check fails in 72 of
  its 72 runs.
- discarded-round-strobes: 146 / 695 / 90

The campaign itself reports 12 checks, 12 PASS, 0 FAIL.
