// SPDX-License-Identifier: CERN-OHL-W-2.0
// srp_top timer-arm FIFO arms (issue #230) — independent expectations, never
// DUT logic.
//
// KL_srp_top queues each stream FSM's timer ops in a FIFO of its own (32
// words, distributed RAM since #230) and issues them on its one merged arm
// face, round-robin, one word every two clocks. A scoreboard holds the FIFO
// contract: every op an FSM offers leaves the merged face exactly once, in
// that FSM's order and unmodified, except an op offered while its FIFO holds
// 32 words, which the full guard refuses. The Makefile builds this harness at
// sources/sinks 1/1, 2/2, 3/5 and 9/9. The stimulus of the first arm is an
// own LeaveAll, which ages both registrar arrays at once, so both FSMs offer
// an op in the same clock; and a talker re-declaration accepted on that
// LeaveAll's clock, which offers the talker FIFO a second op while the
// listener's is served first, so the talker FIFO holds two words when the
// merged face selects it (both are preconditions, checked). The checks:
//   TF1  the talker FIFO issues every op it accepted once, in its order and
//       unmodified: no word lost, reordered, altered, duplicated or invented;
//   TF2  the same for the listener FIFO;
//   TF3  no op is refused while the issue runs unforced;
//   TF4  with the merged issue held (the harness's one forced state,
//       srp_store_wrap), the talker FIFO accepts exactly 32 ops and refuses
//       the rest; released, it issues those 32 in order, unmodified;
//   TF5  the same for the listener FIFO.
// Expectations are built here: MRPDUs from 802.1Q §10.8.1.2 / §35.2.2 (the
// same independent builder as sim_main.cpp), the FIFO model from the
// contract above.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <vector>
#include "Vsrp_store_wrap.h"
#include "verilated.h"
#include "../common/verilator_harness.hpp"

#ifndef TB_SOURCES
#error "TB_SOURCES (talker sources) must be defined by the build"
#endif
#ifndef TB_SINKS
#error "TB_SINKS (listener sinks) must be defined by the build"
#endif

constexpr int kSources = TB_SOURCES;
constexpr int kSinks = TB_SINKS;

// a failing check names the shape it failed at, sources/sinks
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { \
    ++fails; printf("FAIL: " __VA_ARGS__); printf(" [%d/%d]\n", kSources, kSinks); \
  } \
} while (0)

namespace {

// ---------------------------------------------------------------------------
// constants (TB shape, srp_store_wrap)
// ---------------------------------------------------------------------------
constexpr int      MS_CYC  = 40;                    // wrap: 1 ms = 40 clk
constexpr uint64_t OWN_MAC = 0x0A0B0C0D0E0FULL;
constexpr uint64_t EID     = 0x123456789ABCDEF0ULL;
constexpr int      OP_GUARD = 200000;               // class-B handshake guard, clocks
constexpr uint64_t CYCLE_BUDGET = 20000000ULL;      // whole run, all arms
constexpr size_t   FIFO_DEPTH = 32;                 // KL_srp_top TFD_C
// owner tags on the merged arm face (KL_srp_top defaults)
constexpr int TK_OWNER = 0x40;
constexpr int LS_OWNER = 0x60;
// an own LeaveAll is drawn 10-15 s after the last start; this bounds a wait
constexpr int LEAVEALL_WAIT_MS = 16000;
constexpr int LEAVE_MS = 5000;                      // T-MRP-LEAVE (F08.1)
// a peer LeaveAll MRPDU offers each FIFO two ops per stream; the stall arm
// feeds them until each FIFO has been offered this many
constexpr uint64_t STALL_OFFERS = FIFO_DEPTH + 4;

constexpr int OP_DECL_TK = 0;
constexpr int OP_DECL_LS = 2;
constexpr int EV_JOININ  = 1;
constexpr int DECL_READY = 2;
constexpr int REG_IN     = 1;

// ---------------------------------------------------------------------------
// independent MRPDU builder (802.1Q §10.8.1.2 BNF, §35.2.2)
// ---------------------------------------------------------------------------
struct Vec {
  bool la = false;                      // LeaveAllEvent on this VectorHeader
  int nov = 1;
  std::vector<uint8_t> fv;              // FirstValue bytes
  std::vector<int> ev;                  // ThreePacked lane, one per value
  std::vector<int> fp;                  // FourPacked lane (Listener only)
};
struct Msg { int type; int alen; bool listener; std::vector<Vec> vecs; };

void put16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(v >> 8); b.push_back(v & 0xFF);
}
void put_mac(std::vector<uint8_t>& b, uint64_t m) {
  for (int i = 5; i >= 0; i--) b.push_back((m >> (8 * i)) & 0xFF);
}
std::vector<uint8_t> vec_bytes(const Vec& v, bool listener) {
  std::vector<uint8_t> b;
  b.push_back((v.la ? 0x20 : 0x00) | ((v.nov >> 8) & 0x1F));
  b.push_back(v.nov & 0xFF);
  b.insert(b.end(), v.fv.begin(), v.fv.end());
  for (int i = 0; i < v.nov; i += 3) {
    int e[3] = {};
    for (int j = 0; j < 3 && i + j < v.nov; j++) e[j] = v.ev[i + j];
    b.push_back(static_cast<uint8_t>(((e[0] * 6) + e[1]) * 6 + e[2]));
  }
  if (listener) {
    for (int i = 0; i < v.nov; i += 4) {
      int p[4] = {};
      for (int j = 0; j < 4 && i + j < v.nov; j++) p[j] = v.fp[i + j];
      b.push_back(static_cast<uint8_t>(p[0] * 64 + p[1] * 16 + p[2] * 4 + p[3]));
    }
  }
  return b;
}
// body = ProtocolVersion..EndMark (the shape fed to the decoder)
std::vector<uint8_t> mrpdu_body(const std::vector<Msg>& ms) {
  std::vector<uint8_t> b;
  b.push_back(0x00);                    // ProtocolVersion
  for (const Msg& m : ms) {
    b.push_back(static_cast<uint8_t>(m.type));
    b.push_back(static_cast<uint8_t>(m.alen));
    std::vector<uint8_t> body;
    for (const Vec& v : m.vecs) {
      auto vb = vec_bytes(v, m.listener);
      body.insert(body.end(), vb.begin(), vb.end());
    }
    put16(b, static_cast<uint16_t>(body.size() + 2));  // + EndMark (MSRP)
    b.insert(b.end(), body.begin(), body.end());
    put16(b, 0x0000);                   // AttributeList EndMark
  }
  put16(b, 0x0000);                     // MRPDU EndMark
  return b;
}
std::vector<uint8_t> fv_talker(uint64_t sid, uint64_t da, uint16_t vid, uint16_t mfs,
                               uint16_t mif, int prio, int rank, uint32_t lat) {
  std::vector<uint8_t> b;
  for (int i = 7; i >= 0; i--) b.push_back((sid >> (8 * i)) & 0xFF);
  put_mac(b, da);
  put16(b, vid); put16(b, mfs); put16(b, mif);
  b.push_back(static_cast<uint8_t>((prio << 5) | (rank << 4)));
  for (int i = 3; i >= 0; i--) b.push_back((lat >> (8 * i)) & 0xFF);
  return b;
}
std::vector<uint8_t> fv_sid(uint64_t sid) {
  std::vector<uint8_t> b;
  for (int i = 7; i >= 0; i--) b.push_back((sid >> (8 * i)) & 0xFF);
  return b;
}

uint64_t own_sid(int s) { return (OWN_MAC << 16) | (0x100 + s); }
uint64_t own_da(int s) { return 0x91e0f0010100ULL + s; }
uint64_t peer_sid(int k) { return 0x1122334455660100ULL + k; }
uint64_t peer_da(int k) { return 0x91e0f0112300ULL + k; }

// ---------------------------------------------------------------------------
// the FIFO model: what each FSM offered, less what the full guard refused,
// in order; the merged face must issue exactly that
// ---------------------------------------------------------------------------
struct Scoreboard {
  std::deque<uint64_t> q[2];            // 0 talker FIFO, 1 listener FIFO
  uint64_t offered[2] = {};
  uint64_t refused[2] = {};
  uint64_t issued[2] = {};
  uint64_t wrong[2] = {};               // issued word differs from the model's head
  uint64_t invented[2] = {};            // issued with nothing queued
  uint64_t held_issue = 0;              // FSM word issued while the issue is held
  uint64_t both = 0;                    // clocks in which both FSMs offer
  uint64_t deep[2] = {};                // selections of a FIFO holding >= 2 words

  void clear() { *this = Scoreboard{}; }

  // which FIFO an issued word came from, by its owner tag; -1 = cadence
  static int fifo_of(uint64_t w) {
    int owner = static_cast<int>((w >> 32) & 0xFF);
    if (owner >= TK_OWNER && owner < TK_OWNER + kSources) return 0;
    if (owner >= LS_OWNER && owner < LS_OWNER + kSinks) return 1;
    return -1;
  }
  void offer(int u, uint64_t w) {
    ++offered[u];
    if (q[u].size() >= FIFO_DEPTH) ++refused[u];
    else q[u].push_back(w);
  }
  // one clock, low phase: the merged face shows the word issued at the last
  // edge; the FSM faces show what each FIFO is written at the coming edge
  void sample(Vsrp_store_wrap* d, bool held) {
    if (d->dbg_arm_valid_o) {
      uint64_t w = d->dbg_arm_word_o;
      int u = fifo_of(w);
      if (u >= 0) {
        ++issued[u];
        if (held) ++held_issue;
        if (q[u].empty()) {
          ++invented[u];
        } else {
          if (q[u].front() != w) ++wrong[u];
          q[u].pop_front();
        }
      }
    }
    bool tv = d->dbg_tk_arm_valid_o != 0;
    bool lv = d->dbg_ls_arm_valid_o != 0;
    if (tv) offer(0, d->dbg_tk_arm_word_o);
    if (lv) offer(1, d->dbg_ls_arm_word_o);
    if (tv && lv) ++both;
    // coverage only: the FIFO the issue selects this clock (round-robin,
    // talker first unless it was served last) and how many words it holds
    if (d->dbg_tm_select_o && !held) {
      unsigned ct = d->dbg_tf_cnt_tk_o;
      unsigned cl = d->dbg_tf_cnt_ls_o;
      if (ct != 0 && (!d->dbg_tm_rr_o || cl == 0)) { if (ct >= 2) ++deep[0]; }
      else if (cl >= 2) { ++deep[1]; }
    }
  }
};

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------
struct H {
  Vsrp_store_wrap* d;
  uint64_t t = 0;
  Scoreboard sb;
  bool held = false;          // tm_stall_i for the coming edges
  bool streaming = false;
  // called in the low phase, after the scoreboard: may drive inputs for the
  // coming edge (a request accepted on the clock it observes)
  std::function<void()> hook;
  uint64_t la_actions = 0;

  explicit H(Vsrp_store_wrap* dd) : d(dd) {}

  void step() {
    if (t >= CYCLE_BUDGET) {
      std::fprintf(stderr, "CYCLE_BUDGET: storage arms exceeded %llu clocks\n",
                   static_cast<unsigned long long>(CYCLE_BUDGET));
      std::exit(3);
    }
    d->tm_stall_i = held;
    d->clk_i = 0; d->eval();
    if (d->rst_n) {
      sb.sample(d, held_edge);
      if (d->dbg_la_action_o) ++la_actions;
      if (hook) hook();
    }
    // TX arbiter emulation (03 §8): accept requests, stream the slot out
    d->ser_ready_i = 1;
    d->txreq_ready_i = 0;
    d->ser_req_i = 0;
    bool finished = false;
    if (streaming && d->ser_valid_o && d->ser_last_o) { streaming = false; finished = true; }
    if (!streaming && !finished && d->txreq_valid_o) {
      d->txreq_ready_i = 1;
      d->ser_req_i = 1;
      d->ser_slot_i = d->txreq_slot_o;
      streaming = true;
    }
    d->clk_i = 1; d->eval();
    held_edge = held;
    t++;
  }
  void idle(int n) { for (int i = 0; i < n; i++) step(); }
  void run_ms(int ms) { idle(ms * MS_CYC); }

  void reset() {
    streaming = false; held = false; held_edge = false; hook = nullptr;
    d->rst_n = 0;
    d->own_mac_i = OWN_MAC; d->entity_id_i = EID; d->link_up_i = 0;
    d->mrp_valid_i = 0; d->mrp_data_i = 0; d->mrp_last_i = 0; d->mrp_msrp_i = 1;
    d->req_valid_i = 0; d->req_op_i = 0; d->req_index_i = 0;
    d->req_stream_id_i = 0; d->req_da_i = 0; d->req_vid_i = 0;
    d->req_max_frame_i = 0; d->req_max_interval_i = 0; d->req_lstn_state_i = 0;
    d->txreq_ready_i = 0; d->ser_req_i = 0; d->ser_slot_i = 0; d->ser_ready_i = 1;
    idle(10);
    d->rst_n = 1;
    sb.clear(); la_actions = 0;
    idle(5);
  }

  // feed one header-stripped MSRP MRPDU into the decoder (handshake honored)
  void feed(const std::vector<uint8_t>& b) {
    for (size_t i = 0; i < b.size();) {
      d->mrp_valid_i = 1;
      d->mrp_data_i = b[i];
      d->mrp_msrp_i = 1;
      d->mrp_last_i = (i + 1 == b.size()) ? 1 : 0;
      d->clk_i = 0; d->eval();
      bool ready = d->mrp_ready_o != 0;
      step();
      if (ready) i++;                   // consumed at that posedge
    }
    d->mrp_valid_i = 0;
    d->mrp_last_i = 0;
    idle(8);
  }

  void drive_request(int opc, int idx, uint64_t sid, uint64_t da, int vid, int mfs,
                     int mif, int lstn) {
    d->req_valid_i = 1;
    d->req_op_i = opc; d->req_index_i = idx;
    d->req_stream_id_i = sid; d->req_da_i = da; d->req_vid_i = vid;
    d->req_max_frame_i = mfs; d->req_max_interval_i = mif;
    d->req_lstn_state_i = lstn;
  }
  bool await_response() {
    d->req_valid_i = 0;
    for (int i = 0; i < OP_GUARD; i++) {
      if (d->rsp_valid_o) { step(); return d->rsp_status_o == 0; }
      step();
    }
    return false;
  }
  // class-B op; true when it answered OK
  bool op(int opc, int idx, uint64_t sid, uint64_t da, int vid, int mfs, int mif, int lstn) {
    int guard = OP_GUARD;
    while (!d->req_ready_o && guard--) step();
    drive_request(opc, idx, sid, da, vid, mfs, mif, lstn);
    step();                              // accepted at this posedge
    return await_response();
  }

  int t_reg(int s) const { return (d->dbg_t_reg_o >> (2 * s)) & 3; }
  int l_reg(int k) const { return (d->dbg_l_reg_o >> (2 * k)) & 3; }

 private:
  bool held_edge = false;
};

class SrpStoreSuite {
 public:
  SrpStoreSuite() : d(model.get()), h(d) {}

  int run() {
    printf("timer-arm FIFOs: %d sources, %d sinks\n", kSources, kSinks);
    own_leaveall_fills_both_fifos_in_order();
    held_issue_fills_each_fifo_to_its_guard();
    printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
    return fails ? 1 : 0;
  }

 private:
  const milan::tb::Model<Vsrp_store_wrap> model;
  Vsrp_store_wrap* d;
  H h;
  int checks = 0;
  int fails = 0;

  // Listener JoinIn/Ready for every source but `skip` (talker registrars),
  // Talker Advertise JoinIn for every sink (listener registrars); `la` flags
  // LeaveAll on each message's first vector, as a peer's LeaveAll MRPDU does
  std::vector<uint8_t> peer_declarations(bool la, int skip = -1) const {
    Msg lr{3, 8, true, {}};
    for (int s = 0; s < kSources; ++s) {
      if (s == skip) continue;
      lr.vecs.push_back(Vec{la && lr.vecs.empty(), 1, fv_sid(own_sid(s)), {EV_JOININ}, {DECL_READY}});
    }
    Msg ta{1, 25, false, {}};
    for (int k = 0; k < kSinks; ++k)
      ta.vecs.push_back(Vec{la && k == 0, 1, fv_talker(peer_sid(k), peer_da(k), 2, 29, 1, 3, 1, 500),
                            {EV_JOININ}, {}});
    return lr.vecs.empty() ? mrpdu_body({ta}) : mrpdu_body({lr, ta});
  }

  bool all_registered() const {
    bool in = true;
    for (int s = 0; s < kSources; ++s) in &= h.t_reg(s) == REG_IN;
    for (int k = 0; k < kSinks; ++k) in &= h.l_reg(k) == REG_IN;
    return in;
  }

  // reset, link up, every source declared, every sink settled, and every
  // registrar of both FSMs registered (IN) by the peer
  void bring_up() {
    h.reset();
    d->link_up_i = 1;
    h.idle(10);
    for (int s = 0; s < kSources; ++s)
      h.op(OP_DECL_TK, s, own_sid(s), own_da(s), 2, 29, 1, 0);
    for (int k = 0; k < kSinks; ++k)
      h.op(OP_DECL_LS, k, peer_sid(k), peer_da(k), 2, 0, 0, DECL_READY);
    h.feed(peer_declarations(false));
    h.run_ms(300);
  }

  // run until the encoder accepts an own MSRP LeaveAll (sLA ages both
  // registrar arrays); on that clock, `on_action` may drive a request
  bool await_leaveall(const std::function<void()>& on_action) {
    uint64_t before = h.la_actions;
    bool seen = false;
    h.hook = [&]() {
      if (!seen && h.la_actions != before) { seen = true; on_action(); }
    };
    for (long i = 0; i < static_cast<long>(LEAVEALL_WAIT_MS) * MS_CYC && !seen; ++i) h.step();
    h.hook = nullptr;
    return seen;
  }

  void drain() {
    for (int i = 0; i < 400 && !(h.sb.q[0].empty() && h.sb.q[1].empty()); ++i) h.step();
    h.idle(20);
  }

  // TF1-TF3: an own LeaveAll offers both FIFOs an op in one clock. On a
  // LeaveAll clock where the issue will serve the listener FIFO first (the
  // round-robin points at it), a re-declaration of source 0 is accepted: its
  // gate op cancels the leave timer the LeaveAll just started, so the talker
  // FIFO is offered a second op before its first is issued. On a LeaveAll
  // where it points at the talker FIFO, the peer re-joins all but source 0,
  // whose leave timer then runs out: an odd number of issues, so the next
  // LeaveAll finds the round-robin on the listener FIFO.
  void own_leaveall_fills_both_fifos_in_order() {
    bring_up();
    CHECK(all_registered(), "TF1 precondition: every registrar of both FSMs is IN");
    bool fired = false;
    int attempts = 0;
    while (!fired && attempts < 3) {
      ++attempts;
      bool redeclare = false;
      bool seen = await_leaveall([&]() {
        if (d->dbg_tm_rr_o && d->req_ready_o) {
          h.drive_request(OP_DECL_TK, 0, own_sid(0), own_da(0), 2, 29, 1, 0);
          redeclare = true;
        }
      });
      CHECK(seen, "TF1 precondition: an own LeaveAll is accepted within %d ms (attempt %d)",
            LEAVEALL_WAIT_MS, attempts);
      if (!seen) break;
      if (redeclare) {                   // accepted at the LeaveAll clock's posedge
        CHECK(h.await_response(), "TF1 precondition: the re-declaration of source 0 answers OK");
        fired = true;
        h.feed(peer_declarations(false));
      } else {
        h.idle(4);
        h.feed(peer_declarations(false, 0));
        h.run_ms(LEAVE_MS + 300);
        h.feed(peer_declarations(false));
      }
      h.run_ms(300);
    }
    drain();
    const Scoreboard& sb = h.sb;
    printf("STORE natural %d/%d: offered %llu/%llu issued %llu/%llu both %llu deep %llu/%llu "
           "attempts %d\n", kSources, kSinks,
           static_cast<unsigned long long>(sb.offered[0]), static_cast<unsigned long long>(sb.offered[1]),
           static_cast<unsigned long long>(sb.issued[0]), static_cast<unsigned long long>(sb.issued[1]),
           static_cast<unsigned long long>(sb.both), static_cast<unsigned long long>(sb.deep[0]),
           static_cast<unsigned long long>(sb.deep[1]), attempts);
    CHECK(sb.wrong[0] == 0 && sb.invented[0] == 0 && sb.q[0].empty(),
          "TF1: the talker FIFO issues every op it accepted, once, in order, unmodified "
          "(wrong %llu, invented %llu, left %zu)", static_cast<unsigned long long>(sb.wrong[0]),
          static_cast<unsigned long long>(sb.invented[0]), sb.q[0].size());
    CHECK(sb.wrong[1] == 0 && sb.invented[1] == 0 && sb.q[1].empty(),
          "TF2: the listener FIFO issues every op it accepted, once, in order, unmodified "
          "(wrong %llu, invented %llu, left %zu)", static_cast<unsigned long long>(sb.wrong[1]),
          static_cast<unsigned long long>(sb.invented[1]), sb.q[1].size());
    CHECK(sb.both >= 1, "TF2 precondition: both FSMs offer an op in the same clock (%llu clocks)",
          static_cast<unsigned long long>(sb.both));
    CHECK(fired, "TF1 precondition: the re-declaration lands on a LeaveAll clock (%d attempts)",
          attempts);
    CHECK(sb.deep[0] >= 1,
          "TF1 precondition: the talker FIFO holds two or more words when selected (%llu)",
          static_cast<unsigned long long>(sb.deep[0]));
    CHECK(sb.refused[0] == 0 && sb.refused[1] == 0,
          "TF3: no op is refused while the issue runs (%llu/%llu)",
          static_cast<unsigned long long>(sb.refused[0]), static_cast<unsigned long long>(sb.refused[1]));
  }

  // TF4, TF5: the merged issue held, peer LeaveAll MRPDUs offer each FIFO two ops
  // per stream (the LeaveAll ages, the re-join cancels) until both have been
  // offered more than 32; released, each issues the 32 it accepted
  void held_issue_fills_each_fifo_to_its_guard() {
    bring_up();
    CHECK(all_registered(), "TF4 precondition: every registrar of both FSMs is IN");
    h.idle(4);
    h.held = true;
    const Scoreboard at_hold = h.sb;
    int pdus = 0;
    while ((h.sb.offered[0] - at_hold.offered[0] < STALL_OFFERS
            || h.sb.offered[1] - at_hold.offered[1] < STALL_OFFERS) && pdus < 200) {
      h.feed(peer_declarations(true));
      ++pdus;
    }
    h.idle(20);
    const Scoreboard at_release = h.sb;
    h.held = false;
    drain();
    const Scoreboard& sb = h.sb;
    printf("STORE held %d/%d: %d MRPDUs, offered %llu/%llu refused %llu/%llu queued %zu/%zu "
           "issued after release %llu/%llu\n", kSources, kSinks, pdus,
           static_cast<unsigned long long>(sb.offered[0] - at_hold.offered[0]),
           static_cast<unsigned long long>(sb.offered[1] - at_hold.offered[1]),
           static_cast<unsigned long long>(sb.refused[0]), static_cast<unsigned long long>(sb.refused[1]),
           at_release.q[0].size(), at_release.q[1].size(),
           static_cast<unsigned long long>(sb.issued[0] - at_release.issued[0]),
           static_cast<unsigned long long>(sb.issued[1] - at_release.issued[1]));
    CHECK(sb.offered[0] - at_hold.offered[0] >= STALL_OFFERS
              && sb.offered[1] - at_hold.offered[1] >= STALL_OFFERS,
          "TF4 precondition: each FIFO is offered more than %zu ops while held", FIFO_DEPTH);
    CHECK(at_release.held_issue == 0,
          "TF4 precondition: no FSM op leaves the merged face while the issue is held (%llu)",
          static_cast<unsigned long long>(at_release.held_issue));
    for (int u = 0; u < 2; ++u) {
      const char* name = u ? "listener" : "talker";
      const uint64_t issued = sb.issued[u] - at_release.issued[u];
      CHECK(issued == FIFO_DEPTH && sb.wrong[u] == 0 && sb.invented[u] == 0 && sb.q[u].empty(),
            "TF%d: released, the %s FIFO issues the %zu ops it accepted, in order, unmodified "
            "(issued %llu, wrong %llu, invented %llu, left %zu)", 4 + u, name, FIFO_DEPTH,
            static_cast<unsigned long long>(issued), static_cast<unsigned long long>(sb.wrong[u]),
            static_cast<unsigned long long>(sb.invented[u]), sb.q[u].size());
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  SrpStoreSuite suite;
  return suite.run();
}
