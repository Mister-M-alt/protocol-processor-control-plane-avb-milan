// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_acmp_nvm_shadow suite — persistence shadow against an INDEPENDENT
// record model, with the REAL KL_pp_nvm_port and the REAL KL_pp_acmp_listener
// in the loop (face compatibility by elaboration, not transcription).
//
// The harness plays the physical NVM (a region-store device model behind
// the port's device face, with grant/completion delays, per-byte stalls
// and targeted error injection) and independently re-implements the F07.8
// record contract: crc16 CCITT-FALSE, big-endian 16-bit header fields, and
// the 20-byte BINDING[i] payload {flags[valid,started,sw], rsv, uid, tk
// EID, ctlr EID} transcribed from 05 §5 / 07 §5 — never from the RTL.
// Injected F07.6 record images (transcribed from the doc figure) stand in
// for executor write-backs: settle, unbind, started-change, and volatile
// -field-only churn that must cost zero NVM traffic.
//
// Proven: debounce coalescing (N changes in one T-NVM-DEBOUNCE window ->
// one ERASE+WRITE burst per touched sink, byte-exact against the model);
// write-through on CHANGE only; boot replay driving the listener's exact
// pre_* face (all sinks ascending, field mapping checked at the accept AND
// against the PRB_W_AVAIL record the listener then writes, discovery
// armed); per-record vendor defaults (empty region, bad crc, bad layout
// version, bad length) that never abort; a TORN mid-record read-back that
// atomically aborts the WHOLE restore (no preload at all, restore_fail);
// change-during-restore ordering (the capture wins over the image, its
// sink is never preloaded, the live value is flushed back); bounded
// commit retry then the sticky alarm.
//
// Group L drives the listener's four work faces through the REAL
// KL_pp_acmp_lsn_admit, from producer models that hold a presented request
// until its handshake as the dispatch queue, the event router and the AECP
// engine do, and grades what the listener TOOK and ANSWERED and what the
// device ends up holding (issue #92, and issue #93's S4 admission).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Vacmp_nvm_wrap.h"
#include "verilated.h"
#include "../common/verilator_harness.hpp"

#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// ---- geometry (pinned by the Makefile / wrap params) ----------------------
constexpr int N_SINKS   = 8;
constexpr int REC_BASE  = 0x20;
constexpr int DEB_TICKS = 50;      // -GDEB_TICKS_P
constexpr int RETRY_MAX = 2;
constexpr int REG_BYTES = 64;
//! ACMP_REC_W_C (384 b, F07.6) carried as 32-bit Verilator words.
constexpr int REC_WORDS = 12;
// device opcodes: plain integers on the port's `dev_op_o`, so `constexpr`
// rather than an `enum class` that would need a cast at every use.
constexpr int OP_READ  = 0;
constexpr int OP_WRITE = 1;
constexpr int OP_ERASE = 2;

// ---- independent F07.8 record model (07 §5.2 transcription) ---------------
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

struct Bind {
  bool valid = false;
  bool started = false;
  bool sw = false;
  uint16_t uid = 0;
  uint64_t tk = 0;
  uint64_t ctlr = 0;
  bool operator==(const Bind& o) const {
    return valid == o.valid && started == o.started && sw == o.sw &&
           uid == o.uid && tk == o.tk && ctlr == o.ctlr;
  }
};

// the 20-byte BINDING payload (05 §5 shadow set; layout per the shadow's
// banner contract, transcribed independently)
static std::vector<uint8_t> payload_of(const Bind& b) {
  std::vector<uint8_t> p(20, 0);
  p[0] = uint8_t((b.valid ? 1 : 0) | (b.started ? 2 : 0) | (b.sw ? 4 : 0));
  p[2] = uint8_t(b.uid >> 8);
  p[3] = uint8_t(b.uid & 0xFF);
  for (int i = 0; i < 8; ++i) p[4 + i]  = uint8_t(b.tk >> (8 * (7 - i)));
  for (int i = 0; i < 8; ++i) p[12 + i] = uint8_t(b.ctlr >> (8 * (7 - i)));
  return p;
}

static std::vector<uint8_t> frame(uint8_t rid, const std::vector<uint8_t>& pl,
                                  uint8_t ver = 0x02, int force_plen = -1) {
  uint16_t plen = (force_plen >= 0) ? uint16_t(force_plen)
                                    : uint16_t(pl.size());
  std::vector<uint8_t> f;
  f.push_back(0x17);
  f.push_back(0x22);
  f.push_back(ver);
  f.push_back(rid);
  f.push_back(uint8_t(plen >> 8));
  f.push_back(uint8_t(plen & 0xFF));
  std::vector<uint8_t> cb(f);
  cb.insert(cb.end(), pl.begin(), pl.end());
  uint16_t c = crc16(cb);
  f.push_back(uint8_t(c >> 8));
  f.push_back(uint8_t(c & 0xFF));
  f.insert(f.end(), pl.begin(), pl.end());
  return f;
}

// ---- F07.6 record-image bit helpers (doc transcription) -------------------
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

// build an F07.6 image: the persisted set from b, volatile fields = junk
// seeded by j (the shadow must ignore every one of them)
static void mk_rec(uint32_t* w, const Bind& b, uint8_t j) {
  for (int i = 0; i < REC_WORDS; ++i) w[i] = 0;
  wput(w, 0, 3, j % 8);                 // sm_state       (volatile)
  wput(w, 3, 3, j % 4);                 // pbsta          (volatile)
  wput(w, 6, 5, j % 32);                // acmpsta        (volatile)
  wput(w, 11, 1, b.valid ? 1 : 0);      // bound
  wput(w, 12, 1, b.started ? 1 : 0);    // started
  wput(w, 13, 1, b.sw ? 1 : 0);         // sw
  wput(w, 14, 1, j & 1);                // retried        (volatile)
  wput(w, 15, 2, j % 4);                // srp_decl       (volatile)
  wput(w, 17, 1, (j >> 1) & 1);         // tk_reg         (volatile)
  wput(w, 18, 1, (j >> 2) & 1);         // tk_disc        (volatile)
  wput(w, 32, 64, b.tk);                // talker_eid
  wput(w, 96, 16, b.uid);               // talker_uid
  wput(w, 112, 16, 0x1000u + j);        // probe_seq      (volatile)
  wput(w, 128, 64, b.ctlr);             // bind_ctlr_eid
  wput(w, 192, 64, 0xDEAD0000ull + j);  // settled sid    (volatile)
  wput(w, 256, 48, 0x91E0F0ull + j);    // settled da     (volatile)
  wput(w, 304, 12, 2);                  // settled vlan   (volatile)
  wput(w, 320, 32, 0x77000000u + j);    // last_avail     (volatile)
  wput(w, 352, 8, j);                   // if_idx         (volatile)
}

// ---- accepted-preload record ----------------------------------------------
struct Accept {
  int sink;
  Bind b;
};

// ---- the 56-byte Milan ACMPDU (F05.13 offsets, big-endian fields) ---------
constexpr int PDU_BYTES = 56;
constexpr uint64_t OUR_EID = 0x0A0B0C0D0E0F1011ull;   // the wrap's entity_id
// IEEE 1722.1 Table 8.1 message types under their Milan names (Milan 5.5.2)
constexpr uint8_t M_PROBE_TX_CMD = 0;
constexpr uint8_t M_BIND_RX_CMD  = 6;
constexpr uint8_t M_BIND_RX_RSP  = 7;
constexpr uint8_t M_UNBIND_CMD   = 8;
constexpr uint8_t M_UNBIND_RSP   = 9;
constexpr uint8_t M_GETRX_CMD    = 10;
constexpr uint8_t M_GETRX_RSP    = 11;
// ACMP flags (IEEE 1722.1 Table 8.2)
constexpr uint16_t F_FAST_CONNECT   = 0x0002;
constexpr uint16_t F_STREAMING_WAIT = 0x0008;

static void be_put(std::vector<uint8_t>& p, int at, uint64_t v, int n) {
  for (int i = 0; i < n; ++i) p[size_t(at + i)] = uint8_t(v >> (8 * (n - 1 - i)));
}
static uint64_t be_get(const std::vector<uint8_t>& p, int at, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | p[size_t(at + i)];
  return v;
}

static std::vector<uint8_t> acmpdu(uint8_t msg, uint8_t status, uint64_t ctlr,
                                   uint64_t tk, uint16_t tkuid, uint16_t luid,
                                   uint16_t cc, uint16_t seq, uint16_t flags) {
  std::vector<uint8_t> p(PDU_BYTES, 0);
  p[0] = 0xFC;                              // subtype
  p[1] = uint8_t(msg & 0x0F);               // h=0, version 0, message_type
  p[2] = uint8_t(status << 3);              // status, cdl[10:8] = 0
  p[3] = 44;                                // cdl (Milan 5.5.2: 44)
  be_put(p, 12, ctlr, 8);                   // controller_entity_id
  be_put(p, 20, tk, 8);                     // talker_entity_id
  be_put(p, 28, OUR_EID, 8);                // listener_entity_id
  be_put(p, 36, tkuid, 2);                  // talker_unique_id
  be_put(p, 38, luid, 2);                   // listener_unique_id
  be_put(p, 46, cc, 2);                     // connection_count
  be_put(p, 48, seq, 2);                    // sequence_id
  be_put(p, 50, flags, 2);                  // flags
  return p;
}

// Milan Table 5.37 / F05.14: the GET_RX_STATE_RESPONSE of a sink whose
// binding came back from NVM and has not settled: bound, the saved talker,
// FAST_CONNECT, STREAMING_WAIT copied from the saved binding, stream fields 0
static std::vector<uint8_t> getrx_bound(int sink, const Bind& b, uint64_t ctlr,
                                        uint16_t seq) {
  return acmpdu(M_GETRX_RSP, 0, ctlr, b.tk, b.uid, uint16_t(sink), 1, seq,
                uint16_t(F_FAST_CONNECT | (b.sw ? F_STREAMING_WAIT : 0)));
}

// a work request as its producer holds it
struct Req {
  uint8_t msg = M_GETRX_CMD;
  int sink = 0;
  uint64_t ctlr = 0;
  uint64_t tk = 0;
  uint16_t tkuid = 0;
  uint16_t seq = 0;
  uint16_t flags = 0;
  long at = 0;              // first cycle it may be presented
};

struct Resp {
  long cyc;
  std::vector<uint8_t> b;
};

namespace {

// ---- harness ---------------------------------------------------------------
struct Harness {
  //! The persistence shadow, the physical-NVM device model, the monitors and
  //! the tally in one object. `checks`/`fails` were file-scope mutables (I.2)
  //! and the lettered phases below were one 284-line `main` (F.3).
  const milan::tb::Model<Vacmp_nvm_wrap> model;
  Vacmp_nvm_wrap* const d = model.get();
  long cycles = 0;

  // device model
  uint8_t store[N_SINKS][REG_BYTES];
  struct DevOp {
    int op;
    int region;
    int offset;
    int len;
  };
  std::vector<DevOp> ops;
  int d_st = 0;                  // 0 idle, 1 data, 2 completing
  int d_reqwait = 0;
  bool d_gnt = false;
  bool d_done = false;
  bool d_err = false;
  bool d_busy = false;
  DevOp d_cur{0, 0, 0, 0};
  int d_bytes = 0;
  int d_stall = 0;
  bool d_rhold = false;
  int done_ctr = 0;
  int err_ctr = 0;
  int gnt_delay = 1;
  int op_delay = 2;
  int rstall = 0;
  int wstall = 0;
  // a device that holds the header READ of one region (issue #93, S3):
  // 1 until the manager's stall count reaches hold_n, 2 until hold_n cycles
  // after the manager's abort, 3 for ever
  int hold_region = -1;
  int hold_mode = 0;
  long hold_n = 0;
  bool hold_cur = false;
  bool hold_rel = false;           // the held command was released
  long hold_done_cyc = -1;         // the held command's own completion
  long drain_on = -1, drain_off = -1;   // the arbiter's drain: first, last cycle
  long drain_bytes = 0;            // device bytes the drain moved
  long dev_first_rd = -1;          // the device's first restore byte of a boot
  long mgr_first_rd = -1;          // ...and the first that reached the manager
  long abort_cyc = -1;             // the manager's first abort
  long wd_max = 0;                 // the manager's largest stall count
  long last_read_done = -1;        // the last READ the device completed
  long first_erase = -1;           // the first ERASE the device accepted
  long erase_after_read = -1;      // ...and the READ completion before it
  int writes_in_drain = 0;         // ERASE/WRITE accepted while draining
  int aborts = 0;
  int drain_leaks = 0;             // cycles a drained read reached the manager
  // a device that ends the next READ of a region with done after N bytes
  int short_region = -1;
  int short_n = 0;
  bool short_cur = false;
  // manager 1 of the arbiter: one READ, held until its grant
  bool m1_pend = false;
  int m1_rid = 0;
  long m1_gnt_cyc = -1;
  long m1_end_cyc = -1;
  // targeted error injection
  int err_op = -1;
  int err_region = -1;
  int err_offset = -1;
  int err_after = -1;
  int err_count = 0;
  bool fail_cur = false;

  // monitors
  std::vector<Accept> accepts;
  bool pre_valid_seen = false;
  int lsn_preload_wr = 0;        // listener PRB_W_AVAIL preload write-backs
  std::vector<Accept> lsn_pre_recs;
  int disc_arms = 0;
  uint64_t last_disc_eid = 0;

  int checks = 0;
  int fails = 0;

  // carried between phases B, C and D: the op count and the record the
  // started-change left behind
  size_t ops_b = 0;
  Bind b2b;

  // ---- producers in front of the admission gate, and the listener's pools.
  //      A presented request is HELD until its producer-side handshake, as
  //      KL_pp_dispatch's queue head, the event router's sticky latch and
  //      the AECP engine's START/STOP request are.
  std::deque<Req> txq;             // dispatch queue (head presented)
  //! the event router's talker-event source: a head held until its ack; a
  //! LEVEL is re-presented after every ack until `until` (0 = for ever)
  struct Tk {
    uint8_t kind = 0;
    bool failed = false;
    int sink = 0;
    long at = 0;
    long until = 0;
    bool level = false;
  };
  std::deque<Tk> tkq;
  //! the AECP engine's START/STOP request: held until the listener's
  //! completion (ready or error), then retired
  struct Strq {
    int sink = 0;
    bool val = false;
    long at = 0;
  };
  std::deque<Strq> strq;
  struct Done {
    long cyc;
    int sink;
    bool val;
    bool err;
  };
  std::vector<Done> strq_done;
  //! the timer service's expiry bus: owner `exp_owner` every `exp_every`
  //! cycles from `exp_from` up to `exp_until` (exp_every 0 = none)
  long exp_from = 0, exp_until = 0;
  int exp_every = 0;
  unsigned exp_owner = 32;
  long exp_owned = 0;              // listener-owner expiries presented while owned
  long exp_after = 0;              // ...and after the release
  // the PRNG draw port: answers two cycles after a request
  int prng_cnt = 0;
  bool prng_fire = false;
  int head_slot = -1;              // RX slot the head's payload sits in
  int rx_next = 0;
  uint8_t rxmem[4][576];
  uint8_t rx_pending = 0;
  uint8_t txmem[5][64];
  int txlen[5] = {};
  bool txfree[5] = {true, true, true, true, true};
  bool gnt_pending = false;
  int gnt_slot = 0;
  std::vector<Resp> resps;         // every ACMPDU the listener committed
  //! per-cycle hooks, each dropped once it returns true (a case's trigger)
  std::vector<std::function<bool()>> hooks;

  // ---- group L monitors (cleared by reset) ----------------------------------
  std::vector<long> txn_pops, txn_takes;
  int txn_mismatch = 0;            // cycles a pop and a take disagreed
  std::vector<long> tk_pops, tk_takes;
  int tk_mismatch = 0;
  long done_cyc = -1;              // restore_done_o first seen
  long rel_cyc = -1;               // the gate's release first seen
  std::vector<long> pre_take_cyc;  // preload taken (valid && ready)
  std::vector<long> pre_wr_cyc;    // the listener's preload record write
  std::vector<long> arm_cyc;       // the listener's A4 strobe
  long pre_wait = 0;               // current run of an untaken offer
  long pre_wait_max = 0;           // longest run of an untaken offer
  uint32_t own_states = 0;         // listener states seen while owned
  int own_effects = 0;             // side effects while owned
  int own_takes = 0;               // work taken at a listener face while owned
  struct RecWr {
    long cyc;
    int sink;
    bool bound;
    uint64_t tk;
  };
  std::vector<RecWr> recwrs;

  Harness() {
    memset(store, 0, sizeof store);
    memset(rxmem, 0, sizeof rxmem);
    memset(txmem, 0, sizeof txmem);
  }

  int row(int region) const { return (region - REC_BASE) & (N_SINKS - 1); }

  void arm_err(int op, int region, int offset, int after, int count) {
    err_op = op; err_region = region; err_offset = offset;
    err_after = after; err_count = count;
  }
  void disarm_err() { err_op = -1; err_count = 0; }

  void seed_region(int sink, const std::vector<uint8_t>& bytes) {
    memset(store[sink], 0, REG_BYTES);
    for (size_t i = 0; i < bytes.size() && i < REG_BYTES; ++i)
      store[sink][i] = bytes[i];
  }
  bool store_match(int sink, const std::vector<uint8_t>& bytes) const {
    for (size_t i = 0; i < bytes.size(); ++i)
      if (store[sink][i] != bytes[i]) return false;
    return true;
  }

  void drive_dev() {
    d->dev_gnt_i  = d_gnt;
    d->dev_done_i = d_done;
    d->dev_err_i  = d_err;
    d->dev_busy_i = d_busy;
    bool wr = (d_st == 1 && d_cur.op == OP_WRITE && d_stall == 0);
    d->dev_wready_i = wr;
    bool rv = (d_st == 1 && d_cur.op == OP_READ && d_bytes < d_cur.len
               && (d_stall == 0 || d_rhold) && !holding());
    d->dev_rvalid_i = rv;
    d->dev_rdata_i  = rv ? store[row(d_cur.region)]
                                [(d_cur.offset + d_bytes) % REG_BYTES]
                         : 0;
  }

  //! the held command's release is one-way: mode 1 releases in the cycle
  //! the manager's stall count reads hold_n, mode 2 hold_n cycles after the
  //! manager's abort, mode 3 never
  bool holding() {
    if (!hold_cur || hold_rel) return false;
    if ((hold_mode == 1 && long(d->mgr_wd_o) >= hold_n)
        || (hold_mode == 2 && abort_cyc >= 0 && cycles >= abort_cyc + hold_n)) {
      hold_rel = true;
      return false;
    }
    return true;
  }

  void sample_dev() {
    bool drove_gnt = d_gnt;
    bool drove_done = d_done;
    bool drove_err = d_err;

    if (drove_gnt && d->dev_req_o) {
      d_cur = {int(d->dev_op_o), int(d->dev_region_o),
               int(d->dev_offset_o), int(d->dev_len_o)};
      ops.push_back(d_cur);
      if (d_cur.op != OP_READ && d->arb_drain_o) ++writes_in_drain;
      if (d_cur.op == OP_ERASE && first_erase < 0) {
        first_erase = cycles;
        erase_after_read = last_read_done;
      }
      d_busy = true; d_bytes = 0; d_stall = 0; d_rhold = false;
      fail_cur = (err_count > 0)
                 && (err_op < 0 || d_cur.op == err_op)
                 && (err_region < 0 || d_cur.region == err_region)
                 && (err_offset < 0 || d_cur.offset == err_offset);
      if (fail_cur) --err_count;
      hold_cur = hold_region >= 0 && d_cur.op == OP_READ
                 && d_cur.region == hold_region && d_cur.offset == 0;
      if (hold_cur) { hold_region = -1; hold_rel = false; }
      short_cur = short_region >= 0 && d_cur.op == OP_READ
                  && d_cur.region == short_region && d_cur.offset == 0;
      if (short_cur) short_region = -1;
      if (fail_cur && err_after < 0) {
        err_ctr = op_delay; d_st = 2;
      } else if (d_cur.op == OP_ERASE) {
        memset(store[row(d_cur.region)], 0xFF, REG_BYTES);
        done_ctr = op_delay; d_st = 2;
      } else if (d_cur.len == 0) {
        done_ctr = op_delay; d_st = 2;
      } else {
        d_st = 1;
      }
    }

    if (d_st == 1 && d_cur.op == OP_WRITE) {
      if (d->dev_wready_i && d->dev_wvalid_o) {
        store[row(d_cur.region)][(d_cur.offset + d_bytes) % REG_BYTES] =
            d->dev_wdata_o;
        ++d_bytes;
        d_stall = wstall;
        if (fail_cur && d_bytes == err_after) { err_ctr = 2; d_st = 2; }
        else if (d_bytes == d_cur.len) { done_ctr = op_delay; d_st = 2; }
      } else if (d_stall > 0) --d_stall;
    }

    if (d_st == 1 && d_cur.op == OP_READ) {
      if (d->dev_rvalid_i) {
        if (d->dev_rready_o) {
          ++d_bytes; d_rhold = false; d_stall = rstall;
          if (fail_cur && d_bytes == err_after) { err_ctr = 2; d_st = 2; }
          else if (d_bytes == d_cur.len) { done_ctr = op_delay; d_st = 2; }
          else if (short_cur && d_bytes == short_n) {
            short_cur = false; done_ctr = op_delay; d_st = 2;   // ended short
          }
        } else {
          d_rhold = true;
        }
      } else if (d_stall > 0) --d_stall;
    }

    if (drove_gnt)  d_gnt  = false;
    if (drove_done) d_done = false;
    if (drove_err)  d_err  = false;

    if (d_st == 0 && !d_busy && d->dev_req_o && !drove_gnt) {
      if (++d_reqwait >= gnt_delay) { d_gnt = true; d_reqwait = 0; }
    }
    if (d_st == 2) {
      if (done_ctr > 0 && --done_ctr == 0) {
        d_done = true; d_busy = false; d_st = 0;
        if (d_cur.op == OP_READ) last_read_done = cycles;
        if (hold_cur) { hold_cur = false; hold_done_cyc = cycles; }
      }
      if (err_ctr  > 0 && --err_ctr  == 0) { d_err  = true; d_busy = false; d_st = 0; }
    }
  }

  //! Neither sampler reads a cycle held in reset: the registers it would
  //! read still hold the state the reset is about to clear.
  void sample_monitors() {
    if (!d->rst_n) return;
    if (d->pre_valid_o) pre_valid_seen = true;
    if (d->pre_valid_o && d->pre_ready_o && !d->pre_hold_i) {
      Accept a;
      a.sink      = int(d->pre_sink_o);
      a.b.valid   = true;
      a.b.started = d->pre_started_o;
      a.b.sw      = d->pre_sw_o;
      a.b.uid     = uint16_t(d->pre_talker_uid_o);
      a.b.tk      = d->pre_talker_eid_o;
      a.b.ctlr    = d->pre_ctlr_eid_o;
      accepts.push_back(a);
    }
    if (d->lsn_recwr_o) {
      const uint32_t* w = &d->lsn_recwr_rec_o[0];
      // a preload write-back: PRB_W_AVAIL (sm 1) + bound, from X_PRELOAD
      // (a later GET_RX_STATE write-back of the same record is not one)
      if (wget(w, 0, 3) == 1 && wget(w, 11, 1) == 1 && d->lsn_state_o == 2) {
        Accept a;
        a.sink      = int(d->lsn_recwr_sink_o);
        a.b.valid   = true;
        a.b.started = wget(w, 12, 1) != 0;
        a.b.sw      = wget(w, 13, 1) != 0;
        a.b.uid     = uint16_t(wget(w, 96, 16));
        a.b.tk      = wget(w, 32, 64);
        a.b.ctlr    = wget(w, 128, 64);
        lsn_pre_recs.push_back(a);
        ++lsn_preload_wr;
      }
    }
    if (d->lsn_disc_arm_o) {
      ++disc_arms;
      last_disc_eid = d->lsn_disc_eid_o;
    }
  }

  // the 03 §4 record of an ACMP command received on the wire, in the bit
  // positions of pp_pkg's pp_txn_t (393 bits, 13 words)
  void pack_txn(const Req& r, int slot) {
    uint32_t* w = &d->p_txn_i[0];
    for (int i = 0; i < 13; ++i) w[i] = 0;
    wput(w, 391, 2, 0);                    // origin RX
    wput(w, 354, 3, 1);                    // protocol ACMP
    wput(w, 350, 4, r.msg);                // message_type
    wput(w, 334, 11, 44);                  // cdl
    wput(w, 222, 64, r.ctlr);              // controller_entity_id
    wput(w, 158, 64, OUR_EID);             // target (listener_entity_id)
    wput(w, 142, 16, r.seq);               // sequence_id
    wput(w, 124, 16, r.msg);               // opcode mirror
    wput(w, 60, 16, uint64_t(r.sink));     // operands.unique_id
    wput(w, 57, 3, uint64_t(slot));        // rx_slot
    wput(w, 0, 2, 1);                      // ACMP multicast disposition
  }

  void drive_producers() {
    for (size_t i = 0; i < hooks.size();) {
      if (hooks[i]()) hooks.erase(hooks.begin() + long(i));
      else ++i;
    }
    // dispatch head: its payload is placed in an RX slot when it is first
    // presented, and it stays presented until the producer sees its pop
    bool pres = !txq.empty() && cycles >= txq.front().at;
    if (pres && head_slot < 0) {
      const Req& r = txq.front();
      head_slot = rx_next;
      rx_next = (rx_next + 1) % 4;
      std::vector<uint8_t> p = acmpdu(r.msg, 0, r.ctlr, r.tk, r.tkuid,
                                      uint16_t(r.sink), 0, r.seq, r.flags);
      memcpy(rxmem[head_slot], p.data(), PDU_BYTES);
    }
    if (pres) pack_txn(txq.front(), head_slot);
    d->p_txn_valid_i = pres;
    d->m1_req_i = m1_pend;
    d->m1_we_i = 0;
    d->m1_rid_i = uint8_t(m1_rid);
    d->m1_wvalid_i = 0;
    d->m1_wdata_i = 0;
    d->m1_rready_i = 1;
    d->m1_abort_i = 0;
    while (!tkq.empty() && tkq.front().level && tkq.front().until > 0
           && cycles >= tkq.front().until) tkq.pop_front();   // a finite level ends
    const bool tk = !tkq.empty() && cycles >= tkq.front().at;
    d->p_tk_valid_i = tk;
    d->p_tk_kind_i = tk ? tkq.front().kind : 0;
    d->p_tk_failed_i = tk && tkq.front().failed;
    d->p_tk_sink_i = tk ? uint16_t(tkq.front().sink) : 0;
    const bool st = !strq.empty() && cycles >= strq.front().at;
    d->p_strm_valid_i = st;
    d->p_strm_sink_i = st ? uint16_t(strq.front().sink) : 0;
    d->p_strm_val_i = st && strq.front().val;
    const bool ex = exp_every > 0 && cycles >= exp_from && cycles < exp_until
                    && (cycles - exp_from) % exp_every == 0;
    d->p_exp_valid_i = ex;
    d->p_exp_owner_i = uint8_t(exp_owner);
    d->p_exp_slot_i = uint8_t(9 + (exp_owner - 32));
    d->draw_busy_i = prng_cnt > 0;
    d->draw_valid_i = prng_fire;
    d->draw_ms_i = prng_fire ? 17 : 0;
    if (prng_fire) prng_fire = false;
    else if (prng_cnt > 0 && --prng_cnt == 0) prng_fire = true;
    // the pools answer last cycle's requests
    d->rxs_rd_data_i = rx_pending;
    d->txs_alloc_gnt_i = gnt_pending;
    d->txs_alloc_slot_i = uint8_t(gnt_slot);
    gnt_pending = false;
  }

  void sample_producers() {
    if (!d->rst_n) return;
    const bool pop  = d->p_txn_valid_i && d->p_txn_ready_o;
    const bool take = d->l_txn_valid_o && d->l_txn_ready_o;
    if (pop != take) ++txn_mismatch;
    if (pop) { txn_pops.push_back(cycles); txq.pop_front(); head_slot = -1; }
    if (take) txn_takes.push_back(cycles);
    const bool tpop  = d->p_tk_valid_i && d->p_tk_ready_o;
    const bool ttake = d->l_tk_valid_o && d->l_tk_ready_o;
    if (tpop != ttake) ++tk_mismatch;
    if (tpop) {
      tk_pops.push_back(cycles);
      Tk& h = tkq.front();
      if (!h.level || (h.until > 0 && cycles + 1 >= h.until)) tkq.pop_front();
    }
    if (ttake) tk_takes.push_back(cycles);
    if (d->p_strm_valid_i && (d->strm_ready_o || d->strm_error_o)) {
      strq_done.push_back({cycles, strq.front().sink, strq.front().val,
                           d->strm_error_o != 0});
      strq.pop_front();
    }
    if (d->p_exp_valid_i && d->p_exp_owner_i >= 32 && d->p_exp_owner_i < 32 + N_SINKS) {
      if (d->gate_own_o) ++exp_owned;
      else ++exp_after;
    }
    if (d->draw_req_o && prng_cnt == 0 && !prng_fire) prng_cnt = 2;
    if (d->rxs_rd_en_o) rx_pending = rxmem[d->rxs_rd_slot_o][d->rxs_rd_addr_o];
    if (d->txs_alloc_req_o) {
      for (int i = 0; i < 4; ++i)
        if (txfree[i]) { txfree[i] = false; gnt_slot = i; gnt_pending = true; break; }
    }
    if (d->txs_wr_valid_o && d->txs_wr_addr_o < 64)
      txmem[d->txs_wr_slot_o][d->txs_wr_addr_o] = d->txs_wr_data_o;
    if (d->txs_wr_commit_o) txlen[d->txs_wr_slot_o] = d->txs_wr_len_o;
    if (d->txreq_valid_o) {
      const int s = d->txreq_slot_o;
      resps.push_back({cycles, std::vector<uint8_t>(
          txmem[s], txmem[s] + std::min(txlen[s], PDU_BYTES))});
      txfree[s] = true;                    // the arbiter frees after serializing
    }
    if (d->restore_done_o && done_cyc < 0) done_cyc = cycles;
    if (d->gate_released_o && rel_cyc < 0) rel_cyc = cycles;
    const bool offered = d->pre_valid_o && !d->pre_hold_i;
    if (offered && d->pre_ready_o) pre_take_cyc.push_back(cycles);
    if (offered && !d->pre_ready_o) {
      pre_wait_max = std::max(pre_wait_max, ++pre_wait);
    } else {
      pre_wait = 0;
    }
    if (d->lsn_recwr_o) {
      const uint32_t* w = &d->lsn_recwr_rec_o[0];
      recwrs.push_back({cycles, int(d->lsn_recwr_sink_o), wget(w, 11, 1) != 0,
                        wget(w, 32, 64)});
      if (d->lsn_state_o == 2) pre_wr_cyc.push_back(cycles);   // X_PRELOAD
    }
    if (d->lsn_disc_arm_o) arm_cyc.push_back(cycles);
    if (d->mgr_abort_o) { ++aborts; if (abort_cyc < 0) abort_cyc = cycles; }
    if (d->dev_rvalid_i && d->dev_rready_o && dev_first_rd < 0) dev_first_rd = cycles;
    if (d->mgr_rvalid_o && mgr_first_rd < 0) mgr_first_rd = cycles;
    wd_max = std::max(wd_max, long(d->mgr_wd_o));
    if (d->arb_drain_o && (d->mgr_rvalid_o || d->mgr_done_o || d->mgr_err_o
                           || d->m1_rvalid_o || d->m1_done_o || d->m1_err_o))
      ++drain_leaks;
    if (d->arb_drain_o) {
      if (drain_on < 0) drain_on = cycles;
      drain_off = cycles;
      if (d->dev_rvalid_i && d->dev_rready_o) ++drain_bytes;
    }
    if (d->m1_req_i && d->m1_gnt_o) { m1_pend = false; m1_gnt_cyc = cycles; }
    if (d->m1_done_o || d->m1_err_o) m1_end_cyc = cycles;
    if (d->gate_own_o) {
      own_states |= 1u << (d->lsn_state_o & 31);
      own_effects += d->tmr_arm_valid_o + d->txs_alloc_req_o + d->rxs_free_o
                   + d->lsn_settle_o + d->lsn_teardown_o + d->lsn_disarm_o
                   + d->lsn_notify_o + d->strm_ready_o + d->strm_error_o
                   + d->draw_req_o;
      own_takes += take + (d->l_tk_valid_o && d->l_tk_ready_o)
                 + d->l_strm_valid_o + d->l_exp_valid_o;
    }
  }

  void tick() {
    drive_dev();
    drive_producers();
    d->clk_i = 0; d->eval();
    sample_monitors();
    sample_producers();
    sample_dev();
    d->clk_i = 1; d->eval();
    ++cycles;
  }

  void run(int n) { for (int i = 0; i < n; ++i) tick(); }

  template <typename F>
  bool run_until(F cond, int max_cycles) {
    for (int i = 0; i < max_cycles; ++i) {
      if (cond()) return true;
      tick();
    }
    return cond();
  }

  //! `keep_producers` models a producer that is NOT reset with the entity
  //! and keeps presenting what it held: the stricter case for the gate.
  void reset(bool keep_producers = false) {
    d->rst_n = 0;
    d->tick_i = 0;
    d->restore_go_i = 0;
    d->tb_cap_wr_i = 0;
    d->tb_cap_sink_i = 0;
    for (int i = 0; i < REC_WORDS; ++i) d->tb_cap_rec_i[i] = 0;
    d->pre_hold_i = 0;
    d->p_tk_valid_i = 0; d->p_tk_kind_i = 0; d->p_tk_failed_i = 0;
    d->p_tk_sink_i = 0;
    d->p_strm_valid_i = 0; d->p_strm_sink_i = 0; d->p_strm_val_i = 0;
    d->p_exp_valid_i = 0; d->p_exp_slot_i = 0; d->p_exp_owner_i = 0;
    d->draw_busy_i = 0; d->draw_valid_i = 0; d->draw_ms_i = 0;
    d_st = 0; d_reqwait = 0; d_gnt = d_done = d_err = d_busy = false;
    d_bytes = d_stall = 0; d_rhold = false; done_ctr = err_ctr = 0;
    disarm_err();
    ops.clear(); accepts.clear(); lsn_pre_recs.clear();
    pre_valid_seen = false; lsn_preload_wr = 0; disc_arms = 0;
    if (!keep_producers) {
      txq.clear(); hooks.clear(); tkq.clear(); strq.clear(); exp_every = 0;
    }
    strq_done.clear();
    tk_pops.clear(); tk_takes.clear(); tk_mismatch = 0;
    hold_region = -1; hold_mode = 0; hold_n = 0; hold_cur = false;
    hold_rel = false; dev_first_rd = mgr_first_rd = -1;
    hold_done_cyc = -1; abort_cyc = -1; aborts = 0; drain_leaks = 0;
    drain_on = drain_off = -1; drain_bytes = 0;
    wd_max = 0; last_read_done = first_erase = erase_after_read = -1;
    writes_in_drain = 0;
    short_region = -1; short_n = 0; short_cur = false;
    m1_pend = false; m1_gnt_cyc = -1; m1_end_cyc = -1;
    d->m1_req_i = 0; d->m1_abort_i = 0;
    exp_owned = exp_after = 0;
    prng_cnt = 0; prng_fire = false;
    head_slot = -1;
    gnt_pending = false;
    for (bool& f : txfree) f = true;
    resps.clear(); recwrs.clear();
    txn_pops.clear(); txn_takes.clear(); txn_mismatch = 0;
    done_cyc = rel_cyc = -1;
    pre_take_cyc.clear(); pre_wr_cyc.clear(); arm_cyc.clear();
    pre_wait = pre_wait_max = 0;
    own_states = 0; own_effects = 0; own_takes = 0;
    run(5);
    d->rst_n = 1;
    d->tick_i = 1;               // 1 tick per cycle: window = DEB_TICKS cycles
    run(N_SINKS + 6);            // both init sweeps complete
  }

  void inject(int sink, const Bind& b, uint8_t junk) {
    uint32_t w[REC_WORDS];
    mk_rec(w, b, junk);
    d->tb_cap_sink_i = uint8_t(sink);
    for (int i = 0; i < REC_WORDS; ++i) d->tb_cap_rec_i[i] = w[i];
    d->tb_cap_wr_i = 1;
    tick();
    d->tb_cap_wr_i = 0;
    tick();
  }

  void go() {
    d->restore_go_i = 1;
    tick();
    d->restore_go_i = 0;
  }

  bool restore_done() { return d->restore_done_o != 0; }

  int count_ops(int op, int region = -1, int offset = -1) const {
    int n = 0;
    for (const DevOp& o : ops)
      if (o.op == op && (region < 0 || o.region == region)
          && (offset < 0 || o.offset == offset)) ++n;
    return n;
  }

  void check_empty_boot_reports_blank();
  void check_capture_debounce_and_write_through();
  void check_volatile_only_churn_costs_no_traffic();
  void check_unbind_capture_commits();
  void check_bounded_commit_retry_and_the_alarm();
  void check_boot_replay_from_a_seeded_image();
  void check_a_torn_readback_aborts_the_whole_restore();
  void check_a_change_during_restore_wins();
  void check_a_change_during_its_own_flush_reserializes();
  void check_the_unflushed_export_contract();

  // ---- group L: the boot window and the listener's admission ---------------
  Bind l_saved[N_SINKS];
  void l_seed(const std::vector<std::pair<int, Bind>>& s);
  void l_boot(bool keep_producers = false);
  void l_finish();
  void l_push(uint8_t msg, int sink, uint16_t seq, long at = 0,
              uint64_t tk = 0, uint16_t tkuid = 0, uint16_t flags = 0);
  std::vector<Resp> l_resps(uint8_t msg) const;
  bool l_walk_ok(std::string& why);
  bool l_nvm_is(int sink, const Bind& b) const;
  bool l_nvm_untouched(std::string& why) const;
  bool l_round_trip(const Bind (&want)[N_SINKS], std::string& why);
  void l_grade(const char* tag, int n_txn, bool nvm_untouched);
  void check_l00_restore_control();
  void check_l05_read_only_in_the_window();
  void check_l05_anywhere_in_the_window();
  void check_l09_live_changes_stay_ordered();
  void check_l10_release_boundary();
  void check_l01_talker_events();
  void check_l04_polled_reads();
  void check_l06_start_stop();
  void check_l07_expiries();

  // ---- group N: the walk's cause and its deadline (issue #93, S1 and S3) -
  bool n_failed_walk(unsigned cause, std::string& why);
  void check_n1_device_errors_fail_the_walk();
  void check_n2_unframed_keeps_the_default();
  void check_n3_silence_while_reading();
  void check_n4_silence_before_reading();
  void check_n5_the_deadline_boundary();
  void check_n6_a_second_manager_on_the_port();
  void check_l_reset_boundaries();
  int report();
  int run_suite();
};

// ---- constants -------------------------------------------------------------
constexpr uint64_t TK_A = 0x00221100AABBCCDDull;
constexpr uint64_t TK_B = 0x00221100AABBCC55ull;
constexpr uint64_t CTL1 = 0x0011223344556677ull;
constexpr uint64_t CTL2 = 0x0099AABBCCDDEEFFull;

// ======================================================== A: empty boot
void Harness::check_empty_boot_reports_blank() {
  reset();
  CHECK(d->pre_ready_o, "A0 listener idle, pre_ready high");
  go();
  CHECK(run_until([&] { return restore_done(); }, 5000),
        "A1 restore completes on an empty NVM");
  CHECK(!d->restore_fail_o && d->restore_cause_o == 0,
        "A2 empty NVM is defaults, not a failure (cause %u)",
        unsigned(d->restore_cause_o));
  // ...but "not a failure" is not "a restore happened". Milan 5.3.8.2 makes
  // the bound state reportable, and done is set on THIS path exactly as it is
  // on a walk that put every sink back, so an integrator reading done alone
  // cannot tell the two apart. restore_blank_o is the pin that can.
  CHECK(d->restore_blank_o,
        "A2b an empty NVM reports BLANK: done, but zero records validated");
  CHECK(!pre_valid_seen, "A3 no preload driven from an empty NVM");
  CHECK(count_ops(OP_READ) == N_SINKS,
        "A4 walk = one header read per sink (got %d)", count_ops(OP_READ));
  CHECK(d->dbg_dirty_o == 0, "A5 nothing dirty after restore");
  CHECK(d->dbg_valid_o == 0, "A6 nothing valid after restore");
}

// ============================= B: capture + debounce + write-through bytes
void Harness::check_capture_debounce_and_write_through() {
  Bind b2{true, false, true, 1, TK_A, CTL1};
  inject(2, b2, 0x11);
  run(10);
  CHECK(count_ops(OP_WRITE) == 0, "B1 no commit inside the debounce window");
  CHECK((d->dbg_dirty_o >> 2) & 1, "B2 change marked dirty");
  b2b = b2; b2b.started = true;                 // started-change
  inject(2, b2b, 0x12);
  Bind b5{true, false, false, 2, TK_B, CTL2};
  inject(5, b5, 0x13);
  CHECK(run_until([&] { return count_ops(OP_WRITE) >= 2; }, 3000),
        "B3 burst released after T-NVM-DEBOUNCE");
  run(300);                                    // drain + prove quiescence
  CHECK(count_ops(OP_WRITE) == 2,
        "B4 three changes coalesce to ONE burst of two records (got %d)",
        count_ops(OP_WRITE));
  CHECK(count_ops(OP_ERASE) == 2, "B5 each commit is ERASE then WRITE");
  CHECK(store_match(2, frame(uint8_t(REC_BASE + 2), payload_of(b2b))),
        "B6 sink 2 record byte-exact vs the model (latest fields win)");
  CHECK(store_match(5, frame(uint8_t(REC_BASE + 5), payload_of(b5))),
        "B7 sink 5 record byte-exact vs the model");
  CHECK(d->dbg_dirty_o == 0, "B8 dirty cleared after the burst");
  ops_b = ops.size();
}

// ============================= C: volatile-only churn costs zero traffic
void Harness::check_volatile_only_churn_costs_no_traffic() {
  inject(2, b2b, 0x77);                        // same persisted set, new junk
  run(DEB_TICKS * 3);
  CHECK(d->dbg_dirty_o == 0, "C1 volatile-field write-back never dirties");
  CHECK(ops.size() == ops_b, "C2 volatile-field write-back costs no NVM op");
}

// ======================================================= D: unbind capture
void Harness::check_unbind_capture_commits() {
  Bind un{};                                     // valid=0, all zero
  inject(5, un, 0x00);
  CHECK(run_until([&] { return ops.size() >= ops_b + 2; }, 3000),
        "D1 unbind commits after the window");
  run(200);
  CHECK(store_match(5, frame(uint8_t(REC_BASE + 5), payload_of(un))),
        "D2 unbind record byte-exact (valid=0 payload)");
  CHECK(ops.size() == ops_b + 2, "D3 exactly one ERASE+WRITE for the unbind");
}

// ================================== E: commit retry (bounded) + the alarm
void Harness::check_bounded_commit_retry_and_the_alarm() {
  Bind b1{true, true, false, 3, TK_B, CTL1};
  arm_err(OP_WRITE, REC_BASE + 1, -1, -1, 1);  // fail the first WRITE once
  inject(1, b1, 0x21);
  CHECK(run_until([&] {
          return count_ops(OP_WRITE, REC_BASE + 1) >= 2 && !d->dbg_dirty_o;
        }, 4000),
        "E1 errored commit is retried and lands");
  CHECK(store_match(1, frame(uint8_t(REC_BASE + 1), payload_of(b1))),
        "E2 retried record byte-exact");
  CHECK(!d->alarm_o, "E3 a recovered retry never alarms");
  Bind b1b = b1; b1b.started = false;
  arm_err(OP_WRITE, REC_BASE + 1, -1, -1, 100);  // fail every WRITE
  inject(1, b1b, 0x22);
  CHECK(run_until([&] { return d->alarm_o != 0; }, 6000),
        "E4 retries exhausted -> sticky side-port alarm");
  run(100);
  CHECK(d->dbg_dirty_o == 0, "E5 given-up record drops dirty (no livelock)");
  disarm_err();
  size_t ops_e = ops.size();
  Bind b4{true, false, true, 4, TK_A, CTL2};
  inject(4, b4, 0x23);
  CHECK(run_until([&] {
          return count_ops(OP_WRITE, REC_BASE + 4) >= 1 && !d->dbg_dirty_o;
        }, 4000),
        "E6 the engine still commits after an alarm");
  CHECK(store_match(4, frame(uint8_t(REC_BASE + 4), payload_of(b4))),
        "E7 post-alarm record byte-exact");
  (void)ops_e;
}

// ============================================ F: boot replay, seeded image
void Harness::check_boot_replay_from_a_seeded_image() {
  Bind f0{true, true, false, 5, 0x1111111122222222ull, CTL1};
  Bind f3{true, false, true, 6, 0x3333333344444444ull, CTL2};
  Bind f7{true, true, true, 7, 0x5555555566666666ull, 0x0123456789ABCDEFull};
  Bind f4{};                                      // a committed unbind
  for (int k = 0; k < N_SINKS; ++k) seed_region(k, {});
  seed_region(0, frame(uint8_t(REC_BASE + 0), payload_of(f0)));
  seed_region(3, frame(uint8_t(REC_BASE + 3), payload_of(f3)));
  seed_region(7, frame(uint8_t(REC_BASE + 7), payload_of(f7)));
  seed_region(4, frame(uint8_t(REC_BASE + 4), payload_of(f4)));
  {  // sink 6: crc corrupted; sink 1: wrong layout version; sink 2: bad length
    auto c = frame(uint8_t(REC_BASE + 6), payload_of(f0));
    c[15] ^= 0x40;
    seed_region(6, c);
    //! a version this build does NOT accept. It was 0x02 while the shadow
    //! was at 0x01; issue #78 moved the shadow to 0x02 (the record's bytes
    //! are unchanged but `started` means something now), which would have
    //! quietly turned this negative case into a valid record - the "wrong
    //! version is refused" property would then have been passing on a
    //! record the RTL accepts.
    seed_region(1, frame(uint8_t(REC_BASE + 1), payload_of(f3), 0x03));
    auto pl24 = payload_of(f7); pl24.resize(24, 0);
    seed_region(2, frame(uint8_t(REC_BASE + 2), pl24));
  }
  reset();
  go();
  // backpressure the first preload at the handshake itself. The listener's
  // own priority order no longer can: the admission gate holds every other
  // source at its producer until the walk ends (group L).
  CHECK(run_until([&] { return d->pre_valid_o != 0; }, 5000),
        "F0 replay reaches the first preload");
  {
    d->pre_hold_i = 1;
    bool stable = true;
    uint64_t tk0 = d->pre_talker_eid_o;
    uint16_t s0 = d->pre_sink_o;
    for (int i = 0; i < 25; ++i) {
      tick();
      if (!d->pre_valid_o || d->pre_talker_eid_o != tk0
          || d->pre_sink_o != s0) stable = false;
    }
    CHECK(stable, "F1 pre_* held stable under listener backpressure");
    CHECK(accepts.empty(), "F2 no accept while the listener is busy");
    d->pre_hold_i = 0;
  }
  CHECK(run_until([&] { return restore_done(); }, 20000),
        "F3 restore completes");
  run(5);              // let the listener's last X_PRELOAD strobes land
  CHECK(!d->restore_fail_o && d->restore_cause_o == 0,
        "F4 per-record defaults never abort the restore (cause %u)",
        unsigned(d->restore_cause_o));
  // the counter-case to A2b: three regions held framed, crc-clean records, so
  // the media ANSWERED and this walk is not blank even though five of the
  // eight sinks fell back to the vendor default.
  CHECK(!d->restore_blank_o,
        "F4b a walk that validated records is NOT reported blank");
  CHECK(accepts.size() == 3,
        "F5 exactly the three valid bindings preload (got %zu)",
        accepts.size());
  if (accepts.size() == 3) {
    CHECK(accepts[0].sink == 0 && accepts[1].sink == 3
              && accepts[2].sink == 7,
          "F6 replay walks sinks in ascending order");
    CHECK(accepts[0].b == f0, "F7 sink 0 preload fields exact");
    CHECK(accepts[1].b == f3, "F8 sink 3 preload fields exact");
    CHECK(accepts[2].b == f7, "F9 sink 7 preload fields exact");
  }
  CHECK(lsn_pre_recs.size() == 3,
        "F10 listener wrote three PRB_W_AVAIL records");
  if (lsn_pre_recs.size() == 3) {
    CHECK(lsn_pre_recs[0].sink == 0 && lsn_pre_recs[0].b == f0,
          "F11 listener record 0 carries the restored binding");
    CHECK(lsn_pre_recs[1].sink == 3 && lsn_pre_recs[1].b == f3,
          "F12 listener record 3 carries the restored binding");
    CHECK(lsn_pre_recs[2].sink == 7 && lsn_pre_recs[2].b == f7,
          "F13 listener record 7 carries the restored binding");
  }
  CHECK(disc_arms == 3 && last_disc_eid == f7.tk,
        "F14 discovery armed per preload (A4), talker EID exact");
  CHECK(d->dbg_valid_o == ((1u << 0) | (1u << 3) | (1u << 7)),
        "F15 valid bits mirror the restored image");
  CHECK(d->dbg_dirty_o == 0,
        "F16 the listener's preload write-backs are compare-equal, no dirty");
  run(DEB_TICKS * 3);
  CHECK(count_ops(OP_WRITE) == 0,
        "F17 a clean restore triggers no NVM write-back at all");
  CHECK(count_ops(OP_READ, REC_BASE + 0) == 2
            && count_ops(OP_READ, REC_BASE + 5) == 1,
        "F18 valid record = header+payload reads; empty region = header only");
}

// ===================================== G: torn read-back aborts the WHOLE
void Harness::check_a_torn_readback_aborts_the_whole_restore() {
  for (int k = 0; k < N_SINKS; ++k)
    seed_region(k, frame(uint8_t(REC_BASE + k), payload_of(
        Bind{true, (k & 1) != 0, (k & 2) != 0, uint16_t(k),
             0xAA00000000000000ull + uint64_t(k), CTL1 + k})));
  reset();
  arm_err(OP_READ, REC_BASE + 3, 8, 5, 1);   // tear region 3 payload @5 B
  go();
  CHECK(run_until([&] { return restore_done(); }, 20000),
        "G1 aborted restore still terminates");
  CHECK(d->restore_fail_o && d->restore_cause_o == 1,
        "G2 torn mid-record read-back -> restore_fail, cause torn (%u)",
        unsigned(d->restore_cause_o));
  CHECK(!pre_valid_seen,
        "G3 atomic reject: not ONE preload was driven (sinks 0..2 included)");
  CHECK(d->dbg_valid_o == 0, "G4 the whole image is discarded");
  // three records were already validated before the tear; the atomic reject
  // throws them away, so the blank level has to follow the image and not the
  // history, or a torn walk would report records it no longer holds.
  CHECK(d->restore_blank_o,
        "G4b the atomic reject leaves the walk reporting ZERO records");
  CHECK(count_ops(OP_READ, REC_BASE + 4) == 0,
        "G5 the walk stops at the tear");
  disarm_err();
  Bind g0{true, false, false, 9, TK_A, CTL2};
  inject(0, g0, 0x31);
  CHECK(run_until([&] {
          return count_ops(OP_WRITE, REC_BASE + 0) >= 1 && !d->dbg_dirty_o;
        }, 4000),
        "G6 live capture still commits after an aborted restore");
  CHECK(store_match(0, frame(uint8_t(REC_BASE + 0), payload_of(g0))),
        "G7 post-abort record byte-exact");
}

// ============================== H: change during restore — the capture wins
void Harness::check_a_change_during_restore_wins() {
  std::vector<Bind> hv;
  for (int k = 0; k < N_SINKS; ++k) {
    hv.push_back(Bind{true, (k & 1) != 0, (k & 2) != 0, uint16_t(0x30 + k),
                      0xBB00000000000000ull + uint64_t(k), CTL2 + k});
    seed_region(k, frame(uint8_t(REC_BASE + k), payload_of(hv[k])));
  }
  reset();
  rstall = 2;                                 // stretch the walk
  go();
  // "before its read": live change for sink 6 while the walk is at sink 0
  CHECK(run_until([&] { return !ops.empty(); }, 2000),
        "H0 walk started");
  Bind n6{true, true, false, 0x66, 0x6666666677777777ull, CTL1};
  inject(6, n6, 0x41);
  // "after its read": live change for sink 2 once its record was stored
  CHECK(run_until([&] {
          return count_ops(OP_READ, REC_BASE + 3, 0) >= 1;
        }, 8000),
        "H1 walk reached sink 3 (sink 2 stored)");
  Bind n2{true, false, true, 0x22, 0x2222222233333333ull, CTL2};
  inject(2, n2, 0x42);
  CHECK(run_until([&] { return restore_done(); }, 30000),
        "H2 restore completes around the live changes");
  rstall = 0;
  CHECK(!d->restore_fail_o, "H3 live changes are not a failure");
  CHECK(((d->dbg_touched_o >> 2) & 1) && ((d->dbg_touched_o >> 6) & 1),
        "H4 both live changes marked touched");
  bool pre26 = false;
  for (const Accept& a : accepts)
    if (a.sink == 2 || a.sink == 6) pre26 = true;
  CHECK(!pre26, "H5 a changed sink is never preloaded (ordering)");
  CHECK(accepts.size() == 6, "H6 the six unchanged sinks preload (got %zu)",
        accepts.size());
  for (const Accept& a : accepts) {
    if (a.sink >= 0 && a.sink < N_SINKS && a.sink != 2 && a.sink != 6) {
      CHECK(a.b == hv[a.sink], "H7 sink %d preload fields exact", a.sink);
    }
  }
  CHECK(run_until([&] {
          return count_ops(OP_WRITE, REC_BASE + 2) >= 1
              && count_ops(OP_WRITE, REC_BASE + 6) >= 1
              && !d->dbg_dirty_o;
        }, 6000),
        "H8 the live changes flush after the restore");
  CHECK(store_match(2, frame(uint8_t(REC_BASE + 2), payload_of(n2))),
        "H9 sink 2 NVM ends at the LIVE value, not the restored one");
  CHECK(store_match(6, frame(uint8_t(REC_BASE + 6), payload_of(n6))),
        "H10 sink 6 NVM ends at the LIVE value, not the restored one");
}

// ============ I: change DURING the flush of its own record (taint path)
void Harness::check_a_change_during_its_own_flush_reserializes() {
  size_t wr_before = size_t(count_ops(OP_WRITE, REC_BASE + 5));
  Bind i5{true, false, false, 0x51, 0x5151515152525252ull, CTL1};
  inject(5, i5, 0x51);
  CHECK(run_until([&] {
          return size_t(count_ops(OP_WRITE, REC_BASE + 5)) > wr_before;
        }, 4000),
        "I0 flush of sink 5 starts");
  Bind i5b = i5; i5b.started = true; i5b.uid = 0x52;  // land mid-serialization
  inject(5, i5b, 0x52);
  CHECK(run_until([&] { return d->dbg_dirty_o == 0; }, 6000),
        "I1 the tainted commit re-serializes until clean");
  run(200);
  CHECK(store_match(5, frame(uint8_t(REC_BASE + 5), payload_of(i5b))),
        "I2 NVM converges to the NEWEST value after a mid-flush change");
  CHECK(count_ops(OP_WRITE, REC_BASE + 5) >= int(wr_before) + 2,
        "I3 the stale image was re-committed, not trusted");
}

// ===================== X: dbg_dirty_o IS the unflushed-state contract
// protocol_processor_top publishes this vector as nvm_unflushed_o (issue
// #90) and an integrator ORs it into a "saved state pending" bit, so the
// three edges the pin promises are graded here, at the source, cycle by
// cycle: it rises when a change is ACCEPTED (a capture whose persisted set
// differs), it never drops before that change is committed with done, and
// on retry exhaustion it drops on the SAME cycle the sticky alarm rises —
// never silently. The suite's earlier groups sample the vector at rest;
// what a pending bit needs is the span between those samples.
void Harness::check_the_unflushed_export_contract() {
  for (int k = 0; k < N_SINKS; ++k) seed_region(k, {});
  reset();
  go();
  CHECK(run_until([&] { return restore_done(); }, 5000),
        "X0 the walk that puts the manager in RUN completed");
  CHECK(d->dbg_dirty_o == 0, "X0b nothing unflushed before the change");

  // X1: a change is TAKEN -> exactly that sink reads unflushed
  Bind x3{true, true, false, 9, TK_A, CTL2};
  const size_t wr_before = size_t(count_ops(OP_WRITE, REC_BASE + 3));
  inject(3, x3, 0x31);
  CHECK(run_until([&] { return d->dbg_dirty_o != 0; }, 20),
        "X1 the accepted change raises the unflushed vector");
  CHECK(d->dbg_dirty_o == (1u << 3),
        "X1b only the changed sink reads unflushed (0x%02x)",
        unsigned(d->dbg_dirty_o));

  // X2: it HOLDS through the debounce and the burst, and only the commit's
  //     done clears it. Sampled every cycle, so a pin that pulsed, or that
  //     cleared when the burst started, fails here and not by luck of when
  //     the other groups happen to look.
  bool on_done = false;
  bool cleared = false;
  for (int i = 0; i < 4000 && !cleared; ++i) {
    //! the manager-face completion AS THIS CYCLE PRESENTS IT: the bit the
    //! shadow clears on it only reads 0 after the edge below
    const bool done_this_cycle = d->dbg_port_done_o != 0;
    tick();
    if (!(d->dbg_dirty_o & (1u << 3))) {
      cleared = true;
      on_done = done_this_cycle;
    }
  }
  CHECK(cleared && size_t(count_ops(OP_WRITE, REC_BASE + 3)) > wr_before,
        "X2 the change was committed and stopped reading unflushed");
  CHECK(on_done,
        "X3 the unflushed bit falls on the commit's done, not before it");
  CHECK(store_match(3, frame(uint8_t(REC_BASE + 3), payload_of(x3))),
        "X3b the record that cleared the bit is byte-exact");

  // X4: retries exhausted -> the bit drops, and NEVER without the alarm.
  //     The two are graded on the same cycle: a pin that cleared on the
  //     give-up while the alarm was still low would lose the change with
  //     nothing left to report it.
  Bind x5{true, false, true, 10, TK_B, CTL1};
  arm_err(OP_WRITE, REC_BASE + 5, -1, -1, 100);   // fail every WRITE
  inject(5, x5, 0x32);
  CHECK(run_until([&] { return (d->dbg_dirty_o >> 5) & 1; }, 20),
        "X4 the change that will be given up reads unflushed first");
  bool alarm_at_drop = false;
  bool dropped = false;
  for (int i = 0; i < 8000 && !dropped; ++i) {
    tick();
    if (!((d->dbg_dirty_o >> 5) & 1)) {
      dropped = true;
      alarm_at_drop = d->alarm_o != 0;
    }
  }
  CHECK(dropped, "X5 the given-up change stops reading unflushed");
  CHECK(alarm_at_drop,
        "X5b the give-up raises alarm_o on the cycle it clears the bit");
  disarm_err();
}

// ======================= L: the boot window and the listener's admission
// Issue #92: during the binding walk, a read-only GET_RX_STATE the listener
// served ended in a record write-back the shadow took as a live change, so
// the restored binding of a sink stored and not yet preloaded was withdrawn
// and the listener's unbound record flushed over the saved one. Every case
// boots on the bindings of the FIRST and the LAST sink and grades what the
// listener took and answered, the preloads, the release, and what the
// device holds at the end; the reset round trips boot again on that.
constexpr uint64_t L_CTLR    = 0x00CCCCCCCCCC00CCull;
constexpr uint64_t L_TKC     = 0x00BB00BB00BB0001ull;
constexpr uint16_t L_TKC_UID = 0x0B01;
// KL_acmp_nvm_shadow's engine states the triggers key on (its hstate_e)
constexpr unsigned M_RS_STORE = 4;
constexpr unsigned M_RP_DRIVE = 7;
// KL_pp_acmp_listener states admissible while the gate owns its faces:
// X_INIT, X_IDLE, X_PRELOAD (its xstate_e codes 0, 1, 2)
constexpr uint32_t OWNED_STATES = 0x7u;
const Bind L_B0{true, true, false, 0x0A01, 0x00A1A1A1A1A10001ull,
                0x00C0C0C0C0C00001ull};
const Bind L_B7{true, false, true, 0x0B02, 0x00A2A2A2A2A20002ull,
                0x00C0C0C0C0C00002ull};
constexpr int L_LAST = N_SINKS - 1;

void Harness::l_seed(const std::vector<std::pair<int, Bind>>& s) {
  for (int k = 0; k < N_SINKS; ++k) {
    seed_region(k, {});
    l_saved[k] = Bind{};
  }
  for (const auto& e : s) {
    seed_region(e.first, frame(uint8_t(REC_BASE + e.first), payload_of(e.second)));
    l_saved[e.first] = e.second;
  }
}

void Harness::l_boot(bool keep_producers) {
  reset(keep_producers);
  go();
}

void Harness::l_push(uint8_t msg, int sink, uint16_t seq, long at,
                     uint64_t tk, uint16_t tkuid, uint16_t flags) {
  Req r;
  r.msg = msg; r.sink = sink; r.ctlr = L_CTLR; r.tk = tk; r.tkuid = tkuid;
  r.seq = seq; r.flags = flags; r.at = at;
  txq.push_back(r);
}

// run the boot out: the walk, the release, every held and queued request,
// their replies and any flush they cause, then prove quiescence
void Harness::l_finish() {
  run_until([&] {
    bool tk_quiet = true;
    for (const Tk& t : tkq) tk_quiet = tk_quiet && t.level;
    return rel_cyc >= 0 && txq.empty() && strq.empty() && tk_quiet
        && hooks.empty() && !d->lsn_busy_o;
  }, 40000);
  run(100);
  run_until([&] { return d->dbg_dirty_o == 0 && d_st == 0 && !d_busy; }, 8000);
  run(DEB_TICKS * 3);
}

std::vector<Resp> Harness::l_resps(uint8_t msg) const {
  std::vector<Resp> v;
  for (const Resp& r : resps)
    if (r.b.size() == size_t(PDU_BYTES) && (r.b[1] & 0x0F) == msg) v.push_back(r);
  return v;
}

bool Harness::l_walk_ok(std::string& why) {
  char b[160];
  why.clear();
  auto bad = [&](const char* s) {
    if (!why.empty()) why += "; ";
    why += s;
  };
  if (!d->restore_done_o || d->restore_fail_o) bad("the walk is not done, or failed");
  std::vector<int> want;
  for (int k = 0; k < N_SINKS; ++k)
    if (l_saved[k].valid) want.push_back(k);
  if (accepts.size() != want.size()) {
    snprintf(b, sizeof b, "%zu preloads taken, %zu saved", accepts.size(), want.size());
    bad(b);
  } else {
    for (size_t i = 0; i < want.size(); ++i)
      if (accepts[i].sink != want[i] || !(accepts[i].b == l_saved[want[i]])) {
        snprintf(b, sizeof b, "preload %zu is not sink %d as saved", i, want[i]);
        bad(b);
      }
  }
  if (lsn_pre_recs.size() != want.size()) {
    snprintf(b, sizeof b, "%zu preload records written", lsn_pre_recs.size());
    bad(b);
  } else {
    for (size_t i = 0; i < want.size(); ++i)
      if (lsn_pre_recs[i].sink != want[i] || !(lsn_pre_recs[i].b == l_saved[want[i]]))
        bad("a preload record is not written bound as saved");
  }
  if (pre_wait_max != 0) {
    snprintf(b, sizeof b, "an offer waited %ld cycles untaken", pre_wait_max);
    bad(b);
  }
  if (rel_cyc < 0 || done_cyc < 0) {
    bad("no release");
  } else {
    size_t arms_before = 0;
    for (long c : arm_cyc) arms_before += (c < rel_cyc);
    bool wr_before = pre_wr_cyc.size() == want.size();
    for (long c : pre_wr_cyc) wr_before = wr_before && (c < rel_cyc);
    if (!wr_before || arms_before != want.size())
      bad("the release precedes a preload's record write or discovery arm");
    if (rel_cyc < done_cyc || rel_cyc - done_cyc > 4) {
      snprintf(b, sizeof b, "released %ld cycles after the terminal", rel_cyc - done_cyc);
      bad(b);
    }
  }
  if (own_states & ~OWNED_STATES) {
    snprintf(b, sizeof b, "listener states 0x%x while owned", own_states);
    bad(b);
  }
  if (own_effects || own_takes) {
    snprintf(b, sizeof b, "%d side effects, %d takes while owned", own_effects, own_takes);
    bad(b);
  }
  if (d->dbg_touched_o) {
    snprintf(b, sizeof b, "touched 0x%02x", unsigned(d->dbg_touched_o));
    bad(b);
  }
  return why.empty();
}

bool Harness::l_nvm_is(int sink, const Bind& b) const {
  return store_match(sink, frame(uint8_t(REC_BASE + sink), payload_of(b)));
}

bool Harness::l_nvm_untouched(std::string& why) const {
  char b[200];
  why.clear();
  if (count_ops(OP_WRITE) || count_ops(OP_ERASE)) {
    snprintf(b, sizeof b, "%d writes, %d erases", count_ops(OP_WRITE),
             count_ops(OP_ERASE));
    why = b;
  }
  for (int k = 0; k < N_SINKS; ++k) {
    if (l_saved[k].valid && !l_nvm_is(k, l_saved[k])) {
      std::string hex;
      for (int i = 8; i < 20; ++i) {
        snprintf(b, sizeof b, "%02x", store[k][i]);
        hex += b;
      }
      snprintf(b, sizeof b, "%ssink %d holds %s", why.empty() ? "" : "; ", k,
               hex.c_str());
      why += b;
    }
  }
  return why.empty();
}

// a reset, then the next boot's walk: what comes back is what NVM holds
bool Harness::l_round_trip(const Bind (&want)[N_SINKS], std::string& why) {
  char b[120];
  why.clear();
  reset();
  go();
  run_until([&] {
    return d->restore_done_o && !d->pre_valid_o && !d->lsn_busy_o
        && !d->lsn_disc_arm_o;
  }, 20000);
  run(10);
  size_t n = 0;
  for (int k = 0; k < N_SINKS; ++k) {
    if (!want[k].valid) continue;
    bool found = false;
    for (const Accept& a : accepts)
      if (a.sink == k && a.b == want[k]) found = true;
    if (!found) {
      snprintf(b, sizeof b, "%ssink %d not restored as saved", why.empty() ? "" : "; ", k);
      why += b;
    }
    ++n;
  }
  if (accepts.size() != n) {
    snprintf(b, sizeof b, "%s%zu preloads, want %zu", why.empty() ? "" : "; ",
             accepts.size(), n);
    why += b;
  }
  return why.empty();
}

void Harness::l_grade(const char* tag, int n_txn, bool nvm_untouched) {
  std::string why;
  CHECK(hooks.empty(), "%s: the case's trigger fired", tag);
  CHECK(l_walk_ok(why), "%s: walk, preloads, release and ownership: %s", tag,
        why.c_str());
  bool after = true;
  for (long c : txn_takes) after = after && rel_cyc >= 0 && c >= rel_cyc;
  CHECK(txn_mismatch == 0 && txn_pops.size() == size_t(n_txn)
            && txn_takes.size() == size_t(n_txn) && after,
        "%s: each of %d requests popped exactly when the listener took it, "
        "at or after the release (pops %zu, takes %zu, %d cycles disagreed)",
        tag, n_txn, txn_pops.size(), txn_takes.size(), txn_mismatch);
  bool tk_after = true;
  for (long c : tk_takes) tk_after = tk_after && rel_cyc >= 0 && c >= rel_cyc;
  CHECK(tk_mismatch == 0 && tk_after,
        "%s: every talker event acknowledged exactly when the listener took it, "
        "at or after the release (%zu takes, %d cycles disagreed)", tag,
        tk_takes.size(), tk_mismatch);
  if (nvm_untouched)
    CHECK(l_nvm_untouched(why),
          "%s: the saved bindings are still in NVM, nothing written: %s", tag,
          why.c_str());
}

void Harness::check_l00_restore_control() {
  l_seed({{0, L_B0}, {L_LAST, L_B7}});
  l_boot();
  l_finish();
  l_push(M_GETRX_CMD, 0, 0x100, cycles);
  l_push(M_GETRX_CMD, L_LAST, 0x101, cycles);
  l_finish();
  l_grade("L00 control, no traffic in the window", 2, true);
  const auto g = l_resps(M_GETRX_RSP);
  CHECK(g.size() == 2 && g[0].b == getrx_bound(0, L_B0, L_CTLR, 0x100)
            && g[1].b == getrx_bound(L_LAST, L_B7, L_CTLR, 0x101),
        "L00: the listener answers both restored bindings after the release");
  std::string why;
  CHECK(l_round_trip(l_saved, why), "L00: a reset restores the same bindings: %s",
        why.c_str());
}

void Harness::check_l05_read_only_in_the_window() {
  struct C {
    const char* tag;
    unsigned state;      // the manager state the GETs are presented in
    int sink;            // ...for this sink (restore cursor or preload sink)
    std::vector<int> gets;
  };
  const std::vector<C> cs = {
    {"L05a first sink's GET between its store and its preload", M_RS_STORE, 0, {0}},
    {"L05b later sink's GET between its store and its preload", M_RS_STORE, L_LAST, {L_LAST}},
    {"L05c first sink's GET at the later sink's store", M_RS_STORE, L_LAST, {0}},
    {"L05d later sink's GET at the first sink's store", M_RS_STORE, 0, {L_LAST}},
    {"L05e consecutive GETs of both sinks", M_RS_STORE, 0, {0, 0, L_LAST, L_LAST, 0}},
    {"L05f later sink's GET as the first sink's preload is offered", M_RP_DRIVE, 0, {L_LAST}},
  };
  for (const C& c : cs) {
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    l_boot();
    hooks.push_back([this, c] {
      const int at = (c.state == M_RS_STORE) ? int(d->mgr_rs_sink_o)
                                             : int(d->pre_sink_o);
      if (d->mgr_state_o != c.state || at != c.sink) return false;
      for (size_t i = 0; i < c.gets.size(); ++i)
        l_push(M_GETRX_CMD, c.gets[i], uint16_t(0x500 + i), cycles);
      return true;
    });
    l_finish();
    l_grade(c.tag, int(c.gets.size()), true);
    const auto g = l_resps(M_GETRX_RSP);
    bool exact = g.size() == c.gets.size();
    for (size_t i = 0; exact && i < g.size(); ++i)
      exact = g[i].b == getrx_bound(c.gets[i], l_saved[c.gets[i]], L_CTLR,
                                    uint16_t(0x500 + i));
    CHECK(exact, "%s: every reply reports the restored binding (%zu replies)",
          c.tag, g.size());
    std::string why;
    CHECK(l_round_trip(l_saved, why), "%s: a reset restores the same bindings: %s",
          c.tag, why.c_str());
  }
}

// every cycle of the window, one boot each: from the reset's release,
// through restore_go, the whole walk and its release, a single GET of the
// first or the last sink is presented at that cycle
void Harness::check_l05_anywhere_in_the_window() {
  l_seed({{0, L_B0}, {L_LAST, L_B7}});
  reset();
  const long t0 = cycles;
  run(3);
  go();
  run_until([&] { return rel_cyc >= 0 && done_cyc >= 0; }, 20000);
  const long span = std::max(rel_cyc, done_cyc) - t0 + 3;
  CHECK(span > 20 && span < 2000, "L05s: the window measured (%ld cycles)", span);
  for (int sink : {0, L_LAST}) {
    int bad_walk = 0, bad_take = 0, bad_reply = 0, bad_nvm = 0;
    long first_walk = -1, first_take = -1, first_reply = -1, first_nvm = -1;
    std::string why_walk, why_nvm;
    for (long off = 0; off <= span; ++off) {
      l_seed({{0, L_B0}, {L_LAST, L_B7}});
      reset();
      const long at = cycles + off;
      hooks.push_back([this, at, sink] {
        if (cycles < at) return false;
        l_push(M_GETRX_CMD, sink, 0x600, cycles);
        return true;
      });
      run(3);
      go();
      l_finish();
      std::string why;
      if (!l_walk_ok(why)) {
        if (!bad_walk++) { first_walk = off; why_walk = why; }
      }
      bool after = txn_takes.size() == 1 && txn_pops.size() == 1
                && txn_mismatch == 0 && txn_takes[0] >= rel_cyc;
      if (!after && !bad_take++) first_take = off;
      const auto g = l_resps(M_GETRX_RSP);
      if (!(g.size() == 1 && g[0].b == getrx_bound(sink, l_saved[sink], L_CTLR, 0x600))
          && !bad_reply++) first_reply = off;
      if (!l_nvm_untouched(why) && !bad_nvm++) { first_nvm = off; why_nvm = why; }
    }
    CHECK(bad_walk == 0, "L05s sink %d: %d presentation cycles broke the walk "
          "(first at +%ld: %s)", sink, bad_walk, first_walk, why_walk.c_str());
    CHECK(bad_take == 0, "L05s sink %d: %d presentation cycles were not taken "
          "once, after the release (first at +%ld)", sink, bad_take, first_take);
    CHECK(bad_reply == 0, "L05s sink %d: %d presentation cycles answered other "
          "than the restored binding (first at +%ld)", sink, bad_reply, first_reply);
    CHECK(bad_nvm == 0, "L05s sink %d: %d presentation cycles changed NVM "
          "(first at +%ld: %s)", sink, bad_nvm, first_nvm, why_nvm.c_str());
  }
}

void Harness::check_l09_live_changes_stay_ordered() {
  // the new binding a BIND_RX of the first sink to another talker leaves
  // behind: no STREAMING_WAIT, so it lands started (IEEE 7.4.35)
  const Bind nb{true, true, false, L_TKC_UID, L_TKC, L_CTLR};
  const std::vector<uint8_t> bind_rsp =
      acmpdu(M_BIND_RX_RSP, 0, L_CTLR, L_TKC, L_TKC_UID, 0, 1, 0x901, 0);
  std::string why;
  {
    const char* tag = "L09 BIND of the first sink to another talker, held from reset";
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    l_push(M_BIND_RX_CMD, 0, 0x901, 0, L_TKC, L_TKC_UID, 0);
    go();
    l_finish();
    l_push(M_GETRX_CMD, 0, 0x902, cycles);
    l_finish();
    l_grade(tag, 2, false);
    const auto br = l_resps(M_BIND_RX_RSP);
    const auto g = l_resps(M_GETRX_RSP);
    CHECK(br.size() == 1 && br[0].b == bind_rsp && g.size() == 1
              && g[0].b == getrx_bound(0, nb, L_CTLR, 0x902),
          "%s: answered SUCCESS after the restore, and the sink reports it", tag);
    CHECK(l_nvm_is(0, nb) && l_nvm_is(L_LAST, L_B7)
              && count_ops(OP_WRITE, REC_BASE + 0) == 1
              && count_ops(OP_WRITE, REC_BASE + L_LAST) == 0,
          "%s: the live change wins in NVM by coming later, once", tag);
    Bind want[N_SINKS];
    want[0] = nb;
    want[L_LAST] = L_B7;
    CHECK(l_round_trip(want, why), "%s: a reset restores the new binding: %s", tag,
          why.c_str());
  }
  {
    const char* tag = "L09u UNBIND of the later sink at the first sink's store";
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    l_boot();
    hooks.push_back([this] {
      if (d->mgr_state_o != M_RS_STORE || d->mgr_rs_sink_o != 0) return false;
      l_push(M_UNBIND_CMD, L_LAST, 0x903, cycles);
      return true;
    });
    l_finish();
    l_grade(tag, 1, false);
    const auto u = l_resps(M_UNBIND_RSP);
    CHECK(u.size() == 1
              && u[0].b == acmpdu(M_UNBIND_RSP, 0, L_CTLR, 0, 0, uint16_t(L_LAST),
                                  0, 0x903, 0),
          "%s: answered SUCCESS after the restore", tag);
    CHECK(l_nvm_is(L_LAST, Bind{}) && l_nvm_is(0, L_B0)
              && count_ops(OP_WRITE, REC_BASE + 0) == 0,
          "%s: the saved unbind replaces the restored binding, sink 0 untouched", tag);
    Bind want[N_SINKS];
    want[0] = L_B0;
    CHECK(l_round_trip(want, why), "%s: a reset restores only the first sink: %s",
          tag, why.c_str());
  }
  {
    const char* tag = "L09q a queue of reads and changes at the first sink's store";
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    l_boot();
    hooks.push_back([this] {
      if (d->mgr_state_o != M_RS_STORE || d->mgr_rs_sink_o != 0) return false;
      l_push(M_GETRX_CMD, 0, 0x911, cycles);
      l_push(M_BIND_RX_CMD, 0, 0x912, cycles, L_TKC, L_TKC_UID, 0);
      l_push(M_GETRX_CMD, 0, 0x913, cycles);
      l_push(M_UNBIND_CMD, L_LAST, 0x914, cycles);
      l_push(M_GETRX_CMD, L_LAST, 0x915, cycles);
      return true;
    });
    l_finish();
    l_grade(tag, 5, false);
    std::vector<std::vector<uint8_t>> want_r = {
      getrx_bound(0, L_B0, L_CTLR, 0x911),
      acmpdu(M_BIND_RX_RSP, 0, L_CTLR, L_TKC, L_TKC_UID, 0, 1, 0x912, 0),
      getrx_bound(0, nb, L_CTLR, 0x913),
      acmpdu(M_UNBIND_RSP, 0, L_CTLR, 0, 0, uint16_t(L_LAST), 0, 0x914, 0),
      acmpdu(M_GETRX_RSP, 0, L_CTLR, 0, 0, uint16_t(L_LAST), 0, 0x915, 0),
    };
    std::vector<std::vector<uint8_t>> got;
    for (const Resp& r : resps)
      if ((r.b[1] & 0x0F) != M_PROBE_TX_CMD) got.push_back(r.b);
    CHECK(got == want_r,
          "%s: five replies in queue order, each on the state its predecessor "
          "left (%zu replies)", tag, got.size());
    CHECK(l_nvm_is(0, nb) && l_nvm_is(L_LAST, Bind{}),
          "%s: NVM ends at the queue's last word for both sinks", tag);
    Bind want[N_SINKS];
    want[0] = nb;
    CHECK(l_round_trip(want, why), "%s: a reset restores what the queue left: %s",
          tag, why.c_str());
  }
}

// a request presented in the gate's last owned cycle (m1), its first
// released one (z) and the one after (p1): taken once, after the release.
// The boot is deterministic, so a control boot measures where the release
// lands and the three presentations are placed against that cycle, not
// against a copy of the gate's own release condition.
void Harness::check_l10_release_boundary() {
  l_seed({{0, L_B0}, {L_LAST, L_B7}});
  reset();
  long t0 = cycles;
  go();
  run_until([&] { return rel_cyc >= 0; }, 20000);
  const long rel_off = rel_cyc - t0;
  static const char* const tags[] = {
    "L10m1 GET presented in the last owned cycle",
    "L10z GET presented in the first released cycle",
    "L10p1 GET presented the cycle after the release"};
  for (int v = 0; v < 3; ++v) {
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    t0 = cycles;
    const long at = t0 + rel_off - 1 + v;
    auto owned_at = std::make_shared<int>(-1);
    hooks.push_back([this, at, v, owned_at] {
      if (cycles < at) return false;
      *owned_at = d->gate_own_o;
      l_push(M_GETRX_CMD, 0, uint16_t(0xA00 + v), cycles);
      return true;
    });
    go();
    l_finish();
    l_grade(tags[v], 1, true);
    CHECK(*owned_at == (v == 0 ? 1 : 0) && rel_cyc == t0 + rel_off
              && txn_takes.size() == 1 && txn_takes[0] == std::max(at, rel_cyc),
          "%s: presented %s, taken in cycle %ld (release %ld)", tags[v],
          v == 0 ? "while owned" : "released",
          txn_takes.empty() ? -1L : txn_takes[0] - t0, rel_off);
    const auto g = l_resps(M_GETRX_RSP);
    CHECK(g.size() == 1 && g[0].b == getrx_bound(0, L_B0, L_CTLR, uint16_t(0xA00 + v)),
          "%s: answered with the restored binding", tags[v]);
  }
}

void Harness::check_l_reset_boundaries() {
  std::string why;
  {
    // a GET held in the window, and the entity reset under it before the
    // release: a producer that keeps presenting it gets it taken once, after
    // the NEXT walk's release
    const char* tag = "L20 a held GET across a reset inside the window";
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    l_boot();
    hooks.push_back([this] {
      if (d->mgr_state_o != M_RS_STORE || d->mgr_rs_sink_o != 0) return false;
      l_push(M_GETRX_CMD, 0, 0xB00, cycles);
      return true;
    });
    run_until([&] { return accepts.size() == 1; }, 20000);
    CHECK(accepts.size() == 1 && txn_pops.empty() && txq.size() == 1
              && count_ops(OP_WRITE) == 0 && count_ops(OP_ERASE) == 0,
          "%s: held, not taken, nothing written when the reset lands", tag);
    l_boot(true);
    l_finish();
    l_grade(tag, 1, true);
    const auto g = l_resps(M_GETRX_RSP);
    CHECK(g.size() == 1 && g[0].b == getrx_bound(0, L_B0, L_CTLR, 0xB00),
          "%s: answered once, with the restored binding", tag);
  }
  {
    // the power cut in the preload phase: after the first sink's preload was
    // taken, written and armed, before the last sink's
    const char* tag = "L21 a power cut inside the preload phase";
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    l_boot();
    run_until([&] { return pre_wr_cyc.size() == 1 && !arm_cyc.empty(); }, 20000);
    CHECK(accepts.size() == 1 && d->gate_own_o && count_ops(OP_WRITE) == 0
              && count_ops(OP_ERASE) == 0,
          "%s: cut inside the phase with nothing written", tag);
    l_boot();
    l_finish();
    l_grade(tag, 0, true);
  }
  {
    // a BIND held from reset, reset again before its release: the live
    // change still lands once, after the second walk, and persists
    const char* tag = "L22 a held BIND across a reset inside the window";
    const Bind nb{true, true, false, L_TKC_UID, L_TKC, L_CTLR};
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    l_push(M_BIND_RX_CMD, 0, 0xB02, 0, L_TKC, L_TKC_UID, 0);
    go();
    run_until([&] { return accepts.size() == 1; }, 20000);
    CHECK(txn_pops.empty() && txq.size() == 1, "%s: held when the reset lands", tag);
    l_boot(true);
    l_finish();
    l_grade(tag, 1, false);
    CHECK(l_resps(M_BIND_RX_RSP).size() == 1 && l_nvm_is(0, nb)
              && l_nvm_is(L_LAST, L_B7),
          "%s: answered once and persisted after the second walk", tag);
    Bind want[N_SINKS];
    want[0] = nb;
    want[L_LAST] = L_B7;
    CHECK(l_round_trip(want, why), "%s: a reset restores the new binding: %s", tag,
          why.c_str());
  }
}

// ---- issue #93 S4: every other work face, from reset and against the later
// sink. The talker event the router holds as a LEVEL is the one R217 R3-F1
// found holding the preload phase for ever: a droppable event (a sink index
// out of range) the listener acknowledges and drops, re-raised at once.
constexpr int L_DROP_SINK = 0xFFFF;

void Harness::check_l01_talker_events() {
  struct C {
    const char* tag;
    bool after_first;    // raised the cycle after the first sink's preload
    Tk ev;
    bool level_until;    // a finite level: ends 2000 cycles after the go
  };
  const std::vector<C> cs = {
    {"L01 a talker-event level held from reset, for ever", false,
     Tk{0, false, L_DROP_SINK, 0, 0, true}, false},
    {"L02 a finite talker-event level from reset", false,
     Tk{0, false, L_DROP_SINK, 0, 0, true}, true},
    {"L03 a talker-event level against the later sink", true,
     Tk{0, false, L_DROP_SINK, 0, 0, true}, false},
    {"L03b EVT_TK_DISCOVERED of the first sink after its preload", true,
     Tk{0, false, 0, 0, 0, false}, false},
  };
  for (const C& c : cs) {
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    if (c.after_first) {
      hooks.push_back([this, c] {
        if (pre_take_cyc.empty()) return false;
        Tk t = c.ev;
        t.at = cycles;
        tkq.push_back(t);
        return true;
      });
    } else {
      Tk t = c.ev;
      if (c.level_until) t.until = cycles + 2000;
      tkq.push_back(t);
    }
    go();
    l_finish();
    l_push(M_GETRX_CMD, 0, 0xC00, cycles);
    l_push(M_GETRX_CMD, L_LAST, 0xC01, cycles);
    l_finish();
    l_grade(c.tag, 2, true);
    const auto g = l_resps(M_GETRX_RSP);
    const bool disc = !c.ev.level;
    // the discovered event arms the first sink's T-ACMP-DELAY (A12): the
    // record moves to PRB_W_DELAY and the answer still reports the binding
    CHECK(g.size() == 2 && g[0].b == getrx_bound(0, L_B0, L_CTLR, 0xC00)
              && g[1].b == getrx_bound(L_LAST, L_B7, L_CTLR, 0xC01),
          "%s: the listener serves commands after the release and both "
          "restored bindings answer", c.tag);
    if (disc)
      CHECK(tk_takes.size() == 1 && tk_pops.size() == 1,
            "%s: the queued event is taken exactly once (%zu)", c.tag,
            tk_takes.size());
    else
      CHECK(!tk_takes.empty(), "%s: the level flows once released (%zu takes)",
            c.tag, tk_takes.size());
  }
}

void Harness::check_l04_polled_reads() {
  // a controller polling GET_RX_STATE of the first sink from reset: every
  // answer queued behind the held head comes after the release, in order
  const char* tag = "L04 GET_RX_STATE polled from reset";
  l_seed({{0, L_B0}, {L_LAST, L_B7}});
  reset();
  for (int i = 0; i < 4; ++i) l_push(M_GETRX_CMD, i & 1 ? L_LAST : 0, uint16_t(0xD00 + i), 0);
  go();
  l_finish();
  l_grade(tag, 4, true);
  const auto g = l_resps(M_GETRX_RSP);
  bool exact = g.size() == 4;
  for (int i = 0; exact && i < 4; ++i)
    exact = g[size_t(i)].b == getrx_bound(i & 1 ? L_LAST : 0, i & 1 ? L_B7 : L_B0,
                                          L_CTLR, uint16_t(0xD00 + i));
  CHECK(exact, "%s: four answers in order, each the restored binding", tag);
}

void Harness::check_l06_start_stop() {
  struct C {
    const char* tag;
    std::vector<Strq> reqs;   // presented from reset unless `later`
    bool later;               // raised the cycle after the first preload
    Bind b0, b7;              // what each sink must end at, NVM and restored
  };
  Bind b0s = L_B0; b0s.started = false;        // L_B0 stopped
  Bind b7s = L_B7; b7s.started = true;         // L_B7 started
  const std::vector<C> cs = {
    {"L06 STOP of the first sink held from reset", {{0, false, 0}}, false, b0s, L_B7},
    {"L06b START then STOP of the first sink from reset",
     {{0, true, 0}, {0, false, 0}}, false, b0s, L_B7},
    {"L06c START of the later sink against its preload", {{L_LAST, true, 0}}, true,
     L_B0, b7s},
  };
  for (const C& c : cs) {
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    if (c.later) {
      hooks.push_back([this, c] {
        if (pre_take_cyc.empty()) return false;
        for (Strq q : c.reqs) { q.at = cycles; strq.push_back(q); }
        return true;
      });
    } else {
      for (const Strq& q : c.reqs) strq.push_back(q);
    }
    go();
    l_finish();
    l_grade(c.tag, 0, false);
    bool ok = strq_done.size() == c.reqs.size();
    for (size_t i = 0; ok && i < c.reqs.size(); ++i)
      ok = !strq_done[i].err && strq_done[i].cyc >= rel_cyc
        && strq_done[i].sink == c.reqs[i].sink && strq_done[i].val == c.reqs[i].val;
    CHECK(ok, "%s: each request completes once, in order, after the release "
          "(%zu completions)", c.tag, strq_done.size());
    CHECK(l_nvm_is(0, c.b0) && l_nvm_is(L_LAST, c.b7),
          "%s: the started state the requests left is what NVM holds", c.tag);
    Bind want[N_SINKS];
    want[0] = c.b0;
    want[L_LAST] = c.b7;
    std::string why;
    CHECK(l_round_trip(want, why), "%s: a reset restores it: %s", c.tag, why.c_str());
  }
}

void Harness::check_l07_expiries() {
  struct C {
    const char* tag;
    unsigned owner;
    bool later;
  };
  const std::vector<C> cs = {
    {"L07 an expiry of the first sink's owner every cycle from reset", 32, false},
    {"L07b an expiry of the later sink's owner every cycle against its preload",
     32 + L_LAST, true},
  };
  for (const C& c : cs) {
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    exp_owner = c.owner;
    exp_every = 1;
    exp_from = cycles;
    exp_until = cycles + 20000;
    if (c.later) {
      exp_every = 0;
      hooks.push_back([this] {
        if (pre_take_cyc.empty()) return false;
        exp_every = 1;
        exp_from = cycles;
        return true;
      });
    }
    go();
    run_until([&] { return rel_cyc >= 0; }, 20000);
    const long rel = rel_cyc;
    exp_until = std::min(exp_until, rel + 50);   // a burst past the release, then quiet
    l_finish();
    l_grade(c.tag, 0, true);
    CHECK(exp_owned > 0 && d->gate_exp_drop_o == exp_owned && exp_after > 0,
          "%s: every expiry while owned refused and counted (%ld presented, "
          "count %u), %ld reach the listener after the release", c.tag, exp_owned,
          unsigned(d->gate_exp_drop_o), exp_after);
    exp_every = 0;
  }
}

// ======================= N: the walk's cause and its deadline (issue #93)
// S1: a read that ends with nothing forwarded is an empty record only when
// the port says the device answered with something that is not a record
// (UNFRAMED); a DEVICE error there fails the WHOLE walk, as a tear does.
// S3: the read phase's waits on the port are bounded by RS_TMO_CYC_P cycles
// without progress; an expiry fails the walk and abandons an issued read to
// the arbiter, which drains it. Each failed walk still ends, the listener is
// released within four cycles and answers on defaults, and nothing is
// written.
constexpr long RS_TMO = 3000;      // -GRS_TMO_CYC_P

bool Harness::n_failed_walk(unsigned cause, std::string& why) {
  char b[200];
  why.clear();
  auto bad = [&](const char* t) { if (!why.empty()) why += "; "; why += t; };
  if (!d->restore_done_o || !d->restore_fail_o || d->restore_cause_o != cause) {
    snprintf(b, sizeof b, "done %u fail %u cause %u, want 1 1 %u",
             unsigned(d->restore_done_o), unsigned(d->restore_fail_o),
             unsigned(d->restore_cause_o), cause);
    bad(b);
  }
  if (pre_valid_seen || !accepts.empty() || d->dbg_valid_o != 0)
    bad("a preload was offered, or a restored binding survived");
  if (!d->restore_blank_o) bad("a failed walk reports records it discarded");
  if (rel_cyc < 0 || done_cyc < 0 || rel_cyc - done_cyc > 4) bad("no release within four cycles");
  if (own_effects || own_takes) bad("work while owned");
  if (count_ops(OP_WRITE) || count_ops(OP_ERASE)) bad("NVM was written");
  return why.empty();
}

void Harness::check_n1_device_errors_fail_the_walk() {
  struct C {
    const char* tag;
    int sink;      // the record whose header read fails
    int after;     // err after N header bytes; -1 at once; -2 a short read at 5
  };
  const std::vector<C> cs = {
    {"N1a a device error at the first record's header read", 0, -1},
    {"N1b a device error inside a middle record's header", 3, 3},
    {"N1c the last record's header read ended short", L_LAST, -2},
    {"N1d a device error after the first header, before its done", 0, 8},
  };
  for (const C& c : cs) {
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    if (c.after == -2) {
      short_region = REC_BASE + c.sink;
      short_n = 5;
    } else {
      arm_err(OP_READ, REC_BASE + c.sink, 0, c.after, 1);
    }
    go();
    l_finish();
    std::string why;
    CHECK(n_failed_walk(2, why), "%s: the WHOLE walk fails with cause 2: %s", c.tag,
          why.c_str());
    CHECK(count_ops(OP_READ, REC_BASE + c.sink) == 1
              && (c.sink == L_LAST || count_ops(OP_READ, REC_BASE + c.sink + 1) == 0),
          "%s: the walk stops at the failing record", c.tag);
    disarm_err();
    l_push(M_GETRX_CMD, 0, 0xE00, cycles);
    l_finish();
    const auto g = l_resps(M_GETRX_RSP);
    CHECK(g.size() == 1
              && g[0].b == acmpdu(M_GETRX_RSP, 0, L_CTLR, 0, 0, 0, 0, 0xE00, 0),
          "%s: the listener is released and answers on the vendor default", c.tag);
  }
}

void Harness::check_n2_unframed_keeps_the_default() {
  struct C {
    const char* tag;
    uint8_t fill;          // the middle record's header bytes
    bool magic_only;       // corrupt only the magic of a framed record
  };
  const std::vector<C> cs = {
    {"N2a an erased middle record (eight 0xFF)", 0xFF, false},
    {"N2b a middle record whose magic is corrupt", 0, true},
  };
  for (const C& c : cs) {
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    if (c.magic_only) {
      seed_region(3, frame(uint8_t(REC_BASE + 3), payload_of(L_B0)));
      store[3][0] ^= 0x10;
    } else {
      memset(store[3], c.fill, REG_BYTES);
    }
    l_boot();
    l_finish();
    l_grade(c.tag, 0, true);
    CHECK(!d->restore_fail_o && d->restore_cause_o == 0 && !d->restore_blank_o
              && count_ops(OP_READ, REC_BASE + 3) == 1
              && count_ops(OP_READ, REC_BASE + L_LAST) == 2,
          "%s: the record keeps its default and the walk completes past it", c.tag);
  }
}

void Harness::check_n3_silence_while_reading() {
  // W13: a middle record's header read granted and never answered
  const char* tag = "N3 a record read the device never answers";
  l_seed({{0, L_B0}, {L_LAST, L_B7}});
  reset();
  hold_region = REC_BASE + 3;
  hold_mode = 3;
  go();
  const long t0 = cycles;
  run_until([&] { return rel_cyc >= 0; }, 4 * RS_TMO);
  std::string why;
  CHECK(n_failed_walk(3, why), "%s: the walk fails whole at its deadline with "
        "cause 3: %s", tag, why.c_str());
  CHECK(done_cyc >= 0 && done_cyc - t0 <= RS_TMO + 200 && aborts == 1,
        "%s: within the deadline of the silence (%ld cycles), the read abandoned "
        "once (%d)", tag, done_cyc - t0, aborts);
  // commands are served on defaults; persistence is not
  l_push(M_GETRX_CMD, L_LAST, 0xE10, cycles);
  Bind n2{true, true, false, 0x22, 0x2222222233333333ull, CTL2};
  inject(2, n2, 0x61);
  run(DEB_TICKS * 3 + 4 * RS_TMO);
  const auto g = l_resps(M_GETRX_RSP);
  CHECK(g.size() == 1 && g[0].b == acmpdu(M_GETRX_RSP, 0, L_CTLR, 0, 0,
                                          uint16_t(L_LAST), 0, 0xE10, 0),
        "%s: the listener answers on the vendor default", tag);
  CHECK(d->arb_drain_o && ((d->dbg_dirty_o >> 2) & 1) && count_ops(OP_WRITE) == 0
            && count_ops(OP_ERASE) == 0 && drain_leaks == 0,
        "%s: the port stays quarantined and a later change stays pending, "
        "never written", tag);
}

void Harness::check_n4_silence_before_reading() {
  // the port held by manager 1's read, which the device never answers: the
  // walk waits in H_RS_REQ, issues nothing, and ends at its deadline
  const char* tag = "N4 the port never comes idle for the walk";
  l_seed({{0, L_B0}, {L_LAST, L_B7}});
  reset();
  hold_region = 0x30;
  hold_mode = 3;
  m1_rid = 0x30;
  m1_pend = true;
  run_until([&] { return m1_gnt_cyc >= 0; }, 200);
  go();
  const long t0 = cycles;
  run_until([&] { return rel_cyc >= 0; }, 4 * RS_TMO);
  std::string why;
  CHECK(m1_gnt_cyc >= 0 && n_failed_walk(3, why),
        "%s: the walk fails whole at its deadline with cause 3: %s", tag, why.c_str());
  bool none = true;
  for (int k = 0; k < N_SINKS; ++k) none = none && count_ops(OP_READ, REC_BASE + k) == 0;
  CHECK(none && aborts == 0 && done_cyc - t0 <= RS_TMO + 200,
        "%s: no read of its own issued, nothing to abandon, ended in %ld cycles",
        tag, done_cyc - t0);
}

// The deadline watches the MANAGER's face: the port collects and checks a
// header before it forwards a byte, so the walk's first byte trails the
// device's by a fixed lag. A control boot measures it, and the two cases place
// the device's answer so that the walk's first byte lands in the last cycle
// before the expiry, or in the first cycle after it.
void Harness::check_n5_the_deadline_boundary() {
  l_seed({{0, L_B0}, {L_LAST, L_B7}});
  l_boot();
  run_until([&] { return mgr_first_rd >= 0; }, 2000);
  const long lag = mgr_first_rd - dev_first_rd;
  CHECK(dev_first_rd >= 0 && lag > 0 && lag < 100,
        "N5 control: the walk's first byte trails the device's by %ld cycles", lag);
  {
    // W13b: in time
    const char* tag = "N5a the walk's first byte in the last cycle before the deadline";
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    hold_region = REC_BASE + 0;
    hold_mode = 1;
    hold_n = RS_TMO - 1 - lag;
    go();
    l_finish();
    l_grade(tag, 0, true);
    CHECK(wd_max == RS_TMO - 1 && aborts == 0 && d->restore_cause_o == 0
              && !d->restore_fail_o,
          "%s: the stall count reached %ld of %ld and the walk completed", tag,
          wd_max, RS_TMO);
  }
  {
    // W13c: one cycle late. Drained, nothing preloaded, and the next change
    // persists once the device ended the read
    const char* tag = "N5b the walk's first byte one cycle after the deadline";
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    hold_region = REC_BASE + 0;
    hold_mode = 1;
    hold_n = RS_TMO - lag;
    go();
    run_until([&] { return rel_cyc >= 0; }, 4 * RS_TMO);
    std::string why;
    CHECK(n_failed_walk(3, why) && aborts == 1 && wd_max == RS_TMO - 1,
          "%s: the walk fails whole at its deadline with cause 3 (%d aborts, "
          "stall count %ld): %s", tag, aborts, wd_max, why.c_str());
    Bind n2{true, false, true, 0x23, 0x2323232344444444ull, CTL1};
    inject(2, n2, 0x62);
    run_until([&] { return d->dbg_dirty_o == 0 && first_erase >= 0; }, 8 * RS_TMO);
    run(200);
    CHECK(hold_rel && drain_on == abort_cyc + 1 && drain_bytes > 0
              && mgr_first_rd < 0 && drain_leaks == 0 && !d->arb_drain_o,
          "%s: the late read moved %ld bytes in the drain and none reached the "
          "manager", tag, drain_bytes);
    CHECK(first_erase > drain_off && writes_in_drain == 0,
          "%s: the next operation is issued only after the drained one ended "
          "(drain %ld..%ld, erase at %ld)", tag, drain_on, drain_off, first_erase);
    CHECK(l_nvm_is(2, n2) && l_nvm_is(0, L_B0) && l_nvm_is(L_LAST, L_B7),
          "%s: the later change persists and the saved records stay", tag);
  }
  {
    // A device that ends the abandoned read LONG after the deadline, with a
    // live change waiting for the port the whole time: the change stays
    // pending while the port is quarantined, nothing reaches either manager,
    // and the change is issued only once the device ended the drained read.
    const char* tag = "N5c the abandoned read ended 2000 cycles after the deadline";
    constexpr long LATE = 2000;
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    hold_region = REC_BASE + 0;
    hold_mode = 2;
    hold_n = LATE;
    go();
    run_until([&] { return rel_cyc >= 0; }, 4 * RS_TMO);
    std::string why;
    CHECK(n_failed_walk(3, why) && aborts == 1,
          "%s: the walk fails whole at its deadline with cause 3: %s", tag,
          why.c_str());
    Bind n2{true, true, false, 0x24, 0x2424242455555555ull, CTL2};
    inject(2, n2, 0x63);
    run_until([&] { return hold_rel; }, int(2 * LATE));
    const bool pending = d->arb_drain_o && ((d->dbg_dirty_o >> 2) & 1)
                         && count_ops(OP_WRITE) == 0 && count_ops(OP_ERASE) == 0;
    CHECK(hold_rel && cycles >= abort_cyc + LATE && pending,
          "%s: until the device answers the port stays drained and the change "
          "pending, never written", tag);
    run_until([&] { return d->dbg_dirty_o == 0 && first_erase >= 0; }, 8 * RS_TMO);
    run(200);
    CHECK(drain_bytes > 0 && mgr_first_rd < 0 && drain_leaks == 0
              && drain_off >= abort_cyc + LATE && !d->arb_drain_o,
          "%s: the late read moved %ld bytes in the drain, none reached a "
          "manager, and the drain ended with it", tag, drain_bytes);
    CHECK(first_erase > drain_off && writes_in_drain == 0,
          "%s: the change is issued only after the drained read ended "
          "(drain %ld..%ld, erase at %ld)", tag, drain_on, drain_off, first_erase);
    CHECK(l_nvm_is(2, n2) && l_nvm_is(0, L_B0) && l_nvm_is(L_LAST, L_B7),
          "%s: the change then persists and the saved records stay", tag);
    Bind want[N_SINKS];
    want[0] = L_B0;
    want[2] = n2;
    want[L_LAST] = L_B7;
    CHECK(l_round_trip(want, why), "%s: a reset restores all three: %s", tag,
          why.c_str());
  }
}

// The arbiter in front of the port, with the walk as manager 0 and a second
// record reader as manager 1. The binding manager raises a REGISTERED request
// one cycle after it reads the port idle, so the busy it reads must also
// cover the cycle manager 1 is granted; a manager-0 request landing on that
// grant would be lost and the walk would wait for its data. Manager 1's read
// is presented at every offset from the go to the terminal of a control walk,
// which places its grant on every cycle the walk samples the port: every walk
// completes as saved and every manager-1 read completes.
void Harness::check_n6_a_second_manager_on_the_port() {
  l_seed({{0, L_B0}, {L_LAST, L_B7}});
  reset();
  const long g0 = cycles;
  go();
  run_until([&] { return done_cyc >= 0; }, 20000);
  const int SPAN = int(done_cyc - g0);
  CHECK(SPAN > 100, "N6 control: the walk takes %d cycles from its go", SPAN);
  int bad_walks = 0, bad_m1 = 0, first_bad = -1, contended = 0;
  for (int o = 0; o < SPAN; ++o) {
    l_seed({{0, L_B0}, {L_LAST, L_B7}});
    reset();
    const long at = cycles + o;             // the go is presented at `cycles`
    hooks.push_back([this, at] {
      if (cycles < at) return false;
      m1_rid = 0x30;
      m1_pend = true;
      return true;
    });
    go();
    l_finish();
    std::string why;
    const bool walk = l_walk_ok(why) && !d->restore_fail_o && d->restore_cause_o == 0
                      && l_nvm_untouched(why);
    const bool m1 = m1_gnt_cyc >= 0 && m1_end_cyc > m1_gnt_cyc && !m1_pend;
    if (!walk) { ++bad_walks; if (first_bad < 0) first_bad = o; }
    if (!m1) { ++bad_m1; if (first_bad < 0) first_bad = o; }
    if (m1_gnt_cyc >= 0 && done_cyc >= 0 && m1_gnt_cyc < done_cyc) ++contended;
  }
  CHECK(bad_walks == 0,
        "N6 a second manager's read at each of %d offsets: every walk completes "
        "as saved (%d did not, first at offset %d)", SPAN, bad_walks, first_bad);
  CHECK(bad_m1 == 0 && contended == SPAN,
        "N6 every second-manager read is granted inside the walk and completes "
        "(%d did not, %d of %d inside the walk)", bad_m1, contended, SPAN);
}

int Harness::report() {
  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  return fails ? 1 : 0;
}

int Harness::run_suite() {
  check_empty_boot_reports_blank();
  check_capture_debounce_and_write_through();
  check_volatile_only_churn_costs_no_traffic();
  check_unbind_capture_commits();
  check_bounded_commit_retry_and_the_alarm();
  check_boot_replay_from_a_seeded_image();
  check_a_torn_readback_aborts_the_whole_restore();
  check_a_change_during_restore_wins();
  check_a_change_during_its_own_flush_reserializes();
  check_the_unflushed_export_contract();
  check_l00_restore_control();
  check_l05_read_only_in_the_window();
  check_l05_anywhere_in_the_window();
  check_l09_live_changes_stay_ordered();
  check_l10_release_boundary();
  check_l_reset_boundaries();
  check_l01_talker_events();
  check_l04_polled_reads();
  check_l06_start_stop();
  check_l07_expiries();
  check_n1_device_errors_fail_the_walk();
  check_n2_unframed_keeps_the_default();
  check_n3_silence_while_reading();
  check_n4_silence_before_reading();
  check_n5_the_deadline_boundary();
  check_n6_a_second_manager_on_the_port();
  return report();
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  Harness h;
  return h.run_suite();
}
