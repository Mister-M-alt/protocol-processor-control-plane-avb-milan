// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_nvm_port suite — independent device model + manager BFM, never DUT
// logic.
//
// The harness plays BOTH neighbors of the class-F port (02 §8 / F02.8):
// a manager BFM that frames records per 07 §5.2 (magic 0x1722, layout
// version, record_id, payload_length, crc16 — 16-bit fields big-endian on
// the byte stream) and streams them with configurable stalls, and an
// independent region-port device model (req/gnt, op READ/WRITE/ERASE,
// region + offset + len, byte phases with stalls, busy/done/err, error
// injection at any op or data byte). It checks byte-exact delivery both
// directions, the ERASE-then-WRITE commit shape, busy/done/err sequencing
// (busy low at the pulse, pulses exactly once), back-to-back ops, req-
// while-busy refusal, header refusals (bad magic / oversize length) with
// zero device traffic, and mid-op device errors surfacing exactly once.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "VKL_pp_nvm_port.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// device-face op codes — mirror of the contract, independent of the RTL
enum { OP_READ = 0, OP_WRITE = 1, OP_ERASE = 2 };

static const int N_REGIONS = 8;    // test ids stay 0..7
static const int REG_BYTES = 2048;
static const int MAXP      = 1024; // pinned by -GMAX_PAYLOAD_P in the Makefile

// CRC-16/CCITT-FALSE — the manager's in-band integrity field; the DUT must
// carry it opaquely (07 §5.3 puts computation/validation in the manager).
static uint16_t crc16(const std::vector<uint8_t>& b) {
  uint16_t c = 0xFFFF;
  for (uint8_t x : b) {
    c ^= uint16_t(uint16_t(x) << 8);
    for (int i = 0; i < 8; ++i)
      c = (c & 0x8000) ? uint16_t(uint16_t(c << 1) ^ 0x1021)
                       : uint16_t(c << 1);
  }
  return c;
}

// frame a record per 07 §5.2 (big-endian 16-bit fields on the byte stream)
static std::vector<uint8_t> frame(uint8_t rec, const std::vector<uint8_t>& pl,
                                  uint16_t magic = 0x1722, int force_plen = -1) {
  uint16_t plen = (force_plen >= 0) ? uint16_t(force_plen)
                                    : uint16_t(pl.size());
  std::vector<uint8_t> f;
  f.push_back(uint8_t(magic >> 8));
  f.push_back(uint8_t(magic & 0xFF));
  f.push_back(0x01);                       // layout_version
  f.push_back(rec);
  f.push_back(uint8_t(plen >> 8));
  f.push_back(uint8_t(plen & 0xFF));
  std::vector<uint8_t> cb(f);              // crc over header-sans-crc + payload
  cb.insert(cb.end(), pl.begin(), pl.end());
  uint16_t c = crc16(cb);
  f.push_back(uint8_t(c >> 8));
  f.push_back(uint8_t(c & 0xFF));
  f.insert(f.end(), pl.begin(), pl.end());
  return f;
}

static std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> p(n);
  for (size_t i = 0; i < n; ++i) p[i] = uint8_t(seed + 7 * i);
  return p;
}

struct DevOp { int op, region, offset, len; };

struct Harness {
  VKL_pp_nvm_port* dut;

  // ---- independent device model (region port backend) ----
  uint8_t store[N_REGIONS][REG_BYTES];
  std::vector<DevOp> ops;                 // every accepted device command
  int erase_count[N_REGIONS] = {0};
  int gnt_delay = 1, op_delay = 4;        // req->gnt, data-end->done cycles
  int wstall = 0, rstall = 0;             // device-side inter-byte stalls
  int err_at_op = -1;                     // op index since arm to fail
  int err_after_bytes = -1;               // fail after N data bytes (-1: at once)
  int ops_since_arm = 0;
  bool fail_cur = false;

  int  d_st = 0;                          // 0 idle, 1 data, 2 completion timer
  int  d_reqwait = 0;
  bool d_gnt = false, d_done = false, d_err = false, d_busy = false;
  DevOp d_cur{0, 0, 0, 0};
  int  d_bytes = 0, d_stall = 0;
  bool d_rhold = false;
  int  done_ctr = 0, err_ctr = 0;

  // ---- manager BFM ----
  int m_mode = 0;                         // 0 idle, 1 commit stream, 2 restore
  std::vector<uint8_t> m_wbytes;
  size_t m_widx = 0;
  int m_stall = 0;
  int mgr_wstall = 0, mgr_rstall = 0;

  // per-op capture
  int done_pulses = 0, err_pulses = 0;
  bool busy_seen = false, busy_ok = true; // busy must be LOW at done/err (F02.8)
  std::vector<uint8_t> rbytes;
  long cycles = 0;

  explicit Harness(VKL_pp_nvm_port* d) : dut(d) {
    memset(store, 0xEE, sizeof store);
  }

  void arm_err(int at_op, int after_bytes) {
    err_at_op = at_op; err_after_bytes = after_bytes; ops_since_arm = 0;
  }
  void disarm_err() { err_at_op = -1; err_after_bytes = -1; }

  void drive_dev() {
    dut->dev_gnt_i  = d_gnt;
    dut->dev_done_i = d_done;
    dut->dev_err_i  = d_err;
    dut->dev_busy_i = d_busy;
    bool wr = (d_st == 1 && d_cur.op == OP_WRITE && d_stall == 0);
    dut->dev_wready_i = wr;
    bool rv = (d_st == 1 && d_cur.op == OP_READ && d_bytes < d_cur.len
               && (d_stall == 0 || d_rhold));
    dut->dev_rvalid_i = rv;
    dut->dev_rdata_i  = rv
        ? store[d_cur.region % N_REGIONS][(d_cur.offset + d_bytes) % REG_BYTES]
        : 0;
  }

  void sample_dev() {
    bool drove_gnt = d_gnt, drove_done = d_done, drove_err = d_err;

    // command accept
    if (drove_gnt && dut->dev_req_o) {
      d_cur = {int(dut->dev_op_o), int(dut->dev_region_o),
               int(dut->dev_offset_o), int(dut->dev_len_o)};
      ops.push_back(d_cur);
      d_busy = true; d_bytes = 0; d_stall = 0; d_rhold = false;
      fail_cur = (err_at_op >= 0 && ops_since_arm == err_at_op);
      ++ops_since_arm;
      if (fail_cur && err_after_bytes < 0) {
        err_ctr = op_delay; d_st = 2;
      } else if (d_cur.op == OP_ERASE) {
        int r = d_cur.region % N_REGIONS;
        memset(store[r], 0xFF, REG_BYTES);
        ++erase_count[r];
        done_ctr = op_delay; d_st = 2;
      } else if (d_cur.len == 0) {
        done_ctr = op_delay; d_st = 2;
      } else {
        d_st = 1;
      }
    }

    // write byte accept
    if (d_st == 1 && d_cur.op == OP_WRITE) {
      if (dut->dev_wready_i && dut->dev_wvalid_o) {
        store[d_cur.region % N_REGIONS][(d_cur.offset + d_bytes) % REG_BYTES] =
            dut->dev_wdata_o;
        ++d_bytes;
        d_stall = wstall;
        if (fail_cur && d_bytes == err_after_bytes) { err_ctr = 2; d_st = 2; }
        else if (d_bytes == d_cur.len) { done_ctr = op_delay; d_st = 2; }
      } else if (d_stall > 0) --d_stall;
    }

    // read byte delivery
    if (d_st == 1 && d_cur.op == OP_READ) {
      if (dut->dev_rvalid_i) {
        if (dut->dev_rready_o) {
          ++d_bytes; d_rhold = false; d_stall = rstall;
          if (fail_cur && d_bytes == err_after_bytes) { err_ctr = 2; d_st = 2; }
          else if (d_bytes == d_cur.len) { done_ctr = op_delay; d_st = 2; }
        } else {
          d_rhold = true;               // hold the byte until accepted
        }
      } else if (d_stall > 0) --d_stall;
    }

    // one-cycle pulses
    if (drove_gnt)  d_gnt  = false;
    if (drove_done) d_done = false;
    if (drove_err)  d_err  = false;

    // grant scheduling
    if (d_st == 0 && !d_busy && dut->dev_req_o && !drove_gnt) {
      if (++d_reqwait >= gnt_delay) { d_gnt = true; d_reqwait = 0; }
    }

    // completion timers
    if (d_st == 2) {
      if (done_ctr > 0 && --done_ctr == 0) { d_done = true; d_busy = false; d_st = 0; }
      if (err_ctr  > 0 && --err_ctr  == 0) { d_err  = true; d_busy = false; d_st = 0; }
    }
  }

  void tick() {
    // manager drive
    if (m_mode == 1) {
      bool v = (m_widx < m_wbytes.size() && m_stall == 0);
      dut->nvm_wvalid_i = v;
      dut->nvm_wdata_i  = v ? m_wbytes[m_widx] : 0;
      dut->nvm_rready_i = 0;
    } else if (m_mode == 2) {
      dut->nvm_wvalid_i = 0;
      dut->nvm_rready_i = (m_stall == 0);
    } else {
      dut->nvm_wvalid_i = 0;
      dut->nvm_rready_i = 0;
    }
    drive_dev();

    dut->clk_i = 0; dut->eval();

    // pre-edge sampling: what the registers (and both neighbors) see
    if (dut->nvm_done_o) ++done_pulses;
    if (dut->nvm_err_o)  ++err_pulses;
    if (dut->nvm_busy_o) busy_seen = true;
    if ((dut->nvm_done_o || dut->nvm_err_o) && dut->nvm_busy_o) busy_ok = false;
    if (m_mode == 1) {
      if (dut->nvm_wvalid_i && dut->nvm_wready_o) { ++m_widx; m_stall = mgr_wstall; }
      else if (m_stall > 0) --m_stall;
    } else if (m_mode == 2) {
      if (dut->nvm_rvalid_o && dut->nvm_rready_i) {
        rbytes.push_back(dut->nvm_rdata_o);
        m_stall = mgr_rstall;
      } else if (m_stall > 0) --m_stall;
    }
    sample_dev();

    dut->clk_i = 1; dut->eval();
    ++cycles;
  }

  void clear_capture() {
    done_pulses = err_pulses = 0;
    busy_seen = false; busy_ok = true;
    rbytes.clear();
  }

  void start(bool we, uint8_t rec) {
    dut->nvm_req_i = 1;
    dut->nvm_we_i  = we;
    dut->nvm_record_id_i = rec;
    tick();
    dut->nvm_req_i = 0;
  }

  // run until done/err (+ drain to catch double pulses); 0 done, 1 err, -1 t/o
  int run_op(long max_cycles = 100000, int drain = 30) {
    for (long i = 0; i < max_cycles; ++i) {
      tick();
      if (done_pulses + err_pulses > 0) {
        for (int d = 0; d < drain; ++d) tick();
        m_mode = 0;
        return err_pulses ? 1 : 0;
      }
    }
    m_mode = 0;
    return -1;
  }

  int commit(uint8_t rec, const std::vector<uint8_t>& f, int drain = 30) {
    clear_capture();
    m_mode = 1; m_wbytes = f; m_widx = 0; m_stall = 0;
    start(true, rec);
    return run_op(100000, drain);
  }

  int restore(uint8_t rec, int drain = 30) {
    clear_capture();
    m_mode = 2; m_stall = 0;
    start(false, rec);
    return run_op(100000, drain);
  }

  bool store_match(int region, const std::vector<uint8_t>& f) {
    for (size_t i = 0; i < f.size(); ++i)
      if (store[region][i] != f[i]) return false;
    return true;
  }
};

static bool op_is(const DevOp& o, int op, int region, int offset, int len) {
  return o.op == op && o.region == region && o.offset == offset && o.len == len;
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_pp_nvm_port;
  Harness h(dut);

  // ---- reset --------------------------------------------------------------
  dut->rst_n = 0;
  dut->nvm_req_i = 0; dut->nvm_we_i = 0; dut->nvm_record_id_i = 0;
  dut->nvm_wvalid_i = 0; dut->nvm_wdata_i = 0; dut->nvm_rready_i = 0;
  dut->dev_gnt_i = 0; dut->dev_wready_i = 0; dut->dev_rvalid_i = 0;
  dut->dev_rdata_i = 0; dut->dev_busy_i = 0; dut->dev_done_i = 0;
  dut->dev_err_i = 0;
  for (int i = 0; i < 4; ++i) h.tick();
  dut->rst_n = 1;
  h.tick();
  CHECK(!dut->nvm_busy_o && !dut->nvm_done_o && !dut->nvm_err_o,
        "idle after reset");
  CHECK(!dut->dev_req_o && !dut->dev_wvalid_o && !dut->nvm_rvalid_o,
        "no spontaneous device/read activity after reset");

  // ---- T1: commit envelope — ERASE then WRITE, byte-exact ----------------
  auto f1 = frame(3, pattern(24, 0x30));
  int rc = h.commit(3, f1);
  CHECK(rc == 0, "T1 commit completes with done, rc=%d", rc);
  CHECK(h.done_pulses == 1 && h.err_pulses == 0,
        "T1 exactly one done, no err (got %d/%d)", h.done_pulses, h.err_pulses);
  CHECK(h.busy_seen && h.busy_ok, "T1 busy high mid-op, low at the pulse");
  CHECK(h.ops.size() == 2, "T1 two device ops, got %zu", h.ops.size());
  CHECK(h.ops.size() >= 1 && op_is(h.ops[0], OP_ERASE, 3, 0, 0),
        "T1 op0 = ERASE region 3 len 0 (got op %d reg %d off %d len %d)",
        h.ops[0].op, h.ops[0].region, h.ops[0].offset, h.ops[0].len);
  CHECK(h.ops.size() >= 2 && op_is(h.ops[1], OP_WRITE, 3, 0, 32),
        "T1 op1 = WRITE region 3 off 0 len 32");
  CHECK(h.erase_count[3] == 1, "T1 erase pulsed region 3 once");
  CHECK(h.store_match(3, f1), "T1 device store byte-exact (header+crc+payload)");
  CHECK(h.store[3][f1.size()] == 0xFF, "T1 erase visible past the record");

  // ---- T2: restore envelope — header read then payload read, byte-exact --
  auto f2 = frame(5, pattern(40, 0xA0));
  memcpy(h.store[5], f2.data(), f2.size());
  h.ops.clear();
  rc = h.restore(5);
  CHECK(rc == 0, "T2 restore completes with done, rc=%d", rc);
  CHECK(h.done_pulses == 1 && h.err_pulses == 0, "T2 exactly one done");
  CHECK(h.rbytes == f2, "T2 restored stream byte-exact (%zu bytes)",
        h.rbytes.size());
  CHECK(h.ops.size() == 2 && op_is(h.ops[0], OP_READ, 5, 0, 8)
            && op_is(h.ops[1], OP_READ, 5, 8, 40),
        "T2 device ops = READ hdr(8) then READ payload(40)");
  CHECK(h.erase_count[5] == 0, "T2 restore never erases");

  // ---- T3: zero-payload record both directions ---------------------------
  auto f3 = frame(6, {});
  h.ops.clear();
  rc = h.commit(6, f3);
  CHECK(rc == 0 && h.done_pulses == 1, "T3 zero-payload commit done");
  CHECK(h.ops.size() == 2 && op_is(h.ops[1], OP_WRITE, 6, 0, 8),
        "T3 WRITE len 8 (header only)");
  CHECK(h.store_match(6, f3), "T3 store byte-exact");
  h.ops.clear();
  rc = h.restore(6);
  CHECK(rc == 0 && h.rbytes == f3, "T3 zero-payload restore byte-exact");
  CHECK(h.ops.size() == 1 && op_is(h.ops[0], OP_READ, 6, 0, 8),
        "T3 restore issues only the header READ");

  // ---- T4: stall torture on all four byte interfaces ---------------------
  h.mgr_wstall = 3; h.mgr_rstall = 2; h.wstall = 2; h.rstall = 3;
  h.gnt_delay = 5; h.op_delay = 9;
  auto f4 = frame(2, pattern(65, 0x11));
  rc = h.commit(2, f4);
  CHECK(rc == 0 && h.done_pulses == 1, "T4 commit done under stalls");
  CHECK(h.store_match(2, f4), "T4 store byte-exact under stalls");
  rc = h.restore(2);
  CHECK(rc == 0 && h.done_pulses == 1, "T4 restore done under stalls");
  CHECK(h.rbytes == f4, "T4 restored stream byte-exact under stalls");
  h.mgr_wstall = 0; h.mgr_rstall = 0; h.wstall = 0; h.rstall = 0;
  h.gnt_delay = 1; h.op_delay = 4;

  // ---- T5: back-to-back ops (req immediately after each pulse) -----------
  auto f5a = frame(0, pattern(12, 0x50));
  auto f5b = frame(1, pattern(20, 0x60));
  h.ops.clear();
  int rca = h.commit(0, f5a, /*drain=*/0);
  int rcb = h.commit(1, f5b, /*drain=*/0);
  int rcc = h.restore(0, /*drain=*/0);
  CHECK(rca == 0 && rcb == 0 && rcc == 0, "T5 back-to-back ops all done");
  CHECK(h.ops.size() == 6, "T5 2+2+2 device ops, got %zu", h.ops.size());
  CHECK(h.store_match(0, f5a) && h.store_match(1, f5b),
        "T5 both commits byte-exact");
  CHECK(h.rbytes == f5a, "T5 immediate re-read byte-exact");

  // ---- T6: req while busy is ignored (single outstanding, F02.8) ---------
  h.gnt_delay = 8; h.op_delay = 30;
  auto f6 = frame(7, pattern(16, 0x70));
  h.ops.clear();
  h.clear_capture();
  h.m_mode = 1; h.m_wbytes = f6; h.m_widx = 0; h.m_stall = 0;
  h.start(true, 7);
  for (int i = 0; i < 40; ++i) h.tick();
  CHECK(dut->nvm_busy_o, "T6 op still in flight at the poke");
  dut->nvm_req_i = 1; dut->nvm_we_i = 0; dut->nvm_record_id_i = 5;
  for (int i = 0; i < 5; ++i) h.tick();
  dut->nvm_req_i = 0; dut->nvm_we_i = 0;
  rc = h.run_op();
  CHECK(rc == 0 && h.done_pulses == 1, "T6 exactly one done for the one op");
  CHECK(h.ops.size() == 2, "T6 no extra device op from the spurious req");
  bool no_read = true;
  for (auto& o : h.ops) if (o.op == OP_READ) no_read = false;
  CHECK(no_read, "T6 the spurious restore never reached the device");
  CHECK(h.store_match(7, f6), "T6 original commit still byte-exact");
  h.gnt_delay = 1; h.op_delay = 4;

  // ---- T7: device error during ERASE — err exactly once, no WRITE -------
  auto f7 = frame(4, pattern(10, 0x90));
  h.arm_err(0, -1);
  h.ops.clear();
  rc = h.commit(4, f7);
  CHECK(rc == 1, "T7 commit fails, rc=%d", rc);
  CHECK(h.err_pulses == 1 && h.done_pulses == 0,
        "T7 err exactly once, done never (got %d/%d)",
        h.err_pulses, h.done_pulses);
  CHECK(h.busy_ok, "T7 busy low at the err pulse");
  CHECK(h.ops.size() == 1 && h.ops[0].op == OP_ERASE,
        "T7 the WRITE was never issued after the erase error");
  h.disarm_err();
  rc = h.commit(4, f7);
  CHECK(rc == 0 && h.done_pulses == 1, "T7b port recovered: retry commits");
  CHECK(h.store_match(4, f7), "T7b retry byte-exact");

  // ---- T8: device error during the WRITE's HEADER pump -------------------
  // op 1 is the WRITE and it carries the framed record, so a cut after 5
  // bytes is still inside the 8-byte header -- the record's payload has not
  // started. T15 cuts the same op at 12 bytes for the data phase proper.
  h.arm_err(1, 5);
  h.ops.clear();
  rc = h.commit(4, f7);
  CHECK(rc == 1 && h.err_pulses == 1 && h.done_pulses == 0,
        "T8 write-phase error surfaces exactly once");
  CHECK(h.ops.size() == 2, "T8 no device op after the failed WRITE");
  h.disarm_err();

  // ---- T9: device error mid READ payload phase ---------------------------
  h.arm_err(1, 7);
  h.ops.clear();
  rc = h.restore(5);
  CHECK(rc == 1 && h.err_pulses == 1 && h.done_pulses == 0,
        "T9 read-phase error surfaces exactly once");
  CHECK(h.rbytes.size() < f2.size(), "T9 stream cut short (%zu of %zu)",
        h.rbytes.size(), f2.size());
  h.disarm_err();

  // ---- T10: unframed commit stream refused (magic, 07 §5.2) --------------
  auto fbad = frame(3, pattern(8, 0x22), /*magic=*/0xDEAD);
  h.ops.clear();
  rc = h.commit(3, fbad);
  CHECK(rc == 1 && h.err_pulses == 1 && h.done_pulses == 0,
        "T10 bad-magic commit refused with one err");
  CHECK(h.ops.empty(), "T10 refusal before any device traffic");

  // ---- T11: oversize payload_length refused ------------------------------
  auto fbig = frame(3, pattern(4, 0x33), 0x1722, /*force_plen=*/MAXP + 1);
  h.ops.clear();
  rc = h.commit(3, fbig);
  CHECK(rc == 1 && h.err_pulses == 1,
        "T11 oversize payload_length refused with one err");
  CHECK(h.ops.empty(), "T11 refusal before any device traffic");

  // ---- T12: stored record with bad magic refused on restore --------------
  uint8_t save0 = h.store[5][0];
  h.store[5][0] = 0x00;
  h.ops.clear();
  rc = h.restore(5);
  CHECK(rc == 1 && h.err_pulses == 1 && h.done_pulses == 0,
        "T12 bad stored magic -> one err");
  CHECK(h.ops.size() == 1 && op_is(h.ops[0], OP_READ, 5, 0, 8),
        "T12 only the header probe was issued");
  CHECK(h.rbytes.empty(), "T12 nothing forwarded to the manager");
  h.store[5][0] = save0;

  // ---- T13: stored record with oversize length refused on restore --------
  uint8_t save4 = h.store[5][4], save5 = h.store[5][5];
  h.store[5][4] = 0xFF; h.store[5][5] = 0xFF;
  h.ops.clear();
  rc = h.restore(5);
  CHECK(rc == 1 && h.err_pulses == 1, "T13 oversize stored length -> one err");
  CHECK(h.ops.size() == 1 && h.rbytes.empty(),
        "T13 header probe only, nothing forwarded");
  h.store[5][4] = save4; h.store[5][5] = save5;

  // ---- T14: refusals leave the port serviceable --------------------------
  rc = h.restore(5);
  CHECK(rc == 0 && h.rbytes == f2, "T14 clean restore after refusals");
  h.tick();
  CHECK(!dut->nvm_busy_o && !dut->nvm_done_o && !dut->nvm_err_o,
        "idle after the refusal phases");

  // ---- T15: POWER CUT mid-commit (issue #70) -----------------------------
  // A commit is ERASE(region) then WRITE(0, 8+plen). Cut the power inside the
  // WRITE and the region is left erased-plus-partial: the record being written
  // is gone AND so is whatever it replaced. That is a property of writing a
  // slot in place, and it is the reason the flash map reserves A/B slots --
  // so this phase pins what the port DOES guarantee rather than asserting a
  // survival the single-slot layout cannot give.
  //
  // The guarantee that matters for #70 is the one on the next phase: a torn
  // commit of ONE record must not disturb ANOTHER. Here we pin that the torn
  // image never restores as a VALID record -- "never a half-record that
  // restores as garbage" -- graded with the suite's own CRC, because the port
  // carries the CRC opaquely and only the manager can reject on it.
  {
    std::vector<uint8_t> whole = frame(7, pattern(24, 0x40));
    h.ops.clear();
    rc = h.commit(7, whole);
    CHECK(rc == 0 && h.store_match(7, whole), "T15 seed record committed");

    std::vector<uint8_t> replacement = frame(7, pattern(24, 0x90));
    h.ops.clear();
    h.arm_err(1, 12);                 // op 0 = ERASE, op 1 = WRITE: cut at 12 B
    rc = h.commit(7, replacement);
    h.disarm_err();
    CHECK(rc == 1 && h.err_pulses == 1 && h.done_pulses == 0,
          "T15 torn commit reports err, never done");
    // busy_seen is added because busy_ok alone starts true and only clears
    // when a pulse coincides with busy high, so it passes for a port that
    // never pulsed. It does NOT catch a wedge: a wedged port holds busy high,
    // so busy_seen is true and no pulse ever contradicts busy_ok. The wedge is
    // caught by the rc check above; this pair only pins the pulse's timing.
    CHECK(h.busy_seen && h.busy_ok, "T15 busy raised then low at the err pulse");

    // the cut must be REAL: neither the old record nor the new one is intact
    CHECK(!h.store_match(7, whole) && !h.store_match(7, replacement),
          "T15 the torn image is neither the old record nor the new one");

    // ...and it must not read back as a valid record. Either the port refuses
    // it at the header, or the bytes it forwards fail the manager's CRC.
    h.ops.clear();
    rc = h.restore(7);
    bool refused = (rc == 1 && h.rbytes.empty());
    bool crc_rejects = false;
    if (!refused && h.rbytes.size() >= 8) {
      std::vector<uint8_t> cb(h.rbytes.begin(), h.rbytes.begin() + 6);
      cb.insert(cb.end(), h.rbytes.begin() + 8, h.rbytes.end());
      uint16_t stored = uint16_t(uint16_t(h.rbytes[6] << 8) | h.rbytes[7]);
      crc_rejects = (crc16(cb) != stored);
    }
    CHECK(refused || crc_rejects,
          "T15 a torn record never restores as a valid one");
    // ...and record WHICH branch fired: today the port forwards the whole
    // 32-byte image and only the CRC rejects it. Pinning that keeps a
    // regression toward refusing every restore from silently satisfying
    // the disjunction above.
    CHECK(crc_rejects && h.rbytes.size() == whole.size(),
          "T15 the torn image is forwarded whole and rejected on CRC");

    // the port survives the cut: a clean re-commit is byte-exact again
    h.ops.clear();
    rc = h.commit(7, replacement);
    CHECK(rc == 0 && h.store_match(7, replacement),
          "T15 serviceable after the cut: clean re-commit is byte-exact");
  }

  // ---- T16: a torn commit must not disturb the REST of the saved set -----
  // This is the #70 property proper. Records live in their own regions, so a
  // power cut while writing one must leave every other record readable and
  // byte-exact -- otherwise one interrupted save loses the whole set.
  {
    std::vector<uint8_t> keep = frame(4, pattern(16, 0x11));
    rc = h.commit(4, keep);
    CHECK(rc == 0 && h.store_match(4, keep), "T16 neighbour record committed");

    // snapshot EVERY region: checking only the neighbour goes blind if the
    // clobber lands one region over, and the README claims "every other
    // record", not "the record next door".
    int erases_before[N_REGIONS];
    std::vector<std::vector<uint8_t>> store_before(N_REGIONS);
    for (int r = 0; r < N_REGIONS; ++r) {
      erases_before[r] = h.erase_count[r];
      store_before[r].assign(h.store[r], h.store[r] + REG_BYTES);
    }
    h.ops.clear();
    h.arm_err(1, 5);
    rc = h.commit(1, frame(1, pattern(16, 0x22)));
    h.disarm_err();
    CHECK(rc == 1, "T16 the neighbouring commit was torn");

    bool other_erased = false, other_moved = false;
    for (int r = 0; r < N_REGIONS; ++r) {
      if (r == 1) continue;                       // the torn record's own region
      if (h.erase_count[r] != erases_before[r]) other_erased = true;
      if (!std::equal(store_before[r].begin(), store_before[r].end(),
                      h.store[r])) other_moved = true;
    }
    // Anti-vacuity: "no OTHER region moved" is trivially true if the commit
    // never reached the device at all. Pin that it DID touch its own region
    // first, so the isolation claim below is made about a real operation.
    // Both halves are needed: the ERASE alone fires even when zero data bytes
    // ever move (arm_err(1,-1) fails the WRITE before its first byte), so the
    // region's own bytes must be seen to change as well.
    CHECK(h.erase_count[1] > erases_before[1],
          "T16 the torn commit really did erase its own region");
    // "the bytes changed" is NOT enough: the ERASE alone changes them, to
    // 0xFF. What distinguishes a WRITE that moved payload is a byte past the
    // erased state. Without this, arming the WRITE to fail before its first
    // byte (arm_err(1, -1)) leaves the whole phase green while proving nothing.
    bool wrote_payload = false;
    for (int b = 0; b < REG_BYTES; ++b)
      if (h.store[1][b] != 0xFF) { wrote_payload = true; break; }
    CHECK(wrote_payload,
          "T16 the torn commit really did move payload into its own region");
    CHECK(!other_erased, "T16 the torn commit erased no other region");
    CHECK(!other_moved, "T16 no other region's bytes moved");
    CHECK(h.store_match(4, keep),
          "T16 the neighbour's stored bytes are untouched");
    h.ops.clear();
    rc = h.restore(4);
    CHECK(rc == 0 && h.rbytes == keep,
          "T16 the neighbour still restores byte-exactly after the cut");
  }

  // ---------------------------------------------------------------- T17
  // The cut that NOR flash actually produces. T15/T16 cut mid-stream, while
  // bytes are still moving. A real program failure is not reported then: the
  // device latches the bytes, starts the program cycle, and raises its error
  // only when that cycle ends -- after the LAST byte, with busy still high.
  // That is the port's S_WWAIT arm (KL_pp_nvm_port.sv:236-239), the widest
  // window in a commit, and no phase above enters it. Arming at exactly the
  // write length takes the device's fail branch in preference to its done
  // branch, so every byte is consumed and then err replaces done.
  {
    std::vector<uint8_t> rec = frame(3, pattern(20, 0xC5));
    h.ops.clear();
    int rc = h.commit(3, rec);
    CHECK(rc == 0 && h.store_match(3, rec), "T17 seed record committed");

    std::vector<uint8_t> late = frame(3, pattern(20, 0xD6));
    h.ops.clear();
    // op 0 = ERASE, op 1 = WRITE. The WRITE carries the whole framed record,
    // so cutting after that many bytes lands in the completion window.
    h.arm_err(1, static_cast<int>(late.size()));
    rc = h.commit(3, late);
    h.disarm_err();
    CHECK(rc == 1 && h.err_pulses == 1 && h.done_pulses == 0,
          "T17 a failure in the completion window reports err, never done");
    CHECK(h.busy_seen && h.busy_ok,
          "T17 busy raised then low at the err pulse");
    // A wedge and a wrong answer are different failures and rc == 1 above
    // already excludes both, so do not restate it. Assert instead what only a
    // released port can show: busy low and idle once the pulse has passed.
    CHECK(!dut->nvm_busy_o, "T17 the port is idle after the late failure");

    // What the array holds afterwards is NOT pinned here, and deliberately.
    // This device model writes every byte and then reports the failure, so
    // region 3 now holds a well-formed `late`. Real NOR may leave the last
    // page half-programmed. The port cannot tell those apart and neither can
    // this model, so the phase pins what the PORT owes -- err not done, busy
    // released, bus not stranded -- and only that the port stays usable.
    h.ops.clear();
    rc = h.commit(3, rec);
    CHECK(rc == 0 && h.store_match(3, rec),
          "T17 the port accepts the next commit after a late failure");
  }

  // The real close. The check above used to carry this name but T15/T16/T17
  // were appended after it, so nothing pinned the port's state at the end of
  // the run any more -- a tear phase could leave it busy and no check would say.
  h.tick();
  CHECK(!dut->nvm_busy_o && !dut->nvm_done_o && !dut->nvm_err_o,
        "idle again at the end of the run");

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
