<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# aecp_notify registry monitor lifecycle

Builds `KL_aecp_notify` with two controller rows and drives its registry,
timer, PRNG, and CONTROLLER_AVAILABLE faces directly.

The suite registers a TIME_LIMITED controller, starts its availability probe,
expires the registry row while that probe is active, and requires a targeted
cancellation before the row is cleared. It then reuses the same row for a
different controller and verifies that the next probe carries only the new
Entity ID and MAC tuple. Section IX then grades the registry's identity index.

Run `make`. Exit status zero and the printed check tally are required.

## Two builds

| Build | Override | Runs |
|---|---|---|
| `obj_dir/Vaecp_notify_sim` | none: `EN_IDENTIFY_NOTIF_P` = 0, the default | the registry monitor lifecycle above (10 checks), then section IX (6 checks) |
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

## Section IX: the identity index (issue #232)

The availability monitor matches each valid command's {Entity ID, MAC} against
every registry row in the command's own cycle. `KL_aecp_notify` does that through
a LUTRAM identity index beside the row table, not a comparator per row (see its
"identity index" comment). Row 0 holds B, which reused A's row, and B's probe is
live, so a match on row 0 shows in the same cycle as the probe's cancellation
(`ca_cancel_valid_o`). The first three checks only evaluate, with no clock edge:

- **IX1** A's command, the row's previous controller, does not match.
- **IX2** none of the 112 identities one bit from B's matches.
- **IX3** B's command matches.
- **IX4a** a second controller, C, registers into row 1.
- **IX4** two cycles after its REGISTER, while the index rewrites row 1, C's
  command and a failed probe for row 1 arrive in the same cycle: the command
  wins, as the block's `ca_fail` arm requires, and the row stays.
- **IX4b** the same failure alone removes the row.

These checks pass on the comparator-bank form the index replaced as well
(processor `f4167536`), so they describe behaviour that did not change.

Mutation record (planted by `tb/pp_top/notify_mutants.py`, which runs `make run`
here; all four KILLED):

| Mutant | Planted | Failing checks |
|---|---|---|
| `ix_old_identity_kept` | a row write never clears the old identity | 1: IX1 |
| `ix_new_identity_unset` | a row write never sets the new identity | 2: IX3, IX4 |
| `ix_last_chunk_ignored` | the match ignores the last 6-bit chunk | 1: IX2 |
| `ix_rewrite_unmatched` | the two rewrite cycles read the index, not the compare | 1: IX4 |
