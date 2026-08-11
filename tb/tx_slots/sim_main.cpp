// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_tx_slots suite — independent expectations, never DUT logic.
//
// Proves the 03 §7/§8 TX slot pool contract, F01.5 "P-TX 4x576 + 1600":
// plain allocation walks slots 0..3 lowest-first and NEVER returns the
// oversize slot; an oversize allocation (the Δ8 READ_DESCRIPTOR class,
// Milan §5.4.1) is granted slot 4 only, and waits for slot 4 even while
// 0..3 sit free. Byte-exact alloc/write/commit/serialize over all five
// slots with header-after-payload random-access writes; serialization
// stalls — never skips — under random ser_ready_i drops; the slot
// auto-frees on the final consumed byte and is immediately reusable; two
// committed slots are serviced strictly one at a time; 1-byte and
// full-size (576 / 1600) frames; writes after commit are discarded.
//
// The reference model is contract-level bookkeeping (slot lifecycle, its
// own byte images, free/ready accounting) — it shares no pipeline, no
// skid and no address math with the DUT.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "VKL_pp_tx_slots.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// ---- independent contract model -------------------------------------------
struct RefPool {
  enum St { FREE, ALLOC, READY, STREAM };
  St      st[5];
  int     len[5];
  uint8_t img[5][1600];

  RefPool() { for (int i = 0; i < 5; ++i) { st[i] = FREE; len[i] = 0; }
              memset(img, 0, sizeof img); }

  static int cap(int s) { return s == 4 ? 1600 : 576; }

  int alloc(bool oversize) {
    if (oversize) { if (st[4] == FREE) { st[4] = ALLOC; return 4; } return -1; }
    for (int i = 0; i < 4; ++i)
      if (st[i] == FREE) { st[i] = ALLOC; return i; }
    return -1;
  }
  void write(int s, int a, uint8_t d) {
    if (st[s] == ALLOC && a < cap(s)) img[s][a] = d;      // else: discarded
  }
  void commit(int s, int l) {
    if (st[s] == ALLOC) { st[s] = READY; len[s] = l; }
  }
  void start(int s)    { if (st[s] == READY) st[s] = STREAM; }
  void freeSlot(int s) { st[s] = FREE; }

  int nfree() const {
    int n = 0; for (int i = 0; i < 5; ++i) n += (st[i] == FREE); return n;
  }
  uint32_t readyMask() const {
    uint32_t m = 0;
    for (int i = 0; i < 5; ++i) if (st[i] == READY) m |= 1u << i;
    return m;
  }
};

// ---- harness ---------------------------------------------------------------
struct Harness {
  VKL_pp_tx_slots* dut;
  RefPool ref;
  uint32_t rng = 0xC0FFEE01u;

  // pre-edge observation of the serialize port (what the arbiter sees)
  int o_valid = 0, o_last = 0, o_ready = 0;
  uint8_t o_data = 0;

  bool last_pulse_ok = false;   // grant was a one-cycle pulse

  explicit Harness(VKL_pp_tx_slots* d) : dut(d) {}

  uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

  void tick() {
    // settle combinational logic with this cycle's inputs
    dut->clk_i = 0; dut->eval();
    // observe the stream handshake PRE-EDGE (what the registers see)
    o_valid = dut->ser_valid_o; o_last = dut->ser_last_o;
    o_ready = dut->ser_ready_i; o_data = dut->ser_data_o;
    // rising edge: registers update
    dut->clk_i = 1; dut->eval();
  }

  // one request pulse per try; grant is registered — visible after the edge
  int alloc_dut(bool ov, int tries = 3) {
    int got = -1;
    for (int k = 0; k < tries; ++k) {
      dut->alloc_req_i = 1; dut->oversize_i = ov;
      tick();
      dut->alloc_req_i = 0; dut->oversize_i = 0;
      if (dut->alloc_gnt_o) got = dut->alloc_slot_o;
      tick();                                       // pulse must clear here
      if (got >= 0) { last_pulse_ok = (dut->alloc_gnt_o == 0); break; }
      if (dut->alloc_gnt_o) { got = dut->alloc_slot_o; break; }  // late = defect elsewhere
    }
    return got;
  }

  void wr(int s, int a, uint8_t d) {
    dut->wr_slot_i = s; dut->wr_addr_i = a; dut->wr_data_i = d;
    dut->wr_valid_i = 1;
    tick();
    dut->wr_valid_i = 0;
    ref.write(s, a, d);
  }

  void commit(int s, int l) {
    dut->wr_slot_i = s; dut->wr_len_i = l; dut->wr_commit_i = 1;
    tick();
    dut->wr_commit_i = 0;
    ref.commit(s, l);
  }

  static uint8_t pat(int s, int a, int epoch) {
    return uint8_t(0x35 * s + 7 * a + 0x11 + 0x4D * epoch);
  }

  // builders write headers AFTER payloads: offsets [hdr..len) first, then [0..hdr)
  void fill_ha(int s, int len, int hdr, int epoch) {
    for (int a = hdr; a < len; ++a) wr(s, a, pat(s, a, epoch));
    for (int a = 0; a < hdr; ++a)   wr(s, a, pat(s, a, epoch));
  }

  struct SRes {
    std::vector<uint8_t> bytes;
    bool done = false;
    int  hold_viol = 0;      // a stall changed/skipped the presented byte
    int  spurious  = 0;      // ser_valid after the frame completed
    int  last_pos  = -1;     // byte index where ser_last was consumed
    int  consume_span = 0;   // cycles from first to last consume, inclusive
    uint32_t mid_ready = 0;  // slots_ready_o sampled just after first valid
  };

  SRes stream(int slot, size_t explen, int ready_pct, int maxcyc,
              int bg_slot = -1, bool assert_req = true, bool drain = true) {
    SRes r;
    ref.start(slot);
    if (assert_req) { dut->ser_req_i = 1; dut->ser_slot_i = slot; }
    bool req_up = true;
    bool stall_pending = false;
    uint8_t stall_data = 0; int stall_last = 0;
    int first_c = -1, last_c = -1;
    bool mid_sampled = false;

    for (int cyc = 0; cyc < maxcyc && !r.done; ++cyc) {
      dut->ser_ready_i = (rnd() % 100u) < (uint32_t)ready_pct;
      tick();
      if (o_valid && req_up) {
        // frame accepted: drop our request (or hand the line to bg_slot)
        req_up = false;
        if (bg_slot >= 0) dut->ser_slot_i = bg_slot;
        else              dut->ser_req_i = 0;
      }
      if (o_valid && !mid_sampled) {
        r.mid_ready = dut->slots_ready_o;
        mid_sampled = true;
      }
      if (stall_pending &&
          (!o_valid || o_data != stall_data || o_last != stall_last))
        r.hold_viol++;
      if (o_valid && !o_ready) {
        stall_pending = true; stall_data = o_data; stall_last = o_last;
      } else {
        stall_pending = false;
      }
      if (o_valid && o_ready) {
        if (first_c < 0) first_c = cyc;
        last_c = cyc;
        r.bytes.push_back(o_data);
        if (o_last) { r.last_pos = int(r.bytes.size()) - 1; r.done = true; }
        else if (r.bytes.size() > explen) break;      // runaway guard
      }
    }
    if (first_c >= 0) r.consume_span = last_c - first_c + 1;
    if (r.done) ref.freeSlot(slot);                   // documented auto-free
    if (drain) {
      dut->ser_req_i = 0; dut->ser_ready_i = 1;
      for (int k = 0; k < 4; ++k) { tick(); if (o_valid) r.spurious++; }
    }
    return r;
  }
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_pp_tx_slots;
  Harness h(dut);

  dut->alloc_req_i = 0; dut->oversize_i = 0;
  dut->wr_slot_i = 0; dut->wr_addr_i = 0; dut->wr_data_i = 0;
  dut->wr_valid_i = 0; dut->wr_commit_i = 0; dut->wr_len_i = 0;
  dut->ser_req_i = 0; dut->ser_slot_i = 0; dut->ser_ready_i = 0;

  // ---- A: reset state -------------------------------------------------
  dut->rst_n = 0;
  for (int i = 0; i < 4; ++i) h.tick();
  dut->rst_n = 1;
  h.tick();
  CHECK(dut->slots_free_o == 5, "A all five slots free after reset");
  CHECK(dut->slots_ready_o == 0, "A nothing ready after reset");
  CHECK(dut->alloc_gnt_o == 0 && dut->ser_valid_o == 0, "A quiescent outputs");

  // ---- B: allocation policy ------------------------------------------
  for (int k = 0; k < 4; ++k) {
    int exp = h.ref.alloc(false);
    int got = h.alloc_dut(false);
    CHECK(got == exp, "B plain alloc #%d got %d exp %d", k, got, exp);
  }
  CHECK(h.last_pulse_ok, "B grant is a one-cycle pulse");
  CHECK(dut->slots_free_o == 1, "B one slot (the oversize) left free");
  {
    int exp = h.ref.alloc(false);                    // -1: std slots exhausted
    int got = h.alloc_dut(false, 6);
    CHECK(got == exp && got == -1,
          "B plain alloc NEVER returns slot 4 (got %d)", got);
  }
  {
    int exp = h.ref.alloc(true);                     // 4
    int got = h.alloc_dut(true);
    CHECK(got == exp && got == 4, "B oversize alloc returns 4 (got %d)", got);
  }
  CHECK(dut->slots_free_o == 0, "B pool exhausted");
  {
    int exp = h.ref.alloc(true);                     // -1: slot 4 busy
    int got = h.alloc_dut(true, 4);
    CHECK(got == exp && got == -1,
          "B oversize alloc waits for slot 4 (got %d)", got);
  }
  {
    int got = h.alloc_dut(false, 4);
    CHECK(got == -1, "B plain alloc denied when all busy (got %d)", got);
  }

  // ---- C: fill (header after payload) + commit ------------------------
  const int lens[5] = {576, 1, 137, 42, 1600};       // full-std, 1-byte, mid, mid, full-oversize
  const int hdrs[5] = {14, 0, 12, 5, 22};
  for (int s = 0; s < 5; ++s) h.fill_ha(s, lens[s], hdrs[s], 0);
  CHECK(dut->ser_valid_o == 0, "C no stream during fill");
  h.commit(0, lens[0]);
  h.commit(1, lens[1]);
  h.commit(2, lens[2]);

  // ---- D: serialize request on a non-READY (still ALLOC) slot ---------
  {
    dut->ser_req_i = 1; dut->ser_slot_i = 3;
    bool sawv = false;
    for (int k = 0; k < 3; ++k) { h.tick(); sawv |= (h.o_valid != 0); }
    dut->ser_req_i = 0;
    h.tick();
    CHECK(!sawv, "D uncommitted slot never streams");
    CHECK(dut->slots_ready_o == h.ref.readyMask() && dut->slots_free_o == 0,
          "D state untouched by the ignored request (rdy %02x)",
          (unsigned)dut->slots_ready_o);
  }
  h.commit(3, lens[3]);
  h.commit(4, lens[4]);
  CHECK(dut->slots_ready_o == 0x1F, "C+D all five committed -> ready 0x1F");
  CHECK(dut->slots_free_o == 0, "C+D none free while all queued");

  // ---- E: serialize all five, out of order, random backpressure -------
  auto frame = [&](const char* tag, int slot, int ready_pct, int bg = -1,
                   bool assert_req = true, bool drain = true) {
    int len = h.ref.len[slot];
    std::vector<uint8_t> exp(h.ref.img[slot], h.ref.img[slot] + len);
    uint32_t exp_mid = h.ref.readyMask() & ~(1u << slot);
    auto r = h.stream(slot, size_t(len), ready_pct, len * 6 + 200,
                      bg, assert_req, drain);
    CHECK(r.done, "%s completes", tag);
    CHECK(r.bytes == exp, "%s byte-exact (%zu of %d bytes)", tag,
          r.bytes.size(), len);
    CHECK(r.last_pos == len - 1, "%s ser_last on byte %d exp %d", tag,
          r.last_pos, len - 1);
    CHECK(r.hold_viol == 0, "%s backpressure stalls, never skips (%d viol)",
          tag, r.hold_viol);
    if (drain)
      CHECK(r.spurious == 0, "%s no bytes after ser_last (%d)", tag, r.spurious);
    CHECK(dut->slots_free_o == (uint32_t)h.ref.nfree() &&
          dut->slots_ready_o == h.ref.readyMask(),
          "%s auto-freed on eof (free %u exp %d)", tag,
          (unsigned)dut->slots_free_o, h.ref.nfree());
    return std::make_pair(r, exp_mid);
  };

  {
    auto [r, exp_mid] = frame("E slot2", 2, 60);
    CHECK(r.mid_ready == exp_mid,
          "E others stay ready mid-stream (got %02x exp %02x)",
          r.mid_ready, exp_mid);
  }
  frame("E slot0 (full 576)", 0, 70);
  frame("E slot4 (full 1600 oversize)", 4, 55);
  {
    auto [r, exp_mid] = frame("E slot1 (1-byte)", 1, 100);
    (void)exp_mid;
    CHECK(r.consume_span == 1, "E 1-byte frame consumed in one cycle (%d)",
          r.consume_span);
  }
  {
    auto [r, exp_mid] = frame("E slot3", 3, 100);
    (void)exp_mid;
    CHECK(r.consume_span == lens[3],
          "E full-ready streams one byte per cycle (span %d exp %d)",
          r.consume_span, lens[3]);
  }
  CHECK(dut->slots_free_o == 5 && dut->slots_ready_o == 0,
        "E pool fully drained");

  // ---- F: two committed slots, serviced one at a time -----------------
  {
    int a = h.ref.alloc(false); int ga = h.alloc_dut(false);
    int b = h.ref.alloc(false); int gb = h.alloc_dut(false);
    CHECK(ga == a && a == 0, "F re-alloc lowest freed slot (got %d)", ga);
    CHECK(gb == b && b == 1, "F second alloc next lowest (got %d)", gb);
    h.fill_ha(a, 96, 14, 1); h.commit(a, 96);
    h.fill_ha(b, 64, 9, 1);  h.commit(b, 64);
    CHECK(dut->slots_ready_o == 0x03, "F both committed");
    // stream A while holding ser_req for B the whole time
    auto [ra, mid_a] = frame("F slotA", a, 65, /*bg=*/b,
                             /*assert_req=*/true, /*drain=*/false);
    (void)mid_a;
    CHECK((ra.mid_ready >> b) & 1, "F B stays ready while A streams");
    // request line is still up for B: it must now stream, intact
    frame("F slotB", b, 80, -1, /*assert_req=*/false, /*drain=*/true);
  }

  // ---- G: reuse + write-after-commit is discarded ---------------------
  {
    int exp = h.ref.alloc(false);
    int got = h.alloc_dut(false);
    CHECK(got == exp && got == 0, "G freed slot is reusable (got %d)", got);
    h.fill_ha(0, 33, 4, 2);
    h.commit(0, 33);
    h.wr(0, 3, 0xEE);            // rogue write after commit: must not land
    frame("G reused slot0", 0, 60);
  }

  // ---- H: zero-length commit is freed on service, no bytes ------------
  {
    int exp = h.ref.alloc(false);
    int got = h.alloc_dut(false);
    CHECK(got == exp && got == 0, "H alloc for the empty frame (got %d)", got);
    h.commit(0, 0);
    CHECK((dut->slots_ready_o & 1) == 1, "H zero-length commit shows ready");
    dut->ser_req_i = 1; dut->ser_slot_i = 0;
    bool sawv = false;
    for (int k = 0; k < 3; ++k) { h.tick(); sawv |= (h.o_valid != 0); }
    dut->ser_req_i = 0;
    h.tick();
    h.ref.freeSlot(0);
    CHECK(!sawv, "H empty frame emits no bytes");
    CHECK(dut->slots_free_o == 5 && dut->slots_ready_o == 0,
          "H empty frame freed on service");
  }

  // ---- I: quiescence ---------------------------------------------------
  {
    bool spur = false;
    for (int k = 0; k < 10; ++k) {
      h.tick();
      spur |= (dut->alloc_gnt_o != 0) || (h.o_valid != 0);
    }
    CHECK(!spur, "I no spurious grants or bytes when idle");
    CHECK(dut->slots_free_o == 5 && dut->slots_ready_o == 0, "I final state clean");
  }

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
