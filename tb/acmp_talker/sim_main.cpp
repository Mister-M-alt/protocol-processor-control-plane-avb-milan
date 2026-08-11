// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_acmp_talker suite — independent expectations, never DUT logic.
//
// Every expected response below is spelled out field-by-field from the
// 05 §6bis tables (F05.11) and the DA-gate law (F05.12), not derived from
// DUT state: the C++ model is the document. Covered: both response tables
// INCLUDING their deliberate difference (PROBE echoes FAST_CONNECT +
// STREAMING_WAIT and FORCES REGISTERING_FAILED = 0 — the flag law the
// pipewire reference inverted — while GET_TX_STATE zeroes the listener
// fields and reads REGISTERING_FAILED LIVE from the srp face); the DA
// lifecycle walk none -> maap-granted -> declared; MAAP conflict ->
// withdraw -> T-SRP-LEAVEALL2 (2 x PRNG draw, compressed by TB-driven
// expiry) -> re-alloc -> re-declare with the NEW DA; the PCP-change
// backoff that KEEPS the DA; T-SRP-DAFRESH freshness expiry; the V3
// truncated-PDU zero-fill; per-source independence; and the stateless
// property (identical query twice around interleaved traffic = identical
// response bytes).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "VKL_acmp_talker.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// ---- independent 393-bit pp_txn_t codec (03 §4 declared widths) --------
static const int REC_WORDS = 13;

struct Rec {
  uint32_t w[REC_WORDS];
  Rec() { memset(w, 0, sizeof w); }
  void set(int lsb, int width, uint64_t v) {
    for (int i = 0; i < width; ++i) {
      int b = lsb + i;
      if ((v >> i) & 1ull) w[b >> 5] |= (1u << (b & 31));
    }
  }
};

enum {
  F_DISP = 0, F_DEADLINE = 2, F_TX_SLOT = 34, F_HZ_KEY = 37,
  F_HZ_CLASS = 53, F_RX_SLOT = 57, F_UNIQUE_ID = 60, F_CONFIG_IX = 76,
  F_DESC_IX = 92, F_DESC_TYPE = 108, F_OPCODE = 124, F_CR = 140,
  F_U = 141, F_SEQ = 142, F_TARGET_EID = 158, F_CTLR_EID = 222,
  F_SRC_MAC = 286, F_CDL = 334, F_STATUS = 345, F_MSG_TYPE = 350,
  F_PROTOCOL = 354, F_ARRIVAL = 357, F_IF_INDEX = 389, F_ORIGIN = 391
};

enum { O_RX = 0, P_ACMP = 1, D_ACMP_MC = 1, HZ_STREAM_CFG = 2 };

// ---- 05 §3 / F05.11 constants (the document, not the DUT) --------------
enum { MT_PROBE = 0, MT_DISC = 2, MT_GTXS = 4, MT_GTXC = 12 };
enum { ST_OK = 0, ST_TK_UNKNOWN = 2, ST_DMAC_FAIL = 3, ST_NSUPP = 31 };
static const uint16_t FL_FC = 0x0002, FL_SW = 0x0008, FL_RF = 0x0040;
enum { LSN_NONE = 0, LSN_READY = 1, LSN_ASKING_FAILED = 3 };

static const int N_SRC = 8;
static const int TMR_BASE = 17;        // IF + 2*SI (08 §5 F08.4 order)
static const int OWNER_BASE = 0x50;
static const uint32_t T_DAFRESH = 15000;

static const uint64_t OWN_EID = 0x0102030405060708ull;
static uint64_t sid_of(int s)  { return 0xAB00000000000000ull | unsigned(s); }
static uint64_t da_pool(int n) { return 0x91E0F0000100ull + unsigned(n); }
static const int VID = 2;

// ---- recorders ----------------------------------------------------------
struct Resp {
  uint8_t mt, status, ifx;
  uint64_t sid, ceid, teid, leid, da;
  uint16_t tuid, luid, cc, seq, flags, vlan;
  bool operator==(const Resp& o) const { return memcmp(this, &o, sizeof o) == 0; }
};
struct GateEv { bool open; int src; uint64_t sid, da; int vlan; };
struct Arm    { int slot, owner; uint32_t dl; bool cancel; };
struct MReq   { int src; bool rel; };

struct Hn {
  VKL_acmp_talker* d;
  // rx-slot model (sync read, committed length)
  uint8_t slot_mem[4][576];
  uint16_t slot_len[4];
  bool pend_rd = false; int pend_slot = 0, pend_addr = 0;
  // maap auto-responder
  int  mrsp_cnt = 0; bool mrsp_ok = true, mrsp_rel = false; uint64_t mrsp_da = 0;
  bool auto_grant = true; int da_seq = 0;
  // prng responder
  int  draw_cnt = 0; uint16_t draw_val = 10000; bool prng_busy = false;
  std::vector<int> draws;
  // captures
  std::vector<Resp>   resps;
  std::vector<GateEv> gates;
  std::vector<Arm>    arms;
  std::vector<MReq>   mreqs;
  int frees = 0, consumed = 0;

  explicit Hn(VKL_acmp_talker* dd) : d(dd) {
    memset(slot_mem, 0, sizeof slot_mem);
    memset(slot_len, 0, sizeof slot_len);
  }

  void tick() {
    // scheduled single-cycle inputs
    d->maap_rsp_valid_i = 0;
    d->prng_draw_valid_i = 0;
    if (mrsp_cnt > 0 && --mrsp_cnt == 0) {
      d->maap_rsp_valid_i = 1;
      d->maap_rsp_ok_i = mrsp_ok;
      d->maap_rsp_da_i = mrsp_da;
    }
    if (draw_cnt > 0 && --draw_cnt == 0) {
      d->prng_draw_valid_i = 1;
      d->prng_draw_ms_i = draw_val;
      prng_busy = false;
    }
    d->prng_draw_busy_i = prng_busy;
    // sync-read data from LAST cycle's request
    d->rxs_rd_data_i = pend_rd ? slot_mem[pend_slot][pend_addr] : 0;

    d->clk_i = 0; d->eval();
    d->rxs_slot_len_i = slot_len[d->rxs_rd_slot_o & 3];
    d->eval();

    // observe pre-edge (what the registers see)
    if (d->resp_valid_o) {
      Resp r{};
      r.mt = d->resp_msg_type_o;  r.status = d->resp_status_o;
      r.ifx = d->resp_if_index_o;
      r.sid = d->resp_stream_id_o; r.ceid = d->resp_controller_eid_o;
      r.teid = d->resp_talker_eid_o; r.leid = d->resp_listener_eid_o;
      r.da = d->resp_dest_mac_o;
      r.tuid = d->resp_talker_uid_o; r.luid = d->resp_listener_uid_o;
      r.cc = d->resp_conn_count_o; r.seq = d->resp_seq_id_o;
      r.flags = d->resp_flags_o; r.vlan = d->resp_vlan_id_o;
      resps.push_back(r);
    }
    if (d->gate_open_o || d->gate_close_o)
      gates.push_back({bool(d->gate_open_o), int(d->gate_src_o),
                       uint64_t(d->gate_stream_id_o),
                       uint64_t(d->gate_da_o), int(d->gate_vlan_o)});
    if (d->tmr_arm_valid_o)
      arms.push_back({int(d->tmr_arm_slot_o), int(d->tmr_arm_owner_o),
                      uint32_t(d->tmr_arm_deadline_ms_o),
                      bool(d->tmr_arm_cancel_o)});
    if (d->maap_req_valid_o && d->maap_req_ready_i) {
      MReq m{int(d->maap_req_src_o), bool(d->maap_req_release_o)};
      mreqs.push_back(m);
      if (auto_grant) {
        mrsp_cnt = 3; mrsp_ok = true; mrsp_rel = m.rel;
        mrsp_da = m.rel ? 0 : da_pool(da_seq++);
      }
    }
    if (d->prng_draw_req_o) {
      draws.push_back(int(d->prng_draw_kind_o));
      prng_busy = true; draw_cnt = 2;
    }
    if (d->rxs_free_o) ++frees;
    if (d->txn_ready_o) ++consumed;
    pend_rd = d->rxs_rd_en_o; pend_slot = d->rxs_rd_slot_o & 3;
    pend_addr = d->rxs_rd_addr_o;

    d->clk_i = 1; d->eval();
    d->tmr_exp_valid_i = 0;           // expiry pulses last one edge
    d->srp_pcp_change_i = 0;
    d->maap_conflict_valid_i = 0;
  }

  void run(int n) { for (int i = 0; i < n; ++i) tick(); }

  // one talker command through the dispatch-in face + RX slot model
  bool send(int mt, uint16_t uid, uint64_t ctlr, uint16_t seq,
            uint64_t leid, uint16_t luid, uint16_t flags,
            int ifx = 0, uint16_t pdu_len = 56, uint64_t target = OWN_EID) {
    const int s = 1;
    memset(slot_mem[s], 0, 576);
    for (int i = 0; i < 8; ++i)
      slot_mem[s][28 + i] = uint8_t(leid >> (8 * (7 - i)));
    slot_mem[s][38] = uint8_t(luid >> 8); slot_mem[s][39] = uint8_t(luid);
    slot_mem[s][50] = uint8_t(flags >> 8); slot_mem[s][51] = uint8_t(flags);
    slot_len[s] = pdu_len;

    Rec r;
    r.set(F_ORIGIN, 2, O_RX);          r.set(F_IF_INDEX, 2, unsigned(ifx));
    r.set(F_ARRIVAL, 32, d->now_ms_i); r.set(F_PROTOCOL, 3, P_ACMP);
    r.set(F_MSG_TYPE, 4, unsigned(mt)); r.set(F_CDL, 11, 44);
    r.set(F_SRC_MAC, 48, 0x00E04C000001ull);
    r.set(F_CTLR_EID, 64, ctlr);       r.set(F_TARGET_EID, 64, target);
    r.set(F_SEQ, 16, seq);             r.set(F_OPCODE, 16, unsigned(mt));
    r.set(F_UNIQUE_ID, 16, uid);       r.set(F_RX_SLOT, 3, unsigned(s));
    r.set(F_HZ_CLASS, 4, HZ_STREAM_CFG); r.set(F_HZ_KEY, 16, uid);
    r.set(F_TX_SLOT, 3, 7);
    r.set(F_DEADLINE, 32, uint32_t(d->now_ms_i + 50));
    r.set(F_DISP, 2, D_ACMP_MC);
    for (int i = 0; i < REC_WORDS; ++i) d->txn_i[i] = r.w[i];

    int before = consumed;
    d->txn_valid_i = 1;
    for (int i = 0; i < 400 && consumed == before; ++i) tick();
    d->txn_valid_i = 0;
    tick();
    return consumed != before;
  }

  // ---- pop-checkers ----
  Resp pop_resp(const char* tag) {
    Resp r{};
    CHECK(!resps.empty(), "%s: response missing", tag);
    if (!resps.empty()) { r = resps.front(); resps.erase(resps.begin()); }
    return r;
  }
  void expect_resp(const char* t, const Resp& e) {
    Resp g = pop_resp(t);
    CHECK(g.mt == e.mt,       "%s msg_type got %u want %u", t, g.mt, e.mt);
    CHECK(g.status == e.status, "%s status got %u want %u", t, g.status, e.status);
    CHECK(g.sid == e.sid,     "%s stream_id got %016llx", t, (unsigned long long)g.sid);
    CHECK(g.ceid == e.ceid,   "%s controller_eid got %016llx", t, (unsigned long long)g.ceid);
    CHECK(g.teid == e.teid,   "%s talker_eid got %016llx", t, (unsigned long long)g.teid);
    CHECK(g.leid == e.leid,   "%s listener_eid got %016llx", t, (unsigned long long)g.leid);
    CHECK(g.tuid == e.tuid,   "%s talker_uid got %u", t, g.tuid);
    CHECK(g.luid == e.luid,   "%s listener_uid got %u", t, g.luid);
    CHECK(g.da == e.da,       "%s dest_mac got %012llx", t, (unsigned long long)g.da);
    CHECK(g.cc == e.cc,       "%s connection_count got %u", t, g.cc);
    CHECK(g.seq == e.seq,     "%s sequence_id got %u", t, g.seq);
    CHECK(g.flags == e.flags, "%s flags got %04x want %04x", t, g.flags, e.flags);
    CHECK(g.vlan == e.vlan,   "%s vlan got %u", t, g.vlan);
    CHECK(g.ifx == e.ifx,     "%s if_index got %u", t, g.ifx);
  }
  void expect_open(const char* t, int src, uint64_t da) {
    CHECK(!gates.empty(), "%s: gate strobe missing", t);
    if (gates.empty()) return;
    GateEv g = gates.front(); gates.erase(gates.begin());
    CHECK(g.open,             "%s expected open, got close", t);
    CHECK(g.src == src,       "%s open src got %d", t, g.src);
    CHECK(g.sid == sid_of(src), "%s open sid got %016llx", t, (unsigned long long)g.sid);
    CHECK(g.da == da,         "%s open da got %012llx want %012llx", t,
          (unsigned long long)g.da, (unsigned long long)da);
    CHECK(g.vlan == VID,      "%s open vlan got %d", t, g.vlan);
  }
  void expect_close(const char* t, int src) {
    CHECK(!gates.empty(), "%s: gate strobe missing", t);
    if (gates.empty()) return;
    GateEv g = gates.front(); gates.erase(gates.begin());
    CHECK(!g.open,      "%s expected close, got open", t);
    CHECK(g.src == src, "%s close src got %d", t, g.src);
  }
  void expect_arm(const char* t, int src, uint32_t dl, bool cancel) {
    CHECK(!arms.empty(), "%s: timer arm missing", t);
    if (arms.empty()) return;
    Arm a = arms.front(); arms.erase(arms.begin());
    CHECK(a.slot == TMR_BASE + src,    "%s arm slot got %d", t, a.slot);
    CHECK(a.owner == OWNER_BASE + src, "%s arm owner got %02x", t, a.owner);
    CHECK(a.cancel == cancel,          "%s arm cancel got %d", t, a.cancel);
    if (!cancel)
      CHECK(a.dl == dl, "%s arm deadline got %u want %u", t, a.dl, dl);
    else ++checks;   // deadline is dont-care on cancel: keep tallies even
  }
  void expect_mreq(const char* t, int src, bool rel) {
    CHECK(!mreqs.empty(), "%s: maap request missing", t);
    if (mreqs.empty()) return;
    MReq m = mreqs.front(); mreqs.erase(mreqs.begin());
    CHECK(m.src == src, "%s maap src got %d", t, m.src);
    CHECK(m.rel == rel, "%s maap rel got %d", t, m.rel);
  }
  void drained(const char* t) {
    CHECK(resps.empty() && gates.empty() && arms.empty() && mreqs.empty(),
          "%s: leftovers resp=%zu gate=%zu arm=%zu mreq=%zu", t,
          resps.size(), gates.size(), arms.size(), mreqs.size());
  }
  void fire_expiry(int src) {
    d->tmr_exp_valid_i = 1;
    d->tmr_exp_slot_i  = TMR_BASE + src;
    d->tmr_exp_owner_i = OWNER_BASE + src;
    tick();
  }
  void set_lsn(int src, int st) {
    uint16_t v = d->srp_lsn_reg_state_i;
    v = uint16_t((v & ~(3u << (2 * src))) | (unsigned(st) << (2 * src)));
    d->srp_lsn_reg_state_i = v;
    run(12);
  }
};

// common echo skeleton for an expected response
static Resp echo(int mt, int st, uint16_t uid, uint64_t ctlr, uint16_t seq,
                 uint64_t leid, uint16_t luid) {
  Resp e{};
  e.mt = uint8_t(mt | 1); e.status = uint8_t(st); e.ifx = 0;
  e.ceid = ctlr; e.teid = OWN_EID; e.leid = leid;
  e.tuid = uid; e.luid = luid; e.cc = 0; e.seq = seq;
  e.sid = 0; e.da = 0; e.flags = 0; e.vlan = 0;
  return e;
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* d = new VKL_acmp_talker;
  Hn h(d);

  // static configuration before reset release
  d->rst_n = 0; d->txn_valid_i = 0;
  d->own_entity_id_i = OWN_EID;
  d->cfg_src_en_i = 0xFF;
  d->cfg_src_iface_i = 0;                 // all sources on interface 0
  for (int s = 0; s < N_SRC; ++s) {
    d->cfg_stream_id_i[2 * s]     = uint32_t(sid_of(s));
    d->cfg_stream_id_i[2 * s + 1] = uint32_t(sid_of(s) >> 32);
  }
  d->srp_lsn_reg_state_i = 0;
  d->srp_class_vid_i = VID;
  d->srp_pcp_change_i = 0;
  d->maap_req_ready_i = 1;
  d->maap_rsp_valid_i = 0; d->maap_conflict_valid_i = 0;
  d->maap_conflict_src_i = 0;
  d->tmr_exp_valid_i = 0; d->tmr_exp_slot_i = 0; d->tmr_exp_owner_i = 0;
  d->prng_draw_busy_i = 0; d->prng_draw_valid_i = 0; d->prng_draw_ms_i = 0;
  d->now_ms_i = 100;
  for (int i = 0; i < 5; ++i) h.tick();
  d->rst_n = 1;

  // ---- A: boot walk — every enabled source allocates its DA -----------
  h.run(400);
  uint64_t da[N_SRC];
  for (int s = 0; s < N_SRC; ++s) { da[s] = da_pool(s); }
  CHECK(h.mreqs.size() == N_SRC, "A allocs got %zu", h.mreqs.size());
  for (int s = 0; s < N_SRC; ++s) h.expect_mreq("A", s, false);
  CHECK(h.gates.empty(), "A no declarations without probe/listener");
  CHECK(h.resps.empty(), "A no responses");
  CHECK(h.arms.empty(), "A no timer arms");
  h.drained("A");

  // ---- B: PROBE_TX success + the two trap tables ----------------------
  uint32_t t1 = 1000; d->now_ms_i = t1;
  const uint64_t L1 = 0x1111222233334444ull, C1 = 0xC0FFEE00000000C1ull;
  CHECK(h.send(MT_PROBE, 3, C1, 0x100, L1, 7, 0x804A), "B1 consumed");
  h.run(8);
  h.expect_arm("B1 dafresh", 3, t1 + T_DAFRESH, false);
  h.expect_open("B1 declare", 3, da[3]);
  {
    Resp e = echo(MT_PROBE, ST_OK, 3, C1, 0x100, L1, 7);
    e.sid = sid_of(3); e.da = da[3]; e.vlan = VID;
    e.flags = FL_FC | FL_SW;                 // echoed; 0x8040 must NOT echo
    h.expect_resp("B1", e);
  }
  {
    CHECK(h.send(MT_GTXS, 3, C1, 0x101, L1, 7, 0xFFFF), "B2 consumed");
    Resp e = echo(MT_GTXS, ST_OK, 3, C1, 0x101, 0, 0);  // listener fields 0
    e.sid = sid_of(3); e.da = da[3]; e.vlan = VID; e.flags = 0;
    h.expect_resp("B2", e);
  }
  // the deliberate difference: RF live in GET_TX_STATE, forced 0 in PROBE
  h.set_lsn(3, LSN_ASKING_FAILED);
  {
    CHECK(h.send(MT_GTXS, 3, C1, 0x102, 0, 0, 0), "B3 consumed");
    Resp e = echo(MT_GTXS, ST_OK, 3, C1, 0x102, 0, 0);
    e.sid = sid_of(3); e.da = da[3]; e.vlan = VID;
    e.flags = FL_RF;                          // live ASKING_FAILED
    h.expect_resp("B3", e);
  }
  uint32_t t2 = 2000; d->now_ms_i = t2;
  CHECK(h.send(MT_PROBE, 3, C1, 0x103, L1, 7, FL_FC | FL_SW), "B4 consumed");
  h.expect_arm("B4 re-ping", 3, t2 + T_DAFRESH, false);
  {
    Resp e = echo(MT_PROBE, ST_OK, 3, C1, 0x103, L1, 7);
    e.sid = sid_of(3); e.da = da[3]; e.vlan = VID;
    e.flags = FL_FC | FL_SW;                  // RF forced 0 despite ASKING_FAILED
    h.expect_resp("B4", e);
  }
  // stateless: identical query twice = identical bytes
  CHECK(h.send(MT_GTXS, 3, C1, 0x104, 0, 0, 0), "B5a consumed");
  Resp b5a = h.pop_resp("B5a");
  CHECK(h.send(MT_GTXS, 3, C1, 0x104, 0, 0, 0), "B5b consumed");
  Resp b5b = h.pop_resp("B5b");
  CHECK(b5a == b5b, "B5 identical query = identical answer");
  CHECK(b5a.flags == FL_RF && b5a.da == da[3], "B5 content sane");
  h.drained("B");

  // ---- C: error / no-op / unsupported paths ---------------------------
  {
    CHECK(h.send(MT_PROBE, 8, C1, 0x110, L1, 2, 0x0003), "C1 consumed");
    Resp e = echo(MT_PROBE, ST_TK_UNKNOWN, 8, C1, 0x110, L1, 2);
    e.flags = FL_FC;                          // 0x0001 is not echoed
    h.expect_resp("C1 unknown-id", e);
  }
  {
    CHECK(h.send(MT_GTXS, 15, C1, 0x111, L1, 2, 0xFFFF), "C2 consumed");
    Resp e = echo(MT_GTXS, ST_TK_UNKNOWN, 15, C1, 0x111, 0, 0);
    h.expect_resp("C2 unknown-id", e);
  }
  { // ingress interface mismatch: silently ignored, still retired + freed
    int f0 = h.frees;
    CHECK(h.send(MT_PROBE, 2, C1, 0x112, L1, 2, FL_FC, 1), "C3 consumed");
    h.run(4);
    CHECK(h.resps.empty(), "C3 no response on wrong interface");
    CHECK(h.arms.empty(), "C3 no freshness ping on wrong interface");
    CHECK(h.frees == f0 + 1, "C3 slot freed");
  }
  {
    CHECK(h.send(MT_DISC, 3, C1, 0x113, L1, 7, 0xFFFF), "C4 consumed");
    Resp e = echo(MT_DISC, ST_OK, 3, C1, 0x113, L1, 7);  // flags forced 0
    h.expect_resp("C4 disconnect no-op", e);
    CHECK(h.gates.empty(), "C4 changes nothing");
  }
  {
    CHECK(h.send(MT_GTXC, 3, C1, 0x114, L1, 7, 0), "C5 consumed");
    Resp e = echo(MT_GTXC, ST_NSUPP, 3, C1, 0x114, L1, 7);
    h.expect_resp("C5 get_tx_connection", e);
  }
  { // V3: flags beyond the committed PDU length read as 0
    uint32_t t3 = 3000; d->now_ms_i = t3;
    CHECK(h.send(MT_PROBE, 0, C1, 0x115, L1, 4, FL_FC | FL_SW, 0, 44),
          "C6 consumed");
    h.run(8);
    h.expect_arm("C6 ping", 0, t3 + T_DAFRESH, false);
    h.expect_open("C6 declare", 0, da[0]);
    Resp e = echo(MT_PROBE, ST_OK, 0, C1, 0x115, L1, 4);
    e.sid = sid_of(0); e.da = da[0]; e.vlan = VID;
    e.flags = 0;                              // flags @50 beyond 44-B PDU
    h.expect_resp("C6 truncated", e);
  }
  { // non-talker transaction: consumed, freed, ignored
    int f0 = h.frees;
    CHECK(h.send(MT_PROBE, 3, C1, 0x116, L1, 7, 0, 0, 56, 0xDEADull),
          "C7 consumed");
    h.run(4);
    CHECK(h.resps.empty() && h.gates.empty() && h.arms.empty(),
          "C7 foreign talker_entity_id ignored");
    CHECK(h.frees == f0 + 1, "C7 slot freed");
  }
  h.drained("C");

  // ---- D: T-SRP-DAFRESH freshness expiry ------------------------------
  h.set_lsn(3, LSN_NONE);                     // gate now rests on freshness
  CHECK(h.gates.empty(), "D1 still fresh: no withdraw on listener loss");
  d->now_ms_i = t2 + T_DAFRESH + 1;           // window of B4 has lapsed
  h.fire_expiry(3);
  h.run(8);
  h.expect_close("D2 freshness lapse", 3);
  {
    CHECK(h.send(MT_GTXS, 3, C1, 0x121, 0, 0, 0), "D3 consumed");
    Resp e = echo(MT_GTXS, ST_OK, 3, C1, 0x121, 0, 0);
    e.sid = sid_of(3); e.da = da[3]; e.vlan = VID;  // DA kept while undeclared
    h.expect_resp("D3", e);
  }
  uint32_t t4 = 40000; d->now_ms_i = t4;
  CHECK(h.send(MT_PROBE, 3, C1, 0x122, L1, 7, FL_FC), "D4 consumed");
  h.run(8);
  h.expect_arm("D4 ping", 3, t4 + T_DAFRESH, false);
  h.expect_open("D4 re-declare", 3, da[3]);
  { Resp e = echo(MT_PROBE, ST_OK, 3, C1, 0x122, L1, 7);
    e.sid = sid_of(3); e.da = da[3]; e.vlan = VID; e.flags = FL_FC;
    h.expect_resp("D4", e); }
  h.drained("D");

  // ---- E: MAAP conflict -> withdraw -> LEAVEALL2 -> new DA ------------
  uint32_t t5 = 50000; d->now_ms_i = t5;
  h.draw_val = 10000;                         // T-MRP-LEAVEALL draw
  d->maap_conflict_valid_i = 1; d->maap_conflict_src_i = 3;
  h.tick();
  h.run(20);
  h.expect_close("E1 conflict withdraw", 3);
  CHECK(h.draws.size() == 1 && h.draws[0] == 3, "E1 draw kind 3");
  h.draws.clear();
  h.expect_arm("E1 leaveall2", 3, t5 + 2 * 10000, false);  // 2x the draw
  {
    CHECK(h.send(MT_PROBE, 3, C1, 0x130, L1, 7, FL_FC), "E2 consumed");
    Resp e = echo(MT_PROBE, ST_DMAC_FAIL, 3, C1, 0x130, L1, 7);
    e.flags = FL_FC;                          // conflicted DA is invalid
    h.expect_resp("E2 backoff probe", e);
    CHECK(h.arms.empty(), "E2 no DAFRESH arm while BACKOFF holds the slot");
  }
  h.set_lsn(3, LSN_READY);                    // a listener is registered
  d->now_ms_i = t5 + 2 * 10000 + 1;
  h.fire_expiry(3);
  h.run(40);
  h.expect_mreq("E3 re-alloc", 3, false);
  uint64_t da3b = da_pool(8);                 // ninth grant overall
  h.expect_open("E3 re-declare new DA", 3, da3b);
  {
    CHECK(h.send(MT_GTXS, 3, C1, 0x131, 0, 0, 0), "E4 consumed");
    Resp e = echo(MT_GTXS, ST_OK, 3, C1, 0x131, 0, 0);
    e.sid = sid_of(3); e.da = da3b; e.vlan = VID;
    h.expect_resp("E4 new DA answered", e);
  }
  h.drained("E");

  // ---- E7: conflict while DA_OK (nothing declared): re-alloc, no backoff
  d->maap_conflict_valid_i = 1; d->maap_conflict_src_i = 7;
  h.tick();
  h.run(30);
  CHECK(h.draws.empty(), "E7 no backoff draw when nothing was declared");
  CHECK(h.gates.empty(), "E7 no withdraw when nothing was declared");
  h.expect_mreq("E7 re-alloc", 7, false);
  uint64_t da7b = da_pool(9);
  {
    CHECK(h.send(MT_GTXS, 7, C1, 0x132, 0, 0, 0), "E7b consumed");
    Resp e = echo(MT_GTXS, ST_OK, 7, C1, 0x132, 0, 0);
    e.sid = sid_of(7); e.da = da7b; e.vlan = VID;
    h.expect_resp("E7b", e);
  }
  h.drained("E7");

  // ---- F: SR-class PCP change -> backoff that KEEPS the DA ------------
  h.set_lsn(1, LSN_READY);
  h.expect_open("F1 listener-driven declare", 1, da[1]);
  uint32_t t6 = 90000; d->now_ms_i = t6;
  h.draw_val = 12000;
  d->srp_pcp_change_i = 1;
  h.tick();
  h.run(90);                                  // src0 (C6), src1, src3 declaring
  h.expect_close("F2 pcp withdraw src0", 0);
  h.expect_arm("F2 leaveall2 src0", 0, t6 + 2 * 12000, false);
  h.expect_close("F2 pcp withdraw src1", 1);
  h.expect_arm("F2 leaveall2 src1", 1, t6 + 2 * 12000, false);
  h.expect_close("F2 pcp withdraw src3", 3);
  h.expect_arm("F2 leaveall2 src3", 3, t6 + 2 * 12000, false);
  CHECK(h.draws.size() == 3, "F2 three draws");
  h.draws.clear();
  {
    CHECK(h.send(MT_PROBE, 1, C1, 0x140, L1, 9, FL_SW), "F3 consumed");
    Resp e = echo(MT_PROBE, ST_OK, 1, C1, 0x140, L1, 9);
    e.sid = sid_of(1); e.da = da[1]; e.vlan = VID; e.flags = FL_SW;
    h.expect_resp("F3 pcp backoff keeps the DA", e);
    CHECK(h.arms.empty(), "F3 no DAFRESH arm while BACKOFF holds the slot");
  }
  // compressed exit: expiry fired while the F3 ping is still fresh — the
  // gate re-declares AND re-arms DAFRESH to the ABSOLUTE remaining window
  uint32_t tping = t6;                        // F3 was sent at t6
  h.fire_expiry(1);
  h.run(10);
  h.expect_open("F4 re-declare same DA", 1, da[1]);
  h.expect_arm("F4 remaining freshness", 1, tping + T_DAFRESH, false);
  d->now_ms_i = t6 + 2 * 12000 + 1;
  h.fire_expiry(3);
  h.run(10);
  h.expect_open("F5 re-declare src3 same DA", 3, da3b);
  // src0: backoff over, but stale ping + no listener = gate stays closed
  h.fire_expiry(0);
  h.run(10);
  CHECK(h.gates.empty() && h.arms.empty(),
        "F6 src0 exits backoff without declaring");
  h.drained("F");

  // ---- G: per-source independence + stateless across interleave -------
  {
    CHECK(h.send(MT_GTXS, 2, C1, 0x150, 0, 0, 0), "G1 consumed");
    Resp e = echo(MT_GTXS, ST_OK, 2, C1, 0x150, 0, 0);
    e.sid = sid_of(2); e.da = da[2]; e.vlan = VID;
    h.expect_resp("G1 src2 untouched by src1/src3 churn", e);
  }
  uint32_t t7 = 200000; d->now_ms_i = t7;
  CHECK(h.send(MT_PROBE, 4, C1, 0x151, L1, 5, FL_FC), "G2a consumed");
  h.run(8);
  Resp g2a = h.pop_resp("G2a");
  h.expect_open("G2a declare", 4, da[4]);
  h.expect_arm("G2a ping", 4, t7 + T_DAFRESH, false);
  // interleaved traffic on other sources
  CHECK(h.send(MT_GTXS, 5, C1, 0x152, 0, 0, 0), "G3 consumed");
  h.pop_resp("G3");
  h.set_lsn(5, LSN_READY);
  h.expect_open("G3 declare src5", 5, da[5]);
  CHECK(h.send(MT_DISC, 4, C1, 0x153, L1, 5, 0), "G4 consumed");
  h.pop_resp("G4");
  // identical PROBE again: byte-identical answer, no hidden state grew
  CHECK(h.send(MT_PROBE, 4, C1, 0x151, L1, 5, FL_FC), "G2b consumed");
  h.run(8);
  Resp g2b = h.pop_resp("G2b");
  CHECK(g2a == g2b, "G2 stateless: identical probe = identical bytes");
  CHECK(g2a.status == ST_OK && g2a.da == da[4], "G2 content sane");
  h.expect_arm("G2b re-ping", 4, t7 + T_DAFRESH, false);
  h.drained("G");

  // ---- H: source removed from the configuration -----------------------
  d->cfg_src_en_i = 0xBF;                     // src6 leaves
  h.run(30);
  h.expect_arm("H1 cancel shared slot", 6, 0, true);
  h.expect_mreq("H1 release DA", 6, true);
  CHECK(h.gates.empty(), "H1 src6 was not declaring: no withdraw");
  {
    CHECK(h.send(MT_PROBE, 6, C1, 0x160, L1, 3, FL_FC), "H2 consumed");
    Resp e = echo(MT_PROBE, ST_TK_UNKNOWN, 6, C1, 0x160, L1, 3);
    e.flags = FL_FC;
    h.expect_resp("H2 disabled source", e);
  }
  h.drained("H");

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete d;
  return fails ? 1 : 0;
}
