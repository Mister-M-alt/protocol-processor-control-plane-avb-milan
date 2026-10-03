// SPDX-License-Identifier: CERN-OHL-W-2.0
// AECP registry monitor lifecycle, including TIME_LIMITED row reuse; with
// AECP_NOTIFY_IDENT (the second build, P-EN-IDENTIFY-NOTIFICATION = 1), the
// identify schedule at the full timebase instead (identify_timebase.hpp).
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>
#include "../common/verilator_harness.hpp"
#include "VKL_aecp_notify.h"
#include "verilated.h"

#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

static constexpr uint8_t OWN_TL = 0xA0;
static constexpr uint8_t OWN_MON = 0xD0;
static constexpr uint8_t REGMON_BASE = 25;
static constexpr uint8_t N_CTRL = 2;

// Cycle budgets for the polling loops below. Each one gives up when its budget
// is spent, so a DUT that never asserts fails the check instead of hanging.
static constexpr int REGISTRY_ACCEPT_CYCLES = 24;
static constexpr int DRAW_REQUEST_CYCLES = 16;
static constexpr int PROBE_WAIT_CYCLES = 12;
static constexpr int DRAIN_WATCH_CYCLES = 8;

namespace {

#ifndef AECP_NOTIFY_IDENT
// The tally the CHECK macro keeps was a pair of file-scope statics, and so was
// nothing else here; both are the state of one run of this harness, so they
// belong to the object that performs it.
struct Harness {
  VKL_aecp_notify* d = nullptr;
  uint32_t now = 1000;
  int checks = 0;
  int fails = 0;

  int run();
  bool cancels_row0(uint64_t eid, uint64_t mac);
  void identity_index(uint64_t eid_old, uint64_t mac_old, uint64_t eid, uint64_t mac);

  void tick() {
    d->now_ms_i = now;
    d->clk_i = 0;
    d->eval();
    d->clk_i = 1;
    d->eval();
  }

  void idle(int n = 1) {
    for (int i = 0; i < n; ++i) tick();
  }

  void register_row(uint64_t eid, uint64_t mac, bool time_limited) {
    d->rgy_state_i = 0;
    d->rgy_op_i = 0;
    d->rgy_eid_i = eid;
    d->rgy_mac_i = mac;
    d->rgy_tl_i = time_limited;
    d->rgy_req_i = 1;
    int guard = 0;
    while (guard++ < REGISTRY_ACCEPT_CYCLES) {
      d->clk_i = 0;
      d->eval();
      if (!d->rgy_wait_o) break;
      tick();
    }
    CHECK(guard < REGISTRY_ACCEPT_CYCLES && d->rgy_data_o == 0,
          "registry accepts controller tuple");
    tick();
    d->rgy_req_i = 0;
    idle(2);
  }

  void complete_draw() {
    d->prng_draw_busy_i = 0;
    int guard = 0;
    while (guard++ < DRAW_REQUEST_CYCLES) {
      d->clk_i = 0;
      d->eval();
      if (d->prng_draw_req_o) break;
      tick();
    }
    CHECK(guard < DRAW_REQUEST_CYCLES,
          "registration requests an independent monitor draw");
    d->prng_draw_ms_i = 30000;
    d->prng_draw_valid_i = 1;
    tick();
    d->prng_draw_valid_i = 0;
    d->prng_draw_busy_i = 1;
    idle(2);
  }

  void expire(uint8_t slot, uint8_t owner) {
    d->tmr_exp_slot_i = slot;
    d->tmr_exp_owner_i = owner;
    d->tmr_exp_valid_i = 1;
    tick();
    d->tmr_exp_valid_i = 0;
  }

  bool wait_probe(uint64_t eid, uint64_t mac) {
    for (int i = 0; i < PROBE_WAIT_CYCLES; ++i) {
      d->clk_i = 0;
      d->eval();
      if (d->ca_valid_o) {
        bool exact = d->ca_owner_o == 0 && d->ca_ctlr_eid_o == eid
                     && static_cast<uint64_t>(d->ca_mac_o) == mac;
        tick();
        return exact;
      }
      tick();
    }
    return false;
  }
};

int Harness::run() {
  const milan::tb::Model<VKL_aecp_notify> model;
  VKL_aecp_notify* const dut = model.get();
  d = dut;

  dut->rgy_req_i = 0;
  dut->rgy_state_i = 0;
  dut->rgy_op_i = 0;
  dut->rgy_eid_i = 0;
  dut->rgy_mac_i = 0;
  dut->rgy_tl_i = 0;
  dut->ev_stri_in_i = 0;
  dut->ev_stri_out_i = 0;
  dut->ev_avb_i = 0;
  dut->ev_asp_i = 0;
  dut->ev_amap_i = 0;
  dut->ev_amap_remove_i = 0;
  dut->ev_amap_type_i = 0;
  dut->ev_amap_index_i = 0;
  dut->ev_amap_count_i = 0;
  dut->ev_amap_excl_eid_i = 0;
  dut->ev_ctr_i = 0;
  dut->ev_ctr_type_i = 0;
  dut->ev_ctr_index_i = 0;
  dut->ev_cmd_i = 0;
  dut->ev_cmd_class_i = 0;
  dut->ev_cmd_type_i = 0;
  dut->ev_cmd_index_i = 0;
  dut->ev_cmd_arg0_i = 0;
  dut->ev_cmd_arg1_i = 0;
  dut->ev_cmd_excl_eid_i = 0;
  dut->rx_cmd_valid_i = 0;
  dut->rx_cmd_eid_i = 0;
  dut->rx_cmd_mac_i = 0;
  dut->prng_draw_busy_i = 1;
  dut->prng_draw_valid_i = 0;
  dut->prng_draw_ms_i = 0;
  dut->ca_ready_i = 1;
  dut->ca_rsp_valid_i = 0;
  dut->ca_rsp_owner_i = 0;
  dut->ca_fail_valid_i = 0;
  dut->ca_fail_owner_i = 0;
  dut->uns_done_i = 1;
  dut->tmr_exp_valid_i = 0;
  dut->tmr_exp_slot_i = 0;
  dut->tmr_exp_owner_i = 0;

  dut->rst_n = 0;
  idle(4);
  dut->rst_n = 1;
  idle(2);

  const uint64_t EID_A = 0x1111000000000001ull;
  const uint64_t MAC_A = 0x020000000001ull;
  const uint64_t EID_B = 0x2222000000000002ull;
  const uint64_t MAC_B = 0x020000000002ull;

  register_row(EID_A, MAC_A, true);
  CHECK(dut->dbg_reg_cnt_o == 1, "A TIME_LIMITED row is live");
  complete_draw();
  expire(REGMON_BASE + N_CTRL, OWN_MON);
  CHECK(wait_probe(EID_A, MAC_A),
        "A monitor expiry launches the exact registered tuple");

  // Expire the registry row while its availability exchange is live. The
  // drain cycle must cancel owner zero before row zero becomes reusable.
  expire(REGMON_BASE, OWN_TL);
  bool saw_cancel = false;
  for (int i = 0; i < DRAIN_WATCH_CYCLES; ++i) {
    dut->clk_i = 0;
    dut->eval();
    if (dut->ca_cancel_valid_o && dut->ca_cancel_owner_o == 0)
      saw_cancel = true;
    tick();
  }
  CHECK(saw_cancel,
        "TIME_LIMITED drain cancels the active availability exchange");
  CHECK(dut->dbg_reg_cnt_o == 0, "expired row is removed after cancellation");

  register_row(EID_B, MAC_B, false);
  CHECK(dut->dbg_reg_cnt_o == 1, "expired row can be reused by a new tuple");
  complete_draw();
  expire(REGMON_BASE + N_CTRL, OWN_MON);
  CHECK(wait_probe(EID_B, MAC_B),
        "reused row launches only the new controller tuple");

  identity_index(EID_A, MAC_A, EID_B, MAC_B);
  return fails ? 1 : 0;
}

// ---- IX: the identity index (issue #232) ------------------------------------
// The availability monitor matches a command's {eid, mac} against every row in
// the command's own cycle, through a LUTRAM index rather than a comparator per
// row. Row 0 holds B, which reused A's row, and B's probe is live, so a match
// on row 0 shows combinationally as the probe's cancellation (ca_cancel_valid_o).
// The probes below only evaluate: no clock edge, no state change.
bool Harness::cancels_row0(uint64_t eid, uint64_t mac) {
  d->rx_cmd_eid_i = eid;
  d->rx_cmd_mac_i = mac;
  d->rx_cmd_valid_i = 1;
  d->clk_i = 0;
  d->eval();
  const bool hit = d->ca_cancel_valid_o && d->ca_cancel_owner_o == 0;
  d->rx_cmd_valid_i = 0;
  d->eval();
  return hit;
}

void Harness::identity_index(uint64_t eid_old, uint64_t mac_old,
                             uint64_t eid, uint64_t mac) {
  CHECK(!cancels_row0(eid_old, mac_old),
        "IX1: the reused row no longer matches its previous controller");
  int neighbours = 0;
  for (int b = 0; b < 112; ++b) {
    const uint64_t e = b >= 48 ? eid ^ (1ull << (b - 48)) : eid;
    const uint64_t m = b < 48 ? mac ^ (1ull << b) : mac;
    if (cancels_row0(e, m)) ++neighbours;
  }
  CHECK(neighbours == 0,
        "IX2: no identity one bit from a registered one matches (%d of 112 did)",
        neighbours);
  CHECK(cancels_row0(eid, mac),
        "IX3: the registered identity matches in the command's cycle");

  // A REGISTER rewrites its row's index in the two cycles after the row write.
  // Two cycles after C claims row 1, C's command must still win against a
  // failed probe reported in the same cycle (KL_aecp_notify's ca_fail arm),
  // so the row stays; the same failure alone removes it.
  const uint64_t EID_C = 0x3333000000000003ull;
  const uint64_t MAC_C = 0x020000000003ull;
  d->rgy_state_i = 0;
  d->rgy_op_i = 0;
  d->rgy_eid_i = EID_C;
  d->rgy_mac_i = MAC_C;
  d->rgy_tl_i = 0;
  d->rgy_req_i = 1;
  int guard = 0;
  while (guard++ < REGISTRY_ACCEPT_CYCLES) {
    d->clk_i = 0;
    d->eval();
    if (!d->rgy_wait_o) break;
    tick();
  }
  CHECK(guard < REGISTRY_ACCEPT_CYCLES && d->rgy_data_o == 0,
        "IX4a: the registry accepts a second controller");
  tick();                                   // the row write's own cycle
  d->rx_cmd_eid_i = EID_C;
  d->rx_cmd_mac_i = MAC_C;
  d->rx_cmd_valid_i = 1;
  d->ca_fail_owner_i = 1;
  d->ca_fail_valid_i = 1;
  tick();                                   // the cycle after it
  d->rx_cmd_valid_i = 0;
  d->ca_fail_valid_i = 0;
  d->rgy_req_i = 0;
  idle(DRAIN_WATCH_CYCLES);
  CHECK(d->dbg_reg_cnt_o == 2,
        "IX4: a command two cycles after its REGISTER keeps the row against a "
        "failed probe in the same cycle");
  d->ca_fail_owner_i = 1;
  d->ca_fail_valid_i = 1;
  tick();
  d->ca_fail_valid_i = 0;
  idle(DRAIN_WATCH_CYCLES);
  CHECK(d->dbg_reg_cnt_o == 1, "IX4b: the same failure alone removes the row");
}

#else
// ---- FT: the identify schedule at the full timebase -------------------------
// tb/pp_top grades the burst on the wire with 1 ms compressed to 100 clocks,
// where one tick is no longer than a frame's own build and serialization, so
// the sequencer's one-tick margins cannot be seen there (reviews R420-2 S1 and
// R421-2 S2). Here 1 ms is 100,000 clocks, the F01.5 default P-CLK-HZ of
// 100 MHz with the top's default prescaler (TIM_DIV_US_P 100, TIM_DIV_MS_P
// 1000), and the bench is the engine, the MAC and the shared timer service, so
// a frame leaves on the clock the bench picks:
//   - IDENT-BURST counts from the next ms boundary after a frame left
//     (IEEE 1722.1-2021 7.5.1: "a delay of 150 milliseconds between
//     transmissions"), so a frame that leaves on the last clock whose arm still
//     lands in its ms is followed no sooner than T-IDENT-BURST (FT2), and one
//     that leaves on a ms's first clock less than one tick later (FT3);
//   - t0, the base of Figure 7-142's timeout (currentTime + 1 second), is the
//     next boundary after the first frame left, so a held button re-arms no
//     sooner than T-IDENT-REARM after it (FT4).
// The timer model fires a due slot on the first clock of its deadline's ms,
// with no sweep delay: the least favourable placement for both margins (the
// real service's sweep only adds clocks).
struct IdentHarness {
  static constexpr uint64_t CPM = 100000;          // clocks per ms
  static constexpr uint64_t BURST = 150 * CPM;     // T-IDENT-BURST (F08.1)
  static constexpr uint64_t REARM = 1000 * CPM;    // T-IDENT-REARM (F08.1)
  static constexpr uint64_t LATE = 8;              // job presentation after an expiry
  static constexpr uint64_t ACCEPT = 20;           // the engine's pick and build
  static constexpr uint64_t SER = 1000;            // the frame on the wire, at least
  static constexpr uint64_t END = CPM - 2;         // the last clock whose arm stays in its ms
  static constexpr uint64_t START = 0;
  static constexpr uint32_t BASE_MS = 1000;
  static constexpr uint8_t IDENT_KIND = 15;        // the uns face's identify job (pp_pkg)
  struct Slot {
    bool armed = false;
    uint8_t owner = 0;
    uint32_t deadline = 0;
  };
  struct Fire {
    uint8_t slot;
    uint8_t owner;
  };
  struct Frame {
    uint64_t presented = 0;                        // the job reached the uns face
    uint64_t departed = 0;                         // the first clock uns_tx_busy_i read 0
  };
  VKL_aecp_notify* d = nullptr;
  uint64_t cyc = 0;
  int checks = 0;
  int fails = 0;
  std::array<Slot, 128> slots{};
  std::deque<Fire> due;                            // one expiry per clock, in slot order

  uint32_t now() const { return BASE_MS + uint32_t(cyc / CPM); }

  //! one clock: the timer model's expiry, the ms timebase, the arm port
  void tick() {
    if (cyc % CPM == 0) {
      for (size_t s = 0; s < slots.size(); ++s) {
        if (slots[s].armed && int32_t(now() - slots[s].deadline) >= 0) {
          due.push_back({uint8_t(s), slots[s].owner});
          slots[s].armed = false;
        }
      }
    }
    d->tmr_exp_valid_i = due.empty() ? 0 : 1;
    if (!due.empty()) {
      d->tmr_exp_slot_i = due.front().slot;
      d->tmr_exp_owner_i = due.front().owner;
      due.pop_front();
    }
    d->now_ms_i = now();
    d->clk_i = 0;
    d->eval();
    if (d->tmr_arm_valid_o) {
      Slot& s = slots[d->tmr_arm_slot_o];
      s.armed = !d->tmr_arm_cancel_o;
      s.owner = d->tmr_arm_owner_o;
      s.deadline = d->tmr_arm_deadline_ms_o;
    }
    d->clk_i = 1;
    d->eval();
    ++cyc;
  }
  void idle(uint64_t n) {
    for (uint64_t i = 0; i < n; ++i) tick();
  }
  bool ident_presented() const { return d->uns_valid_o && d->uns_kind_o == IDENT_KIND; }

  //! the engine and the MAC for one identify job: take it, retire it with
  //! its frame granted (uns_done_i with uns_tx_busy_i high, as the top's
  //! departure flop is set on the same grant), hold the frame at least SER
  //! clocks, and let its last byte go `phase` clocks into a ms
  Frame frame(uint64_t phase) {
    Frame f;
    for (uint64_t c = 0; c < 2 * REARM && !ident_presented(); ++c) tick();
    if (!ident_presented()) return f;
    f.presented = cyc;
    idle(ACCEPT);
    d->uns_done_i = 1;
    d->uns_tx_busy_i = 1;
    tick();
    d->uns_done_i = 0;
    idle(SER);
    while (cyc % CPM != phase) tick();
    d->uns_tx_busy_i = 0;
    f.departed = cyc;
    return f;
  }
  void quiet_inputs();
  int run();
};

void IdentHarness::quiet_inputs() {
  d->rgy_req_i = 0;
  d->rgy_state_i = 0;
  d->ev_stri_in_i = 0;
  d->ev_stri_out_i = 0;
  d->ev_avb_i = 0;
  d->ev_asp_i = 0;
  d->ev_amap_i = 0;
  d->ev_ctr_i = 0;
  d->ev_cmd_i = 0;
  d->rx_cmd_valid_i = 0;
  d->prng_draw_busy_i = 1;
  d->prng_draw_valid_i = 0;
  d->ca_ready_i = 1;
  d->ca_rsp_valid_i = 0;
  d->ca_fail_valid_i = 0;
  d->uns_done_i = 0;
  d->uns_tx_busy_i = 0;
  d->tmr_exp_valid_i = 0;
  d->identify_button_i = 0;
  d->identify_index_i = 5;
}

int IdentHarness::run() {
  const milan::tb::Model<VKL_aecp_notify> model;
  d = model.get();
  quiet_inputs();
  d->rst_n = 0;
  idle(4);
  d->rst_n = 1;
  idle(CPM);
  //! held from here past the timeout: one burst, then Figure 7-142's re-entry
  d->identify_button_i = 1;
  const Frame f1 = frame(END);
  const Frame f2 = frame(START);
  const Frame f3 = frame(END);
  const Frame f4 = frame(END);
  d->identify_button_i = 0;
  idle(CPM);
  CHECK(f1.presented && f2.presented && f3.presented && f4.presented,
        "FT1: a held press presents a burst's three identify jobs, then at the "
        "timeout a fourth (premise; %d %d %d %d)", f1.presented ? 1 : 0,
        f2.presented ? 1 : 0, f3.presented ? 1 : 0, f4.presented ? 1 : 0);
  if (!(f1.presented && f2.presented && f3.presented && f4.presented)) return 1;
  const uint64_t g12 = f2.presented - f1.departed;
  printf("  [i] FT2: frame 1 left %lu clocks into its ms; frame 2 presented %lu "
         "clocks later\n", static_cast<unsigned long>(f1.departed % CPM),
         static_cast<unsigned long>(g12));
  CHECK(g12 >= BURST && g12 <= BURST + LATE, "FT2: frame 1 left two clocks "
        "before a ms boundary, and frame 2 is presented T-IDENT-BURST after that "
        "boundary, never one tick sooner: %lu clocks, want %lu to %lu",
        static_cast<unsigned long>(g12), static_cast<unsigned long>(BURST),
        static_cast<unsigned long>(BURST + LATE));
  const uint64_t g23 = f3.presented - f2.departed;
  printf("  [i] FT3: frame 2 left on its ms's first clock; frame 3 presented %lu "
         "clocks later\n", static_cast<unsigned long>(g23));
  CHECK(g23 >= BURST && g23 <= BURST + CPM + LATE, "FT3: frame 3 is presented less "
        "than one tick past T-IDENT-BURST after frame 2 left on its ms's first "
        "clock: %lu clocks, want %lu to %lu", static_cast<unsigned long>(g23),
        static_cast<unsigned long>(BURST), static_cast<unsigned long>(BURST + CPM + LATE));
  const uint64_t t14 = f4.presented - f1.departed;
  printf("  [i] FT4: the held button's next burst presented %lu clocks after frame "
         "1 left\n", static_cast<unsigned long>(t14));
  CHECK(t14 >= REARM && t14 <= REARM + LATE, "FT4: held, the next burst starts "
        "at Figure 7-142's timeout, T-IDENT-REARM after the boundary two clocks "
        "after the first frame left, never one tick sooner: %lu clocks, want %lu "
        "to %lu", static_cast<unsigned long>(t14), static_cast<unsigned long>(REARM),
        static_cast<unsigned long>(REARM + LATE));
  return fails ? 1 : 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  //! NOT the canonical tally shape: this binary is ONE of the suite's two
  //! builds, and run_suites.sh reads only the LAST matching line, so a
  //! canonical line here would drop the other build's checks from the total.
  //! The Makefile sums both builds and prints the one canonical line.
#ifdef AECP_NOTIFY_IDENT
  IdentHarness harness;
  const char* const build = "identify";
#else
  Harness harness;
  const char* const build = "default";
#endif
  const int rc = harness.run();
  printf("[build %s] %d checks, %d failures\n", build, harness.checks, harness.fails);
  FILE* acc = fopen("obj_dir/build_tally.txt", "a");
  if (acc == nullptr) {
    printf("FAIL: this build's tally cannot be recorded for the Makefile\n");
    return 1;
  }
  fprintf(acc, "%d %d\n", harness.checks, harness.fails);
  fclose(acc);
  return rc;
}
