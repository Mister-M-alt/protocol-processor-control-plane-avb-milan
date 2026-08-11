<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# pp_top — the processor top, end-to-end wire truth

Builds `protocol_processor_top` (every landed module of the tree wired:
validator + replicated RX pools + normalizer + dispatch + ADP engine + ACMP
listener/talker + SRP engine behind `KL_mrp_strip` + TX pool/arbiter + the
ACMP Ethernet-prepend shim + timer/PRNG muxes + scoreboard, event router,
originator, trace ring, side port, NVM shadow + port) under `pp_top_wrap`
and drives it ONLY through the top's external contract: one MAC byte stream
in, one MAC byte stream out, the side-port host face, the SRP service face
and the NVM device face. Time is compressed to 1 ms = 100 clk (the 89-slot
deadline sweep still fits a ms tick), so every window measured below is the
REAL timer/PRNG path.

Expectations are independent C++ builders/parsers from the doc byte
offsets — F04.5 ADPDU, F05.13 Milan ACMPDU, 802.1Q §10.8/§35.2.2 MRPDU BNF,
Milan §4.3.3.2 Σ-slope — never DUT logic.

`make` — exit 0 = PASS; tally `73 checks: 73 PASS, 0 FAIL`.

## What it proves

- **R** boot restore over a blank NVM device: all 8 BINDING regions read,
  `restore_done` without `restore_fail`.
- **S0/S1** quiescence + snapshot identity; SRP bring-up: the FIRST MSRP
  frame is the Domain default declaration `New {6,3,2}`, byte-exact.
- **S2** `DECLARE_TALKER` (svc face) → Σ-slope admission equals the
  independent Milan model (sum, granted, admitted, no over-limit) → Talker
  Advertise `New` AND MVRP VID `New` byte-exact on the MAC stream.
- **S3** entity enable → 82 B ENTITY_AVAILABLE byte-exact (aidx 0) inside
  the T-ADP-DELAY-START window; re-advertise with aidx 1 at the T-ADP-ADV
  5 s + 0-4 s anti-storm cadence.
- **S4** ENTITY_DISCOVER in → delayed byte-exact response at the running
  available_index; zero front-end drops.
- **S5** host face: ctrl scratch RW, status flags, firmware-window error
  when disabled, snapshot reads clean.
- **S6** BIND_RX in → byte-exact BIND_RX_RESPONSE; talker ENTITY_AVAILABLE
  in → discovery event through the router (trace-ring record checked via
  the host face) → byte-exact PROBE_TX_COMMAND inside the T-ACMP-DELAY
  window, Ethernet header prepended by the top (lane 2 shim).
- **S7** TX interleave: ADP response + ACMP GET_RX_STATE response + SRP
  Talker Advertise pushed together — each frame byte-exact and whole, all
  three arbiter lanes take grants.
- **S8** certified two-class Domain arrival (FirstValue {5,2,5}, nov 2)
  adopts {3,5} and re-declares `Lv{6,3,2}+New{6,3,5}` byte-exact; listener
  READY end-to-end (class-D snapshot + Listener Ready `New` byte-exact +
  TK_ATTR_REGISTERED trace record).
- **S9** the S6 binding commits through the debounced NVM shadow: framed
  F07.8 record (magic 0x1722) carrying the bound talker EID at the device
  face.

## Snapshot window map (side port 0x20000, implemented by the top)

| word | content |
|---|---|
| 0 | magic 0x4B4C5050 "KLPP" |
| 1 | shape {SI, SO, RX slots, TX slots} |
| 2 | now_ms |
| 3 | flags {…, nvm_alarm, over_limit, adopted, seeded, link, enable} |
| 4-6 | RX front-end drop counters + pool overrun |
| 7-9 | dispatch levels + stall counters + hdr-latch drops |
| 10 | class A {prio, vid} |
| 11 | Σ granted slope bps |
| 12 | {sr_admitted, active, tk_reg_state} |
| 13 | {tk_decl_state, lstn_reg_state} |
| 14 | {lstn_decl_state, vid_active} |
| 15 | {scoreboard holds/full/barrier, trace wr_count} |
| 16-23 | acc_latency[sink] |
| 24-31 | arm/mrp/txreq drop counters, slots free, lane grant counts, bound mask, granted[0], adv SM |

Trace window 0x40000: record = 4 words, lane 0 = now_ms, lane 1 =
{source, flags, payload} (event-router consumer glue).

## Mutation record (backup / sed / run / restore)

| # | what was broken | result |
|---|---|---|
| M1 | `KL_mrp_strip` strips 13 bytes instead of 14 (`body_w` compare 4'd14→4'd13) | 9 FAIL — S8 Domain adoption, listener READY, class-D: the SRP RX seam is load-bearing |
| M2 | ACMP prepend shim EtherType 0x22F0→0x22F1 | 8 FAIL — S6/S7 every ACMP wire check: the prepended header is what the wire sees |
| M3 | steer prefetch reads the addressed EID at PDU offset 27 instead of 28 | 16 FAIL — S6/S7/S9 the listener silently ignores mis-addressed heads (and the binding never commits): the target_eid rewrite is the real multicast discriminator |

All three bite; originals restored; suite back to 73/73.

## Recorded seams and honest limits

- The validator's V9 pass-through has NO msrp/mvrp select — `KL_mrp_strip`
  derives it from the EtherType bytes it strips (V9 already enforced the
  DA/EtherType pairing).
- The validator's F03.4 `target_eid` for ACMP is the @4 stream_id; both
  ACMP engines discriminate on the ADDRESSED entity id — the top's steer
  prefetch rewrites the head from the slot bytes (doc conflict reported;
  03 §4 says only "as applicable").
- S6 allows the probe to RACE the trace record: a short bind-armed
  T-ACMP-DELAY draw can put the probe on the wire around the discovery
  walk, so the trace check polls a bounded 500 ms window instead of
  reading once.
- MSRP byte-exact checks run under `la_guard()`: the PRNG-drawn 10-15 s
  LeaveAll would otherwise fold the expected vector into an LA PDU. The
  MVRP byte-exact check runs early (before the first LeaveAll) because an
  idle MVRP participant latches an expired LeaveAll until its next tx
  opportunity — there is no clean later window by construction.
- The AECP pop face is tied `ready = 0` (P4 µCPU seam); AEM frames would
  park in the dispatch queue and are deliberately not sent.
- The NVM device model is blank flash (reads answer 0xFF): a record failing
  the F07.8 magic/layout check is SKIPPED by the shadow, which is the
  documented no-saved-binding path. Torn-stream restore aborts are covered
  by the `acmp_nvm` suite, not here.
- The wrap exposes observe-only cross-module taps (`dbg_*`) used during
  bring-up; the checks themselves read only wire frames + the host face.
