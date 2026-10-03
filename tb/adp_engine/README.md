<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# adp_engine — KL_adp_engine suite, with the MTXW walk of F04.2 and F04.3

Proves the ADP engine (`hdl/adp/KL_adp_engine.sv`) against
[04](../../docs/architecture/04_adp_engine.md) in full: `make` = build + run,
exit 0 = PASS, 1330 checks. `make mutants` runs the checked-in mutation
campaign below.

The top (`tb_adp_top.sv`) is pure wiring: the engine plus the **real
`KL_pp_prng`** (draw port tapped), so the two DISTINCT delay-draw kinds are
graded against the actual rejection sampler. Everything else is an
independent C++ model, never DUT logic: the timer-service arm/expiry
contract (absolute-ms deadlines, harness-owned `now_ms`), the
`KL_pp_tx_slots` alloc/write/commit protocol (one-cycle grant a cycle after
the request, grant withholding, in-slot write bounds), the `KL_pp_rx_slots`
sync-read/free contract, the dispatch valid/ready pop face (txn packed by an
independent 393-bit `pp_txn_t` codec), and an 82-byte wire-frame builder
written straight from the 04 §3 field-sourcing table + F04.5 offsets.

Covered: byte-exact ADPDU for **both** message types (all 82 wire bytes,
entity_id at wire byte 18, F04.6 caps 0x0000C588, cdl 56, Δ5 valid_time
10/0); the F04.2 advertise SM — startup kind-1 draw, T-ADP-ADV 5 s re-arm
per send, DISCOVER (eid 0 / own / foreign / ignored-in-DELAY), GM_CHANGE
re-advertise + gm sampled at build, LINK_DOWN with
**no** departing then kind-2 re-entry, disable ⇒ ENTITY_DEPARTING; the 04 §5
available_index doc rule (0 at power-up, ++ after each AVAILABLE, departing
carries the pre-reset value then resets to 0); statistical draw separation
(40 kind-1 ≤ 2000, 40 kind-2 ≤ 4000, kind-2 max > 2000, maxima differ); the
F04.3 per-sink discovery SM — gm/domain guard both ways, multi-sink fan-out
once each, restart pair (DEPARTED then DISCOVERED) on stale index,
stale+foreign-GM departure, interface_index mismatch ignored (record
untouched), DEPARTING per matched sink, T-ADP-NOADP = rx valid_time (20 s
and 10 s arms) with aging ⇒ DEPARTED, unbind disarm + cancel; and the pool
protocol invariants accumulated over the whole run (never oversize, never a
write before grant, alloc_req always one cycle, one TX request per
committed 82-byte frame).

P12 (issue #41, REQ-ADP-006, Milan §5.6.1), which runs right after P0: with
`entity_enable_i` low the link rises, stays up for 4100 ms of modeled time,
bounces for 20 ms and stays up for another 4100 ms. Each window is longer than
the whole T-ADP-DELAY span (0..4000 ms, Milan §5.6.3.5.3), and the timer
service is modeled end to end inside it (an armed slot fires by itself when
`now` passes its deadline), so a gate that let LINK_UP through would reach the
wire. Graded: no draw request at the PRNG tap, no timer arm or cancel, no
committed frame or TX request, and `dbg_adv_state` DOWN in every clock. The
PRNG's seed latch is checked too, so the link rise demonstrably reached the
draw path.

P11 (issue #40, REQ-ADP-005, Milan §5.6.2 note with IEEE §6.2.2.18):
`current_cfg_i` changes between two adverts and the second is compared with
the first byte by byte. Only wire bytes 64..65 move to the new index, besides
available_index (50..53), which moves by its own +1. The index also moves
right after wire byte 64 is written: the frame carries the index sampled when
the build began, never the high byte of one index and the low byte of the
other. ENTITY_DEPARTING carries the current index as well. The processor-level
half, where SET_CONFIGURATION moves the index, is `tb/pp_top` section AD.

## The MTXW walk (P13, issue #85)

The matrix-walker category of [09 §3](../../docs/architecture/09_verification.md#3-test-categories)
for both ADP state machines, in the shape `tb/acmp_listener` walks F05.3: every
cell of two tables is driven from an **independent C++ transcription of Milan
v1.2**, never of the RTL, through legitimate stimulus only (no state backdoor),
and every observable is compared with the cell. The walk ends with
`CHECK(adv_cells == 45)`, `CHECK(disc_cells == 33)` and a check that each of
F04.3's eight arcs is a walked cell that passed.

Every cell cites the Milan v1.2 clause that rules its next state, frame and
timer action, and the IEEE 1722.1-2021 clause it replaces or follows. 04
derives each pair: [F04.7](../../docs/architecture/04_adp_engine.md#fig-04-advcells)
for the advertise cells, [F04.8](../../docs/architecture/04_adp_engine.md#fig-04-discarcs)
for the discovery arcs. The C++ tables carry both into every failure message,
and two checks count the cells that cite both (45 of 45, and 33 of 33, where
IEEE has no counterpart of a sink binding).

Cell classes: **N** a transition clause; **I** `-` in the table, ignored and
proven inert; **S** `x` in the table but physically reachable as a stray (an
expiry of a slot whose cancel lost the race in the top's arm queue), injected
and proven inert; **C** `x` and impossible by construction, where the
precondition that rules the event out is checked instead (the link level, the
enable level, or the T-ID the shared slot carries in that state).

**F04.2, the advertise SM** (Milan Table 5.51 and §5.6.3.5; the DISCOVER split
of §5.6.3.1; a not-started column for the §5.6.1 boot gate, where the hardware
holds DOWN; the F04.2 DELAY walked in both hardware phases). 45 cells:
12 N, 20 I, 6 S, 7 C.

| Event | NOT STARTED (§5.6.1) | DOWN | WAITING | DELAY, draw in flight | DELAY, timer armed |
|---|---|---|---|---|---|
| RCV_ADP_DISCOVER (entity_id 0) | I | I | N §5.6.3.5.4 | I | I |
| RCV_ADP_DISCOVER (own entity_id) | I | I | N §5.6.3.5.4 | I | I |
| ENTITY_DISCOVER (another entity_id) | I | I | I §5.6.3.1 | I | I |
| TMR_ADVERTISE | S | S | N §5.6.3.5.5 | S | C (slot holds T-ADP-DELAY) |
| TMR_DELAY | S | S | C (slot holds T-ADP-ADV) | S | N §5.6.3.5.9 |
| LINK_UP | I | N §5.6.3.5.3 | C (link up) | C | C |
| LINK_DOWN | I | C (link down) | N §5.6.3.5.6 | N §5.6.3.5.10 | N §5.6.3.5.10 |
| GM_CHANGE | I | I | N §5.6.3.5.7 | I | I |
| SHUTDOWN (enable falls) | C (enable low) | I | N §5.6.3.5.8 | N §5.6.3.5.11 | N §5.6.3.5.11 |

The tables below give each cell's class and Milan clause; F04.7 adds its
IEEE clause. Each cell grades the advertise state after it, the PRNG draw requests (an N
cell entering DELAY draws exactly one kind-2 T-ADP-DELAY, 0..4000 ms), every
timer operation on the shared slot (the arm is the last one, at `now` + the
draw, or + 5000 ms into WAITING; a cancel wherever the clause says Stop, none
where the slot is untouched; nothing on any other slot), the committed frame
byte-exact (ENTITY_AVAILABLE on TMR_DELAY, ENTITY_DEPARTING with the
pre-reset index on SHUTDOWN, nothing otherwise), available_index (+1, reset to
0, or unchanged), no discovery event, and the RX-slot free of each DISCOVER.
The engine keeps no GPTP_GM_CHANGED tick (the counter is the integrator's,
processor issue #44 and 02 §4.6). The five cells
issue #85 named are DOWN x {DISCOVER, GM_CHANGE, SHUTDOWN} (inert: no draw,
no frame, no timer operation) and DELAY x
{LINK_DOWN (timer cancelled, no DEPARTING), SHUTDOWN (DEPARTING sent, index
reset)}, the last two in both DELAY phases. In the draw phase the event is
applied while the draw request is on the port and the PRNG has not taken it,
so it acts strictly before the draw is delivered (a DISCOVER needs two clocks
to classify, and at the old entry point it acted on the delivery edge, where
`walk-delay-answers-discover` stayed green in that column).

**F04.3, the talker-discovery SM of one sink** (Milan Table 5.54 and
§5.6.4.5, each guard of those steps its own row; the unbound column of
§5.6.4.1, where only bound sinks process ADP; bind and unbind from 04 §6.2).
33 cells: 13 N, 15 I, 2 S, 3 C, rotated over all eight sinks, each bound to a
talker of its own. TK_NOT_DISCOVERED is entered by an unbind and re-bind after
a discovery, so it carries a stale saved record (index 500, interface 0) that
§5.6.4.5.1 must not compare against.

| Event | unbound (§5.6.4.1) | TK_NOT_DISCOVERED | TK_DISCOVERED |
|---|---|---|---|
| AVAILABLE, GM and domain match, index > last | I | N §5.6.4.5.1: DISCOVERED | N §5.6.4.5.2 step 3: no event, re-arm |
| AVAILABLE, match, index <= last | I | N §5.6.4.5.1 (no index test) | N steps 2a, 2c, 3: DEPARTED then DISCOVERED |
| AVAILABLE, GM mismatch, index > last | I | I §5.6.4.5.1 step 1 | N step 3 (no GM test on a fresh index) |
| AVAILABLE, GM mismatch, index <= last | I | I | N steps 2a, 2b: DEPARTED, timer stopped |
| AVAILABLE, domain mismatch, index <= last | I | I | N step 2b: DEPARTED, timer stopped |
| AVAILABLE, interface_index differs | I | N §5.6.4.5.1 (no interface test) | I §5.6.4.5.2 step 1 |
| DEPARTING, interface_index matches | I | I | N §5.6.4.5.3: DEPARTED, timer stopped |
| DEPARTING, interface_index differs | I | I | I §5.6.4.5.3 step 1 |
| TMR_NO_ADP | S | S | N §5.6.4.5.4: DEPARTED |
| UNBIND | C | N: disarmed silently | N: disarmed, timer stopped |
| BIND | N: F04.3's entry arc | C | C |

Each cell grades the sink's bound and discovered bits, the exact event
sequence on the class-C port (sink and order), the sink's T-ADP-NOADP
operations (one arm at the received valid_time, 20 s here; one cancel; or
none; nothing on another slot), that nothing is transmitted, and the RX-slot
free. F04.3's eight arcs map onto the cells BIND x unbound; match x NOT;
GM mismatch x NOT; fresh match, restart match, stale GM mismatch, DEPARTING
and TMR_NO_ADP x DISCOVERED (F04.8 lists each with its clauses). Each arc has a
planted mutant of its own in the campaign below (the `arc-` arms), which must
fail that arc's own check; the GM and domain guard is planted at both of its
sites (TK_NOT_DISCOVERED, and the restart pair), the restart detector as an
off-by-one at its boundary (an index equal to the last one).

Interop note, **still open** (issue #85 item 4, which needs a live
controller and belongs to a bench lane): the available_index rule implements
the DOC (04 §5 / IEEE §6.2.2.15) and **diverges from the reference platform's
every-ADPDU increment**. Adjudication against live controllers (Hive /
la_avdecc) is required before cutover; the engine banner carries the same
warning. The walk grades the doc rule in every cell, so it cannot close this.

Mutation-proven 2026-08-11 (backup/sed/run/restore):
1. merged draw kinds (`ADP_DRAW_KIND_START_C` → `ADP_DRAW_KIND_DELAY_C` in
   the request picker, the review §8 item 5 bug class) — **291 of 533
   fail** (the engine's kind-consistency guard discards the mis-kinded
   draw, so the startup advertise never happens);
2. reference-platform available_index (increment on every ADPDU, no reset
   on DEPARTING) — **2 fail** (P7 reset-to-0 + restart-index-0 frame);
3. ENTITY_DEPARTING queued on link-down (Milan §5.6.3.5.6 violation) —
   **5 fail** (P6 no-departing window + P7 frame mismatches; the P6
   window was widened to 150 idle cycles after this mutant initially
   escaped the too-short 5-cycle window);
4. talker-restart detector deleted (stale index treated as fresh) —
   **1 fail** (P9e restart pair).

## Mutation campaign (`make mutants`)

`mutants.py` applies each reviewed patch in `mutations/` to a scratch copy of
the tree of its own with `git apply`, runs one suite target there, and requires
the named check to fail in a completed simulation. It reads logs only, never
production source. A positive control of every (suite, target) pair runs
first, each in its own copy. `MUTANT_OUTPUT` (default `/tmp/adp-mutants`)
receives one log per arm. `python3 mutants.py --output DIR [--only a,b]
[--jobs N]` runs it directly: `--jobs N` (default 4, the meaning and default of
`tb/pp_top/d3_mutants.py`; `make mutants` runs the default) builds and runs up
to N copies at once, and the results are printed in the table's order whatever
order they finish in.
Counts below were taken on 2026-09-29 with Verilator 5.050, 30 of 30 arms killed.
Measured 2026-10-02 at `85da751` with Verilator 5.050, each run pinned to 4 of
the host's 16 CPUs: `--jobs 1` took 502 s and `--jobs 8` 333 s. Every control
and arm gave the same verdict and the same failing checks in both runs, the
counts below.
Re-run 2026-10-03 at the head of lane P1 (issues #52, #59, #61, #62, #63, #83):
both controls PASS and all 30 arms are KILLED. Four pp_top rows carry that run's
counts, because the lane's AD8 and AD9 fail as AD6 and AD7 do under them.
Re-run 2026-10-04 with Verilator 5.050 at the head of issue #85's lane, which adds
the eight `arc-` arms: both controls PASS and all 38 arms are KILLED, each earlier
arm with the count in its row (169 s at `--jobs 4`). An `arc-` arm must fail its
arc's own check, `P13 F04.3 arc ...`, not only some cell.

| Arm | Suite, target | What is broken | Failing checks |
|---|---|---|---|
| `cfg-read-live` | adp_engine | the builder reads `current_cfg_i` live instead of the index sampled at build (the shipped form before issue #40) | 1: P11c, the frame carries `0104`, the high byte of `0102` and the low byte of `0304` |
| `cfg-dependent-field` | adp_engine | identify_control_index made configuration-dependent (XOR with the index) | 14: P11b bytes 66 and 67 move with the configuration, and every byte-exact frame (P2, P5, P7, P11a, P11d, P11e, and the walk's four frames) |
| `cfg-dependent-field-top` | pp_top `adp-config` | the same patch, at the processor | 9: AD1, AD2 and AD3 (a byte outside 50..53 and 64..65 moved), AD4, and AD6 to AD9 (the first advert after the roll-back, the reset, the corrupt record and the torn read, at configuration 1) |
| `cfg-frozen-at-top` | pp_top `adp-config` | the top feeds the engine `current_cfg_i` again (the wiring before issue #40) | 4: AD2 (the index stays 1 after SET_CONFIGURATION(0)), AD4, AD5 (the restored 0 is not advertised) |
| `cfg-overlay-only` | pp_top `adp-config` | the top feeds the overlay with no image-default fallback | 6: AD1 and AD1b carry the overlay's reset 0, not the image's 1, and so do the first adverts after AD6's roll-back, AD7's reset, AD8's corrupt record and AD9's torn read |
| `cfg-nonzero-for-valid` | pp_top `adp-config` | the overlay counts as set when it is non-zero, instead of by its valid flag | 3: AD2, a SET to 0 advertises the default 1; AD5, so does a restored 0 |
| `cfg-valid-not-sticky` | pp_top `adp-config` | the engine's valid flag is high only in the write cycle | 7: AD2, AD4, AD5 (the restored 0 is not advertised), the flag split from the store's in AD5 and AD6, and AD7's premise (no flag after the SET) |
| `cfg-valid-any-selector` | pp_top `adp-config` | the valid flag is set by a write to any row of the store | 1: AD1b, SET_CLOCK_SOURCE makes the advert carry 0 |
| `cfg-valid-ucpu-bus` | pp_top `adp-config` | the valid flag decodes the µCPU's side of the state-bus selection, so the D3 writer's restore write of the configuration row is not seen (the branch's decode kept as it was before the merge of PR #132) | 3: AD5 (the first advert after the restore carries the image default 1 while GET reads the restored 0), and the flag split from the store's in AD5 and AD6 |
| `cfg-valid-hard-reset` | pp_top `adp-config` | the valid flag resets on the hard reset only, not with the store (so the D3 roll-back does not clear it) | 2: AD6 (after the roll-back the advert carries the overlay's reset 0 while GET reads the image default 1), and the flag split from the store's |
| `cfg-valid-no-reset` | pp_top `adp-config` | the valid flag has no reset, so it survives a reset that clears the row (review R406-1 F-1) | 5: AD7 (after a SUCCESS SET_CONFIGURATION(0) and a reset with nothing to restore, the advert carries the overlay's reset 0 while GET reads the image default 1, and the flag split from the store's), AD6, AD8 and AD9 likewise after the roll-back, the corrupt record and the torn read, and the flag split in AD5 until the restore writes the row (9 since lane P1's AD8 and AD9) |
| `gate-enable-dropped` | adp_engine | `entity_enable_i` removed from the DOWN-exit condition (issue #41's mutant) | 30: all four P12 checks (2 draw requests, 5 timer operations, 2 frames, 32,800 clocks out of DOWN), 21 later checks the premature adverts displace (P1 to P5), and the walk's LINK_UP and GM_CHANGE x NOT STARTED cells |
| `gate-enable-dropped-top` | pp_top `adp-config` | the same patch, at the processor | 3: AD0 (an ADPDU on the wire and 84 of 84 samples out of DOWN), AD1b (the index runs one ahead). At the head of lane P1 it fails 7: those 3 and AD5 to AD8, whose first advert after the reboot never comes in its 2,600 ms wait (the early adverts left during the restore, which the 59 records of the name stage lengthen, and were flushed with it). In the full default pp_top run (7,924 checks after the merge of PR #132) it fails these 3 and D3R14, which also sees an ADPDU before its restore terminal, 4 in all: S0's 20 ms window and S3's queue check stay green, because the premature draw outlasts S0 to S3 and the early advert then passes S3 as the first one |
| `walk-down-answers-discover` | adp_engine | DOWN answers RCV_ADP_DISCOVER (issue #85's named mutation) | 16: both DISCOVER rows x DOWN and x NOT STARTED, each drawing, arming and cancelling where the cell is inert |
| `walk-delay-ignores-link-down` | adp_engine | DELAY ignores LINK_DOWN (issue #85's named mutation) | 4: LINK_DOWN x both DELAY phases stay in DELAY; the armed one keeps its timer |
| `walk-down-answers-gm-change` | adp_engine | DOWN answers GM_CHANGE | 8: GM_CHANGE x DOWN and x NOT STARTED |
| `walk-down-shutdown-departs` | adp_engine | SHUTDOWN in DOWN queues an ENTITY_DEPARTING | 1: SHUTDOWN x DOWN sends a frame |
| `walk-delay-answers-discover` | adp_engine | DELAY answers RCV_ADP_DISCOVER | 9: both DISCOVER rows x both DELAY phases, and P4c |
| `walk-delay-shutdown-silent` | adp_engine | SHUTDOWN sends ENTITY_DEPARTING from WAITING only | 4: SHUTDOWN x both DELAY phases send nothing and keep the index |
| `walk-stale-draw-arms` | adp_engine | a delay draw delivered after the SM left DRAW still arms | 4: LINK_DOWN and SHUTDOWN x DELAY (draw in flight) end in DELAY with an arm |
| `walk-departing-keeps-index` | adp_engine | available_index not reset after ENTITY_DEPARTING | 6: SHUTDOWN x WAITING and both DELAY phases, P7 twice, P11e |
| `walk-foreign-discover-answered` | adp_engine | an ENTITY_DISCOVER for another entity_id is answered | 8: foreign DISCOVER x WAITING, P4d twice, P5 twice |
| `walk-link-down-keeps-timer` | adp_engine | LINK_DOWN from WAITING/DELAY leaves the timer running | 3: LINK_DOWN x WAITING and x DELAY (timer armed), P6 |
| `disc-fresh-checks-gm` | adp_engine | a fresh index is refused on a GM mismatch (a test §5.6.4.5.2 does not make) | 3: GM mismatch, index > last x DISCOVERED |
| `disc-not-discovered-checks-index` | adp_engine | TK_NOT_DISCOVERED compares the index with the stale record | 14: index <= last and interface differs x NOT, and every later cell entered through a discovery at index 700 over a higher stale record, including the NOADP arc |
| `disc-not-discovered-checks-interface` | adp_engine | TK_NOT_DISCOVERED compares interface_index with the stale record | 3: interface differs x NOT |
| `disc-restart-not-rediscovered` | adp_engine | the restart pair loses its DISCOVERED | 4: index <= last x DISCOVERED, its arc and the arc count, P9e |
| `disc-departing-ignores-interface` | adp_engine | DEPARTING departs whatever its interface_index | 3: DEPARTING, interface differs x DISCOVERED |
| `disc-stray-noadp-departs` | adp_engine | a T-ADP-NOADP expiry departs a sink that is not discovered | 2: TMR_NO_ADP x unbound and x NOT |
| `disc-unbind-keeps-timer` | adp_engine | unbinding a discovered sink leaves its T-ADP-NOADP running | 2: UNBIND x DISCOVERED, P9k |
| `arc-bind-keeps-discovered` | adp_engine | F04.3 entry arc: an unbind no longer clears the discovered state, so a sink bound again starts in TK_DISCOVERED | 46: arc 1 (BIND x unbound ends discovered), arcs 2 and 3 and the arc count, P9k twice, and every cell entered through the unbind of a discovered sink |
| `arc-discover-no-noadp-arm` | adp_engine | TK_NOT_DISCOVERED to TK_DISCOVERED arms no T-ADP-NOADP (Milan §5.6.4.5.1 step 3) | 13: arcs 2 and 8 and the arc count, the three discoveries x NOT, TMR_NO_ADP x DISCOVERED (no timer left to expire), P9a, P9c twice, P9j |
| `arc-not-discovered-no-guard` | adp_engine | TK_NOT_DISCOVERED takes an ENTITY_AVAILABLE whatever its grandmaster and domain (the §5.6.4.5.1 step 1 guard) | 14: arc 3 and the arc count, the three GM and domain mismatch cells x NOT (state, event, timer each), P9d three times |
| `arc-fresh-no-rearm` | adp_engine | a fresh index in TK_DISCOVERED does not restart T-ADP-NOADP (§5.6.4.5.2 step 3) | 5: arc 4 and the arc count, both index > last cells x DISCOVERED, P9b |
| `arc-restart-detector-off-by-one` | adp_engine | the restart detector takes an index equal to the last one as fresh (`>=` for `>`) | 10: arcs 5 and 6 and the arc count, and the restart, GM mismatch and domain mismatch cells x DISCOVERED, each walked at an index equal to the last |
| `arc-restart-skips-guard` | adp_engine | a stale index restarts the talker whatever its grandmaster and domain (the §5.6.4.5.2 step 2b guard) | 12: arc 6 and the arc count, the GM and domain mismatch cells x DISCOVERED (event pair, state, timer), P9f three times, P9f2 |
| `arc-departing-silent` | adp_engine | an ENTITY_DEPARTING departs without EVT_TK_DEPARTED (§5.6.4.5.3 step 3) | 4: arc 7 and the arc count, DEPARTING x DISCOVERED, P9h |
| `arc-noadp-expiry-silent` | adp_engine | a T-ADP-NOADP expiry departs without EVT_TK_DEPARTED (§5.6.4.5.4) | 4: arc 8 and the arc count, TMR_NO_ADP x DISCOVERED, P9i |

Known limits (honestly): the suite runs the shipping shape (1 interface,
8 sinks) only; the timer service and slot pools are modeled, not
instantiated (their own suites own those RTL contracts); event-port
ordering between a NOADP expiry and a same-sink iteration in flight is
not exercised (rare, ACMP re-probes either way); the walk takes one event
per cell, so two events in the same clock are not walked beyond the
draw-phase cells above; and the available_index interop note stays open
until a live controller adjudicates it (issue #85 item 4).
