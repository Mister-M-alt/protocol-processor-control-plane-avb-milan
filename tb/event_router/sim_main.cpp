// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_event_router suite — independent expectations, never DUT logic.
//
// The C++ model independently implements the documented policy (02 §5
// class-C sticky events + the 03 §2/§5 router): per-source sticky latch
// holding the FIRST payload until ack, re-fire before ack = coalesced flag
// + one saturating lost tick, ack+re-fire on the same cycle = a NEW event
// (nothing lost), and a round-robin presentation whose base advances past
// each delivered source. Every cycle the DUT outputs are compared against
// the model pre-edge (lockstep), every delivery is matched, the lost-count
// read port is spot-read on a rotating index every cycle, and the phase-J
// conservation law strobes = delivered + lost + pending is closed per
// source — the "never silently dropped" contract as arithmetic.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "VKL_pp_event_router.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// shipping shape — must match the DUT parameter defaults (no -G overrides)
enum { N = 16, PW = 16 };

struct Delivery { int src; uint16_t payload; bool lost; };

// ------------------------------------------------------------------ model
struct Model {
  bool     pending[N]   = {};
  bool     coalesced[N] = {};
  uint16_t payload[N]   = {};
  uint8_t  lost_sat[N]  = {};   // what the DUT counter must read
  uint64_t lost_true[N] = {};   // unbounded, for the conservation law
  uint64_t strobes[N]   = {};
  uint64_t delivered[N] = {};
  bool     valid = false;
  int      sel = 0, ptr = 0;

  bool any_pending() const {
    for (int s = 0; s < N; ++s) if (pending[s]) return true;
    return false;
  }

  // one clock edge: this cycle's strobes/payloads/ack, pre-edge state read
  void step(uint32_t strobe_mask, const uint16_t* pl, bool ack) {
    const bool deliver = valid && ack;
    const int  dsrc    = deliver ? sel : -1;

    // grant from the registered pending set minus the delivered event
    bool pa[N];
    for (int s = 0; s < N; ++s) pa[s] = pending[s];
    if (deliver) pa[dsrc] = false;
    const int base = deliver ? (sel + 1) % N : ptr;
    int gnt = -1;
    for (int k = 0; k < N; ++k) {
      const int idx = (base + k) % N;
      if (pa[idx]) { gnt = idx; break; }
    }

    // per-source sticky latch
    for (int s = 0; s < N; ++s) {
      if (strobe_mask & (1u << s)) {
        ++strobes[s];
        if (!pending[s] || s == dsrc) {          // a NEW event
          pending[s] = true; payload[s] = pl[s]; coalesced[s] = false;
        } else {                                 // re-fire before ack
          coalesced[s] = true;
          ++lost_true[s];
          if (lost_sat[s] != 0xFF) ++lost_sat[s];
        }
      } else if (s == dsrc) {
        pending[s] = false; coalesced[s] = false;
      }
    }

    if (deliver) { ++delivered[dsrc]; ptr = (dsrc + 1) % N; }
    if (deliver || !valid) { valid = (gnt >= 0); if (gnt >= 0) sel = gnt; }
  }
};

// ---------------------------------------------------------------- harness
struct Harness {
  VKL_pp_event_router* dut;
  Model    m;
  uint64_t cycle = 0;
  uint16_t pl[N] = {};
  uint64_t out_mm = 0, lost_rd_mm = 0;
  uint64_t first_mm_cycle = 0;
  std::string first_mm_what;
  std::vector<Delivery> dl;

  explicit Harness(VKL_pp_event_router* d) : dut(d) {}

  void mm(const char* what) {
    if (!out_mm && !lost_rd_mm) { first_mm_cycle = cycle; first_mm_what = what; }
  }

  void set_pl(int s, uint16_t v) {
    pl[s] = v;
    const int w = (s * PW) / 32, sh = (s * PW) % 32;
    const uint32_t msk = 0xFFFFu << sh;
    dut->src_payload_i[w] = (dut->src_payload_i[w] & ~msk)
                          | (uint32_t(v) << sh);
  }

  void tick(uint32_t strobe_mask, bool ack) {
    dut->src_strobe_i = (uint16_t)strobe_mask;
    dut->evt_ack_i    = ack;
    const int ls = int(cycle % N);
    dut->lost_src_i = (uint8_t)ls;

    // settle combinational logic with this cycle's inputs
    dut->clk_i = 0; dut->eval();

    // observe PRE-EDGE: registered outputs vs the model's registered state
    if ((bool)dut->evt_valid_o != m.valid) { mm("valid"); ++out_mm; }
    if (m.valid && dut->evt_valid_o) {
      if ((int)dut->evt_src_o != m.sel)                 { mm("src");     ++out_mm; }
      if ((uint16_t)dut->evt_payload_o != m.payload[m.sel]) { mm("payload"); ++out_mm; }
      if ((bool)dut->evt_lost_o != m.coalesced[m.sel])  { mm("lost");    ++out_mm; }
    } else if (!m.valid && dut->evt_lost_o) { mm("lost@idle"); ++out_mm; }
    if ((uint8_t)dut->lost_count_o != m.lost_sat[ls]) { mm("lost_rd"); ++lost_rd_mm; }

    if (m.valid && ack) dl.push_back({m.sel, m.payload[m.sel], m.coalesced[m.sel]});

    m.step(strobe_mask, pl, ack);

    // rising edge: registers update
    dut->clk_i = 1; dut->eval();
    ++cycle;
  }

  // combinational read of one lost counter (no clock edge)
  uint8_t read_lost(int s) {
    dut->lost_src_i = (uint8_t)s;
    dut->clk_i = 0; dut->eval();
    return dut->lost_count_o;
  }

  // ack until the router is empty (bounded)
  int drain() {
    int n = 0;
    while ((m.valid || m.any_pending()) && n < 100) { tick(0, true); ++n; }
    return n;
  }
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_pp_event_router;
  Harness h(dut);

  // ---- reset ----------------------------------------------------------
  dut->rst_n = 0; dut->src_strobe_i = 0; dut->evt_ack_i = 0;
  dut->lost_src_i = 0;
  for (int w = 0; w < (N * PW) / 32; ++w) dut->src_payload_i[w] = 0;
  for (int i = 0; i < 4; ++i) { dut->clk_i = 0; dut->eval(); dut->clk_i = 1; dut->eval(); }
  dut->rst_n = 1;

  CHECK(dut->evt_valid_o == 0, "R1 idle after reset");
  {
    bool allz = true;
    for (int s = 0; s < N; ++s) allz &= (h.read_lost(s) == 0);
    CHECK(allz, "R2 all lost counters zero after reset");
  }

  // ---- A: single event delivered exactly once -------------------------
  h.set_pl(3, 0xABCD);
  h.tick(1u << 3, false);
  h.tick(0, false);
  CHECK(dut->evt_valid_o == 1, "A1 valid two cycles after the strobe");
  CHECK(dut->evt_src_o == 3, "A2 src 3 got %d", (int)dut->evt_src_o);
  CHECK(dut->evt_payload_o == 0xABCD, "A3 payload got %04x", (int)dut->evt_payload_o);
  CHECK(dut->evt_lost_o == 0, "A4 no loss flag");
  {
    bool sticky = true;
    for (int i = 0; i < 5; ++i) {
      h.tick(0, false);
      sticky &= dut->evt_valid_o && dut->evt_src_o == 3
             && dut->evt_payload_o == 0xABCD;
    }
    CHECK(sticky, "A5 event + payload held over 5 un-acked cycles");
  }
  h.tick(0, true);
  CHECK(dut->evt_valid_o == 0, "A6 valid drops after ack");
  CHECK(h.dl.size() == 1 && h.dl[0].src == 3 && h.dl[0].payload == 0xABCD
        && !h.dl[0].lost, "A7 delivered exactly once, clean");
  {
    for (int i = 0; i < 8; ++i) h.tick(0, false);
    CHECK(h.dl.size() == 1 && dut->evt_valid_o == 0, "A8 no re-delivery");
  }
  CHECK(h.read_lost(3) == 0, "A9 nothing lost");

  // ---- B: re-strobe before ack — FIRST payload holds, counter ticks ---
  h.set_pl(5, 0xAAAA);
  h.tick(1u << 5, false);
  h.tick(0, false);
  h.set_pl(5, 0xBBBB);
  h.tick(1u << 5, false);
  CHECK(dut->evt_payload_o == 0xAAAA, "B1 payload stays the FIRST got %04x",
        (int)dut->evt_payload_o);
  CHECK(dut->evt_lost_o == 1, "B2 evt_lost set on coalesce");
  CHECK(h.read_lost(5) == 1, "B3 lost count 1 got %d", h.read_lost(5));
  h.set_pl(5, 0xCCCC);
  h.tick(1u << 5, false);
  CHECK(h.read_lost(5) == 2, "B4 lost count 2 got %d", h.read_lost(5));
  CHECK(dut->evt_payload_o == 0xAAAA, "B5 payload still the FIRST");
  h.tick(0, true);
  CHECK(h.dl.back().src == 5 && h.dl.back().payload == 0xAAAA
        && h.dl.back().lost, "B6 delivery carries FIRST payload + lost flag");
  h.set_pl(5, 0xDDDD);
  h.tick(1u << 5, false);
  h.tick(0, false);
  CHECK(dut->evt_valid_o && dut->evt_src_o == 5
        && dut->evt_payload_o == 0xDDDD, "B7 next event carries NEW payload");
  CHECK(dut->evt_lost_o == 0, "B8 fresh event has no loss flag");
  CHECK(h.read_lost(5) == 2, "B9 counter cumulative, not cleared by ack");
  h.drain();

  // ---- C: coalesce on a queued-but-not-presented source ---------------
  h.set_pl(1, 0x1111); h.set_pl(2, 0x2222);
  h.tick((1u << 1) | (1u << 2), false);
  h.tick(0, false);
  CHECK(dut->evt_valid_o && dut->evt_src_o == 1, "C1 src 1 presented first");
  h.set_pl(2, 0x2299);
  h.tick(1u << 2, false);
  CHECK(h.read_lost(2) == 1, "C2 queued source coalesces too");
  h.tick(0, true);                      // delivers 1, loads 2
  h.tick(0, true);                      // delivers 2
  CHECK(h.dl.back().src == 2 && h.dl.back().payload == 0x2222
        && h.dl.back().lost, "C3 queued source delivers FIRST payload + flag");

  // ---- D: ack + strobe on the same cycle = a NEW event, nothing lost --
  h.set_pl(9, 0x9001);
  h.tick(1u << 9, false);
  h.tick(0, false);
  h.set_pl(9, 0x9002);
  h.tick(1u << 9, true);                // ack the old, latch the new
  CHECK(h.dl.back().src == 9 && h.dl.back().payload == 0x9001
        && !h.dl.back().lost, "D1 ack cycle delivers the OLD event");
  CHECK(dut->evt_valid_o == 0, "D2 one-cycle bubble through empty");
  h.tick(0, false);
  CHECK(dut->evt_valid_o && dut->evt_src_o == 9
        && dut->evt_payload_o == 0x9002 && dut->evt_lost_o == 0,
        "D3 NEW event presents with the new payload, clean");
  CHECK(h.read_lost(9) == 0, "D4 no lost tick on the ack+strobe race");
  h.drain();

  // ---- E: N simultaneous — all delivered exactly once, RR order -------
  {
    for (int s = 0; s < N; ++s) h.set_pl(s, (uint16_t)(0xE000 | s));
    const int p0 = h.m.ptr;
    const size_t d0 = h.dl.size();
    h.tick(0xFFFF, false);
    const int cyc = h.drain();
    CHECK(h.dl.size() - d0 == N, "E1 all %d delivered got %zu", N,
          h.dl.size() - d0);
    CHECK(cyc == N + 1, "E2 one event per cycle (%d cycles for %d + load)",
          cyc, N);
    bool order = true, pay = true, clean = true;
    bool once[N] = {};
    for (size_t i = d0; i < h.dl.size(); ++i) {
      const int exp = (p0 + int(i - d0)) % N;
      order &= (h.dl[i].src == exp);
      pay   &= (h.dl[i].payload == (0xE000 | h.dl[i].src));
      clean &= !h.dl[i].lost;
      if (h.dl[i].src >= 0 && h.dl[i].src < N) once[h.dl[i].src] = true;
    }
    bool all = true;
    for (int s = 0; s < N; ++s) all &= once[s];
    CHECK(order, "E3 round-robin order from the pointer");
    CHECK(all, "E4 every source delivered");
    CHECK(pay, "E5 every payload matches its source");
    CHECK(clean, "E6 no loss flags on a single burst");
  }

  // ---- F: RR pointer wrap, directed -----------------------------------
  h.set_pl(11, 0x0B0B);
  h.tick(1u << 11, false); h.tick(0, false); h.tick(0, true);  // ptr -> 12
  {
    const size_t d0 = h.dl.size();
    h.set_pl(1, 0x0101); h.set_pl(13, 0x0D0D);
    h.tick((1u << 1) | (1u << 13), false);
    h.drain();
    CHECK(h.dl.size() - d0 == 2 && h.dl[d0].src == 13
          && h.dl[d0 + 1].src == 1,
          "F1 wrap order 13 then 1 (ptr at 12) got %d,%d",
          h.dl.size() - d0 > 0 ? h.dl[d0].src : -1,
          h.dl.size() - d0 > 1 ? h.dl[d0 + 1].src : -1);
  }

  // ---- H: lost counter exact then saturating (fresh source 4) ---------
  CHECK(h.read_lost(4) == 0, "H0 source 4 pristine");
  h.set_pl(4, 0x4441);
  h.tick(1u << 4, false);               // the event
  for (int i = 0; i < 100; ++i) h.tick(1u << 4, false);
  CHECK(h.read_lost(4) == 100, "H1 exact count got %d", h.read_lost(4));
  for (int i = 0; i < 155; ++i) h.tick(1u << 4, false);
  CHECK(h.read_lost(4) == 0xFF, "H2 saturates at 255 got %d", h.read_lost(4));
  for (int i = 0; i < 60; ++i) h.tick(1u << 4, false);
  CHECK(h.read_lost(4) == 0xFF, "H3 315 re-fires: pegged, no wrap got %d",
        h.read_lost(4));
  h.tick(0, true);
  CHECK(h.dl.back().src == 4 && h.dl.back().payload == 0x4441
        && h.dl.back().lost, "H4 still delivers the FIRST payload");

  // ---- I: ack while idle is a no-op -----------------------------------
  {
    const size_t d0 = h.dl.size();
    for (int i = 0; i < 3; ++i) h.tick(0, true);
    CHECK(dut->evt_valid_o == 0 && h.dl.size() == d0, "I1 idle ack ignored");
    h.set_pl(7, 0x0777);
    h.tick(1u << 7, false); h.tick(0, false);
    CHECK(dut->evt_valid_o && dut->evt_src_o == 7, "I2 router alive after");
    h.drain();
  }

  // ---- G: full saturation — one delivery per cycle, rotating ----------
  {
    for (int s = 0; s < N; ++s) h.set_pl(s, (uint16_t)(0x7000 | s));
    const size_t d0 = h.dl.size();
    for (int i = 0; i < 80; ++i) h.tick(0xFFFF, true);
    const size_t nd = h.dl.size() - d0;
    CHECK(nd >= 78, "G1 %zu deliveries in 80 saturated cycles", nd);
    bool rot = true;
    for (size_t i = d0 + 1; i < h.dl.size(); ++i)
      rot &= (h.dl[i].src == (h.dl[i - 1].src + 1) % N);
    CHECK(rot, "G2 saturated order rotates by one every cycle");
    h.tick(0, true); h.drain();
    bool sat_ok = true;
    for (int s = 0; s < N; ++s) sat_ok &= (h.read_lost(s) == h.m.lost_sat[s]);
    CHECK(sat_ok, "G3 all 16 climbing counters read back exactly");
  }

  // ---- J: randomized soak, >= 2000 deliveries, model lockstep ---------
  {
    const size_t d0 = h.dl.size();
    uint32_t rng = 0xC0FFEE01u;
    auto rnd = [&rng]() {
      rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
    };
    uint64_t cycles = 0;
    while (h.dl.size() - d0 < 2200 && cycles < 60000) {
      uint32_t mask = 0;
      for (int s = 0; s < N; ++s) {
        if ((rnd() & 7u) == 0) {               // ~1/8 strobe probability
          mask |= 1u << s;
          h.set_pl(s, (uint16_t)rnd());
        }
      }
      const bool ack = (rnd() & 3u) != 0;      // ~3/4 ack probability
      h.tick(mask, ack);
      ++cycles;
    }
    h.drain();
    CHECK(h.dl.size() - d0 >= 2200, "J1 %zu random deliveries",
          h.dl.size() - d0);
    CHECK(h.out_mm == 0,
          "J2 per-cycle lockstep clean (%llu mismatches, first '%s' @%llu)",
          (unsigned long long)h.out_mm, h.first_mm_what.c_str(),
          (unsigned long long)h.first_mm_cycle);
    CHECK(h.lost_rd_mm == 0,
          "J3 rotating lost-port reads clean (%llu mismatches)",
          (unsigned long long)h.lost_rd_mm);
    bool every = true;
    for (int s = 0; s < N; ++s) every &= (h.m.delivered[s] > 0);
    CHECK(every, "J4 no source starved");
    CHECK(!h.m.valid && !h.m.any_pending(), "J5 fully drained");
    // conservation: every strobe is a delivery or a counted loss — the
    // "never silently dropped" contract, closed per source
    for (int s = 0; s < N; ++s) {
      CHECK(h.m.strobes[s] == h.m.delivered[s] + h.m.lost_true[s],
            "J6.%d conservation: %llu strobes = %llu delivered + %llu lost",
            s, (unsigned long long)h.m.strobes[s],
            (unsigned long long)h.m.delivered[s],
            (unsigned long long)h.m.lost_true[s]);
    }
    for (int s = 0; s < N; ++s) {
      CHECK(h.read_lost(s) == h.m.lost_sat[s],
            "J7.%d final counter got %d want %d", s, h.read_lost(s),
            (int)h.m.lost_sat[s]);
    }
  }

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
