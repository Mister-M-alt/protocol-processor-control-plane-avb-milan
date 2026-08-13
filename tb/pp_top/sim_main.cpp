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
static const uint16_t AEM_GET_SAMPLING_RATE = 0x0015;
static const uint16_t AEM_IDENTIFY_NOTIF  = 0x0026;
enum { AECP_SUCCESS = 0, AECP_NOT_IMPLEMENTED = 1, AECP_NO_SUCH_DESCRIPTOR = 2,
       AECP_BAD_ARGUMENTS = 7 };

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
  putbe(&f[36], cmd_type & 0x7FFF, 2);            // u = 0
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
  putbe(&d[308], 1, 2);                           // configurations_count
  putbe(&d[310], CFGIX, 2);                       // current_configuration
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
  std::deque<std::vector<uint8_t>> q_aecp;

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
          dram_rd64(dram_addr - 0x20000000u + uint32_t(8 * dram_idx));
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
    dram_busy = false; dram_wait = 0;
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
  };
  std::vector<uint8_t> desc_entity = entity_descriptor();
  std::vector<uint8_t> desc_clkdom = clock_domain_descriptor();
  h.dram = build_image(img_ents, {desc_entity, desc_clkdom},
                       {"PP Reference Entity", "Clock Domain 0"}, 1);

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
    got = cmd(AEM_GET_SAMPLING_RATE, sr_pl, 0x5555);
    want = expect(AECP_NOT_IMPLEMENTED, AEM_GET_SAMPLING_RATE, 0x5555, sr_pl);
    CHECK(!got.empty(), "A5: an unimplemented opcode answered with silence");
    CHECK(got == want, "A5: NOT_IMPLEMENTED echo is not byte-exact");
    if (!got.empty() && got != want) { dump("got ", got); dump("want", want); }

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

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete d;
  return fails ? 1 : 0;
}
