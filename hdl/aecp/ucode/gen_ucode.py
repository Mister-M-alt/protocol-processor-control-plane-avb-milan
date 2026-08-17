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
ST_LOCKED = 3         # Table 7-141 ENTITY_LOCKED (the LOCK denial arm)
ST_STRMRUN = 12       # Table 7-141 STREAM_IS_RUNNING — Milan's refusal for a
                      # SET aimed at a bound Stream Input or a streaming
                      # Stream Output (SS5.4.2.5 / SS5.4.2.7 / SS5.4.2.9)
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
E_NSUPPE  = 864      # echo + NOT_SUPPORTED (ACQUIRE, LOCK on non-ENTITY)
E_LOCKEN  = 872      # LOCK_ENTITY (Milan 5.4.2.2, IEEE 7.4.2)
E_LOCKUNS = 896      # unsolicited LOCK_ENTITY (Table 5.22 + 7.5.2)
E_GSTRI   = 912      # GET_STREAM_INFO (Milan 5.4.2.10 80-byte response)
E_GAVB    = 944      # GET_AVB_INFO (IEEE 7.4.40, Milan 5.4.2.23)
E_GASP    = 976      # GET_AS_PATH (IEEE 7.4.41, Milan 5.4.2.24)
E_GAMAPO  = 996      # GET_AUDIO_MAP on a Stream Port OUTPUT (Milan 5.4.2.26)
E_GCTRSNS = 796      # GET_COUNTERS on a type with no counters: NOT_SUPPORTED
# --- the read-side command set (Milan 5.4.2.3/.6/.8/.14/.16) -----------------
# Words 998..2047 were one contiguous free run; these five take the front of
# it on 16-word boundaries so a program can grow by a few words without
# renumbering its neighbours.
E_EAVL    = 1008     # ENTITY_AVAILABLE (Milan 5.4.2.3, IEEE 7.4.3)
# The read-side programs moved to 32-word slots at VERSION 0x004C: each grew
# an OVERLAY ARM (read the dynamic-state store's valid flag, branch, take the
# setting or the image) and no longer fits a 16-word pitch. The slots are 32
# so the SET family's validation arms can grow into them without another
# renumbering; 1,000 words of the ROM are still free.
E_GCFG    = 1024     # GET_CONFIGURATION (Milan 5.4.2.6, IEEE 7.4.8)
E_GSFMT   = 1056     # GET_STREAM_FORMAT (Milan 5.4.2.8, IEEE 7.4.10)
E_GSRATE  = 1088     # GET_SAMPLING_RATE (Milan 5.4.2.14, IEEE 7.4.22)
E_GCLKS   = 1120     # GET_CLOCK_SOURCE (Milan 5.4.2.16, IEEE 7.4.24)
E_SSRATE = 1152     # SET_SAMPLING_RATE (Milan 5.4.2.13, IEEE 7.4.21)
E_SCLKS   = 1184     # SET_CLOCK_SOURCE (Milan 5.4.2.15, IEEE 7.4.23)
# ...and the two wrong-target refusals they share. Table 7-141's NOT_SUPPORTED
# ("the command is implemented but the target of the command is not
# supported") has to carry the FULL response body, not a command-sized echo -
# the 0x0049/0x004A GET_COUNTERS round is the precedent and la_avdecc's
# checkResponsePayload is the reason. A zero-valued body is the same wire bytes
# for GET_SAMPLING_RATE (rate 0) and GET_CLOCK_SOURCE (index 0 + reserved 0),
# so ONE stub serves both and the engine grows two µPC arms, not five.
E_GCTRL   = 1248     # GET_CONTROL (Milan 5.4.2.18, IEEE 7.4.26)
E_SCTRL   = 1280     # SET_CONTROL (Milan 5.4.2.17, IEEE 7.4.25)
E_TIZ8NS  = 1216     # {type, index} + 8 zero bytes, NOT_SUPPORTED
E_TIZ4NS  = 1224     # {type, index} + 4 zero bytes, NOT_SUPPORTED
E_LOCKED4 = 1232     # {type, index} + 4 zero bytes, ENTITY_LOCKED
E_BADARG4 = 1240     # {type, index} + 4 zero bytes, BAD_ARGUMENTS
# ...and the same two refusals in the CONTROL response FORM. A refusal has to
# be the size of the response it refuses (only NOT_IMPLEMENTED may answer at
# command length), and a Milan IDENTIFY control carries ONE value byte, so its
# body is 5 and its cdl 17 where the others are 8 and 20.
E_LOCKED1 = 1312     # {type, index} + 1 zero byte, ENTITY_LOCKED
E_BADARG1 = 1320     # {type, index} + 1 zero byte, BAD_ARGUMENTS
E_NSUPP1  = 1328     # {type, index} + 1 zero byte, NOT_SUPPORTED
# A refusal has to be the size of the response it refuses: the stubs above are
# {type, index} shaped and cannot serve a command whose body is not.
E_SCFG    = 1456     # SET_CONFIGURATION (Milan 5.4.2.5, IEEE 7.4.7)
E_SCFGRUN = 1488     # SET_CONFIGURATION's STREAM_IS_RUNNING arm (dispatch lands
                     # here, so it is the one arm whose status is not already set
                     # by the checking op that branched)
E_SCFGLK  = 1500     # ...its ENTITY_LOCKED arm
E_SCFGBAD = 1513     # ...its BAD_ARGUMENTS arm
E_SCFGEMT = 1526     # ...the body all three share
E_RDESCENT = 1568    # READ_DESCRIPTOR(ENTITY) with current_configuration overlay
DT_CONTROL = 0x001A  # 1722.1-2021 Table 7-1

# --- the dynamic-state store's regions and field selectors -------------------
# KL_aecp_dyn_state.sv owns these; the address carries the FIELD and the
# descriptor index rides its own port, because the state-port address is an
# immediate and nothing else. Region 0x1 is the value, 0x2 the valid flag.
RGN_DYN  = 0x10000
RGN_DYNV = 0x20000
SEL_CFG    = 0 * 8
SEL_RATE   = 1 * 8
SEL_CLKSRC = 2 * 8
SEL_FMTIN  = 3 * 8
SEL_FMTOUT = 4 * 8
SEL_PTOFF  = 5 * 8
SEL_START  = 6 * 8
SEL_IDENT  = 7 * 8
# 1722.1-2021 Table 7-1: the one descriptor type this program is dispatched
# for (KL_aecp_engine refuses every other type back to the NOT_IMPLEMENTED
# echo before dispatch, so the constant emitted at @24 is also a guarantee).
DT_STREAM_PORT_INPUT = 0x000E
DT_STREAM_PORT_OUTPUT = 0x000F

# --- descriptor field offsets the read-side programs address -----------------
# The state port's RGN_DATA reads a 64-bit LANE (st_addr[15:3] picks it) and
# hands it over in WIRE order — byte n of the lane at bit [63-8n -: 8]. The
# µISA has no shift, so a field is only reachable by BUILD_FLD if it ends the
# lane (right-justified), and otherwise has to ride COPY_BUF, which starts at
# the lane boundary. Each offset below is therefore quoted with the lane it
# lives in and how it is taken. Verified against the image the store actually
# serves — avdecc/gen_aem_store.py, not against a reading of the tables.
#
#   ENTITY.current_configuration      @310, lane 304..311, lane bytes 6..7
#                                     -> [15:0], BUILD_FLD FMT_W
#   AUDIO_UNIT.current_sampling_rate  @136, lane 136..143, lane bytes 0..3
#                                     -> lane head, COPY_BUF 4 bytes
#   CLOCK_DOMAIN.clock_source_index   @70,  lane 64..71,   lane bytes 6..7
#                                     -> [15:0], BUILD_FLD FMT_W
ENT_CURCFG_LANE = 304     # ENTITY: gen_aem_store.py d_entity, total 312 B
ENT_DESC_LEN = 312        # IEEE 1722.1-2021 §7.2.1 ENTITY descriptor size
AU_RATE_OFF = 136         # AUDIO_UNIT: `assert len(b) == 144` after the count
CD_SRCIDX_LANE = 64       # CLOCK_DOMAIN: wb["CLOCK_SRC_IDX"] = base + 70

# IEEE 1722.1-2021 Table 7-144 (ENTITY_AVAILABLE flags). The table numbers its
# bits with 0 = MSB, so its "Bit 31" is the LSB — the same convention Table
# 5.16's counters_valid mask 0x00000F3F already pins in this file.
EA_ENTITY_ACQUIRED = 0x00000001   # never set: ACQUIRE_ENTITY is refused (Δ7)
EA_ENTITY_LOCKED = 0x00000002

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
occupied = set()


#! the program count is COUNTED, never restated: three tracks add µprograms to
#! this file in parallel and a hand-maintained total is the one line they all
#! collide on and the first one to go stale
placed = []


def place(at, words):
    for i, w in enumerate(words):
        assert (at + i) not in occupied, f"overlap at {at + i}"
        occupied.add(at + i)
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
# READ_DESCRIPTOR's register contract, set by KL_aecp_engine at dispatch (the
# µISA has no shift, so every field a µprogram emits has to arrive
# right-justified in some register):
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

# --- READ_DESCRIPTOR(ENTITY) dynamic current_configuration overlay ---------
# IEEE 1722.1-2021 §7.4.8.2 makes GET_CONFIGURATION.configuration_index
# equivalent to ENTITY.current_configuration. SET_CONFIGURATION writes the
# dynamic-state store, so returning all 312 static ENTITY bytes after a SET
# makes the two commands disagree. The engine re-dispatches only ENTITY reads
# here after the payload walk has captured descriptor_type.
#
# The overlay is deliberately an assembly operation, not a write into the
# descriptor image: the image remains read-only, other descriptor reads remain
# byte-exact, and a failed SET cannot leak its rejected argument. COPY_BUF
# accepts the 310-byte residual exactly; its final partial lane writes bytes
# beyond resp_len, which are never emitted. A noncanonical ENTITY length falls
# back to E_RDESC so this overlay cannot truncate an image it does not
# understand.
place(E_RDESCENT, [
    u('MOVE', rd=12, ra=0, imm=0),               # reserved @26
    u('READ_ST', rd=9, imm=RGN_NCFG),            # configurations_count
    u('CHECK_ARG', ra=14, rb=9, fmt=FMT_W,
      cnd=REL_LT, imm=E_RDSTUB),
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # requested ENTITY[index]
    u('BR_STATUS', cnd=0, imm=E_RDSTUB),
    u('READ_ST', rd=8, imm=RGN_LEN),
    u('COMPARE', ra=8, fmt=FMT_W, imm=ENT_DESC_LEN),
    u('BR_STATUS', cnd=2, imm=E_RDESCENT + 9),   # canonical 312-byte ENTITY
    u('BRANCH', imm=E_RDESC),                    # preserve unknown layouts
    u('MOVE', rd=8, ra=0, imm=ENT_DESC_LEN - 2), # static prefix length
    u('READ_ST', rd=3, imm=RGN_DYNV + SEL_CFG),  # controller-set value valid?
    u('COMPARE', ra=3, fmt=FMT_D, imm=0),
    u('BR_STATUS', cnd=2, imm=E_RDESCENT + 15),  # unset -> image default
    u('READ_ST', rd=1, imm=RGN_DYN + SEL_CFG),
    u('BRANCH', imm=E_RDESCENT + 17),
    u('READ_ST', rd=1, imm=RGN_DATA + ENT_CURCFG_LANE),
    u('NOP', imm=1),                             # image-read writeback seam
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=14, fmt=FMT_W),            # configuration_index @24
    u('BUILD_FLD', ra=12, fmt=FMT_W),            # reserved @26
    u('COPY_BUF', ra=8, imm=RGN_DATA),           # ENTITY bytes 0..309
    u('BUILD_FLD', ra=1, fmt=FMT_W),             # current_configuration
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
# Figure 7-67 fixes ONE response for every status: descriptor_type @24,
# descriptor_index @26, counters_valid @28, then a block of THIRTY-TWO
# quadlets @32..@156 - payload 136 B, AECPDU 160 B, control_data_length 148
# (the offset-from-@12 convention, F06.14). §7.4.42 defines no separate error
# form, so an error answer carries the same fixed body with counters_valid 0
# and a zero block - nothing exists, nothing claimed.
#
# EXISTENCE IS THE STORE'S, the bench probe's first strictness rule: the
# program opens with the same RGN_LOCATE E_GAMAP opens with, so a
# descriptor_index the image lacks answers Table 7-141's NO_SUCH_DESCRIPTOR
# ("A descriptor with the descriptor_type and descriptor_index specified does
# not exist") for exactly the indices READ_DESCRIPTOR refuses - never SUCCESS
# over an empty mask. The MISS arm emits the fixed zero body WITHOUT touching
# the gather face at all: the integrator's wrong-object guard stays as the
# second line of defense, not the answer. (The type gate - GET_COUNTERS on a
# type this build keeps no counters for answers NOT_SUPPORTED - lives in
# KL_aecp_engine's registered A_PLD-exit re-dispatch, not here: the refused
# command never dispatches this program.)
#
# The register contract, set by KL_aecp_engine at dispatch (now the same
# shape as E_GSTRI's):
#   r14 = {16'd0, descriptor_index, descriptor_type, 16'd0} - the store's
#         locate key on st_wdata ({index, type, cfg 0})
#   r13 = {32'd0, descriptor_type, descriptor_index} - one FMT_D BUILD_FLD
#         lays @24..@27 in wire order
# The 32 quadlets never touch a register, READ_CTRS moving them gather-port-
# to-response-buffer one beat at a time; the MISS arm's 33 zero dwords ride
# the APPEND iterator instead.
place(E_GCTRS, [
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),               # miss -> NSD
    u('BR_STATUS', cnd=0, imm=E_GCTRS + 17),             # miss: the zero body
    u('SET_STATUS', imm=ST_OK),
    u('GATHER_EXT', rd=1, cnd=GX_CTR_MASK_CND, imm=0),   # counters_valid
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=1,  fmt=FMT_D),            # counters_valid    @28
] + [u('READ_CTRS', cnd=c) for c in GX_CTR_BLOCK_CND] + [   # 32 quadlets @32
    u('SEND_RESP'),
    u('END'),
    # miss arm (E_GCTRS + 17): the same fixed body, all zero, no gathers
    u('MOVE', rd=2, ra=0, imm=0),
    u('MOVE', rd=6, ra=0, imm=33),               # mask dword + 32 quadlets
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('ITER_OPEN', ra=6),
    u('BR_STATUS', cnd=1, imm=E_GCTRS + 26),     # loop: test FIRST
    u('APPEND', ra=2, fmt=FMT_D),
    u('ITER_NEXT'),
    u('BRANCH', imm=E_GCTRS + 22),
    u('SEND_RESP'),                              # out:
    u('END'),
])

# --- GET_COUNTERS on a counter-less type (the la_avdecc size law) ------------
# Table 7-141's NOT_SUPPORTED for a target outside the kept set - carried in
# the FULL fixed Figure 7-67 body, because the reference stack sizes every
# non-success AEM response except NOT_IMPLEMENTED against the RESPONSE length:
# la_avdecc protocolAemPayloads.cpp checkResponsePayload reflects ONLY
# NotImplemented at command length ("If status is NotImplemented, we expect a
# reflected message (using Command length)") and demands
# >= AecpAemGetCountersResponsePayloadSize (136) for everything else - a
# command-sized NOT_SUPPORTED echo is exactly the "Incorrect payload size"
# complaint the r49a bench probe logged. Two words: set the status, fall into
# E_GCTRS's zero-body emitter (type/index echoed, zero mask, zero block,
# cdl 148, no gathers).
place(E_GCTRSNS, [
    u('SET_STATUS', imm=ST_NSUPP),
    u('BRANCH', imm=E_GCTRS + 17),
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
#   - EXISTENCE is the DESCRIPTOR STORE's: DESC_ADDR locates the addressed
#     STREAM_PORT_INPUT or STREAM_PORT_OUTPUT in the READ_DESCRIPTOR image, so
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
#         locate key ({index, type, cfg 0} on st_wdata; GET_AUDIO_MAP names no
#         configuration_index, and the shipping image has one configuration.
#         This literal key must change before a multi-configuration image ships)
#   r13 = {32'd0, descriptor_index, map_index} - one FMT_D BUILD_FLD emits
#         @26..@29 in wire order, and [15:0] is the CHECK_ARG operand
#         (the µISA has no shift, so the engine packs each field where a
#         µop can reach it)
# The descriptor_type emitted at @24 is a MOVE constant. STREAM_PORT_INPUT
# enters here, STREAM_PORT_OUTPUT enters through E_GAMAPO and overrides the
# constant, and every other descriptor type keeps the NOT_IMPLEMENTED echo.
# r14[15:0], where other programs keep the @24 field, is the locate key's cfg
# half instead.
place(E_GAMAP, [
    #! the type constant loads FIRST so E_GAMAPO below can override it and
    #! fall into the shared tail - one program, two Table 7-1 types
    u('MOVE', rd=8, ra=0, imm=DT_STREAM_PORT_INPUT),  # descriptor_type @24
    u('MOVE', rd=12, ra=0, imm=0),                    # reserved @34
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

# --- echo + NOT_SUPPORTED ----------------------------------------------------
# ACQUIRE_ENTITY (Milan §5.4.2.1: "The PAAD-AE shall not reply SUCCESS to an
# ACQUIRE_ENTITY command. It should reply with the NOT_SUPPORTED error code")
# and a LOCK_ENTITY naming any target but ENTITY[0] (Milan §5.4.2.2:
# "NOT_SUPPORTED shall be returned in this case"). The response is the
# command's own payload echoed: for ACQUIRE that IS §7.4.1.1's response
# layout, because a command carries flags + owner_id 0 + the descriptor, and
# a PAAD that can never be acquired answers owner_id 0.
place(E_NSUPPE, [
    u('SET_STATUS', imm=ST_NSUPP),
    u('BUILD_HDR', ra=15, rb=13),
    u('SEND_RESP'),
    u('END'),
])

# --- LOCK_ENTITY (IEEE §7.4.2, Milan §5.4.2.2) -------------------------------
# The op and the readback both ride the rgy face (KL_aecp_notify owns the
# state): RG_OP performs LOCK or UNLOCK - the engine derives which from flags
# bit 0, the §7.4.2.1 UNLOCK flag - and answers {bit1 changed, bit0 denied};
# RG_STATE reads the holder AFTER the op, which is exactly §7.4.2.1's
# locked_id ("set to the Entity ID of the ATDECC Controller that is holding
# the lock in a response", 0 when free). Denied maps to ENTITY_LOCKED - the
# refusal that names the holder is also the answer to Milan's UNLOCK-as-query
# note. Register contract: r14[31:0] = the command's flags word (engine-
# packed, emitted verbatim at @24); the descriptor echo is constant 0 because
# only ENTITY[0] ever reaches this program (the engine's walk gate).
# The MOVE between the two gathers is also the once-per-edge gap the notify
# face requires.
place(E_LOCKEN, [
    u('GATHER_EXT', rd=1, **RG_OP),              # the lock/unlock op
    u('MOVE', rd=12, ra=0, imm=0),               # r12 = 0 (+ the gather gap)
    u('GATHER_EXT', rd=3, **RG_STATE),           # locked_id, post-op
    u('COMPARE', ra=1, fmt=FMT_D, imm=1),        # z = denied
    u('BR_STATUS', cnd=2, imm=E_LOCKEN + 7),     # denied ->
    u('SET_STATUS', imm=ST_OK),
    u('BRANCH', imm=E_LOCKEN + 8),
    u('SET_STATUS', imm=ST_LOCKED),              # denied: ENTITY_LOCKED
    u('BUILD_HDR', ra=15, rb=13),                # emit (both statuses):
    u('BUILD_FLD', ra=14, fmt=FMT_D),            # flags echo          @24
    u('BUILD_FLD', ra=3,  fmt=FMT_Q),            # locked_id           @28
    u('BUILD_FLD', ra=12, fmt=FMT_D),            # ENTITY[0] echo      @36
    u('SEND_RESP'),
    u('END'),
])

# --- unsolicited LOCK_ENTITY (IEEE §7.5.2 list + Milan Table 5.22) -----------
# Sent on every lock-state CHANGE (took, released, 60 s auto-unlock) to every
# registered controller except the requester. The body reports CURRENT state:
# flags 0, locked_id = the holder now (0 after an unlock or the timeout - the
# Table 5.22 row is exactly "the entity automatically unlocks itself").
place(E_LOCKUNS, [
    u('GATHER_EXT', rd=3, **RG_STATE),           # the holder right now
    u('MOVE', rd=12, ra=0, imm=0),
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=12, fmt=FMT_D),            # flags               @24
    u('BUILD_FLD', ra=3,  fmt=FMT_Q),            # locked_id           @28
    u('BUILD_FLD', ra=12, fmt=FMT_D),            # ENTITY[0]           @36
    u('SEND_RESP'),
    u('END'),
])

# --- gather selectors the Milan-info face answers (06 §6.2/§6.10) ------------
# cnd 0xB carries the face's word selector in imm[3:0]; the engine forwards
# the low nibble on gsi_sel_o and the KIND on gsi_kind_o, so the three
# commands own disjoint word tables behind one port group. GET_STREAM_INFO's
# words, each one BUILD_FLD wide (offsets are AECPDU bytes of Milan v1.2
# Figure 5.1):
#   0 FLAGS   {32'0, flags}                                     -> @28 dword
#   1 FMT     stream_format                                     -> @32 qword
#   2 SID     stream_id                                         -> @40 qword
#   3 LAT     {32'0, msrp_accumulated_latency}                  -> @48 dword
#   4 DMACFC  {stream_dest_mac, msrp_failure_code, 8'0}         -> @52 qword
#   5 BRIDGE  msrp_failure_bridge_id                            -> @60 qword
#   6 VLANEX  {stream_vlan_id, 16'0, flags_ex}                  -> @68 qword
#   7 PBSTA   {32'0, {pbsta[2:0], acmpsta[4:0]}, 24'0}          -> @76 dword
def GSI(n):
    return dict(cnd=0xB, imm=n)

# --- GET_STREAM_INFO (IEEE §7.4.16, Milan §5.4.2.10) -------------------------
# Milan replaces §7.4.15.1's response with the 80-byte Figure 5.1 layout:
# the IEEE body through stream_vlan_id, a reserved word, flags_ex, and the
# pbsta/acmpsta byte (§5.3.8.6: a 3-bit probing status and the 5-bit ACMP
# status). Payload 56, cdl 68 - "a 1722.1 controller ... will use the
# control_data_length field ... to determine if the new fields are present".
#
# WHO DECIDES WHAT (the §6.2 split): EXISTENCE is the descriptor store's
# (DESC_ADDR against the same image READ_DESCRIPTOR serves - a miss answers
# NO_SUCH_DESCRIPTOR with the full, zero-flagged body); every VALUE and every
# VALIDITY FLAG is the integrator's, through the gsi face - the binding view,
# SRP registrars, probing state and formats live there, and Milan's validity
# matrix (Tables 5.9-5.12) is exactly "which of the integrator's registers
# hold truth right now", which no parser can second-guess. An unwired face
# answers all-zero flags: every field honestly absent.
#
# Register contract (engine at dispatch): r14 = the locate key
# {index, type, cfg 0}; r13 = {32'0, descriptor_type, descriptor_index} so
# one FMT_D BUILD_FLD lays @24..@27 in wire order.
place(E_GSTRI, [
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # miss -> NO_SUCH_DESCRIPTOR
    u('BR_STATUS', cnd=0, imm=E_GSTRI + 3),      # (status survives the skip)
    u('SET_STATUS', imm=ST_OK),
    u('GATHER_EXT', rd=1, **GSI(0)),             # flags
    u('GATHER_EXT', rd=2, **GSI(1)),             # stream_format
    u('GATHER_EXT', rd=3, **GSI(2)),             # stream_id
    u('GATHER_EXT', rd=4, **GSI(3)),             # msrp_accumulated_latency
    u('GATHER_EXT', rd=5, **GSI(4)),             # dmac + failure_code
    u('GATHER_EXT', rd=6, **GSI(5)),             # failure_bridge_id
    u('GATHER_EXT', rd=7, **GSI(6)),             # vlan + flags_ex
    u('GATHER_EXT', rd=8, **GSI(7)),             # pbsta/acmpsta
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type + index          @24
    u('BUILD_FLD', ra=1,  fmt=FMT_D),            # flags                 @28
    u('BUILD_FLD', ra=2,  fmt=FMT_Q),            # stream_format         @32
    u('BUILD_FLD', ra=3,  fmt=FMT_Q),            # stream_id             @40
    u('BUILD_FLD', ra=4,  fmt=FMT_D),            # msrp_acc_latency      @48
    u('BUILD_FLD', ra=5,  fmt=FMT_Q),            # dmac + fail_code      @52
    u('BUILD_FLD', ra=6,  fmt=FMT_Q),            # failure_bridge_id     @60
    u('BUILD_FLD', ra=7,  fmt=FMT_Q),            # vlan + resv + flags_ex @68
    u('BUILD_FLD', ra=8,  fmt=FMT_D),            # pbsta/acmpsta + resv  @76
    u('SEND_RESP'),
    u('END'),
])

# --- GET_AVB_INFO (IEEE §7.4.40, Milan §5.4.2.23) ----------------------------
# Response (Figure 7-63, AECPDU offsets): descriptor_type @24, index @26,
# gptp_grandmaster_id @28, propagation_delay @36, gptp_domain_number @40,
# flags @41, msrp_mappings_count @42, then 4-byte {traffic_class, priority,
# vlan_id} mappings @44. The gPTP words and the mapping list are the
# INTEGRATOR's through the Milan-info face (kind 1):
#   sel 0 -> gptp_grandmaster_id (qword)
#   sel 1 -> {propagation_delay[31:0], domain[7:0], flags[7:0], count[15:0]}
#            - one qword lays @36..@43 in wire order, and [15:0] doubles as
#            the ITER_OPEN count exactly like the audio-map GEOM word
#   sel 8 -> mapping record ordinal gsi_ord_o (record-class: bit 3)
# A face that answers count 0 emits an EMPTY list and cdl 32 - absent, never
# invented; propagation_delay 0 is likewise the honest "not measured" (the
# reference fabric's gPTP plane does not export pDelay - recorded in 06 §7).
place(E_GAVB, [
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # miss -> NO_SUCH_DESCRIPTOR
    u('BR_STATUS', cnd=0, imm=E_GAVB + 3),
    u('SET_STATUS', imm=ST_OK),
    u('GATHER_EXT', rd=1, cnd=0xB, imm=0),       # grandmaster_id
    u('GATHER_EXT', rd=2, cnd=0xB, imm=1),       # {pdelay, domain, flags, count}
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type + index          @24
    u('BUILD_FLD', ra=1,  fmt=FMT_Q),            # gptp_grandmaster_id   @28
    u('BUILD_FLD', ra=2,  fmt=FMT_Q),            # pdelay+domain+flags+count @36
    u('ITER_OPEN', ra=2),                        # count = r2[7:0]
    u('BR_STATUS', cnd=1, imm=E_GAVB + 15),      # loop: 0-trip safe
    u('GATHER_EXT', rd=3, cnd=0xB, imm=8),       # mapping[gsi_ord]
    u('APPEND', ra=3, fmt=FMT_D),                # {tc, prio, vid} @44 + 4k
    u('ITER_NEXT'),
    u('BRANCH', imm=E_GAVB + 10),
    u('SEND_RESP'),                              # out:
    u('END'),
])

# --- GET_AS_PATH (IEEE §7.4.41, Milan §5.4.2.24) -----------------------------
# "The path_sequence field is set to pathSequence of the latest IEEE Std
# 802.1AS-2020 Announce message PathTrace TLV" - which of the path a leaf
# device KNOWS is the integrator's affair: the reference fabric carries only
# the elected grandmaster's identity, so its face answers count 1 with that
# one ClockIdentity (count 0 with no GM) - the conformant minimal answer,
# recorded in 06 §7. The command carries the INDEX at @24 (§7.4.41.1 - no
# type field), so r13[15:0] is the index and the locate key is engine-packed
# with the AVB_INTERFACE constant. Face words (kind 2): sel 0 -> {48'0,
# count}; sel 8 -> path entry gsi_ord_o (record-class).
place(E_GASP, [
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # miss -> NO_SUCH_DESCRIPTOR
    u('BR_STATUS', cnd=0, imm=E_GASP + 3),
    u('SET_STATUS', imm=ST_OK),
    u('GATHER_EXT', rd=2, cnd=0xB, imm=0),       # count
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_W),            # descriptor_index      @24
    u('BUILD_FLD', ra=2,  fmt=FMT_W),            # count                 @26
    u('ITER_OPEN', ra=2),
    u('BR_STATUS', cnd=1, imm=E_GASP + 13),      # loop: 0-trip safe
    u('GATHER_EXT', rd=3, cnd=0xB, imm=8),       # path_sequence[gsi_ord]
    u('APPEND', ra=3, fmt=FMT_Q),                # ClockIdentity @28 + 8k
    u('ITER_NEXT'),
    u('BRANCH', imm=E_GASP + 8),
    u('SEND_RESP'),                              # out:
    u('END'),
])

# --- GET_AUDIO_MAP, Stream Port OUTPUT (Milan §5.4.2.26's second half) -------
# "the PAAD-AE shall implement the GET_AUDIO_MAP command ... for each Stream
# Port Output of the currently set Configuration" - the half that was a
# recorded gap while the capture-side map RAM had no readback. The program IS
# E_GAMAP with the other Table 7-1 type constant: the engine dispatches by
# the walked descriptor_type, the locate key already carries it, and the
# integrator's face routes on amap_desc_type_o to the store for that direction.
place(E_GAMAPO, [
    u('MOVE', rd=8, ra=0, imm=DT_STREAM_PORT_OUTPUT),
    u('BRANCH', imm=E_GAMAP + 1),
])

# =============================================================================
# The read-side command set: five commands Milan v1.2 §5.4.2 makes a SHALL and
# this engine answered with the NOT_IMPLEMENTED echo until now.
#
# All five are pure reads. None of them changes entity state, so none needs the
# lock check, the NVM mark or a notification — which is exactly why they land
# together and ahead of the SET family. Three read the descriptor image the
# store already serves READ_DESCRIPTOR from, one reads the integrator's
# GET_STREAM_INFO face, one reads the lock holder off the registry face.
#
# EXISTENCE IS THE STORE'S in every one of them, the rule GET_COUNTERS and
# GET_AUDIO_MAP already follow: a {type, index} the image lacks answers
# NO_SUCH_DESCRIPTOR carried in the FULL fixed response body, zero-valued.
# la_avdecc's checkResponsePayload reflects only NOT_IMPLEMENTED at command
# length and sizes every other status against the RESPONSE form, so a
# short error body is an "Incorrect payload size" complaint, not an answer.
# =============================================================================

# --- ENTITY_AVAILABLE (Milan §5.4.2.3, IEEE §7.4.3.2, Figure 7-29) -----------
# "The PAAD-AE shall implement the ENTITY_AVAILABLE command as specified in
# [ATDECC, Clause 7.4.3]" — the liveness probe every controller leans on, and
# the 2021 response is no longer payload-less: flags @24, acquired_controller_id
# @28, locked_controller_id @36. Payload 20, cdl 32.
#
# acquired_controller_id is INVARIANTLY zero and ENTITY_ACQUIRED is invariantly
# clear, because Milan Δ7 forbids this entity ever granting ACQUIRE_ENTITY
# (§5.4.2.1, the E_NSUPPE arm). The lock half is real: the holder eid comes off
# the registry face with rgy_state = 1, which KL_aecp_notify answers with the
# holder and NO walk and NO op — so this command can never disturb the
# registry it reads. A zero holder IS "not locked" (§7.4.2's own convention),
# so one 64-bit COMPARE decides the flag.
place(E_EAVL, [
    u('MOVE', rd=2, ra=0, imm=0),                # acquired_controller_id = 0
    u('MOVE', rd=3, ra=0, imm=0),                # flags = 0 (not locked)
    u('GATHER_EXT', rd=1, cnd=0, imm=1),         # r1 = lock holder (sel[0]=1)
    u('COMPARE', ra=1, fmt=FMT_Q, imm=0),        # z = holder is zero
    u('BR_STATUS', cnd=2, imm=E_EAVL + 6),       # unlocked: keep flags 0
    u('MOVE', rd=3, ra=0, imm=EA_ENTITY_LOCKED),
    u('SET_STATUS', imm=ST_OK),                  # E_EAVL + 6
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=3, fmt=FMT_D),             # flags                  @24
    u('BUILD_FLD', ra=2, fmt=FMT_Q),             # acquired_controller_id @28
    u('BUILD_FLD', ra=1, fmt=FMT_Q),             # locked_controller_id   @36
    u('SEND_RESP'),
    u('END'),
])

# --- GET_CONFIGURATION (Milan §5.4.2.6, IEEE §7.4.8.2, Figure 7-33) ----------
# Response payload: reserved @24, configuration_index @26. Payload 4, cdl 16.
# §7.4.8.2: "set to descriptor_index of the current configuration. This is
# equivalent to the current_configuration field in the ENTITY descriptor" — so
# the ENTITY descriptor IS the source, read from the same image READ_DESCRIPTOR
# serves rather than from a second copy that could disagree with it.
#
# The command carries no payload, so nothing walks and r14 would already be the
# ENTITY[0] key by the A_IDLE zeroing. The key is built explicitly anyway: a
# malformed command claiming a longer cdl DOES walk @24.. into cfg_ix_r, and a
# locate must not follow whatever that put there.
# THE OVERLAY ARM MATTERS MORE HERE THAN ANYWHERE ELSE. SET_CONFIGURATION
# writes the dynamic store; without this arm GET_CONFIGURATION goes on reading
# the ENTITY descriptor's STATIC current_configuration and the round trip does
# not exist — a controller sets 1 and reads back 0 forever. The first cut of
# this pair shipped exactly that way, and the test could not see it because
# both sources read 0 at reset and the test only ever used index 0.
place(E_GCFG, [
    u('MOVE', rd=2, ra=0, imm=0),                # ENTITY[0] key AND @24 reserved
    u('MOVE', rd=1, ra=0, imm=0),                # configuration_index, miss = 0
    u('READ_ST', rd=3, imm=RGN_DYNV + SEL_CFG),  # has a controller set it?
    u('COMPARE', ra=3, fmt=FMT_D, imm=0),
    u('BR_STATUS', cnd=2, imm=E_GCFG + 7),       # z = unset -> the image
    u('READ_ST', rd=1, imm=RGN_DYN + SEL_CFG),   # the controller's value
    u('BRANCH', imm=E_GCFG + 11),
    u('DESC_ADDR', ra=2, imm=RGN_LOCATE),        # E_GCFG + 7: the image arm
    #! to +12, NOT +11: +11 is SET_STATUS ST_OK, and landing on it would
    #! overwrite the NO_SUCH_DESCRIPTOR that DESC_ADDR just set with SUCCESS.
    u('BR_STATUS', cnd=0, imm=E_GCFG + 12),
    u('READ_ST', rd=1, imm=RGN_DATA + ENT_CURCFG_LANE),
    u('NOP', imm=1),
    u('SET_STATUS', imm=ST_OK),                  # E_GCFG + 11
    u('BUILD_HDR', ra=15, rb=13),                # E_GCFG + 12
    u('BUILD_FLD', ra=2, fmt=FMT_W),             # reserved            @24
    u('BUILD_FLD', ra=1, fmt=FMT_W),             # configuration_index @26
    u('SEND_RESP'),
    u('END'),
])

# --- GET_STREAM_FORMAT (Milan §5.4.2.8, IEEE §7.4.10.2, Figure 7-34) ---------
# Response payload: descriptor_type @24, descriptor_index @26, stream_format
# @28. Payload 12, cdl 24.
#
# The VALUE is the integrator's, not the image's. §7.4.10.2 calls stream_format
# "the current stream format", and current means after any SET_STREAM_FORMAT
# and after the Milan §5.5 binding handshake has adapted a listener — the
# image's current_format is only the power-on default. The gsi face already
# publishes exactly this qword as GET_STREAM_INFO's sel 1, so this command
# reuses the face rather than opening a second, divergeable path to the same
# fact. EXISTENCE stays the store's, as in E_GSTRI.
place(E_GSFMT, [
    u('MOVE', rd=2, ra=0, imm=0),                # the miss arm's zero format
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # miss -> NO_SUCH_DESCRIPTOR
    u('BR_STATUS', cnd=0, imm=E_GSFMT + 5),      # miss: zero body, no gather
    u('SET_STATUS', imm=ST_OK),
    u('GATHER_EXT', rd=2, **GSI(1)),             # stream_format
    u('BUILD_HDR', ra=15, rb=13),                # E_GSFMT + 5
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=2, fmt=FMT_Q),             # stream_format        @28
    u('SEND_RESP'),
    u('END'),
])

# --- GET_SAMPLING_RATE (Milan §5.4.2.14, IEEE §7.4.22.2, Figure 7-45) --------
# "For each Audio Unit, the PAAD-AE shall implement the GET_SAMPLING_RATE
# command". Response payload: descriptor_type @24, descriptor_index @26,
# sampling_rate @28. Payload 8, cdl 20.
#
# current_sampling_rate opens its lane (offset 136, and the generator's own
# `assert len(b) == 144` two fields later is what pins that), so it cannot be
# taken right-justified into a register and rides COPY_BUF from the lane head
# instead. That splits the emitter in two: the hit arm copies four bytes out of
# the image, the miss arm emits four zero bytes, and both fall into one
# SEND_RESP so the response form is identical either way.
# THE OVERLAY ARM, and it is the same three instructions in every GET below.
# A controller that has issued SET_SAMPLING_RATE owns this value; until then
# the descriptor image does. So: read the dynamic store's VALID flag for this
# field and descriptor, compare it against zero, and branch to the image arm
# when it is clear. Reading the store unconditionally would report a
# controller's setting that nobody made (every row reads zero-and-invalid out
# of reset); reading the image unconditionally would lose the setting.
place(E_GSRATE, [
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # existence is the image's
    u('MOVE', rd=8, ra=0, imm=4),                # COPY_BUF length
    u('MOVE', rd=2, ra=0, imm=0),                # the miss arm's zero rate
    u('BR_STATUS', cnd=0, imm=E_GSRATE + 16),    # miss -> NO_SUCH_DESCRIPTOR
    u('SET_STATUS', imm=ST_OK),
    u('READ_ST', rd=3, imm=RGN_DYNV + SEL_RATE), # has a controller set it?
    u('COMPARE', ra=3, fmt=FMT_D, imm=0),
    u('BR_STATUS', cnd=2, imm=E_GSRATE + 11),    # z = unset -> the image
    u('READ_ST', rd=2, imm=RGN_DYN + SEL_RATE),  # the controller's value
    u('BRANCH', imm=E_GSRATE + 16),              # ...and emit it as a dword
    u('NOP'),
    u('BUILD_HDR', ra=15, rb=13),                # E_GSRATE + 11: the image arm
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('COPY_BUF', ra=8, imm=RGN_DATA + AU_RATE_OFF),   # sampling_rate  @28
    u('SEND_RESP'),
    u('END'),
    u('BUILD_HDR', ra=15, rb=13),                # E_GSRATE + 16: value in r2
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=2, fmt=FMT_D),             # the rate, or zero on a miss
    u('SEND_RESP'),
    u('END'),
])

# --- SET_SAMPLING_RATE (Milan §5.4.2.13, IEEE §7.4.21.1, Figure 7-45) -------
# "For each Audio Unit of the currently set Configuration, the PAAD-AE shall
# implement the SET_SAMPLING_RATE command". Command and response share the
# figure, so the response is the same 8-byte body the getter emits and it
# carries the value now in force.
#
# THE LOCK IS THE FIRST THING CHECKED, before the locate and before the write:
# Milan repeats in every SET clause that a locked PAAD "shall not accept a
# <CMD> command from a different controller". CHECK_LOCK branches when the
# entity is held by somebody other than r15's controller, and sets
# ENTITY_LOCKED on the way out.
#
# The rate/mapping-mismatch refusal of §5.4.2.13 is a MAY, not a SHALL, and is
# deliberately not implemented — grading a MAY as a SHALL would refuse rates
# this device can actually serve.
place(E_SSRATE, [
    u('MOVE', rd=2, ra=0, imm=0),                # the refusal arms' zero body
    u('CHECK_LOCK', ra=15, imm=E_LOCKED4),       # held by another controller?
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # does the Audio Unit exist?
    u('BR_STATUS', cnd=0, imm=E_SSRATE + 12),   # no -> NO_SUCH_DESCRIPTOR
    u('WRITE_ST', ra=12, fmt=FMT_D, imm=RGN_DYN + SEL_RATE),
    u('NVM_MARK', imm=1),                        # §5.3.5.1: persist it
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=12, fmt=FMT_D),            # the rate now in force @28
    u('SEND_RESP'),
    u('END'),
    u('BUILD_HDR', ra=15, rb=13),                # E_SSRATE + 12: the refusal
    u('BUILD_FLD', ra=13, fmt=FMT_D),
    u('BUILD_FLD', ra=2, fmt=FMT_D),
    u('SEND_RESP'),
    u('END'),
])

# --- SET_CLOCK_SOURCE (Milan §5.4.2.15, IEEE §7.4.23.1, Figure 7-47) --------
# "For each Clock Domain, the PAAD-AE shall implement the SET_CLOCK_SOURCE
# command." Same shape as the setter above; the response body is
# clock_source_index @28 + reserved @30.
#
# THIS ONE IS LOAD-BEARING BEYOND ITS OWN CLAUSE. It is the only writer of the
# live clock_source_index, which is why the CRF media clock could never be
# selected and why KL_mmcm_drp_servo and the packet-grid NCO were
# structurally off: nothing could move the index off 0 = INTERNAL.
place(E_SCLKS, [
    u('MOVE', rd=2, ra=0, imm=0),                # reserved @30, and the refusals
    u('CHECK_LOCK', ra=15, imm=E_LOCKED4),
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # does the Clock Domain exist?
    u('BR_STATUS', cnd=0, imm=E_SCLKS + 13),
    u('WRITE_ST', ra=12, fmt=FMT_W, imm=RGN_DYN + SEL_CLKSRC),
    u('NVM_MARK', imm=1),                        # §5.3.11.1: persist it
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=12, fmt=FMT_W),            # clock_source_index   @28
    u('BUILD_FLD', ra=2, fmt=FMT_W),             # reserved             @30
    u('SEND_RESP'),
    u('END'),
    u('BUILD_HDR', ra=15, rb=13),                # E_SCLKS + 13: the refusal
    u('BUILD_FLD', ra=13, fmt=FMT_D),
    u('BUILD_FLD', ra=2, fmt=FMT_D),             # index 0 + reserved 0
    u('SEND_RESP'),
    u('END'),
])

# --- GET_CLOCK_SOURCE (Milan §5.4.2.16, IEEE §7.4.24.2, Figure 7-47) ---------
# "For each Clock Domain, the PAAD-AE shall implement the GET_CLOCK_SOURCE
# command". Response payload: descriptor_type @24, descriptor_index @26,
# clock_source_index @28, reserved @30. Payload 8, cdl 20.
#
# clock_source_index ENDS its lane, so one READ_ST puts it right-justified and
# no COPY_BUF is needed. Note what this answers today: the index is read from
# the image, and until SET_CLOCK_SOURCE lands the image's 0 (INTERNAL) is also
# the truth — milan_datapath pins clock_source_index at 0 for the life of the
# build, so this is a report, not a claim that the field is settable.
place(E_GCLKS, [
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # existence is the image's
    u('MOVE', rd=2, ra=0, imm=0),                # @30 reserved
    u('MOVE', rd=1, ra=0, imm=0),                # the miss arm's zero index
    u('BR_STATUS', cnd=0, imm=E_GCLKS + 11),     # miss -> NO_SUCH_DESCRIPTOR
    u('SET_STATUS', imm=ST_OK),
    u('READ_ST', rd=3, imm=RGN_DYNV + SEL_CLKSRC),   # set by a controller?
    u('COMPARE', ra=3, fmt=FMT_D, imm=0),
    u('BR_STATUS', cnd=2, imm=E_GCLKS + 10),     # z = unset -> the image
    u('READ_ST', rd=1, imm=RGN_DYN + SEL_CLKSRC),    # the controller's value
    u('BRANCH', imm=E_GCLKS + 11),
    u('READ_ST', rd=1, imm=RGN_DATA + CD_SRCIDX_LANE),   # E_GCLKS + 10
    u('BUILD_HDR', ra=15, rb=13),                # E_GCLKS + 11
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=1, fmt=FMT_W),             # clock_source_index   @28
    u('BUILD_FLD', ra=2, fmt=FMT_W),             # reserved             @30
    u('SEND_RESP'),
    u('END'),
])

# --- the shared wrong-target refusals (Table 7-141 NOT_SUPPORTED) ------------
# §7.4.10/§7.4.22/§7.4.24 each name the descriptor types their command acts on,
# and this PAAD implements one of each: STREAM_INPUT/STREAM_OUTPUT for a
# format, AUDIO_UNIT for a rate, CLOCK_DOMAIN for a clock source. A command
# naming anything else is Table 7-141's NOT_SUPPORTED, and the engine's type
# gate re-dispatches HERE rather than to the command-sized E_NSUPPE echo,
# because only NOT_IMPLEMENTED may be command-sized (see the 0x004A round).
# The type and index are echoed so a controller can tell WHICH target was
# refused; every value is zero, because nothing exists to report.
place(E_TIZ8NS, [
    u('MOVE', rd=2, ra=0, imm=0),
    u('SET_STATUS', imm=ST_NSUPP),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=2, fmt=FMT_Q),             # 8 zero bytes         @28
    u('SEND_RESP'),
    u('END'),
])
place(E_TIZ4NS, [
    u('MOVE', rd=2, ra=0, imm=0),
    u('SET_STATUS', imm=ST_NSUPP),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=2, fmt=FMT_D),             # 4 zero bytes         @28
    u('SEND_RESP'),
    u('END'),
])

# --- the SET family's two refusals, in the same full-body form --------------
# Milan makes the lock refusal a SHALL in every SET clause, and the end-station
# test plan's es-4.18 checks that the ENTITY_LOCKED response still carries the
# CURRENT value rather than the rejected one. Zero is what these stubs carry,
# which is correct for the refusal arms that reach them: a locked entity has
# not located anything, so it has no current value to report and inventing one
# would be worse than reporting none.
place(E_LOCKED4, [
    u('MOVE', rd=2, ra=0, imm=0),
    u('SET_STATUS', imm=ST_LOCKED),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=2, fmt=FMT_D),             # 4 zero bytes         @28
    u('SEND_RESP'),
    u('END'),
])
place(E_BADARG4, [
    u('MOVE', rd=2, ra=0, imm=0),
    u('SET_STATUS', imm=ST_BADARG),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),
    u('BUILD_FLD', ra=2, fmt=FMT_D),
    u('SEND_RESP'),
    u('END'),
])

# --- GET_CONTROL (Milan §5.4.2.18, IEEE §7.4.26.2, Figure 7-49) -------------
# "For the 'Identify' Control, the PAAD-AE shall implement the GET_CONTROL
# command." Response: descriptor_type @24, descriptor_index @26, then the
# control's values. A Milan IDENTIFY control is one CONTROL_LINEAR_UINT8
# (IEEE §7.3.5.2), so V = 1, the payload is 5 and the cdl is 17.
#
# THE VALUE COMES FROM THE DYNAMIC STORE ALONE, with no image arm, and that is
# the clause rather than a shortcut: Milan §5.3.12 makes the IDENTIFY value
# VOLATILE with 0 ("not identifying") as its state after reset, and §5.3.4
# lists it among the three things a power cycle clears. An unwritten row reads
# exactly 0, so the store IS the reset default. (The image's copy sits at
# CONTROL offset 108, which is mid-lane and unreachable to a µISA with no
# shift — but even if it were reachable, reading it would be wrong the moment
# SET_CONTROL moved the value.)
place(E_GCTRL, [
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # existence is the image's
    u('MOVE', rd=2, ra=0, imm=0),                # the miss arm's zero value
    u('BR_STATUS', cnd=0, imm=E_GCTRL + 5),      # miss -> NO_SUCH_DESCRIPTOR
    u('SET_STATUS', imm=ST_OK),
    u('READ_ST', rd=2, imm=RGN_DYN + SEL_IDENT),
    u('BUILD_HDR', ra=15, rb=13),                # E_GCTRL + 5
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=2, fmt=FMT_B),             # the value @28, ONE byte
    u('SEND_RESP'),
    u('END'),
])

# --- SET_CONTROL (Milan §5.4.2.17, IEEE §7.4.25.1, Figure 7-49) -------------
# "For the 'Identify' Control, the PAAD-AE shall implement the SET_CONTROL
# command as specified in [ATDECC, Clause 7.4.25] and [ATDECC, Clause
# 7.3.5.2]." Lock-protected like every SET.
#
# ONLY 0 AND 255 ARE LEGAL. §7.3.5.2 gives the Identify control minimum 0,
# maximum 255 and STEP 255, so the step alone admits exactly two values, and
# Milan §5.3.12 names them: 0 = not identifying, 255 = identifying. Anything
# else is IEEE §7.4.25's out-of-range BAD_ARGUMENTS. The µISA has no
# "not equal" branch, so the test is two equality compares that jump to the
# accept and a fall-through that refuses — which also reads in the order the
# clause is written.
#
# No NVM_MARK: §5.3.12 keeps this value volatile, so committing it to flash
# would both violate the clause and burn erase cycles on a blinking front
# panel.
place(E_SCTRL, [
    u('MOVE', rd=2, ra=0, imm=0),                # the refusal arms' zero body
    u('CHECK_LOCK', ra=15, imm=E_LOCKED1),       # held by another controller?
    u('DESC_ADDR', ra=14, imm=RGN_LOCATE),       # does the CONTROL exist?
    u('BR_STATUS', cnd=0, imm=E_SCTRL + 16),     # no -> NO_SUCH_DESCRIPTOR
    u('COMPARE', ra=12, fmt=FMT_B, imm=0),       # value == 0 ?
    u('BR_STATUS', cnd=2, imm=E_SCTRL + 9),
    u('COMPARE', ra=12, fmt=FMT_B, imm=255),     # value == 255 ?
    u('BR_STATUS', cnd=2, imm=E_SCTRL + 9),
    u('BRANCH', imm=E_BADARG1),                  # neither: out of range
    u('WRITE_ST', ra=12, fmt=FMT_B,              # E_SCTRL + 9: accept
      imm=RGN_DYN + SEL_IDENT),
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),            # type @24 + index @26
    u('BUILD_FLD', ra=12, fmt=FMT_B),            # the value now in force @28
    u('SEND_RESP'),
    u('END'),
    u('BUILD_HDR', ra=15, rb=13),                # E_SCTRL + 16: the miss arm
    u('BUILD_FLD', ra=13, fmt=FMT_D),
    u('BUILD_FLD', ra=2, fmt=FMT_B),
    u('SEND_RESP'),
    u('END'),
])

# --- the CONTROL-shaped refusals (cdl 17) -----------------------------------
place(E_LOCKED1, [
    u('MOVE', rd=2, ra=0, imm=0),
    u('SET_STATUS', imm=ST_LOCKED),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),
    u('BUILD_FLD', ra=2, fmt=FMT_B),
    u('SEND_RESP'),
    u('END'),
])
place(E_BADARG1, [
    u('MOVE', rd=2, ra=0, imm=0),
    u('SET_STATUS', imm=ST_BADARG),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),
    u('BUILD_FLD', ra=2, fmt=FMT_B),
    u('SEND_RESP'),
    u('END'),
])
place(E_NSUPP1, [
    u('MOVE', rd=2, ra=0, imm=0),
    u('SET_STATUS', imm=ST_NSUPP),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=13, fmt=FMT_D),
    u('BUILD_FLD', ra=2, fmt=FMT_B),
    u('SEND_RESP'),
    u('END'),
])

# --- SET_CONFIGURATION (Milan §5.4.2.5, IEEE §7.4.7.1, Figure 7-33) ---------
# Body: reserved @24, configuration_index @26 — payload 4, cdl 16. Its @24
# word is a RESERVED field and not a descriptor type, which is why the engine
# packs r13 for it separately from every other command here.
#
# THE REFUSAL IS A REDUCTION OVER EVERY STREAM, not a per-descriptor test:
#
#   "The PAAD-AE shall not accept a SET_CONFIGURATION command if ONE OF the
#    Stream Input is bound or ONE OF the Stream Output is streaming. In this
#    case, the STREAM_IS_RUNNING error code shall be returned."
#
# The engine computes that reduction (`any_running_w`) and refuses at
# dispatch, so this program runs only when the entity is genuinely idle.
place(E_SCFG, [
    u('MOVE', rd=2, ra=0, imm=0),                # the @24 reserved field
    u('CHECK_LOCK', ra=15, imm=E_SCFGLK),        # locked -> ENTITY_LOCKED
    #! the index has to be IN RANGE before it is stored. Without this a
    #! SET_CONFIGURATION(0xFFFF) answers SUCCESS while the dynamic store drops
    #! the write on the floor — a false success. RGN_NCFG is the image's
    #! configurations_count, the authority READ_DESCRIPTOR already checks.
    u('READ_ST', rd=9, imm=RGN_NCFG),
    u('CHECK_ARG', ra=12, rb=9, fmt=FMT_W,
      cnd=REL_LT, imm=E_SCFGBAD),
    u('WRITE_ST', ra=12, fmt=FMT_W, imm=RGN_DYN + SEL_CFG),
    u('NVM_MARK', imm=1),                        # es-5.1 item 1: persist it
    u('SET_STATUS', imm=ST_OK),
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=2, fmt=FMT_W),             # reserved            @24
    u('BUILD_FLD', ra=12, fmt=FMT_W),            # configuration_index @26
    u('SEND_RESP'),
    u('END'),
])

# SET_CONFIGURATION's three refusals share one BODY, and each runs its own copy
# of the current-value overlay. IEEE §7.4.7.1: "The response always contains the
# current value, that is it contains the new value if the command succeeds or
# THE OLD VALUE IF IT FAILS." So a refusal echoes the configuration the entity is
# STILL in, never the one that was rejected.
#
# WHY THE OVERLAY IS DUPLICATED RATHER THAN SHARED. The first cut read
# `RGN_DYN + SEL_CFG` raw, which answers 0 until some controller has written the
# store, so before the first successful SET, every refusal echoed 0 instead of
# the image's ENTITY.current_configuration. E_GCFG has the valid-bit arm that
# fixes this; the refusal emitter did not. Sharing one copy is not expressible
# here: the arm's status has to be applied AFTER the overlay, because the image
# arm's miss guard is `BR_STATUS cnd=0`, which tests `status != SUCCESS` and
# would fire on the refusal itself, and SET_STATUS takes an immediate, so the
# arm cannot be carried through a shared tail in a register. The ROM holds three
# copies; `_scfg_cur()` below is the single source they are generated from, so
# they cannot drift apart. tb/pp_top W3c grades two of the three.
def _scfg_cur(base):
    """The 10 words that leave r1 = the CURRENT configuration and r2 = 0.

    `base` is the address of the FIRST of the ten, so the two internal branch
    targets stay correct wherever the block is placed. Runs with the status
    register clean; see the SET_STATUS(ST_OK) prologue on the two arms that
    are entered from a checking op.
    """
    return [
        u('MOVE', rd=2, ra=0, imm=0),                 # ENTITY[0] key AND @24
        u('MOVE', rd=1, ra=0, imm=0),                 # a miss reads 0
        u('READ_ST', rd=3, imm=RGN_DYNV + SEL_CFG),   # has a controller set it?
        u('COMPARE', ra=3, fmt=FMT_D, imm=0),
        u('BR_STATUS', cnd=2, imm=base + 7),          # z = unset -> the image
        u('READ_ST', rd=1, imm=RGN_DYN + SEL_CFG),    # the controller's value
        u('BRANCH', imm=base + 10),
        u('DESC_ADDR', ra=2, imm=RGN_LOCATE),         # base + 7: the image arm
        u('BR_STATUS', cnd=0, imm=base + 10),         # miss -> r1 stays 0
        u('READ_ST', rd=1, imm=RGN_DATA + ENT_CURCFG_LANE),
    ]

#! Dispatch lands here, so this is the one arm the engine did not already give a
#! status to, hence no SET_STATUS(ST_OK) prologue.
place(E_SCFGRUN, _scfg_cur(E_SCFGRUN) + [
    u('SET_STATUS', imm=ST_STRMRUN),
    u('BRANCH', imm=E_SCFGEMT),
])

#! CHECK_LOCK has ALREADY written ENTITY_LOCKED by the time it branches here, and
#! CHECK_ARG likewise writes BAD_ARGUMENTS. Both arms clear it before the overlay
#! and re-apply it after, because the overlay's miss guard reads the status.
place(E_SCFGLK, [u('SET_STATUS', imm=ST_OK)] + _scfg_cur(E_SCFGLK + 1) + [
    u('SET_STATUS', imm=ST_LOCKED),
    u('BRANCH', imm=E_SCFGEMT),
])

place(E_SCFGBAD, [u('SET_STATUS', imm=ST_OK)] + _scfg_cur(E_SCFGBAD + 1) + [
    u('SET_STATUS', imm=ST_BADARG),
    u('BRANCH', imm=E_SCFGEMT),
])

place(E_SCFGEMT, [
    u('BUILD_HDR', ra=15, rb=13),
    u('BUILD_FLD', ra=2, fmt=FMT_W),             # reserved            @24
    u('BUILD_FLD', ra=1, fmt=FMT_W),             # configuration_index @26
    u('SEND_RESP'),
    u('END'),
])


# --- deterministic non-degenerate fill ---------------------------------------
for i in range(ROM_DEPTH):
    # A placed NOP encodes as zero, so contents cannot prove availability.
    if i not in occupied and i not in (0,):
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
