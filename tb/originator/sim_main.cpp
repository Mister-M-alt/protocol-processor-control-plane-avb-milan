// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_originator suite — independent expectations, never DUT logic.
//
// Proves the 03 §5 originator + inflight table against a C++ stub of the
// KL_pp_timer_service arm/expiry port protocol (arm = {slot, owner,
// absolute-ms deadline}, cancel clears, a fired slot self-disarms) and a
// hold/release tally standing in for the KL_pp_tx_slots pool. Covered:
// issue -> serializer acceptance -> timer arm -> response routes + frees;
// per-owner seq counters; queue delay never consumes an attempt timeout;
// timeout -> ONE exact re-send (same held slot, same seq — the original seq
// still routes after the retry), whose timer waits for retry acceptance;
// second timeout fails; responses after fail and
// mismatched {seq}/{key} are silently ignored but counted (F09.4, 8-bit
// wrap); interleaved inflights route independently; table-full refusal +
// reuse of the freed id; response-beats-expiry races (other entry and same
// entry); stale/foreign expiry tags are inert; arm slots always legal and
// every hold is released exactly once.
#include <cstdint>
#include <cstdio>
#include <vector>
#include "VKL_pp_originator.h"
#include "verilated.h"
#include "../common/verilator_harness.hpp"
#include "../common/req_tag.hpp"

#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails_n; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

constexpr int NTMR = 89;            // pp_pkg::PP_TIMER_SLOTS_C
constexpr uint8_t TAG = 0xC0;       // TMR_TAG_P << 4
constexpr int NIFL = 12;            // INFLIGHT_P default = 4 CA + 8 probe
constexpr int NSLOT = 8;            // hold/release slot handle is 3 bits wide
constexpr int IGN_CNT_MAX = 255;    // rsp_ign_cnt_o is an 8-bit counter

struct ArmOp { bool cancel; uint8_t slot; uint8_t owner; uint32_t deadline; };
struct Ev { uint8_t owner; uint8_t id; };
//! field-wise equality, so a pulse log compares against a model's prediction
static bool operator==(const ArmOp& a, const ArmOp& b) {
  return a.cancel == b.cancel && a.slot == b.slot && a.owner == b.owner
         && a.deadline == b.deadline;
}
static bool operator==(const Ev& a, const Ev& b) {
  return a.owner == b.owner && a.id == b.id;
}

struct Harness {
  VKL_pp_originator* dut;
  uint32_t now = 1000;

  // ---- timer-service stub (the arm-port protocol model) ----
  struct TSlot { bool armed = false; uint8_t owner = 0; uint32_t deadline = 0; };
  TSlot tmr[NTMR];
  int arms = 0;
  int cancels = 0;
  bool oob = false;
  std::vector<ArmOp> armops;

  // ---- pool stand-in: hold/release tally ----
  int held[NSLOT] = {0};
  int holds = 0;
  int releases = 0;
  bool neg_release = false;

  // cumulative event logs + per-tick pulse flags
  std::vector<Ev> rts;
  std::vector<Ev> fls;
  std::vector<uint8_t> sends;
  std::vector<uint8_t> resends;
  bool p_rt = false;
  bool p_resend = false;
  bool p_fail = false;

  // per-tick comb handshake snapshot
  bool ready = false;
  bool gnt = false;
  uint8_t id = 0;
  uint16_t seq = 0;

  explicit Harness(VKL_pp_originator* d) : dut(d) {}

  void clr() {
    dut->iss_valid_i = 0;
    dut->cancel_valid_i = 0;
    dut->rsp_valid_i = 0;
    dut->exp_valid_i = 0;
    dut->send_accept_valid_i = 0;
  }

  void tick() {
    dut->now_ms_i = now;
    dut->clk_i = 0; dut->eval();
    // pre-edge observation window
    ready = dut->iss_ready_o; gnt = dut->iss_gnt_o;
    id = dut->iss_id_o; seq = dut->iss_seq_o;
    p_rt = dut->rt_valid_o; p_resend = dut->resend_valid_o;
    p_fail = dut->fail_valid_o;
    if (dut->tmr_arm_valid_o) {
      ArmOp op{static_cast<bool>(dut->tmr_arm_cancel_o),
               static_cast<uint8_t>(dut->tmr_arm_slot_o),
               static_cast<uint8_t>(dut->tmr_arm_owner_o),
               static_cast<uint32_t>(dut->tmr_arm_deadline_ms_o)};
      armops.push_back(op);
      if (op.slot >= NTMR) { oob = true; }
      else if (op.cancel) { ++cancels; tmr[op.slot].armed = false; }
      else { ++arms; tmr[op.slot] = {true, op.owner, op.deadline}; }
    }
    if (dut->hold_valid_o) { ++held[dut->hold_slot_o & (NSLOT - 1)]; ++holds; }
    if (dut->release_valid_o) {
      if (--held[dut->release_slot_o & (NSLOT - 1)] < 0) neg_release = true;
      ++releases;
    }
    if (dut->send_valid_o)
      sends.push_back(static_cast<uint8_t>(dut->send_slot_o));
    if (dut->resend_valid_o)
      resends.push_back(static_cast<uint8_t>(dut->resend_slot_o));
    if (dut->rt_valid_o)
      rts.push_back({static_cast<uint8_t>(dut->rt_owner_o),
                     static_cast<uint8_t>(dut->rt_id_o)});
    if (dut->fail_valid_o)
      fls.push_back({static_cast<uint8_t>(dut->fail_owner_o),
                     static_cast<uint8_t>(dut->fail_id_o)});
    dut->clk_i = 1; dut->eval();
  }

  void idle(int n = 1) { clr(); for (int i = 0; i < n; ++i) tick(); }

  void accept(uint8_t slot) {
    clr();
    dut->send_accept_valid_i = 1;
    dut->send_accept_slot_i = slot;
    tick();
    dut->send_accept_valid_i = 0;
  }

  struct Res { bool gnt; uint8_t id; uint16_t seq; };
  Res issue(uint8_t owner, uint8_t txslot, uint16_t key,
            uint8_t tmrslot, uint16_t to_ms, bool auto_accept = true) {
    clr();
    dut->iss_valid_i = 1; dut->iss_owner_i = owner;
    dut->iss_tx_slot_i = txslot; dut->iss_key_i = key;
    dut->iss_tmr_slot_i = tmrslot; dut->iss_timeout_ms_i = to_ms;
    tick();
    Res r{gnt, id, seq};
    dut->iss_valid_i = 0;
    if (r.gnt && auto_accept) {
      accept(txslot);
      idle(2);  // acceptance is parked, then serviced through the timer port
    }
    return r;
  }

  uint16_t preview(uint8_t owner) {          // iss_seq_o with valid low
    clr(); dut->iss_owner_i = owner; tick(); return seq;
  }

  void respond(uint16_t key, uint16_t s) {
    clr();
    dut->rsp_valid_i = 1; dut->rsp_key_i = key; dut->rsp_seq_i = s;
    tick();
    dut->rsp_valid_i = 0;
  }

  void cancel(uint8_t owner) {
    clr();
    dut->cancel_valid_i = 1;
    dut->cancel_owner_i = owner;
    tick();
    dut->cancel_valid_i = 0;
  }

  void fire(uint8_t slot, uint8_t owner) {   // one expiry pulse on the bus
    clr();
    dut->exp_valid_i = 1; dut->exp_slot_i = slot; dut->exp_owner_i = owner;
    tick();
    dut->exp_valid_i = 0;
  }

  // stub sweep step: fire the lowest armed slot whose deadline has passed
  // (self-disarm on fire, like the RTL timer); -1 if none due
  int expire_due() {
    for (int s = 0; s < NTMR; ++s)
      if (tmr[s].armed && static_cast<int32_t>(now - tmr[s].deadline) >= 0) {
        tmr[s].armed = false;
        fire(static_cast<uint8_t>(s), tmr[s].owner);
        return s;
      }
    return -1;
  }

  int armed_count() const {
    int n = 0;
    for (auto& t : tmr) if (t.armed) ++n;
    return n;
  }
  int held_total() const {
    int n = 0;
    for (int i = 0; i < NSLOT; ++i) n += held[i];
    return n;
  }
};

// ---- R: the independent inflight model (issue #86 acceptance 3) ---------
// Built from the 03 section 5 contract and this module's banner, never from
// its code: an issue takes the lowest free entry, the owner's next sequence
// id, a hold and a send of its TX slot; the serializer's acceptance arms
// {timer slot, {tag, id}, now + timeout}; a {key, seq} response routes to
// the owner, releases the slot and disarms the timer; the first expiry
// re-sends the same held slot and re-arms only at that retry's acceptance;
// the second fails to the owner and releases with no disarm (the slot
// self-disarmed); an owner's cancellation releases and disarms; anything
// else that answers is ignored and counted.
namespace {
struct IflModel {
  struct Entry {
    bool live = false;
    uint8_t owner = 0;
    uint16_t key = 0;
    uint16_t seq = 0;
    uint8_t tx = 0;
    uint8_t tmr = 0;
    uint16_t timeout = 0;
    bool retried = false;
    bool armed = false;
    bool await = false;
  };
  Entry e[NIFL];
  uint16_t next_seq[16] = {0};
  uint8_t ignored = 0;

  int live_of(uint8_t owner) const {
    for (int i = 0; i < NIFL; ++i) if (e[i].live && e[i].owner == owner) return i;
    return -1;
  }
  int lowest_free() const {
    for (int i = 0; i < NIFL; ++i) if (!e[i].live) return i;
    return -1;
  }
  bool tx_busy(uint8_t tx) const {
    for (const auto& x : e) if (x.live && x.tx == tx) return true;
    return false;
  }
  uint16_t busy() const {
    uint16_t b = 0;
    for (int i = 0; i < NIFL; ++i) if (e[i].live) b = static_cast<uint16_t>(b | (1u << i));
    return b;
  }
};

//! what one event must produce on the module's pulse lanes
struct IflWant {
  std::vector<Ev> rts;
  std::vector<Ev> fls;
  std::vector<uint8_t> sends;
  std::vector<uint8_t> resends;
  std::vector<ArmOp> arms;
  int holds = 0;
  int releases = 0;
};
}  // namespace

// ---- the suite ----------------------------------------------------------
// One object owns the model, the timer-service stub and the tally, so every
// phase below is a named member function rather than another stretch of a
// `main` nobody can scope (Core Guidelines I.2, F.3).
namespace {
class OriginatorSuite {
 public:
  int run();

 private:
  void reset_leaves_the_table_empty();
  void issue_then_response_routes_and_frees();
  void freed_id_is_reused_and_the_timeout_is_plumbed();
  void timeout_resends_once_and_the_original_seq_still_routes();
  void second_timeout_fails_and_a_late_response_is_counted();
  void mismatched_seq_or_key_is_ignored_and_counted();
  void interleaved_inflights_route_independently();
  void table_full_refuses_then_reuses_the_freed_id();
  void response_to_another_entry_wins_over_a_pending_expiry();
  void same_entry_response_beats_its_own_expiry();
  void stale_expiry_is_inert();
  void foreign_owner_tag_is_inert();
  void cancellation_frees_the_held_slot_and_the_timer();
  void queue_delay_never_consumes_the_attempt_budget();
  void acceptance_survives_another_entrys_response();
  void cancellation_survives_another_entrys_response();
  void the_ignored_counter_wraps_at_eight_bits();
  void final_invariants_hold();
  // R: the seeded session against IflModel
  uint32_t next_random();
  void r_issue(uint8_t owner);
  void r_accept(int id);
  void r_respond(int id);
  void r_stray_response();
  void r_expire();
  void r_cancel(uint8_t owner);
  void r_judge(const IflWant& want, const char* what);
  void r_step();
  void r_drain();
  void a_seeded_session_matches_the_inflight_model();

  const milan::tb::Model<VKL_pp_originator> model;
  VKL_pp_originator* const dut = model.get();
  Harness h{dut};
  int checks = 0;
  int fails_n = 0;
  // independent per-owner seq mirror
  uint16_t mseq[16] = {0};
  // G2's entry and its retry/fail cursors: G3 and G4 fire against that same
  // entry and assert nothing has moved since, so all three outlive G2.
  Harness::Res g3{};
  size_t g_r0 = 0;
  size_t g_f0 = 0;
  // R's model, its seeded generator, and what the session exercised
  IflModel m;
  uint32_t rng = 0x86A46301u;
  uint16_t r_key = 0x4000;
  long r_divergences = 0;
  long r_events = 0;
  long r_routes = 0;
  long r_retries = 0;
  long r_fails = 0;
  long r_cancels = 0;
  long r_strays = 0;
  int r_peak = 0;
};
}  // namespace

void OriginatorSuite::reset_leaves_the_table_empty() {
  dut->rst_n = 0; h.clr();
  dut->iss_owner_i = 0; dut->iss_tx_slot_i = 0; dut->iss_key_i = 0;
  dut->iss_tmr_slot_i = 0; dut->iss_timeout_ms_i = 0;
  dut->cancel_owner_i = 0;
  dut->rsp_seq_i = 0; dut->rsp_key_i = 0;
  dut->exp_slot_i = 0; dut->exp_owner_i = 0;
  dut->send_accept_slot_i = 0;
  for (int i = 0; i < 4; ++i) h.tick();
  dut->rst_n = 1;
  h.idle(1);
  CHECK(h.ready, "ready after reset");
  CHECK(dut->inflight_busy_o == 0, "no inflight after reset");
  CHECK(dut->rsp_ign_cnt_o == 0, "ign counter zero after reset");
}

// ---- A: issue -> respond routes + frees -----------------------------
void OriginatorSuite::issue_then_response_routes_and_frees() {
  auto a = h.issue(2, 1, 0x0A11, 40, 200);
  CHECK(a.gnt, "A issue granted");
  CHECK(a.seq == mseq[2], "A seq from owner-2 counter got %u", a.seq);
  mseq[2]++;
  CHECK(a.id == 0, "A lowest free id got %u", a.id);
  h.idle(1);
  CHECK(h.sends.size() == 1 && h.sends[0] == 1, "A send pulse for tx slot 1");
  CHECK(h.holds == 1 && h.held[1] == 1, "A hold pinned tx slot 1");
  CHECK(h.arms == 1 && h.resends.empty(), "A one arm, no resend");
  CHECK(!h.armops.back().cancel && h.armops.back().slot == 40,
        "A armed timer slot 40 got %u", h.armops.back().slot);
  CHECK(h.armops.back().owner == (TAG | a.id), "A arm owner tag {0xC, id}");
  CHECK(h.armops.back().deadline == h.now + 200,
        "A deadline now+200 got %u", h.armops.back().deadline);
  CHECK(dut->inflight_busy_o == 0x001, "A busy bit set");
  CHECK(h.preview(2) == mseq[2], "A owner-2 counter advanced");
  CHECK(h.preview(5) == 0, "A owner-5 counter untouched");
  h.respond(0x0A11, a.seq);
  h.idle(2);
  CHECK(h.rts.size() == 1 && h.rts[0].owner == 2 && h.rts[0].id == a.id,
        "A response routed to owner 2 id 0");
  CHECK(h.releases == 1 && h.held[1] == 0, "A slot released on route");
  CHECK(h.cancels == 1 && h.armops.back().cancel
        && h.armops.back().slot == 40, "A timer disarmed on route");
  CHECK(dut->inflight_busy_o == 0, "A entry freed");
  CHECK(dut->rsp_ign_cnt_o == 0, "A matched response not counted");
  CHECK(h.armed_count() == 0, "A no armed slot leaked");
}

// ---- B: freed id reused; timeout value plumbed through --------------
void OriginatorSuite::freed_id_is_reused_and_the_timeout_is_plumbed() {
  auto b = h.issue(5, 2, 0x0B22, 41, 250);
  CHECK(b.gnt && b.id == 0, "B reuses freed id 0");
  CHECK(b.seq == 0, "B owner-5 first seq");
  mseq[5]++;
  h.idle(1);
  CHECK(h.armops.back().deadline == h.now + 250,
        "B deadline now+250 (timeout from the issue port)");
  h.respond(0x0B22, b.seq);
  h.idle(2);
  CHECK(h.rts.back().owner == 5, "B routed to owner 5");
}

// ---- C1: timeout -> ONE exact re-send, original seq still routes ----
void OriginatorSuite::timeout_resends_once_and_the_original_seq_still_routes() {
  size_t s0 = h.sends.size();
  size_t r0 = h.resends.size();
  auto c = h.issue(7, 3, 0x0777, 50, 200);
  CHECK(c.gnt, "C1 granted");
  h.idle(1);
  h.now += 200;
  CHECK(h.expire_due() == 50, "C1 expiry fired on slot 50");
  int arms0 = h.arms;
  h.idle(2);
  CHECK(h.resends.size() == r0 + 1 && h.resends.back() == 3,
        "C1 one re-send of the HELD slot 3");
  CHECK(h.sends.size() == s0 + 1, "C1 no fresh send on retry");
  CHECK(h.arms == arms0, "C1 retry queue delay did not arm timer");
  h.accept(3);
  h.idle(2);
  CHECK(!h.armops.back().cancel && h.armops.back().slot == 50
        && h.armops.back().deadline == h.now + 200,
        "C1 re-armed same slot after retry acceptance");
  CHECK((dut->inflight_busy_o & (1u << c.id)) != 0, "C1 still inflight");
  h.respond(0x0777, c.seq);   // the ORIGINAL seq — never re-assigned
  h.idle(2);
  CHECK(h.rts.back().owner == 7 && h.rts.back().id == c.id,
        "C1 original seq routes after the exact duplicate");
  CHECK(h.held[3] == 0 && h.armed_count() == 0, "C1 released + disarmed");
}

// ---- C2: second timeout fails; response after fail ignored+counted --
void OriginatorSuite::second_timeout_fails_and_a_late_response_is_counted() {
  size_t r0 = h.resends.size();
  size_t f0 = h.fls.size();
  int cx0 = h.cancels;
  auto d = h.issue(4, 2, 0x0444, 51, 200);
  h.idle(1);
  h.now += 200;
  CHECK(h.expire_due() == 51, "C2 first expiry");
  h.idle(2);
  CHECK(h.resends.size() == r0 + 1, "C2 retry sent");
  int arms0 = h.arms;
  h.now += 1000;
  CHECK(h.expire_due() == -1, "C2 queued retry has no running timer");
  CHECK(h.arms == arms0, "C2 queued retry did not re-arm");
  h.accept(2);
  h.idle(2);
  h.now += 200;
  CHECK(h.expire_due() == 51, "C2 second expiry (re-armed slot fired)");
  h.idle(2);
  CHECK(h.fls.size() == f0 + 1 && h.fls.back().owner == 4
        && h.fls.back().id == d.id, "C2 fail to owner 4");
  CHECK(h.resends.size() == r0 + 1, "C2 no second re-send");
  CHECK(h.held[2] == 0, "C2 slot released on fail");
  CHECK(h.cancels == cx0, "C2 no cancel on fail (slot self-disarmed)");
  CHECK(dut->inflight_busy_o == 0, "C2 entry freed");
  uint8_t cnt0 = dut->rsp_ign_cnt_o;
  size_t rt0 = h.rts.size();
  h.respond(0x0444, d.seq);
  h.idle(2);
  CHECK(h.rts.size() == rt0, "C2 response after fail not routed");
  CHECK(dut->rsp_ign_cnt_o == static_cast<uint8_t>(cnt0 + 1),
        "C2 response after fail counted");
}

// ---- D: mismatched {seq} / {key} ignored + counted ------------------
void OriginatorSuite::mismatched_seq_or_key_is_ignored_and_counted() {
  auto e = h.issue(9, 4, 0x0E55, 60, 200);
  h.idle(1);
  uint8_t cnt0 = dut->rsp_ign_cnt_o; size_t rt0 = h.rts.size();
  h.respond(0x0E55, static_cast<uint16_t>(e.seq + 1));   // wrong seq
  h.idle(1);
  CHECK(h.rts.size() == rt0, "D wrong seq not routed");
  h.respond(0x0BAD, e.seq);                   // wrong key
  h.idle(1);
  CHECK(h.rts.size() == rt0, "D wrong key not routed");
  CHECK(dut->rsp_ign_cnt_o == static_cast<uint8_t>(cnt0 + 2),
        "D both misses counted");
  h.respond(0x0E55, e.seq);
  h.idle(2);
  CHECK(h.rts.size() == rt0 + 1 && h.rts.back().owner == 9,
        "D entry survived the mismatches and still routes");
}

// ---- E: interleaved inflights route independently -------------------
void OriginatorSuite::interleaved_inflights_route_independently() {
  size_t rt0 = h.rts.size();
  auto e1 = h.issue(1, 0, 0x0501, 61, 200);
  auto e2 = h.issue(6, 1, 0x0502, 62, 200);
  auto e3 = h.issue(1, 2, 0x0503, 63, 200);
  h.idle(1);
  CHECK(e1.id == 0 && e2.id == 1 && e3.id == 2, "E ids in order");
  CHECK(e1.seq + 1 == e3.seq, "E owner-1 seq advanced between issues");
  CHECK(e2.seq == 0, "E owner-6 counter independent (first use) got %u",
        e2.seq);
  h.respond(0x0502, e2.seq);  h.idle(2);
  h.respond(0x0503, e3.seq);  h.idle(2);
  h.respond(0x0501, e1.seq);  h.idle(2);
  CHECK(h.rts.size() == rt0 + 3, "E all three routed");
  CHECK(h.rts[rt0].owner == 6 && h.rts[rt0].id == 1, "E middle first");
  CHECK(h.rts[rt0 + 1].owner == 1 && h.rts[rt0 + 1].id == 2, "E third next");
  CHECK(h.rts[rt0 + 2].owner == 1 && h.rts[rt0 + 2].id == 0, "E first last");
  CHECK(h.held_total() == 0 && h.armed_count() == 0, "E all freed");
}

// ---- F: table-full refusal + reuse ----------------------------------
void OriginatorSuite::table_full_refuses_then_reuses_the_freed_id() {
  size_t s0 = h.sends.size();
  Harness::Res fill[NIFL];
  bool all_gnt = true;
  bool ids_ok = true;
  for (int i = 0; i < NIFL; ++i) {
    fill[i] = h.issue(static_cast<uint8_t>(i), static_cast<uint8_t>(i % 5),
                      static_cast<uint16_t>(0x100 + i),
                      static_cast<uint8_t>(20 + i), 200);
    all_gnt &= fill[i].gnt;
    ids_ok  &= (fill[i].id == i);
  }
  h.idle(1);
  CHECK(all_gnt, "F all 12 issues granted");
  CHECK(ids_ok, "F ids allocated in order");
  CHECK(dut->inflight_busy_o == 0xFFF, "F table full");
  CHECK(!h.ready, "F ready low when full");
  auto r13 = h.issue(13, 0, 0x0999, 44, 200);
  CHECK(!r13.gnt, "F 13th issue refused");
  h.idle(1);
  CHECK(h.sends.size() == s0 + NIFL, "F refused issue sent nothing");
  h.respond(static_cast<uint16_t>(0x100 + 3), fill[3].seq);
  h.idle(2);
  CHECK(h.ready, "F ready again after one free");
  auto r14 = h.issue(15, 3, 0x0F0F, 45, 200);
  CHECK(r14.gnt && r14.id == 3, "F freed id 3 reused");
  h.idle(1);
  for (int i = 0; i < NIFL; ++i) {
    if (i == 3) continue;
    h.respond(static_cast<uint16_t>(0x100 + i), fill[i].seq);
    h.idle(1);
  }
  h.respond(0x0F0F, r14.seq);
  h.idle(2);
  CHECK(dut->inflight_busy_o == 0, "F drained");
  CHECK(h.held_total() == 0 && h.armed_count() == 0, "F no leaks");
}

// ---- G1: response (other entry) wins the cycle over a pending expiry
void OriginatorSuite::response_to_another_entry_wins_over_a_pending_expiry() {
  auto g1 = h.issue(1, 0, 0x0A01, 70, 200);
  auto g2 = h.issue(2, 1, 0x0B02, 71, 300);
  h.idle(1);
  h.now += 200;                        // g1 due, g2 not
  h.tmr[70].armed = false;             // stub self-disarm at fire
  h.fire(70, static_cast<uint8_t>(TAG | g1.id));  // pend parks at this edge
  h.respond(0x0B02, g2.seq);           // next cycle: response competes
  h.tick();
  CHECK(h.p_rt && !h.p_resend, "G1 response processed first");
  h.tick();
  CHECK(h.p_resend && h.resends.back() == 0, "G1 expiry retried next cycle");
  h.respond(0x0A01, g1.seq);
  h.idle(2);
  CHECK(h.rts.back().owner == 1 && h.armed_count() == 0, "G1 cleaned up");
}

// ---- G2: same-entry response + expiry in ONE cycle: response wins ---
void OriginatorSuite::same_entry_response_beats_its_own_expiry() {
  g3 = h.issue(3, 4, 0x0C03, 72, 200);
  h.idle(1);
  size_t rt0 = h.rts.size(); g_r0 = h.resends.size(); g_f0 = h.fls.size();
  h.clr();
  h.tmr[72].armed = false;
  dut->exp_valid_i = 1; dut->exp_slot_i = 72;
  dut->exp_owner_i = static_cast<uint8_t>(TAG | g3.id);
  dut->rsp_valid_i = 1; dut->rsp_key_i = 0x0C03; dut->rsp_seq_i = g3.seq;
  h.tick();
  h.clr();
  h.idle(3);
  CHECK(h.rts.size() == rt0 + 1, "G2 routed");
  CHECK(h.resends.size() == g_r0 && h.fls.size() == g_f0,
        "G2 no retry/fail — the response won");
  CHECK(dut->inflight_busy_o == 0, "G2 entry freed once");
}

// ---- G3: stale expiry (entry already freed) is inert ----------------
void OriginatorSuite::stale_expiry_is_inert() {
  h.fire(72, static_cast<uint8_t>(TAG | g3.id));
  h.idle(2);
  CHECK(h.resends.size() == g_r0 && h.fls.size() == g_f0, "G3 stale fire inert");
}

// ---- G4: foreign owner tag is not ours ------------------------------
void OriginatorSuite::foreign_owner_tag_is_inert() {
  h.fire(73, 0x51);
  h.idle(2);
  CHECK(h.resends.size() == g_r0 && h.fls.size() == g_f0, "G4 foreign tag inert");
}

// ---- H: owner cancellation frees the held slot and timer ------------
void OriginatorSuite::cancellation_frees_the_held_slot_and_the_timer() {
  auto hc = h.issue(8, 3, 0x0C08, 74, 250);
  h.idle(1);
  size_t rt0 = h.rts.size(); size_t f0 = h.fls.size();
  int rel0 = h.releases;
  int can0 = h.cancels;
  h.cancel(8);
  h.idle(2);
  CHECK((dut->inflight_busy_o & (1u << hc.id)) == 0,
        "H cancellation freed owner-8 entry");
  CHECK(h.releases == rel0 + 1 && h.held[3] == 0,
        "H cancellation released held slot");
  CHECK(h.cancels == can0 + 1 && h.armed_count() == 0,
        "H cancellation disarmed timeout");
  CHECK(h.rts.size() == rt0 && h.fls.size() == f0,
        "H cancellation produced no route or failure");
  h.cancel(8);
  h.idle(1);
  CHECK(h.releases == rel0 + 1 && h.cancels == can0 + 1,
        "H absent-owner cancellation is inert");
}

// ---- I: initial queue delay cannot consume the attempt budget -------
void OriginatorSuite::queue_delay_never_consumes_the_attempt_budget() {
  int arms0 = h.arms;
  auto qi = h.issue(10, 0, 0xCA10, 75, 250, false);
  CHECK(qi.gnt, "I delayed issue granted");
  h.idle(1);
  CHECK(h.sends.back() == 0 && h.arms == arms0,
        "I initial send queued without timer arm");
  h.now += 1000;
  CHECK(h.expire_due() == -1, "I no expiry before serializer acceptance");
  CHECK((dut->inflight_busy_o & (1u << qi.id)) != 0,
        "I queued entry remains live past timeout duration");
  h.accept(0);
  h.idle(2);
  CHECK(h.arms == arms0 + 1 && h.armops.back().slot == 75,
        "I timer armed on serializer acceptance");
  CHECK(h.armops.back().deadline == h.now + 250,
        "I full timeout budget begins at acceptance");
  h.cancel(10);
  h.idle(2);
  CHECK((dut->inflight_busy_o & (1u << qi.id)) == 0,
        "I delayed entry cancelled cleanly");
}

// ---- J: acceptance is lossless beside another entry's response ------
void OriginatorSuite::acceptance_survives_another_entrys_response() {
  auto ja = h.issue(11, 1, 0xCA11, 76, 250);
  auto jb = h.issue(12, 2, 0xCA12, 77, 250, false);
  h.idle(1);
  int arms0 = h.arms;
  size_t rt0 = h.rts.size();
  h.clr();
  dut->rsp_valid_i = 1; dut->rsp_key_i = 0xCA11; dut->rsp_seq_i = ja.seq;
  dut->send_accept_valid_i = 1; dut->send_accept_slot_i = 2;
  h.tick();
  h.clr();
  h.idle(2);
  CHECK(h.rts.size() == rt0 + 1 && h.rts.back().id == ja.id,
        "J response retained priority over another slot acceptance");
  CHECK(h.arms == arms0 + 1 && h.armops.back().slot == 77,
        "J simultaneous serializer acceptance was parked and armed");
  CHECK((dut->inflight_busy_o & (1u << jb.id)) != 0,
        "J accepted second entry remains live");
  h.respond(0xCA12, jb.seq);
  h.idle(2);
  CHECK((dut->inflight_busy_o & (1u << jb.id)) == 0,
        "J second entry responds and releases normally");
}

// ---- J2: cancellation is lossless beside another entry's response ---
void OriginatorSuite::cancellation_survives_another_entrys_response() {
  auto jc = h.issue(13, 3, 0xCA13, 78, 250);
  auto jd = h.issue(14, 4, 0xCA14, 79, 250);
  h.idle(1);
  size_t rt0 = h.rts.size();
  int rel0 = h.releases;
  int can0 = h.cancels;
  h.clr();
  dut->rsp_valid_i = 1; dut->rsp_key_i = 0xCA13; dut->rsp_seq_i = jc.seq;
  dut->cancel_valid_i = 1; dut->cancel_owner_i = 14;
  h.tick();
  h.clr();
  h.idle(3);
  CHECK(h.rts.size() == rt0 + 1 && h.rts.back().id == jc.id,
        "J2 response retains priority over another entry cancellation");
  CHECK((dut->inflight_busy_o & (1u << jd.id)) == 0,
        "J2 simultaneous cancellation was parked and processed");
  CHECK(h.releases == rel0 + 2,
        "J2 response and parked cancellation release both slots");
  CHECK(h.cancels == can0 + 2 && h.armed_count() == 0,
        "J2 both timer cancellations complete without a leak");
}

// ---- K: the 8-bit ignored counter wraps -----------------------------
void OriginatorSuite::the_ignored_counter_wraps_at_eight_bits() {
  uint8_t cnt0 = dut->rsp_ign_cnt_o;
  for (int i = 0; i < IGN_CNT_MAX - static_cast<int>(cnt0); ++i)
    h.respond(0xDEAD, 0x9999);
  CHECK(dut->rsp_ign_cnt_o == IGN_CNT_MAX, "H counter reaches 255");
  h.respond(0xDEAD, 0x9999);
  CHECK(dut->rsp_ign_cnt_o == 0, "H 8-bit counter wraps to 0");
}

// ---- final invariants ------------------------------------------------
void OriginatorSuite::final_invariants_hold() {
  h.idle(2);
  CHECK(h.ready, "final: ready");
  CHECK(!h.oob, "final: every arm/cancel used a legal timer slot");
  CHECK(!h.neg_release, "final: never released an unheld slot");
  CHECK(h.holds == h.releases, "final: every hold released exactly once "
        "(%d vs %d)", h.holds, h.releases);
  CHECK(h.armed_count() == 0, "final: no armed timer slot leaked");
  CHECK(dut->inflight_busy_o == 0, "final: table empty");
}

// ---- R: a seeded session against the independent inflight model ---------
// Sixteen owners (one CONTROLLER_AVAILABLE exchange each at most, as the
// notify block's monitor issues them) and eight TX slots churn through
// issues, serializer acceptances, responses, stray responses, expiries fired
// in a random order among the due slots, and cancellations. Every event is
// applied alone, the module settles, and every pulse it produced is compared
// with the model's prediction, with the live-entry map and the ignored count.
uint32_t OriginatorSuite::next_random() {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}

void OriginatorSuite::r_judge(const IflWant& want, const char* what) {
  ++r_events;
  bool same = h.rts == want.rts && h.fls == want.fls && h.sends == want.sends
              && h.resends == want.resends && h.armops == want.arms
              && h.holds == want.holds && h.releases == want.releases;
  same = same && dut->inflight_busy_o == m.busy() && dut->rsp_ign_cnt_o == m.ignored;
  if (!same && ++r_divergences <= 6)
    printf("  R event %ld (%s) diverges from the model\n", r_events, what);
}

void OriginatorSuite::r_issue(uint8_t owner) {
  IflWant w{h.rts, h.fls, h.sends, h.resends, h.armops, h.holds, h.releases};
  uint8_t tx = static_cast<uint8_t>(next_random() % NSLOT);
  while (m.tx_busy(tx)) tx = static_cast<uint8_t>((tx + 1) % NSLOT);
  const int id = m.lowest_free();
  if (id < 0) return;
  const uint16_t timeout = static_cast<uint16_t>(50 + next_random() % 250);
  const uint16_t key = r_key++;
  auto r = h.issue(owner, tx, key, static_cast<uint8_t>(20 + owner), timeout, false);
  h.idle(3);
  const bool grant_ok = r.gnt && r.id == id && r.seq == m.next_seq[owner];
  if (!grant_ok && ++r_divergences <= 6)
    printf("  R issue: grant %d id %u seq %u, model id %d seq %u\n", r.gnt, r.id, r.seq, id,
           m.next_seq[owner]);
  m.e[id] = {true, owner, key, m.next_seq[owner]++, tx, static_cast<uint8_t>(20 + owner),
             timeout, false, false, true};
  w.sends.push_back(tx);
  ++w.holds;
  r_judge(w, "issue");
}

void OriginatorSuite::r_accept(int id) {
  IflWant w{h.rts, h.fls, h.sends, h.resends, h.armops, h.holds, h.releases};
  auto& x = m.e[id];
  h.accept(x.tx);
  w.arms.push_back({false, x.tmr, static_cast<uint8_t>(TAG | id), h.now + x.timeout});
  h.idle(3);
  x.await = false;
  x.armed = true;
  r_judge(w, "acceptance");
}

void OriginatorSuite::r_respond(int id) {
  IflWant w{h.rts, h.fls, h.sends, h.resends, h.armops, h.holds, h.releases};
  auto& x = m.e[id];
  h.respond(x.key, x.seq);
  h.idle(3);
  w.rts.push_back({x.owner, static_cast<uint8_t>(id)});
  ++w.releases;
  w.arms.push_back({true, x.tmr, static_cast<uint8_t>(TAG | id), 0});
  x.live = false;
  ++r_routes;
  r_judge(w, "response");
}

//! a response nothing is waiting for: half the time a live exchange's key
//! with a sequence id it never issued (F09.4), else a key nobody holds
void OriginatorSuite::r_stray_response() {
  IflWant w{h.rts, h.fls, h.sends, h.resends, h.armops, h.holds, h.releases};
  const int id = static_cast<int>(next_random() % NIFL);
  if ((next_random() & 1) && m.e[id].live)
    h.respond(m.e[id].key, static_cast<uint16_t>(m.e[id].seq + 1 + next_random() % 100));
  else
    h.respond(static_cast<uint16_t>(0x2000 + next_random() % 0x1000),
              static_cast<uint16_t>(next_random()));
  h.idle(3);
  ++m.ignored;
  ++r_strays;
  r_judge(w, "stray response");
}

void OriginatorSuite::r_cancel(uint8_t owner) {
  IflWant w{h.rts, h.fls, h.sends, h.resends, h.armops, h.holds, h.releases};
  const int id = m.live_of(owner);
  h.cancel(owner);
  h.idle(3);
  if (id >= 0) {
    ++w.releases;
    w.arms.push_back({true, m.e[id].tmr, static_cast<uint8_t>(TAG | id), 0});
    m.e[id].live = false;
    ++r_cancels;
  }
  r_judge(w, "cancellation");
}

//! time moves to the next due deadline, and ONE due slot fires, picked at
//! random among those due (the sweep order is not part of the contract)
void OriginatorSuite::r_expire() {
  std::vector<int> due;
  uint32_t soonest = 0;
  bool any = false;
  for (int i = 0; i < NIFL; ++i) {
    const auto& x = m.e[i];
    if (!x.live || !x.armed) continue;
    const uint32_t d = h.tmr[x.tmr].deadline;
    if (!any || static_cast<int32_t>(d - soonest) < 0) soonest = d;
    any = true;
  }
  if (!any) return;
  if (static_cast<int32_t>(h.now - soonest) < 0) h.now = soonest + next_random() % 40;
  for (int i = 0; i < NIFL; ++i)
    if (m.e[i].live && m.e[i].armed && static_cast<int32_t>(h.now - h.tmr[m.e[i].tmr].deadline) >= 0)
      due.push_back(i);
  const int id = due[next_random() % due.size()];
  auto& x = m.e[id];
  IflWant w{h.rts, h.fls, h.sends, h.resends, h.armops, h.holds, h.releases};
  h.tmr[x.tmr].armed = false;
  h.fire(x.tmr, static_cast<uint8_t>(TAG | id));
  h.idle(3);
  x.armed = false;
  if (!x.retried) {
    w.resends.push_back(x.tx);
    x.retried = true;
    x.await = true;
    ++r_retries;
  } else {
    w.fls.push_back({x.owner, static_cast<uint8_t>(id)});
    ++w.releases;
    x.live = false;
    ++r_fails;
  }
  r_judge(w, "expiry");
}

void OriginatorSuite::r_step() {
  const uint32_t pick = next_random() % 100;
  const uint8_t owner = static_cast<uint8_t>(next_random() % 16);
  int live = 0;
  for (const auto& x : m.e) live += x.live ? 1 : 0;
  r_peak = live > r_peak ? live : r_peak;
  std::vector<int> waiting;
  std::vector<int> armed;
  for (int i = 0; i < NIFL; ++i) {
    if (m.e[i].live && m.e[i].await) waiting.push_back(i);
    //! on the wire: armed, or a first attempt whose retry still waits
    if (m.e[i].live && (!m.e[i].await || m.e[i].retried)) armed.push_back(i);
  }
  if (pick < 30 && m.live_of(owner) < 0 && live < NSLOT) r_issue(owner);
  else if (pick < 50 && !waiting.empty()) r_accept(waiting[next_random() % waiting.size()]);
  else if (pick < 68 && !armed.empty()) r_respond(armed[next_random() % armed.size()]);
  else if (pick < 72) r_stray_response();
  else if (pick < 90) r_expire();
  else r_cancel(owner);
}

//! everything left is accepted, answered or cancelled: nothing may leak
void OriginatorSuite::r_drain() {
  for (int i = 0; i < NIFL; ++i) {
    if (!m.e[i].live) continue;
    if (m.e[i].await) r_accept(i);
    r_respond(i);
  }
}

void OriginatorSuite::a_seeded_session_matches_the_inflight_model() {
  dut->rst_n = 0;
  h.idle(4);
  dut->rst_n = 1;
  h.idle(2);
  for (auto& t : h.tmr) t.armed = false;
  for (int i = 0; i < 4000; ++i) r_step();
  r_drain();
  printf("  [i] R: seed 0x86A46301, %ld events, peak %d live exchanges; %ld routed, "
         "%ld retried, %ld failed, %ld cancelled, %ld strays\n", r_events, r_peak, r_routes,
         r_retries, r_fails, r_cancels, r_strays);
  REQ_TAG("REQ-AEM-005", "RND", "R: a seeded session of 16 owners' overlapping exchanges");
  CHECK(r_divergences == 0, "R: a seeded session of 16 owners' overlapping exchanges, "
        "zero divergence from the independent inflight model (%ld of %ld events)",
        r_divergences, r_events);
  CHECK(r_peak >= 6 && r_routes >= 200 && r_retries >= 100 && r_fails >= 30
            && r_cancels >= 50 && r_strays >= 50,
        "R b: every arm was exercised with overlap (peak %d, routed %ld, retried %ld, "
        "failed %ld, cancelled %ld, strays %ld)", r_peak, r_routes, r_retries, r_fails,
        r_cancels, r_strays);
  CHECK(dut->inflight_busy_o == 0 && h.armed_count() == 0 && h.held_total() == 0,
        "R c: the drained session leaks no entry, timer or held slot");
}

int OriginatorSuite::run() {
  reset_leaves_the_table_empty();
  issue_then_response_routes_and_frees();
  freed_id_is_reused_and_the_timeout_is_plumbed();
  timeout_resends_once_and_the_original_seq_still_routes();
  second_timeout_fails_and_a_late_response_is_counted();
  mismatched_seq_or_key_is_ignored_and_counted();
  interleaved_inflights_route_independently();
  table_full_refuses_then_reuses_the_freed_id();
  response_to_another_entry_wins_over_a_pending_expiry();
  same_entry_response_beats_its_own_expiry();
  stale_expiry_is_inert();
  foreign_owner_tag_is_inert();
  cancellation_frees_the_held_slot_and_the_timer();
  queue_delay_never_consumes_the_attempt_budget();
  acceptance_survives_another_entrys_response();
  cancellation_survives_another_entrys_response();
  the_ignored_counter_wraps_at_eight_bits();
  a_seeded_session_matches_the_inflight_model();
  final_invariants_hold();

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails_n, fails_n);
  return fails_n ? 1 : 0;
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  OriginatorSuite suite;
  return suite.run();
}
