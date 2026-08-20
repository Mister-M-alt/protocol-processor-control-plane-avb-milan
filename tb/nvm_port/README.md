<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# nvm_port — KL_pp_nvm_port class-F suite

Proves the class-F NVM port (`hdl/packet_engine/KL_pp_nvm_port.sv`,
[02 §8](../../docs/architecture/02_interfaces.md) F02.8 +
[07 §5](../../docs/architecture/07_memory_maps.md) F07.8): `make` = build + run,
exit 0 = PASS, 66 checks. `-GMAX_PAYLOAD_P=1024` pins the geometry the C++
constants mirror.

The harness plays BOTH neighbors, independently of the RTL: a **manager BFM**
that frames records per 07 §5.2 (magic 0x1722, layout_version, record_id,
payload_length, crc16 — 16-bit fields big-endian on the stream; crc computed by
the harness, opaque to the DUT) and streams them with configurable stalls, and
a **device model** implementing the region port (req/gnt with delay, op
READ/WRITE/ERASE_REGION, region + offset + len, stalling byte phases,
busy/done/err with completion delays, error injection at any command index or
data byte, a seeded byte store, and a log of every accepted command).

Covered: the commit envelope ERASE_REGION → WRITE(0, 8+plen) with the device
store byte-exact against the manager's framed record and the erase visible past
it; the restore envelope READ(0,8) → READ(8,plen) with the returned stream
byte-exact; zero-payload records both ways (restore then issues only the header
probe); stall torture on all four byte interfaces at once (gnt and done
delayed); back-to-back ops with req re-asserted the cycle after the pulse;
req-while-busy ignored (single outstanding, F02.8); busy/done/err sequencing —
busy high mid-op, LOW at the pulse, done and err mutually exclusive and exactly
once per op; a device error during ERASE surfacing as exactly one manager-face
err with the WRITE never issued, plus recovery on retry; errors mid WRITE-data
and mid READ-payload; refusals per 07 §5.2 with zero (further) device traffic —
bad magic on commit, oversize payload_length on commit, bad magic and oversize
length in the stored record on restore (nothing forwarded to the manager); and
the port serviceable again after every refusal.

Known limits (honest): the CRC16 is carried, never checked — that is the
manager's job per 07 §5.3, so a corrupt-crc record passes this port by design
and must be caught by the NVM-manager suite (P4). The byte order of the 16-bit
header fields on the stream is a design decision of the port (network order),
not pinned by the doc.

Power-cut coverage (issue #70, added 2026-08-20). A commit is ERASE(region)
then WRITE(0, 8+plen), so a cut inside the WRITE leaves the region erased plus a
partial record: the record being written is gone **and so is whatever it
replaced**. That is a property of writing a slot in place, and it is why the
flash map reserves A/B slots — the suite pins what the port DOES guarantee
rather than a survival this layout cannot give:

- **T15** a torn commit reports `err` and never `done`, with busy low at the
  pulse; the torn image is neither the old record nor the new one (so the cut is
  real, not a no-op); it never restores as a VALID record — either the port
  refuses it at the header or the bytes it forwards fail the manager's CRC,
  which the suite computes itself; and the port is serviceable afterwards.
- **T16** the property #70 actually needs: a torn commit of one record leaves
  **every other record** untouched — the neighbour's region is never erased, its
  stored bytes are byte-identical, and it still restores byte-exactly after the
  cut. One interrupted save must not lose the whole saved set.

Mutation-proven 2026-08-11 (backup → sed → run → restore → green):
- **M1** commit skips the ERASE (`S_WEREQ` target rewritten to `S_WWREQ`):
  fails 11 of 55 (op-log shape, erase pulse/visibility, erase-error path).
- **M2** magic gate dropped from `hdr_ok_w`: fails 5 of 55 (both bad-magic
  refusals and the nothing-forwarded check).
- **M3** payload pump off-by-one (`bcnt_r == plen_r` for `plen_r - 1`, both
  directions): fails 38 of 55 (every data-phase op times out or mismatches).
- **M4** (2026-08-20) the write phase swallows the device error (`S_WDPUMP`'s
  `if (dev_err_i)` forced false): fails 7 of 66 — every T15/T16 check. A torn
  commit then looks clean, which is exactly the false success #70 exists to
  remove.
