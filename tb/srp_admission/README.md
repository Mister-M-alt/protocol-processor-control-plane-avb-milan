<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# SRP admission freshness

`make` runs the default eight-source admission shape. `make shapes` runs
1, 2, 3, 5 and 8 sources. The wrapper captures request/TSpec changes at the
same edge as `KL_srp_top` and strobes the admission invalidation input. Its
only internal probe reads the slope sampling index to cover every phase.

The independent oracle computes exact 64-bit slopes from Milan §4.3.3.2:
`(max(MaxFrameSize + 22, 68) + 20) * MaxIntervalFrames * 64000`, followed
by greedy source-order admission against the 75% ceiling. On every clock,
any grant must carry the current declaration's exact slope, a refused
TSpec must have no grant, and an unadmitted source must publish zero slope.
Every completed round must publish the sum of its granted slopes. Settled
grants, sum and refusal are checked against the independent model.
Declaration acceptance must retain unrelated published grants and hold the
previous aggregate while discarding the partial round.

Every source and sampling phase covers cold refusal, grow from 224 to
20000 bytes, shrink back, identical re-declaration, withdrawal for just one
cycle, rapid changes to both TSpec fields at each pipeline stage, and
saturating overflow followed by an admissible minimum frame. A competing
source set checks greedy priority and capacity reuse. The corresponding
service-port/Listener-PDU integration is in [srp_top](../srp_top/README.md).

Latency is counted from the declaration capture edge (cycle 0) to the
first grant. With no further changes, the measured cycle sets are:

| Sources | Grant latency, cycles |
|---|---|
| 1 | 4 |
| 2 | 4, 6 |
| 3 | 6, 9 |
| 5 | 5, 10, 15 |
| 8 | 8, 16, 24 |

The general bound and optimistic-window/accounting contract are documented
in [10 §6.3](../../docs/architecture/10_srp_engine.md#sec-10-admission-freshness).
The unit bench does not model the Listener registrar or parent licences.

Mutation command (logs go to a caller-selected directory):

```sh
python3 tb/srp_admission/mutants.py --output /tmp/srp-admission-mutants
```

The campaign builds clean controls and a mutant that removes the slope
validity guards from fit/refusal, restoring evaluation of the old cached
slope. Both the two-source and eight-source mutants must fail the named
`refused current TSpec never pulses a grant` assertion. A build failure
does not count as a killed mutant. Each temporary build tree is deleted.

Measured 2026-09-24: both controls pass; the mutant fails 184 of 5252
checks at two sources and 4032 of 723046 at eight sources. The campaign
itself reports 4 checks, 4 PASS, 0 FAIL.
