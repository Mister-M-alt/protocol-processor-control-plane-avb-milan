// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_timer_service suite — independent expectations, never DUT logic.
//
// The model derives expected time purely from cycle arithmetic — after
// reset release, now(c) = c / (DIV_US * DIV_MS) — and expected expiries
// purely from the arm history: an armed slot with absolute deadline D
// fires exactly once, during the first sweep whose now_ms >= D, i.e. at
// ms max(D, now_at_arm + 1), within SLOTS + 2 cycles of that ms boundary.
// It never mirrors the DUT's prescaler counters, sweep walker, or RAM.
#include <cstdint>
#include <cstdio>
#include <vector>
#include "VKL_pp_timer_service.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// compressed-time geometry — must match the -G overrides in the Makefile
enum { DIV_US = 4, DIV_MS = 5, CPM = DIV_US * DIV_MS, SLOTS = 12 };

struct Event { uint64_t cycle; unsigned slot; unsigned owner; uint32_t now; };

struct Harness {
  VKL_pp_timer_service* dut;
  uint64_t cycle = 0;              // cycles since reset release
  std::vector<Event> ev;
  uint32_t last_now = 0;
  int      mono_viol = 0;          // now_ms ever decreased or jumped by >1
  uint64_t tick_count = 0, last_tick = 0;
  int      spacing_viol = 0;       // ms ticks not exactly CPM cycles apart

  explicit Harness(VKL_pp_timer_service* d) : dut(d) {}

  void tick() {
    // settle combinational logic with this cycle's inputs
    dut->clk_i = 0; dut->eval();
    // observe combinational outputs PRE-EDGE (what the event bus sees)
    if (dut->exp_valid_o)
      ev.push_back({cycle, (unsigned)dut->exp_slot_o,
                    (unsigned)dut->exp_owner_o, (uint32_t)dut->now_ms_o});
    if (dut->tick_ms_o) {
      if (tick_count && (cycle - last_tick) != CPM) ++spacing_viol;
      last_tick = cycle;
      ++tick_count;
    }
    uint32_t now = dut->now_ms_o;
    if (now < last_now || now - last_now > 1) ++mono_viol;
    last_now = now;
    // rising edge: registers update
    dut->clk_i = 1; dut->eval();
    ++cycle;
  }

  // independent timebase: pure spec arithmetic, no DUT state
  uint32_t model_now() const { return (uint32_t)(cycle / CPM); }

  void run_ms(unsigned n) { for (unsigned i = 0; i < n * CPM; ++i) tick(); }

  void arm(unsigned slot, unsigned owner, uint32_t deadline_ms) {
    dut->arm_valid_i = 1; dut->arm_cancel_i = 0;
    dut->arm_slot_i = slot; dut->arm_owner_i = owner;
    dut->arm_deadline_ms_i = deadline_ms;
    tick();
    dut->arm_valid_i = 0;
  }

  void cancel(unsigned slot) {
    dut->arm_valid_i = 1; dut->arm_cancel_i = 1; dut->arm_slot_i = slot;
    tick();
    dut->arm_valid_i = 0; dut->arm_cancel_i = 0;
  }

  int count_slot(unsigned slot) const {
    int n = 0;
    for (const auto& e : ev) if (e.slot == slot) ++n;
    return n;
  }
  const Event* first_slot(unsigned slot) const {
    for (const auto& e : ev) if (e.slot == slot) return &e;
    return nullptr;
  }
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_pp_timer_service;
  Harness h(dut);

  // ---- reset (synchronous active-low) ---------------------------------
  dut->rst_n = 0;
  dut->arm_valid_i = 0; dut->arm_cancel_i = 0;
  dut->arm_slot_i = 0; dut->arm_owner_i = 0; dut->arm_deadline_ms_i = 0;
  for (int i = 0; i < 4; ++i) {
    dut->clk_i = 0; dut->eval();
    dut->clk_i = 1; dut->eval();
  }
  dut->rst_n = 1;
  dut->clk_i = 0; dut->eval();
  CHECK(dut->now_ms_o == 0, "now_ms is 0 out of reset (got %u)", dut->now_ms_o);
  CHECK(dut->exp_valid_o == 0, "no expiry out of reset");

  // ---- T1: free-run, nothing armed ------------------------------------
  h.run_ms(5);   // 100 cycles
  CHECK(h.model_now() == 5 && dut->now_ms_o == 5,
        "now free-runs against the cycle model (got %u, want 5)", dut->now_ms_o);
  CHECK(h.ev.empty(),
        "five full sweeps with zero armed slots emit nothing (got %zu)",
        h.ev.size());
  CHECK(h.tick_count == 5, "five ms ticks in 100 cycles (got %llu)",
        (unsigned long long)h.tick_count);
  CHECK(h.spacing_viol == 0, "ms ticks exactly %d cycles apart", CPM);

  // ---- T2: N staggered arms — model expected expiry times -------------
  // armed at now = 5, so every deadline > 6 fires exactly at its own ms
  struct ArmSpec { unsigned slot, owner; uint32_t dl; };
  const ArmSpec specs[6] = {
    {0, 0xC0, 7},  {2, 0xA2, 8},  {7, 0xE7, 9},
    {5, 0xB5, 10}, {11, 0xD1, 12}, {3, 0xF3, 15},
  };
  for (const auto& s : specs) h.arm(s.slot, s.owner, s.dl);
  h.run_ms(12);  // through now = 17 > 15
  for (const auto& s : specs) {
    CHECK(h.count_slot(s.slot) == 1, "T2 slot %u fires exactly once (got %d)",
          s.slot, h.count_slot(s.slot));
    const Event* e = h.first_slot(s.slot);
    CHECK(e && e->owner == s.owner,
          "T2 slot %u owner tag 0x%02X (got 0x%02X)", s.slot, s.owner,
          e ? e->owner : 0u);
    CHECK(e && e->now == s.dl,
          "T2 slot %u fires at its absolute deadline %u ms (got %u)",
          s.slot, s.dl, e ? e->now : 0u);
    CHECK(e && e->cycle >= (uint64_t)s.dl * CPM &&
              e->cycle <= (uint64_t)s.dl * CPM + SLOTS + 2,
          "T2 slot %u expiry inside the first sweep after its ms", s.slot);
  }
  CHECK(h.ev.size() == 6, "T2 no stray events (got %zu)", h.ev.size());
  h.ev.clear();

  // ---- T3: cancel before expiry never fires ---------------------------
  {
    uint32_t n3 = h.model_now();
    h.arm(4, 0x44, n3 + 5);
    h.run_ms(2);
    h.cancel(4);
    h.run_ms(8);   // well past n3 + 5
    CHECK(h.ev.empty(), "T3 cancel before expiry never fires (got %zu)",
          h.ev.size());
  }

  // ---- T4: re-arm after expiry (slot 2 already fired in T2) -----------
  {
    uint32_t n4 = h.model_now();
    h.arm(2, 0xA9, n4 + 3);
    h.run_ms(6);
    CHECK(h.count_slot(2) == 1, "T4 re-armed slot fires again (got %d)",
          h.count_slot(2));
    const Event* e = h.first_slot(2);
    CHECK(e && e->owner == 0xA9, "T4 re-arm carries the NEW owner tag");
    CHECK(e && e->now == n4 + 3, "T4 re-arm fires at the new deadline (got %u)",
          e ? e->now : 0u);
    CHECK(h.ev.size() == 1, "T4 no stray events (got %zu)", h.ev.size());
    h.ev.clear();
  }

  // ---- T5: two expiries in the same ms serialize one per cycle --------
  {
    uint32_t n5 = h.model_now();
    h.arm(6, 0x66, n5 + 4);
    h.arm(9, 0x99, n5 + 4);
    h.run_ms(6);
    CHECK(h.count_slot(6) == 1 && h.count_slot(9) == 1,
          "T5 both same-ms slots fire once (got %d, %d)",
          h.count_slot(6), h.count_slot(9));
    const Event *a = h.first_slot(6), *b = h.first_slot(9);
    CHECK(a && b && a->now == b->now && a->now == n5 + 4,
          "T5 both fire in the same ms");
    CHECK(a && b && a->cycle != b->cycle,
          "T5 expiries serialize: distinct cycles");
    CHECK(a && b && b->cycle > a->cycle,
          "T5 sweep order: ascending slot index");
    CHECK(a && b && b->cycle - a->cycle == 3,
          "T5 one slot per cycle: slots 6->9 are 3 cycles apart (got %llu)",
          (a && b) ? (unsigned long long)(b->cycle - a->cycle) : 0ull);
    CHECK(h.ev.size() == 2, "T5 exactly two events (got %zu)", h.ev.size());
    h.ev.clear();
  }

  // ---- T6: a deadline already in the past fires on the next sweep -----
  {
    uint32_t n6 = h.model_now();
    h.arm(1, 0x11, n6 - 3);
    h.run_ms(3);
    CHECK(h.count_slot(1) == 1, "T6 past deadline fires (got %d)",
          h.count_slot(1));
    const Event* e = h.first_slot(1);
    CHECK(e && (e->now == n6 + 1 || e->now == n6),
          "T6 past deadline fires on the next sweep (got %u, armed at %u)",
          e ? e->now : 0u, n6);
    h.ev.clear();
  }

  // ---- T7: quiet tail — self-disarm holds, timebase stays true --------
  h.run_ms(5);
  CHECK(h.ev.empty(), "T7 expired slots stay disarmed (got %zu)", h.ev.size());
  CHECK(h.mono_viol == 0, "now_ms monotonic, +0/+1 per cycle, entire run");
  CHECK(h.spacing_viol == 0, "every ms tick exactly %d cycles apart, entire run",
        CPM);
  CHECK(dut->now_ms_o == h.model_now(),
        "now matches the independent cycle model at end (got %u, want %u)",
        dut->now_ms_o, h.model_now());

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
