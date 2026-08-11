// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_aecp_ucpu skeleton suite — independent expectations, never DUT logic.
//
// Models the state port (2-cycle read latency, locate XOR map), the gather
// port (2-cycle latency, sel-keyed data), a reluctant TX (3 stall cycles),
// and the lock context. Captures every response-buffer write and checks the
// exact bytes each µprogram must produce, per the 06 §8 exemplar semantics.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "VKL_aecp_ucpu.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// entry points — mirror gen_ucode.py
enum { E_GETSR = 16, E_ALU = 64, E_ITER = 128, E_CHKARG = 192,
       E_LOCK = 224, E_GATHER = 256 };

static const uint64_t CTLR  = 0xC0FFEE00DEADBEEFull;
static const uint64_t OPD1  = 0x0000000000001234ull;   // seq echo in r13

struct Harness {
  VKL_aecp_ucpu* dut;
  uint64_t t = 0;
  // captured response buffer + bookkeeping
  uint32_t buf[160];
  bool     bad_write = false;
  int      sends = 0;
  uint32_t last_len = 0, last_status = 0;
  // state-port model
  int      st_lat = 0;
  bool     st_err_next = false;
  uint64_t st_data_next = 0;
  // gather model
  int      gx_lat = 0;
  uint64_t gx_data_next = 0;
  // tx model
  int      tx_wait = 0;
  bool     lock_scenario = false;

  explicit Harness(VKL_aecp_ucpu* d) : dut(d) { memset(buf, 0, sizeof buf); }

  static uint64_t gxval(uint8_t sel) {
    if (sel == 0x25) return 0x1111222233334444ull;
    if ((sel & 0xF0) == 0x10) return 0xC0ull + (sel & 0x0F);
    return 0x5A5A5A5A00000000ull | sel;
  }

  void tick() {
    // ---- respond to last cycle's requests (combinational inputs first)
    dut->st_rvalid_i = 0; dut->st_err_i = 0;
    dut->gx_valid_i  = 0;
    dut->st_ready_i  = 1;
    dut->lock_held_i = lock_scenario;
    dut->lock_ctlr_i = 0x1122334455667788ull;

    if (dut->st_req_o && !dut->st_we_o) {
      if (st_lat == 0) {
        uint32_t a = dut->st_addr_o;
        if (a == (0x100u ^ 0x0003u)) { st_data_next = 0x500; st_err_next = false; }
        else if (a == (0x100u ^ 0xBADu)) { st_data_next = 0; st_err_next = true; }
        else if (a == 0x508u) { st_data_next = 0xBB80; st_err_next = false; }
        else { st_data_next = 0xEEEE; st_err_next = false; }
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

    if (dut->resp_send_o) { /* accepted this cycle */ }
    dut->tx_ready_i = (tx_wait == 0);
    if (tx_wait > 0) --tx_wait;

    // ---- settle combinational logic with this cycle's inputs
    dut->clk_i = 0; dut->eval();
    ++t;

    // ---- observe combinational outputs PRE-EDGE (what the registers see)
    if (dut->rb_we_o) {
      uint32_t a = dut->rb_addr_o, d = dut->rb_wdata_o, s = dut->rb_wstrb_o;
      if (d == 0xBAD) bad_write = true;
      if (a / 4 < 160) {
        uint32_t m = ((s & 1) ? 0x000000FFu : 0) | ((s & 2) ? 0x0000FF00u : 0)
                   | ((s & 4) ? 0x00FF0000u : 0) | ((s & 8) ? 0xFF000000u : 0);
        buf[a / 4] = (buf[a / 4] & ~m) | (d & m);
      }
    }
    if (dut->resp_send_o && dut->tx_ready_i) {
      ++sends; last_len = dut->resp_len_o; last_status = dut->resp_status_o;
    }

    // ---- rising edge: registers update
    dut->clk_i = 1; dut->eval();
  }

  // run one dispatched program to completion
  bool run(uint16_t upc, uint64_t opd0, bool lock, int max_cycles = 400) {
    memset(buf, 0, sizeof buf);
    bad_write = false; sends = 0; lock_scenario = lock;
    tx_wait = 3;                       // make SEND_RESPONSE stall first
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

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_aecp_ucpu;
  Harness h(dut);

  // reset (synchronous active-low)
  dut->rst_n = 0; dut->disp_valid_i = 0;
  for (int i = 0; i < 4; ++i) h.tick();
  dut->rst_n = 1;
  h.tick();
  CHECK(dut->disp_ready_o == 1, "idle after reset");

  // ---- P1: GET_SAMPLING_RATE, success --------------------------------
  CHECK(h.run(E_GETSR, 0x0000000700250003ull, false), "P1 completes");
  CHECK(h.buf[0] == 0xC0FFEE00u, "P1 hdr0 eid-hi got %08x", h.buf[0]);
  CHECK(h.buf[1] == 0xDEADBEEFu, "P1 hdr1 eid-lo got %08x", h.buf[1]);
  CHECK(h.buf[2] == hdr2(0), "P1 hdr2 status/seq got %08x", h.buf[2]);
  CHECK(h.buf[3] == 0x0000BB80u, "P1 rate field got %08x", h.buf[3]);
  CHECK(h.last_len == 16, "P1 len 16 got %u", h.last_len);
  CHECK(h.last_status == 0, "P1 status SUCCESS got %u", h.last_status);
  CHECK(h.sends == 1, "P1 exactly one send got %d", h.sends);

  // ---- P1b: locate miss -> NO_SUCH_DESCRIPTOR through the fail handler
  CHECK(h.run(E_GETSR, 0x0000000700250BADull, false), "P1b completes");
  CHECK(h.last_status == 2, "P1b status NO_SUCH_DESCRIPTOR got %u", h.last_status);
  CHECK(h.buf[2] == hdr2(2), "P1b hdr2 got %08x", h.buf[2]);
  CHECK(h.last_len == 12, "P1b header-only len got %u", h.last_len);
  CHECK(h.buf[3] == 0, "P1b no field written");

  // ---- P2: ALU / RAW / branch-flush / merge / qword field -------------
  CHECK(h.run(E_ALU, 0, false), "P2 completes");
  CHECK(!h.bad_write, "P2 poison ops were flushed");
  CHECK(h.buf[3] == 5 && h.buf[4] == 7 && h.buf[5] == 1 && h.buf[6] == 5,
        "P2 fields got %x %x %x %x", h.buf[3], h.buf[4], h.buf[5], h.buf[6]);
  CHECK(h.buf[7] == 0xC0FFEE00u && h.buf[8] == 0xDEADBEEFu,
        "P2 qword field got %08x %08x", h.buf[7], h.buf[8]);
  CHECK(h.last_len == 36, "P2 len 36 got %u", h.last_len);
  CHECK(h.last_status == 0, "P2 status got %u", h.last_status);

  // ---- P3: ITER/APPEND loop -------------------------------------------
  CHECK(h.run(E_ITER, 0, false), "P3 completes");
  CHECK(h.buf[3] == 0xAB && h.buf[4] == 0xAB && h.buf[5] == 0xAB,
        "P3 three appends got %x %x %x", h.buf[3], h.buf[4], h.buf[5]);
  CHECK(h.buf[6] == 0, "P3 exactly three appends");
  CHECK(h.last_len == 24, "P3 len 24 got %u", h.last_len);

  // ---- P4: CHECK_ARG failure ------------------------------------------
  CHECK(h.run(E_CHKARG, 0, false), "P4 completes");
  CHECK(h.last_status == 13, "P4 BAD_ARGUMENTS got %u", h.last_status);
  CHECK(h.buf[2] == hdr2(13), "P4 hdr2 got %08x", h.buf[2]);

  // ---- P5: CHECK_LOCK failure ------------------------------------------
  CHECK(h.run(E_LOCK, 0, true), "P5 completes");
  CHECK(h.last_status == 3, "P5 LOCKED got %u", h.last_status);
  CHECK(h.buf[2] == hdr2(3), "P5 hdr2 got %08x", h.buf[2]);

  // ---- P5b: same program, lock free -> SUCCESS -------------------------
  CHECK(h.run(E_LOCK, 0, false), "P5b completes");
  CHECK(h.last_status == 0, "P5b unlocked SUCCESS got %u", h.last_status);

  // ---- P6: GATHER_EXT + qword + READ_COUNTERS burst --------------------
  CHECK(h.run(E_GATHER, 0, false), "P6 completes");
  CHECK(h.buf[3] == 0x11112222u && h.buf[4] == 0x33334444u,
        "P6 gathered qword got %08x %08x", h.buf[3], h.buf[4]);
  CHECK(h.buf[5] == 0xC0 && h.buf[6] == 0xC1 && h.buf[7] == 0xC2 &&
        h.buf[8] == 0xC3, "P6 counters got %x %x %x %x",
        h.buf[5], h.buf[6], h.buf[7], h.buf[8]);
  CHECK(h.last_len == 36, "P6 len 36 got %u", h.last_len);

  // ---- re-dispatch readiness -------------------------------------------
  h.tick();
  CHECK(dut->disp_ready_o == 1, "ready again after all programs");

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
