// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_adp_engine suite, the second build: two AVB interfaces (N_IF_P = 2,
// P-N-AVB-INTERFACES, issue #69), section IF.
//
// Milan v1.2 5.6.3 runs one advertise state machine per AVB interface, so each
// interface leaves DOWN on its own link, draws and delays on its own, answers
// the ENTITY_DISCOVER and GM_CHANGE of its own interface, and advertises with
// its own interface_index (IEEE 1722.1-2021 6.2.2.18), available_index
// (6.2.2.15, 04 section 5) and gPTP grandmaster and domain (Milan 5.6.2.6,
// 5.6.2.7). The discovery machine's grandmaster and domain guard reads the
// ingress interface's pair (Milan 5.6.4.5.1).
//
// The services are modeled as in sim_main.cpp's harness, cut to what this
// section reads: the timer service (an armed slot fires once `now` passes its
// deadline, one expiry per clock), the TX pool (a grant the cycle after each
// request), the RX pool's sync read and the dispatch pop. Every expected byte
// is built here from the F04.5 offsets, never read back from the DUT.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "Vtb_adp_top.h"
#include "verilated.h"
#include "../common/verilator_harness.hpp"
#include "../common/req_tag.hpp"

#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

namespace {

// ---- fixed configuration (sim_main.cpp's entity) ------------------------
constexpr uint64_t EID   = 0x1122334455667788ull;
constexpr uint64_t EMID  = 0x99AABBCCDDEEFF01ull;
constexpr uint64_t MAC   = 0x001B921122AAull;
constexpr uint16_t TKSRC = 0x0008;
constexpr uint16_t TKCAP = 0x4801;
constexpr uint16_t LSNK  = 0x0008;
constexpr uint16_t LSCAP = 0x4801;
constexpr uint16_t CFGIX = 0x0002;
constexpr uint16_t IDIX  = 0x0005;
//! each interface's gPTP pair; different, so a frame or a guard that reads
//! the wrong interface's pair shows on the wire
constexpr uint64_t GM_A  = 0xA1A2A3A4A5A6A7A8ull;
constexpr uint64_t GM_A2 = 0xA9A9A9A9A9A9A9A9ull;     // interface 0 after its GM_CHANGE
constexpr uint64_t GM_B  = 0xB1B2B3B4B5B6B7B8ull;
constexpr uint8_t  DOM_A = 0x00;
constexpr uint8_t  DOM_B = 0x07;
constexpr uint64_t T1    = 0xAAAA00000000AAA1ull;     // a remote talker

constexpr unsigned N_IF = 2;
constexpr unsigned SLOT_NOADP0 = N_IF;                 // the engine's default: ADV base 0 + N_IF_P
constexpr int kAdpduBytes = 82;
constexpr unsigned kSlotBytes = 576;
constexpr unsigned SLOT_NULL = 7;
constexpr unsigned ST_DOWN = 0;
constexpr unsigned ST_DRAW = 1;
constexpr unsigned ST_WAIT = 3;
constexpr uint8_t MSG_AVAIL = 0;
constexpr uint8_t MSG_DISCOVER = 2;
//! ENTITY_AVAILABLE's valid_time field 10: 20 s of T-ADP-NOADP
constexpr uint8_t VT_20S = 10;

// ---- the 393-bit pp_txn_t (03 section 4 struct order), the fields read --
constexpr int REC_WORDS = 13;
constexpr int F_TX_SLOT    = 34;
constexpr int F_RX_SLOT    = 57;
constexpr int F_OPCODE     = 124;
constexpr int F_TARGET_EID = 158;
constexpr int F_STATUS     = 345;
constexpr int F_MSG_TYPE   = 350;
constexpr int F_PROTOCOL   = 354;
constexpr int F_IF_INDEX   = 389;

struct Rec {
  std::array<uint32_t, REC_WORDS> w{};
  void set(int lsb, int width, uint64_t v) {
    for (int i = 0; i < width; ++i) {
      const int b = lsb + i;
      if ((v >> i) & 1ull) w[b >> 5] |= (1u << (b & 31));
      else                 w[b >> 5] &= ~(1u << (b & 31));
    }
  }
};

//! an ADP transaction as dispatch presents it, on interface `ifx`
Rec adp_txn(uint8_t msg, uint64_t eid, uint8_t vt, unsigned rx_slot, unsigned ifx) {
  Rec r;
  r.set(F_IF_INDEX, 2, ifx);
  r.set(F_PROTOCOL, 3, 0);                     // ADP
  r.set(F_MSG_TYPE, 4, msg);
  r.set(F_STATUS, 5, vt);
  r.set(F_TARGET_EID, 64, eid);
  r.set(F_OPCODE, 16, msg);
  r.set(F_RX_SLOT, 3, rx_slot);
  r.set(F_TX_SLOT, 3, SLOT_NULL);
  return r;
}

void putbe(uint8_t* p, uint64_t v, int n) {
  for (int i = 0; i < n; ++i) p[i] = uint8_t(v >> (8 * (n - 1 - i)));
}
uint32_t getbe32(const std::vector<uint8_t>& f, int at) {
  return (uint32_t(f[at]) << 24) | (uint32_t(f[at + 1]) << 16)
       | (uint32_t(f[at + 2]) << 8) | uint32_t(f[at + 3]);
}

//! the F04.5 wire frame of interface `ifx`
std::vector<uint8_t> model_frame(bool departing, uint64_t gm, uint8_t dom, uint32_t aidx,
                                 unsigned ifx) {
  std::vector<uint8_t> f(kAdpduBytes, 0);
  putbe(&f[0], 0x91E0F0010000ull, 6);
  putbe(&f[6], MAC, 6);
  putbe(&f[12], 0x22F0, 2);
  f[14] = 0xFA;
  f[15] = departing ? 0x01 : 0x00;
  f[16] = uint8_t((departing ? 0 : 10) << 3);
  f[17] = 56;
  putbe(&f[18], EID, 8);
  putbe(&f[26], EMID, 8);
  putbe(&f[34], 0x0000C588ull, 4);
  putbe(&f[38], TKSRC, 2);
  putbe(&f[40], TKCAP, 2);
  putbe(&f[42], LSNK, 2);
  putbe(&f[44], LSCAP, 2);
  putbe(&f[50], aidx, 4);
  putbe(&f[54], gm, 8);
  f[62] = dom;
  putbe(&f[64], CFGIX, 2);
  putbe(&f[66], IDIX, 2);
  putbe(&f[68], ifx, 2);                       // interface_index
  return f;
}

struct Frame {
  std::vector<uint8_t> b;
  unsigned ifx;                                // the TX request's interface
  uint32_t ms;
};

struct IfHarness {
  const milan::tb::Model<Vtb_adp_top> model;
  Vtb_adp_top* const d = model.get();
  uint32_t now = 10000;
  int checks = 0;
  int fails = 0;
  // timer service
  std::array<bool, 128> armed{};
  std::array<uint32_t, 128> deadline{};
  bool exp_pulse = false;
  unsigned exp_slot = 0;
  // TX pool: 0 free, 1 allocated, 2 committed
  std::array<int, 5> slot_st{};
  std::array<std::vector<uint8_t>, 5> slot_buf;
  bool gnt_sched = false;
  unsigned gnt_slot = 0;
  std::vector<Frame> frames;                   // one per TX request, in order
  // RX pool
  std::array<std::array<uint8_t, kSlotBytes>, 4> rx_pdu{};
  uint8_t rd_next = 0;
  // class-C events: {departed, sink}
  std::vector<std::pair<bool, unsigned>> evts;
  std::array<uint64_t, N_IF> gm{GM_A, GM_B};

  IfHarness() {
    for (auto& s : slot_buf) s.assign(kSlotBytes, 0);
  }
  unsigned state(unsigned ifx) const { return (d->dbg_adv_state_o >> (2 * ifx)) & 3u; }
  uint32_t aidx(unsigned ifx) const { return uint32_t(d->dbg_avail_index_o >> (32 * ifx)); }
  void publish_gm() {
    for (unsigned i = 0; i < N_IF; ++i) {
      d->gm_id_i[2 * i] = uint32_t(gm[i]);
      d->gm_id_i[2 * i + 1] = uint32_t(gm[i] >> 32);
    }
    d->gptp_domain_i = uint16_t((DOM_B << 8) | DOM_A);
  }
  void observe();
  void tick() {
    d->now_ms_i = now;
    d->rxs_rd_data_i = rd_next;
    d->tmr_exp_valid_i = exp_pulse;
    d->tmr_exp_slot_i = exp_slot;
    d->tmr_exp_owner_i = exp_slot;
    exp_pulse = false;
    d->txs_alloc_gnt_i = gnt_sched;
    d->txs_alloc_slot_i = gnt_slot;
    gnt_sched = false;
    d->clk_i = 0;
    d->eval();
    observe();
    d->clk_i = 1;
    d->eval();
  }
  void idle(int n) {
    for (int i = 0; i < n; ++i) tick();
  }
  //! `ms` of modeled time, four clocks per ms, the timer service live
  void run_ms(unsigned ms) {
    for (unsigned t = 0; t < ms; ++t) {
      ++now;
      for (unsigned s = 0; s < armed.size(); ++s) {
        if (armed[s] && int32_t(now - deadline[s]) >= 0) {
          armed[s] = false;
          exp_pulse = true;
          exp_slot = s;
          break;
        }
      }
      idle(4);
    }
  }
  //! modeled time until interface `ifx` sends a frame, up to `ms`
  bool run_until_frame(unsigned ifx, unsigned ms) {
    const size_t from = frames.size();
    for (unsigned t = 0; t < ms; ++t) {
      run_ms(1);
      for (size_t i = from; i < frames.size(); ++i)
        if (frames[i].ifx == ifx) return true;
    }
    return false;
  }
  bool send_txn(const Rec& r) {
    for (int i = 0; i < REC_WORDS; ++i) d->txn_i[i] = r.w[i];
    d->txn_valid_i = 1;
    int i = 0;
    for (; i < 200 && !d->txn_ready_o; ++i) tick();
    tick();
    d->txn_valid_i = 0;
    for (i = 0; i < 400; ++i) {
      tick();
      if (d->txn_ready_o) return true;
    }
    return false;
  }
  //! a remote ADPDU in RX slot `slot` (PDU offsets, subtype at byte 0)
  void load_remote(unsigned slot, uint64_t eid, uint32_t aidx_v, uint64_t gm_v, uint8_t dom) {
    std::array<uint8_t, kSlotBytes>& p = rx_pdu[slot];
    p.fill(0);
    p[0] = 0xFA;
    p[1] = MSG_AVAIL;
    p[2] = uint8_t(VT_20S << 3);
    p[3] = 56;
    putbe(&p[4], eid, 8);
    putbe(&p[36], aidx_v, 4);
    putbe(&p[40], gm_v, 8);
    p[48] = dom;
  }
  std::vector<Frame> frames_of(unsigned ifx, size_t from) const {
    std::vector<Frame> v;
    for (size_t i = from; i < frames.size(); ++i)
      if (frames[i].ifx == ifx) v.push_back(frames[i]);
    return v;
  }
  void configure_and_reset();
  void start_on_interface_zero();
  void start_on_interface_one();
  void indices_are_per_interface();
  void discover_reaches_its_interface();
  void gm_change_reaches_its_interface();
  void link_down_on_one_interface();
  void guard_reads_the_ingress_pair();
  void shutdown_departs_per_interface();
  int run();
};

void IfHarness::observe() {
  if (d->tmr_arm_valid_o) {
    const unsigned s = d->tmr_arm_slot_o;
    armed[s] = !d->tmr_arm_cancel_o;
    deadline[s] = d->tmr_arm_deadline_ms_o;
  }
  if (d->txs_alloc_req_o) {
    for (unsigned s = 0; s < 4; ++s) {
      if (slot_st[s] == 0) {
        slot_st[s] = 1;
        gnt_sched = true;
        gnt_slot = s;
        break;
      }
    }
  }
  if (d->txs_wr_valid_o && d->txs_wr_slot_o < 5 && d->txs_wr_addr_o < kSlotBytes)
    slot_buf[d->txs_wr_slot_o][d->txs_wr_addr_o] = d->txs_wr_data_o;
  if (d->txs_wr_commit_o && d->txs_wr_slot_o < 5) slot_st[d->txs_wr_slot_o] = 2;
  if (d->txreq_valid_o && d->txreq_slot_o < 5) {
    const unsigned s = d->txreq_slot_o;
    frames.push_back({std::vector<uint8_t>(slot_buf[s].begin(),
                                           slot_buf[s].begin() + kAdpduBytes),
                      unsigned(d->txreq_if_o), now});
    slot_st[s] = 0;
  }
  if (d->rxs_rd_en_o) rd_next = rx_pdu[d->rxs_rd_slot_o & 3][d->rxs_rd_addr_o % kSlotBytes];
  if (d->evt_valid_o) evts.emplace_back(d->evt_departed_o != 0, unsigned(d->evt_sink_o));
}

void IfHarness::configure_and_reset() {
  d->entity_id_i = EID;
  d->entity_model_id_i = EMID;
  d->own_mac_i = MAC;
  d->talker_sources_i = TKSRC;
  d->talker_caps_i = TKCAP;
  d->listener_sinks_i = LSNK;
  d->listener_caps_i = LSCAP;
  d->current_cfg_i = CFGIX;
  d->identify_index_i = IDIX;
  publish_gm();
  d->entity_enable_i = 0;
  d->link_up_i = 0;
  d->gm_change_i = 0;
  d->txn_valid_i = 0;
  d->bound_i = 0;
  for (int i = 0; i < 16; ++i) d->bound_talker_eid_i[i] = 0;
  d->rst_n = 0;
  idle(5);
  d->rst_n = 1;
  idle(3);
}

// IF1, IF2: enabled with interface 0's link alone up, interface 0 advertises
// and interface 1 stays DOWN and silent.
void IfHarness::start_on_interface_zero() {
  d->link_up_i = 1;                            // interface 0; seeds the PRNG
  d->entity_enable_i = 1;
  const bool sent = run_until_frame(0, 2100);
  CHECK(sent && state(1) == ST_DOWN && frames_of(1, 0).empty() && state(0) == ST_WAIT,
        "IF1: enabled with interface 0's link alone up, interface 0 advertises within "
        "T-ADP-DELAY-START and interface 1 stays DOWN and silent (states %u/%u, %zu frame(s) "
        "on interface 1)", state(0), state(1), frames_of(1, 0).size());
  const std::vector<Frame> f0 = frames_of(0, 0);
  CHECK(f0.size() == 1 && f0[0].b == model_frame(false, GM_A, DOM_A, 0, 0),
        "IF2: interface 0's ENTITY_AVAILABLE is byte-exact: interface_index 0, interface 0's "
        "grandmaster and domain, available_index 0, and its TX request names interface 0");
}

// IF3: interface 1's link comes up; it advertises with its own interface_index,
// grandmaster, domain and available_index 0, on a TX request naming interface 1.
void IfHarness::start_on_interface_one() {
  const size_t from = frames.size();
  d->link_up_i = 3;
  const bool sent = run_until_frame(1, 4100);
  const std::vector<Frame> f1 = frames_of(1, from);
  REQ_TAG("REQ-SCP-003", "DIR", "IF3: interface 1's link comes up");
  CHECK(sent && f1.size() == 1 && f1[0].b == model_frame(false, GM_B, DOM_B, 0, 1),
        "IF3: interface 1's link comes up and interface 1 advertises within T-ADP-DELAY, "
        "byte-exact: interface_index 1, interface 1's grandmaster and domain, available_index "
        "0, and its TX request names interface 1 (%zu frame(s))", f1.size());
  if (!f1.empty() && f1[0].b != model_frame(false, GM_B, DOM_B, 0, 1)) {
    printf("  [i] IF3: interface_index %02x%02x, gm %016llx, domain %02x, available_index %u\n",
           f1[0].b[68], f1[0].b[69],
           static_cast<unsigned long long>((uint64_t(getbe32(f1[0].b, 54)) << 32)
                                           | getbe32(f1[0].b, 58)),
           f1[0].b[62], getbe32(f1[0].b, 50));
  }
}

// IF4: over 20 s (each interface advertises every 5 to 9 s: T-ADP-ADV, then
// T-ADP-DELAY) each interface's frames count their own available_index,
// 0, 1, 2 ..., and the engine's per-interface index agrees.
void IfHarness::indices_are_per_interface() {
  run_ms(20000);
  bool ok = true;
  std::array<size_t, N_IF> n{};
  for (unsigned i = 0; i < N_IF; ++i) {
    const std::vector<Frame> f = frames_of(i, 0);
    n[i] = f.size();
    for (size_t k = 0; k < f.size(); ++k) ok = ok && getbe32(f[k].b, 50) == uint32_t(k);
    ok = ok && aidx(i) == uint32_t(f.size());
  }
  CHECK(ok && n[0] >= 3 && n[1] >= 3,
        "IF4: each interface's frames carry its own available_index, 0 then +1 per frame of "
        "that interface (%zu and %zu frames; next indices %u and %u)", n[0], n[1], aidx(0),
        aidx(1));
}

// IF5: with both machines WAITING, ENTITY_DISCOVER (entity_id 0) arriving on
// interface 1 restarts interface 1's machine alone (Milan Table 5.51,
// RCV_ADP_DISCOVER in WAITING): interface 1 leaves WAITING for a delay and
// advertises inside T-ADP-DELAY, and interface 0 stays WAITING. Then the same
// on interface 0.
void IfHarness::discover_reaches_its_interface() {
  for (unsigned ifx = N_IF; ifx-- > 0;) {
    const unsigned other = 1 - ifx;
    for (unsigned t = 0; t < 10000 && !(state(0) == ST_WAIT && state(1) == ST_WAIT); ++t)
      run_ms(1);
    const uint32_t t0 = now;
    const bool consumed = send_txn(adp_txn(MSG_DISCOVER, 0, 0, 2, ifx));
    const unsigned s_this = state(ifx);
    const unsigned s_other = state(other);
    const bool early = run_until_frame(ifx, 4100);
    const uint32_t dt = now - t0;
    CHECK(consumed && s_this != ST_WAIT && s_other == ST_WAIT && early && dt <= 4100,
          "IF5: ENTITY_DISCOVER on interface %u, both machines WAITING, restarts interface "
          "%u's advertise machine alone: it leaves WAITING (state %u) and advertises %u ms "
          "later, inside T-ADP-DELAY; interface %u stays WAITING (state %u)",
          ifx, ifx, s_this, dt, other, s_other);
  }
}

int IfHarness::run() {
  configure_and_reset();
  CHECK(state(0) == ST_DOWN && state(1) == ST_DOWN && aidx(0) == 0 && aidx(1) == 0,
        "IF0: both advertise machines reset to DOWN with available_index 0");
  start_on_interface_zero();
  start_on_interface_one();
  indices_are_per_interface();
  discover_reaches_its_interface();
  gm_change_reaches_its_interface();
  link_down_on_one_interface();
  guard_reads_the_ingress_pair();
  shutdown_departs_per_interface();
  return fails ? 1 : 0;
}

// IF6: interface 0 publishes a new grandmaster and strobes its GM_CHANGE alone:
// interface 0 re-advertises with the new grandmaster, interface 1 stays
// WAITING and keeps its own.
void IfHarness::gm_change_reaches_its_interface() {
  (void)run_until_frame(1, 6000);
  gm[0] = GM_A2;
  publish_gm();
  d->gm_change_i = 1;                          // interface 0 alone
  tick();
  d->gm_change_i = 0;
  const unsigned s0 = state(0);
  const unsigned s1 = state(1);
  const size_t from = frames.size();
  const bool sent = run_until_frame(0, 4100);
  const std::vector<Frame> f0 = frames_of(0, from);
  const std::vector<Frame> f1 = frames_of(1, from);
  const bool gm0 = !f0.empty() && f0[0].b == model_frame(false, GM_A2, DOM_A, getbe32(f0[0].b, 50), 0);
  const bool gm1 = f1.empty() || f1[0].b == model_frame(false, GM_B, DOM_B, getbe32(f1[0].b, 50), 1);
  CHECK(s0 == ST_DRAW && s1 == ST_WAIT && sent && gm0 && gm1,
        "IF6: GM_CHANGE on interface 0 alone re-advertises interface 0 with its new "
        "grandmaster; interface 1 stays WAITING and keeps its own (states %u/%u)", s0, s1);
}

// IF7: interface 1's link goes down: interface 1 returns to DOWN without an
// ENTITY_DEPARTING (Milan 5.6.3.5.6); interface 0 keeps advertising.
void IfHarness::link_down_on_one_interface() {
  const size_t from = frames.size();
  d->link_up_i = 1;
  run_ms(10000);                               // longer than one 5 to 9 s cadence
  const std::vector<Frame> f1 = frames_of(1, from);
  const std::vector<Frame> f0 = frames_of(0, from);
  CHECK(state(1) == ST_DOWN && f1.empty() && state(0) != ST_DOWN && !f0.empty(),
        "IF7: interface 1's LINK_DOWN takes interface 1 to DOWN with no frame at all, while "
        "interface 0 keeps advertising (states %u/%u; %zu and %zu frames)", state(0), state(1),
        f0.size(), f1.size());
}

// IF8: sink 0 bound to T1, whose grandmaster and domain are interface 1's.
// T1's ENTITY_AVAILABLE received on interface 0, whose pair differs, does not
// discover it; the same ADPDU received on interface 1 does (Milan 5.6.4.5.1).
void IfHarness::guard_reads_the_ingress_pair() {
  d->bound_talker_eid_i[0] = uint32_t(T1);
  d->bound_talker_eid_i[1] = uint32_t(T1 >> 32);
  d->bound_i = 1;
  idle(4);
  const size_t e0 = evts.size();
  load_remote(1, T1, 5, GM_B, DOM_B);
  const bool c0 = send_txn(adp_txn(MSG_AVAIL, T1, VT_20S, 1, 0));
  const size_t after0 = evts.size() - e0;
  const bool c1 = send_txn(adp_txn(MSG_AVAIL, T1, VT_20S, 1, 1));
  idle(4);
  const bool discovered = evts.size() == e0 + 1 && !evts.back().first && evts.back().second == 0;
  CHECK(c0 && c1 && after0 == 0 && discovered && armed[SLOT_NOADP0],
        "IF8: T1's ENTITY_AVAILABLE carrying interface 1's grandmaster and domain is not "
        "matched when received on interface 0, and discovers sink 0 when received on "
        "interface 1 (%zu event(s) after interface 0, %zu in all)", after0, evts.size() - e0);
}

// IF9: both links up, then the entity is disabled: each interface sends its own
// ENTITY_DEPARTING, carrying its own interface_index and pre-reset
// available_index, and both indices return to 0 (04 section 5).
void IfHarness::shutdown_departs_per_interface() {
  d->link_up_i = 3;
  (void)run_until_frame(1, 4100);
  run_ms(10);
  const uint32_t a0 = aidx(0);
  const uint32_t a1 = aidx(1);
  const size_t from = frames.size();
  d->entity_enable_i = 0;
  run_ms(100);                                 // two 82-byte builds, one after the other
  const std::vector<Frame> f0 = frames_of(0, from);
  const std::vector<Frame> f1 = frames_of(1, from);
  const bool ok0 = f0.size() == 1 && f0[0].b == model_frame(true, GM_A2, DOM_A, a0, 0);
  const bool ok1 = f1.size() == 1 && f1[0].b == model_frame(true, GM_B, DOM_B, a1, 1);
  CHECK(ok0 && ok1 && aidx(0) == 0 && aidx(1) == 0,
        "IF9: disabling the entity sends one ENTITY_DEPARTING per interface, each byte-exact "
        "with its own interface_index and pre-reset available_index (%u and %u), and both "
        "indices return to 0 (%zu and %zu frames)", a0, a1, f0.size(), f1.size());
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  IfHarness h;
  const int rc = h.run();
  //! NOT the canonical tally shape: this binary is the suite's second build,
  //! and the Makefile sums both builds into the one canonical line
  printf("[build interfaces] %d checks, %d failures\n", h.checks, h.fails);
  FILE* acc = fopen("obj_dir/build_tally.txt", "a");
  if (acc == nullptr) {
    printf("FAIL: this build's tally cannot be recorded for the Makefile\n");
    return 1;
  }
  fprintf(acc, "%d %d\n", h.checks, h.fails);
  fclose(acc);
  return rc;
}
