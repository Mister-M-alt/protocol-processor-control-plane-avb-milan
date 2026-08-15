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
ST_NORES = 8          # Table 7-141 NO_RESOURCES (Milan SS5.4.2.21's refusal)
REL_EQ, REL_NE, REL_LT, REL_GE = 0, 1, 2, 3

# KL_aecp_desc_store state-port regions (see its banner): the µISA cannot put a
# 48-bit locate key on a 20-bit address, so the region nibble selects what a
# state access MEANS and the key rides st_wdata (= rf[ra]).
RGN_DATA   = 0x00000   # the located descriptor's bytes
RGN_NBASE  = 0xC0000   # name-table entry of the located descriptor
RGN_NCFG   = 0xD0000   # configurations_count
RGN_LEN    = 0xE0000   # located descriptor length
RGN_LOCATE = 0xF0000   # perform a locate

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
E_RDESC   = 640      # READ_DESCRIPTOR (06 §6.1) — KL_aecp_engine dispatches here
E_RDSTUB  = 672      # its IEEE §7.4.5 failure stub
E_BADARG  = 704      # echo + BAD_ARGUMENTS (IEEE §7.4.39.2 opcode rule)
E_MVUINFO = 736      # MVU GET_MILAN_INFO (Milan v1.2 §5.4.4.1, Figure 5.4)

# --- what this device reports in GET_MILAN_INFO -----------------------------
# Milan v1.2 §4.2.4 and §5.4.4.1: "A PAAD shall set the value of the
# protocol_version field in the GET_MILAN_INFO response to 1."
MILAN_PROTOCOL_VERSION = 1
# Table 5.20 has exactly two flags and this device is entitled to NEITHER.
# REDUNDANCY (0x00000001) is a claim to implement Milan §8 seamless redundancy;
# this is a single-AVB-interface PAAD (P-N-AVB-INTERFACES = 1) with one
# AVB_INTERFACE descriptor, so there is no second stream to be seamless with.
# TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING (0x00000002) is a claim to accept map
# changes while a Stream Output streams (§5.3.9.1); this build answers
# ADD/REMOVE_AUDIO_MAPPINGS with NOT_IMPLEMENTED, so it cannot change a mapping
# at all. An overclaimed flag makes a controller take a path the gateware
# cannot serve — the same class of defect as reporting a restore that never
# happened. When P-EN-TALKER-DYN-MAPPINGS-RUNNING becomes real (06 §6.9), this
# is the one line that moves.
MILAN_FEATURES_FLAGS = 0x00000000
# §5.4.4.1: four dot-separated 8-bit numbers, "set to 0 if the PAAD-AE has not
# passed any Milan certification". This device has passed none.
MILAN_CERT_VERSION = 0x00000000
# Table 5.18 command_type, in the @28..@29 word whose top bit is the r field
# that §5.4.3.2.2 requires to be zero.
MVU_GET_MILAN_INFO = 0x0000
E_GCTRS   = 768      # GET_COUNTERS (06 §6.6, IEEE §7.4.42) — Milan 5.4.2.25
E_GAMAP   = 800      # GET_AUDIO_MAP (06 §6.5, IEEE §7.4.44) - Milan 5.4.2.26
E_REGUN   = 832      # REGISTER_UNSOLICITED_NOTIFICATION (Milan 5.4.2.21)
E_DEREG   = 844      # DEREGISTER_UNSOLICITED_NOTIFICATION (Milan 5.4.2.22)
E_UNSOK   = 852      # payload-less unsolicited response (auto-DEREGISTER)
E_NOSEND  = 858      # a job whose response type has no program yet: no frame
# 1722.1-2021 Table 7-1: the one descriptor type this program is dispatched
# for (KL_aecp_engine refuses every other type back to the NOT_IMPLEMENTED
# echo before dispatch, so the constant emitted at @24 is also a guarantee).
DT_STREAM_PORT_INPUT = 0x000E

# --- gather selectors the counters face answers (06 §6.6) --------------------
# gx_sel is {cnd, imm[3:0]} for GATHER_EXT and {cnd, beat} for READ_CTRS, so the
# 32 counters_block quadlets need cnd 0..7 x 4 beats and the counters_valid word
# needs a selector OUTSIDE that range. Bit 7 of the selector is what separates
# them, which is why the mask sits at cnd 8: KL_aecp_engine reads sel[7] alone.
GX_CTR_MASK_CND = 8    # -> sel 0x80
GX_CTR_BLOCK_CND = range(8)   # -> sel 0x00..0x03, 0x10..0x13, ... 0x70..0x73

# --- gather selectors the audio-map face answers (06 §6.5) -------------------
# The SAME 8-bit sel space, and the overlap with the counter selectors above is
# deliberate: KL_aecp_engine routes the gather bus BY COMMAND (its `amap_r`
# discriminator), never by selector value, so each face owns the whole space
# while its command is in flight. The engine maps these three to amap_sel_o:
#   sel 0x00 NMAPS  -> number_of_maps of the addressed port, right-justified
#                      ({48'd0, N}); 0 = the fabric knows no such port
#   sel 0x01 GEOM   -> {32'd0, number_of_maps[15:0], number_of_mappings[15:0]}:
#                      one FMT_D BUILD_FLD emits @30..@33 in wire order, and
#                      [15:0] doubles as the ITER_OPEN count. The face answers
#                      number_of_mappings = 0 for any page it has no data for
#                      (unknown port, map_index out of range) - the wrong-object
#                      guard, so an error stub emits an empty page by the same
#                      wire the success path uses.
#   sel 0x10 RECORD -> the 8-byte §7.4.44.2.1 record
#                      {stream_index, stream_channel, cluster_offset,
#                       cluster_channel} as one big-endian qword; the record
#                      ordinal rides amap_rec_o, an engine-side counter that
#                      increments per completed RECORD gather.
AM_NMAPS = dict(cnd=0, imm=0)   # -> sel 0x00
AM_GEOM = dict(cnd=0, imm=1)    # -> sel 0x01
AM_REC = dict(cnd=1, imm=0)     # -> sel 0x10

rom = [0] * ROM_DEPTH


#! the program count is COUNTED, never restated: three tracks add µprograms to
#! this file in parallel and a hand-maintained total is the one line they all
#! collide on and the first one to go stale
placed = []


def place(at, words):
    for i, w in enumerate(words):
        assert rom[at + i] == 0, f"overlap at {at + i}"
        rom[at + i] = w
    placed.append(at)


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

# --- READ_DESCRIPTOR (06 §6.1) ----------------------------------------------
# The one AEM command this processor really answers. Register contract, set by
# KL_aecp_engine at dispatch (the µISA has no shift, so every field a µprogram
# emits has to arrive right-justified in some register):
#   r15 = controller_entity_id
#   r14 = {--, descriptor_index, descriptor_type, configuration_index}
#         -> [15:0] is the configuration_index emitted at @24
#         -> [47:0] is the store's locate key on st_wdata
#   r13 = {--, descriptor_type, descriptor_index}
#         -> the 4-byte {type, index} stub of IEEE §7.4.5 as one dword
# Response payload: configuration_index(2) reserved(2) descriptor(N), so the
# AECPDU is 28 + N (F06.14). A bad configuration_index is BAD_ARGUMENTS and a
# missing descriptor is NO_SUCH_DESCRIPTOR — both answer the 4-byte stub.
place(E_RDESC, [
    u('MOVE', rd=12, ra=0, imm=0),               # r12 = 0 (the reserved field)
    u('READ_ST', rd=9, imm=RGN_NCFG),            # r9 = configurations_count
    u('CHECK_ARG', ra=14, rb=9, fmt=FMT_W,       # cfg < count, else BAD_ARGS
      cnd=REL_LT, imm=E_RDSTUB),
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # miss -> NO_SUCH_DESCRIPTOR
    u('BR_STATUS', cnd=0, imm=E_RDSTUB),
    u('READ_ST', rd=8, imm=RGN_LEN),             # r8 = descriptor length
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=14, fmt=FMT_W),            # configuration_index @24
    u('BUILD_FLD', ra=12, fmt=FMT_W),            # reserved @26
    u('COPY_BUF', ra=8, imm=RGN_DATA),           # the descriptor @28..
    u('SEND_RESP'),
    u('END'),
])
place(E_RDSTUB, [
    u('MOVE', rd=12, ra=0, imm=0),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=14, fmt=FMT_W),            # configuration_index @24
    u('BUILD_FLD', ra=12, fmt=FMT_W),            # reserved @26
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # {type, index} stub @28
    u('SEND_RESP'),
    u('END'),
])

# --- BAD_ARGUMENTS with the command echoed (IEEE §7.4.39.2) ------------------
# IDENTIFY_NOTIFICATION as a COMMAND, and a truncated READ_DESCRIPTOR. The
# engine pre-loads the command payload and keeps its length, so this is the
# command frame back with message_type + 1 and status 7.
place(E_BADARG, [
    u('SET_STATUS', imm=ST_BADARG),
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- MVU GET_MILAN_INFO (Milan v1.2 §5.4.4.1, Figure 5.4) --------------------
# The answer a Milan controller asks for FIRST and the one that decides whether
# it treats this device as a PAAD-AE at all: with NOT_IMPLEMENTED it records
# protocol_version 0 / features_flags 0x0 and stops there.
#
# The response payload starts at AECPDU @24 = response-buffer byte 12, so the
# cursor lays down, in order: protocol_id[31:0] (@24, the tail of the 48-bit
# id whose first two bytes the engine echoes in the header word), r +
# command_type (@28), reserved (@30), then Figure 5.4's three 32-bit fields.
# That is 20 bytes, so the AECPDU is 44 B and control_data_length is 32.
#
# Every one of those fields is RESTATED from a constant rather than echoed out
# of the command. §5.4.4.1 says the reserved field "shall be set to 0 by the
# sender", and echoing would forward whatever a controller happened to put
# there; the µISA has no byte-swap and no shift, so each field arrives
# right-justified in its own register (06 §8).
place(E_MVUINFO, [
    u('MOVE', rd=1, imm=0x00C50A),                # protocol_id @24..@25
    u('MOVE', rd=2, imm=0x00C100),                # protocol_id @26..@27
    u('MOVE', rd=3, imm=MVU_GET_MILAN_INFO),      # r = 0 + command_type @28
    u('MOVE', rd=4, imm=0),                       # reserved @30 (sender = 0)
    u('MOVE', rd=5, imm=MILAN_PROTOCOL_VERSION),
    u('MOVE', rd=6, imm=MILAN_FEATURES_FLAGS),
    u('MOVE', rd=7, imm=MILAN_CERT_VERSION),
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=1, fmt=FMT_W),
    u('BUILD_FLD', ra=2, fmt=FMT_W),
    u('BUILD_FLD', ra=3, fmt=FMT_W),
    u('BUILD_FLD', ra=4, fmt=FMT_W),
    u('BUILD_FLD', ra=5, fmt=FMT_D),              # protocol_version @32
    u('BUILD_FLD', ra=6, fmt=FMT_D),              # features_flags @36
    u('BUILD_FLD', ra=7, fmt=FMT_D),              # certification_version @40
    u('SEND_RESP'),
    u('END'),
])

# --- GET_COUNTERS (IEEE 1722.1-2021 §7.4.42, Milan v1.2 §5.4.2.25) ----------
# The command Milan makes mandatory for every AVB Interface, Clock Domain,
# Stream Input and Stream Output of the current configuration, and the one
# la_avdecc gates the Milan badge on: its s_MilanMandatoryStreamInputCounters is
# Milan Table 5.16 exactly (mask 0x00000F3F), and a STREAM_INPUT answer missing
# one bit of it costs the entity the Milan compatibility flag.
#
# Figure 7-67 fixes the response: descriptor_type @24, descriptor_index @26,
# counters_valid @28, then a block of THIRTY-TWO quadlets @32..@156. The block
# is fixed-size on every status, so the payload is 136 B, the AECPDU 160 B and
# control_data_length 148 — the offset-from-@12 convention (F06.14).
#
# There is NO branch and NO status arm in this program, and that is a decision.
# counters_valid bit n means "quadlet n exists and is valid" (§7.4.42.2), so a
# descriptor this build keeps no counters for is answered SUCCESS with a mask of
# zero: honest ("I have none") rather than a mask of ones over a block of zeros,
# which is the advertised-zero lie the campaign has already had to remove twice.
# ENTITY is that case by the standard itself — Table 7-150 has nothing but
# ENTITY_SPECIFIC bits, none of them Milan-mandatory.
#
# The register contract, set by KL_aecp_engine at dispatch:
#   r14[15:0] = descriptor_type   (AECPDU @24)
#   r13[15:0] = descriptor_index  (AECPDU @26)
# The µISA has no shift, so each field arrives right-justified in its own
# register; the 32 quadlets never touch a register at all, READ_CTRS moving them
# gather-port-to-response-buffer one beat at a time.
place(E_GCTRS, [
    u('GATHER_EXT', rd=1, cnd=GX_CTR_MASK_CND, imm=0),   # counters_valid
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=14, fmt=FMT_W),            # descriptor_type   @24
    u('BUILD_FLD', ra=13, fmt=FMT_W),            # descriptor_index  @26
    u('BUILD_FLD', ra=1,  fmt=FMT_D),            # counters_valid    @28
] + [u('READ_CTRS', cnd=c) for c in GX_CTR_BLOCK_CND] + [   # 32 quadlets @32
    u('SEND_RESP'),
    u('END'),
])

# --- GET_AUDIO_MAP (IEEE 1722.1-2021 §7.4.44, Milan v1.2 §5.4.2.26) ----------
# The command a strict controller (la_avdecc without the
# IGNORE_NEITHER_STATIC_NOR_DYNAMIC_MAPPINGS workaround) needs answered before
# it will finish enumerating a device whose Stream Port Inputs carry
# number_of_maps = 0 - which Milan §5.3.3.9 makes every Milan Stream Port
# Input ("The Stream Port Input of a Configuration shall not contain any
# AUDIO_MAP descriptor").
#
# Response payload (§7.4.44.2, offsets are AECPDU bytes): descriptor_type @24,
# descriptor_index @26, map_index @28, number_of_maps @30,
# number_of_mappings @32, reserved @34, then 8-byte mapping records @36. So
# the AECPDU is 36 + 8·M and cdl is 24 + 8·M (offset-from-@12, F06.14).
#
# WHO DECIDES WHAT (the 06 §6.5 split):
#   - EXISTENCE is the DESCRIPTOR STORE's: DESC_ADDR locates
#     STREAM_PORT_INPUT[index] in the same image READ_DESCRIPTOR serves, so
#     GET_AUDIO_MAP answers NO_SUCH_DESCRIPTOR for exactly the indices
#     READ_DESCRIPTOR answers it for - one authority, no drift.
#   - PAGE LAW is the µprogram's: §7.4.44.1 "If the map_index is beyond the
#     range of available maps then it returns a BAD_ARGUMENT status", so
#     CHECK_ARG demands map_index < number_of_maps.
#   - GEOMETRY and CONTENT are the INTEGRATOR's, through the amap_* gather
#     face: Milan §5.4.2.26 fixes the partition per Configuration and the
#     mappings live in the integrator's routing fabric, cycles away from this
#     parser. The face's number_of_mappings answers 0 for any page it has no
#     data for, which is what makes ONE emit path serve success and both
#     error stubs (an error response still carries the full 12-byte fixed
#     part - a controller that deserializes on every status gets a
#     well-formed frame, never a truncated one).
#
# Register contract, set by KL_aecp_engine at dispatch:
#   r14 = {16'd0, descriptor_index, descriptor_type, 16'd0} - the store's
#         locate key ({index, type, cfg 0} on st_wdata; GET_AUDIO_MAP names
#         no configuration_index, and configuration 0 is current by
#         construction - SET_CONFIGURATION is not implemented)
#   r13 = {32'd0, descriptor_index, map_index} - one FMT_D BUILD_FLD emits
#         @26..@29 in wire order, and [15:0] is the CHECK_ARG operand
#         (the µISA has no shift, so the engine packs each field where a
#         µop can reach it)
# The descriptor_type emitted at @24 is a MOVE constant: the engine only
# dispatches this program for STREAM_PORT_INPUT (everything else keeps the
# NOT_IMPLEMENTED echo - the recorded Stream Port OUTPUT gap), so the
# constant is the captured value by guarantee, and r14[15:0] - where the
# other programs keep the @24 field - is the locate key's cfg half instead.
place(E_GAMAP, [
    u('MOVE', rd=12, ra=0, imm=0),                    # reserved @34
    u('MOVE', rd=8, ra=0, imm=DT_STREAM_PORT_INPUT),  # descriptor_type @24
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # miss -> NO_SUCH_DESCRIPTOR
    u('GATHER_EXT', rd=5, **AM_NMAPS),           # r5 = number_of_maps
    u('GATHER_EXT', rd=6, **AM_GEOM),            # r6 = {nmaps, nmappings}
    u('BR_STATUS', cnd=0, imm=E_GAMAP + 8),      # NSD: skip to the emit
    u('SET_STATUS', imm=ST_OK),
    u('CHECK_ARG', ra=13, rb=5, fmt=FMT_W,       # map_index < number_of_maps
      cnd=REL_LT, imm=E_GAMAP + 8),              # else BAD_ARGUMENTS (§7.4.44.1)
    u('BUILD_HDR', ra=15, rb=13),                # emit (all three statuses):
    u('BUILD_FLD', ra=8, fmt=FMT_W),             # descriptor_type       @24
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # descriptor_index @26 + map_index @28
    u('BUILD_FLD', ra=6, fmt=FMT_D),             # number_of_maps @30 + number_of_mappings @32
    u('BUILD_FLD', ra=12, fmt=FMT_W),            # reserved              @34
    u('ITER_OPEN', ra=6),                        # count = number_of_mappings
    u('BR_STATUS', cnd=1, imm=E_GAMAP + 19),     # loop: 0-trip safe (test FIRST)
    u('GATHER_EXT', rd=7, **AM_REC),             # record amap_rec_o, 8 B
    u('APPEND', ra=7, fmt=FMT_Q),
    u('ITER_NEXT'),
    u('BRANCH', imm=E_GAMAP + 14),
    u('SEND_RESP'),                              # out:
    u('END'),
])

# --- gather selectors the registry/lock face answers (06 §6.4/§6.7) ----------
# The SAME command-routed 8-bit sel space as the other faces. Engine mapping:
# sel bit 0 = rgy_state_o (1 = read the lock holder, no op); everything else
# ignored - the op itself comes from the command's opcode/flags, never the
# selector. RULE (KL_aecp_notify's once-per-edge contract): two consecutive
# rgy gathers must be separated by a non-gather microop, or the second would
# ride the first's still-high request; every program below obeys by testing
# the first result before gathering again.
RG_OP = dict(cnd=0xA, imm=0)      # -> sel 0xA0: perform the command's op
RG_STATE = dict(cnd=0xA, imm=1)   # -> sel 0xA1: read locked_id (P2)

# --- REGISTER_UNSOLICITED_NOTIFICATION (IEEE §7.4.37, Milan §5.4.2.21) -------
# THE named Milan 1.3 5.4.2.21 demotion in the reference controller's log.
# The op walks the Milan §5.3.4.2 list in KL_aecp_notify; result 0 = entry
# created or refreshed, 1 = table full. Milan §5.4.2.21: "if the PAAD-AE
# still has not enough resources to create a new entry in the list of
# registered controllers, it shall return the NO_RESOURCES error code" (the
# CONTROLLER_AVAILABLE eviction probe of the same clause is a MAY - not
# attempted, recorded in the notify banner).
#
# The response payload is the ENGINE'S ECHO of the command's own bytes:
# §7.4.37.1's command and response "share the same AECPDU format", and the
# entity "shall accept [the] command both with or without the new flags
# field" - echoing returns a 2021 command's flags and a 2013 command's
# nothing, each in its own format. So this program emits no field of its own.
place(E_REGUN, [
    u('GATHER_EXT', rd=1, **RG_OP),              # the registry op
    u('COMPARE', ra=1, fmt=FMT_D, imm=0),        # z = created/refreshed
    u('BR_STATUS', cnd=2, imm=E_REGUN + 5),      # (also the gather gap)
    u('SET_STATUS', imm=ST_NORES),               # full -> NO_RESOURCES
    u('BRANCH', imm=E_REGUN + 6),
    u('SET_STATUS', imm=ST_OK),                  # ok:
    u('BUILD_HDR', ra=15, rb=13),                # emit: the flags echo rides
    u('SEND_RESP'),                              # the engine's echo path
    u('END'),
])

# --- DEREGISTER_UNSOLICITED_NOTIFICATION (IEEE §7.4.38, Milan §5.4.2.22) -----
# Always SUCCESS: removing an absent registration is idempotent - neither
# clause defines an error arm, and §7.4.38.1's command_specific_data is zero
# length so the echo emits nothing.
place(E_DEREG, [
    u('GATHER_EXT', rd=1, **RG_OP),              # the removal op
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- payload-less unsolicited response ---------------------------------------
# The automatic DEREGISTER_UNSOLICITED_NOTIFICATION of IEEE §7.4.37.2 ("the
# ATDECC Entity shall send a DEREGISTER_UNSOLICITED_NOTIFICATION unsolicited
# response for the ATDECC Controller and remove the registration") and Milan
# Table 5.22 ("sent only to this controller"). The engine synthesizes the
# whole header - controller, MAC, per-entry sequence_id, u = 1 - so the
# program is just "SUCCESS, no payload".
place(E_UNSOK, [
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- the no-send arm ---------------------------------------------------------
# An unsolicited job kind whose response program has not landed retires with
# NO frame (the engine counts it as a drop): a GET_STREAM_INFO notification
# with an invented empty body would be worse than none.
place(E_NOSEND, [
    u('END'),
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
    print(f"{a.out}: {ROM_DEPTH} words, {len(placed)} programs")
