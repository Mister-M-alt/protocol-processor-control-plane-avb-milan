// SPDX-License-Identifier: CERN-OHL-W-2.0
// KL_pp_nvm_port's deadline against random devices and managers (issue #15).
//
// The review's randomized harness (R436-2's pause probe, its modes), folded in
// as a standing check: its own device model, its own manager and its own count
// of owed cycles, written from the port's banner rather than from the RTL, and
// independent of sim_main.cpp. The Makefile builds it once per bound in
// FUZZ_TMOS and hands the bound over as NVM_PORT_TMO, so the smallest legal
// bounds, which the suite cannot be built at, are graded here.
//
//   legal  - every event the device owes comes within the bound, biased to the
//            exact bound: a grant at most TMO cycles after the request shows,
//            a byte or a terminal at most TMO cycles after the device's
//            previous event, a terminal riding a final byte or an ERASE's
//            grant, a done for no command where nothing is owned, and now and
//            then an err in place of any one event. The manager drops its
//            strobe at random, periodically, or for up to five deadlines.
//            Never a DEADLINE.
//   silent - the device withholds one event of the operation for ever while
//            the manager drops its strobe: one err, cause DEADLINE, never
//            done, on exactly the (TMO + 1)-th owed cycle since the device's
//            last event, counted here from the banner.
//   resume - a command abandoned by a deadline, which the device ends r cycles
//            into the next request's wait and then keeps legal pace: that
//            request is served if r <= TMO and ends one err DEADLINE if not.
//   babble - a READ abandoned by a deadline at any of its bytes or at its
//            terminal, whose device then presents its bytes one every TMO / 2
//            cycles, past its length for ever, and never ends it: a broken
//            backend. The port takes the bytes the READ still owed and no
//            more, and the request waiting on it ends one err DEADLINE.
//
// Half the records of resume and babble are long, up to 1,024 bytes, and each
// seed's resume also abandons one READ of the largest payload the port
// accepts, owing every byte of it, which the Makefile builds at the
// parameter's largest legal value: what an abandoned READ owes is counted as
// wide as any READ the port can issue.
//
// Each property is ONE named check per build (FZ1 to FZ10), its message
// carrying the counts, so the tally is the same whatever the seeds draw.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "VKL_pp_nvm_port.h"
#include "verilated.h"
#include "../common/verilator_harness.hpp"

namespace {

constexpr int TMO = NVM_PORT_TMO;
constexpr int kRegions = 8;
constexpr int kRegBytes = 65536;                 // a record of any legal length
constexpr int kLongPayload = 1024;               // the top's MAX_PAYLOAD_P: random records stop here
constexpr int kOpRead = 0;
constexpr int kOpWrite = 1;
constexpr int kOpErase = 2;
constexpr long kGuard = 200L * (TMO + 10) * 80;  // cycles to a pulse, past any legal op
constexpr int kOps = 600;                        // operations per mode and seed
constexpr uint64_t kFirstSeed = 11;               // seeds 11 to 13
constexpr uint64_t kLastSeed = 13;

uint16_t crc16(const std::vector<uint8_t>& b) {
  uint16_t c = 0xFFFF;
  for (uint8_t x : b) {
    c ^= uint16_t(uint16_t(x) << 8);
    for (int i = 0; i < 8; ++i)
      c = (c & 0x8000) ? uint16_t(uint16_t(c << 1) ^ 0x1021) : uint16_t(c << 1);
  }
  return c;
}

std::vector<uint8_t> frame(uint8_t rec, const std::vector<uint8_t>& pl) {
  std::vector<uint8_t> f = {0x17, 0x22, 0x01, rec, uint8_t(pl.size() >> 8),
                            uint8_t(pl.size() & 0xFF)};
  std::vector<uint8_t> cb(f);
  cb.insert(cb.end(), pl.begin(), pl.end());
  const uint16_t c = crc16(cb);
  f.push_back(uint8_t(c >> 8));
  f.push_back(uint8_t(c & 0xFF));
  f.insert(f.end(), pl.begin(), pl.end());
  return f;
}

//! The device's events of one operation, in order, each an obligation: a
//! commit's ERASE grant and terminal, its WRITE grant, bytes and terminal; a
//! restore's header grant, eight bytes and terminal, then, for a payload, its
//! grant, bytes and terminal.
int obligations(bool we, size_t frame_bytes) {
  if (we) return 3 + int(frame_bytes) + 1;
  return 10 + (frame_bytes > 8 ? 2 + int(frame_bytes) - 8 : 0);
}

//! What one operation saw, read off the manager face.
struct Outcome {
  int dones = 0;
  int errs = 0;
  int cause = -1;
  bool busy_at_pulse = false;
  long pulse_at = -1;
  long owed_at_pulse = -1;      // owed cycles since the device's last event
};

//! Tallies of one property: how many operations it was graded on, how many
//! broke it, and the first one that did.
struct Tally {
  long seen = 0;
  long bad = 0;
  char first[160] = "";
  void grade(bool ok, const char* what) {
    ++seen;
    if (ok) return;
    if (bad++ == 0) snprintf(first, sizeof first, "%s", what);
  }
};

class PauseFuzz {
 public:
  explicit PauseFuzz(VKL_pp_nvm_port* d) : dut(d) {}
  int run();

 private:
  // ---- the device ----
  uint8_t mem[kRegions][kRegBytes];
  bool cmd = false;             // a command accepted and not ended
  bool dphase = false;          // ...with bytes still to move
  int op = 0;
  int reg = 0;
  int off = 0;
  int len = 0;
  int moved = 0;
  long req_at = -1;
  int req_wait = 0;
  long next_at = 0;             // earliest cycle of the next byte or terminal
  int obl_n = 0;                // obligations met this operation
  int sil_n = -1;               // withhold this obligation for ever
  int err_at = -1;              // an err in place of this obligation
  bool hold_dev = false;        // hold every event until released (resume)
  bool babble = false;          // a READ's bytes past its length, never its end
  int babble_rx = 0;            // ...of which the port took this many
  long stray_block = 0;
  // ---- the manager ----
  int m = 0;                    // 0 idle, 1 commit, 2 restore
  std::vector<uint8_t> w;
  size_t wi = 0;
  std::vector<uint8_t> r;
  int pat = 0;
  double p = 0;
  int per = 1;
  long hold_until = 0;
  // ---- capture ----
  Outcome o;
  long owed_since_evt = 0;
  long cyc = 0;
  std::mt19937_64 rng;
  VKL_pp_nvm_port* const dut;
  Tally answered;               // FZ1
  Tally no_deadline;            // FZ2
  Tally errs_device;            // FZ3
  Tally byte_exact;             // FZ4
  Tally silent_dl;              // FZ5
  Tally silent_exact;           // FZ6
  Tally resume_branch;          // FZ7
  Tally babble_dl;              // FZ9
  long served = 0;
  long refused = 0;
  int widest = 0;               // the largest payload the port accepts, asked of it
  long at_header = 0;           // FZ10: READs babble abandoned at a header byte
  long at_payload = 0;          // ...at a payload byte
  long at_terminal = 0;         // ...at a terminal, every byte moved
  long served_long = 0;         // ...and requests resume served behind a READ owing 256 or more
  long served_widest = 0;       // ...or owing the largest payload the port accepts

  int rnd(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
  double u() { return std::uniform_real_distribution<double>(0, 1)(rng); }
  int pick_wait(int maxw) {     // biased to both edges, the bound above all
    const double x = u();
    if (x < 0.35) return maxw;
    if (x < 0.5) return 0;
    return rnd(0, maxw);
  }
  bool strobe();
  void new_pattern(bool may_hold);
  bool oracle_owed() const;
  void drive_manager();
  void tick();
  void device_after_edge(bool gnt, bool gdone, bool wx, bool rx, bool cdone,
                         bool done, bool err);
  void reset_all();
  void start(bool we, uint8_t rec, const std::vector<uint8_t>& f);
  bool finish(long guard);
  size_t long_payload();
  int largest_legal_length();
  void one_legal(std::vector<std::vector<uint8_t>>& good);
  void one_silent(std::vector<std::vector<uint8_t>>& good);
  void one_resume(std::vector<std::vector<uint8_t>>& good);
  void one_babble(std::vector<std::vector<uint8_t>>& good);
  void one_widest(std::vector<std::vector<uint8_t>>& good);
  void begin_run(uint64_t seed);
  int report();
};

//! The manager's strobe this cycle under its pattern: always; dropped at
//! random; dropped once a period; or dropped at random with an occasional
//! hold of up to five deadlines. A manager handshake is never charged.
bool PauseFuzz::strobe() {
  if (cyc < hold_until) return false;
  switch (pat) {
    case 0: return true;
    case 1: return u() >= p;
    case 2: return (cyc % per) != 0;
    default:
      if (u() < 0.01) { hold_until = cyc + rnd(1, 5 * TMO + 3); return false; }
      return u() >= p;
  }
}

void PauseFuzz::new_pattern(bool may_hold) {
  pat = rnd(0, may_hold ? 3 : 2);
  p = 0.1 + 0.4 * rnd(0, 2);    // each cycle dropped with probability 0.1, 0.5 or 0.9
  per = rnd(2, 2 * TMO + 3);
}

//! The banner's owed cycle, independent of the RTL's: while an operation is
//! in flight the device owes a grant while a request is up, a byte the port
//! presents or is ready for, and the terminal of a command whose bytes are
//! all moved.
bool PauseFuzz::oracle_owed() const {
  if (!dut->nvm_busy_o) return false;
  if (dut->dev_req_o) return true;
  if (!cmd) return false;
  if (dphase) return op == kOpWrite ? bool(dut->dev_wvalid_o) : bool(dut->dev_rready_o);
  return true;
}

void PauseFuzz::drive_manager() {
  const bool s = strobe();
  dut->nvm_wvalid_i = 0;
  dut->nvm_wdata_i = 0;
  dut->nvm_rready_i = 0;
  if (m == 1 && wi < w.size() && s) {
    dut->nvm_wvalid_i = 1;
    dut->nvm_wdata_i = w[wi];
  } else if (m == 2) {
    dut->nvm_rready_i = s;
  }
}

//! One clock: the manager and the device decide on what the port shows,
//! the pulse and the owed count are sampled before the edge, then the edge.
void PauseFuzz::tick() {
  drive_manager();
  dut->dev_gnt_i = 0; dut->dev_done_i = 0; dut->dev_err_i = 0; dut->dev_wready_i = 0;
  dut->dev_rvalid_i = 0; dut->dev_rdata_i = 0; dut->dev_busy_i = cmd;
  dut->clk_i = 0; dut->eval();

  const bool owed_now = oracle_owed();
  const bool this_err = err_at >= 0 && obl_n == err_at;
  const bool quiet = (sil_n >= 0 && obl_n == sil_n) || hold_dev;
  bool gnt = false;
  bool err = false;
  bool done = false;
  bool wr = false;
  bool rv = false;
  bool stray = false;
  if (!cmd) {
    if (dut->dev_req_o) {
      if (req_at < 0) { req_at = cyc; req_wait = pick_wait(TMO); }
      if (!quiet && cyc - req_at >= req_wait) {
        if (this_err) err = true;
        else gnt = true;
      }
    } else {
      req_at = -1;
    }
    // a done that belongs to nobody, never beside a real event
    stray = !gnt && !err && cyc > stray_block && u() < 0.02;
  } else {
    const bool ready = !quiet && cyc >= next_at;
    if (ready && this_err) err = true;
    else if (ready && dphase && op == kOpWrite) wr = true;
    else if (ready && (dphase || babble)) rv = true;
    else if (ready) done = true;
  }
  dut->dev_gnt_i = gnt; dut->dev_err_i = err; dut->dev_wready_i = wr; dut->dev_rvalid_i = rv;
  if (rv) dut->dev_rdata_i = mem[reg][(off + moved) % kRegBytes];
  dut->eval();
  // a terminal riding the final byte or an ERASE's grant, never where the
  // terminal is the withheld or the erring obligation
  const bool term_free = !(sil_n >= 0 && obl_n + 1 == sil_n) && !(err_at >= 0 && obl_n + 1 == err_at)
                         && !hold_dev && !babble;
  const bool wx = wr && dut->dev_wvalid_o;
  const bool rx = rv && dut->dev_rready_o;
  const bool cdone = cmd && dphase && (wx || rx) && moved + 1 == len && term_free && u() < 0.5;
  const bool gdone = gnt && int(dut->dev_op_o) == kOpErase && term_free && u() < 0.4;
  dut->dev_done_i = done || cdone || gdone || stray;
  dut->eval();

  const bool evt = gnt || err || wx || rx || done || cdone || gdone;
  if ((dut->nvm_done_o || dut->nvm_err_o) && o.pulse_at < 0) {
    o.pulse_at = cyc; o.owed_at_pulse = owed_since_evt; o.busy_at_pulse = dut->nvm_busy_o;
  }
  if (dut->nvm_done_o) ++o.dones;
  if (dut->nvm_err_o) { ++o.errs; o.cause = dut->nvm_err_cause_o; }
  owed_since_evt = evt ? 0 : owed_since_evt + (owed_now ? 1 : 0);
  if (m == 1 && dut->nvm_wvalid_i && dut->nvm_wready_o) ++wi;
  if (m == 2 && dut->nvm_rvalid_o && dut->nvm_rready_i) r.push_back(dut->nvm_rdata_o);
  device_after_edge(gnt, gdone, wx, rx, cdone, done, err);
  const bool req_was_up = dut->nvm_req_i;
  dut->clk_i = 1; dut->eval();
  if (req_was_up) dut->nvm_req_i = 0;     // one cycle, sampled at this edge
  ++cyc;
}

//! The device's own state after the edge: a command taken, a byte moved, a
//! command ended.
void PauseFuzz::device_after_edge(bool gnt, bool gdone, bool wx, bool rx, bool cdone,
                                  bool done, bool err) {
  if (gnt) {
    cmd = true; op = dut->dev_op_o; reg = dut->dev_region_o % kRegions;
    off = dut->dev_offset_o; len = dut->dev_len_o; moved = 0; req_at = -1; ++obl_n;
    if (op == kOpErase) { memset(mem[reg], 0xFF, kRegBytes); dphase = false; }
    else dphase = len > 0;
    next_at = cyc + 1 + pick_wait(TMO);
    if (gdone) { cmd = false; ++obl_n; }
    stray_block = cyc + 2;
  }
  if (wx) mem[reg][(off + moved) % kRegBytes] = dut->dev_wdata_o;
  if (wx || rx) {
    ++moved; ++obl_n;
    if (moved == len) { dphase = false; if (cdone) { cmd = false; ++obl_n; } }
    next_at = cyc + 1 + (babble ? TMO / 2 : pick_wait(TMO));
    stray_block = cyc + 2;
    if (babble) ++babble_rx;
  }
  if ((done && cmd && !dphase) || err) {
    cmd = false; dphase = false; ++obl_n; stray_block = cyc + 2;
  }
}

void PauseFuzz::reset_all() {
  dut->rst_n = 0; dut->nvm_req_i = 0;
  m = 0;
  for (int i = 0; i < 4; ++i) tick();
  dut->rst_n = 1;
  cmd = false; dphase = false; req_at = -1; sil_n = -1; err_at = -1; hold_dev = false;
  babble = false;
  tick();
}

void PauseFuzz::start(bool we, uint8_t rec, const std::vector<uint8_t>& f) {
  while (dut->nvm_busy_o || dut->nvm_done_o || dut->nvm_err_o) tick();
  o = Outcome{};
  owed_since_evt = 0; obl_n = 0;
  m = we ? 1 : 2; w = f; wi = 0; r.clear();
  dut->nvm_req_i = 1; dut->nvm_we_i = we; dut->nvm_record_id_i = rec;
}

//! Run to the pulse and three cycles past it; false if none within `guard`.
bool PauseFuzz::finish(long guard) {
  for (long i = 0; i < guard; ++i) {
    tick();
    if (o.pulse_at >= 0) {
      for (int k = 0; k < 3; ++k) tick();
      m = 0;
      return true;
    }
  }
  m = 0;
  return false;
}

//! A long payload length: the longest random record, or one drawn up to it.
size_t PauseFuzz::long_payload() {
  const int top = std::min(widest, kLongPayload);
  return size_t(u() < 0.5 ? top : rnd(std::min(25, top), top));
}

//! The largest payload_length the port accepts, asked of the port itself: a
//! stored header whose payload_length is past it is refused once its READ
//! completes (UNFRAMED, the banner's refusal (b)), and one within it is taken
//! and its payload READ requested. A binary search over the 16-bit field, so
//! the answer is the build's MAX_PAYLOAD_P whatever the Makefile passed.
int PauseFuzz::largest_legal_length() {
  int lo = 0;                   // taken: a payload_length of 0 always is
  int hi = 65536;               // refused: past the field
  while (hi - lo > 1) {
    const int mid = (lo + hi) / 2;
    const std::vector<uint8_t> f = frame(0, std::vector<uint8_t>(size_t(mid)));
    reset_all();
    std::copy(f.begin(), f.begin() + 8, mem[0]);
    pat = 0;
    start(false, 0, f);
    bool taken = false;
    for (long i = 0; i < kGuard && o.pulse_at < 0 && !taken; ++i) {
      tick();
      taken = dut->dev_req_o && dut->dev_offset_o == 8;
    }
    if (taken) lo = mid; else hi = mid;
  }
  reset_all();
  return lo;
}

//! One operation against a contract-legal device: FZ1-FZ4.
void PauseFuzz::one_legal(std::vector<std::vector<uint8_t>>& good) {
  const uint8_t rec = uint8_t(rnd(0, kRegions - 1));
  const bool we = good[rec].empty() || u() < 0.5;
  std::vector<uint8_t> pl(size_t(rnd(0, 24)));
  for (auto& b : pl) b = uint8_t(rnd(0, 255));
  const std::vector<uint8_t> f = we ? frame(rec, pl) : good[rec];
  new_pattern(true);
  const bool will_err = u() < 0.04;
  err_at = will_err ? rnd(0, obligations(we, f.size()) - 1) : -1;
  start(we, rec, f);
  const bool ok = finish(kGuard);
  char what[160];
  snprintf(what, sizeof what, "cycle %ld: %s of %zu bytes to region %d, pattern %d, err at %d",
           cyc, we ? "commit" : "restore", f.size(), rec, pat, err_at);
  answered.grade(ok && o.dones + o.errs == 1 && !o.busy_at_pulse, what);
  no_deadline.grade(!(o.errs && o.cause == 3), what);
  errs_device.grade(o.errs ? (will_err && o.cause == 1) : !will_err, what);
  if (o.dones == 1) {
    bool exact = true;
    if (we) {
      for (size_t i = 0; i < f.size(); ++i) exact = exact && mem[rec][i] == f[i];
      good[rec] = f;
    } else {
      exact = r == f;
    }
    byte_exact.grade(exact, what);
  } else if (we) {
    good[rec].clear();
  }
  err_at = -1;
  if (cmd || !ok) reset_all();
}

//! One operation whose device withholds one owed event for ever: FZ5, FZ6.
//! The region is then committed again on a prompt device.
void PauseFuzz::one_silent(std::vector<std::vector<uint8_t>>& good) {
  const uint8_t rec = uint8_t(rnd(0, kRegions - 1));
  const bool we = good[rec].empty() || u() < 0.5;
  std::vector<uint8_t> pl(size_t(rnd(0, 24)));
  for (auto& b : pl) b = uint8_t(rnd(0, 255));
  const std::vector<uint8_t> f = we ? frame(rec, pl) : good[rec];
  new_pattern(false);
  sil_n = rnd(0, obligations(we, f.size()) - 1);
  start(we, rec, f);
  const bool ok = finish(kGuard);
  char what[160];
  snprintf(what, sizeof what, "cycle %ld: %s silent at obligation %d, pattern %d, %ld owed",
           cyc, we ? "commit" : "restore", sil_n, pat, o.owed_at_pulse);
  silent_dl.grade(ok && o.dones == 0 && o.errs == 1 && o.cause == 3, what);
  silent_exact.grade(o.owed_at_pulse == TMO + 1, what);
  good[rec].clear();
  reset_all();
  pat = 0;
  const std::vector<uint8_t> g = frame(rec, pl);
  start(true, rec, g);
  if (finish(kGuard) && o.dones == 1) good[rec] = g;
  if (cmd) reset_all();
}

//! A command abandoned at a byte or a terminal (never a grant, and never a
//! WRITE's byte, which is contained), then the next operation at once; the
//! device ends the abandoned command `rr` cycles into that operation's first
//! owed cycle and keeps legal pace after: FZ7.
void PauseFuzz::one_resume(std::vector<std::vector<uint8_t>>& good) {
  const uint8_t rec = uint8_t(rnd(0, kRegions - 1));
  std::vector<uint8_t> pl(size_t(rnd(0, 24)));
  if (u() < 0.5) pl.resize(long_payload());      // ...or a long one
  for (auto& b : pl) b = uint8_t(rnd(0, 255));
  if (good[rec].empty()) {
    pat = 0; start(true, rec, frame(rec, pl));
    if (finish(kGuard) && o.dones == 1) good[rec] = frame(rec, pl);
    if (cmd) reset_all();
    return;
  }
  const bool ab_we = u() < 0.4;
  const std::vector<uint8_t> af = ab_we ? frame(rec, pl) : good[rec];
  const int nobl = obligations(ab_we, af.size());
  int pick = 0;
  for (;;) {
    pick = rnd(0, nobl - 1);
    const bool is_gnt = ab_we ? (pick == 0 || pick == 2) : (pick == 0 || (af.size() > 8 && pick == 10));
    const bool wr_data = ab_we && pick >= 3 && pick < 3 + int(af.size());
    if (!is_gnt && !wr_data) break;
  }
  pat = 0; sil_n = pick;
  start(ab_we, rec, af);
  if (!finish(kGuard) || o.errs != 1 || o.cause != 3 || !cmd) { reset_all(); return; }
  sil_n = -1;
  const int owed = op == kOpRead ? len - moved : 0;
  const uint8_t rec2 = uint8_t((rec + 1 + rnd(0, kRegions - 2)) % kRegions);
  const bool we2 = good[rec2].empty() || u() < 0.5;
  std::vector<uint8_t> pl2(size_t(rnd(0, 24)));
  if (u() < 0.5) pl2.resize(long_payload());
  for (auto& b : pl2) b = uint8_t(rnd(0, 255));
  const std::vector<uint8_t> f2 = we2 ? frame(rec2, pl2) : good[rec2];
  new_pattern(false);
  hold_dev = true;
  start(we2, rec2, f2);
  for (long i = 0; i < kGuard && !(dut->nvm_busy_o && (!we2 || wi >= 8)); ++i) tick();
  const int rr = pick_wait(2 * TMO + 2);
  for (long i = 0; i < rr - 1 && o.pulse_at < 0; ++i) tick();
  next_at = cyc + (rr > 0 ? 1 : 0);
  hold_dev = false;
  const bool ok = finish(kGuard);
  char what[160];
  snprintf(what, sizeof what, "cycle %ld: the abandoned command ended %d cycles into a %s's "
           "wait: %d done, %d err, cause %d", cyc, rr, we2 ? "commit" : "restore",
           o.dones, o.errs, o.cause);
  const bool in_time = rr <= TMO;
  const bool right = ok && (in_time ? (o.dones == 1 && o.errs == 0 && (we2 || r == f2))
                                    : (o.dones == 0 && o.errs == 1 && o.cause == 3));
  resume_branch.grade(right, what);
  if (right) ++(in_time ? served : refused);
  if (right && in_time && owed >= 256) ++served_long;
  if (we2 && o.dones == 1) good[rec2] = f2; else if (we2) good[rec2].clear();
  if (ab_we) good[rec].clear();
  reset_all();
}

//! A READ abandoned at one of its bytes or at its terminal, then the next
//! operation at once, while the device presents the READ's bytes one every
//! TMO / 2 cycles, past its length for ever, and never ends it: FZ9.
void PauseFuzz::one_babble(std::vector<std::vector<uint8_t>>& good) {
  const uint8_t rec = uint8_t(rnd(0, kRegions - 1));
  if (good[rec].size() < 10) {  // a payload of two bytes at least, one still to come
    std::vector<uint8_t> pl(size_t(rnd(2, 24)));
    if (u() < 0.5) pl.resize(long_payload());    // ...or a long one
    for (auto& b : pl) b = uint8_t(rnd(0, 255));
    pat = 0; start(true, rec, frame(rec, pl));
    if (finish(kGuard) && o.dones == 1) good[rec] = frame(rec, pl);
    if (cmd) reset_all();
    return;
  }
  const std::vector<uint8_t> af = good[rec];
  pat = 0;
  sil_n = 11 + rnd(0, int(af.size()) - 10);   // a payload byte
  if (u() < 0.5) {                              // ...or as often a header byte (1-8), the
    const int k = rnd(1, 11);                   // header's terminal (9), the payload's last
    sil_n = k <= 9 ? k : int(af.size()) + k - 8;  // byte or its terminal
  }
  start(false, rec, af);
  if (!finish(kGuard) || o.errs != 1 || o.cause != 3 || !cmd) { reset_all(); return; }
  sil_n = -1;
  const int owed = len - moved;
  ++(moved == len ? at_terminal : off == 0 ? at_header : at_payload);
  const uint8_t rec2 = uint8_t((rec + 1 + rnd(0, kRegions - 2)) % kRegions);
  const bool we2 = good[rec2].empty() || u() < 0.5;
  std::vector<uint8_t> pl2(size_t(rnd(0, 24)));
  for (auto& b : pl2) b = uint8_t(rnd(0, 255));
  const std::vector<uint8_t> f2 = we2 ? frame(rec2, pl2) : good[rec2];
  new_pattern(false);
  hold_dev = true;
  start(we2, rec2, f2);
  for (long i = 0; i < kGuard && !(dut->nvm_busy_o && (!we2 || wi >= 8)); ++i) tick();
  hold_dev = false;
  babble = true; babble_rx = 0; next_at = cyc;
  const bool ok = finish((owed + 400L) * (TMO + 2));
  char what[160];
  snprintf(what, sizeof what, "cycle %ld: a %s behind a READ owing %d bytes: %d done, %d err, "
           "cause %d, %d taken", cyc, we2 ? "commit" : "restore", owed, o.dones, o.errs,
           o.cause, babble_rx);
  babble_dl.grade(ok && o.dones == 0 && o.errs == 1 && o.cause == 3 && babble_rx == owed, what);
  good[rec2].clear();
  good[rec].clear();
  reset_all();
}

//! A READ of the largest payload the port accepts, put in the device's memory
//! and abandoned before its first payload byte, so that it owes every byte of
//! it; then a restore at once, which the device ends the READ within the bound
//! of, keeping legal pace after: FZ7, served. Once per seed.
void PauseFuzz::one_widest(std::vector<std::vector<uint8_t>>& good) {
  std::vector<uint8_t> pl(static_cast<size_t>(widest));
  for (auto& b : pl) b = uint8_t(rnd(0, 255));
  const std::vector<uint8_t> af = frame(0, pl);
  std::vector<uint8_t> pl2(size_t(rnd(0, 24)));
  for (auto& b : pl2) b = uint8_t(rnd(0, 255));
  const std::vector<uint8_t> f2 = frame(1, pl2);
  std::copy(af.begin(), af.end(), mem[0]);
  std::copy(f2.begin(), f2.end(), mem[1]);
  good[0].clear();
  good[1] = f2;
  pat = 0;
  sil_n = 11;                                   // its first payload byte
  start(false, 0, af);
  if (!finish(kGuard) || o.errs != 1 || o.cause != 3 || !cmd || len - moved != widest) {
    reset_all();
    return;
  }
  sil_n = -1;
  new_pattern(false);
  hold_dev = true;
  start(false, 1, f2);
  for (long i = 0; i < kGuard && !dut->nvm_busy_o; ++i) tick();
  const int rr = rnd(0, TMO);                   // the device ends it within the bound
  for (long i = 0; i < rr - 1 && o.pulse_at < 0; ++i) tick();
  next_at = cyc + (rr > 0 ? 1 : 0);
  hold_dev = false;
  const bool ok = finish(kGuard + long(widest) * (TMO + 2));
  char what[160];
  snprintf(what, sizeof what, "cycle %ld: a restore behind a READ owing %d bytes, ended %d cycles "
           "into its wait: %d done, %d err, cause %d", cyc, widest, rr, o.dones, o.errs, o.cause);
  const bool right = ok && o.dones == 1 && o.errs == 0 && r == f2;
  resume_branch.grade(right, what);
  if (right) { ++served; ++served_widest; }
  reset_all();
}

void PauseFuzz::begin_run(uint64_t seed) {
  rng.seed(seed);
  memset(mem, 0xFF, sizeof mem);
  reset_all();
}

int PauseFuzz::run() {
  begin_run(kFirstSeed);
  widest = largest_legal_length();
  printf("fuzz: the largest payload the port accepts, asked of it: %d bytes\n", widest);
  for (uint64_t seed = kFirstSeed; seed <= kLastSeed; ++seed) {
    begin_run(seed);
    std::vector<std::vector<uint8_t>> good(kRegions);
    for (int n = 0; n < kOps; ++n) one_legal(good);
    begin_run(seed);
    good.assign(kRegions, {});
    for (int n = 0; n < kOps; ++n) one_silent(good);
    begin_run(seed);
    good.assign(kRegions, {});
    for (int n = 0; n < kOps; ++n) one_resume(good);
    one_widest(good);
    begin_run(seed);
    good.assign(kRegions, {});
    for (int n = 0; n < kOps; ++n) one_babble(good);
  }
  return report();
}

int PauseFuzz::report() {
  int checks = 0;
  int fails = 0;
  auto check = [&](const char* id, const Tally& t, const char* property) {
    ++checks;
    const bool ok = t.seen > 0 && t.bad == 0;
    if (!ok) ++fails;
    printf("%s: %s %s (%ld of %ld operations broke it%s%s)\n", ok ? "PASS" : "FAIL", id,
           property, t.bad, t.seen, t.bad ? "; first: " : "", t.first);
  };
  check("FZ1", answered, "every operation of a legal device answered exactly once, busy low at the pulse");
  check("FZ2", no_deadline, "no DEADLINE against a device that presented every owed event within the bound");
  check("FZ3", errs_device, "an err only where the device erred, and named DEVICE");
  check("FZ4", byte_exact, "every commit and restore answered done is byte-exact");
  check("FZ5", silent_dl, "a device silent at one owed event ends the operation in one err DEADLINE, never done");
  check("FZ6", silent_exact, "...on exactly the (TMO + 1)-th owed cycle since the device's last event");
  check("FZ7", resume_branch, "a request behind an abandoned command is served if the device ends it within the bound, DEADLINE if not");
  ++checks;
  const bool both = served > 0 && refused > 0;
  if (!both) ++fails;
  printf("%s: FZ8 both of FZ7's branches were taken (%ld served, %ld DEADLINE)\n",
         both ? "PASS" : "FAIL", served, refused);
  check("FZ9", babble_dl, "a request behind a READ whose device presents bytes past its length ends DEADLINE, the port taking only the bytes the READ owed");
  ++checks;
  const long seeds = long(kLastSeed - kFirstSeed + 1);
  const bool reached = at_header > 0 && at_payload > 0 && at_terminal > 0 && served_long > 0
                       && served_widest == seeds;
  if (!reached) ++fails;
  printf("%s: FZ10 the abandonments reached every branch of what a READ owes: babble's at header "
         "bytes, payload bytes and terminals, and resume served requests behind READs owing 256 bytes "
         "or more and behind one owing the largest payload the port accepts, %d bytes, each seed (%ld, "
         "%ld and %ld; %ld, and %ld of %ld)\n",
         reached ? "PASS" : "FAIL", widest, at_header, at_payload, at_terminal, served_long,
         served_widest, seeds);
  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  FILE* acc = fopen("obj_dir/build_tally.txt", "a");
  if (acc == nullptr) {
    printf("FAIL: this build's tally cannot be recorded for the Makefile\n");
    return 1;
  }
  fprintf(acc, "%d %d\n", checks, fails);
  fclose(acc);
  return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  const milan::tb::Model<VKL_pp_nvm_port> model;
  PauseFuzz fuzz(model.get());
  printf("fuzz: MEM_TIMEOUT_CYC_P = %d, seeds 11 to 13, %d operations per mode and seed\n",
         TMO, kOps);
  return fuzz.run();
}
