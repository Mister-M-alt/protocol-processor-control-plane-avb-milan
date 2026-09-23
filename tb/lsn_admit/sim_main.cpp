// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_acmp_lsn_admit suite: the gate alone against an independent model
// of the ownership contract (docs/architecture/05 section 5.1), every
// output compared on every cycle.
//
// The integration suites (tb/acmp_nvm group L, tb/pp_top section BW) drive
// the gate from the real binding manager and listener, whose timing makes
// three of the four release terms redundant: the manager's terminal rises
// one cycle after its last preload was taken, when the listener is already
// back in X_IDLE and pre_valid is already low. This suite drives the four
// release inputs independently, so the terminal can coincide with a preload
// still presented, the listener still busy or its A4 strobe still up, and
// each term is graded on its own.
#include <cstdint>
#include <cstdio>
#include "VKL_pp_acmp_lsn_admit.h"
#include "verilated.h"
#include "../common/verilator_harness.hpp"

#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

namespace {

constexpr unsigned N_SINKS = 8;        // the module's defaults
constexpr unsigned OWNER_BASE = 32;

// the contract, written from the architecture page, not from the RTL
struct Model {
  bool own = true;
  unsigned drops = 0;
  void reset() { own = true; drops = 0; }
  // one clock edge with these inputs
  void edge(bool done, bool pre, bool busy, bool arm, bool exp, unsigned owner) {
    const bool lsn_owner = owner >= OWNER_BASE && owner < OWNER_BASE + N_SINKS;
    if (own && exp && lsn_owner && drops < 0xFFFF) ++drops;
    if (own && done && !pre && !busy && !arm) own = false;
  }
};

struct In {
  bool done, pre, busy, arm;
  bool ptxn, ltxnr, ptk, ltkr, pstrm, pexp;
  unsigned owner;
};

// xorshift, so the run is reproducible
struct Rng {
  uint32_t s = 0x92A218u;
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  bool bit(unsigned pct) { return next() % 100 < pct; }
};

class LsnAdmitSuite {
 public:
  int run();

 private:
  VKL_pp_acmp_lsn_admit* d = nullptr;
  Model m;
  int checks = 0;
  int fails = 0;
  long mism = 0;          // cycles any output disagreed with the model
  long first_mism = -1;
  long cyc = 0;

  void drive(const In& i) {
    d->walk_done_i = i.done; d->pre_valid_i = i.pre;
    d->lsn_busy_i = i.busy;  d->lsn_arm_i = i.arm;
    d->p_txn_valid_i = i.ptxn; d->l_txn_ready_i = i.ltxnr;
    d->p_tk_valid_i = i.ptk;   d->l_tk_ready_i = i.ltkr;
    d->p_strm_valid_i = i.pstrm;
    d->p_exp_valid_i = i.pexp; d->p_exp_owner_i = uint8_t(i.owner);
    d->clk_i = 0;
    d->eval();
  }
  // compare every output with the model for the inputs of this cycle
  bool agrees(const In& i) const {
    const bool rel = !m.own;
    return d->own_o == m.own && d->released_o == rel
        && d->l_txn_valid_o == (i.ptxn && rel) && d->p_txn_ready_o == (i.ltxnr && rel)
        && d->l_tk_valid_o == (i.ptk && rel) && d->p_tk_ready_o == (i.ltkr && rel)
        && d->l_strm_valid_o == (i.pstrm && rel) && d->l_exp_valid_o == (i.pexp && rel)
        && d->dbg_exp_drop_o == m.drops;
  }
  void step(const In& i) {
    drive(i);
    if (d->rst_n && !agrees(i)) {
      if (first_mism < 0) first_mism = cyc;
      ++mism;
    }
    d->clk_i = 1;
    d->eval();
    if (d->rst_n) m.edge(i.done, i.pre, i.busy, i.arm, i.pexp, i.owner);
    ++cyc;
  }
  void reset() {
    d->rst_n = 0;
    step(In{});
    step(In{});
    d->rst_n = 1;
    m.reset();
  }
};

int LsnAdmitSuite::run() {
  const milan::tb::Model<VKL_pp_acmp_lsn_admit> model;
  d = model.get();

  // ---- R: reset ownership, every face held ------------------------------
  reset();
  In all{};
  all.ptxn = all.ltxnr = all.ptk = all.ltkr = all.pstrm = all.pexp = true;
  all.owner = OWNER_BASE;
  drive(all);
  CHECK(d->own_o && !d->released_o, "R1 the gate owns the faces from reset");
  CHECK(!d->l_txn_valid_o && !d->p_txn_ready_o && !d->l_tk_valid_o
            && !d->p_tk_ready_o && !d->l_strm_valid_o && !d->l_exp_valid_o,
        "R2 nothing passes while owned, neither valid nor ready");

  // ---- T: each release term held alone against the terminal --------------
  struct Term { const char* name; In in; };
  In t_pre{};  t_pre.done = true;  t_pre.pre = true;
  In t_busy{}; t_busy.done = true; t_busy.busy = true;
  In t_arm{};  t_arm.done = true;  t_arm.arm = true;
  In t_none{}; t_none.pre = false;
  const Term terms[] = {
    {"a preload still presented", t_pre},
    {"the listener still busy", t_busy},
    {"the last A4 strobe still up", t_arm},
    {"no terminal yet", t_none},
  };
  for (const Term& t : terms) {
    reset();
    for (int k = 0; k < 50; ++k) step(t.in);
    drive(t.in);
    CHECK(d->own_o, "T %s: the gate keeps the faces for 50 cycles", t.name);
    In go{}; go.done = true;
    step(go);
    drive(go);
    CHECK(!d->own_o && d->released_o,
          "T %s: released the cycle after the term clears", t.name);
  }

  // ---- O: once released, never owned again before a reset ----------------
  {
    reset();
    In go{}; go.done = true;
    step(go);
    bool stayed = true;
    Rng r;
    for (int k = 0; k < 2000; ++k) {
      In i{};
      i.done = r.bit(50); i.pre = r.bit(50); i.busy = r.bit(50); i.arm = r.bit(50);
      step(i);
      drive(i);
      stayed = stayed && !d->own_o;
    }
    CHECK(stayed, "O1 the release is one-way until the next reset");
    reset();
    drive(In{});
    CHECK(d->own_o, "O2 a reset takes the faces back");
  }

  // ---- D: the refused-expiry count ----------------------------------------
  {
    reset();
    In e{}; e.pexp = true;
    unsigned want = 0;
    for (unsigned owner = 0; owner < 256; ++owner) {
      e.owner = owner;
      step(e);
      if (owner >= OWNER_BASE && owner < OWNER_BASE + N_SINKS) ++want;
    }
    drive(In{});
    CHECK(d->dbg_exp_drop_o == want,
          "D1 only a listener owner's expiry is counted (%u, want %u)",
          unsigned(d->dbg_exp_drop_o), want);
    e.owner = OWNER_BASE + N_SINKS - 1;
    for (unsigned k = 0; k < 0x10000u + 10; ++k) step(e);
    drive(In{});
    CHECK(d->dbg_exp_drop_o == 0xFFFF, "D2 the count saturates at 0xFFFF");
    In go{}; go.done = true;
    step(go);
    const unsigned held = d->dbg_exp_drop_o;
    reset();
    drive(In{});
    CHECK(held == 0xFFFF && d->dbg_exp_drop_o == 0, "D3 a reset clears it");
    step(go);
    e.owner = OWNER_BASE;
    for (int k = 0; k < 20; ++k) step(e);
    drive(In{});
    CHECK(d->dbg_exp_drop_o == 0, "D4 nothing is counted once released");
  }

  // ---- M: random stimulus, every output against the model every cycle ----
  {
    Rng r;
    mism = 0;
    first_mism = -1;
    long releases = 0;
    long coincide[4] = {0, 0, 0, 0};   // the terminal met each term
    for (int run = 0; run < 400; ++run) {
      reset();
      // a walk whose terminal comes after a random span, with the release
      // terms raised at random around it
      const int span = 5 + int(r.next() % 60);
      for (int k = 0; k < span + 60; ++k) {
        In i{};
        i.done = k >= span;
        i.pre = r.bit(k < span ? 40 : 45);
        i.busy = r.bit(k < span ? 40 : 45);
        i.arm = r.bit(k < span ? 20 : 30);
        i.ptxn = r.bit(50); i.ltxnr = r.bit(50);
        i.ptk = r.bit(50);  i.ltkr = r.bit(50);
        i.pstrm = r.bit(50); i.pexp = r.bit(30);
        i.owner = r.next() % 64;
        if (m.own && i.done) {
          coincide[0] += i.pre; coincide[1] += i.busy; coincide[2] += i.arm;
          coincide[3] += !i.pre && !i.busy && !i.arm;
        }
        const bool was = m.own;
        step(i);
        releases += was && !m.own;
      }
    }
    CHECK(mism == 0, "M1 every output agrees with the model on every cycle "
          "(%ld disagreeing cycles, first at %ld)", mism, first_mism);
    CHECK(releases > 300 && coincide[0] > 100 && coincide[1] > 100
              && coincide[2] > 50 && coincide[3] > 300,
          "M2 the stimulus met the terminal with each term (%ld %ld %ld %ld) "
          "and released %ld walks", coincide[0], coincide[1], coincide[2],
          coincide[3], releases);
  }

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  LsnAdmitSuite s;
  return s.run();
}
