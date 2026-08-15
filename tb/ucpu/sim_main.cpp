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
       E_STPRE = 592, E_MVUINFO = 736, E_GCTRS = 768, E_GAMAP = 800,
       E_REGUN = 832, E_DEREG = 844, E_UNSOK = 852, E_NOSEND = 858,
       E_NSUPPE = 864, E_LOCKEN = 872, E_LOCKUNS = 896, E_GSTRI = 912,
       E_GAVB = 944, E_GASP = 976 };

// IEEE 1722.1-2021 Table 7-141
enum { ST_OK = 0, ST_NIMPL = 1, ST_NOSUCH = 2, ST_LOCKED = 3,
       ST_BADARG = 7, ST_NORES = 8, ST_NSUPP = 11 };

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

  // the program being run + its dispatch operands: the gather model below is
  // PER-PROGRAM, because the engine routes the gather bus by command - two
  // faces legitimately claim the same selector values (gen_ucode.py's
  // audio-map note), so one flat sel->value table cannot model both.
  uint16_t cur_upc = 0;
  uint64_t cur_opd0 = 0, cur_opd1 = 0;
  int      amap_recs = 0;                 // RECORD gathers completed this run
  int      gsi_recs = 0;                  // Milan-info record gathers (sel 0xB8)
  int      gasp_count = 3;                // GET_AS_PATH path length served
  std::vector<uint8_t> gx_sels;           // every completed gather's selector

  explicit Harness(VKL_aecp_ucpu* d) : dut(d) { memset(buf, 0, sizeof buf); }

  // ---- the GET_AUDIO_MAP integrator model (06 §6.5 face semantics) ----
  // Fixed partition of 3 pages; page 0 deliberately EMPTY (the 0-trip arm),
  // page 1 carries 3 records, page 2 carries 1. An unknown port (the 0x0BAD
  // index) answers 0 everywhere; an out-of-range page answers 0 mappings
  // under the real number_of_maps - the wrong-object guard the RTL face must
  // also implement.
  static const int AM_NMAPS_C = 3;
  int am_index()  const { return int((cur_opd0 >> 32) & 0xFFFF); }
  int am_page()   const { return int(cur_opd1 & 0xFFFF); }
  int am_nmaps()  const { return am_index() == 0x0BAD ? 0 : AM_NMAPS_C; }
  int am_count()  const {
    if (am_nmaps() == 0 || am_page() >= am_nmaps()) return 0;
    return am_page() == 1 ? 3 : (am_page() == 2 ? 1 : 0);
  }
  // record k of the served page: four distinct 16-bit fields so a permuted
  // or repeated record cannot pass
  static uint64_t am_rec(int k) {
    return (uint64_t(0x1100 | k) << 48) | (uint64_t(0x2200 | k) << 32) |
           (uint64_t(0x3300 | k) << 16) |  uint64_t(0x4400 | k);
  }

  // the registry/lock op face result (06 SS6.4/SS6.7): sel 0xA0 performs
  // the in-flight command's op; the value below is what KL_aecp_notify
  // would answer (0 = done, 1 = table full / denied)
  uint64_t rgy_result = 0;
  uint64_t rgy_holder = 0;   // sel 0xA1: the lock holder's eid (0 = free)

  uint64_t gxval(uint8_t sel) {
    if (cur_upc == E_REGUN || cur_upc == E_DEREG ||
        cur_upc == E_LOCKEN || cur_upc == E_LOCKUNS) {
      if (sel == 0xA0) return rgy_result;
      if (sel == 0xA1) return rgy_holder;
      return 0;
    }
    if (cur_upc == E_GAMAP) {
      if (sel == 0x00) return uint64_t(am_nmaps());
      if (sel == 0x01) return (uint64_t(am_nmaps()) << 16)
                            |  uint64_t(am_count());
      if (sel == 0x10) return (amap_recs < am_count()) ? am_rec(amap_recs)
                                                       : 0;
      return 0;
    }
    if (cur_upc == E_GAVB) {
      if (sel == 0xB0) return 0x1112131415161718ull;          // gm
      if (sel == 0xB1) return (0x0000'0BADull << 32) | (0x00ull << 24)
                            | (0x07ull << 16) | 2;            // count 2
      if (sel == 0xB8) return 0x0603'0002ull + uint64_t(gsi_recs) * 0x01000000ull;
      return 0;
    }
    if (cur_upc == E_GASP) {
      if (sel == 0xB0) return uint64_t(gasp_count);
      if (sel == 0xB8) return 0xAB00'0000'0000'0000ull | unsigned(gsi_recs);
      return 0;
    }
    if (cur_upc == E_GSTRI) {
      // the Milan-info face: distinct per-selector words (06 SS6.2)
      if ((sel & 0xF0) == 0xB0)
        return 0xB000'0000'0000'0000ull | (uint64_t(sel & 0x0F) << 32) |
               (0x00C0'0000ull | (sel & 0x0F));
      return 0;
    }
    if (sel == 0x25) return 0x1111222233334444ull;
    if (sel == 0x30) return 1;                     // MAP_VALIDATE pass
    if (sel == 0x40) return 0;                     // MAP_VALIDATE fail
    if ((sel & 0xF0) == 0x10) return 0xC0ull + (sel & 0x0F);
    return 0x5A5A5A5A00000000ull | sel;
  }

  uint64_t st_read(uint32_t a, bool name, uint64_t wdata, bool* err) {
    *err = false;
    if (name) return name_store;                   // name region, any offset
    // the RGN_LOCATE region (0xF0000): the key rides st_wdata as
    // {index[47:32], type[31:16], cfg[15:0]} - E_GAMAP's shape. Its cfg half
    // is 0, so the XORed address is the bare region; hit/miss must come from
    // the key itself, exactly as the real store decides.
    if ((a & 0xF0000u) == 0xF0000u)
      { *err = (((wdata >> 32) & 0xFFFFu) == 0x0BADu); return 0x500; }
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
        st_data_next = st_read(dut->st_addr_o, dut->st_name_o,
                               dut->st_wdata_o, &err);
        st_err_next = err;
        st_lat = 2;
      } else if (--st_lat == 0) {
        dut->st_rvalid_i = 1;
        dut->st_rdata_i  = st_data_next;
        dut->st_err_i    = st_err_next;
      }
    } else st_lat = 0;

    if (dut->gx_req_o) {
      if (gx_lat == 0) {
        gx_data_next = gxval(dut->gx_sel_o);
        gx_sels.push_back(dut->gx_sel_o);
        if (cur_upc == E_GAMAP && dut->gx_sel_o == 0x10) ++amap_recs;
        if (dut->gx_sel_o == 0xB8) ++gsi_recs;
        gx_lat = 2;
      }
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

  bool run(uint16_t upc, uint64_t opd0, bool lock, int max_cycles = 2000,
           uint64_t opd1 = OPD1) {
    memset(buf, 0, sizeof buf);
    bad_write = false; sends = 0; lock_scenario = lock;
    rb_accepts = 0; rb_held_cycles = 0; rb_have_held = false;
    rb_hold = rb_stall;
    stw.clear(); commits = 0; nvm_marks.clear(); notify_classes.clear();
    cur_upc = upc; cur_opd0 = opd0; cur_opd1 = opd1;
    amap_recs = 0; gsi_recs = 0; gx_sels.clear();
    tx_wait = 3;
    dut->disp_upc_i = upc;
    dut->disp_ctlr_eid_i = CTLR;
    dut->disp_opd0_i = opd0;
    dut->disp_opd1_i = opd1;
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
    struct Prog { const char* name; uint16_t upc; uint64_t opd0;
                  uint64_t opd1; };
    static const Prog progs[] = {
      {"GETSR", E_GETSR, IDX_OK, OPD1}, {"ALU", E_ALU, 0, OPD1},
      {"ITER", E_ITER, 0, OPD1},
      {"GATHER", E_GATHER, 0, OPD1},    {"COPY", E_COPY, IDX_OK, OPD1},
      {"FMT", E_FMT, 0, OPD1},          {"OVF", E_OVF, 0, OPD1},
      {"ACQ", E_ACQ, 0, OPD1},          {"NOTIMPL", E_NOTIMPL, 0, OPD1},
      {"MVUINFO", E_MVUINFO, 0, OPD1},
      // the audio-map success shape: index 2, page 1 -> 3 appended records,
      // so the stall sweep covers its GATHER + APPEND loop too
      {"GAMAP", E_GAMAP, 0x00000002000E0000ull, 0x0000000000020001ull},
    };
    for (const auto& p : progs) {
      h.rb_stall = 0;
      bool ok0 = h.run(p.upc, p.opd0, false, 2000, p.opd1);
      std::vector<uint8_t> img0(h.buf, h.buf + sizeof h.buf);
      uint32_t len0 = h.last_len, st0 = h.last_status;
      int snd0 = h.sends, acc0 = h.rb_accepts, held0 = h.rb_held_cycles;

      h.rb_stall = 9;
      bool ok9 = h.run(p.upc, p.opd0, false, 2000, p.opd1);
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

  // ---- P18: GET_COUNTERS lays out IEEE §7.4.42.2's block, all 32 quadlets
  // The response is fixed-size on every status: descriptor_type, then
  // descriptor_index, then counters_valid, then THIRTY-TWO quadlets. A short
  // block is a deserialize error at the controller (Hive 4.3.1 reports exactly
  // that shape of defect as "Incorrect payload size"), so the length is a check
  // in its own right, and so is every quadlet's selector: quadlet n must come
  // from gather selector {n>>2, n&3} and from nowhere else, because that
  // mapping is the whole reason the block needs no decoder.
  //
  // Byte order here is the TB buffer's own little-endian convenience, not the
  // wire's — KL_aecp_resp_buf owns the 1722.1 big-endian placement and tb/pp_top
  // grades it against real AECPDU bytes.
  {
    const uint16_t DESC_STREAM_INPUT = 0x0005;
    //! the locate-first contract (this round): r14 is the store key
    //! {index, type, cfg 0} and r13 = {type, index} for the one FMT_D emit
    const uint64_t CT_KEY  = 0x0000000700050000ull;   // STREAM_INPUT[7], hit
    const uint64_t CT_TYIX = 0x0000000000050007ull;
    CHECK(h.run(E_GCTRS, CT_KEY, false, 2000, CT_TYIX), "P18 completes");
    CHECK(h.last_status == ST_OK, "P18 status got %u", h.last_status);
    // 12 header + type 2 + index 2 + counters_valid 4 + 32 x 4 = 148, which is
    // the control_data_length of a 160-byte GET_COUNTERS response (F06.14)
    CHECK(h.last_len == 148, "P18 len got %u, want 148", h.last_len);
    CHECK(h.sends == 1, "P18 one send got %d", h.sends);
    //! the old form of this check was an operator-precedence TAUTOLOGY
    //! ((a == b) | DESC_STREAM_INPUT: always nonzero) - found when the
    //! locate-first landing changed the layout and it stayed green.
    //! Parenthesized now, and graded against the FMT_D {type, index} emit.
    CHECK(h.w32(12) == (uint32_t)(CT_TYIX & 0xFFFFFFFFu),
          "P18 {type, index} got %08x", h.w32(12));
    CHECK(h.w32(16) == (uint32_t)h.gxval(0x80),
          "P18 counters_valid comes from selector 0x80, got %08x", h.w32(16));
    int bad_q = -1;
    for (int n = 0; n < 32; ++n) {
      uint8_t sel = uint8_t(((n >> 2) << 4) | (n & 3));
      if (h.w32(20 + 4 * n) != (uint32_t)h.gxval(sel)) { bad_q = n; break; }
    }
    CHECK(bad_q < 0, "P18 quadlet %d is not what selector 0x%02x answered",
          bad_q, bad_q < 0 ? 0 : (((bad_q >> 2) << 4) | (bad_q & 3)));
    CHECK(h.w32(148) == 0, "P18 wrote past the 32-quadlet block");
    CHECK(h.commits == 0 && h.nvm_marks.empty() && h.notify_classes.empty(),
          "P18 a GET has no effects");
  }

  // ---- P18b: the locate miss answers NO_SUCH_DESCRIPTOR, zero body --------
  // (this round's first strictness rule: Table 7-141 "A descriptor with the
  //  descriptor_type and descriptor_index specified does not exist" - and
  //  the fixed Figure 7-67 body still emits, all zero, with the gather face
  //  NEVER consulted: the store, not the face, is the existence authority)
  {
    const uint64_t CT_MISS = 0x00000BAD00050000ull;   // the store-miss index
    const uint64_t CT_TYIX = 0x0000000000050BADull;
    CHECK(h.run(E_GCTRS, CT_MISS, false, 4000, CT_TYIX), "P18b completes");
    CHECK(h.last_status == ST_NOSUCH,
          "P18b status NO_SUCH_DESCRIPTOR got %u", h.last_status);
    CHECK(h.last_len == 148, "P18b the fixed body still emits, len %u",
          h.last_len);
    CHECK(h.w32(12) == 0x00050BADu, "P18b {type, index} echoed, got %08x",
          h.w32(12));
    CHECK(h.w32(16) == 0, "P18b counters_valid ZERO, got %08x", h.w32(16));
    int nz = 0;
    for (int n = 0; n < 32; ++n) if (h.w32(20 + 4 * n) != 0) ++nz;
    CHECK(nz == 0, "P18b %d nonzero quadlets in a NO_SUCH_DESCRIPTOR body", nz);
    CHECK(h.gx_sels.empty(),
          "P18b the face was asked %zu times about a nonexistent object",
          h.gx_sels.size());
  }

  // ---- P19: GET_AUDIO_MAP lays out IEEE §7.4.44.2's response --------------
  // Fixed part {descriptor_type, descriptor_index, map_index, number_of_maps,
  // number_of_mappings, reserved} then 8-byte records, so the payload is
  // 12 + 8·M and resp_len 24 + 8·M. The register contract is the engine's:
  // r14 = the locate key {index, STREAM_PORT_INPUT, cfg 0} and r13 =
  // {descriptor_index, map_index}; the harness's face model partitions into
  // 3 pages with page 0 EMPTY, page 1 = 3 records, page 2 = 1 (mirrors
  // gen_ucode.py's E_GAMAP note). Byte order is the TB buffer's little-endian
  // convenience - tb/pp_top grades the real wire bytes.
  {
    auto w16 = [&](uint32_t a) {
      return uint32_t(h.buf[a]) | uint32_t(h.buf[a + 1]) << 8;
    };
    const uint64_t AM_IDX2 = 0x00000002000E0000ull;   // index 2, hit
    const uint64_t AM_MISS = 0x00000BAD000E0000ull;   // the store-miss index
    auto am_opd1 = [](uint16_t index, uint16_t page) {
      return (uint64_t(index) << 16) | page;
    };
    auto count_sel = [&](uint8_t sel) {
      int n = 0;
      for (uint8_t s : h.gx_sels) if (s == sel) ++n;
      return n;
    };

    // P19a: page 1 of a 3-page port - SUCCESS with all three records
    CHECK(h.run(E_GAMAP, AM_IDX2, false, 2000, am_opd1(2, 1)),
          "P19a completes");
    CHECK(h.last_status == ST_OK, "P19a status got %u", h.last_status);
    CHECK(h.last_len == 24 + 8 * 3, "P19a len got %u, want 48", h.last_len);
    CHECK(h.sends == 1, "P19a one send got %d", h.sends);
    CHECK(w16(12) == 0x000E, "P19a descriptor_type got %04x, want 000E "
          "(Table 7-1 STREAM_PORT_INPUT)", w16(12));
    CHECK(h.w32(14) == 0x00020001u,
          "P19a {descriptor_index, map_index} got %08x", h.w32(14));
    CHECK(h.w32(18) == 0x00030003u,
          "P19a {number_of_maps, number_of_mappings} got %08x", h.w32(18));
    CHECK(w16(22) == 0, "P19a reserved got %04x", w16(22));
    int bad_r = -1;
    for (int k = 0; k < 3; ++k) {
      uint64_t r = Harness::am_rec(k);
      if (h.w32(24 + 8 * k) != uint32_t(r >> 32) ||
          h.w32(28 + 8 * k) != uint32_t(r)) { bad_r = k; break; }
    }
    CHECK(bad_r < 0, "P19a record %d is not the face's record %d", bad_r,
          bad_r);
    CHECK(h.w32(48) == 0, "P19a wrote past the last record");
    CHECK(count_sel(0x00) == 1 && count_sel(0x01) == 1,
          "P19a NMAPS/GEOM asked once each, got %d/%d",
          count_sel(0x00), count_sel(0x01));
    CHECK(count_sel(0x10) == 3,
          "P19a exactly 3 RECORD gathers, got %d", count_sel(0x10));
    CHECK(h.commits == 0 && h.nvm_marks.empty() && h.notify_classes.empty(),
          "P19a a GET has no effects");

    // P19b: page 0 is EMPTY - SUCCESS, number_of_mappings 0, ZERO records.
    // This is the 0-trip loop arm: an iterator tested after its body would
    // append a phantom record here.
    CHECK(h.run(E_GAMAP, AM_IDX2, false, 2000, am_opd1(2, 0)),
          "P19b completes");
    CHECK(h.last_status == ST_OK, "P19b status got %u", h.last_status);
    CHECK(h.last_len == 24, "P19b len got %u, want 24", h.last_len);
    CHECK(h.w32(18) == 0x00030000u,
          "P19b {number_of_maps, 0} got %08x", h.w32(18));
    CHECK(h.w32(24) == 0, "P19b a record was appended to an empty page");
    CHECK(count_sel(0x10) == 0,
          "P19b the face was asked for %d records of an empty page",
          count_sel(0x10));

    // P19c: map_index = number_of_maps - §7.4.44.1's BAD_ARGUMENT, and the
    // response still carries the full fixed part with the REAL
    // number_of_maps (Milan §5.4.2.26: always N) over an empty page
    CHECK(h.run(E_GAMAP, AM_IDX2, false, 2000, am_opd1(2, 3)),
          "P19c completes");
    CHECK(h.last_status == ST_BADARG, "P19c BAD_ARGUMENTS=7 got %u",
          h.last_status);
    CHECK(h.last_len == 24, "P19c len got %u, want 24", h.last_len);
    CHECK(h.w32(14) == 0x00020003u,
          "P19c {descriptor_index, map_index} echoed, got %08x", h.w32(14));
    CHECK(h.w32(18) == 0x00030000u,
          "P19c {number_of_maps real, mappings 0} got %08x", h.w32(18));
    CHECK(count_sel(0x10) == 0, "P19c records fetched on a refused page");

    // P19d: the store misses the locate - NO_SUCH_DESCRIPTOR from DESC_ADDR
    // itself (no SET_STATUS in that arm), fixed part all-zero geometry
    CHECK(h.run(E_GAMAP, AM_MISS, false, 2000, am_opd1(0x0BAD, 0)),
          "P19d completes");
    CHECK(h.last_status == ST_NOSUCH, "P19d NO_SUCH_DESCRIPTOR=2 got %u",
          h.last_status);
    CHECK(h.last_len == 24, "P19d len got %u, want 24", h.last_len);
    CHECK(h.w32(18) == 0, "P19d geometry not zero on a miss: %08x",
          h.w32(18));
    CHECK(count_sel(0x10) == 0, "P19d records fetched for a missing port");
  }

  h.tick();
  CHECK(dut->disp_ready_o == 1, "ready again after all programs");

  // ---- N: the registration pair + the unsolicited stubs ----------------
  // (Milan SS5.4.2.21/SS5.4.2.22; gen_ucode.py E_REGUN/E_DEREG/E_UNSOK/
  //  E_NOSEND). The op itself lives in KL_aecp_notify - here the face is
  //  modeled, so what is proved is the microprogram's status law and that
  //  exactly ONE op gather is issued per command (the once-per-edge rule).
  h.rgy_result = 0;
  CHECK(h.run(E_REGUN, 0, false), "N1 REGISTER ok-arm completes");
  CHECK(h.last_status == ST_OK, "N1 status SUCCESS got %u", h.last_status);
  CHECK(h.last_len == 12, "N1 header-only buffer (flags ride the echo), got %u",
        h.last_len);
  CHECK(h.sends == 1, "N1 one send");
  {
    int ops = 0;
    for (uint8_t v : h.gx_sels) if (v == 0xA0) ++ops;
    CHECK(ops == 1, "N1 exactly one registry op gather, got %d", ops);
  }

  h.rgy_result = 1;
  CHECK(h.run(E_REGUN, 0, false), "N2 REGISTER full-arm completes");
  CHECK(h.last_status == ST_NORES,
        "N2 status NO_RESOURCES (Milan SS5.4.2.21) got %u", h.last_status);
  CHECK(h.sends == 1, "N2 still answers");

  h.rgy_result = 0;
  CHECK(h.run(E_DEREG, 0, false), "N3 DEREGISTER completes");
  CHECK(h.last_status == ST_OK, "N3 status SUCCESS got %u", h.last_status);
  CHECK(h.sends == 1, "N3 one send");
  {
    int ops = 0;
    for (uint8_t v : h.gx_sels) if (v == 0xA0) ++ops;
    CHECK(ops == 1, "N3 exactly one registry op gather, got %d", ops);
  }

  CHECK(h.run(E_UNSOK, 0, false), "N4 unsolicited-ok completes");
  CHECK(h.last_status == ST_OK && h.last_len == 12 && h.sends == 1,
        "N4 SUCCESS, empty payload, one send (st %u len %u sends %d)",
        h.last_status, h.last_len, h.sends);
  CHECK(h.gx_sels.empty(), "N4 no gathers");

  CHECK(h.run(E_NOSEND, 0, false), "N5 no-send arm completes");
  CHECK(h.sends == 0, "N5 emits NO frame, got %d", h.sends);

  // ---- L: LOCK_ENTITY + the NOT_SUPPORTED echo + the notification --------
  // (Milan SS5.4.2.1/SS5.4.2.2, IEEE SS7.4.1/SS7.4.2; gen_ucode.py E_NSUPPE
  //  / E_LOCKEN / E_LOCKUNS. r14[31:0] carries the command's flags word.)
  h.rgy_result = 2;                          // took the lock (changed)
  h.rgy_holder = 0x1111222233334444ull;
  CHECK(h.run(E_LOCKEN, 0x0000000000000001ull, false), "L1 LOCK ok completes");
  CHECK(h.last_status == ST_OK, "L1 SUCCESS got %u", h.last_status);
  CHECK(h.w32(12) == 1, "L1 flags echoed at @24, got %08x", h.w32(12));
  CHECK(h.w32(16) == 0x11112222u && h.w32(20) == 0x33334444u,
        "L1 locked_id = the holder (%08x %08x)", h.w32(16), h.w32(20));
  CHECK(h.w32(24) == 0, "L1 ENTITY[0] echo");
  CHECK(h.last_len == 28, "L1 len 28 got %u", h.last_len);
  {
    int op = 0, st = 0;
    for (uint8_t v : h.gx_sels) { if (v == 0xA0) ++op; if (v == 0xA1) ++st; }
    CHECK(op == 1 && st == 1, "L1 one op + one state gather (%d, %d)", op, st);
  }

  h.rgy_result = 1;                          // held by another controller
  h.rgy_holder = 0x9999888877776666ull;
  CHECK(h.run(E_LOCKEN, 0, false), "L2 LOCK denied completes");
  CHECK(h.last_status == ST_LOCKED,
        "L2 ENTITY_LOCKED (Table 7-141 code 3) got %u", h.last_status);
  CHECK(h.w32(16) == 0x99998888u && h.w32(20) == 0x77776666u,
        "L2 the denial names the holder");

  h.rgy_holder = 0xAAAABBBBCCCCDDDDull;
  CHECK(h.run(E_LOCKUNS, 0, false), "L3 unsolicited LOCK completes");
  CHECK(h.last_status == ST_OK && h.last_len == 28,
        "L3 SUCCESS, full 16-byte body (st %u len %u)",
        h.last_status, h.last_len);
  CHECK(h.w32(12) == 0, "L3 flags 0");
  CHECK(h.w32(16) == 0xAAAABBBBu && h.w32(20) == 0xCCCCDDDDu,
        "L3 locked_id = the current holder");

  CHECK(h.run(E_NSUPPE, 0, false), "L4 NOT_SUPPORTED echo completes");
  CHECK(h.last_status == ST_NSUPP && h.last_len == 12 && h.sends == 1,
        "L4 NOT_SUPPORTED, echo-sized (st %u len %u sends %d)",
        h.last_status, h.last_len, h.sends);

  // ---- G: GET_STREAM_INFO (Milan SS5.4.2.10 Figure 5.1 layout) -----------
  // r14 = the locate key {index, type, cfg 0}; r13 = {type, index} for the
  // @24 dword. Every value word comes from the face model above; what is
  // proved here is the LAYOUT - each selector's word lands at its Figure 5.1
  // offset - and that a locate miss keeps NO_SUCH_DESCRIPTOR while still
  // emitting the full 56-byte body the face answered.
  {
    const uint64_t KEY_OK  = 0x0000000300050000ull;   // STREAM_INPUT[3]
    const uint64_t TYIX    = 0x0000000000050003ull;
    CHECK(h.run(E_GSTRI, KEY_OK, false, 4000, TYIX), "G1 completes");
    CHECK(h.last_status == ST_OK, "G1 SUCCESS got %u", h.last_status);
    CHECK(h.last_len == 68, "G1 len 12+56 got %u", h.last_len);
    CHECK(h.w32(12) == 0x00050003u, "G1 type+index @24, got %08x", h.w32(12));
    CHECK(h.w32(16) == 0x00C00000u, "G1 flags = face word 0 low, got %08x",
          h.w32(16));
    CHECK(h.w32(20) == 0xB0000001u && h.w32(24) == 0x00C00001u,
          "G1 stream_format = face word 1 (%08x %08x)", h.w32(20), h.w32(24));
    CHECK(h.w32(28) == 0xB0000002u && h.w32(32) == 0x00C00002u,
          "G1 stream_id = face word 2");
    CHECK(h.w32(36) == 0x00C00003u, "G1 latency = word 3 low");
    CHECK(h.w32(40) == 0xB0000004u && h.w32(44) == 0x00C00004u,
          "G1 dmac+failcode = word 4");
    CHECK(h.w32(48) == 0xB0000005u && h.w32(52) == 0x00C00005u,
          "G1 bridge_id = word 5");
    CHECK(h.w32(56) == 0xB0000006u && h.w32(60) == 0x00C00006u,
          "G1 vlan+flags_ex = word 6");
    CHECK(h.w32(64) == 0x00C00007u, "G1 pbsta dword = word 7 low");
    {
      int n = 0;
      for (uint8_t v : h.gx_sels) if ((v & 0xF0) == 0xB0) ++n;
      CHECK(n == 8, "G1 exactly eight face words asked, got %d", n);
    }

    const uint64_t KEY_BAD = 0x00000BAD00050000ull;
    CHECK(h.run(E_GSTRI, KEY_BAD, false, 4000, TYIX), "G2 completes");
    CHECK(h.last_status == ST_NOSUCH,
          "G2 locate miss keeps NO_SUCH_DESCRIPTOR, got %u", h.last_status);
    CHECK(h.last_len == 68, "G2 the full body still emits, len %u",
          h.last_len);
  }

  // ---- V: GET_AVB_INFO + GET_AS_PATH (IEEE SS7.4.40/SS7.4.41) ------------
  {
    const uint64_t KEY  = 0x0000000300050000ull;   // any locate hit
    const uint64_t TYIX = 0x0000000000090000ull;   // {type 9, index 0}
    CHECK(h.run(E_GAVB, KEY, false, 4000, TYIX), "V1 AVB_INFO completes");
    CHECK(h.last_status == ST_OK && h.last_len == 12 + 28,
          "V1 SUCCESS, 20 fixed + 2x4 mappings (st %u len %u)",
          h.last_status, h.last_len);
    CHECK(h.w32(12) == 0x00090000u, "V1 type+index @24");
    CHECK(h.w32(16) == 0x11121314u && h.w32(20) == 0x15161718u,
          "V1 grandmaster @28");
    CHECK(h.w32(24) == 0x00000BADu, "V1 propagation_delay @36");
    CHECK(h.w32(28) == 0x00070002u, "V1 domain+flags+count @40, got %08x",
          h.w32(28));
    CHECK(h.w32(32) == 0x06030002u && h.w32(36) == 0x07030002u,
          "V1 mappings in ordinal order (%08x %08x)", h.w32(32), h.w32(36));

    h.gasp_count = 3;
    CHECK(h.run(E_GASP, KEY, false, 4000, 0x0000000000000000ull),
          "V2 AS_PATH completes");
    CHECK(h.last_status == ST_OK && h.last_len == 12 + 4 + 24,
          "V2 SUCCESS, count 3 (st %u len %u)", h.last_status, h.last_len);
    CHECK(h.w32(12) == 0x00030000u,
          "V2 index @24 + count @26 (le-view %08x)", h.w32(12));
    CHECK(h.w32(16) == 0xAB000000u && h.w32(20) == 0x00000000u,
          "V2 entry 0 @28");
    CHECK(h.w32(32) == 0xAB000000u && h.w32(36) == 0x00000002u,
          "V2 entry 2 at @44 (%08x %08x)", h.w32(32), h.w32(36));

    h.gasp_count = 0;
    CHECK(h.run(E_GASP, KEY, false, 4000, 0), "V3 empty path completes");
    CHECK(h.last_status == ST_OK && h.last_len == 16,
          "V3 count 0 emits an empty list, len %u", h.last_len);
  }

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
