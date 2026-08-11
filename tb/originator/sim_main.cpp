// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_originator suite — independent expectations, never DUT logic.
//
// Proves the 03 §5 originator + inflight table against a C++ stub of the
// KL_pp_timer_service arm/expiry port protocol (arm = {slot, owner,
// absolute-ms deadline}, cancel clears, a fired slot self-disarms) and a
// hold/release tally standing in for the KL_pp_tx_slots pool. Covered:
// issue -> respond routes + frees; per-owner seq counters; timeout -> ONE
// exact re-send (same held slot, same seq — the original seq still routes
// after the retry) -> second timeout fails; responses after fail and
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

static int checks = 0, fails_n = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails_n; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

static const int NTMR = 89;         // pp_pkg::PP_TIMER_SLOTS_C
static const uint8_t TAG = 0xC0;    // TMR_TAG_P << 4
static const int NIFL = 12;         // INFLIGHT_P default = 4 CA + 8 probe

struct ArmOp { bool cancel; uint8_t slot; uint8_t owner; uint32_t deadline; };
struct Ev { uint8_t owner; uint8_t id; };

struct Harness {
  VKL_pp_originator* dut;
  uint32_t now = 1000;

  // ---- timer-service stub (the arm-port protocol model) ----
  struct TSlot { bool armed = false; uint8_t owner = 0; uint32_t deadline = 0; };
  TSlot tmr[NTMR];
  int arms = 0, cancels = 0;
  bool oob = false;
  std::vector<ArmOp> armops;

  // ---- pool stand-in: hold/release tally ----
  int held[8] = {0};
  int holds = 0, releases = 0;
  bool neg_release = false;

  // cumulative event logs + per-tick pulse flags
  std::vector<Ev> rts, fls;
  std::vector<uint8_t> sends, resends;
  bool p_rt = false, p_resend = false, p_fail = false;

  // per-tick comb handshake snapshot
  bool ready = false, gnt = false;
  uint8_t id = 0;
  uint16_t seq = 0;

  explicit Harness(VKL_pp_originator* d) : dut(d) {}

  void clr() {
    dut->iss_valid_i = 0;
    dut->rsp_valid_i = 0;
    dut->exp_valid_i = 0;
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
      ArmOp op{(bool)dut->tmr_arm_cancel_o, (uint8_t)dut->tmr_arm_slot_o,
               (uint8_t)dut->tmr_arm_owner_o, (uint32_t)dut->tmr_arm_deadline_ms_o};
      armops.push_back(op);
      if (op.slot >= NTMR) { oob = true; }
      else if (op.cancel) { ++cancels; tmr[op.slot].armed = false; }
      else { ++arms; tmr[op.slot] = {true, op.owner, op.deadline}; }
    }
    if (dut->hold_valid_o) { ++held[dut->hold_slot_o & 7]; ++holds; }
    if (dut->release_valid_o) {
      if (--held[dut->release_slot_o & 7] < 0) neg_release = true;
      ++releases;
    }
    if (dut->send_valid_o) sends.push_back((uint8_t)dut->send_slot_o);
    if (dut->resend_valid_o) resends.push_back((uint8_t)dut->resend_slot_o);
    if (dut->rt_valid_o)
      rts.push_back({(uint8_t)dut->rt_owner_o, (uint8_t)dut->rt_id_o});
    if (dut->fail_valid_o)
      fls.push_back({(uint8_t)dut->fail_owner_o, (uint8_t)dut->fail_id_o});
    dut->clk_i = 1; dut->eval();
  }

  void idle(int n = 1) { clr(); for (int i = 0; i < n; ++i) tick(); }

  struct Res { bool gnt; uint8_t id; uint16_t seq; };
  Res issue(uint8_t owner, uint8_t txslot, uint16_t key,
            uint8_t tmrslot, uint16_t to_ms) {
    clr();
    dut->iss_valid_i = 1; dut->iss_owner_i = owner;
    dut->iss_tx_slot_i = txslot; dut->iss_key_i = key;
    dut->iss_tmr_slot_i = tmrslot; dut->iss_timeout_ms_i = to_ms;
    tick();
    Res r{gnt, id, seq};
    dut->iss_valid_i = 0;
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
      if (tmr[s].armed && (int32_t)(now - tmr[s].deadline) >= 0) {
        tmr[s].armed = false;
        fire((uint8_t)s, tmr[s].owner);
        return s;
      }
    return -1;
  }

  int armed_count() const {
    int n = 0; for (auto& t : tmr) if (t.armed) ++n; return n;
  }
  int held_total() const {
    int n = 0; for (int i = 0; i < 8; ++i) n += held[i]; return n;
  }
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_pp_originator;
  Harness h(dut);

  // independent per-owner seq mirror
  uint16_t mseq[16] = {0};

  dut->rst_n = 0; h.clr();
  dut->iss_owner_i = 0; dut->iss_tx_slot_i = 0; dut->iss_key_i = 0;
  dut->iss_tmr_slot_i = 0; dut->iss_timeout_ms_i = 0;
  dut->rsp_seq_i = 0; dut->rsp_key_i = 0;
  dut->exp_slot_i = 0; dut->exp_owner_i = 0;
  for (int i = 0; i < 4; ++i) h.tick();
  dut->rst_n = 1;
  h.idle(1);
  CHECK(h.ready, "ready after reset");
  CHECK(dut->inflight_busy_o == 0, "no inflight after reset");
  CHECK(dut->rsp_ign_cnt_o == 0, "ign counter zero after reset");

  // ---- A: issue -> respond routes + frees -----------------------------
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

  // ---- B: freed id reused; timeout value plumbed through --------------
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

  // ---- C1: timeout -> ONE exact re-send, original seq still routes ----
  size_t s0 = h.sends.size(), r0 = h.resends.size();
  auto c = h.issue(7, 3, 0x0777, 50, 200);
  CHECK(c.gnt, "C1 granted");
  h.idle(1);
  h.now += 200;
  CHECK(h.expire_due() == 50, "C1 expiry fired on slot 50");
  h.idle(2);
  CHECK(h.resends.size() == r0 + 1 && h.resends.back() == 3,
        "C1 one re-send of the HELD slot 3");
  CHECK(h.sends.size() == s0 + 1, "C1 no fresh send on retry");
  CHECK(!h.armops.back().cancel && h.armops.back().slot == 50
        && h.armops.back().deadline == h.now + 200,
        "C1 re-armed same slot, new deadline");
  CHECK((dut->inflight_busy_o & (1u << c.id)) != 0, "C1 still inflight");
  h.respond(0x0777, c.seq);   // the ORIGINAL seq — never re-assigned
  h.idle(2);
  CHECK(h.rts.back().owner == 7 && h.rts.back().id == c.id,
        "C1 original seq routes after the exact duplicate");
  CHECK(h.held[3] == 0 && h.armed_count() == 0, "C1 released + disarmed");

  // ---- C2: second timeout fails; response after fail ignored+counted --
  r0 = h.resends.size();
  size_t f0 = h.fls.size();
  int cx0 = h.cancels;
  auto d = h.issue(4, 2, 0x0444, 51, 200);
  h.idle(1);
  h.now += 200;
  CHECK(h.expire_due() == 51, "C2 first expiry");
  h.idle(2);
  CHECK(h.resends.size() == r0 + 1, "C2 retry sent");
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
  CHECK(dut->rsp_ign_cnt_o == (uint8_t)(cnt0 + 1),
        "C2 response after fail counted");

  // ---- D: mismatched {seq} / {key} ignored + counted ------------------
  auto e = h.issue(9, 4, 0x0E55, 60, 200);
  h.idle(1);
  cnt0 = dut->rsp_ign_cnt_o; rt0 = h.rts.size();
  h.respond(0x0E55, (uint16_t)(e.seq + 1));   // wrong seq
  h.idle(1);
  CHECK(h.rts.size() == rt0, "D wrong seq not routed");
  h.respond(0x0BAD, e.seq);                   // wrong key
  h.idle(1);
  CHECK(h.rts.size() == rt0, "D wrong key not routed");
  CHECK(dut->rsp_ign_cnt_o == (uint8_t)(cnt0 + 2), "D both misses counted");
  h.respond(0x0E55, e.seq);
  h.idle(2);
  CHECK(h.rts.size() == rt0 + 1 && h.rts.back().owner == 9,
        "D entry survived the mismatches and still routes");

  // ---- E: interleaved inflights route independently -------------------
  rt0 = h.rts.size();
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

  // ---- F: table-full refusal + reuse ----------------------------------
  s0 = h.sends.size();
  Harness::Res fill[NIFL];
  bool all_gnt = true, ids_ok = true;
  for (int i = 0; i < NIFL; ++i) {
    fill[i] = h.issue((uint8_t)i, (uint8_t)(i % 5), (uint16_t)(0x100 + i),
                      (uint8_t)(20 + i), 200);
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
  h.respond((uint16_t)(0x100 + 3), fill[3].seq);
  h.idle(2);
  CHECK(h.ready, "F ready again after one free");
  auto r14 = h.issue(15, 3, 0x0F0F, 45, 200);
  CHECK(r14.gnt && r14.id == 3, "F freed id 3 reused");
  h.idle(1);
  for (int i = 0; i < NIFL; ++i) {
    if (i == 3) continue;
    h.respond((uint16_t)(0x100 + i), fill[i].seq);
    h.idle(1);
  }
  h.respond(0x0F0F, r14.seq);
  h.idle(2);
  CHECK(dut->inflight_busy_o == 0, "F drained");
  CHECK(h.held_total() == 0 && h.armed_count() == 0, "F no leaks");

  // ---- G1: response (other entry) wins the cycle over a pending expiry
  auto g1 = h.issue(1, 0, 0x0A01, 70, 200);
  auto g2 = h.issue(2, 1, 0x0B02, 71, 300);
  h.idle(1);
  h.now += 200;                        // g1 due, g2 not
  h.tmr[70].armed = false;             // stub self-disarm at fire
  h.fire(70, (uint8_t)(TAG | g1.id));  // pend parks at this edge
  h.respond(0x0B02, g2.seq);           // next cycle: response competes
  h.tick();
  CHECK(h.p_rt && !h.p_resend, "G1 response processed first");
  h.tick();
  CHECK(h.p_resend && h.resends.back() == 0, "G1 expiry retried next cycle");
  h.respond(0x0A01, g1.seq);
  h.idle(2);
  CHECK(h.rts.back().owner == 1 && h.armed_count() == 0, "G1 cleaned up");

  // ---- G2: same-entry response + expiry in ONE cycle: response wins ---
  auto g3 = h.issue(3, 4, 0x0C03, 72, 200);
  h.idle(1);
  rt0 = h.rts.size(); r0 = h.resends.size(); f0 = h.fls.size();
  h.clr();
  h.tmr[72].armed = false;
  dut->exp_valid_i = 1; dut->exp_slot_i = 72;
  dut->exp_owner_i = (uint8_t)(TAG | g3.id);
  dut->rsp_valid_i = 1; dut->rsp_key_i = 0x0C03; dut->rsp_seq_i = g3.seq;
  h.tick();
  h.clr();
  h.idle(3);
  CHECK(h.rts.size() == rt0 + 1, "G2 routed");
  CHECK(h.resends.size() == r0 && h.fls.size() == f0,
        "G2 no retry/fail — the response won");
  CHECK(dut->inflight_busy_o == 0, "G2 entry freed once");

  // ---- G3: stale expiry (entry already freed) is inert ----------------
  h.fire(72, (uint8_t)(TAG | g3.id));
  h.idle(2);
  CHECK(h.resends.size() == r0 && h.fls.size() == f0, "G3 stale fire inert");

  // ---- G4: foreign owner tag is not ours ------------------------------
  h.fire(73, 0x51);
  h.idle(2);
  CHECK(h.resends.size() == r0 && h.fls.size() == f0, "G4 foreign tag inert");

  // ---- H: the 8-bit ignored counter wraps -----------------------------
  cnt0 = dut->rsp_ign_cnt_o;
  for (int i = 0; i < 255 - (int)cnt0; ++i) h.respond(0xDEAD, 0x9999);
  CHECK(dut->rsp_ign_cnt_o == 255, "H counter reaches 255");
  h.respond(0xDEAD, 0x9999);
  CHECK(dut->rsp_ign_cnt_o == 0, "H 8-bit counter wraps to 0");

  // ---- final invariants ------------------------------------------------
  h.idle(2);
  CHECK(h.ready, "final: ready");
  CHECK(!h.oob, "final: every arm/cancel used a legal timer slot");
  CHECK(!h.neg_release, "final: never released an unheld slot");
  CHECK(h.holds == h.releases, "final: every hold released exactly once "
        "(%d vs %d)", h.holds, h.releases);
  CHECK(h.armed_count() == 0, "final: no armed timer slot leaked");
  CHECK(dut->inflight_busy_o == 0, "final: table empty");

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails_n, fails_n);
  delete dut;
  return fails_n ? 1 : 0;
}
