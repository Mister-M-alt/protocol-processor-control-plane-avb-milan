// SPDX-License-Identifier: CERN-OHL-W-2.0
// pp_top suite — END-TO-END wire truth through the WHOLE processor top:
// one MAC byte stream in, one MAC byte stream out. Every expectation is an
// independent C++ frame builder/parser working from the doc byte offsets
// (F04.5 ADPDU, F05.13 Milan ACMPDU, 802.1Q §10.8/§35.2.2 MRPDUs) — never
// DUT logic. Scenarios: boot restore over a blank NVM device; entity enable
// -> byte-exact 82 B ADPDU inside the T-ADP-DELAY-START window + the 5 s
// re-advertise cadence with available_index++; ENTITY_DISCOVER -> delayed
// byte-exact response; BIND_RX -> byte-exact BIND_RX_RESPONSE, talker
// ENTITY_AVAILABLE -> discovery -> byte-exact PROBE_TX_COMMAND (trace ring
// + status snapshot cross-checked); DECLARE_TALKER (svc) -> Σ-slope
// admission (independent Milan §4.3.3.2 model) -> byte-exact Talker
// Advertise + MVRP VID New; TX interleave of ADP + ACMP + SRP frames, each
// byte-exact, no truncation; certified two-class Domain adoption -> Lv+New
// re-declaration; listener READY end-to-end; NVM debounce commit of the
// captured binding (F07.8 framing spot-checked at the device face); and the
// maap face both ways — with no allocator the talker still answers (the
// walker must not wedge on an unaccepted request), with one the granted
// address reaches acmp_declaring_o, the ACMP answer and the SRP wire.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>
#include "Vpp_top_wrap.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// ---------------------------------------------------------------------------
// TB shape
// ---------------------------------------------------------------------------
static const int      MS_CYC = 100;                  // wrap: 1 ms = 100 clk
//! the wrap's compile-time memory map (07 §3.3 image, 03 §7 response buffer)
static const uint32_t DESC_BASE  = 0x20000000u;
static const uint32_t RESP_BASE  = 0x20100000u;
static const uint32_t RESP_BYTES = 592u;             // 16 + DESC_LINE_BYTES_P
static const uint64_t OWN_MAC = 0x0A0B0C0D0E0FULL;
static const uint64_t EID     = 0x123456789ABCDEF0ULL;
static const uint64_t EMID    = 0x00E0DECAFB0B0001ULL;
static const uint16_t TKSRC = 0x0008, TKCAP = 0x6001;
static const uint16_t LSNK  = 0x0008, LSCAP = 0x4801;
static const uint16_t CFGIX = 0x0000, IDIX = 0x0005;
static const uint64_t GM0  = 0xA1A2A3A4A5A6A7A8ULL;
static const uint8_t  DOM0 = 0x00;
static const uint32_t RATE    = 100000000u;          // 100 Mb/s port
static const uint64_t LIMIT   = 75000000ull;         // 75 % ceiling
static const uint32_t ACC_LAT = 0x000186A0u;
static const uint64_t CTLR_MAC  = 0x0202DEADBEEFULL;
static const uint64_t CTLR_EID  = 0x7777000000000042ULL;
static const uint64_t CTLR2_EID = 0x7777000000000043ULL;
static const uint64_t T1_EID  = 0xAAAA00000000AAA1ULL;
static const uint64_t T1_MAC  = 0x020200000AA1ULL;
static const uint16_t T1_UID  = 3;
static const uint64_t ACMP_MC = 0x91E0F0010000ULL;
static const uint64_t MSRP_DA = 0x0180C200000EULL;
static const uint64_t MVRP_DA = 0x0180C2000021ULL;

// MRP attribute events (802.1Q §35.2.2.7.2) + Listener declarations
enum { EV_NEW = 0, EV_JOININ = 1, EV_IN = 2, EV_JOINMT = 3, EV_MT = 4,
       EV_LV = 5 };
enum { DECL_IGNORE = 0, DECL_ASKFAIL = 1, DECL_READY = 2, DECL_READYFAIL = 3 };
// svc face ops / status (KL_srp_top class-B contract)
enum { OP_DECL_TK = 0, OP_WDRW_TK = 1, OP_DECL_LS = 2, OP_WDRW_LS = 3,
       OP_GET_DOM = 4 };
enum { ST_OK = 0, ST_FAIL = 1, ST_UNSUP = 2 };

// ---------------------------------------------------------------------------
// byte helpers
// ---------------------------------------------------------------------------
static void putbe(uint8_t* p, uint64_t v, int n) {
  for (int i = 0; i < n; ++i) p[i] = uint8_t(v >> (8 * (n - 1 - i)));
}
static void put16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(v >> 8); b.push_back(v & 0xFF);
}
static void put_mac(std::vector<uint8_t>& b, uint64_t m) {
  for (int i = 5; i >= 0; i--) b.push_back((m >> (8 * i)) & 0xFF);
}

// ---------------------------------------------------------------------------
// independent F04.5 ADPDU model (doc byte offsets)
// ---------------------------------------------------------------------------
static std::vector<uint8_t> adp_frame(uint8_t msg, uint64_t src_mac,
                                      uint64_t eid, uint8_t vt,
                                      uint32_t aidx, uint64_t gm,
                                      uint8_t dom, uint64_t emid,
                                      uint16_t tksrc, uint16_t tkcap,
                                      uint16_t lsnk, uint16_t lscap,
                                      uint32_t ecaps, uint16_t cfg,
                                      uint16_t idix) {
  std::vector<uint8_t> f(82, 0);
  putbe(&f[0], ACMP_MC, 6);
  putbe(&f[6], src_mac, 6);
  putbe(&f[12], 0x22F0, 2);
  f[14] = 0xFA;
  f[15] = msg & 0x0F;
  f[16] = uint8_t((vt << 3) | 0);
  f[17] = 56;
  putbe(&f[18], eid, 8);
  putbe(&f[26], emid, 8);
  putbe(&f[34], ecaps, 4);
  putbe(&f[38], tksrc, 2);
  putbe(&f[40], tkcap, 2);
  putbe(&f[42], lsnk, 2);
  putbe(&f[44], lscap, 2);
  putbe(&f[46], 0, 4);
  putbe(&f[50], aidx, 4);
  putbe(&f[54], gm, 8);
  f[62] = dom;
  putbe(&f[64], cfg, 2);
  putbe(&f[66], idix, 2);
  return f;
}
// our own expected AVAILABLE (F04.6 entity_capabilities = 0x0000C588)
static std::vector<uint8_t> own_avail(uint32_t aidx) {
  return adp_frame(0, OWN_MAC, EID, 10, aidx, GM0, DOM0, EMID,
                   TKSRC, TKCAP, LSNK, LSCAP, 0x0000C588u, CFGIX, IDIX);
}

// ---------------------------------------------------------------------------
// independent F05.13 Milan ACMPDU model (56 B PDU + Ethernet header)
// ---------------------------------------------------------------------------
static std::vector<uint8_t> acmp_frame(uint64_t src_mac, uint8_t msg,
                                       uint8_t status, uint64_t sid,
                                       uint64_t ctlr, uint64_t tkeid,
                                       uint64_t lseid, uint16_t tkuid,
                                       uint16_t luid, uint64_t da,
                                       uint16_t cc, uint16_t seq,
                                       uint16_t flags, uint16_t vlan) {
  std::vector<uint8_t> f(70, 0);
  putbe(&f[0], ACMP_MC, 6);
  putbe(&f[6], src_mac, 6);
  putbe(&f[12], 0x22F0, 2);
  f[14] = 0xFC;
  f[15] = msg & 0x0F;
  f[16] = uint8_t((status << 3) | 0);   // cdl = 44 -> [10:8] = 0
  f[17] = 44;
  putbe(&f[18], sid, 8);
  putbe(&f[26], ctlr, 8);
  putbe(&f[34], tkeid, 8);
  putbe(&f[42], lseid, 8);
  putbe(&f[50], tkuid, 2);
  putbe(&f[52], luid, 2);
  putbe(&f[54], da, 6);
  putbe(&f[60], cc, 2);
  putbe(&f[62], seq, 2);
  putbe(&f[64], flags, 2);
  putbe(&f[66], vlan, 2);
  return f;
}

// ---------------------------------------------------------------------------
// independent Σ-slope admission model (Milan v1.2 §4.3.3.2)
// ---------------------------------------------------------------------------
static uint64_t slope_bps(uint32_t mfs, uint32_t mif) {
  uint64_t f = (uint64_t)mfs + 22;
  if (f < 68) f = 68;
  return (f + 20) * mif * 8000ull * 8ull;
}

// ---------------------------------------------------------------------------
// independent MRPDU builder/parser (802.1Q §10.8.1.2 BNF, §35.2.2)
// ---------------------------------------------------------------------------
struct Vec {
  bool la = false;
  int nov = 1;
  std::vector<uint8_t> fv;
  std::vector<int> ev;
  std::vector<int> fp;
};
struct Msg { int type; int alen; bool listener; std::vector<Vec> vecs; };

static std::vector<uint8_t> vec_bytes(const Vec& v, bool listener) {
  std::vector<uint8_t> b;
  b.push_back((v.la ? 0x20 : 0x00) | ((v.nov >> 8) & 0x1F));
  b.push_back(v.nov & 0xFF);
  b.insert(b.end(), v.fv.begin(), v.fv.end());
  for (int i = 0; i < v.nov; i += 3) {
    int e[3] = {0, 0, 0};
    for (int j = 0; j < 3 && i + j < v.nov; j++) e[j] = v.ev[i + j];
    b.push_back((uint8_t)(((e[0] * 6) + e[1]) * 6 + e[2]));
  }
  if (listener) {
    for (int i = 0; i < v.nov; i += 4) {
      int p[4] = {0, 0, 0, 0};
      for (int j = 0; j < 4 && i + j < v.nov; j++) p[j] = v.fp[i + j];
      b.push_back((uint8_t)(p[0] * 64 + p[1] * 16 + p[2] * 4 + p[3]));
    }
  }
  return b;
}
static std::vector<uint8_t> mrpdu_frame(bool msrp, uint64_t src_mac,
                                        const std::vector<Msg>& ms) {
  std::vector<uint8_t> b;
  put_mac(b, msrp ? MSRP_DA : MVRP_DA);
  put_mac(b, src_mac);
  put16(b, msrp ? 0x22EA : 0x88F5);
  b.push_back(0x00);                    // ProtocolVersion
  for (const Msg& m : ms) {
    b.push_back((uint8_t)m.type);
    b.push_back((uint8_t)m.alen);
    std::vector<uint8_t> body;
    for (const Vec& v : m.vecs) {
      auto vb = vec_bytes(v, m.listener);
      body.insert(body.end(), vb.begin(), vb.end());
    }
    if (msrp) put16(b, (uint16_t)(body.size() + 2));
    b.insert(b.end(), body.begin(), body.end());
    put16(b, 0x0000);
  }
  put16(b, 0x0000);
  return b;
}
static std::vector<uint8_t> fv_talker(uint64_t sid, uint64_t da, uint16_t vid,
                                      uint16_t mfs, uint16_t mif, int prio,
                                      int rank, uint32_t lat) {
  std::vector<uint8_t> b;
  for (int i = 7; i >= 0; i--) b.push_back((sid >> (8 * i)) & 0xFF);
  put_mac(b, da);
  put16(b, vid); put16(b, mfs); put16(b, mif);
  b.push_back((uint8_t)((prio << 5) | (rank << 4)));
  for (int i = 3; i >= 0; i--) b.push_back((lat >> (8 * i)) & 0xFF);
  return b;
}
static std::vector<uint8_t> fv_sid(uint64_t sid) {
  std::vector<uint8_t> b;
  for (int i = 7; i >= 0; i--) b.push_back((sid >> (8 * i)) & 0xFF);
  return b;
}
static std::vector<uint8_t> fv_domain(uint8_t cid, uint8_t prio,
                                      uint16_t vid) {
  std::vector<uint8_t> b{cid, prio};
  put16(b, vid);
  return b;
}
static std::vector<uint8_t> fv_vid(uint16_t vid) {
  std::vector<uint8_t> b;
  put16(b, vid);
  return b;
}

// structure-walking parser for set-style checks
struct PVec { int type; bool la; int nov; std::vector<uint8_t> fv;
              std::vector<int> ev; };
struct PFrame { bool ok = false; bool msrp = false; std::vector<PVec> vecs; };
static PFrame parse_mrpdu(const std::vector<uint8_t>& f) {
  PFrame r;
  if (f.size() < 17) return r;
  uint16_t et = (f[12] << 8) | f[13];
  if (et == 0x22EA) r.msrp = true;
  else if (et == 0x88F5) r.msrp = false;
  else return r;
  size_t i = 14;
  if (f[i++] != 0x00) return r;
  while (i + 1 < f.size()) {
    if (f[i] == 0x00 && f[i + 1] == 0x00) { i += 2; r.ok = true; break; }
    int type = f[i++];
    if (i >= f.size()) return r;
    int alen = f[i++];
    if (r.msrp) { if (i + 2 > f.size()) return r; i += 2; }
    for (;;) {
      if (i + 2 > f.size()) return r;
      if (f[i] == 0x00 && f[i + 1] == 0x00) { i += 2; break; }
      PVec v; v.type = type;
      v.la  = (f[i] & 0xE0) != 0;
      v.nov = ((f[i] & 0x1F) << 8) | f[i + 1];
      i += 2;
      if (i + (size_t)alen > f.size()) return r;
      v.fv.assign(f.begin() + i, f.begin() + i + alen);
      i += alen;
      int n3 = (v.nov + 2) / 3;
      if (i + (size_t)n3 > f.size()) return r;
      for (int k = 0; k < n3; k++) {
        int b = f[i + k];
        v.ev.push_back(b / 36); v.ev.push_back((b / 6) % 6);
        v.ev.push_back(b % 6);
      }
      i += n3;
      if (r.msrp && type == 3) i += (v.nov + 3) / 4;
      if (i > f.size()) return r;
      v.ev.resize(v.nov);
      r.vecs.push_back(v);
    }
  }
  return r;
}
static uint64_t fv_u64(const std::vector<uint8_t>& fv, int off, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v = (v << 8) | fv[off + i];
  return v;
}
static bool frame_has(const std::vector<uint8_t>& f, bool msrp, int type,
                      uint64_t key, int ev) {
  auto p = parse_mrpdu(f);
  if (p.msrp != msrp) return false;
  for (auto& v : p.vecs) {
    if (v.type != type) continue;
    uint64_t k = 0;
    if (!msrp)          k = fv_u64(v.fv, 0, 2);
    else if (type == 4) k = v.fv[0];
    else                k = fv_u64(v.fv, 0, 8);
    if (k == key && !v.ev.empty() && v.ev[0] == ev) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// independent IEEE 1722.1-2021 §9.3.1/§7.4 AECPDU model + the 07 §3.3 image
// ---------------------------------------------------------------------------
// cdl is the offset-from-@12 length this architecture uses throughout (F06.14
// "GET_COUNTERS 160 B, cdl 148"), so an AEM PDU of 24 + payload has
// cdl = 12 + payload.
static const uint16_t AEM_READ_DESCRIPTOR = 0x0004;
//! the NOT_IMPLEMENTED probe. It used to be GET_SAMPLING_RATE, which stopped
//! being unimplemented when Milan §5.4.2.14 landed; WRITE_DESCRIPTOR is the
//! durable replacement, because the descriptor store is read-only at run time
//! by construction (KL_aecp_desc_store: "A write to any region other than the
//! name table is DROPPED") and Milan requires no such command. If a future
//! round ever implements it, MOVE THIS PROBE - do not weaken the check.
static const uint16_t AEM_WRITE_DESCRIPTOR = 0x0005;
static const uint16_t AEM_ENTITY_AVAILABLE = 0x0002;
static const uint16_t AEM_GET_CONFIGURATION = 0x0007;
static const uint16_t AEM_GET_STREAM_FORMAT = 0x0009;
static const uint16_t AEM_GET_SAMPLING_RATE = 0x0015;
static const uint16_t AEM_GET_CLOCK_SOURCE = 0x0017;
static const uint16_t AEM_SET_SAMPLING_RATE = 0x0014;
static const uint16_t AEM_SET_CLOCK_SOURCE = 0x0016;
static const uint16_t AEM_SET_CONTROL = 0x0018;
static const uint16_t AEM_GET_CONTROL = 0x0019;
static const uint16_t AEM_SET_CONFIGURATION = 0x0006;
enum { AECP_STREAM_IS_RUNNING = 12, AECP_ENTITY_LOCKED = 3 };
static const uint16_t AEM_IDENTIFY_NOTIF  = 0x0026;
static const uint16_t AEM_GET_COUNTERS    = 0x0029;
static const uint16_t AEM_GET_AUDIO_MAP   = 0x002B;
static const uint16_t AEM_GET_DYNAMIC_INFO = 0x004B;
enum { AECP_SUCCESS = 0, AECP_NOT_IMPLEMENTED = 1, AECP_NO_SUCH_DESCRIPTOR = 2,
       AECP_BAD_ARGUMENTS = 7, AECP_NOT_SUPPORTED = 11 };

static std::vector<uint8_t> aecp_frame(uint64_t da, uint64_t sa,
                                       uint8_t msg_type, uint8_t status,
                                       uint64_t target_eid, uint64_t ctlr_eid,
                                       uint16_t seq, uint16_t cmd_type,
                                       const std::vector<uint8_t>& payload,
                                       bool pad60 = true) {
  size_t n = 38 + payload.size();
  std::vector<uint8_t> f(n, 0);
  putbe(&f[0], da, 6);
  putbe(&f[6], sa, 6);
  putbe(&f[12], 0x22F0, 2);
  f[14] = 0xFB;                                   // AVTP subtype AECP
  f[15] = uint8_t(msg_type & 0x0F);               // sv = 0, version = 0
  uint16_t cdl = uint16_t(12 + payload.size());
  f[16] = uint8_t(((status & 0x1F) << 3) | ((cdl >> 8) & 0x07));
  f[17] = uint8_t(cdl & 0xFF);
  putbe(&f[18], target_eid, 8);
  putbe(&f[26], ctlr_eid, 8);
  putbe(&f[34], seq, 2);
  // @22..@23 IS NOT ALWAYS A command_type. This masked bit 15 unconditionally
  // with the comment "u = 0", which is right for an AEM AECPDU (1722.1-2021
  // 9.3.2.1 makes `u` a 1-bit field ahead of `cr` and a 14-bit
  // command_type) and wrong for every other
  // message_type. 9.6.2 Figure 9-12 gives a VENDOR_UNIQUE AECPDU a 48-bit
  // protocol_id at @22..@27 with no u bit in it, and 9.4 gives an
  // ADDRESS_ACCESS AECPDU a tlv_count there. Masking regardless meant the
  // harness could not put an OUI with bit 7 set ONTO the wire at all, so no
  // check written with it could ever grade what the DUT did with one - a
  // testbench that quietly agreed with the DUT about a byte neither of them
  // was allowed to disagree on.
  const bool aem_like = (msg_type == 0) || (msg_type == 1);
  putbe(&f[36], aem_like ? (uint16_t)(cmd_type & 0x7FFF) : cmd_type, 2);
  for (size_t i = 0; i < payload.size(); ++i) f[38 + i] = payload[i];
  if (pad60 && f.size() < 60) f.resize(60, 0);
  return f;
}

// ---- descriptor bodies, straight from the IEEE §7.2 field offsets ----------
static std::vector<uint8_t> entity_descriptor() {
  std::vector<uint8_t> d(312, 0);                 // §7.2.1, 07 §3.2 = 312 B
  putbe(&d[0],   0x0000, 2);                      // descriptor_type
  putbe(&d[2],   0x0000, 2);                      // descriptor_index
  putbe(&d[4],   EID, 8);                         // entity_id
  putbe(&d[12],  EMID, 8);                        // entity_model_id
  putbe(&d[20],  0x0000C588u, 4);                 // entity_capabilities
  putbe(&d[24],  TKSRC, 2);                       // talker_stream_sources
  putbe(&d[26],  TKCAP, 2);                       // talker_capabilities
  putbe(&d[28],  LSNK, 2);                        // listener_stream_sinks
  putbe(&d[30],  LSCAP, 2);                       // listener_capabilities
  putbe(&d[32],  0, 4);                           // controller_capabilities
  putbe(&d[36],  0, 4);                           // available_index
  putbe(&d[40],  0, 8);                           // association_id
  const char* nm = "PP Reference Entity";         // entity_name @48, 64 B
  memcpy(&d[48], nm, strlen(nm));
  putbe(&d[112], 0, 2);                           // vendor_name_string
  putbe(&d[114], 1, 2);                           // model_name_string
  const char* fw = "2.0.0";                       // firmware_version @116
  memcpy(&d[116], fw, strlen(fw));
  const char* gn = "Milan Endpoints";             // group_name @180
  memcpy(&d[180], gn, strlen(gn));
  const char* sn = "PP-0000-0002";                // serial_number @244
  memcpy(&d[244], sn, strlen(sn));
  putbe(&d[308], 2, 2);                           // configurations_count
  putbe(&d[310], CFGIX, 2);                       // current_configuration
  return d;
}
static std::vector<uint8_t> audio_unit_descriptor(uint16_t ix,
                                                  uint32_t rate) {
  // §7.2.3 Table 7-5: the fixed part runs to 144, with current_sampling_rate
  // at 136, sampling_rates_offset at 140 and sampling_rates_count at 142.
  // That 136 is the offset GET_SAMPLING_RATE's COPY_BUF addresses, and it is
  // a multiple of 8 — the field opens its lane, which is why it can be copied
  // at all (a lane read would deliver it in bits [63:32], and the µISA has no
  // shift). Two rates here, so 152 B total.
  std::vector<uint8_t> d(152, 0);
  putbe(&d[0],   0x0002, 2);                      // descriptor_type
  putbe(&d[2],   ix, 2);                          // descriptor_index
  const char* nm = "Audio Unit 0";
  memcpy(&d[4], nm, strlen(nm));                  // object_name @4, 64 B
  putbe(&d[68],  0xFFFF, 2);                      // localized_description
  putbe(&d[70],  0x0000, 2);                      // clock_domain_index
  putbe(&d[72],  1, 2); putbe(&d[74], 0, 2);      // stream input ports
  putbe(&d[76],  1, 2); putbe(&d[78], 0, 2);      // stream output ports
  putbe(&d[136], rate, 4);                        // current_sampling_rate
  putbe(&d[140], 144, 2);                         // sampling_rates_offset
  putbe(&d[142], 2, 2);                           // sampling_rates_count
  putbe(&d[144], 48000u, 4);
  putbe(&d[148], 96000u, 4);
  return d;
}
//! Test-only 312-byte non-ENTITY descriptor. A real SIGNAL_MULTIPLEXER is
//! 76 bytes, but this deliberately matches the ENTITY length so the
//! E_RDESCENT type gate cannot be masked by its canonical-length guard.
static std::vector<uint8_t> non_entity_312_descriptor(uint16_t ix) {
  std::vector<uint8_t> d(312, 0);
  putbe(&d[0], 0x0022, 2);                         // SIGNAL_MULTIPLEXER
  putbe(&d[2], ix, 2);
  const char* nm = "NonEntity312";
  memcpy(&d[4], nm, strlen(nm));                  // object_name @4, 64 B
  putbe(&d[310], 0xBEEF, 2);                      // overlay tripwire
  return d;
}
static std::vector<uint8_t> control_descriptor(uint16_t ix) {
  // IEEE 7.2.22 Table 7-28, the Milan "Identify" CONTROL (5.3.3.10). Only the
  // locate has to hit for GET/SET_CONTROL: the VALUE is volatile state in the
  // dynamic store, never the image (Milan 5.3.12), so nothing in these bytes
  // is read by the command under test. control_type is the Identify UUID from
  // IEEE Table 7-98.
  std::vector<uint8_t> d(112, 0);
  putbe(&d[0],  0x001A, 2);
  putbe(&d[2],  ix, 2);
  const char* nm = "Identify";
  memcpy(&d[4], nm, strlen(nm));                  // object_name @4, 64 B
  putbe(&d[68], 0xFFFF, 2);                       // localized_description
  putbe(&d[80], 0x0000, 2);                       // CONTROL_LINEAR_UINT8
  putbe(&d[82], 0x90E0F00000000001ull, 8);        // control_type = IDENTIFY
  putbe(&d[94], 104, 2);                          // values_offset
  putbe(&d[96], 1, 2);                            // number_of_values
  return d;
}
static std::vector<uint8_t> clock_domain_descriptor() {
  // §7.2.32: 76 fixed + 2 x clock_sources. 78 is NOT a multiple of 8, which is
  // the case COPY_BUFFER has to stop mid-lane on.
  std::vector<uint8_t> d(78, 0);
  putbe(&d[0],  0x0024, 2);
  putbe(&d[2],  0x0000, 2);
  const char* nm = "Clock Domain 0";
  memcpy(&d[4], nm, strlen(nm));                  // object_name @4, 64 B
  putbe(&d[68], 0xFFFF, 2);                       // localized_description
  putbe(&d[70], 0x0000, 2);                       // clock_source_index
  putbe(&d[72], 76, 2);                           // clock_sources_offset
  putbe(&d[74], 1, 2);                            // clock_sources_count
  putbe(&d[76], 0, 2);                            // clock_source_0
  return d;
}

static std::vector<uint8_t> stream_port_descriptor(uint16_t ty, uint16_t ix,
                                                   uint16_t clusters,
                                                   uint16_t base_cluster,
                                                   uint16_t maps = 0,
                                                   uint16_t base_map = 0) {
  // §7.2.13 Table 7-23: 20 bytes, no object_name. number_of_maps = 0 declares
  // dynamic mapping; a nonzero value names the port's static AUDIO_MAPs.
  std::vector<uint8_t> d(20, 0);
  putbe(&d[0],  ty, 2);                           // descriptor_type
  putbe(&d[2],  ix, 2);                           // descriptor_index
  putbe(&d[4],  0x0000, 2);                       // clock_domain_index
  putbe(&d[6],  0x0000, 2);                       // port_flags
  putbe(&d[8],  0x0000, 2);                       // number_of_controls
  putbe(&d[10], 0x0000, 2);                       // base_control
  putbe(&d[12], clusters, 2);                     // number_of_clusters
  putbe(&d[14], base_cluster, 2);                 // base_cluster
  putbe(&d[16], maps, 2);                         // number_of_maps
  putbe(&d[18], base_map, 2);                     // base_map
  return d;
}

static std::vector<uint8_t> audio_map_descriptor(uint16_t ix,
                                                  uint64_t mapping) {
  // §7.2.19 Table 7-32: one static mapping, starting at byte 8.
  std::vector<uint8_t> d(16, 0);
  putbe(&d[0], 0x0017, 2);                        // AUDIO_MAP
  putbe(&d[2], ix, 2);
  putbe(&d[4], 8, 2);                             // mappings_offset
  putbe(&d[6], 1, 2);                             // number_of_mappings
  putbe(&d[8], mapping, 8);
  return d;
}

static std::vector<uint8_t> stream_descriptor(uint16_t ty, uint16_t ix) {
  // SS7.2.6 Table 7-8: fixed part through buffer_length @128..131 plus one
  // 8-byte format = 140 B. Existence is all GET_STREAM_INFO's locate needs;
  // the VALUES it answers come from the integrator's face, never from here.
  std::vector<uint8_t> d(140, 0);
  putbe(&d[0], ty, 2);
  putbe(&d[2], ix, 2);
  snprintf(reinterpret_cast<char*>(&d[4]), 60, "Stream %u.%u", ty & 0xF, ix);
  putbe(&d[68], 0xFFFF, 2);                       // localized_description
  putbe(&d[70], 0x0000, 2);                       // clock_domain_index
  putbe(&d[72], 0x0000, 2);                       // stream_flags
  putbe(&d[74], 0x00A0020140000800ull, 8);        // current_format (AAF)
  putbe(&d[82], 132, 2);                          // formats_offset
  putbe(&d[84], 1, 2);                            // number_of_formats
  putbe(&d[128], 192, 4);                         // buffer_length
  putbe(&d[132], 0x00A0020140000800ull, 8);       // format 0
  return d;
}

static std::vector<uint8_t> avb_interface_descriptor(uint16_t ix) {
  // SS7.2.8: fixed 98 B. Existence feeds GET_AVB_INFO/GET_AS_PATH's locate.
  std::vector<uint8_t> d(98, 0);
  putbe(&d[0], 0x0009, 2);
  putbe(&d[2], ix, 2);
  snprintf(reinterpret_cast<char*>(&d[4]), 60, "AVB Interface %u", ix);
  putbe(&d[68], 0xFFFF, 2);                       // localized_description
  putbe(&d[70], OWN_MAC, 6);                      // mac_address
  putbe(&d[76], 0x0007, 2);                       // interface_flags (gPTP+AS)
  putbe(&d[78], EID, 8);                          // clock_identity
  d[86] = 250; d[87] = 248;                       // priority1, clock_class
  putbe(&d[88], 0x4100, 2);                       // offset_scaled_log_variance
  d[90] = 0x21; d[91] = 247; d[92] = 0;           // accuracy, prio2, domain
  d[93] = 0xFD; d[94] = 0; d[95] = 0xFD;          // log intervals
  putbe(&d[96], 1, 2);                            // port_number
  return d;
}

// ---- the flat memory image (gen_desc_image.py layout, built independently) --
struct ImgEnt { uint16_t cfg, type, count, len, nbase, stride; uint32_t off; };

static std::vector<uint8_t> build_image(
    std::vector<ImgEnt>& ents,
    const std::vector<std::vector<uint8_t>>& bodies,
    const std::vector<const char*>& names, uint16_t n_cfg) {
  uint32_t idx_off = 32;
  uint32_t cur = idx_off + 16u * uint32_t(ents.size());
  cur = (cur + 7u) & ~7u;
  std::vector<uint32_t> where;
  for (auto& e : ents) {
    e.off = cur;
    for (uint16_t k = 0; k < e.count; ++k) { where.push_back(cur);
                                             cur += e.stride; }
  }
  uint32_t nm_off = cur;
  cur += 64u * uint32_t(names.size());
  uint32_t total = (cur + 7u) & ~7u;

  std::vector<uint8_t> b(total, 0);
  for (size_t i = 0; i < bodies.size() && i < where.size(); ++i)
    memcpy(&b[where[i]], bodies[i].data(), bodies[i].size());
  for (size_t i = 0; i < names.size(); ++i)
    memcpy(&b[nm_off + 64 * i], names[i], strlen(names[i]));

  uint16_t dmax = 0;
  for (auto& e : ents) if (e.len > dmax) dmax = e.len;
  putbe(&b[0], 0x41454D49u, 4);                   // "AEMI"
  putbe(&b[4], 1, 2);
  putbe(&b[6], n_cfg, 2);
  putbe(&b[8], uint16_t(ents.size()), 2);
  putbe(&b[10], uint16_t(names.size()), 2);
  putbe(&b[12], idx_off, 4);
  putbe(&b[16], nm_off, 4);
  putbe(&b[20], total, 4);
  putbe(&b[24], dmax, 2);
  for (size_t i = 0; i < ents.size(); ++i) {
    uint8_t* e = &b[idx_off + 16 * i];
    putbe(&e[0], ents[i].cfg, 2);   putbe(&e[2], ents[i].type, 2);
    putbe(&e[4], ents[i].count, 2); putbe(&e[6], ents[i].len, 2);
    putbe(&e[8], ents[i].off, 4);   putbe(&e[12], ents[i].nbase, 2);
    putbe(&e[14], ents[i].stride, 2);
  }
  uint32_t sum = 0;
  for (int i = 0; i < 7; ++i)
    sum += (uint32_t(b[4*i]) << 24) | (uint32_t(b[4*i+1]) << 16) |
           (uint32_t(b[4*i+2]) << 8) | b[4*i+3];
  putbe(&b[28], 0xFFFFFFFFu - sum, 4);
  return b;
}

static void dump(const char* tag, const std::vector<uint8_t>& f) {
  printf("  %s (%zu B):", tag, f.size());
  for (uint8_t c : f) printf(" %02x", c);
  printf("\n");
}

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------
struct NvmOp { int op; uint8_t region; uint16_t off, len;
               std::vector<uint8_t> wr; };

struct H {
  Vpp_top_wrap* d;
  vluint64_t t = 0;
  // MAC TX capture
  bool in_frame = false;
  std::vector<uint8_t> cur;
  std::deque<std::vector<uint8_t>> q_adp, q_acmp, q_msrp, q_mvrp;
  int tx_frames = 0;
  int adp_avail_seen = 0;    // AVAILABLE frames captured (aidx oracle)
  uint32_t la_msrp_ms = 0;   // last MSRP LeaveAllEvent seen (ms)
  // NVM device model (blank flash: reads answer 0xFF)
  enum { NV_IDLE, NV_READ, NV_WRITE, NV_ERASE } nv_st = NV_IDLE;
  uint16_t nv_left = 0;
  int      nv_done_lag = 0;
  NvmOp    nv_cur;
  std::vector<NvmOp> nvm_ops;
  // MAAP allocator model (02 §4.2). OFF by default: the processor ships
  // with the allocator in the integrating fabric, and "no allocator wired
  // yet" must be a survivable wiring, not a wedge.
  bool maap_on = false, maap_grant_ok = true;
  int  maap_offers = 0, maap_hold_cur = 0, maap_hold_last = 0, maap_da_seq = 0;
  int  maap_rsp_cnt = 0;
  uint64_t maap_rsp_da = 0;
  std::vector<std::pair<int, bool>> maap_reqs;      // {src, release}
  // acmp_declaring_o edge log: the gate LEVEL must be seen MOVING
  uint8_t decl_prev = 0;
  std::vector<std::pair<int, bool>> decl_edges;     // {src, rising}
  // descriptor-image DRAM model at DESC_BASE_P. NON-ZERO latency by default:
  // the reference SoC measures ~1424 ns on a miss to main memory, and a store
  // that only ever sees zero-latency answers is untested against the thing
  // that makes it hard.
  std::vector<uint8_t> dram;
  int  dram_lat = 31;
  bool dram_busy = false;
  uint32_t dram_addr = 0;
  int dram_beats = 0, dram_idx = 0, dram_wait = 0;
  uint64_t dram_reqs = 0;
  // AECP response-buffer memory at RESP_BASE_P (03 §7). READ + WRITE, and
  // NON-ZERO latency on BOTH channels by default — a buffer tested only
  // against zero-latency memory is untested against the thing that makes it
  // hard. `rmem_off` ties the whole master off, which the contract says is a
  // legal wiring the processor must DEGRADE against, not hang on.
  std::vector<uint8_t> rmem = std::vector<uint8_t>(RESP_BYTES, 0xC3);
  int  rmem_rlat = 23, rmem_wlat = 17;
  bool rmem_off = false, rmem_werr = false, rmem_rerr = false;
  bool rm_busy = false, rm_wbusy = false;
  uint32_t rm_addr = 0, rm_waddr = 0;
  uint64_t rm_wdata = 0;
  uint8_t  rm_wstrb = 0;
  int  rm_beats = 0, rm_idx = 0, rm_wait = 0, rm_wcnt = 0;
  uint64_t rm_reqs = 0, rm_writes = 0;
  vluint64_t aecp_rx_t = 0;
  std::deque<std::vector<uint8_t>> q_aecp;
  //! MAAP frames (subtype 0xFE) with their eof-time compressed ms
  std::deque<std::pair<std::vector<uint8_t>, uint32_t>> q_maap;

  // ---- the integrator's counter store (06 §6.6) ------------------------
  // This model is the INTEGRATOR, not the DUT: the processor lays out IEEE
  // §7.4.42.2's block and asks {descriptor_type, descriptor_index, quadlet};
  // what a quadlet MEANS is decided here, exactly as it is decided in the
  // fabric. Two different masks on purpose — Milan v1.2 Table 5.16's ten for
  // a CRF Media Clock Input that keeps no tv-bit tallies, those ten plus
  // TIMESTAMP_VALID/NOT_VALID for an AAF sink that does — so the suite proves
  // the processor carries whatever mask it is given rather than one blanket
  // answer. Nothing behind a CLEAR mask bit is ever non-zero here: a value
  // under an unset bit would make the wire check pass for the wrong reason.
  static const uint32_t CTR_MASK_AAF = 0x00000FFFu;
  static const uint32_t CTR_MASK_CRF = 0x00000F3Fu;
  static const uint32_t CTR_MASK_SOUT = 0x0000001Fu;
  int  ctr_hold = 2;          // cycles the store makes the engine wait
  bool ctr_stuck = false;     // a face that never answers at all
  int  ctr_hold_cur = 0;
  uint64_t ctr_reads = 0;
  //! the quadlets asked for, consecutive repeats folded away. The face is a
  //! LEVEL: while the response buffer refuses a write the same quadlet is
  //! asked again, which is free and harmless — what must never happen is the
  //! index MOVING under that back-pressure, because then a held beat writes
  //! the wrong quadlet
  std::vector<uint8_t> ctr_seq;
  static uint32_t ctr_mask(uint16_t ty, uint16_t ix) {
    if (ty == 0x0005 && ix == 0) return CTR_MASK_AAF;
    if (ty == 0x0005 && ix == 1) return CTR_MASK_CRF;
    if (ty == 0x0006 && ix <= 1) return CTR_MASK_SOUT;
    return 0;
  }
  static uint32_t ctr_value(uint16_t ty, uint16_t ix, uint8_t w) {
    uint32_t m = ctr_mask(ty, ix);
    if (w == 32) return m;
    if (w > 31 || !((m >> w) & 1u)) return 0;
    return 0xC0000000u | (uint32_t(ty) << 16) | (uint32_t(ix) << 8) | w;
  }

  // ---- the GET_AUDIO_MAP store (06 §6.5): what a port's dynamic mappings
  // ARE is decided here, exactly as milan_datapath decides it from its render
  // map RAM. Two ports on purpose: port 0 has ONE page holding 2 mappings,
  // port 1 has THREE pages (0 empty, 1 with 3, 2 with 1) so the §7.4.44.1
  // paging is proved against pages that really differ. Everything out of
  // range answers zero - the wrong-object guard the fabric must mirror.
  int  amap_hold = 2;          // cycles the store makes the engine wait
  bool amap_stuck = false;     // a face that never answers at all
  int  amap_hold_cur = 0;
  uint64_t amap_reads = 0;
  //! every completed query, folded like ctr_seq: {sel, rec} pairs
  std::vector<std::pair<uint8_t, uint8_t>> amap_seq;

  // ---- ADD/REMOVE_AUDIO_MAPPINGS transaction store ---------------------
  // Port 0 in each direction is dynamic. Port 1 exists in the descriptor
  // image but is static, which gives the command suite a real NOT_SUPPORTED
  // target. Claims model the final value of every key before commit so a
  // late invalid row cannot leave an earlier write behind.
  int  amap_edit_hold = 0;
  bool amap_edit_stuck = false;
  bool amap_edit_postcommit_wait = false;
  bool amap_edit_reject_commit = false;
  int  amap_edit_hold_cur = 0;
  bool amap_edit_seen = false;
  uint8_t amap_edit_seen_phase = 0, amap_edit_seen_rec = 0;
  uint64_t amap_edit_reply = 0;
  bool amap_edit_active = false, amap_edit_remove = false;
  bool amap_edit_changed = false, amap_edit_finish_changed = false;
  uint16_t amap_edit_type = 0, amap_edit_index = 0, amap_edit_count = 0;
  uint64_t amap_edit_mutations = 0;
  bool amap_edit_mode = false;
  std::vector<uint64_t> amap_edit_in0, amap_edit_in1;
  std::vector<uint64_t> amap_edit_out0, amap_edit_out1;
  std::vector<uint64_t> amap_edit_claims;
  std::vector<std::pair<uint8_t, uint8_t>> amap_edit_seq;

  static bool amap_has(const std::vector<uint64_t>& v, uint64_t row) {
    return std::find(v.begin(), v.end(), row) != v.end();
  }
  static uint32_t amap_edit_key(uint16_t ty, uint64_t row) {
    uint16_t sc = uint16_t(row >> 32);
    uint16_t co = uint16_t(row >> 16);
    uint16_t cc = uint16_t(row);
    return ty == 0x000E ? (uint32_t(co) << 16) | cc : sc;
  }
  static bool amap_edit_geometry(uint16_t ty, uint16_t ix, uint64_t row) {
    uint16_t si = uint16_t(row >> 48), sc = uint16_t(row >> 32);
    uint16_t co = uint16_t(row >> 16), cc = uint16_t(row);
    if (ty == 0x000E)
      return cc == 0 && co < 8 && si < 2 && sc < 8;
    if (ty == 0x000F)
      return cc == 0 && co < 25 && si < 2 && sc < 8 && ix < 2;
    return false;
  }
  std::vector<uint64_t>& amap_edit_live(uint16_t ty, uint16_t ix) {
    if (ty == 0x000E) return ix == 0 ? amap_edit_in0 : amap_edit_in1;
    return ix == 0 ? amap_edit_out0 : amap_edit_out1;
  }
  bool amap_edit_context() const {
    return amap_edit_active
        && uint16_t(d->amap_edit_remove_o) == uint16_t(amap_edit_remove)
        && uint16_t(d->amap_edit_desc_type_o) == amap_edit_type
        && uint16_t(d->amap_edit_desc_index_o) == amap_edit_index
        && uint16_t(d->amap_edit_count_o) == amap_edit_count;
  }
  bool amap_edit_validate_row(uint64_t row) {
    if (!amap_edit_geometry(amap_edit_type, amap_edit_index, row)) return false;
    uint32_t key = amap_edit_key(amap_edit_type, row);
    for (uint64_t claim : amap_edit_claims) {
      if (amap_edit_key(amap_edit_type, claim) == key) return claim == row;
    }
    for (uint64_t live : amap_edit_live(amap_edit_type, amap_edit_index)) {
      if (amap_edit_key(amap_edit_type, live) == key) return live == row;
    }
    if (amap_edit_type == 0x000F) {
      const auto& other = amap_edit_index == 0 ? amap_edit_out1 : amap_edit_out0;
      for (uint64_t live : other)
        if (amap_edit_key(amap_edit_type, live) == key) return false;
    }
    return !amap_edit_remove;
  }
  void amap_edit_accept() {
    uint8_t phase = uint8_t(d->amap_edit_phase_o);
    uint8_t rec = uint8_t(d->amap_edit_rec_o);
    uint64_t row = uint64_t(d->amap_edit_record_o);
    amap_edit_seq.push_back({phase, rec});
    amap_edit_reply = 0;

    if (phase == 0) {
      amap_edit_type = uint16_t(d->amap_edit_desc_type_o);
      amap_edit_index = uint16_t(d->amap_edit_desc_index_o);
      amap_edit_count = uint16_t(d->amap_edit_count_o);
      amap_edit_remove = d->amap_edit_remove_o != 0;
      amap_edit_active = ((amap_edit_type == 0x000E
                           || amap_edit_type == 0x000F)
                          && amap_edit_index < 2);
      amap_edit_changed = false;
      amap_edit_claims.clear();
      amap_edit_reply = amap_edit_active ? 1 : 0;
      return;
    }
    if (phase == 3) {
      amap_edit_active = false;
      amap_edit_claims.clear();
      return;
    }
    if (!amap_edit_context()) return;

    if (phase == 4 && rec < amap_edit_count) {
      bool ok = amap_edit_validate_row(row);
      amap_edit_reply = ok ? 1 : 0;
      if (ok && !amap_has(amap_edit_claims, row))
        amap_edit_claims.push_back(row);
    } else if (phase == 1) {
      amap_edit_reply = amap_edit_reject_commit ? 0 : 1;
    } else if (phase == 5 && rec < amap_edit_count) {
      auto& live = amap_edit_live(amap_edit_type, amap_edit_index);
      auto it = std::find(live.begin(), live.end(), row);
      if (amap_edit_remove) {
        if (it != live.end()) {
          live.erase(it); amap_edit_changed = true; ++amap_edit_mutations;
        }
      } else if (it == live.end()) {
        live.push_back(row); amap_edit_changed = true; ++amap_edit_mutations;
      }
      amap_edit_reply = 1;
    } else if (phase == 2) {
      amap_edit_finish_changed = amap_edit_changed;
      amap_edit_reply = amap_edit_changed ? 1 : 0;
      amap_edit_active = false;
      amap_edit_claims.clear();
    }
  }
  static uint16_t amap_nmaps(uint16_t ty, uint16_t ix) {
    if (ty == 0x000E) {                      // render-side map RAM
      if (ix == 0) return 1;
      if (ix == 1) return 3;
      return 0;
    }
    if (ty == 0x000F) {                      // capture-side map RAM (0x0017)
      if (ix == 0) return 2;
      if (ix == 1) return 1;
      return 0;
    }
    return 0;
  }
  static uint16_t amap_count(uint16_t ty, uint16_t ix, uint16_t page) {
    if (page >= amap_nmaps(ty, ix)) return 0;
    if (ty == 0x000F) return page == 0 ? 4 : 1;
    if (ix == 0) return 2;
    return page == 1 ? 3 : (page == 2 ? 1 : 0);
  }
  //! record k of (port, page): four distinct 16-bit fields keyed on all
  //! three coordinates, so a record served for the wrong port, page or
  //! ordinal cannot match
  static uint64_t amap_rec(uint16_t ty, uint16_t ix, uint16_t page,
                           uint8_t k) {
    //! ty bit 0 separates the render-side and capture-side stores, so a
    //! record served for the wrong DIRECTION cannot match either
    uint16_t tag = uint16_t(((ty & 1) << 15) | (ix << 12) | (page << 8) | k);
    return (uint64_t(0x1000 | tag) << 48) | (uint64_t(0x2000 | tag) << 32) |
           (uint64_t(0x3000 | tag) << 16) |  uint64_t(0x4000 | tag);
  }
  static uint64_t amap_value(uint16_t ty, uint16_t ix, uint16_t page,
                             uint8_t sel, uint8_t rec) {
    if (sel == 0) return amap_nmaps(ty, ix);
    if (sel == 1) return (uint64_t(amap_nmaps(ty, ix)) << 16)
                       |  amap_count(ty, ix, page);
    if (sel == 2) return (rec < amap_count(ty, ix, page))
                       ? amap_rec(ty, ix, page, rec) : 0;
    return 0;
  }
  uint64_t amap_query_value(uint16_t ty, uint16_t ix, uint16_t page,
                            uint8_t sel, uint8_t rec) {
    if (!amap_edit_mode) return amap_value(ty, ix, page, sel, rec);
    if ((ty != 0x000E && ty != 0x000F) || ix >= 2) return 0;
    uint16_t pages = ty == 0x000E ? 1 : 4;
    if (sel == 0) return pages;
    if (page >= pages) return sel == 1 ? (uint64_t(pages) << 16) : 0;
    auto& live = amap_edit_live(ty, ix);
    std::vector<uint64_t> rows;
    for (uint64_t v : live) {
      uint16_t co = uint16_t(v >> 16);
      if (co / 8 == page) rows.push_back(v);
    }
    if (sel == 1) return (uint64_t(pages) << 16) | rows.size();
    if (sel == 2 && rec < rows.size()) return rows[rec];
    return 0;
  }

  // ---- the Milan-info face model (06 SS6.2/SS6.10): the INTEGRATOR ----
  // What a word MEANS is decided here exactly as milan_datapath decides it
  // from its binding view and SRP registrars; the suite proves the processor
  // lays out whatever the face answers, byte-exact, and that unknown indices
  // answer zeros (absent, never invented). Values are keyed on all three
  // coordinates so a word served for the wrong kind, index or selector
  // cannot match.
  int  gsi_hold = 2;
  bool gsi_stuck = false;
  int  gsi_hold_cur = 0;
  uint64_t gsi_reads = 0;
  static uint64_t gsi_value(uint8_t kind, uint16_t ty, uint16_t ix,
                            uint8_t sel, uint8_t ord) {
    if (kind == 0) {                      // GET_STREAM_INFO words
      if (ty != 0x0005 && ty != 0x0006) return 0;
      if (ix >= 2) return 0;              // fabric knows sinks/sources 0..1
      uint64_t tag = (uint64_t(ty) << 8) | ix;
      switch (sel) {
        case 0: return 0x80000000u | (uint32_t(ty) << 8) | ix;   // flags
        case 1: return 0x00A0'0000'0000'0000ull | (tag << 16) | 1;
        case 2: return 0x00B0'0000'0000'0000ull | (tag << 16) | 2;
        case 3: return 0x000C'0000ull | tag;                     // latency
        case 4: return 0x91E0'F00D'0000'0000ull | (tag << 8);    // dmac+fc
        case 5: return 0x00D0'0000'0000'0000ull | (tag << 16) | 5;
        case 6: return (0x0002ull << 48) | 0x0000'0001ull | (tag << 16);
        case 7: return (uint64_t(0x60u | (ix & 0x1F)) << 24);    // pbsta byte
        default: return 0;
      }
    }
    if (kind == 1) {                      // GET_AVB_INFO words
      if (ty != 0x0009 || ix != 0) return 0;
      switch (sel) {
        case 0: return 0xA1A2'A3A4'A5A6'A7A8ull;             // gm id
        case 1: return (0x0000'1234ull << 32)                // pdelay
                     | (0x00ull << 24) | (0x07ull << 16)     // domain, flags
                     | 2;                                     // 2 mappings
        case 8: return (ord == 0) ? 0x0603'0002ull           // {tc 6, prio 3, vid 2}
              :        (ord == 1) ? 0x0502'0002ull           // {tc 5, prio 2, vid 2}
              :                     0;
        default: return 0;
      }
    }
    if (kind == 2) {                      // GET_AS_PATH words
      if (ix != 0) return 0;
      if (sel == 0) return 3;             // three ClockIdentities
      if (sel == 8) return (ord < 3) ? (0xC1D1'0000'0000'0000ull | ord) : 0;
      return 0;
    }
    (void)ord;
    return 0;
  }

  uint64_t rmem_rd64(uint32_t a) const {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      uint32_t k = a + uint32_t(i);
      v = (v << 8) | ((k < rmem.size()) ? rmem[k] : 0x5Cu);
    }
    return v;
  }

  uint64_t dram_rd64(uint32_t a) const {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      uint32_t k = a + uint32_t(i);
      v = (v << 8) | ((k < dram.size()) ? dram[k] : 0xA5);
    }
    return v;
  }

  static uint64_t maap_da(int n) { return 0x91E0F0AABB00ULL + unsigned(n); }
  bool saw_decl_edge(int src, bool rising) const {
    for (const auto& e : decl_edges)
      if (e.first == src && e.second == rising) return true;
    return false;
  }

  explicit H(Vpp_top_wrap* dd) : d(dd) {}

  void step() {
    d->clk_i = 0; d->eval();

    // ---- MAC TX capture (always ready this suite) ----
    d->tx_ready_i = 1;
    if (d->tx_valid_o) {
      if (d->tx_sof_o) { cur.clear(); in_frame = true; }
      if (in_frame) cur.push_back(d->tx_data_o);
      if (d->tx_eof_o && in_frame) {
        in_frame = false;
        tx_frames++;
        uint16_t et = cur.size() > 13 ? ((cur[12] << 8) | cur[13]) : 0;
        if (et == 0x22F0 && cur.size() > 14 && cur[14] == 0xFA) {
          if ((cur[15] & 0x0F) == 0) adp_avail_seen++;
          q_adp.push_back(cur);
        } else if (et == 0x22F0 && cur.size() > 14 && cur[14] == 0xFC) {
          q_acmp.push_back(cur);
        } else if (et == 0x22F0 && cur.size() > 14 && cur[14] == 0xFB) {
          q_aecp.push_back(cur);
          aecp_rx_t = t;                       // for the response-cost check
        } else if (et == 0x22F0 && cur.size() > 14 && cur[14] == 0xFE) {
          q_maap.push_back({cur, uint32_t(d->dbg_now_ms_o)});
        } else if (et == 0x22EA) {
          auto p = parse_mrpdu(cur);
          for (auto& v : p.vecs) {
            if (v.la) la_msrp_ms = d->dbg_now_ms_o;
          }
          q_msrp.push_back(cur);
        } else if (et == 0x88F5) {
          q_mvrp.push_back(cur);
        }
      }
    }

    // ---- descriptor-image DRAM model (07 §3.3) ----
    d->desc_mem_req_ready_i = dram_busy ? 0 : 1;
    d->desc_mem_rsp_valid_i = 0;
    d->desc_mem_rsp_data_i  = 0;
    d->desc_mem_rsp_last_i  = 0;
    d->desc_mem_rsp_err_i   = 0;
    if (dram_busy && dram_wait == 0 && dram_idx < dram_beats) {
      d->desc_mem_rsp_valid_i = 1;
      d->desc_mem_rsp_data_i =
          dram_rd64(dram_addr - DESC_BASE + uint32_t(8 * dram_idx));
      d->desc_mem_rsp_last_i = (dram_idx == dram_beats - 1) ? 1 : 0;
    }
    if (!dram_busy) {
      if (d->desc_mem_req_valid_o) {
        dram_busy  = true;
        dram_addr  = d->desc_mem_req_addr_o;
        dram_beats = d->desc_mem_req_beats_o;
        dram_idx   = 0;
        dram_wait  = dram_lat;
        ++dram_reqs;
      }
    } else if (dram_wait > 0) {
      --dram_wait;
    } else if (d->desc_mem_rsp_valid_i && d->desc_mem_rsp_ready_o) {
      if (++dram_idx >= dram_beats) dram_busy = false;
    }

    // ---- AECP response-buffer memory model (03 §7) ----
    d->resp_mem_req_ready_i = (!rm_busy && !rmem_off) ? 1 : 0;
    d->resp_mem_rsp_valid_i = 0;
    d->resp_mem_rsp_data_i  = 0;
    d->resp_mem_rsp_last_i  = 0;
    d->resp_mem_rsp_err_i   = 0;
    if (rm_busy && rm_wait == 0 && rm_idx < rm_beats) {
      d->resp_mem_rsp_valid_i = 1;
      d->resp_mem_rsp_data_i =
          rmem_rd64(rm_addr - RESP_BASE + uint32_t(8 * rm_idx));
      d->resp_mem_rsp_last_i = (rm_idx == rm_beats - 1) ? 1 : 0;
      d->resp_mem_rsp_err_i  = rmem_rerr ? 1 : 0;
    }
    if (!rm_busy) {
      if (d->resp_mem_req_valid_o && !rmem_off) {
        rm_busy  = true;
        rm_addr  = d->resp_mem_req_addr_o;
        rm_beats = d->resp_mem_req_beats_o;
        rm_idx   = 0;
        rm_wait  = rmem_rlat;
        ++rm_reqs;
      }
    } else if (rm_wait > 0) {
      --rm_wait;
    } else if (d->resp_mem_rsp_valid_i && d->resp_mem_rsp_ready_o) {
      if (rmem_rerr || ++rm_idx >= rm_beats) rm_busy = false;
    }

    d->resp_mem_wr_ready_i = (!rm_wbusy && !rmem_off) ? 1 : 0;
    d->resp_mem_wr_done_i  = 0;
    d->resp_mem_wr_err_i   = 0;
    if (!rm_wbusy) {
      if (d->resp_mem_wr_valid_o && !rmem_off) {
        rm_wbusy = true;
        rm_waddr = d->resp_mem_wr_addr_o;
        rm_wdata = d->resp_mem_wr_data_o;
        rm_wstrb = d->resp_mem_wr_strb_o;
        rm_wcnt  = rmem_wlat;
      }
    } else if (--rm_wcnt <= 0) {
      // byte n of the lane is bits [63-8n -: 8]; a byte whose strobe is 0 is
      // NOT modified — the model enforces the contract it documents
      if (!rmem_werr) {
        for (int i = 0; i < 8; ++i) {
          if ((rm_wstrb >> i) & 1) {
            uint32_t k = rm_waddr - RESP_BASE + uint32_t(i);
            if (k < rmem.size()) rmem[k] = uint8_t(rm_wdata >> (56 - 8 * i));
          }
        }
      }
      d->resp_mem_wr_done_i = 1;
      d->resp_mem_wr_err_i  = rmem_werr ? 1 : 0;
      rm_wbusy = false;
      ++rm_writes;
    }

    // ---- MAAP allocator model (02 §4.2) ----
    d->maap_req_ready_i = maap_on ? 1 : 0;
    d->maap_rsp_valid_i = 0;
    if (maap_rsp_cnt > 0 && --maap_rsp_cnt == 0) {
      d->maap_rsp_valid_i = 1;
      d->maap_rsp_ok_i = maap_grant_ok ? 1 : 0;
      d->maap_rsp_da_i = maap_rsp_da;
    }
    if (d->maap_req_valid_o) {
      if (maap_hold_cur == 0) maap_offers++;
      maap_hold_cur++;
    } else if (maap_hold_cur) {
      maap_hold_last = maap_hold_cur; maap_hold_cur = 0;
    }
    if (d->maap_req_valid_o && d->maap_req_ready_i) {
      bool rel = d->maap_req_release_o != 0;
      maap_reqs.push_back({(int)d->maap_req_src_o, rel});
      maap_rsp_cnt = 3;
      maap_rsp_da = (rel || !maap_grant_ok) ? 0 : maap_da(maap_da_seq++);
    }
    if ((uint8_t)d->acmp_declaring_o != decl_prev) {
      uint8_t now = (uint8_t)d->acmp_declaring_o;
      for (int s = 0; s < 8; s++)
        if (((now ^ decl_prev) >> s) & 1)
          decl_edges.push_back({s, bool((now >> s) & 1)});
      decl_prev = now;
    }

    // ---- NVM device model ----
    d->nvm_dev_gnt_i = 0;
    d->nvm_dev_rvalid_i = 0;
    d->nvm_dev_wready_i = 0;
    d->nvm_dev_done_i = 0;
    d->nvm_dev_err_i = 0;
    d->nvm_dev_busy_i = (nv_st != NV_IDLE);
    if (nv_st == NV_IDLE) {
      if (d->nvm_dev_req_o) {
        d->nvm_dev_gnt_i = 1;
        nv_cur = NvmOp{ (int)d->nvm_dev_op_o, (uint8_t)d->nvm_dev_region_o,
                        (uint16_t)d->nvm_dev_offset_o,
                        (uint16_t)d->nvm_dev_len_o, {} };
        nv_left = nv_cur.len;
        nv_done_lag = 2;
        if (nv_cur.op == 0)      nv_st = NV_READ;    // NVMP_OP_READ_C
        else if (nv_cur.op == 1) nv_st = NV_WRITE;
        else                     nv_st = NV_ERASE;
      }
    } else if (nv_st == NV_READ) {
      if (nv_left) {
        d->nvm_dev_rvalid_i = 1;
        d->nvm_dev_rdata_i = 0xFF;                   // blank flash
        if (d->nvm_dev_rready_o) nv_left--;
      } else if (--nv_done_lag <= 0) {
        d->nvm_dev_done_i = 1;
        nvm_ops.push_back(nv_cur);
        nv_st = NV_IDLE;
      }
    } else if (nv_st == NV_WRITE) {
      if (nv_left) {
        d->nvm_dev_wready_i = 1;
        if (d->nvm_dev_wvalid_o) {
          nv_cur.wr.push_back((uint8_t)d->nvm_dev_wdata_o);
          nv_left--;
        }
      } else if (--nv_done_lag <= 0) {
        d->nvm_dev_done_i = 1;
        nvm_ops.push_back(nv_cur);
        nv_st = NV_IDLE;
      }
    } else {                                         // ERASE
      if (--nv_done_lag <= 0) {
        d->nvm_dev_done_i = 1;
        nvm_ops.push_back(nv_cur);
        nv_st = NV_IDLE;
      }
    }

    // ---- GET_COUNTERS store (06 §6.6): the face pushes back by DEFAULT,
    // because a store that answers in the same cycle never exercises the hold
    d->ctr_wait_i = 0;
    d->ctr_data_i = 0;
    if (d->ctr_req_o) {
      if (ctr_stuck || ctr_hold_cur < ctr_hold) {
        d->ctr_wait_i = 1;
        ++ctr_hold_cur;
      } else {
        uint8_t w = (uint8_t)d->ctr_word_o;
        d->ctr_data_i = ctr_value((uint16_t)d->ctr_desc_type_o,
                                  (uint16_t)d->ctr_desc_index_o, w);
        ctr_hold_cur = 0;
        ++ctr_reads;
        if (ctr_seq.empty() || ctr_seq.back() != w) ctr_seq.push_back(w);
      }
    } else {
      ctr_hold_cur = 0;
    }

    // ---- GET_AUDIO_MAP store (06 §6.5): same reluctant default ----
    d->amap_wait_i = 0;
    d->amap_data_i = 0;
    if (d->amap_req_o) {
      if (amap_stuck || amap_hold_cur < amap_hold) {
        d->amap_wait_i = 1;
        ++amap_hold_cur;
      } else {
        uint8_t sel = (uint8_t)d->amap_sel_o, rec = (uint8_t)d->amap_rec_o;
        d->amap_data_i = amap_query_value((uint16_t)d->amap_desc_type_o,
                                          (uint16_t)d->amap_desc_index_o,
                                          (uint16_t)d->amap_map_index_o,
                                          sel, rec);
        amap_hold_cur = 0;
        ++amap_reads;
        if (amap_seq.empty() || amap_seq.back() != std::make_pair(sel, rec))
          amap_seq.push_back({sel, rec});
      }
    } else {
      amap_hold_cur = 0;
    }

    // ADD/REMOVE_AUDIO_MAPPINGS transaction face.
    d->amap_edit_wait_i = 0;
    d->amap_edit_data_i = 0;
    if (d->amap_edit_req_o) {
      uint8_t phase = uint8_t(d->amap_edit_phase_o);
      uint8_t rec = uint8_t(d->amap_edit_rec_o);
      bool postcommit = phase == 2 || phase == 5;
      if (!postcommit
          && (amap_edit_stuck || amap_edit_hold_cur < amap_edit_hold)) {
        d->amap_edit_wait_i = 1;
        ++amap_edit_hold_cur;
      } else {
        if (postcommit && amap_edit_postcommit_wait)
          d->amap_edit_wait_i = 1;
        if (!amap_edit_seen || amap_edit_seen_phase != phase
                            || amap_edit_seen_rec != rec) {
          amap_edit_seen = true;
          amap_edit_seen_phase = phase;
          amap_edit_seen_rec = rec;
          amap_edit_accept();
        }
        d->amap_edit_data_i = amap_edit_reply;
      }
    } else {
      amap_edit_hold_cur = 0;
      amap_edit_seen = false;
    }

    // ---- Milan-info face (06 SS6.2/SS6.10) ----
    d->gsi_wait_i = 0;
    d->gsi_data_i = 0;
    if (d->gsi_req_o) {
      if (gsi_stuck || gsi_hold_cur < gsi_hold) {
        d->gsi_wait_i = 1;
        ++gsi_hold_cur;
      } else {
        d->gsi_data_i = gsi_value((uint8_t)d->gsi_kind_o,
                                  (uint16_t)d->gsi_desc_type_o,
                                  (uint16_t)d->gsi_desc_index_o,
                                  (uint8_t)d->gsi_sel_o,
                                  (uint8_t)d->gsi_ord_o);
        gsi_hold_cur = 0;
        ++gsi_reads;
      }
    } else {
      gsi_hold_cur = 0;
    }
    d->eval();

    d->clk_i = 1; d->eval();
    t++;
  }
  void idle(int n) { for (int i = 0; i < n; i++) step(); }
  void run_ms(int ms) { idle(ms * MS_CYC); }
  uint32_t now_ms() { return d->dbg_now_ms_o; }

  void reset() {
    d->rst_n = 0;
    d->entity_id_i = EID; d->entity_model_id_i = EMID;
    d->own_mac_i = OWN_MAC;
    d->talker_sources_i = TKSRC; d->talker_caps_i = TKCAP;
    d->listener_sinks_i = LSNK;  d->listener_caps_i = LSCAP;
    d->current_cfg_i = CFGIX;    d->identify_index_i = IDIX;
    d->entity_enable_i = 0; d->link_up_i = 0; d->gm_change_i = 0;
    d->gm_id_i = GM0; d->gptp_domain_i = DOM0;
    d->p2p_i = 1; d->cfg_rank_i = 1;
    d->cfg_acc_lat_ns_i = ACC_LAT; d->port_rate_bps_i = RATE;
    d->cfg_tspec_max_frame_i = 1024;
    d->rx_valid_i = 0; d->rx_data_i = 0; d->rx_last_i = 0;
    d->tx_ready_i = 1;
    d->aecp_txn_ready_i = 0;              // P4 uCPU seam: defined tie-off
    d->restore_go_i = 0;
    d->nvm_dev_gnt_i = 0; d->nvm_dev_wready_i = 0;
    d->nvm_dev_rvalid_i = 0; d->nvm_dev_rdata_i = 0;
    d->nvm_dev_busy_i = 0; d->nvm_dev_done_i = 0; d->nvm_dev_err_i = 0;
    d->host_req_valid_i = 0; d->host_we_i = 0;
    d->host_addr_i = 0; d->host_wdata_i = 0;
    d->svc_valid_i = 0; d->svc_op_i = 0; d->svc_index_i = 0;
    d->svc_stream_id_i = 0; d->svc_da_i = 0; d->svc_vid_i = 0;
    d->svc_max_frame_i = 0; d->svc_lstn_state_i = 0;
    d->maap_req_ready_i = 0; d->maap_rsp_valid_i = 0; d->maap_rsp_ok_i = 0;
    d->maap_rsp_da_i = 0;
    d->maap_conflict_valid_i = 0; d->maap_conflict_src_i = 0;
    d->desc_mem_req_ready_i = 0; d->desc_mem_rsp_valid_i = 0;
    d->desc_mem_rsp_data_i = 0; d->desc_mem_rsp_last_i = 0;
    d->desc_mem_rsp_err_i = 0;
    d->resp_mem_req_ready_i = 0; d->resp_mem_rsp_valid_i = 0;
    d->resp_mem_rsp_data_i = 0; d->resp_mem_rsp_last_i = 0;
    d->resp_mem_rsp_err_i = 0; d->resp_mem_wr_ready_i = 0;
    d->resp_mem_wr_done_i = 0; d->resp_mem_wr_err_i = 0;
    dram_busy = false; dram_wait = 0;
    rm_busy = false; rm_wbusy = false; rm_wait = 0; rm_wcnt = 0;
    amap_edit_hold_cur = 0; amap_edit_seen = false;
    amap_edit_active = false; amap_edit_changed = false;
    amap_edit_finish_changed = false; amap_edit_mutations = 0;
    amap_edit_in0.clear(); amap_edit_in1.clear(); amap_edit_out0.clear();
    amap_edit_claims.clear();
    amap_edit_seq.clear();
    idle(20);
    d->rst_n = 1;
    idle(10);
  }

  // feed one whole wire frame into the MAC RX trunk, one byte per cycle
  void feed(const std::vector<uint8_t>& f) {
    for (size_t i = 0; i < f.size(); i++) {
      d->rx_valid_i = 1;
      d->rx_data_i = f[i];
      d->rx_last_i = (i + 1 == f.size()) ? 1 : 0;
      step();
    }
    d->rx_valid_i = 0;
    d->rx_last_i = 0;
    idle(4);
  }

  // side-port host access: {rdata, err}
  struct HostRsp { uint32_t data; bool err; bool got; };
  HostRsp host(bool we, uint32_t addr, uint32_t wdata = 0) {
    d->host_req_valid_i = 1;
    d->host_we_i = we ? 1 : 0;
    d->host_addr_i = addr;
    d->host_wdata_i = wdata;
    HostRsp r{0, false, false};
    for (int i = 0; i < 1000; i++) {
      step();
      if (d->host_rvalid_o) {
        r.data = d->host_rdata_o; r.err = d->host_err_o != 0; r.got = true;
        break;
      }
    }
    d->host_req_valid_i = 0;
    step();
    return r;
  }
  uint32_t snap(int word) { return host(false, 0x20000u + word).data; }
  uint32_t trace_lane(int rec, int lane) {
    return host(false, 0x40000u + (uint32_t)rec * 4 + lane).data;
  }

  // svc face op: {status, data}
  struct SvcRsp { int status; uint32_t data; bool got; };
  SvcRsp svc(int op, int idx, uint64_t sid = 0, uint64_t da = 0, int vid = 0,
             int mfs = 0, int lstn = 0) {
    int guard = 200000;
    while (!d->svc_ready_o && guard--) step();
    d->svc_valid_i = 1;
    d->svc_op_i = op; d->svc_index_i = idx;
    d->svc_stream_id_i = sid; d->svc_da_i = da; d->svc_vid_i = vid;
    d->svc_max_frame_i = mfs; d->svc_lstn_state_i = lstn;
    step();
    d->svc_valid_i = 0;
    SvcRsp r{-1, 0, false};
    for (int i = 0; i < 400000; i++) {
      if (d->svc_rsp_valid_o) {
        r.status = d->svc_rsp_status_o; r.data = d->svc_rsp_data_o;
        r.got = true;
        step();
        break;
      }
      step();
    }
    return r;
  }

  template <typename P>
  std::vector<uint8_t> wait_frame(std::deque<std::vector<uint8_t>>& q,
                                  int timeout_ms, P pred) {
    long budget = (long)timeout_ms * MS_CYC;
    for (;;) {
      while (!q.empty()) {
        auto f = q.front(); q.pop_front();
        if (pred(f)) return f;
      }
      if (budget-- <= 0) return {};
      step();
    }
  }
  std::vector<uint8_t> wait_any(std::deque<std::vector<uint8_t>>& q,
                                int timeout_ms) {
    return wait_frame(q, timeout_ms,
                      [](const std::vector<uint8_t>&) { return true; });
  }
  // align into a clean slot of the 200 ms MRP join cadence (see srp_top
  // suite): actions taken here drain alone at the next join tick
  void sync_join() {
    for (;;) {
      uint32_t ph = now_ms() % 1000u;
      if (ph >= 250 && ph <= 350) break;
      step();
    }
    q_msrp.clear(); q_mvrp.clear();
  }
  void flush_all() { q_adp.clear(); q_acmp.clear();
                     q_msrp.clear(); q_mvrp.clear(); }
  // MSRP byte-exact checks need a LeaveAll-free window: if the PRNG-drawn
  // 10-15 s leavealltimer is close, let it fire (MSRP always has periodic
  // traffic once the Domain is declared), then start clean
  void la_guard() {
    if (now_ms() - la_msrp_ms >= 8000) {
      (void)wait_frame(q_msrp, 18000, [](const std::vector<uint8_t>& fr) {
        auto p = parse_mrpdu(fr);
        for (auto& v : p.vecs) if (v.la) return true;
        return false;
      });
      run_ms(50);
    }
    q_msrp.clear(); q_mvrp.clear();
  }
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* d = new Vpp_top_wrap;
  H h(d);

  // The entity model lives in the integrator's main memory (07 §3.3): load it
  // BEFORE reset, exactly as software does before entity_enable. The image is
  // built here from the IEEE §7.2 field offsets and the documented header /
  // index-map layout — nothing in it comes from the DUT or from the
  // generator's output.
  std::vector<ImgEnt> img_ents = {
    {CFGIX, 0x0000, 1, 312, 0, 312, 0},          // ENTITY
    {CFGIX, 0x0024, 1,  78, 1,  80, 0},          // CLOCK_DOMAIN (not %8)
    //! the two STREAM_PORT_INPUTs the audio-map store models: the image is
    //! the EXISTENCE authority (E_GAMAP's DESC_ADDR locate), so an index
    //! past these two must answer NO_SUCH_DESCRIPTOR whatever the face says
    {CFGIX, 0x000E, 2,  20, 1,  24, 0},          // STREAM_PORT_INPUT x2
    //! GET_STREAM_INFO existence targets: the store is the authority, so
    //! index 2+ of either type must answer NO_SUCH_DESCRIPTOR whatever the
    //! Milan-info face would say
    {CFGIX, 0x0005, 2, 140, 1, 144, 0},          // STREAM_INPUT x2
    {CFGIX, 0x0006, 2, 140, 1, 144, 0},          // STREAM_OUTPUT x2
    {CFGIX, 0x0009, 1,  98, 1, 104, 0},          // AVB_INTERFACE
    {CFGIX, 0x000F, 3,  20, 1,  24, 0},          // two dynamic, one static
    {CFGIX, 0x0017, 1,  16, 0,  16, 0},          // static output AUDIO_MAP
    //! GET_SAMPLING_RATE's target. The rate is 96000, NOT the 48000 a
    //! hardcoded answer would most plausibly be, so a program that invents
    //! the value instead of copying it out of the image fails section W.
    {CFGIX, 0x0002, 1, 152, 0, 152, 0},          // AUDIO_UNIT
    //! GET_CONTROL / SET_CONTROL's target: Milan 5.3.3.10 makes the primary
    //! IDENTIFY control exist in every configuration at the same index
    {CFGIX, 0x001A, 1, 112, 0, 112, 0},          // CONTROL (Identify)
    //! Test-only shape that makes E_RDESCENT's type guard load-bearing.
    {CFGIX, 0x0022, 1, 312, 0, 312, 0},          // SIGNAL_MULTIPLEXER
  };
  std::vector<uint8_t> desc_entity = entity_descriptor();
  std::vector<uint8_t> desc_clkdom = clock_domain_descriptor();
  //! geometry consistent with H::amap_*: port 0 = 8 clusters at base 0 (one
  //! page of 8), port 1 = 24 clusters at base 8 (three pages of 8)
  std::vector<uint8_t> desc_spi0 = stream_port_descriptor(0x000E, 0, 8, 0);
  std::vector<uint8_t> desc_spi1 = stream_port_descriptor(0x000E, 1, 24, 8);
  h.dram = build_image(img_ents,
                       {desc_entity, desc_clkdom, desc_spi0, desc_spi1,
                        stream_descriptor(0x0005, 0), stream_descriptor(0x0005, 1),
                        stream_descriptor(0x0006, 0), stream_descriptor(0x0006, 1),
                        avb_interface_descriptor(0),
                        stream_port_descriptor(0x000F, 0, 8, 0),
                        stream_port_descriptor(0x000F, 1, 8, 8),
                        stream_port_descriptor(0x000F, 2, 8, 16, 1, 0),
                        audio_map_descriptor(0, 0),
                        audio_unit_descriptor(0, 96000u),
                        control_descriptor(0),
                        non_entity_312_descriptor(0)},
                       //! TWO configurations, so SET_CONFIGURATION has a
                       //! legal non-zero index to be tested with. Only
                       //! configuration 0 carries descriptors, which is a
                       //! legitimate shape and makes configuration 1 a clean
                       //! NO_SUCH_DESCRIPTOR target for READ_DESCRIPTOR.
                       //! A3's out-of-range probe uses index 3 and is
                       //! unaffected.
                       {"PP Reference Entity", "Clock Domain 0"}, 2);

  h.reset();

  // ==== R. boot restore over blank NVM (07 §5.3) ==========================
  {
    d->restore_go_i = 1;
    h.idle(5);
    d->restore_go_i = 0;
    int guard = 400000;
    while (!d->restore_done_o && guard--) h.step();
    CHECK(d->restore_done_o == 1, "R: restore_done");
    CHECK(d->restore_fail_o == 0, "R: no restore_fail on a blank device");
    int reads = 0;
    for (auto& o : h.nvm_ops) if (o.op == 0) reads++;
    CHECK(reads >= 8, "R: all 8 BINDING regions read, got %d", reads);
    CHECK(d->nvm_alarm_o == 0, "R: no commit alarm");
  }

  // ==== S0. link up, pre-enable quiescence + snapshot identity ============
  {
    CHECK(h.snap(0) == 0x4B4C5050u, "S0: snapshot magic KLPP");
    CHECK(h.snap(1) == 0x08080404u, "S0: shape word {SI,SO,RX,TX}");
    d->link_up_i = 1;
    h.idle(50);
    uint32_t f = h.snap(3);
    CHECK((f & 0x7) == 0x6, "S0: flags {seeded, link, !enable}, got 0x%x", f);
    uint32_t m0 = h.snap(2);
    h.run_ms(20);
    uint32_t m1 = h.snap(2);
    CHECK(m1 >= m0 + 15 && m1 <= m0 + 30,
          "S0: now_ms advances at the compressed rate (%u -> %u)", m0, m1);
    CHECK(h.q_adp.empty() && h.q_acmp.empty(),
          "S0: no 1722.1 TX before entity enable");
  }

  // ==== S1. SRP bring-up: Domain default declaration byte-exact ===========
  {
    auto f = h.wait_any(h.q_msrp, 1200);
    CHECK(!f.empty(), "S1: first MSRP frame within 1.2 s of link");
    Msg m{4, 4, false, {Vec{false, 1, fv_domain(6, 3, 2), {EV_NEW}, {}}}};
    auto exp = mrpdu_frame(true, OWN_MAC, {m});
    CHECK(f == exp, "S1: Domain New {6,3,2} byte-exact");
    if (!f.empty() && f != exp) { dump("got", f); dump("exp", exp); }
    uint32_t w10 = h.snap(10);
    CHECK(((w10 >> 16) & 7) == 3 && (w10 & 0xFFF) == 2,
          "S1: class-D domain defaults via snapshot, got 0x%08x", w10);
    CHECK((h.snap(3) & 0x8) == 0, "S1: DEFAULTS state (not adopted)");
    CHECK(h.snap(4) == 0 && h.snap(5) == 0 && h.snap(6) == 0,
          "S1: no RX front-end drops");
  }

  // ==== S2. DECLARE_TALKER (svc) -> admission -> Advertise byte-exact =====
  // (early on purpose: the first MVRP join must precede the first PRNG-
  //  drawn 10-15 s LeaveAll for the byte-exact check to be deterministic)
  const uint64_t SID0 = (OWN_MAC << 16) | 0x0001;
  const uint64_t DA0  = 0x91E0F00A0B01ULL;
  {
    h.sync_join();
    auto r = h.svc(OP_DECL_TK, 0, SID0, DA0, 2, 256, 0);
    CHECK(r.got && r.status == ST_OK, "S2: DECLARE_TALKER src 0 OK");
    uint64_t sum_model = slope_bps(256, 1);
    h.run_ms(250);
    uint32_t w12 = h.snap(12);
    CHECK(((w12 >> 24) & 1) == 1, "S2: src 0 admitted");
    CHECK(h.snap(11) == (uint32_t)sum_model,
          "S2: sum slope matches the Milan model (%u vs %llu)",
          h.snap(11), (unsigned long long)sum_model);
    CHECK(h.snap(30) == (uint32_t)sum_model, "S2: granted slope for src 0");
    CHECK(((h.snap(3) >> 4) & 1) == 0, "S2: no over_limit");
    CHECK(((h.snap(13) >> 16) & 3) == 1, "S2: tk_decl_state ADVERTISE");
    auto fv = fv_talker(SID0, DA0, 2, 256, 1, 3, 1, ACC_LAT);
    auto exp = mrpdu_frame(true, OWN_MAC,
                           {Msg{1, 25, false,
                                {Vec{false, 1, fv, {EV_NEW}, {}}}}});
    auto f = h.wait_frame(h.q_msrp, 800, [&](const std::vector<uint8_t>& fr) {
      return frame_has(fr, true, 1, SID0, EV_NEW);
    });
    CHECK(f == exp, "S2: Talker Advertise New byte-exact");
    if (!f.empty() && f != exp) { dump("got", f); dump("exp", exp); }
    auto expv = mrpdu_frame(false, OWN_MAC,
                            {Msg{1, 2, false,
                                 {Vec{false, 1, fv_vid(2), {EV_NEW}, {}}}}});
    auto g = h.wait_frame(h.q_mvrp, 800, [&](const std::vector<uint8_t>& fr) {
      return frame_has(fr, false, 1, 2, EV_NEW);
    });
    CHECK(g == expv, "S2: MVRP VID 2 New byte-exact");
    if (!g.empty() && g != expv) { dump("got", g); dump("exp", expv); }
  }

  // ==== S3. entity enable -> ADPDU in the advertise window + cadence ======
  // (cadence law on the wire: T-ADP-ADV 5 s re-arm + the 0-4 s pre-
  //  advertise anti-storm draw of F04.2 -> inter-frame gap in [5, 9] s)
  {
    h.flush_all();
    uint32_t t0 = h.now_ms();
    d->entity_enable_i = 1;
    auto f = h.wait_any(h.q_adp, 2600);
    uint32_t adv1_ms = h.now_ms();
    CHECK(!f.empty(), "S3: ADPDU within the T-ADP-DELAY-START window");
    CHECK(f.size() == 82, "S3: 82-byte wire frame, got %zu", f.size());
    auto exp = own_avail(0);
    CHECK(f == exp, "S3: ENTITY_AVAILABLE #1 byte-exact (aidx 0)");
    if (!f.empty() && f != exp) { dump("got", f); dump("exp", exp); }
    CHECK(adv1_ms - t0 <= 2200,
          "S3: first advertise inside 0..2 s + margin (%u ms)", adv1_ms - t0);
    CHECK((f.size() == 82) && ((f[16] >> 3) == 10),
          "S3: valid_time 10 (Milan 20 s validity)");
    auto f2 = h.wait_any(h.q_adp, 9800);
    uint32_t adv2_ms = h.now_ms();
    CHECK(!f2.empty(), "S3: re-advertise arrives");
    auto exp2 = own_avail(1);
    CHECK(f2 == exp2, "S3: ENTITY_AVAILABLE #2 byte-exact (aidx 1)");
    if (!f2.empty() && f2 != exp2) { dump("got", f2); dump("exp", exp2); }
    uint32_t gap = adv2_ms - adv1_ms;
    CHECK(gap >= 4800 && gap <= 9500,
          "S3: T-ADP-ADV 5 s + 0-4 s anti-storm, measured %u ms", gap);
  }

  // ==== S4. ENTITY_DISCOVER -> delayed byte-exact response ================
  {
    h.flush_all();
    uint32_t t0 = h.now_ms();
    int a0 = h.adp_avail_seen;
    auto disc = adp_frame(2, CTLR_MAC, 0, 0, 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0);
    h.feed(disc);
    auto f = h.wait_any(h.q_adp, 4600);
    CHECK(!f.empty(), "S4: DISCOVER answered");
    auto exp = own_avail((uint32_t)a0);
    CHECK(f == exp, "S4: response byte-exact at the running available_index");
    if (!f.empty() && f != exp) { dump("got", f); dump("exp", exp); }
    CHECK(h.now_ms() - t0 <= 4500,
          "S4: inside the T-ADP-DELAY window (%u ms)", h.now_ms() - t0);
    CHECK(h.snap(4) == 0 && h.snap(5) == 0 && h.snap(6) == 0,
          "S4: DISCOVER dropped nothing at the front end");
    CHECK(((h.snap(7) >> 24) & 0xFF) == 0, "S4: ADP queue drained");
  }

  // ==== S5. side-port host face: ctrl scratch, flags, trace, fw err =======
  {
    auto w = h.host(true, 0x30000u, 0xC0DEC0DEu);
    CHECK(w.got && !w.err, "S5: ctrl scratch write completes");
    auto r = h.host(false, 0x30000u);
    CHECK(r.got && r.data == 0xC0DEC0DEu, "S5: ctrl scratch reads back");
    auto fl = h.host(false, 0x30001u);
    CHECK(fl.got && (fl.data & 0xF) == 0x5,
          "S5: ctrl word 1 {done, enable}, got 0x%x", fl.data);
    auto fw = h.host(false, 0x50000u);
    CHECK(fw.got && fw.err, "S5: firmware window errors when disabled");
    CHECK((h.snap(15) & 0xFFFF) > 0, "S5: trace ring has records");
    auto s = h.host(false, 0x20000u);
    CHECK(s.got && !s.err, "S5: snapshot window read clean");
  }

  // ==== S6. BIND_RX -> response; ENTITY_AVAILABLE -> discovery -> probe ===
  {
    h.flush_all();
    auto bind = acmp_frame(CTLR_MAC, 6, 0, 0, CTLR_EID, T1_EID, EID,
                           T1_UID, 0, 0, 0, 0x1234, 0, 0);
    h.feed(bind);
    auto f = h.wait_any(h.q_acmp, 400);
    CHECK(!f.empty(), "S6: BIND_RX answered");
    auto expr = acmp_frame(OWN_MAC, 7, 0, 0, CTLR_EID, T1_EID, EID,
                           T1_UID, 0, 0, 1, 0x1234, 0, 0);
    CHECK(f == expr, "S6: BIND_RX_RESPONSE SUCCESS byte-exact");
    if (!f.empty() && f != expr) { dump("got", f); dump("exp", expr); }
    CHECK(((h.snap(28) >> 24) & 0xFF) == 0x01,
          "S6: discovery armed for sink 0 (binding view)");

    // the bound talker appears on the network
    uint32_t t0 = h.now_ms();
    auto avail = adp_frame(0, T1_MAC, T1_EID, 10, 0, GM0, DOM0,
                           0xBBB0000000000001ULL, 8, TKCAP, 0, 0,
                           0x0000C588u, 0, 0);
    h.feed(avail);
    auto p = h.wait_any(h.q_acmp, 1600);
    CHECK(!p.empty(), "S6: PROBE_TX after discovery + T-ACMP-DELAY");
    auto expp = acmp_frame(OWN_MAC, 0, 0, 0, CTLR_EID, T1_EID, EID,
                           T1_UID, 0, 0, 0, 0x0000, 0x0002, 0);
    CHECK(p == expp, "S6: PROBE_TX_COMMAND byte-exact (FAST_CONNECT, seq 0)");
    if (!p.empty() && p != expp) { dump("got", p); dump("exp", expp); }
    CHECK(h.now_ms() - t0 <= 1500,
          "S6: probe inside the T-ACMP-DELAY window (%u ms)",
          h.now_ms() - t0);
    // trace ring carries the routed EVT_TK_DISCOVERED{sink 0}. The probe
    // can legally RACE the event (a short bind-armed T-ACMP-DELAY draw vs
    // the discovery walk), so the trace gets a bounded settle window.
    bool disc_traced = false;
    for (int tries = 0; tries < 50 && !disc_traced; tries++) {
      int n = (int)(h.snap(15) & 0xFFFF);
      for (int k = 1; k <= 24 && n - k >= 0; k++) {
        uint32_t lane1 = h.trace_lane((n - k) & 0xFF, 1);
        if (((lane1 >> 24) & 0xFF) == 16 && (lane1 & 0xFFFF) == 0) {
          disc_traced = true;
          break;
        }
      }
      if (!disc_traced) h.run_ms(10);
    }
    CHECK(disc_traced, "S6: trace ring holds the TK_DISCOVERED{0} event");
    CHECK((h.snap(26) >> 16) >= 2,
          "S6: ACMP TX lane granted twice (response + probe)");
  }

  // ==== S7. TX interleave: ADP + ACMP + SRP, byte-exact each ==============
  {
    h.la_guard();
    h.sync_join();
    h.flush_all();
    uint32_t g_acmp0 = h.snap(26) >> 16;
    uint32_t g_adp0  = h.snap(26) & 0xFFFF;
    uint32_t g_srp0  = h.snap(27) >> 16;
    const uint64_t SID1 = (OWN_MAC << 16) | 0x0002;
    const uint64_t DA1  = 0x91E0F00A0B11ULL;
    // three producers pushed together: ACMP GET_RX_STATE, SRP declare,
    // ADP DISCOVER
    auto getrx = acmp_frame(CTLR_MAC, 10, 0, 0, CTLR2_EID, 0, EID,
                            0, 0, 0, 0, 0x4444, 0, 0);
    h.feed(getrx);
    auto r = h.svc(OP_DECL_TK, 1, SID1, DA1, 2, 100, 0);
    CHECK(r.got && r.status == ST_OK, "S7: DECLARE_TALKER src 1 OK");
    int a0 = h.adp_avail_seen;
    auto disc = adp_frame(2, CTLR_MAC, EID, 0, 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0);
    h.feed(disc);

    // ACMP: GET_RX_STATE of the bound (not settled) sink 0
    auto fa = h.wait_any(h.q_acmp, 400);
    CHECK(!fa.empty(), "S7: GET_RX_STATE answered");
    auto expa = acmp_frame(OWN_MAC, 11, 0, 0, CTLR2_EID, T1_EID, EID,
                           T1_UID, 0, 0, 1, 0x4444, 0x0002, 0);
    CHECK(fa == expa, "S7: GET_RX_STATE_RESPONSE byte-exact");
    if (!fa.empty() && fa != expa) { dump("got", fa); dump("exp", expa); }
    // SRP: Talker Advertise New for src 1 at the next join tick
    auto fv1 = fv_talker(SID1, DA1, 2, 100, 1, 3, 1, ACC_LAT);
    auto exps = mrpdu_frame(true, OWN_MAC,
                            {Msg{1, 25, false,
                                 {Vec{false, 1, fv1, {EV_NEW}, {}}}}});
    auto fs = h.wait_frame(h.q_msrp, 900, [&](const std::vector<uint8_t>& fr) {
      return frame_has(fr, true, 1, SID1, EV_NEW);
    });
    CHECK(fs == exps, "S7: src 1 Talker Advertise New byte-exact");
    if (!fs.empty() && fs != exps) { dump("got", fs); dump("exp", exps); }
    // ADP: the delayed DISCOVER response (running aidx oracle; the pending
    // cadence advertise carries the same image either way)
    auto fd = h.wait_any(h.q_adp, 4600);
    CHECK(!fd.empty(), "S7: DISCOVER answered among the interleave");
    auto expd = own_avail((uint32_t)a0);
    CHECK(fd == expd, "S7: ADP response byte-exact, no truncation");
    if (!fd.empty() && fd != expd) { dump("got", fd); dump("exp", expd); }
    CHECK((h.snap(26) >> 16) > g_acmp0 && (h.snap(26) & 0xFFFF) > g_adp0
          && (h.snap(27) >> 16) > g_srp0,
          "S7: all three TX lanes took grants");
    CHECK(fa.size() == 70 && fd.size() == 82 && parse_mrpdu(fs).ok,
          "S7: every frame whole on the wire");
  }

  // ==== S8. certified two-class Domain adoption + listener READY ==========
  {
    // clear the talkers first so the re-declaration drains alone
    auto r0 = h.svc(OP_WDRW_TK, 0);
    auto r1 = h.svc(OP_WDRW_TK, 1);
    CHECK(r0.got && r0.status == ST_OK && r1.got && r1.status == ST_OK,
          "S8: withdrawals OK");
    h.run_ms(700);
    h.la_guard();
    h.sync_join();
    // FirstValue {5, 2, VID 5}, NumberOfValues 2 — class A is value 1
    Msg dom{4, 4, false, {Vec{false, 2, fv_domain(5, 2, 5),
                              {EV_JOININ, EV_JOININ}, {}}}};
    h.feed(mrpdu_frame(true, T1_MAC, {dom}));
    h.run_ms(20);
    uint32_t w10 = h.snap(10);
    CHECK(((w10 >> 16) & 7) == 3 && (w10 & 0xFFF) == 5,
          "S8: adopted {prio 3, vid 5}, got 0x%08x", w10);
    CHECK((h.snap(3) & 0x8) != 0, "S8: ADOPTED state");
    auto rd = h.svc(OP_GET_DOM, 6);
    CHECK(rd.got && rd.status == ST_OK && (rd.data & 0xFFF) == 5,
          "S8: GET_DOMAIN via svc face reports the adopted VID");
    Msg re{4, 4, false, {Vec{false, 1, fv_domain(6, 3, 2), {EV_LV}, {}},
                         Vec{false, 1, fv_domain(6, 3, 5), {EV_NEW}, {}}}};
    auto expd = mrpdu_frame(true, OWN_MAC, {re});
    auto f = h.wait_frame(h.q_msrp, 900, [](const std::vector<uint8_t>& fr) {
      return frame_has(fr, true, 4, 6, EV_LV);
    });
    CHECK(f == expd, "S8: Domain Lv+New re-declaration byte-exact");
    if (!f.empty() && f != expd) { dump("got", f); dump("exp", expd); }

    // listener sink 2 READY end-to-end over the adopted domain
    const uint64_t SIDX = 0x1122334455660001ULL;
    const uint64_t DAX  = 0x91E0F0112233ULL;
    auto rl = h.svc(OP_DECL_LS, 2, SIDX, DAX, 5, 0, DECL_READY);
    CHECK(rl.got && rl.status == ST_OK, "S8: DECLARE_LISTENER sink 2 OK");
    h.sync_join();
    Msg adv{1, 25, false, {Vec{false, 1,
            fv_talker(SIDX, DAX, 5, 0x0100, 1, 3, 1, 0x00012345),
            {EV_JOININ}, {}}}};
    h.feed(mrpdu_frame(true, T1_MAC, {adv}));
    h.run_ms(20);
    CHECK(((h.snap(12) >> 4) & 3) == 1,
          "S8: tk_reg_state[2] ADVERTISE in class-D");
    CHECK(h.snap(16 + 2) == 0x00012345u,
          "S8: acc_latency[2] latched, got 0x%08x", h.snap(16 + 2));
    CHECK(((h.snap(14) >> 20) & 3) == 2,
          "S8: lstn_decl_state[2] READY in class-D");
    Msg lsn{3, 8, true, {Vec{false, 1, fv_sid(SIDX),
                             {EV_NEW}, {DECL_READY}}}};
    auto expl = mrpdu_frame(true, OWN_MAC, {lsn});
    auto fl = h.wait_frame(h.q_msrp, 900, [&](const std::vector<uint8_t>& fr) {
      return frame_has(fr, true, 3, SIDX, EV_NEW);
    });
    CHECK(fl == expl, "S8: Listener Ready New byte-exact");
    if (!fl.empty() && fl != expl) { dump("got", fl); dump("exp", expl); }
    // the registration reached the ACMP listener through the router: the
    // trace holds a TK_ATTR_REGISTERED{sink 2} record (source 2)
    int n = (int)(h.snap(15) & 0xFFFF);
    bool reg_traced = false;
    for (int k = 1; k <= 16 && n - k >= 0; k++) {
      uint32_t lane1 = h.trace_lane((n - k) & 0xFF, 1);
      if (((lane1 >> 24) & 0xFF) == 2 && (lane1 & 0xFF) == 2) {
        reg_traced = true;
        break;
      }
    }
    CHECK(reg_traced, "S8: trace holds TK_ATTR_REGISTERED{sink 2}");
  }

  // ==== S9. NVM debounce commit of the S6 binding ==========================
  {
    h.run_ms(700);                       // > T-NVM-DEBOUNCE at 1 ms ticks
    const NvmOp* wr = nullptr;
    for (auto& o : h.nvm_ops) {
      if (o.op == 1 && o.region == 0x20) wr = &o;   // keep the LAST one
    }
    CHECK(wr != nullptr, "S9: BINDING[0] commit reached the device");
    bool hdr_ok = false, eid_ok = false, len_ok = false;
    if (wr) {
      len_ok = (wr->len > 8) && (wr->wr.size() == wr->len);
      hdr_ok = wr->wr.size() >= 2 && wr->wr[0] == 0x17 && wr->wr[1] == 0x22;
      const uint8_t pat[8] = {0xAA, 0xAA, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xA1};
      for (size_t i = 0; i + 8 <= wr->wr.size(); i++) {
        if (!memcmp(&wr->wr[i], pat, 8)) { eid_ok = true; break; }
      }
    }
    CHECK(len_ok, "S9: framed record streamed whole (8 B header + payload)");
    CHECK(hdr_ok, "S9: F07.8 magic 0x1722 leads the record");
    CHECK(eid_ok, "S9: committed record carries the bound talker EID");
    CHECK(d->nvm_alarm_o == 0, "S9: no commit alarm");
  }

  // ==== S10. the maap face: the DA gate no fabric can open by itself ======
  // 01 §3 puts address allocation OUTSIDE this processor, so the talker DA
  // gate (and with it every Talker declaration into SRP) can ONLY be opened
  // from the top's maap port. Two halves: with no allocator the processor
  // must DEGRADE (answer honestly, never wedge), and with one the granted
  // address must reach the gate, the ACMP answers and the SRP wire.
  {
    const uint64_t SID_T0 = (OWN_MAC << 16) | 0x0000;   // wrap: sid[k]={mac,k}
    const uint16_t VID_ADOPTED = 5;                     // S8 adopted {3, 5}
    h.flush_all();

    // (a) the whole run so far has had NO allocator: the port is driven,
    //     nothing was accepted, and no source declares
    CHECK(h.maap_offers > 0,
          "S10: the top OFFERED maap requests with no allocator (%d)",
          h.maap_offers);
    CHECK(h.maap_reqs.empty(), "S10: nothing accepted without an allocator");
    CHECK(d->acmp_declaring_o == 0, "S10: no source declares without a DA");

    // (b) THE REGRESSION: the talker walker still serves commands. Before
    //     the accept window existed, one unaccepted allocation parked the
    //     walker forever and this GET_TX_STATE was never answered.
    auto gts = acmp_frame(CTLR_MAC, 4, 0, 0, CTLR_EID, EID, 0,
                          0, 0, 0, 0, 0x5150, 0, 0);
    h.feed(gts);
    auto f = h.wait_any(h.q_acmp, 400);
    CHECK(!f.empty(), "S10: GET_TX_STATE answered with maap absent");
    auto expf = acmp_frame(OWN_MAC, 5, 0, SID_T0, CTLR_EID, EID, 0,
                           0, 0, 0, 0, 0x5150, 0, VID_ADOPTED);
    CHECK(f == expf, "S10: GET_TX_STATE_RESPONSE byte-exact, DA 0");
    if (!f.empty() && f != expf) { dump("got", f); dump("exp", expf); }

    // (c) a probe with no DA is answered TALKER_DEST_MAC_FAILED — the
    //     honest degrade — and asks the (now present) allocator again
    h.maap_on = true;
    auto prb = acmp_frame(CTLR_MAC, 0, 0, 0, CTLR_EID, EID, T1_EID,
                          0, 7, 0, 0, 0x5151, 0x000A, 0);
    h.feed(prb);
    auto p = h.wait_any(h.q_acmp, 400);
    CHECK(!p.empty(), "S10: PROBE_TX answered");
    auto expp = acmp_frame(OWN_MAC, 1, 3, 0, CTLR_EID, EID, T1_EID,
                           0, 7, 0, 0, 0x5151, 0x000A, 0);
    CHECK(p == expp, "S10: PROBE_TX_RESPONSE DEST_MAC_FAILED byte-exact");
    if (!p.empty() && p != expp) { dump("got", p); dump("exp", expp); }

    // (d) the grant travels: maap -> DA gate -> the published level
    for (int i = 0; i < 200 && !(d->acmp_declaring_o & 1); i++) h.idle(10);
    CHECK(h.saw_decl_edge(0, true),
          "S10: acmp_declaring_o[0] OBSERVED 0 -> 1 on the MAAP grant");
    CHECK(h.maap_reqs.size() == 1 && h.maap_reqs[0].first == 0
          && !h.maap_reqs[0].second, "S10: exactly one ALLOC_DA, source 0");
    const uint64_t DA_G0 = H::maap_da(0);

    // (e) ... and reaches the ACMP answer
    auto gts2 = acmp_frame(CTLR_MAC, 4, 0, 0, CTLR_EID, EID, 0,
                           0, 0, 0, 0, 0x5152, 0, 0);
    h.feed(gts2);
    auto f2 = h.wait_any(h.q_acmp, 400);
    auto expf2 = acmp_frame(OWN_MAC, 5, 0, SID_T0, CTLR_EID, EID, 0,
                            0, 0, DA_G0, 0, 0x5152, 0, VID_ADOPTED);
    CHECK(f2 == expf2, "S10: the granted DA is what GET_TX_STATE answers");
    if (!f2.empty() && f2 != expf2) { dump("got", f2); dump("exp", expf2); }

    // (f) ... and reaches the SRP wire as the declared dest MAC. THIS is
    //     what the tied-off face silenced: the talker half of SRP.
    std::vector<uint8_t> fvgot;
    auto ta = h.wait_frame(h.q_msrp, 2500,
                           [&](const std::vector<uint8_t>& fr) {
      auto pp = parse_mrpdu(fr);
      for (auto& v : pp.vecs)
        if (v.type == 1 && v.fv.size() >= 14 && fv_u64(v.fv, 0, 8) == SID_T0) {
          fvgot = v.fv;
          return true;
        }
      return false;
    });
    CHECK(!ta.empty(), "S10: the gate reached SRP (Talker Advertise, src 0)");
    CHECK(fvgot.size() >= 14 && fv_u64(fvgot, 8, 6) == DA_G0,
          "S10: the declared dest MAC is the MAAP-granted address");
    CHECK(fvgot.size() >= 16 && fv_u64(fvgot, 14, 2) == VID_ADOPTED,
          "S10: declared on the adopted SR-class VID");
  }

  // ==== A. READ_DESCRIPTOR end to end (06 §6.1, 07 §3.3) ==================
  // A real AEM command on the MAC byte stream must come back as a BYTE-EXACT
  // AECPDU carrying the descriptor that lives in main memory. Before this
  // landed the command reached a pop face nobody popped and nothing came out.
  {
    auto cmd = [&](uint16_t op, const std::vector<uint8_t>& pl, uint16_t seq,
                   uint64_t target = EID, uint8_t mt = 0) {
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, mt, 0, target, CTLR_EID, seq, op,
                        pl));
      return h.wait_any(h.q_aecp, 200);
    };
    auto rdesc_pl = [](uint16_t cfg, uint16_t ty, uint16_t ix) {
      std::vector<uint8_t> p(8, 0);
      putbe(&p[0], cfg, 2); putbe(&p[4], ty, 2); putbe(&p[6], ix, 2);
      return p;
    };
    auto expect = [&](uint8_t status, uint16_t op, uint16_t seq,
                      const std::vector<uint8_t>& pl) {
      return aecp_frame(CTLR_MAC, OWN_MAC, 1, status, EID, CTLR_EID, seq, op,
                        pl);
    };

    CHECK(d->dbg_img_valid_o == 1,
          "A0: descriptor image validated out of DRAM (fault %u)",
          (unsigned)d->dbg_img_fault_o);
    //! snapshot word 25 = {13'd0, rx_slots_free[15:0], tx_slots_free[2:0]}.
    //! The TX pool is NOT idle here — SRP keeps committed frames in flight —
    //! so A11 demands no REGRESSION against this baseline rather than a fixed
    //! count, while the RX pool must come back whole.
    uint32_t tx_free_before = h.snap(25) & 0x7u;

    // ---- A1: the ENTITY descriptor, byte-exact on the wire ---------------
    uint16_t cmd0 = d->dbg_aecp_cmd_o, rsp0 = d->dbg_aecp_resp_o;
    uint64_t mem0 = h.dram_reqs;
    auto got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0000, 0), 0x1111);
    std::vector<uint8_t> pl;
    pl.resize(4, 0);
    putbe(&pl[0], CFGIX, 2);
    pl.insert(pl.end(), desc_entity.begin(), desc_entity.end());
    auto want = expect(AECP_SUCCESS, AEM_READ_DESCRIPTOR, 0x1111, pl);
    CHECK(!got.empty(), "A1: no READ_DESCRIPTOR response came back");
    CHECK(got.size() == want.size(), "A1: response is %zu B, want %zu",
          got.size(), want.size());
    CHECK(got == want, "A1: READ_DESCRIPTOR response is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    CHECK(d->dbg_aecp_cmd_o == cmd0 + 1 && d->dbg_aecp_resp_o == rsp0 + 1,
          "A1: command/response counters moved once");
    // ONE burst per command: the whole point of the line buffer is that a
    // descriptor costs one memory latency, not one per byte
    CHECK(h.dram_reqs == mem0 + 1,
          "A1: %llu memory bursts for one descriptor, want 1",
          (unsigned long long)(h.dram_reqs - mem0));

    // ---- A2: a bad descriptor_index is NO_SUCH_DESCRIPTOR + the §7.4.5 stub
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0000, 5), 0x2222);
    want = expect(AECP_NO_SUCH_DESCRIPTOR, AEM_READ_DESCRIPTOR, 0x2222,
                  rdesc_pl(CFGIX, 0x0000, 5));
    CHECK(!got.empty(), "A2: no response to a bad descriptor_index");
    CHECK(got == want, "A2: NO_SUCH_DESCRIPTOR response is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // an unknown descriptor_type answers the same way
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0099, 0), 0x2233);
    want = expect(AECP_NO_SUCH_DESCRIPTOR, AEM_READ_DESCRIPTOR, 0x2233,
                  rdesc_pl(CFGIX, 0x0099, 0));
    CHECK(got == want, "A2b: unknown descriptor_type is not byte-exact");

    // ---- A3: a bad configuration_index is BAD_ARGUMENTS (06 §6.1) --------
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(3, 0x0000, 0), 0x3333);
    want = expect(AECP_BAD_ARGUMENTS, AEM_READ_DESCRIPTOR, 0x3333,
                  rdesc_pl(3, 0x0000, 0));
    CHECK(!got.empty(), "A3: no response to a bad configuration_index");
    CHECK(got == want, "A3: BAD_ARGUMENTS response is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- A4: a descriptor whose length is NOT a multiple of 8 ------------
    // COPY_BUFFER reads 8-byte lanes; if it advanced by the lane instead of
    // the residual, this response would carry 2 bytes of the next descriptor
    // and lie about control_data_length
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0024, 0), 0x4444);
    pl.assign(4, 0);
    putbe(&pl[0], CFGIX, 2);
    pl.insert(pl.end(), desc_clkdom.begin(), desc_clkdom.end());
    want = expect(AECP_SUCCESS, AEM_READ_DESCRIPTOR, 0x4444, pl);
    CHECK(!got.empty(), "A4: no CLOCK_DOMAIN response");
    CHECK(got.size() == 38 + 4 + 78,
          "A4: CLOCK_DOMAIN response is %zu B, want %d", got.size(),
          38 + 4 + 78);
    CHECK(got == want, "A4: 78-byte descriptor response is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- A5: an opcode this build does not implement ---------------------
    // NOT_IMPLEMENTED with the command echoed (F06.14 / IEEE §9.3.5.3.3) —
    // never silence, never a malformed frame
    std::vector<uint8_t> sr_pl(4, 0);
    putbe(&sr_pl[0], 0x0002, 2);                        // AUDIO_UNIT, index 0
    got = cmd(AEM_WRITE_DESCRIPTOR, sr_pl, 0x5555);
    want = expect(AECP_NOT_IMPLEMENTED, AEM_WRITE_DESCRIPTOR, 0x5555, sr_pl);
    CHECK(!got.empty(), "A5: an unimplemented opcode answered with silence");
    CHECK(got == want, "A5: NOT_IMPLEMENTED echo is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- A5b: a NOT_IMPLEMENTED response is sized by ITS COMMAND ----------
    // IEEE §9.3.5.3.3 demands "a correctly sized response", and the reflected-
    // command reading is the one the reference stack implements on BOTH sides:
    // la_avdecc answers an unhandled command by reflecting it
    // (localEntityImpl.ipp "Reflect back the command, and return a
    // NotImplemented error code") and its controller checks a NOT_IMPLEMENTED
    // payload for EQUALITY with the command's length
    // (protocolAemPayloads.cpp checkResponsePayload). So control_data_length
    // must be 12 + the command's payload at every length, and the payload
    // bytes must be the command's own.
    //
    // A5 alone proves one 4-byte case, which a length stuck at 4, an echo of
    // zeros, or a length left over from the previous command all survive. A
    // live Hive 4.3.1 session (2026-08-14) reported "Incorrect payload size"
    // against exactly this class, so it is swept: empty, 4, 8, 16, and one
    // past the 60-octet Ethernet floor where padding can no longer hide a
    // wrong length. 0x7FFD/0x7FFE are unassigned in Table 7-140 and stay
    // NOT_IMPLEMENTED whatever else this engine grows.
    //! GET_AUDIO_MAP and both audio-map edit commands left this sweep when
    //! they became real answers. Their refusals and variable bodies are
    //! graded in sections Q and R.
    struct { uint16_t op; size_t n; const char* what; } nisz[] = {
      {0x7FFE,  0, "unassigned opcode, empty payload"},
      {0x004D,  4, "GET_MAX_TRANSIT_TIME (§7.4.78.1, the Hive 4.3.1 case)"},
      //! 0x0000 ACQUIRE_ENTITY left this sweep when Milan §5.4.2.1's
      //! NOT_SUPPORTED answer landed - its echo is graded in section L
      {0x7FFC, 16, "unassigned opcode, 16-byte payload"},
      {0x7FFD, 72, "unassigned opcode, past the 60-octet floor"},
      {0x004D,  4, "GET_MAX_TRANSIT_TIME again, after a 72-byte command"},
    };
    uint16_t niseq = 0x5560;
    for (auto& c : nisz) {
      std::vector<uint8_t> p(c.n);
      //! never zeros: an echo that emitted the right COUNT of the wrong bytes
      //! would pass a zero-filled payload
      for (size_t i = 0; i < c.n; ++i) p[i] = uint8_t(0xA0 + i);
      got = cmd(c.op, p, ++niseq);
      want = expect(AECP_NOT_IMPLEMENTED, c.op, niseq, p);
      CHECK(got == want, "A5b: %s: the response is not the echoed command",
            c.what);
      if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
      //! control_data_length read off the wire rather than compared to the
      //! model: a builder that shared the bug would agree with the DUT and
      //! prove nothing
      uint16_t cdl = got.size() > 17
                     ? uint16_t(((got[16] & 0x07) << 8) | got[17]) : 0xFFFFu;
      CHECK(cdl == 12 + c.n, "A5b: %s: cdl %u, want %zu", c.what,
            (unsigned)cdl, 12 + c.n);
      size_t wlen = (38 + c.n < 60) ? 60 : 38 + c.n;
      CHECK(got.size() == wlen, "A5b: %s: %zu B on the wire, want %zu",
            c.what, got.size(), wlen);
    }

    // ---- A6: IDENTIFY_NOTIFICATION as a COMMAND (IEEE §7.4.39.2) ---------
    std::vector<uint8_t> id_pl(4, 0);
    putbe(&id_pl[0], 0x001A, 2);
    got = cmd(AEM_IDENTIFY_NOTIF, id_pl, 0x6666);
    want = expect(AECP_BAD_ARGUMENTS, AEM_IDENTIFY_NOTIF, 0x6666, id_pl);
    CHECK(!got.empty(), "A6: IDENTIFY_NOTIFICATION command got no answer");
    CHECK(got == want, "A6: the opcode-specific BAD_ARGUMENTS is not "
          "byte-exact");

    // ---- A7: a truncated READ_DESCRIPTOR is BAD_ARGUMENTS ----------------
    // it must NOT locate whatever zeros happened to follow the header
    std::vector<uint8_t> short_pl(4, 0);
    got = cmd(AEM_READ_DESCRIPTOR, short_pl, 0x7777);
    want = expect(AECP_BAD_ARGUMENTS, AEM_READ_DESCRIPTOR, 0x7777, short_pl);
    CHECK(!got.empty(), "A7: a truncated READ_DESCRIPTOR got no answer");
    CHECK(got == want, "A7: truncated-command answer is not byte-exact");

    // ---- A8: a command for another entity is dropped (F06.2) -------------
    uint16_t drop0 = d->dbg_aecp_drop_o;
    h.q_aecp.clear();
    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID ^ 0xFFULL, CTLR_EID,
                      0x8888, AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0, 0)));
    h.run_ms(20);
    CHECK(h.q_aecp.empty(),
          "A8: answered a command addressed to another entity_id");
    CHECK(d->dbg_aecp_drop_o == drop0 + 1, "A8: the drop was counted");

    // ---- A9: an AEM RESPONSE arriving as input is never answered ---------
    drop0 = d->dbg_aecp_drop_o;
    h.q_aecp.clear();
    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 1, 0, EID, CTLR_EID, 0x9999,
                      AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0, 0)));
    h.run_ms(20);
    CHECK(h.q_aecp.empty(), "A9: answered an AECP RESPONSE (storm hazard)");
    CHECK(d->dbg_aecp_drop_o == drop0 + 1, "A9: the response drop was counted");

    // ---- A10: back-to-back commands, sequence_id echoed each time --------
    for (uint16_t k = 0; k < 3; ++k) {
      uint16_t sq = uint16_t(0xA000 + k);
      got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0000, 0), sq);
      pl.assign(4, 0);
      putbe(&pl[0], CFGIX, 2);
      pl.insert(pl.end(), desc_entity.begin(), desc_entity.end());
      want = expect(AECP_SUCCESS, AEM_READ_DESCRIPTOR, sq, pl);
      CHECK(got == want, "A10: repeat %u is not byte-exact", k);
    }

    // ---- A11: no slot is silted up by the AECP path ----------------------
    // the engine owns the RX slot from pop to free and the TX slot from alloc
    // to serialize. A one-slot-per-command leak would strand the RX pool
    // (4 slots) inside eight commands and the responses would simply stop, so
    // drive well past it and then demand BOTH pools back — sampled after a
    // quiet window, since ADP/SRP frames of their own may be mid-flight.
    for (uint16_t k = 0; k < 8; ++k) {
      got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0024, 0),
                uint16_t(0xB000 + k));
      CHECK(!got.empty() && got.size() == 38 + 4 + 78,
            "A11: command %u of the leak run went unanswered", k);
    }
    h.run_ms(60);
    uint32_t pools = h.snap(25);
    CHECK(((pools >> 3) & 0xFFFFu) == 4u,
          "A11: %u of 4 RX slots free after the AECP traffic",
          (pools >> 3) & 0xFFFFu);
    //! the TX pool breathes with SRP's own 200 ms cadence, so poll for the
    //! baseline to come back instead of sampling one instant; a leak would
    //! never return it (and would have stalled the eight commands above)
    uint32_t tx_free_after = pools & 0x7u;
    for (int q = 0; q < 12 && tx_free_after < tx_free_before; ++q) {
      h.run_ms(40);
      tx_free_after = h.snap(25) & 0x7u;
    }
    CHECK(tx_free_after >= tx_free_before,
          "A11: TX slots free never came back: %u -> %u",
          tx_free_before, tx_free_after);
    CHECK((h.snap(32) >> 16) == d->dbg_aecp_cmd_o,
          "A11: the snapshot window publishes the command counter");
    CHECK((h.snap(34) & 1u) == 1u,
          "A11: the snapshot window publishes image-valid");
  }

  // ==== M. MVU GET_MILAN_INFO (Milan v1.2 §5.4.4.1) =======================
  // The command a Milan controller sends FIRST, before it reads a single
  // descriptor, and the one whose answer decides whether it treats this device
  // as a PAAD-AE at all. It is NOT an AEM opcode: Milan §5.4.3.2 puts a 48-bit
  // protocol_id at @22..@27 and the MVU command_type at @28..@29, so the field
  // the 03 §4 record calls `opcode` holds the first two bytes of the
  // protocol_id — the whole point of the MVU sub-decode is that the bytes
  // which identify the command are ones only the payload walk reads.
  {
    // Milan §5.4.3.2.1: Avnu OUI-36 00-1B-C5-0A-C + MVU's 0x100.
    const uint16_t MVU_PID_HI  = 0x001B;          // @22..@23
    const uint32_t MVU_PID_LO  = 0xC50AC100u;     // @24..@27
    const uint16_t MVU_INFO    = 0x0000;          // Table 5.18 GET_MILAN_INFO
    const uint8_t  VU_COMMAND  = 6, VU_RESPONSE = 7;

    // Figure 5.3: protocol_id, r + command_type, reserved — an 8-byte payload
    // counted from @24, so control_data_length is 20.
    auto mvu_cmd_pl = [&](uint32_t pid_lo, uint16_t ct, size_t bytes) {
      std::vector<uint8_t> p(bytes, 0);
      if (bytes >= 4) putbe(&p[0], pid_lo, 4);
      if (bytes >= 6) putbe(&p[4], ct, 2);
      return p;
    };
    auto mvu = [&](uint32_t pid_lo, uint16_t ct, uint16_t seq,
                   size_t bytes = 8) {
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, VU_COMMAND, 0, EID, CTLR_EID, seq,
                        MVU_PID_HI, mvu_cmd_pl(pid_lo, ct, bytes)));
      return h.wait_any(h.q_aecp, 200);
    };
    auto mvu_expect = [&](uint8_t status, uint16_t seq,
                          const std::vector<uint8_t>& pl) {
      return aecp_frame(CTLR_MAC, OWN_MAC, VU_RESPONSE, status, EID, CTLR_EID,
                        seq, MVU_PID_HI, pl);
    };

    // ---- M1: the Figure 5.4 response, byte-exact --------------------------
    // 20 payload bytes from @24, so the AECPDU is 44 B and cdl is 32; the
    // frame still leaves the wire at the 60-byte Ethernet minimum.
    std::vector<uint8_t> info_pl(20, 0);
    putbe(&info_pl[0],  MVU_PID_LO, 4);           // protocol_id @24..@27
    putbe(&info_pl[4],  MVU_INFO, 2);             // r = 0 + command_type @28
    putbe(&info_pl[6],  0u, 2);                   // reserved @30
    putbe(&info_pl[8],  1u, 4);                   // protocol_version @32
    putbe(&info_pl[12], 0u, 4);                   // features_flags @36
    putbe(&info_pl[16], 0u, 4);                   // certification_version @40

    uint16_t mcmd0 = d->dbg_aecp_cmd_o, mrsp0 = d->dbg_aecp_resp_o;
    auto got = mvu(MVU_PID_LO, MVU_INFO, 0xC001);
    auto want = mvu_expect(AECP_SUCCESS, 0xC001, info_pl);
    CHECK(!got.empty(), "M1: GET_MILAN_INFO answered with silence");
    CHECK(got == want, "M1: GET_MILAN_INFO response is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    CHECK(d->dbg_aecp_cmd_o == mcmd0 + 1 && d->dbg_aecp_resp_o == mrsp0 + 1,
          "M1: command/response counters moved once");
    if (got.size() >= 60) {
      CHECK((got[15] & 0x0F) == VU_RESPONSE,
            "M1: message_type is %u, want VENDOR_UNIQUE_RESPONSE",
            got[15] & 0x0F);
      // Milan Table 5.19 / IEEE Table 9-6: MVU SUCCESS is 0, the same code the
      // AEM path uses, so the status register needs no remapping
      CHECK((got[16] >> 3) == 0, "M1: MVU status is %u, want SUCCESS",
            got[16] >> 3);
      CHECK(((((got[16] & 0x07) << 8) | got[17]) == 32),
            "M1: control_data_length is %u, want 32",
            ((got[16] & 0x07) << 8) | got[17]);
      CHECK(std::equal(got.begin() + 36, got.begin() + 42,
                       std::vector<uint8_t>{0x00, 0x1B, 0xC5, 0x0A, 0xC1,
                                            0x00}.begin()),
            "M1: the response protocol_id is not 00-1B-C5-0A-C1-00");
    }

    // ---- M2: the three fields a controller actually records ---------------
    // Decoded from the wire rather than inferred from M1's compare, because
    // this is the content the whole command exists for. features_flags is 0
    // ON PURPOSE: Table 5.20's REDUNDANCY would claim Milan §8 on a
    // single-interface PAAD, and TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING would
    // claim map changes while streaming from a build that answers
    // ADD/REMOVE_AUDIO_MAPPINGS with NOT_IMPLEMENTED.
    if (got.size() >= 60) {
      uint32_t pv = 0, ff = 0, cv = 0;
      for (int i = 0; i < 4; ++i) {
        pv = (pv << 8) | got[46 + i];             // AECPDU @32
        ff = (ff << 8) | got[50 + i];             // AECPDU @36
        cv = (cv << 8) | got[54 + i];             // AECPDU @40
      }
      CHECK(pv == 1u, "M2: protocol_version is %u, want 1 (Milan §4.2.4)", pv);
      CHECK(ff == 0u, "M2: features_flags is 0x%08x, want 0 — this PAAD "
            "implements neither Table 5.20 feature", ff);
      CHECK(cv == 0u, "M2: certification_version is 0x%08x, want 0 — no "
            "Milan certification has been passed", cv);
    }

    // ---- M3: a FOREIGN protocol_id is still NOT_IMPLEMENTED ---------------
    // Same Avnu OUI-36, different 12-bit protocol identifier. Nothing above
    // @26 tells these two apart, so this is what proves the whole 48-bit id is
    // compared and not just its head (06 §6.9: wrong protocol_id -> VU
    // response echoing the protocol_id, NOT_IMPLEMENTED).
    auto foreign = mvu_cmd_pl(0xC50AC101u, MVU_INFO, 8);
    got = mvu(0xC50AC101u, MVU_INFO, 0xC002);
    want = mvu_expect(AECP_NOT_IMPLEMENTED, 0xC002, foreign);
    CHECK(!got.empty(), "M3: a foreign vendor-unique protocol got silence");
    CHECK(got == want, "M3: the foreign-protocol_id echo is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- M4: an MVU command_type this build does not implement -------------
    // GET_SYSTEM_UNIQUE_ID (Table 5.18 0x0002) is a Milan RECOMMENDATION this
    // build does not serve: MVU status 1 with the command echoed, never
    // silence and never a Figure 5.4 body it cannot fill.
    auto suid = mvu_cmd_pl(MVU_PID_LO, 0x0002, 8);
    got = mvu(MVU_PID_LO, 0x0002, 0xC003);
    want = mvu_expect(AECP_NOT_IMPLEMENTED, 0xC003, suid);
    CHECK(!got.empty(), "M4: an unimplemented MVU command_type got silence");
    CHECK(got == want, "M4: the unimplemented-MVU echo is not byte-exact");

    // ---- M5: the r field is compared, the reserved field is not -----------
    // Milan §5.4.3.2.2 requires r = 0 in every MVU message and gives the
    // receiver no leave to ignore it; §5.4.4.1's reserved field, by contrast,
    // is explicitly "ignored by the receiver". So r = 1 is not this command
    // (echo), while a junk reserved field still gets the real answer with a
    // reserved field of 0 — the response must never forward it.
    auto rset = mvu_cmd_pl(MVU_PID_LO, 0x8000, 8);
    got = mvu(MVU_PID_LO, 0x8000, 0xC004);
    want = mvu_expect(AECP_NOT_IMPLEMENTED, 0xC004, rset);
    CHECK(got == want, "M5: r = 1 was not echoed as NOT_IMPLEMENTED");

    auto junk = mvu_cmd_pl(MVU_PID_LO, MVU_INFO, 8);
    junk[6] = 0xDE; junk[7] = 0xAD;                // reserved @30..@31
    h.q_aecp.clear();
    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, VU_COMMAND, 0, EID, CTLR_EID, 0xC005,
                      MVU_PID_HI, junk));
    got = h.wait_any(h.q_aecp, 200);
    want = mvu_expect(AECP_SUCCESS, 0xC005, info_pl);
    CHECK(got == want,
          "M5b: a junk reserved field changed the answer (it must be ignored "
          "on the command and zero in the response)");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- M6: a truncated MVU command ---------------------------------------
    // 6 payload bytes stop at @29, so the reserved field never arrived and the
    // command is not the Figure 5.3 one: echo it rather than answer a
    // GET_MILAN_INFO assembled from bytes nobody read.
    auto trunc = mvu_cmd_pl(MVU_PID_LO, MVU_INFO, 6);
    got = mvu(MVU_PID_LO, MVU_INFO, 0xC006, 6);
    want = mvu_expect(AECP_NOT_IMPLEMENTED, 0xC006, trunc);
    CHECK(!got.empty(), "M6: a truncated MVU command got silence");
    CHECK(got == want, "M6: the truncated-MVU echo is not byte-exact");

    // ---- M7: the descriptor path is untouched ------------------------------
    // Hive enumerating is the biggest thing this processor does; an MVU
    // command must not leave the engine, the response buffer or the RX pool in
    // a state the next READ_DESCRIPTOR trips over.
    std::vector<uint8_t> rcmd_pl(8, 0);
    putbe(&rcmd_pl[0], CFGIX, 2);                 // ENTITY, index 0
    h.q_aecp.clear();
    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0xC007,
                      AEM_READ_DESCRIPTOR, rcmd_pl));
    got = h.wait_any(h.q_aecp, 400);
    std::vector<uint8_t> rpl(4, 0);
    putbe(&rpl[0], CFGIX, 2);
    rpl.insert(rpl.end(), desc_entity.begin(), desc_entity.end());
    want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID,
                      0xC007, AEM_READ_DESCRIPTOR, rpl);
    CHECK(got == want,
          "M7: READ_DESCRIPTOR regressed after an MVU exchange");

    // ---- M8: the WHOLE protocol_id decides, all 48 bits --------------------
    // M3 sends one foreign protocol_id and it differs from MVU's in its LAST
    // BYTE ONLY, so it exercises one comparison term out of four. Deleting
    // either of the @22..@23 or @24..@25 comparisons, or weakening the @26
    // one, left both suites fully green: 40 of the 48 bits the design compares
    // were guarded by nothing.
    //
    // ONE CASE PER OCTET, not per comparison. The design compares in four
    // terms - @22..@23, @24..@25, then @26 and @27 one byte at a time - but a
    // regression does not have to respect those boundaries, and a first draft
    // of this table that moved TWO bytes at once still let a weakened @26
    // term through, because @27 alone was enough to reject the frame. Six
    // cases, each differing from Milan's identifier in exactly one octet, is
    // the granularity at which no single term can stop being made unnoticed.
    struct VuCase { uint16_t hi; uint32_t lo; const char* what; };
    const VuCase VU_FOREIGN[] = {
      {0xFF1B, 0xC50AC100u, "foreign in @22 (OUI octet 1)"},
      {0x00FF, 0xC50AC100u, "foreign in @23 (OUI octet 2)"},
      {0x001B, 0xFF0AC100u, "foreign in @24 (OUI octet 3)"},
      {0x001B, 0xC5FFC100u, "foreign in @25 (OUI-36 nibble + protocol)"},
      {0x001B, 0xC50AFF00u, "foreign in @26 (protocol number, high)"},
      {0x001B, 0xC50AC1FFu, "foreign in @27 (protocol number, low)"},
      {0x001B, 0xC50AC101u, "one BIT from MVU, in the last octet"},
    };
    for (const VuCase& c : VU_FOREIGN) {
      auto fpl = mvu_cmd_pl(c.lo, MVU_INFO, 8);
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, VU_COMMAND, 0, EID, CTLR_EID,
                        0xC010, c.hi, fpl));
      got = h.wait_any(h.q_aecp, 200);
      want = aecp_frame(CTLR_MAC, OWN_MAC, VU_RESPONSE, AECP_NOT_IMPLEMENTED,
                        EID, CTLR_EID, 0xC010, c.hi, fpl);
      CHECK(!got.empty(), "M8: %s got silence", c.what);
      CHECK(got == want, "M8: %s was not echoed as NOT_IMPLEMENTED", c.what);
      if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    }

    // ---- M8b: a vendor's protocol_id survives the echo, BIT FOR BIT --------
    // 1722.1-2021 9.6.2 Figure 9-12 gives a VENDOR_UNIQUE AECPDU a 48-bit
    // protocol_id at @22..@27 with NO u bit anywhere in it. The header
    // emitter used to clear bit 7 of @22 for every message type, on the
    // reasoning that @22's top bit is always `u`. Per 9.3.2 that bit is a
    // field of its own ahead of `cr`, and it exists only on an
    // AEM AECPDU (9.3.2.1). Every OUI with bit 7 set came back mangled.
    //
    // MVU could never have shown it: Avnu's 00-1B-C5 has bit 7 clear, so the
    // whole M-section, the live board probes and every fuzz seed were immune
    // by construction. These two identifiers are chosen for that bit alone.
    const VuCase VU_HIGHBIT[] = {
      {0xFC1B, 0xC50AC100u, "an OUI with bit 7 of @22 SET (0xFC)"},
      {0x801B, 0xC50AC100u, "an OUI that is bit 7 and nothing else (0x80)"},
    };
    for (const VuCase& c : VU_HIGHBIT) {
      auto fpl = mvu_cmd_pl(c.lo, MVU_INFO, 8);
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, VU_COMMAND, 0, EID, CTLR_EID,
                        0xC011, c.hi, fpl));
      got = h.wait_any(h.q_aecp, 200);
      CHECK(!got.empty(), "M8b: %s got silence", c.what);
      if (got.size() >= 24) {
        // @22 is frame byte 36: the ONE byte the old mask touched.
        CHECK(got[36] == (uint8_t)(c.hi >> 8),
              "M8b: %s: protocol_id[47:40] came back 0x%02X, want 0x%02X",
              c.what, got[36], (uint8_t)(c.hi >> 8));
      }
      want = aecp_frame(CTLR_MAC, OWN_MAC, VU_RESPONSE, AECP_NOT_IMPLEMENTED,
                        EID, CTLR_EID, 0xC011, c.hi, fpl);
      CHECK(got == want, "M8b: %s: the echo is not byte-exact", c.what);
      if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    }

    // ---- M8c: ...and an AEM command still clears its u bit -----------------
    // The fix is a discriminator, not a deletion, so the other side of it
    // needs a check too. 9.3.2.1: `u` is a 1-bit field of an AEM AECPDU's
    // command_type and a SOLICITED response carries it clear. A controller
    // that sent an AEM command with the bit set (it should not, but the field
    // is on the wire) must still get a solicited response back.
    std::vector<uint8_t> upl(8, 0);
    putbe(&upl[0], CFGIX, 2);                     // ENTITY, index 0
    h.q_aecp.clear();
    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0xC012,
                      (uint16_t)(0x8000u | AEM_READ_DESCRIPTOR), upl));
    got = h.wait_any(h.q_aecp, 400);
    CHECK(!got.empty(), "M8c: an AEM command with u set got silence");
    if (got.size() >= 24) {
      CHECK((got[36] & 0x80) == 0,
            "M8c: the AEM response kept u set (byte @22 = 0x%02X)", got[36]);
      CHECK((((unsigned)got[36] << 8) | got[37]) == AEM_READ_DESCRIPTOR,
            "M8c: command_type is 0x%04X, want 0x%04X",
            ((unsigned)got[36] << 8) | got[37], AEM_READ_DESCRIPTOR);
    }

    // ---- M9: an OUI that COLLIDES with an AEM opcode (issue #83) ----------
    // The dispatch reads AECPDU @22..@23 as `opcode`, and on a VENDOR_UNIQUE
    // message those two bytes are the head of a 48-bit protocol_id. Some
    // discriminators guarded on message_type and some did not, so a vendor
    // whose OUI began 00-04 was answered by READ_DESCRIPTOR: a 354-byte
    // VENDOR_UNIQUE_RESPONSE carrying OUR entity descriptor, with the caller's
    // protocol_id partly overwritten by @24..@27 of the AEM body.
    //
    // IT IS A CLASS, NOT AN INSTANCE. Every AEM opcode the dispatch names is
    // an OUI head that can collide, so all four are sent here rather than the
    // one that was found. 00:04:xx and 00:24:xx are densely assigned blocks;
    // this was reachable on a real link, not a curiosity.
    {
      //! THE OUI HEADS THAT COLLIDE ARE THE DISPATCH'S OPCODES, and getting
      //! that list from the engine rather than from memory matters: a first
      //! cut of this test used 0x0024 believing it to be
      //! IDENTIFY_NOTIFICATION, which is 0x0026. 0x0024 is
      //! REGISTER_UNSOLICITED_NOTIFICATION -- an opcode, and a colliding one,
      //! but not the arm that was broken -- so the probe missed the arm it was
      //! aimed at and a mutation restoring that arm's guard survived it. The
      //! list below is extracted from the engine's OP_*_C localparams.
      //!
      //! `bytes` is the payload length: the short-command arms are only
      //! reachable below their clause's minimum cdl, so 0x0029 and 0x002B are
      //! sent short on purpose.
      //! `mt` too, because VENDOR_UNIQUE is only ONE of the message types
      //! whose @22..@23 is not a command_type. The RX validator buckets AECP
      //! as 6/7 -> MVU, 2/3 -> ADDRESS_ACCESS and EVERYTHING ELSE -> AEM, so
      //! AVC_COMMAND (4), HDCP_APM_COMMAND (8), the reserved 10/12 and
      //! EXTENDED_COMMAND (14) arrive in the AEM bucket carrying an
      //! `avc_length` (Figure 9-9) where the dispatch reads an opcode. A
      //! review measured mt=4 with avc_length 0x0004 drawing our 312-octet
      //! ENTITY descriptor, and avc_length 0x0014 -- an ordinary AV/C length,
      //! and also OP_SET_SAMP_RATE_C -- WRITING THE SAMPLING RATE and
      //! answering SUCCESS. Guarding the protocol bucket alone was not enough.
      //! EVERY opcode the engine names, against EVERY non-AEM message type.
      //! A review found the previous cut swept four of twenty-one, so removing
      //! the guard from any of the other seventeen arms -- SET_CONFIGURATION,
      //! SET_CLOCK_SOURCE, SET_CONTROL, ACQUIRE, LOCK, all state-changing --
      //! passed the whole suite. The comment above it already claimed the list
      //! came from the dispatch. It does now.
      static const uint16_t kOpcodes[] = {
        0x0000, 0x0001, 0x0002, 0x0004, 0x0006, 0x0007, 0x0009, 0x000F,
        0x0014, 0x0015, 0x0016, 0x0017, 0x0018, 0x0019, 0x0022, 0x0023,
        0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002B,
      };
      //! 0x0022/0x0023 (START/STOP_STREAMING) joined this sweep with issue
      //! #78. Every opcode the engine decodes has to be here, or the #83
      //! guard - "the dispatch is on message_type, not on the residual
      //! protocol bucket" - is simply untested for the newest arm, which is
      //! exactly the one nobody has looked at yet.
      //! the residual bucket in full (KL_pp_rx_validator: 6/7 MVU, 2/3 AA,
      //! everything else AEM), plus AA itself
      static const uint8_t kMsgTypes[] = {2, 4, 6, 8, 10, 12, 14};

      struct Col { uint8_t mt; uint16_t hi; size_t bytes; const char* what; };
      std::vector<Col> cols;
      for (uint8_t mt : kMsgTypes)
        for (uint16_t op : kOpcodes)
          cols.push_back({mt, op, 8, "non-AEM message carrying an AEM opcode"});
      //! the short-command arms, which need a cdl below their clause minimum
      cols.push_back({6, 0x0004,  2, "VENDOR_UNIQUE / READ_DESCRIPTOR short"});
      cols.push_back({6, 0x0029,  2, "VENDOR_UNIQUE / GET_COUNTERS short"});
      cols.push_back({6, 0x002B,  2, "VENDOR_UNIQUE / GET_AUDIO_MAP short"});
      //! and bit 15 set, which the u-bit ternary at the 6'd36 header byte is
      //! the only thing standing between and a corrupted protocol_id
      cols.push_back({6, 0x8004,  8, "VENDOR_UNIQUE, OUI with bit 15 set"});
      cols.push_back({4, 0x8004,  8, "AVC_COMMAND, length word with bit 15 set"});
      //! TWO ARMS RE-DISPATCH ON THE PAYLOAD, so for those the filler body is
      //! not enough: a `AA-BB-CC-DD` @24..@25 is neither a descriptor type nor
      //! a stream-port type, so under a broken guard the frame lands on a
      //! type-invalid stub whose refusal is BYTE-IDENTICAL to the correct one.
      //! The row then passes whether or not the guard exists.
      //!
      //! Both are given a real body below, in the payload override. A review
      //! found the SET_SAMPLING_RATE one first and I fixed only that; the
      //! GET_AUDIO_MAP arm had the same hole and a second review found it
      //! still open. They are listed together here so the next one is not
      //! missed: if an arm keys on payload CONTENT, the sweep's filler cannot
      //! test it.

      uint16_t sq = 0xC020;
      for (const auto& c : cols) {
        //! a full 48-bit protocol_id: the colliding head plus a nonzero tail,
        //! so a response that overwrites @24..@27 is visible as well as one
        //! that answers with the wrong body
        std::vector<uint8_t> pl(c.bytes, 0);
        if (c.bytes >= 4) putbe(&pl[0], 0xAABBCCDDu, 4);  // protocol_id @24..
        //! ...except the SET_SAMPLING_RATE-shaped row, which needs a real
        //! AUDIO_UNIT descriptor and a real rate to be able to write anything
        if (c.mt == 4 && c.hi == 0x0014 && c.bytes == 8) {
          putbe(&pl[0], 0x0002, 2);                 // AUDIO_UNIT
          putbe(&pl[2], 0x0000, 2);                 // index 0
          putbe(&pl[4], 48000u, 4);                 // a rate that is NOT 96000
        }
        //! GET_AUDIO_MAP re-dispatches on @24..@25 too (STREAM_PORT_IN/OUT),
        //! so give it a real one or the guard on `amap_w` is untested
        if (c.hi == 0x002B && c.bytes == 8) {
          putbe(&pl[0], 0x000E, 2);                 // STREAM_PORT_INPUT
          putbe(&pl[2], 0x0000, 2);                 // index 0
          putbe(&pl[4], 0u, 4);                     // map_index 0
        }
        h.q_aecp.clear();
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, c.mt, 0, EID, CTLR_EID, sq,
                          c.hi, pl));
        auto r = h.wait_any(h.q_aecp, 400);
        auto want = aecp_frame(CTLR_MAC, OWN_MAC, (uint8_t)(c.mt | 1),
                               AECP_NOT_IMPLEMENTED,
                               EID, CTLR_EID, sq, c.hi, pl);
        CHECK(!r.empty(), "M9: mt=%u word %04X (%s) got silence",
              c.mt, c.hi, c.what);
        CHECK(r == want,
              "M9: mt=%u word %04X must be NOT_IMPLEMENTED with the command "
              "echoed WHOLE (%s)", c.mt, c.hi, c.what);
        if (!r.empty() && r != want) { dump("got ", r); dump("want", want); }
        //! the protocol_id is the byte a wrong answer corrupts, so grade it
        //! explicitly rather than relying on the byte-exact compare alone
        //! compare against what was SENT, not against a constant: one row
        //! carries an AUDIO_UNIT body instead of the AA-BB-CC-DD filler
        bool echoed = r.size() >= 38 + c.bytes
                      && (((unsigned)r[36] << 8) | r[37]) == c.hi;
        for (size_t i = 0; echoed && i < c.bytes; ++i)
          echoed = (r[38 + i] == pl[i]);
        CHECK(echoed,
              "M9: ...and every echoed byte survives (mt=%u word %04X, "
              "%zu-byte payload)", c.mt, c.hi, c.bytes);
        sq++;
      }

      //! AND IT MUST NOT HAVE CHANGED ANYTHING. The status byte alone would
      //! not have caught the worst of this: mt=4 with avc_length 0x0014
      //! reached SET_SAMPLING_RATE's microprogram and WROTE the rate, then
      //! answered SUCCESS. Read the rate back through the command that serves
      //! it, because a refusal that still moved state is not a refusal.
      {
        std::vector<uint8_t> rp(4, 0);
        putbe(&rp[0], 0x0002, 2);                 // AUDIO_UNIT, index 0
        h.q_aecp.clear();
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0xC030,
                          AEM_GET_SAMPLING_RATE, rp));
        auto g = h.wait_any(h.q_aecp, 400);
        unsigned rate = (g.size() >= 46)
                        ? (((unsigned)g[42] << 24) | ((unsigned)g[43] << 16)
                           | ((unsigned)g[44] << 8) | g[45]) : 0u;
        CHECK(!g.empty() && g.size() >= 46 && ((g[16] >> 3) & 0x1F) == 0,
              "M9b: GET_SAMPLING_RATE still answers SUCCESS after the storm");
        CHECK(rate == 96000u,
              "M9b2: ...and the rate is UNMOVED at 96000 - an AV/C length "
              "that collides with SET_SAMPLING_RATE must not write it, got %u",
              rate);
      }

      //! A NON-COLLIDING OUI, for symmetry. Note this is NOT the control that
      //! rules out "refuses every vendor command" -- it expects the same
      //! NOT_IMPLEMENTED echo as the rows above, so it cannot tell the two
      //! apart. M1 is that control: GET_MILAN_INFO byte-exact at SUCCESS.
      std::vector<uint8_t> pl(8, 0);
      putbe(&pl[0], 0xC50AC101u, 4);
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 6, 0, EID, CTLR_EID, sq,
                        MVU_PID_HI, pl));
      auto r = h.wait_any(h.q_aecp, 400);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 7, AECP_NOT_IMPLEMENTED,
                             EID, CTLR_EID, sq, MVU_PID_HI, pl);
      CHECK(r == want,
            "M9c: a non-colliding vendor OUI still round-trips whole");
    }
  }


  // ==== B. the response buffer lives in MAIN MEMORY (03 §7) ===============
  // The 592-byte response buffer used to be fabric state — 5,079 flip-flops
  // inside KL_aecp_engine, and the instances the placer could not pack on the
  // reference part. It is now KL_aecp_resp_buf over the resp_mem_* master, and
  // these checks prove the move is INVISIBLE on the wire, VISIBLE in memory,
  // and SAFE when the memory is not there. The BFM injects non-zero latency on
  // both channels by default (23 clocks read, 17 write).
  {
    auto cmd = [&](uint16_t op, const std::vector<uint8_t>& pl, uint16_t seq) {
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, seq, op, pl));
      return h.wait_any(h.q_aecp, 400);
    };
    auto rdesc_pl = [](uint16_t cfg, uint16_t ty, uint16_t ix) {
      std::vector<uint8_t> p(8, 0);
      putbe(&p[0], cfg, 2); putbe(&p[4], ty, 2); putbe(&p[6], ix, 2);
      return p;
    };

    // ---- B1: one response = one read burst + exactly the lanes it wrote --
    h.rmem.assign(RESP_BYTES, 0xC3);
    uint64_t rq0 = h.rm_reqs, rw0 = h.rm_writes;
    uint32_t lane0 = d->dbg_resp_lane_o;
    vluint64_t t0 = h.t;
    auto got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0024, 0), 0xC001);
    size_t pld  = 4 + desc_clkdom.size();
    // COPY_BUFFER writes whole 32-bit words, so the µCPU touches the 4-byte
    // prefix plus the descriptor rounded UP to its 8-byte lanes; the lane
    // holding buffer byte 11 + that many bytes is the last one written
    size_t wrote = 4 + ((desc_clkdom.size() + 7) / 8) * 8;
    size_t lanes = (11 + wrote) >> 3;
    CHECK(!got.empty() && got.size() == 38 + pld,
          "B1: CLOCK_DOMAIN response is %zu B, want %zu", got.size(),
          38 + pld);
    CHECK(h.rm_reqs == rq0 + 1,
          "B1: %llu response-memory read bursts for one response, want 1",
          (unsigned long long)(h.rm_reqs - rq0));
    CHECK(h.rm_writes - rw0 == lanes,
          "B1: %llu lane writes, want %zu",
          (unsigned long long)(h.rm_writes - rw0), lanes);
    CHECK(uint32_t(d->dbg_resp_lane_o - lane0) == uint32_t(lanes),
          "B1: the block counted %u lane writes, the memory saw %zu",
          uint32_t(d->dbg_resp_lane_o - lane0), lanes);
    CHECK(d->dbg_resp_fault_o == 0 && d->dbg_resp_err_o == 0,
          "B1: a clean response reported fault %u", (unsigned)d->dbg_resp_fault_o);

    // ---- B2: the bytes on the wire ARE the bytes in main memory ----------
    // an independent observation: the payload is compared against the model's
    // own memory image, not against the DUT's account of it
    bool same = !got.empty();
    for (size_t i = 0; i < pld && same; ++i)
      if (h.rmem[12 + i] != got[38 + i]) same = false;
    CHECK(same, "B2: the emitted payload is not the image left in main memory");

    // ---- B3: a zero-strobe byte is never modified ------------------------
    // buffer bytes 8..11 belong to the first lane but to the µCPU's discarded
    // header record, so they carry no strobe and must survive untouched
    CHECK(h.rmem[8] == 0xC3 && h.rmem[9] == 0xC3 && h.rmem[10] == 0xC3 &&
          h.rmem[11] == 0xC3,
          "B3: a byte whose write strobe was 0 was modified in memory "
          "(%02x %02x %02x %02x)", h.rmem[8], h.rmem[9], h.rmem[10],
          h.rmem[11]);

    // ---- B4: the measured cost of the worst response we can build --------
    // IEEE 1722.1 §9.2.1.1 gives a command 100 ms; P-CLK-HZ is 100 MHz, so the
    // budget is 10,000,000 clocks. Measure, do not assume.
    h.rmem.assign(RESP_BYTES, 0xC3);
    t0 = h.t;
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0000, 0), 0xC002);
    vluint64_t cost = h.aecp_rx_t - t0;
    std::vector<uint8_t> epl(4, 0);
    putbe(&epl[0], CFGIX, 2);
    epl.insert(epl.end(), desc_entity.begin(), desc_entity.end());
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID,
                            0xC002, AEM_READ_DESCRIPTOR, epl),
          "B4: the 312-byte descriptor response is not byte-exact");
    printf("  [B4] command byte 0 -> response byte 0: %llu clocks "
           "(%.3f %% of the 10,000,000-clock AECP budget)\n",
           (unsigned long long)cost, 100.0 * double(cost) / 10.0e6);
    CHECK(cost < 10000000ull,
          "B4: %llu clocks blows the 100 ms AECP budget",
          (unsigned long long)cost);
    CHECK(cost < 40000ull,
          "B4: %llu clocks — the memory-backed response path regressed",
          (unsigned long long)cost);

    // ---- B4b: the same, at the REFERENCE SoC's measured memory latency ---
    // docs/architecture/07 §3.3 records ~1424 ns on a miss to main memory,
    // which is 143 clocks at P-CLK-HZ = 100 MHz. This is the number the
    // "latency is free" claim actually rests on, so it is measured here
    // rather than argued.
    h.rmem.assign(RESP_BYTES, 0xC3);
    h.rmem_rlat = 143; h.rmem_wlat = 143;
    uint64_t rw1 = h.rm_writes;
    t0 = h.t;
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0000, 0), 0xC002);
    vluint64_t cost143 = h.aecp_rx_t - t0;
    uint64_t lanes143 = h.rm_writes - rw1;
    h.rmem_rlat = 23; h.rmem_wlat = 17;
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID,
                            0xC002, AEM_READ_DESCRIPTOR, epl),
          "B4b: the response is not byte-exact at 1424 ns memory latency");
    printf("  [B4b] at 143 clocks (~1424 ns) per access: %llu clocks for a "
           "%zu-byte payload over %llu lane writes (%.3f %% of budget)\n",
           (unsigned long long)cost143, epl.size(),
           (unsigned long long)lanes143, 100.0 * double(cost143) / 10.0e6);
    CHECK(cost143 < 10000000ull,
          "B4b: %llu clocks blows the 100 ms AECP budget",
          (unsigned long long)cost143);

    // ---- B5: an ECHOED payload never touches the response memory ---------
    // §9.3.5.3.3's echo is the command verbatim and the command is still in
    // its RX slot: staging it through main memory would be pure waste
    rq0 = h.rm_reqs; rw0 = h.rm_writes;
    std::vector<uint8_t> sr_pl(4, 0);
    putbe(&sr_pl[0], 0x0002, 2);
    got = cmd(AEM_WRITE_DESCRIPTOR, sr_pl, 0xC003);
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_NOT_IMPLEMENTED, EID,
                            CTLR_EID, 0xC003, AEM_WRITE_DESCRIPTOR, sr_pl),
          "B5: the echoed NOT_IMPLEMENTED response is not byte-exact");
    CHECK(h.rm_reqs == rq0,
          "B5: an echoed payload cost %llu response-memory read bursts",
          (unsigned long long)(h.rm_reqs - rq0));
    CHECK(h.rm_writes == rw0,
          "B5: an echoed payload cost %llu response-memory writes",
          (unsigned long long)(h.rm_writes - rw0));

    // ---- B6: a TIED-OFF response master is a legal wiring ----------------
    // the contract says so, so the failure must be a well-formed
    // ENTITY_MISBEHAVING answer (IEEE §7.4 status 10) — never silence, never a
    // SUCCESS carrying bytes nobody read, never a leaked slot
    uint32_t rerr0 = d->dbg_resp_err_o;
    uint16_t rsp0  = d->dbg_aecp_resp_o;
    h.rmem_off = true;
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0024, 0), 0xC004);
    h.rmem_off = false;
    CHECK(!got.empty(), "B6: a dead response memory answered with silence");
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, 10, EID, CTLR_EID, 0xC004,
                            AEM_READ_DESCRIPTOR, {}),
          "B6: the ENTITY_MISBEHAVING answer is not byte-exact");
    if (!got.empty() &&
        got != aecp_frame(CTLR_MAC, OWN_MAC, 1, 10, EID, CTLR_EID, 0xC004,
                          AEM_READ_DESCRIPTOR, {})) dump("got ", got);
    CHECK(d->dbg_resp_err_o == rerr0 + 1,
          "B6: the voided response was not counted (%u -> %u)", rerr0,
          (unsigned)d->dbg_resp_err_o);
    CHECK(d->dbg_resp_fault_o != 0,
          "B6: no fault code for a memory that never answered");
    CHECK(d->dbg_aecp_resp_o == rsp0 + 1,
          "B6: the response counter did not move");
    // the wire only ever says ENTITY_MISBEHAVING; the snapshot window is
    // where an integrator learns WHICH channel of the bridge failed
    CHECK((h.snap(36) & 7u) == (uint32_t)d->dbg_resp_fault_o,
          "B6: snapshot word 36 does not publish the fault code");
    CHECK((h.snap(35) >> 16) == (uint32_t)d->dbg_resp_err_o,
          "B6: snapshot word 35 does not publish the voided-response count");

    // ---- B7: it heals with no reset --------------------------------------
    h.rmem.assign(RESP_BYTES, 0xC3);
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0000, 0), 0xC005);
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID,
                            0xC005, AEM_READ_DESCRIPTOR, epl),
          "B7: the next response after a memory fault is not byte-exact");
    CHECK(d->dbg_resp_fault_o == 0,
          "B7: the fault code stuck across responses");

    // ---- B8: a bridge that reports a WRITE error voids the response too --
    rerr0 = d->dbg_resp_err_o;
    h.rmem_werr = true;
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0024, 0), 0xC006);
    h.rmem_werr = false;
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, 10, EID, CTLR_EID, 0xC006,
                            AEM_READ_DESCRIPTOR, {}),
          "B8: a write error did not degrade to ENTITY_MISBEHAVING");
    CHECK(d->dbg_resp_err_o == rerr0 + 1, "B8: the write error was not counted");

    // ---- B9: a bridge that reports a READ error, likewise ----------------
    rerr0 = d->dbg_resp_err_o;
    h.rmem_rerr = true;
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0024, 0), 0xC007);
    h.rmem_rerr = false;
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, 10, EID, CTLR_EID, 0xC007,
                            AEM_READ_DESCRIPTOR, {}),
          "B9: a read error did not degrade to ENTITY_MISBEHAVING");
    CHECK(d->dbg_resp_err_o == rerr0 + 1, "B9: the read error was not counted");

    // ---- B10: none of that silted up a slot ------------------------------
    h.rmem.assign(RESP_BYTES, 0xC3);
    for (uint16_t k = 0; k < 6; ++k) {
      got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0024, 0),
                uint16_t(0xC010 + k));
      CHECK(!got.empty() && got.size() == 38 + pld,
            "B10: command %u after the fault run went unanswered", k);
    }
    h.run_ms(60);
    CHECK(((h.snap(25) >> 3) & 0xFFFFu) == 4u,
          "B10: %u of 4 RX slots free after the fault run",
          (h.snap(25) >> 3) & 0xFFFFu);

    // ---- B11: a slow memory only costs TIME ------------------------------
    // the same command against a bridge four times slower must produce the
    // same bytes: latency is not a correctness parameter
    h.rmem.assign(RESP_BYTES, 0xC3);
    h.rmem_rlat = 97; h.rmem_wlat = 71;
    got = cmd(AEM_READ_DESCRIPTOR, rdesc_pl(CFGIX, 0x0000, 0), 0xC020);
    h.rmem_rlat = 23; h.rmem_wlat = 17;
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID,
                            0xC020, AEM_READ_DESCRIPTOR, epl),
          "B11: a slower memory changed the bytes on the wire");
  }

  // ==== K. GET_COUNTERS end to end (06 §6.6; IEEE §7.4.42, Milan §5.4.2.25)
  // Milan v1.2 §5.3.8.10 makes the Table 5.6 counters mandatory "for each
  // Stream Input", §5.4.2.25 makes GET_COUNTERS the way to read them, and
  // la_avdecc's s_MilanMandatoryStreamInputCounters (Milan 1.3 Clause 5.3.8.10
  // in its own comment) is that set exactly: MEDIA_LOCKED, MEDIA_UNLOCKED,
  // STREAM_INTERRUPTED, SEQ_NUM_MISMATCH, MEDIA_RESET, TIMESTAMP_UNCERTAIN,
  // UNSUPPORTED_FORMAT, LATE_TIMESTAMP, EARLY_TIMESTAMP, FRAMES_RX — mask
  // 0x00000F3F. A STREAM_INPUT answer missing one bit of it costs the entity
  // its Milan compatibility flag, so that mask is a check of its own below.
  {
    const uint16_t DT_ENTITY = 0x0000, DT_STREAM_INPUT = 0x0005;
    const uint32_t MILAN_MANDATORY_SI = 0x00000F3Fu;

    auto cmd = [&](uint16_t op, const std::vector<uint8_t>& pl, uint16_t seq) {
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, seq, op, pl));
      return h.wait_any(h.q_aecp, 400);
    };
    auto ctr_pl = [](uint16_t ty, uint16_t ix) {
      std::vector<uint8_t> p(4, 0);
      putbe(&p[0], ty, 2); putbe(&p[2], ix, 2);
      return p;
    };
    //! the model's own §7.4.42.2 payload: descriptor_type, descriptor_index,
    //! counters_valid, then THIRTY-TWO quadlets, built from the store the
    //! harness plays — never from anything the DUT emitted
    auto ctr_expect_pl = [&](uint16_t ty, uint16_t ix) {
      std::vector<uint8_t> p(136, 0);
      putbe(&p[0], ty, 2);
      putbe(&p[2], ix, 2);
      putbe(&p[4], H::ctr_mask(ty, ix), 4);
      for (int n = 0; n < 32; ++n)
        putbe(&p[8 + 4 * n], H::ctr_value(ty, ix, uint8_t(n)), 4);
      return p;
    };
    auto expect = [&](uint8_t status, uint16_t op, uint16_t seq,
                      const std::vector<uint8_t>& pl) {
      return aecp_frame(CTLR_MAC, OWN_MAC, 1, status, EID, CTLR_EID, seq, op,
                        pl);
    };
    auto valid_mask_of = [](const std::vector<uint8_t>& f) {
      return (f.size() < 46) ? 0u
           : (uint32_t(f[42]) << 24 | uint32_t(f[43]) << 16 |
              uint32_t(f[44]) << 8  | uint32_t(f[45]));
    };

    // ---- K1: STREAM_INPUT 0, byte-exact, and the size the figure fixes ----
    auto got = cmd(AEM_GET_COUNTERS, ctr_pl(DT_STREAM_INPUT, 0), 0xD001);
    auto want = expect(AECP_SUCCESS, AEM_GET_COUNTERS, 0xD001,
                       ctr_expect_pl(DT_STREAM_INPUT, 0));
    CHECK(!got.empty(), "K1: no GET_COUNTERS response came back");
    // Figure 7-67 runs the block to byte 156, so the AECPDU is 160 B and the
    // frame 174 B; Hive reports a short one as "Incorrect payload size"
    CHECK(got.size() == 38 + 136, "K1: response is %zu B, want %d",
          got.size(), 38 + 136);
    CHECK(got == want, "K1: GET_COUNTERS response is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    CHECK(got.size() > 17 && ((got[16] & 0x07) << 8 | got[17]) == 148,
          "K1: control_data_length is %u, want 148",
          got.size() > 17 ? ((got[16] & 0x07) << 8 | got[17]) : 0);

    // ---- K2: the Milan mandatory set is PRESENT (the la_avdecc gate) ------
    CHECK((valid_mask_of(got) & MILAN_MANDATORY_SI) == MILAN_MANDATORY_SI,
          "K2: STREAM_INPUT 0 counters_valid 0x%08x misses Milan Table 5.16",
          valid_mask_of(got));

    // ---- K3: index 1 is a DIFFERENT object, read from AECPDU @26 ---------
    // §7.4.42.1 puts descriptor_index at @26 where READ_DESCRIPTOR puts a
    // reserved field; reading the wrong offset answers index 0's counters for
    // every index, which is the failure this check exists to catch
    got = cmd(AEM_GET_COUNTERS, ctr_pl(DT_STREAM_INPUT, 1), 0xD002);
    want = expect(AECP_SUCCESS, AEM_GET_COUNTERS, 0xD002,
                  ctr_expect_pl(DT_STREAM_INPUT, 1));
    CHECK(got == want, "K3: STREAM_INPUT 1 response is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    CHECK(valid_mask_of(got) == H::CTR_MASK_CRF,
          "K3: mask 0x%08x, want the store's 0x%08x — the processor must "
          "carry the mask it is given, not one of its own",
          valid_mask_of(got), H::CTR_MASK_CRF);
    CHECK((valid_mask_of(got) & MILAN_MANDATORY_SI) == MILAN_MANDATORY_SI,
          "K3: STREAM_INPUT 1 misses Milan Table 5.16");
    // TIMESTAMP_VALID / TIMESTAMP_NOT_VALID are block quadlets 6 and 7; this
    // object keeps neither, so both the mask bits and the quadlets are 0
    CHECK(got.size() >= 174 &&
          (valid_mask_of(got) & 0x000000C0u) == 0 &&
          got[46 + 24] == 0 && got[46 + 28] == 0,
          "K3: an unclaimed counter still put bytes in the block");

    // ---- K4: ENTITY — NOT_SUPPORTED in the FULL fixed body ----------------
    // Table 7-150 gives the ENTITY descriptor nothing but ENTITY_SPECIFIC
    // bits and Milan makes none mandatory, so the target refuses Table
    // 7-141's NOT_SUPPORTED - carried in the full Figure 7-67 body (zero
    // mask, zero block, cdl 148), because the reference stack reflects ONLY
    // NOT_IMPLEMENTED at command length and sizes every other non-success
    // answer against the response form (la_avdecc checkResponsePayload;
    // the r49a probe's "Incorrect payload size" complaint was the old
    // command-sized echo here). The supported set stays exactly
    // {STREAM_INPUT, STREAM_OUTPUT, AVB_INTERFACE, CLOCK_DOMAIN}.
    {
      got = cmd(AEM_GET_COUNTERS, ctr_pl(DT_ENTITY, 0), 0xD003);
      want = expect(11, AEM_GET_COUNTERS, 0xD003,
                    ctr_expect_pl(DT_ENTITY, 0));
      CHECK(got == want,
            "K4: ENTITY refuses NOT_SUPPORTED in the full zero-flagged body");
      if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
      CHECK(got.size() == 38 + 136,
            "K4: the refusal still owes the fixed 160-byte AECPDU, got %zu B",
            got.size());
    }
    // ...and STREAM_OUTPUT is a supported target. Milan Table 5.17 compacts
    // its five counters into quadlets 0..4, so the integrator's mask is 0x1F.
    {
      for (uint16_t ix = 0; ix < 2; ix++) {
        got = cmd(AEM_GET_COUNTERS, ctr_pl(0x0006, ix), 0xD00B + ix);
        want = expect(AECP_SUCCESS, AEM_GET_COUNTERS, 0xD00B + ix,
                      ctr_expect_pl(0x0006, ix));
        CHECK(got == want,
              "K4b: STREAM_OUTPUT %u carries its byte-exact block", ix);
        if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
        CHECK(valid_mask_of(got) == H::CTR_MASK_SOUT,
              "K4b: STREAM_OUTPUT %u mask 0x%08x, want 0x%08x",
              ix, valid_mask_of(got), H::CTR_MASK_SOUT);
        CHECK(got.size() >= 174 && got[46] == 0xC0 && got[62] == 0xC0,
              "K4b: output %u keeps START and FRAMES_TX at quadlets 0 and 4", ix);
      }
    }
    // ...while the OTHER two supported types keep their old answers - this
    // TB's face backs only STREAM_INPUT, so both come back SUCCESS with an
    // empty mask over a real, located object (the parent's [CTRS2] proves
    // the real masks)
    {
      got = cmd(AEM_GET_COUNTERS, ctr_pl(0x0009, 0), 0xD00C);
      want = expect(AECP_SUCCESS, AEM_GET_COUNTERS, 0xD00C,
                    ctr_expect_pl(0x0009, 0));
      CHECK(got == want, "K4c: AVB_INTERFACE 0 stays SUCCESS (empty here)");
      got = cmd(AEM_GET_COUNTERS, ctr_pl(0x0024, 0), 0xD00D);
      want = expect(AECP_SUCCESS, AEM_GET_COUNTERS, 0xD00D,
                    ctr_expect_pl(0x0024, 0));
      CHECK(got == want, "K4d: CLOCK_DOMAIN 0 stays SUCCESS (empty here)");
    }
    // ---- K4e: a NONEXISTENT index refuses NO_SUCH_DESCRIPTOR --------------
    // (the probe's first strictness rule: Table 7-141 "A descriptor with the
    //  descriptor_type and descriptor_index specified does not exist"). The
    //  fixed Figure 7-67 body still emits, all zero, and the counters face
    //  is NEVER consulted about an object the store refused.
    {
      uint64_t reads0 = h.ctr_reads;
      got = cmd(AEM_GET_COUNTERS, ctr_pl(DT_STREAM_INPUT, 2), 0xD00E);
      want = expect(AECP_NO_SUCH_DESCRIPTOR, AEM_GET_COUNTERS, 0xD00E,
                    ctr_expect_pl(DT_STREAM_INPUT, 2));
      CHECK(got == want,
            "K4e: index 2 is NO_SUCH_DESCRIPTOR with the zero-flagged body");
      if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
      CHECK(h.ctr_reads == reads0,
            "K4e: the face was asked about a nonexistent object");
    }
    {
      uint64_t reads0 = h.ctr_reads;
      got = cmd(AEM_GET_COUNTERS, ctr_pl(0x0006, 2), 0xD00F);
      want = expect(AECP_NO_SUCH_DESCRIPTOR, AEM_GET_COUNTERS, 0xD00F,
                    ctr_expect_pl(0x0006, 2));
      CHECK(got == want,
            "K4f: Stream Output 2 is NO_SUCH_DESCRIPTOR with the full body");
      if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
      CHECK(h.ctr_reads == reads0,
            "K4f: the face was asked about a nonexistent Stream Output");
    }

    // ---- K5: a truncated GET_COUNTERS is BAD_ARGUMENTS -------------------
    // §7.4.42.1's command is descriptor_type + descriptor_index; answering
    // ENTITY-index-0 out of the zeros that follow a short header is a silent
    // misinterpretation, exactly as it is for a truncated READ_DESCRIPTOR
    std::vector<uint8_t> short_pl(2, 0);
    got = cmd(AEM_GET_COUNTERS, short_pl, 0xD004);
    want = expect(AECP_BAD_ARGUMENTS, AEM_GET_COUNTERS, 0xD004, short_pl);
    CHECK(!got.empty(), "K5: a truncated GET_COUNTERS got no answer");
    CHECK(got == want, "K5: truncated-command answer is not byte-exact");

    // ---- K6: the store's back-pressure is not a correctness parameter ----
    int hold0 = h.ctr_hold;
    h.ctr_hold = 0;
    auto fast = cmd(AEM_GET_COUNTERS, ctr_pl(DT_STREAM_INPUT, 0), 0xD005);
    h.ctr_hold = 11;
    auto slow = cmd(AEM_GET_COUNTERS, ctr_pl(DT_STREAM_INPUT, 0), 0xD005);
    h.ctr_hold = hold0;
    CHECK(!fast.empty() && fast == slow,
          "K6: an 11-cycle hold per quadlet changed the bytes on the wire");
    CHECK(fast == expect(AECP_SUCCESS, AEM_GET_COUNTERS, 0xD005,
                         ctr_expect_pl(DT_STREAM_INPUT, 0)),
          "K6: the zero-hold run is not byte-exact either");

    // ---- K7: a WEDGED store must not take the descriptor path with it ----
    // ctr_wait_i held forever is the one way this face can stop a command
    // retiring, and the µCPU it stops is the same one READ_DESCRIPTOR runs on
    uint16_t rerr0 = d->dbg_resp_err_o;
    h.ctr_stuck = true;
    got = cmd(AEM_GET_COUNTERS, ctr_pl(DT_STREAM_INPUT, 0), 0xD006);
    h.ctr_stuck = false;
    CHECK(!got.empty(), "K7: a wedged counter store hung the AECP engine");
    CHECK(got == expect(10 /* ENTITY_MISBEHAVING, §7.4 status 10 */,
                        AEM_GET_COUNTERS, 0xD006, {}),
          "K7: the voided response is not the bare ENTITY_MISBEHAVING answer");
    CHECK(d->dbg_resp_err_o == uint16_t(rerr0 + 1),
          "K7: the voided response was not counted");
    // and the crown jewel still works
    std::vector<uint8_t> rd(8, 0);
    putbe(&rd[0], CFGIX, 2); putbe(&rd[4], 0x0000, 2); putbe(&rd[6], 0, 2);
    got = cmd(AEM_READ_DESCRIPTOR, rd, 0xD007);
    std::vector<uint8_t> epl(4, 0);
    putbe(&epl[0], CFGIX, 2);
    epl.insert(epl.end(), desc_entity.begin(), desc_entity.end());
    CHECK(got == expect(AECP_SUCCESS, AEM_READ_DESCRIPTOR, 0xD007, epl),
          "K7: READ_DESCRIPTOR regressed after a counters-face timeout");

    // ---- K8: every quadlet, once, in order, and none under back-pressure --
    // The counters_valid word first (it is emitted at @28, before the block),
    // then quadlets 0..31 in Table 7-157 offset order. Folding consecutive
    // repeats is deliberate: the face is a level and a held write re-asks the
    // same word, which is harmless — a MOVING index under a held write is not,
    // and is exactly what a beat counter that advances while the buffer says
    // no would produce
    h.ctr_seq.clear();
    got = cmd(AEM_GET_COUNTERS, ctr_pl(DT_STREAM_INPUT, 0), 0xD008);
    std::vector<uint8_t> want_seq;
    want_seq.push_back(32);
    for (int n = 0; n < 32; ++n) want_seq.push_back(uint8_t(n));
    CHECK(!got.empty() && h.ctr_seq == want_seq,
          "K8: the store was asked for %zu distinct quadlets in this order, "
          "want the mask then 0..31", h.ctr_seq.size());
  }

  // ==== Q. GET_AUDIO_MAP end to end (06 §6.5; IEEE §7.4.44, Milan §5.4.2.26)
  // Milan v1.2 §5.3.3.9 forbids AUDIO_MAP descriptors on every Stream Port
  // Input ("The Stream Port Input of a Configuration shall not contain any
  // AUDIO_MAP descriptor"), so a Milan input's mappings are ONLY reachable
  // through this command - a strict controller that reads NOT_IMPLEMENTED
  // here sees no mappings at all and fails enumeration. §5.4.2.26 fixes the
  // paging ("The PAAD-AE shall always return N in the number_of_maps field
  // ... no matter the actual count of dynamic mappings") and §7.4.44.1 the
  // page bound ("If the map_index is beyond the range of available maps then
  // it returns a BAD_ARGUMENT status").
  {
    const uint16_t AEM_GET_AUDIO_MAP = 0x002B;
    const uint16_t DT_SPI = 0x000E, DT_SPO = 0x000F;

    auto cmd = [&](uint16_t op, const std::vector<uint8_t>& pl, uint16_t seq) {
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, seq, op, pl));
      return h.wait_any(h.q_aecp, 400);
    };
    auto am_pl = [](uint16_t ty, uint16_t ix, uint16_t page,
                    uint16_t rsvd = 0) {
      std::vector<uint8_t> p(8, 0);
      putbe(&p[0], ty, 2); putbe(&p[2], ix, 2);
      putbe(&p[4], page, 2); putbe(&p[6], rsvd, 2);
      return p;
    };
    //! the model's own §7.4.44.2 payload, built from the store the harness
    //! plays - never from anything the DUT emitted
    auto am_expect_pl = [&](uint16_t ty, uint16_t ix, uint16_t page,
                            uint16_t nmaps, uint16_t cnt) {
      std::vector<uint8_t> p(12 + 8 * size_t(cnt), 0);
      putbe(&p[0], ty, 2);  putbe(&p[2], ix, 2);
      putbe(&p[4], page, 2); putbe(&p[6], nmaps, 2);
      putbe(&p[8], cnt, 2);                        // reserved @10 stays 0
      for (uint16_t k = 0; k < cnt; ++k)
        putbe(&p[12 + 8 * size_t(k)], H::amap_rec(ty, ix, page, uint8_t(k)), 8);
      return p;
    };
    auto expect = [&](uint8_t status, uint16_t seq,
                      const std::vector<uint8_t>& pl) {
      return aecp_frame(CTLR_MAC, OWN_MAC, 1, status, EID, CTLR_EID, seq,
                        AEM_GET_AUDIO_MAP, pl);
    };

    // ---- Q1: port 0's one page, byte-exact with both records --------------
    auto got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 0, 0), 0xE001);
    auto want = expect(AECP_SUCCESS, 0xE001,
                       am_expect_pl(DT_SPI, 0, 0, 1, 2));
    CHECK(!got.empty(), "Q1: no GET_AUDIO_MAP response came back");
    CHECK(got == want, "Q1: GET_AUDIO_MAP response is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    CHECK(got.size() > 17 && ((got[16] & 0x07) << 8 | got[17]) == 24 + 16,
          "Q1: control_data_length is %u, want 40",
          got.size() > 17 ? ((got[16] & 0x07) << 8 | got[17]) : 0);

    // ---- Q2: the §5.4.2.26 partition - three pages, each its own content --
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 1, 1), 0xE002);
    want = expect(AECP_SUCCESS, 0xE002, am_expect_pl(DT_SPI, 1, 1, 3, 3));
    CHECK(got == want, "Q2: page 1 of port 1 is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 1, 2), 0xE003);
    want = expect(AECP_SUCCESS, 0xE003, am_expect_pl(DT_SPI, 1, 2, 3, 1));
    CHECK(got == want, "Q2: page 2 of port 1 is not byte-exact");
    //! an EMPTY page is SUCCESS with number_of_mappings 0 and the full fixed
    //! part - §5.4.2.26: "will return 0 mapping ... if there is no dynamic
    //! mapping referencing the Audio Clusters' channels which are in subset P"
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 1, 0), 0xE004);
    want = expect(AECP_SUCCESS, 0xE004, am_expect_pl(DT_SPI, 1, 0, 3, 0));
    CHECK(got == want, "Q2: the EMPTY page 0 is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- Q3: map_index = N is BAD_ARGUMENTS, with the REAL N still told ---
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 1, 3), 0xE005);
    want = expect(AECP_BAD_ARGUMENTS, 0xE005,
                  am_expect_pl(DT_SPI, 1, 3, 3, 0));
    CHECK(got == want, "Q3: page N answer is not the BAD_ARGUMENTS stub");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- Q4: an index past the image is NO_SUCH_DESCRIPTOR ----------------
    // The IMAGE is the existence authority (E_GAMAP locates the descriptor
    // in the same store READ_DESCRIPTOR serves), so GET_AUDIO_MAP refuses
    // exactly the indices READ_DESCRIPTOR refuses
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 2, 0), 0xE006);
    want = expect(AECP_NO_SUCH_DESCRIPTOR, 0xE006,
                  am_expect_pl(DT_SPI, 2, 0, 0, 0));
    CHECK(got == want, "Q4: index-past-the-image answer is not the "
          "NO_SUCH_DESCRIPTOR stub");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- Q5: STREAM_PORT_OUTPUT is served off the capture-side store ------
    // Milan §5.4.2.26's second half ("for each Stream Port Output of the
    // currently set Configuration"), un-gapped now that the capture map RAM
    // has a readback: same paging law, same stubs, records keyed to the
    // OUTPUT direction so a render-side answer cannot pass.
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPO, 0, 0), 0xE007);
    want = expect(AECP_SUCCESS, 0xE007, am_expect_pl(DT_SPO, 0, 0, 2, 4));
    CHECK(got == want, "Q5: OUTPUT port 0 page 0 is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPO, 0, 1), 0xE00B);
    want = expect(AECP_SUCCESS, 0xE00B, am_expect_pl(DT_SPO, 0, 1, 2, 1));
    CHECK(got == want, "Q5b: OUTPUT port 0 page 1 is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPO, 0, 2), 0xE00C);
    want = expect(AECP_BAD_ARGUMENTS, 0xE00C, am_expect_pl(DT_SPO, 0, 2, 2, 0));
    CHECK(got == want, "Q5c: OUTPUT page law still §7.4.44.1");
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPO, 3, 0), 0xE00D);
    want = expect(AECP_NO_SUCH_DESCRIPTOR, 0xE00D,
                  am_expect_pl(DT_SPO, 3, 0, 0, 0));
    CHECK(got == want, "Q5d: OUTPUT existence still the image's");
    //! ...and a type that is NEITHER port direction keeps the echo
    auto ju_pl = am_pl(0x0002, 0, 0);            // AUDIO_UNIT
    got = cmd(AEM_GET_AUDIO_MAP, ju_pl, 0xE00E);
    want = expect(AECP_NOT_IMPLEMENTED, 0xE00E, ju_pl);
    CHECK(got == want, "Q5e: a non-port type keeps the NOT_IMPLEMENTED echo");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- Q6: a truncated command is BAD_ARGUMENTS -------------------------
    // §7.4.44.1's command runs through the reserved word; shorter never
    // carried map_index, and answering page 0 out of residual zeros would be
    // a silent misinterpretation (the K5 reasoning)
    std::vector<uint8_t> short_pl(6, 0);
    putbe(&short_pl[0], DT_SPI, 2); putbe(&short_pl[2], 0, 2);
    got = cmd(AEM_GET_AUDIO_MAP, short_pl, 0xE008);
    want = expect(AECP_BAD_ARGUMENTS, 0xE008, short_pl);
    CHECK(!got.empty(), "Q6: a truncated GET_AUDIO_MAP got no answer");
    CHECK(got == want, "Q6: truncated-command answer is not byte-exact");

    // ---- Q7: the reserved word must not become the port -------------------
    // §7.4.44.1 @30..@31 is reserved; the engine's payload walk shares its
    // registers with READ_DESCRIPTOR's shape, whose @30 IS descriptor_index,
    // so an unguarded walk would answer about port 0xBEEF while the
    // controller asked about port 1 - same class as the K-series padded-
    // command guard, and the check that makes the walk guard load-bearing
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 1, 2, 0xBEEF), 0xE009);
    want = expect(AECP_SUCCESS, 0xE009, am_expect_pl(DT_SPI, 1, 2, 3, 1));
    CHECK(got == want,
          "Q7: a nonzero reserved word changed the addressed port");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

    // ---- Q8: the store's back-pressure is not a correctness parameter -----
    int hold0 = h.amap_hold;
    h.amap_hold = 0;
    auto fast = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 1, 1), 0xE00A);
    h.amap_hold = 11;
    auto slow = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 1, 1), 0xE00A);
    h.amap_hold = hold0;
    CHECK(!fast.empty() && fast == slow,
          "Q8: an 11-cycle hold per word changed the bytes on the wire");
    CHECK(fast == expect(AECP_SUCCESS, 0xE00A,
                         am_expect_pl(DT_SPI, 1, 1, 3, 3)),
          "Q8: the zero-hold run is not byte-exact either");

    // ---- Q9: a WEDGED store must not take the descriptor path with it -----
    uint16_t rerr0 = d->dbg_resp_err_o;
    h.amap_stuck = true;
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 0, 0), 0xE00B);
    h.amap_stuck = false;
    CHECK(!got.empty(), "Q9: a wedged audio-map store hung the AECP engine");
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, 10 /* ENTITY_MISBEHAVING */,
                            EID, CTLR_EID, 0xE00B, AEM_GET_AUDIO_MAP, {}),
          "Q9: the voided response is not the bare ENTITY_MISBEHAVING answer");
    CHECK(d->dbg_resp_err_o == uint16_t(rerr0 + 1),
          "Q9: the voided response was not counted");
    // the crown jewel still works - and so does the audio map after it
    std::vector<uint8_t> rd(8, 0);
    putbe(&rd[0], CFGIX, 2); putbe(&rd[4], 0x000E, 2); putbe(&rd[6], 1, 2);
    got = cmd(AEM_READ_DESCRIPTOR, rd, 0xE00C);
    std::vector<uint8_t> epl(4, 0);
    putbe(&epl[0], CFGIX, 2);
    epl.insert(epl.end(), desc_spi1.begin(), desc_spi1.end());
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                            CTLR_EID, 0xE00C, AEM_READ_DESCRIPTOR, epl),
          "Q9: READ_DESCRIPTOR regressed after an audio-map-face timeout");
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 0, 0), 0xE00D);
    CHECK(got == expect(AECP_SUCCESS, 0xE00D, am_expect_pl(DT_SPI, 0, 0, 1, 2)),
          "Q9: GET_AUDIO_MAP itself regressed after its own face timeout");

    // ---- Q10: the query order and the record ordinal ----------------------
    // NMAPS then GEOM then records 0..count-1, and the ordinal RESTARTS per
    // command - a counter that survived a command would serve page 1's third
    // record as the next command's first
    h.amap_seq.clear();
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 1, 1), 0xE00E);
    std::vector<std::pair<uint8_t, uint8_t>> want_q =
        {{0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2}};
    CHECK(!got.empty() && h.amap_seq == want_q,
          "Q10: %zu distinct store queries, want NMAPS, GEOM, then records "
          "0..2", h.amap_seq.size());
    h.amap_seq.clear();
    got = cmd(AEM_GET_AUDIO_MAP, am_pl(DT_SPI, 0, 0), 0xE00F);
    want_q = {{0, 0}, {1, 0}, {2, 0}, {2, 1}};
    CHECK(!got.empty() && h.amap_seq == want_q,
          "Q10: the record ordinal did not restart with the command");
  }

  // ==== R. ADD/REMOVE_AUDIO_MAPPINGS =====================================
  // IEEE 1722.1-2021 7.4.45 and 7.4.46 require an exact reflected body,
  // whole-command validation before any write, duplicate-safe removal, lock
  // ordering, and a notification after every successful command.
  {
    const uint16_t GET = 0x002B, ADD = 0x002C, REMOVE = 0x002D;
    const uint16_t DT_SPI = 0x000E, DT_SPO = 0x000F;
    const uint64_t C2_MAC = 0x0202C2C2C2C2ull;
    auto row = [](uint16_t si, uint16_t sc, uint16_t co, uint16_t cc = 0) {
      return (uint64_t(si) << 48) | (uint64_t(sc) << 32)
           | (uint64_t(co) << 16) | cc;
    };
    auto edit_pl = [](uint16_t ty, uint16_t ix,
                      const std::vector<uint64_t>& rows) {
      std::vector<uint8_t> p(8 + 8 * rows.size(), 0);
      putbe(&p[0], ty, 2); putbe(&p[2], ix, 2);
      putbe(&p[4], rows.size(), 2);
      for (size_t i = 0; i < rows.size(); ++i)
        putbe(&p[8 + 8 * i], rows[i], 8);
      return p;
    };
    auto cmd_from = [&](uint64_t mac, uint64_t eid, uint16_t op,
                        uint16_t seq, const std::vector<uint8_t>& p) {
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, mac, 0, 0, EID, eid,
                        seq, op, p));
      return h.wait_any(h.q_aecp, 400);
    };
    auto cmd = [&](uint16_t op, uint16_t seq,
                   const std::vector<uint8_t>& p) {
      return cmd_from(CTLR_MAC, CTLR_EID, op, seq, p);
    };
    auto expect = [&](uint8_t status, uint16_t op, uint16_t seq,
                      const std::vector<uint8_t>& p) {
      return aecp_frame(CTLR_MAC, OWN_MAC, 1, status, EID, CTLR_EID,
                        seq, op, p);
    };
    auto status = [](const std::vector<uint8_t>& f) {
      return f.size() > 16 ? uint8_t(f[16] >> 3) : uint8_t(0xFF);
    };
    auto map_rows = [&](uint16_t ty, uint16_t ix, uint16_t page,
                        uint16_t seq) {
      std::vector<uint8_t> q(8, 0);
      putbe(&q[0], ty, 2); putbe(&q[2], ix, 2); putbe(&q[4], page, 2);
      auto f = cmd(GET, seq, q);
      std::vector<uint64_t> rows;
      if (f.size() < 50 || status(f) != AECP_SUCCESS) return rows;
      uint16_t count = uint16_t((uint16_t(f[46]) << 8) | f[47]);
      for (uint16_t i = 0; i < count && 50 + 8 * size_t(i) + 7 < f.size(); ++i) {
        uint64_t v = 0;
        for (int b = 0; b < 8; ++b) v = (v << 8) | f[50 + 8 * size_t(i) + b];
        rows.push_back(v);
      }
      return rows;
    };

    h.amap_edit_mode = true;
    h.amap_edit_in0.clear(); h.amap_edit_out0.clear(); h.amap_edit_out1.clear();
    h.amap_edit_seq.clear(); h.amap_edit_mutations = 0;

    // A late conflict on cluster 0 rejects the whole command from empty.
    uint64_t c0 = row(0, 0, 0), c1 = row(0, 1, 1);
    auto p = edit_pl(DT_SPI, 0, {c0, c1, row(1, 1, 0)});
    auto got = cmd(ADD, 0xE100, p);
    CHECK(got == expect(AECP_BAD_ARGUMENTS, ADD, 0xE100, p),
          "R1: a conflicting full command did not return BAD_ARGUMENTS");
    CHECK(h.amap_edit_in0.empty()
          && map_rows(DT_SPI, 0, 0, 0xE101).empty(),
          "R1: conflict left a partial input mapping");

    // Fill the complete eight-cluster page, then read it back through GET.
    std::vector<uint64_t> linear;
    for (uint16_t i = 0; i < 8; ++i) linear.push_back(row(0, i, i));
    p = edit_pl(DT_SPI, 0, linear);
    got = cmd(ADD, 0xE102, p);
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE102, p),
          "R2: full-page ADD response is not byte-exact SUCCESS");
    CHECK(map_rows(DT_SPI, 0, 0, 0xE103) == linear,
          "R2: GET_AUDIO_MAP did not return the full committed page");

    uint64_t m0 = h.amap_edit_mutations;
    got = cmd(ADD, 0xE104, p);
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE104, p)
          && h.amap_edit_mutations == m0 && !h.amap_edit_finish_changed,
          "R3: idempotent full-page ADD changed state");

    auto bad_remove = linear;
    bad_remove.push_back(row(1, 0, 7));
    p = edit_pl(DT_SPI, 0, bad_remove);
    got = cmd(REMOVE, 0xE105, p);
    CHECK(got == expect(AECP_BAD_ARGUMENTS, REMOVE, 0xE105, p)
          && map_rows(DT_SPI, 0, 0, 0xE106) == linear,
          "R4: absent REMOVE row caused a partial removal");

    p = edit_pl(DT_SPI, 0, linear);
    got = cmd(REMOVE, 0xE107, p);
    CHECK(got == expect(AECP_SUCCESS, REMOVE, 0xE107, p)
          && map_rows(DT_SPI, 0, 0, 0xE108).empty(),
          "R5: correct full-page REMOVE did not empty the map");
    got = cmd(REMOVE, 0xE109, p);
    CHECK(got == expect(AECP_BAD_ARGUMENTS, REMOVE, 0xE109, p),
          "R6: repeated REMOVE of an empty map did not fail");

    cmd(ADD, 0xE10A, edit_pl(DT_SPI, 0, linear));
    std::vector<uint64_t> duplicates;
    for (uint64_t v : linear) { duplicates.push_back(v); duplicates.push_back(v); }
    p = edit_pl(DT_SPI, 0, duplicates);
    got = cmd(REMOVE, 0xE10B, p);
    CHECK(got == expect(AECP_SUCCESS, REMOVE, 0xE10B, p)
          && h.amap_edit_in0.empty(),
          "R7: duplicated REMOVE rows were not ignored safely");

    p = edit_pl(DT_SPI, 0, {row(0, 0, 0), row(1, 1, 0)});
    got = cmd(ADD, 0xE10C, p);
    CHECK(got == expect(AECP_BAD_ARGUMENTS, ADD, 0xE10C, p)
          && h.amap_edit_in0.empty(),
          "R8: nonredundant input conflict was accepted");

    uint64_t cross = row(0, 0, 0);
    p = edit_pl(DT_SPO, 0, {cross});
    got = cmd(ADD, 0xE10D, p);
    auto p2 = edit_pl(DT_SPO, 1, {cross});
    auto got2 = cmd(ADD, 0xE10E, p2);
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE10D, p)
          && got2 == expect(AECP_BAD_ARGUMENTS, ADD, 0xE10E, p2),
          "R9: cross-port output-channel conflict status is wrong");
    CHECK(map_rows(DT_SPO, 0, 0, 0xE10F) == std::vector<uint64_t>{cross}
          && map_rows(DT_SPO, 1, 0, 0xE110).empty(),
          "R9: cross-port refusal changed either output map");
    cmd(REMOVE, 0xE111, p);

    uint64_t out = row(0, 1, 16);
    p = edit_pl(DT_SPO, 0, {out});
    h.amap_edit_reject_commit = true;
    got = cmd(ADD, 0xE112, p);
    h.amap_edit_reject_commit = false;
    CHECK(got == expect(AECP_BAD_ARGUMENTS, ADD, 0xE112, p),
          "R10: running-output ADD recheck did not refuse the edit");
    CHECK(h.amap_edit_out0.empty(),
          "R10: running-output ADD still wrote the map");
    got = cmd(ADD, 0xE113, p);
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE113, p)
          && H::amap_has(h.amap_edit_out0, out),
          "R10: the same output row did not commit when idle");
    h.amap_edit_reject_commit = true;
    got = cmd(REMOVE, 0xE114, p);
    h.amap_edit_reject_commit = false;
    CHECK(got == expect(AECP_BAD_ARGUMENTS, REMOVE, 0xE114, p)
          && H::amap_has(h.amap_edit_out0, out),
          "R10: running-output REMOVE changed the map");
    cmd(REMOVE, 0xE115, p);

    p = edit_pl(DT_SPI, 1, {});
    got = cmd(ADD, 0xE116, p);
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE116, p),
          "R11a: required dynamic Stream Port Input was not supported");
    p = edit_pl(DT_SPO, 2, {});
    got = cmd(ADD, 0xE122, p);
    CHECK(got == expect(AECP_NOT_SUPPORTED, ADD, 0xE122, p),
          "R11b: static Stream Port Output did not return NOT_SUPPORTED");

    p = edit_pl(DT_SPI, 0, {row(0, 0, 0)});
    putbe(&p[4], 2, 2);                    // count says two, body carries one
    got = cmd(ADD, 0xE117, p);
    auto normalized = edit_pl(DT_SPI, 0, {row(0, 0, 0)});
    CHECK(got == expect(AECP_BAD_ARGUMENTS, ADD, 0xE117, normalized),
          "R12: malformed response did not name its one contained record");

    std::vector<uint8_t> short_edit(4, 0);
    putbe(&short_edit[0], DT_SPI, 2); putbe(&short_edit[2], 0, 2);
    got = cmd(ADD, 0xE128, short_edit);
    CHECK(got == expect(AECP_BAD_ARGUMENTS, ADD, 0xE128,
                        edit_pl(DT_SPI, 0, {})),
          "R12a: short edit response omitted Figure 7-71's fixed body");

    // Reserved command bytes are ignored on receipt and zero on transmission.
    p = edit_pl(DT_SPI, 0, {row(0, 5, 5)});
    p[6] = 0xA5; p[7] = 0x5A;
    normalized = p; normalized[6] = 0; normalized[7] = 0;
    got = cmd(ADD, 0xE126, p);
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE126, normalized)
          && H::amap_has(h.amap_edit_in0, row(0, 5, 5)),
          "R12b: response retransmitted nonzero reserved command bytes");
    got = cmd(REMOVE, 0xE127, normalized);
    CHECK(got == expect(AECP_SUCCESS, REMOVE, 0xE127, normalized),
          "R12b: reserved-field regression cleanup failed");

    p = edit_pl(0x0002, 0, {});            // AUDIO_UNIT is not a Stream Port
    got = cmd(ADD, 0xE118, p);
    CHECK(got == expect(AECP_NOT_SUPPORTED, ADD, 0xE118, p),
          "R13: non-Stream-Port target did not return NOT_SUPPORTED");

    // A lock held by C2 refuses C1 before the transaction face can mutate.
    std::vector<uint8_t> lock(16, 0);
    got = cmd_from(C2_MAC, CTLR2_EID, 0x0001, 0xE119, lock);
    CHECK(status(got) == AECP_SUCCESS, "R14: C2 could not take the lock");
    p = edit_pl(DT_SPI, 0, {row(0, 0, 0)});
    got = cmd(ADD, 0xE11A, p);
    CHECK(status(got) == 3 && h.amap_edit_in0.empty(),
          "R14: foreign locked ADD was not refused as ENTITY_LOCKED");
    lock[3] = 1;
    got = cmd_from(C2_MAC, CTLR2_EID, 0x0001, 0xE11B, lock);
    CHECK(status(got) == AECP_SUCCESS, "R14: C2 could not release the lock");

    // Register C2, then prove changed ADD/REMOVE reflect byte-exact to C2.
    std::vector<uint8_t> flags(4, 0);
    got = cmd_from(C2_MAC, CTLR2_EID, 0x0024, 0xE11C, flags);
    CHECK(status(got) == AECP_SUCCESS, "R15: C2 registration failed");
    uint64_t notice_row = row(0, 2, 2);
    p = edit_pl(DT_SPI, 0, {notice_row});
    got = cmd(ADD, 0xE11D, p);
    auto uns = h.wait_any(h.q_aecp, 400);
    auto want_uns = aecp_frame(C2_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                               CTLR2_EID, 0, ADD, p);
    want_uns[36] |= 0x80;
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE11D, p)
          && uns == want_uns,
          "R15: changed ADD did not notify only C2 with the reflected body");

    got = cmd(ADD, 0xE11E, p);
    uns = h.wait_any(h.q_aecp, 400);
    want_uns = aecp_frame(C2_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                          CTLR2_EID, 1, ADD, p);
    want_uns[36] |= 0x80;
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE11E, p)
          && uns == want_uns,
          "R16: idempotent ADD did not emit the required notification");

    got = cmd(REMOVE, 0xE11F, p);
    uns = h.wait_any(h.q_aecp, 400);
    want_uns = aecp_frame(C2_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                          CTLR2_EID, 2, REMOVE, p);
    want_uns[36] |= 0x80;
    CHECK(got == expect(AECP_SUCCESS, REMOVE, 0xE11F, p)
          && uns == want_uns,
          "R17: changed REMOVE did not notify C2 with sequence 2");
    cmd_from(C2_MAC, CTLR2_EID, 0x0025, 0xE120, {});

    p = edit_pl(DT_SPI, 0, {});
    h.amap_edit_stuck = true;
    got = cmd(ADD, 0xE121, p);
    h.amap_edit_stuck = false;
    CHECK(got == expect(10, ADD, 0xE121, p),
          "R18: wedged edit store omitted the fixed mapping response body");

    std::vector<uint64_t> reserved = {row(0, 3, 3), row(0, 4, 4)};
    p = edit_pl(DT_SPI, 0, reserved);
    uint64_t before = h.amap_edit_mutations;
    h.amap_edit_postcommit_wait = true;
    got = cmd(ADD, 0xE123, p);
    h.amap_edit_postcommit_wait = false;
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE123, p),
          "R19: post-reservation wait poisoned the successful response");
    CHECK(map_rows(DT_SPI, 0, 0, 0xE124) == reserved
          && h.amap_edit_mutations == before + 2,
          "R19: reserved transaction did not commit both records exactly");
    got = cmd(REMOVE, 0xE125, p);
    CHECK(got == expect(AECP_SUCCESS, REMOVE, 0xE125, p),
          "R19: post-reservation regression cleanup failed");

    // Park an output edit at its phase-1 streaming recheck, then inject a
    // state-changing PROBE_TX for source 1. The live MAP_CFG hold must keep
    // STREAM_CFG in the dispatch queue until the complete mapping command
    // retires. Before the scoreboard was wired this probe reached the talker,
    // requested a MAAP address, and opened its declaration while the mapping
    // command was still between validation and write-back.
    uint64_t serialized_out = row(0, 2, 17);
    p = edit_pl(DT_SPO, 0, {serialized_out});
    before = h.amap_edit_mutations;
    h.q_aecp.clear(); h.q_acmp.clear();
    h.decl_edges.clear();
    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID,
                      0xE12E, ADD, p));
    bool at_recheck = false;
    for (int i = 0; i < 400; ++i) {
      if (d->amap_edit_req_o && d->amap_edit_phase_o == 1) {
        at_recheck = true;
        h.amap_edit_hold = 200;
        break;
      }
      h.step();
    }
    CHECK(at_recheck, "R19a: output edit did not reach phase-1 recheck");
    auto probe_during_map = acmp_frame(CTLR_MAC, 0, 0, 0, CTLR_EID, EID,
                                       T1_EID, 1, 7, 0, 0, 0xE12F,
                                       0x000A, 0);
    h.feed(probe_during_map);
    CHECK(((h.snap(15) >> 24) & 0xFF) != 0,
          "R19a: MAP_CFG did not own a live scoreboard hold");
    CHECK(h.amap_edit_mutations == before && h.q_acmp.empty()
          && !h.saw_decl_edge(1, true),
          "R19a: STREAM_CFG crossed the held MAP_CFG recheck");
    h.amap_edit_hold = 0;
    got = h.wait_any(h.q_aecp, 400);
    auto probe_after_map = h.wait_any(h.q_acmp, 400);
    for (int i = 0; i < 400 && !h.saw_decl_edge(1, true); ++i) h.step();
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE12E, p)
          && !probe_after_map.empty()
          && (probe_after_map[15] & 0x0F) == 1
          && h.saw_decl_edge(1, true)
          && H::amap_has(h.amap_edit_out0, serialized_out),
          "R19a: deferred MAP_CFG then STREAM_CFG did not complete in order");
    got = cmd(REMOVE, 0xE130, p);
    CHECK(got == expect(AECP_SUCCESS, REMOVE, 0xE130, p),
          "R19a: serialized output-map cleanup failed");

    // IEEE 1722.1-2021 9.2.2.6 caps command cdl at 524 octets. Figure 7-71
    // uses 20 + 8*N, so 63 records is the exact command maximum. Milan 5.4.1
    // lifts the ceiling for responses only. Repeating one ADD row proves that
    // every legal staged ordinal is read without inventing a conflict.
    std::vector<uint64_t> full_slot(63, row(0, 6, 6));
    p = edit_pl(DT_SPI, 0, full_slot);
    got = cmd(ADD, 0xE129, p);
    CHECK(got == expect(AECP_SUCCESS, ADD, 0xE129, p)
          && map_rows(DT_SPI, 0, 0, 0xE12A)
             == std::vector<uint64_t>{row(0, 6, 6)},
          "R20: 63-record maximum command was truncated or misapplied");
    std::vector<uint64_t> over_limit(64, row(0, 7, 7));
    p = edit_pl(DT_SPI, 0, over_limit);
    before = h.amap_edit_mutations;
    got = cmd(ADD, 0xE12B, p);
    CHECK(got == expect(AECP_BAD_ARGUMENTS, ADD, 0xE12B, p)
          && h.amap_edit_mutations == before
          && map_rows(DT_SPI, 0, 0, 0xE12C)
             == std::vector<uint64_t>{row(0, 6, 6)},
          "R20: 64-record over-limit command changed the map");
    p = edit_pl(DT_SPI, 0, {row(0, 6, 6)});
    got = cmd(REMOVE, 0xE12D, p);
    CHECK(got == expect(AECP_SUCCESS, REMOVE, 0xE12D, p),
          "R20: command-bound regression cleanup failed");
  }

  // ==== U. REGISTER/DEREGISTER_UNSOLICITED_NOTIFICATION ===================
  // (IEEE 1722.1-2021 SS7.4.37/SS7.4.38, Milan v1.2 SS5.4.2.21/SS5.4.2.22 +
  //  SS5.3.4.2's 16-controller list; the SS7.4.37.2 TIME_LIMITED expiry with
  //  its automatic DEREGISTER notification, u = 1, per-entry sequence_id -
  //  Milan Table 5.22 "sent only to this controller". The wrap compresses
  //  the 300 s window to 400 ms.)
  {
    h.flush_all();
    h.q_aecp.clear();
    std::vector<uint8_t> fl0(4, 0);

    // ---- U1: 2021-format REGISTER (flags 0) -> SUCCESS, flags echoed ----
    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7001,
                      0x0024, fl0));
    auto f = h.wait_any(h.q_aecp, 400);
    auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID,
                           0x7001, 0x0024, fl0);
    CHECK(!f.empty(), "U1: REGISTER answered");
    CHECK(f == want, "U1: SUCCESS byte-exact, 2021 format (flags echoed)");
    if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

    // ---- U2: 2013-format REGISTER (SS7.4.37.1: no flags field) ----------
    const uint64_t C2_MAC = 0x0202C2C2C2C2ull;
    h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7002,
                      0x0024, {}));
    f = h.wait_any(h.q_aecp, 400);
    want = aecp_frame(C2_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR2_EID,
                      0x7002, 0x0024, {});
    CHECK(!f.empty() && f == want,
          "U2: 2013-format REGISTER accepted and answered in its own format");
    if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

    // ---- U3: DEREGISTER -> SUCCESS; removing an absent one stays SUCCESS
    h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7003,
                      0x0025, {}));
    f = h.wait_any(h.q_aecp, 400);
    want = aecp_frame(C2_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR2_EID,
                      0x7003, 0x0025, {});
    CHECK(!f.empty() && f == want, "U3: DEREGISTER SUCCESS byte-exact");
    h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7004,
                      0x0025, {}));
    f = h.wait_any(h.q_aecp, 400);
    CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == AECP_SUCCESS,
          "U3b: dereg of an absent registration is idempotent SUCCESS");

    // ---- U4: Milan SS5.3.4.2's capacity: 16 rows, the 17th refuses ------
    int ok_regs = 0;
    for (int k = 0; k < 15; ++k) {   // U1's controller still holds one row
      uint64_t mac = 0x020200BB0000ull + unsigned(k);
      uint64_t eid = 0x8888000000000100ull + unsigned(k);
      h.feed(aecp_frame(OWN_MAC, mac, 0, 0, EID, eid,
                        uint16_t(0x7100 + k), 0x0024, fl0));
      auto r = h.wait_any(h.q_aecp, 400);
      if (!r.empty() && ((r[16] >> 3) & 0x1F) == AECP_SUCCESS) ++ok_regs;
    }
    CHECK(ok_regs == 15, "U4: 16 controllers register (fillers ok: %d/15)",
          ok_regs);
    h.feed(aecp_frame(OWN_MAC, 0x020200BBFFFFull, 0, 0, EID,
                      0x888800000000FFFFull, 0x71FF, 0x0024, fl0));
    f = h.wait_any(h.q_aecp, 400);
    CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == 8,
          "U4b: the 17th refuses NO_RESOURCES (Milan SS5.4.2.21), status %d",
          f.empty() ? -1 : ((f[16] >> 3) & 0x1F));

    // ---- U4c: a duplicate re-register while full REFRESHES, never full --
    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7005,
                      0x0024, fl0));
    f = h.wait_any(h.q_aecp, 400);
    CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == AECP_SUCCESS,
          "U4c: duplicate {eid, mac} refreshes its row while the table is full");

    for (int k = 0; k < 15; ++k) {
      uint64_t mac = 0x020200BB0000ull + unsigned(k);
      uint64_t eid = 0x8888000000000100ull + unsigned(k);
      h.feed(aecp_frame(OWN_MAC, mac, 0, 0, EID, eid,
                        uint16_t(0x7200 + k), 0x0025, {}));
      h.wait_any(h.q_aecp, 400);
    }

    // ---- U5: TIME_LIMITED -> 300 s (compressed 400 ms) -> the automatic
    //          DEREGISTER notification, u = 1, seq 0, this controller only
    std::vector<uint8_t> fl_tl(4, 0);
    fl_tl[3] = 0x01;                       // Table 7-147 TIME_LIMITED
    h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7006,
                      0x0024, fl_tl));
    f = h.wait_any(h.q_aecp, 400);
    CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == AECP_SUCCESS,
          "U5: TIME_LIMITED REGISTER accepted");
    h.q_aecp.clear();
    auto uns = h.wait_any(h.q_aecp, 700);
    auto exp_uns = aecp_frame(C2_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                              CTLR2_EID, 0x0000, 0x0025, {});
    exp_uns[36] |= 0x80;                   // SS9.3.2.1: u = 1
    CHECK(!uns.empty(), "U5b: the expiry notification arrives");
    CHECK(uns == exp_uns,
          "U5c: unsolicited DEREGISTER byte-exact (u=1, entry seq 0)");
    if (!uns.empty() && uns != exp_uns) { dump("got", uns); dump("exp", exp_uns); }
    auto more = h.wait_any(h.q_aecp, 300);
    CHECK(more.empty(),
          "U5d: sent only to this controller, once (Milan Table 5.22)");

    // ---- U6: re-registration re-arms the window (SS7.4.37.2) ------------
    h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7007,
                      0x0024, fl_tl));
    h.wait_any(h.q_aecp, 400);
    h.run_ms(250);
    h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7008,
                      0x0024, fl_tl));
    h.wait_any(h.q_aecp, 400);
    h.q_aecp.clear();
    h.run_ms(300);                         // past the ORIGINAL deadline
    CHECK(h.q_aecp.empty(), "U6: the refresh re-armed the 300 s window");
    auto uns2 = h.wait_any(h.q_aecp, 400); // the refreshed deadline fires
    CHECK(!uns2.empty() && (uns2[36] & 0x80) != 0,
          "U6b: the refreshed deadline expires with u = 1");

    // ---- U7: the M7-style READ_DESCRIPTOR regression --------------------
    h.q_aecp.clear();
    std::vector<uint8_t> rd(8, 0);
    putbe(&rd[0], CFGIX, 2); putbe(&rd[4], 0x0000, 2); putbe(&rd[6], 0, 2);
    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7009,
                      AEM_READ_DESCRIPTOR, rd));
    auto got = h.wait_any(h.q_aecp, 400);
    std::vector<uint8_t> epl(4, 0);
    putbe(&epl[0], CFGIX, 2);
    epl.insert(epl.end(), desc_entity.begin(), desc_entity.end());
    auto want_rd = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                              CTLR_EID, 0x7009, AEM_READ_DESCRIPTOR, epl);
    CHECK(!got.empty() && got == want_rd,
          "U7: READ_DESCRIPTOR byte-exact with registrations live");

    h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x700A,
                      0x0025, {}));
    h.wait_any(h.q_aecp, 400);
  }

  // ==== L. ACQUIRE_ENTITY + LOCK_ENTITY (Milan SS5.4.2.1/SS5.4.2.2) =======
  // (IEEE SS7.4.1/SS7.4.2: ACQUIRE never SUCCESS -> NOT_SUPPORTED echo; LOCK
  //  is real - one holder, UNLOCK flag, ENTITY[0] only, 60 s expiry
  //  compressed to 400 ms by the wrap - and every lock-state CHANGE goes out
  //  as an unsolicited LOCK_ENTITY response to the registered controllers
  //  except the requester, per-entry sequence_id counting up.)
  {
    h.flush_all();
    h.q_aecp.clear();
    const uint64_t C2_MAC = 0x0202C2C2C2C2ull;
    auto lockpld = [](uint32_t flags, uint16_t ty, uint16_t ix) {
      std::vector<uint8_t> b(16, 0);
      putbe(&b[0], flags, 4); putbe(&b[12], ty, 2); putbe(&b[14], ix, 2);
      return b;
    };
    auto lockresp = [&](uint64_t mac, uint64_t eid, uint16_t seq,
                        uint8_t status, uint32_t flags, uint64_t holder) {
      std::vector<uint8_t> b(16, 0);
      putbe(&b[0], flags, 4); putbe(&b[4], holder, 8);
      return aecp_frame(mac, OWN_MAC, 1, status, EID, eid, seq, 0x0001, b);
    };

    // ---- L1: ACQUIRE_ENTITY -> NOT_SUPPORTED with the command echoed ----
    {
      std::vector<uint8_t> acq(16, 0);          // flags 0, owner 0, ENTITY[0]
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7301,
                        0x0000, acq));
      auto f = h.wait_any(h.q_aecp, 400);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, 11, EID, CTLR_EID, 0x7301,
                             0x0000, acq);
      CHECK(!f.empty() && f == want,
            "L1: ACQUIRE answers NOT_SUPPORTED, command echoed (Milan 5.4.2.1)");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
      //! ...and the PERSISTENT flag changes nothing (the blanket refusal)
      std::vector<uint8_t> acq_p = acq; acq_p[3] = 0x01;
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7302,
                        0x0000, acq_p));
      f = h.wait_any(h.q_aecp, 400);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == 11,
            "L1b: PERSISTENT ACQUIRE refused the same way");
    }

    // ---- L4: the ACMP listener reads the SAME lock (Milan 5.5.2.4) ------
    //! runs BEFORE any controller registers: a bind is a Table 5.22
    //! GET_STREAM_INFO trigger since the P3 stage, and this block's job is
    //! the lock gate, not the notification stream (section G proves that)
    {
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7320,
                        0x0001, lockpld(0, 0, 0)));
      auto fl = h.wait_any(h.q_aecp, 400);
      CHECK(!fl.empty() && ((fl[16] >> 3) & 0x1F) == 0,
            "L4a: the gate test's own lock takes");
      h.q_acmp.clear();
      // sink 2, from the NON-holder: CONTROLLER_NOT_AUTHORIZED (13)
      auto bind2 = acmp_frame(C2_MAC, 6, 0, 0, CTLR2_EID, T1_EID, EID,
                              T1_UID, 2, 0, 0, 0x4321, 0, 0);
      h.feed(bind2);
      auto f = h.wait_any(h.q_acmp, 400);
      CHECK(!f.empty(), "L4: locked BIND_RX from a foreign controller answered");
      CHECK(!f.empty() && (f[15] & 0x0F) == 7
            && ((f[16] >> 3) & 0x1F) == 13
            && f.size() > 53 && ((f[52] << 8) | f[53]) == 2,
            "L4b: CONTROLLER_NOT_AUTHORIZED for sink 2 (msg %d st %d)",
            f.empty() ? -1 : (f[15] & 0x0F),
            f.empty() ? -1 : ((f[16] >> 3) & 0x1F));
      // ...and the HOLDER may bind: SUCCESS
      auto bind1 = acmp_frame(CTLR_MAC, 6, 0, 0, CTLR_EID, T1_EID, EID,
                              T1_UID, 2, 0, 0, 0x4322, 0, 0);
      h.feed(bind1);
      f = h.wait_any(h.q_acmp, 400);
      auto wantb = acmp_frame(OWN_MAC, 7, 0, 0, CTLR_EID, T1_EID, EID,
                              T1_UID, 2, 0, 1, 0x4322, 0, 0);
      CHECK(!f.empty() && f == wantb,
            "L4c: the locking controller binds through its own lock");
      if (!f.empty() && f != wantb) { dump("got", f); dump("exp", wantb); }
      // unbind to leave sink 2 clean, then unlock (from the holder)
      h.feed(acmp_frame(CTLR_MAC, 8, 0, 0, CTLR_EID, T1_EID, EID,
                        T1_UID, 2, 0, 0, 0x4323, 0, 0));
      h.wait_any(h.q_acmp, 400);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7321,
                        0x0001, lockpld(1, 0, 0)));
      h.wait_any(h.q_aecp, 400);
      h.q_aecp.clear();
    }


    // ---- L2: C2 registers; C1 locks; C2 gets the u=1 notification -------
    {
      std::vector<uint8_t> fl0(4, 0);
      h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7303,
                        0x0024, fl0));
      h.wait_any(h.q_aecp, 400);
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7304,
                        0x0001, lockpld(0, 0, 0)));
      auto f = h.wait_any(h.q_aecp, 400);
      auto want = lockresp(CTLR_MAC, CTLR_EID, 0x7304, 0, 0, CTLR_EID);
      CHECK(!f.empty() && f == want,
            "L2: LOCK SUCCESS, locked_id = the taker");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
      auto uns = h.wait_any(h.q_aecp, 400);
      auto wantu = lockresp(C2_MAC, CTLR2_EID, 0x0000, 0, 0, CTLR_EID);
      wantu[36] |= 0x80;
      CHECK(!uns.empty() && uns == wantu,
            "L2b: registered C2 told u=1, seq 0, locked_id = holder");
      if (!uns.empty() && uns != wantu) { dump("got", uns); dump("exp", wantu); }
    }

    // ---- L3: a foreign LOCK is denied naming the holder; no notification
    {
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7305,
                        0x0001, lockpld(0, 0, 0)));
      auto f = h.wait_any(h.q_aecp, 400);
      auto want = lockresp(C2_MAC, CTLR2_EID, 0x7305, 3, 0, CTLR_EID);
      CHECK(!f.empty() && f == want,
            "L3: ENTITY_LOCKED naming the holder");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
      //! the silence budget stays well inside the 400 ms lock window - the
      //! sections up to L5 must run under a HELD lock
      auto more = h.wait_any(h.q_aecp, 150);
      CHECK(more.empty(), "L3b: a denied lock changes nothing, notifies nobody");
    }

    // ---- L5: UNLOCK by the holder -> notification seq INCREMENTS --------
    {
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7306,
                        0x0001, lockpld(1, 0, 0)));      // UNLOCK flag
      auto f = h.wait_any(h.q_aecp, 400);
      auto want = lockresp(CTLR_MAC, CTLR_EID, 0x7306, 0, 1, 0);
      CHECK(!f.empty() && f == want,
            "L5: UNLOCK SUCCESS, locked_id 0");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
      auto uns = h.wait_any(h.q_aecp, 400);
      auto wantu = lockresp(C2_MAC, CTLR2_EID, 0x0001, 0, 0, 0);
      wantu[36] |= 0x80;
      CHECK(!uns.empty() && uns == wantu,
            "L5b: C2's second notification carries sequence_id 1 (Milan 5.4.5.1)");
      if (!uns.empty() && uns != wantu) { dump("got", uns); dump("exp", wantu); }
      //! the already-unlocked query: SUCCESS, no change, no notification
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7307,
                        0x0001, lockpld(1, 0, 0)));
      f = h.wait_any(h.q_aecp, 400);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == 0 && f[36] == 0 &&
            std::all_of(f.begin() + 42, f.begin() + 50,
                        [](uint8_t b) { return b == 0; }),
            "L5c: UNLOCK-as-query on a free entity: SUCCESS, locked_id 0");
      auto more = h.wait_any(h.q_aecp, 150);
      CHECK(more.empty(), "L5d: the query changed nothing, notified nobody");
    }

    // ---- L6: keep-alive re-lock re-arms the 60 s window -----------------
    {
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7308,
                        0x0001, lockpld(0, 0, 0)));
      h.wait_any(h.q_aecp, 400);        // SUCCESS
      h.wait_any(h.q_aecp, 400);        // C2's notification (seq 2)
      h.run_ms(150);
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7309,
                        0x0001, lockpld(0, 0, 0)));      // keep-alive
      auto f = h.wait_any(h.q_aecp, 400);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == 0,
            "L6: the holder's re-lock answers SUCCESS");
      auto more = h.wait_any(h.q_aecp, 150);
      CHECK(more.empty(), "L6b: a keep-alive changes nothing, notifies nobody");
      h.run_ms(150);                    // past the ORIGINAL 400 ms deadline
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x730A,
                        0x0001, lockpld(0, 0, 0)));
      f = h.wait_any(h.q_aecp, 400);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == 3,
            "L6c: still locked past the original deadline (re-armed)");
      h.q_aecp.clear();
      auto uns = h.wait_any(h.q_aecp, 700);   // the refreshed deadline fires
      auto wantu = lockresp(C2_MAC, CTLR2_EID, 0x0003, 0, 0, 0);
      wantu[36] |= 0x80;
      CHECK(!uns.empty() && uns == wantu,
            "L6d: 60 s auto-unlock notifies (Milan Table 5.22), locked_id 0, seq 3");
      if (!uns.empty() && uns != wantu) { dump("got", uns); dump("exp", wantu); }
    }

    // ---- L7: LOCK on a non-ENTITY target -> NOT_SUPPORTED echo ----------
    {
      h.q_aecp.clear();
      auto pld = lockpld(0, 0x0005, 0);        // STREAM_INPUT[0]
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x730B,
                        0x0001, pld));
      auto f = h.wait_any(h.q_aecp, 400);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, 11, EID, CTLR_EID, 0x730B,
                             0x0001, pld);
      CHECK(!f.empty() && f == want,
            "L7: locking STREAM_INPUT refuses NOT_SUPPORTED (Milan 5.4.2.2)");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    }

    // ---- L8: a truncated LOCK is BAD_ARGUMENTS --------------------------
    {
      std::vector<uint8_t> shortp(8, 0);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x730C,
                        0x0001, shortp));
      auto f = h.wait_any(h.q_aecp, 400);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == 7,
            "L8: an 8-byte LOCK payload answers BAD_ARGUMENTS");
    }

    // ---- L9: reads stay open to everyone while locked -------------------
    {
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x730D,
                        0x0001, lockpld(0, 0, 0)));
      h.wait_any(h.q_aecp, 400);        // the response
      h.wait_any(h.q_aecp, 400);        // C2's notification (seq 4)
      h.q_aecp.clear();
      std::vector<uint8_t> rd(8, 0);
      putbe(&rd[0], CFGIX, 2); putbe(&rd[4], 0x0000, 2);
      h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x730E,
                        AEM_READ_DESCRIPTOR, rd));
      auto got = h.wait_any(h.q_aecp, 400);
      CHECK(!got.empty() && ((got[16] >> 3) & 0x1F) == 0,
            "L9: READ_DESCRIPTOR from a non-holder answers while locked");
      // unlock + deregister: leave the entity clean
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x730F,
                        0x0001, lockpld(1, 0, 0)));
      h.wait_any(h.q_aecp, 400);
      h.wait_any(h.q_aecp, 400);        // C2's notification
      h.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, 0x7310,
                        0x0025, {}));
      h.wait_any(h.q_aecp, 400);
    }
  }

  // ==== G. GET_STREAM_INFO (IEEE SS7.4.16, Milan SS5.4.2.10) ==============
  // (the Milan 80-byte response: flags_ex + pbsta/acmpsta; every value and
  //  every validity flag is the INTEGRATOR's through the gsi face - the
  //  harness above IS that integrator - while existence is the descriptor
  //  store's, so index 2 refuses NO_SUCH_DESCRIPTOR with a zero-flagged
  //  body whatever the face would answer.)
  {
    h.flush_all();
    h.q_aecp.clear();
    auto gsi_body = [&](uint16_t ty, uint16_t ix, bool known) {
      std::vector<uint8_t> b(56, 0);
      putbe(&b[0], ty, 2);
      putbe(&b[2], ix, 2);
      if (known) {
        putbe(&b[4],  (uint32_t)H::gsi_value(0, ty, ix, 0, 0), 4);
        putbe(&b[8],  H::gsi_value(0, ty, ix, 1, 0), 8);
        putbe(&b[16], H::gsi_value(0, ty, ix, 2, 0), 8);
        putbe(&b[24], (uint32_t)H::gsi_value(0, ty, ix, 3, 0), 4);
        putbe(&b[28], H::gsi_value(0, ty, ix, 4, 0), 8);
        putbe(&b[36], H::gsi_value(0, ty, ix, 5, 0), 8);
        putbe(&b[44], H::gsi_value(0, ty, ix, 6, 0), 8);
        putbe(&b[52], (uint32_t)H::gsi_value(0, ty, ix, 7, 0), 4);
      }
      return b;
    };
    auto gsi_cmd = [&](uint16_t ty, uint16_t ix, uint16_t seq) {
      std::vector<uint8_t> p2(4, 0);
      putbe(&p2[0], ty, 2); putbe(&p2[2], ix, 2);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, seq,
                        0x000F, p2));
      return h.wait_any(h.q_aecp, 500);
    };

    // ---- G1/G2: byte-exact Milan responses, input + output side ---------
    auto f = gsi_cmd(0x0005, 0, 0x7401);
    auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID,
                           0x7401, 0x000F, gsi_body(0x0005, 0, true));
    CHECK(!f.empty() && f == want,
          "G1: STREAM_INPUT[0] Milan 80-byte response byte-exact (cdl 68)");
    if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    f = gsi_cmd(0x0006, 1, 0x7402);
    want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID,
                      0x7402, 0x000F, gsi_body(0x0006, 1, true));
    CHECK(!f.empty() && f == want,
          "G2: STREAM_OUTPUT[1] Milan response byte-exact");
    if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

    // ---- G3: existence is the store's: index 2 -> NO_SUCH_DESCRIPTOR ----
    f = gsi_cmd(0x0005, 2, 0x7403);
    want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_NO_SUCH_DESCRIPTOR, EID,
                      CTLR_EID, 0x7403, 0x000F, gsi_body(0x0005, 2, false));
    CHECK(!f.empty() && f == want,
          "G3: unknown index refuses NO_SUCH_DESCRIPTOR, zero-flagged body");
    if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

    // ---- G4: a truncated command is BAD_ARGUMENTS -----------------------
    {
      std::vector<uint8_t> shortp(2, 0);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7404,
                        0x000F, shortp));
      f = h.wait_any(h.q_aecp, 400);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == AECP_BAD_ARGUMENTS,
            "G4: a 2-byte GET_STREAM_INFO payload answers BAD_ARGUMENTS");
    }

    // ---- G5: a non-stream target refuses NOT_SUPPORTED ------------------
    f = gsi_cmd(0x0024, 0, 0x7405);          // CLOCK_DOMAIN exists, wrong verb
    {
      std::vector<uint8_t> p2(4, 0);
      putbe(&p2[0], 0x0024, 2);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, 11, EID, CTLR_EID, 0x7405,
                        0x000F, p2);
      CHECK(!f.empty() && f == want,
            "G5: GET_STREAM_INFO on CLOCK_DOMAIN echoes NOT_SUPPORTED");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    }

    // ---- G6: a wedged face voids honestly, then recovers ----------------
    {
      h.gsi_stuck = true;
      f = gsi_cmd(0x0005, 1, 0x7406);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == 10 && f.size() == 60,
            "G6: face wedge -> bare ENTITY_MISBEHAVING, never zeros under SUCCESS");
      h.gsi_stuck = false;
      f = gsi_cmd(0x0005, 1, 0x7407);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID,
                        0x7407, 0x000F, gsi_body(0x0005, 1, true));
      CHECK(!f.empty() && f == want, "G6b: next command answers clean");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    }

    // ---- G7: the Table 5.22 notification on a binding event -------------
    {
      std::vector<uint8_t> fl0(4, 0);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7408,
                        0x0024, fl0));
      h.wait_any(h.q_aecp, 400);
      h.q_aecp.clear();
      // bind sink 1 -> the fabric's bound state changes -> unsolicited
      // GET_STREAM_INFO for STREAM_INPUT[1] to the registered controller
      h.feed(acmp_frame(CTLR_MAC, 6, 0, 0, CTLR_EID, T1_EID, EID,
                        T1_UID, 1, 0, 0, 0x7409, 0, 0));
      auto uns = h.wait_any(h.q_aecp, 500);
      auto wantu = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                              CTLR_EID, 0x0000, 0x000F,
                              gsi_body(0x0005, 1, true));
      wantu[36] |= 0x80;
      CHECK(!uns.empty() && uns == wantu,
            "G7: bind emits the u=1 GET_STREAM_INFO for that sink (seq 0)");
      if (!uns.empty() && uns != wantu) { dump("got", uns); dump("exp", wantu); }
      // unbind: a second notification, sequence_id counting up
      h.q_aecp.clear();
      h.feed(acmp_frame(CTLR_MAC, 8, 0, 0, CTLR_EID, T1_EID, EID,
                        T1_UID, 1, 0, 0, 0x740A, 0, 0));
      uns = h.wait_any(h.q_aecp, 500);
      wantu = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                         CTLR_EID, 0x0001, 0x000F, gsi_body(0x0005, 1, true));
      wantu[36] |= 0x80;
      CHECK(!uns.empty() && uns == wantu,
            "G7b: unbind notifies again, sequence_id 1");
      if (!uns.empty() && uns != wantu) { dump("got", uns); dump("exp", wantu); }
      // ---- G7c: a STOP_STREAMING pushes ONE unsolicited GET_STREAM_INFO,
      // and a bind/unbind pushes exactly one (not two). Milan Table 5.22
      // lists "Started/stopped state (Stream Input only)"; 5.3.8.7 calls the
      // state undefined while unbound, so a bind and an unbind are NOT
      // started/stopped changes and must not add a second frame beside the
      // one G7/G7b already grade. This block is what catches a trigger that
      // fires too widely: an extra frame per bind shifts every later
      // response in this suite by one, which is a cascade, not a nit.
      h.q_acmp.clear(); h.q_aecp.clear();
      h.feed(acmp_frame(CTLR_MAC, 6, 0, 0, CTLR_EID, T1_EID, EID,
                        T1_UID, 1, 0, 0, 0x740C, 0, 0));
      auto ub = h.wait_any(h.q_acmp, 400);
      CHECK(!ub.empty() && ((ub[16] >> 3) & 0x1F) == 0,
            "G7c: re-bound sink 1 for the started/stopped notification");
      // the bind's own notification (seq 2), then NOTHING else for it
      auto n1 = h.wait_any(h.q_aecp, 500);
      CHECK(!n1.empty(), "G7c2: the bind notified once");
      auto extra = h.wait_any(h.q_aecp, 300);
      CHECK(extra.empty(),
            "G7d: the bind pushed exactly ONE notification, not two "
            "(a started/stopped trigger that fires on bind duplicates it)");

      // now a real started/stopped change, under a live binding
      h.q_aecp.clear();
      std::vector<uint8_t> sti(4, 0);
      putbe(&sti[0], 0x0005, 2); putbe(&sti[2], 1, 2);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x740D,
                        0x0023, sti));
      auto rsp = h.wait_any(h.q_aecp, 600);      // the solicited response
      CHECK(!rsp.empty() && ((rsp[16] >> 3) & 0x1F) == 0,
            "G7e: STOP_STREAMING on the bound sink answered SUCCESS");
      auto push = h.wait_any(h.q_aecp, 600);     // ...then the unsolicited
      CHECK(!push.empty() && push.size() > 37
            && ((push[36] & 0x80) != 0)
            && (((push[36] & 0x7F) << 8) | push[37]) == 0x000F,
            "G7f: ...and a STOP_STREAMING pushes the u=1 GET_STREAM_INFO "
            "Table 5.22 asks for");
      auto push2 = h.wait_any(h.q_aecp, 300);
      CHECK(push2.empty(), "G7g: ...exactly one, not two");

      // deregister: leave the table clean. The unbind below ALSO pushes its
      // own notification (G7b grades that behaviour) - drain it here rather
      // than leave it in the queue, or the next section reads this block's
      // leftover frame and every byte-exact check after it shifts by one.
      h.q_acmp.clear();
      h.feed(acmp_frame(CTLR_MAC, 8, 0, 0, CTLR_EID, T1_EID, EID,
                        T1_UID, 1, 0, 0, 0x740E, 0, 0));
      h.wait_any(h.q_acmp, 400);
      (void)h.wait_any(h.q_aecp, 500);          // the unbind's notification
      h.idle(200);
      h.q_aecp.clear();
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x740B,
                        0x0025, {}));
      h.wait_any(h.q_aecp, 400);
    }

    // ---- G8: the M7-style READ_DESCRIPTOR regression --------------------
    {
      h.q_aecp.clear();
      std::vector<uint8_t> rd(8, 0);
      putbe(&rd[0], CFGIX, 2); putbe(&rd[4], 0x0000, 2);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x740C,
                        AEM_READ_DESCRIPTOR, rd));
      auto got2 = h.wait_any(h.q_aecp, 400);
      std::vector<uint8_t> epl(4, 0);
      putbe(&epl[0], CFGIX, 2);
      epl.insert(epl.end(), desc_entity.begin(), desc_entity.end());
      auto want2 = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                              CTLR_EID, 0x740C, AEM_READ_DESCRIPTOR, epl);
      CHECK(!got2.empty() && got2 == want2,
            "G8: READ_DESCRIPTOR byte-exact after the stream-info paths");
    }
  }

  // ==== V. GET_AVB_INFO + GET_AS_PATH (Milan SS5.4.2.23/SS5.4.2.24) =======
  // (IEEE SS7.4.40/SS7.4.41; the gPTP words and both arrays are the
  //  INTEGRATOR's through the Milan-info face kinds 1 and 2 - count-many
  //  records emitted, zero-count faces emit empty lists honestly.)
  {
    h.flush_all();
    h.q_aecp.clear();

    // ---- V1: GET_AVB_INFO byte-exact (2 msrp_mappings -> cdl 40) --------
    {
      std::vector<uint8_t> p2(4, 0);
      putbe(&p2[0], 0x0009, 2);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7501,
                        0x0027, p2));
      auto f = h.wait_any(h.q_aecp, 500);
      std::vector<uint8_t> body(28, 0);
      putbe(&body[0],  0x0009, 2);                    // type
      putbe(&body[2],  0, 2);                         // index
      putbe(&body[4],  0xA1A2A3A4A5A6A7A8ull, 8);     // gm
      putbe(&body[12], 0x00001234u, 4);               // propagation_delay
      body[16] = 0x00; body[17] = 0x07;               // domain, flags
      putbe(&body[18], 2, 2);                         // count
      putbe(&body[20], 0x06030002u, 4);               // mapping 0
      putbe(&body[24], 0x05020002u, 4);               // mapping 1
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7501, 0x0027, body);
      CHECK(!f.empty() && f == want,
            "V1: GET_AVB_INFO byte-exact, both mappings in order");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    }

    // ---- V2: GET_AS_PATH byte-exact (count 3 -> cdl 40) -----------------
    {
      std::vector<uint8_t> p2(4, 0);                  // index 0 + reserved
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7502,
                        0x0028, p2));
      auto f = h.wait_any(h.q_aecp, 500);
      std::vector<uint8_t> body(28, 0);
      putbe(&body[0], 0, 2);                          // descriptor_index
      putbe(&body[2], 3, 2);                          // count
      putbe(&body[4],  0xC1D1000000000000ull, 8);
      putbe(&body[12], 0xC1D1000000000001ull, 8);
      putbe(&body[20], 0xC1D1000000000002ull, 8);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7502, 0x0028, body);
      CHECK(!f.empty() && f == want,
            "V2: GET_AS_PATH byte-exact, the path in order");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    }

    // ---- V3: existence still rules: AVB_INTERFACE[1] does not exist -----
    {
      std::vector<uint8_t> p2(4, 0);
      putbe(&p2[0], 0x0009, 2); putbe(&p2[2], 1, 2);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7503,
                        0x0027, p2));
      auto f = h.wait_any(h.q_aecp, 500);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == AECP_NO_SUCH_DESCRIPTOR,
            "V3: GET_AVB_INFO on a missing interface refuses NO_SUCH_DESCRIPTOR");
      std::vector<uint8_t> p3(4, 0);
      putbe(&p3[0], 1, 2);                            // AS_PATH index 1
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7504,
                        0x0028, p3));
      f = h.wait_any(h.q_aecp, 500);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == AECP_NO_SUCH_DESCRIPTOR,
            "V3b: GET_AS_PATH likewise");
    }

    // ---- V4: truncated commands are BAD_ARGUMENTS -----------------------
    {
      std::vector<uint8_t> p2(2, 0);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7505,
                        0x0027, p2));
      auto f = h.wait_any(h.q_aecp, 400);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == AECP_BAD_ARGUMENTS,
            "V4: short GET_AVB_INFO answers BAD_ARGUMENTS");
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7506,
                        0x0028, {}));
      f = h.wait_any(h.q_aecp, 400);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == AECP_BAD_ARGUMENTS,
            "V4b: empty GET_AS_PATH answers BAD_ARGUMENTS");
    }

    // ---- V5: GET_AVB_INFO on a non-interface type -----------------------
    {
      std::vector<uint8_t> p2(4, 0);
      putbe(&p2[0], 0x0024, 2);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7507,
                        0x0027, p2));
      auto f = h.wait_any(h.q_aecp, 400);
      CHECK(!f.empty() && ((f[16] >> 3) & 0x1F) == 11,
            "V5: GET_AVB_INFO on CLOCK_DOMAIN echoes NOT_SUPPORTED");
    }

    // ---- V6: a grandmaster change notifies BOTH kinds -------------------
    {
      std::vector<uint8_t> fl0(4, 0);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7508,
                        0x0024, fl0));
      h.wait_any(h.q_aecp, 400);
      h.q_aecp.clear();
      d->gm_change_i = 1;
      h.step();
      d->gm_change_i = 0;
      // GET_AVB_INFO outranks GET_AS_PATH in the emission pick
      auto u1 = h.wait_any(h.q_aecp, 500);
      auto u2 = h.wait_any(h.q_aecp, 500);
      CHECK(!u1.empty() && !u2.empty(),
            "V6: both gPTP notifications arrive on a GM change");
      bool k1 = !u1.empty() && u1.size() > 37 && (u1[36] & 0x80)
                && u1[37] == 0x27 && ((u1[34] << 8) | u1[35]) == 0;
      bool k2 = !u2.empty() && u2.size() > 37 && (u2[36] & 0x80)
                && u2[37] == 0x28 && ((u2[34] << 8) | u2[35]) == 1;
      CHECK(k1, "V6b: first the u=1 GET_AVB_INFO, entry seq 0");
      CHECK(k2, "V6c: then the u=1 GET_AS_PATH, entry seq 1");
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7509,
                        0x0025, {}));
      h.wait_any(h.q_aecp, 400);
    }

    // ---- V7: the M7-style READ_DESCRIPTOR regression --------------------
    {
      h.q_aecp.clear();
      std::vector<uint8_t> rd(8, 0);
      putbe(&rd[0], CFGIX, 2); putbe(&rd[4], 0x0000, 2);
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x750A,
                        AEM_READ_DESCRIPTOR, rd));
      auto got2 = h.wait_any(h.q_aecp, 400);
      CHECK(!got2.empty() && ((got2[16] >> 3) & 0x1F) == 0
            && got2.size() == 38 + 4 + 312,
            "V7: READ_DESCRIPTOR intact after the gPTP paths");
    }
  }

  // ==== W. the read-side command set ======================================
  // ENTITY_AVAILABLE (Milan §5.4.2.3), GET_CONFIGURATION (§5.4.2.6),
  // GET_STREAM_FORMAT (§5.4.2.8), GET_SAMPLING_RATE (§5.4.2.14) and
  // GET_CLOCK_SOURCE (§5.4.2.16) — the five SHALLs that answered the
  // NOT_IMPLEMENTED echo before this round, and five of the commands the
  // Milan end-station test plan hard-gates (es-4.2, es-4.3, es-4.4, es-4.8,
  // es-4.9). Every check is byte-exact against a payload this file builds
  // from the IEEE figure, never from the DUT's own answer.
  //
  // THE FALSIFIERS MATTER MORE THAN THE HAPPY PATHS here, because four of
  // these five commands could be faked by a program that emits a plausible
  // constant. So: the sampling rate in the image is 96000 and not 48000; the
  // stream format is the per-{type,index} value only the Milan-info face
  // knows; and the configuration index and clock source index are each read
  // twice with the IMAGE PATCHED IN BETWEEN, so a constant cannot survive.
  {
    h.q_aecp.clear();
    auto st = [](const std::vector<uint8_t>& f) {
      return f.empty() ? 0xFF : ((f[16] >> 3) & 0x1F);
    };
    //! the RESPONSE SIZE has to be read off control_data_length, never off
    //! the frame: every payload in this section is under the 60-octet
    //! Ethernet minimum, so all five commands pad to exactly 60 B on the wire
    //! and a frame-length check would pass on any of them
    auto cdl = [](const std::vector<uint8_t>& f) {
      return f.size() < 18 ? -1
                           : int(((f[16] & 0x07) << 8) | f[17]);
    };
    auto ask = [&](uint16_t op, const std::vector<uint8_t>& pl, uint16_t seq) {
      h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, seq, op, pl));
      return h.wait_any(h.q_aecp, 600);
    };
    auto ti = [](uint16_t ty, uint16_t ix) {
      std::vector<uint8_t> p(4, 0);
      putbe(&p[0], ty, 2); putbe(&p[2], ix, 2);
      return p;
    };

    // ---- W1: ENTITY_AVAILABLE, unlocked (§7.4.3.2, Figure 7-29) ---------
    // payload 20 -> cdl 32. Both controller-id fields zero: this entity can
    // never be acquired (Milan Δ7), and section L left it unlocked.
    {
      auto f = ask(AEM_ENTITY_AVAILABLE, {}, 0x7601);
      std::vector<uint8_t> body(20, 0);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7601, AEM_ENTITY_AVAILABLE, body);
      CHECK(!f.empty() && f == want,
            "W1: ENTITY_AVAILABLE byte-exact, flags 0, both ids 0");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    }

    // ---- W2: ENTITY_AVAILABLE reports the lock (Table 7-144) ------------
    // ENTITY_LOCKED is 0x00000002 — Table 7-144 numbers its bits with 0 = MSB,
    // so its "Bit 30" is the second-least-significant bit. The holder eid is
    // read off the registry face with rgy_state = 1, which is a query and not
    // an operation: the lock must still be held afterwards.
    {
      std::vector<uint8_t> lk(16, 0);                 // flags 0 = LOCK
      putbe(&lk[12], 0x0000, 2); putbe(&lk[14], 0, 2);   // ENTITY[0]
      auto l = ask(0x0001, lk, 0x7602);
      CHECK(!l.empty() && st(l) == AECP_SUCCESS, "W2: LOCK_ENTITY granted");

      auto f = ask(AEM_ENTITY_AVAILABLE, {}, 0x7603);
      std::vector<uint8_t> body(20, 0);
      putbe(&body[0], 0x00000002u, 4);                // ENTITY_LOCKED
      putbe(&body[12], CTLR_EID, 8);                  // locked_controller_id
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7603, AEM_ENTITY_AVAILABLE, body);
      CHECK(!f.empty() && f == want,
            "W2b: ENTITY_AVAILABLE carries ENTITY_LOCKED + the holder eid");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // the query must not have released it: a LOCK from the holder re-arms
      // and still answers SUCCESS, and the UNLOCK below is what clears it
      std::vector<uint8_t> ul(16, 0);
      putbe(&ul[2], 1, 2);                            // flags = UNLOCK
      auto u = ask(0x0001, ul, 0x7604);
      CHECK(!u.empty() && st(u) == AECP_SUCCESS, "W2c: UNLOCK accepted");

      f = ask(AEM_ENTITY_AVAILABLE, {}, 0x7605);
      CHECK(!f.empty() && cdl(f) == 32
            && f[38] == 0 && f[39] == 0 && f[40] == 0 && f[41] == 0,
            "W2d: after UNLOCK the flags are clear again");
    }

    // ---- W3: GET_CONFIGURATION (§7.4.8.2, Figure 7-33) ------------------
    // reserved @24 + configuration_index @26, payload 4 -> cdl 16. The value
    // is the ENTITY descriptor's current_configuration at offset 310, read
    // through the same store READ_DESCRIPTOR uses.
    {
      auto f = ask(AEM_GET_CONFIGURATION, {}, 0x7606);
      std::vector<uint8_t> body(4, 0);
      putbe(&body[2], CFGIX, 2);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7606, AEM_GET_CONFIGURATION, body);
      CHECK(!f.empty() && f == want, "W3: GET_CONFIGURATION byte-exact");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    }

    // ---- W3b: THE FALSIFIER — patch the image, the answer must follow ---
    // CFGIX is zero, so W3 alone cannot tell a real read from a hardcoded
    // zero. Poke current_configuration in the DRAM model and ask again: the
    // store re-fetches per locate, so a program that reads answers 0x0007 and
    // a program that invents still answers 0.
    {
      uint32_t ent_off = 0;                           // ENTITY body offset
      for (auto& e : img_ents) if (e.type == 0x0000) ent_off = e.off;
      CHECK(ent_off != 0, "W3b: the ENTITY entry was located in the image");
      uint8_t save_hi = h.dram[ent_off + 310], save_lo = h.dram[ent_off + 311];
      h.dram[ent_off + 310] = 0x00; h.dram[ent_off + 311] = 0x07;

      auto f = ask(AEM_GET_CONFIGURATION, {}, 0x7607);
      std::vector<uint8_t> body(4, 0);
      putbe(&body[2], 0x0007, 2);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7607, AEM_GET_CONFIGURATION, body);
      CHECK(!f.empty() && f == want,
            "W3b: GET_CONFIGURATION follows the image, it does not invent");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // ---- W3c: THE SAME FALSIFIER, ON THE REFUSAL PATH ---------------
      // The refusal body is bound by IEEE 7.4.7.1 exactly as the success body
      // is: it carries the CURRENT value. "Current" before any controller has
      // written the dynamic store is the IMAGE's, which the first cut of the
      // refusal emitter did not read. It took `RGN_DYN + SEL_CFG` raw, so
      // every refusal echoed 0 until the first successful SET set the valid
      // bit, and no check could see it because the store's reset value and
      // the image's value were both 0.
      //
      // This runs HERE, before any SET_CONFIGURATION has succeeded, which is
      // the only window where the fallback arm is reachable. The image still
      // reads 0x0007 from W3b above.
      {
        //! truncated -> BAD_ARGUMENTS, taken at dispatch on cdl alone
        std::vector<uint8_t> shortpl(2, 0);
        auto b = ask(AEM_SET_CONFIGURATION, shortpl, 0x7608);
        CHECK(!b.empty() && st(b) == AECP_BAD_ARGUMENTS,
              "W3c: a truncated SET_CONFIGURATION is BAD_ARGUMENTS");
        CHECK(b.size() >= 42 && (((unsigned)b[40] << 8) | b[41]) == 0x0007,
              "W3c2: ...and the refusal echoes the IMAGE's current "
              "configuration, not the unwritten store's 0; got %u",
              b.size() >= 42 ? (((unsigned)b[40] << 8) | b[41]) : 999u);

        //! sink 0 is still bound from S6, so a well-formed command takes the
        //! STREAM_IS_RUNNING arm: a different program, same overlay
        std::vector<uint8_t> full(4, 0);
        auto r = ask(AEM_SET_CONFIGURATION, full, 0x7609);
        CHECK(!r.empty() && st(r) == AECP_STREAM_IS_RUNNING,
              "W3c3: a well-formed one is STREAM_IS_RUNNING (S6's bind is "
              "still live), got %d", st(r));
        CHECK(r.size() >= 42 && (((unsigned)r[40] << 8) | r[41]) == 0x0007,
              "W3c4: ...and that arm echoes the image's value too; got %u",
              r.size() >= 42 ? (((unsigned)r[40] << 8) | r[41]) : 999u);

        //! READ_DESCRIPTOR(ENTITY) must use the same image fallback before
        //! the dynamic configuration store has been written.
        std::vector<uint8_t> rd(8, 0);
        putbe(&rd[0], CFGIX, 2);
        putbe(&rd[4], 0x0000, 2);
        auto e = ask(AEM_READ_DESCRIPTOR, rd, 0x760A);
        CHECK(!e.empty() && st(e) == AECP_SUCCESS && cdl(e) == 12 + 4 + 312,
              "W3d: READ_DESCRIPTOR(ENTITY) answered before any SET");
        CHECK(e.size() >= 42 + 312
              && (((unsigned)e[42 + 310] << 8) | e[42 + 311]) == 0x0007,
              "W3d2: current_configuration comes from the image; got %u",
              e.size() >= 42 + 312
                ? (((unsigned)e[42 + 310] << 8) | e[42 + 311]) : 999u);
        auto gc = ask(AEM_GET_CONFIGURATION, {}, 0x760B);
        CHECK(!gc.empty() && gc.size() >= 42
              && (((unsigned)gc[40] << 8) | gc[41])
                 == (((unsigned)e[42 + 310] << 8) | e[42 + 311]),
              "W3d3: GET_CONFIGURATION agrees with READ_DESCRIPTOR");
      }

      h.dram[ent_off + 310] = save_hi; h.dram[ent_off + 311] = save_lo;
    }

    // ---- W4: GET_STREAM_FORMAT (§7.4.10.2, Figure 7-34) -----------------
    // type @24, index @26, stream_format @28: payload 12 -> cdl 24. The value
    // is the INTEGRATOR's (Milan-info face kind 0 selector 1), the same word
    // GET_STREAM_INFO publishes — §7.4.10.2's "current stream format" is
    // current after a bind adapts it, which no static image can know.
    for (uint16_t ty : {uint16_t(0x0005), uint16_t(0x0006)}) {
      for (uint16_t ix = 0; ix < 2; ++ix) {
        auto f = ask(AEM_GET_STREAM_FORMAT, ti(ty, ix),
                     uint16_t(0x7610 + (ty << 4) + ix));
        std::vector<uint8_t> body(12, 0);
        putbe(&body[0], ty, 2); putbe(&body[2], ix, 2);
        putbe(&body[4], H::gsi_value(0, ty, ix, 1, 0), 8);
        auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                               CTLR_EID, uint16_t(0x7610 + (ty << 4) + ix),
                               AEM_GET_STREAM_FORMAT, body);
        CHECK(!f.empty() && f == want,
              "W4: GET_STREAM_FORMAT byte-exact for type %04x index %u",
              ty, ix);
        if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
      }
    }

    // ---- W4b: existence is the STORE's, not the face's ------------------
    // The face answers zero for index 2+, but a zero format is a claim. The
    // store has two of each, so index 2 is NO_SUCH_DESCRIPTOR — carried in
    // the full 12-byte body, because only NOT_IMPLEMENTED may answer at
    // command length.
    {
      auto f = ask(AEM_GET_STREAM_FORMAT, ti(0x0005, 2), 0x7620);
      CHECK(!f.empty() && st(f) == AECP_NO_SUCH_DESCRIPTOR,
            "W4b: STREAM_INPUT[2] refuses NO_SUCH_DESCRIPTOR");
      CHECK(cdl(f) == 12 + 12,
            "W4b2: the refusal still carries the full 12-byte body, cdl %d",
            cdl(f));
    }

    // ---- W4c: a wrong target is NOT_SUPPORTED in the full body ----------
    {
      auto f = ask(AEM_GET_STREAM_FORMAT, ti(0x0024, 0), 0x7621);
      CHECK(!f.empty() && st(f) == AECP_NOT_SUPPORTED,
            "W4c: GET_STREAM_FORMAT on CLOCK_DOMAIN refuses NOT_SUPPORTED");
      CHECK(cdl(f) == 12 + 12,
            "W4c2: ...in the full response body, cdl %d", cdl(f));
      CHECK(f.size() > 41 && f[38] == 0x00 && f[39] == 0x24,
            "W4c3: ...with the refused type echoed");
    }

    // ---- W5: GET_SAMPLING_RATE (§7.4.22.2, Figure 7-45) -----------------
    // type @24, index @26, sampling_rate @28: payload 8 -> cdl 20. 96000 is
    // in the image and 48000 is the value a hardcoded answer would pick.
    {
      auto f = ask(AEM_GET_SAMPLING_RATE, ti(0x0002, 0), 0x7630);
      std::vector<uint8_t> body(8, 0);
      putbe(&body[0], 0x0002, 2); putbe(&body[2], 0, 2);
      putbe(&body[4], 96000u, 4);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7630, AEM_GET_SAMPLING_RATE, body);
      CHECK(!f.empty() && f == want,
            "W5: GET_SAMPLING_RATE byte-exact, 96000 out of the image");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    }

    // ---- W5b: a missing unit and a wrong target -------------------------
    {
      auto f = ask(AEM_GET_SAMPLING_RATE, ti(0x0002, 1), 0x7631);
      CHECK(!f.empty() && st(f) == AECP_NO_SUCH_DESCRIPTOR && cdl(f) == 20,
            "W5b: AUDIO_UNIT[1] refuses NO_SUCH_DESCRIPTOR in the full body");
      f = ask(AEM_GET_SAMPLING_RATE, ti(0x0005, 0), 0x7632);
      CHECK(!f.empty() && st(f) == AECP_NOT_SUPPORTED && cdl(f) == 20,
            "W5c: a STREAM_INPUT target refuses NOT_SUPPORTED, full body");
    }

    // ---- W6: GET_CLOCK_SOURCE (§7.4.24.2, Figure 7-47) ------------------
    // type @24, index @26, clock_source_index @28, reserved @30: payload 8
    // -> cdl 20. clock_source_index ENDS its 64-bit lane, so it is the one
    // field of the three that a plain lane read delivers right-justified.
    {
      auto f = ask(AEM_GET_CLOCK_SOURCE, ti(0x0024, 0), 0x7640);
      std::vector<uint8_t> body(8, 0);
      putbe(&body[0], 0x0024, 2); putbe(&body[2], 0, 2);
      putbe(&body[4], 0x0000, 2);                     // the image's value
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7640, AEM_GET_CLOCK_SOURCE, body);
      CHECK(!f.empty() && f == want, "W6: GET_CLOCK_SOURCE byte-exact");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
    }

    // ---- W6b: THE FALSIFIER, again by patching the image ----------------
    {
      uint32_t cd_off = 0;
      for (auto& e : img_ents) if (e.type == 0x0024) cd_off = e.off;
      CHECK(cd_off != 0, "W6b: the CLOCK_DOMAIN entry was located");
      uint8_t s_hi = h.dram[cd_off + 70], s_lo = h.dram[cd_off + 71];
      h.dram[cd_off + 70] = 0x00; h.dram[cd_off + 71] = 0x02;

      auto f = ask(AEM_GET_CLOCK_SOURCE, ti(0x0024, 0), 0x7641);
      std::vector<uint8_t> body(8, 0);
      putbe(&body[0], 0x0024, 2); putbe(&body[2], 0, 2);
      putbe(&body[4], 0x0002, 2);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7641, AEM_GET_CLOCK_SOURCE, body);
      CHECK(!f.empty() && f == want,
            "W6b: GET_CLOCK_SOURCE follows the image, it does not invent");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      h.dram[cd_off + 70] = s_hi; h.dram[cd_off + 71] = s_lo;
    }

    // ---- W7: the three {type, index} reads gate their length ------------
    // §7.4.42.1's shape is 4 payload bytes, so cdl 16 is the whole command
    // and anything shorter never reached descriptor_index. The two
    // payload-less commands have NO such floor — §7.4.3.1 and §7.4.8.1 both
    // say "the command_specific_data field is zero length", so cdl 12 is
    // correct for them and must not be refused.
    {
      std::vector<uint8_t> shortpl(2, 0);
      for (uint16_t op : {AEM_GET_STREAM_FORMAT, AEM_GET_SAMPLING_RATE,
                          AEM_GET_CLOCK_SOURCE}) {
        auto f = ask(op, shortpl, uint16_t(0x7650 + op));
        CHECK(!f.empty() && st(f) == AECP_BAD_ARGUMENTS,
              "W7: a truncated %04x answers BAD_ARGUMENTS", op);
      }
    }

    // ---- W8: GET_DYNAMIC_INFO batch semantics --------------------------
    // IEEE 1722.1-2021 7.4.76 requires a complete whitelist pre-scan,
    // independent record statuses, silent overflow skips, and continued
    // processing after a skipped record. Milan 5.4.2.29 makes that command
    // mandatory. These checks build the aggregate independently from the
    // ordinary command responses.
    {
      auto direc = [](uint16_t op, const std::vector<uint8_t>& data,
                      uint8_t info_status = 0) {
        std::vector<uint8_t> r(8, 0);
        putbe(&r[0], data.size(), 2);
        r[4] = info_status;
        putbe(&r[6], op, 2);
        r.insert(r.end(), data.begin(), data.end());
        return r;
      };
      auto append = [](std::vector<uint8_t>& dst,
                       const std::vector<uint8_t>& src) {
        dst.insert(dst.end(), src.begin(), src.end());
      };
      auto gcfg_body = [&]() {
        std::vector<uint8_t> b(4, 0);
        putbe(&b[2], CFGIX, 2);
        return b;
      };
      auto gsfmt_body = [&](uint16_t ty, uint16_t ix) {
        std::vector<uint8_t> b(12, 0);
        putbe(&b[0], ty, 2); putbe(&b[2], ix, 2);
        putbe(&b[4], H::gsi_value(0, ty, ix, 1, 0), 8);
        return b;
      };
      auto gsi_body = [&](uint16_t ty, uint16_t ix, bool known) {
        std::vector<uint8_t> b(56, 0);
        putbe(&b[0], ty, 2); putbe(&b[2], ix, 2);
        if (known) {
          putbe(&b[4],  (uint32_t)H::gsi_value(0, ty, ix, 0, 0), 4);
          putbe(&b[8],  H::gsi_value(0, ty, ix, 1, 0), 8);
          putbe(&b[16], H::gsi_value(0, ty, ix, 2, 0), 8);
          putbe(&b[24], (uint32_t)H::gsi_value(0, ty, ix, 3, 0), 4);
          putbe(&b[28], H::gsi_value(0, ty, ix, 4, 0), 8);
          putbe(&b[36], H::gsi_value(0, ty, ix, 5, 0), 8);
          putbe(&b[44], H::gsi_value(0, ty, ix, 6, 0), 8);
          putbe(&b[52], (uint32_t)H::gsi_value(0, ty, ix, 7, 0), 4);
        }
        return b;
      };
      auto ctr_body = [&](uint16_t ty, uint16_t ix) {
        std::vector<uint8_t> b(136, 0);
        putbe(&b[0], ty, 2); putbe(&b[2], ix, 2);
        putbe(&b[4], H::ctr_mask(ty, ix), 4);
        for (int n = 0; n < 32; ++n)
          putbe(&b[8 + 4 * n], H::ctr_value(ty, ix, uint8_t(n)), 4);
        return b;
      };
      auto rate_body = [](uint16_t ty, uint16_t ix, uint32_t rate) {
        std::vector<uint8_t> b(8, 0);
        putbe(&b[0], ty, 2); putbe(&b[2], ix, 2);
        putbe(&b[4], rate, 4);
        return b;
      };

      // Two implemented getters in one exact response.
      std::vector<uint8_t> req;
      append(req, direc(AEM_GET_CONFIGURATION, {}));
      append(req, direc(AEM_GET_STREAM_FORMAT, ti(0x0005, 1)));
      auto f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7660);
      std::vector<uint8_t> body;
      append(body, direc(AEM_GET_CONFIGURATION, gcfg_body(), AECP_SUCCESS));
      append(body, direc(AEM_GET_STREAM_FORMAT,
                         gsfmt_body(0x0005, 1), AECP_SUCCESS));
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7660, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8: two-element GET_DYNAMIC_INFO response is byte-exact");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // A missing descriptor changes only that record's status.
      req.clear(); body.clear();
      append(req, direc(AEM_GET_SAMPLING_RATE, ti(0x0002, 1)));
      append(req, direc(AEM_GET_CONFIGURATION, {}));
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7661);
      std::vector<uint8_t> miss_rate(8, 0);
      putbe(&miss_rate[0], 0x0002, 2); putbe(&miss_rate[2], 1, 2);
      append(body, direc(AEM_GET_SAMPLING_RATE, miss_rate,
                         AECP_NO_SUCH_DESCRIPTOR));
      append(body, direc(AEM_GET_CONFIGURATION, gcfg_body(), AECP_SUCCESS));
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x7661, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8b: a missing descriptor is a per-record status");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // One forbidden variable-size getter rejects the whole list before the
      // valid GET_CONFIGURATION ahead of it can reach the descriptor store.
      req.clear();
      append(req, direc(AEM_GET_CONFIGURATION, {}));
      append(req, direc(AEM_GET_AUDIO_MAP, ti(0x000E, 0)));
      uint64_t mem_before = h.dram_reqs;
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7662);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_BAD_ARGUMENTS, EID,
                        CTLR_EID, 0x7662, AEM_GET_DYNAMIC_INFO, req);
      CHECK(!f.empty() && f == want,
            "W8c: forbidden GET_AUDIO_MAP rejects the complete batch");
      CHECK(h.dram_reqs == mem_before,
            "W8c2: pre-scan rejection processed no earlier record");

      // The fourth 144-byte counter result would exceed cdl 524. It is
      // omitted, while the smaller GET_CONFIGURATION after it is retained.
      // Every counter target is distinct so skipping the wrong ordinal cannot
      // produce an identical frame.
      req.clear(); body.clear();
      const std::vector<std::pair<uint16_t, uint16_t>> overflow_ctrs = {
        {0x0005, 0}, {0x0005, 1}, {0x0006, 0}, {0x0009, 0}
      };
      for (const auto& target : overflow_ctrs)
        append(req, direc(AEM_GET_COUNTERS,
                          ti(target.first, target.second)));
      append(req, direc(AEM_GET_CONFIGURATION, {}));
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7663);
      for (size_t n = 0; n < 3; ++n)
        append(body, direc(AEM_GET_COUNTERS,
                           ctr_body(overflow_ctrs[n].first,
                                    overflow_ctrs[n].second),
                           AECP_SUCCESS));
      append(body, direc(AEM_GET_CONFIGURATION, gcfg_body(), AECP_SUCCESS));
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x7663, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8d: overflow skips one record and continues with the next");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // Milan replaces the IEEE GET_STREAM_INFO body with exactly 56 bytes.
      req = direc(0x000F, ti(0x0005, 0));
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7664);
      body = direc(0x000F, gsi_body(0x0005, 0, true), AECP_SUCCESS);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x7664, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8e: GET_STREAM_INFO record carries the Milan 56-byte body");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // Whitelist membership does not claim implementation. GET_NAME is
      // legal in a batch, so it receives a record-level NOT_SUPPORTED and
      // its fixed command data is copied exactly.
      std::vector<uint8_t> name_arg = {0xD3, 0x1C, 0xA5, 0x7E};
      req = direc(0x0011, name_arg);
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7665);
      body = direc(0x0011, name_arg, AECP_NOT_SUPPORTED);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x7665, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8f: legal unimplemented getter is NOT_SUPPORTED per record");

      // An empty list is a valid request and produces an empty SUCCESS body.
      req.clear(); body.clear();
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7666);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x7666, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8g: an empty GET_DYNAMIC_INFO list succeeds exactly");

      // Header truncation and a data length that runs past the command both
      // reject the complete list with its original bytes echoed.
      req.assign(7, 0);
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7667);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_BAD_ARGUMENTS, EID,
                        CTLR_EID, 0x7667, AEM_GET_DYNAMIC_INFO, req);
      CHECK(!f.empty() && f == want,
            "W8h: a truncated record rejects the complete list");
      req.assign(8, 0);
      putbe(&req[0], 4, 2);
      putbe(&req[6], AEM_GET_CONFIGURATION, 2);
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7668);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_BAD_ARGUMENTS, EID,
                        CTLR_EID, 0x7668, AEM_GET_DYNAMIC_INFO, req);
      CHECK(!f.empty() && f == want,
            "W8i: a record data overrun rejects the complete list");

      // Section 7.4.76.1 requires SUCCESS in a command's info_status, but it
      // also requires each parseable element to be handled independently. A
      // malformed status is therefore contained to its record and does not
      // suppress a valid neighbour.
      req.clear(); body.clear();
      append(req, direc(AEM_GET_CONFIGURATION, {}, AECP_NOT_SUPPORTED));
      append(req, direc(AEM_GET_CONFIGURATION, {}));
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x7669);
      append(body, direc(AEM_GET_CONFIGURATION, {}, AECP_BAD_ARGUMENTS));
      append(body, direc(AEM_GET_CONFIGURATION, gcfg_body(), AECP_SUCCESS));
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x7669, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8j: a non-SUCCESS info_status fails only its record");

      // info_status occupies the complete byte. A high bit is not reserved
      // padding, and a malformed later record must not erase an earlier
      // valid result.
      req.clear(); body.clear();
      append(req, direc(AEM_GET_CONFIGURATION, {}));
      append(req, direc(AEM_GET_CONFIGURATION, {}, 0x20));
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x766E);
      append(body, direc(AEM_GET_CONFIGURATION, gcfg_body(), AECP_SUCCESS));
      append(body, direc(AEM_GET_CONFIGURATION, {}, AECP_BAD_ARGUMENTS));
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x766E, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8j2: the complete info_status byte is graded per record");

      // The discriminator is the complete 16-bit info_command_type. The
      // high bit must not be treated as the outer AEM u bit and masked away.
      req = direc(0x8007, {});
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x766A);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_BAD_ARGUMENTS, EID,
                        CTLR_EID, 0x766A, AEM_GET_DYNAMIC_INFO, req);
      CHECK(!f.empty() && f == want,
            "W8k: the record command discriminator remains 16 bits");

      // Exercise every member of the exact thirteen-command whitelist in one
      // request. Empty data gives record BAD_ARGUMENTS for implemented
      // four-byte getters and NOT_SUPPORTED for legal unimplemented getters;
      // neither outcome is an outer BAD_ARGUMENTS rejection.
      const std::vector<uint16_t> whitelist = {
        0x0007, 0x0009, 0x000B, 0x000D, 0x000F, 0x0011, 0x0013,
        0x0015, 0x0017, 0x001D, 0x0029, 0x0048, 0x004A
      };
      req.clear(); body.clear();
      for (uint16_t op : whitelist) {
        append(req, direc(op, {}));
        if (op == AEM_GET_CONFIGURATION) {
          append(body, direc(op, gcfg_body(), AECP_SUCCESS));
        } else if ((op == AEM_GET_STREAM_FORMAT) || (op == 0x000F)
                   || (op == AEM_GET_SAMPLING_RATE)
                   || (op == AEM_GET_CLOCK_SOURCE)
                   || (op == AEM_GET_COUNTERS)) {
          append(body, direc(op, {}, AECP_BAD_ARGUMENTS));
        } else {
          append(body, direc(op, {}, AECP_NOT_SUPPORTED));
        }
      }
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x766B);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x766B, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8l: all thirteen fixed-size getters pass the whitelist");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // Three counter records, one Milan stream-info record and one eight-byte
      // unsupported record total 512 payload bytes. With the 12-byte AECP
      // header this is the exact cdl 524 boundary and must not be skipped.
      req.clear(); body.clear();
      for (int n = 0; n < 3; ++n) {
        append(req, direc(AEM_GET_COUNTERS, ti(0x0005, 0)));
        append(body, direc(AEM_GET_COUNTERS, ctr_body(0x0005, 0),
                           AECP_SUCCESS));
      }
      append(req, direc(0x000F, ti(0x0005, 0)));
      append(body, direc(0x000F, gsi_body(0x0005, 0, true), AECP_SUCCESS));
      name_arg = {0xE1, 0x72, 0x3B, 0xC4, 0x5D, 0xA6, 0x8F, 0x10};
      append(req, direc(0x0011, name_arg));
      append(body, direc(0x0011, name_arg, AECP_NOT_SUPPORTED));
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x766C);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x766C, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want && cdl(f) == 524,
            "W8m: an exact cdl 524 response is retained");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // IEEE 1722.1-2021 9.2.2.6 still caps commands at cdl 524. Milan 5.4.1
      // lifts that limit only for responses. This 525-byte command must be
      // rejected before its single oversized result can be skipped and leave
      // the aggregate length pointing at unwritten response-buffer bytes.
      name_arg.assign(505, 0);
      req = direc(0x0011, name_arg);
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x766D);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_BAD_ARGUMENTS, EID,
                        CTLR_EID, 0x766D, AEM_GET_DYNAMIC_INFO, req);
      CHECK(!f.empty() && f == want && cdl(f) == 525,
            "W8n: an oversized cdl 525 command is rejected exactly");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // The batched GET_CONFIGURATION must use the getter's actual image
      // value. Zero is not evidence because both reset state and the default
      // image contain zero.
      uint32_t ent_off = 0;
      for (auto& e : img_ents) if (e.type == 0x0000) ent_off = e.off;
      CHECK(ent_off != 0, "W8o: the ENTITY entry was located in the image");
      uint8_t save_hi = h.dram[ent_off + 310];
      uint8_t save_lo = h.dram[ent_off + 311];
      h.dram[ent_off + 310] = 0x00;
      h.dram[ent_off + 311] = 0x07;
      req = direc(AEM_GET_CONFIGURATION, {});
      body = direc(AEM_GET_CONFIGURATION,
                   std::vector<uint8_t>{0x00, 0x00, 0x00, 0x07},
                   AECP_SUCCESS);
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x76E0);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x76E0, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8o2: batched GET_CONFIGURATION follows the image value");
      h.dram[ent_off + 310] = save_hi;
      h.dram[ent_off + 311] = save_lo;

      // Wrong-target fixed getters retain their standalone response lengths.
      // A command-sized refusal would shift every following record.
      req.clear(); body.clear();
      append(req, direc(AEM_GET_STREAM_FORMAT, ti(0x0002, 0)));
      append(req, direc(AEM_GET_SAMPLING_RATE, ti(0x0005, 0)));
      append(req, direc(AEM_GET_COUNTERS, ti(0x0000, 0)));
      std::vector<uint8_t> wrong_fmt(12, 0);
      putbe(&wrong_fmt[0], 0x0002, 2);
      std::vector<uint8_t> wrong_rate(8, 0);
      putbe(&wrong_rate[0], 0x0005, 2);
      std::vector<uint8_t> wrong_ctrs(136, 0);
      append(body, direc(AEM_GET_STREAM_FORMAT, wrong_fmt,
                         AECP_NOT_SUPPORTED));
      append(body, direc(AEM_GET_SAMPLING_RATE, wrong_rate,
                         AECP_NOT_SUPPORTED));
      append(body, direc(AEM_GET_COUNTERS, wrong_ctrs,
                         AECP_NOT_SUPPORTED));
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x76E1);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x76E1, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8p: wrong targets retain all three fixed response shapes");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // Exercise the descriptor-image hit arm of GET_SAMPLING_RATE both before
      // another record and as the final record. The four-byte COPY_BUF must
      // not write a second word past its declared response.
      req.clear(); body.clear();
      append(req, direc(AEM_GET_SAMPLING_RATE, ti(0x0002, 0)));
      append(req, direc(AEM_GET_CONFIGURATION, {}));
      append(body, direc(AEM_GET_SAMPLING_RATE,
                         rate_body(0x0002, 0, 96000), AECP_SUCCESS));
      append(body, direc(AEM_GET_CONFIGURATION, gcfg_body(), AECP_SUCCESS));
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x76E2);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x76E2, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8q: sampling-rate hit preserves the following record");

      for (size_t n = 28; n < 32; ++n) h.rmem[n] = 0xA7;
      req = direc(AEM_GET_SAMPLING_RATE, ti(0x0002, 0));
      body = direc(AEM_GET_SAMPLING_RATE,
                   rate_body(0x0002, 0, 96000), AECP_SUCCESS);
      f = ask(AEM_GET_DYNAMIC_INFO, req, 0x76E3);
      want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                        CTLR_EID, 0x76E3, AEM_GET_DYNAMIC_INFO, body);
      CHECK(!f.empty() && f == want,
            "W8q2: final sampling-rate hit is byte-exact");
      CHECK(h.rmem[28] == 0xA7 && h.rmem[29] == 0xA7
            && h.rmem[30] == 0xA7 && h.rmem[31] == 0xA7,
            "W8q3: four-byte COPY_BUF leaves the next word untouched");
    }

    // ---- W9: SET_SAMPLING_RATE, and the overlay it creates --------------
    // Milan 5.4.2.13 / IEEE 7.4.21.1: the command and its response share
    // Figure 7-45, so the answer is the same 8-byte body the getter emits and
    // it carries the value now in force. The check that matters is the one
    // AFTER: a GET must stop reading the image and start reading the setting.
    {
      std::vector<uint8_t> pl(8, 0);
      putbe(&pl[0], 0x0002, 2); putbe(&pl[2], 0, 2);
      putbe(&pl[4], 48000u, 4);                 // the image says 96000
      auto f = ask(AEM_SET_SAMPLING_RATE, pl, 0x7670);
      std::vector<uint8_t> body(8, 0);
      putbe(&body[0], 0x0002, 2); putbe(&body[2], 0, 2);
      putbe(&body[4], 48000u, 4);
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x7670, AEM_SET_SAMPLING_RATE, body);
      CHECK(!f.empty() && f == want,
            "W9: SET_SAMPLING_RATE byte-exact, echoing the value it stored");
      if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }

      // THE POINT OF THE WHOLE ROUND: the getter must now answer the
      // CONTROLLER'S value, not the image's 96000. A GET that still read
      // 96000 would mean the overlay arm never fired; one that read 0 would
      // mean it fired but the store did not keep the write.
      auto g = ask(AEM_GET_SAMPLING_RATE, ti(0x0002, 0), 0x7671);
      CHECK(!g.empty() && st(g) == AECP_SUCCESS && cdl(g) == 20,
            "W9b: GET_SAMPLING_RATE answered SUCCESS at cdl 20");
      if (g.size() >= 46) {
        const unsigned long got = ((unsigned long)g[42] << 24)
                                | ((unsigned long)g[43] << 16)
                                | ((unsigned long)g[44] << 8) | g[45];
        CHECK(got == 48000ul,
              "W9b2: GET_SAMPLING_RATE reads %lu, the SET stored 48000", got);
      }
    }

    // ---- W10: SET_CLOCK_SOURCE, the only writer of the live index --------
    {
      std::vector<uint8_t> pl(8, 0);
      putbe(&pl[0], 0x0024, 2); putbe(&pl[2], 0, 2);
      putbe(&pl[4], 0x0002, 2);                 // reserved stays 0
      auto f = ask(AEM_SET_CLOCK_SOURCE, pl, 0x7672);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS && cdl(f) == 20,
            "W10: SET_CLOCK_SOURCE answered SUCCESS at cdl 20");
      if (f.size() >= 48) {
        CHECK((((unsigned)f[42] << 8) | f[43]) == 0x0002,
              "W10b: the response carries the index it stored");
        CHECK((((unsigned)f[44] << 8) | f[45]) == 0,
              "W10c: reserved @30 is zero");
      }
      auto g = ask(AEM_GET_CLOCK_SOURCE, ti(0x0024, 0), 0x7673);
      if (g.size() >= 46) {
        CHECK((((unsigned)g[42] << 8) | g[43]) == 0x0002,
              "W10d: GET_CLOCK_SOURCE reads %u, the SET stored 2",
              (((unsigned)g[42] << 8) | g[43]));
      }
    }

    // ---- W11: a SET's refusals ------------------------------------------
    // A short command never reached its argument, and storing whatever the
    // slot held would be worse than refusing. A wrong target is Table 7-141's
    // NOT_SUPPORTED, in the full body like every other refusal here.
    {
      std::vector<uint8_t> shortpl(4, 0);
      putbe(&shortpl[0], 0x0002, 2);
      auto f = ask(AEM_SET_SAMPLING_RATE, shortpl, 0x7674);
      CHECK(!f.empty() && st(f) == AECP_BAD_ARGUMENTS,
            "W11: a SET_SAMPLING_RATE short of its value is BAD_ARGUMENTS");
      CHECK(cdl(f) == 20, "W11b: ...in the full body, cdl %d", cdl(f));

      std::vector<uint8_t> wrong(8, 0);
      putbe(&wrong[0], 0x0005, 2);          // a STREAM_INPUT, not an Audio Unit
      putbe(&wrong[4], 48000u, 4);
      f = ask(AEM_SET_SAMPLING_RATE, wrong, 0x7675);
      CHECK(!f.empty() && st(f) == AECP_NOT_SUPPORTED,
            "W11c: a SET on the wrong descriptor type is NOT_SUPPORTED");
      CHECK(cdl(f) == 20, "W11d: ...in the full body, cdl %d", cdl(f));

      // ...and neither refusal may have moved the stored value
      auto g = ask(AEM_GET_SAMPLING_RATE, ti(0x0002, 0), 0x7676);
      if (g.size() >= 46) {
        const unsigned long got = ((unsigned long)g[42] << 24)
                                | ((unsigned long)g[43] << 16)
                                | ((unsigned long)g[44] << 8) | g[45];
        CHECK(got == 48000ul,
              "W11e: a refused SET changed the stored rate to %lu", got);
      }
    }

    // ---- W12: the IDENTIFY control (Milan 5.4.2.17/.18, 5.3.12) ---------
    // The response body is FIVE bytes, not eight: IEEE 7.3.5.2 gives the
    // Identify control one CONTROL_LINEAR_UINT8 value, so cdl is 17. Getting
    // that wrong is the whole class of bug the 0x004A round was about.
    {
      auto g = ask(AEM_GET_CONTROL, ti(0x001A, 0), 0x7680);
      CHECK(!g.empty() && st(g) == AECP_SUCCESS,
            "W12: GET_CONTROL answered SUCCESS");
      CHECK(cdl(g) == 17, "W12b: cdl is 17 (5 payload bytes), got %d", cdl(g));
      CHECK(g.size() > 42 && g[42] == 0,
            "W12c: Milan 5.3.12 makes the reset value 0, got %u",
            g.size() > 42 ? (unsigned)g[42] : 999u);

      // 255 = identifying (5.3.12), and the response carries what is now in
      // force. A device that stored it but answered the old value would pass
      // a naive echo check and fail a controller's read-back.
      std::vector<uint8_t> pl(5, 0);
      putbe(&pl[0], 0x001A, 2); putbe(&pl[2], 0, 2);
      pl[4] = 255;
      auto f = ask(AEM_SET_CONTROL, pl, 0x7681);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS && cdl(f) == 17,
            "W12d: SET_CONTROL answered SUCCESS at cdl 17");
      CHECK(f.size() > 42 && f[42] == 255,
            "W12e: the response carries the value it stored");

      g = ask(AEM_GET_CONTROL, ti(0x001A, 0), 0x7682);
      CHECK(g.size() > 42 && g[42] == 255,
            "W12f: GET_CONTROL now reads 255, got %u",
            g.size() > 42 ? (unsigned)g[42] : 999u);

      // ...and back to 0
      pl[4] = 0;
      f = ask(AEM_SET_CONTROL, pl, 0x7683);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS,
            "W12g: SET_CONTROL back to 0 accepted");
      g = ask(AEM_GET_CONTROL, ti(0x001A, 0), 0x7684);
      CHECK(g.size() > 42 && g[42] == 0, "W12h: ...and reads back 0");
    }

    // ---- W13: only 0 and 255 are legal ----------------------------------
    // IEEE 7.3.5.2 gives the Identify control minimum 0, maximum 255 and STEP
    // 255, so the step alone admits exactly two values; 7.4.25 makes anything
    // else BAD_ARGUMENTS. 128 is the value a device that only range-checked
    // min/max would wrongly accept.
    {
      std::vector<uint8_t> pl(5, 0);
      putbe(&pl[0], 0x001A, 2); putbe(&pl[2], 0, 2);
      pl[4] = 128;
      auto f = ask(AEM_SET_CONTROL, pl, 0x7685);
      CHECK(!f.empty() && st(f) == AECP_BAD_ARGUMENTS,
            "W13: SET_CONTROL 128 is BAD_ARGUMENTS (step 255)");
      CHECK(cdl(f) == 17, "W13b: ...in the CONTROL body, cdl %d", cdl(f));

      auto g = ask(AEM_GET_CONTROL, ti(0x001A, 0), 0x7686);
      CHECK(g.size() > 42 && g[42] == 0,
            "W13c: the refused SET did not change the value");

      // a wrong descriptor type refuses NOT_SUPPORTED, still at cdl 17
      f = ask(AEM_GET_CONTROL, ti(0x0002, 0), 0x7687);
      CHECK(!f.empty() && st(f) == AECP_NOT_SUPPORTED && cdl(f) == 17,
            "W13d: GET_CONTROL on an AUDIO_UNIT is NOT_SUPPORTED at cdl 17");
    }

    // ---- W15: SET_CONFIGURATION and the STREAM_IS_RUNNING reduction ------
    // Milan 5.4.2.5: "shall not accept a SET_CONFIGURATION command if ONE OF
    // the Stream Input is bound or ONE OF the Stream Output is streaming".
    //
    // THE BIND IS MADE THE REAL WAY. Section S6 bound sink 0 with an actual
    // ACMP BIND_RX and this bench observes dbg_bound0_o rather than forcing
    // it, so the refusal below is graded against a stream that is genuinely
    // bound. A test that forced the predicate would prove only that the
    // dispatch arm reads its own input.
    {
      CHECK(h.d->dbg_bound0_o == 1,
            "W15: sink 0 is genuinely bound (S6's ACMP BIND_RX) before the "
            "refusal is graded");

      std::vector<uint8_t> pl(4, 0);
      putbe(&pl[2], 0x0000, 2);                 // reserved @24, cfg index @26
      auto f = ask(AEM_SET_CONFIGURATION, pl, 0x7695);
      CHECK(!f.empty() && st(f) == AECP_STREAM_IS_RUNNING,
            "W15b: SET_CONFIGURATION refuses STREAM_IS_RUNNING while a sink "
            "is bound, got status %d", st(f));
      CHECK(cdl(f) == 16, "W15c: ...in the 4-byte response form, cdl %d",
            cdl(f));
      CHECK(f.size() >= 42 && (((unsigned)f[38] << 8) | f[39]) == 0,
            "W15d: reserved @24 is zero in the refusal");

      // and GET_CONFIGURATION is read-only, so it is correctly exempt
      auto g = ask(AEM_GET_CONFIGURATION, {}, 0x7696);
      CHECK(!g.empty() && st(g) == AECP_SUCCESS,
            "W15e: GET_CONFIGURATION is read-only and stays SUCCESS while "
            "bound");
    }

    // ---- W16: unbind, and the same command now succeeds -------------------
    // The other half of the gate. A refusal that never lifts is as wrong as
    // one that never fires, and this is the check that tells them apart.
    {
      auto unbind = acmp_frame(CTLR_MAC, 8, 0, 0, CTLR_EID, T1_EID, EID,
                               T1_UID, 0, 0, 0, 0x1234, 0, 0);
      h.feed(unbind);
      h.wait_any(h.q_acmp, 600);
      h.idle(200);
      CHECK(h.d->dbg_bound0_o == 0, "W16: UNBIND_RX cleared the bound state");

      //! ---- W16a: the ENTITY_LOCKED arm's IMAGE path -------------------
      //! This is the only window in the run where nothing is running AND the
      //! dynamic store is still unwritten; the two conditions that make the
      //! locked arm reach its image fallback. W3c could not get here because
      //! sink 0 was still bound, so the refusal was taken at dispatch before
      //! CHECK_LOCK ran. Poke the image the way W3b does, so the arm's answer
      //! is a value neither the store nor a hardcoded zero can produce.
      {
        uint32_t ent_off = 0;
        for (auto& e : img_ents) if (e.type == 0x0000) ent_off = e.off;
        uint8_t hi = h.dram[ent_off + 310], lo = h.dram[ent_off + 311];
        h.dram[ent_off + 310] = 0x00; h.dram[ent_off + 311] = 0x07;

        std::vector<uint8_t> lk(16, 0);              // flags 0 = LOCK
        auto l = ask(0x0001, lk, 0x7690);
        CHECK(!l.empty() && st(l) == AECP_SUCCESS,
              "W16a: the bench holds the lock, nothing running, store unwritten");
        std::vector<uint8_t> q(4, 0);
        putbe(&q[2], 0x0001, 2);
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR2_EID, 0x7691,
                          AEM_SET_CONFIGURATION, q));
        auto x = h.wait_any(h.q_aecp, 600);
        CHECK(!x.empty() && st(x) == AECP_ENTITY_LOCKED,
              "W16a2: a foreign controller is refused ENTITY_LOCKED, got %d",
              st(x));
        CHECK(x.size() >= 42 && (((unsigned)x[40] << 8) | x[41]) == 0x0007,
              "W16a3: ...and that arm falls back to the IMAGE's current "
              "configuration, not the unwritten store's 0; got %u",
              x.size() >= 42 ? (((unsigned)x[40] << 8) | x[41]) : 999u);
        CHECK(cdl(x) == 16, "W16a4: ...at cdl 16, got %d", cdl(x));

        std::vector<uint8_t> ul(16, 0);
        putbe(&ul[2], 1, 2);                         // flags = UNLOCK
        ask(0x0001, ul, 0x7692);
        h.dram[ent_off + 310] = hi; h.dram[ent_off + 311] = lo;
      }

      std::vector<uint8_t> pl(4, 0);
      putbe(&pl[2], 0x0000, 2);
      auto f = ask(AEM_SET_CONFIGURATION, pl, 0x7697);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS && cdl(f) == 16,
            "W16b: SET_CONFIGURATION succeeds once nothing is running, got "
            "status %d", st(f));
      CHECK(f.size() >= 42 && (((unsigned)f[40] << 8) | f[41]) == 0x0000,
            "W16c: ...echoing the configuration_index it stored");

      // ...and the getter reads the setting back out of the dynamic store
      auto g = ask(AEM_GET_CONFIGURATION, {}, 0x7698);
      CHECK(!g.empty() && st(g) == AECP_SUCCESS && g.size() >= 42
            && (((unsigned)g[40] << 8) | g[41]) == 0x0000,
            "W16d: GET_CONFIGURATION reads the value SET_CONFIGURATION stored");
    }

    // ---- W17: the OTHER predicate half, a STREAMING OUTPUT ---------------
    // Milan 5.3.7.3 makes a Stream Output "streaming" only when BOTH halves
    // hold: this talker declares Advertise AND a downstream Listener has
    // registered Ready (or Ready Failed) for it. W15/W16 graded the Stream
    // Input half (bound). Without this section the `|| (|strm_streaming_i)`
    // term of the reduction could be deleted outright and every check in
    // this bench would still pass, leaving half the gate unverified.
    //
    // NOTHING IS FORCED. src 0 has been declaring Advertise since S10 opened
    // the MAAP DA gate; the missing half arrives as a real inbound MSRP
    // Listener attribute on the wire. Sink 0 is UNBOUND here (W16 unbound
    // it), so a refusal below can only come from the output half.
    {
      const uint64_t SID_T0 = (OWN_MAC << 16) | 0x0000;

      CHECK(h.d->dbg_bound0_o == 0,
            "W17: no Stream Input is bound; the input half cannot be what "
            "refuses below");
      CHECK(((h.snap(13) >> 16) & 3) == 1,
            "W17a: src 0 still declares Talker Advertise (S10's MAAP grant)");

      // one half is not enough: Advertise alone is NOT streaming (5.3.7.3)
      CHECK((h.snap(13) & 3) == 0,
            "W17c: no Listener has registered yet, so lstn_reg_state[0] is 0");
      CHECK(h.d->dbg_streaming0_o == 0,
            "W17d: Advertise WITHOUT a registered Listener is not streaming "
            "both halves are required");
      {
        std::vector<uint8_t> pl(4, 0);
        putbe(&pl[2], 0x0001, 2);
        auto f = ask(AEM_SET_CONFIGURATION, pl, 0x769A);
        CHECK(!f.empty() && st(f) == AECP_SUCCESS,
              "W17e: ...and SET_CONFIGURATION is accepted in that state, got "
              "status %d", st(f));
      }

      // now the second half arrives on the wire: a peer declares Listener
      // Ready for the stream this talker advertises
      h.sync_join();
      Msg lsn{3, 8, true, {Vec{false, 1, fv_sid(SID_T0),
                               {EV_JOININ}, {DECL_READY}}}};
      h.feed(mrpdu_frame(true, T1_MAC, {lsn}));
      h.run_ms(30);
      CHECK((h.snap(13) & 3) == 2,
            "W17f: the inbound Listener Ready registered, lstn_reg_state[0] "
            "is READY, got %u", h.snap(13) & 3);
      CHECK(h.d->dbg_streaming0_o == 1,
            "W17g: Advertise AND a registered Listener; Stream Output 0 is "
            "STREAMING per 5.3.7.3");

      std::vector<uint8_t> pl(4, 0);
      putbe(&pl[2], 0x0000, 2);
      auto f = ask(AEM_SET_CONFIGURATION, pl, 0x769B);
      CHECK(!f.empty() && st(f) == AECP_STREAM_IS_RUNNING,
            "W17h: SET_CONFIGURATION refuses STREAM_IS_RUNNING while a "
            "Stream Output is streaming, got status %d", st(f));
      CHECK(cdl(f) == 16, "W17i: ...in the 4-byte response form, cdl %d",
            cdl(f));
      //! IEEE 7.4.7.1 again: the refusal carries the CURRENT configuration,
      //! which W17e moved to 1, not the image or rejected value of 0.
      CHECK(f.size() >= 42 && (((unsigned)f[40] << 8) | f[41]) == 0x0001,
            "W17j: the refusal echoes current configuration 1, not 0; got %u",
            f.size() >= 42 ? (((unsigned)f[40] << 8) | f[41]) : 999u);

      //! PRECEDENCE, recorded because nothing orders it. The refusal above
      //! is taken at DISPATCH, before the program runs, so a foreign
      //! controller hitting a locked entity that also has a running stream
      //! gets STREAM_IS_RUNNING rather than ENTITY_LOCKED; E_SCFG's
      //! CHECK_LOCK is never reached. Milan 5.4.2.5 and IEEE 7.4.7.2 both
      //! state their refusal without ordering it against the other, so
      //! either answer conforms; this check exists so the choice is a
      //! decision on the record instead of an accident of dispatch order.
      {
        std::vector<uint8_t> lk(16, 0);                 // flags 0 = LOCK
        auto l = ask(0x0001, lk, 0x76C0);
        CHECK(!l.empty() && st(l) == AECP_SUCCESS,
              "W17m: the bench takes the lock while the output streams");
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR2_EID, 0x76C1,
                          AEM_SET_CONFIGURATION, pl));
        auto x = h.wait_any(h.q_aecp, 600);
        CHECK(!x.empty() && st(x) == AECP_STREAM_IS_RUNNING,
              "W17n: locked AND running, from a foreign controller; the "
              "dispatch-level STREAM_IS_RUNNING wins over ENTITY_LOCKED, "
              "got %d", st(x));
        CHECK(cdl(x) == 16, "W17o: ...still at the full response cdl 16");
        std::vector<uint8_t> ul(16, 0);
        putbe(&ul[2], 1, 2);                            // flags = UNLOCK
        ask(0x0001, ul, 0x76C2);
      }

      // and it LIFTS: the Listener leaves, the output stops streaming, the
      // same command is accepted. A gate that never opens is as wrong as one
      // that never closes.
      h.sync_join();
      Msg lv{3, 8, true, {Vec{false, 1, fv_sid(SID_T0),
                              {EV_LV}, {DECL_READY}}}};
      h.feed(mrpdu_frame(true, T1_MAC, {lv}));
      h.run_ms(30);
      CHECK(h.d->dbg_streaming0_o == 0,
            "W17k: the Listener left; Stream Output 0 stops streaming");
      auto g = ask(AEM_SET_CONFIGURATION, pl, 0x769C);
      CHECK(!g.empty() && st(g) == AECP_SUCCESS,
            "W17l: ...and SET_CONFIGURATION is accepted again, got status %d",
            st(g));
      // put the store back where W18 expects to find it
      std::vector<uint8_t> z(4, 0);
      putbe(&z[2], 0x0000, 2);
      ask(AEM_SET_CONFIGURATION, z, 0x769D);
    }

    // ---- W17b: the TALKER half of 5.3.7.3, graded ------------------------
    // W17 proved a registered Listener is necessary. It did NOT prove the
    // Talker term is doing any work: a review mutated the reduction three
    // ways: deleting the Talker term, widening it to `!= NONE` so a Talker
    // FAILED counts, and widening the Listener test to any non-zero
    // registration so ASKING_FAILED counts, and all 449 checks stayed green.
    // Half of an AND was defended. These two cases close it.
    //
    //   Milan 5.3.7.3 wants ADVERTISE specifically, and READY (or READY
    //   FAILED) specifically. Declaring is not streaming; asking is not ready.
    {
      const uint64_t SID_T0 = (OWN_MAC << 16) | 0x0000;

      // (a) ASKING_FAILED registers, but it is not Ready. A reduction that
      //     tests `|lstn_reg_state[s]` instead of bit 1 calls this streaming.
      h.sync_join();
      Msg af{3, 8, true, {Vec{false, 1, fv_sid(SID_T0),
                              {EV_JOININ}, {DECL_ASKFAIL}}}};
      h.feed(mrpdu_frame(true, T1_MAC, {af}));
      h.run_ms(30);
      CHECK((h.snap(13) & 3) == 1,
            "W17b: an ASKING_FAILED Listener registered, lstn_reg_state[0] "
            "is 1, got %u", h.snap(13) & 3);
      CHECK(((h.snap(13) >> 16) & 3) == 1,
            "W17b2: ...while src 0 still declares Advertise");
      CHECK(h.d->dbg_streaming0_o == 0,
            "W17b3: ASKING_FAILED is not READY; the Stream Output is NOT "
            "streaming");
      {
        std::vector<uint8_t> pl(4, 0);
        auto f = ask(AEM_SET_CONFIGURATION, pl, 0x769E);
        CHECK(!f.empty() && st(f) == AECP_SUCCESS,
              "W17b4: ...so SET_CONFIGURATION is accepted, got status %d",
              st(f));
      }

      // (b) a Talker FAILED is DECLARING but not streaming. Getting there
      //     honestly: re-declare src 0 at a frame size whose Σ-slope busts
      //     the 75 % class ceiling, so admission refuses and the declaration
      //     publishes as MSRP Talker Failed. Nothing is forced.
      h.sync_join();
      Msg rdy{3, 8, true, {Vec{false, 1, fv_sid(SID_T0),
                               {EV_JOININ}, {DECL_READY}}}};
      h.feed(mrpdu_frame(true, T1_MAC, {rdy}));
      h.run_ms(30);
      CHECK(h.d->dbg_streaming0_o == 1,
            "W17b5: back to Advertise + Listener Ready; streaming again "
            "(the control for what follows)");

      //! The re-declaration resets the stream record and takes the Listener
      //! registration with it, so the Listener has to arrive AFTER the
      //! talker is failed; otherwise this section grades 0 && 0 and proves
      //! nothing about which term did the work.
      auto rf = h.svc(OP_DECL_TK, 0, SID_T0, H::maap_da(0), 5, 1500, 0);
      CHECK(rf.got, "W17b6: the over-ceiling re-declaration was answered");
      h.run_ms(300);
      CHECK(((h.snap(3) >> 4) & 1) == 1,
            "W17b7: admission refused it; over_limit is set");
      CHECK(((h.snap(13) >> 16) & 3) == 2,
            "W17b8: src 0 publishes Talker FAILED, tk_decl_state[0] is 2, "
            "got %u", (h.snap(13) >> 16) & 3);
      h.sync_join();
      h.feed(mrpdu_frame(true, T1_MAC, {rdy}));
      h.run_ms(30);
      CHECK((h.snap(13) & 3) == 2,
            "W17b9: ...and a Listener registers READY on it anyway, got %u",
            h.snap(13) & 3);
      CHECK(h.d->dbg_streaming0_o == 0,
            "W17b10: a Talker FAILED is declaring but NOT streaming; "
            "5.3.7.3 wants ADVERTISE, not any declaration");
      {
        std::vector<uint8_t> pl(4, 0);
        auto f = ask(AEM_SET_CONFIGURATION, pl, 0x769F);
        CHECK(!f.empty() && st(f) == AECP_SUCCESS,
              "W17b11: ...so SET_CONFIGURATION is accepted, got status %d",
              st(f));
      }

      // put SRP back: the Listener leaves and src 0 re-declares within the
      // ceiling, so the sections after this one start from a quiet plane
      h.sync_join();
      Msg lv{3, 8, true, {Vec{false, 1, fv_sid(SID_T0),
                              {EV_LV}, {DECL_READY}}}};
      h.feed(mrpdu_frame(true, T1_MAC, {lv}));
      h.svc(OP_DECL_TK, 0, SID_T0, H::maap_da(0), 5, 256, 0);
      h.run_ms(300);
      CHECK(h.d->dbg_streaming0_o == 0,
            "W17b12: the plane is quiet again before the next section");
    }

    // ---- W18: SET_CONFIGURATION with a NON-ZERO index --------------------
    // The first cut of this command never captured configuration_index at all
    // — it stored and echoed 0 forever — and the tests could not see it
    // because they only ever sent 0, which is simultaneously the request, the
    // reset value and the correct answer. A no-op was indistinguishable from
    // a correct implementation. Every check here uses a non-zero value.
    {
      // the image declares 2 configurations for this bench, so 1 is legal
      std::vector<uint8_t> pl(4, 0);
      putbe(&pl[2], 0x0001, 2);
      auto f = ask(AEM_SET_CONFIGURATION, pl, 0x76A4);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS && cdl(f) == 16,
            "W18: SET_CONFIGURATION(1) is SUCCESS at cdl 16, got status %d",
            st(f));
      CHECK(f.size() >= 42 && (((unsigned)f[40] << 8) | f[41]) == 0x0001,
            "W18b: the response echoes the index it was ASKED for, got %u",
            f.size() >= 42 ? (((unsigned)f[40] << 8) | f[41]) : 999u);

      // and the getter must read the store, not the static image
      auto g = ask(AEM_GET_CONFIGURATION, {}, 0x76A5);
      const unsigned get_cfg = g.size() >= 42
                             ? (((unsigned)g[40] << 8) | g[41]) : 999u;
      CHECK(!g.empty() && g.size() >= 42
            && get_cfg == 0x0001,
            "W18c: GET_CONFIGURATION reads 1 back — the round trip exists");

      // IEEE 1722.1-2021 §7.4.8.2 calls that value equivalent to the
      // ENTITY descriptor's current_configuration. The field is the final
      // word of the 312-byte descriptor, at response-frame bytes 352..353.
      std::vector<uint8_t> rdent(8, 0);
      putbe(&rdent[0], CFGIX, 2);                // configuration_index
      putbe(&rdent[4], 0x0000, 2);               // ENTITY
      auto e = ask(AEM_READ_DESCRIPTOR, rdent, 0x76A9);
      CHECK(!e.empty() && st(e) == AECP_SUCCESS && e.size() >= 354,
            "W18c2: READ_DESCRIPTOR(ENTITY) succeeds after SET_CONFIGURATION");
      const unsigned ent_cfg = e.size() >= 354
                             ? (((unsigned)e[352] << 8) | e[353]) : 998u;
      CHECK(e.size() >= 354 && ent_cfg == get_cfg,
            "W18c3: ENTITY.current_configuration equals GET_CONFIGURATION");

      // out of range must NOT be a false success
      std::vector<uint8_t> bad(4, 0);
      putbe(&bad[2], 0xFFFF, 2);
      f = ask(AEM_SET_CONFIGURATION, bad, 0x76A6);
      CHECK(!f.empty() && st(f) == AECP_BAD_ARGUMENTS,
            "W18d: SET_CONFIGURATION(0xFFFF) is BAD_ARGUMENTS, got %d", st(f));
      //! Index equal to configurations_count distinguishes a correct `<`
      //! bound from `<=` or a fixed wider limit. The image declares two.
      std::vector<uint8_t> edge(4, 0);
      putbe(&edge[2], 0x0002, 2);
      auto b = ask(AEM_SET_CONFIGURATION, edge, 0x76AB);
      CHECK(!b.empty() && st(b) == AECP_BAD_ARGUMENTS,
            "W18d2: configurations_count boundary is BAD_ARGUMENTS, got %d",
            st(b));
      CHECK(b.size() >= 42 && (((unsigned)b[40] << 8) | b[41]) == 0x0001,
            "W18d3: boundary refusal echoes current configuration 1; got %u",
            b.size() >= 42 ? (((unsigned)b[40] << 8) | b[41]) : 999u);
      CHECK(cdl(f) == 16, "W18e: ...at cdl 16, got %d", cdl(f));
      //! IEEE 7.4.7.1: "The response always contains the current value ... the
      //! OLD value if it fails." Not the rejected one.
      CHECK(f.size() >= 42 && (((unsigned)f[40] << 8) | f[41]) == 0x0001,
            "W18f: a refusal echoes the CURRENT configuration (1), not the "
            "rejected 0xFFFF — got %u",
            f.size() >= 42 ? (((unsigned)f[40] << 8) | f[41]) : 999u);

      // ...and the refusal changed nothing
      g = ask(AEM_GET_CONFIGURATION, {}, 0x76A7);
      const unsigned get_after_bad = g.size() >= 42
                                   ? (((unsigned)g[40] << 8) | g[41]) : 999u;
      CHECK(!g.empty() && g.size() >= 42
            && get_after_bad == 0x0001,
            "W18g: the refused SET left the configuration at 1");
      e = ask(AEM_READ_DESCRIPTOR, rdent, 0x76AA);
      const unsigned ent_after_bad = e.size() >= 354
                                   ? (((unsigned)e[352] << 8) | e[353]) : 998u;
      CHECK(e.size() >= 354 && ent_after_bad == get_after_bad,
            "W18h: GET and ENTITY still agree after the refused SET");

      //! DELIBERATELY NOT PUT BACK. W19 below grades the ENTITY_LOCKED
      //! refusal's echo, and that arm has its own copy of the current-value
      //! overlay. With the store at 0 its store arm, its image arm and a
      //! hardcoded zero are three indistinguishable answers, which is how a
      //! review mutated that arm's base address, and deleted the arm outright,
      //! with all 468 checks still green. Leaving the store at 1 while the
      //! image reads 0 separates them.
    }

    // ---- W19: the lock outranks these commands too ------------------------
    // Milan repeats in every SET clause that a locked PAAD "shall not accept a
    // <CMD> command from a different controller", and IEEE 7.4.35.2/7.4.36.2
    // put ENTITY_LOCKED ahead of the wrong-target refusal. Deleting CHECK_LOCK
    // from either program left the first cut of this suite green.
    {
      std::vector<uint8_t> lk(16, 0);              // flags 0 = LOCK, ENTITY[0]
      auto l = ask(0x0001, lk, 0x76B0);
      CHECK(!l.empty() && st(l) == AECP_SUCCESS, "W19: the bench holds the lock");

      //! a DIFFERENT controller now tries each command. `ask2` reuses the
      //! transactor with a foreign controller_entity_id.
      auto ask2 = [&](uint16_t op, const std::vector<uint8_t>& p, uint16_t sq) {
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR2_EID, sq, op, p));
        return h.wait_any(h.q_aecp, 600);
      };

      //! the store holds 1 (W18) and the image holds 0, so the echo below
      //! tells the locked arm's overlay apart from a raw read and from a
      //! hardcoded zero. Send 0, a value that is BOTH the image's and NOT
      //! the current one, so echoing the argument is also distinguishable.
      auto gpre = ask(AEM_GET_CONFIGURATION, {}, 0x76B3);
      CHECK(!gpre.empty() && gpre.size() >= 42
            && (((unsigned)gpre[40] << 8) | gpre[41]) == 0x0001,
            "W19a: the store still holds 1 going into the locked refusal");

      std::vector<uint8_t> pl(4, 0);
      putbe(&pl[2], 0x0000, 2);
      auto f = ask2(AEM_SET_CONFIGURATION, pl, 0x76B4);
      CHECK(!f.empty() && st(f) == AECP_ENTITY_LOCKED,
            "W19g: SET_CONFIGURATION from a foreign controller is "
            "ENTITY_LOCKED, got %d", st(f));
      //! The refusal is still a SET_CONFIGURATION response, so 7.4.7.1 binds
      //! it as much as it binds BAD_ARGUMENTS: full response size, carrying
      //! the CURRENT value. Grading only the status byte let a refusal that
      //! echoed the rejected index, or answered at command length, pass.
      CHECK(cdl(f) == 16, "W19g2: ...at the full response cdl 16, got %d",
            cdl(f));
      CHECK(f.size() >= 42 && (((unsigned)f[40] << 8) | f[41]) == 0x0001,
            "W19g3: ...echoing the CURRENT configuration 1, not the rejected "
            "0, not the image's 0, not a hardcoded 0; got %u",
            f.size() >= 42 ? (((unsigned)f[40] << 8) | f[41]) : 999u);
      auto g = ask(AEM_GET_CONFIGURATION, {}, 0x76B5);
      CHECK(!g.empty() && g.size() >= 42
            && (((unsigned)g[40] << 8) | g[41]) == 0x0001,
            "W19h: ...and it did not change the configuration");

      // release, then restore the store for the sections after this one
      std::vector<uint8_t> ul(16, 0);
      putbe(&ul[2], 1, 2);                          // flags = UNLOCK
      ask(0x0001, ul, 0x76B6);
      std::vector<uint8_t> zero(4, 0);
      ask(AEM_SET_CONFIGURATION, zero, 0x76B7);
    }

    // ---- W20: the miss and short-command paths are correctly SIZED --------
    // Only NOT_IMPLEMENTED may answer under the response form's size. The
    // first cut branched past the body builders on the miss path and emitted
    // a bare 12-byte header for a NO_SUCH_DESCRIPTOR.
    {
      std::vector<uint8_t> shortpl(2, 0);
      auto f = ask(AEM_SET_CONFIGURATION, shortpl, 0x76C2);
      CHECK(!f.empty() && st(f) == AECP_BAD_ARGUMENTS,
            "W20f: a truncated SET_CONFIGURATION is BAD_ARGUMENTS");
      CHECK(cdl(f) == 16, "W20g: ...at cdl 16, got %d", cdl(f));

    }

    // ---- W21: START/STOP_STREAMING (Milan 5.4.2.19 / 5.4.2.20) ----------
    // IEEE Figure 7-59 makes command and response the SAME shape - four
    // bytes, {descriptor_type @24, descriptor_index @26} - so every arm
    // here, success and refusal alike, is cdl 16 and echoes what it was
    // asked about. A refusal that shortened the body would be a wire defect
    // no status check could see.
    //
    // Sink 0 is BOUND by section S6 above, and Milan 5.3.8.7 makes the
    // started/stopped state a property of that binding.
    {
      const uint16_t DT_STREAM_INPUT = 0x0005, DT_STREAM_OUTPUT = 0x0006;
      const uint16_t OP_START = 0x0022, OP_STOP = 0x0023;
      //! START/STOP completion now holds the AECP response behind the record
      //! commit or required no-op examination. Read the started mirror with
      //! no post-response delay so these rows grade the response boundary.
      auto started = [&]() { return (unsigned)d->aecp_strm_started_o; };

      // W21bind: establish the precondition IN THIS BLOCK rather than lean
      // on section S6 far above - the sections between it and here bind and
      // unbind sinks, so inheriting that state would make this block's
      // result depend on test ORDER. BIND_RX with flags = 0, i.e.
      // STREAMING_WAIT clear.
      h.q_acmp.clear();
      h.feed(acmp_frame(CTLR_MAC, 6, 0, 0, CTLR_EID, T1_EID, EID,
                        T1_UID, 0, 0, 0, 0x7A00, 0, 0));
      auto bindrsp = h.wait_any(h.q_acmp, 400);
      // never discard the answer to a step a later assertion depends on:
      // a refused bind would make every row below grade an unbound sink
      CHECK(!bindrsp.empty() && bindrsp.size() > 16
            && ((bindrsp[16] >> 3) & 0x1F) == 0,
            "W21bind: BIND_RX for the block's own precondition SUCCEEDED "
            "(status=%d)",
            bindrsp.size() > 16 ? ((bindrsp[16] >> 3) & 0x1F) : -1);
      h.idle(400);                    // ACMP bind still retires after its response

      // W21pre: the PRECONDITION, asserted rather than assumed. Sink 0 was
      // bound by S6 with STREAMING_WAIT clear, so Milan 5.3.8.7 + IEEE
      // 7.4.35 make it STARTED. Without this row, W21d below ("STOP cleared
      // the bit") passes just as well when the bit was never set - the
      // expected value would coincide with the reset value and the check
      // could not fail.
      unsigned sb = started();
      CHECK((sb & 1u) == 1u,
            "W21pre: a bind with STREAMING_WAIT clear leaves sink 0 STARTED "
            "(started=0x%02X)", sb);

      // W21a: STOP on the bound sink succeeds, echoing its own descriptor
      auto f = ask(OP_STOP, ti(DT_STREAM_INPUT, 0), 0x7700);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS,
            "W21a: STOP_STREAMING on a bound Stream Input is SUCCESS (st=%d)",
            f.empty() ? -1 : st(f));
      CHECK(cdl(f) == 16, "W21b: ...at cdl 16, got %d", cdl(f));
      CHECK(f.size() >= 42 && ((f[38] << 8) | f[39]) == DT_STREAM_INPUT
            && ((f[40] << 8) | f[41]) == 0,
            "W21c: ...echoing {STREAM_INPUT, 0} at @24");

      // W21d: and it REACHED the record - the started view must now be clear.
      // Reading the fabric-facing bit is the point: a response that says
      // SUCCESS while the bit never moved is the false success this whole
      // ticket exists to make impossible.
      sb = started();
      CHECK((sb & 1u) == 0u,
            "W21d: STOP_STREAMING did not clear the started bit "
            "(started=0x%02X)", sb);

      // W21e: repeating it is still SUCCESS and still changes nothing
      f = ask(OP_STOP, ti(DT_STREAM_INPUT, 0), 0x7701);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS && cdl(f) == 16,
            "W21e: a repeated STOP_STREAMING is SUCCESS (5.4.2.20's Note)");
      sb = started();
      CHECK((sb & 1u) == 0u,
            "W21f: ...and the bit is still clear (started=0x%02X)", sb);

      // W21g: START puts it back
      f = ask(OP_START, ti(DT_STREAM_INPUT, 0), 0x7702);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS && cdl(f) == 16,
            "W21g: START_STREAMING on a bound, stopped Stream Input");
      sb = started();
      CHECK((sb & 1u) == 1u,
            "W21h: START_STREAMING did not set the started bit "
            "(started=0x%02X)", sb);

      // W21i: a Stream OUTPUT is NOT_SUPPORTED - Milan 5.4.2.19 says so in
      // as many words, and 5.3.7.3 excludes a stopped Stream Output entirely
      f = ask(OP_START, ti(DT_STREAM_OUTPUT, 0), 0x7703);
      CHECK(!f.empty() && st(f) == AECP_NOT_SUPPORTED,
            "W21i: START_STREAMING on a Stream Output is NOT_SUPPORTED "
            "(st=%d)", f.empty() ? -1 : st(f));
      CHECK(cdl(f) == 16, "W21j: ...at cdl 16, got %d", cdl(f));
      CHECK(f.size() >= 42 && ((f[38] << 8) | f[39]) == DT_STREAM_OUTPUT,
            "W21k: ...echoing the type it refused");
      sb = started();
      CHECK((sb & 1u) == 1u,
            "W21l: a refused Stream Output command moved a Stream Input bit "
            "(started=0x%02X)", sb);

      // W21m: so is any other type. A locate on {ENTITY, 0} HITS, so this is
      // the row that proves the type is checked BEFORE the write and not
      // left to the descriptor lookup.
      f = ask(OP_STOP, ti(0x0000, 0), 0x7704);
      CHECK(!f.empty() && st(f) == AECP_NOT_SUPPORTED && cdl(f) == 16,
            "W21m: STOP_STREAMING on ENTITY is NOT_SUPPORTED (st=%d)",
            f.empty() ? -1 : st(f));
      sb = started();
      CHECK((sb & 1u) == 1u,
            "W21n: ...and it did not stop sink 0 on the way past "
            "(started=0x%02X)", sb);

      // W21o: an index the image does not hold is NO_SUCH_DESCRIPTOR
      f = ask(OP_START, ti(DT_STREAM_INPUT, 0x00FF), 0x7705);
      CHECK(!f.empty() && st(f) == AECP_NO_SUCH_DESCRIPTOR,
            "W21o: a nonexistent Stream Input is NO_SUCH_DESCRIPTOR (st=%d)",
            f.empty() ? -1 : st(f));
      CHECK(cdl(f) == 16, "W21p: ...at cdl 16, got %d", cdl(f));

      // W21q: too short to carry Figure 7-59's four bytes
      std::vector<uint8_t> shortpl(2, 0);
      f = ask(OP_START, shortpl, 0x7706);
      CHECK(!f.empty() && st(f) == AECP_BAD_ARGUMENTS,
            "W21q: a truncated START_STREAMING is BAD_ARGUMENTS (st=%d)",
            f.empty() ? -1 : st(f));
      CHECK(cdl(f) == 16,
            "W21r: ...still at the response form's cdl 16, got %d", cdl(f));

      // W21s: an unbound sink is a no-op that still answers SUCCESS -
      // 5.4.2.19's Note, and 5.3.8.7 calls the state undefined while unbound
      // W21idx: the command's INDEX has to reach the record. Bind sink 1 as
      // well, stop THAT one, and require sink 0 to be untouched. Without a
      // second BOUND sink every request in the suite targets index 0, so
      // `strm_set_index_o = 16'd0` is a mutation nothing can see - a
      // controller stopping sink 3 would stop sink 0 instead.
      {
        h.q_acmp.clear();
        h.feed(acmp_frame(CTLR_MAC, 6, 0, 0, CTLR_EID, T1_EID, EID,
                          T1_UID + 1, 1, 0, 0, 0x7A50, 0, 0));
        auto b1 = h.wait_any(h.q_acmp, 400);
        CHECK(!b1.empty() && b1.size() > 16 && ((b1[16] >> 3) & 0x1F) == 0,
              "W21idx: BIND_RX of sink 1 succeeded (status=%d)",
              b1.size() > 16 ? ((b1[16] >> 3) & 0x1F) : -1);
        h.idle(400);                  // settle the ACMP record write
        unsigned both = started();
        CHECK((both & 0x3u) == 0x3u,
              "W21idx2: both sinks are bound and started (started=0x%02X)",
              both);

        f = ask(OP_STOP, ti(DT_STREAM_INPUT, 1), 0x7A51);
        CHECK(!f.empty() && st(f) == AECP_SUCCESS,
              "W21idx3: STOP on sink 1 is SUCCESS (st=%d)",
              f.empty() ? -1 : st(f));
        unsigned after1 = started();
        CHECK((after1 & 0x2u) == 0u,
              "W21idx4: ...sink 1 STOPPED (started=0x%02X)", after1);
        CHECK((after1 & 0x1u) == 1u,
              "W21idx5: ...and sink 0 was NOT touched (started=0x%02X) - "
              "this is the row that proves the index reaches the record",
              after1);

        // put sink 1 back the way it was found
        h.q_acmp.clear();
        h.feed(acmp_frame(CTLR_MAC, 8, 0, 0, CTLR_EID, T1_EID, EID,
                          T1_UID + 1, 1, 0, 0, 0x7A52, 0, 0));
        (void)h.wait_any(h.q_acmp, 400);
        h.idle(400);                  // settle the ACMP record write
      }

      // (the image holds STREAM_INPUT 0 and 1; sink 1 is unbound again here)
      f = ask(OP_START, ti(DT_STREAM_INPUT, 1), 0x7707);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS && cdl(f) == 16,
            "W21s: START_STREAMING on an UNBOUND sink is SUCCESS (st=%d)",
            f.empty() ? -1 : st(f));
      sb = started();
      CHECK(((sb >> 1) & 1u) == 0u,
            "W21t: ...and it did NOT start an unbound Stream Input "
            "(started=0x%02X)", sb);

      // W21cc: the request must survive a BUSY record walker. The AECP
      // µprogram settles the status (locate, lock) and only then issues the
      // write, so if that write were fire-and-forget it would be DROPPED
      // whenever the ACMP walker happened to be mid-transaction - and the
      // controller would hold a SUCCESS for a change that never happened.
      // Overlap them deliberately: start the AECP command, then push ACMP
      // work in behind it so the walker is occupied when the write lands.
      {
        // ensure a known starting point: started
        (void)ask(OP_START, ti(DT_STREAM_INPUT, 0), 0x7A20);
        CHECK((started() & 1u) == 1u, "W21cc: precondition, sink 0 started");

        // Make it STOP first, so the pair below starts from a known 0.
        (void)ask(OP_STOP, ti(DT_STREAM_INPUT, 0), 0x7A1F);
        CHECK((started() & 1u) == 0u, "W21cc2: ...and stopped for the pair");

        h.q_acmp.clear();
        h.q_aecp.clear();
        //! TWO commands under ACMP load, and what this DOES and does NOT
        //! prove, because the answer changed when the holder landed.
        //!
        //! PROVES: two START/STOP commands issued back to back while the
        //! ACMP walker is mid-transaction BOTH take effect, in order - the
        //! second is not overwritten by the first still draining, and
        //! neither is lost to the walker being busy.
        //!
        //! DOES NOT PROVE: that the engine honours `strm_set_ready_i`.
        //! Mutating `st_ready_w` for region 3 to a constant 1 leaves this
        //! suite fully green, and that is not a gap in the rows below - it is
        //! unreachable from the wire. The holder is one deep and drains at
        //! TOP priority, so by the time a second command's WRITE_ST issues
        //! (a whole response later, single-threaded µCPU) the holder is
        //! empty and ready is high regardless. The listener-side property -
        //! ready DROPS while a request is pending - is graded directly in
        //! tb/acmp_listener (S1c2), where the request can be posted by hand.
        //! Recorded rather than left as an implied claim.
        // LEAD
        for (int i = 0; i < 3; ++i)
          h.feed(acmp_frame(CTLR_MAC, 10, 0, 0, CTLR2_EID, 0, EID,
                            0, 0, 0, 0, uint16_t(0x7A30 + i), 0, 0));
        // ENDLEAD
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7A21,
                          OP_START, ti(DT_STREAM_INPUT, 0)));
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7A22,
                          OP_STOP, ti(DT_STREAM_INPUT, 0)));
        auto rf = h.wait_any(h.q_aecp, 900);
        CHECK(!rf.empty() && st(rf) == AECP_SUCCESS,
              "W21dd: the first overlapped command answered SUCCESS (st=%d)",
              rf.empty() ? -1 : st(rf));
        //! a LONGER window than the first: the second command's WRITE_ST is
        //! exactly the one that meets a full holder, so it stalls until the
        //! walker drains it - which the trailing ACMP burst deliberately
        //! delays. That stall IS the mechanism under test, so the timeout has
        //! to outlast it or the test fails on its own premise.
        auto rf2 = h.wait_any(h.q_aecp, 4000);
        CHECK(!rf2.empty() && st(rf2) == AECP_SUCCESS,
              "W21dd2: ...and so did the second (st=%d)",
              rf2.empty() ? -1 : st(rf2));
        unsigned sb2 = started();
        CHECK((sb2 & 1u) == 0u,
              "W21ee: the SECOND command's effect survived a busy walker "
              "(started=0x%02X) - START then STOP must end STOPPED; a "
              "SUCCESS whose effect was overwritten is the defect the "
              "holder's ready line exists to prevent", sb2);
      }

      // W21w: Milan 5.4.2.19/.20 - "If the PAAD-AE is locked by a
      // controller, it shall not accept a START_STREAMING command from a
      // DIFFERENT controller". Lock as CTLR_EID, then command as CTLR2_EID.
      {
        // The refused command below is graded on its EFFECT as well as its
        // status, so the bit must not already be at the value a refusal
        // would leave it at. START first: now a lock check that failed to
        // fire is visible as the bit going 1 -> 0.
        (void)ask(OP_START, ti(DT_STREAM_INPUT, 0), 0x7A0F);
        CHECK((started() & 1u) == 1u,
              "W21w0: precondition, sink 0 started before the lock rows");

        std::vector<uint8_t> lk(16, 0);          // flags = 0 -> LOCK
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7A10,
                          0x0001, lk));
        auto lr = h.wait_any(h.q_aecp, 600);
        CHECK(!lr.empty() && ((lr[16] >> 3) & 0x1F) == 0,
              "W21w: the block's own LOCK_ENTITY took (status=%d)",
              lr.size() > 16 ? ((lr[16] >> 3) & 0x1F) : -1);

        unsigned before = started();
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR2_EID, 0x7A11,
                          OP_STOP, ti(DT_STREAM_INPUT, 0)));
        auto lf = h.wait_any(h.q_aecp, 600);
        CHECK(!lf.empty() && st(lf) == AECP_ENTITY_LOCKED,
              "W21x: STOP_STREAMING from a different controller is "
              "ENTITY_LOCKED (st=%d)", lf.empty() ? -1 : st(lf));
        CHECK(cdl(lf) == 16, "W21y: ...at cdl 16, got %d", cdl(lf));
        unsigned after = started();
        CHECK(after == before && (after & 1u) == 1u,
              "W21z: a locked-out STOP_STREAMING moved the record anyway "
              "(0x%02X -> 0x%02X) - the sink was STARTED going in, so a "
              "missing lock check shows up here as a 1 -> 0", before, after);

        // ...and the SAME controller is still served
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7A12,
                          OP_STOP, ti(DT_STREAM_INPUT, 0)));
        auto ok = h.wait_any(h.q_aecp, 600);
        CHECK(!ok.empty() && st(ok) == AECP_SUCCESS,
              "W21za: the LOCK HOLDER is still served (st=%d)",
              ok.empty() ? -1 : st(ok));
        CHECK((started() & 1u) == 0u,
              "W21zb: ...and its STOP reached the record");

        std::vector<uint8_t> ul(16, 0);
        putbe(&ul[2], 1, 2);                     // flags = UNLOCK
        h.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x7A13,
                          0x0001, ul));
        (void)h.wait_any(h.q_aecp, 600);
      }

      // W21u: unbind is the lifecycle owner - Milan 5.3.8.7 calls the state
      // "undefined when the Stream Input is not bound", so the bit goes with
      // the binding. This ALSO restores what this block changed: a bound
      // sink makes SET_CONFIGURATION refuse with STREAM_IS_RUNNING
      // (5.4.2.5), which the read-side rows further down depend on.
      (void)ask(OP_START, ti(DT_STREAM_INPUT, 0), 0x7A00);
      CHECK((started() & 1u) == 1u,
            "W21u0: precondition, sink 0 started before the unbind");
      h.q_acmp.clear();
      h.feed(acmp_frame(CTLR_MAC, 8, 0, 0, CTLR_EID, T1_EID, EID,
                        T1_UID, 0, 0, 0, 0x7A01, 0, 0));
      auto unb = h.wait_any(h.q_acmp, 400);
      CHECK(!unb.empty() && unb.size() > 16
            && ((unb[16] >> 3) & 0x1F) == 0,
            "W21u: UNBIND_RX succeeded (status=%d)",
            unb.size() > 16 ? ((unb[16] >> 3) & 0x1F) : -1);
      h.idle(400);                    // ACMP unbind retires after its response
      sb = started();
      CHECK((sb & 1u) == 0u,
            "W21v: unbind cleared the started bit (started=0x%02X)", sb);
    }

    // ---- W8: READ_DESCRIPTOR still intact after the whole section -------
    {
      std::vector<uint8_t> one(4, 0);
      putbe(&one[2], 0x0001, 2);
      ask(AEM_SET_CONFIGURATION, one, 0x765F);

      std::vector<uint8_t> rd(8, 0);
      putbe(&rd[0], CFGIX, 2); putbe(&rd[4], 0x0000, 2);
      auto f = ask(AEM_READ_DESCRIPTOR, rd, 0x7660);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS && cdl(f) == 12 + 4 + 312,
            "W8: READ_DESCRIPTOR intact after the read-side set");
      CHECK(f.size() >= 42 + 312
            && (((unsigned)f[42 + 310] << 8) | f[42 + 311]) == 0x0001,
            "W8b: ENTITY overlay follows the dynamic store; got %u",
            f.size() >= 42 + 312
              ? (((unsigned)f[42 + 310] << 8) | f[42 + 311]) : 999u);
    }

    // ---- W8c: the ENTITY overlay is type-gated --------------------------
    {
      std::vector<uint8_t> rd(8, 0);
      putbe(&rd[0], CFGIX, 2);
      putbe(&rd[4], 0x0022, 2);
      auto f = ask(AEM_READ_DESCRIPTOR, rd, 0x7661);
      CHECK(!f.empty() && st(f) == AECP_SUCCESS && cdl(f) == 12 + 4 + 312,
            "W8c: test-only 312-byte non-ENTITY descriptor is served");
      CHECK(f.size() >= 42 + 312
            && (((unsigned)f[42 + 310] << 8) | f[42 + 311]) == 0xBEEF,
            "W8d: non-ENTITY tail remains unchanged; got %#06x",
            f.size() >= 42 + 312
              ? (((unsigned)f[42 + 310] << 8) | f[42 + 311]) : 0);
    }
  }

  // ==== MP. the INTERNAL MAAP engine (11; IEEE 1722-2016 Annex B) =========
  // A SECOND DUT instance runs the same processor with cfg_maap_internal_i
  // = 1 from reset — the quasi-static select is a pre-enable decision, so
  // flipping it mid-run on the first instance would test a wiring no
  // integration ever has. Everything above ran with 0 and proved the landed
  // external-seam behaviour byte-identical; this section proves the
  // internal answer end to end: the Annex B claim walk on the real MAC
  // stream through the real lane arbiter, the dispatch route into the
  // engine, the DEFEND path, the talker granted from the internal claim
  // with the external port group quiesced, the conflict fan-out closing the
  // DA gate, and an AECP descriptor read regression while all of it runs.
  // Self-contained: fresh DUT, fresh harness, local helpers only.
  {
    auto* d2 = new Vpp_top_wrap;
    H h2(d2);
    h2.dram = h.dram;                       // the same 07 SS3.3 image
    d2->cfg_maap_internal_i = 1;
    d2->cfg_maap_count_i = 8;
    d2->cfg_maap_seed_offset_i = 0;
    d2->cfg_maap_seed_valid_i = 0;
    h2.reset();

    // Annex B frame builder (Figure B.1; 42 real bytes padded to 60)
    auto maap_frame = [](uint64_t da, uint64_t sa, int msg, uint64_t req_s,
                         uint16_t req_c, uint64_t con_s, uint16_t con_c) {
      std::vector<uint8_t> f;
      for (int i = 5; i >= 0; --i) f.push_back(uint8_t(da >> (8 * i)));
      for (int i = 5; i >= 0; --i) f.push_back(uint8_t(sa >> (8 * i)));
      f.push_back(0x22); f.push_back(0xF0);
      f.push_back(0xFE); f.push_back(uint8_t(msg & 0x0F));
      f.push_back(0x08); f.push_back(0x10);            // maap_ver 1, cdl 16
      for (int i = 0; i < 8; ++i) f.push_back(0x00);   // stream_id
      for (int i = 5; i >= 0; --i) f.push_back(uint8_t(req_s >> (8 * i)));
      f.push_back(uint8_t(req_c >> 8)); f.push_back(uint8_t(req_c));
      for (int i = 5; i >= 0; --i) f.push_back(uint8_t(con_s >> (8 * i)));
      f.push_back(uint8_t(con_c >> 8)); f.push_back(uint8_t(con_c));
      while (f.size() < 60) f.push_back(0x00);
      return f;
    };
    auto wait_maap = [&](size_t count, int budget_ms) {
      long cyc = long(budget_ms) * MS_CYC;
      while (h2.q_maap.size() < count && cyc-- > 0) h2.step();
      return h2.q_maap.size() >= count;
    };

    // ---- MP1: link up -> the whole Table B.7 acquisition on the wire ----
    // The claim needs no entity_enable: addresses are owned before the
    // entity advertises, so the talker's very first ALLOC can be granted.
    d2->link_up_i = 1;
    CHECK(wait_maap(5, 4 * 700),
          "MP1: 4 PROBEs + ANNOUNCE within four probe intervals");
    const uint64_t base = d2->maap_addr_o;
    CHECK((base >> 16) == 0x91E0F000ull && (base & 0xFFFFu) <= 0xFE00u - 8u,
          "MP1: claim inside the Table B.9 pool (got %012llx)",
          (unsigned long long)base);
    if (h2.q_maap.size() >= 5) {
      auto pexp = maap_frame(0x91E0F000FF00ull, OWN_MAC, 1, base, 8, 0, 0);
      auto aexp = maap_frame(0x91E0F000FF00ull, OWN_MAC, 3, base, 8, 0, 0);
      for (int k = 0; k < 4; ++k) {
        CHECK(h2.q_maap[size_t(k)].first == pexp,
              "MP1: PROBE %d byte-exact on the MAC stream", k + 1);
        if (h2.q_maap[size_t(k)].first != pexp) {
          dump("got", h2.q_maap[size_t(k)].first); dump("exp", pexp);
        }
      }
      CHECK(h2.q_maap[4].first == aexp, "MP1: first ANNOUNCE byte-exact");
      for (int k = 1; k < 4; ++k) {
        long dt = long(h2.q_maap[size_t(k)].second)
                - long(h2.q_maap[size_t(k) - 1].second);
        CHECK(dt >= 500 && dt <= 601,
              "MP1: probe interval %d = %ld ms outside (500, 600)", k, dt);
      }
      long dta = long(h2.q_maap[4].second) - long(h2.q_maap[3].second);
      CHECK(dta <= 50, "MP1: probeCount! announces immediately (%ld ms)", dta);
    }
    CHECK(d2->maap_addr_valid_o && d2->maap_state_o == 2,
          "MP1: claim published valid in DEFEND");
    CHECK(h2.maap_offers == 0,
          "MP1: the external maap port group stayed quiet (%d offers)",
          h2.maap_offers);

    // ---- MP2: a conflicting PROBE arrives -> DEFEND, byte-exact ---------
    const uint64_t prober = 0x0A1122334455ull;
    h2.q_maap.clear();
    h2.feed(maap_frame(0x91E0F000FF00ull, prober, 1, base + 4, 8, 0, 0));
    CHECK(wait_maap(1, 200), "MP2: DEFEND sent");
    if (!h2.q_maap.empty()) {
      auto dexp = maap_frame(prober, OWN_MAC, 2, base + 4, 8, base + 4, 4);
      CHECK(h2.q_maap[0].first == dexp,
            "MP2: DEFEND byte-exact (echo + B.3.6.6 overlap), unicast");
      if (h2.q_maap[0].first != dexp) {
        dump("got", h2.q_maap[0].first); dump("exp", dexp);
      }
    }
    CHECK(d2->maap_defends_o == 1 && d2->maap_addr_valid_o,
          "MP2: defended, claim kept");

    // ---- MP3: the talker is granted from the INTERNAL claim -------------
    d2->entity_enable_i = 1;
    h2.run_ms(20);
    const uint64_t SID_T0 = (OWN_MAC << 16);       // wrap: sid[k] = {mac, k}
    auto prb = acmp_frame(CTLR_MAC, 0, 0, 0, CTLR_EID, EID, T1_EID,
                          0, 7, 0, 0, 0x6001, 0x000A, 0);
    h2.feed(prb);
    auto p = h2.wait_any(h2.q_acmp, 400);
    // no DA is installed yet at the answer instant: the honest first answer
    auto expp = acmp_frame(OWN_MAC, 1, 3, 0, CTLR_EID, EID, T1_EID,
                           0, 7, 0, 0, 0x6001, 0x000A, 0);
    CHECK(p == expp, "MP3: first PROBE_TX_RESPONSE DEST_MAC_FAILED");
    for (int i = 0; i < 200 && !(d2->acmp_declaring_o & 1); i++) h2.idle(10);
    CHECK(h2.saw_decl_edge(0, true),
          "MP3: the internal grant opened acmp_declaring_o[0]");
    auto gts = acmp_frame(CTLR_MAC, 4, 0, 0, CTLR_EID, EID, 0,
                          0, 0, 0, 0, 0x6002, 0, 0);
    h2.feed(gts);
    auto f2 = h2.wait_any(h2.q_acmp, 400);
    auto expf2 = acmp_frame(OWN_MAC, 5, 0, SID_T0, CTLR_EID, EID, 0,
                            0, 0, base, 0, 0x6002, 0, 2 /* default VID */);
    CHECK(f2 == expf2,
          "MP3: GET_TX_STATE answers the internally granted base + 0");
    if (!f2.empty() && f2 != expf2) { dump("got", f2); dump("exp", expf2); }
    CHECK(h2.maap_offers == 0 && h2.maap_reqs.empty(),
          "MP3: no request ever left the top (%d offers)", h2.maap_offers);

    // ---- MP4: a conflicting ANNOUNCE from a rev-lower peer -> yield -----
    h2.q_maap.clear();
    const uint64_t winner = 0x010000000001ull;     // reversed-octet lower
    h2.feed(maap_frame(0x91E0F000FF00ull, winner, 3, base, 8, 0, 0));
    for (int i = 0; i < 200 && (d2->acmp_declaring_o & 1); i++) h2.idle(10);
    CHECK(h2.saw_decl_edge(0, false),
          "MP4: the conflict fan-out closed the DA gate");
    CHECK(!d2->maap_addr_valid_o || d2->maap_addr_o != base,
          "MP4: the contested claim was withdrawn");
    CHECK(d2->maap_conflicts_o == 1, "MP4: one re-address counted");
    CHECK(wait_maap(5, 5 * 700), "MP4: fresh walk completed");
    const uint64_t base2 = d2->maap_addr_o;
    CHECK(d2->maap_addr_valid_o && base2 != base,
          "MP4: re-claimed on a fresh range (%012llx)",
          (unsigned long long)base2);

    // ---- MP5: MAAP on the AVDECC multicast DA is not for us -------------
    h2.q_maap.clear();
    h2.feed(maap_frame(0x91E0F0010000ull, prober, 1, base2, 8, 0, 0));
    h2.run_ms(60);
    CHECK(h2.q_maap.empty() && d2->maap_defends_o == 1,
          "MP5: mis-addressed PROBE dropped at the DA-qualified subtype gate");

    // ---- MP6: the descriptor path is untouched (the M7 regression) ------
    {
      h2.q_aecp.clear();
      std::vector<uint8_t> rd(8, 0);
      putbe(&rd[0], CFGIX, 2); putbe(&rd[4], 0x0000, 2); putbe(&rd[6], 0, 2);
      h2.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x6003,
                         AEM_READ_DESCRIPTOR, rd));
      auto got = h2.wait_any(h2.q_aecp, 400);
      std::vector<uint8_t> epl(4, 0);
      putbe(&epl[0], CFGIX, 2);
      epl.insert(epl.end(), desc_entity.begin(), desc_entity.end());
      auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID,
                             CTLR_EID, 0x6003, AEM_READ_DESCRIPTOR, epl);
      CHECK(!got.empty(), "MP6: READ_DESCRIPTOR answered with MAAP running");
      CHECK(got == want, "MP6: READ_DESCRIPTOR byte-exact with MAAP running");
    }
    delete d2;
  }

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete d;
  return fails ? 1 : 0;
}
