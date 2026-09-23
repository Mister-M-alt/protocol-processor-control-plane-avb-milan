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
  // pre-edge samples
  bool last_enc_ready = false;
  bool last_txreq = false;
  int  last_txslot = 0;
  bool last_ser_valid = false;
  bool last_ser_last = false;
  uint8_t last_ser_data = 0;
  bool last_user_ready = false;
  bool last_vlan_ev_valid = false;

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
  }
  void run(int n) { for (int i = 0; i < n; ++i) tick(); }

  void set_val(const uint8_t v[kFirstValueBytes]) {
    for (int w = 0; w < kValueWords; ++w) d->enc_ev_value_i[w] = 0;
    for (int j = 0; j < kFirstValueBytes; ++j) {
      const int low = 264 - 8 * j;               // byte j at [271-8j : 264-8j]
      d->enc_ev_value_i[low / 32] |= uint32_t(v[j]) << (low % 32);
    }
  }

  bool push_enc(const Ev& e, int timeout = kHandshakeTimeoutCycles) {
    d->enc_ev_app_i       = uint8_t(e.app);
    d->enc_ev_attr_type_i = uint8_t(e.type);
    d->enc_ev_event_i     = uint8_t(e.event);
    d->enc_ev_fourpack_i  = uint8_t(e.fp);
    set_val(e.val);
    d->enc_ev_valid_i = 1;
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
  void domain_declares_adopts_and_ignores_repeats();
  void domain_surfaces_class_a_and_reverts_on_link_down();
  void vlan_refcounts_every_vid_and_freezes_the_old_one();
  void bridge_the_fsm_declarations_onto_real_frames();

  int checks = 0;
  int fails = 0;

  const milan::tb::Model<Vsrp_tb_wrap> model;
  Vsrp_tb_wrap* const d = model.get();
  H h{d};
  std::vector<uint8_t> got;
};

void SrpSuite::bring_out_of_reset() {
  // defaults
  d->rst_n = 0;
  d->enc_ev_valid_i = 0; d->enc_join_tick_i = 0; d->enc_leaveall_i = 0;
  d->enc_txreq_ready_i = 0;
  d->ser_req_i = 0; d->ser_ready_i = 0; d->ser_slot_i = 0;
  d->dom_link_up_i = 0; d->dom_rx_valid_i = 0;
  d->dom_periodic_tick_i = 0; d->dom_leaveall_tick_i = 0;
  d->dom_ev_ready_i = 1;
  d->vlan_user_valid_i = 0; d->vlan_periodic_tick_i = 0;
  d->vlan_leaveall_tick_i = 0; d->vlan_ev_ready_i = 1;
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

void SrpSuite::domain_declares_adopts_and_ignores_repeats() {
  // =====================================================================
  // D — Domain FSM walk (F10.2)
  // =====================================================================
  {
    // D1: startup with the link already up = declare the defaults
    h.dom_evs.clear();
    d->dom_link_up_i = 1;
    h.run(10);
    CHECK(h.dom_evs.size() == 1, "D1 one declaration got %zu", h.dom_evs.size());
    CHECK(h.dom_evs.size() == 1 && h.dom_evs[0].first == 0
          && h.dom_evs[0].second == 0x06030002u,
          "D1 New {6, 3, 2} got ev %d val %08x",
          h.dom_evs.empty() ? -1 : h.dom_evs[0].first,
          h.dom_evs.empty() ? 0u : h.dom_evs[0].second);
    CHECK(d->dom_class_a_prio_o == 3 && d->dom_class_a_vid_o == 2,
          "D1 class-D defaults {3, 2}");
    CHECK(d->dom_adopted_o == 0, "D1 DEFAULTS state");

    // D2: periodic re-join
    h.dom_evs.clear();
    d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;
    h.run(10);
    CHECK(h.dom_evs.size() == 1 && h.dom_evs[0].first == 1
          && h.dom_evs[0].second == 0x06030002u, "D2 periodic JoinIn");

    // D3: LeaveAll re-declare
    h.dom_evs.clear();
    d->dom_leaveall_tick_i = 1; h.tick(); d->dom_leaveall_tick_i = 0;
    h.run(10);
    CHECK(h.dom_evs.size() == 1 && h.dom_evs[0].first == 1,
          "D3 LeaveAll re-join");

    // D4: adopt a differing Class A Domain {6, 5, VID 7}
    h.dom_evs.clear();
    const int ch0 = h.dom_changes;
    d->dom_rx_class_id_i = 6; d->dom_rx_prio_i = 5;
    d->dom_rx_vid_i = 7; d->dom_rx_nov_i = 1;
    d->dom_rx_valid_i = 1; h.tick(); d->dom_rx_valid_i = 0;
    h.run(10);
    CHECK(h.dom_evs.size() == 2, "D4 withdraw + re-declare got %zu",
          h.dom_evs.size());
    CHECK(h.dom_evs.size() == 2 && h.dom_evs[0].first == 5
          && h.dom_evs[0].second == 0x06030002u,
          "D4 Lv of the old declaration first");
    CHECK(h.dom_evs.size() == 2 && h.dom_evs[1].first == 0
          && h.dom_evs[1].second == 0x06050007u,
          "D4 New of the adopted FirstValue second");
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
    CHECK(h.dom_evs.size() == 2 && h.dom_evs[0].first == 5
          && h.dom_evs[0].second == 0x06050007u,
          "D6 two-class shape: old {5, 7} withdrawn");
    CHECK(h.dom_evs.size() == 2 && h.dom_evs[1].first == 0
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

    // D8: adopt {6, 4, 8}; periodic and LeaveAll do NOT revert (F10.2)
    h.dom_evs.clear();
    d->dom_rx_class_id_i = 6; d->dom_rx_prio_i = 4;
    d->dom_rx_vid_i = 8; d->dom_rx_nov_i = 1;
    d->dom_rx_valid_i = 1; h.tick(); d->dom_rx_valid_i = 0;
    h.run(10);
    h.dom_evs.clear();
    d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;
    h.run(10);
    d->dom_leaveall_tick_i = 1; h.tick(); d->dom_leaveall_tick_i = 0;
    h.run(10);
    CHECK(d->dom_class_a_prio_o == 4 && d->dom_class_a_vid_o == 8,
          "D8 cadence never reverts an adoption");
    CHECK(h.dom_evs.size() == 2 && h.dom_evs[0].first == 1
          && h.dom_evs[0].second == 0x06040008u
          && h.dom_evs[1].first == 1 && h.dom_evs[1].second == 0x06040008u,
          "D8 re-joins carry the ADOPTED declaration");
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
    d->dom_periodic_tick_i = 1; h.tick(); d->dom_periodic_tick_i = 0;
    h.run(10);
    CHECK(h.dom_evs.empty(), "D9 nothing declared while the link is down");
    d->dom_link_up_i = 1;
    h.run(10);
    CHECK(h.dom_evs.size() == 1 && h.dom_evs[0].first == 0
          && h.dom_evs[0].second == 0x06030002u,
          "D9 LINK_UP re-declares the defaults (New)");
  }
}

void SrpSuite::vlan_refcounts_every_vid_and_freezes_the_old_one() {
  // =====================================================================
  // V — VLAN FSM walk (corrected F10.3: per-VID refcount, frozen VID)
  // =====================================================================
  {
    // V1: first user of VID 2 declares it
    h.vlan_evs.clear();
    CHECK(h.vlan_op(true, 2), "V1 join accepted");
    CHECK(h.vlan_evs.size() == 1 && h.vlan_evs[0].first == 0
          && h.vlan_evs[0].second == 2, "V1 first user -> New VID 2");
    CHECK(d->vlan_vid_active_o == 0x1, "V1 one live entry");

    // V2: second user of the same VID is silent
    h.vlan_evs.clear();
    CHECK(h.vlan_op(true, 2), "V2 join accepted");
    CHECK(h.vlan_evs.empty(), "V2 same VID -> refcount only, ONE join total");

    // V3: periodic re-join of every held VID (one)
    h.vlan_evs.clear();
    d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;
    h.run(40);
    CHECK(h.vlan_evs.size() == 1 && h.vlan_evs[0].first == 1
          && h.vlan_evs[0].second == 2, "V3 periodic JoinIn VID 2");

    // V4: Domain VID changed to 5 — a NEW user brings VID 5, the old
    // declaring users keep VID 2 (frozen): two VIDs briefly live
    h.vlan_evs.clear();
    CHECK(h.vlan_op(true, 5), "V4 join accepted");
    CHECK(h.vlan_evs.size() == 1 && h.vlan_evs[0].first == 0
          && h.vlan_evs[0].second == 5, "V4 New VID 5");
    CHECK(d->vlan_vid_active_o == 0x3, "V4 two VIDs live across the change");

    // V5: LeaveAll re-joins EVERY VID with users
    h.vlan_evs.clear();
    d->vlan_leaveall_tick_i = 1; h.tick(); d->vlan_leaveall_tick_i = 0;
    h.run(60);
    CHECK(h.vlan_evs.size() == 2, "V5 LeaveAll re-joins both VIDs got %zu",
          h.vlan_evs.size());
    CHECK(h.vlan_evs.size() == 2
          && h.vlan_evs[0] == std::make_pair(1, 2)
          && h.vlan_evs[1] == std::make_pair(1, 5),
          "V5 JoinIn VID 2 and JoinIn VID 5");

    // V6: frozen VID 2 held until its LAST user leaves
    h.vlan_evs.clear();
    CHECK(h.vlan_op(false, 2), "V6 first leave accepted");
    CHECK(h.vlan_evs.empty(), "V6 a remaining user keeps the VID declared");
    CHECK(d->vlan_vid_active_o == 0x3, "V6 still two live entries");
    CHECK(h.vlan_op(false, 2), "V6 second leave accepted");
    CHECK(h.vlan_evs.size() == 1 && h.vlan_evs[0].first == 5
          && h.vlan_evs[0].second == 2, "V6 last user out -> Lv VID 2");
    CHECK(d->vlan_vid_active_o == 0x2, "V6 only VID 5 remains");
    h.vlan_evs.clear();
    d->vlan_periodic_tick_i = 1; h.tick(); d->vlan_periodic_tick_i = 0;
    h.run(40);
    CHECK(h.vlan_evs.size() == 1 && h.vlan_evs[0].second == 5,
          "V6 periodic now re-joins VID 5 only");

    // V7: leaving an unknown VID is an error strobe, never a wire event
    h.vlan_evs.clear();
    const int e0 = h.vlan_errs;
    CHECK(h.vlan_op(false, 9), "V7 op accepted");
    CHECK(h.vlan_errs == e0 + 1 && h.vlan_evs.empty(),
          "V7 unknown VID -> err strobe, no event");

    // V8: table capacity — 4 entries, the 5th distinct VID errs
    h.vlan_evs.clear();
    CHECK(h.vlan_op(true, 6) && h.vlan_op(true, 7) && h.vlan_op(true, 8),
          "V8 three more VIDs accepted");
    CHECK(h.vlan_evs.size() == 3, "V8 each first user declared");
    const int e1 = h.vlan_errs;
    CHECK(h.vlan_op(true, 10), "V8 overflow op accepted");
    CHECK(h.vlan_errs == e1 + 1, "V8 full table -> err strobe");

    // V9: retire and re-declare the same VID
    h.vlan_evs.clear();
    CHECK(h.vlan_op(false, 5), "V9 leave accepted");
    CHECK(h.vlan_evs.size() == 1 && h.vlan_evs[0].first == 5,
          "V9 Lv VID 5 when its only user leaves");
    CHECK(h.vlan_op(true, 5), "V9 re-join accepted");
    CHECK(h.vlan_evs.size() == 2 && h.vlan_evs[1].first == 0,
          "V9 fresh first user -> New again");
  }
}

void SrpSuite::bridge_the_fsm_declarations_onto_real_frames() {
  // =====================================================================
  // B — bridge: the FSMs' own declarations, through the encoder, onto
  //     real frames (harness plays the event router)
  // =====================================================================
  {
    // leave only VID 5 live for a deterministic re-join set
    CHECK(h.vlan_op(false, 6) && h.vlan_op(false, 7) && h.vlan_op(false, 8),
          "B0 trim table to one VID");
    // stop auto-draining the FSM event faces
    d->dom_ev_ready_i = 0; d->vlan_ev_ready_i = 0;
    d->dom_periodic_tick_i = 1; d->vlan_periodic_tick_i = 1;
    h.tick();
    d->dom_periodic_tick_i = 0; d->vlan_periodic_tick_i = 0;
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
    std::vector<Ev> bm = {mk_ev(0, 4, 1, 0, {0x06, 0x03, 0x00, 0x02})};
    std::vector<Ev> bv = {mk_ev(1, 1, 1, 0, {0x00, 0x05})};
    CHECK(h.capture_pdu(0, got), "B1 Domain frame captured");
    check_pdu("B1-domain", got, model_pdu(bm, false, OWN_MAC));
    CHECK(h.capture_pdu(1, got), "B1 VLAN frame captured");
    check_pdu("B1-vlan", got, model_pdu(bv, false, OWN_MAC));
  }
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
  domain_declares_adopts_and_ignores_repeats();
  domain_surfaces_class_a_and_reverts_on_link_down();
  vlan_refcounts_every_vid_and_freezes_the_old_one();
  bridge_the_fsm_declarations_onto_real_frames();

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  SrpSuite suite;
  return suite.run();
}
