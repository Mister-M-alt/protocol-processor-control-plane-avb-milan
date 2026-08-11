// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_rx_validator suite — independent expectations, never DUT logic.
//
// Hand-built wire frames, byte-exact, drive the F03.2 flow and the front-end
// F03.6 rules: every V-rule the validator owns violated once (the F09.4 TOL
// rows), a padded 64-B minimum frame parsed by cdl (V2), cdl+12 > frame
// dropped + counted (V1), h/version != 0 (V8), unknown subtype, DA-gate
// misses, MRP frames routed byte-exactly (V9) with LLDP/802.1X on the same
// group DAs ignored, and pool-refusal silence (rx_overrun belongs to the
// pool). Parsed-header beats are compared field-by-field against an
// independent offset model (F03.4/F04.5/F05.13/F06.10 byte offsets read
// straight out of the frame bytes, tail fields read as 0 — V3). The slot
// pool is a C++ stub of KL_pp_rx_slots' write-side port protocol: it grants
// alloc combinationally, accumulates wr bytes until wr_last, and records
// commit-on-good / abort-on-mid-frame-fail.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "VKL_pp_rx_validator.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

typedef std::vector<uint8_t> Bytes;

static const uint64_t OWN_MAC   = 0x02AABBCCDDEEull;
static const uint64_t DA_AVDECC = 0x91E0F0010000ull;
static const uint64_t DA_MSRP   = 0x0180C200000Eull;
static const uint64_t DA_MVRP   = 0x0180C2000021ull;

// pp_pkg protocol codes
enum { P_ADP = 0, P_ACMP = 1, P_AEM = 2, P_MVU = 3, P_AA = 4 };

// ---- independent frame model (spec offsets, not DUT logic) ---------------
struct Hdr {
  uint8_t  protocol = 0, msg_type = 0, status = 0;
  uint16_t cdl = 0, seq = 0, opcode = 0;
  uint64_t src_mac = 0, ctlr = 0, target = 0, operands = 0;
  bool     u = false, cr = false;
};
struct Expect {
  int  da = 0, et = 0, sub = 0, ver = 0, len = 0;   // counter deltas
  bool mrp = false, alloc = false, commit = false, abort_ = false;
  Bytes slot;
  Hdr   hdr;
};

static uint64_t mac48(const Bytes& f, size_t off) {
  uint64_t v = 0;
  for (int i = 0; i < 6; ++i) v = (v << 8) | f[off + i];
  return v;
}

static Expect classify(const Bytes& f, bool pool_ok) {
  Expect e;
  size_t n = f.size();
  if (n < 6) { e.da = 1; return e; }                 // DA never completed
  uint64_t da = mac48(f, 0);
  if (da == DA_MSRP || da == DA_MVRP) {              // V9 branch, never counted
    if (n >= 14) {
      uint16_t et = uint16_t(f[12]) << 8 | f[13];
      if ((da == DA_MSRP && et == 0x22EA) || (da == DA_MVRP && et == 0x88F5))
        e.mrp = true;                                // else LLDP/802.1X: ignore
    }
    return e;
  }
  if (da != OWN_MAC && da != DA_AVDECC) { e.da = 1; return e; }
  if (n < 14) { e.et = 1; return e; }                // EtherType never completed
  uint16_t et = uint16_t(f[12]) << 8 | f[13];
  if (et != 0x22F0) { e.et = 1; return e; }
  if (n == 14) { e.len = 1; return e; }              // no payload, no alloc
  e.alloc = true;
  if (!pool_ok) return e;                            // pool counts rx_overrun
  uint8_t sub = f[14];
  if (sub != 0xFA && sub != 0xFB && sub != 0xFC) { e.sub = 1; e.abort_ = true; return e; }
  if (n >= 16 && (f[15] & 0xF0)) { e.ver = 1; e.abort_ = true; return e; }  // V8
  size_t pl = n - 14;
  if (pl < 4) { e.len = 1; e.abort_ = true; return e; }
  size_t cdl = (size_t(f[16] & 7) << 8) | f[17];
  if (pl < cdl + 12 || cdl + 12 > 576) { e.len = 1; e.abort_ = true; return e; }  // V1
  e.commit = true;
  e.slot.assign(f.begin() + 14, f.begin() + 14 + long(cdl + 12));   // V2: by cdl
  // header fields at the documented offsets; tail fields read as 0 (V3)
  auto rd = [&](size_t off) -> uint8_t { return (off < cdl + 12) ? f[14 + off] : 0; };
  auto r16 = [&](size_t off) -> uint16_t { return uint16_t(rd(off)) << 8 | rd(off + 1); };
  auto r64 = [&](size_t off) -> uint64_t {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | rd(off + size_t(i));
    return v;
  };
  Hdr& h = e.hdr;
  h.msg_type = f[15] & 0x0F;
  h.status   = f[16] >> 3;
  h.cdl      = uint16_t(cdl);
  h.src_mac  = mac48(f, 6);
  h.target   = r64(4);
  if (sub == 0xFA) {                                 // ADP (F04.5)
    h.protocol = P_ADP; h.opcode = h.msg_type;
  } else if (sub == 0xFC) {                          // ACMP (F05.13)
    h.protocol = P_ACMP; h.opcode = h.msg_type;
    h.ctlr = r64(12); h.seq = r16(48);
    uint16_t uid = (h.msg_type >= 6 && h.msg_type <= 11) ? r16(38) : r16(36);
    h.operands = uid;                                // unique_id lane [15:0]
  } else {                                           // AECP (F06.10)
    h.ctlr = r64(12); h.seq = r16(20);
    h.u = (rd(22) >> 7) & 1; h.cr = (rd(22) >> 6) & 1;
    h.opcode = uint16_t(rd(22) & 0x3F) << 8 | rd(23);
    h.protocol = (h.msg_type == 6 || h.msg_type == 7) ? P_MVU
               : (h.msg_type == 2 || h.msg_type == 3) ? P_AA : P_AEM;
    h.operands = (uint64_t(r16(24)) << 48) | (uint64_t(r16(26)) << 32);
  }
  return e;
}

// ---- frame builders ------------------------------------------------------
static void put_mac(Bytes& f, uint64_t m) {
  for (int i = 5; i >= 0; --i) f.push_back(uint8_t(m >> (8 * i)));
}
static void put64(Bytes& f, uint64_t v) {
  for (int i = 7; i >= 0; --i) f.push_back(uint8_t(v >> (8 * i)));
}
static void put16(Bytes& f, uint16_t v) { f.push_back(uint8_t(v >> 8)); f.push_back(uint8_t(v)); }

static Bytes eth(uint64_t da, uint64_t sa, uint16_t et) {
  Bytes f; put_mac(f, da); put_mac(f, sa); put16(f, et); return f;
}
static void fill_pat(Bytes& f, size_t upto, uint8_t seed) {
  while (f.size() < upto) f.push_back(uint8_t(seed + f.size() * 13));
}

// ---- harness with the slot-pool port-protocol stub -----------------------
struct Hn {
  VKL_pp_rx_validator* d;
  bool  pool_allow = true;
  bool  stub_active = false, stub_closed = false;
  int   stub_slot = -1, next_slot = 0;
  Bytes stub_bytes;
  int   allocs = 0, commits = 0, aborts = 0, hdrs = 0;
  Bytes last_commit; int last_commit_slot = -1;
  Bytes mrp; int mrp_lasts = 0;
  Hdr   hg; uint8_t hg_slot = 7;

  explicit Hn(VKL_pp_rx_validator* dd) : d(dd) {}

  void tick(bool v, uint8_t data, bool last) {
    d->rx_valid_i = v; d->rx_data_i = data; d->rx_last_i = last;
    d->alloc_gnt_i = 0; d->alloc_slot_i = 0;
    d->clk_i = 0; d->eval();
    if (d->alloc_req_o) {                    // combinational grant, pool style
      ++allocs;
      if (pool_allow && !stub_active) {
        d->alloc_gnt_i = 1; d->alloc_slot_i = uint8_t(next_slot);
        stub_active = true; stub_closed = false;
        stub_slot = next_slot; stub_bytes.clear();
        next_slot = (next_slot + 1) & 3;
        d->eval();
      }
    }
    // sample registered outputs pre-edge (byte accept before abort, as pool)
    if (d->wr_valid_o && stub_active && !stub_closed && stub_bytes.size() < 576) {
      stub_bytes.push_back(d->wr_data_o);
      if (d->wr_last_o) stub_closed = true;
    }
    if (d->wr_abort_o && stub_active) { stub_active = false; ++aborts; }
    else if (d->wr_commit_o && stub_active) {
      stub_active = false; ++commits;
      last_commit = stub_bytes; last_commit_slot = stub_slot;
    }
    if (d->mrp_valid_o) { mrp.push_back(d->mrp_data_o); if (d->mrp_last_o) ++mrp_lasts; }
    if (d->hdr_valid_o) {
      ++hdrs;
      hg.protocol = d->hdr_protocol_o;  hg.msg_type = d->hdr_msg_type_o;
      hg.status = d->hdr_status_o;      hg.cdl = d->hdr_cdl_o;
      hg.src_mac = d->hdr_src_mac_o;    hg.ctlr = d->hdr_controller_eid_o;
      hg.target = d->hdr_target_eid_o;  hg.seq = d->hdr_sequence_id_o;
      hg.u = d->hdr_u_o; hg.cr = d->hdr_cr_o;
      hg.opcode = d->hdr_opcode_o;      hg.operands = d->hdr_operands_o;
      hg_slot = d->hdr_rx_slot_o;
    }
    d->clk_i = 1; d->eval();
  }
  void idle(int n = 1) { for (int i = 0; i < n; ++i) tick(false, 0, false); }
  void frame(const Bytes& f, int gap = 0) {
    for (size_t i = 0; i < f.size(); ++i) {
      tick(true, f[i], i + 1 == f.size());
      if (gap) idle(gap);
    }
  }
};

// ---- per-case runner -----------------------------------------------------
struct Snap { int da, et, sub, ver, len, allocs, commits, aborts, hdrs; size_t mrp; };
static Snap snap(Hn& h) {
  return { int(h.d->rx_da_count_o), int(h.d->rx_ethertype_count_o),
           int(h.d->rx_subtype_count_o), int(h.d->rx_version_count_o),
           int(h.d->rx_length_count_o), h.allocs, h.commits, h.aborts,
           h.hdrs, h.mrp.size() };
}

static void run_case(Hn& h, const char* nm, const Bytes& f,
                     bool pool_ok = true, int gap = 0) {
  Expect e = classify(f, pool_ok);
  Snap b = snap(h);
  size_t mrp_last_b = size_t(h.mrp_lasts);
  h.pool_allow = pool_ok;
  h.frame(f, gap);
  h.idle(40);
  h.pool_allow = true;
  Snap a = snap(h);
  CHECK(a.da - b.da == e.da && a.et - b.et == e.et && a.sub - b.sub == e.sub &&
        a.ver - b.ver == e.ver && a.len - b.len == e.len,
        "%s counters d/e/s/v/l got %d %d %d %d %d want %d %d %d %d %d", nm,
        a.da - b.da, a.et - b.et, a.sub - b.sub, a.ver - b.ver, a.len - b.len,
        e.da, e.et, e.sub, e.ver, e.len);
  CHECK(a.allocs - b.allocs == (e.alloc ? 1 : 0),
        "%s allocs got %d want %d", nm, a.allocs - b.allocs, e.alloc ? 1 : 0);
  CHECK(a.commits - b.commits == (e.commit ? 1 : 0),
        "%s commits got %d want %d", nm, a.commits - b.commits, e.commit ? 1 : 0);
  CHECK(a.aborts - b.aborts == (e.abort_ ? 1 : 0),
        "%s aborts got %d want %d", nm, a.aborts - b.aborts, e.abort_ ? 1 : 0);
  CHECK(a.hdrs - b.hdrs == (e.commit ? 1 : 0),
        "%s hdr beats got %d want %d", nm, a.hdrs - b.hdrs, e.commit ? 1 : 0);
  if (e.commit)
    CHECK(h.last_commit == e.slot, "%s slot bytes exact (got %zu want %zu)",
          nm, h.last_commit.size(), e.slot.size());
  if (e.mrp) {
    Bytes got(h.mrp.begin() + long(b.mrp), h.mrp.end());
    CHECK(got == f, "%s MRP byte-exact (got %zu want %zu)", nm, got.size(), f.size());
    CHECK(h.mrp_lasts - int(mrp_last_b) == 1, "%s one mrp_last", nm);
  } else {
    CHECK(a.mrp == b.mrp, "%s no MRP leak got %zu extra", nm, a.mrp - b.mrp);
  }
}

static void check_hdr(Hn& h, const char* nm, const Hdr& e) {
  const Hdr& g = h.hg;
  CHECK(g.protocol == e.protocol, "%s protocol got %u want %u", nm, g.protocol, e.protocol);
  CHECK(g.msg_type == e.msg_type, "%s msg_type got %u want %u", nm, g.msg_type, e.msg_type);
  CHECK(g.status == e.status, "%s status got %u want %u", nm, g.status, e.status);
  CHECK(g.cdl == e.cdl, "%s cdl got %u want %u", nm, g.cdl, e.cdl);
  CHECK(g.src_mac == e.src_mac, "%s src_mac got %012llx", nm,
        (unsigned long long)g.src_mac);
  CHECK(g.ctlr == e.ctlr, "%s ctlr_eid got %016llx want %016llx", nm,
        (unsigned long long)g.ctlr, (unsigned long long)e.ctlr);
  CHECK(g.target == e.target, "%s target_eid got %016llx want %016llx", nm,
        (unsigned long long)g.target, (unsigned long long)e.target);
  CHECK(g.seq == e.seq, "%s seq got %04x want %04x", nm, g.seq, e.seq);
  CHECK(g.u == e.u && g.cr == e.cr, "%s u/cr got %d/%d want %d/%d", nm,
        g.u, g.cr, e.u, e.cr);
  CHECK(g.opcode == e.opcode, "%s opcode got %04x want %04x", nm, g.opcode, e.opcode);
  CHECK(g.operands == e.operands, "%s operands got %016llx want %016llx", nm,
        (unsigned long long)g.operands, (unsigned long long)e.operands);
  CHECK(h.hg_slot == uint8_t(h.last_commit_slot), "%s rx_slot handle got %u want %d",
        nm, h.hg_slot, h.last_commit_slot);
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* d = new VKL_pp_rx_validator;
  Hn h(d);

  d->own_mac_i = OWN_MAC;
  d->rst_n = 0;
  h.idle(4);
  d->rst_n = 1;
  h.idle(2);
  CHECK(d->rx_da_count_o == 0 && d->rx_length_count_o == 0, "counters reset");

  // ---- F1: AECP AEM command to own unicast, exact fit (V1 boundary) -----
  Bytes f1 = eth(OWN_MAC, 0x5254001A2B3Cull, 0x22F0);
  f1.push_back(0xFB); f1.push_back(0x00);            // subtype, h/ver/msg AEM_CMD
  f1.push_back(0x00); f1.push_back(20);              // status 0, cdl 20
  put64(f1, 0x1122334455667788ull);                  // target_entity_id
  put64(f1, 0xC0FFEE00DEADBEEFull);                  // controller_entity_id
  put16(f1, 0xBEEF);                                 // sequence_id
  f1.push_back(0xC0 | 0x01); f1.push_back(0x23);     // u=1 cr=1 ct=0x0123
  put16(f1, 0x0005); put16(f1, 0x0002);              // desc_type, desc_index
  put16(f1, 0xAAAA); put16(f1, 0x5555);              // command payload
  run_case(h, "F1", f1);
  check_hdr(h, "F1", classify(f1, true).hdr);

  // ---- F2: padded 64-B minimum frame parsed by cdl (V2, F09.4 row) ------
  Bytes f2 = eth(OWN_MAC, 0x5254001A2B3Cull, 0x22F0);
  f2.push_back(0xFB); f2.push_back(0x00);
  f2.push_back(0x00); f2.push_back(12);              // REGISTER_UNSOLICITED, cdl 12
  put64(f2, 0x1122334455667788ull);
  put64(f2, 0xC0FFEE00DEADBEEFull);
  put16(f2, 0x0102);
  f2.push_back(0x00); f2.push_back(0x24);            // ct 0x0024
  fill_pat(f2, 60, 0x00);                            // pad to min Ethernet
  run_case(h, "F2", f2);
  check_hdr(h, "F2", classify(f2, true).hdr);
  CHECK(h.last_commit.size() == 24, "F2 slot len is cdl+12 got %zu",
        h.last_commit.size());

  // ---- F3: Milan 56-B truncated ACMPDU, BIND_RX (V3, F09.4 row) ---------
  Bytes f3 = eth(DA_AVDECC, 0x02AACC00DD01ull, 0x22F0);
  f3.push_back(0xFC); f3.push_back(0x06);            // BIND_RX command
  f3.push_back(0x00); f3.push_back(44);              // status 0, cdl 44
  put64(f3, 0x9911223344556677ull);                  // stream_id
  put64(f3, 0xC0FFEE00DEADBEEFull);                  // controller
  put64(f3, 0x1111111111111111ull);                  // talker
  put64(f3, 0x2222222222222222ull);                  // listener
  put16(f3, 0x0003); put16(f3, 0x0004);              // talker/listener unique
  put_mac(f3, 0x91E0F0002233ull);                    // stream_dest_mac
  put16(f3, 0x0001);                                 // connection_count
  put16(f3, 0xCAFE);                                 // sequence_id @48
  put16(f3, 0x0008); put16(f3, 0x0002);              // flags, vlan
  put16(f3, 0x0000);                                 // connected_listeners_entries @54
  run_case(h, "F3", f3);
  check_hdr(h, "F3", classify(f3, true).hdr);
  CHECK((h.hg.operands & 0xFFFF) == 0x0004, "F3 listener unique_id");

  // ---- F4: IEEE 2013 short-form ACMPDU, cdl 24: tail reads 0 (V3) -------
  Bytes f4 = eth(DA_AVDECC, 0x02AACC00DD01ull, 0x22F0);
  f4.push_back(0xFC); f4.push_back(0x00);            // PROBE_TX (talker side)
  f4.push_back(0x00); f4.push_back(24);              // cdl 24 -> PDU 36 B
  put64(f4, 0x9911223344556677ull);
  put64(f4, 0xC0FFEE00DEADBEEFull);
  put64(f4, 0x1111111111111111ull);
  fill_pat(f4, 14 + 36, 0x40);
  run_case(h, "F4", f4);
  check_hdr(h, "F4", classify(f4, true).hdr);
  CHECK(h.hg.seq == 0 && h.hg.operands == 0, "F4 tail fields read as 0");

  // ---- F5: ADP ENTITY_AVAILABLE (F04.5: valid_time in the status lane) --
  Bytes f5 = eth(DA_AVDECC, 0x02AACC00DD01ull, 0x22F0);
  f5.push_back(0xFA); f5.push_back(0x00);
  f5.push_back(uint8_t(10 << 3)); f5.push_back(56);  // valid_time 10, cdl 56
  put64(f5, 0xAA11223344556677ull);                  // entity_id
  fill_pat(f5, 14 + 68, 0x60);
  run_case(h, "F5", f5);
  check_hdr(h, "F5", classify(f5, true).hdr);
  CHECK(h.hg.status == 10 && h.hg.ctlr == 0, "F5 valid_time 10, no ctlr");

  // ---- F6: MVU and AA message types demux off the AECP subtype ----------
  Bytes f6 = f1; f6[15] = 0x06;                      // VU_COMMAND
  run_case(h, "F6", f6);
  CHECK(h.hg.protocol == P_MVU, "F6 protocol MVU got %u", h.hg.protocol);
  Bytes f6b = f1; f6b[15] = 0x02;                    // ADDRESS_ACCESS command
  run_case(h, "F6b", f6b);
  CHECK(h.hg.protocol == P_AA, "F6b protocol AA got %u", h.hg.protocol);

  // ---- F7: cdl + 12 > frame payload: dropped + counted (V1, TOL) --------
  Bytes f7 = f1; f7[17] = 40;                        // cdl 40 but payload 32
  run_case(h, "F7", f7);

  // ---- F8/F8b: h != 0 / version != 0 (V8, TOL) --------------------------
  Bytes f8 = f1; f8[15] = 0x80;                      // h = 1
  run_case(h, "F8", f8);
  Bytes f8b = f1; f8b[15] = 0x20;                    // version = 2
  run_case(h, "F8b", f8b);

  // ---- F9: unknown subtype (TOL) ----------------------------------------
  Bytes f9 = f1; f9[14] = 0x7A;
  run_case(h, "F9", f9);

  // ---- F10: wrong EtherType on own unicast ------------------------------
  Bytes f10 = f1; f10[12] = 0x08; f10[13] = 0x00;    // IPv4
  run_case(h, "F10", f10);

  // ---- F11: foreign unicast + foreign multicast DA ----------------------
  Bytes f11 = f1; f11[0] = 0x02; f11[5] = 0x99;      // some other unicast
  run_case(h, "F11", f11);
  Bytes f11b = eth(0x01005E000001ull, 0x5254001A2B3Cull, 0x22F0);
  fill_pat(f11b, 60, 0x10);
  run_case(h, "F11b", f11b);

  // ---- F12: MRP routes byte-exactly (V9) --------------------------------
  Bytes f12 = eth(DA_MSRP, 0x02AACC00DD01ull, 0x22EA);
  fill_pat(f12, 60, 0x21);                           // MSRP MRPDU body
  run_case(h, "F12", f12);
  Bytes f13 = eth(DA_MVRP, 0x02AACC00DD01ull, 0x88F5);
  fill_pat(f13, 68, 0x33);
  run_case(h, "F13", f13);

  // ---- F14: LLDP / 802.1X on the MRP group DAs are ignored (V9) ---------
  Bytes f14 = eth(DA_MSRP, 0x02AACC00DD01ull, 0x88CC);
  fill_pat(f14, 60, 0x44);
  run_case(h, "F14", f14);
  Bytes f14b = eth(DA_MVRP, 0x02AACC00DD01ull, 0x888Eull);
  fill_pat(f14b, 60, 0x45);
  run_case(h, "F14b", f14b);

  // ---- F15: pool refusal is silent here (rx_overrun counts in the pool) -
  run_case(h, "F15", f1, /*pool_ok=*/false);

  // ---- F16: oversize inbound cdl (cdl+12 > slot capacity) ---------------
  Bytes f16 = eth(OWN_MAC, 0x5254001A2B3Cull, 0x22F0);
  f16.push_back(0xFB); f16.push_back(0x00);
  f16.push_back(0x02); f16.push_back(0x58);          // cdl 600
  fill_pat(f16, 14 + 700, 0x50);
  run_case(h, "F16", f16);

  // ---- F17: runts at each gate ------------------------------------------
  Bytes f17a(f1.begin(), f1.begin() + 3);            // DA incomplete -> rx_da
  f17a.back() = f17a.back();                          // (content irrelevant)
  run_case(h, "F17a", f17a);
  Bytes f17b(f1.begin(), f1.begin() + 10);           // ET incomplete -> rx_ethertype
  run_case(h, "F17b", f17b);
  Bytes f17c(f1.begin(), f1.begin() + 14);           // no payload -> rx_length
  run_case(h, "F17c", f17c);
  Bytes f17d(f1.begin(), f1.begin() + 16);           // header torn -> rx_length
  run_case(h, "F17d", f17d);

  // ---- F18: gappy input (rx_valid bubbles) still parses -----------------
  run_case(h, "F18", f3, true, /*gap=*/3);
  check_hdr(h, "F18", classify(f3, true).hdr);

  // ---- F19: back-to-back frames, no idle between ------------------------
  {
    Snap b = snap(h);
    h.frame(f12);                                    // MRP frame...
    size_t mrp_before = b.mrp;
    h.frame(f1);                                     // ...1722.1 frame right after
    h.frame(f3);                                     // ...and an ACMP frame
    h.idle(80);
    Snap a = snap(h);
    CHECK(a.commits - b.commits == 2, "F19 two commits got %d", a.commits - b.commits);
    CHECK(a.da == b.da && a.et == b.et && a.sub == b.sub && a.ver == b.ver &&
          a.len == b.len, "F19 no drops");
    Bytes got(h.mrp.begin() + long(mrp_before), h.mrp.end());
    CHECK(got == f12, "F19 MRP byte-exact through the interleave");
    CHECK(h.last_commit == classify(f3, true).slot, "F19 last slot is the ACMPDU");
    check_hdr(h, "F19", classify(f3, true).hdr);
  }

  // ---- F20: rx_da saturates at 0xFFFF, never wraps ----------------------
  {
    Bytes bad = eth(0x020000000042ull, 0x5254001A2B3Cull, 0x22F0);
    bad.resize(6);                                   // 6-byte runt, bad DA
    int start = int(d->rx_da_count_o);
    for (int i = 0; i < 65600 - start; ++i) h.frame(bad);
    h.idle(80);
    CHECK(d->rx_da_count_o == 0xFFFF, "F20 saturated got %u",
          unsigned(d->rx_da_count_o));
    h.frame(bad); h.frame(bad); h.idle(40);
    CHECK(d->rx_da_count_o == 0xFFFF, "F20 no wrap got %u",
          unsigned(d->rx_da_count_o));
  }

  // ---- F21: a good frame still parses after everything above ------------
  run_case(h, "F21", f1);
  check_hdr(h, "F21", classify(f1, true).hdr);

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete d;
  return fails ? 1 : 0;
}
