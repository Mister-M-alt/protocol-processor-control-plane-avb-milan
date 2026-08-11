#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""µcode ROM image generator for KL_aecp_ucpu (docs/architecture/06 §8).

Emits 2048 lines of 12 hex digits (48-bit µops, ucpu_pkg.sv encoding):
    [47:43] op | [42:39] rd | [38:35] ra | [34:31] rb
    | [30:28] fmt | [27:24] cnd | [23:0] imm24

The programs are the exemplars of 06 §8 hand-assembled against the skeleton's
semantics plus the suite's functional/conformance torture blocks (tb/ucpu).
Status codes follow IEEE 1722.1-2021 Table 7-141 (ucpu_pkg.sv):
SUCCESS 0 · NOT_IMPLEMENTED 1 · NO_SUCH_DESCRIPTOR 2 · ENTITY_LOCKED 3 ·
BAD_ARGUMENTS 7 · NOT_SUPPORTED 11. Unused ROM words are a varied
deterministic fill — contents are irrelevant to LUT once the ROM is BRAM.
"""
import argparse

OPS = {
    'NOP': 0, 'BRANCH': 1, 'BR_STATUS': 2, 'END': 3,
    'MOVE': 4, 'COMPARE': 5, 'SET_MASKED': 6,
    'DESC_ADDR': 7, 'READ_ST': 8, 'WRITE_ST': 9, 'NAME_RD': 10,
    'NAME_WR': 11, 'COPY_BUF': 12,
    'CHECK_LOCK': 13, 'CHECK_ARG': 14, 'MAP_VALID': 15,
    'GATHER_EXT': 16, 'READ_CTRS': 17,
    'ITER_OPEN': 18, 'ITER_NEXT': 19, 'APPEND': 20,
    'COMMIT': 21, 'NVM_MARK': 22, 'NOTIFY_ENQ': 23,
    'SET_STATUS': 24, 'SET_LENGTH': 25, 'BUILD_HDR': 26,
    'BUILD_FLD': 27, 'SEND_RESP': 28,
}
FMT_B, FMT_W, FMT_D, FMT_Q = 0, 1, 2, 3
ST_OK, ST_NIMPL, ST_BADARG, ST_NSUPP = 0, 1, 7, 11

ROM_DEPTH = 2048


def u(op, rd=0, ra=0, rb=0, fmt=0, cnd=0, imm=0):
    assert 0 <= imm < (1 << 24), hex(imm)
    return (OPS[op] << 43) | (rd << 39) | (ra << 35) | (rb << 31) \
        | (fmt << 28) | (cnd << 24) | imm


# Entry points — tb/ucpu/sim_main.cpp mirrors these constants.
E_FAILSAFE = 8       # 06 §8: the forced-respond arm (IEEE §9.3.2.6)
E_GETSR   = 16
E_FAIL    = 40
E_ALU     = 64
E_ITER    = 128
E_CHKARG  = 192
E_LOCK    = 224
E_GATHER  = 256
E_SETSR   = 288      # SET_SAMPLING_RATE exemplar with effects
E_NAME    = 320
E_COPY    = 352
E_MAPV    = 384
E_MAPVF   = 400
E_OVF     = 416      # 524-byte cap + skip-on-overflow (§7.4.76.1)
E_FMT     = 512
E_NOTIMPL = 560      # unknown-opcode path (IEEE §9.3.5.3.3, REQ-FWX-001)
E_ACQ     = 576      # ACQUIRE_ENTITY exemplar (Milan Δ7: NOT_SUPPORTED)
E_STPRE   = 592      # status preservation through FAIL_SAFE

rom = [0] * ROM_DEPTH


def place(at, words):
    for i, w in enumerate(words):
        assert rom[at + i] == 0, f"overlap at {at + i}"
        rom[at + i] = w


# --- FAIL_SAFE: respond with the best current status, always ----------------
place(E_FAILSAFE, [
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- GET_SAMPLING_RATE exemplar (06 §8), success + shared fail handler ------
place(E_GETSR, [
    u('DESC_ADDR', ra=14, imm=0x00100),
    u('BR_STATUS', cnd=0, imm=E_FAIL),
    u('READ_ST', rd=1, imm=8),
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=1, fmt=FMT_D),
    u('SEND_RESP'),
    u('END'),
])
place(E_FAIL, [
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- ALU / branch / RAW / merge torture -------------------------------------
place(E_ALU, [
    u('MOVE', rd=1, imm=5),
    u('COMPARE', ra=1, fmt=FMT_D, imm=5),        # RAW on r1; z=1
    u('BR_STATUS', cnd=2, imm=E_ALU + 4),
    u('MOVE', rd=9, imm=0xBAD),                  # must be flushed
    u('MOVE', rd=2, imm=7),
    u('COMPARE', ra=1, rb=2, fmt=FMT_D),         # 5 vs 7: lt=1
    u('BR_STATUS', cnd=3, imm=E_ALU + 8),
    u('MOVE', rd=9, imm=0xBAD),
    u('MOVE', rd=4, imm=0),
    u('MOVE', rd=5, imm=0xFF),
    u('SET_MASKED', rd=4, ra=1, rb=5),           # r4 = 5
    u('MOVE', rd=3, imm=1),
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=1, fmt=FMT_D),
    u('BUILD_FLD', ra=2, fmt=FMT_D),
    u('BUILD_FLD', ra=3, fmt=FMT_D),
    u('BUILD_FLD', ra=4, fmt=FMT_D),
    u('BUILD_FLD', ra=15, fmt=FMT_Q),
    u('SEND_RESP'),
    u('END'),
])

# --- ITER_OPEN / APPEND / ITER_NEXT loop (GDI shape) -------------------------
place(E_ITER, [
    u('MOVE', rd=6, imm=3),
    u('MOVE', rd=1, imm=0xAB),
    u('ITER_OPEN', ra=6),
    u('APPEND', ra=1, fmt=FMT_D),                # loop:
    u('ITER_NEXT'),
    u('BR_STATUS', cnd=1, imm=E_ITER + 7),
    u('BRANCH', imm=E_ITER + 3),
    u('SET_STATUS', imm=ST_OK),                  # out:
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- CHECK_ARG failure -> BAD_ARGUMENTS (7) ---------------------------------
place(E_CHKARG, [
    u('MOVE', rd=1, imm=5),
    u('MOVE', rd=2, imm=7),
    u('CHECK_ARG', ra=1, rb=2, fmt=FMT_D, cnd=0, imm=E_CHKARG + 4),
    u('SET_STATUS', imm=ST_OK),                  # skipped on failure
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- CHECK_LOCK failure -> ENTITY_LOCKED (3) --------------------------------
place(E_LOCK, [
    u('CHECK_LOCK', ra=15, imm=E_LOCK + 2),
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- GATHER_EXT + qword field + READ_COUNTERS burst --------------------------
place(E_GATHER, [
    u('GATHER_EXT', rd=7, cnd=2, imm=5),         # sel = 0x25
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=7, fmt=FMT_Q),
    u('READ_CTRS', cnd=1),                       # 4 beats, sel 0x10..0x13
    u('SEND_RESP'),
    u('END'),
])

# --- SET_SAMPLING_RATE exemplar (06 §8): checks, write-back, effects --------
place(E_SETSR, [
    u('CHECK_LOCK', ra=15, imm=E_FAIL),
    u('DESC_ADDR', ra=14, imm=0x00100),
    u('BR_STATUS', cnd=0, imm=E_FAIL),
    u('MOVE', rd=2, imm=0xBB80),                 # requested rate
    u('MOVE', rd=3, imm=0xBB80),                 # allowed-set entry
    u('CHECK_ARG', ra=2, rb=3, fmt=FMT_D, cnd=0, imm=E_FAIL),
    u('WRITE_ST', ra=2, fmt=FMT_D, imm=8),       # current_rate <- arg
    u('COMMIT'),                                 # atomicity point
    u('NVM_MARK', imm=0x21),                     # sampling_rate record
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=2, fmt=FMT_D),
    u('SEND_RESP'),
    u('NOTIFY_ENQ', imm=5),                      # unsolicited, excl requester
    u('END'),
])

# --- NAME_RD / NAME_WR (name region select) ---------------------------------
place(E_NAME, [
    u('DESC_ADDR', ra=14, imm=0x00100),
    u('BR_STATUS', cnd=0, imm=E_FAIL),
    u('NAME_RD', rd=4, imm=0x10),
    u('NAME_WR', ra=4, fmt=FMT_Q, imm=0x18),
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=4, fmt=FMT_Q),
    u('SEND_RESP'),
    u('END'),
])

# --- COPY_BUFFER: 16 descriptor bytes into the response ----------------------
place(E_COPY, [
    u('DESC_ADDR', ra=14, imm=0x00100),
    u('BR_STATUS', cnd=0, imm=E_FAIL),
    u('MOVE', rd=8, imm=16),
    u('COPY_BUF', ra=8, imm=0x20),
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- MAP_VALIDATE pass / fail ------------------------------------------------
place(E_MAPV, [
    u('MAP_VALID', cnd=3, imm=E_FAIL),           # sel 0x30 -> pass
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])
place(E_MAPVF, [
    u('MAP_VALID', cnd=4, imm=E_MAPVF + 2),      # sel 0x40 -> fail: 7 + branch
    u('SET_STATUS', imm=ST_OK),                  # skipped
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- APPEND at the 524-byte cap: skip-on-overflow (IEEE §7.4.76.1) ----------
place(E_OVF, [
    u('MOVE', rd=6, imm=70),                     # 70 elements of 8 B
    u('MOVE', rd=1, imm=0xCAFE),
    u('ITER_OPEN', ra=6),
    u('APPEND', ra=1, fmt=FMT_Q),                # loop: 64 fit, rest skip
    u('ITER_NEXT'),
    u('BR_STATUS', cnd=1, imm=E_OVF + 7),
    u('BRANCH', imm=E_OVF + 3),
    u('BR_STATUS', cnd=4, imm=E_OVF + 10),       # out: ovf? ->
    u('SET_STATUS', imm=ST_OK),                  # (no-ovf path)
    u('BRANCH', imm=E_OVF + 11),
    u('SET_STATUS', imm=ST_NSUPP),               # ovf marker for the TB
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- formats: write strobes, truncating moves, 64-bit compare, rb-RAW -------
place(E_FMT, [
    u('DESC_ADDR', ra=14, imm=0x00100),
    u('BR_STATUS', cnd=0, imm=E_FAIL),
    u('MOVE', rd=1, imm=0x123456),
    u('WRITE_ST', ra=1, fmt=FMT_B, imm=0x40),    # strobe 0x01
    u('WRITE_ST', ra=1, fmt=FMT_W, imm=0x40),    # strobe 0x03
    u('WRITE_ST', ra=1, fmt=FMT_Q, imm=0x40),    # strobe 0xFF
    u('MOVE', rd=2, ra=1, fmt=FMT_B),            # 0x56
    u('MOVE', rd=3, ra=1, fmt=FMT_W),            # 0x3456
    u('MOVE', rd=6, imm=0),
    u('MOVE', rd=5, imm=0xF0),
    u('SET_MASKED', rd=6, ra=1, rb=5),           # rb-RAW on r5; r6 = 0x50
    u('COMPARE', ra=15, rb=15, fmt=FMT_Q),       # 64-bit equality; z=1
    u('BR_STATUS', cnd=2, imm=E_FMT + 14),
    u('MOVE', rd=9, imm=0xBAD),                  # must be flushed
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=6, fmt=FMT_D),             # @12: 0x50
    u('BUILD_FLD', ra=3, fmt=FMT_W),             # @16: 0x3456
    u('BUILD_FLD', ra=2, fmt=FMT_B),             # @18: 0x56 (unaligned byte)
    u('SEND_RESP'),
    u('END'),
])

# --- unknown-opcode path: echo + NOT_IMPLEMENTED (IEEE §9.3.5.3.3) ----------
place(E_NOTIMPL, [
    u('SET_STATUS', imm=ST_NIMPL),
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- ACQUIRE_ENTITY exemplar (Milan Δ7): NOT_SUPPORTED, owner_id = 0 --------
place(E_ACQ, [
    u('SET_STATUS', imm=ST_NSUPP),
    u('MOVE', rd=7, imm=0),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=7, fmt=FMT_Q),             # owner_id = 0 echo
    u('SEND_RESP'),
    u('END'),
])

# --- FAIL_SAFE preserves the best current status (§9.3.2.6 arm) -------------
place(E_STPRE, [
    u('SET_STATUS', imm=ST_BADARG),
    u('BRANCH', imm=E_FAILSAFE),
])

# --- deterministic non-degenerate fill ---------------------------------------
for i in range(ROM_DEPTH):
    if rom[i] == 0 and i not in (0,):
        rom[i] = u('MOVE', rd=(i % 13) + 1, imm=(i * 2654435761) & 0xFFFFFF) \
            if (i % 3) else u('NOP', imm=i & 0xFFFFFF)

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--out', default='ucode.hex')
    a = ap.parse_args()
    with open(a.out, 'w') as f:
        for w in rom:
            f.write(f"{w:012x}\n")
    print(f"{a.out}: {ROM_DEPTH} words, 18 programs")
