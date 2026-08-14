// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_aecp_ucpu suite — independent expectations, never DUT logic.
//
// Functional + conformance checks over 18 µprograms: the 06 §8 exemplars
// (GET/SET_SAMPLING_RATE, ACQUIRE_ENTITY, GET_DYNAMIC_INFO shape), the
// §9.3.2.6 FAIL_SAFE forced-respond arm, the §7.4.76.1 skip-on-overflow rule
// at the Milan 524-byte cap, Table 7-141 status codes, and every µISA
// operation class. The harness models the state port (2-cycle latency, name
// region, locate + forced miss), the gather port, a reluctant TX, and the
// lock context; it captures response-buffer BYTES, state-port writes and
// effect strobes, and checks exact values.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "VKL_aecp_ucpu.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// entry points — mirror gen_ucode.py
enum { E_FAILSAFE = 8, E_GETSR = 16, E_ALU = 64, E_ITER = 128,
       E_CHKARG = 192, E_LOCK = 224, E_GATHER = 256, E_SETSR = 288,
       E_NAME = 320, E_COPY = 352, E_MAPV = 384, E_MAPVF = 400,
       E_OVF = 416, E_FMT = 512, E_NOTIMPL = 560, E_ACQ = 576,
       E_STPRE = 592, E_MVUINFO = 736 };

// IEEE 1722.1-2021 Table 7-141
enum { ST_OK = 0, ST_NIMPL = 1, ST_NOSUCH = 2, ST_LOCKED = 3,
       ST_BADARG = 7, ST_NSUPP = 11 };

static const uint64_t CTLR = 0xC0FFEE00DEADBEEFull;
static const uint64_t OPD1 = 0x0000000000001234ull;
static const uint64_t NAMEQ = 0x4E414D455F303031ull;   // "NAME_001"

struct StWrite { bool name; uint32_t addr; uint64_t data; uint8_t strb; };

struct Harness {
  VKL_aecp_ucpu* dut;
  uint8_t  buf[640];
  bool     bad_write = false;
  int      sends = 0;
  uint32_t last_len = 0, last_status = 0;
  std::vector<StWrite> stw;
  int      commits = 0;
  std::vector<uint8_t> nvm_marks;
  std::vector<uint8_t> notify_classes;
  uint64_t name_store = NAMEQ;
  int      st_lat = 0;
  bool     st_err_next = false, st_name_next = false;
  uint64_t st_data_next = 0;
  int      gx_lat = 0;
  uint64_t gx_data_next = 0;
  int      tx_wait = 0;
  bool     lock_scenario = false;
  // response-buffer backpressure model — NON-ZERO by default
  int      rb_stall = 2;
  int      rb_hold  = 2;
  int      rb_accepts = 0;      // writes the buffer actually took
  int      rb_held_cycles = 0;  // cycles a presented write was refused
  int      rb_mutations = 0;    // a HELD write whose payload changed: a defect
  bool     rb_have_held = false;
  uint32_t rb_h_addr = 0, rb_h_data = 0;
  uint8_t  rb_h_strb = 0;

  explicit Harness(VKL_aecp_ucpu* d) : dut(d) { memset(buf, 0, sizeof buf); }

  static uint64_t gxval(uint8_t sel) {
    if (sel == 0x25) return 0x1111222233334444ull;
    if (sel == 0x30) return 1;                     // MAP_VALIDATE pass
    if (sel == 0x40) return 0;                     // MAP_VALIDATE fail
    if ((sel & 0xF0) == 0x10) return 0xC0ull + (sel & 0x0F);
    return 0x5A5A5A5A00000000ull | sel;
  }

  uint64_t st_read(uint32_t a, bool name, bool* err) {
    *err = false;
    if (name) return name_store;                   // name region, any offset
    if (a == (0x100u ^ 0x0003u)) return 0x500;     // locate hit -> base
    if (a == (0x100u ^ 0x0BADu)) { *err = true; return 0; }
    if (a == 0x508u) return 0xBB80;                // current_rate
    if (a == 0x520u) return 0x1111111122222222ull; // copy lane 0
    if (a == 0x528u) return 0x3333333344444444ull; // copy lane 1
    return 0xEEEE;
  }

  void tick() {
    dut->st_rvalid_i = 0; dut->st_err_i = 0;
    dut->gx_valid_i  = 0;
    dut->st_ready_i  = 1;
    dut->lock_held_i = lock_scenario;
    dut->lock_ctlr_i = 0x1122334455667788ull;

    if (dut->st_req_o && !dut->st_we_o) {
      if (st_lat == 0) {
        bool err = false;
        st_data_next = st_read(dut->st_addr_o, dut->st_name_o, &err);
        st_err_next = err;
        st_lat = 2;
      } else if (--st_lat == 0) {
        dut->st_rvalid_i = 1;
        dut->st_rdata_i  = st_data_next;
        dut->st_err_i    = st_err_next;
      }
    } else st_lat = 0;

    if (dut->gx_req_o) {
      if (gx_lat == 0) { gx_data_next = gxval(dut->gx_sel_o); gx_lat = 2; }
      else if (--gx_lat == 0) { dut->gx_valid_i = 1; dut->gx_data_i = gx_data_next; }
    } else gx_lat = 0;

    dut->tx_ready_i = (tx_wait == 0);
    if (tx_wait > 0) --tx_wait;

    // settle combinational logic with this cycle's inputs
    dut->clk_i = 0; dut->eval();

    // ---- response-buffer backpressure (KL_aecp_resp_buf is not single
    // cycle: a lane flush is a main-memory round trip). Refuse each write
    // for `rb_stall` cycles, then take it.
    dut->rb_ready_i = 1;
    if (dut->rb_we_o && rb_hold > 0) { dut->rb_ready_i = 0; --rb_hold; }
    dut->eval();

    // observe combinational outputs PRE-EDGE (what the registers see)
    if (dut->rb_we_o && !dut->rb_ready_i) {
      // a REFUSED write must be re-presented byte-for-byte identically —
      // that is the whole contract a memory-backed buffer relies on
      ++rb_held_cycles;
      if (rb_have_held && (rb_h_addr != (uint32_t)dut->rb_addr_o ||
                           rb_h_data != (uint32_t)dut->rb_wdata_o ||
                           rb_h_strb != (uint8_t)dut->rb_wstrb_o))
        ++rb_mutations;
      rb_have_held = true;
      rb_h_addr = dut->rb_addr_o; rb_h_data = dut->rb_wdata_o;
      rb_h_strb = dut->rb_wstrb_o;
    }
    if (dut->rb_we_o && dut->rb_ready_i) {
      uint32_t a = dut->rb_addr_o, d = dut->rb_wdata_o;
      uint8_t  s = dut->rb_wstrb_o;
      if (d == 0xBAD) bad_write = true;
      for (int i = 0; i < 4; ++i)
        if ((s >> i) & 1 && a + i < sizeof buf)
          buf[a + i] = uint8_t(d >> (8 * i));
      ++rb_accepts;
      rb_hold = rb_stall;
      rb_have_held = false;
    }
    if (dut->st_req_o && dut->st_we_o && dut->st_ready_i)
      stw.push_back({(bool)dut->st_name_o, (uint32_t)dut->st_addr_o,
                     (uint64_t)dut->st_wdata_o, (uint8_t)dut->st_wstrb_o});
    if (dut->eff_commit_o) ++commits;
    if (dut->eff_nvm_stb_o) nvm_marks.push_back(dut->eff_nvm_mark_o);
    if (dut->eff_notify_stb_o) notify_classes.push_back(dut->eff_notify_class_o);
    if (dut->resp_send_o && dut->tx_ready_i) {
      ++sends; last_len = dut->resp_len_o; last_status = dut->resp_status_o;
    }

    // rising edge: registers update
    dut->clk_i = 1; dut->eval();
  }

  uint32_t w32(uint32_t a) const {
    return uint32_t(buf[a]) | uint32_t(buf[a+1]) << 8 |
           uint32_t(buf[a+2]) << 16 | uint32_t(buf[a+3]) << 24;
  }

  bool run(uint16_t upc, uint64_t opd0, bool lock, int max_cycles = 2000) {
    memset(buf, 0, sizeof buf);
    bad_write = false; sends = 0; lock_scenario = lock;
    rb_accepts = 0; rb_held_cycles = 0; rb_have_held = false;
    rb_hold = rb_stall;
    stw.clear(); commits = 0; nvm_marks.clear(); notify_classes.clear();
    tx_wait = 3;
    dut->disp_upc_i = upc;
    dut->disp_ctlr_eid_i = CTLR;
    dut->disp_opd0_i = opd0;
    dut->disp_opd1_i = OPD1;
    dut->disp_valid_i = 1;
    tick();
    dut->disp_valid_i = 0;
    for (int i = 0; i < max_cycles; ++i) {
      tick();
      if (dut->done_o) return true;
    }
    return false;
  }
};

static uint32_t hdr2(uint32_t status) { return 0x12340000u | ((status & 0x1F) << 8); }
static const uint64_t IDX_OK  = 0x0000000700250003ull;
static const uint64_t IDX_BAD = 0x0000000700250BADull;

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_aecp_ucpu;
  Harness h(dut);

  dut->rst_n = 0; dut->disp_valid_i = 0;
  for (int i = 0; i < 4; ++i) h.tick();
  dut->rst_n = 1;
  h.tick();
  CHECK(dut->disp_ready_o == 1, "idle after reset");

  // ---- P1: GET_SAMPLING_RATE exemplar, success (06 §8) ----------------
  CHECK(h.run(E_GETSR, IDX_OK, false), "P1 completes");
  CHECK(h.w32(0) == 0xC0FFEE00u, "P1 hdr0 eid-hi got %08x", h.w32(0));
  CHECK(h.w32(4) == 0xDEADBEEFu, "P1 hdr1 eid-lo got %08x", h.w32(4));
  CHECK(h.w32(8) == hdr2(ST_OK), "P1 hdr2 got %08x", h.w32(8));
  CHECK(h.w32(12) == 0x0000BB80u, "P1 rate field got %08x", h.w32(12));
  CHECK(h.last_len == 16, "P1 len 16 got %u", h.last_len);
  CHECK(h.last_status == ST_OK, "P1 status got %u", h.last_status);
  CHECK(h.sends == 1, "P1 one send got %d", h.sends);
  CHECK(h.commits == 0 && h.nvm_marks.empty() && h.notify_classes.empty(),
        "P1 GET has no effects");

  // ---- P1b: locate miss -> NO_SUCH_DESCRIPTOR (Table 7-141 code 2) ----
  CHECK(h.run(E_GETSR, IDX_BAD, false), "P1b completes");
  CHECK(h.last_status == ST_NOSUCH, "P1b status got %u", h.last_status);
  CHECK(h.w32(8) == hdr2(ST_NOSUCH), "P1b hdr2 got %08x", h.w32(8));
  CHECK(h.last_len == 12, "P1b header-only got %u", h.last_len);
  CHECK(h.w32(12) == 0, "P1b no field written");

  // ---- P2: ALU / RAW / branch-flush / merge / qword field -------------
  CHECK(h.run(E_ALU, 0, false), "P2 completes");
  CHECK(!h.bad_write, "P2 poison ops flushed");
  CHECK(h.w32(12) == 5 && h.w32(16) == 7 && h.w32(20) == 1 && h.w32(24) == 5,
        "P2 fields %x %x %x %x", h.w32(12), h.w32(16), h.w32(20), h.w32(24));
  CHECK(h.w32(28) == 0xC0FFEE00u && h.w32(32) == 0xDEADBEEFu,
        "P2 qword %08x %08x", h.w32(28), h.w32(32));
  CHECK(h.last_len == 36, "P2 len got %u", h.last_len);

  // ---- P3: ITER/APPEND loop (GDI iteration shape) ---------------------
  CHECK(h.run(E_ITER, 0, false), "P3 completes");
  CHECK(h.w32(12) == 0xAB && h.w32(16) == 0xAB && h.w32(20) == 0xAB,
        "P3 appends %x %x %x", h.w32(12), h.w32(16), h.w32(20));
  CHECK(h.w32(24) == 0, "P3 exactly three");
  CHECK(h.last_len == 24, "P3 len got %u", h.last_len);

  // ---- P4: CHECK_ARG -> BAD_ARGUMENTS = 7 (Table 7-141) ---------------
  CHECK(h.run(E_CHKARG, 0, false), "P4 completes");
  CHECK(h.last_status == ST_BADARG, "P4 BAD_ARGUMENTS=7 got %u", h.last_status);
  CHECK(h.w32(8) == hdr2(ST_BADARG), "P4 hdr2 got %08x", h.w32(8));

  // ---- P5: CHECK_LOCK -> ENTITY_LOCKED = 3 ----------------------------
  CHECK(h.run(E_LOCK, 0, true), "P5 completes");
  CHECK(h.last_status == ST_LOCKED, "P5 LOCKED got %u", h.last_status);
  CHECK(h.run(E_LOCK, 0, false), "P5b completes");
  CHECK(h.last_status == ST_OK, "P5b unlocked got %u", h.last_status);

  // ---- P6: GATHER_EXT + READ_COUNTERS burst ---------------------------
  CHECK(h.run(E_GATHER, 0, false), "P6 completes");
  CHECK(h.w32(12) == 0x11112222u && h.w32(16) == 0x33334444u,
        "P6 qword %08x %08x", h.w32(12), h.w32(16));
  CHECK(h.w32(20) == 0xC0 && h.w32(24) == 0xC1 && h.w32(28) == 0xC2 &&
        h.w32(32) == 0xC3, "P6 counters %x %x %x %x",
        h.w32(20), h.w32(24), h.w32(28), h.w32(32));
  CHECK(h.last_len == 36, "P6 len got %u", h.last_len);

  // ---- P7: SET_SAMPLING_RATE exemplar — write-back + effects ----------
  CHECK(h.run(E_SETSR, IDX_OK, false), "P7 completes");
  CHECK(h.last_status == ST_OK, "P7 status got %u", h.last_status);
  CHECK(h.stw.size() == 1, "P7 one state write got %zu", h.stw.size());
  if (h.stw.size() == 1) {
    CHECK(!h.stw[0].name && h.stw[0].addr == 0x508,
          "P7 write addr 0x508 got 0x%x", h.stw[0].addr);
    CHECK((h.stw[0].data & 0xFFFFFFFFu) == 0xBB80, "P7 write data");
    CHECK(h.stw[0].strb == 0x0F, "P7 dword strobe got %02x", h.stw[0].strb);
  }
  CHECK(h.commits == 1, "P7 COMMIT once got %d", h.commits);
  CHECK(h.nvm_marks.size() == 1 && h.nvm_marks[0] == 0x21,
        "P7 NVM_MARK 0x21");
  CHECK(h.notify_classes.size() == 1 && h.notify_classes[0] == 5,
        "P7 NOTIFY_ENQ class 5");
  CHECK(h.w32(12) == 0xBB80, "P7 echoes the rate");
  // P7b: locked by another controller -> ENTITY_LOCKED, no write, no effects
  CHECK(h.run(E_SETSR, IDX_OK, true), "P7b completes");
  CHECK(h.last_status == ST_LOCKED, "P7b LOCKED got %u", h.last_status);
  CHECK(h.stw.empty(), "P7b no state write under lock");
  CHECK(h.commits == 0 && h.nvm_marks.empty() && h.notify_classes.empty(),
        "P7b no effects under lock");

  // ---- P8: NAME region read/write ------------------------------------
  CHECK(h.run(E_NAME, IDX_OK, false), "P8 completes");
  CHECK(h.stw.size() == 1 && h.stw[0].name, "P8 name-region write flagged");
  if (h.stw.size() == 1) {
    CHECK(h.stw[0].addr == 0x518, "P8 name addr got 0x%x", h.stw[0].addr);
    CHECK(h.stw[0].data == NAMEQ, "P8 write echoes the read");
    CHECK(h.stw[0].strb == 0xFF, "P8 qword strobe");
  }
  CHECK(h.w32(12) == uint32_t(NAMEQ >> 32) && h.w32(16) == uint32_t(NAMEQ),
        "P8 response carries the name got %08x %08x", h.w32(12), h.w32(16));

  // ---- P9: COPY_BUFFER — descriptor bytes into the response -----------
  CHECK(h.run(E_COPY, IDX_OK, false), "P9 completes");
  CHECK(h.w32(12) == 0x11111111u && h.w32(16) == 0x22222222u &&
        h.w32(20) == 0x33333333u && h.w32(24) == 0x44444444u,
        "P9 lanes %08x %08x %08x %08x",
        h.w32(12), h.w32(16), h.w32(20), h.w32(24));
  CHECK(h.last_len == 28, "P9 len got %u", h.last_len);

  // ---- P10: MAP_VALIDATE pass and fail --------------------------------
  CHECK(h.run(E_MAPV, 0, false), "P10 completes");
  CHECK(h.last_status == ST_OK, "P10 pass got %u", h.last_status);
  CHECK(h.run(E_MAPVF, 0, false), "P10b completes");
  CHECK(h.last_status == ST_BADARG, "P10b fail -> 7 got %u", h.last_status);

  // ---- P11: the 524-byte cap — skip-on-overflow (IEEE §7.4.76.1) ------
  CHECK(h.run(E_OVF, 0, false, 1500), "P11 completes");
  CHECK(h.last_len == 524, "P11 capped at 524 got %u", h.last_len);
  CHECK(dut->dbg_ovf_o == 1, "P11 overflow flag set");
  CHECK(h.last_status == ST_NSUPP, "P11 ovf branch taken got %u", h.last_status);
  CHECK(h.w32(12) == 0 && h.w32(16) == 0xCAFE &&
        h.w32(516) == 0 && h.w32(520) == 0xCAFE,
        "P11 first/last fitting elements (qword = hi word first)");
  CHECK(h.w32(524) == 0 && h.w32(528) == 0, "P11 nothing past the cap");

  // ---- P12: write strobes, truncating moves, 64-bit compare, rb-RAW ---
  CHECK(h.run(E_FMT, IDX_OK, false), "P12 completes");
  CHECK(h.stw.size() == 3, "P12 three writes got %zu", h.stw.size());
  if (h.stw.size() == 3) {
    CHECK(h.stw[0].strb == 0x01 && h.stw[1].strb == 0x03 &&
          h.stw[2].strb == 0xFF, "P12 strobes %02x %02x %02x",
          h.stw[0].strb, h.stw[1].strb, h.stw[2].strb);
    CHECK(h.stw[0].addr == 0x540, "P12 write addr got 0x%x", h.stw[0].addr);
  }
  CHECK(!h.bad_write, "P12 poison flushed");
  CHECK(h.w32(12) == 0x50, "P12 merged got %x", h.w32(12));
  CHECK(h.buf[16] == 0x56 && h.buf[17] == 0x34, "P12 word field bytes");
  CHECK(h.buf[18] == 0x56, "P12 unaligned byte field");
  CHECK(h.last_len == 19, "P12 len got %u", h.last_len);

  // ---- P13: unknown-opcode path -> NOT_IMPLEMENTED (§9.3.5.3.3) -------
  CHECK(h.run(E_NOTIMPL, 0, false), "P13 completes");
  CHECK(h.last_status == ST_NIMPL, "P13 NOT_IMPLEMENTED=1 got %u", h.last_status);
  CHECK(h.last_len == 12, "P13 echo-size got %u", h.last_len);

  // ---- P14: ACQUIRE_ENTITY exemplar (Milan Δ7) ------------------------
  CHECK(h.run(E_ACQ, 0, false), "P14 completes");
  CHECK(h.last_status == ST_NSUPP, "P14 NOT_SUPPORTED=11 got %u", h.last_status);
  CHECK(h.w32(12) == 0 && h.w32(16) == 0, "P14 owner_id = 0");
  CHECK(h.last_len == 20, "P14 len got %u", h.last_len);

  // ---- P15: FAIL_SAFE arm preserves the best current status ----------
  CHECK(h.run(E_STPRE, 0, false), "P15 completes");
  CHECK(h.last_status == ST_BADARG, "P15 status preserved got %u", h.last_status);
  CHECK(h.w32(8) == hdr2(ST_BADARG), "P15 hdr carries it");
  CHECK(h.last_len == 12, "P15 header-only");
  CHECK(h.sends == 1, "P15 exactly one send");
  // P15b: the arm itself, dispatched clean -> SUCCESS response
  CHECK(h.run(E_FAILSAFE, 0, false), "P15b completes");
  CHECK(h.last_status == ST_OK && h.last_len == 12, "P15b clean arm");

  // ---- P17: MVU GET_MILAN_INFO (Milan v1.2 §5.4.4.1, Figure 5.4) ------
  // The µprogram builds the whole 20-byte payload from constants: the tail of
  // the 48-bit protocol_id, r + command_type, the reserved word the sender
  // must zero, then the three 32-bit fields. Checked FIELD BY FIELD rather
  // than as a length, because a wrong protocol_version or an overclaimed
  // features_flags is a lie a controller believes.
  {
    auto w16 = [&](uint32_t a) {
      return uint32_t(h.buf[a]) | uint32_t(h.buf[a + 1]) << 8;
    };
    CHECK(h.run(E_MVUINFO, 0, false), "P17 completes");
    CHECK(h.last_status == ST_OK, "P17 SUCCESS got %u", h.last_status);
    //! 12 header bytes + 20 payload = AECPDU 44 B, control_data_length 32
    CHECK(h.last_len == 32, "P17 len got %u, want 32", h.last_len);
    CHECK(w16(12) == 0xC50A && w16(14) == 0xC100,
          "P17 protocol_id tail %04x%04x, want C50AC100", w16(12), w16(14));
    CHECK(w16(16) == 0x0000, "P17 r+command_type got %04x, want 0000",
          w16(16));
    CHECK(w16(18) == 0x0000, "P17 reserved got %04x, want 0000", w16(18));
    CHECK(h.w32(20) == 1u, "P17 protocol_version got %u, want 1 "
          "(Milan §4.2.4)", h.w32(20));
    CHECK(h.w32(24) == 0u, "P17 features_flags got %08x — Table 5.20's two "
          "flags are both unimplemented here", h.w32(24));
    CHECK(h.w32(28) == 0u, "P17 certification_version got %08x, want 0",
          h.w32(28));
  }

  // ---- P16: the response-buffer face is FLOW CONTROLLED ---------------
  // Every program above ran against a 2-cycle stall. These prove the µCPU is
  // INVARIANT to how hard the buffer pushes back: a buffer in main memory can
  // refuse for a whole memory round trip, and the bytes, the length, the
  // status and the number of writes must all be identical to a buffer that
  // never refuses at all.
  {
    struct Prog { const char* name; uint16_t upc; uint64_t opd0; };
    static const Prog progs[] = {
      {"GETSR", E_GETSR, IDX_OK}, {"ALU", E_ALU, 0}, {"ITER", E_ITER, 0},
      {"GATHER", E_GATHER, 0},    {"COPY", E_COPY, IDX_OK},
      {"FMT", E_FMT, 0},          {"OVF", E_OVF, 0},
      {"ACQ", E_ACQ, 0},          {"NOTIMPL", E_NOTIMPL, 0},
      {"MVUINFO", E_MVUINFO, 0},
    };
    for (const auto& p : progs) {
      h.rb_stall = 0;
      bool ok0 = h.run(p.upc, p.opd0, false);
      std::vector<uint8_t> img0(h.buf, h.buf + sizeof h.buf);
      uint32_t len0 = h.last_len, st0 = h.last_status;
      int snd0 = h.sends, acc0 = h.rb_accepts, held0 = h.rb_held_cycles;

      h.rb_stall = 9;
      bool ok9 = h.run(p.upc, p.opd0, false);
      std::vector<uint8_t> img9(h.buf, h.buf + sizeof h.buf);

      CHECK(ok0 && ok9, "P16 %s retires at both stalls", p.name);
      CHECK(held0 == 0, "P16 %s: zero-stall run held %d cycles", p.name, held0);
      CHECK(h.rb_held_cycles == 9 * acc0,
            "P16 %s: %d held cycles for %d writes, want %d", p.name,
            h.rb_held_cycles, acc0, 9 * acc0);
      CHECK(img0 == img9, "P16 %s: the response bytes changed under stall",
            p.name);
      CHECK(h.last_len == len0 && h.last_status == st0,
            "P16 %s: len/status %u/%u vs %u/%u", p.name, h.last_len,
            h.last_status, len0, st0);
      CHECK(h.sends == snd0, "P16 %s: %d sends vs %d", p.name, h.sends, snd0);
      CHECK(h.rb_accepts == acc0,
            "P16 %s: %d writes accepted under stall vs %d — a stalled write "
            "was duplicated or lost", p.name, h.rb_accepts, acc0);
      CHECK(h.rb_mutations == 0,
            "P16 %s: a REFUSED write changed while it was held", p.name);
    }
    h.rb_stall = 2;
  }

  h.tick();
  CHECK(dut->disp_ready_o == 1, "ready again after all programs");

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
