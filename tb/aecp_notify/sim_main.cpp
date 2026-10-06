// SPDX-License-Identifier: CERN-OHL-W-2.0
// AECP registry monitor lifecycle, including TIME_LIMITED row reuse; with
// AECP_NOTIFY_IDENT (the second build, P-EN-IDENTIFY-NOTIFICATION = 1), the
// identify schedule at the full timebase instead (identify_timebase.hpp).
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>
#include "../common/verilator_harness.hpp"
#include "VKL_aecp_notify.h"
#include "verilated.h"

#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

static constexpr uint8_t OWN_TL = 0xA0;
static constexpr uint8_t OWN_MON = 0xD0;
static constexpr uint8_t REGMON_BASE = 25;
static constexpr uint8_t N_CTRL = 2;
static constexpr uint8_t KIND_DEREG = 0;         // pp_pkg PP_UNS_DEREG_C
static constexpr uint8_t KIND_CTRS = 6;          // pp_pkg PP_UNS_CTRS_C
static constexpr uint8_t KIND_NAME = 8;          // pp_pkg PP_UNS_NAME_C
static constexpr uint16_t DT_AVB_INTERFACE = 0x0009;

// Cycle budgets for the polling loops below. Each one gives up when its budget
// is spent, so a DUT that never asserts fails the check instead of hanging.
static constexpr int REGISTRY_ACCEPT_CYCLES = 24;
static constexpr int DRAW_REQUEST_CYCLES = 16;
static constexpr int PROBE_WAIT_CYCLES = 12;
static constexpr int DRAIN_WATCH_CYCLES = 8;
static constexpr int COUNTER_JOB_CYCLES = 16;
// The pick and the walk to a round's next job, in cycles (one ms each in TW and DR).
static constexpr uint32_t WALK_MS = 8;

namespace {

#ifndef AECP_NOTIFY_IDENT
// The tally the CHECK macro keeps was a pair of file-scope statics, and so was
// nothing else here; both are the state of one run of this harness, so they
// belong to the object that performs it.
struct Harness {
  static constexpr uint64_t EID_C = 0x3333000000000003ull;
  static constexpr uint64_t MAC_C = 0x020000000003ull;
  VKL_aecp_notify* d = nullptr;
  uint32_t now = 1000;
  int checks = 0;
  int fails = 0;

  int run();
  bool cancels_row0(uint64_t eid, uint64_t mac);
  void identity_index(uint64_t eid_old, uint64_t mac_old, uint64_t eid, uint64_t mac);
  bool register_to_write(uint64_t eid, uint64_t mac);
  int after_failure(uint8_t row, bool command, uint64_t eid, uint64_t mac);
  void rewrite_window();
  bool counter_job();
  void counter_stamps();
  struct Job {
    uint32_t ms;                            // first seen in this ms; 0 = none
    uint64_t mac;
  };
  void counter_change();
  Job counter_presented(uint32_t last);
  uint32_t retire();
  void hold_until(uint32_t ms);
  void round_waits_for_tx();
  struct UnsJob {
    uint32_t ms = 0;                        // first seen in this ms; 0 = none
    uint8_t kind = 0;
    uint16_t dt = 0;
    uint16_t di = 0;
    uint16_t arg0 = 0;
    uint16_t arg1 = 0;
    uint16_t seq = 0;
    uint64_t mac = 0;
  };
  UnsJob job_presented(uint32_t last);
  std::vector<UnsJob> sent_at_once(uint32_t last);
  void dereg_counter_round();
  void dereg_command_round();
  void dereg_round_waits();
  void cancel_clock();

  void tick() {
    d->now_ms_i = now;
    d->clk_i = 0;
    d->eval();
    d->clk_i = 1;
    d->eval();
  }

  void idle(int n = 1) {
    for (int i = 0; i < n; ++i) tick();
  }

  //! a REGISTER of {eid, mac}; true when the registry accepted the tuple
  bool registers(uint64_t eid, uint64_t mac, bool time_limited) {
    d->rgy_state_i = 0;
    d->rgy_op_i = 0;
    d->rgy_eid_i = eid;
    d->rgy_mac_i = mac;
    d->rgy_tl_i = time_limited;
    d->rgy_req_i = 1;
    int guard = 0;
    while (guard++ < REGISTRY_ACCEPT_CYCLES) {
      d->clk_i = 0;
      d->eval();
      if (!d->rgy_wait_o) break;
      tick();
    }
    const bool accepted = guard < REGISTRY_ACCEPT_CYCLES && d->rgy_data_o == 0;
    tick();
    d->rgy_req_i = 0;
    idle(2);
    return accepted;
  }

  void register_row(uint64_t eid, uint64_t mac, bool time_limited) {
    CHECK(registers(eid, mac, time_limited), "registry accepts controller tuple");
  }

  //! the monitor draw a registration asks for, answered with 30 s; true when
  //! it was asked for
  bool draws() {
    d->prng_draw_busy_i = 0;
    int guard = 0;
    while (guard++ < DRAW_REQUEST_CYCLES) {
      d->clk_i = 0;
      d->eval();
      if (d->prng_draw_req_o) break;
      tick();
    }
    const bool asked = guard < DRAW_REQUEST_CYCLES;
    d->prng_draw_ms_i = 30000;
    d->prng_draw_valid_i = 1;
    tick();
    d->prng_draw_valid_i = 0;
    d->prng_draw_busy_i = 1;
    idle(2);
    return asked;
  }

  void complete_draw() {
    CHECK(draws(), "registration requests an independent monitor draw");
  }

  void expire(uint8_t slot, uint8_t owner) {
    d->tmr_exp_slot_i = slot;
    d->tmr_exp_owner_i = owner;
    d->tmr_exp_valid_i = 1;
    tick();
    d->tmr_exp_valid_i = 0;
  }

  bool wait_probe(uint64_t eid, uint64_t mac) {
    for (int i = 0; i < PROBE_WAIT_CYCLES; ++i) {
      d->clk_i = 0;
      d->eval();
      if (d->ca_valid_o) {
        bool exact = d->ca_owner_o == 0 && d->ca_ctlr_eid_o == eid
                     && static_cast<uint64_t>(d->ca_mac_o) == mac;
        tick();
        return exact;
      }
      tick();
    }
    return false;
  }

  void warm_reset() {
    d->rst_n = 0;
    idle(2);
    d->rst_n = 1;
    idle(2);
  }
};

int Harness::run() {
  const milan::tb::Model<VKL_aecp_notify> model;
  VKL_aecp_notify* const dut = model.get();
  d = dut;

  dut->rgy_req_i = 0;
  dut->rgy_state_i = 0;
  dut->rgy_op_i = 0;
  dut->rgy_eid_i = 0;
  dut->rgy_mac_i = 0;
  dut->rgy_tl_i = 0;
  dut->ev_stri_in_i = 0;
  dut->ev_stri_out_i = 0;
  dut->ev_avb_i = 0;
  dut->ev_asp_i = 0;
  dut->ev_amap_i = 0;
  dut->ev_amap_remove_i = 0;
  dut->ev_amap_type_i = 0;
  dut->ev_amap_index_i = 0;
  dut->ev_amap_count_i = 0;
  dut->ev_amap_excl_eid_i = 0;
  dut->ev_ctr_i = 0;
  dut->ev_ctr_type_i = 0;
  dut->ev_ctr_index_i = 0;
  dut->ev_cmd_i = 0;
  dut->ev_cmd_class_i = 0;
  dut->ev_cmd_type_i = 0;
  dut->ev_cmd_index_i = 0;
  dut->ev_cmd_arg0_i = 0;
  dut->ev_cmd_arg1_i = 0;
  dut->ev_cmd_excl_eid_i = 0;
  dut->rx_cmd_valid_i = 0;
  dut->rx_cmd_eid_i = 0;
  dut->rx_cmd_mac_i = 0;
  dut->prng_draw_busy_i = 1;
  dut->prng_draw_valid_i = 0;
  dut->prng_draw_ms_i = 0;
  dut->ca_ready_i = 1;
  dut->ca_rsp_valid_i = 0;
  dut->ca_rsp_owner_i = 0;
  dut->ca_fail_valid_i = 0;
  dut->ca_fail_owner_i = 0;
  dut->uns_done_i = 1;
  dut->tmr_exp_valid_i = 0;
  dut->tmr_exp_slot_i = 0;
  dut->tmr_exp_owner_i = 0;

  dut->rst_n = 0;
  idle(4);
  dut->rst_n = 1;
  idle(2);

  const uint64_t EID_A = 0x1111000000000001ull;
  const uint64_t MAC_A = 0x020000000001ull;
  const uint64_t EID_B = 0x2222000000000002ull;
  const uint64_t MAC_B = 0x020000000002ull;

  register_row(EID_A, MAC_A, true);
  CHECK(dut->dbg_reg_cnt_o == 1, "A TIME_LIMITED row is live");
  complete_draw();
  expire(REGMON_BASE + N_CTRL, OWN_MON);
  CHECK(wait_probe(EID_A, MAC_A),
        "A monitor expiry launches the exact registered tuple");

  // Expire the registry row while its availability exchange is live. The
  // drain cycle must cancel owner zero before row zero becomes reusable.
  expire(REGMON_BASE, OWN_TL);
  bool saw_cancel = false;
  for (int i = 0; i < DRAIN_WATCH_CYCLES; ++i) {
    dut->clk_i = 0;
    dut->eval();
    if (dut->ca_cancel_valid_o && dut->ca_cancel_owner_o == 0)
      saw_cancel = true;
    tick();
  }
  CHECK(saw_cancel,
        "TIME_LIMITED drain cancels the active availability exchange");
  CHECK(dut->dbg_reg_cnt_o == 0, "expired row is removed after cancellation");

  register_row(EID_B, MAC_B, false);
  CHECK(dut->dbg_reg_cnt_o == 1, "expired row can be reused by a new tuple");
  complete_draw();
  expire(REGMON_BASE + N_CTRL, OWN_MON);
  CHECK(wait_probe(EID_B, MAC_B),
        "reused row launches only the new controller tuple");

  identity_index(EID_A, MAC_A, EID_B, MAC_B);
  rewrite_window();
  counter_stamps();
  round_waits_for_tx();
  dereg_counter_round();
  dereg_command_round();
  dereg_round_waits();
  cancel_clock();
  return fails ? 1 : 0;
}

// ---- IX: the identity index (issue #232) ------------------------------------
// The availability monitor matches a command's {eid, mac} against every row in
// the command's own cycle, through a LUTRAM index rather than a comparator per
// row. Row 0 holds B, which reused A's row, and B's probe is live, so a match
// on row 0 shows combinationally as the probe's cancellation (ca_cancel_valid_o).
// The probes below only evaluate: no clock edge, no state change.
bool Harness::cancels_row0(uint64_t eid, uint64_t mac) {
  d->rx_cmd_eid_i = eid;
  d->rx_cmd_mac_i = mac;
  d->rx_cmd_valid_i = 1;
  d->clk_i = 0;
  d->eval();
  const bool hit = d->ca_cancel_valid_o && d->ca_cancel_owner_o == 0;
  d->rx_cmd_valid_i = 0;
  d->eval();
  return hit;
}

void Harness::identity_index(uint64_t eid_old, uint64_t mac_old,
                             uint64_t eid, uint64_t mac) {
  CHECK(!cancels_row0(eid_old, mac_old),
        "IX1: the reused row no longer matches its previous controller");
  int neighbours = 0;
  for (int b = 0; b < 112; ++b) {
    const uint64_t e = b >= 48 ? eid ^ (1ull << (b - 48)) : eid;
    const uint64_t m = b < 48 ? mac ^ (1ull << b) : mac;
    if (cancels_row0(e, m)) ++neighbours;
  }
  CHECK(neighbours == 0,
        "IX2: no identity one bit from a registered one matches (%d of 112 did)",
        neighbours);
  CHECK(cancels_row0(eid, mac),
        "IX3: the registered identity matches in the command's cycle");

  // A REGISTER rewrites its row's index over the row write's own cycle and the
  // cycle after it. Two cycles after C claims row 1, C's command must still
  // win against a failed probe reported in the same cycle (KL_aecp_notify's
  // ca_fail arm), so the row stays; the same failure alone removes it.
  d->rgy_state_i = 0;
  d->rgy_op_i = 0;
  d->rgy_eid_i = EID_C;
  d->rgy_mac_i = MAC_C;
  d->rgy_tl_i = 0;
  d->rgy_req_i = 1;
  int guard = 0;
  while (guard++ < REGISTRY_ACCEPT_CYCLES) {
    d->clk_i = 0;
    d->eval();
    if (!d->rgy_wait_o) break;
    tick();
  }
  CHECK(guard < REGISTRY_ACCEPT_CYCLES && d->rgy_data_o == 0,
        "IX4a: the registry accepts a second controller");
  tick();                                   // the row write's own cycle
  d->rx_cmd_eid_i = EID_C;
  d->rx_cmd_mac_i = MAC_C;
  d->rx_cmd_valid_i = 1;
  d->ca_fail_owner_i = 1;
  d->ca_fail_valid_i = 1;
  tick();                                   // the cycle after it
  d->rx_cmd_valid_i = 0;
  d->ca_fail_valid_i = 0;
  d->rgy_req_i = 0;
  idle(DRAIN_WATCH_CYCLES);
  CHECK(d->dbg_reg_cnt_o == 2,
        "IX4: a command two cycles after its REGISTER keeps the row against a "
        "failed probe in the same cycle");
  d->ca_fail_owner_i = 1;
  d->ca_fail_valid_i = 1;
  tick();
  d->ca_fail_valid_i = 0;
  idle(DRAIN_WATCH_CYCLES);
  CHECK(d->dbg_reg_cnt_o == 1, "IX4b: the same failure alone removes the row");
}

// REGISTER up to the row write's own cycle: N_APPLY decides the write, and
// rgy_wait_o falls in the next cycle, the one the write lands in and the
// index clears the old identity in. Returns there, with the clock low and the
// request still raised, so the caller presents that cycle's inputs.
bool Harness::register_to_write(uint64_t eid, uint64_t mac) {
  d->rgy_state_i = 0;
  d->rgy_op_i = 0;
  d->rgy_eid_i = eid;
  d->rgy_mac_i = mac;
  d->rgy_tl_i = 0;
  d->rgy_req_i = 1;
  int guard = 0;
  while (guard++ < REGISTRY_ACCEPT_CYCLES) {
    d->clk_i = 0;
    d->eval();
    if (!d->rgy_wait_o) break;
    tick();
  }
  return guard < REGISTRY_ACCEPT_CYCLES && d->rgy_data_o == 0;
}

// One clock carrying a failed probe for `row` and, when `command`, a command
// from {eid, mac}; then the drain's time. Returns the live row count: a
// command that matches the row keeps it (the ca_fail arm), else it goes.
int Harness::after_failure(uint8_t row, bool command, uint64_t eid, uint64_t mac) {
  d->rx_cmd_eid_i = eid;
  d->rx_cmd_mac_i = mac;
  d->rx_cmd_valid_i = command;
  d->ca_fail_owner_i = row;
  d->ca_fail_valid_i = 1;
  tick();
  d->rx_cmd_valid_i = 0;
  d->ca_fail_valid_i = 0;
  d->rgy_req_i = 0;
  idle(DRAIN_WATCH_CYCLES);
  return d->dbg_reg_cnt_o;
}

// IX5 and IX6: the rewrite's first cycle. While the row write lands, the
// index clears the old identity, and the row's match is the one comparator
// against what rows_r holds then: the old row, as the comparator bank read
// it. Row 1 still holds C, which IX4b removed, so D's REGISTER reuses it, and
// in its row write's own cycle C's command still matches row 1. A reset in
// that cycle lets the row write and the clear land and drops the set, so
// rows_r holds E while the index holds nothing for the row; E's next REGISTER
// takes the same row, and in its row write's own cycle only the comparator
// can match E.
void Harness::rewrite_window() {
  const uint64_t EID_D = 0x4444000000000004ull;
  const uint64_t MAC_D = 0x020000000004ull;
  const uint64_t EID_E = 0x5555000000000005ull;
  const uint64_t MAC_E = 0x020000000005ull;
  const bool d_taken = register_to_write(EID_D, MAC_D);
  const int live5 = after_failure(1, true, EID_C, MAC_C);
  CHECK(d_taken && live5 == 2,
        "IX5: in the row write's own cycle, the reused row's previous controller C, "
        "which rows_r still holds, keeps the row against a failed probe in the same "
        "cycle (%d rows live)", live5);
  CHECK(after_failure(1, false, 0, 0) == 1, "IX5b: the same failure alone removes the row");

  warm_reset();                             // every row free: E takes row 0
  const bool e_taken = register_to_write(EID_E, MAC_E);
  d->rgy_req_i = 0;
  warm_reset();                             // in the row write's own cycle
  CHECK(e_taken && d->dbg_reg_cnt_o == 0,
        "IX6a: a reset in the row write's own cycle leaves the registry empty");
  const bool e_again = register_to_write(EID_E, MAC_E);
  const int live6 = after_failure(0, true, EID_E, MAC_E);
  CHECK(e_again && live6 == 1,
        "IX6: after that reset, E registered into the same row keeps it against a "
        "failed probe in its row write's own cycle (%d rows live)", live6);
  CHECK(after_failure(0, true, EID_E, MAC_E) == 1,
        "IX6b: after the rewrite, E's command keeps the row through the index");
}

// TS: the counter throttle stamps (issue #232). A descriptor's one-second
// stamp has no reset: ctr_sent_r is its valid bit, and a warm reset clears
// only that bit. So the first change after a reset goes out at once, however
// recent the stale stamp. The bench's clock stays in one ms throughout.
bool Harness::counter_job() {
  d->ev_ctr_type_i = DT_AVB_INTERFACE;
  d->ev_ctr_index_i = 0;
  d->ev_ctr_i = 1;
  tick();
  d->ev_ctr_i = 0;
  for (int i = 0; i < COUNTER_JOB_CYCLES; ++i) {
    d->clk_i = 0;
    d->eval();
    const bool job = d->uns_valid_o && d->uns_kind_o == KIND_CTRS;
    tick();
    if (job) return true;
  }
  return false;
}

void Harness::counter_stamps() {
  warm_reset();                             // independent of section IX's rows
  register_row(EID_C, MAC_C, false);
  CHECK(counter_job(), "TS1: a counter change goes out to the registered controller");
  CHECK(!counter_job(), "TS2: a second change in the same second is held");
  warm_reset();
  register_row(EID_C, MAC_C, false);
  CHECK(counter_job(),
        "TS3: after a warm reset, a change in that same second goes out at once");
}

// TW: a counter round that waits for the TX slot (issue #148). The engine
// retires a job (uns_done_i) when the TX arbiter grants its frame, so a job
// held for the TX slot is sent late. Milan Table 5.22 (T-CTR-NOTIF) spaces a
// descriptor's GET_COUNTERS notifications a second apart at each controller,
// so the next round waits a second from the previous round's last send, never
// from its selection. Here the bench is the engine and retires each job when
// the section says, and the clock advances one ms per cycle.
void Harness::counter_change() {
  d->ev_ctr_type_i = DT_AVB_INTERFACE;
  d->ev_ctr_index_i = 0;
  d->ev_ctr_i = 1;
  tick();
  d->ev_ctr_i = 0;
  ++now;
}

//! cycles at one ms each until a GET_COUNTERS job is presented, up to ms `last`
Harness::Job Harness::counter_presented(uint32_t last) {
  while (now <= last) {
    d->clk_i = 0;
    d->eval();
    if (d->uns_valid_o && d->uns_kind_o == KIND_CTRS) return {now, d->uns_mac_o};
    tick();
    ++now;
  }
  return {0, 0};
}

//! the presented job's frame is granted now: returns the ms of that send
uint32_t Harness::retire() {
  const uint32_t sent = now;
  d->uns_done_i = 1;
  tick();
  d->uns_done_i = 0;
  ++now;
  return sent;
}

void Harness::hold_until(uint32_t ms) {
  while (now < ms) {
    tick();
    ++now;
  }
}

void Harness::round_waits_for_tx() {
  constexpr uint64_t EID_D = 0x4444000000000004ull;
  constexpr uint64_t MAC_D = 0x020000000004ull;
  constexpr uint32_t LATE = 8;              // the pick and the walk to the job, in cycles
  warm_reset();
  d->uns_done_i = 0;
  now = 2000;
  register_row(EID_C, MAC_C, false);        // row 0
  register_row(EID_D, MAC_D, false);        // row 1
  counter_change();
  const Job c1 = counter_presented(now + LATE);
  (void)retire();                           // C's frame leaves at once
  const Job d1 = counter_presented(now + LATE);
  hold_until(2600);                         // D's waits 600 ms for the TX slot
  const uint32_t sent1 = retire();
  hold_until(2700);
  counter_change();                         // inside the second after that send
  const Job c2 = counter_presented(sent1 + 1000 + LATE);
  printf("  [i] TW1: the round's last job sent at ms %u; the next round presented at ms %u\n",
         sent1, c2.ms);
  CHECK(c1.mac == MAC_C && d1.mac == MAC_D && c2.ms >= sent1 + 1000
            && c2.ms <= sent1 + 1000 + LATE,
        "TW1: a round first presented at ms %u whose last job waited for the TX slot until ms "
        "%u holds a change made 100 ms later until a second after that send: next round "
        "at ms %u, want %u to %u", c1.ms, sent1, c2.ms, sent1 + 1000, sent1 + 1000 + LATE);
  (void)retire();
  const Job d2 = counter_presented(now + LATE);
  (void)retire();
  // a change made while a round's job waits more than a second for the TX
  // slot waits a second from the round's last send, not from its selection
  hold_until(5000);
  counter_change();
  const Job c3 = counter_presented(now + LATE);
  hold_until(5100);
  counter_change();
  hold_until(6500);                         // C's job waits 1.5 s for the TX slot
  (void)retire();
  const Job d3 = counter_presented(now + LATE);
  const uint32_t sent3 = retire();
  const Job c4 = counter_presented(sent3 + 1000 + LATE);
  printf("  [i] TW2: the round's last job sent at ms %u; the next round presented at ms %u\n",
         sent3, c4.ms);
  CHECK(d2.mac == MAC_D && c3.mac == MAC_C && d3.mac == MAC_D && c4.ms >= sent3 + 1000
            && c4.ms <= sent3 + 1000 + LATE,
        "TW2: a change made at ms 5100, while a round first presented at ms %u waited for the TX "
        "slot until ms 6500, goes out a second after the round's last send at ms %u: next "
        "round at ms %u, want %u to %u", c3.ms, sent3, c4.ms, sent3 + 1000, sent3 + 1000 + LATE);
  d->uns_done_i = 1;
}

// DR: a DEREGISTER drained between two jobs of a round (issue #158). A TIME_LIMITED
// expiry, or a failed CONTROLLER_AVAILABLE retry, parks a controller's row while
// its job waits, and the row is drained before the round's next job: the
// controller is owed its own DEREGISTER notification. The round's remaining
// controllers must still receive the round's notification, and the expired one
// its DEREGISTER alone (kind 0, descriptor 0000:0, its own sequence_id), in either
// order. Rows C (0) and D (1); the clock advances one ms per cycle, as in TW.

//! cycles at one ms each until any job is presented, up to ms `last`
Harness::UnsJob Harness::job_presented(uint32_t last) {
  while (now <= last) {
    d->clk_i = 0;
    d->eval();
    if (d->uns_valid_o) {
      return {now, d->uns_kind_o, d->uns_desc_type_o, d->uns_desc_index_o,
              d->uns_arg0_o, d->uns_arg1_o, d->uns_seq_o, d->uns_mac_o};
    }
    tick();
    ++now;
  }
  return {};
}

//! every job presented up to ms `last`, each sent in the ms it is presented
std::vector<Harness::UnsJob> Harness::sent_at_once(uint32_t last) {
  std::vector<UnsJob> jobs;
  for (UnsJob j = job_presented(last); j.ms != 0; j = job_presented(last)) {
    jobs.push_back(j);
    (void)retire();
  }
  return jobs;
}

//! `jobs` holds exactly one job to `mac`, and it carries `want`'s response,
//! descriptor, arguments and sequence_id
bool one_job(const std::vector<Harness::UnsJob>& jobs, uint64_t mac,
             const Harness::UnsJob& want) {
  int n = 0;
  bool same = false;
  for (const Harness::UnsJob& j : jobs) {
    if (j.mac != mac) continue;
    ++n;
    same = j.kind == want.kind && j.dt == want.dt && j.di == want.di && j.arg0 == want.arg0
           && j.arg1 == want.arg1 && j.seq == want.seq;
  }
  return n == 1 && same;
}

void print_jobs(const char* id, const std::vector<Harness::UnsJob>& jobs) {
  for (const Harness::UnsJob& j : jobs) {
    printf("  [i] %s: after the drain, ms %u: kind %u, %04x:%u, args %u/%u, seq %u, to %012llx\n",
           id, j.ms, unsigned(j.kind), unsigned(j.dt), unsigned(j.di), unsigned(j.arg0),
           unsigned(j.arg1), unsigned(j.seq), static_cast<unsigned long long>(j.mac));
  }
}

//! DR1: the issue's probe, a GET_COUNTERS round on AVB_INTERFACE[0] whose first
//! controller's TIME_LIMITED registration expires while its job waits
void Harness::dereg_counter_round() {
  constexpr uint64_t EID_D = 0x4444000000000004ull;
  constexpr uint64_t MAC_D = 0x020000000004ull;
  warm_reset();
  d->uns_done_i = 0;
  now = 20000;
  register_row(EID_C, MAC_C, true);         // row 0, TIME_LIMITED
  register_row(EID_D, MAC_D, false);        // row 1
  counter_change();
  const UnsJob c1 = job_presented(now + WALK_MS);
  expire(REGMON_BASE, OWN_TL);              // C's registration expires now
  (void)retire();
  const std::vector<UnsJob> rest = sent_at_once(now + 4 * WALK_MS);
  print_jobs("DR1", rest);
  const UnsJob round{0, KIND_CTRS, DT_AVB_INTERFACE, 0, 0, 0, 0, 0};
  CHECK(c1.mac == MAC_C && one_job({c1}, MAC_C, round) && one_job(rest, MAC_D, round),
        "DR1: in a GET_COUNTERS round on 0009:0, C's registration expires while its job "
        "waits, and the round's remaining controller D still receives the round's "
        "GET_COUNTERS 0009:0 (%zu jobs after the drain)", rest.size());
  const UnsJob own{0, KIND_DEREG, 0, 0, 0, 0, 1, 0};
  CHECK(rest.size() == 2 && one_job(rest, MAC_C, own),
        "DR1b: C alone receives its own DEREGISTER (kind 0, 0000:0, sequence_id 1), once");
  d->uns_done_i = 1;
}

//! DR2: a command round (SET_NAME, every field non-zero; the block carries them
//! through unread) whose first controller's CONTROLLER_AVAILABLE retry fails
//! while its job waits; the requester is not registered
void Harness::dereg_command_round() {
  constexpr uint64_t EID_D = 0x4444000000000004ull;
  constexpr uint64_t MAC_D = 0x020000000004ull;
  constexpr uint64_t EID_R = 0x7777000000000007ull;
  warm_reset();
  d->uns_done_i = 0;
  now = 25000;
  register_row(EID_C, MAC_C, false);        // row 0
  register_row(EID_D, MAC_D, false);        // row 1
  d->ev_cmd_class_i = 7;                    // SET_NAME
  d->ev_cmd_type_i = 0x0005;
  d->ev_cmd_index_i = 1;
  d->ev_cmd_arg0_i = 2;
  d->ev_cmd_arg1_i = 3;
  d->ev_cmd_excl_eid_i = EID_R;
  d->ev_cmd_i = 1;
  tick();
  d->ev_cmd_i = 0;
  ++now;
  const UnsJob c1 = job_presented(now + WALK_MS);
  d->ca_fail_owner_i = 0;                   // C's retry fails now
  d->ca_fail_valid_i = 1;
  tick();
  d->ca_fail_valid_i = 0;
  (void)retire();
  const std::vector<UnsJob> rest = sent_at_once(now + 4 * WALK_MS);
  print_jobs("DR2", rest);
  const UnsJob round{0, KIND_NAME, 0x0005, 1, 2, 3, 0, 0};
  CHECK(c1.mac == MAC_C && one_job({c1}, MAC_C, round) && one_job(rest, MAC_D, round),
        "DR2: in a SET_NAME round on 0005:1 (arguments 2 and 3), C's CONTROLLER_AVAILABLE "
        "retry fails while its job waits, and the round's remaining controller D still "
        "receives the round's notification (%zu jobs after the drain)", rest.size());
  const UnsJob own{0, KIND_DEREG, 0, 0, 0, 0, 1, 0};
  CHECK(rest.size() == 2 && one_job(rest, MAC_C, own),
        "DR2b: C alone receives its own DEREGISTER (kind 0, 0000:0, sequence_id 1), once");
  d->uns_done_i = 1;
}

//! DR3: TW with the drain (review R477-1 S2 on PR #159). C's registration expires
//! in a GET_COUNTERS round, the job after the drain waits 1.5 s for the TX slot
//! and a change arrives during the wait: D's next GET_COUNTERS still waits a
//! second from D's own send in the round
void Harness::dereg_round_waits() {
  constexpr uint64_t EID_D = 0x4444000000000004ull;
  constexpr uint64_t MAC_D = 0x020000000004ull;
  warm_reset();
  d->uns_done_i = 0;
  now = 30000;
  register_row(EID_C, MAC_C, true);         // row 0, TIME_LIMITED
  register_row(EID_D, MAC_D, false);        // row 1
  counter_change();
  (void)job_presented(now + WALK_MS);       // C's job
  expire(REGMON_BASE, OWN_TL);
  const uint32_t sent0 = retire();
  UnsJob held = job_presented(now + WALK_MS);
  hold_until(sent0 + 100);
  counter_change();                         // inside the wait
  hold_until(sent0 + 1500);                 // the TX slot frees
  held.ms = retire();                       // from here a job's ms is its send
  std::vector<UnsJob> jobs = sent_at_once(held.ms + 1000 + 2 * WALK_MS);
  jobs.insert(jobs.begin(), held);
  std::vector<uint32_t> to_d;               // D's GET_COUNTERS sends, in order
  for (const UnsJob& j : jobs) {
    if (j.mac == MAC_D && j.kind == KIND_CTRS && j.dt == DT_AVB_INTERFACE) to_d.push_back(j.ms);
  }
  const uint32_t first = to_d.empty() ? 0 : to_d[0];
  const uint32_t next = to_d.size() < 2 ? 0 : to_d[1];
  printf("  [i] DR3: the job after the drain (kind %u to %012llx) sent at ms %u; D's "
         "GET_COUNTERS sent at ms %u and next presented at ms %u\n", unsigned(held.kind),
         static_cast<unsigned long long>(held.mac), held.ms, first, next);
  CHECK(to_d.size() == 2 && next >= first + 1000 && next <= first + 1000 + WALK_MS,
        "DR3: C's registration expires in a GET_COUNTERS round, the job after the drain "
        "waits for the TX slot until ms %u and a change arrives at ms %u: D's GET_COUNTERS "
        "sent at ms %u, the next at ms %u, want %u to %u (%zu of D's GET_COUNTERS seen)",
        held.ms, sent0 + 100, first, next, first + 1000, first + 1000 + WALK_MS, to_d.size());
  d->uns_done_i = 1;
}

//! CX (issue #163): the top registers the originator's withdraw mask, so a
//! cancellation reaches the TX arbiter one clock after the originator takes
//! it. That is the only clock it gains: a command from the probing controller
//! cancels the probe in the command's own clock, before the edge that takes
//! the command, and in no clock after it.
void Harness::cancel_clock() {
  constexpr uint64_t EID_P = 0x5555000000000005ull;
  constexpr uint64_t MAC_P = 0x020000000005ull;
  warm_reset();
  const bool registered = registers(EID_P, MAC_P, false);   // row 0
  const bool drawn = draws();
  expire(REGMON_BASE + N_CTRL, OWN_MON);
  const bool probing = registered && drawn && wait_probe(EID_P, MAC_P);
  idle(2);
  d->rx_cmd_eid_i = EID_P;
  d->rx_cmd_mac_i = MAC_P;
  d->rx_cmd_valid_i = 1;
  d->clk_i = 0;
  d->eval();
  const bool own = d->ca_cancel_valid_o && d->ca_cancel_owner_o == 0;
  tick();                                   // the edge that takes the command
  d->rx_cmd_valid_i = 0;
  int later = 0;
  for (int i = 0; i < DRAIN_WATCH_CYCLES; ++i) {
    d->clk_i = 0;
    d->eval();
    later += d->ca_cancel_valid_o ? 1 : 0;
    tick();
  }
  CHECK(probing && own && later == 0,
        "CX1: a command from the probing controller cancels its probe in the command's "
        "own clock (probe live %d, cancelled then %d) and in no later one (%d of %d)",
        int(probing), int(own), later, DRAIN_WATCH_CYCLES);
}

#else
// ---- FT: the identify schedule at the full timebase -------------------------
// tb/pp_top grades the burst on the wire with 1 ms compressed to 100 clocks,
// where one tick is no longer than a frame's own build and serialization, so
// the sequencer's one-tick margins cannot be seen there (reviews R420-2 S1 and
// R421-2 S2). Here 1 ms is 100,000 clocks, the F01.5 default P-CLK-HZ of
// 100 MHz with the top's default prescaler (TIM_DIV_US_P 100, TIM_DIV_MS_P
// 1000), and the bench is the engine, the MAC and the shared timer service, so
// a frame leaves on the clock the bench picks:
//   - IDENT-BURST counts from the next ms boundary after a frame left
//     (IEEE 1722.1-2021 7.5.1: "a delay of 150 milliseconds between
//     transmissions"), so a frame that leaves on the last clock whose arm still
//     lands in its ms is followed no sooner than T-IDENT-BURST (FT2), and one
//     that leaves on a ms's first clock less than one tick later (FT3);
//   - t0, the base of Figure 7-142's timeout (currentTime + 1 second), is the
//     next boundary after the first frame left, so a held button re-arms no
//     sooner than T-IDENT-REARM after it (FT4).
// The timer model fires a due slot on the first clock of its deadline's ms,
// with no sweep delay: the least favourable placement for both margins (the
// real service's sweep only adds clocks).
struct IdentHarness {
  static constexpr uint64_t CPM = 100000;          // clocks per ms
  static constexpr uint64_t BURST = 150 * CPM;     // T-IDENT-BURST (F08.1)
  static constexpr uint64_t REARM = 1000 * CPM;    // T-IDENT-REARM (F08.1)
  static constexpr uint64_t LATE = 8;              // job presentation after an expiry
  static constexpr uint64_t ACCEPT = 20;           // the engine's pick and build
  static constexpr uint64_t SER = 1000;            // the frame on the wire, at least
  static constexpr uint64_t END = CPM - 2;         // the last clock whose arm stays in its ms
  static constexpr uint64_t START = 0;
  static constexpr uint32_t BASE_MS = 1000;
  static constexpr uint8_t IDENT_KIND = 15;        // the uns face's identify job (pp_pkg)
  struct Slot {
    bool armed = false;
    uint8_t owner = 0;
    uint32_t deadline = 0;
  };
  struct Fire {
    uint8_t slot;
    uint8_t owner;
  };
  struct Frame {
    uint64_t presented = 0;                        // the job reached the uns face
    uint64_t departed = 0;                         // the first clock uns_tx_busy_i read 0
  };
  VKL_aecp_notify* d = nullptr;
  uint64_t cyc = 0;
  int checks = 0;
  int fails = 0;
  std::array<Slot, 128> slots{};
  std::deque<Fire> due;                            // one expiry per clock, in slot order

  uint32_t now() const { return BASE_MS + uint32_t(cyc / CPM); }

  //! one clock: the timer model's expiry, the ms timebase, the arm port
  void tick() {
    if (cyc % CPM == 0) {
      for (size_t s = 0; s < slots.size(); ++s) {
        if (slots[s].armed && int32_t(now() - slots[s].deadline) >= 0) {
          due.push_back({uint8_t(s), slots[s].owner});
          slots[s].armed = false;
        }
      }
    }
    d->tmr_exp_valid_i = due.empty() ? 0 : 1;
    if (!due.empty()) {
      d->tmr_exp_slot_i = due.front().slot;
      d->tmr_exp_owner_i = due.front().owner;
      due.pop_front();
    }
    d->now_ms_i = now();
    d->clk_i = 0;
    d->eval();
    if (d->tmr_arm_valid_o) {
      Slot& s = slots[d->tmr_arm_slot_o];
      s.armed = !d->tmr_arm_cancel_o;
      s.owner = d->tmr_arm_owner_o;
      s.deadline = d->tmr_arm_deadline_ms_o;
    }
    d->clk_i = 1;
    d->eval();
    ++cyc;
  }
  void idle(uint64_t n) {
    for (uint64_t i = 0; i < n; ++i) tick();
  }
  bool ident_presented() const { return d->uns_valid_o && d->uns_kind_o == IDENT_KIND; }

  //! the engine and the MAC for one identify job: take it, retire it with
  //! its frame granted (uns_done_i with uns_tx_busy_i high, as the top's
  //! departure flop is set on the same grant), hold the frame at least SER
  //! clocks, and let its last byte go `phase` clocks into a ms
  Frame frame(uint64_t phase) {
    Frame f;
    for (uint64_t c = 0; c < 2 * REARM && !ident_presented(); ++c) tick();
    if (!ident_presented()) return f;
    f.presented = cyc;
    idle(ACCEPT);
    d->uns_done_i = 1;
    d->uns_tx_busy_i = 1;
    tick();
    d->uns_done_i = 0;
    idle(SER);
    while (cyc % CPM != phase) tick();
    d->uns_tx_busy_i = 0;
    f.departed = cyc;
    return f;
  }
  void quiet_inputs();
  int run();
};

void IdentHarness::quiet_inputs() {
  d->rgy_req_i = 0;
  d->rgy_state_i = 0;
  d->ev_stri_in_i = 0;
  d->ev_stri_out_i = 0;
  d->ev_avb_i = 0;
  d->ev_asp_i = 0;
  d->ev_amap_i = 0;
  d->ev_ctr_i = 0;
  d->ev_cmd_i = 0;
  d->rx_cmd_valid_i = 0;
  d->prng_draw_busy_i = 1;
  d->prng_draw_valid_i = 0;
  d->ca_ready_i = 1;
  d->ca_rsp_valid_i = 0;
  d->ca_fail_valid_i = 0;
  d->uns_done_i = 0;
  d->uns_tx_busy_i = 0;
  d->tmr_exp_valid_i = 0;
  d->identify_button_i = 0;
  d->identify_index_i = 5;
}

int IdentHarness::run() {
  const milan::tb::Model<VKL_aecp_notify> model;
  d = model.get();
  quiet_inputs();
  d->rst_n = 0;
  idle(4);
  d->rst_n = 1;
  idle(CPM);
  //! held from here past the timeout: one burst, then Figure 7-142's re-entry
  d->identify_button_i = 1;
  const Frame f1 = frame(END);
  const Frame f2 = frame(START);
  const Frame f3 = frame(END);
  const Frame f4 = frame(END);
  d->identify_button_i = 0;
  idle(CPM);
  CHECK(f1.presented && f2.presented && f3.presented && f4.presented,
        "FT1: a held press presents a burst's three identify jobs, then at the "
        "timeout a fourth (premise; %d %d %d %d)", f1.presented ? 1 : 0,
        f2.presented ? 1 : 0, f3.presented ? 1 : 0, f4.presented ? 1 : 0);
  if (!(f1.presented && f2.presented && f3.presented && f4.presented)) return 1;
  const uint64_t g12 = f2.presented - f1.departed;
  printf("  [i] FT2: frame 1 left %lu clocks into its ms; frame 2 presented %lu "
         "clocks later\n", static_cast<unsigned long>(f1.departed % CPM),
         static_cast<unsigned long>(g12));
  CHECK(g12 >= BURST && g12 <= BURST + LATE, "FT2: frame 1 left two clocks "
        "before a ms boundary, and frame 2 is presented T-IDENT-BURST after that "
        "boundary, never one tick sooner: %lu clocks, want %lu to %lu",
        static_cast<unsigned long>(g12), static_cast<unsigned long>(BURST),
        static_cast<unsigned long>(BURST + LATE));
  const uint64_t g23 = f3.presented - f2.departed;
  printf("  [i] FT3: frame 2 left on its ms's first clock; frame 3 presented %lu "
         "clocks later\n", static_cast<unsigned long>(g23));
  CHECK(g23 >= BURST && g23 <= BURST + CPM + LATE, "FT3: frame 3 is presented less "
        "than one tick past T-IDENT-BURST after frame 2 left on its ms's first "
        "clock: %lu clocks, want %lu to %lu", static_cast<unsigned long>(g23),
        static_cast<unsigned long>(BURST), static_cast<unsigned long>(BURST + CPM + LATE));
  const uint64_t t14 = f4.presented - f1.departed;
  printf("  [i] FT4: the held button's next burst presented %lu clocks after frame "
         "1 left\n", static_cast<unsigned long>(t14));
  CHECK(t14 >= REARM && t14 <= REARM + LATE, "FT4: held, the next burst starts "
        "at Figure 7-142's timeout, T-IDENT-REARM after the boundary two clocks "
        "after the first frame left, never one tick sooner: %lu clocks, want %lu "
        "to %lu", static_cast<unsigned long>(t14), static_cast<unsigned long>(REARM),
        static_cast<unsigned long>(REARM + LATE));
  return fails ? 1 : 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  //! NOT the canonical tally shape: this binary is ONE of the suite's two
  //! builds, and run_suites.sh reads only the LAST matching line, so a
  //! canonical line here would drop the other build's checks from the total.
  //! The Makefile sums both builds and prints the one canonical line.
#ifdef AECP_NOTIFY_IDENT
  IdentHarness harness;
  const char* const build = "identify";
#else
  Harness harness;
  const char* const build = "default";
#endif
  const int rc = harness.run();
  printf("[build %s] %d checks, %d failures\n", build, harness.checks, harness.fails);
  FILE* acc = fopen("obj_dir/build_tally.txt", "a");
  if (acc == nullptr) {
    printf("FAIL: this build's tally cannot be recorded for the Makefile\n");
    return 1;
  }
  fprintf(acc, "%d %d\n", harness.checks, harness.fails);
  fclose(acc);
  return rc;
}
