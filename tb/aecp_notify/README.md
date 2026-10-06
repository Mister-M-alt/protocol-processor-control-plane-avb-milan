<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# aecp_notify registry monitor lifecycle

Builds `KL_aecp_notify` with two controller rows and drives its registry,
timer, PRNG, and CONTROLLER_AVAILABLE faces directly.

The suite registers a TIME_LIMITED controller, starts its availability probe,
expires the registry row while that probe is active, and requires a targeted
cancellation before the row is cleared. It then reuses the same row for a
different controller and verifies that the next probe carries only the new
Entity ID and MAC tuple. Section IX then grades the registry's identity index,
section TS the counter throttle stamps' valid bit, section TW a counter round
that waits for the TX slot, and section DR a DEREGISTER drained between two jobs
of a round. A third build, at two AVB interfaces, runs sections PT, CK, PD and
CA: the registry row's port, the per-interface counter rows, the registry depth
per interface and the availability probes of a controller on both interfaces.

Run `make`. Exit status zero and the printed check tally are required.

## Three builds

| Build | Override | Runs |
|---|---|---|
| `obj_dir/Vaecp_notify_sim` | none: `EN_IDENTIFY_NOTIF_P` = 0, the default | the registry monitor lifecycle above (10 checks), then section IX (11 checks), section TS (5 checks), section TW (4 checks) and section DR (11 checks) |
| `obj_idn/Vaecp_notify_idn` | `EN_IDENTIFY_NOTIF_P` = 1 (`AECP_NOTIFY_IDENT`) | section FT alone |
| `obj_if2/Vaecp_notify_if2` | `N_IF_P` = 2, P-N-AVB-INTERFACES (`AECP_NOTIFY_IF2`) | sections PT (6 checks), CK (5 checks), PD (3 checks) and CA (5 checks) alone, from `port_tuple.hpp` |

Each binary prints its own build's count, and the Makefile prints the one
canonical tally, summed over the three. `make identify` builds and runs the
second build alone (the identify mutation campaign's arm), and `make interfaces`
the third (the notify campaign's arms for issue #69).

## Sections PT, CK, PD and CA: two AVB interfaces (issue #69)

Milan v1.2 5.3.4.2 makes a registry entry {Entity ID, MAC address, port,
Sequence ID of the next unsolicited notification}, at least 16 per AVB
interface. With `N_IF_P` at 2 each row stores the port, the interface the
REGISTER arrived on (`rgy_port_i`), and each interface has `N_CTRL_P` rows (here
2) of its own: row r is {index, port}. The bench is the engine, the PRNG, the
timer service and the CA builder with the originator behind it: it retires each
job when the section says, and the clock advances one ms per cycle while a
section watches the job face.

- **PT2** REGISTER from E on port 0 holds one entry, and the same Entity ID and
  MAC on port 1 is a second.
- **PT4** one notification per entry, each at its own Sequence ID: a round before
  the second REGISTER reaches E once at 0, the round after it twice, at 1 (port
  0's entry) and 0 (port 1's).
- **PT3** a repeated REGISTER on port 1 refreshes port 1's entry: still two
  entries.
- **PT5** DEREGISTER from E on port 1 removes one entry; **PT6** it was port 1's:
  the next round reaches E once, at port 0's Sequence ID 2; **PT7** DEREGISTER on
  port 0 removes the last.

Section CK grades the AVB_INTERFACE counter rows (Milan Table 5.22: at most one
GET_COUNTERS a second per descriptor), with one controller registered:

- **CK1** a change of AVB_INTERFACE 1's counters goes out as GET_COUNTERS on
  0009:1; **CK2** AVB_INTERFACE 0's, in the same second, goes out at once on
  0009:0, its window its own; **CK3** a second change of interface 1 inside its
  second waits a second from interface 1's send; **CK4** AVB_INTERFACE 2 names no
  interface of a two-interface build and pushes nothing; **CK5** CLOCK_DOMAIN 0
  keeps its slot beside the new row.

Section PD grades the depth per interface and the row's timer identity:

- **PD1** each interface holds `N_CTRL_P` entries of its own: two REGISTERs on
  port 0 succeed and a third is refused NO_RESOURCES, then port 1 takes two and
  refuses its third.
- **PD2** row r = {index, port} arms TIME_LIMITED slot `REGMON_BASE` + r with owner
  tag 0xA0 + index, and its monitor draw arms `REGMON_BASE` + 4 + r with 0xD0 +
  index (the owner tags and the CA owner hold `N_CTRL_P` values).
- **PD3** an expiry is decoded to its row from tag and slot: row 3's TIME_LIMITED
  deadline removes row 3 alone, with its DEREGISTER to its controller, though row
  2 has the same tag; row 1's monitor deadline probes row 1's controller, though
  row 0 has the same tag.

Section CA grades the CONTROLLER_AVAILABLE probes (Milan 5.4.5.3) of rows that
share a controller or a CA owner:

- **CA1** E holds an entry on each port (CA owners 0 and 1) and both probes are
  out; one command from E cancels both exchanges, one per cycle (the review
  R512-1 probe P3: one command cancelled one, and the other's failure then
  removed E's port-1 entry).
- **CA2** the cancelled exchanges' late failures and responses touch nothing: no
  entry goes, no DEREGISTER is queued, and no draw is asked but the two E's
  command asked.
- **CA1b** a TIME_LIMITED drain's cancel and a command's cancel in the drain's
  cycle are both sent.
- **CA3** the rows of one index, one per port, share CA owner `index`, so they
  take turns: A's probe goes out alone while B's is due, A's failure removes A
  (the failure is the live probe's), and B's probe follows.
- **CA4** after a cancel, the next probe of the same CA owner waits out the settle
  (four cycles or more), so the cancelled exchange's response two cycles after the
  cancel asks no draw and is not taken for the new probe, whose own failure then
  removes its row.

Mutation record (planted by `tb/pp_top/notify_mutants.py`, which runs `make
interfaces` here; all eighteen KILLED; the three #69 controls of the registry port's
source, `rgy_port_tied_zero`, `rgy_port_from_latest_frame` and
`dereg_matches_other_port`, are graded by `tb/pp_top` section IF):

| Mutant | Planted | Failing checks |
|---|---|---|
| `port_not_compared` | the walk matches {eid, mac} without the port | 7: PT2 (a refresh, one entry), PT4, PT3, PT5, PT6, CA1, CA2 |
| `port_not_latched` | the op's port is latched as 0 whatever `rgy_port_i` says | 12: PT2, PT4, PT3, PT5, PT6, PD1, PD2, PD3, CA1 to CA4 |
| `port_not_stored` | a claimed or refreshed row stores port 0 | 4: PT3 (the refresh finds no row and claims a third), PT5, PT6, PT7 |
| `avb_counter_row_dropped` | AVB_INTERFACE 1's change sets no slot | 2: CK1, CK3 |
| `avb_counter_row_collapsed` | AVB_INTERFACE 1's change sets AVB_INTERFACE 0's slot | 3: CK1, CK2, CK3 |
| `avb_counter_named_clock` | AVB_INTERFACE 1's slot is named CLOCK_DOMAIN 0 | 2: CK1, CK3 |
| `avb_counter_any_index` | the map takes any AVB_INTERFACE index into index 0's slot (C7's `ctr-notify-avb-any-index` edit) | 5: CK1, CK2, CK3, CK4, CK5 |
| `avb_counter_name_overlaps_clock` | the slot naming starts at interface 0, so CLOCK_DOMAIN 0's slot is named AVB_INTERFACE 0 | 1: CK5 |
| `depth_shared` | a REGISTER claims any free row, not one of its own port | 6: PD1, PD2, PD3, CA1, CA2, CA1b |
| `depth_not_keyed` | the registry holds `N_CTRL_P` rows in all | 8: PD1, PD2, PD3, CA1, CA2, CA1b, CA3, CA4 |
| `registry_tag_port_bits` | a TIME_LIMITED arm's owner tag takes the row's port bits, not its index | 1: PD2 |
| `monitor_tag_port_bits` | a monitor arm's owner tag takes the row's port bits | 1: PD2 |
| `expiry_port_dropped` | an expiry is decoded to the port-0 row of its tag's index | 4: PD3, CA1, CA3, CA4 |
| `cancel_one_per_command` | a cancel not sent in its cycle is dropped, not held (review R512-1 F1) | 2: CA1, CA1b |
| `report_fail_ignores_probe` | a failure is taken for its owner's last row whether or not its probe is live | 2: CA2, CA3 |
| `report_rsp_ignores_probe` | a response is taken likewise | 1: CA2 |
| `owner_turns_dropped` | a probe no longer waits while its CA owner is held | 2: CA3, CA4 |
| `settle_dropped` | no settle after a cancel | 1: CA4 |

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

IX4 grades the rewrite's second cycle. In its first, the cycle in which the row
write lands, the index clears the old identity, and the row's match is the one
comparator against what `rows_r` holds then: the old row, as the comparator bank
read it.

- **IX5** D registers into row 1, which still holds C (IX4b removed C). In D's row
  write's own cycle, C's command and a failed probe for row 1 arrive together: C
  still matches row 1, so the command wins and the row stays.
- **IX5b** the same failure alone removes the row.
- **IX6a** after a warm reset E registers into row 0, and a second reset lands in
  E's row write's own cycle. The row write and the clear land and the set does not,
  so `rows_r` holds E while the index holds nothing for row 0; the registry is
  empty.
- **IX6** E registers again and takes row 0. In its row write's own cycle, E's
  command and a failed probe for row 0 arrive together: only the comparator can
  match E there, and the row stays.
- **IX6b** after the rewrite, E's command and a failed probe arrive together again:
  the re-indexed row matches through the index and stays.

These checks pass on the comparator-bank form the index replaced as well
(processor `f4167536`, and `main` `83999eba`, whose `KL_aecp_notify.sv` is the
same), so they describe behaviour that did not change.

## Section TS: the counter throttle stamps (issue #232)

Each counter descriptor's one-second throttle stamp, `ctr_last_r`, has no reset:
`ctr_sent_r` is its valid bit, and a warm reset clears only that bit. So after a
reset the first change goes out at once, however recent the stale stamp. The
section starts from a warm reset and a registered controller; the bench's clock
stays in one ms throughout, and a change on AVB_INTERFACE[0] is watched for its
GET_COUNTERS job:

- **TS1** a change goes out to the registered controller.
- **TS2** a second change in the same second is held.
- **TS3** after a warm reset and a new REGISTER, a change in that same second
  goes out at once.

The two REGISTERs of the section are checked as well. TS passes on `main`'s RTL,
whose stamps had a reset, too.

## Section TW: a counter round that waits for the TX slot (issue #148)

The engine retires a job (`uns_done_i`) when the TX arbiter grants its frame, so a
job held for the TX slot is sent late. Milan Table 5.22 (`T-CTR-NOTIF`) spaces a
descriptor's GET_COUNTERS notifications a second apart at each controller, so the
next round waits a second from the previous round's last send, never from its
selection. The bench is the engine: it retires each job when the section says, and
its clock advances one ms per cycle. Two controllers, C (row 0) and D (row 1), are
registered after a warm reset (both REGISTERs checked), and the changes are on
AVB_INTERFACE[0]:

- **TW1** a round is presented at ms 2004; C's job is sent at once and D's waits for
  the TX slot until ms 2600. A change at ms 2700 is held until a second after that
  last send: the next round is presented at ms 3603, against 3600 to 3608. On
  `main`'s RTL, which stamped the round at its selection, at ms 3005.
- **TW2** a round is presented at ms 5004 and C's job waits for the TX slot until
  ms 6500; a change arrives at ms 5100, inside that wait. D's job is sent at ms
  6504, and the change goes out a second after it: the next round is presented at
  ms 7507, against 7504 to 7512. On `main`'s RTL at ms 6508, right after the round:
  the window opened a second after the selection, while C's job still waited.

The upper bound (8 cycles: the pick and the walk to the job) shows that the held
change is sent, not lost.

Mutation record (planted by `tb/pp_top/notify_mutants.py`, which runs `make run`
here; all thirteen KILLED). The fifth to the seventh are PR #153's review faults
(R452-1 and R453-1), each the reviewer's own edit; the next three are issue #148's,
and the last three issue #158's (section DR, below). Since #158 the three #148
controls fail DR3 as well, at ms 31512: none of them follows D's wait after the drain:

| Mutant | Planted | Failing checks |
|---|---|---|
| `ix_old_identity_kept` | a row write never clears the old identity | 1: IX1 |
| `ix_new_identity_unset` | a row write never sets the new identity | 3: IX3, IX4, IX6b |
| `ix_last_chunk_ignored` | the match ignores the last 6-bit chunk | 1: IX2 |
| `ix_rewrite_unmatched` | the two rewrite cycles read the index, not the compare | 3: IX4, IX6, IX6b |
| `override_set_only` | the compare covers only the rewrite's second cycle; the row write's own cycle reads the index | 2: IX6, IX6b |
| `own_compare_new_row` | the rewrite's compare reads the incoming row, not what `rows_r` holds | 1: IX5 |
| `stamp_read_without_valid` | a counter stamp is read without its valid bit, `ctr_sent_r` | 1: TS3 |
| `counter_spacing_from_selection_tw` | the stamp no longer follows a waiting job: the limit restarts at the round's selection (`main`'s rule) | 3: TW1 (ms 3005), TW2 (ms 6508), DR3 |
| `counter_stamp_at_send_only` | the stamp written at the job's send alone, not while it waits | 2: TW2 (ms 6508), DR3 |
| `counter_stamp_first_job_only` | the stamp follows only the round's first job (row 0) | 3: TW1 (ms 3007), TW2 (ms 7503), DR3 |
| `dereg_mid_round_no_hold` | a drained DEREGISTER no longer waits for the round's boundary (`main`'s rule) | 3: DR1, DR2, DR3 |
| `dereg_pending_stops_follow` | the counter stamp stops following while a DEREGISTER is pending (review R477-1 S2's hazard) | 1: DR3 (ms 31512) |
| `dereg_lost_at_round_end` | the round's end drops the held DEREGISTER | 2: DR1b, DR2b |

## Section DR: a DEREGISTER drained between two jobs of a round (issue #158)

A TIME_LIMITED expiry, or a failed CONTROLLER_AVAILABLE retry, parks a registry
row. The walk drains it between two jobs of a round and latches the controller's
own DEREGISTER notification (Milan Table 5.22: "sent only to this controller").
On `main` that single-shot job rewrote the round's response kind, descriptor and
arguments, so every remaining controller of the round received a DEREGISTER
(kind 0, descriptor 0000:0) in place of the round's notification. The DEREGISTER
now waits for the round's boundary. The bench is the engine, as in TW, with C in
row 0 and D in row 1, both registered after a warm reset (all six REGISTERs
checked):

- **DR1** (the issue's probe) a GET_COUNTERS round on AVB_INTERFACE[0], whose
  first job, to C, waits while C's TIME_LIMITED registration expires. D still
  receives the round's GET_COUNTERS 0009:0 (ms 20010).
- **DR1b** C alone receives its own DEREGISTER, once: kind 0, descriptor 0000:0,
  sequence_id 1 (ms 20013, after the round).
- **DR2** a SET_NAME round (descriptor 0005:1, arguments 2 and 3; the block
  carries them through unread, and the requester is not registered), whose
  first job, to C, waits while C's CONTROLLER_AVAILABLE retry fails. D still
  receives the round's notification, every field intact.
- **DR2b** as DR1b.
- **DR3** (review R477-1 S2 on PR #159) TW with the drain: C's registration
  expires in a GET_COUNTERS round, the job after the drain waits 1.5 s for the
  TX slot, and a change arrives during the wait. D's next GET_COUNTERS waits a
  second from D's own send in the round: sent at ms 31504, the next presented at
  ms 32508, against 32504 to 32512.

The checks accept D's job and C's DEREGISTER in either order, so they grade the
round's notification and the DEREGISTER's own semantics, not the order the fix
chose. On `main`'s RTL DR1, DR2 and DR3 fail: D receives kind 0, descriptor
0000:0 in DR1 and DR2, and in DR3 D's first GET_COUNTERS after the drain is the
next round's (ms 31513). DR1b and DR2b pass on `main`.
