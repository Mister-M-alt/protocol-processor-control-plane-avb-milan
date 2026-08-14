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
static const uint16_t AEM_GET_SAMPLING_RATE = 0x0015;
static const uint16_t AEM_IDENTIFY_NOTIF  = 0x0026;
static const uint16_t AEM_GET_COUNTERS    = 0x0029;
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
  // @22..@23 IS NOT ALWAYS A command_type. This masked bit 15 unconditionally
  // with the comment "u = 0", which is right for an AEM AECPDU (1722.1-2021
  // 9.2.1.7 puts `u` in the top bit of command_type) and wrong for every other
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

static std::vector<uint8_t> stream_port_input_descriptor(uint16_t ix,
                                                         uint16_t clusters,
                                                         uint16_t base_cluster) {
  // §7.2.13 Table 7-23: 20 bytes, no object_name. number_of_maps = 0 is the
  // DYNAMIC-mapping declaration (§7.2.13's convention, restated by Milan
  // §5.3.3.9), which is exactly the shape GET_AUDIO_MAP exists to serve.
  std::vector<uint8_t> d(20, 0);
  putbe(&d[0],  0x000E, 2);                       // descriptor_type
  putbe(&d[2],  ix, 2);                           // descriptor_index
  putbe(&d[4],  0x0000, 2);                       // clock_domain_index
  putbe(&d[6],  0x0000, 2);                       // port_flags
  putbe(&d[8],  0x0000, 2);                       // number_of_controls
  putbe(&d[10], 0x0000, 2);                       // base_control
  putbe(&d[12], clusters, 2);                     // number_of_clusters
  putbe(&d[14], base_cluster, 2);                 // base_cluster
  putbe(&d[16], 0x0000, 2);                       // number_of_maps: dynamic
  putbe(&d[18], 0x0000, 2);                       // base_map (ignored)
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
    if (ty != 0x0005) return 0;              // only STREAM_INPUT is backed
    if (ix == 0) return CTR_MASK_AAF;
    if (ix == 1) return CTR_MASK_CRF;
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
  static uint16_t amap_nmaps(uint16_t ty, uint16_t ix) {
    if (ty != 0x000E) return 0;              // only STREAM_PORT_INPUT backed
    if (ix == 0) return 1;
    if (ix == 1) return 3;
    return 0;
  }
  static uint16_t amap_count(uint16_t ty, uint16_t ix, uint16_t page) {
    if (page >= amap_nmaps(ty, ix)) return 0;
    if (ix == 0) return 2;
    return page == 1 ? 3 : (page == 2 ? 1 : 0);
  }
  //! record k of (port, page): four distinct 16-bit fields keyed on all
  //! three coordinates, so a record served for the wrong port, page or
  //! ordinal cannot match
  static uint64_t amap_rec(uint16_t ix, uint16_t page, uint8_t k) {
    uint16_t tag = uint16_t((ix << 12) | (page << 8) | k);
    return (uint64_t(0x1000 | tag) << 48) | (uint64_t(0x2000 | tag) << 32) |
           (uint64_t(0x3000 | tag) << 16) |  uint64_t(0x4000 | tag);
  }
  static uint64_t amap_value(uint16_t ty, uint16_t ix, uint16_t page,
                             uint8_t sel, uint8_t rec) {
    if (sel == 0) return amap_nmaps(ty, ix);
    if (sel == 1) return (uint64_t(amap_nmaps(ty, ix)) << 16)
                       |  amap_count(ty, ix, page);
    if (sel == 2) return (rec < amap_count(ty, ix, page))
                       ? amap_rec(ix, page, rec) : 0;
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
        d->amap_data_i = amap_value((uint16_t)d->amap_desc_type_o,
                                    (uint16_t)d->amap_desc_index_o,
                                    (uint16_t)d->amap_map_index_o, sel, rec);
        amap_hold_cur = 0;
        ++amap_reads;
        if (amap_seq.empty() || amap_seq.back() != std::make_pair(sel, rec))
          amap_seq.push_back({sel, rec});
      }
    } else {
      amap_hold_cur = 0;
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
  };
  std::vector<uint8_t> desc_entity = entity_descriptor();
  std::vector<uint8_t> desc_clkdom = clock_domain_descriptor();
  //! geometry consistent with H::amap_*: port 0 = 8 clusters at base 0 (one
  //! page of 8), port 1 = 24 clusters at base 8 (three pages of 8)
  std::vector<uint8_t> desc_spi0 = stream_port_input_descriptor(0, 8, 0);
  std::vector<uint8_t> desc_spi1 = stream_port_input_descriptor(1, 24, 8);
  h.dram = build_image(img_ents,
                       {desc_entity, desc_clkdom, desc_spi0, desc_spi1},
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
    //! 0x002B GET_AUDIO_MAP left this sweep when it became a real answer -
    //! its refusals are graded in section Q, including the non-input-port
    //! echo this entry used to cover by accident (payload 0xA0A1... is not
    //! a STREAM_PORT_INPUT type). 0x002C ADD_AUDIO_MAPPINGS holds the
    //! same-payload-shape slot and stays NOT_IMPLEMENTED (the recorded gap).
    struct { uint16_t op; size_t n; const char* what; } nisz[] = {
      {0x7FFE,  0, "unassigned opcode, empty payload"},
      {0x004D,  4, "GET_MAX_TRANSIT_TIME (§7.4.78.1, the Hive 4.3.1 case)"},
      {0x002C,  8, "ADD_AUDIO_MAPPINGS (§7.4.45.1)"},
      {0x0000, 16, "ACQUIRE_ENTITY (§7.4.1.1)"},
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
    // reasoning that @22's top bit is always `u`, and that is only true of an
    // AEM AECPDU (9.2.1.7). Every OUI with bit 7 set came back mangled.
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
    // needs a check too. 9.2.1.7: `u` is the top bit of an AEM AECPDU's
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
    got = cmd(AEM_GET_SAMPLING_RATE, sr_pl, 0xC003);
    CHECK(got == aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_NOT_IMPLEMENTED, EID,
                            CTLR_EID, 0xC003, AEM_GET_SAMPLING_RATE, sr_pl),
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

    // ---- K4: ENTITY — SUCCESS with an EMPTY mask, still full size ---------
    // IEEE Table 7-150 gives the ENTITY descriptor nothing but ENTITY_SPECIFIC
    // bits and Milan makes none of them mandatory, so the honest answer is
    // "no counters here" (§7.4.42.2: a SET bit means the quadlet is valid) —
    // not a mask of ones over a block that never moves
    got = cmd(AEM_GET_COUNTERS, ctr_pl(DT_ENTITY, 0), 0xD003);
    want = expect(AECP_SUCCESS, AEM_GET_COUNTERS, 0xD003,
                  ctr_expect_pl(DT_ENTITY, 0));
    CHECK(got == want, "K4: ENTITY GET_COUNTERS is not byte-exact");
    CHECK(got.size() == 38 + 136,
          "K4: an empty mask still owes the full block, got %zu B", got.size());
    CHECK(valid_mask_of(got) == 0, "K4: ENTITY claimed counters it has none of");

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
        putbe(&p[12 + 8 * size_t(k)], H::amap_rec(ix, page, uint8_t(k)), 8);
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

    // ---- Q5: STREAM_PORT_OUTPUT keeps the NOT_IMPLEMENTED echo ------------
    // The RECORDED gap (Milan §5.4.2.26 also wants outputs with no static
    // map served; this build's talker-side store has a different shape).
    // The refusal is the §9.3.5.3.3 echo - byte-exact, sized by the command
    auto spo_pl = am_pl(DT_SPO, 0, 0);
    got = cmd(AEM_GET_AUDIO_MAP, spo_pl, 0xE007);
    want = expect(AECP_NOT_IMPLEMENTED, 0xE007, spo_pl);
    CHECK(got == want, "Q5: the non-input-port refusal is not the echo");
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
