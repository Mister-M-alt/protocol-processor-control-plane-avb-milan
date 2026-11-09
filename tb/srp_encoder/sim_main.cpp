// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_srp_encoder / KL_srp_domain / KL_srp_vlan suite — independent
// expectations, never DUT logic.
//
// The reference packer builds MRPDU byte images straight from 802.1Q
// §10.8.1/§35.2.2 (run grouping over +1 successors, ThreePacked radix-6,
// FourPacked radix-4, AttributeListLength INCLUDING the AttributeList
// EndMark, dual EndMark) and the frames captured for comparison crossed the
// REAL KL_pp_tx_slots RAM via its serialize port. Domain (F10.2) and VLAN
// (F10.3, corrected per-VID refcount) walks are checked event-by-event; a
// bridge phase plays the not-yet-landed event router and feeds both FSMs'
// declarations into the encoder for end-to-end frames.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include "Vsrp_tb_wrap.h"
#include "verilated.h"
#include "../common/verilator_harness.hpp"
#include "../common/req_tag.hpp"

// The tally lives in the suite object below; CHECK names those members, so it
// expands only inside a SrpSuite member function.
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// ---- independent 802.1Q reference packer ----------------------------------
// The widest FirstValue either application carries: Talker Failed is 34 bytes,
// so every FirstValue image in this file is a zero-padded buffer of that size.
constexpr int kFirstValueBytes = 34;

struct Ev {
  int app;            // 0 MSRP, 1 MVRP
  int type;           // AttributeType
  int event;          // 0 New, 1 JoinIn, 2 In, 3 JoinMt, 4 Mt, 5 Lv
  int fp;             // Listener FourPacked declaration parameter
  uint8_t val[kFirstValueBytes];  // FirstValue, wire order, zero-padded
};

static Ev mk_ev(int app, int type, int event, int fp,
                std::initializer_list<uint8_t> bytes) {
  Ev e{}; e.app = app; e.type = type; e.event = event; e.fp = fp;
  memset(e.val, 0, sizeof e.val);
  int i = 0; for (uint8_t b : bytes) e.val[i++] = b;
  return e;
}

// bytes [off, off+len) of a, incremented as one big-endian integer, == b?
static bool inc_eq(const uint8_t* a, const uint8_t* b, int off, int len) {
  uint8_t tmp[kFirstValueBytes];
  memcpy(tmp, a, kFirstValueBytes);
  for (int i = off + len - 1; i >= off; --i) { if (++tmp[i] != 0) break; }
  return memcmp(tmp + off, b + off, len) == 0;
}
static bool eq_rng(const uint8_t* a, const uint8_t* b, int off, int len) {
  return memcmp(a + off, b + off, len) == 0;
}

// +1 successor under the attribute's own increment rule (802.1Q §35.2.2.8/.9)
static bool is_succ(const Ev& p, const Ev& n) {
  if (p.app != n.app || p.type != n.type) return false;
  if (p.app == 1) return inc_eq(p.val, n.val, 0, 2) && eq_rng(p.val, n.val, 2, 32);
  switch (p.type) {
    case 1: case 2:   // Talker: stream_id and DA step together
      return inc_eq(p.val, n.val, 0, 8) && inc_eq(p.val, n.val, 8, 6)
          && eq_rng(p.val, n.val, 14, 20);
    case 3:           // Listener: stream_id steps
      return inc_eq(p.val, n.val, 0, 8) && eq_rng(p.val, n.val, 8, 26);
    case 4:           // Domain: SRclassID and priority step, VID pinned
      return inc_eq(p.val, n.val, 0, 1) && inc_eq(p.val, n.val, 1, 1)
          && eq_rng(p.val, n.val, 2, 32);
  }
  return false;
}

static int attr_len(int app, int type) {
  if (app == 1) return 2;
  switch (type) { case 1: return 25; case 2: return 34; case 3: return 8; }
  return 4;
}

// AttributeTypes a participant registers: MSRP 1..4, MVRP 1 (VID).
static int n_types(int app) { return app ? 1 : 4; }

// One MRPDU (Ethernet frame image) for one application's pending events.
// A LeaveAll MRPDU is per Attribute Type (802.1Q-2014 §10.8.2.6; §10.7.5.20
// NOTE: "it must generate a LeaveAll Attribute for each Attribute Type
// supported by the application"): the first VectorAttribute of every type
// carries LeaveAllEvent, and each type with no vector gets a LeaveAll-only
// VectorAttribute after the drained messages, lowest type first (the
// encoder's documented order): NumberOfValues 0, FirstValue present at its
// full AttributeLength and zero, no packed events (§10.8.2.8 f and g,
// §10.8.2.10.1 NOTE).
static std::vector<uint8_t> model_pdu(const std::vector<Ev>& evs, bool leaveall,
                                      const uint8_t own[6]) {
  const int app = evs[0].app;
  static constexpr uint8_t da_msrp[6] = {0x01, 0x80, 0xC2,
                                         0x00, 0x00, 0x0E};
  static constexpr uint8_t da_mvrp[6] = {0x01, 0x80, 0xC2,
                                         0x00, 0x00, 0x21};
  std::vector<uint8_t> f;
  const uint8_t* da = app ? da_mvrp : da_msrp;
  f.insert(f.end(), da, da + 6);
  f.insert(f.end(), own, own + 6);
  const uint16_t et = app ? 0x88F5 : 0x22EA;
  f.push_back(uint8_t(et >> 8)); f.push_back(uint8_t(et & 0xFF));
  f.push_back(0x00);                                    // ProtocolVersion
  struct Run { int type; const Ev* first; std::vector<int> e3, e4; };
  std::vector<Run> runs;
  for (size_t i = 0; i < evs.size(); ++i) {
    if (!runs.empty() && runs.back().type == evs[i].type
        && int(runs.back().e3.size()) < 12 && is_succ(evs[i - 1], evs[i])) {
      runs.back().e3.push_back(evs[i].event);
      runs.back().e4.push_back(evs[i].fp);
    } else {
      runs.push_back({evs[i].type, &evs[i], {evs[i].event}, {evs[i].fp}});
    }
  }
  size_t r = 0;
  bool type_seen[5] = {false, false, false,
                       false, false};           // indexed by AttributeType
  while (r < runs.size()) {
    const int t = runs[r].type;
    const int alen = attr_len(app, t);
    f.push_back(uint8_t(t)); f.push_back(uint8_t(alen));
    size_t ll_pos = 0;
    if (app == 0) { ll_pos = f.size(); f.push_back(0); f.push_back(0); }
    while (r < runs.size() && runs[r].type == t) {
      const Run& R = runs[r];
      const int nov = int(R.e3.size());
      const int la = (leaveall && !type_seen[t]) ? 1 : 0;
      type_seen[t] = true;
      f.push_back(uint8_t((la << 5) | ((nov >> 8) & 0x1F)));
      f.push_back(uint8_t(nov & 0xFF));
      f.insert(f.end(), R.first->val, R.first->val + alen);
      for (int k = 0; k < (nov + 2) / 3; ++k) {
        const int e0 = (3 * k     < nov) ? R.e3[size_t(3 * k)]     : 0;
        const int e1 = (3 * k + 1 < nov) ? R.e3[size_t(3 * k + 1)] : 0;
        const int e2 = (3 * k + 2 < nov) ? R.e3[size_t(3 * k + 2)] : 0;
        f.push_back(uint8_t(e0 * 36 + e1 * 6 + e2));
      }
      if (app == 0 && t == 3) {
        for (int k = 0; k < (nov + 3) / 4; ++k) {
          int v[4] = {0, 0,
                      0, 0};
          for (int m = 0; m < 4; ++m)
            if (4 * k + m < nov) v[m] = R.e4[size_t(4 * k + m)];
          f.push_back(uint8_t(v[0] * 64 + v[1] * 16 + v[2] * 4 + v[3]));
        }
      }
      ++r;
    }
    f.push_back(0); f.push_back(0);                    // AttributeList EndMark
    if (app == 0) {                                    // §35.2.2.6: EndMark counted
      const int ll = int(f.size()) - int(ll_pos) - 2;
      f[ll_pos] = uint8_t(ll >> 8); f[ll_pos + 1] = uint8_t(ll & 0xFF);
    }
  }
  for (int t = 1; leaveall && t <= n_types(app); ++t) {
    if (type_seen[t]) continue;
    const int alen = attr_len(app, t);
    f.push_back(uint8_t(t)); f.push_back(uint8_t(alen));
    if (app == 0) {                                    // header + FirstValue + EndMark
      f.push_back(uint8_t((alen + 4) >> 8)); f.push_back(uint8_t((alen + 4) & 0xFF));
    }
    f.push_back(0x20); f.push_back(0x00);              // LeaveAll, NumberOfValues 0
    f.insert(f.end(), size_t(alen), uint8_t(0));       // FirstValue, ignored
    f.push_back(0); f.push_back(0);                    // AttributeList EndMark
  }
  f.push_back(0); f.push_back(0);                      // MRPDU EndMark
  return f;
}

// Independent structural read of one captured MRPDU frame image (802.1Q-2014
// §10.8.1.2 BNF): one entry per VectorAttribute. False when the image does
// not parse, an MSRP AttributeListLength disagrees with the counted list, or
// anything follows the MRPDU EndMark.
struct PVec {
  int msg;                    // Message index in the PDU
  int type;                   // AttributeType
  int alen;                   // AttributeLength
  int listlen;                // MSRP AttributeListLength (-1 for MVRP)
  int la;                     // LeaveAllEvent
  int nov;                    // NumberOfValues
  std::vector<uint8_t> fv;    // FirstValue
  int packed;                 // ThreePacked + FourPacked octets
};

static bool parse_pdu(const std::vector<uint8_t>& f, std::vector<PVec>& out) {
  out.clear();
  if (f.size() < 17) return false;
  const bool msrp = (f[12] == 0x22 && f[13] == 0xEA);
  size_t i = 14;
  if (f[i++] != 0x00) return false;                    // ProtocolVersion
  for (int msg = 0;; ++msg) {
    if (i + 2 > f.size()) return false;
    if (f[i] == 0 && f[i + 1] == 0) return i + 2 == f.size();
    const int type = f[i++];
    const int alen = f[i++];
    int ll = -1;
    if (msrp) {
      if (i + 2 > f.size()) return false;
      ll = (f[i] << 8) | f[i + 1];
      i += 2;
    }
    const size_t list0 = i;
    for (;;) {
      if (i + 2 > f.size()) return false;
      if (f[i] == 0 && f[i + 1] == 0) { i += 2; break; }
      PVec v;
      v.msg = msg; v.type = type; v.alen = alen; v.listlen = ll;
      v.la = f[i] >> 5;
      v.nov = ((f[i] & 0x1F) << 8) | f[i + 1];
      i += 2;
      if (i + size_t(alen) > f.size()) return false;
      v.fv.assign(f.begin() + long(i), f.begin() + long(i + size_t(alen)));
      i += size_t(alen);
      v.packed = (v.nov + 2) / 3 + ((msrp && type == 3) ? (v.nov + 3) / 4 : 0);
      i += size_t(v.packed);
      if (i > f.size()) return false;
      out.push_back(v);
    }
    if (msrp && int(i - list0) != ll) return false;
  }
}

// One JoinIn declaration of MSRP AttributeType t (1 Talker Advertise, 2 Talker
// Failed, 3 Listener Ready, 4 Domain) — the LeaveAll suites' building block.
static Ev typed_ev(int t) {
  switch (t) {
    case 1:
      return mk_ev(0, 1, 1, 0, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x01,
                                0x91, 0xE0, 0xF0, 0x00, 0x33, 0x44,
                                0x00, 0x02, 0x00, 0x1D, 0x00, 0x01, 0x70, 0x00,
                                0x00, 0x0F, 0x42});
    case 2: {
      Ev e = mk_ev(0, 2, 1, 0, {});
      for (int i = 0; i < kFirstValueBytes; ++i) e.val[i] = uint8_t(0xB0 + i);
      return e;
    }
    case 3:
      return mk_ev(0, 3, 1, 2, {0, 0, 0, 0, 0, 0, 0x05, 0x10});
    default:
      return mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02});
  }
}

// The bench switch's own LeaveAll-only messages, transcribed from the
// milan-fpga #117 Run B tap capture (tap-runB.pcap, switch port
// 3c:c0:c6:fe:02:11, the LeaveAll MRPDU at 10.429430 s): {type, length,
// AttributeListLength, VectorHeader LeaveAll + NumberOfValues 0}, then that
// many zero FirstValue octets, then the AttributeList EndMark.
struct WireLaOnly { uint8_t head[6]; int zeros; };
constexpr WireLaOnly kSwitchTalkerAdv = {{0x01, 0x19, 0x00,
                                          0x1D, 0x20, 0x00}, 25};
constexpr WireLaOnly kSwitchTalkerFail = {{0x02, 0x22, 0x00,
                                           0x26, 0x20, 0x00}, 34};
constexpr WireLaOnly kSwitchListener = {{0x03, 0x08, 0x00,
                                         0x0C, 0x20, 0x00}, 8};

static void put_la_only(std::vector<uint8_t>& out, const WireLaOnly& w) {
  out.insert(out.end(), w.head, w.head + 6);
  out.insert(out.end(), size_t(w.zeros), uint8_t(0));
  out.push_back(0x00); out.push_back(0x00);
}

// ---- harness ---------------------------------------------------------------
constexpr uint8_t OWN_MAC[6] = {0x02, 0x4B, 0x4C,
                                0x00, 0x00, 0x01};

// enc_ev_value_i is 272 bits wide: nine 32-bit Verilator words.
constexpr int kValueWords = 9;
// Cycles a ready handshake is given before the harness calls it backpressured.
constexpr int kHandshakeTimeoutCycles = 100;
// Cycles the TX request face is waited on before the drain is called lost.
constexpr int kTxreqTimeoutCycles = 5000;
// The longest frame a capture can hold: a tagged Ethernet frame.
constexpr size_t kMaxFrameBytes = 1522;
// Loop guard on one slot's serialize walk: far longer than any legal frame.
constexpr int kSerializeGuardCycles = 8000;

struct H {
  Vsrp_tb_wrap* d;
  std::vector<std::pair<int, uint32_t>> dom_evs;
  std::vector<std::pair<int, int>>      vlan_evs;
  int dom_changes = 0;
  int vlan_errs = 0;
  int commits = 0;
  int allocs = 0;
  int drops = 0;
  int own_actions = 0;
  int mvrp_tx = 0;               // enc_tx_mvrp_o strobes
  // pre-edge samples
  bool last_enc_ready = false;
  bool last_txreq = false;
  int  last_txslot = 0;
  bool last_ser_valid = false;
  bool last_ser_last = false;
  uint8_t last_ser_data = 0;
  bool last_user_ready = false;
  bool last_vlan_ev_valid = false;
  bool last_prepare_done = false;
  bool last_dom_tx_done = false;
  bool last_vlan_tx_done = false;
  int dom_tx_dones = 0;          // dom_tx_done_o strobes
  bool drop_on_accept = false;   // an offer drops on the clock it is taken, as a producer's does

  explicit H(Vsrp_tb_wrap* dd) : d(dd) {}

  void tick() {
    d->clk_i = 0; d->eval();
    // observe PRE-EDGE — what the registers are about to consume
    last_enc_ready     = d->enc_ev_ready_o;
    last_txreq         = d->enc_txreq_valid_o;
    last_txslot        = d->enc_txreq_slot_o;
    last_ser_valid     = d->ser_valid_o;
    last_ser_last      = d->ser_last_o;
    last_ser_data      = d->ser_data_o;
    last_user_ready    = d->vlan_user_ready_o;
    last_vlan_ev_valid = d->vlan_ev_valid_o;
    last_prepare_done = d->enc_la_done_o;
    last_dom_tx_done  = d->dom_tx_done_o;
    last_vlan_tx_done = d->vlan_tx_done_o;
    if (d->dom_tx_done_o) ++dom_tx_dones;
    if (d->enc_la_tx_o) ++own_actions;
    if (d->enc_tx_mvrp_o) ++mvrp_tx;
    if (d->dom_ev_valid_o && d->dom_ev_ready_i)
      dom_evs.push_back({int(d->dom_ev_event_o), uint32_t(d->dom_ev_value_o)});
    if (d->vlan_ev_valid_o && d->vlan_ev_ready_i)
      vlan_evs.push_back({int(d->vlan_ev_event_o), int(d->vlan_ev_vid_o)});
    if (d->dom_evt_change_o) ++dom_changes;
    if (d->vlan_user_err_o) ++vlan_errs;
    if (d->dbg_commit_o) ++commits;
    if (d->dbg_alloc_gnt_o) ++allocs;
    if (d->enc_ev_drop_o) ++drops;
    d->clk_i = 1; d->eval();
    if (drop_on_accept && last_enc_ready) d->enc_ev_valid_i = 0;
  }
  void run(int n) { for (int i = 0; i < n; ++i) tick(); }

  void set_val(const uint8_t v[kFirstValueBytes]) {
    for (int w = 0; w < kValueWords; ++w) d->enc_ev_value_i[w] = 0;
    for (int j = 0; j < kFirstValueBytes; ++j) {
      const int low = 264 - 8 * j;               // byte j at [271-8j : 264-8j]
      d->enc_ev_value_i[low / 32] |= uint32_t(v[j]) << (low % 32);
    }
  }

  // present an event on the push face; it stays offered until enc_ev_valid_i drops
  void offer(const Ev& e) {
    d->enc_ev_app_i       = uint8_t(e.app);
    d->enc_ev_attr_type_i = uint8_t(e.type);
    d->enc_ev_event_i     = uint8_t(e.event);
    d->enc_ev_fourpack_i  = uint8_t(e.fp);
    set_val(e.val);
    d->enc_ev_valid_i = 1;
  }

  bool push_enc(const Ev& e, int timeout = kHandshakeTimeoutCycles) {
    offer(e);
    for (int i = 0; i < timeout; ++i) {
      tick();
      if (last_enc_ready) { d->enc_ev_valid_i = 0; return true; }
    }
    d->enc_ev_valid_i = 0;
    return false;
  }

  // wait for the TX request face, accept it, drain the slot via the pool
  bool wait_and_drain(std::vector<uint8_t>& out, int to = kTxreqTimeoutCycles) {
    int slot = -1;
    for (int i = 0; i < to && slot < 0; ++i) {
      tick();
      if (last_txreq) slot = last_txslot;
    }
    if (slot < 0) return false;
    d->enc_txreq_ready_i = 1; tick(); d->enc_txreq_ready_i = 0;
    d->ser_slot_i = uint8_t(slot); d->ser_req_i = 1; d->ser_ready_i = 1;
    out.clear();
    for (int i = 0; i < kSerializeGuardCycles; ++i) {
      tick();
      d->ser_req_i = 0;
      if (last_ser_valid) {
        out.push_back(last_ser_data);
        if (last_ser_last) { d->ser_ready_i = 0; return true; }
      }
    }
    d->ser_ready_i = 0;
    return false;
  }

  bool capture_pdu(int app, std::vector<uint8_t>& out,
                   int to = kTxreqTimeoutCycles) {
    d->enc_join_tick_i = uint8_t(1u << app);
    tick();
    d->enc_join_tick_i = 0;
    return wait_and_drain(out, to);
  }

  // One MSRP tx! opportunity of the Domain participant as KL_srp_top's round
  // gives it (issue #216): the tick, then until its tx_done_o (the tx!'s
  // message, if any, taken by the bridge), bounded; `own` is the own sLA's
  // round, whose LeaveAll ages the Registrar and takes the txLA! row.
  // Returns whether the tx! was done.
  bool dom_tx(bool own = false) {
    d->dom_tx_tick_i = 1;
    if (own) { d->dom_leaveall_tick_i = 1; d->dom_leaveall_own_i = 1; }
    tick();
    d->dom_tx_tick_i = 0; d->dom_leaveall_tick_i = 0; d->dom_leaveall_own_i = 0;
    bool done = false;
    for (int i = 0; i < 40 && !done; ++i) { tick(); done = last_dom_tx_done; }
    run(2);
    return done;
  }

  // A Domain cadence event (periodic!, or a received LeaveAll: rLA!), then
  // the MSRP tx! opportunity a few clocks later, as KL_srp_top's round gives it.
  void dom_cadence(bool leaveall) {
    if (leaveall) d->dom_leaveall_tick_i = 1; else d->dom_periodic_tick_i = 1;
    tick();
    d->dom_leaveall_tick_i = 0; d->dom_periodic_tick_i = 0;
    run(3);
    dom_tx();
    run(8);
  }

  // One received event (srp_event_e) of a Class A Domain value: every event
  // reaches the Applicant; a registering one (New, JoinIn, JoinMt) and an Lv
  // also the Registrar and the adoption, as KL_srp_top wires them.
  void dom_rx_ev(int cid, int prio, int vid, int nov, int ev) {
    d->dom_rx_class_id_i = uint8_t(cid); d->dom_rx_prio_i = uint8_t(prio);
    d->dom_rx_vid_i = uint16_t(vid); d->dom_rx_nov_i = uint16_t(nov);
    d->dom_rx_any_i = 1; d->dom_rx_ev_i = uint8_t(ev);
    if (ev == 0 || ev == 1 || ev == 3) d->dom_rx_valid_i = 1;
    if (ev == 5) d->dom_rx_lv_i = 1;
    tick();
    d->dom_rx_lv_i = 0; d->dom_rx_valid_i = 0; d->dom_rx_any_i = 0;
  }
  // a registering event (JoinIn), or its Lv
  void dom_rx(int cid, int prio, int vid, int nov, bool lv) {
    dom_rx_ev(cid, prio, vid, nov, lv ? 5 : 1);
  }

  // One MVRP tx! of the VLAN participant (issue #216): the tick, then until
  // tx_done_o (its walk done, every message taken), bounded; `own` is the own
  // sLA's tx! (txLA!). Returns whether the walk was done.
  bool vlan_tx(bool own = false) {
    d->vlan_tx_tick_i = 1; d->vlan_tx_la_i = own ? 1 : 0;
    tick();
    d->vlan_tx_tick_i = 0; d->vlan_tx_la_i = 0;
    bool done = false;
    for (int i = 0; i < 80 && !done; ++i) { tick(); done = last_vlan_tx_done; }
    run(2);
    return done;
  }
  // One received MVRP value: VID `vid` with event `ev` (srp_event_e)
  void vlan_rx(int vid, int ev) {
    d->vlan_rx_valid_i = 1; d->vlan_rx_vid_i = uint16_t(vid); d->vlan_rx_ev_i = uint8_t(ev);
    tick();
    d->vlan_rx_valid_i = 0;
  }

  bool vlan_op(bool join, int vid, int timeout = kHandshakeTimeoutCycles) {
    d->vlan_user_join_i = join; d->vlan_user_vid_i = uint16_t(vid);
    d->vlan_user_valid_i = 1;
    for (int i = 0; i < timeout; ++i) {
      tick();
      if (last_user_ready) { d->vlan_user_valid_i = 0; run(30); return true; }
    }
    d->vlan_user_valid_i = 0;
    return false;
  }
};

namespace {

//! The whole SRP suite as one object: the tally, the Verilated model, the BFM
//! and the frame under comparison are members, so nothing this translation
//! unit mutates lives at file scope (I.2).
class SrpSuite {
 public:
  int run();

 private:
  void check_pdu(const char* name, const std::vector<uint8_t>& got,
                 const std::vector<uint8_t>& exp) {
    CHECK(got.size() == exp.size(), "%s: length got %zu exp %zu",
          name, got.size(), exp.size());
    const bool same = (got == exp);
    CHECK(same, "%s: byte-exact against the independent packer", name);
    if (!same) {
      printf("  exp:"); for (uint8_t b : exp) printf(" %02x", b); printf("\n");
      printf("  got:"); for (uint8_t b : got) printf(" %02x", b); printf("\n");
    }
  }

  void check_leaveall_shape(const char* name, const std::vector<uint8_t>& f,
                            int declared);
  void check_only_flags_differ(const char* name,
                               const std::vector<uint8_t>& la,
                               const std::vector<uint8_t>& plain, int declared);

  void bring_out_of_reset();
  void encode_the_two_minimal_pdus();
  void aggregate_one_window_into_one_frame();
  void carry_a_wide_firstvalue_and_one_leaveall();
  void keep_the_two_participants_independent();
  void pad_the_packed_lanes_and_drop_unknown_types();
  void fold_a_full_table_then_backpressure_the_overflow();
  void flag_every_registered_type_in_one_leaveall();
  void match_the_switch_leaveall_only_vectors();
  void flag_each_type_once_across_repeated_messages();
  void keep_the_mvrp_leaveall_on_its_one_type();
  void take_one_leaveall_per_drain();
  void arbitrate_prepare_and_mvrp();
  void domain_declares_adopts_and_ignores_repeats();
  void domain_surfaces_class_a_and_reverts_on_link_down();
  void domain_rejoin_follows_its_registrar();
  void domain_rejoin_after_a_withdrawal();
  void domain_rejoin_across_adoption_and_link();
  void domain_tx_is_done_once_its_message_is_in();
  void domain_applicant_takes_the_received_messages();
  void domain_vp_is_not_ap();
  bool dom_seq(std::initializer_list<std::pair<int, uint32_t>> exp) const {
    return h.dom_evs == std::vector<std::pair<int, uint32_t>>(exp);
  }
  bool vlan_seq(std::initializer_list<std::pair<int, int>> exp) const {
    return h.vlan_evs == std::vector<std::pair<int, int>>(exp);
  }
  //! the messages the bridge took, as a failure names them
  std::string dom_shown() const {
    std::string t;
    for (const auto& e : h.dom_evs) {
      char b[32];
      std::snprintf(b, sizeof b, "%s%d:%08x", t.empty() ? "" : " ", e.first, e.second);
      t += b;
    }
    return t.empty() ? std::string("nothing") : t;
  }
  std::string vlan_shown() const {
    std::string t;
    for (const auto& e : h.vlan_evs) {
      char b[32];
      std::snprintf(b, sizeof b, "%s%d:VID%d", t.empty() ? "" : " ", e.first, e.second);
      t += b;
    }
    return t.empty() ? std::string("nothing") : t;
  }
  //! entry i's Table 10-3 Applicant state and Registrar IN
  int vapp(int i) const { return int((d->vlan_app_state_o >> (4 * i)) & 0xF); }
  bool vreg(int i) const { return ((d->vlan_reg_in_o >> i) & 1) != 0; }
  void vlan_refcounts_every_vid_and_freezes_the_old_one();
  void vlan_withdraws_each_vid_at_its_tx();
  void bridge_the_fsm_declarations_onto_real_frames();
  void track_which_vids_have_left();
  void vlan_registrar_drives_sj();
  void vlan_registrar_ages_and_note_8();
  bool vr_ladder(int v);
  void vr_rejoin();
  void vlan_vp_is_not_ap();
  void vlan_link_change_restarts_each_vid();
  void vlan_link_up_withdraws_the_held_message();
  void send_the_mvrp_leaveall_alone();
  void keep_one_entry_per_value();
  void restart_the_search_when_the_offer_changes();
  void merge_inside_a_prepared_round();
  void bridge_two_cadences_into_one_vector();
  void search_beside_the_other_drain();
  void keep_every_streamid_bit_in_the_identity();
  void keep_every_vid_bit_in_the_identity();
  void carry_the_newer_firstvalue();
  void merge_the_mvrp_withdrawals();
  void take_the_listener_type_of_a_replacing_s();
  void drop_an_unknown_type_at_once();
  void hold_a_push_off_its_drain_start();
  int bridge(int cycles);
  void expect_frame(const char* name, int app, const std::vector<Ev>& in,
                    const std::vector<Ev>& exp, bool leaveall = false);
  void offer_through_a_drain(const std::string& name, const std::vector<Ev>& drained,
                             const std::vector<Ev>& searched, const Ev& offered,
                             const std::vector<Ev>& after);
  void offer_on_the_start_clock(const std::string& name, const Ev& pending, const Ev& offered,
                                bool prepared);

  int checks = 0;
  int fails = 0;

  const milan::tb::Model<Vsrp_tb_wrap> model;
  Vsrp_tb_wrap* const d = model.get();
  H h{d};
  // sized, so a row that reads a frame nothing was captured into fails by name
  std::vector<uint8_t> got = std::vector<uint8_t>(kMaxFrameBytes);
};

void SrpSuite::bring_out_of_reset() {
  // defaults
  d->rst_n = 0;
  d->enc_ev_valid_i = 0; d->enc_join_tick_i = 0; d->enc_leaveall_i = 0;
  d->enc_la_prepare_i = 0;
  d->enc_txreq_ready_i = 0;
  d->ser_req_i = 0; d->ser_ready_i = 0; d->ser_slot_i = 0;
  d->dom_link_up_i = 0; d->dom_rx_valid_i = 0; d->dom_rx_lv_i = 0;
  d->dom_rx_any_i = 0; d->dom_rx_ev_i = 0;
  d->dom_periodic_tick_i = 0; d->dom_leaveall_tick_i = 0; d->dom_leaveall_own_i = 0;
  d->dom_tx_tick_i = 0;
  d->dom_ev_ready_i = 1;
  d->vlan_user_valid_i = 0; d->vlan_periodic_tick_i = 0;
  d->vlan_leaveall_tick_i = 0; d->vlan_ev_ready_i = 1; d->vlan_mvrp_tx_i = 0;
  d->vlan_rx_valid_i = 0; d->vlan_rx_vid_i = 0; d->vlan_rx_ev_i = 0;
  d->vlan_tx_tick_i = 0; d->vlan_tx_la_i = 0;
  d->vlan_link_up_i = 1;
  uint64_t mac = 0;
  for (int i = 0; i < 6; ++i) mac = (mac << 8) | OWN_MAC[i];
  d->enc_own_mac_i = mac;
  h.run(5);
  d->rst_n = 1;
  h.run(3);
}

void SrpSuite::encode_the_two_minimal_pdus() {
  // =====================================================================
  // E1 — single Domain New: minimal MSRP PDU, both EndMarks, listlen rule
  // =====================================================================
  {
    std::vector<Ev> evs = {mk_ev(0, 4, 0, 0, {0x06, 0x03, 0x00, 0x02})};
    CHECK(h.push_enc(evs[0]), "E1 push accepted");
    const int c0 = h.commits;
    h.run(100);
    CHECK(h.commits == c0, "E1 no emission before the join tick");
    CHECK(h.capture_pdu(0, got), "E1 PDU captured");
    check_pdu("E1", got, model_pdu(evs, false, OWN_MAC));
    CHECK(got.size() == 30, "E1 frame is 30 bytes got %zu", got.size());
    CHECK(got[14] == 0x00, "E1 ProtocolVersion 0");
    CHECK(got[17] == 0x00 && got[18] == 0x09,
          "E1 AttributeListLength 9 INCLUDES the EndMark got %02x%02x",
          got[17], got[18]);
    REQ_TAG("REQ-SRP-003", "DIR", "E1 dual EndMark explicit ahead of MAC padding");
    CHECK(got[26] == 0 && got[27] == 0 && got[28] == 0 && got[29] == 0,
          "E1 dual EndMark explicit ahead of MAC padding");
    CHECK(h.commits == c0 + 1, "E1 exactly one commit");
    CHECK(d->enc_dbg_cnt_msrp_o == 0, "E1 pending table drained");
  }

  // =====================================================================
  // E2 — single MVRP VID New: no AttributeListLength field in MVRP
  // =====================================================================
  {
    std::vector<Ev> evs = {mk_ev(1, 1, 0, 0, {0x00, 0x02})};
    CHECK(h.push_enc(evs[0]), "E2 push accepted");
    CHECK(h.capture_pdu(1, got), "E2 PDU captured");
    check_pdu("E2", got, model_pdu(evs, false, OWN_MAC));
    CHECK(got.size() == 26, "E2 frame is 26 bytes got %zu", got.size());
    CHECK(got[12] == 0x88 && got[13] == 0xF5, "E2 MVRP EtherType");
    CHECK(got[15] == 1 && got[16] == 2,
          "E2 message header is {type, len} with NO list-length field");
  }
}

void SrpSuite::aggregate_one_window_into_one_frame() {
  // =====================================================================
  // E3 — cadence aggregation: N pushed events -> ONE frame on the tick
  // =====================================================================
  {
    std::vector<Ev> evs = {
      mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02}),
      mk_ev(0, 3, 0, 2, {0, 0, 0, 0, 0, 0, 0, 0x10}),
      mk_ev(0, 3, 1, 2, {0, 0, 0, 0, 0, 0, 0, 0x40}),   // not a successor
      mk_ev(0, 1, 0, 0, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x07,
                         0x91, 0xE0, 0xF0, 0x00, 0x11, 0x22,
                         0x00, 0x02, 0x00, 0x64, 0x00, 0x01, 0x70, 0x00,
                         0x00, 0x0F, 0x42}),
    };
    const int c0 = h.commits;
    const int a0 = h.allocs;
    for (auto& e : evs) CHECK(h.push_enc(e), "E3 push accepted");
    h.run(200);
    CHECK(h.commits == c0 && h.allocs == a0,
          "E3 events accumulate silently between ticks");
    CHECK(d->enc_dbg_cnt_msrp_o == 4, "E3 four events pending");
    CHECK(h.capture_pdu(0, got), "E3 PDU captured");
    CHECK(h.commits == c0 + 1 && h.allocs == a0 + 1,
          "E3 one window -> one MRPDU (one alloc, one commit)");
    check_pdu("E3", got, model_pdu(evs, false, OWN_MAC));
    CHECK(d->enc_dbg_cnt_msrp_o == 0, "E3 drained");
  }

  // =====================================================================
  // E4 — Listener sorted run: NoV=3, mixed events, FourPacked lanes
  // =====================================================================
  {
    std::vector<Ev> evs = {
      mk_ev(0, 3, 0, 2, {0, 0, 0, 0, 0, 0, 0x01, 0x10}),
      mk_ev(0, 3, 1, 3, {0, 0, 0, 0, 0, 0, 0x01, 0x11}),
      mk_ev(0, 3, 1, 1, {0, 0, 0, 0, 0, 0, 0x01, 0x12}),
    };
    for (auto& e : evs) CHECK(h.push_enc(e), "E4 push accepted");
    CHECK(h.capture_pdu(0, got), "E4 PDU captured");
    check_pdu("E4", got, model_pdu(evs, false, OWN_MAC));
    CHECK(got[19] == 0x00 && got[20] == 0x03, "E4 NumberOfValues 3");
    CHECK(got[29] == 0 * 36 + 1 * 6 + 1, "E4 ThreePacked radix-6 byte");
    CHECK(got[30] == 2 * 64 + 3 * 16 + 1 * 4 + 0,
          "E4 FourPacked radix-4 byte (pad 0)");
  }

  // =====================================================================
  // E5 — Talker Advertise run of 2, then a same-type NON-successor
  //      (stream_id steps but DA does not): new vector, same message
  // =====================================================================
  {
    Ev t0 = mk_ev(0, 1, 0, 0, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x07,
                               0x91, 0xE0, 0xF0, 0x00, 0x11, 0x22,
                               0x00, 0x02, 0x00, 0x64, 0x00, 0x01, 0x70, 0x00,
                               0x00, 0x0F, 0x42});
    Ev t1 = t0; t1.val[7] = 0x08; t1.val[13] = 0x23; t1.event = 1; // +1 both
    Ev t2 = t0; t2.val[7] = 0x09; t2.val[13] = 0x23;               // DA frozen
    std::vector<Ev> evs = {t0, t1, t2};
    for (auto& e : evs) CHECK(h.push_enc(e), "E5 push accepted");
    CHECK(h.capture_pdu(0, got), "E5 PDU captured");
    check_pdu("E5", got, model_pdu(evs, false, OWN_MAC));
    CHECK(got[19] == 0x00 && got[20] == 0x02,
          "E5 first vector aggregated NoV=2");
  }
}

void SrpSuite::carry_a_wide_firstvalue_and_one_leaveall() {
  // =====================================================================
  // E6 — Talker Failed: 34-byte FirstValue survives byte-exact
  // =====================================================================
  {
    Ev f0 = mk_ev(0, 2, 0, 0, {});
    for (int i = 0; i < kFirstValueBytes; ++i) f0.val[i] = uint8_t(0xA0 + i);
    std::vector<Ev> evs = {f0};
    CHECK(h.push_enc(f0), "E6 push accepted");
    CHECK(h.capture_pdu(0, got), "E6 PDU captured");
    check_pdu("E6", got, model_pdu(evs, false, OWN_MAC));
  }

  // =====================================================================
  // E7 — LeaveAll injection: per Attribute Type — the first VectorHeader
  //      of EACH type carries it (802.1Q §10.8.2.6), then consumed — the
  //      next PDU carries none
  // =====================================================================
  {
    std::vector<Ev> evs = {
      mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02}),
      mk_ev(0, 3, 1, 2, {0, 0, 0, 0, 0, 0, 0x02, 0x50}),
    };
    d->enc_leaveall_i = 1; h.tick(); d->enc_leaveall_i = 0;
    for (auto& e : evs) CHECK(h.push_enc(e), "E7 push accepted");
    CHECK(h.capture_pdu(0, got), "E7 PDU captured");
    check_pdu("E7", got, model_pdu(evs, true, OWN_MAC));
    CHECK((got[19] >> 5) == 1, "E7 LeaveAllEvent rides the Domain vector");
    // second message: type@28 len@29 listlen@30-31 -> its VectorHeader @32
    CHECK((got[32] >> 5) == 1,
          "E7 the Listener message carries its own LeaveAllEvent");
    std::vector<Ev> ev2 = {mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02})};
    CHECK(h.push_enc(ev2[0]), "E7b push accepted");
    CHECK(h.capture_pdu(0, got), "E7b PDU captured");
    check_pdu("E7b", got, model_pdu(ev2, false, OWN_MAC));
    CHECK((got[19] >> 5) == 0, "E7b LeaveAll consumed by the first PDU");
  }
}

void SrpSuite::keep_the_two_participants_independent() {
  // =====================================================================
  // E8 — both participants pending; each tick drains ONLY its own table
  // =====================================================================
  {
    std::vector<Ev> em = {mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02})};
    std::vector<Ev> ev = {mk_ev(1, 1, 1, 0, {0x00, 0x02}),
                          mk_ev(1, 1, 0, 0, {0x00, 0x05})};
    CHECK(h.push_enc(em[0]), "E8 MSRP push");
    CHECK(h.push_enc(ev[0]) && h.push_enc(ev[1]), "E8 MVRP pushes");
    CHECK(h.capture_pdu(0, got), "E8 MSRP PDU captured");
    check_pdu("E8-msrp", got, model_pdu(em, false, OWN_MAC));
    CHECK(d->enc_dbg_cnt_mvrp_o == 2, "E8 MVRP table untouched by MSRP tick");
    CHECK(h.capture_pdu(1, got), "E8 MVRP PDU captured");
    check_pdu("E8-mvrp", got, model_pdu(ev, false, OWN_MAC));
    CHECK(d->enc_dbg_cnt_mvrp_o == 0, "E8 MVRP drained by its own tick");
  }

  // =====================================================================
  // E9 — simultaneous ticks: MSRP first, MVRP queued behind it
  // =====================================================================
  {
    std::vector<Ev> em = {mk_ev(0, 3, 5, 0, {0, 0, 0, 0, 0, 0, 0x03, 0x33})};
    std::vector<Ev> ev = {mk_ev(1, 1, 5, 0, {0x00, 0x07})};
    CHECK(h.push_enc(em[0]) && h.push_enc(ev[0]), "E9 pushes");
    d->enc_join_tick_i = 3; h.tick(); d->enc_join_tick_i = 0;
    CHECK(h.wait_and_drain(got), "E9 first PDU");
    check_pdu("E9-msrp", got, model_pdu(em, false, OWN_MAC));
    CHECK(h.wait_and_drain(got), "E9 second PDU follows automatically");
    check_pdu("E9-mvrp", got, model_pdu(ev, false, OWN_MAC));
  }
}

void SrpSuite::pad_the_packed_lanes_and_drop_unknown_types() {
  // =====================================================================
  // E10 — MVRP VID run + FourPacked absence; 5-element Listener run:
  //       ThreePacked and FourPacked padding both exercised
  // =====================================================================
  {
    std::vector<Ev> ev = {mk_ev(1, 1, 0, 0, {0x00, 0x10}),
                          mk_ev(1, 1, 1, 0, {0x00, 0x11}),
                          mk_ev(1, 1, 1, 0, {0x00, 0x12})};
    for (auto& e : ev) CHECK(h.push_enc(e), "E10 MVRP push");
    CHECK(h.capture_pdu(1, got), "E10 MVRP PDU captured");
    check_pdu("E10-mvrp", got, model_pdu(ev, false, OWN_MAC));
    std::vector<Ev> el;
    for (int i = 0; i < 5; ++i) {
      Ev e = mk_ev(0, 3, (i == 0) ? 0 : 1, (i % 4),
                   {0, 0, 0, 0, 0, 0, 0x09, uint8_t(0x60 + i)});
      el.push_back(e);
      CHECK(h.push_enc(e), "E10 Listener push");
    }
    CHECK(h.capture_pdu(0, got), "E10 Listener PDU captured");
    check_pdu("E10-lstn", got, model_pdu(el, false, OWN_MAC));
    CHECK(got[19] == 0x00 && got[20] == 0x05, "E10 NoV=5 aggregated");
  }

  // =====================================================================
  // E11 — unknown AttributeType is dropped with a strobe, never encoded
  // =====================================================================
  {
    const int dr0 = h.drops;
    Ev bad = mk_ev(0, 9, 0, 0, {0x01});
    CHECK(h.push_enc(bad), "E11 handshake still completes");
    CHECK(h.drops == dr0 + 1, "E11 drop strobe fired");
    CHECK(d->enc_dbg_cnt_msrp_o == 0, "E11 nothing pended");
  }
}

void SrpSuite::fold_a_full_table_then_backpressure_the_overflow() {
  // =====================================================================
  // E12 — full-table run: 12 consecutive Listeners fold to ONE NoV=12
  //       vector; the 13th push backpressures until the drain
  // =====================================================================
  {
    std::vector<Ev> el;
    for (int i = 0; i < 12; ++i) {
      Ev e = mk_ev(0, 3, (i == 0) ? 0 : 1, 2,
                   {0, 0, 0, 0, 0, 0, 0x0A, uint8_t(0x00 + i)});
      el.push_back(e);
      CHECK(h.push_enc(e), "E12 push %d accepted", i);
    }
    Ev extra = mk_ev(0, 3, 1, 2, {0, 0, 0, 0, 0, 0, 0x0A, 0x0C});
    CHECK(!h.push_enc(extra, 40), "E12 13th push backpressured while full");
    CHECK(h.capture_pdu(0, got), "E12 PDU captured");
    check_pdu("E12", got, model_pdu(el, false, OWN_MAC));
    CHECK(got[19] == 0x00 && got[20] == 0x0C, "E12 NoV=12 single vector");
    CHECK(h.push_enc(extra), "E12 push accepted after the drain");
    CHECK(h.capture_pdu(0, got), "E12b PDU captured");
    check_pdu("E12b", got, model_pdu({extra}, false, OWN_MAC));
  }
}

// Structural LeaveAll properties of one captured MSRP LeaveAll MRPDU whose
// drain declared the AttributeTypes in `declared` (bit t-1 for type t), read
// by the independent parser — never by the packer the byte-exact check uses.
void SrpSuite::check_leaveall_shape(const char* name,
                                    const std::vector<uint8_t>& f,
                                    int declared) {
  std::vector<PVec> vs;
  CHECK(parse_pdu(f, vs), "%s: one well-formed MRPDU, list lengths agree", name);
  int last_declared_msg = -1;
  for (const PVec& v : vs) {
    if (v.nov > 0 && v.msg > last_declared_msg) last_declared_msg = v.msg;
  }
  for (int t = 1; t <= 4; ++t) {
    int flags = 0;
    int la_only = 0;
    int first_la = -1;
    for (const PVec& v : vs) {
      if (v.type != t) continue;
      if (first_la < 0) first_la = v.la;
      if (v.la == 1) ++flags;
      if (v.nov == 0) ++la_only;
    }
    CHECK(flags == 1 && first_la == 1,
          "%s: type %d carries LeaveAll once, on its first vector (%d flags)",
          name, t, flags);
    const bool is_declared = (declared >> (t - 1)) & 1;
    CHECK(la_only == (is_declared ? 0 : 1),
          "%s: type %d (%s) has %d LeaveAll-only vectors", name, t,
          is_declared ? "declared" : "not declared", la_only);
  }
  for (const PVec& v : vs) {
    if (v.nov != 0) continue;
    bool zero = true;
    for (uint8_t b : v.fv) zero = zero && (b == 0);
    const int alen = attr_len(0, v.type);
    CHECK(v.la == 1 && v.packed == 0 && zero && int(v.fv.size()) == alen
          && v.listlen == alen + 4 && v.msg > last_declared_msg,
          "%s: type %d LeaveAll-only vector: LeaveAll, NoV 0, zero FirstValue"
          " of AttributeLength %d, no packed events, AttributeListLength %d,"
          " after every declared message", name, v.type, alen, alen + 4);
  }
}

// The same drain with and without LeaveAll: the declared messages are
// byte-identical except the LeaveAllEvent bit of one VectorHeader per
// declared type, and only the LeaveAll-only messages are added — nothing at
// all when every registered type is declared.
void SrpSuite::check_only_flags_differ(const char* name,
                                       const std::vector<uint8_t>& la,
                                       const std::vector<uint8_t>& plain,
                                       int declared) {
  int added = 0;
  int n_declared = 0;
  for (int t = 1; t <= 4; ++t) {
    if ((declared >> (t - 1)) & 1) ++n_declared;
    else                           added += attr_len(0, t) + 8;
  }
  CHECK(la.size() == plain.size() + size_t(added),
        "%s: LeaveAll adds exactly %d octets (got %zu, plain %zu)",
        name, added, la.size(), plain.size());
  int diffs = 0;
  bool only_la_bit = true;
  for (size_t i = 0; i + 2 < plain.size() && i < la.size(); ++i) {
    if (la[i] == plain[i]) continue;
    ++diffs;
    if ((la[i] ^ plain[i]) != 0x20 || (la[i] & 0x20) == 0) only_la_bit = false;
  }
  CHECK(diffs == n_declared && only_la_bit,
        "%s: declared messages differ only by one LeaveAllEvent bit per type"
        " (%d octets differ, %d types declared)", name, diffs, n_declared);
}

void SrpSuite::flag_every_registered_type_in_one_leaveall() {
  // =====================================================================
  // L1 — every combination of declared MSRP types (15 non-empty subsets
  //      of {Talker Advertise, Talker Failed, Listener, Domain}): the
  //      LeaveAll MRPDU flags every registered type once and adds one
  //      LeaveAll-only vector for each type it declares nothing of; the
  //      same drain without LeaveAll is the reference for "unchanged"
  // =====================================================================
  constexpr int kPushOrder[4] = {4, 3,
                                 1, 2};
  for (int mask = 1; mask <= 15; ++mask) {
    const std::string tag = "L1[" + std::to_string(mask) + "]";
    const char* name = tag.c_str();
    std::vector<Ev> evs;
    for (int k = 0; k < 4; ++k) {             // rotate the push order too
      const int t = kPushOrder[(k + mask) % 4];
      if ((mask >> (t - 1)) & 1) evs.push_back(typed_ev(t));
    }
    d->enc_leaveall_i = 1; h.tick(); d->enc_leaveall_i = 0;
    for (auto& e : evs) CHECK(h.push_enc(e), "%s push accepted", name);
    CHECK(h.capture_pdu(0, got), "%s LeaveAll PDU captured", name);
    const std::vector<uint8_t> la = got;
    check_pdu(name, la, model_pdu(evs, true, OWN_MAC));
    check_leaveall_shape(name, la, mask);
    for (auto& e : evs) CHECK(h.push_enc(e), "%s plain push accepted", name);
    CHECK(h.capture_pdu(0, got), "%s plain PDU captured", name);
    check_only_flags_differ(name, la, got, mask);
  }
}

void SrpSuite::match_the_switch_leaveall_only_vectors() {
  // =====================================================================
  // L2 — the NumberOfValues-0 vectors byte-for-byte: a Domain-only drain
  //      closes with exactly the three LeaveAll-only messages the bench
  //      switch sends after its own Domain message; the Domain one (the
  //      switch never needs it) from 802.1Q §10.8.2.8 f by hand
  // =====================================================================
  {
    const Ev dm = typed_ev(4);
    d->enc_leaveall_i = 1; h.tick(); d->enc_leaveall_i = 0;
    CHECK(h.push_enc(dm), "L2 push accepted");
    CHECK(h.capture_pdu(0, got), "L2 PDU captured");
    std::vector<uint8_t> tail;
    put_la_only(tail, kSwitchTalkerAdv);
    put_la_only(tail, kSwitchTalkerFail);
    put_la_only(tail, kSwitchListener);
    tail.push_back(0x00); tail.push_back(0x00);          // MRPDU EndMark
    constexpr size_t kDomainEnd = 15 + 13;   // eth + version, Domain message
    CHECK(got.size() == kDomainEnd + tail.size(),
          "L2 frame is %zu octets got %zu", kDomainEnd + tail.size(), got.size());
    CHECK(got.size() == kDomainEnd + tail.size()
          && std::equal(tail.begin(), tail.end(), got.begin() + long(kDomainEnd)),
          "L2 LeaveAll-only tail equals the switch's wire bytes");
    CHECK(got[19] == 0x20 && got[20] == 0x01,
          "L2 the declared Domain vector carries LeaveAll, NoV 1");
  }
  {
    const Ev ls = typed_ev(3);
    d->enc_leaveall_i = 1; h.tick(); d->enc_leaveall_i = 0;
    CHECK(h.push_enc(ls), "L2b push accepted");
    CHECK(h.capture_pdu(0, got), "L2b PDU captured");
    const std::vector<uint8_t> dom_la_only = {0x04, 0x04, 0x00, 0x08,
                                              0x20, 0x00,
                                              0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00};
    CHECK(got.size() > dom_la_only.size() + 2
          && std::equal(dom_la_only.begin(), dom_la_only.end(),
                        got.end() - long(dom_la_only.size() + 2)),
          "L2b Domain LeaveAll-only message: list length 8, NoV 0, 4 zero octets");
  }
}

void SrpSuite::flag_each_type_once_across_repeated_messages() {
  // =====================================================================
  // L3 — a type spread over two vectors of one message AND a later second
  //      message of the same type: LeaveAll only on its first vector
  // =====================================================================
  std::vector<Ev> evs = {
    mk_ev(0, 3, 1, 2, {0, 0, 0, 0, 0, 0, 0x07, 0x10}),
    mk_ev(0, 3, 1, 2, {0, 0, 0, 0, 0, 0, 0x07, 0x40}),   // vector 2
    mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02}),
    mk_ev(0, 3, 1, 3, {0, 0, 0, 0, 0, 0, 0x07, 0x70}),   // message 3
  };
  d->enc_leaveall_i = 1; h.tick(); d->enc_leaveall_i = 0;
  for (auto& e : evs) CHECK(h.push_enc(e), "L3 push accepted");
  CHECK(h.capture_pdu(0, got), "L3 PDU captured");
  check_pdu("L3", got, model_pdu(evs, true, OWN_MAC));
  check_leaveall_shape("L3", got, 0xC);
  std::vector<PVec> vs;
  CHECK(parse_pdu(got, vs) && vs.size() == 6, "L3 six vectors got %zu", vs.size());
  CHECK(vs.size() == 6 && vs[0].type == 3 && vs[0].la == 1
        && vs[1].type == 3 && vs[1].msg == 0 && vs[1].la == 0
        && vs[3].type == 3 && vs[3].msg == 2 && vs[3].la == 0,
        "L3 the Listener's second vector and second message carry no LeaveAll");
}

void SrpSuite::keep_the_mvrp_leaveall_on_its_one_type() {
  // =====================================================================
  // L4 — MVRP has one Attribute Type: its LeaveAll MRPDU is unchanged —
  //      the first VID vector only, nothing appended; the MSRP lane is
  //      untouched by it
  // =====================================================================
  std::vector<Ev> ev = {mk_ev(1, 1, 1, 0, {0x00, 0x02}),
                        mk_ev(1, 1, 1, 0, {0x00, 0x09})};   // two vectors
  d->enc_leaveall_i = 2; h.tick(); d->enc_leaveall_i = 0;
  for (auto& e : ev) CHECK(h.push_enc(e), "L4 push accepted");
  CHECK(h.capture_pdu(1, got), "L4 MVRP PDU captured");
  check_pdu("L4", got, model_pdu(ev, true, OWN_MAC));
  const std::vector<uint8_t> la = got;
  std::vector<PVec> vs;
  CHECK(parse_pdu(la, vs) && vs.size() == 2 && vs[0].la == 1 && vs[1].la == 0
        && vs[0].msg == 0 && vs[1].msg == 0,
        "L4 LeaveAll on the first VID vector only, one message, none added");
  for (auto& e : ev) CHECK(h.push_enc(e), "L4 plain push accepted");
  CHECK(h.capture_pdu(1, got), "L4 plain PDU captured");
  int diffs = 0;
  for (size_t i = 0; i < got.size() && i < la.size(); ++i) diffs += (got[i] != la[i]);
  CHECK(la.size() == got.size() && diffs == 1 && (la[17] ^ got[17]) == 0x20,
        "L4 with and without LeaveAll differ by the one flag bit");
  const Ev dm = typed_ev(4);
  CHECK(h.push_enc(dm), "L4 MSRP push accepted");
  CHECK(h.capture_pdu(0, got), "L4 MSRP PDU captured");
  check_pdu("L4 MSRP lane", got, model_pdu({dm}, false, OWN_MAC));
}

void SrpSuite::take_one_leaveall_per_drain() {
  // =====================================================================
  // L5 — a LeaveAll requested while a drain is already writing is carried
  //      WHOLE by the next MRPDU: never split across two, never dropped
  // =====================================================================
  {
    const Ev tf = typed_ev(2);        // 34-byte FirstValue: a long drain
    CHECK(h.push_enc(tf), "L5 push accepted");
    const int c0 = h.commits;
    d->enc_join_tick_i = 1; h.tick(); d->enc_join_tick_i = 0;
    h.run(30);                        // past the first VectorHeader
    CHECK(h.commits == c0, "L5 the drain is still writing");
    d->enc_leaveall_i = 1; h.tick(); d->enc_leaveall_i = 0;
    CHECK(h.wait_and_drain(got), "L5 running drain completes");
    check_pdu("L5 running drain", got, model_pdu({tf}, false, OWN_MAC));
    CHECK(h.push_enc(tf), "L5 second push accepted");
    CHECK(h.capture_pdu(0, got), "L5 next PDU captured");
    check_pdu("L5 next PDU", got, model_pdu({tf}, true, OWN_MAC));
  }
  // =====================================================================
  // L6 — a LeaveAll on the drain's start cycle is taken by that drain,
  //      once: the following MRPDU carries none
  // =====================================================================
  {
    const Ev dm = typed_ev(4);
    CHECK(h.push_enc(dm), "L6 push accepted");
    d->enc_leaveall_i = 1; d->enc_join_tick_i = 1; h.tick();
    d->enc_leaveall_i = 0; d->enc_join_tick_i = 0;
    CHECK(h.wait_and_drain(got), "L6 PDU captured");
    check_pdu("L6 start-cycle LeaveAll", got, model_pdu({dm}, true, OWN_MAC));
    CHECK(h.push_enc(dm), "L6 second push accepted");
    CHECK(h.capture_pdu(0, got), "L6 next PDU captured");
    check_pdu("L6 next PDU", got, model_pdu({dm}, false, OWN_MAC));
  }
}

// D, DR, DT, DA: the AttributeEvent codes of 802.1Q-2018 10.8.2.10.4 the
// Domain and VID messages are graded with, the default Domain FirstValue {6,
// 3, VID 2}, and the Table 10-3 Applicant states the observability ports
// report (issue #216). Every expected message comes from Table 10-3 and Table
// 10-4, not from the RTL.
constexpr int kNew = 0;
constexpr int kJoinIn = 1;
constexpr int kIn = 2;
constexpr int kJoinMt = 3;
constexpr int kMt = 4;
constexpr int kLv = 5;
constexpr uint32_t kDefDomain = 0x06030002u;
constexpr int kVO = 0;
constexpr int kVP = 1;
constexpr int kVN = 2;
constexpr int kAN = 3;
constexpr int kAA = 4;
constexpr int kQA = 5;
constexpr int kLA = 6;

void SrpSuite::domain_declares_adopts_and_ignores_repeats() {
  // =====================================================================
  // D — Domain FSM walk (F10.2). The declared value's Applicant follows
  //     802.1Q-2018 Table 10-3 (issue #216): each message is the one the table
  //     gives at a tx! opportunity (h.dom_tx), none at the request
  // =====================================================================
  {
    // D1: startup with the link already up = declare the defaults (Begin!,
    // then New!: VN), nothing before a tx!; then New, New again from AN,
    // JoinMt from AA (the Registrar MT, note 8), and nothing from QA
    h.dom_evs.clear();
    d->dom_link_up_i = 1;
    h.run(10);
    CHECK(h.dom_evs.empty() && d->dom_app_state_o == kVN,
          "D1 LINK_UP declares with New!: VN, nothing sent before a tx! (state %d)",
          int(d->dom_app_state_o));
    bool done = h.dom_tx();
    CHECK(done && h.dom_evs.size() == 1 && h.dom_evs[0].first == kNew
          && h.dom_evs[0].second == kDefDomain,
          "D1 New {6, 3, 2} got ev %d val %08x",
          h.dom_evs.empty() ? -1 : h.dom_evs[0].first,
          h.dom_evs.empty() ? 0u : h.dom_evs[0].second);
    done = h.dom_tx() && done;
    done = h.dom_tx() && done;
    done = h.dom_tx() && done;
    CHECK(done && dom_seq({{kNew, kDefDomain}, {kNew, kDefDomain}, {kJoinMt, kDefDomain}})
          && d->dom_app_state_o == kQA,
          "D1 four tx!: New, New again (AN), JoinMt (AA), then nothing (QA), each done (%s)",
          dom_shown().c_str());
    CHECK(d->dom_class_a_prio_o == 3 && d->dom_class_a_vid_o == 2,
          "D1 class-D defaults {3, 2}");
    CHECK(d->dom_adopted_o == 0, "D1 DEFAULTS state");

    // D2: periodic! (QA -> AA), then the tx!: JoinMt, nothing registered
    h.dom_evs.clear();
    h.dom_cadence(false);
    CHECK(dom_seq({{kJoinMt, kDefDomain}}), "D2 periodic JoinMt");

    // D3: a received LeaveAll (rLA!: QA -> VP): the tx! re-joins, JoinMt
    // (VP -> AA), and so does the tx! after it (AA -> QA)
    h.dom_evs.clear();
    h.dom_cadence(true);
    CHECK(dom_seq({{kJoinMt, kDefDomain}}) && d->dom_app_state_o == kAA,
          "D3 LeaveAll re-join, then AA (%s)", dom_shown().c_str());
    h.dom_tx();
    CHECK(dom_seq({{kJoinMt, kDefDomain}, {kJoinMt, kDefDomain}}) && d->dom_app_state_o == kQA,
          "D3 the tx! after the LeaveAll's re-join sends JoinMt from AA, then QA");

    // D4: adopt a differing Class A Domain {6, 5, VID 7}: the old value's Lv
    // at once, the adopted value New! (VN): its New at the next tx!
    h.dom_evs.clear();
    const int ch0 = h.dom_changes;
    d->dom_rx_class_id_i = 6; d->dom_rx_prio_i = 5;
    d->dom_rx_vid_i = 7; d->dom_rx_nov_i = 1;
    d->dom_rx_valid_i = 1; h.tick(); d->dom_rx_valid_i = 0;
    h.run(10);
    CHECK(dom_seq({{kLv, kDefDomain}}), "D4 Lv of the old declaration first, at once");
    h.dom_tx();
    CHECK(dom_seq({{kLv, kDefDomain}, {kNew, 0x06050007u}}),
          "D4 New of the adopted FirstValue second, at the tx!");
    CHECK(h.dom_changes == ch0 + 1, "D4 one DOMAIN_CHANGE strobe");
    CHECK(d->dom_class_a_prio_o == 5 && d->dom_class_a_vid_o == 7,
          "D4 levels track the adoption");
    CHECK(d->dom_adopted_o == 1, "D4 ADOPTED state");

    // D5: the same Domain again is NOT a change
    h.dom_evs.clear();
    const int ch1 = h.dom_changes;
    d->dom_rx_valid_i = 1; h.tick(); d->dom_rx_valid_i = 0;
    h.run(10);
    CHECK(h.dom_evs.empty(), "D5 no re-adoption of identical parameters");
    CHECK(h.dom_changes == ch1, "D5 no DOMAIN_CHANGE strobe");
  }
}

void SrpSuite::domain_surfaces_class_a_and_reverts_on_link_down() {
  {
    // D6: the certified two-class bridge shape — FirstValue {5, 2, VID 2},
    // NoV=2: Class A is value 1, priority 2 + (6 - 5) = 3
    h.dom_evs.clear();
    const int ch2 = h.dom_changes;
    d->dom_rx_class_id_i = 5; d->dom_rx_prio_i = 2;
    d->dom_rx_vid_i = 2; d->dom_rx_nov_i = 2;
    d->dom_rx_valid_i = 1; h.tick(); d->dom_rx_valid_i = 0;
    h.run(10);
    h.dom_tx();
    CHECK(h.dom_evs.size() == 2 && h.dom_evs[0].first == kLv
          && h.dom_evs[0].second == 0x06050007u,
          "D6 two-class shape: old {5, 7} withdrawn");
    CHECK(h.dom_evs.size() == 2 && h.dom_evs[1].first == kNew
          && h.dom_evs[1].second == 0x06030002u,
          "D6 surfaced Class A priority 3 VID 2 declared");
    CHECK(h.dom_changes == ch2 + 1, "D6 DOMAIN_CHANGE fired");
    CHECK(d->dom_class_a_prio_o == 3 && d->dom_class_a_vid_o == 2,
          "D6 levels from the range rule, not FirstValue equality");

    // D7: vectors that do NOT cover Class A are ignored
    h.dom_evs.clear();
    const int ch3 = h.dom_changes;
    d->dom_rx_class_id_i = 4; d->dom_rx_prio_i = 1;
    d->dom_rx_vid_i = 9; d->dom_rx_nov_i = 2;      // covers 4, 5 only
    d->dom_rx_valid_i = 1; h.tick(); d->dom_rx_valid_i = 0;
    h.run(6);
    d->dom_rx_class_id_i = 7; d->dom_rx_nov_i = 5; // starts past 6
    d->dom_rx_valid_i = 1; h.tick(); d->dom_rx_valid_i = 0;
    h.run(6);
    d->dom_rx_class_id_i = 6; d->dom_rx_nov_i = 0; // empty vector
    d->dom_rx_valid_i = 1; h.tick(); d->dom_rx_valid_i = 0;
    h.run(6);
    CHECK(h.dom_evs.empty() && h.dom_changes == ch3,
          "D7 non-covering vectors ignored");

    // D8: adopt {6, 4, 8}; periodic and LeaveAll do NOT revert (F10.2). The
    // adopting declaration registered {6, 4, 8}: New, then New again from AN
    // to QA (note 8, the Registrar IN), periodic! then JoinIn; a received
    // LeaveAll ages it to LV and nothing re-registers it: JoinMt
    d->dom_rx_class_id_i = 6; d->dom_rx_prio_i = 4;
    d->dom_rx_vid_i = 8; d->dom_rx_nov_i = 1;
    d->dom_rx_valid_i = 1; h.tick(); d->dom_rx_valid_i = 0;
    h.run(10);
    h.dom_evs.clear();
    h.dom_tx();
    h.dom_tx();
    h.dom_cadence(false);
    h.dom_cadence(true);
    CHECK(d->dom_class_a_prio_o == 4 && d->dom_class_a_vid_o == 8,
          "D8 cadence never reverts an adoption");
    CHECK(dom_seq({{kNew, 0x06040008u}, {kNew, 0x06040008u}, {kJoinIn, 0x06040008u},
                   {kJoinMt, 0x06040008u}}),
          "D8 re-joins carry the ADOPTED declaration (%s)", dom_shown().c_str());
    CHECK(d->dom_adopted_o == 1, "D8 still ADOPTED");

    // D9: LINK_DOWN is the only revert; LINK_UP re-declares the defaults
    h.dom_evs.clear();
    const int ch4 = h.dom_changes;
    d->dom_link_up_i = 0;
    h.run(10);
    CHECK(h.dom_changes == ch4 + 1, "D9 revert fires DOMAIN_CHANGE");
    CHECK(d->dom_class_a_prio_o == 3 && d->dom_class_a_vid_o == 2,
          "D9 levels back to the defaults");
    CHECK(d->dom_adopted_o == 0, "D9 DEFAULTS state");
    const bool done = h.dom_tx();
    CHECK(h.dom_evs.empty() && done, "D9 nothing declared while the link is down, its tx! done");
    d->dom_link_up_i = 1;
    h.run(10);
    h.dom_tx();
    CHECK(h.dom_evs.size() == 1 && h.dom_evs[0].first == kNew
          && h.dom_evs[0].second == 0x06030002u,
          "D9 LINK_UP re-declares the defaults (New)");
  }
}

void SrpSuite::domain_rejoin_follows_its_registrar() {
  // =====================================================================
  // DR — the Domain re-join is sJ (802.1Q-2018 §10.7.6.3): JoinIn only while
  //     the Registrar of the declared value is IN, JoinMt in MT or LV.
  //     Registrar per Table 10-4 with Milan v1.2 §4.2.7.2.2 (IN -> MT on
  //     rLv!). Expected codes come from those clauses, not from the RTL:
  //     DR1..DR5 here, DR6..DR8 and DR9..DR11 in the two that follow.
  // =====================================================================
  {
    // the defaults declared at D9, the ladder ending: New again (AN -> AA,
    // nothing registered), JoinMt (QA)
    h.dom_tx();
    h.dom_tx();

    // DR1: no peer: the periodic re-join is JoinMt
    h.dom_evs.clear();
    h.dom_cadence(false);
    CHECK(dom_seq({{kJoinMt, kDefDomain}}),
          "DR1 no peer: the periodic re-join is JoinMt {6, 3, 2}");

    // DR2: the peer declares the same Domain: registered (no adoption), and
    // the periodic re-join is JoinIn
    h.dom_evs.clear();
    h.dom_rx(6, 3, 2, 1, false);
    h.run(5);
    h.dom_cadence(false);
    CHECK(dom_seq({{kJoinIn, kDefDomain}}),
          "DR2 peer declaring: the periodic re-join is JoinIn {6, 3, 2}");

    // DR3: the bridge's LeaveAll MRPDU: LeaveAll on the Domain lane (IN ->
    // LV, QA -> VP), then its own Domain declaration in the same MRPDU (LV ->
    // IN). The re-join waits for the tx!, so the declaration registers
    // first: JoinIn
    h.dom_evs.clear();
    d->dom_leaveall_tick_i = 1; h.tick(); d->dom_leaveall_tick_i = 0;
    h.run(5);
    h.dom_rx(6, 3, 2, 1, false);
    h.run(5);
    CHECK(h.dom_evs.empty(), "DR3a a received LeaveAll's re-join waits for tx!");
    h.dom_tx();
    CHECK(dom_seq({{kJoinIn, kDefDomain}}),
          "DR3 a peer LeaveAll re-declared before tx!: the re-join is JoinIn");

    // DR4: the own LeaveAll (sLA on the round's tx!: txLA!) from QA, where
    // tx! sends nothing and txLA! sends sJ; the sLA ages the registration to
    // LV in that clock: its sJ is JoinMt
    h.dom_tx();                                     // AA -> QA, JoinIn
    h.dom_evs.clear();
    h.dom_tx(true);
    CHECK(dom_seq({{kJoinMt, kDefDomain}}) && d->dom_app_state_o == kQA,
          "DR4 own LeaveAll: the registration is LV at its tx!, JoinMt (txLA! from QA; %s)",
          dom_shown().c_str());

    // DR5: LV holds until the peer declares again (LV -> IN)
    h.dom_evs.clear();
    h.dom_cadence(false);
    h.dom_rx(6, 3, 2, 1, false);
    h.dom_cadence(false);
    CHECK(dom_seq({{kJoinMt, kDefDomain}, {kJoinIn, kDefDomain}}),
          "DR5 LV re-joins JoinMt until the peer re-declares, then JoinIn");
  }
}

void SrpSuite::domain_rejoin_after_a_withdrawal() {
  // DR6..DR8: Lv of this value and of another, and a same-clock race
  {
    // DR6: the peer withdraws: IN -> MT at once (Milan v1.2 §4.2.7.2.2), so
    // the very next re-join (rLv!: QA -> VP, sJ at the tx!) is JoinMt
    h.dom_evs.clear();
    h.dom_rx(6, 3, 2, 1, true);
    h.dom_cadence(false);
    REQ_TAG("REQ-SRP-005", "MTXW", "DR6 peer withdrawal (Lv)");
    CHECK(dom_seq({{kJoinMt, kDefDomain}}),
          "DR6 peer withdrawal (Lv): the very next re-join is JoinMt");

    // DR7: an Lv of another Domain value leaves this registration IN
    h.dom_evs.clear();
    h.dom_rx(6, 3, 2, 1, false);
    h.dom_rx(6, 4, 9, 1, true);
    h.dom_cadence(false);
    CHECK(dom_seq({{kJoinIn, kDefDomain}}),
          "DR7 an Lv of another Domain value leaves the registration IN");

    // DR8: a registering event in the LeaveAll's own clock wins (10 §6.5)
    h.dom_rx(6, 3, 2, 1, true);                    // MT first
    h.dom_evs.clear();
    d->dom_rx_class_id_i = 6; d->dom_rx_prio_i = 3;
    d->dom_rx_vid_i = 2; d->dom_rx_nov_i = 1;
    d->dom_rx_any_i = 1; d->dom_rx_ev_i = kJoinIn;
    d->dom_leaveall_tick_i = 1; d->dom_rx_valid_i = 1; h.tick();
    d->dom_leaveall_tick_i = 0; d->dom_rx_valid_i = 0; d->dom_rx_any_i = 0;
    h.run(3);
    h.dom_tx();
    CHECK(dom_seq({{kJoinIn, kDefDomain}}),
          "DR8 a registering event in the LeaveAll's clock keeps IN: JoinIn");
  }
}

void SrpSuite::domain_rejoin_across_adoption_and_link() {
  // DR9..DR11: the registration across an adoption and the link
  {
    // DR9: an adoption takes the adopting declaration's registration along:
    // the old Lv, the adopted value's New, New again (AN to QA, note 8), then
    // the periodic re-join JoinIn
    h.dom_evs.clear();
    h.dom_rx(6, 5, 7, 1, false);
    h.run(10);
    h.dom_tx();
    h.dom_tx();
    h.dom_cadence(false);
    CHECK(dom_seq({{kLv, kDefDomain}, {kNew, 0x06050007u}, {kNew, 0x06050007u},
                   {kJoinIn, 0x06050007u}}),
          "DR9 the adopted value is registered: Lv, New, New, then JoinIn {6, 5, 7} (%s)",
          dom_shown().c_str());

    // DR10: an adoption waiting behind the queue, withdrawn before it
    // applies, is declared but not registered: New, New, then JoinMt (AA)
    h.dom_evs.clear();
    d->dom_ev_ready_i = 0;
    h.dom_cadence(false);                           // JoinIn held in the queue
    h.dom_rx(6, 6, 9, 1, false);
    h.run(3);
    h.dom_rx(6, 6, 9, 1, true);
    h.run(3);
    d->dom_ev_ready_i = 1;
    h.run(10);
    h.dom_tx();
    h.dom_tx();
    h.dom_tx();
    CHECK(dom_seq({{kJoinIn, 0x06050007u}, {kLv, 0x06050007u},
                   {kNew, 0x06060009u}, {kNew, 0x06060009u}, {kJoinMt, 0x06060009u}}),
          "DR10 an adoption withdrawn before it applies re-joins JoinMt (%s)",
          dom_shown().c_str());

    // DR11: LINK_DOWN and LINK_UP clear the registration (Flush!, Begin!):
    // New, New again (AN -> AA, note 8: MT), then JoinMt
    h.dom_rx(6, 6, 9, 1, false);                   // IN again
    h.dom_evs.clear();
    d->dom_link_up_i = 0;
    h.run(10);
    d->dom_link_up_i = 1;
    h.run(10);
    h.dom_tx();
    h.dom_tx();
    h.dom_tx();
    CHECK(dom_seq({{kNew, kDefDomain}, {kNew, kDefDomain}, {kJoinMt, kDefDomain}}),
          "DR11 LINK_DOWN and LINK_UP clear the registration: New, New, then JoinMt (%s)",
          dom_shown().c_str());
  }
}

void SrpSuite::domain_tx_is_done_once_its_message_is_in() {
  // =====================================================================
  // DT — the Domain's tx! is done (tx_done_o, which KL_srp_top's round waits
  //      for before the MSRP drain) only once the message it made is in the
  //      encoder, so that tx!'s MRPDU carries it (issue #216)
  // =====================================================================
  {
    // the defaults in QA after DR11; periodic! (AA), then a tx! with the
    // encoder face refusing
    d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;
    h.dom_evs.clear();
    d->dom_ev_ready_i = 0;
    const int done0 = h.dom_tx_dones;
    d->dom_tx_tick_i = 1; h.tick(); d->dom_tx_tick_i = 0;
    h.run(30);
    const bool held = h.dom_tx_dones == done0 && d->dom_ev_valid_o && h.dom_evs.empty();
    d->dom_ev_ready_i = 1;
    h.run(10);
    CHECK(held && h.dom_tx_dones == done0 + 1 && dom_seq({{kJoinMt, kDefDomain}}),
          "DT1 a tx! whose message the encoder has not taken is not done; done once, after it "
          "(%d strobes)", h.dom_tx_dones - done0);
    // DT2: a tx! with nothing to send (QA) is done at once
    const int done1 = h.dom_tx_dones;
    d->dom_tx_tick_i = 1; h.tick(); d->dom_tx_tick_i = 0;
    h.run(4);
    CHECK(h.dom_tx_dones == done1 + 1 && dom_seq({{kJoinMt, kDefDomain}}),
          "DT2 a tx! with nothing to send (QA) is done within four clocks");
    // DT3: a tx! while the queue is full (an earlier message and an
    // adoption's Lv held) is owed until its message fits, then served
    h.dom_evs.clear();
    d->dom_ev_ready_i = 0;
    d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;
    d->dom_tx_tick_i = 1; h.tick(); d->dom_tx_tick_i = 0;          // JoinMt held (q0)
    h.run(3);
    d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;
    d->dom_tx_tick_i = 1; h.tick(); d->dom_tx_tick_i = 0;          // JoinMt (q1)
    h.run(3);
    d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;
    d->dom_tx_tick_i = 1; h.tick(); d->dom_tx_tick_i = 0;          // owed: no room
    h.run(3);
    d->dom_ev_ready_i = 1;
    h.run(20);
    CHECK(dom_seq({{kJoinMt, kDefDomain}, {kJoinMt, kDefDomain}, {kJoinMt, kDefDomain}})
          && d->dom_app_state_o == kQA,
          "DT3 a tx! with the queue full is owed until its message fits, then served (%s)",
          dom_shown().c_str());
  }
}

void SrpSuite::domain_applicant_takes_the_received_messages() {
  // =====================================================================
  // DA — the received messages of the declared value move its Applicant
  //      (Table 10-3, with operPointToPointMAC TRUE, notes 4 and 5)
  // =====================================================================
  {
    // DA1: rJoinIn! in AA (the periodic re-join owed) goes QA: nothing sent
    h.dom_evs.clear();
    d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;   // QA -> AA
    h.dom_rx_ev(6, 3, 2, 1, kJoinIn);
    h.dom_tx();
    CHECK(h.dom_evs.empty() && d->dom_app_state_o == kQA,
          "DA1 a received JoinIn in AA leaves nothing to send: QA (state %d)",
          int(d->dom_app_state_o));
    // DA2: rMt! in QA goes AA: the tx! sends sJ, JoinIn (the Registrar IN)
    h.dom_rx_ev(6, 3, 2, 1, kMt);
    h.dom_tx();
    CHECK(dom_seq({{kJoinIn, kDefDomain}}),
          "DA2 a received Mt in QA owes the re-join: JoinIn (%s)", dom_shown().c_str());
    // DA3: rIn! in AA goes QA (note 5, point-to-point): nothing sent
    h.dom_evs.clear();
    d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;
    h.dom_rx_ev(6, 3, 2, 1, kIn);
    h.dom_tx();
    CHECK(h.dom_evs.empty() && d->dom_app_state_o == kQA,
          "DA3 a received In in AA (point-to-point) leaves nothing to send: QA");
    // DA4: rJoinMt! in QA goes AA: the tx! sends JoinIn (JoinMt registers)
    h.dom_rx_ev(6, 3, 2, 1, kJoinMt);
    h.dom_tx();
    CHECK(dom_seq({{kJoinIn, kDefDomain}}),
          "DA4 a received JoinMt in QA owes the re-join: JoinIn");
    // DA5: a message of another Domain value moves nothing
    h.dom_evs.clear();
    h.dom_rx_ev(6, 4, 9, 1, kMt);
    h.dom_tx();
    CHECK(h.dom_evs.empty() && d->dom_app_state_o == kQA,
          "DA5 a received Mt of another Domain value moves nothing");
  }
}

void SrpSuite::domain_vp_is_not_ap() {
  // =====================================================================
  // DA6, DA7 — the declared value's Applicant in VP, where Table 10-3 gives
  //      VP and AP different rows (802.1Q-2018, operPointToPointMAC TRUE).
  //      A received JoinIn leaves VP alone (note 4), so a peer LeaveAll
  //      riding its JoinIn (rLA!: QA -> VP, then rJoinIn!) owes sJ on two
  //      tx! (VP -> AA, AA -> QA), where AP would send it once (AP -> QA).
  //      txLA! from VP sends s and goes AA, so the own LeaveAll's tx! after
  //      a peer Lv (rLv!: QA -> VP) sends Mt, then one JoinMt (AA -> QA),
  //      where VP kept would send two and an sJ would replace the Mt.
  //      Issue #216.
  // =====================================================================
  bring_out_of_reset();
  d->dom_link_up_i = 1;
  h.tick();
  h.dom_evs.clear();
  for (int i = 0; i < 3; ++i) h.dom_tx();          // New, New, JoinMt: QA
  const bool qa = dom_seq({{kNew, kDefDomain}, {kNew, kDefDomain}, {kJoinMt, kDefDomain}})
               && d->dom_app_state_o == kQA;
  h.dom_evs.clear();
  d->dom_leaveall_tick_i = 1; h.tick(); d->dom_leaveall_tick_i = 0;   // rLA!: QA -> VP
  const bool vp = d->dom_app_state_o == kVP;
  h.dom_rx_ev(6, 3, 2, 1, kJoinIn);                                    // rJoinIn!: VP (note 4)
  const bool kept = d->dom_app_state_o == kVP;
  for (int i = 0; i < 3; ++i) h.dom_tx();
  CHECK(qa && vp && kept && dom_seq({{kJoinIn, kDefDomain}, {kJoinIn, kDefDomain}})
        && d->dom_app_state_o == kQA,
        "DA6 a peer LeaveAll riding its JoinIn leaves the Domain in VP (note 4): JoinIn on two "
        "tx!, then nothing (%s)", dom_shown().c_str());
  h.dom_evs.clear();
  h.dom_rx_ev(6, 3, 2, 1, kLv);                                        // rLv!: QA -> VP, MT
  const bool vp2 = d->dom_app_state_o == kVP;
  h.dom_tx(true);                                                      // txLA! from VP: s, AA
  const bool s_aa = dom_seq({{kMt, kDefDomain}}) && d->dom_app_state_o == kAA;
  h.dom_tx();
  h.dom_tx();
  CHECK(vp2 && s_aa && dom_seq({{kMt, kDefDomain}, {kJoinMt, kDefDomain}})
        && d->dom_app_state_o == kQA,
        "DA7 the own LeaveAll's tx! finds the Domain in VP: Mt (s, then AA), then one JoinMt, "
        "then nothing (%s)", dom_shown().c_str());
}

void SrpSuite::vlan_refcounts_every_vid_and_freezes_the_old_one() {
  // =====================================================================
  // V — VLAN FSM walk (corrected F10.3: per-VID refcount, frozen VID). Each
  //     VID's Applicant follows Table 10-3 (issue #216): its messages are the
  //     ones its state gives at an MVRP tx! (h.vlan_tx)
  // =====================================================================
  {
    // V1: the first user of VID 2 declares it (New!: VN), nothing before a
    // tx!; then New, New again (AN), JoinMt (AA, the Registrar MT), nothing
    h.vlan_evs.clear();
    CHECK(h.vlan_op(true, 2), "V1 join accepted");
    CHECK(h.vlan_evs.empty() && vapp(0) == kVN,
          "V1 first user: New! (VN), nothing sent before a tx! (state %d)", vapp(0));
    CHECK(d->vlan_vid_active_o == 0x1, "V1 one live entry");
    bool done = h.vlan_tx();
    CHECK(done && vlan_seq({{kNew, 2}}), "V1 first user -> New VID 2");
    done = h.vlan_tx() && done;
    done = h.vlan_tx() && done;
    done = h.vlan_tx() && done;
    CHECK(done && vlan_seq({{kNew, 2}, {kNew, 2}, {kJoinMt, 2}}) && vapp(0) == kQA,
          "V1 four tx!: New, New again (AN), JoinMt (AA), then nothing (QA), each done (%s)",
          vlan_shown().c_str());

    // V2: second user of the same VID is silent
    h.vlan_evs.clear();
    CHECK(h.vlan_op(true, 2), "V2 join accepted");
    h.vlan_tx();
    CHECK(h.vlan_evs.empty(), "V2 same VID -> refcount only, ONE join total");

    // V3: periodic! (QA -> AA), then the tx!: JoinMt, its Registrar MT
    h.vlan_evs.clear();
    d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;
    h.vlan_tx();
    CHECK(vlan_seq({{kJoinMt, 2}}),
          "V3 periodic JoinMt VID 2, its Registrar MT (802.1Q-2018 10.7.6.3; %s)",
          vlan_shown().c_str());

    // V4: Domain VID changed to 5 — a NEW user brings VID 5, the old
    // declaring users keep VID 2 (frozen): two VIDs briefly live
    h.vlan_evs.clear();
    CHECK(h.vlan_op(true, 5), "V4 join accepted");
    h.vlan_tx();
    CHECK(vlan_seq({{kNew, 5}}), "V4 New VID 5");
    CHECK(d->vlan_vid_active_o == 0x3, "V4 two VIDs live across the change");

    // V5: a received LeaveAll (rLA!) moves EVERY entry: VID 2 QA -> VP, VID
    // 5 AN -> VN; the tx! re-joins VID 2 and repeats VID 5's New
    h.vlan_evs.clear();
    d->vlan_leaveall_tick_i = 1; h.tick(); d->vlan_leaveall_tick_i = 0;
    h.vlan_tx();
    CHECK(h.vlan_evs.size() == 2, "V5 LeaveAll re-joins both VIDs got %zu",
          h.vlan_evs.size());
    CHECK(vlan_seq({{kJoinMt, 2}, {kNew, 5}}), "V5 JoinMt VID 2 and New VID 5 (%s)",
          vlan_shown().c_str());
    // the own LeaveAll's tx! (txLA!): VID 2 AA -> sJ, VID 5 AN -> sN, both QA
    h.vlan_evs.clear();
    h.vlan_tx(true);
    CHECK(vlan_seq({{kJoinMt, 2}, {kNew, 5}}) && vapp(0) == kQA && vapp(1) == kQA,
          "V5 the own LeaveAll's tx! (txLA!): JoinMt VID 2 and New VID 5, then QA (%s)",
          vlan_shown().c_str());

  }
}

void SrpSuite::vlan_withdraws_each_vid_at_its_tx() {
  {
    // V6: frozen VID 2 held until its LAST user leaves; its Lv at the tx!
    h.vlan_evs.clear();
    CHECK(h.vlan_op(false, 2), "V6 first leave accepted");
    CHECK(h.vlan_evs.empty(), "V6 a remaining user keeps the VID declared");
    CHECK(d->vlan_vid_active_o == 0x3, "V6 still two live entries");
    CHECK(h.vlan_op(false, 2), "V6 second leave accepted");
    CHECK(h.vlan_evs.empty() && vapp(0) == kLA && d->vlan_vid_active_o == 0x2,
          "V6 last user out: Lv! (LA), nothing before the tx!");
    h.vlan_tx();
    CHECK(vlan_seq({{kLv, 2}}), "V6 last user out -> Lv VID 2");
    CHECK(d->vlan_vid_active_o == 0x2, "V6 only VID 5 remains");
    h.vlan_evs.clear();
    d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;
    h.vlan_tx();
    CHECK(h.vlan_evs.size() == 1 && h.vlan_evs[0].second == 5,
          "V6 periodic now re-joins VID 5 only");

    // V7: leaving an unknown VID is an error strobe, never a wire event
    h.vlan_evs.clear();
    const int e0 = h.vlan_errs;
    CHECK(h.vlan_op(false, 9), "V7 op accepted");
    h.vlan_tx();
    CHECK(h.vlan_errs == e0 + 1 && h.vlan_evs.empty(),
          "V7 unknown VID -> err strobe, no event");

    // V8: table capacity — 4 entries (VID 2's freed by its Lv), the 5th
    // distinct VID errs
    h.vlan_evs.clear();
    CHECK(h.vlan_op(true, 6) && h.vlan_op(true, 7) && h.vlan_op(true, 8),
          "V8 three more VIDs accepted");
    h.vlan_tx();
    CHECK(h.vlan_evs.size() == 3, "V8 each first user declared");
    const int e1 = h.vlan_errs;
    CHECK(h.vlan_op(true, 10), "V8 overflow op accepted");
    CHECK(h.vlan_errs == e1 + 1, "V8 full table -> err strobe");

    // V9: retire and re-declare the same VID (VIDs 6, 7 and 8 send their
    // second New at the same tx!: only VID 5's messages are read)
    auto of5 = [&]() {
      std::vector<std::pair<int, int>> m;
      for (const auto& e : h.vlan_evs) if (e.second == 5) m.push_back(e);
      return m;
    };
    h.vlan_evs.clear();
    CHECK(h.vlan_op(false, 5), "V9 leave accepted");
    h.vlan_tx();
    CHECK(of5().size() == 1 && of5()[0].first == kLv,
          "V9 Lv VID 5 when its only user leaves");
    CHECK(h.vlan_op(true, 5), "V9 re-join accepted");
    h.vlan_tx();
    CHECK(of5().size() == 2 && of5()[1].first == kNew,
          "V9 fresh first user -> New again");

    // V10: a VID re-joined while its Lv is still owed (LA + New!: VN) sends
    // New and no Lv (Table 10-3)
    h.vlan_evs.clear();
    CHECK(h.vlan_op(false, 5) && h.vlan_op(true, 5), "V10 leave and re-join accepted");
    h.vlan_tx();
    CHECK(vlan_seq({{kNew, 5}}),
          "V10 a VID re-joined before its Lv went out sends New, no Lv (%s)",
          vlan_shown().c_str());

    // V11: a txLA! before a withdrawn VID's tx! (LA -> LO) sends nothing for
    // it; the next tx! sends the s it owes (Mt, the Registrar MT), then the
    // entry has nothing left to send
    std::vector<std::pair<int, int>> six;
    auto take6 = [&]() {
      for (const auto& e : h.vlan_evs) if (e.second == 6) six.push_back(e);
      h.vlan_evs.clear();
    };
    h.vlan_evs.clear();
    CHECK(h.vlan_op(false, 6), "V11 leave accepted");
    h.vlan_tx(true); take6();
    const bool none_at_la = six.empty();
    h.vlan_tx(); take6();
    h.vlan_tx(); take6();
    const std::vector<std::pair<int, int>> mt6{{kMt, 6}};
    CHECK(none_at_la && six == mt6,
          "V11 a withdrawn VID's txLA! sends nothing (LO), the next tx! its Mt, then nothing");
  }
}

void SrpSuite::bridge_the_fsm_declarations_onto_real_frames() {
  // =====================================================================
  // B — bridge: the FSMs' own declarations, through the encoder, onto
  //     real frames (harness plays the event router)
  // =====================================================================
  {
    // leave only VID 5 live for a deterministic re-join set; the Lvs go out
    // at a tx!
    CHECK(h.vlan_op(false, 7) && h.vlan_op(false, 8), "B0 trim table to one VID");
    h.vlan_tx();
    // stop auto-draining the FSM event faces; periodic! (QA -> AA), then
    // each participant's tx!
    d->dom_ev_ready_i = 0; d->vlan_ev_ready_i = 0;
    d->dom_periodic_tick_i = 1; d->vlan_periodic_tick_i = 1;
    h.tick();
    d->dom_periodic_tick_i = 0; d->vlan_periodic_tick_i = 0;
    d->dom_tx_tick_i = 1; d->vlan_tx_tick_i = 1;
    h.tick();
    d->dom_tx_tick_i = 0; d->vlan_tx_tick_i = 0;
    // bridge both faces into the encoder push port (domain first)
    int bridged_dom = 0;
    int bridged_vlan = 0;
    for (int i = 0; i < 300; ++i) {
      d->clk_i = 0; d->eval();
      const bool dv = d->dom_ev_valid_o;
      const bool vv = d->vlan_ev_valid_o;
      if (dv) {
        d->enc_ev_app_i = 0; d->enc_ev_attr_type_i = 4;
        d->enc_ev_event_i = d->dom_ev_event_o & 7; d->enc_ev_fourpack_i = 0;
        const uint32_t v = d->dom_ev_value_o;
        uint8_t val[kFirstValueBytes] = {0};
        val[0] = uint8_t(v >> 24); val[1] = uint8_t(v >> 16);
        val[2] = uint8_t(v >> 8);  val[3] = uint8_t(v);
        h.set_val(val);
        d->enc_ev_valid_i = 1;
      } else if (vv) {
        d->enc_ev_app_i = 1; d->enc_ev_attr_type_i = 1;
        d->enc_ev_event_i = d->vlan_ev_event_o & 7; d->enc_ev_fourpack_i = 0;
        const uint16_t v = d->vlan_ev_vid_o;
        uint8_t val[kFirstValueBytes] = {0};
        val[0] = uint8_t(v >> 8); val[1] = uint8_t(v);
        h.set_val(val);
        d->enc_ev_valid_i = 1;
      } else {
        d->enc_ev_valid_i = 0;
      }
      d->eval();
      const bool rdy = d->enc_ev_ready_o;
      d->dom_ev_ready_i = dv && rdy;
      d->vlan_ev_ready_i = !dv && vv && rdy;
      d->eval();
      if (dv && rdy) ++bridged_dom;
      if (!dv && vv && rdy) ++bridged_vlan;
      d->clk_i = 1; d->eval();
    }
    d->enc_ev_valid_i = 0; d->dom_ev_ready_i = 1; d->vlan_ev_ready_i = 1;
    CHECK(bridged_dom == 1 && bridged_vlan == 1,
          "B1 one Domain and one VLAN re-join bridged got %d %d",
          bridged_dom, bridged_vlan);
    // the Domain registered since DA4 (a received JoinMt): JoinIn
    std::vector<Ev> bm = {mk_ev(0, 4, kJoinIn, 0, {0x06, 0x03, 0x00, 0x02})};
    std::vector<Ev> bv = {mk_ev(1, 1, kJoinMt, 0, {0x00, 0x05})};
    CHECK(h.capture_pdu(0, got), "B1 Domain frame captured");
    check_pdu("B1-domain", got, model_pdu(bm, false, OWN_MAC));
    CHECK(h.capture_pdu(1, got), "B1 VLAN frame captured");
    check_pdu("B1-vlan", got, model_pdu(bv, false, OWN_MAC));
  }
}

void SrpSuite::arbitrate_prepare_and_mvrp() {
  bring_out_of_reset();
  const auto first = mk_ev(1, 1, 0, 0, {0, 2});
  const auto second = mk_ev(1, 1, 0, 0, {0, 3});
  CHECK(h.push_enc(first), "O8: first VID queued before preparation");
  d->enc_ev_app_i = 1; d->enc_ev_attr_type_i = 1;
  d->enc_ev_event_i = 0; d->enc_ev_fourpack_i = 0;
  h.set_val(second.val);
  d->enc_ev_valid_i = 1;
  d->enc_join_tick_i = 2;
  d->enc_la_prepare_i = 1;
  // Issue #214: an MVRP table already holding a value is searched for the
  // offered one first, one entry per clock, so with VID 2 pending VID 3 is
  // taken on the clock after its join clock, while no MVRP drain has started
  h.tick();
  const bool on_join_clock = h.last_enc_ready;
  d->enc_join_tick_i = 0;
  h.tick();
  CHECK(!on_join_clock && h.last_enc_ready && d->enc_dbg_cnt_mvrp_o == 2,
        "O8: MSRP preparation leaves MVRP intake open on its join clock "
        "(VID 3 taken after one search clock)");
  d->enc_ev_valid_i = 0; d->enc_join_tick_i = 0;
  const int actions = h.own_actions;
  for (int i = 0; i < 50; ++i) {
    h.tick();
    if (h.last_prepare_done) break;
  }
  CHECK(h.last_prepare_done && h.own_actions == actions + 1,
        "O8: simultaneous MVRP tick accepts exactly one MSRP action");
  d->enc_la_prepare_i = 0;
  std::vector<uint8_t> msrp;
  std::vector<uint8_t> mvrp;
  CHECK(h.capture_pdu(0, msrp), "O8: prepared MSRP round completes");
  CHECK(h.wait_and_drain(mvrp), "O8: deferred MVRP join completes without another tick");
  CHECK(mvrp == model_pdu({first, second}, false, OWN_MAC),
        "O8: deferred MVRP frame contains both accepted VIDs");
}

// W — issue #65 (Milan 4.3.2): the encoder strobes each MVRP MRPDU the
// arbiter accepts, and the VLAN participant records per entry whether a
// declaration of it has been in one since the entry was allocated. Issue
// #216: a declaration is handed over at the VID's tx!, by the walk.
void SrpSuite::track_which_vids_have_left() {
  bring_out_of_reset();
  // W1: one strobe per accepted MVRP MRPDU, none for MSRP
  const int tx0 = h.mvrp_tx;
  CHECK(h.push_enc(mk_ev(1, 1, 0, 0, {0x00, 0x07})), "W1 MVRP push accepted");
  CHECK(h.capture_pdu(1, got), "W1 MVRP PDU captured");
  CHECK(h.mvrp_tx == tx0 + 1, "W1 one MVRP transmission strobe got %d", h.mvrp_tx - tx0);
  CHECK(h.push_enc(mk_ev(0, 4, 0, 0, {0x06, 0x03, 0x00, 0x02})), "W1 MSRP push accepted");
  CHECK(h.capture_pdu(0, got), "W1 MSRP PDU captured");
  CHECK(h.mvrp_tx == tx0 + 1, "W1 no strobe for an MSRP MRPDU");
  auto sent = [&](int e) { return (d->vlan_vid_sent_o >> e) & 1; };
  auto vid = [&](int e) { return int((d->vlan_vid_val_o >> (12 * e)) & 0xFFF); };
  auto transmit = [&]() { d->vlan_mvrp_tx_i = 1; h.tick(); d->vlan_mvrp_tx_i = 0; h.run(2); };
  // W2: a new VID is live but not sent until a transmission after its New
  // was handed over at its tx!
  CHECK(h.vlan_op(true, 7) && d->vlan_vid_active_o == 0x1 && !sent(0) && vid(0) == 7,
        "W2 New! VID 7 live, not yet sent");
  h.vlan_tx();
  CHECK(!sent(0) && vid(0) == 7, "W2 New VID 7 handed over, not yet sent");
  transmit();
  CHECK(sent(0) && vid(0) == 7, "W2 sent once an MVRP MRPDU leaves");
  // W3: a New the encoder has not accepted is not in a transmission
  d->vlan_ev_ready_i = 0;
  CHECK(h.vlan_op(true, 9) && d->vlan_vid_active_o == 0x3 && !sent(1), "W3 VID 9 live");
  d->vlan_tx_tick_i = 1; h.tick(); d->vlan_tx_tick_i = 0; h.run(10);
  CHECK(d->vlan_ev_valid_o && !sent(1), "W3 New VID 9 held");
  transmit();
  CHECK(!sent(1) && sent(0), "W3 a transmission before the hand-over does not count");
  d->vlan_ev_ready_i = 1; h.run(10);
  CHECK(!sent(1), "W3 handed over, still not sent");
  transmit();
  CHECK(sent(1) && vid(1) == 9, "W3 sent at the next transmission");
  // W4: a second user keeps it; the last leave retires it; a re-join waits again
  CHECK(h.vlan_op(true, 7) && sent(0), "W4 a second user of VID 7 keeps it sent");
  CHECK(h.vlan_op(false, 7) && sent(0), "W4 one user left: still sent");
  CHECK(h.vlan_op(false, 7) && !sent(0) && d->vlan_vid_active_o == 0x2,
        "W4 last user left: retired, not sent");
  CHECK(h.vlan_op(true, 7) && !sent(0) && sent(1), "W4 re-joined VID 7 waits for its New to leave");
  h.vlan_tx();
  transmit();
  CHECK(sent(0) && vid(0) == 7, "W4 sent again after the next transmission");
  // W5: the re-joins of sent entries never un-send them
  h.vlan_evs.clear();
  d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;
  h.vlan_tx();
  CHECK(h.vlan_evs.size() == 2 && sent(0) && sent(1), "W5 re-joined entries stay sent");
  transmit();
  CHECK(d->vlan_vid_sent_o == 0x3, "W5 and after the re-joins leave");
}

void SrpSuite::vlan_registrar_drives_sj() {
  // =====================================================================
  // VR — each VID's Registrar (802.1Q-2018 Table 10-4; for MVRP as printed:
  //      rLv!, rLA! and txLA! take IN to LV, which sJ encodes as MT) and the
  //      sJ it encodes (10.7.6.3), and the received messages on its
  //      Applicant (Table 10-3, operPointToPointMAC TRUE). Issue #216.
  // =====================================================================
  bring_out_of_reset();
  auto ladder = [&](int v) { return vr_ladder(v); };
  auto rejoin = [&]() { vr_rejoin(); };
  CHECK(h.vlan_op(true, 2) && ladder(2), "VR0 VID 2 declared: New, New, JoinMt (%s)",
        vlan_shown().c_str());
  // VR1: a received JoinIn of the VID: its Registrar IN, the re-join JoinIn
  h.vlan_rx(2, kJoinIn);
  rejoin();
  CHECK(vlan_seq({{kJoinIn, 2}}) && vreg(0),
        "VR1 a received JoinIn of the VID: its Registrar IN, the periodic re-join JoinIn (%s)",
        vlan_shown().c_str());
  // VR2: a received Lv: its Registrar LV (QA -> VP): the tx! sends JoinMt
  h.vlan_rx(2, kLv);
  h.vlan_evs.clear();
  h.vlan_tx();
  CHECK(vlan_seq({{kJoinMt, 2}}) && !vreg(0),
        "VR2 a received Lv: its Registrar LV, the re-join JoinMt (%s)", vlan_shown().c_str());
  // VR3: a received value of another VID moves nothing of VID 2
  h.vlan_rx(3, kJoinIn);
  h.vlan_evs.clear();
  h.vlan_tx();                                      // AA -> QA: JoinMt
  CHECK(vlan_seq({{kJoinMt, 2}}) && !vreg(0),
        "VR3 a received JoinIn of another VID changes neither its Registrar nor its message");
  // VR4: a received In or Mt registers nothing (Table 10-4); the Mt moves the
  // Applicant (QA -> AA): the tx! sends JoinMt
  h.vlan_rx(2, kIn);
  h.vlan_rx(2, kMt);
  h.vlan_evs.clear();
  h.vlan_tx();
  CHECK(vlan_seq({{kJoinMt, 2}}) && !vreg(0),
        "VR4 a received In or Mt registers nothing; the Mt owes the re-join: JoinMt (%s)",
        vlan_shown().c_str());
  // VR5: a received JoinMt registers (Table 10-4) and owes the re-join (QA ->
  // AA): JoinIn
  h.vlan_rx(2, kJoinMt);
  h.vlan_evs.clear();
  h.vlan_tx();
  CHECK(vlan_seq({{kJoinIn, 2}}) && vreg(0),
        "VR5 a received JoinMt registers and owes the re-join: JoinIn (%s)", vlan_shown().c_str());
}

//! three MVRP tx! after a VID's New!: New, New, then JoinMt (its Registrar MT)
bool SrpSuite::vr_ladder(int v) {
  h.vlan_evs.clear();
  for (int i = 0; i < 3; ++i) h.vlan_tx();
  return vlan_seq({{kNew, v}, {kNew, v}, {kJoinMt, v}});
}

//! periodic! (QA -> AA), then the MVRP tx! that sends the re-join
void SrpSuite::vr_rejoin() {
  h.vlan_evs.clear();
  d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;
  h.vlan_tx();
}

void SrpSuite::vlan_registrar_ages_and_note_8() {
  auto ladder = [&](int v) { return vr_ladder(v); };
  auto rejoin = [&]() { vr_rejoin(); };
  // VR6: a received LeaveAll takes IN to LV (and QA to VP): JoinMt
  d->vlan_leaveall_tick_i = 1; h.tick(); d->vlan_leaveall_tick_i = 0;
  h.vlan_evs.clear();
  h.vlan_tx();
  CHECK(vlan_seq({{kJoinMt, 2}}) && !vreg(0),
        "VR6 a received LeaveAll takes the Registrar to LV: JoinMt (%s)", vlan_shown().c_str());
  // VR7: a registering value in the LeaveAll's own clock wins (10 §6.5)
  d->vlan_leaveall_tick_i = 1; d->vlan_rx_valid_i = 1; d->vlan_rx_vid_i = 2;
  d->vlan_rx_ev_i = kJoinIn;
  h.tick();
  d->vlan_leaveall_tick_i = 0; d->vlan_rx_valid_i = 0;
  h.vlan_evs.clear();
  h.vlan_tx();
  CHECK(vlan_seq({{kJoinIn, 2}}) && vreg(0),
        "VR7 a registering value in the LeaveAll's clock keeps IN: JoinIn (%s)",
        vlan_shown().c_str());
  // VR8: the own LeaveAll's tx! (txLA!, from QA: sJ) ages IN to LV in its
  // clock: JoinMt
  h.vlan_tx();                                      // AA -> QA, JoinIn
  h.vlan_evs.clear();
  h.vlan_tx(true);
  CHECK(vlan_seq({{kJoinMt, 2}}) && !vreg(0),
        "VR8 the own LeaveAll's tx! ages the Registrar to LV: JoinMt (%s)", vlan_shown().c_str());
  // VR9: rJoinIn! in AA goes QA (nothing sent); rIn! in AA goes QA (note 5)
  rejoin();                                         // JoinMt: QA -> AA -> QA
  d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;   // AA
  h.vlan_rx(2, kJoinIn);
  h.vlan_evs.clear();
  h.vlan_tx();
  const bool joinin_quiet = h.vlan_evs.empty() && vapp(0) == kQA;
  d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;   // AA
  h.vlan_rx(2, kIn);
  h.vlan_tx();
  CHECK(joinin_quiet && h.vlan_evs.empty() && vapp(0) == kQA,
        "VR9 a received JoinIn or In in AA leaves nothing to send: QA");
  // VR10: note 8 — a VID declared with its Registrar IN goes from AN to QA:
  // New, New, and no JoinMt
  CHECK(h.vlan_op(true, 4), "VR10 VID 4 declared");
  h.vlan_rx(4, kJoinIn);
  h.vlan_evs.clear();
  for (int i = 0; i < 3; ++i) h.vlan_tx();
  CHECK(vlan_seq({{kNew, 4}, {kNew, 4}}) && vapp(1) == kQA,
        "VR10 note 8: from AN with the Registrar IN the tx! sends New and goes QA, no JoinMt (%s)",
        vlan_shown().c_str());
  // VR11: a VID allocated again starts with its Registrar MT (Begin!)
  CHECK(h.vlan_op(false, 4), "VR11 VID 4 withdrawn");
  h.vlan_tx();
  CHECK(h.vlan_op(true, 4) && !vreg(1) && ladder(4),
        "VR11 a VID allocated again starts with its Registrar MT: New, New, JoinMt (%s)",
        vlan_shown().c_str());
}

void SrpSuite::vlan_vp_is_not_ap() {
  // =====================================================================
  // VR12, VR13 — VID 2's Applicant in VP, the two Table 10-3 rows where VP
  //      and AP differ, as DA6 and DA7 grade them for the Domain: a peer
  //      LeaveAll riding its JoinIn owes JoinIn on two tx! (note 4), and the
  //      own LeaveAll's tx! after a peer Lv sends Mt, then one JoinMt.
  //      Issue #216.
  // =====================================================================
  bring_out_of_reset();
  CHECK(h.vlan_op(true, 2) && vr_ladder(2) && vapp(0) == kQA,
        "VR12 VID 2 declared: New, New, JoinMt (%s)", vlan_shown().c_str());
  h.vlan_evs.clear();
  d->vlan_leaveall_tick_i = 1; h.tick(); d->vlan_leaveall_tick_i = 0;   // rLA!: QA -> VP
  const bool vp = vapp(0) == kVP;
  h.vlan_rx(2, kJoinIn);                                                // rJoinIn!: VP (note 4)
  const bool kept = vapp(0) == kVP && vreg(0);
  for (int i = 0; i < 3; ++i) h.vlan_tx();
  CHECK(vp && kept && vlan_seq({{kJoinIn, 2}, {kJoinIn, 2}}) && vapp(0) == kQA,
        "VR12 a peer LeaveAll riding its JoinIn leaves VID 2 in VP (note 4): JoinIn on two tx!, "
        "then nothing (%s)", vlan_shown().c_str());
  h.vlan_evs.clear();
  h.vlan_rx(2, kLv);                                                    // rLv!: QA -> VP, LV
  const bool vp2 = vapp(0) == kVP && !vreg(0);
  h.vlan_tx(true);                                                      // txLA! from VP: s, AA
  const bool s_aa = vlan_seq({{kMt, 2}}) && vapp(0) == kAA;
  h.vlan_tx();
  h.vlan_tx();
  CHECK(vp2 && s_aa && vlan_seq({{kMt, 2}, {kJoinMt, 2}}) && vapp(0) == kQA,
        "VR13 the own LeaveAll's tx! finds VID 2 in VP: Mt (s, then AA), then one JoinMt, then "
        "nothing (%s)", vlan_shown().c_str());
}

void SrpSuite::vlan_link_change_restarts_each_vid() {
  // =====================================================================
  // VR14 to VR17 — a link change on each VID (802.1Q-2018 Table 10-4 and
  //      Table 10-3, as this engine maps them, issue #216): LINK_DOWN is
  //      Flush! and LINK_UP is Begin!, both take the Registrar to MT; LINK_UP
  //      is Begin! on the Applicant too (VO), then New! for a VID with users
  //      (VN), so the new link partner gets New, New, then the Join. A link
  //      change wins over a received value of its clock. A VID whose Lv is
  //      owed and that has no user is VO at LINK_UP: nothing is owed.
  // =====================================================================
  bring_out_of_reset();
  CHECK(h.vlan_op(true, 2) && vr_ladder(2), "VR14 VID 2 declared: New, New, JoinMt (%s)",
        vlan_shown().c_str());
  h.vlan_rx(2, kJoinIn);
  const bool in = vreg(0) && vapp(0) == kQA;
  d->vlan_link_up_i = 0; h.tick();                                      // LINK_DOWN: Flush!
  const bool flushed = !vreg(0) && vapp(0) == kQA;
  vr_rejoin();                                                          // periodic!, tx!
  CHECK(in && flushed && vlan_seq({{kJoinMt, 2}}),
        "VR14 LINK_DOWN (Flush!) takes VID 2's Registrar from IN to MT and leaves its Applicant: "
        "the re-join is JoinMt (%s)", vlan_shown().c_str());
  h.vlan_rx(2, kJoinIn);                                                // IN again, link down
  const bool in2 = vreg(0);
  d->vlan_link_up_i = 1; h.tick();                                      // LINK_UP: Begin!, New!
  const bool begun = !vreg(0) && vapp(0) == kVN;
  CHECK(in2 && begun && vr_ladder(2),
        "VR15 LINK_UP (Begin!) takes VID 2's Registrar from IN to MT and restarts its Applicant "
        "(VN): New, New, then JoinMt (%s)", vlan_shown().c_str());
  // VR16: a received JoinIn on the clock of either link change registers nothing
  d->vlan_link_up_i = 0;
  d->vlan_rx_valid_i = 1; d->vlan_rx_vid_i = 2; d->vlan_rx_ev_i = kJoinIn;
  h.tick();
  d->vlan_rx_valid_i = 0;
  const bool down_wins = !vreg(0);
  d->vlan_link_up_i = 1;
  d->vlan_rx_valid_i = 1; d->vlan_rx_vid_i = 2; d->vlan_rx_ev_i = kJoinIn;
  h.tick();
  d->vlan_rx_valid_i = 0;
  CHECK(down_wins && !vreg(0) && vapp(0) == kVN,
        "VR16 a received JoinIn on the clock of LINK_DOWN or LINK_UP leaves the Registrar MT, "
        "and LINK_UP leaves VN");
  // VR17: the last user leaves (Lv!: LA, the Lv owed), then the link bounces
  vr_ladder(2);
  const bool left = h.vlan_op(false, 2) && vapp(0) == kLA && d->vlan_vid_active_o == 0;
  d->vlan_link_up_i = 0; h.tick();
  d->vlan_link_up_i = 1; h.tick();
  const bool vo = vapp(0) == kVO;
  h.vlan_evs.clear();
  h.vlan_tx();
  h.vlan_tx();
  CHECK(left && vo && h.vlan_evs.empty() && vapp(0) == kVO,
        "VR17 a VID whose Lv is owed and that has no user is VO at LINK_UP (Begin!): no Lv and no "
        "New go out (%s)", vlan_shown().c_str());
}

void SrpSuite::vlan_link_up_withdraws_the_held_message() {
  // =====================================================================
  // VR18 — a LINK_UP inside a tx! walk (issue #216): the message the walk
  //      holds for the encoder was decided before the link came up, so it
  //      is withdrawn and the VID takes that walk's tx! restarted: New. On
  //      the clock the walk evaluates the VID, the LINK_UP comes first: New.
  // =====================================================================
  bring_out_of_reset();
  CHECK(h.vlan_op(true, 2) && vr_ladder(2), "VR18 VID 2 declared: New, New, JoinMt (%s)",
        vlan_shown().c_str());
  d->vlan_link_up_i = 0; h.tick();
  h.vlan_rx(2, kJoinIn);                                                // IN, link down
  d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;   // QA -> AA
  h.vlan_evs.clear();
  d->vlan_ev_ready_i = 0;
  d->vlan_tx_tick_i = 1; h.tick(); d->vlan_tx_tick_i = 0;
  h.run(6);
  const bool held = d->vlan_ev_valid_o && d->vlan_ev_event_o == kJoinIn;
  d->vlan_link_up_i = 1; h.tick();                                      // LINK_UP, walk in its push
  const bool withdrawn = !h.last_vlan_ev_valid;
  d->vlan_ev_ready_i = 1;
  bool done = false;
  for (int i = 0; i < 80 && !done; ++i) { h.tick(); done = h.last_vlan_tx_done; }
  h.run(2);
  CHECK(held && withdrawn && done && vlan_seq({{kNew, 2}}) && vapp(0) == kAN,
        "VR18 a LINK_UP while the walk holds VID 2's JoinIn withdraws it: the walk sends New (%s)",
        vlan_shown().c_str());
  // the LINK_UP on the clock the walk evaluates VID 2 (IN again, AA)
  d->vlan_link_up_i = 0; h.tick();
  h.vlan_tx();
  h.vlan_rx(2, kJoinIn);
  d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;
  const bool aa_in = vapp(0) == kAA && vreg(0);
  h.vlan_evs.clear();
  d->vlan_tx_tick_i = 1; h.tick(); d->vlan_tx_tick_i = 0;
  d->vlan_link_up_i = 1; h.tick();                                      // the walk's first entry
  done = false;
  for (int i = 0; i < 80 && !done; ++i) { h.tick(); done = h.last_vlan_tx_done; }
  h.run(2);
  CHECK(aa_in && done && vlan_seq({{kNew, 2}}) && vapp(0) == kAN,
        "VR18 a LINK_UP on the clock the walk evaluates VID 2 comes first: the walk sends New, "
        "not the JoinIn of AA (%s)", vlan_shown().c_str());
}

void SrpSuite::send_the_mvrp_leaveall_alone() {
  // =====================================================================
  // L7 — issue #216: an MVRP drain whose table holds nothing still carries
  //      its participant's LeaveAll (802.1Q-2018 10.7.9, Table 10-5), as the
  //      LeaveAll-only MVRPDU: one VID message, NumberOfValues 0, a zero
  //      FirstValue (10.8.2.8 f); a tick with neither emits nothing
  // =====================================================================
  bring_out_of_reset();
  const int c0 = h.commits;
  d->enc_join_tick_i = 2; h.tick(); d->enc_join_tick_i = 0;
  h.run(200);
  CHECK(h.commits == c0 && !d->enc_txreq_valid_o,
        "L7 an MVRP tick with nothing pending and no LeaveAll emits nothing");
  d->enc_leaveall_i = 2; d->enc_join_tick_i = 2; h.tick();
  d->enc_leaveall_i = 0; d->enc_join_tick_i = 0;
  CHECK(h.wait_and_drain(got), "L7 the LeaveAll-only MVRPDU is captured");
  const std::vector<uint8_t> exp = [] {
    std::vector<uint8_t> f;
    for (int i = 0; i < 6; ++i) f.push_back(i == 0 ? 0x01 : i == 1 ? 0x80 : i == 2 ? 0xC2
                                            : i == 3 ? 0x00 : i == 4 ? 0x00 : 0x21);
    for (int i = 0; i < 6; ++i) f.push_back(OWN_MAC[i]);
    for (uint8_t b : {0x88, 0xF5, 0x00, 0x01, 0x02, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})
      f.push_back(b);
    return f;
  }();
  check_pdu("L7 the LeaveAll-only MVRPDU", got, exp);
  const int m0 = h.mvrp_tx;
  (void)m0;
  // and the MSRP drain first, then the MVRP LeaveAll it deferred
  CHECK(h.push_enc(mk_ev(0, 4, 0, 0, {0x06, 0x03, 0x00, 0x02})), "L7 MSRP push accepted");
  d->enc_join_tick_i = 3; d->enc_leaveall_i = 2; h.tick();
  d->enc_join_tick_i = 0; d->enc_leaveall_i = 0;
  std::vector<uint8_t> msrp;
  CHECK(h.wait_and_drain(msrp) && msrp.size() > 13 && msrp[12] == 0x22,
        "L7 simultaneous ticks: the MSRP drain first");
  CHECK(h.wait_and_drain(got), "L7 the deferred LeaveAll-only MVRPDU follows");
  check_pdu("L7 the deferred LeaveAll-only MVRPDU", got, exp);
}

// PV — issue #214: one entry per attribute value in a pending table, so one
// MRPDU carries each value once. 802.1Q-2022 10.7.7 runs one Applicant per
// value and Table 10-3 gives it one action per tx!; a value inside two
// VectorAttributes of one MRPDU is two events for it (10.7.5.13 c),
// 10.8.2.10.1). The expected message of each merge comes from Table 10-3,
// not from the RTL: New! and Lv! take any state to VN or LA (New, Lv); a
// join leaves VN in VN (New stays) and otherwise sends sJ with the newer
// Registrar reading (10.7.6.3); an s (In, Mt) is owed only where nothing else
// is.
void SrpSuite::expect_frame(const char* name, int app, const std::vector<Ev>& in,
                            const std::vector<Ev>& exp, bool leaveall) {
  bool all = true;
  for (const Ev& e : in) all &= h.push_enc(e);
  CHECK(all, "%s: every push accepted", name);
  CHECK(h.capture_pdu(app, got), "%s: frame captured", name);
  check_pdu(name, got, model_pdu(exp, leaveall, OWN_MAC));
}

void SrpSuite::keep_one_entry_per_value() {
  bring_out_of_reset();
  const auto dom = [](int ev, uint8_t prio, uint8_t vid) {
    return mk_ev(0, 4, ev, 0, {0x06, prio, 0x00, vid});
  };
  const auto lis = [](int ev, int fp, uint8_t lo) {
    return mk_ev(0, 3, ev, fp, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, lo});
  };
  const auto vid = [](int ev, uint8_t v) { return mk_ev(1, 1, ev, 0, {0x00, v}); };
  constexpr int NEW = 0;
  constexpr int JIN = 1;
  constexpr int IN = 2;
  constexpr int JMT = 3;
  constexpr int MT = 4;
  constexpr int LV = 5;
  constexpr int ASKF = 1;
  constexpr int READY = 2;

  // PV1: the capture's shape: periodic! hands over JoinIn, the received
  // LeaveAll's re-join at tx! JoinIn again: one Domain vector
  expect_frame("PV1 two re-joins of one Domain value: one JoinIn", 0,
               {dom(JIN, 3, 2), dom(JIN, 3, 2)}, {dom(JIN, 3, 2)});
  CHECK(d->enc_dbg_cnt_msrp_o == 0, "PV1 the table drained");
  // PV2: LINK_UP's New and a periodic! re-join: VN stays VN, New
  expect_frame("PV2 New then a re-join: New", 0,
               {dom(NEW, 3, 2), dom(JMT, 3, 2)}, {dom(NEW, 3, 2)});
  // PV3: two re-joins: the newer Registrar reading
  expect_frame("PV3 JoinMt then JoinIn: JoinIn", 0,
               {dom(JMT, 3, 2), dom(JIN, 3, 2)}, {dom(JIN, 3, 2)});
  // PV4: Lv! and New! replace what was owed
  expect_frame("PV4 a re-join then Lv: Lv", 0,
               {dom(JIN, 3, 2), dom(LV, 3, 2)}, {dom(LV, 3, 2)});
  expect_frame("PV4 Lv then New: New", 0,
               {dom(LV, 3, 2), dom(NEW, 3, 2)}, {dom(NEW, 3, 2)});
  expect_frame("PV4 New then Lv: Lv", 0,
               {dom(NEW, 3, 5), dom(LV, 3, 5)}, {dom(LV, 3, 5)});
  expect_frame("PV4 Lv then a join: JoinMt", 0,
               {dom(LV, 3, 2), dom(JMT, 3, 2)}, {dom(JMT, 3, 2)});
  // PV5: an s (In, Mt) is owed only where nothing else is
  expect_frame("PV5 In then JoinMt: JoinMt", 0,
               {dom(IN, 3, 2), dom(JMT, 3, 2)}, {dom(JMT, 3, 2)});
  expect_frame("PV5 JoinIn then Mt: JoinIn", 0,
               {dom(JIN, 3, 2), dom(MT, 3, 2)}, {dom(JIN, 3, 2)});
  expect_frame("PV5 Lv then Mt: Lv", 0,
               {dom(LV, 3, 2), dom(MT, 3, 2)}, {dom(LV, 3, 2)});
  expect_frame("PV5 Mt then In: In", 0,
               {dom(MT, 3, 2), dom(IN, 3, 2)}, {dom(IN, 3, 2)});
  // PV6: a Listener value keeps the newer declaration type (35.2.2.7.2),
  // except under an s that did not replace
  expect_frame("PV6 Listener Ready then AskingFailed: AskingFailed", 0,
               {lis(JIN, READY, 0x10), lis(JIN, ASKF, 0x10)}, {lis(JIN, ASKF, 0x10)});
  expect_frame("PV6 Listener New Ready then a re-join AskingFailed: New AskingFailed", 0,
               {lis(NEW, READY, 0x10), lis(JMT, ASKF, 0x10)}, {lis(NEW, ASKF, 0x10)});
  expect_frame("PV6 Listener New Ready then Mt Ignore: New Ready", 0,
               {lis(NEW, READY, 0x10), lis(MT, 0, 0x10)}, {lis(NEW, READY, 0x10)});
  // PV7: the merged entry keeps its place, so runs still fold: stream 0x10,
  // its successor 0x11, then 0x10 again: one vector of two values
  expect_frame("PV7 a value again after its successor: one run of two", 0,
               {lis(JMT, READY, 0x10), lis(JMT, READY, 0x11), lis(JIN, READY, 0x10)},
               {lis(JIN, READY, 0x10), lis(JMT, READY, 0x11)});
  // PV8: AttributeType is part of the value: a Talker Advertise, a Talker
  // Failed and a Listener of one StreamID are three values
  {
    Ev ta = mk_ev(0, 1, JMT, 0, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x10,
                                 0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01});
    Ev tf = ta; tf.type = 2; tf.event = NEW;
    expect_frame("PV8 three types of one StreamID: three vectors", 0,
                 {ta, lis(JIN, READY, 0x10), tf}, {ta, lis(JIN, READY, 0x10), tf});
  }
  // PV9: MVRP: the VID New and the re-join walks of one window: New once;
  // three re-join walks: JoinIn once; another VID stays apart
  expect_frame("PV9 VID 2 New then two re-joins: New", 1,
               {vid(NEW, 2), vid(JIN, 2), vid(JIN, 2)}, {vid(NEW, 2)});
  expect_frame("PV9 three re-join walks of VID 2 and VID 7: one each", 1,
               {vid(JIN, 2), vid(JIN, 7), vid(JIN, 2), vid(JIN, 7), vid(JIN, 2)},
               {vid(JIN, 2), vid(JIN, 7)});
  CHECK(d->enc_dbg_cnt_mvrp_o == 0, "PV9 the table drained");
}

// PV10: the intake may re-arbitrate while an event waits (KL_srp_top's
// fixed-priority merge): a value offered after another one had searched past
// its entry still finds that entry
void SrpSuite::restart_the_search_when_the_offer_changes() {
  bring_out_of_reset();
  const auto dom = [](int ev, uint8_t vid) {
    return mk_ev(0, 4, ev, 0, {0x06, 0x03, 0x00, vid});
  };
  bool all = h.push_enc(dom(3, 2)) && h.push_enc(dom(3, 5)) && h.push_enc(dom(3, 7));
  CHECK(all && d->enc_dbg_cnt_msrp_o == 3, "PV10 three Domain values pending");
  const Ev absent = dom(3, 9);
  const Ev again = dom(1, 2);
  d->enc_ev_app_i = 0; d->enc_ev_attr_type_i = 4; d->enc_ev_fourpack_i = 0;
  d->enc_ev_event_i = uint8_t(absent.event); h.set_val(absent.val);
  d->enc_ev_valid_i = 1;
  h.tick();
  const bool early0 = h.last_enc_ready;
  h.tick();
  const bool early1 = h.last_enc_ready;
  d->enc_ev_event_i = uint8_t(again.event); h.set_val(again.val);
  int clocks = 0;
  for (; clocks < 16; ++clocks) { h.tick(); if (h.last_enc_ready) break; }
  d->enc_ev_valid_i = 0;
  CHECK(!early0 && !early1 && h.last_enc_ready && d->enc_dbg_cnt_msrp_o == 3,
        "PV10 a value offered after another searched past its entry merges into it "
        "(taken after %d clocks, %u pending)", clocks + 1, unsigned(d->enc_dbg_cnt_msrp_o));
  CHECK(h.capture_pdu(0, got), "PV10 frame captured");
  check_pdu("PV10", got, model_pdu({dom(1, 2), dom(3, 5), dom(3, 7)}, false, OWN_MAC));
}

// PV11: the own LeaveAll's txLA! re-join reaches a table that already holds
// the periodic! re-join of the same value: its LeaveAll MRPDU carries it once
void SrpSuite::merge_inside_a_prepared_round() {
  bring_out_of_reset();
  const Ev dm = mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02});
  const Ev re = mk_ev(0, 4, 3, 0, {0x06, 0x03, 0x00, 0x02});
  const Ev ls = mk_ev(0, 3, 1, 2, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x10});
  CHECK(h.push_enc(dm), "PV11 the periodic! re-join pending");
  d->enc_la_prepare_i = 1;
  for (int i = 0; i < 50; ++i) { h.tick(); if (h.last_prepare_done) break; }
  d->enc_la_prepare_i = 0;
  CHECK(h.last_prepare_done, "PV11 the own LeaveAll round is prepared");
  CHECK(h.push_enc(re) && h.push_enc(ls), "PV11 the txLA! walk's pushes accepted");
  CHECK(d->enc_dbg_cnt_msrp_o == 2, "PV11 the re-join merged (%u pending)",
        unsigned(d->enc_dbg_cnt_msrp_o));
  CHECK(h.capture_pdu(0, got), "PV11 LeaveAll MRPDU captured");
  check_pdu("PV11 own LeaveAll MRPDU: the Domain once, JoinMt", got,
            model_pdu({re, ls}, true, OWN_MAC));
}

// forward the Domain and VLAN participants' events into the encoder for
// `cycles` clocks, Domain first (the KL_srp_top intake order); returns the
// number forwarded
int SrpSuite::bridge(int cycles) {
  int n = 0;
  for (int i = 0; i < cycles; ++i) {
    d->clk_i = 0; d->eval();
    const bool dv = d->dom_ev_valid_o;
    const bool vv = d->vlan_ev_valid_o;
    uint8_t val[kFirstValueBytes] = {0};
    if (dv) {
      d->enc_ev_app_i = 0; d->enc_ev_attr_type_i = 4;
      d->enc_ev_event_i = d->dom_ev_event_o & 7; d->enc_ev_fourpack_i = 0;
      const uint32_t v = d->dom_ev_value_o;
      val[0] = uint8_t(v >> 24); val[1] = uint8_t(v >> 16);
      val[2] = uint8_t(v >> 8);  val[3] = uint8_t(v);
      h.set_val(val);
      d->enc_ev_valid_i = 1;
    } else if (vv) {
      d->enc_ev_app_i = 1; d->enc_ev_attr_type_i = 1;
      d->enc_ev_event_i = d->vlan_ev_event_o & 7; d->enc_ev_fourpack_i = 0;
      const uint16_t v = d->vlan_ev_vid_o;
      val[0] = uint8_t(v >> 8); val[1] = uint8_t(v);
      h.set_val(val);
      d->enc_ev_valid_i = 1;
    } else {
      d->enc_ev_valid_i = 0;
    }
    d->eval();
    const bool rdy = d->enc_ev_ready_o;
    d->dom_ev_ready_i = dv && rdy;
    d->vlan_ev_ready_i = !dv && vv && rdy;
    d->eval();
    n += (dv || vv) && rdy;
    d->clk_i = 1; d->eval();
  }
  d->enc_ev_valid_i = 0;
  return n;
}

// PV12: the participants themselves hand a value over twice in one window
// (the cause), and the frame carries it once
void SrpSuite::bridge_two_cadences_into_one_vector() {
  // PV12 (issue #214, with issue #216's Applicants): each Applicant makes one
  // message per tx!, so a value is handed over twice in one transmission
  // window only when two tx! come before its drain; the encoder keeps one
  // entry per value, and the MRPDU carries the message the merge leaves
  bring_out_of_reset();
  d->dom_ev_ready_i = 0; d->vlan_ev_ready_i = 0;
  const Ev d_new = mk_ev(0, 4, 0, 0, {0x06, 0x03, 0x00, 0x02});
  const Ev d_jin = mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02});
  auto dom_tick = [&]() { d->dom_tx_tick_i = 1; h.tick(); d->dom_tx_tick_i = 0; };
  auto vlan_tick = [&](bool own) {
    d->vlan_tx_tick_i = 1; d->vlan_tx_la_i = own ? 1 : 0; h.tick();
    d->vlan_tx_tick_i = 0; d->vlan_tx_la_i = 0;
  };
  // LINK_UP (VN): two tx! before the MSRP drain, New from VN and from AN
  d->dom_link_up_i = 1;
  int n = bridge(20);
  dom_tick();
  n += bridge(40);
  dom_tick();
  n += bridge(40);
  CHECK(n == 2, "PV12 the Domain handed over New twice (%d events)", n);
  CHECK(h.capture_pdu(0, got), "PV12 frame captured");
  check_pdu("PV12 two tx! of a fresh declaration before one drain: Domain New once", got,
            model_pdu({d_new}, false, OWN_MAC));
  // the peer declares it (IN; AA -> QA); periodic! and its tx! hand a JoinIn
  // over, then a received LeaveAll (QA -> VP), the peer's re-declaration and
  // the next tx! another, before the drain: Domain JoinIn once
  h.dom_rx(6, 3, 2, 1, false);
  h.run(5);
  d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;
  dom_tick();
  n = bridge(40);
  d->dom_leaveall_tick_i = 1; h.tick(); d->dom_leaveall_tick_i = 0;
  h.run(3);
  h.dom_rx(6, 3, 2, 1, false);
  dom_tick();
  n += bridge(40);
  CHECK(n == 2, "PV12 the Domain handed over two re-joins (%d events)", n);
  CHECK(h.capture_pdu(0, got), "PV12 second frame captured");
  check_pdu("PV12 a received LeaveAll and periodic! in one window: Domain JoinIn once",
            got, model_pdu({d_jin}, false, OWN_MAC));
  // MVRP: a user's VID (VN), then two tx! and the own LeaveAll's (New, New,
  // and sJ from AA), before the MVRP drain: VID 4 New once
  CHECK(h.vlan_op(true, 4), "PV12 a user joins VID 4");
  vlan_tick(false);
  n = bridge(40);
  vlan_tick(false);
  n += bridge(40);
  vlan_tick(true);
  n += bridge(40);
  CHECK(n == 3, "PV12 the VLAN participant handed VID 4 over three times (%d events)", n);
  CHECK(h.capture_pdu(1, got), "PV12 MVRP frame captured");
  check_pdu("PV12 New, New and a LeaveAll's re-join in one window: VID 4 New once", got,
            model_pdu({mk_ev(1, 1, 0, 0, {0x00, 0x04})}, false, OWN_MAC));
  d->dom_ev_ready_i = 1; d->vlan_ev_ready_i = 1;
}

// One application drains `drained` while an event of the other application,
// whose table holds `searched`, stays offered from the join tick to the end of
// the frame: the search and the merge of each repeat run beside the whole drain.
void SrpSuite::offer_through_a_drain(const std::string& name, const std::vector<Ev>& drained,
                                     const std::vector<Ev>& searched, const Ev& offered,
                                     const std::vector<Ev>& after) {
  const char* row = name.c_str();
  bool all = true;
  for (const Ev& e : drained) all &= h.push_enc(e);
  for (const Ev& e : searched) all &= h.push_enc(e);
  CHECK(all, "%s: every push accepted", row);
  h.offer(offered);
  const bool frame = h.capture_pdu(drained[0].app, got);
  d->enc_ev_valid_i = 0;
  CHECK(frame, "%s: the drain's frame captured", row);
  check_pdu((name + ": the drain reads its own entries").c_str(), got,
            model_pdu(drained, false, OWN_MAC));
  CHECK(h.capture_pdu(offered.app, got), "%s: the searched table's frame captured", row);
  check_pdu((name + ": the search merged into its own table").c_str(), got,
            model_pdu(after, false, OWN_MAC));
}

// PV13: each table has its own read port, and a search takes only its own
// application's (10 §3): an offer of one application, with that table non-empty,
// leaves the other application's drain reading its entries in order, in both
// directions. A value one MRPDU then carried twice is the issue's symptom.
void SrpSuite::search_beside_the_other_drain() {
  bring_out_of_reset();
  const auto dom = [](int ev, uint8_t prio, uint8_t vid) {
    return mk_ev(0, 4, ev, 0, {0x06, prio, 0x00, vid});
  };
  const auto vid = [](int ev, uint8_t v) { return mk_ev(1, 1, ev, 0, {0x00, v}); };
  constexpr int JIN = 1;
  constexpr int JMT = 3;
  const std::vector<Ev> msrp = {dom(JMT, 3, 2), dom(JIN, 3, 5), dom(JMT, 3, 7), dom(JIN, 3, 9)};
  const std::vector<Ev> mvrp = {vid(JMT, 2), vid(JIN, 5), vid(JMT, 7), vid(JIN, 9)};
  offer_through_a_drain("PV13 an MVRP search beside an MSRP drain", msrp, mvrp, vid(JIN, 5),
                        mvrp);
  offer_through_a_drain("PV13 an MSRP search beside an MVRP drain", mvrp, msrp,
                        dom(JIN, 3, 5), msrp);
  // a value the table does not hold: the first repeat appends, the next ones merge
  std::vector<Ev> grown = mvrp;
  grown.push_back(vid(JIN, 11));
  offer_through_a_drain("PV13 an absent VID offered beside an MSRP drain", msrp, mvrp,
                        vid(JIN, 11), grown);
  grown = msrp;
  grown.push_back(dom(JIN, 3, 11));
  offer_through_a_drain("PV13 an absent Domain value offered beside an MVRP drain", mvrp, msrp,
                        dom(JIN, 3, 11), grown);
}

// PV14: the identity of a Talker or Listener value is its whole StreamID
// (802.1Q-2022 35.2.2.8.1, 35.2.2.8.2): two StreamIDs that differ in one bit, each
// of the 64 in turn, are two values with two Applicants (10.7.7), so a merge of
// them would lose one declaration
void SrpSuite::keep_every_streamid_bit_in_the_identity() {
  bring_out_of_reset();
  constexpr int JMT = 3;
  for (const int type : {1, 2, 3}) {
    for (int octet = 0; octet < 8; ++octet) {
      for (int bit = 0; bit < 8; ++bit) {
        const Ev first = typed_ev(type);
        Ev second = first;
        second.val[octet] ^= uint8_t(1u << bit);
        second.event = JMT;
        const std::string name = "PV14 two type " + std::to_string(type)
            + " StreamIDs that differ only in octet " + std::to_string(octet)
            + ", bit " + std::to_string(bit) + ": two values";
        expect_frame(name.c_str(), 0, {first, second}, {first, second});
      }
    }
  }
}

// PV15: an MVRP value is its whole VID (802.1Q-2022 11.2.3.1.7, 0 to 4094): two
// VIDs that differ in one bit, each of the twelve in turn, are two values
void SrpSuite::keep_every_vid_bit_in_the_identity() {
  bring_out_of_reset();
  constexpr int JIN = 1;
  constexpr int JMT = 3;
  for (int bit = 0; bit < 12; ++bit) {
    const int other = 2 ^ (1 << bit);
    const Ev first = mk_ev(1, 1, JIN, 0, {0x00, 0x02});
    const Ev second = mk_ev(1, 1, JMT, 0, {uint8_t(other >> 8), uint8_t(other)});
    const std::string name = "PV15 VID 2 and the VID that differs only in bit "
        + std::to_string(bit) + ": two values";
    expect_frame(name.c_str(), 1, {first, second}, {first, second});
  }
}

// PV16: MSRP allows no FirstValue change for an existing StreamID (802.1Q-2022
// 35.2.2.8), so two Talker values of one StreamID in one window merge, and the
// entry carries the newer one: what the talker now declares (10 §3)
void SrpSuite::carry_the_newer_firstvalue() {
  bring_out_of_reset();
  const std::pair<int, const char*> talkers[] = {{1, "Advertise"}, {2, "Failed"}};
  for (const auto& [type, label] : talkers) {
    const Ev older = typed_ev(type);
    Ev newer = older;
    for (int i = 8; i < attr_len(0, type); ++i) newer.val[i] ^= 0x5A;
    const std::string name = std::string("PV16 two Talker ") + label
        + " values of one StreamID, later fields changed: the newer one";
    expect_frame(name.c_str(), 0, {older, newer}, {newer});
  }
}

// PV17: an MVRP merge reaches the entry's event (802.1Q-2022 Table 10-3): Lv!
// takes AA and QA to LA and VN to LA, New! takes LA to VN. KL_srp_vlan hands
// an Lv over when a VID's last user leaves, which can fall in the window of a
// pending New or JoinIn, and a re-join of a retired VID hands a New over
void SrpSuite::merge_the_mvrp_withdrawals() {
  bring_out_of_reset();
  const auto vid = [](int ev) { return mk_ev(1, 1, ev, 0, {0x00, 0x02}); };
  constexpr int NEW = 0;
  constexpr int JIN = 1;
  constexpr int LV = 5;
  expect_frame("PV17 VID JoinIn then Lv: Lv", 1, {vid(JIN), vid(LV)}, {vid(LV)});
  expect_frame("PV17 VID New then Lv: Lv", 1, {vid(NEW), vid(LV)}, {vid(LV)});
  expect_frame("PV17 VID Lv then New: New", 1, {vid(LV), vid(NEW)}, {vid(NEW)});
  CHECK(d->enc_dbg_cnt_mvrp_o == 0, "PV17 the table drained");
}

// PV18: an In or Mt that replaces an In or Mt brings its own declaration type
// (10 §3): the Listener lane follows the event it was pushed with
void SrpSuite::take_the_listener_type_of_a_replacing_s() {
  bring_out_of_reset();
  const auto lis = [](int ev, int fp) {
    return mk_ev(0, 3, ev, fp, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x10});
  };
  constexpr int IN = 2;
  constexpr int MT = 4;
  constexpr int ASKF = 1;
  constexpr int READY = 2;
  expect_frame("PV18 Listener In Ready then Mt AskingFailed: Mt AskingFailed", 0,
               {lis(IN, READY), lis(MT, ASKF)}, {lis(MT, ASKF)});
  expect_frame("PV18 Listener Mt AskingFailed then In Ready: In Ready", 0,
               {lis(MT, ASKF), lis(IN, READY)}, {lis(IN, READY)});
}

// PV19: an unknown AttributeType has no value to search for, so it is taken and
// dropped on the clock it is offered, whatever the table holds (E11 with the
// table empty; the push face is never held for a value it will not keep)
void SrpSuite::drop_an_unknown_type_at_once() {
  bring_out_of_reset();
  const Ev dm = mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02});
  const Ev vd = mk_ev(1, 1, 1, 0, {0x00, 0x02});
  CHECK(h.push_enc(dm) && h.push_enc(vd), "PV19 a value pending in each table");
  const int dropped = h.drops;
  CHECK(h.push_enc(mk_ev(0, 9, 0, 0, {0x01}), 1),
        "PV19 an unknown MSRP type taken at once beside a pending value");
  CHECK(h.push_enc(mk_ev(1, 2, 0, 0, {0x01}), 1),
        "PV19 an unknown MVRP type taken at once beside a pending value");
  CHECK(h.drops == dropped + 2, "PV19 both dropped (%d strobes)", h.drops - dropped);
  CHECK(d->enc_dbg_cnt_msrp_o == 1 && d->enc_dbg_cnt_mvrp_o == 1, "PV19 nothing pended");
  CHECK(h.capture_pdu(0, got), "PV19 MSRP frame captured");
  check_pdu("PV19 the MSRP frame carries the pending value", got,
            model_pdu({dm}, false, OWN_MAC));
  CHECK(h.capture_pdu(1, got), "PV19 MVRP frame captured");
  check_pdu("PV19 the MVRP frame carries the pending value", got,
            model_pdu({vd}, false, OWN_MAC));
}

// The search of `offered` ends on the clock the join tick starts the drain of
// `pending`, in a round prepared for an own LeaveAll when `prepared`. The offer
// drops on the clock it is taken, as a producer's does, so a push the drain takes
// and loses on any clock is missing from the next frame.
void SrpSuite::offer_on_the_start_clock(const std::string& name, const Ev& pending,
                                        const Ev& offered, bool prepared) {
  const char* row = name.c_str();
  const int app = pending.app;
  if (prepared) {
    d->enc_la_prepare_i = 1;
    for (int i = 0; i < 50; ++i) { h.tick(); if (h.last_prepare_done) break; }
    d->enc_la_prepare_i = 0;
    CHECK(h.last_prepare_done, "%s: the own LeaveAll round is prepared", row);
  }
  CHECK(h.push_enc(pending), "%s: a value pending", row);
  h.drop_on_accept = true;
  h.offer(offered);
  h.tick();
  d->enc_join_tick_i = uint8_t(1u << app);
  h.tick();
  d->enc_join_tick_i = 0;
  CHECK(!h.last_enc_ready, "%s: the offer waits on the clock its drain starts", row);
  CHECK(h.wait_and_drain(got), "%s: the drain's frame captured", row);
  check_pdu((name + ": the drain carries what it counted").c_str(), got,
            model_pdu({pending}, prepared, OWN_MAC));
  for (int i = 0; i < kHandshakeTimeoutCycles && d->enc_ev_valid_i; ++i) h.tick();
  CHECK(!d->enc_ev_valid_i, "%s: the offer is taken after the drain", row);
  h.drop_on_accept = false;
  d->enc_ev_valid_i = 0;
  CHECK(h.capture_pdu(app, got), "%s: the next frame captured", row);
  check_pdu((name + ": the offer rides the next MRPDU").c_str(), got,
            model_pdu({offered}, false, OWN_MAC));
}

// PV20: the drain counts its entries on its start clock and clears the count when
// it commits, so an event taken from that clock to the commit would be outside the
// count and lost; the push waits and rides the next MRPDU (802.1Q-2022 10.8.3.1 a),
// 10 §3). A prepared own LeaveAll round starts from E_COLLECT, the other drains
// from E_IDLE, and each has its own term in the guard
void SrpSuite::hold_a_push_off_its_drain_start() {
  bring_out_of_reset();
  const Ev pending = mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02});
  const Ev offered = mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x07});
  offer_on_the_start_clock("PV20 a Domain value on the MSRP drain's start clock", pending, offered,
                           false);
  offer_on_the_start_clock("PV20 a VID on the MVRP drain's start clock",
                           mk_ev(1, 1, 1, 0, {0x00, 0x02}), mk_ev(1, 1, 1, 0, {0x00, 0x07}),
                           false);
  offer_on_the_start_clock("PV20 a Domain value on the prepared LeaveAll round's start clock",
                           pending, offered, true);
}

int SrpSuite::run() {
  bring_out_of_reset();
  encode_the_two_minimal_pdus();
  aggregate_one_window_into_one_frame();
  carry_a_wide_firstvalue_and_one_leaveall();
  keep_the_two_participants_independent();
  pad_the_packed_lanes_and_drop_unknown_types();
  fold_a_full_table_then_backpressure_the_overflow();
  flag_every_registered_type_in_one_leaveall();
  match_the_switch_leaveall_only_vectors();
  flag_each_type_once_across_repeated_messages();
  keep_the_mvrp_leaveall_on_its_one_type();
  take_one_leaveall_per_drain();
  arbitrate_prepare_and_mvrp();
  domain_declares_adopts_and_ignores_repeats();
  domain_surfaces_class_a_and_reverts_on_link_down();
  domain_rejoin_follows_its_registrar();
  domain_rejoin_after_a_withdrawal();
  domain_rejoin_across_adoption_and_link();
  domain_tx_is_done_once_its_message_is_in();
  domain_applicant_takes_the_received_messages();
  vlan_refcounts_every_vid_and_freezes_the_old_one();
  vlan_withdraws_each_vid_at_its_tx();
  bridge_the_fsm_declarations_onto_real_frames();
  track_which_vids_have_left();
  vlan_registrar_drives_sj();
  vlan_registrar_ages_and_note_8();
  domain_vp_is_not_ap();
  vlan_vp_is_not_ap();
  vlan_link_change_restarts_each_vid();
  vlan_link_up_withdraws_the_held_message();
  send_the_mvrp_leaveall_alone();
  keep_one_entry_per_value();
  restart_the_search_when_the_offer_changes();
  merge_inside_a_prepared_round();
  bridge_two_cadences_into_one_vector();
  search_beside_the_other_drain();
  keep_every_streamid_bit_in_the_identity();
  keep_every_vid_bit_in_the_identity();
  carry_the_newer_firstvalue();
  merge_the_mvrp_withdrawals();
  take_the_listener_type_of_a_replacing_s();
  drop_an_unknown_type_at_once();
  hold_a_push_off_its_drain_start();

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  SrpSuite suite;
  return suite.run();
}
