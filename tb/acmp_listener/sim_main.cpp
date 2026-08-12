// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_acmp_listener suite — THE MTXW WALK (docs/architecture/09 §3).
//
// Every one of the 112 cells of F05.3 (14 events x 8 states) is driven
// against an INDEPENDENT C++ matrix model transcribed from the doc table
// WITHOUT gen_ltn_rom.py — the two transcriptions must agree through the
// DUT's behavior; that is the point. Every '—'/'ign' cell is proven inert
// (state unchanged, no action strobes, no frames, no timer ops, no notify).
// The harness emulates the four landed faces cycle-exactly (KL_pp_rx_slots
// sync read, KL_pp_tx_slots grant-after-request, KL_pp_timer_service arm
// bus, KL_pp_prng draw) and captures every observable: record write-backs,
// 56-byte committed ACMPDUs, timer arm/cancel ops, action strobes.
//
// Timer-row '—' cells come in two honest kinds: states with NO armed SM
// timer (UNB/PWA/SOK — 12 cells) get a spurious expiry injected and full
// inertness checked; states whose armed T-ID aliases the shared slot (15
// cells) are proven impossible by construction — the deadline armed on
// entry equals the state's own T-ID, so the foreign expiry cannot exist —
// plus the model's own transcription must mark the cell '—'.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "VKL_acmp_listener.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// ---- constants shared with the doc (not with the RTL) ---------------------
static const uint64_t OUR_EID = 0x0A0B0C0D0E0F1011ull;
static const uint64_t CTL1 = 0x0011223344556677ull;
static const uint64_t CTL2 = 0x0099AABBCCDDEEFFull;
static const uint64_t TK_A = 0x00221100AABBCCDDull;
static const uint64_t TK_B = 0x00221100AABBCC55ull;
static const uint16_t TKUID_A = 1, TKUID_B = 2;
static const uint32_t NOW = 50000;
static const uint16_t DRAWVAL = 777;         // scripted PRNG kind-0 draw
static const int N_SINKS = 8;
static const int TMR_BASE = 9, OWNER_BASE = 32;
enum { T_CMD = 200, T_RETRY = 4000, T_NOTK = 10000 };
enum { ST_OK = 0, ST_LUID = 1, ST_TT = 7, ST_NOAUTH = 13, ST_NOBW = 5 };
enum { M_PROBE_CMD = 0, M_PROBE_RESP = 1, M_BIND = 6, M_BIND_R = 7,
       M_UNBIND = 8, M_UNBIND_R = 9, M_GETRX = 10, M_GETRX_R = 11 };
enum { S_UNB, S_PWA, S_PWD, S_PWR, S_PW2, S_PWT, S_SNR, S_SOK };
enum { R_BIND_SAME, R_BIND_NEW, R_UNBIND, R_GETRX, R_PROBE_OK, R_PROBE_FAIL,
       R_TMR_DELAY, R_TMR_CMD, R_TMR_RETRY, R_TMR_NOTK,
       R_TK_DISC, R_TK_DEP, R_TK_REG, R_TK_UNREG };
static const char* RN[14] = {"BIND_SAME","BIND_NEW","UNBIND","GETRX",
  "PROBE_OK","PROBE_FAIL","TMR_DELAY","TMR_CMD","TMR_RETRY","TMR_NOTK",
  "TK_DISC","TK_DEP","TK_REG","TK_UNREG"};
static const char* SN[8] = {"UNB","PWA","PWD","PWR","PW2","PWT","SNR","SOK"};

// ---- bit helpers ----------------------------------------------------------
static void wput(uint32_t* w, int lsb, int width, uint64_t v) {
  for (int i = 0; i < width; ++i) {
    int b = lsb + i;
    if ((v >> i) & 1) w[b >> 5] |= 1u << (b & 31);
    else              w[b >> 5] &= ~(1u << (b & 31));
  }
}
static uint64_t wget(const uint32_t* w, int lsb, int width) {
  uint64_t v = 0;
  for (int i = 0; i < width; ++i) {
    int b = lsb + i;
    if ((w[b >> 5] >> (b & 31)) & 1) v |= 1ull << i;
  }
  return v;
}

// ---- the wire ACMPDU (F05.13 transcription — used for stimulus AND
// expectation; big-endian fields at the documented byte offsets) ------------
struct Pdu { uint8_t b[56]; };
static void be(uint8_t* p, uint64_t v, int n) {
  for (int i = 0; i < n; ++i) p[i] = uint8_t(v >> (8 * (n - 1 - i)));
}
static Pdu mk_pdu(uint8_t msg, uint8_t status, uint64_t sid, uint64_t ctlr,
                  uint64_t tkeid, uint64_t leid, uint16_t tkuid,
                  uint16_t luid, uint64_t da, uint16_t cc, uint16_t seq,
                  uint16_t flags, uint16_t vlan) {
  Pdu p{};
  p.b[0] = 0xFC;                       // subtype
  p.b[1] = msg & 0x0F;                 // h=0, ver=0, message_type
  p.b[2] = uint8_t((status << 3) | 0); // status . cdl[10:8] (cdl 44)
  p.b[3] = 44;
  be(p.b + 4, sid, 8);
  be(p.b + 12, ctlr, 8);
  be(p.b + 20, tkeid, 8);
  be(p.b + 28, leid, 8);
  be(p.b + 36, tkuid, 2);
  be(p.b + 38, luid, 2);
  be(p.b + 40, da, 6);
  be(p.b + 46, cc, 2);
  be(p.b + 48, seq, 2);
  be(p.b + 50, flags, 2);
  be(p.b + 52, vlan, 2);
  return p;
}

// ---- record image (F07.6 transcription) -----------------------------------
struct Rec {
  uint8_t sm = 0, pbsta = 0, acmpsta = 0, srp_decl = 0;
  bool bound = false, started = false, sw = false, retried = false;
  bool tk_reg = false, tk_disc = false;
  uint64_t talker_eid = 0, bind_ctlr = 0, sid = 0, da = 0;
  uint16_t talker_uid = 0, probe_seq = 0, vlan = 0;
  uint32_t last_avail = 0;
  uint8_t ifx = 0, smh = 0, noadp = 0;
  bool operator==(const Rec& o) const {
    return sm == o.sm && pbsta == o.pbsta && acmpsta == o.acmpsta &&
           srp_decl == o.srp_decl && bound == o.bound &&
           started == o.started && sw == o.sw && retried == o.retried &&
           tk_reg == o.tk_reg && tk_disc == o.tk_disc &&
           talker_eid == o.talker_eid && bind_ctlr == o.bind_ctlr &&
           sid == o.sid && da == o.da && talker_uid == o.talker_uid &&
           probe_seq == o.probe_seq && vlan == o.vlan &&
           last_avail == o.last_avail && ifx == o.ifx && smh == o.smh &&
           noadp == o.noadp;
  }
};
static Rec unpack(const uint32_t* w) {
  Rec r;
  r.sm = uint8_t(wget(w, 0, 3));   r.pbsta = uint8_t(wget(w, 3, 3));
  r.acmpsta = uint8_t(wget(w, 6, 5));
  r.bound = wget(w, 11, 1);        r.started = wget(w, 12, 1);
  r.sw = wget(w, 13, 1);           r.retried = wget(w, 14, 1);
  r.srp_decl = uint8_t(wget(w, 15, 2));
  r.tk_reg = wget(w, 17, 1);       r.tk_disc = wget(w, 18, 1);
  r.talker_eid = wget(w, 32, 64);  r.talker_uid = uint16_t(wget(w, 96, 16));
  r.probe_seq = uint16_t(wget(w, 112, 16));
  r.bind_ctlr = wget(w, 128, 64);  r.sid = wget(w, 192, 64);
  r.da = wget(w, 256, 48);         r.vlan = uint16_t(wget(w, 304, 12));
  r.last_avail = uint32_t(wget(w, 320, 32));
  r.ifx = uint8_t(wget(w, 352, 8)); r.smh = uint8_t(wget(w, 360, 8));
  r.noadp = uint8_t(wget(w, 368, 8));
  return r;
}

// ---- stimulus -------------------------------------------------------------
struct Stim {
  enum K { TXN, EXP, TK } k = TXN;
  uint8_t msg = 0, status = 0;
  uint16_t uid = 0, seq = 0, flags = 0, tk_uid = 0, vlan = 0;
  uint64_t ctlr = 0, tk_eid = 0, sid = 0, da = 0, target = OUR_EID;
  uint8_t tk_kind = 0; bool tk_fail = false;
  uint8_t protocol = 1;  // PP_PROTO_ACMP
};

// ---- expected effects -----------------------------------------------------
struct TOp { bool cancel; uint32_t deadline; };
struct Exp {
  bool wrote = false;
  std::vector<Pdu> frames;
  std::vector<TOp> tops;
  bool settle = false, teardown = false, disc_arm = false;
  bool disc_disarm = false, nvm = false, nvm_set = false;
  uint64_t disc_eid = 0, settle_sid = 0, settle_da = 0;
  uint16_t settle_vlan = 0;
  int notify = 0, frees = 0;
  int row = -1;  // classified row (for walk bookkeeping)
};

// ==========================================================================
// THE INDEPENDENT MATRIX MODEL — F05.3 transcribed from the doc table,
// row by row, WITHOUT looking at gen_ltn_rom.py. Cell = {kind, next,
// action list in the doc's listed order}. 'cond' = the dagger legend:
// tk? PWD/A12 : PWA/A17 (base acts run first).
// ==========================================================================
enum { C_DASH, C_IGN, C_NORM, C_COND };
struct MCell { int t; int next; const char* acts; };
static const MCell M[14][8] = {
  // BIND_RX same talker+source: UNB impossible, else self/A1 A6 A3
  {{C_DASH,0,""},        {C_NORM,S_PWA,"1 6 3"}, {C_NORM,S_PWD,"1 6 3"},
   {C_NORM,S_PWR,"1 6 3"},{C_NORM,S_PW2,"1 6 3"},{C_NORM,S_PWT,"1 6 3"},
   {C_NORM,S_SNR,"1 6 3"},{C_NORM,S_SOK,"1 6 3"}},
  // BIND_RX new/different source
  {{C_NORM,S_PWR,"1 2 3 4 5"},
   {C_NORM,S_PWR,"1 11 9 2 3 4 5"},  {C_NORM,S_PWR,"1 11 9 2 3 4 5"},
   {C_NORM,S_PWR,"1 11 9 2 3 4 5"},  {C_NORM,S_PWR,"1 11 9 2 3 4 5"},
   {C_NORM,S_PWR,"1 11 9 2 3 4 5"},
   {C_NORM,S_PWR,"1 11 8 9 2 3 4 5"},{C_NORM,S_PWR,"1 11 8 9 2 3 4 5"}},
  // UNBIND_RX
  {{C_NORM,S_UNB,"1 7"},
   {C_NORM,S_UNB,"1 11 9 10 7"},  {C_NORM,S_UNB,"1 11 9 10 7"},
   {C_NORM,S_UNB,"1 11 9 10 7"},  {C_NORM,S_UNB,"1 11 9 10 7"},
   {C_NORM,S_UNB,"1 11 9 10 7"},
   {C_NORM,S_UNB,"1 11 8 9 10 7"},{C_NORM,S_UNB,"1 11 8 9 10 7"}},
  // GET_RX_STATE
  {{C_NORM,S_UNB,"16"},{C_NORM,S_PWA,"16"},{C_NORM,S_PWD,"16"},
   {C_NORM,S_PWR,"16"},{C_NORM,S_PW2,"16"},{C_NORM,S_PWT,"16"},
   {C_NORM,S_SNR,"16"},{C_NORM,S_SOK,"16"}},
  // PROBE_RESP SUCCESS
  {{C_IGN,0,""},{C_IGN,0,""},{C_IGN,0,""},
   {C_NORM,S_SNR,"11 15"},{C_NORM,S_SNR,"11 15"},
   {C_IGN,0,""},{C_IGN,0,""},{C_IGN,0,""}},
  // PROBE_RESP != SUCCESS
  {{C_IGN,0,""},{C_IGN,0,""},{C_IGN,0,""},
   {C_NORM,S_PWT,"11 14"},{C_NORM,S_PWT,"11 14"},
   {C_IGN,0,""},{C_IGN,0,""},{C_IGN,0,""}},
  // T-ACMP-DELAY expiry
  {{C_DASH,0,""},{C_DASH,0,""},{C_NORM,S_PWR,"5"},{C_DASH,0,""},
   {C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""}},
  // T-ACMP-CMD expiry
  {{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},{C_NORM,S_PW2,"13"},
   {C_NORM,S_PWT,"14"},{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""}},
  // T-ACMP-RETRY expiry (dagger)
  {{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},
   {C_DASH,0,""},{C_COND,0,""},{C_DASH,0,""},{C_DASH,0,""}},
  // T-ACMP-NOTK expiry (dagger, A8 first)
  {{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},
   {C_DASH,0,""},{C_DASH,0,""},{C_COND,0,"8"},{C_DASH,0,""}},
  // EVT_TK_DISCOVERED ('ign (note)': bookkeeping still tracks)
  {{C_DASH,0,""},{C_NORM,S_PWD,"12"},{C_IGN,0,""},{C_IGN,0,""},
   {C_IGN,0,""},{C_IGN,0,""},{C_IGN,0,""},{C_IGN,0,""}},
  // EVT_TK_DEPARTED (SNR/SOK note only: reservation kept)
  {{C_DASH,0,""},{C_DASH,0,""},
   {C_NORM,S_PWA,"11 17"},{C_NORM,S_PWA,"11 17"},
   {C_NORM,S_PWA,"11 17"},{C_NORM,S_PWA,"11 17"},
   {C_IGN,0,""},{C_IGN,0,""}},
  // EVT_TK_REGISTERED — SNR only; SOK is the documented REF-BUG 'x'
  {{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},
   {C_DASH,0,""},{C_DASH,0,""},{C_NORM,S_SOK,"11"},{C_DASH,0,""}},
  // EVT_TK_UNREGISTERED (dagger, A8 first) — SOK only
  {{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},
   {C_DASH,0,""},{C_DASH,0,""},{C_DASH,0,""},{C_COND,0,"8"}},
};

struct Model {
  Rec rec[N_SINKS];
  uint16_t probe_ctr = 0;
  bool lock_held = false;
  uint64_t lock_eid = 0;

  // classify the physical stimulus onto an F05.3 row (mirrors 05 §3 + the
  // shared-SM-slot rule of 08 §5) — independent of the DUT's classifier
  int classify(int sink, const Stim& s) const {
    const Rec& r = rec[sink];
    if (s.k == Stim::TXN) {
      if (s.msg == M_BIND)
        return (r.bound && s.tk_eid == r.talker_eid &&
                s.tk_uid == r.talker_uid) ? R_BIND_SAME : R_BIND_NEW;
      if (s.msg == M_UNBIND) return R_UNBIND;
      if (s.msg == M_GETRX) return R_GETRX;
      return (s.status == ST_OK) ? R_PROBE_OK : R_PROBE_FAIL;
    }
    if (s.k == Stim::EXP) {
      switch (r.sm) {
        case S_PWD: return R_TMR_DELAY;
        case S_PWR: case S_PW2: return R_TMR_CMD;
        case S_PWT: return R_TMR_RETRY;
        case S_SNR: return R_TMR_NOTK;
        default:    return R_TMR_CMD;  // spurious: every such cell is '—'
      }
    }
    return R_TK_DISC + s.tk_kind;
  }

  Pdu f_bind(const Stim& s) const {
    return mk_pdu(M_BIND_R, ST_OK, 0, s.ctlr, s.tk_eid, OUR_EID, s.tk_uid,
                  s.uid, 0, 1, s.seq, uint16_t(s.flags & 0x0008), 0);
  }
  Pdu f_unbind(const Stim& s) const {
    return mk_pdu(M_UNBIND_R, ST_OK, 0, s.ctlr, s.tk_eid, OUR_EID, s.tk_uid,
                  s.uid, 0, 0, s.seq, 0, 0);
  }
  Pdu f_getrx(int sink, const Stim& s) const {
    const Rec& r = rec[sink];
    bool settled = (r.sm == S_SNR || r.sm == S_SOK);
    bool rf = (r.sm == S_SOK) && ((r.srp_decl >> 1) & 1);
    uint16_t fl = r.bound ? uint16_t(0x0002 | (r.sw ? 0x0008 : 0) |
                                     (rf ? 0x0040 : 0)) : 0;
    return mk_pdu(M_GETRX_R, ST_OK, settled ? r.sid : 0, s.ctlr,
                  r.bound ? r.talker_eid : 0, OUR_EID,
                  r.bound ? r.talker_uid : 0, s.uid,
                  settled ? r.da : 0, r.bound ? 1 : 0, s.seq, fl,
                  settled ? r.vlan : 0);
  }
  Pdu f_probe(int sink) const {
    const Rec& r = rec[sink];
    return mk_pdu(M_PROBE_CMD, ST_OK, 0, r.bind_ctlr, r.talker_eid, OUR_EID,
                  r.talker_uid, uint16_t(sink), 0, 0, r.probe_seq, 0x0002, 0);
  }
  Pdu f_err(uint8_t msg, uint8_t status, const Stim& s) const {
    return mk_pdu(uint8_t(msg + 1), status, 0, s.ctlr, s.tk_eid, OUR_EID,
                  s.tk_uid, s.uid, 0, 0, s.seq, 0, 0);
  }

  // predict all observable effects of one stimulus (the reference)
  Exp predict(int sink, const Stim& s) {
    Exp e;
    e.frees = (s.k == Stim::TXN) ? 1 : 0;
    if (s.k == Stim::TXN) {
      if (s.protocol != 1 ||
          !(s.msg == M_PROBE_RESP || s.msg == M_BIND || s.msg == M_UNBIND ||
            s.msg == M_GETRX) || s.target != OUR_EID)
        return e;  // consumed silently, nothing else
      if (s.uid >= N_SINKS) {
        if (s.msg != M_PROBE_RESP)
          e.frames.push_back(f_err(s.msg, ST_LUID, s));
        return e;
      }
      if (s.msg == M_PROBE_RESP) {
        const Rec& r = rec[sink];
        if (!(s.ctlr == r.bind_ctlr && s.tk_eid == r.talker_eid &&
              s.tk_uid == r.talker_uid && s.seq == r.probe_seq))
          return e;  // 05 §3: silently ignore
      }
    }
    int row = classify(sink, s);
    e.row = row;
    Rec& r = rec[sink];
    const MCell& c = M[row][r.sm];

    // event bookkeeping, gated exactly by cell validity (not '—')
    bool valid = (c.t != C_DASH);
    if (valid && row == R_TK_DISC) r.tk_disc = true;
    if (valid && row == R_TK_DEP) r.tk_disc = false;
    if (valid && c.t != C_IGN && row == R_TK_REG) {
      r.tk_reg = true;
      r.srp_decl = uint8_t((r.srp_decl & 1) | (s.tk_fail ? 2 : 0));
    }
    e.wrote = true;               // every classified work item writes back
    r.smh = uint8_t(TMR_BASE + sink);
    if (c.t == C_DASH || c.t == C_IGN) return e;

    // resolve the cell's action list + next state
    std::vector<int> acts;
    int tok = 0;
    for (const char* p = c.acts; *p;) {
      if (*p >= '0' && *p <= '9') { tok = tok * 10 + (*p - '0'); }
      else if (tok) { acts.push_back(tok); tok = 0; }
      ++p;
    }
    if (tok) acts.push_back(tok);
    int next = c.next;
    if (c.t == C_COND) {
      if (r.tk_disc) { next = S_PWD; acts.push_back(12); }
      else           { next = S_PWA; acts.push_back(17); }
    }

    // A1 gate (05 §6.3 legend)
    bool has_a1 = !acts.empty() && acts[0] == 1;
    if (has_a1 && lock_held && lock_eid != s.ctlr) {
      e.frames.push_back(f_err(s.msg, ST_NOAUTH, s));
      e.wrote = false;            // abort cell: no commit
      return e;
    }

    bool mut = false;
    r.sm = uint8_t(next);
    for (int a : acts) {
      switch (a) {
        case 1: break;
        case 2:
          r.talker_eid = s.tk_eid; r.talker_uid = s.tk_uid;
          r.bind_ctlr = s.ctlr; r.sw = (s.flags >> 3) & 1; r.bound = true;
          e.nvm = true; e.nvm_set = true; mut = true; break;
        case 3: e.frames.push_back(f_bind(s)); break;
        case 4:
          e.disc_arm = true; e.disc_eid = r.talker_eid; r.pbsta = 1;
          mut = true; break;
        case 5:
          r.probe_seq = probe_ctr++; r.retried = false; r.pbsta = 2;
          r.acmpsta = 0; e.frames.push_back(f_probe(sink));
          e.tops.push_back({false, NOW + T_CMD}); mut = true; break;
        case 6:
          r.bind_ctlr = s.ctlr; r.sw = (s.flags >> 3) & 1; mut = true; break;
        case 7: e.frames.push_back(f_unbind(s)); break;
        case 8:
          e.teardown = true; r.sid = 0; r.da = 0; r.vlan = 0;
          r.tk_reg = false; r.srp_decl = 0; mut = true; break;
        case 9: e.disc_disarm = true; r.tk_disc = false; mut = true; break;
        case 10:
          r.talker_eid = 0; r.talker_uid = 0; r.bind_ctlr = 0;
          r.probe_seq = 0; r.bound = false; r.sw = false;
          r.started = false; r.retried = false; r.pbsta = 0; r.acmpsta = 0;
          e.nvm = true; e.nvm_set = false; mut = true; break;
        case 11: e.tops.push_back({true, 0}); break;
        case 12:
          r.pbsta = 2; r.acmpsta = 0;
          e.tops.push_back({false, NOW + DRAWVAL}); mut = true; break;
        case 13:
          r.retried = true; e.frames.push_back(f_probe(sink));
          e.tops.push_back({false, NOW + T_CMD}); mut = true; break;
        case 14:
          e.tops.push_back({false, NOW + T_RETRY});
          r.acmpsta = (row == R_PROBE_FAIL) ? s.status : ST_TT;
          mut = true; break;
        case 15:
          r.sid = s.sid; r.da = s.da; r.vlan = uint16_t(s.vlan & 0xFFF);
          r.srp_decl |= 1; r.pbsta = 3; r.acmpsta = 0;
          e.settle = true; e.settle_sid = s.sid; e.settle_da = s.da;
          e.settle_vlan = uint16_t(s.vlan & 0xFFF);
          e.tops.push_back({false, NOW + T_NOTK}); mut = true; break;
        case 16: e.frames.push_back(f_getrx(sink, s)); break;
        case 17: r.pbsta = 1; r.acmpsta = 0; mut = true; break;
        default: break;
      }
    }
    e.notify = mut ? 1 : 0;
    return e;
  }
};

// ==========================================================================
// Harness: cycle-exact emulation of the landed faces + collectors
// ==========================================================================
struct ATop { bool cancel; uint8_t slot, owner; uint32_t deadline; };
struct Col {
  int wrotes = 0, frees = 0, notifies = 0;
  std::vector<Pdu> frames;
  std::vector<ATop> tops;
  bool settle = false, teardown = false, disc_arm = false;
  bool disc_disarm = false, nvm = false, nvm_set = false;
  uint64_t disc_eid = 0, settle_sid = 0, settle_da = 0;
  uint16_t settle_vlan = 0;
  uint8_t free_slot = 0xFF;
  void clear() { *this = Col(); }
};

struct Harness {
  VKL_acmp_listener* d;
  Rec shadow[N_SINKS];
  Col col;
  uint8_t rxmem[4][576] = {};
  uint8_t txmem[5][1600] = {};
  uint16_t txlen[5] = {};
  bool txfree[5] = {true, true, true, true, true};
  bool gnt_pending = false;
  uint8_t gnt_slot = 0;
  uint8_t rx_pending_byte = 0;
  int prng_cnt = 0;
  bool prng_fire = false;
  // one-shot injections for the NEXT tick
  bool inj_exp = false; uint8_t inj_exp_sink = 0;
  int next_rx_slot = 0;
  uint32_t last_arm_deadline[N_SINKS] = {};

  explicit Harness(VKL_acmp_listener* dut) : d(dut) {}

  void tick() {
    // inputs for this cycle (face responses to LAST cycle's outputs)
    d->rxs_rd_data_i = rx_pending_byte;
    d->txs_alloc_gnt_i = gnt_pending;
    d->txs_alloc_slot_i = gnt_slot;
    gnt_pending = false;
    d->draw_busy_i = (prng_cnt > 0);
    d->draw_valid_i = prng_fire;
    d->draw_ms_i = prng_fire ? DRAWVAL : 0;
    if (prng_fire) prng_fire = false;
    else if (prng_cnt > 0 && --prng_cnt == 0) prng_fire = true;
    d->tmr_exp_valid_i = inj_exp;
    d->tmr_exp_slot_i = uint8_t(TMR_BASE + inj_exp_sink);
    d->tmr_exp_owner_i = uint8_t(OWNER_BASE + inj_exp_sink);
    inj_exp = false;
    d->now_ms_i = NOW;

    d->clk_i = 0; d->eval();

    // sample this cycle's outputs (pre-edge)
    if (d->rxs_rd_en_o)
      rx_pending_byte = rxmem[d->rxs_rd_slot_o][d->rxs_rd_addr_o];
    if (d->txs_alloc_req_o && !d->txs_oversize_o) {
      for (int i = 0; i < 4; ++i)
        if (txfree[i]) { txfree[i] = false; gnt_slot = uint8_t(i);
                         gnt_pending = true; break; }
    }
    if (d->txs_wr_valid_o && d->txs_wr_addr_o < 1600)
      txmem[d->txs_wr_slot_o][d->txs_wr_addr_o] = d->txs_wr_data_o;
    if (d->txs_wr_commit_o) txlen[d->txs_wr_slot_o] = d->txs_wr_len_o;
    if (d->txreq_valid_o) {
      Pdu p{};
      int n = txlen[d->txreq_slot_o] < 56 ? txlen[d->txreq_slot_o] : 56;
      memcpy(p.b, txmem[d->txreq_slot_o], size_t(n));
      col.frames.push_back(p);
      txfree[d->txreq_slot_o] = true;  // arbiter auto-free after serialize
    }
    if (d->draw_req_o && prng_cnt == 0 && !prng_fire) prng_cnt = 2;
    if (d->tmr_arm_valid_o) {
      col.tops.push_back({bool(d->tmr_arm_cancel_o), d->tmr_arm_slot_o,
                          d->tmr_arm_owner_o, d->tmr_arm_deadline_ms_o});
      if (!d->tmr_arm_cancel_o && d->tmr_arm_slot_o >= TMR_BASE &&
          d->tmr_arm_slot_o < TMR_BASE + N_SINKS)
        last_arm_deadline[d->tmr_arm_slot_o - TMR_BASE] =
            d->tmr_arm_deadline_ms_o;
    }
    if (d->rxs_free_o) { col.frees++; col.free_slot = d->rxs_free_slot_o; }
    if (d->act_settle_o) {
      col.settle = true; col.settle_sid = d->act_settle_sid_o;
      col.settle_da = d->act_settle_da_o; col.settle_vlan = d->act_settle_vlan_o;
    }
    if (d->act_teardown_o) col.teardown = true;
    if (d->act_disc_arm_o) { col.disc_arm = true;
                             col.disc_eid = d->act_disc_talker_eid_o; }
    if (d->act_disc_disarm_o) col.disc_disarm = true;
    if (d->act_nvm_o) { col.nvm = true; col.nvm_set = d->act_nvm_set_o; }
    if (d->act_notify_o) col.notifies++;
    if (d->dbg_recwr_o) {
      col.wrotes++;
      shadow[d->dbg_recwr_sink_o] = unpack(&d->dbg_recwr_rec_o[0]);
    }

    d->clk_i = 1; d->eval();
  }

  bool wait_idle(int cap = 4000) {
    for (int i = 0; i < cap; ++i) { if (d->txn_ready_o) return true; tick(); }
    return false;
  }
  // wait_idle returns on the first idle cycle WITHOUT sampling it; strobes
  // registered on the retire edge (rxs_free_o) are visible exactly then, so
  // every drive ends with one trailing sampled tick
  bool drain() { bool ok = wait_idle(); tick(); return ok; }

  // drive one stimulus to completion; returns false on hang/refusal
  bool drive(int sink, const Stim& s) {
    if (!wait_idle()) return false;
    if (s.k == Stim::TXN) {
      int slot = next_rx_slot; next_rx_slot = (next_rx_slot + 1) % 4;
      Pdu p = mk_pdu(s.msg, s.status, s.sid, s.ctlr, s.tk_eid, s.target,
                     s.tk_uid, s.uid, s.da, 0, s.seq, s.flags, s.vlan);
      memcpy(rxmem[slot], p.b, 56);
      uint32_t* w = &d->txn_i[0];
      for (int i = 0; i < 13; ++i) w[i] = 0;
      wput(w, 391, 2, 0);                    // origin RX
      wput(w, 354, 3, s.protocol);           // protocol
      wput(w, 350, 4, s.msg);                // msg_type
      wput(w, 345, 5, s.status);             // status_in
      wput(w, 334, 11, 44);                  // cdl
      wput(w, 222, 64, s.ctlr);              // controller_eid
      wput(w, 158, 64, s.target);            // target_eid (listener)
      wput(w, 142, 16, s.seq);               // sequence_id
      wput(w, 124, 16, s.msg);               // opcode mirror
      wput(w, 92, 16, s.tk_uid);             // operands.desc_index
      wput(w, 60, 16, s.uid);                // operands.unique_id
      wput(w, 57, 3, uint64_t(slot));        // rx_slot
      wput(w, 0, 2, 1);                      // resp_disposition ACMP mcast
      d->txn_valid_i = 1;
      tick();                                // accepted (ready was high)
      d->txn_valid_i = 0;
      tick();
      return drain();
    }
    if (s.k == Stim::EXP) {
      inj_exp = true; inj_exp_sink = uint8_t(sink);
      tick();                                // pendexp set
      tick();                                // popped
      return drain();
    }
    d->evt_tk_valid_i = 1;
    d->evt_tk_kind_i = s.tk_kind;
    d->evt_tk_failed_i = s.tk_fail;
    d->evt_tk_sink_i = uint16_t(sink);
    // ready requires idle && !txn_valid && !pend — we are idle already
    tick();                                  // accepted
    d->evt_tk_valid_i = 0;
    tick();
    return drain();
  }
};

// ---- comparators ----------------------------------------------------------
static void chk_rec(const char* tag, const Rec& got, const Rec& want) {
  CHECK(got.sm == want.sm, "%s: sm_state got %s want %s", tag,
        SN[got.sm & 7], SN[want.sm & 7]);
  CHECK(got.pbsta == want.pbsta && got.acmpsta == want.acmpsta,
        "%s: pbsta/acmpsta got %u/%u want %u/%u", tag, got.pbsta,
        got.acmpsta, want.pbsta, want.acmpsta);
  CHECK(got.bound == want.bound && got.started == want.started &&
        got.sw == want.sw && got.retried == want.retried &&
        got.srp_decl == want.srp_decl && got.tk_reg == want.tk_reg &&
        got.tk_disc == want.tk_disc,
        "%s: flags got b%d s%d w%d r%d d%u g%d t%d want b%d s%d w%d r%d d%u g%d t%d",
        tag, got.bound, got.started, got.sw, got.retried, got.srp_decl,
        got.tk_reg, got.tk_disc, want.bound, want.started, want.sw,
        want.retried, want.srp_decl, want.tk_reg, want.tk_disc);
  CHECK(got.talker_eid == want.talker_eid && got.talker_uid == want.talker_uid
        && got.bind_ctlr == want.bind_ctlr,
        "%s: binding got %016lx/%u/%016lx want %016lx/%u/%016lx", tag,
        (unsigned long)got.talker_eid, got.talker_uid,
        (unsigned long)got.bind_ctlr, (unsigned long)want.talker_eid,
        want.talker_uid, (unsigned long)want.bind_ctlr);
  CHECK(got.probe_seq == want.probe_seq, "%s: probe_seq got %u want %u",
        tag, got.probe_seq, want.probe_seq);
  CHECK(got.sid == want.sid && got.da == want.da && got.vlan == want.vlan,
        "%s: settled got %016lx/%012lx/%u want %016lx/%012lx/%u", tag,
        (unsigned long)got.sid, (unsigned long)got.da, got.vlan,
        (unsigned long)want.sid, (unsigned long)want.da, want.vlan);
  CHECK(got.smh == want.smh && got.noadp == want.noadp &&
        got.last_avail == want.last_avail,
        "%s: plumbing smh %u want %u", tag, got.smh, want.smh);
}

static void chk_cell(const char* tag, Harness& h, int sink, const Exp& e) {
  CHECK(h.col.wrotes == (e.wrote ? 1 : 0), "%s: writebacks got %d want %d",
        tag, h.col.wrotes, e.wrote ? 1 : 0);
  CHECK(h.col.frames.size() == e.frames.size(), "%s: frames got %zu want %zu",
        tag, h.col.frames.size(), e.frames.size());
  for (size_t i = 0; i < e.frames.size() && i < h.col.frames.size(); ++i) {
    bool eq = memcmp(h.col.frames[i].b, e.frames[i].b, 56) == 0;
    CHECK(eq, "%s: frame %zu bytes differ", tag, i);
    if (!eq) {
      for (int k = 0; k < 56; ++k)
        if (h.col.frames[i].b[k] != e.frames[i].b[k])
          printf("    byte %d got %02x want %02x\n", k,
                 h.col.frames[i].b[k], e.frames[i].b[k]);
    }
  }
  CHECK(h.col.tops.size() == e.tops.size(), "%s: timer ops got %zu want %zu",
        tag, h.col.tops.size(), e.tops.size());
  for (size_t i = 0; i < e.tops.size() && i < h.col.tops.size(); ++i) {
    const ATop& g = h.col.tops[i];
    CHECK(g.cancel == e.tops[i].cancel && g.slot == TMR_BASE + sink &&
          g.owner == OWNER_BASE + sink &&
          (g.cancel || g.deadline == e.tops[i].deadline),
          "%s: timer op %zu got {c%d s%u o%u %u} want {c%d s%u o%u %u}", tag,
          i, g.cancel, g.slot, g.owner, g.deadline, e.tops[i].cancel,
          unsigned(TMR_BASE + sink), unsigned(OWNER_BASE + sink),
          e.tops[i].deadline);
  }
  CHECK(h.col.settle == e.settle && h.col.teardown == e.teardown &&
        h.col.disc_arm == e.disc_arm && h.col.disc_disarm == e.disc_disarm &&
        h.col.nvm == e.nvm && (!e.nvm || h.col.nvm_set == e.nvm_set),
        "%s: strobes got st%d td%d da%d dd%d nv%d/%d want st%d td%d da%d dd%d nv%d/%d",
        tag, h.col.settle, h.col.teardown, h.col.disc_arm, h.col.disc_disarm,
        h.col.nvm, h.col.nvm_set, e.settle, e.teardown, e.disc_arm,
        e.disc_disarm, e.nvm, e.nvm_set);
  if (e.settle)
    CHECK(h.col.settle_sid == e.settle_sid && h.col.settle_da == e.settle_da
          && h.col.settle_vlan == e.settle_vlan,
          "%s: settle payload got %016lx/%012lx/%u", tag,
          (unsigned long)h.col.settle_sid, (unsigned long)h.col.settle_da,
          h.col.settle_vlan);
  if (e.disc_arm)
    CHECK(h.col.disc_eid == e.disc_eid, "%s: disc payload got %016lx want %016lx",
          tag, (unsigned long)h.col.disc_eid, (unsigned long)e.disc_eid);
  CHECK(h.col.notifies == e.notify, "%s: notify got %d want %d", tag,
        h.col.notifies, e.notify);
  CHECK(h.col.frees == e.frees, "%s: rx frees got %d want %d", tag,
        h.col.frees, e.frees);
}

// ==========================================================================
int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* d = new VKL_acmp_listener;
  Harness h(d);
  Model m;

  // ---- reset + init sweep -------------------------------------------------
  d->rst_n = 0; d->txn_valid_i = 0; d->evt_tk_valid_i = 0;
  d->pre_valid_i = 0; d->lock_held_i = 0; d->lock_ctlr_i = 0;
  d->entity_id_i = OUR_EID;
  for (int i = 0; i < 4; ++i) h.tick();
  d->rst_n = 1;
  for (int i = 0; i < 12; ++i) h.tick();
  CHECK(d->txn_ready_o == 1, "idle after reset + init sweep");
  {
    bool allz = true;
    for (int i = 0; i < N_SINKS; ++i) allz &= (h.shadow[i] == Rec());
    CHECK(allz, "init sweep zeroed all %d records", N_SINKS);
  }
  h.col.clear();

  // ---- helpers over harness + model in lock-step --------------------------
  auto step = [&](int sink, const Stim& s, bool check, const char* tag) {
    h.col.clear();
    Exp e = m.predict(sink, s);
    bool done = h.drive(sink, s);
    CHECK(done, "%s: completes", tag);
    if (check) {
      chk_cell(tag, h, sink, e);
      chk_rec(tag, h.shadow[sink], m.rec[sink]);
    } else {
      CHECK(h.shadow[sink].sm == m.rec[sink].sm,
            "%s(setup): state sync got %s want %s", tag,
            SN[h.shadow[sink].sm & 7], SN[m.rec[sink].sm & 7]);
    }
    return e;
  };
  auto S_bind = [&](uint64_t tk, uint16_t tu, uint64_t ct, bool sw) {
    Stim s; s.k = Stim::TXN; s.msg = M_BIND; s.tk_eid = tk; s.tk_uid = tu;
    s.ctlr = ct; s.flags = sw ? 0x0008 : 0; s.seq = 0x4100; return s;
  };
  auto S_unbind = [&]() {
    Stim s; s.k = Stim::TXN; s.msg = M_UNBIND; s.ctlr = CTL1;
    s.tk_eid = TK_A; s.tk_uid = TKUID_A; s.seq = 0x4200; return s;
  };
  auto S_getrx = [&]() {
    Stim s; s.k = Stim::TXN; s.msg = M_GETRX; s.ctlr = CTL2; s.seq = 0x4300;
    return s;
  };
  auto S_probe = [&](int sink, uint8_t status) {
    const Rec& r = m.rec[sink];
    Stim s; s.k = Stim::TXN; s.msg = M_PROBE_RESP; s.status = status;
    s.ctlr = r.bind_ctlr; s.tk_eid = r.talker_eid; s.tk_uid = r.talker_uid;
    s.seq = r.probe_seq; s.sid = 0x5544332211002233ull + uint64_t(sink);
    s.da = 0x91E0F0004455ull; s.vlan = 2;
    return s;
  };
  auto S_exp = [&]() { Stim s; s.k = Stim::EXP; return s; };
  auto S_tk = [&](uint8_t kind, bool fail = false) {
    Stim s; s.k = Stim::TK; s.tk_kind = kind; s.tk_fail = fail; return s;
  };

  auto goto_state = [&](int sink, int st, const char* tag) {
    Stim b = S_bind(TK_A, TKUID_A, CTL1, false); b.uid = uint16_t(sink);
    Stim u = S_unbind(); u.uid = uint16_t(sink);
    step(sink, u, false, tag);                    // any state -> UNB
    if (st == S_UNB) return;
    step(sink, b, false, tag);                    // UNB -> PWR
    if (st == S_PWR) return;
    Stim s;
    switch (st) {
      case S_PWA:
        s = S_tk(1); step(sink, s, false, tag); break;      // DEP -> PWA
      case S_PWD:
        s = S_tk(1); step(sink, s, false, tag);
        s = S_tk(0); step(sink, s, false, tag); break;      // DISC -> PWD
      case S_PW2:
        step(sink, S_exp(), false, tag); break;             // CMD -> PW2
      case S_PWT: {
        Stim p = S_probe(sink, ST_NOBW); p.uid = uint16_t(sink);
        step(sink, p, false, tag); break;                   // fail -> PWT
      }
      case S_SNR: case S_SOK: {
        Stim p = S_probe(sink, ST_OK); p.uid = uint16_t(sink);
        step(sink, p, false, tag);                          // ok -> SNR
        if (st == S_SOK) { s = S_tk(2); step(sink, s, false, tag); }
        break;
      }
      default: break;
    }
  };

  // ---- park sink 7 in a settled state for the isolation check -------------
  goto_state(7, S_SOK, "park7");
  Rec park7 = m.rec[7];

  // ======================= THE MTXW WALK ==================================
  // every (event row x state) cell of F05.3; aliased timer '—' cells are
  // proven impossible-by-construction (own-T-ID deadline check) instead of
  // injected — the shared SM slot cannot carry the foreign T-ID.
  static const int OWN_MS[8] = {-1, -1, DRAWVAL, T_CMD, T_CMD, T_RETRY,
                                T_NOTK, -1};
  int cells = 0;
  for (int row = 0; row < 14; ++row) {
    for (int st = 0; st < 8; ++st) {
      char tag[64];
      snprintf(tag, sizeof tag, "F05.3 %s x %s", RN[row], SN[st]);
      int sink = (row * 8 + st) % 7;   // sink 7 stays parked
      goto_state(sink, st, tag);
      ++cells;

      if (row >= R_TMR_DELAY && row <= R_TMR_NOTK) {
        bool own = (row == R_TMR_DELAY && st == S_PWD) ||
                   (row == R_TMR_CMD && (st == S_PWR || st == S_PW2)) ||
                   (row == R_TMR_RETRY && st == S_PWT) ||
                   (row == R_TMR_NOTK && st == S_SNR);
        if (own) {
          step(sink, S_exp(), true, tag);
        } else if (OWN_MS[st] < 0) {
          // no SM timer armed here: inject a spurious expiry, prove inert
          CHECK(M[row][st].t == C_DASH, "%s: model marks the cell em-dash",
                tag);
          step(sink, S_exp(), true, tag);
        } else {
          // aliased '—': the armed deadline is the state's own T-ID, so
          // this row's expiry cannot exist in this state
          CHECK(M[row][st].t == C_DASH, "%s: model marks the cell em-dash",
                tag);
          CHECK(h.last_arm_deadline[sink] == NOW + uint32_t(OWN_MS[st]),
                "%s: armed deadline %u proves the slot carries the state's "
                "own T-ID (+%d)", tag, h.last_arm_deadline[sink], OWN_MS[st]);
        }
        continue;
      }

      Stim s;
      switch (row) {
        case R_BIND_SAME:
          // craft 'same' from the model record; in UNB nothing is bound and
          // the classifier MUST take the new-source row (the '—' proof)
          s = S_bind(m.rec[sink].talker_eid, m.rec[sink].talker_uid, CTL2,
                     true);
          s.uid = uint16_t(sink);
          {
            Exp e = step(sink, s, true, tag);
            if (st == S_UNB)
              CHECK(e.row == R_BIND_NEW,
                    "%s: unreachable cell — classified as BIND_NEW", tag);
          }
          break;
        case R_BIND_NEW:
          s = S_bind(TK_B, TKUID_B, CTL1, false); s.uid = uint16_t(sink);
          step(sink, s, true, tag);
          break;
        case R_UNBIND:
          s = S_unbind(); s.uid = uint16_t(sink);
          step(sink, s, true, tag);
          break;
        case R_GETRX:
          s = S_getrx(); s.uid = uint16_t(sink);
          step(sink, s, true, tag);
          break;
        case R_PROBE_OK:
          s = S_probe(sink, ST_OK); s.uid = uint16_t(sink);
          step(sink, s, true, tag);
          break;
        case R_PROBE_FAIL:
          s = S_probe(sink, ST_NOBW); s.uid = uint16_t(sink);
          step(sink, s, true, tag);
          break;
        case R_TK_DISC: case R_TK_DEP: case R_TK_UNREG:
          s = S_tk(uint8_t(row - R_TK_DISC));
          step(sink, s, true, tag);
          break;
        case R_TK_REG:
          // Failed attr on purpose: at SOK this is the REF-BUG guard cell
          s = S_tk(2, st == S_SOK);
          step(sink, s, true, tag);
          break;
        default: break;
      }
    }
  }
  CHECK(cells == 112, "MTXW walked %d cells (want 112)", cells);

  // ================== behavior checks beyond the walk =====================
  int sink = 0;
  char tg[64];

  // B1: exact-duplicate probe — A13 bytes == A5 bytes, same seq, retried
  goto_state(sink, S_PWR, "B1");
  Pdu probe1 = h.col.frames.empty() ? Pdu{} : h.col.frames.back();
  {
    // re-collect probe #1 from a fresh bind for byte identity
    goto_state(sink, S_UNB, "B1");
    Stim b = S_bind(TK_A, TKUID_A, CTL1, false); b.uid = uint16_t(sink);
    h.col.clear();
    Exp e = m.predict(sink, b);
    CHECK(h.drive(sink, b), "B1: bind completes");
    CHECK(h.col.frames.size() == 2, "B1: bind resp + probe #1");
    if (h.col.frames.size() == 2) probe1 = h.col.frames[1];
    (void)e;
    uint16_t seq1 = m.rec[sink].probe_seq;
    step(sink, S_exp(), true, "B1 dup");           // PWR -CMD-> PW2 (A13)
    CHECK(!h.col.frames.empty() &&
          memcmp(h.col.frames[0].b, probe1.b, 56) == 0,
          "B1: probe #2 is the exact duplicate of probe #1");
    CHECK(m.rec[sink].probe_seq == seq1 && h.shadow[sink].probe_seq == seq1,
          "B1: same probe_seq %u", seq1);
    CHECK(h.shadow[sink].retried, "B1: retried flag set");
  }

  // B2: double timeout -> PWT with acmpsta 7, T-ACMP-RETRY armed
  step(sink, S_exp(), true, "B2 dbl-timeout");     // PW2 -CMD-> PWT A14(=7)
  CHECK(h.shadow[sink].sm == S_PWT && h.shadow[sink].acmpsta == ST_TT,
        "B2: PWT with acmpsta LISTENER_TALKER_TIMEOUT got %s/%u",
        SN[h.shadow[sink].sm & 7], h.shadow[sink].acmpsta);

  // B3: dagger dual arms
  //   PWT + tk_disc: T-ACMP-RETRY -> PWD / A12 (walk covered the !tk arm)
  { Stim s = S_tk(0); step(sink, s, false, "B3"); }  // ign cell sets tk_disc
  step(sink, S_exp(), true, "B3 RETRY tk arm");
  CHECK(h.shadow[sink].sm == S_PWD, "B3: RETRY with talker -> PWD got %s",
        SN[h.shadow[sink].sm & 7]);
  //   SNR + tk: NOTK -> A8 + PWD / A12
  goto_state(sink, S_SNR, "B3b");
  { Stim s = S_tk(0); step(sink, s, false, "B3b"); }
  step(sink, S_exp(), true, "B3b NOTK tk arm");
  CHECK(h.shadow[sink].sm == S_PWD, "B3b: NOTK with talker -> PWD got %s",
        SN[h.shadow[sink].sm & 7]);
  //   SOK + tk: UNREG -> A8 + PWD / A12
  goto_state(sink, S_SOK, "B3c");
  { Stim s = S_tk(0); step(sink, s, false, "B3c"); }
  { Stim s = S_tk(3); step(sink, s, true, "B3c UNREG tk arm"); }
  CHECK(h.shadow[sink].sm == S_PWD, "B3c: UNREG with talker -> PWD got %s",
        SN[h.shadow[sink].sm & 7]);

  // B4: REF-BUG guard — TalkerFailed rise in SETTLED_RSV_OK is inert
  goto_state(sink, S_SOK, "B4");
  { Stim s = S_tk(2, true); step(sink, s, true, "B4 REF-BUG"); }
  CHECK(h.shadow[sink].sm == S_SOK && !((h.shadow[sink].srp_decl >> 1) & 1),
        "B4: no invented arc, no RF latch from the spurious rise");

  // B5: REGISTERING_FAILED visible — settle then register a Failed attr
  goto_state(sink, S_SNR, "B5");
  { Stim s = S_tk(2, true); step(sink, s, true, "B5 reg-failed"); }
  { Stim g = S_getrx(); g.uid = uint16_t(sink); h.col.clear();
    Exp e = m.predict(sink, g);
    CHECK(h.drive(sink, g), "B5: GET_RX_STATE completes");
    CHECK(!h.col.frames.empty() &&
          (uint16_t(h.col.frames[0].b[50]) << 8 | h.col.frames[0].b[51]) ==
          (0x0002 | 0x0040),
          "B5: flags carry REGISTERING_FAILED 0x0040");
    chk_cell("B5", h, sink, e);
  }

  // B6: LISTENER_UNKNOWN_ID for the three commands; probe resp dropped
  for (uint8_t mm : {uint8_t(M_BIND), uint8_t(M_UNBIND), uint8_t(M_GETRX)}) {
    snprintf(tg, sizeof tg, "B6 msg %u uid 8", mm);
    Stim s; s.k = Stim::TXN; s.msg = mm; s.uid = N_SINKS; s.ctlr = CTL1;
    s.tk_eid = TK_A; s.tk_uid = TKUID_A; s.seq = 0x5000;
    h.col.clear();
    Exp e = m.predict(0, s);
    CHECK(h.drive(0, s), "%s completes", tg);
    chk_cell(tg, h, 0, e);
    CHECK(!h.col.frames.empty() && (h.col.frames[0].b[2] >> 3) == ST_LUID,
          "%s: status LISTENER_UNKNOWN_ID", tg);
  }
  { Stim s; s.k = Stim::TXN; s.msg = M_PROBE_RESP; s.uid = N_SINKS;
    h.col.clear();
    Exp e = m.predict(0, s);
    CHECK(h.drive(0, s), "B6 probe uid 8 completes");
    chk_cell("B6 probe uid 8 (silent)", h, 0, e); }

  // B7: foreign listener EID and foreign protocol are consumed silently
  { Stim s = S_bind(TK_A, TKUID_A, CTL1, false); s.target = 0xDEAD0001;
    h.col.clear(); Exp e = m.predict(0, s);
    CHECK(h.drive(0, s), "B7 foreign EID completes");
    chk_cell("B7 foreign EID", h, 0, e); }
  { Stim s = S_bind(TK_A, TKUID_A, CTL1, false); s.protocol = 2;  // AEM
    h.col.clear(); Exp e = m.predict(0, s);
    CHECK(h.drive(0, s), "B7 foreign protocol completes");
    chk_cell("B7 foreign protocol", h, 0, e); }

  // B8: probe-response guard mismatch (wrong seq) silently ignored
  goto_state(sink, S_PWR, "B8");
  { Stim s = S_probe(sink, ST_OK); s.uid = uint16_t(sink);
    s.seq = uint16_t(s.seq + 1);
    h.col.clear(); Exp e = m.predict(sink, s);
    CHECK(h.drive(sink, s), "B8 completes");
    chk_cell("B8 guard mismatch", h, sink, e);
    CHECK(h.col.wrotes == 0 && h.col.frees == 1,
          "B8: no write-back, slot returned");
    CHECK(h.shadow[sink].sm == S_PWR, "B8: still PRB_W_RESP"); }

  // B9: A1 lock gate — BIND/UNBIND blocked for a foreign holder,
  // unaffected for the holder itself and for GET_RX_STATE
  d->lock_held_i = 1; d->lock_ctlr_i = CTL2;
  m.lock_held = true; m.lock_eid = CTL2;
  { Stim s = S_bind(TK_B, TKUID_B, CTL1, false); s.uid = uint16_t(sink);
    h.col.clear(); Exp e = m.predict(sink, s);
    CHECK(h.drive(sink, s), "B9 locked bind completes");
    chk_cell("B9 locked bind", h, sink, e);
    CHECK(!h.col.frames.empty() && (h.col.frames[0].b[2] >> 3) == ST_NOAUTH,
          "B9: CONTROLLER_NOT_AUTHORIZED");
    CHECK(h.shadow[sink].sm == S_PWR && h.col.wrotes == 0,
          "B9: cell aborted, no commit"); }
  { Stim s = S_unbind(); s.uid = uint16_t(sink);
    h.col.clear(); Exp e = m.predict(sink, s);
    CHECK(h.drive(sink, s), "B9 locked unbind completes");
    chk_cell("B9 locked unbind", h, sink, e); }
  { Stim g = S_getrx(); g.uid = uint16_t(sink); g.ctlr = CTL1;
    h.col.clear(); Exp e = m.predict(sink, g);
    CHECK(h.drive(sink, g), "B9 GET_RX_STATE under lock completes");
    chk_cell("B9 getrx under lock", h, sink, e);
    CHECK(!h.col.frames.empty() && (h.col.frames[0].b[2] >> 3) == ST_OK,
          "B9: GET_RX_STATE unaffected by the lock"); }
  { Stim s = S_bind(TK_B, TKUID_B, CTL2, true); s.uid = uint16_t(sink);
    h.col.clear(); Exp e = m.predict(sink, s);
    CHECK(h.drive(sink, s), "B9 holder bind completes");
    chk_cell("B9 holder bind", h, sink, e);
    CHECK(h.shadow[sink].talker_eid == TK_B,
          "B9: the lock holder's bind commits"); }
  d->lock_held_i = 0; m.lock_held = false;

  // B10: boot preload -> PRB_W_AVAIL with discovery armed (07 §5.3)
  goto_state(2, S_UNB, "B10");
  h.col.clear();
  d->pre_valid_i = 1; d->pre_sink_i = 2; d->pre_talker_eid_i = TK_A;
  d->pre_talker_uid_i = TKUID_A; d->pre_ctlr_eid_i = CTL1;
  d->pre_sw_i = 1; d->pre_started_i = 1;
  h.tick(); d->pre_valid_i = 0; h.tick(); h.wait_idle(); h.tick();
  {
    Rec& r = m.rec[2];
    r = Rec(); r.sm = S_PWA; r.pbsta = 1; r.bound = true; r.sw = true;
    r.started = true; r.talker_eid = TK_A; r.talker_uid = TKUID_A;
    r.bind_ctlr = CTL1; r.smh = uint8_t(TMR_BASE + 2);
    chk_rec("B10 preload", h.shadow[2], r);
    CHECK(h.col.disc_arm && h.col.disc_eid == TK_A,
          "B10: A4 discovery arm with the restored talker");
    CHECK(h.col.wrotes == 1 && h.col.notifies == 0,
          "B10: one record write, no notification at boot");
  }
  { Stim s = S_tk(0); step(2, s, true, "B10 disc after preload"); }
  CHECK(h.shadow[2].sm == S_PWD, "B10: preloaded sink probes on discovery");

  // B11: A11 swallows a stale pending expiry (cancel wins the race)
  goto_state(3, S_PWR, "B11");
  { Stim p = S_probe(3, ST_OK); p.uid = 3;
    h.col.clear();
    Exp e = m.predict(3, p);
    // expiry lands the same tick the settle txn arrives: txn wins dispatch,
    // A11 must clear the pend bit, and NO second work item may run
    int slot = h.next_rx_slot; h.next_rx_slot = (h.next_rx_slot + 1) % 4;
    Pdu pd = mk_pdu(p.msg, p.status, p.sid, p.ctlr, p.tk_eid, p.target,
                    p.tk_uid, p.uid, p.da, 0, p.seq, p.flags, p.vlan);
    memcpy(h.rxmem[slot], pd.b, 56);
    uint32_t* w = &d->txn_i[0];
    for (int i = 0; i < 13; ++i) w[i] = 0;
    wput(w, 354, 3, 1); wput(w, 350, 4, p.msg); wput(w, 345, 5, p.status);
    wput(w, 222, 64, p.ctlr); wput(w, 158, 64, p.target);
    wput(w, 142, 16, p.seq); wput(w, 60, 16, p.uid);
    wput(w, 57, 3, uint64_t(slot)); wput(w, 0, 2, 1);
    d->txn_valid_i = 1;
    h.inj_exp = true; h.inj_exp_sink = 3;
    h.tick();
    d->txn_valid_i = 0;
    h.tick(); h.wait_idle(); h.tick();
    chk_cell("B11 settle beats stale expiry", h, 3, e);
    // the swallowed expiry must never surface: a long quiet window with
    // ZERO activity (a second work item would need ~24 cycles to retire)
    h.col.clear();
    for (int i = 0; i < 60; ++i) h.tick();
    CHECK(h.col.wrotes == 0 && !h.col.teardown && h.col.tops.empty() &&
          h.col.notifies == 0 && h.shadow[3].sm == S_SNR,
          "B11: no second work item; still SETTLED_NO_RSV (got sm %s, "
          "wr %d td %d tops %zu nf %d)", SN[h.shadow[3].sm & 7],
          h.col.wrotes, h.col.teardown, h.col.tops.size(),
          h.col.notifies); }

  // B12: cross-sink isolation — the parked sink was never touched
  CHECK(h.shadow[7] == park7 && m.rec[7] == park7,
        "B12: parked sink 7 record untouched through the whole walk");

  h.wait_idle();
  CHECK(d->txn_ready_o == 1, "idle at the end");

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete d;
  return fails ? 1 : 0;
}
