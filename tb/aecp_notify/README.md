<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# aecp_notify registry monitor lifecycle

Builds `KL_aecp_notify` with two controller rows and drives its registry,
timer, PRNG, and CONTROLLER_AVAILABLE faces directly.

The suite registers a TIME_LIMITED controller, starts its availability probe,
expires the registry row while that probe is active, and requires a targeted
cancellation before the row is cleared. It then reuses the same row for a
different controller and verifies that the next probe carries only the new
Entity ID and MAC tuple.

Run `make`. Exit status zero and the printed check tally are required.

## Two builds

| Build | Override | Runs |
|---|---|---|
| `obj_dir/Vaecp_notify_sim` | none: `EN_IDENTIFY_NOTIF_P` = 0, the default | the registry monitor lifecycle above (10 checks) |
| `obj_idn/Vaecp_notify_idn` | `EN_IDENTIFY_NOTIF_P` = 1 (`AECP_NOTIFY_IDENT`) | section FT alone |

Each binary prints its own build's count, and the Makefile prints the one
canonical tally, summed over both. `make identify` builds and runs the second
build alone (the identify mutation campaign's arm).

## Section FT: the identify schedule at the full timebase

`tb/pp_top` section ID grades IDENTIFY_NOTIFICATION on the wire with 1 ms
compressed to 100 clocks. There one tick is no longer than a frame's own build
and serialization, so the sequencer's one-tick margins cannot be seen (reviews
R420-2 S1 and R421-2 S2). Here 1 ms is 100,000 clocks: the F01.5 default
P-CLK-HZ of 100 MHz with the top's default prescaler. The bench is the engine,
the MAC and the shared timer service, so a frame leaves on the clock the bench
picks. Its timer model fires a due slot on the first clock of its deadline's ms,
with no sweep delay, which is the least favourable placement for both margins:
the real service's sweep only adds clocks.

The button is held from the first frame past Figure 7-142's timeout (IEEE
1722.1-2021 §7.5.1, §7.5.1.2.1 and Figure 7-142):

- **FT1** (premise) the press presents a burst's three identify jobs, and at the
  timeout a fourth.
- **FT2** frame 1 leaves two clocks before a ms boundary, the last clock whose
  IDENT-BURST arm still lands in that ms. Frame 2 is presented T-IDENT-BURST
  after that boundary, never one tick sooner: 15,000,004 clocks after frame 1
  left, against 15,000,000 to 15,000,008.
- **FT3** frame 2 leaves on a ms's first clock: frame 3 is presented less than one
  tick past T-IDENT-BURST after it, 15,100,002 clocks.
- **FT4** the next burst starts at the timeout, T-IDENT-REARM after the boundary
  two clocks after frame 1 left (t0), never one tick sooner: 100,000,004 clocks
  after frame 1 left.

Mutation record (planted by `tb/pp_top/notify_mutants.py`, which runs
`make identify` here; both KILLED):

| Mutant | Planted | Failing checks |
|---|---|---|
| `ident_burst_deadline_one_tick_short` | the IDENT-BURST deadline from the departure's own ms (`+ 1` dropped) | 1: FT2 (14,900,004 clocks) |
| `ident_t0_same_ms` | t0 the first frame's own ms, not the next boundary | 1: FT4 (99,900,004 clocks) |
