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
//   PD  the depth per interface: Milan 5.3.4.2 "at least 16 controllers per AVB
//       Interface", so each interface has P-N-CONTROLLERS rows of its own (here
//       2). Row r is {index, port}: it owns timer slot REGMON_BASE + r for
//       TIME_LIMITED and REGMON_BASE + N_ROW + r for the monitor, and the
//       index is its owner tags' entry and its CA owner.
//   CA  the availability probes (Milan 5.4.5.3) when one controller holds a row
//       on each interface: a command from it supersedes every live probe of its
//       rows, so each is cancelled; a superseded probe's late response or
//       failure touches nothing; the rows of one index share a CA owner, so they
//       take turns, and a cancelled exchange's report cannot land on the next.
// The bench is the engine, the PRNG, the timer service and the CA builder with
// the originator behind it: it retires each job when the section says, and the
// clock advances one ms per cycle while a section watches the job face. Every
// expectation is the clause's, never read back from the DUT.
#include <algorithm>

struct PortHarness {
  static constexpr uint64_t EID_E = 0x5555000000000005ull;
  static constexpr uint64_t MAC_E = 0x020000000005ull;
  static constexpr uint64_t EID_F = 0x6666000000000006ull;
  static constexpr uint64_t MAC_F = 0x020000000006ull;
  //! more controllers for PD and CA: entity id 0x99..0n, MAC 02:00:00:00:00:0n
  static constexpr uint64_t eid_of(unsigned n) { return 0x9999000000000000ull | n; }
  static constexpr uint64_t mac_of(unsigned n) { return 0x020000000000ull | n; }
  static constexpr unsigned N_ROW = 2 * N_CTRL;   //! rows: P-N-CONTROLLERS per interface
  static constexpr unsigned row_of(unsigned index, unsigned port) { return 2 * index + port; }
  static constexpr uint8_t tl_slot(unsigned r) { return uint8_t(REGMON_BASE + r); }
  static constexpr uint8_t mon_slot(unsigned r) { return uint8_t(REGMON_BASE + N_ROW + r); }
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
  //! every timer arm or cancel the block issues, in order (PD)
  struct Arm {
    bool mon;                               // the monitor's arm port, else the registry's
    bool cancel;
    unsigned slot;
    unsigned owner;
    bool operator==(const Arm& o) const {
      return mon == o.mon && cancel == o.cancel && slot == o.slot && owner == o.owner;
    }
  };
  std::vector<Arm> arms;
  //! what the CA face did over a watch (CA)
  struct Face {
    std::vector<std::pair<unsigned, uint64_t>> probes;   // {owner, controller}, in order
    std::vector<long> probe_at;                          // the cycle of each
    std::vector<unsigned> cancels;                       // owners, in order
    std::vector<long> cancel_at;
  };
  long cyc = 0;

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
    ++cyc;
    if (d->tmr_arm_valid_o)
      arms.push_back({false, d->tmr_arm_cancel_o != 0, d->tmr_arm_slot_o, d->tmr_arm_owner_o});
    if (d->mon_arm_valid_o)
      arms.push_back({true, d->mon_arm_cancel_o != 0, d->mon_arm_slot_o, d->mon_arm_owner_o});
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
  uint64_t op(uint8_t code, uint64_t eid, uint64_t mac, uint8_t port, bool tl = false) {
    d->rgy_state_i = 0;
    d->rgy_op_i = code;
    d->rgy_eid_i = eid;
    d->rgy_mac_i = mac;
    d->rgy_port_i = port;
    d->rgy_tl_i = tl;
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
  //! the PRNG: every monitor draw requested over `cycles`, each answered
  //! 30 s; returns how many were requested
  int serve_draws(int cycles) {
    int draws = 0;
    d->prng_draw_busy_i = 0;
    for (int c = 0; c < cycles; ++c) {
      d->clk_i = 0;
      d->eval();
      if (d->prng_draw_req_o) {
        ++draws;
        tick();
        d->prng_draw_ms_i = 30000;
        d->prng_draw_valid_i = 1;
        tick();
        d->prng_draw_valid_i = 0;
        continue;
      }
      tick();
    }
    d->prng_draw_busy_i = 1;
    return draws;
  }
  //! the timer service: one expiry of `slot` armed by `owner`
  void expire(unsigned slot, unsigned owner) {
    d->tmr_exp_slot_i = slot;
    d->tmr_exp_owner_i = owner;
    d->tmr_exp_valid_i = 1;
    tick();
    d->tmr_exp_valid_i = 0;
    idle(2);
  }
  //! row r's monitor deadline expires, the probe held until the builder is ready
  void monitor_due(unsigned r) {
    d->ca_ready_i = 0;
    expire(mon_slot(r), OWN_MON | (r / 2));
  }
  //! one clock of the CA face watched (probes taken, cancels sent), with a
  //! command from {eid, mac} when `command`
  void watch(Face& f, bool command = false, uint64_t eid = 0, uint64_t mac = 0) {
    d->rx_cmd_eid_i = eid;
    d->rx_cmd_mac_i = mac;
    d->rx_cmd_valid_i = command;
    d->clk_i = 0;
    d->eval();
    if (d->ca_valid_o && d->ca_ready_i) {
      f.probes.push_back({d->ca_owner_o, d->ca_ctlr_eid_o});
      f.probe_at.push_back(cyc);
    }
    if (d->ca_cancel_valid_o) {
      f.cancels.push_back(d->ca_cancel_owner_o);
      f.cancel_at.push_back(cyc);
    }
    tick();
    d->rx_cmd_valid_i = 0;
  }
  void watch_for(Face& f, int cycles) {
    for (int c = 0; c < cycles; ++c) watch(f);
  }
  //! the originator reports a response (else a failure) of `owner`'s exchange
  void report(bool response, unsigned owner) {
    d->ca_rsp_owner_i = owner;
    d->ca_fail_owner_i = owner;
    d->ca_rsp_valid_i = response;
    d->ca_fail_valid_i = !response;
  }
  void report_end() {
    d->ca_rsp_valid_i = 0;
    d->ca_fail_valid_i = 0;
  }
  static unsigned mask(const std::vector<unsigned>& owners) {
    unsigned m = 0;
    for (unsigned o : owners) m |= 1u << o;
    return m;
  }
  static unsigned mask(const std::vector<std::pair<unsigned, uint64_t>>& probes) {
    unsigned m = 0;
    for (const auto& p : probes) m |= 1u << p.first;
    return m;
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
  void depth_per_interface();
  void cancel_every_probe();
  void owner_turns();
  void settle_after_cancel();
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
// port 1 refreshes that entry. DEREGISTER on port 1 leaves port 0's entry: the
// next round reaches E once, at 2. A mutant that drops the port from the match
// makes port 1's REGISTER a refresh of port 0's entry (PT2); one that stores no
// port makes the refresh a third entry (PT3).
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
  CHECK(r2 == 0 && rows() == 2,
        "PT3: a repeated REGISTER on port 1 refreshes port 1's entry: still two entries "
        "(result %llu, %u entries)", static_cast<unsigned long long>(r2), rows());
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

// PD: each interface holds P-N-CONTROLLERS entries of its own (PD1): two
// controllers register on port 0, a third there is refused NO_RESOURCES while
// port 1 still takes two, and refuses its third. Every one is TIME_LIMITED, so
// each REGISTER arms its row's slot (PD2): row r = {index, port} owns
// REGMON_BASE + r with owner tag 0xA0 + index, and its monitor draw arms
// REGMON_BASE + N_ROW + r with 0xD0 + index. An expiry is decoded to its row
// from its tag and slot (PD3): row 3's TIME_LIMITED deadline removes row 3
// alone, with its DEREGISTER to its controller, though row 2 has the same tag,
// and row 1's monitor deadline probes row 1's controller, though row 0 has the
// same tag. Mutants: a REGISTER that claims any free row, a registry of
// P-N-CONTROLLERS rows in all (PD1); owner tags from the row's port bits (PD2);
// an expiry decoded without its port (PD3).
void PortHarness::depth_per_interface() {
  warm_reset();
  now = 7000;
  arms.clear();
  // rows: A 1 = {0, 1}, B 0 = {0, 0}, C 2 = {1, 0}, E 3 = {1, 1}
  const uint64_t a = op(OP_REGISTER, eid_of(1), mac_of(1), 1, true);
  const uint64_t b = op(OP_REGISTER, eid_of(2), mac_of(2), 0, true);
  const uint64_t c = op(OP_REGISTER, eid_of(3), mac_of(3), 0, true);
  const uint64_t full0 = op(OP_REGISTER, eid_of(4), mac_of(4), 0, true);
  const uint64_t e = op(OP_REGISTER, eid_of(5), mac_of(5), 1, true);
  const uint64_t full1 = op(OP_REGISTER, eid_of(6), mac_of(6), 1, true);
  CHECK(a == 0 && b == 0 && c == 0 && full0 == NO_RESOURCES && e == 0
        && full1 == NO_RESOURCES && rows() == N_ROW,
        "PD1: each interface holds P-N-CONTROLLERS (2) entries of its own: two REGISTERs on "
        "port 0 succeed and a third is refused NO_RESOURCES, then port 1 takes two and refuses "
        "its third (results %llu %llu %llu %llu %llu %llu, %u entries)",
        static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
        static_cast<unsigned long long>(c), static_cast<unsigned long long>(full0),
        static_cast<unsigned long long>(e), static_cast<unsigned long long>(full1), rows());
  const int draws = serve_draws(40);
  const std::vector<Arm> want = {
      {false, false, tl_slot(1), OWN_TL | 0u}, {false, false, tl_slot(0), OWN_TL | 0u},
      {false, false, tl_slot(2), OWN_TL | 1u}, {false, false, tl_slot(3), OWN_TL | 1u},
      {true, false, mon_slot(0), OWN_MON | 0u}, {true, false, mon_slot(1), OWN_MON | 0u},
      {true, false, mon_slot(2), OWN_MON | 1u}, {true, false, mon_slot(3), OWN_MON | 1u}};
  CHECK(draws == 4 && arms == want,
        "PD2: row r = {index, port} arms TIME_LIMITED slot %u + r with owner 0x%02x + index, "
        "and its monitor slot %u + r with 0x%02x + index (%d draws, %zu arms)",
        unsigned(REGMON_BASE), unsigned(OWN_TL), unsigned(REGMON_BASE + N_ROW),
        unsigned(OWN_MON), draws, arms.size());
  if (!(arms == want)) {
    for (const Arm& x : arms)
      printf("  [i] PD2 arm: %s %s slot %u owner 0x%02x\n", x.mon ? "monitor " : "registry",
             x.cancel ? "cancel" : "arm   ", x.slot, x.owner);
  }
  expire(tl_slot(3), OWN_TL | 1u);
  idle(DRAIN_WATCH_CYCLES);
  const unsigned left = rows();
  const Job j = presented(now + WALK_MS);
  retire();
  monitor_due(row_of(0, 1));
  d->ca_ready_i = 1;
  Face f;
  watch_for(f, 12);
  CHECK(left == N_ROW - 1 && j.kind == KIND_DEREG && j.mac == mac_of(5) && f.probes.size() == 1
        && f.probes[0].first == 0 && f.probes[0].second == eid_of(1),
        "PD3: an expiry is decoded to its row from tag and slot: row 3's TIME_LIMITED deadline "
        "removes row 3 alone (%u entries, DEREGISTER to %012llx), and row 1's monitor deadline "
        "probes row 1's controller, CA owner 0 (%zu probe(s))", left,
        static_cast<unsigned long long>(j.mac), f.probes.size());
}

// CA1: X registers on port 1 (row 1), then E on port 0 (row 0) and on port 1
// (row 3), so E's entries have CA owners 0 and 1. Both of E's monitor deadlines
// expire and both probes go out. One command from E proves it alive on either
// interface, so it supersedes both, and both exchanges are cancelled, one per
// cycle: R512-1's P3, where one command cancelled one probe and the other's
// failure removed E's port-1 entry. CA2: the cancelled exchanges' reports then
// arrive late, a failure and a response of each owner: no entry goes, no
// DEREGISTER is queued, and no draw is asked but the two E's command asked.
// CA1b: a TIME_LIMITED drain's cancel and a command's in the drain's cycle are
// both sent. Mutants: a command's second cancel dropped (CA1, CA1b); a
// failure, or a response, taken for a row whose probe is not live (CA2).
void PortHarness::cancel_every_probe() {
  warm_reset();
  const uint64_t EID_X = eid_of(7);
  const uint64_t MAC_X = mac_of(7);
  (void)op(OP_REGISTER, EID_X, MAC_X, 1);              // row 1
  (void)op(OP_REGISTER, EID_E, MAC_E, 0);              // row 0
  (void)op(OP_REGISTER, EID_E, MAC_E, 1);              // row 3
  const int draws = serve_draws(40);
  monitor_due(row_of(0, 0));
  monitor_due(row_of(1, 1));
  d->ca_ready_i = 1;
  Face out;
  watch_for(out, 20);
  Face cmd;
  watch(cmd, true, EID_E, MAC_E);
  watch_for(cmd, 15);
  printf("  [i] CA1: %d draws; probes to owners 0x%x; one command from E: %zu cancel(s), owners "
         "0x%x\n", draws, mask(out.probes), cmd.cancels.size(), mask(cmd.cancels));
  CHECK(rows() == 3 && out.probes.size() == 2 && mask(out.probes) == 0x3
        && out.probes[0].second == EID_E && out.probes[1].second == EID_E
        && cmd.cancels.size() == 2 && mask(cmd.cancels) == 0x3,
        "CA1: E holds an entry on each port and both probes are out (owners 0x%x); one command "
        "from E cancels both exchanges (%zu cancel(s), owners 0x%x)", mask(out.probes),
        cmd.cancels.size(), mask(cmd.cancels));
  for (unsigned o = 0; o < 2; ++o) {
    report(false, o);
    tick();
    report(true, o);
    tick();
  }
  report_end();
  idle(DRAIN_WATCH_CYCLES);
  const unsigned left = rows();
  const Job j = presented(now + WALK_MS);
  const int redraws = serve_draws(40);
  CHECK(left == 3 && j.ms == 0 && redraws == 2,
        "CA2: the cancelled exchanges' late failures and responses touch nothing: %u entries "
        "(want 3), %s, and %d draw(s), the two E's command asked", left,
        j.ms == 0 ? "no job" : "a job presented", redraws);

  warm_reset();
  (void)op(OP_REGISTER, eid_of(8), mac_of(8), 0, true);  // row 0, TIME_LIMITED
  (void)op(OP_REGISTER, EID_F, MAC_F, 0);                // row 2
  (void)serve_draws(40);
  monitor_due(row_of(0, 0));
  monitor_due(row_of(1, 0));
  d->ca_ready_i = 1;
  Face both;
  watch_for(both, 20);
  const size_t probes = both.probes.size();
  d->tmr_exp_slot_i = tl_slot(0);
  d->tmr_exp_owner_i = OWN_TL | 0u;
  d->tmr_exp_valid_i = 1;
  tick();
  d->tmr_exp_valid_i = 0;
  bool met = false;
  for (int c = 0; c < DRAIN_WATCH_CYCLES; ++c) {
    d->rx_cmd_valid_i = 0;
    d->clk_i = 0;
    d->eval();
    const bool drain = !met && d->ca_cancel_valid_o && d->ca_cancel_owner_o == 0;
    met = met || drain;
    watch(both, drain, EID_F, MAC_F);
  }
  CHECK(probes == 2 && met && both.cancels.size() == 2 && mask(both.cancels) == 0x3
        && rows() == 1,
        "CA1b: row 0's TIME_LIMITED drain cancels its probe, and F's command in the drain's "
        "cycle cancels F's too (%zu cancel(s), owners 0x%x, %u entries)", both.cancels.size(),
        mask(both.cancels), rows());
}

// CA3: A on port 0 and B on port 1 hold rows 0 and 1, CA owner 0 both. Both
// monitor deadlines expire together: A's probe goes out and B's waits while
// owner 0 has an exchange, since two exchanges of one owner could not be told
// apart. A's exchange fails: A's row goes, with its DEREGISTER, and B's probe
// follows. Mutants: no turns (two probes of owner 0 at once); a failure taken
// for a row whose probe is not live (B's row goes instead).
void PortHarness::owner_turns() {
  warm_reset();
  const uint64_t EID_A = eid_of(9);
  const uint64_t MAC_A = mac_of(9);
  const uint64_t EID_B = eid_of(10);
  (void)op(OP_REGISTER, EID_A, MAC_A, 0);              // row 0
  (void)op(OP_REGISTER, EID_B, mac_of(10), 1);         // row 1
  (void)serve_draws(40);
  monitor_due(row_of(0, 0));
  monitor_due(row_of(0, 1));
  d->ca_ready_i = 1;
  Face first;
  watch_for(first, 40);
  report(false, 0);
  tick();
  report_end();
  Face after;
  watch_for(after, 40);
  const Job j = presented(now + WALK_MS);
  retire();
  CHECK(first.probes.size() == 1 && first.probes[0].first == 0 && first.probes[0].second == EID_A
        && after.probes.size() == 1 && after.probes[0].first == 0
        && after.probes[0].second == EID_B && rows() == 1 && j.kind == KIND_DEREG
        && j.mac == MAC_A,
        "CA3: the rows of one index take turns: A's probe goes out alone (%zu probe(s)), its "
        "failure removes A (DEREGISTER to %012llx, %u entries), and B's probe follows (%zu)",
        first.probes.size(), static_cast<unsigned long long>(j.mac), rows(),
        after.probes.size());
}

// CA4: X on port 0 and Y on port 1 share CA owner 0; X's probe is out and Y's
// is due. A command from X cancels X's exchange, and the originator's response
// to it, which raced the cancel, arrives two cycles later. Y's probe waits out
// the settle, so that response finds no live probe of owner 0 and touches
// nothing: it asks no draw (X's command asked the one draw), and Y's probe,
// taken at least four cycles after the cancel, stays live, so its own failure
// removes Y. Mutant: no settle (Y's probe at once, the response taken as Y's).
void PortHarness::settle_after_cancel() {
  warm_reset();
  const uint64_t EID_X = eid_of(11);
  const uint64_t MAC_X = mac_of(11);
  const uint64_t EID_Y = eid_of(12);
  const uint64_t MAC_Y = mac_of(12);
  (void)op(OP_REGISTER, EID_X, MAC_X, 0);              // row 0
  (void)op(OP_REGISTER, EID_Y, MAC_Y, 1);              // row 1
  (void)serve_draws(40);
  monitor_due(row_of(0, 0));
  monitor_due(row_of(0, 1));
  d->ca_ready_i = 1;
  Face first;
  watch_for(first, 10);
  Face f;
  watch(f, true, EID_X, MAC_X);                        // the cancel's cycle
  watch(f);
  report(true, 0);
  watch(f);                                            // two cycles later
  report_end();
  watch_for(f, 10);
  const int draws = serve_draws(40);
  report(false, 0);
  tick();
  report_end();
  idle(DRAIN_WATCH_CYCLES);
  const unsigned left = rows();
  const Job j = presented(now + WALK_MS);
  retire();
  const long gap = (f.probe_at.empty() || f.cancel_at.empty()) ? -1
                                                               : f.probe_at[0] - f.cancel_at[0];
  CHECK(first.probes.size() == 1 && first.probes[0].second == EID_X && f.cancels.size() == 1
        && f.probes.size() == 1 && f.probes[0].first == 0 && f.probes[0].second == EID_Y
        && gap >= 4 && draws == 1 && left == 1 && j.kind == KIND_DEREG && j.mac == MAC_Y,
        "CA4: after X's cancel, Y's probe of the same owner waits out the settle (taken %ld "
        "cycle(s) after the cancel, want 4 or more), so the cancelled exchange's response two "
        "cycles after it asks no draw (%d draw(s), want X's 1) and Y's own failure removes Y "
        "(%u entries, DEREGISTER to %012llx)", gap, draws, left,
        static_cast<unsigned long long>(j.mac));
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
  depth_per_interface();
  cancel_every_probe();
  owner_turns();
  settle_after_cancel();
  return fails ? 1 : 0;
}
