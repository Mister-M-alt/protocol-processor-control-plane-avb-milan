// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_maap suite — the IEEE 1722-2016 Annex B state machine against
// independent expectations (frame bytes from Figure B.1 offsets, the walk
// from Table B.7, intervals from B.3.4, compare_MAC from B.3.6.4 — never
// DUT logic). The engine runs with its REAL services (PRNG, timer at 1 ms =
// 10 clk, TX slot pool); the C++ side plays the TX arbiter's lane and the
// talker's allocator face. What is graded:
//   cold start -> 4 byte-exact PROBEs (1 + MAAP_PROBE_RETRANSMITS) at
//   spacings measured inside the exclusive (500, 600) ms bounds -> the
//   first ANNOUNCE back-to-back with the fourth -> the (30, 32) s announce
//   cadence; the claim publication (addr/valid/state) at each step; the
//   full Table B.7 conflict matrix including both compare_MAC tie-breaks,
//   each driven with a MAC pair whose forward and reversed orders disagree;
//   DEFEND byte-exact with the B.3.6.6 overlap fields; yield ->
//   re-randomize -> per-source conflict fan-out (lowest first) -> re-probe;
//   the allocator seam contract (refuse fast while probing, grant base+s in
//   DEFEND, refuse s >= count, RELEASE acked); footnote-a seeding; the
//   engage/Release! and PortOperational! arcs, with no PDU generated after
//   a Release! in any walker state; maap_version tolerance and
//   reserved-message ignore; the Table B.9 fit clamp's reject-and-redraw arm
//   (a kind-7 stub in the wrapper scripts the overhanging draws) and the
//   footnote-a seed clamp.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <vector>
#include "Vmaap_wrap.h"
#include "verilated.h"
#include "../common/verilator_harness.hpp"

#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

typedef std::vector<uint8_t> Bytes;

// ---- Annex B constants (the standard, not the RTL) ------------------------
constexpr uint64_t MAAP_DA   = 0x91E0F000FF00ull;      // Table B.10
constexpr uint64_t POOL_HI   = 0x91E0F0000000ull;      // Table B.9 base
constexpr unsigned POOL_SIZE = 0xFE00;                 // Table B.9 extent
constexpr uint64_t POOL_LAST = POOL_HI + POOL_SIZE - 1; // 91:E0:F0:00:FD:FF
constexpr uint64_t OWN_MAC   = 0x02AABBCCDDEEull;
constexpr uint64_t EID       = 0x001BC5FFFE000042ull;
constexpr unsigned COUNT     = 8;
constexpr unsigned MAX_COUNT = 255;                    // the widest cfg_count_i
// compare_MAC operands whose FORWARD and octet-REVERSED orders against
// OWN_MAC disagree (B.3.6.4), so a forward compare decides every tie-break
// they take part in the wrong way:
//   WIN_MAC  forward-lower, reversed-higher: compare_MAC TRUE, we win
//   LOSE_MAC forward-higher, reversed-lower: compare_MAC FALSE, we lose
constexpr uint64_t WIN_MAC   = 0x0011223344FFull;
constexpr uint64_t LOSE_MAC  = 0xF21122334401ull;

// ---- harness scales -------------------------------------------------------
constexpr size_t kMinFrameBytes = 60;    // the 802.3 minimum frame, padded to
constexpr int kClkPerMs = 10;            // the compressed timer: 1 ms = 10 clk
constexpr int kProbeBudgetMs = 700;      // one B.3.4.2 probe interval + slack
constexpr int kAnnounceBudgetMs = 33000; // one B.3.4.1 announce interval + slack
constexpr int kWalkRetryRounds = 8;      // probe walks awaited before giving up
constexpr int kRxAcceptCycles = 200;     // guard on the txn_ready_o handshake
constexpr int kAllocWaitCycles = 50;     // guard on the allocator-face answer

struct Frame { Bytes b; uint32_t ms = 0; };

// Figure B.1 frame, 42 real bytes zero-padded to the 60-byte minimum
static Bytes maap_frame(uint64_t da, uint64_t sa, int msg,
                        uint64_t req_start, uint16_t req_cnt,
                        uint64_t con_start, uint16_t con_cnt) {
  Bytes f;
  for (int i = 5; i >= 0; --i) f.push_back(uint8_t(da >> (8 * i)));
  for (int i = 5; i >= 0; --i) f.push_back(uint8_t(sa >> (8 * i)));
  f.push_back(0x22); f.push_back(0xF0);
  f.push_back(0xFE);                       // subtype MAAP
  f.push_back(uint8_t(msg & 0x0F));        // sv 0, version 0, message_type
  f.push_back(0x08);                       // maap_version 1, cdl[10:8] 0
  f.push_back(0x10);                       // cdl 16 (B.2.1)
  for (int i = 0; i < 8; ++i) f.push_back(0x00);   // stream_id (B.2.4)
  for (int i = 5; i >= 0; --i) f.push_back(uint8_t(req_start >> (8 * i)));
  f.push_back(uint8_t(req_cnt >> 8)); f.push_back(uint8_t(req_cnt));
  for (int i = 5; i >= 0; --i) f.push_back(uint8_t(con_start >> (8 * i)));
  f.push_back(uint8_t(con_cnt >> 8)); f.push_back(uint8_t(con_cnt));
  while (f.size() < kMinFrameBytes) f.push_back(0x00);
  return f;
}

// B.3.6.4 octet-wise reversed compare: true = a lower than b
static bool rev_lower(uint64_t a, uint64_t b) {
  uint64_t ra = 0;
  uint64_t rb = 0;
  for (int i = 0; i < 6; ++i) {
    ra = (ra << 8) | ((a >> (8 * i)) & 0xFF);
    rb = (rb << 8) | ((b >> (8 * i)) & 0xFF);
  }
  return ra < rb;
}

// the premise of every tie-break scenario: the reversed order says `winner`
// is lower than `loser`, and the forward order says the opposite
static bool only_reversed_lower(uint64_t winner, uint64_t loser) {
  return rev_lower(winner, loser) && loser < winner;
}

// ---- harness --------------------------------------------------------------
struct H {
  Vmaap_wrap* d;
  std::vector<Frame> tx;                   // captured frames, grant-time ms
  // lane/serializer state
  bool ser_busy = false;
  bool lane_hold = false;                  // a stalled egress: the lane grants nothing
  Bytes ser_cur;
  uint32_t ser_ms = 0;
  size_t grants = 0;                       // lane grants given
  size_t slot_reqs = 0;                    // cycles the engine requested a TX slot
  size_t tmr_starts = 0;                   // timer starts the engine issued
  size_t tmr_expiries = 0;                 // expiries of the engine's timers
  // conflict sweep capture
  bool confl_auto = false;
  std::vector<int> confl_srcs;
  // kind-7 stub: while the script is non-empty, draw k (counted from the
  // last reset of addr_draws) hands the engine addr_script[k], the last
  // entry repeating; empty = the real PRNG's value
  std::vector<uint16_t> addr_script;
  size_t addr_draws = 0;                   // kind-7 draws the engine consumed

  explicit H(Vmaap_wrap* dd) : d(dd) {}

  void step() {
    d->clk_i = 0; d->eval();
    step_body();
  }
  void idle(int n) { for (int i = 0; i < n; ++i) step(); }
  void run_ms(int ms) { idle(ms * kClkPerMs); }
  uint32_t now() { return d->now_ms_o; }

  // wait until `count` frames are captured or the ms budget runs out
  bool wait_frames(size_t count, int budget_ms) {
    long cyc = long(budget_ms) * kClkPerMs;
    while (tx.size() < count && cyc-- > 0) step();
    return tx.size() >= count;
  }

  // inject one MAAP record (the validator's lanes, pre-parsed)
  void rx(int msg, uint64_t sa, uint64_t req_start, uint16_t req_cnt,
          uint64_t con_start = 0, uint16_t con_cnt = 0, int ver = 1) {
    d->txn_msg_i = msg & 0xF;
    d->txn_status_i = ver & 0x1F;
    d->txn_src_mac_i = sa;
    d->txn_req_i = (req_start << 16) | req_cnt;
    d->txn_conf_i = (con_start << 16) | con_cnt;
    d->txn_valid_i = 1;
    for (int g = 0; g < kRxAcceptCycles; ++g) {
      step();
      if (d->txn_ready_o) break;
    }
    d->txn_valid_i = 0;
    step();
  }

  // one allocator-face request; returns {got, ok, da, accept_lat, rsp_lat}.
  // rsp_lat is measured POST-EDGE: the seam registers the answer at the
  // accepting edge itself (the shim's decision 1), so a healthy request
  // reads back rsp_lat == 0 at this sampling granularity.
  struct AllocRes {
    bool got = false;
    bool ok = false;
    uint64_t da = 0;
    int acc_lat = -1;
    int rsp_lat = -1;
  };
  AllocRes alloc(int src, bool release = false) {
    AllocRes r;
    d->alloc_req_src_i = src & 7;
    d->alloc_req_release_i = release ? 1 : 0;
    d->alloc_req_valid_i = 1;
    bool accepted = false;
    for (int t = 0; t < kAllocWaitCycles; ++t) {
      d->clk_i = 0; d->eval();
      const bool take = !accepted && d->alloc_req_valid_i
                        && d->alloc_req_ready_o;   // handshake at THIS edge
      step_body();
      if (take) { accepted = true; r.acc_lat = t; d->alloc_req_valid_i = 0; }
      if (accepted && d->alloc_rsp_valid_o) {
        r.got = true;
        r.ok = d->alloc_rsp_ok_o;
        r.da = d->alloc_rsp_da_o;
        r.rsp_lat = t - r.acc_lat;
        break;
      }
    }
    d->alloc_req_valid_i = 0;
    step();
    return r;
  }
  // the second half of step() (after the pre-edge eval in alloc())
  void step_body() {
    // TX arbiter lane: grant a committed request, then serialize the slot
    d->txreq_ready_i = 0;
    d->ser_ready_i = 1;
    if (!ser_busy && !lane_hold && d->txreq_valid_o) {
      d->txreq_ready_i = 1;                // one-cycle lane grant
      d->ser_req_i = 1;
      d->ser_slot_i = d->txreq_slot_o;
      ser_busy = true; ser_cur.clear(); ser_ms = d->now_ms_o;
      ++grants;
    }
    if (ser_busy && d->ser_valid_o) {
      d->ser_req_i = 0;
      ser_cur.push_back(d->ser_data_o);
      if (d->ser_last_o) { tx.push_back({ser_cur, ser_ms}); ser_busy = false; }
    }
    // conflict ack policy
    d->conflict_ack_i = 0;
    if (confl_auto && d->conflict_valid_o) {
      d->conflict_ack_i = 1;
      confl_srcs.push_back(int(d->conflict_src_o));
    }
    // kind-7 stub: the value the engine samples at this edge
    d->addr_stub_en_i = addr_script.empty() ? 0 : 1;
    if (!addr_script.empty()) {
      d->addr_stub_ms_i = addr_script[std::min(addr_draws, addr_script.size() - 1)];
    }
    d->eval();
    const bool addr_done = d->addr_draw_o;
    if (d->slot_req_o) ++slot_reqs;
    if (d->tmr_start_o) ++tmr_starts;
    if (d->tmr_exp_o) ++tmr_expiries;
    d->clk_i = 1; d->eval();
    if (addr_done) ++addr_draws;
  }
};

static void dump(const char* nm, const Bytes& f) {
  printf("  %s:", nm);
  for (size_t i = 0; i < f.size(); ++i) printf(" %02x", f[i]);
  printf("\n");
}

// ---- the suite ------------------------------------------------------------
// One object owns the model, the lane BFM and the tally, so every scenario
// below is a named member function rather than another thousand lines of
// `main` reading state nobody can scope (Core Guidelines I.2, F.3).
namespace {
class MaapAnnexBSuite {
 public:
  int run();

 private:
  void reset_leaves_the_machine_initial();
  void engage_probes_a_fresh_pool_range();
  void cold_walk_retransmits_then_announces();
  void announce_cadence_holds_inside_the_bounds();
  void allocator_seam_serves_the_valid_claim();
  void probe_over_our_block_is_defended();
  void probe_from_below_names_the_first_allocated_address();
  void non_overlapping_probe_is_ignored();
  void announce_from_a_higher_peer_is_ignored();
  void announce_from_a_lower_peer_yields_and_reprobes();
  void probe_from_a_higher_peer_leaves_our_walk_unmoved();
  void defend_during_probe_yields_without_tie_break();
  void reserved_message_types_change_nothing();
  void empty_range_never_conflicts();
  void higher_maap_version_is_still_processed();
  void release_parks_the_machine_without_a_pdu();
  void provisioned_seed_probes_first_and_is_not_reused();
  void alloc_is_refused_while_probing();
  void overhanging_draws_are_redrawn_until_the_block_fits();
  void a_release_mid_redraw_leaves_no_draw_behind();
  void a_release_during_a_fitting_draw_abandons_it();
  void a_seed_past_the_fit_is_clamped_into_the_pool();
  void the_seed_clamp_boundary_is_exact();
  void expect_a_yield_to_a_fresh_range(const char* tag, uint64_t contested,
                                       unsigned conflicts_before, size_t n0);
  void probe_from_a_lower_peer_yields_the_walk();
  void defend_from_a_higher_peer_is_ignored();
  void defend_from_a_lower_peer_yields_the_claim();
  void announce_during_probe_yields_without_tie_break();
  bool walk_to_the_fourth_probe();
  bool accept_record(int msg, uint64_t sa, uint64_t req_start, uint16_t req_cnt);
  void a_release_on_the_announce_path_is_ordered_by_the_slot_request();
  void a_bounce_inside_the_tx_path_restarts_the_walk();
  void an_outage_behind_a_stalled_lane_withdraws_the_claim();
  void a_short_release_is_never_absorbed();
  void every_release_rearms_the_seed();
  bool run_to_walker(unsigned st, long budget_cycles);
  bool poise_a_fall(int point);
  void no_pdu_is_generated_after_the_fall();

  const milan::tb::Model<Vmaap_wrap> model;
  Vmaap_wrap* const d = model.get();
  H h{d};
  int checks = 0;
  int fails = 0;
  uint64_t base = 0;                       // the claim under test, as it moves
  Bytes probe_exp;                         // the PROBE the cold walk must send
};
}  // namespace

// ---- reset + configuration ------------------------------------------------
void MaapAnnexBSuite::reset_leaves_the_machine_initial() {
  d->rst_n = 0;
  d->cfg_en_i = 0; d->cfg_count_i = COUNT;
  d->cfg_seed_offset_i = 0; d->cfg_seed_valid_i = 0;
  d->own_mac_i = OWN_MAC; d->entity_id_i = EID;
  d->link_up_i = 0;
  d->txn_valid_i = 0; d->alloc_req_valid_i = 0; d->conflict_ack_i = 0;
  d->txreq_ready_i = 0; d->ser_req_i = 0; d->ser_slot_i = 0; d->ser_ready_i = 1;
  d->addr_stub_en_i = 0; d->addr_stub_ms_i = 0;
  h.idle(20);
  d->rst_n = 1;
  h.idle(10);

  CHECK(d->state_o == 0 && !d->addr_valid_o, "U0: INITIAL after reset");
}

// ---- U1: engage -> generate_address + ReserveAddress! -> first PROBE ------
void MaapAnnexBSuite::engage_probes_a_fresh_pool_range() {
  d->link_up_i = 1;                  // PRNG seeds; PortOperational! pending
  h.idle(5);
  d->cfg_en_i = 1;                   // Begin!: the machine may now transmit
  CHECK(h.wait_frames(1, kProbeBudgetMs), "U1: first PROBE within one probe interval");
  base = d->addr_o;
  unsigned off = unsigned(base & 0xFFFF);
  CHECK((base >> 16) == (POOL_HI >> 16),
        "U1: claim carries the pool prefix, got %012llx",
        static_cast<unsigned long long>(base));
  CHECK(off <= POOL_SIZE - COUNT,
        "U1: block fits the Table B.9 pool, offset 0x%04x", off);
  CHECK(d->state_o == 1 && !d->addr_valid_o,
        "U1: PROBE state, claim not yet valid");
  probe_exp = maap_frame(MAAP_DA, OWN_MAC, 1, base, COUNT, 0, 0);
  CHECK(h.tx.size() == 1 && h.tx[0].b == probe_exp, "U1: PROBE byte-exact");
  if (h.tx.size() == 1 && h.tx[0].b != probe_exp) {
    dump("got", h.tx[0].b); dump("exp", probe_exp);
  }
}

// ---- U2: 3 retransmits at (500, 600) ms, then ANNOUNCE + DEFEND -----------
void MaapAnnexBSuite::cold_walk_retransmits_then_announces() {
  CHECK(h.wait_frames(5, 4 * kProbeBudgetMs), "U2: 4 PROBEs + 1 ANNOUNCE on a cold walk");
  if (h.tx.size() >= 5) {
    for (int k = 1; k < 4; ++k) {
      CHECK(h.tx[size_t(k)].b == probe_exp, "U2: PROBE %d byte-exact", k + 1);
      long dt = long(h.tx[size_t(k)].ms) - long(h.tx[size_t(k) - 1].ms);
      // B.3.4.2: strictly 500 < T < 600; the grant-time measurement adds
      // at most one compressed ms of walk jitter either way
      CHECK(dt >= 500 && dt <= 601,
            "U2: probe interval %d = %ld ms outside (500, 600)", k, dt);
    }
    Bytes ann_exp = maap_frame(MAAP_DA, OWN_MAC, 3, base, COUNT, 0, 0);
    CHECK(h.tx[4].b == ann_exp, "U2: first ANNOUNCE byte-exact");
    if (h.tx[4].b != ann_exp) { dump("got", h.tx[4].b); dump("exp", ann_exp); }
    long dt_ann = long(h.tx[4].ms) - long(h.tx[3].ms);
    CHECK(dt_ann <= 50,
          "U2: probeCount! sends the ANNOUNCE immediately (dt %ld ms)", dt_ann);
  }
  CHECK(d->state_o == 2 && d->addr_valid_o && d->addr_o == base,
        "U2: DEFEND state, claim valid at the probed base");
  CHECK(d->conflicts_o == 0 && d->defends_o == 0, "U2: counters idle");
}

// ---- U3: announce cadence strictly inside (30, 32) s ----------------------
void MaapAnnexBSuite::announce_cadence_holds_inside_the_bounds() {
  CHECK(h.wait_frames(6, 33000), "U3: second ANNOUNCE inside 33 s");
  if (h.tx.size() >= 6) {
    long dt = long(h.tx[5].ms) - long(h.tx[4].ms);
    CHECK(dt >= 30000 && dt <= 32001,
          "U3: announce interval %ld ms outside (30000, 32000)", dt);
    CHECK(h.tx[5].b == maap_frame(MAAP_DA, OWN_MAC, 3, base, COUNT, 0, 0),
          "U3: periodic ANNOUNCE byte-exact");
  }
}

// ---- U4: the allocator seam over the valid claim --------------------------
void MaapAnnexBSuite::allocator_seam_serves_the_valid_claim() {
  auto r = h.alloc(0);
  CHECK(r.got && r.acc_lat <= 1 && r.rsp_lat == 0,
        "U4: ALLOC accepted at once, answered at the accept edge (acc %d rsp %d)",
        r.acc_lat, r.rsp_lat);
  CHECK(r.ok && r.da == base, "U4: source 0 granted base (got %012llx)",
        static_cast<unsigned long long>(r.da));
  r = h.alloc(5);
  CHECK(r.got && r.ok && r.da == base + 5, "U4: source 5 granted base + 5");
  // count guard: shrink the quasi-static count so source 5 falls outside
  d->cfg_count_i = 4;
  h.idle(2);
  r = h.alloc(5);
  CHECK(r.got && !r.ok && r.da == 0, "U4: source past the count refused");
  // that count change is a block-identity change: every source is told
  h.confl_auto = true;
  h.idle(30);
  h.confl_auto = false;
  CHECK(h.confl_srcs.size() == 8, "U4: %zu conflict events, want 8",
        h.confl_srcs.size());
  bool ordered = h.confl_srcs.size() == 8;
  for (size_t i = 0; ordered && i < 8; ++i) ordered = h.confl_srcs[i] == int(i);
  CHECK(ordered, "U4: conflict sweep lowest-source-first");
  h.confl_srcs.clear();
  d->cfg_count_i = COUNT;
  h.confl_auto = true; h.idle(30); h.confl_auto = false;   // and back
  h.confl_srcs.clear();
  r = h.alloc(3, /*release=*/true);
  CHECK(r.got && !r.ok && r.rsp_lat == 0,
        "U4: RELEASE acknowledged (a no-op on the block claim)");
  CHECK(d->addr_valid_o, "U4: the block survives a RELEASE");
}

// ---- U5: rProbe! in DEFEND -> byte-exact DEFEND (no tie-break) ------------
void MaapAnnexBSuite::probe_over_our_block_is_defended() {
  size_t n0 = h.tx.size();
  const uint64_t their_mac = 0x0A0000000001ull;  // rev-lower than ours,
  CHECK(rev_lower(their_mac, OWN_MAC),           // and STILL defended
        "U5: premise — the prober is rev-lower than us");
  // their probe wants [base+4, base+11]: overlap = [base+4, base+7]
  h.rx(1, their_mac, base + 4, 8);
  CHECK(h.wait_frames(n0 + 1, 100), "U5: DEFEND sent");
  if (h.tx.size() > n0) {
    Bytes def_exp = maap_frame(their_mac, OWN_MAC, 2, base + 4, 8,
                               base + 4, 4);
    CHECK(h.tx[n0].b == def_exp, "U5: DEFEND byte-exact (B.3.6.6 fields)");
    if (h.tx[n0].b != def_exp) { dump("got", h.tx[n0].b); dump("exp", def_exp); }
  }
  CHECK(d->defends_o == 1 && d->conflicts_o == 0 && d->addr_valid_o,
        "U5: defended, nothing yielded");
}

// ---- U5b: a probe from BELOW our base — the max() term of B.3.6.6 ---------
// requested [base-4, base+3]: the FIRST ALLOCATED address in conflict is
// our base, not the requested start; count = 4 overlapping addresses
void MaapAnnexBSuite::probe_from_below_names_the_first_allocated_address() {
  size_t n0 = h.tx.size();
  const uint64_t their_mac = 0x0A0000000002ull;
  h.rx(1, their_mac, base - 4, 8);
  CHECK(h.wait_frames(n0 + 1, 100), "U5b: DEFEND sent");
  if (h.tx.size() > n0) {
    Bytes def_exp = maap_frame(their_mac, OWN_MAC, 2, base - 4, 8,
                               base, 4);
    CHECK(h.tx[n0].b == def_exp,
          "U5b: conflict_start is the first ALLOCATED address (B.3.6.6)");
    if (h.tx[n0].b != def_exp) { dump("got", h.tx[n0].b); dump("exp", def_exp); }
  }
  CHECK(d->defends_o == 2 && d->addr_valid_o, "U5b: defended, claim kept");
}

// ---- U6: a PROBE with no overlap is ignored (footnote b) ------------------
void MaapAnnexBSuite::non_overlapping_probe_is_ignored() {
  size_t n0 = h.tx.size();
  h.rx(1, 0x0A0000000001ull, base + COUNT, 4);   // starts past our block
  h.run_ms(50);
  CHECK(h.tx.size() == n0 && d->defends_o == 2,
        "U6: non-overlapping PROBE ignored");
}

// ---- U7: rAnnounce! in DEFEND, we are rev-lower -> compare_MAC ignores ----
// the announcer is forward-LOWER than us: only the reversed compare wins
void MaapAnnexBSuite::announce_from_a_higher_peer_is_ignored() {
  CHECK(only_reversed_lower(OWN_MAC, WIN_MAC),
        "U7: premise — we are rev-lower, forward-higher");
  h.rx(3, WIN_MAC, base, COUNT);
  h.run_ms(50);
  CHECK(d->addr_valid_o && d->addr_o == base && d->conflicts_o == 0,
        "U7: claim kept — compare_MAC TRUE takes no action");
}

// ---- U8: rAnnounce! in DEFEND, we are rev-higher -> yield -----------------
// the announcer is forward-HIGHER than us: only the reversed compare loses
void MaapAnnexBSuite::announce_from_a_lower_peer_yields_and_reprobes() {
  size_t n0 = h.tx.size();
  CHECK(only_reversed_lower(LOSE_MAC, OWN_MAC),
        "U8: premise — they are rev-lower, forward-higher");
  h.confl_auto = true;
  h.rx(3, LOSE_MAC, base, COUNT);
  h.idle(50);
  CHECK(!d->addr_valid_o && d->conflicts_o == 1,
        "U8: yielded — claim invalid, re-address counted");
  // the block moved: all 8 sources are told, lowest first
  h.idle(30);
  h.confl_auto = false;
  CHECK(h.confl_srcs.size() == 8, "U8: %zu conflict events, want 8",
        h.confl_srcs.size());
  h.confl_srcs.clear();
  // Restart!: a FRESH random range, 4 probes, announce, DEFEND again
  CHECK(h.wait_frames(n0 + 5, 5 * kProbeBudgetMs), "U8: re-probe walk completed");
  uint64_t base2 = d->addr_o;
  CHECK(base2 != base, "U8: generate_address drew a fresh range");
  CHECK((base2 & 0xFFFF) <= POOL_SIZE - COUNT, "U8: new block fits");
  if (h.tx.size() >= n0 + 5) {
    Bytes p2 = maap_frame(MAAP_DA, OWN_MAC, 1, base2, COUNT, 0, 0);
    Bytes a2 = maap_frame(MAAP_DA, OWN_MAC, 3, base2, COUNT, 0, 0);
    bool probes_ok = true;
    for (int k = 0; k < 4; ++k)
      probes_ok = probes_ok && (h.tx[n0 + size_t(k)].b == p2);
    CHECK(probes_ok, "U8: re-probe frames carry the fresh range");
    CHECK(h.tx[n0 + 4].b == a2, "U8: re-announce byte-exact");
  }
  CHECK(d->addr_valid_o && d->state_o == 2, "U8: claim re-established");
  base = base2;
}

// ---- U9: rProbe! in PROBE, we are rev-lower -> keep probing ---------------
void MaapAnnexBSuite::probe_from_a_higher_peer_leaves_our_walk_unmoved() {
  // force a fresh walk: PortOperational! bounce re-randomizes
  d->link_up_i = 0; h.idle(30);
  size_t n0 = h.tx.size();
  d->link_up_i = 1;
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs), "U9: walk restarted on link return");
  uint64_t b9 = d->addr_o;
  CHECK(d->state_o == 1, "U9: PROBE state");
  CHECK(only_reversed_lower(OWN_MAC, WIN_MAC),
        "U9: premise — we are rev-lower, forward-higher");
  h.rx(1, WIN_MAC, b9, COUNT);                   // their probe, we are rev-lower
  h.run_ms(20);
  CHECK(d->addr_o == b9 && d->conflicts_o == 1,
        "U9: compare_MAC TRUE — our probe walk continues unmoved");
  // walk completes on the SAME range
  CHECK(h.wait_frames(n0 + 5, 5 * kProbeBudgetMs) && d->addr_o == b9 && d->addr_valid_o,
        "U9: claim lands on the contested range we won");
  base = b9;
}

// ---- U10: rDefend! in PROBE -> yield, no tie-break ------------------------
void MaapAnnexBSuite::defend_during_probe_yields_without_tie_break() {
  d->link_up_i = 0; h.idle(30);
  size_t n0 = h.tx.size();
  d->link_up_i = 1;
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs), "U10: probing again");
  uint64_t b10 = d->addr_o;
  const uint64_t hi_mac = 0xF2FFEEDDCCFFull;     // even a rev-higher peer
  h.confl_auto = true;
  // a DEFEND names its owner's range in the CONFLICT fields (B.2.7):
  // requested echoes some third party; the conflict range hits us
  h.rx(2, hi_mac, 0x91E0F0000000ull, 1, b10 + 2, 2);
  h.idle(50);
  h.confl_auto = false;
  h.confl_srcs.clear();
  CHECK(d->conflicts_o == 2, "U10: PROBE-state rDefend! yields, no tie-break");
  CHECK(h.wait_frames(n0 + 6, 6 * kProbeBudgetMs) && d->addr_valid_o,
        "U10: fresh walk completes");
  CHECK(d->addr_o != b10, "U10: range re-randomized");
  base = d->addr_o;
}

// ---- U11: reserved message_type ignored (B.2.2) ---------------------------
void MaapAnnexBSuite::reserved_message_types_change_nothing() {
  size_t n0 = h.tx.size();
  uint8_t c0 = d->conflicts_o;
  h.rx(4, 0x010000000000ull, base, COUNT);       // reserved type, overlaps
  h.rx(0, 0x010000000000ull, base, COUNT);
  h.run_ms(30);
  CHECK(h.tx.size() == n0 && d->conflicts_o == c0 && d->addr_valid_o,
        "U11: reserved message types change nothing");
}

// ---- U12: an empty range never conflicts ----------------------------------
void MaapAnnexBSuite::empty_range_never_conflicts() {
  uint8_t c0 = d->conflicts_o;
  h.rx(3, 0x010000000000ull, base, 0);           // count 0 = empty
  h.run_ms(30);
  CHECK(d->addr_valid_o && d->conflicts_o == c0,
        "U12: zero-count range is empty, no event");
}

// ---- U13: maap_version tolerance (B.2.3.2) --------------------------------
void MaapAnnexBSuite::higher_maap_version_is_still_processed() {
  h.confl_auto = true;
  h.rx(3, 0x010000000000ull, base, COUNT, 0, 0, /*ver=*/2);
  h.idle(50);
  CHECK(!d->addr_valid_o,
        "U13: a higher maap_version with a known type is still processed");
  CHECK(h.wait_frames(h.tx.size() + 1, kProbeBudgetMs), "U13: walk restarted");
  h.idle(50);
  h.confl_auto = false;
  h.confl_srcs.clear();
  // let the fresh walk finish before the next scenario
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  CHECK(d->addr_valid_o, "U13: reclaimed");
  base = d->addr_o;
}

// ---- U14: Release! (engage fall) is local — no PDU, machine parks ---------
void MaapAnnexBSuite::release_parks_the_machine_without_a_pdu() {
  size_t n0 = h.tx.size();
  h.confl_auto = true;
  d->cfg_en_i = 0;
  h.idle(60);
  h.confl_auto = false;
  CHECK(!d->addr_valid_o && d->state_o == 0,
        "U14: INITIAL after Release!, claim withdrawn");
  CHECK(h.tx.size() == n0, "U14: Release! sends NOTHING (footnote c)");
  CHECK(h.confl_srcs.size() == 8, "U14: sources told the block is gone");
  h.confl_srcs.clear();
  // allocator refuses while parked, still in one cycle
  auto r = h.alloc(0);
  CHECK(r.got && !r.ok && r.rsp_lat == 0, "U14: parked ALLOC refused fast");
}

// ---- U15: footnote-a seeding — the provisioned range probes first ---------
void MaapAnnexBSuite::provisioned_seed_probes_first_and_is_not_reused() {
  size_t n0 = h.tx.size();
  d->cfg_seed_offset_i = 0x1234;
  d->cfg_seed_valid_i = 1;
  d->cfg_en_i = 1;
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs), "U15: seeded walk starts");
  CHECK((d->addr_o & 0xFFFF) == 0x1234,
        "U15: first attempt probes the provisioned offset (got 0x%04llx)",
        static_cast<unsigned long long>(d->addr_o & 0xFFFF));
  if (h.tx.size() > n0) {
    Bytes p = maap_frame(MAAP_DA, OWN_MAC, 1, POOL_HI | 0x1234, COUNT, 0, 0);
    CHECK(h.tx[n0].b == p, "U15: seeded PROBE byte-exact");
  }
  // a conflict must NOT reuse the seed: the conflicted range is known-bad
  h.confl_auto = true;
  h.rx(3, 0x010000000000ull, POOL_HI | 0x1234, COUNT);
  h.idle(50);
  CHECK(h.wait_frames(n0 + 2, kProbeBudgetMs), "U15: restart probes");
  CHECK((d->addr_o & 0xFFFF) != 0x1234,
        "U15: the conflicted seed is not probed again");
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  h.confl_auto = false;
  h.confl_srcs.clear();
  CHECK(d->addr_valid_o, "U15: reclaimed after the seed conflict");
  d->cfg_seed_valid_i = 0;
}

// ---- U16: ALLOC refused during PROBE (the shim's decision 1/5 path) -------
void MaapAnnexBSuite::alloc_is_refused_while_probing() {
  d->link_up_i = 0; h.idle(30);
  size_t n0 = h.tx.size();
  d->link_up_i = 1;
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs), "U16: probing");
  CHECK(d->state_o == 1, "U16: PROBE state");
  auto r = h.alloc(0);
  CHECK(r.got && !r.ok && r.acc_lat <= 1 && r.rsp_lat == 0,
        "U16: still-probing ALLOC refused in one cycle, never parked");
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  CHECK(d->addr_valid_o, "U16: claim completes after the refusal");
  auto r2 = h.alloc(1);
  CHECK(r2.got && r2.ok && r2.da == d->addr_o + 1,
        "U16: the same source path grants once the claim stands");
}

// ---- U17: the fit clamp's reject arm (B.1, B.3.6.1, Table B.9) ------------
// The widest block (255 addresses) fits the pool only at offsets up to
// 0xFE00 - 255 = 0xFD01. The kind-7 stub hands the engine the top pool
// offset, then the first offset past the fit, then the fit itself: the first
// two overhang 91:E0:F0:00:FD:FF and must be redrawn, and the PROBE names the
// third, whose block ends exactly at the top of the pool.
void MaapAnnexBSuite::overhanging_draws_are_redrawn_until_the_block_fits() {
  const unsigned fit = POOL_SIZE - MAX_COUNT;          // 0xFD01
  h.confl_auto = true;
  d->link_up_i = 0; h.idle(30);                        // Release!
  h.confl_auto = false;
  h.confl_srcs.clear();
  d->cfg_count_i = MAX_COUNT;
  h.addr_script = {uint16_t(POOL_SIZE - 1), uint16_t(fit + 1), uint16_t(fit)};
  h.addr_draws = 0;
  size_t n0 = h.tx.size();
  d->link_up_i = 1;                                    // PortOperational!
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs), "U17: the widest block is probed");
  CHECK(h.addr_draws == 3,
        "U17: draws 0x%04x and 0x%04x overhang the pool and are redrawn "
        "(%zu draws, want 3)", POOL_SIZE - 1, fit + 1, h.addr_draws);
  const uint64_t b17 = d->addr_o;
  CHECK(b17 == (POOL_HI | fit), "U17: the claim is the first fitting draw, got %012llx",
        static_cast<unsigned long long>(b17));
  CHECK(b17 + MAX_COUNT - 1 <= POOL_LAST,
        "U17: the probed block ends at or below 91:E0:F0:00:FD:FF (ends %012llx)",
        static_cast<unsigned long long>(b17 + MAX_COUNT - 1));
  if (h.tx.size() > n0) {
    Bytes p = maap_frame(MAAP_DA, OWN_MAC, 1, POOL_HI | fit, MAX_COUNT, 0, 0);
    CHECK(h.tx[n0].b == p, "U17: PROBE byte-exact for the fitting block");
    if (h.tx[n0].b != p) { dump("got", h.tx[n0].b); dump("exp", p); }
  }
  h.addr_script.clear();
}

// ---- U17b: a Release! inside the redraw loop leaves no draw behind --------
// generate_address may redraw for as long as the draws overhang the pool,
// and a link loss (Release!) can land while a draw is still in flight. The
// next PortOperational! must run generate_address + ReserveAddress! (Table
// B.7, B.3.5.9) as if that draw had never been asked for. The stub keeps
// every draw overhanging, the link drops at each of four consecutive cycles
// of the loop's request/answer rhythm, and each re-engage must reach its
// first PROBE on a fitting draw.
void MaapAnnexBSuite::a_release_mid_redraw_leaves_no_draw_behind() {
  const unsigned fit = POOL_SIZE - MAX_COUNT;          // 0xFD01
  d->cfg_count_i = MAX_COUNT;
  for (int phase = 0; phase < 4; ++phase) {
    d->link_up_i = 0; h.idle(30);                      // Release!
    h.addr_script = {uint16_t(POOL_SIZE - 1)};         // every draw overhangs
    h.addr_draws = 0;
    d->link_up_i = 1;
    for (int g = 0; g < kRxAcceptCycles && h.addr_draws < 2; ++g) h.step();
    h.idle(phase);
    d->link_up_i = 0; h.idle(30);                      // Release! mid-loop
    h.addr_script = {uint16_t(fit)};
    size_t n0 = h.tx.size();
    d->link_up_i = 1;                                  // PortOperational!
    CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs) && d->addr_o == (POOL_HI | fit),
          "U17b: phase %d, the walk restarts after a Release! mid-redraw", phase);
  }
  h.addr_script.clear();
}

// ---- U17c: a Release! while a FITTING kind-7 draw is in flight ------------
// U17b drops the link only while every draw overhangs, where abandoning the
// draw and waiting for its answer look the same. Here every draw fits. The
// link drops at each of the first 12 cycles after the rise, which covers the
// kind-7 draw and the interval draw after it. A drop before the PROBE's TX
// slot request sends nothing after the Release! (Table B.7 Release!); the
// drops from the request on drain the PROBE, the rule U23 grades, and are
// only counted here. A draw whose answer had not arrived by the fall is
// abandoned, never adopted: addr_o keeps the range it held before the walk.
// The next PortOperational! probes (B.3.5.9).
void MaapAnnexBSuite::a_release_during_a_fitting_draw_abandons_it() {
  constexpr int kOffsets = 12;
  constexpr int kWatchCycles = 50 * kClkPerMs;
  int in_draw = 0;
  int after_draw = 0;
  int owed = 0;
  int sent = 0;
  int adopted = 0;
  int stuck = 0;
  h.confl_auto = true;
  d->cfg_count_i = COUNT;
  for (int k = 0; k < kOffsets; ++k) {
    d->link_up_i = 0; h.idle(30);                      // Release!
    const uint64_t before = d->addr_o;
    const uint16_t fits = ((before & 0xFFFF) == 0x1000) ? 0x2000 : 0x1000;
    h.addr_script = {fits};                            // every draw fits
    h.addr_draws = 0;
    const size_t r0 = h.slot_reqs;
    const size_t g0 = h.grants;
    d->link_up_i = 1;                                  // PortOperational!
    for (int i = 0; i < k; ++i) h.step();
    const size_t answered = h.addr_draws;              // answers seen before the fall
    d->link_up_i = 0;                                  // Release!
    h.step();                                          // the first cycle it is seen
    const bool requested = h.slot_reqs > r0;
    for (int c = 1; c < kWatchCycles; ++c) h.step();
    if (requested) {
      ++owed;
    } else {
      if (answered == 0) {
        ++in_draw;
      } else {
        ++after_draw;
      }
      if (h.grants != g0 || h.slot_reqs != r0) ++sent;
      if (answered == 0 && d->addr_o != before) ++adopted;
    }
    const size_t n0 = h.tx.size();
    d->link_up_i = 1;                                  // PortOperational!
    if (!h.wait_frames(n0 + 1, kProbeBudgetMs) || d->addr_o != (POOL_HI | fits)) ++stuck;
  }
  h.addr_script.clear();
  CHECK(in_draw >= 3 && after_draw >= 1,
        "U17c: premise, the drops land in the kind-7 draw (%d) and after it (%d), "
        "before the slot request (%d from it)", in_draw, after_draw, owed);
  CHECK(sent == 0, "U17c: nothing is sent after a Release! (%d offsets)", sent);
  CHECK(adopted == 0,
        "U17c: a draw in flight at the fall is abandoned, never adopted (%d of %d offsets)",
        adopted, in_draw);
  CHECK(stuck == 0, "U17c: the next PortOperational! probes the next fitting draw "
        "(%d offsets did not)", stuck);
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  h.confl_auto = false;
  h.confl_srcs.clear();
}

// ---- U18: a mis-provisioned seed is clamped into the pool (footnote a) ----
// A provisioned range skips generate_address. One whose block would leave
// the Table B.9 pool (the seed here names 91:E0:F0:00:FF:FF, inside the Table
// B.10 reserved range) probes the clamped offset 0xFE00 - count instead, so
// the whole block still ends at 91:E0:F0:00:FD:FF.
void MaapAnnexBSuite::a_seed_past_the_fit_is_clamped_into_the_pool() {
  const unsigned clamp = POOL_SIZE - COUNT;            // 0xFDF8
  d->link_up_i = 0; h.idle(30);                        // Release! re-arms the seed
  d->cfg_count_i = COUNT;
  d->cfg_seed_offset_i = 0xFFFF;
  d->cfg_seed_valid_i = 1;
  h.addr_draws = 0;
  size_t n0 = h.tx.size();
  d->link_up_i = 1;
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs), "U18: the seeded walk starts");
  CHECK(h.addr_draws == 0,
        "U18: the provisioned range skips generate_address (%zu draws)", h.addr_draws);
  CHECK(d->addr_o == (POOL_HI | clamp),
        "U18: the seed is clamped to offset 0x%04x, got %012llx", clamp,
        static_cast<unsigned long long>(d->addr_o));
  if (h.tx.size() > n0) {
    Bytes p = maap_frame(MAAP_DA, OWN_MAC, 1, POOL_HI | clamp, COUNT, 0, 0);
    CHECK(h.tx[n0].b == p, "U18: PROBE byte-exact at the clamped offset");
    if (h.tx[n0].b != p) { dump("got", h.tx[n0].b); dump("exp", p); }
  }
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  CHECK(d->addr_valid_o && d->addr_o == (POOL_HI | clamp),
        "U18: the clamped block is claimed");
  auto r = h.alloc(COUNT - 1);
  CHECK(r.got && r.ok && r.da == POOL_LAST,
        "U18: the block's last source is granted 91:E0:F0:00:FD:FF (got %012llx)",
        static_cast<unsigned long long>(r.da));
  d->cfg_seed_valid_i = 0;
}

// ---- U18b: the seed clamp's boundary (B.1, Table B.9, footnote a) --------
// The whole block fits the pool at a seed of 0xFE00 - count and below, so
// that seed is probed as given, and so is the one under it. 0xFE00 - count
// + 1 is the first seed whose block would end at 91:E0:F0:00:FE:00, outside
// the pool: it is clamped to 0xFE00 - count. Each is a fresh engagement (a
// Release! re-arms the seed) and each PROBE is byte-exact, with no draw.
void MaapAnnexBSuite::the_seed_clamp_boundary_is_exact() {
  const unsigned fit = POOL_SIZE - COUNT;              // 0xFDF8
  struct Case { unsigned seed; unsigned probed; const char* how; };
  const Case cases[] = {
      {fit + 1, fit, "clamped"},
      {fit, fit, "taken as given"},
      {fit - 1, fit - 1, "taken as given"},
  };
  h.confl_auto = true;
  d->cfg_count_i = COUNT;
  for (const Case& c : cases) {
    d->link_up_i = 0; h.idle(30);                      // Release! re-arms the seed
    d->cfg_seed_offset_i = uint16_t(c.seed);
    d->cfg_seed_valid_i = 1;
    h.addr_draws = 0;
    const size_t n0 = h.tx.size();
    d->link_up_i = 1;                                  // PortOperational!
    const bool probed = h.wait_frames(n0 + 1, kProbeBudgetMs);
    CHECK(probed && h.addr_draws == 0 && d->addr_o == (POOL_HI | c.probed),
          "U18b: seed 0x%04x is %s: probes 0x%04x (got %012llx, %zu draws)", c.seed, c.how,
          c.probed, static_cast<unsigned long long>(d->addr_o), h.addr_draws);
    if (probed) {
      CHECK(h.tx[n0].b == maap_frame(MAAP_DA, OWN_MAC, 1, POOL_HI | c.probed, COUNT, 0, 0),
            "U18b: seed 0x%04x, the PROBE is byte-exact", c.seed);
    }
  }
  d->cfg_seed_valid_i = 0;
  h.confl_auto = false;
  h.confl_srcs.clear();
}

// ---- the Table B.7 yield, as the scenarios below grade it ------------------
// Stop timer + INITIAL/Restart!: one re-address counted, the claim not
// valid, and generate_address runs again, so the next frame is a PROBE of a
// FRESH range inside the pool, never the contested one (B.3.5.3).
void MaapAnnexBSuite::expect_a_yield_to_a_fresh_range(const char* tag,
                                                      uint64_t contested,
                                                      unsigned conflicts_before,
                                                      size_t n0) {
  CHECK(d->conflicts_o == conflicts_before + 1 && !d->addr_valid_o,
        "%s: yielded, one re-address counted (%u -> %u)", tag, conflicts_before,
        unsigned(d->conflicts_o));
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs), "%s: Restart! probes again", tag);
  const uint64_t fresh = d->addr_o;
  CHECK(fresh != contested && (fresh >> 16) == (POOL_HI >> 16)
        && (fresh & 0xFFFF) <= POOL_SIZE - COUNT,
        "%s: the range is re-randomized inside the pool (%012llx after %012llx)",
        tag, static_cast<unsigned long long>(fresh),
        static_cast<unsigned long long>(contested));
  if (h.tx.size() > n0) {
    Bytes p = maap_frame(MAAP_DA, OWN_MAC, 1, fresh, COUNT, 0, 0);
    CHECK(h.tx[n0].b == p, "%s: the fresh range's PROBE is byte-exact", tag);
  }
}

// ---- U19: rProbe! in PROBE from a rev-lower peer -> yield -----------------
// compare_MAC FALSE (the prober is reversed-lower, forward-HIGHER): Stop
// probe_timer, INITIAL/Restart! with a fresh range
void MaapAnnexBSuite::probe_from_a_lower_peer_yields_the_walk() {
  CHECK(only_reversed_lower(LOSE_MAC, OWN_MAC),
        "U19: premise — they are rev-lower, forward-higher");
  h.confl_auto = true;                                 // U18's claim goes
  d->link_up_i = 0; h.idle(30);                        // Release!
  h.confl_auto = false;
  h.confl_srcs.clear();
  size_t n0 = h.tx.size();
  d->link_up_i = 1;
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs) && d->state_o == 1, "U19: probing");
  const uint64_t contested = d->addr_o;
  const unsigned c0 = d->conflicts_o;
  const size_t n1 = h.tx.size();
  h.rx(1, LOSE_MAC, contested, COUNT);
  h.idle(20);
  expect_a_yield_to_a_fresh_range("U19", contested, c0, n1);
}

// ---- U20: rDefend! in DEFEND from a rev-higher peer -> ignored ------------
// compare_MAC TRUE (the defender is reversed-higher, forward-LOWER): no
// further processing, the claim stands and nothing is sent (footnote d)
void MaapAnnexBSuite::defend_from_a_higher_peer_is_ignored() {
  CHECK(only_reversed_lower(OWN_MAC, WIN_MAC),
        "U20: premise — we are rev-lower, forward-higher");
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  CHECK(d->addr_valid_o && d->state_o == 2, "U20: DEFEND state");
  const uint64_t b = d->addr_o;
  const unsigned c0 = d->conflicts_o;
  const size_t n0 = h.tx.size();
  // a DEFEND whose ranges overlap ours (B.3.5.6)
  h.rx(2, WIN_MAC, b + 2, 2, b + 2, 2);
  h.run_ms(50);
  CHECK(d->addr_valid_o && d->addr_o == b && d->conflicts_o == c0 && h.tx.size() == n0,
        "U20: compare_MAC TRUE — the claim stands, nothing sent");
}

// ---- U21: rDefend! in DEFEND from a rev-lower peer -> yield ---------------
void MaapAnnexBSuite::defend_from_a_lower_peer_yields_the_claim() {
  CHECK(only_reversed_lower(LOSE_MAC, OWN_MAC),
        "U21: premise — they are rev-lower, forward-higher");
  CHECK(d->addr_valid_o && d->state_o == 2, "U21: DEFEND state");
  const uint64_t b = d->addr_o;
  const unsigned c0 = d->conflicts_o;
  const size_t n0 = h.tx.size();
  h.confl_auto = true;
  h.rx(2, LOSE_MAC, b + 2, 2, b + 2, 2);
  h.idle(20);
  expect_a_yield_to_a_fresh_range("U21", b, c0, n0);
  h.confl_auto = false;
  CHECK(h.confl_srcs.size() == 8, "U21: the withdrawn claim told all 8 sources (%zu)",
        h.confl_srcs.size());
  h.confl_srcs.clear();
}

// ---- U22: rAnnounce! in PROBE from a rev-higher peer -> still yields ------
// PROBE/rAnnounce! has no compare_MAC: even a peer we beat in BOTH orders
// takes the range
void MaapAnnexBSuite::announce_during_probe_yields_without_tie_break() {
  const uint64_t hi_mac = 0xF2FFEEDDCCFFull;
  CHECK(rev_lower(OWN_MAC, hi_mac) && OWN_MAC < hi_mac,
        "U22: premise — we are lower in both orders");
  CHECK(d->state_o == 1, "U22: probing (the walk U21 restarted)");
  const uint64_t contested = d->addr_o;
  const unsigned c0 = d->conflicts_o;
  const size_t n0 = h.tx.size();
  h.rx(3, hi_mac, contested, COUNT);
  h.idle(20);
  expect_a_yield_to_a_fresh_range("U22", contested, c0, n0);
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  CHECK(d->addr_valid_o, "U22: the fresh range is claimed");
}

// ---- the Release! arcs, as the scenarios below grade them ----------------
// Table B.7 Release!: Stop timer, INITIAL, and no send action; B.3.5.2: the
// range is no longer defended; B.3.2: the functions of each table entry
// execute sequentially, so a Release! that lands while the walker is still
// executing an entry is ordered before it or after it. The TX slot request
// decides which: before it the entry is dropped whole, and from it on the
// frame is past recall and drains as the last act of its entry. Either way
// the claim is withdrawn at the fall and a later PortOperational! starts a
// fresh walk.

// a fresh walk from a released machine, stopped just after the lane grants
// its 4th PROBE: the probeCount! entry (sAnnounce) runs from there
bool MaapAnnexBSuite::walk_to_the_fourth_probe() {
  d->link_up_i = 0; h.idle(30);                        // Release!
  d->link_up_i = 1;                                    // PortOperational!
  const size_t g0 = h.grants;
  for (long c = 0; c < 6L * kProbeBudgetMs * kClkPerMs && h.grants < g0 + 4; ++c) h.step();
  return h.grants == g0 + 4;
}

// present one record and clock until the engine takes it: the walker is in
// W_RX for the cycle after this returns true
bool MaapAnnexBSuite::accept_record(int msg, uint64_t sa, uint64_t req_start,
                                    uint16_t req_cnt) {
  d->txn_msg_i = msg & 0xF;
  d->txn_status_i = 1;
  d->txn_src_mac_i = sa;
  d->txn_req_i = (req_start << 16) | req_cnt;
  d->txn_conf_i = 0;
  d->txn_valid_i = 1;
  bool taken = false;
  for (int g = 0; g < kRxAcceptCycles && !taken; ++g) {
    d->clk_i = 0; d->eval();
    taken = d->txn_ready_o;                            // handshake at THIS edge
    h.step_body();
  }
  d->txn_valid_i = 0;
  return taken;
}

// ---- U23: a Release! swept along the ANNOUNCE's path (probeCount!) --------
// The link drops at each cycle from the 4th PROBE's lane grant until past
// the ANNOUNCE's. Before the ANNOUNCE's TX slot is requested the entry is
// dropped: no ANNOUNCE. From the request on the ANNOUNCE drains byte-exact
// and nothing follows it. At every offset the claim is not valid from the
// first cycle the fall is seen, the machine is INITIAL, and the next
// PortOperational! walks again.
void MaapAnnexBSuite::a_release_on_the_announce_path_is_ordered_by_the_slot_request() {
  constexpr int kOffsets = 80;
  constexpr int kWatchCycles = 50 * kClkPerMs;
  int dropped = 0;
  int drained = 0;
  int walk_fail = 0;
  int sent_unowed = 0;
  int drain_wrong = 0;
  int claim_after = 0;
  int not_initial = 0;
  h.confl_auto = true;
  for (int k = 0; k < kOffsets; ++k) {
    if (!walk_to_the_fourth_probe()) { ++walk_fail; continue; }
    const size_t g4 = h.grants;
    const size_t r4 = h.slot_reqs;
    const size_t n4 = h.tx.size();                     // the 4th PROBE is still serializing
    const uint64_t b = d->addr_o;
    for (int i = 0; i < k; ++i) h.step();
    d->link_up_i = 0;                                  // Release!
    h.step();                                          // the first cycle it is seen
    // requested in a cycle up to and including the first one with the link
    // down: the pool samples that request at the edge the walker sees the fall
    const bool owed = h.slot_reqs > r4;
    bool valid_after = d->addr_valid_o;
    for (int c = 1; c < kWatchCycles; ++c) {
      h.step();
      if (d->addr_valid_o) valid_after = true;
    }
    const size_t frames = h.tx.size() - n4;            // 4th PROBE (+ ANNOUNCE)
    if (owed) {
      ++drained;
      if (h.grants - g4 != 1 || frames != 2
          || h.tx.back().b != maap_frame(MAAP_DA, OWN_MAC, 3, b, COUNT, 0, 0)) {
        ++drain_wrong;
      }
    } else {
      ++dropped;
      if (h.grants != g4 || frames != 1 || h.slot_reqs != r4) ++sent_unowed;
    }
    if (valid_after) ++claim_after;
    if (d->state_o != 0) ++not_initial;
  }
  printf("  U23: %d offsets before the ANNOUNCE's slot request, %d from it\n", dropped, drained);
  CHECK(walk_fail == 0, "U23: every walk reached its 4th PROBE (%d did not)", walk_fail);
  CHECK(dropped > 0 && drained > 0,
        "U23: premise, the sweep lands on both sides of the ANNOUNCE's slot request "
        "(%d before, %d from it)", dropped, drained);
  CHECK(sent_unowed == 0,
        "U23: a Release! before the ANNOUNCE's slot request sends nothing (%d of %d offsets)",
        sent_unowed, dropped);
  CHECK(drain_wrong == 0,
        "U23: a requested ANNOUNCE drains byte-exact and nothing follows it (%d of %d offsets)",
        drain_wrong, drained);
  CHECK(claim_after == 0,
        "U23: no claim is valid after the fall (%d of %d offsets)", claim_after, kOffsets);
  CHECK(not_initial == 0, "U23: INITIAL after the Release! (%d offsets)", not_initial);
  const size_t n0 = h.tx.size();
  d->link_up_i = 1;                                    // PortOperational!
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs) && d->state_o == 1,
        "U23: the next PortOperational! probes");
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  h.confl_auto = false;
  h.confl_srcs.clear();
}

// ---- U24: a link bounce inside a frame's TX path is never absorbed --------
// Release! then PortOperational! (Table B.7): INITIAL, then a fresh walk,
// even when both land while a frame is being built. (a) DEFEND state: an
// rProbe! is answered by sDefend and the link bounces for 5 cycles while
// that DEFEND is built. (b) The link bounces for 5 cycles while the first
// ANNOUNCE (probeCount!) is built. Each frame is past recall and drains, the
// claim is not valid from the fall on, and the next frame is the first
// PROBE of a fresh walk.
void MaapAnnexBSuite::a_bounce_inside_the_tx_path_restarts_the_walk() {
  constexpr int kBounceCycles = 5;
  constexpr int kWatchCycles = 50 * kClkPerMs;
  constexpr int kBuildCycles = 10;                     // into the 60-byte write
  h.confl_auto = true;
  // (a) sDefend in DEFEND state
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  CHECK(d->addr_valid_o && d->state_o == 2, "U24: premise, DEFEND state");
  const uint64_t b = d->addr_o;
  const unsigned c0 = d->conflicts_o;
  const unsigned d0 = d->defends_o;
  const size_t n0 = h.tx.size();
  const size_t r0 = h.slot_reqs;
  const uint64_t prober = 0x0A2233445566ull;
  CHECK(accept_record(1, prober, b + 2, 2), "U24: the rProbe! is taken");
  for (int g = 0; g < kRxAcceptCycles && h.slot_reqs == r0; ++g) h.step();
  h.idle(kBuildCycles);
  CHECK(h.slot_reqs > r0 && d->addr_valid_o, "U24: premise, the DEFEND is being built");
  d->link_up_i = 0;                                    // Release!
  bool valid_after = false;
  for (int c = 0; c < kBounceCycles; ++c) {
    h.step();
    if (d->addr_valid_o) valid_after = true;
  }
  d->link_up_i = 1;                                    // PortOperational!
  for (int c = 0; c < kWatchCycles; ++c) {
    h.step();
    if (d->addr_valid_o) valid_after = true;
  }
  CHECK(!valid_after, "U24: sDefend path, the claim is withdrawn at the fall");
  CHECK(h.wait_frames(n0 + 2, kProbeBudgetMs) && d->state_o == 1,
        "U24: sDefend path, the DEFEND drains and a fresh walk probes (%zu frames, state %u)",
        h.tx.size() - n0, unsigned(d->state_o));
  if (h.tx.size() >= n0 + 2) {
    CHECK(h.tx[n0].b == maap_frame(prober, OWN_MAC, 2, b + 2, 2, b + 2, 2),
          "U24: sDefend path, the drained DEFEND is byte-exact");
    CHECK(h.tx[n0 + 1].b == maap_frame(MAAP_DA, OWN_MAC, 1, d->addr_o, COUNT, 0, 0),
          "U24: sDefend path, the fresh walk's PROBE is byte-exact");
  }
  CHECK(d->conflicts_o == c0 && d->defends_o == d0 + 1,
        "U24: sDefend path, one DEFEND counted and no re-address");
  // (b) sAnnounce at probeCount!
  CHECK(walk_to_the_fourth_probe(), "U24: announce path, the walk reaches its 4th PROBE");
  const size_t n4 = h.tx.size();
  const size_t r4 = h.slot_reqs;
  const uint64_t b4 = d->addr_o;
  for (int g = 0; g < kRxAcceptCycles && h.slot_reqs == r4; ++g) h.step();
  h.idle(kBuildCycles);
  CHECK(h.slot_reqs > r4 && d->state_o == 1, "U24: premise, the ANNOUNCE is being built");
  d->link_up_i = 0;                                    // Release!
  valid_after = false;
  for (int c = 0; c < kBounceCycles; ++c) {
    h.step();
    if (d->addr_valid_o) valid_after = true;
  }
  d->link_up_i = 1;                                    // PortOperational!
  for (int c = 0; c < kWatchCycles; ++c) {
    h.step();
    if (d->addr_valid_o) valid_after = true;
  }
  CHECK(!valid_after, "U24: announce path, no claim is valid after the fall");
  CHECK(h.wait_frames(n4 + 3, kProbeBudgetMs) && d->state_o == 1,
        "U24: announce path, the ANNOUNCE drains and a fresh walk probes (%zu frames, state %u)",
        h.tx.size() - n4, unsigned(d->state_o));
  if (h.tx.size() >= n4 + 3) {
    CHECK(h.tx[n4 + 1].b == maap_frame(MAAP_DA, OWN_MAC, 3, b4, COUNT, 0, 0),
          "U24: announce path, the drained ANNOUNCE is byte-exact");
    CHECK(h.tx[n4 + 2].b == maap_frame(MAAP_DA, OWN_MAC, 1, d->addr_o, COUNT, 0, 0),
          "U24: announce path, the fresh walk's PROBE is byte-exact");
  }
  h.confl_auto = false;
  h.confl_srcs.clear();
}

// ---- U25: a 100 ms outage while the lane grants nothing -------------------
// W_LANE holds its request until the lane grants, and an egress may stall
// while the link is down. A Release! must still withdraw the claim at once
// (B.3.5.2: no longer defended, so the seam refuses), the frame drains when
// the lane returns, and the PortOperational! gives a fresh walk.
void MaapAnnexBSuite::an_outage_behind_a_stalled_lane_withdraws_the_claim() {
  constexpr int kOutageMs = 100;
  h.confl_auto = true;
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  CHECK(d->addr_valid_o && d->state_o == 2, "U25: premise, DEFEND state");
  const uint64_t b = d->addr_o;
  const size_t n0 = h.tx.size();
  const uint64_t prober = 0x0A2233445566ull;
  h.lane_hold = true;                                  // the egress stops granting
  CHECK(accept_record(1, prober, b + 2, 2), "U25: the rProbe! is taken");
  h.idle(80);                                          // the DEFEND is built and waits
  CHECK(d->txreq_valid_o && d->addr_valid_o, "U25: premise, the DEFEND waits for the lane");
  d->link_up_i = 0;                                    // Release!
  h.step();
  int valid_cycles = 0;
  for (int c = 0; c < kOutageMs * kClkPerMs; ++c) {
    h.step();
    if (d->addr_valid_o) ++valid_cycles;
  }
  CHECK(valid_cycles == 0 && d->state_o == 0,
        "U25: the claim is withdrawn for the whole outage (%d cycles valid, state %u)",
        valid_cycles, unsigned(d->state_o));
  auto r = h.alloc(0);
  CHECK(r.got && !r.ok, "U25: an ALLOC during the outage is refused");
  d->link_up_i = 1;                                    // PortOperational!
  h.idle(5);
  h.lane_hold = false;                                 // the egress drains again
  CHECK(h.wait_frames(n0 + 2, kProbeBudgetMs) && d->state_o == 1 && !d->addr_valid_o,
        "U25: the DEFEND drains and a fresh walk probes (%zu frames, state %u)",
        h.tx.size() - n0, unsigned(d->state_o));
  if (h.tx.size() >= n0 + 2) {
    CHECK(h.tx[n0].b == maap_frame(prober, OWN_MAC, 2, b + 2, 2, b + 2, 2),
          "U25: the drained DEFEND is byte-exact");
    CHECK(h.tx[n0 + 1].b == maap_frame(MAAP_DA, OWN_MAC, 1, d->addr_o, COUNT, 0, 0),
          "U25: the fresh walk's PROBE is byte-exact");
  }
  h.confl_auto = false;
  h.confl_srcs.clear();
}

// ---- U26: a Release! of one to four cycles is never absorbed --------------
// The fall is seen for at most a few cycles. (a) In W_IDLE, for 1 to 4
// cycles: the teardown outlasts the shortest falls, so the rise lands before
// the machine is parked. (b) One cycle exactly on the W_RX cycle of an
// ignored record. Each must give INITIAL and a fresh walk whose first PROBE
// follows at once, not the old walk's next PROBE a probe interval later.
void MaapAnnexBSuite::a_short_release_is_never_absorbed() {
  constexpr int kFreshWalkMs = 50;                     // a fresh walk's first PROBE
  for (int n = 1; n <= 5; ++n) {
    const bool on_rx = (n == 5);
    const char* arc = on_rx ? "W_RX" : "W_IDLE";
    const int low = on_rx ? 1 : n;
    d->link_up_i = 0; h.idle(30);                      // Release!
    size_t n0 = h.tx.size();
    d->link_up_i = 1;                                  // PortOperational!
    CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs), "U26: %s %d, premise, a walk", arc, low);
    h.run_ms(20);                                      // parked in W_IDLE, PROBE state
    if (on_rx) {
      // a PROBE far from our block: ignored (footnote b), but it passes W_RX
      CHECK(accept_record(1, 0x0A2233445566ull, d->addr_o + 0x100, 1),
            "U26: %s, premise, the record is taken", arc);
    }
    n0 = h.tx.size();
    d->link_up_i = 0;                                  // Release!
    bool initial = false;
    for (int c = 0; c < low; ++c) {
      h.step();
      if (d->state_o == 0) initial = true;
    }
    d->link_up_i = 1;                                  // PortOperational!
    for (int c = 0; c < 10 && !initial; ++c) {
      h.step();
      if (d->state_o == 0) initial = true;
    }
    CHECK(initial, "U26: %s, a %d-cycle Release! gives INITIAL", arc, low);
    CHECK(h.wait_frames(n0 + 1, kFreshWalkMs) && d->state_o == 1,
          "U26: %s, a %d-cycle Release! is followed by a fresh walk's PROBE", arc, low);
    if (h.tx.size() > n0) {
      CHECK(h.tx[n0].b == maap_frame(MAAP_DA, OWN_MAC, 1, d->addr_o, COUNT, 0, 0),
            "U26: %s %d, the fresh walk's PROBE is byte-exact", arc, low);
    }
  }
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
}

// ---- U27: every Release! re-arms the footnote-a seed ----------------------
// Footnote a lets a Begin!/PortOperational! reuse a provisioned range. The
// engine's rule: the seed is probed first in every engagement, and a
// conflict inside one engagement is never answered with the seed again
// (U15). A seeded walk is contested (PROBE / rAnnounce!, a yield with no
// tie-break), and the Release! lands on each of the two arcs: while
// generate_address redraws for the Restart!, and after the yield-walk's
// first PROBE. The next engagement probes the seed both times.
void MaapAnnexBSuite::every_release_rearms_the_seed() {
  constexpr unsigned kSeed = 0x2000;
  const uint64_t hi_mac = 0xF2FFEEDDCCFFull;           // PROBE/rAnnounce! yields anyway
  h.confl_auto = true;
  for (int arc = 0; arc < 2; ++arc) {
    const char* tag = arc ? "the W_IDLE arc" : "the W_ADDR arc";
    d->link_up_i = 0; h.idle(30);                      // Release!
    h.addr_script.clear();
    d->cfg_count_i = COUNT;
    d->cfg_seed_offset_i = kSeed;
    d->cfg_seed_valid_i = 1;
    size_t n0 = h.tx.size();
    d->link_up_i = 1;                                  // PortOperational!
    CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs) && d->addr_o == (POOL_HI | kSeed),
          "U27: %s, premise, the seed is probed first", tag);
    if (arc == 0) h.addr_script = {uint16_t(POOL_SIZE - 1)};   // hold the redraw loop
    h.addr_draws = 0;
    n0 = h.tx.size();
    h.rx(3, hi_mac, POOL_HI | kSeed, COUNT);           // the seed is contested
    if (arc == 0) {
      for (int g = 0; g < 2000 && h.addr_draws < 3; ++g) h.step();
      CHECK(h.addr_draws >= 3 && h.tx.size() == n0 && d->state_o == 0,
            "U27: %s, premise, generate_address is redrawing (%zu draws)", tag, h.addr_draws);
    } else {
      CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs) && d->addr_o != (POOL_HI | kSeed),
            "U27: %s, premise, the Restart! probes a fresh range", tag);
    }
    d->link_up_i = 0; h.idle(30);                      // Release!
    h.addr_script.clear();
    n0 = h.tx.size();
    d->link_up_i = 1;                                  // PortOperational!
    CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs) && d->addr_o == (POOL_HI | kSeed),
          "U27: %s, the next engagement probes the seed first (got %012llx)", tag,
          static_cast<unsigned long long>(d->addr_o));
    if (h.tx.size() > n0) {
      CHECK(h.tx[n0].b == maap_frame(MAAP_DA, OWN_MAC, 1, POOL_HI | kSeed, COUNT, 0, 0),
            "U27: %s, the seeded PROBE is byte-exact", tag);
    }
  }
  d->cfg_seed_valid_i = 0;
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  h.confl_auto = false;
  h.confl_srcs.clear();
}

// ---- U28: no PDU is generated after the fall, in any walker state ---------
// Table B.7 Release!: PROBE stops probe_timer, DEFEND stops announce_timer,
// both go to INITIAL, and no Release! cell carries sProbe, sAnnounce or
// sDefend. B.3.1 c) and e): a stopped timer does not expire, so no
// probeTimer! or announceTimer! follows the Release!. Each point below
// lands the fall so that the walker first sees it in one state, then holds
// the link down for longer than the longest announce interval (B.3.4.1).
// From the cycle after the first one the fall is seen: no TX slot request,
// no timer started, no timer expiry, no claim. A frame whose slot was
// requested by then belongs to the entry that requested it (B.3.2), and is
// the one frame that may still drain (U23 grades its window).

// KL_pp_maap's walker encoding (wstate_e), read through walker_o only to
// land a fall in a chosen state, never as an expectation
enum WalkerState : unsigned {
  WK_OFF, WK_IDLE, WK_ADDR, WK_IVAL, WK_ALLOC, WK_GWAIT, WK_WRITE, WK_COMMIT,
  WK_LANE, WK_POST, WK_RX, WK_TEARDOWN, WK_STATES
};

const char* const kWalkerName[WK_STATES] = {
    "W_OFF", "W_IDLE", "W_ADDR", "W_IVAL", "W_ALLOC", "W_GWAIT",
    "W_WRITE", "W_COMMIT", "W_LANE", "W_POST", "W_RX", "W_TEARDOWN"};

// how a fall point is reached after the PortOperational! that starts its walk
enum class Poise {
  kRunTo,    // clock until the walker enters the state
  kPark,     // 20 ms after the first PROBE, or after the claim
  kExpiry,   // then clock until one of the engine's timers expires
  kRecord,   // parked, then one record is taken: the walker is in W_RX
  kBounce,   // parked, then the link falls for one cycle and rises again
};

struct FallPoint {
  const char* what;
  unsigned walker;                                     // the state that sees the fall
  Poise how;
  size_t grants;                                       // lane grants first (kRunTo, kExpiry)
  bool defend;                                         // parked in DEFEND, not PROBE
};

const FallPoint kFallPoints[] = {
    {"generate_address", WK_ADDR, Poise::kRunTo, 0, false},
    {"the first PROBE's interval draw", WK_IVAL, Poise::kRunTo, 0, false},
    {"the first PROBE's slot request", WK_ALLOC, Poise::kRunTo, 0, false},
    {"the first PROBE's slot grant", WK_GWAIT, Poise::kRunTo, 0, false},
    {"the first PROBE's byte writes", WK_WRITE, Poise::kRunTo, 0, false},
    {"the first PROBE's commit", WK_COMMIT, Poise::kRunTo, 0, false},
    {"the first PROBE's lane request", WK_LANE, Poise::kRunTo, 0, false},
    {"the first PROBE taken by the lane", WK_POST, Poise::kRunTo, 0, false},
    {"PROBE, parked", WK_IDLE, Poise::kPark, 0, false},
    {"PROBE, the expiry that sends the 4th PROBE latched", WK_IDLE, Poise::kExpiry, 3, false},
    {"PROBE, an ignored record", WK_RX, Poise::kRecord, 0, false},
    {"a rise inside the teardown, its last cycle", WK_TEARDOWN, Poise::kBounce, 0, false},
    {"a rise inside the teardown, the first W_OFF cycle", WK_OFF, Poise::kBounce, 0, false},
    {"probeCount!, the ANNOUNCE's interval draw", WK_IVAL, Poise::kRunTo, 4, false},
    {"DEFEND, parked", WK_IDLE, Poise::kPark, 0, true},
    {"DEFEND, an announce_timer expiry latched", WK_IDLE, Poise::kExpiry, 0, true},
    {"DEFEND, an rProbe! that sDefend would answer", WK_RX, Poise::kRecord, 0, true},
};

// clock until the walker's next edge executes state `st`
bool MaapAnnexBSuite::run_to_walker(unsigned st, long budget_cycles) {
  for (long c = 0; c < budget_cycles && d->walker_o != st; ++c) h.step();
  return d->walker_o == st;
}

// Release!, PortOperational!, then clock until the walker's next edge
// executes the state the point names, with the link still up
bool MaapAnnexBSuite::poise_a_fall(int point) {
  const FallPoint& f = kFallPoints[point];
  const long walk = 6L * kProbeBudgetMs * kClkPerMs;   // a whole 4-probe walk
  d->link_up_i = 0; h.idle(30);                        // Release!
  const size_t g0 = h.grants;
  d->link_up_i = 1;                                    // PortOperational!
  for (long c = 0; c < walk && h.grants < g0 + f.grants; ++c) h.step();
  if (h.grants != g0 + f.grants) return false;
  if (f.how == Poise::kRunTo) return run_to_walker(f.walker, walk);
  if (f.defend) {
    for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
    if (!d->addr_valid_o) return false;
  } else if (f.how != Poise::kExpiry && !h.wait_frames(h.tx.size() + 1, kProbeBudgetMs)) {
    return false;
  }
  if (f.how == Poise::kExpiry) {
    // latched at this edge; the walker serves it at its next one
    const size_t e0 = h.tmr_expiries;
    for (int c = 0; c < kAnnounceBudgetMs * kClkPerMs && h.tmr_expiries == e0; ++c) h.step();
    return h.tmr_expiries > e0 && d->walker_o == f.walker;
  }
  h.run_ms(20);                                        // parked
  if (d->walker_o != WK_IDLE) return false;
  if (f.how == Poise::kRecord) {
    // DEFEND: a PROBE over our block (sDefend); PROBE: one far from it
    const uint64_t start = d->addr_o + (f.defend ? 2 : 0x100);
    if (!accept_record(1, 0x0A2233445566ull, start, f.defend ? 2 : 1)) return false;
  } else if (f.how == Poise::kBounce) {
    d->link_up_i = 0; h.step();                        // Release!, seen in W_IDLE
    d->link_up_i = 1; h.step();                        // PortOperational! in the teardown
    if (f.walker == WK_OFF) h.step();                  // ... and through its last cycle
  }
  return d->walker_o == f.walker;
}

void MaapAnnexBSuite::no_pdu_is_generated_after_the_fall() {
  constexpr int kWatchCycles = kAnnounceBudgetMs * kClkPerMs;
  constexpr int kPoints = static_cast<int>(std::size(kFallPoints));
  int missed = 0;
  int requested = 0;
  int overdrained = 0;
  int started = 0;
  int expired = 0;
  int claimed = 0;
  unsigned covered = 0;
  h.confl_auto = true;
  for (int p = 0; p < kPoints; ++p) {
    const FallPoint& f = kFallPoints[p];
    if (!poise_a_fall(p)) {
      ++missed;
      printf("  U28: %s: the walker is in %s, not %s\n", f.what,
             d->walker_o < WK_STATES ? kWalkerName[d->walker_o] : "?", kWalkerName[f.walker]);
      continue;
    }
    covered |= 1u << f.walker;
    d->link_up_i = 0;                                  // Release!
    h.step();                                          // the first cycle it is seen
    const size_t reqs = h.slot_reqs;
    const size_t grants = h.grants;
    const size_t starts = h.tmr_starts;
    const size_t expiries = h.tmr_expiries;
    const size_t owed = reqs - grants;                 // requested by now, not yet on the lane
    bool valid_after = d->addr_valid_o;
    for (int c = 1; c < kWatchCycles; ++c) {
      h.step();
      if (d->addr_valid_o) valid_after = true;
    }
    printf("  U28: %s (%s): %zu frame(s) owed, %zu drained\n", f.what,
           kWalkerName[f.walker], owed, h.grants - grants);
    if (h.slot_reqs != reqs) ++requested;
    if (owed > 1 || h.grants - grants != owed) ++overdrained;
    if (h.tmr_starts != starts) ++started;
    if (h.tmr_expiries != expiries) ++expired;
    if (valid_after || d->state_o != 0) ++claimed;
  }
  CHECK(missed == 0, "U28: premise, each fall is first seen in its walker state (%d of %d missed)",
        missed, kPoints);
  CHECK(covered == (1u << WK_STATES) - 1,
        "U28: premise, the falls land in all %d walker states (mask 0x%03x)", WK_STATES, covered);
  CHECK(requested == 0,
        "U28: no TX slot request follows the fall, in any walker state, for 33 s (%d of %d falls)",
        requested, kPoints);
  CHECK(overdrained == 0,
        "U28: at most the one frame requested before the fall drains (%d of %d falls)",
        overdrained, kPoints);
  CHECK(started == 0, "U28: no timer is started after the fall (%d of %d falls)", started, kPoints);
  CHECK(expired == 0,
        "U28: the timers are stopped: no expiry for 33 s after the fall (%d of %d falls)",
        expired, kPoints);
  CHECK(claimed == 0, "U28: no claim after the fall, and INITIAL (%d of %d falls)",
        claimed, kPoints);
  const size_t n0 = h.tx.size();
  d->link_up_i = 1;                                    // PortOperational!
  CHECK(h.wait_frames(n0 + 1, kProbeBudgetMs) && d->state_o == 1,
        "U28: the next PortOperational! probes");
  for (int g = 0; g < kWalkRetryRounds && !d->addr_valid_o; ++g) h.run_ms(kProbeBudgetMs);
  h.confl_auto = false;
  h.confl_srcs.clear();
}

int MaapAnnexBSuite::run() {
  reset_leaves_the_machine_initial();
  engage_probes_a_fresh_pool_range();
  cold_walk_retransmits_then_announces();
  announce_cadence_holds_inside_the_bounds();
  allocator_seam_serves_the_valid_claim();
  probe_over_our_block_is_defended();
  probe_from_below_names_the_first_allocated_address();
  non_overlapping_probe_is_ignored();
  announce_from_a_higher_peer_is_ignored();
  announce_from_a_lower_peer_yields_and_reprobes();
  probe_from_a_higher_peer_leaves_our_walk_unmoved();
  defend_during_probe_yields_without_tie_break();
  reserved_message_types_change_nothing();
  empty_range_never_conflicts();
  higher_maap_version_is_still_processed();
  release_parks_the_machine_without_a_pdu();
  provisioned_seed_probes_first_and_is_not_reused();
  alloc_is_refused_while_probing();
  overhanging_draws_are_redrawn_until_the_block_fits();
  a_release_mid_redraw_leaves_no_draw_behind();
  a_release_during_a_fitting_draw_abandons_it();
  a_seed_past_the_fit_is_clamped_into_the_pool();
  the_seed_clamp_boundary_is_exact();
  // the Release! arcs each start from a Release! of their own
  a_release_on_the_announce_path_is_ordered_by_the_slot_request();
  a_bounce_inside_the_tx_path_restarts_the_walk();
  an_outage_behind_a_stalled_lane_withdraws_the_claim();
  a_short_release_is_never_absorbed();
  every_release_rearms_the_seed();
  no_pdu_is_generated_after_the_fall();
  probe_from_a_lower_peer_yields_the_walk();
  defend_from_a_higher_peer_is_ignored();
  defend_from_a_lower_peer_yields_the_claim();
  announce_during_probe_yields_without_tie_break();

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  return fails ? 1 : 0;
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  MaapAnnexBSuite suite;
  return suite.run();
}
