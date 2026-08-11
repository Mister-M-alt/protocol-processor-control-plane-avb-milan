#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""µcode ROM image generator for KL_aecp_ucpu (docs/architecture/06 §8).

Emits 2048 lines of 12 hex digits (48-bit µops, ucpu_pkg.sv encoding):
    [47:43] op | [42:39] rd | [38:35] ra | [34:31] rb
    | [30:28] fmt | [27:24] cnd | [23:0] imm24

The programs below are the exemplars of 06 §8 hand-assembled against the
skeleton's semantics, plus torture blocks for the TB. Unused ROM words are a
varied deterministic fill — contents are irrelevant to LUT once the ROM infers
as BRAM, but a non-degenerate image keeps every tool honest.
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

ROM_DEPTH = 2048


def u(op, rd=0, ra=0, rb=0, fmt=0, cnd=0, imm=0):
    assert 0 <= imm < (1 << 24), hex(imm)
    return (OPS[op] << 43) | (rd << 39) | (ra << 35) | (rb << 31) \
        | (fmt << 28) | (cnd << 24) | imm


# Entry points — the TB mirrors these constants.
E_GETSR, E_FAIL, E_ALU, E_ITER, E_CHKARG, E_LOCK, E_GATHER = \
    16, 40, 64, 128, 192, 224, 256

rom = [0] * ROM_DEPTH


def place(at, words):
    for i, w in enumerate(words):
        assert rom[at + i] == 0, f"overlap at {at + i}"
        rom[at + i] = w


# --- GET_SAMPLING_RATE exemplar (06 §8), success + shared fail handler ------
place(E_GETSR, [
    u('DESC_ADDR', ra=14, imm=0x00100),          # locate AUDIO_UNIT[idx]
    u('BR_STATUS', cnd=0, imm=E_FAIL),           # miss -> fail handler
    u('READ_ST', rd=1, imm=8),                   # r1 <- current_rate
    u('SET_STATUS', imm=0),                      # SUCCESS
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
    u('MOVE', rd=1, imm=5),                      # r1 = 5
    u('COMPARE', ra=1, fmt=FMT_D, imm=5),        # RAW on r1; z=1
    u('BR_STATUS', cnd=2, imm=E_ALU + 4),        # z -> skip poison
    u('MOVE', rd=9, imm=0xBAD),                  # must be skipped
    u('MOVE', rd=2, imm=7),                      # r2 = 7
    u('COMPARE', ra=1, rb=2, fmt=FMT_D),         # 5 vs 7: lt=1
    u('BR_STATUS', cnd=3, imm=E_ALU + 8),        # lt -> skip poison
    u('MOVE', rd=9, imm=0xBAD),                  # must be skipped
    u('MOVE', rd=4, imm=0),                      # r4 = 0
    u('MOVE', rd=5, imm=0xFF),                   # r5 = mask
    u('SET_MASKED', rd=4, ra=1, rb=5),           # r4 = 5
    u('MOVE', rd=3, imm=1),                      # r3 = 1
    u('SET_STATUS', imm=0),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=1, fmt=FMT_D),
    u('BUILD_FLD', ra=2, fmt=FMT_D),
    u('BUILD_FLD', ra=3, fmt=FMT_D),
    u('BUILD_FLD', ra=4, fmt=FMT_D),
    u('BUILD_FLD', ra=15, fmt=FMT_Q),            # 2-beat qword field
    u('SEND_RESP'),
    u('END'),
])

# --- ITER_OPEN / APPEND / ITER_NEXT loop (GDI shape) -------------------------
place(E_ITER, [
    u('MOVE', rd=6, imm=3),                      # 3 elements
    u('MOVE', rd=1, imm=0xAB),
    u('ITER_OPEN', ra=6),
    u('APPEND', ra=1, fmt=FMT_D),                # loop:
    u('ITER_NEXT'),
    u('BR_STATUS', cnd=1, imm=E_ITER + 7),       # done -> out
    u('BRANCH', imm=E_ITER + 3),
    u('SET_STATUS', imm=0),                      # out:
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- CHECK_ARG failure path --------------------------------------------------
place(E_CHKARG, [
    u('MOVE', rd=1, imm=5),
    u('MOVE', rd=2, imm=7),
    u('CHECK_ARG', ra=1, rb=2, fmt=FMT_D, cnd=0,
      imm=E_CHKARG + 4),                         # 5 == 7 fails -> BAD_ARGS
    u('SET_STATUS', imm=0),                      # skipped on failure
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- CHECK_LOCK failure path -------------------------------------------------
place(E_LOCK, [
    u('CHECK_LOCK', ra=15, imm=E_LOCK + 2),      # other owner -> LOCKED
    u('SET_STATUS', imm=0),                      # skipped when locked
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

# --- deterministic non-degenerate fill ---------------------------------------
for i in range(ROM_DEPTH):
    if rom[i] == 0 and i not in (0,):
        # cycle through harmless ops with varied fields
        rom[i] = u('MOVE', rd=(i % 13) + 1, imm=(i * 2654435761) & 0xFFFFFF) \
            if (i % 3) else u('NOP', imm=i & 0xFFFFFF)

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--out', default='ucode.hex')
    a = ap.parse_args()
    with open(a.out, 'w') as f:
        for w in rom:
            f.write(f"{w:012x}\n")
    print(f"{a.out}: {ROM_DEPTH} words, entries "
          f"GETSR={E_GETSR} FAIL={E_FAIL} ALU={E_ALU} ITER={E_ITER} "
          f"CHKARG={E_CHKARG} LOCK={E_LOCK} GATHER={E_GATHER}")
