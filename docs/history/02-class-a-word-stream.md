<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# History: the class-A word-stream contract (never landed)

> **Historical, superseded.** This is the 32-bit word stream the original design specified
> for interface class A. No port of `protocol_processor_top` carries it: the landed top
> presents a byte stream with no RX ready, no `empty` and no `err`. The current contract
> is [02 §3](../architecture/02_interfaces.md#sec-02-class-a) and the
> [integrator guide §3](../guides/integrator.md#3-the-mac-faces). Do not wire against
> this page.

**Provenance.** The text and the two waveform sources below are copied verbatim from
`docs/architecture/02_interfaces.md` §3 at processor `main`
`c050d97153dd0480ae741102c1647eeda9b7f273`, where they sat under a warning that the RTL
did not implement them, and were moved here by processor issue #27. At that commit:
[the page](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/blob/c050d97153dd0480ae741102c1647eeda9b7f273/docs/architecture/02_interfaces.md#3-class-a--packet-streaming),
and the two rendered waveforms,
[F02.3](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/blob/c050d97153dd0480ae741102c1647eeda9b7f273/docs/diagrams/wavedrom/fig-02-rxwave.svg)
and
[F02.4](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/blob/c050d97153dd0480ae741102c1647eeda9b7f273/docs/diagrams/wavedrom/fig-02-txwave.svg).
The anchors `fig-02-rxwave` and `fig-02-txwave` now carry the landed byte-face waveforms.

## The word stream as specified

Word-oriented stream, `DATA_W` = 32 (parameterizable), MSB-first byte lanes, `empty`
gives the unused byte count in the `eof` word. `err` with `eof` invalidates the frame
(RX: drop; TX: MAC aborts with bad FCS).

| Signal | Dir (RX inst.) | Width | Meaning |
|---|---|---|---|
| `valid` | in | 1 | word present |
| `ready` | out | 1 | sink accepts; transfer on `valid ∧ ready` |
| `data` | in | 32 | payload word |
| `sof` / `eof` | in | 1/1 | frame delimiters (both on `valid ∧ ready` words) |
| `empty` | in | 2 | unused bytes in `eof` word |
| `err` | in | 1 | with `eof`: frame invalid |

## Its two waveforms

Kept as sources only and not rendered: a rendered copy here would be a second figure
under the live anchors' names. The rendered SVGs are the two links above.

F02.3, "RX stream: backpressure + end of frame":

```json
{"signal": [
  {"name": "clk",      "wave": "p.........."},
  {"name": "rx_valid", "wave": "01.......0."},
  {"name": "rx_ready", "wave": "1...0.1...."},
  {"name": "rx_sof",   "wave": "010........"},
  {"name": "rx_data",  "wave": "x====..==x.", "data": ["D0", "D1", "D2", "D3", "D4", "D5"]},
  {"name": "rx_eof",   "wave": "0.......10."},
  {"name": "rx_err",   "wave": "0.........."}
],
 "head": {"text": "transfer on valid AND ready; data D3 held through the stall"},
 "foot": {"text": "err would assert together with eof to poison the frame"}}
```

F02.4, "TX stream with arbiter grant (no mid-frame regrant)":

```json
{"signal": [
  {"name": "clk",      "wave": "p.........."},
  {"name": "tx_req_a", "wave": "01....0...."},
  {"name": "tx_req_b", "wave": "01........."},
  {"name": "gnt_a",    "wave": "0.1...0...."},
  {"name": "gnt_b",    "wave": "0......1..."},
  {"name": "tx_valid", "wave": "0.1...0.1.."},
  {"name": "tx_sof",   "wave": "0.10....10."},
  {"name": "tx_data",  "wave": "x.====x.==x", "data": ["W0", "W1", "W2", "W3", "X0", "X1"]},
  {"name": "tx_eof",   "wave": "0....10...."},
  {"name": "tx_ready", "wave": "1.........."}
],
 "head": {"text": "grant is frame-atomic: gnt_a holds until eof, then arbiter moves to b"}}
```

## What replaced it

| Word stream (above) | Landed byte face ([02 §3](../architecture/02_interfaces.md#sec-02-class-a)) |
|---|---|
| 32-bit `data`, `empty` on the `eof` word | 8-bit `rx_data_i` / `tx_data_o`, one byte per transfer |
| RX `ready`, transfer on `valid ∧ ready` | no RX ready: a byte is taken in every cycle `rx_valid_i` is 1 |
| RX `sof` / `eof` | `rx_last_i` only |
| `err` with `eof` poisons the frame | no `err` and no abort: the integrator's RX FIFO presents only complete, FCS-good frames |
| TX `ready` | `tx_ready_i`, a real stall that holds the byte in place |
