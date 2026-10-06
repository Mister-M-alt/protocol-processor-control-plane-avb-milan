// SPDX-License-Identifier: CERN-OHL-W-2.0
// tb/aecp_notify, the third build (AECP_NOTIFY_IF2): KL_aecp_notify with two
// AVB interfaces, P-N-AVB-INTERFACES = 2 (N_IF_P, issue #69). Included by
// sim_main.cpp in that build alone.
//   PT  the registry row's port. Milan v1.2 5.3.4.2: an entry is {Entity ID,
//       MAC address, port, Sequence ID of the next unsolicited notification}.
//       One controller registered on both interfaces holds two entries with
//       two Sequence IDs, and REGISTER and DEREGISTER find the entry of their
//       own port (IEEE 1722.1-2021 7.4.37, 7.4.38; Milan 5.4.2.21, 5.4.2.22).
//   CK  the AVB_INTERFACE counter changes keyed per interface: Milan Table 5.22
//       sends GET_COUNTERS at most once a second per descriptor, so
//       AVB_INTERFACE 1 has a window of its own beside AVB_INTERFACE 0's, and
//       an index past the interfaces is not served.
// The bench is the engine: it retires each job when the section says, and the
// clock advances one ms per cycle while a section watches the job face. Every
// expectation is the clause's, never read back from the DUT.
#include <algorithm>

struct PortHarness {
  static constexpr uint64_t EID_E = 0x5555000000000005ull;
  static constexpr uint64_t MAC_E = 0x020000000005ull;
  static constexpr uint64_t EID_F = 0x6666000000000006ull;
  static constexpr uint64_t MAC_F = 0x020000000006ull;
  //! the requester a command notification excludes: nobody registered
  static constexpr uint64_t EID_X = 0x7777000000000007ull;
  static constexpr uint8_t OP_REGISTER = 0;
  static constexpr uint8_t OP_DEREGISTER = 1;
  static constexpr uint64_t NO_RESOURCES = 1;     //! the rgy result word's refusal
  static constexpr uint64_t UNANSWERED = ~0ull;
  static constexpr uint8_t CLASS_NAME = 7;        //! ev_cmd class: SET_NAME
  static constexpr uint16_t DT_AUDIO_UNIT = 0x0002;
  static constexpr uint16_t DT_CLOCK_DOMAIN = 0x0024;
  VKL_aecp_notify* d = nullptr;
  uint32_t now = 1000;
  int checks = 0;
  int fails = 0;

  struct Job {
    uint32_t ms = 0;                        // first seen in this ms; 0 = none
    uint8_t kind = 0;
    uint16_t dt = 0;
    uint16_t di = 0;
    uint16_t seq = 0;
    uint64_t mac = 0;
  };

  void tick() {
    d->now_ms_i = now;
    d->clk_i = 0;
    d->eval();
    d->clk_i = 1;
    d->eval();
  }
  void idle(int n) {
    for (int i = 0; i < n; ++i) tick();
  }
  void warm_reset() {
    d->rst_n = 0;
    idle(2);
    d->rst_n = 1;
    idle(2);
  }

  //! one registry op from {eid, mac} on `port`: the result word, or UNANSWERED
  uint64_t op(uint8_t code, uint64_t eid, uint64_t mac, uint8_t port) {
    d->rgy_state_i = 0;
    d->rgy_op_i = code;
    d->rgy_eid_i = eid;
    d->rgy_mac_i = mac;
    d->rgy_port_i = port;
    d->rgy_tl_i = 0;
    d->rgy_req_i = 1;
    uint64_t result = UNANSWERED;
    for (int i = 0; i < REGISTRY_ACCEPT_CYCLES; ++i) {
      d->clk_i = 0;
      d->eval();
      if (!d->rgy_wait_o) {
        result = d->rgy_data_o;
        break;
      }
      tick();
    }
    tick();
    d->rgy_req_i = 0;
    idle(2);
    return result;
  }
  unsigned rows() const { return d->dbg_reg_cnt_o; }

  //! cycles at one ms each until a job is presented, up to ms `last`
  Job presented(uint32_t last) {
    while (now <= last) {
      d->clk_i = 0;
      d->eval();
      if (d->uns_valid_o) {
        return {now, d->uns_kind_o, d->uns_desc_type_o, d->uns_desc_index_o, d->uns_seq_o,
                d->uns_mac_o};
      }
      tick();
      ++now;
    }
    return {};
  }
  //! the presented job's frame is granted now
  void retire() {
    d->uns_done_i = 1;
    tick();
    d->uns_done_i = 0;
    ++now;
  }
  //! every job presented up to ms `last`, each retired as soon as it shows
  std::vector<Job> round(uint32_t last) {
    std::vector<Job> jobs;
    for (Job j = presented(last); j.ms != 0; j = presented(last)) {
      jobs.push_back(j);
      retire();
    }
    return jobs;
  }
  //! a successful SET_NAME on AUDIO_UNIT 0 from a requester nobody registered
  void name_changed() {
    d->ev_cmd_class_i = CLASS_NAME;
    d->ev_cmd_type_i = DT_AUDIO_UNIT;
    d->ev_cmd_index_i = 0;
    d->ev_cmd_excl_eid_i = EID_X;
    d->ev_cmd_i = 1;
    tick();
    d->ev_cmd_i = 0;
    ++now;
  }
  void counter_changed(uint16_t dt, uint16_t di) {
    d->ev_ctr_type_i = dt;
    d->ev_ctr_index_i = di;
    d->ev_ctr_i = 1;
    tick();
    d->ev_ctr_i = 0;
    ++now;
  }
  static std::vector<uint16_t> seqs_to(const std::vector<Job>& jobs, uint64_t mac) {
    std::vector<uint16_t> s;
    for (const Job& j : jobs) {
      if (j.mac == mac) s.push_back(j.seq);
    }
    std::sort(s.begin(), s.end());
    return s;
  }
  void quiet_inputs();
  void port_tuple();
  void keyed_counters();
  int run();
};

void PortHarness::quiet_inputs() {
  d->rgy_req_i = 0;
  d->rgy_state_i = 0;
  d->rgy_port_i = 0;
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
}

// PT: E registers on port 0, a round reaches it, then E registers on port 1 as
// well (PT2). Two entries with Sequence IDs of their own: the next round reaches E
// twice, at 1 (port 0's entry) and at 0 (port 1's). A repeated REGISTER on
// port 1 refreshes that entry; with both rows held a third tuple finds none.
// DEREGISTER on port 1 leaves port 0's entry: the next round reaches E once,
// at 2. A mutant that drops the port from the match makes port 1's REGISTER a
// refresh of port 0's entry (PT2); one that stores no port makes the refresh a
// new claim the full table refuses (PT3).
void PortHarness::port_tuple() {
  const uint64_t r0 = op(OP_REGISTER, EID_E, MAC_E, 0);
  const unsigned n0 = rows();
  name_changed();
  const std::vector<Job> first = round(now + 4 * WALK_MS);
  const uint64_t r1 = op(OP_REGISTER, EID_E, MAC_E, 1);
  CHECK(r0 == 0 && n0 == 1 && r1 == 0 && rows() == 2,
        "PT2: REGISTER from E on port 0 holds one entry, and the same Entity ID and MAC on "
        "port 1 is a second (results %llu and %llu, %u then %u entries)",
        static_cast<unsigned long long>(r0), static_cast<unsigned long long>(r1), n0, rows());
  name_changed();
  const std::vector<Job> second = round(now + 4 * WALK_MS);
  const std::vector<uint16_t> s1 = seqs_to(first, MAC_E);
  const std::vector<uint16_t> s2 = seqs_to(second, MAC_E);
  printf("  [i] PT4: round 1 reached E %zu time(s), round 2 %zu time(s)\n", s1.size(),
         s2.size());
  CHECK(s1 == std::vector<uint16_t>{0} && s2 == std::vector<uint16_t>({0, 1}),
        "PT4: one notification per entry, each with its own Sequence ID: round 1 reaches E "
        "once at 0, round 2 twice, at 1 (port 0's entry) and 0 (port 1's)");
  const uint64_t r2 = op(OP_REGISTER, EID_E, MAC_E, 1);
  const uint64_t r3 = op(OP_REGISTER, EID_F, MAC_F, 0);
  CHECK(r2 == 0 && r3 == NO_RESOURCES && rows() == 2,
        "PT3: a repeated REGISTER on port 1 refreshes port 1's entry, and with both rows held "
        "a third tuple is refused NO_RESOURCES (results %llu and %llu, %u entries)",
        static_cast<unsigned long long>(r2), static_cast<unsigned long long>(r3), rows());
  const uint64_t r4 = op(OP_DEREGISTER, EID_E, MAC_E, 1);
  CHECK(r4 == 0 && rows() == 1,
        "PT5: DEREGISTER from E on port 1 removes one entry (result %llu, %u entries)",
        static_cast<unsigned long long>(r4), rows());
  name_changed();
  const std::vector<uint16_t> s3 = seqs_to(round(now + 4 * WALK_MS), MAC_E);
  CHECK(s3 == std::vector<uint16_t>{2},
        "PT6: the entry DEREGISTER on port 1 removed was port 1's: the next round reaches E "
        "once, at port 0's Sequence ID 2 (%zu notification(s))", s3.size());
  const uint64_t r5 = op(OP_DEREGISTER, EID_E, MAC_E, 0);
  CHECK(r5 == 0 && rows() == 0,
        "PT7: DEREGISTER from E on port 0 removes the last entry (result %llu, %u entries)",
        static_cast<unsigned long long>(r5), rows());
}

// CK: one registered controller. AVB_INTERFACE 1's change goes out as its own
// GET_COUNTERS (0009:1), and AVB_INTERFACE 0's, in the same second, is not held
// by it; a second change of interface 1 inside its second waits a second from
// interface 1's send. AVB_INTERFACE 2 names no interface of this build and goes
// nowhere, and CLOCK_DOMAIN 0 keeps its slot beside the new one.
void PortHarness::keyed_counters() {
  warm_reset();
  now = 3000;
  (void)op(OP_REGISTER, EID_F, MAC_F, 1);
  counter_changed(DT_AVB_INTERFACE, 1);
  const Job a1 = presented(now + WALK_MS);
  retire();
  CHECK(a1.kind == KIND_CTRS && a1.dt == DT_AVB_INTERFACE && a1.di == 1 && a1.mac == MAC_F,
        "CK1: a change of AVB_INTERFACE 1's counters goes out as GET_COUNTERS on 0009:1 "
        "(kind %u, %04x:%u)", unsigned(a1.kind), unsigned(a1.dt), unsigned(a1.di));
  counter_changed(DT_AVB_INTERFACE, 0);
  const Job a0 = presented(now + WALK_MS);
  retire();
  CHECK(a0.kind == KIND_CTRS && a0.dt == DT_AVB_INTERFACE && a0.di == 0,
        "CK2: AVB_INTERFACE 0's change in the same second goes out at once, on 0009:0: its "
        "window is its own (kind %u, %04x:%u, %u ms after interface 1's)", unsigned(a0.kind),
        unsigned(a0.dt), unsigned(a0.di), a0.ms - a1.ms);
  counter_changed(DT_AVB_INTERFACE, 1);
  const Job held = presented(a1.ms + 1000 + WALK_MS);
  retire();
  CHECK(held.di == 1 && held.ms >= a1.ms + 1000 && held.ms <= a1.ms + 1000 + WALK_MS,
        "CK3: a second change of AVB_INTERFACE 1 inside its second waits a second from "
        "interface 1's send: presented at ms %u on index %u, want %u to %u on index 1", held.ms,
        unsigned(held.di), a1.ms + 1000, a1.ms + 1000 + WALK_MS);
  counter_changed(DT_AVB_INTERFACE, 2);
  const Job none = presented(now + 2000);
  CHECK(none.ms == 0, "CK4: AVB_INTERFACE 2 names no interface of a two-interface build: no "
        "GET_COUNTERS in two seconds (one presented on %04x:%u)", unsigned(none.dt),
        unsigned(none.di));
  counter_changed(DT_CLOCK_DOMAIN, 0);
  const Job cd = presented(now + WALK_MS);
  retire();
  CHECK(cd.kind == KIND_CTRS && cd.dt == DT_CLOCK_DOMAIN && cd.di == 0,
        "CK5: CLOCK_DOMAIN 0 keeps its own slot beside the interfaces' (kind %u, %04x:%u)",
        unsigned(cd.kind), unsigned(cd.dt), unsigned(cd.di));
}

int PortHarness::run() {
  const milan::tb::Model<VKL_aecp_notify> model;
  d = model.get();
  quiet_inputs();
  d->rst_n = 0;
  idle(4);
  d->rst_n = 1;
  idle(2);
  port_tuple();
  keyed_counters();
  return fails ? 1 : 0;
}
