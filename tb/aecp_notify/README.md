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
that waits for the TX slot, section DR a DEREGISTER drained between two jobs
of a round, section CX the clock of the availability cancellation, section
SC a command cancellation coinciding with a TIME_LIMITED drain, section LF the
flags of the lock notification, section LC the one cause each lock notification
carries, and section CQ the command queue. Every run of
every build ends with section OC, the queue's occupancy at every clock edge.
A third build, at two AVB interfaces, runs sections PT, CK, PD and CA: the registry row's port, the per-interface counter rows, the registry depth
per interface and the availability probes of a controller on both interfaces.

Run `make`. Exit status zero and the printed check tally are required.

## Three builds

| Build | Override | Runs |
|---|---|---|
| `obj_dir/Vaecp_notify_sim` | none: `EN_IDENTIFY_NOTIF_P` = 0, the default | the registry monitor lifecycle above (10 checks), then section IX (11 checks), section TS (5 checks), section TW (4 checks), section DR (11 checks), section CX (1 check), section LF (6 checks), section LC (45 checks), section CQ (8 checks) and OC1 |
| `obj_idn/Vaecp_notify_idn` | `EN_IDENTIFY_NOTIF_P` = 1 (`AECP_NOTIFY_IDENT`) | section FT alone, then OC1 |
| `obj_if2/Vaecp_notify_if2` | `N_IF_P` = 2, P-N-AVB-INTERFACES (`AECP_NOTIFY_IF2`) | sections PT (6 checks), CK (5 checks), PD (3 checks) and CA (5 checks) alone, from `port_tuple.hpp`, then OC1 |

Each run prints its own count, and the Makefile prints the canonical tally.
The default `make` runs the three builds and SC1 and SC2 as separate runs of the
first binary (`--cancel-collision-only` and `--cancel-failure-only`). `make run`
retains the existing three-run record; `make collision` runs SC1 alone and
`make failure-window` runs SC2 alone. `make identify` builds and runs the
second build alone (the identify mutation campaign's arm), and `make interfaces`
the third (the notify campaign's arms for issue #69). The combined suite has
131 checks: 102 in the first run, 5 in FT, 20 in PT/CK/PD/CA and 4 in SC, each run's
last check its OC1. Every build reads `occupancy.vlt`, which makes the queue's
count and occupancy flop readable by the bench and changes no RTL. CX keeps its
one-interface cancellation-clock premise; CA4 measures the engine's settle
interval, before the top's withdraw register, so its four-cycle lower bound
is unchanged by #163.

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
here; all fourteen KILLED). The fifth to the seventh are PR #153's review faults
(R452-1 and R453-1), each the reviewer's own edit; the next three are issue #148's,
the next three issue #158's (section DR, below), and the last issue #163's (section
CX, below). Since #158 the three #148 controls fail DR3 as well, at ms 31512: none of
them follows D's wait after the drain:

| Mutant | Planted | Failing checks |
|---|---|---|
| `ix_old_identity_kept` | a row write never clears the old identity | 1: IX1 |
| `ix_new_identity_unset` | a row write never sets the new identity | 4: IX3, IX4, IX6b, CX1 (3 before #163) |
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
| `cancel_one_clock_late` | the availability cancellation registered, one clock after the command | 2: CX1, IX3 |

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

## Section CX: the clock of the availability cancellation (issue #163)

The top registers the originator's withdraw mask, so a cancellation reaches the TX
arbiter one clock after the originator takes it (`tb/pp_top` section WD grades that
boundary on the wire). The registry monitor adds no clock of its own: its
cancellation is combinational from the command it supersedes. After a warm reset,
E registers (row 0), its monitor draw is answered, the monitor expiry launches E's
probe, and a command from E is presented for one clock:

- **CX1** the cancellation, owner 0, is presented in that clock, before the edge
  that takes the command, and in none of the eight clocks after it.

IX3 reads the same match without a clock edge; CX1 takes the command through its
edge and watches for a late or repeated cancellation. `cancel_one_clock_late`
(above) registers the cancellation and fails both, and `ix_new_identity_unset`, whose
command matches no row, fails CX1 as well.

## Section SC: simultaneous drain and command cancellation (issue #167)

**SC1** registers two controllers at one AVB interface and launches both
availability probes. One registration is TIME_LIMITED. On its drain's cancel
cycle, the other controller sends a command for exactly one clock. The check
observes the cancellation face before every edge and requires each owner
exactly once, the expired controller's targeted DEREGISTER, and one remaining
registration. It repeats with the expired row first and last, so neither
priority order hides a lost cancellation. Every setup operation and both probe
tuples are part of the same check.

The base sends only the expired row's cancellation in both cases. The pending
command cancellation now follows it. Count-two CA1b continues to grade its
existing pending-cancel drain unchanged.

| Mutant | Planted | Failing checks |
|---|---|---|
| `cancel_collision_drops_command` | the count-one pending cancels are discarded each cycle | SC1 |
| `cancel_pending_accepts_failure` | a failure is accepted while its owner's cancellation is pending | SC2 |

**SC2** repeats the coincidence in both row orders and presents a failure for
the commanding owner exactly one cycle later, as in the review's P1 probe.
Both cancellations must still occur exactly once, the commanding row must stay
registered, and no DEREGISTER may target that live controller during the
400-cycle watch. The expired controller must still receive its own DEREGISTER.
Removing the pending-cancel failure guard fails SC2 in both row orders: zero
entries remain and a DEREGISTER targets the live controller. SC2 runs separately
so every existing campaign arm retains its previous failing-check record.

Both controls are arms of `tb/pp_top/notify_mutants.py`.

## Section LF: the LOCK_ENTITY notification's flags (issue #186)

IEEE 1722.1-2021 7.5.2 sends a notification "by using an unsolicited response to the
command", whose flags 7.4.2.1 echoes, so the job of a lock change a LOCK or UNLOCK
caused carries that command's flags word, `uns_arg1_o` the high half and `uns_arg0_o`
the low. Milan Table 5.22's auto-unlock, an unlock, carries UNLOCK (1, Table 7-143),
as an UNLOCK command's does. After a warm
reset C registers (row 0) and controller X changes the lock; each word sets reserved
bits, so a latch of the UNLOCK bit alone is seen. A SET_NAME round whose job the bench
holds keeps a lock round pending, so two changes coalesce, and the flags follow the
last cause, as the exclusion does:

- **LF1** X's LOCK with flags 0x00010000: C's job carries 0001/0000.
- **LF1b** X's UNLOCK with flags 0x80020001: 8002/0001.
- **LF2** X's LOCK with flags 0x00040000 times out before its round leaves: the
  coalesced job is the auto-unlock's, UNLOCK: 0000/0001.
- **LF3a** X's LOCK with flags 0: 0000/0000.
- **LF3** the lock times out with its round held, then X's LOCK with flags 0x00020000:
  the coalesced job carries the LOCK's 0002/0000.

| Mutant | Planted | Failing checks |
|---|---|---|
| `lock_coalesced_timeout_keeps_flags` | a timeout coalesced into a pending lock round keeps the LOCK's flags | LF2; since section LC, also LC1 before and on the claim (both words) |
| `lock_coalesced_timeout_flags_zero` | a timeout coalesced into a pending lock round latches 0, not UNLOCK | LF2; also LC1 before and on the claim (both words) |
| `lock_take_keeps_old_flags` | a LOCK latches no flags | LF1, LF3a, LF3; also LC0, LC1 two clocks after the claim (0x00010000), LC2, LC3, LC4, LC5, LC7 |
| `lock_release_keeps_old_flags` | an UNLOCK latches no flags | LF1b; also LC6 |

The four controls are arms of `tb/pp_top/notify_mutants.py`; `tb/pp_top` sections L
(L5b, L6d) and AX (LN) grade the same flags on the wire.

## Section LC: one cause per lock notification (issue #186)

Every LOCK_ENTITY notification carries the flags and the `locked_id` of one cause
(IEEE 1722.1-2021 7.5.2, 7.4.2.1 and Table 7-143; Milan v1.2 Table 5.22;
REQ-AEM-003, REQ-NOT-001): a LOCK's flags word with its holder, an UNLOCK's word or
the automatic unlock's UNLOCK (0x00000001) with `locked_id` 0. The job's arguments
become the flags when the engine loads the job, and E_LOCKUNS then reads the holder
through the block's state query, so the bench serves every lock job as the engine
does: it takes the arguments at the load edge, raises the state query (whose first
cycle withdraws the job, whose second takes the read and whose third answers it),
reads the holder, and retires the job when it is presented again. Each case must
give exactly the listed frames, each graded by its @24..@35 bytes (flags, then
`locked_id`, big-endian), its destination and its sequence_id. C, X, Y and D are
controllers; X is the first owner.

- **LC0** (premise) C and X registered, X's LOCK with flags 0x00010000, undisturbed:
  the round is claimed one cycle after the op and C alone receives 0x00010000 with
  holder X.
- **LC1** the same, with the lock expiring one clock before the claim, on it, one
  clock after it (the first job not yet presented) and two clocks after it (the
  first job presented), after a LOCK word of 0 and of 0x00010000 (8 checks). Before
  and on the claim: C and X each receive UNLOCK with 0. One clock after: the round
  is re-latched, so C and X each receive UNLOCK, and both again in the next round.
  Two clocks after: the expiry waits for C's job, whose frame is the LOCK's (its
  word, holder X), then X receives UNLOCK, and both again.
- **LC2** the lock expires after the engine loads C's job and before it reads the
  holder: C's frame is the LOCK's, then X receives UNLOCK, and both again.
- **LC3** the lock expires in the cycle the holder read is taken: as LC2.
- **LC4** C and D registered; X locks (its round sent), then unlocks with
  0x80020001; while the UNLOCK round's job to C waits, Y locks with 0x00030000. No
  job carries UNLOCK once Y holds the lock: C and D each receive Y's LOCK with
  holder Y, and both again.
- **LC5** as LC4 with Y registered after C: the rest of the round skips Y, the
  requester of the LOCK its jobs now carry, and so does the next round (C twice).
- **LC6** C and D registered; X locks with 0x00010000 and, while the round's job to
  C waits, unlocks with 0x80020001: C and D each receive the UNLOCK with 0, and both
  again.
- **LC7** the same LOCK; the lock expires while C's job is presented (the expiry
  waits), then X locks again with 0x00040000, which takes the released lock: C and D
  each receive that LOCK with holder X, and both again.

The other 30 checks are the cases' REGISTERs. At `b0f6f9c3`, before the block held the
expiry back and re-latched its rounds, LC0 and LC1's clock before the claim pass and
the rest fail: on the claim the round carries the LOCK's word with `locked_id` 0 and
the automatic unlock is lost (the reviewers' claim-collision reproduction), and in
LC4 C and D receive UNLOCK with holder Y (the mid-round reproduction).

Mutation record (planted by `tb/pp_top/notify_mutants.py`, which runs `make run`
here; all ten KILLED by their named checks):

| Mutant | Planted | Named | Failing checks |
|---|---|---|---|
| `lock_claim_beside_expiry` | the round claim no longer waits behind an expiry applied in its cycle | LC1 | 2: LC1 on the claim (both words) |
| `lock_expiry_in_presented_job` | the expiry no longer waits while a LOCK job is presented | LC2 | 3: LC1 two clocks after the claim (both words), LC2 |
| `lock_expiry_at_holder_read` | the expiry no longer waits in the cycle a state query is taken | LC3 | 4: LC1 two clocks after (both words), LC2, LC3 |
| `lock_expiry_pending_dropped` | an expiry that had to wait is never applied | LC2, LC3 | 5: LC1 two clocks after (both words), LC2, LC3, LC7 |
| `lock_expiry_round_not_relatched` | an expiry during a lock round leaves its arguments and exclusion | LC1 | 6: LC1 one and two clocks after (both words), LC2, LC3 |
| `lock_expiry_relatch_keeps_exclusion` | that re-latch keeps the round's exclusion | LC1 | 6: as the previous |
| `lock_take_round_not_relatched` | a LOCK during a lock round leaves its arguments and exclusion | LC4 | 3: LC4, LC5, LC7 |
| `lock_take_relatch_keeps_exclusion` | that re-latch keeps the round's exclusion | LC5 | 1: LC5 |
| `lock_release_round_not_relatched` | an UNLOCK during a lock round leaves its arguments and exclusion | LC6 | 1: LC6 |
| `lock_op_ignores_due_expiry` | a LOCK or UNLOCK decides on the lock before an expiry owed in its cycle | LC7 | 1: LC7 |

## Sections CQ and OC: the command queue (issue #181)

A successful state-changing command pushes its notification onto a queue of
`P-NOTIF-QUEUE-DEPTH` entries (06 section 7). The emitter claims (pops) one
entry per round, and a push and a claim can share a clock. The queue keeps its
occupancy in a flop of its own, `cmdq_nempty_r`, written beside every count
update, so `amap_busy_o` and the emission pick read one flop instead of
comparing the count. While an entry waits, `amap_busy_o` holds the engine's
next command, which keeps the finite queue lossless (Milan v1.2 5.4.5.1 and
5.4.5.2, REQ-NOT-001 and REQ-NOT-002).

Section CQ runs twice, each time after a warm reset with C (row 0) and D (row 1)
registered (four REGISTERs checked). The bench is the engine and the clock
advances one ms per cycle. Entry n is SET_NAME on 0005:n with arguments
0x100 + n and 0x200 + n. An odd entry comes from C, which it excludes; an even
one comes from an unregistered requester.

- **CQ1** two commands back to back into an idle block. The second is pushed in
  the clock the emitter claims the first (the count reads 1 after it, not 2),
  and it is the last push. C receives entry 0, and D entries 0 and 1, in order,
  each at that controller's next sequence_id: three jobs.
- **CQ1b** `amap_busy_o` is low before the first push, high in every clock from
  the one after it to the last job, and low again after that. In the clock after
  the first push nothing but the queue holds it.
- **CQ2** `P-NOTIF-QUEUE-DEPTH` + 1 = 17 commands back to back while the first
  round's job is held: entry 0 in the round and a full queue behind it (16
  entries at most), the second command again pushed in the first one's claim.
  Every entry reaches each registered controller but its requester, in order,
  at that controller's next sequence_id: 26 jobs, C's at sequence_id 0 to 8 and
  D's at 0 to 16.
- **CQ2b** as CQ1b, through 17 rounds: between two rounds the queue alone holds
  `amap_busy_o` for a clock.

**OC1** closes every run of every build (the default run, FT, PT/CK/PD/CA, SC1
and SC2). `occupancy.vlt` makes `cmdq_count_r` and `cmdq_nempty_r` readable, the
bench samples both after each clock edge, and OC1 requires `cmdq_nempty_r ==
(cmdq_count_r != 0)` at every edge.

CQ1 to CQ2b pass on the RTL before the flop (`main` `08ae0013`, whose emission
pick and `amap_busy_o` compared the count) with the same jobs and the same
`amap_busy_o` trace, so they describe behaviour that did not change. There they
fail with `amap_busy_o` without the count term (CQ1b and CQ2b) and with a claim
that empties the count (CQ2).

Mutation record (planted by `tb/pp_top/notify_mutants.py`, which runs `make run`,
or for the last four arms `make identify`, `make interfaces`, `make collision` and
`make failure-window`, here; all eleven KILLED). The named checks are the ones each
arm must fail; the failing checks are what it failed:

| Mutant | Planted | Named | Failing checks |
|---|---|---|---|
| `occupancy_pop_keeps_set` | a claim without a push leaves the occupancy set | OC1, CQ1, CQ2 | 7: DR2, DR2b, CQ1, CQ1b, CQ2, CQ2b, OC1 |
| `occupancy_push_unseen` | a push does not set the occupancy | OC1, CQ1, CQ1b, CQ2, CQ2b | 9: DR2, DR2b, LF2, LF3, CQ1, CQ1b, CQ2, CQ2b, OC1 |
| `occupancy_pop_one_late` | a claim without a push clears the occupancy one entry late | OC1, CQ1, CQ2 | 5: DR2, DR2b, CQ1, CQ2, OC1 |
| `occupancy_cleared_beside_push` | a claim beside a push clears the occupancy, so CQ1's last entry is stranded | OC1, CQ1 | 2: CQ1, OC1 |
| `occupancy_not_busy` | `amap_busy_o` drops the occupancy term | CQ1b, CQ2b | 2: CQ1b, CQ2b |
| `occupancy_reset_set` | the reset sets the occupancy | OC1 | 32: TS1, TS3, TW1, TW2, DR1, DR1b, DR2, DR2b, DR3, LF1, LF2, LF3, LC0, LC1 (8), LC2 to LC7, CQ1, CQ1b, CQ2, CQ2b, OC1 |
| `occupancy_pop_clears` | a claim without a push clears the occupancy, so CQ2's queue is stranded | OC1, CQ2 | 2: CQ2, OC1 |
| `occupancy_reset_set_identify` | as `occupancy_reset_set`, graded by FT's run | OC1 | 1: OC1 |
| `occupancy_reset_set_interfaces` | as `occupancy_reset_set`, graded by the third build | OC1 | 13: PT4, PT6, CK1, CK2, CK3, CK4, CK5, PD3, CA2, CA1b, CA3, CA4, OC1 |
| `occupancy_reset_set_collision` | as `occupancy_reset_set`, graded by SC1's run | OC1 | 2: SC1, OC1 |
| `occupancy_reset_set_failure` | as `occupancy_reset_set`, graded by SC2's run | OC1 | 2: SC2, OC1 |

`occupancy_push_unseen` and `occupancy_reset_set` also fail section LF's checks
(issue #186): LF holds a SET_NAME round, which goes through the queue.
`occupancy_reset_set` also fails every section LC check but its REGISTERs (15,
issue #186 round 2): after each warm reset the set occupancy makes the pick
claim an empty queue entry before the lock round, so the first job is not a
lock job.
