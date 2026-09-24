<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Descriptor memory response isolation

`make` runs the real store with `KL_aecp_desc_mem_guard`, using the store's
uncompressed watchdog. The independent memory model accepts requests into an
in-order FIFO even while an older burst is owed. A small image contains distinct
STREAM_INPUT and STREAM_OUTPUT marker bytes; descriptor interiors are opaque to
the store, so this fixture tests transport identity rather than descriptor rules.

The suite proves:

- A STREAM_OUTPUT fetch delayed 6,000 cycles times out. The first later locate
  reports the store's existing immediate error; the next STREAM_INPUT locate is
  presented before the old burst arrives and receives its own complete bytes.
  Another locate succeeds after the debt drains.
- A burst that emits one nonterminal beat and then never finishes holds every
  later request. Five more locates each answer an error within 4,160 cycles
  (the default watchdog plus scan/request/answer overhead).
- Debt survives a store-only reset and drains while that store remains reset.
  Hard reset clears it with the memory queue flushed. An error without `last`
  terminates debt and permits the next fetch.
- Request and response payloads pass unchanged. Only accepted requests create
  debt; nonterminal beats, invalid terminal flags and backpressured terminal
  beats cannot clear it. The next request stays held through the terminal cycle.
  Acceptance wins over a coincident stray terminal beat when no debt was owed.

The wrapper exposes the store/guard seam and provides a direct guard stimulus
mode for handshake checks. There is no D3 writer here: owner-release policy,
rollback deadlines and the 5,000/16,000-cycle rollback cases belong to that lane.
The product integration is also exercised by `tb/pp_top` cases A12/A13, through
real READ_DESCRIPTOR commands and byte-exact responses on the MAC interface.

## Reproduction and mutation control

```sh
# Expected failure, using the unmodified store and no guard:
make -C tb/desc_mem_guard baseline
# Expected pass:
make -C tb/desc_mem_guard
# Expected control pass only if the completed late-byte assertion detects the mutant:
python3 tb/desc_mem_guard/mutate.py --output /tmp/desc-guard-control
```

The baseline was executed before any product RTL edit. Its third locate returned
`00060000deadbeef cafef00d01234567` instead of the STREAM_INPUT bytes
`0005000011223344 5566778899aabbcc`.

| Build | Late-case result | Completed assertion |
|---|---|---|
| Store without guard | 17 PASS, 1 FAIL | `third locate late_beats_never_served` |
| Guard | 18 PASS, 0 FAIL | own bytes served, then recovery |
| Guard with both `&& !owed_r` request holds deleted | 17 PASS, 1 FAIL | same wrong bytes and assertion as baseline |

The control writes a separate mutant source; it never edits product RTL. It
requires the exact assertion, wrong bytes, completed scenario and nonzero
simulation verdict. A build failure or process crash does not count as detection.
