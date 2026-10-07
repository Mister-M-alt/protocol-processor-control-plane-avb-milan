// SPDX-License-Identifier: CERN-OHL-W-2.0
// pp_top suite, section IF (issue #69): the seventh build, whose top has
// P-N-AVB-INTERFACES = 2 and whose wrap drives rx_if_index_i. Included by
// sim_main.cpp in that build alone, after the notification sections whose
// bench (NotifyBench) it boots. It grades the redundancy seam end to end,
// through the top's own glue rather than a module's ports:
//   IF1  both advertise machines run: one ENTITY_AVAILABLE per interface,
//        byte-exact but for its interface_index (IEEE 1722.1-2021 6.2.2.18);
//   IF2  the RX frame's interface reaches ADP: ENTITY_DISCOVER received on one
//        interface, both machines WAITING, restarts that interface's machine
//        alone (Milan v1.2 Table 5.51, RCV_ADP_DISCOVER in WAITING);
//   IF3  the registry tuple's port comes from the command's interface (Milan
//        5.3.4.2), not from the latest frame's: a REGISTER on interface 1
//        waits while a frame arrives on interface 0, and still makes a second
//        entry, so a lock change reaches the controller twice, each entry at
//        its own sequence_id (5.4.5.1); IF3b: a DEREGISTER on interface 1, held
//        the same way, removes interface 1's entry, the one whose sequence_id
//        is the lower.
// Every expectation is built here from the clause byte offsets, never read
// back from the DUT.

struct InterfacePhase : NotifyBench {
  //! the lock taker: never registered, so every lock change reaches C alone
  static constexpr uint64_t D_MAC = 0x0202D0D0D0D0ull;
  static constexpr uint64_t D_EID = 0x7777000000000044ull;
  static constexpr unsigned ST_WAIT = 3;
  static constexpr uint16_t OP_LOCK = 0x0001;
  static constexpr uint16_t OP_REGISTER = 0x0024;
  static constexpr uint16_t OP_DEREGISTER = 0x0025;
  //! an ENTITY_DISCOVER for an entity that is not this one: every layer up to
  //! the header latch takes it, and ADP ignores it
  static constexpr uint64_t FOREIGN_EID = 0x7777000000000077ull;
  struct Adv {
    std::vector<uint8_t> f;
    uint32_t ms;
  };
  std::vector<Adv> adp;                        // every ADPDU on the wire, in order
  uint16_t seq = 0x6900;

  using NotifyBench::NotifyBench;

  unsigned adv_state(unsigned ifx) const {
    return (unsigned(io.d->dbg_adp_adv_state_o) >> (2 * ifx)) & 3u;
  }
  static unsigned if_of(const std::vector<uint8_t>& f) {
    return f.size() >= 70 ? ((unsigned(f[68]) << 8) | f[69]) : 0xFFFFu;
  }
  //! the expected ENTITY_AVAILABLE of interface `ifx`: own_avail with its
  //! interface_index (wire bytes 68 and 69, F04.5)
  static std::vector<uint8_t> avail_on(unsigned ifx, uint32_t aidx) {
    std::vector<uint8_t> f = own_avail(aidx);
    putbe(&f[68], ifx, 2);
    return f;
  }
  //! one frame on interface `ifx`, the port naming it for the frame's bytes
  //! alone: from the clock after the last byte it names the other interface,
  //! which rx_if_index_i's contract allows (it is read with the last byte), so
  //! a top that read it any later would take the wrong one
  void feed_on(unsigned ifx, const std::vector<uint8_t>& f) {
    for (size_t i = 0; i < f.size(); ++i) {
      io.d->rx_valid_i = 1;
      io.d->rx_data_i = f[i];
      io.d->rx_last_i = (i + 1 == f.size()) ? 1 : 0;
      io.d->rx_if_index_i = ifx;
      tick();
    }
    io.d->rx_valid_i = 0;
    io.d->rx_last_i = 0;
    io.d->rx_if_index_i = 1 - ifx;
    for (int i = 0; i < 4; ++i) tick();
  }
  //! NotifyBench::ask through feed_on: one command from {mac, eid} on
  //! interface `ifx`, and its solicited response
  std::vector<uint8_t> ask_on(unsigned ifx, uint64_t mac, uint64_t eid, uint16_t op,
                              const std::vector<uint8_t>& pl) {
    const uint16_t s = seq++;
    const size_t from = seen.size();
    feed_on(ifx, aecp_frame(OWN_MAC, mac, 0, 0, EID, eid, s, op, pl));
    for (long c = 0; c <= 800L * MS_CYC; ++c) {
      for (size_t i = from; i < seen.size(); ++i) {
        const std::vector<uint8_t>& f = seen[i].f;
        if (da_of(f) == mac && !unsolicited(f) && seq_of(f) == s) return f;
      }
      tick();
    }
    return {};
  }
  bool succeeds(const std::vector<uint8_t>& r) const {
    return !r.empty() && status_of(r) == AECP_SUCCESS;
  }
  //! `ms` of time, every ADPDU the wire carries logged with its ms
  void step_ms(long ms) {
    for (long c = 0; c < ms * MS_CYC; ++c) {
      tick();
      while (!io.q_adp.empty()) {
        adp.push_back({io.q_adp.front(), uint32_t(io.d->dbg_now_ms_o)});
        io.q_adp.pop_front();
      }
    }
  }
  //! the first ADPDU of interface `ifx` logged at or after `from`, if any
  const Adv* first_on(unsigned ifx, size_t from) const {
    for (size_t i = from; i < adp.size(); ++i)
      if (if_of(adp[i].f) == ifx) return &adp[i];
    return nullptr;
  }
  //! the unsolicited LOCK_ENTITY responses at `mac` since log index `from`,
  //! as their sorted sequence_ids
  std::vector<unsigned> lock_pushes(uint64_t mac, size_t from) const {
    std::vector<unsigned> v;
    for (size_t i = from; i < seen.size(); ++i)
      if (da_of(seen[i].f) == mac && unsolicited(seen[i].f) && ct_of(seen[i].f) == OP_LOCK)
        v.push_back(seq_of(seen[i].f));
    std::sort(v.begin(), v.end());
    return v;
  }
  //! D takes (flags 0) or releases (flags 1, UNLOCK) the ENTITY lock; the
  //! sorted sequence_ids of the pushes C then receives
  std::vector<unsigned> lock_change(uint32_t flags) {
    const size_t from = seen.size();
    const std::vector<uint8_t> r = ask_on(0, D_MAC, D_EID, OP_LOCK,
                                          LockPhase::lockpld(flags, 0, 0));
    run_ms(60);                                // the notification walk, well inside 400 ms
    return succeeds(r) ? lock_pushes(CTLR_MAC, from) : std::vector<unsigned>{99};
  }
  //! A command from C that runs only after a later frame arrived on the other
  //! interface. The MAC TX is held, and D's lock change then queues a push to
  //! every entry of C that the TX cannot carry, so the notification block holds
  //! the engine's command path (amap_busy_o) until it leaves. C's command comes
  //! in on `ifx` and an ENTITY_DISCOVER for another entity on the other
  //! interface; then the TX resumes, the push leaves, and C's command runs.
  struct Held {
    bool ok;                                   // C's response: SUCCESS
    bool after;                                // ... and on the wire after every push
    std::vector<unsigned> pushes;              // the lock change's pushes to C
  };
  Held held(uint32_t flags, unsigned ifx, uint16_t op, const std::vector<uint8_t>& pl) {
    const size_t from = seen.size();
    io.mac_tx_ready = false;
    feed_on(0, aecp_frame(OWN_MAC, D_MAC, 0, 0, EID, D_EID, seq++, OP_LOCK,
                          LockPhase::lockpld(flags, 0, 0)));
    run_ms(2);
    const uint16_t s = seq++;
    feed_on(ifx, aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, s, op, pl));
    feed_on(1 - ifx, adp_frame(2, D_MAC, FOREIGN_EID, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    run_ms(2);
    io.mac_tx_ready = true;
    run_ms(60);
    Held h{false, false, lock_pushes(CTLR_MAC, from)};
    size_t last_push = from;
    for (size_t i = from; i < seen.size(); ++i) {
      const std::vector<uint8_t>& f = seen[i].f;
      if (da_of(f) != CTLR_MAC) continue;
      if (unsolicited(f)) {
        last_push = i;
      } else if (seq_of(f) == s) {
        h.ok = status_of(f) == AECP_SUCCESS;
        h.after = i > last_push;
      }
    }
    return h;
  }
  void both_interfaces_advertise();
  void discover_reaches_its_interface();
  void registry_port_from_the_command();
};

// IF1: booted, enabled with the link up: each interface's machine draws its
// T-ADP-DELAY-START and advertises, interface_index 0 and 1, available_index 0.
void InterfacePhase::both_interfaces_advertise() {
  boot_to_idle(true);
  io.d->rx_if_index_i = 0;
  step_ms(2600);
  const Adv* a0 = first_on(0, 0);
  const Adv* a1 = first_on(1, 0);
  CHECK(a0 != nullptr && a1 != nullptr && a0->f == avail_on(0, 0) && a1->f == avail_on(1, 0),
        "IF1: with two interfaces each advertise machine sends its own ENTITY_AVAILABLE "
        "inside T-ADP-DELAY-START, byte-exact with interface_index 0 and 1 and "
        "available_index 0 (%zu ADPDU(s))", adp.size());
  if (a1 != nullptr && a1->f != avail_on(1, 0)) {
    dump("got", a1->f);
    dump("exp", avail_on(1, 0));
  }
}

// IF2: ENTITY_DISCOVER (entity_id 0) received on interface 1, then on interface
// 0, each while both machines are WAITING: the frame's interface rides the
// header beat to ADP, so the receiving interface's machine leaves WAITING and
// advertises inside T-ADP-DELAY, and the other stays WAITING.
void InterfacePhase::discover_reaches_its_interface() {
  const std::vector<uint8_t> disc = adp_frame(2, CTLR_MAC, 0, 0, 0, 0, 0, 0,
                                              0, 0, 0, 0, 0, 0, 0);
  for (unsigned ifx = 2; ifx-- > 0;) {
    const unsigned other = 1 - ifx;
    for (long t = 0; t < 10000 && !(adv_state(0) == ST_WAIT && adv_state(1) == ST_WAIT); ++t)
      step_ms(1);
    const size_t from = adp.size();
    const uint32_t t0 = uint32_t(io.d->dbg_now_ms_o);
    feed_on(ifx, disc);
    step_ms(2);                                // validator, normalizer, dispatch, ADP
    const unsigned s_this = adv_state(ifx);
    const unsigned s_other = adv_state(other);
    const Adv* sent = nullptr;
    for (long t = 0; t < 4200 && sent == nullptr; ++t) {
      step_ms(1);
      sent = first_on(ifx, from);
    }
    CHECK(s_this != ST_WAIT && s_other == ST_WAIT && sent != nullptr && sent->ms - t0 <= 4100,
          "IF2: ENTITY_DISCOVER received on interface %u restarts interface %u's advertise "
          "machine alone: it leaves WAITING (state %u) and advertises %u ms later, inside "
          "T-ADP-DELAY; interface %u stays WAITING (state %u)", ifx, ifx, s_this,
          sent != nullptr ? sent->ms - t0 : 0u, other, s_other);
  }
}

// IF3: C registers from interface 0, and D's lock reaches it at 0. With D's
// unlock held, C's REGISTER comes in on interface 1 and a frame on interface 0
// after it: the unlock reaches C's one entry at 1, and the REGISTER, run after
// that frame, makes interface 1's entry all the same. The next lock and unlock
// reach C twice each, at 0 and 2, then 1 and 3: two entries with sequence_ids
// of their own. A registry port taken from the latest frame's interface, or tied
// to 0, makes the REGISTER a refresh of interface 0's entry (one push each).
// IF3b: with D's next lock held, C's DEREGISTER comes in on interface 1 and a
// frame on interface 0 after it; the lock reaches both entries, at 2 and 4,
// before the DEREGISTER runs, and the unlock then reaches C once, at 5:
// interface 0's entry, so the one removed was interface 1's, whose sequence_id
// was 3. A DEREGISTER that matched interface 0's entry would leave 3.
void InterfacePhase::registry_port_from_the_command() {
  const std::vector<uint8_t> none(4, 0);       // REGISTER's flags: not TIME_LIMITED
  const bool r0 = succeeds(ask_on(0, CTLR_MAC, CTLR_EID, OP_REGISTER, none));
  const std::vector<unsigned> l0 = lock_change(0);
  const Held reg = held(1, 1, OP_REGISTER, none);
  const std::vector<unsigned> l1 = lock_change(0);
  const std::vector<unsigned> u1 = lock_change(1);
  printf("  [i] IF3: the lock reached C %zu time(s); the held unlock %zu; then the lock %zu and "
         "the unlock %zu\n", l0.size(), reg.pushes.size(), l1.size(), u1.size());
  CHECK(r0 && l0 == std::vector<unsigned>{0} && reg.ok && reg.after
        && reg.pushes == std::vector<unsigned>{1} && l1 == std::vector<unsigned>({0, 2})
        && u1 == std::vector<unsigned>({1, 3}),
        "IF3: C's REGISTER on interface 1, run after a frame received on interface 0, makes "
        "interface 1's entry: the next lock reaches C at sequence_id 0 and 2, the unlock at 1 "
        "and 3 (REGISTER %s, after the held push %s)", reg.ok ? "SUCCESS" : "failed",
        reg.after ? "yes" : "no");
  const Held dereg = held(0, 1, OP_DEREGISTER, {});
  const std::vector<unsigned> u2 = lock_change(1);
  CHECK(dereg.ok && dereg.after && dereg.pushes == std::vector<unsigned>({2, 4})
        && u2 == std::vector<unsigned>{5},
        "IF3b: C's DEREGISTER on interface 1, run after a frame received on interface 0, removes "
        "interface 1's entry: the held lock reaches both entries, at 2 and 4, and the unlock "
        "then reaches C once, at interface 0's 5 (%zu and %zu push(es))", dereg.pushes.size(),
        u2.size());
}

[[maybe_unused]] static void run_interfaces(H& h) {
  const int checks0 = h.checks;
  const int fails0 = h.fails;
  InterfacePhase p{h};
  p.both_interfaces_advertise();
  p.discover_reaches_its_interface();
  p.registry_port_from_the_command();
  printf("IF: %d checks, %d failures\n", h.checks - checks0, h.fails - fails0);
}
