<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# pp_top — the processor top, end-to-end wire truth

Builds `protocol_processor_top` (every landed module of the tree wired:
validator + replicated RX pools + normalizer + dispatch + ADP engine + ACMP
listener/talker + SRP engine behind `KL_mrp_strip` + TX pool/arbiter + the
ACMP Ethernet-prepend shim + timer/PRNG muxes + scoreboard, event router,
originator, trace ring, side port, NVM shadow + port) under `pp_top_wrap`
and drives it ONLY through the top's external contract: one MAC byte stream
in, one MAC byte stream out, the side-port host face, the SRP service face,
the NVM device face and the descriptor-image memory master. Time is compressed to 1 ms = 100 clk (the 89-slot
deadline sweep still fits a ms tick), so every window measured below is the
REAL timer/PRNG path.

Expectations are independent C++ builders/parsers from the doc byte
offsets — F04.5 ADPDU, F05.13 Milan ACMPDU, 802.1Q §10.8/§35.2.2 MRPDU BNF,
Milan §4.3.3.2 Σ-slope — never DUT logic.

`make` — exit 0 = PASS; tally `179 checks: 179 PASS, 0 FAIL`.

## What it proves

- **A** **READ_DESCRIPTOR end to end** (06 §6.1, 07 §3.3) — the seam this
  suite used to stop at. A real AEM command on the MAC byte stream comes back
  as a byte-exact AECPDU carrying a descriptor that lives in MAIN MEMORY,
  fetched over the top's read-only memory master from a latency-injecting
  DRAM model (31-clock first-word latency, never zero):
  - **A1** ENTITY descriptor, whole 354-byte frame byte-exact against an
    independent IEEE §7.2.1 builder; command/response counters move once; and
    **exactly ONE memory burst per command** — the line buffer's whole purpose
    is that a descriptor costs one memory latency, not one per byte.
  - **A2** a bad `descriptor_index` and an unknown `descriptor_type` answer
    `NO_SUCH_DESCRIPTOR` with the 4-byte {type, index} stub of IEEE §7.4.5.
  - **A3** a bad `configuration_index` answers `BAD_ARGUMENTS` (06 §6.1), not
    `NO_SUCH_DESCRIPTOR`.
  - **A4** a 78-byte CLOCK_DOMAIN — a length that is NOT a multiple of 8, so
    `COPY_BUFFER` has to stop mid-lane; a whole-lane advance would put 2 bytes
    of the next descriptor on the wire and lie about `control_data_length`.
  - **A5/A6/A7** an unimplemented opcode answers `NOT_IMPLEMENTED` with the
    command ECHOED (F06.14 / IEEE §9.3.5.3.3) — never silence, never a
    malformed frame; IDENTIFY_NOTIFICATION as a COMMAND answers
    `BAD_ARGUMENTS` (IEEE §7.4.39.2 beats §9.3.5.3.3); a truncated
    READ_DESCRIPTOR answers `BAD_ARGUMENTS` rather than locating whatever
    followed the header.
  - **A5b** the NOT_IMPLEMENTED response is sized by ITS OWN command, swept
    over payloads of 0, 4, 8, 16 and 72 octets and two opcodes Table 7-140
    leaves unassigned: `control_data_length` is read off the wire (not
    compared to the builder, which would share any bug) and must be 12 + the
    command's payload, the echoed bytes are a non-zero pattern, and the frame
    is the padded 60 octets only where the payload is genuinely short. A5
    alone proves one 4-byte case, which a length stuck at 4, an echo of zeros
    or a length held over from the previous command all survive; a live Hive
    4.3.1 session reported "Incorrect payload size" against exactly this
    class (see 06 §8.1 for who was right).
  - **A8/A9** a command addressed to another `entity_id` and an AECP RESPONSE
    arriving as input are both dropped and counted — answering a response is
    how a control plane builds a storm.
  - **A10/A11** three back-to-back commands each echo their own
    `sequence_id`; the snapshot window publishes the counters and image-valid.
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
- **S10** the `maap` face (02 §4.2), which the top publishes because 01 §3
  puts address allocation in the integrating fabric. Run in two halves. With
  NO allocator (`maap_req_ready_i` 0 for the whole run above): the port is
  seen OFFERING requests, nothing is accepted, `acmp_declaring_o` is 0, and —
  the regression — a GET_TX_STATE_COMMAND is still answered byte-exact, plus
  a PROBE_TX answered byte-exact TALKER_DEST_MAC_FAILED. Before the accept
  window existed, one unaccepted allocation parked the single talker walker
  forever and neither answer ever came. With the allocator on: the ALLOC_DA
  is accepted for source 0, `acmp_declaring_o[0]` is observed rising 0 -> 1,
  the granted address is what the next GET_TX_STATE_RESPONSE carries, and the
  same address appears as the dest MAC of the Talker Advertise on the MSRP
  wire — MAAP -> DA gate -> ACMP answer -> SRP declaration, end to end.

## Snapshot window map (side port 0x20000, implemented by the top)

The map moved out of this file. It is a product contract, not a testbench note, and it is
now maintained word by word and bit by bit in the
[operator guide](../../docs/guides/operator.md#5-the-snapshot-window-word-by-word), with
the window list in [07 §5.5](../../docs/architecture/07_memory_maps.md). This suite reads
words 0, 3, 32, 33 and 34 as part of scenarios S5 and A11, and words 35 and 36 in B6
through B9.

Trace window 0x40000: record = 4 words, lane 0 = now_ms, lane 1 =
{source, flags, payload} (event-router consumer glue).

## Mutation record (backup / sed / run / restore)

| # | what was broken | result |
|---|---|---|
| M1 | `KL_mrp_strip` strips 13 bytes instead of 14 (`body_w` compare 4'd14→4'd13) | 9 FAIL — S8 Domain adoption, listener READY, class-D: the SRP RX seam is load-bearing |
| M2 | ACMP prepend shim EtherType 0x22F0→0x22F1 | 8 FAIL — S6/S7 every ACMP wire check: the prepended header is what the wire sees |
| M3 | steer prefetch reads the addressed EID at PDU offset 27 instead of 28 | 16 FAIL — S6/S7/S9 the listener silently ignores mis-addressed heads (and the binding never commits): the target_eid rewrite is the real multicast discriminator |
| M4 | `KL_acmp_talker` S_EV_MAAP loses its timeout exit (the deadlock restored) | 5 FAIL — S10: with no allocator the talker walker never consumes another command, so neither ACMP answer reaches the wire |
| M5 | the top re-ties `.maap_req_ready_i (1'b0)` on the talker instance | 6 FAIL — S10: no grant, no gate, no declared DA on the SRP wire. The port is load-bearing, not decoration |

All five bite; originals restored; suite back to green. The counts above were
taken when the suite stood at 86 checks; scenario A and section B have since
been added, so re-run a mutation before quoting its blast radius.

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
- The AECP pop face is still exposed and still tied `ready = 0` here: the
  AECP head is now drained by `KL_aecp_engine` INSIDE the top, and the port
  is an additional, optional consumer (see its banner).
- Scenario A is the only one that touches the descriptor memory. The image is
  loaded into the DRAM model BEFORE reset, exactly as software does before
  `entity_enable`; the "software has not loaded it" and "no bridge at all"
  arms live in the `desc_store` suite, which owns that face.
- The `A` expectations are byte builders from the IEEE §9.3.1 AECPDU and
  §7.2 descriptor field offsets plus the documented image layout — nothing in
  them comes from the DUT or from `gen_desc_image.py`'s output.
- The S10 allocator is a harness model of the 02 §4.2 op semantics only
  (accept a request, answer once, hand back an address). It proves the FACE
  and the address flow through this processor — never MAAP itself: the
  probe/defend/announce state machine of IEEE 1722 Annex B lives in the
  integrating fabric, outside this repo.
- The NVM device model is blank flash (reads answer 0xFF): a record failing
  the F07.8 magic/layout check is SKIPPED by the shadow, which is the
  documented no-saved-binding path. Torn-stream restore aborts are covered
  by the `acmp_nvm` suite, not here.
- The wrap exposes observe-only cross-module taps (`dbg_*`) used during
  bring-up; the checks themselves read only wire frames + the host face.

## Section B — the response buffer lives in main memory (03 §7.1)

The 592-byte AECP response buffer is no longer fabric state; it is
`KL_aecp_resp_buf` over the `resp_mem_*` master, and the model behind that
master injects **non-zero latency on both channels by default** (23 clocks
read, 17 write). B1 demands one read burst and exactly the lane writes the
write pattern implies; B2 compares the payload on the wire against the model's
own memory image, not against the DUT's account of it; B3 proves a byte whose
write strobe is 0 is never modified; B4/B4b measure the whole path — MAC
command byte 0 to MAC response byte 0 — and check it against the IEEE §9.2.1.1
100 ms budget (10,000,000 clocks at `P-CLK-HZ`), once at the suite's latency
and once at the reference SoC's measured ~1424 ns (143 clocks) per access;
B5 proves an echoed payload costs the response memory **nothing** (it comes
straight out of the RX slot); B6–B9 tie the master off, fail its writes and
fail its reads, and demand a well-formed 60-byte `ENTITY_MISBEHAVING` answer
plus the counters and the snapshot window that name the fault; B10 demands the
slot pools back afterwards; B11 proves a four-times-slower bridge only costs
time.

## Mutation-proven 2026-08-13 (scenario A)

| Break | Went red |
|---|---|
| `COPY_BUFFER` advances by the whole 8-byte lane instead of the residual | **10** red |
| response buffer places fields little-endian instead of big-endian | **8** red |
| unimplemented opcodes fall through to the READ_DESCRIPTOR µprogram | **1** red |
| the frame builder ignores whether the payload byte has arrived from memory yet | **58** red |

## Mutation-proven 2026-08-14 (A5b, response sizing)

| Break | Went red |
|---|---|
| `control_data_length` pinned at 16 (12 + 4) instead of 12 + payload | **23** red, **8** of them A5b |
| the echoed payload capped at 8 octets, with `control_data_length` following it down | **5** red, **all** A5b |

The second one is the result that justifies the block. It produces an
INTERNALLY CONSISTENT frame (the length field matches the bytes actually
emitted, and it still pads to 60), so every pre-existing check stays green:
before A5b the suite could not tell a response sized by its command from one
sized by something else, which is precisely what a controller complains about.
The first row also leaves A5 itself green, because 16 is the right answer for
the one 4-byte payload A5 sends.

The `COPY_BUFFER` one is the interesting result: it goes red HERE and stays
green in `tb/ucpu` (0 checks red there), because that suite's µprogram only copies a
whole number of 8-byte lanes. A descriptor whose length is not a multiple of 8
is a thing only the end-to-end suite sees.
