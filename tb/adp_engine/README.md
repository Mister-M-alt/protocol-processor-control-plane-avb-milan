<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# adp_engine — KL_adp_engine suite

Proves the ADP engine (`hdl/adp/KL_adp_engine.sv`) against
[04](../../docs/architecture/04_adp_engine.md) in full: `make` = build + run,
exit 0 = PASS, 632 checks. `make mutants` runs the checked-in mutation
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
re-advertise + GPTP_GM_CHANGED tick + gm sampled at build, LINK_DOWN with
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

Interop note: the available_index rule implements the DOC (04 §5 /
IEEE §6.2.2.15) and **diverges from the reference platform's
every-ADPDU increment** — adjudication against live controllers
(Hive / la_avdecc) is required before cutover; the engine banner carries
the same warning.

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
the tree with `git apply`, runs one suite target there, and requires the
named check to fail in a completed simulation. It reads logs only, never
production source. A positive control of every (suite, target) pair runs
first. `MUTANT_OUTPUT` (default `/tmp/adp-mutants`) receives one log per arm.
Counts below were taken on 2026-09-29 with Verilator 5.050.

| Arm | Suite, target | What is broken | Failing checks |
|---|---|---|---|
| `cfg-read-live` | adp_engine | the builder reads `current_cfg_i` live instead of the index sampled at build (the shipped form before issue #40) | 1: P11c, the frame carries `0104`, the high byte of `0102` and the low byte of `0304` |
| `cfg-dependent-field` | adp_engine | identify_control_index made configuration-dependent (XOR with the index) | 10: P11b bytes 66 and 67 move with the configuration, and every byte-exact frame (P2, P5, P7, P11a, P11d, P11e) |
| `cfg-dependent-field-top` | pp_top `adp-config` | the same patch, at the processor | 5: AD1, AD2 and AD3 (a byte outside 50..53 and 64..65 moved), AD4 |
| `cfg-frozen-at-top` | pp_top `adp-config` | the top feeds the engine `current_cfg_i` again (the wiring before issue #40) | 3: AD2 (the index stays 1 after SET_CONFIGURATION(0)), AD4 |
| `cfg-overlay-only` | pp_top `adp-config` | the top feeds the overlay with no image-default fallback | 2: AD1 and AD1b carry the overlay's reset 0, not the image's 1 |
| `cfg-nonzero-for-valid` | pp_top `adp-config` | the overlay counts as set when it is non-zero, instead of by its valid flag | 2: AD2, a SET to 0 advertises the default 1 |
| `cfg-valid-not-sticky` | pp_top `adp-config` | the engine's valid flag is high only in the write cycle | 3: AD2, AD4 |
| `cfg-valid-any-selector` | pp_top `adp-config` | the valid flag is set by a write to any row of the store | 1: AD1b, SET_CLOCK_SOURCE makes the advert carry 0 |
| `gate-enable-dropped` | adp_engine | `entity_enable_i` removed from the DOWN-exit condition (issue #41's mutant) | 25: all four P12 checks (2 draw requests, 5 timer operations, 2 frames, 32,800 clocks out of DOWN), and 21 later checks the premature adverts displace (P1 to P5) |
| `gate-enable-dropped-top` | pp_top `adp-config` | the same patch, at the processor | 3: AD0 (an ADPDU on the wire and 84 of 84 samples out of DOWN), AD1b (the index runs one ahead). In the full default pp_top run these are the only 3 failures of 7,766: S0's 20 ms window and S3's queue check stay green, because the premature draw outlasts S0 to S3 and the early advert then passes S3 as the first one |

Known limits (honestly): the suite runs the shipping shape (1 interface,
8 sinks) only; the timer service and slot pools are modeled, not
instantiated (their own suites own those RTL contracts); event-port
ordering between a NOADP expiry and a same-sink iteration in flight is
not exercised (rare, ACMP re-probes either way).
