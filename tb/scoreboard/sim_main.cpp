// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_scoreboard suite — independent expectations, never DUT logic.
//
// The C++ model is an independent transcription of the F03.7 hazard matrix
// (03 §6) as a LITERAL 9x9 table of {NEVER, SAMEKEY, ALWAYS} — a different
// formulation from the RTL's rule chain, so a divergence in either fails the
// run. On top of the matrix the model mirrors only the port CONTRACT: a
// MAX_HOLDS_P-deep hold set with opaque ids (no allocation-policy assumption
// beyond id uniqueness), the registered barrier-drain flag (a refused
// CFG_BARRIER blocks all non-barrier admission until the barrier grants on
// an empty table), and the rule (e) gate (a deadline-kill is honored only
// with the response-queued flag). Combinational outputs (gnt, id, kill_ack)
// are sampled pre-edge; registered outputs (holds, full, barrier_pend) are
// compared post-edge.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "VKL_pp_scoreboard.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// pp_pkg::pp_hazard_e encodings (F03.7 row order)
enum { RO = 0, CFG = 1, STR = 2, MAP = 3, CLK = 4, NAM = 5,
       LCK = 6, REG = 7, IDF = 8 };
static const char* CN[9] = {"RO", "CFG", "STR", "MAP", "CLK", "NAME",
                            "LOCK", "REG", "IDF"};

// conflict kinds
enum Kind { NV = 0, SK = 1, AL = 2 };  // never / same-key / always

// Independent literal transcription of F03.7 (docs/architecture/03 §6):
//  - CFG_BARRIER row/col: ALWAYS (global drain, itself included)
//  - RO row/col: NEVER vs RO, else SAMEKEY (blocked only vs same-key write)
//  - LOCK row/col: ALWAYS vs lock-protected members (CFG STR MAP CLK NAME
//    IDF, per F06.14) + itself; NEVER vs REG (not lock-protected)
//  - same class: REG, IDF single-resource (ALWAYS); STR MAP CLK NAME SAMEKEY
//  - STR x MAP: ALWAYS (format<->mapping cross-lock, class-wide)
//  - all other cross pairs: NEVER
static const Kind M9[9][9] = {
  //         RO   CFG  STR  MAP  CLK  NAM  LCK  REG  IDF
  /* RO  */ {NV,  AL,  SK,  SK,  SK,  SK,  SK,  SK,  SK},
  /* CFG */ {AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL,  AL},
  /* STR */ {SK,  AL,  SK,  AL,  NV,  NV,  AL,  NV,  NV},
  /* MAP */ {SK,  AL,  AL,  SK,  NV,  NV,  AL,  NV,  NV},
  /* CLK */ {SK,  AL,  NV,  NV,  SK,  NV,  AL,  NV,  NV},
  /* NAM */ {SK,  AL,  NV,  NV,  NV,  SK,  AL,  NV,  NV},
  /* LCK */ {SK,  AL,  AL,  AL,  AL,  AL,  AL,  NV,  AL},
  /* REG */ {SK,  AL,  NV,  NV,  NV,  NV,  NV,  AL,  NV},
  /* IDF */ {SK,  AL,  NV,  NV,  NV,  NV,  AL,  NV,  AL},
};

static const int MAXH = 8;  // must match MAX_HOLDS_P

struct Model {
  bool v[MAXH] = {};
  int  c[MAXH] = {}, k[MAXH] = {};
  bool pend = false;

  static bool conflict(int ac, int ak, int bc, int bk) {
    Kind kk = M9[ac][bc];
    if (kk == AL) return true;
    if (kk == SK) return ak == bk;
    return false;
  }
  bool fullT() const { for (bool b : v) if (!b) return false; return true; }
  bool admissible(int cls, int key) const {
    if (fullT()) return false;
    if (pend && cls != CFG) return false;
    for (int i = 0; i < MAXH; i++)
      if (v[i] && conflict(cls, key, c[i], k[i])) return false;
    return true;
  }
  uint32_t mask() const {
    uint32_t m = 0;
    for (int i = 0; i < MAXH; i++) if (v[i]) m |= 1u << i;
    return m;
  }
};

struct Harness {
  VKL_pp_scoreboard* d;
  Model m;

  explicit Harness(VKL_pp_scoreboard* dut) : d(dut) { idle(); }

  void idle() {
    d->adm_req_i = 0; d->rel_valid_i = 0;
    d->kill_valid_i = 0; d->kill_resp_queued_i = 0;
  }
  void settle() { d->clk_i = 0; d->eval(); }
  void edge()   { d->clk_i = 1; d->eval(); }
  void cycle()  { settle(); edge(); }

  // One request cycle. Checks gnt against the model and id uniqueness;
  // returns the granted id or -1. Model bookkeeping included.
  int request(int cls, int key, const char* tag) {
    bool exp = m.admissible(cls, key);
    d->adm_req_i = 1; d->adm_class_i = cls; d->adm_key_i = key;
    settle();
    bool g = d->adm_gnt_o;
    int  id = d->adm_id_o;
    CHECK(g == exp, "%s: req %s key=%d gnt=%d expected=%d",
          tag, CN[cls], key, (int)g, (int)exp);
    if (g) {
      bool ok_id = (id >= 0 && id < MAXH && !m.v[id]);
      CHECK(ok_id, "%s: grant id %d out of range or duplicated", tag, id);
      if (id >= 0 && id < MAXH) { m.v[id] = true; m.c[id] = cls; m.k[id] = key; }
      if (cls == CFG) m.pend = false;
    } else if (cls == CFG) {
      m.pend = true;  // refused barrier latches the drain
    }
    edge();
    idle();
    return g ? id : -1;
  }

  void release(int id) {
    d->rel_valid_i = 1; d->rel_id_i = id;
    cycle();
    idle();
    if (id >= 0 && id < MAXH) m.v[id] = false;
  }

  bool kill(int id, bool respq, const char* tag) {
    bool exp = respq && id >= 0 && id < MAXH && m.v[id];
    d->kill_valid_i = 1; d->kill_id_i = id; d->kill_resp_queued_i = respq;
    settle();
    bool a = d->kill_ack_o;
    CHECK(a == exp, "%s: kill id=%d respq=%d ack=%d expected=%d",
          tag, id, (int)respq, (int)a, (int)exp);
    edge();
    idle();
    if (a && id >= 0 && id < MAXH) m.v[id] = false;
    return a;
  }

  void check_state(const char* tag) {
    CHECK((uint32_t)d->holds_o == m.mask(), "%s: holds %02x expected %02x",
          tag, (unsigned)d->holds_o, (unsigned)m.mask());
    CHECK((bool)d->full_o == m.fullT(), "%s: full_o mismatch", tag);
    CHECK((bool)d->barrier_pend_o == m.pend, "%s: barrier_pend mismatch", tag);
  }

  // Release everything; clear a latched barrier drain by granting it.
  void drain(const char* tag) {
    for (int i = 0; i < MAXH; i++) if (m.v[i]) release(i);
    if (m.pend) {
      int id = request(CFG, 0, tag);
      if (id >= 0) release(id);
    }
  }
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_pp_scoreboard;
  Harness h(dut);

  dut->rst_n = 0;
  for (int i = 0; i < 4; ++i) h.cycle();
  dut->rst_n = 1;
  h.cycle();

  // ---- T0: reset state -------------------------------------------------
  CHECK(dut->holds_o == 0, "T0 no holds after reset");
  CHECK(dut->full_o == 0, "T0 not full after reset");
  CHECK(dut->barrier_pend_o == 0, "T0 no barrier pending after reset");

  // ---- T1: same-key serializes; release reopens ------------------------
  {
    int a = h.request(STR, 5, "T1 first");
    CHECK(a >= 0, "T1 first STR key5 granted");
    int b = h.request(STR, 5, "T1 second");           // model: refused
    CHECK(b < 0, "T1 same-key refused while held");
    h.release(a);
    int c = h.request(STR, 5, "T1 after release");    // model: granted
    CHECK(c >= 0, "T1 granted after release");
    h.release(c);
    h.check_state("T1 end");
  }

  // ---- T2: distinct keys parallel; RO parallel on the SAME key ---------
  {
    int a = h.request(STR, 1, "T2 k1");
    int b = h.request(STR, 2, "T2 k2");
    CHECK(a >= 0 && b >= 0 && a != b, "T2 distinct keys both granted, ids differ");
    int r1 = h.request(RO, 9, "T2 ro1");
    int r2 = h.request(RO, 9, "T2 ro2");               // same key: still parallel
    CHECK(r1 >= 0 && r2 >= 0, "T2 RO same-key admits concurrently");
    h.release(a); h.release(b); h.release(r1); h.release(r2);
    h.check_state("T2 end");
  }

  // ---- T3: systematic 9x9 x {same,diff} sweep vs the literal matrix ----
  for (int a = 0; a < 9; a++) {
    for (int b = 0; b < 9; b++) {
      for (int same = 0; same < 2; same++) {
        char tag[64];
        snprintf(tag, sizeof tag, "T3 %s->%s %s",
                 CN[a], CN[b], same ? "same-key" : "diff-key");
        int ka = 0x42, kb = same ? 0x42 : 0x41;
        int ida = h.request(a, ka, tag);
        CHECK(ida >= 0, "%s: A admitted on empty table", tag);
        int idb = h.request(b, kb, tag);   // expectation checked inside
        if (idb >= 0) h.release(idb);
        if (ida >= 0) h.release(ida);
        h.drain(tag);
        CHECK(dut->holds_o == 0, "%s: drained", tag);
      }
    }
  }

  // ---- T4: rule (e) — kill refused before response-queued --------------
  {
    int a = h.request(STR, 7, "T4 acquire");
    CHECK(a >= 0, "T4 STR key7 granted");
    bool ack0 = h.kill(a, false, "T4 kill-early");     // model: refused
    CHECK(!ack0, "T4 kill refused before response queued");
    int b = h.request(STR, 7, "T4 still-held");        // model: refused
    CHECK(b < 0, "T4 key still serialized after refused kill");
    bool ack1 = h.kill(a, true, "T4 kill-queued");     // model: honored
    CHECK(ack1, "T4 kill honored once response queued");
    int c = h.request(STR, 7, "T4 freed");
    CHECK(c >= 0, "T4 key free after honored kill");
    h.release(c);
    bool ack2 = h.kill(a, true, "T4 kill-stale");      // id no longer held
    CHECK(!ack2, "T4 kill of a non-held id refused");
    h.check_state("T4 end");
  }

  // ---- T5: grant/release ordering ---------------------------------------
  {
    int a = h.request(STR, 3, "T5 acquire");
    CHECK(a >= 0, "T5 granted");
    // same-cycle release + conflicting request: grant sees PRE-release state
    dut->rel_valid_i = 1; dut->rel_id_i = a;
    dut->adm_req_i = 1; dut->adm_class_i = STR; dut->adm_key_i = 3;
    h.settle();
    CHECK(dut->adm_gnt_o == 0, "T5 same-cycle release does not admit");
    h.edge();
    h.idle();
    h.m.v[a] = false;                       // release landed on the edge
    int b = h.request(STR, 3, "T5 next-cycle");
    CHECK(b >= 0, "T5 admitted the cycle after the release");
    // release of a non-held id is a no-op (MAXH > 1, so (b+1)%MAXH != b)
    h.release((b + 1) % MAXH);
    h.check_state("T5 no-op release");
    h.release(b);
    h.check_state("T5 end");
  }

  // ---- T6: holds-table full ---------------------------------------------
  {
    int ids[MAXH];
    for (int i = 0; i < MAXH; i++) {
      ids[i] = h.request(RO, 100 + i, "T6 fill");      // RO: all parallel
      CHECK(ids[i] >= 0, "T6 fill %d granted", i);
    }
    CHECK(dut->full_o == 1, "T6 table full");
    int x = h.request(RO, 999, "T6 overflow");         // no conflict, no room
    CHECK(x < 0, "T6 refused when full despite no conflict");
    h.release(ids[0]);
    CHECK(dut->full_o == 0, "T6 not full after a release");
    int y = h.request(RO, 999, "T6 after-release");
    CHECK(y >= 0, "T6 granted after a release");
    h.release(y);
    for (int i = 1; i < MAXH; i++) h.release(ids[i]);
    h.check_state("T6 end");
  }

  // ---- T7: CFG_BARRIER drain --------------------------------------------
  {
    int s = h.request(STR, 1, "T7 str");
    int c = h.request(CLK, 2, "T7 clk");
    CHECK(s >= 0 && c >= 0, "T7 two writers in flight");
    int b0 = h.request(CFG, 0, "T7 barrier-1");        // refused, latches pend
    CHECK(b0 < 0, "T7 barrier refused while holders live");
    CHECK(dut->barrier_pend_o == 1, "T7 drain latched");
    int n = h.request(NAM, 5, "T7 blocked");           // admissible w/o pend
    CHECK(n < 0, "T7 non-barrier admission blocked during drain");
    h.release(s);
    int b1 = h.request(CFG, 0, "T7 barrier-2");        // one holder left
    CHECK(b1 < 0, "T7 barrier still refused with one holder");
    h.release(c);
    int b2 = h.request(CFG, 0, "T7 barrier-3");        // table empty
    CHECK(b2 >= 0, "T7 barrier granted on empty table");
    CHECK(dut->barrier_pend_o == 0, "T7 drain cleared by the grant");
    int r = h.request(RO, 0x33, "T7 excluded");        // barrier held
    CHECK(r < 0, "T7 everything excluded while barrier held");
    h.release(b2);
    int r2 = h.request(RO, 0x33, "T7 resumed");
    CHECK(r2 >= 0, "T7 normal admission after barrier release");
    h.release(r2);
    h.check_state("T7 end");
  }

  // ---- T8: randomized traffic, >= 2k ops vs the model -------------------
  {
    srand(7);
    const int OPS = 2600;
    int reqs = 0, grants = 0, rels = 0, kills = 0;
    for (int op = 0; op < OPS; op++) {
      int r = rand() % 100;
      if (h.m.pend && r < 30) {
        int id = h.request(CFG, 0, "T8 cfg");
        ++reqs; if (id >= 0) ++grants;
      } else if (r < 55) {
        static const int wcls[12] = {RO, RO, RO, STR, STR, MAP, CLK,
                                     NAM, LCK, REG, IDF, CFG};
        int cls = wcls[rand() % 12];
        int key = rand() % 6;
        int id = h.request(cls, key, "T8 req");
        ++reqs; if (id >= 0) ++grants;
      } else if (r < 85) {
        // release a random active hold (or a random id if none: no-op)
        int live[MAXH], n = 0;
        for (int i = 0; i < MAXH; i++) if (h.m.v[i]) live[n++] = i;
        h.release(n ? live[rand() % n] : rand() % MAXH);
        ++rels;
      } else {
        int live[MAXH], n = 0;
        for (int i = 0; i < MAXH; i++) if (h.m.v[i]) live[n++] = i;
        int id = n ? live[rand() % n] : rand() % MAXH;
        h.kill(id, (rand() & 1) != 0, "T8 kill");
        ++kills;
      }
      if ((op & 63) == 0) h.check_state("T8 mask");
    }
    printf("T8: %d ops (%d req / %d granted, %d rel, %d kill)\n",
           OPS, reqs, grants, rels, kills);
    CHECK(reqs + rels + kills == OPS, "T8 op accounting");
    h.drain("T8 drain");
    h.check_state("T8 end");
    CHECK(dut->holds_o == 0, "T8 all holds returned");
  }

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
