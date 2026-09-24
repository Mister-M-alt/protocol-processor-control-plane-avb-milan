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
uint64_t slope(unsigned frame, unsigned intervals) {
  return (std::max(frame + 22u, 68u) + 20ull) * intervals * 64000ull;
}
class Suite {
  milan::tb::Model<Vsrp_admission_wrap> model;
  Vsrp_admission_wrap* d = model.get();
  std::array<unsigned, 8> frame{}, intervals{}, age{};
  unsigned requests = 0;
  unsigned checks = 0, fails = 0;
  void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++fails; std::printf("FAIL: %s N=%u\n", name, N); }
  }
  void tick() {
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
    if (d->round_o) {
      uint64_t sum = 0;
      for (unsigned s = 0; s < N; ++s) sum += d->granted_o[s];
      check(sum == d->sum_o && sum <= LIMIT, "round publishes coherent bounded sum");
    }
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
    uint64_t sum = 0;
    unsigned grants = 0;
    bool over = false;
    for (unsigned s = 0; s < N; ++s) {
      if (!(requests & (1u << s))) continue;
      auto sl = slope(frame[s], intervals[s]);
      if (sum + sl <= LIMIT) { grants |= 1u << s; sum += sl; }
      else over = true;
    }
    check(d->admitted_o == grants, "settled greedy grants");
    check(d->sum_o == sum, "settled sum of grants");
    check(bool(d->over_o) == over, "settled ceiling refusal");
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
