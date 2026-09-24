// SPDX-License-Identifier: CERN-OHL-W-2.0
// Fresh declaration provenance and the Milan 4.3.3.2 bandwidth recipe.
// The oracle uses current declarations and exact 64-bit arithmetic; it has
// no copy of the DUT's pipeline, validity bits, or saturating arithmetic.
#include "Vsrp_admission_wrap.h"
#include "../common/verilator_harness.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

namespace {
constexpr unsigned N = TB_SOURCES;
constexpr unsigned SETTLE = 3 * N + 4;
constexpr unsigned MAX_LATENCY = N == 1 ? 4 : 3 * N;
constexpr uint64_t LIMIT = 750000000;
// One-interval frames whose class-A slopes compete for the 750 Mb/s ceiling.
constexpr unsigned F700 = 10895;  // 699.968 Mb/s
constexpr unsigned F643 = 10000;  // 642.688 Mb/s
constexpr unsigned F732 = 11395;  // 731.968 Mb/s
constexpr unsigned F400 = 6208;   // 400.000 Mb/s
constexpr unsigned F300 = 4645;   // 299.968 Mb/s
constexpr unsigned F131 = 2000;   // 130.688 Mb/s
constexpr unsigned F100 = 1520;   //  99.968 Mb/s
uint64_t slope(unsigned frame, unsigned intervals) {
  return (std::max(frame + 22u, 68u) + 20ull) * intervals * 64000ull;
}
//! The greedy source-order walk over the current declarations.
struct Greedy {
  unsigned grants = 0;
  uint64_t sum = 0;
  bool over = false;
};
class Suite {
  milan::tb::Model<Vsrp_admission_wrap> model;
  Vsrp_admission_wrap* d = model.get();
  std::array<unsigned, 8> frame{};
  std::array<unsigned, 8> intervals{};
  std::array<unsigned, 8> age{};
  unsigned requests = 0;
  unsigned changed = 0;         // sources whose declaration this edge captures
  unsigned refused_watch = 0;   // sources that must not grant on any clock
  unsigned checks = 0;
  unsigned fails = 0;
  void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++fails; std::printf("FAIL: %s N=%u\n", name, N); }
  }
  Greedy greedy() const {
    Greedy g;
    for (unsigned s = 0; s < N; ++s) {
      if (!(requests & (1u << s))) continue;
      auto sl = slope(frame[s], intervals[s]);
      if (g.sum + sl <= LIMIT) { g.grants |= 1u << s; g.sum += sl; }
      else g.over = true;
    }
    return g;
  }
  void tick() {
    const unsigned grants_before = d->admitted_o;
    const uint32_t sum_before = d->sum_o;
    const bool over_before = d->over_o;
    std::array<uint32_t, 8> slopes_before{};
    for (unsigned s = 0; s < N; ++s) slopes_before[s] = d->granted_o[s];
    d->clk_i = 0; d->eval();
    d->clk_i = 1; d->eval();
    for (unsigned s = 0; s < N; ++s) {
      ++age[s];
      bool grant = d->admitted_o & (1u << s);
      check(!grant || ((requests & (1u << s)) && age[s] >= 4 &&
            d->granted_o[s] == slope(frame[s], intervals[s])),
            "grant belongs to the current declaration after pipeline evaluation");
      check(grant || d->granted_o[s] == 0, "unadmitted slope is zero");
      check(!grant || slope(frame[s], intervals[s]) <= LIMIT,
            "refused current TSpec never pulses a grant");
    }
    check(!(d->admitted_o & refused_watch),
          "refused source never granted while a lower source re-declares");
    if (d->round_o) {
      uint64_t sum = 0;
      for (unsigned s = 0; s < N; ++s) sum += d->granted_o[s];
      check(sum == d->sum_o && sum <= LIMIT, "round publishes coherent bounded sum");
    }
    if (d->rst_n) check_sigma_context(grants_before, slopes_before, sum_before, over_before);
    changed = 0;
  }
  // The grant judged in its Σ context on every clock. A publication is the
  // greedy walk over EVERY current declaration; between publications a grant
  // may only retire with its own declaration, and the aggregate holds.
  void check_sigma_context(unsigned grants_before,
                           const std::array<uint32_t, 8>& slopes_before,
                           uint32_t sum_before, bool over_before) {
    uint64_t live = 0;
    for (unsigned s = 0; s < N; ++s) live += d->granted_o[s];
    check(live <= LIMIT, "live granted sum stays within the ceiling");
    if (d->round_o) {
      const Greedy g = greedy();
      bool slopes_ok = true;
      for (unsigned s = 0; s < N; ++s) {
        uint64_t want = (g.grants & (1u << s)) ? slope(frame[s], intervals[s]) : 0;
        slopes_ok &= d->granted_o[s] == want;
      }
      check(d->admitted_o == g.grants && slopes_ok && d->sum_o == g.sum &&
            bool(d->over_o) == g.over,
            "round publishes the greedy walk over every current declaration");
      return;
    }
    bool frozen = d->admitted_o == (grants_before & ~changed);
    for (unsigned s = 0; s < N; ++s) {
      if (!(changed & (1u << s))) frozen &= d->granted_o[s] == slopes_before[s];
    }
    check(frozen, "between publications a grant only retires with its own declaration");
    check(d->sum_o == sum_before && bool(d->over_o) == over_before,
          "aggregate holds between publications");
  }
  void idle(unsigned cycles) { while (cycles--) tick(); }
  void reset() {
    d->rst_n = 0; d->change_i = 0; d->req_i = 0;
    d->port_rate_bps_i = 1000000000;
    requests = 0; frame.fill(0); intervals.fill(0); age.fill(0);
    idle(4); d->rst_n = 1;
  }
  void change(unsigned s, bool req, unsigned mfs, unsigned mif) {
    const unsigned previous_grants = d->admitted_o;
    const uint32_t previous_sum = d->sum_o;
    const bool previous_over = d->over_o;
    const unsigned shift = 16 * (s % 2);
    d->max_frame_i[s / 2] = (d->max_frame_i[s / 2] & ~(0xffffu << shift))
                           | (mfs << shift);
    d->interval_frames_i[s / 2] = (d->interval_frames_i[s / 2] & ~(0xffffu << shift))
                                 | (mif << shift);
    requests = (requests & ~(1u << s)) | (unsigned(req) << s);
    frame[s] = mfs; intervals[s] = mif; age[s] = 0;
    d->req_i = requests; d->change_i = 1u << s;
    changed = 1u << s;
    tick(); d->change_i = 0;
    // Acceptance edge is age 0 for the latency printed below.
    age[s] = 0;
    check(!(d->admitted_o & (1u << s)), "declaration immediately retires its old grant");
    check((d->admitted_o & ~(1u << s)) == (previous_grants & ~(1u << s)),
          "unrelated published grants survive declaration acceptance");
    check(d->sum_o == previous_sum && bool(d->over_o) == previous_over && !d->round_o,
          "invalidation holds the previous aggregate and aborts the partial round");
  }
  void settled() {
    idle(SETTLE);
    const Greedy g = greedy();
    check(d->admitted_o == g.grants, "settled greedy grants");
    check(d->sum_o == g.sum, "settled sum of grants");
    check(bool(d->over_o) == g.over, "settled ceiling refusal");
  }
  void phase(unsigned p) {
    for (unsigned n = 0; d->sample_index_o != p && n < N; ++n) tick();
    check(d->sample_index_o == p, "source sampling phase reached");
  }
  void admit_latency(unsigned s, unsigned p, const char* kind) {
    int latency = -1;
    for (unsigned c = 1; c <= SETTLE; ++c) {
      tick();
      if (latency < 0 && (d->admitted_o & (1u << s))) {
        latency = c;
        check(d->round_o, "first grant is published at round completion");
      }
    }
    check(latency >= 4 && latency <= int(MAX_LATENCY), "admitted latency bound");
    std::printf("LATENCY N=%u source=%u phase=%u case=%s cycles=%d\n",
                N, s, p, kind, latency);
    settled();
  }
  // Source lo admitted and hi refused, before and after lo re-declares.
  void compete(unsigned lo, unsigned hi, unsigned p) {
    reset(); phase(p);
    change(lo, true, F700, 1); change(hi, true, F131, 1); settled();
    check((d->admitted_o & (1u << lo)) && !(d->admitted_o & (1u << hi)),
          "cross-source precondition: lower source admitted, higher refused");
    phase(p);
  }
  // Issue #112 round 2: a pending re-declaration keeps its capacity. The
  // refused higher source is watched on every clock through an identical,
  // a shrinking, a growing and a doubled re-declaration of the lower one.
  void pending_redeclaration_keeps_capacity(unsigned lo, unsigned hi, unsigned p) {
    struct Step { const char* kind; unsigned frame; bool twice; };
    for (const Step& step : {Step{"cross-identical", F700, false},
                             Step{"cross-shrink", F643, false},
                             Step{"cross-grow", F732, false},
                             Step{"cross-double", F700, true}}) {
      compete(lo, hi, p);
      refused_watch = 1u << hi;
      change(lo, true, step.frame, 1);
      if (step.twice) { idle(5); change(lo, true, step.frame, 1); }
      admit_latency(lo, p, step.kind);
      refused_watch = 0;
    }
  }
  // Capacity is still reused: an evaluated shrink or a withdrawal of lo
  // admits hi, and only in a published round that saw lo's new state.
  void released_capacity_admits(unsigned lo, unsigned hi, unsigned p) {
    for (bool withdraw : {false, true}) {
      compete(lo, hi, p);
      change(lo, !withdraw, 224, 1);
      int latency = -1;
      for (unsigned c = 1; c <= SETTLE; ++c) {
        tick();
        if (latency < 0 && (d->admitted_o & (1u << hi))) {
          latency = c;
          check(d->round_o && (withdraw || (d->admitted_o & (1u << lo))),
                "freed capacity admits the refused source in a published round");
        }
      }
      check(latency >= 1 && latency <= int(MAX_LATENCY), "freed capacity latency bound");
      settled();
    }
  }
  // A pending middle source frees nothing for a later one and leaves the
  // earlier grant in place.
  void pending_middle_keeps_capacity(unsigned p) {
    const unsigned mid = N / 2;
    const unsigned hi = N - 1;
    reset(); phase(p);
    change(0, true, F400, 1); change(mid, true, F300, 1); change(hi, true, F100, 1);
    settled();
    check(d->admitted_o == ((1u << 0) | (1u << mid)),
          "cross-source precondition: first two admitted, last refused");
    phase(p);
    refused_watch = 1u << hi;
    change(mid, true, F300, 1); admit_latency(mid, p, "cross-middle");
    refused_watch = 0;
  }
  void cross_source() {
    if constexpr (N >= 2) {
      for (unsigned p = 0; p < N; ++p) {
        pending_redeclaration_keeps_capacity(0, N - 1, p);
        released_capacity_admits(0, N - 1, p);
        if constexpr (N >= 3) pending_middle_keeps_capacity(p);
      }
    }
  }
 public:
  int run() {
    for (unsigned s = 0; s < N; ++s) {
      for (unsigned p = 0; p < N; ++p) {
        reset(); phase(p);
        change(s, true, 20000, 1); settled(); // cold first declaration
        phase(p); change(s, true, 224, 1); admit_latency(s, p, "shrink");
        phase(p); change(s, true, 20000, 1); settled(); // grow, no withdrawal
        change(s, false, 20000, 1); // less than a round withdrawn
        change(s, true, 224, 1); admit_latency(s, p, "short-withdraw");
        phase(p); change(s, true, 224, 1); admit_latency(s, p, "identical");
        // Different values in BOTH fields at each stage, with repeated
        // redeclarations faster than the pipeline/round can drain.
        for (unsigned gap = 0; gap <= 3; ++gap) {
          change(s, true, 29, 1); idle(gap);
          change(s, true, 900, 100); settled();
        }
        change(s, true, 0xffff, 0xffff); settled(); // saturation refuses
        change(s, true, 29, 1); admit_latency(s, p, "saturation-shrink");
      }
    }
    cross_source();
    reset();
    for (unsigned s = 0; s < N; ++s) change(s, true, 4000, 1);
    settled(); // competition for the common ceiling
    for (unsigned s = 0; s < N; ++s) { change(s, false, 4000, 1); settled(); }
    std::printf("%u checks: %u PASS, %u FAIL\n", checks, checks - fails, fails);
    return fails ? 1 : 0;
  }
};
}
int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  Suite suite;
  return suite.run();
}
