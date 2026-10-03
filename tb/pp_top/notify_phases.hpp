// SPDX-License-Identifier: CERN-OHL-W-2.0
// pp_top suite, the notification sections of lane C6 (issues #54, #58, #80,
// #86), each on a fresh processor of its own (the section AD pattern), so the
// main run's clock is untouched:
//   ID   IDENTIFY_NOTIFICATION origination (#54, REQ-AEM-026; IEEE 1722.1-2021
//        7.4.39, 7.5.1 and Figure 7-142; Milan 5.4.5.4), in the third build,
//        whose top has P-EN-IDENTIFY-NOTIFICATION = 1;
//   ID0  the same button on the default build (the parameter at 0): nothing.
// Included by sim_main.cpp after section D3's phases; every expectation is
// built here from the clause byte offsets, never read back from the DUT.

// ---------------------------------------------------------------------------
// the bench every section below boots
// ---------------------------------------------------------------------------
//! A fresh processor: its own model, the suite's descriptor image, an erased
//! NVM device and both restore walks to their terminal, then link up and
//! enable. Every AECP frame the wire carries is logged with the clock its
//! last byte left on, so spacing is read off the wire and nothing a section
//! does not look at is ever dropped.
struct NotifyBench {
  H& h;                                        // the tally
  const milan::tb::Model<Vpp_top_wrap> model;
  H io;
  struct Seen {
    std::vector<uint8_t> f;
    uint64_t t;
  };
  std::vector<Seen> seen;
  std::vector<Seen> seen_acmp;                 // every ACMP frame, likewise
  //! the IDENT-BURST singleton on the timer buses (pp_top_wrap's taps): each
  //! arm with its ms deadline, and each expiry, the end of a T-IDENT-BURST gap
  struct GapArm {
    uint64_t t;
    uint32_t deadline;
  };
  std::vector<GapArm> gap_arms;
  std::vector<uint64_t> gap_ends;
  //! the latest ms boundary of the timebase: the ms it began and its clock
  uint32_t edge_ms = 0;
  uint64_t edge_t = 0;

  explicit NotifyBench(H& tally) : h(tally), io(model.get()) {}

  static uint64_t da_of(const std::vector<uint8_t>& f) {
    return f.size() >= 6 ? (rd64(&f[0]) >> 16) : 0;
  }
  static unsigned seq_of(const std::vector<uint8_t>& f) {
    return f.size() >= 36 ? ((unsigned(f[34]) << 8) | f[35]) : 0x10000u;
  }
  static bool unsolicited(const std::vector<uint8_t>& f) {
    return f.size() >= 38 && (f[36] & 0x80) != 0;
  }
  static unsigned status_of(const std::vector<uint8_t>& f) {
    return f.size() > 16 ? unsigned((f[16] >> 3) & 0x1F) : 0xFFu;
  }
  //! command_type without the u bit
  static unsigned ct_of(const std::vector<uint8_t>& f) {
    return f.size() >= 38 ? (((unsigned(f[36]) & 0x7F) << 8) | f[37]) : 0x10000u;
  }

  void tick() {
    io.step();
    if (io.d->dbg_ident_gap_arm_o) gap_arms.push_back({io.t, io.d->dbg_ident_gap_deadline_o});
    if (io.d->dbg_ident_gap_end_o) gap_ends.push_back(io.t);
    if (io.d->dbg_now_ms_o != edge_ms) {
      edge_ms = io.d->dbg_now_ms_o;
      edge_t = io.t;
    }
    while (!io.q_aecp.empty()) {
      seen.push_back({io.q_aecp.front(), io.t});
      io.q_aecp.pop_front();
    }
    while (!io.q_acmp.empty()) {
      seen_acmp.push_back({io.q_acmp.front(), io.t});
      io.q_acmp.pop_front();
    }
  }
  void run_ms(long ms) {
    for (long c = 0; c < ms * MS_CYC; ++c) tick();
  }
  //! H::feed, clocked through tick() so a frame sent meanwhile is logged
  void feed(const std::vector<uint8_t>& f) {
    for (size_t i = 0; i < f.size(); ++i) {
      io.d->rx_valid_i = 1;
      io.d->rx_data_i = f[i];
      io.d->rx_last_i = (i + 1 == f.size()) ? 1 : 0;
      tick();
    }
    io.d->rx_valid_i = 0;
    io.d->rx_last_i = 0;
    for (int i = 0; i < 4; ++i) tick();
  }

  void boot_to_idle(bool restore) {
    Suite image(io);
    image.load_descriptor_image();
    io.erase_nvm();
    io.reset();
    seen.clear();
    if (restore) restore_walks();
  }
  void restore_walks() {
    io.d->restore_go_i = 1;
    for (int c = 0; c < 5; ++c) tick();
    io.d->restore_go_i = 0;
    for (long c = 0; c < 400000 && !io.d->restore_done_o; ++c) tick();
    CHECK(io.d->restore_done_o && !io.d->restore_fail_o,
          "notify bench: blank NVM, both restore walks reach done");
    io.d->link_up_i = 1;
    io.d->entity_enable_i = 1;
    run_ms(10);
  }

  //! one command from {mac, eid}; returns its solicited response (u = 0,
  //! addressed back to mac, same sequence_id), leaving everything else in
  //! the log
  std::vector<uint8_t> ask(uint64_t mac, uint64_t eid, uint16_t seq,
                           uint16_t op, const std::vector<uint8_t>& pl,
                           int budget_ms = 800) {
    const size_t from = seen.size();
    feed(aecp_frame(OWN_MAC, mac, 0, 0, EID, eid, seq, op, pl));
    for (long c = 0; c <= budget_ms * MS_CYC; ++c) {
      for (size_t i = from; i < seen.size(); ++i) {
        const auto& f = seen[i].f;
        if (da_of(f) == mac && !unsolicited(f) && seq_of(f) == seq) return f;
      }
      tick();
    }
    return {};
  }
  //! REGISTER_UNSOLICITED_NOTIFICATION (2021 format, flags 0); true on SUCCESS
  bool register_controller(uint64_t mac, uint64_t eid, uint16_t seq) {
    const auto r = ask(mac, eid, seq, 0x0024, std::vector<uint8_t>(4, 0));
    return !r.empty() && status_of(r) == AECP_SUCCESS;
  }
  bool deregister_controller(uint64_t mac, uint64_t eid, uint16_t seq) {
    const auto r = ask(mac, eid, seq, 0x0025, {});
    return !r.empty() && status_of(r) == AECP_SUCCESS;
  }
  //! frames addressed to `mac` at or after log index `from`
  std::vector<Seen> to_mac(uint64_t mac, size_t from) const {
    std::vector<Seen> v;
    for (size_t i = from; i < seen.size(); ++i)
      if (da_of(seen[i].f) == mac) v.push_back(seen[i]);
    return v;
  }
  //! ...and their log indices
  std::vector<size_t> at_mac(uint64_t mac, size_t from) const {
    std::vector<size_t> v;
    for (size_t i = from; i < seen.size(); ++i)
      if (da_of(seen[i].f) == mac) v.push_back(i);
    return v;
  }
  //! the entry's sequence_id model, from the wire alone: how many unsolicited
  //! frames reached `mac` before log index `until` (Milan 5.4.5.1: +1 per
  //! notification sent to the entry)
  unsigned notified_before(uint64_t mac, size_t until) const {
    unsigned n = 0;
    for (size_t i = 0; i < until && i < seen.size(); ++i)
      n += (da_of(seen[i].f) == mac && unsolicited(seen[i].f)) ? 1 : 0;
    return n;
  }
};

// ==== ID. IDENTIFY_NOTIFICATION origination (issue #54, REQ-AEM-026) =====
// IEEE 1722.1-2021 7.5.1: an identification notification is a multicast AECP
// unsolicited response to the Table B.1 "Identification Notifications"
// address (91-E0-F0-01-00-01), sent three times with a 150 ms delay between
// transmissions (txIdentify, 7.5.1.2.1), its controller_entity_id the Table
// 7-180 value (90-E0-F0-FF-FE-01-00-01) and its sequence_id identifySequenceID,
// which starts at 0 at power up and counts up once per IDENTIFY entry
// (Figure 7-142). 7.4.39.1 fixes the body: descriptor_type CONTROL and the
// index of the IDENTIFY control, u = 1. Figure 7-142 re-enters IDENTIFY when
// timeout (entry + 1 s) passes with the button still pressed, and returns to
// WAITING when it is released. Milan 5.4.5.4 makes the whole mechanism a
// "should" behind a user action, and says it is NOT the IDENTIFY control.
//
// The bench compresses 1 ms to 100 clocks, so the job's own build (the
// µprogram, the response-memory round trip, the TX slot) is visible beside
// T-IDENT-BURST: spacing is graded as at least the T- value and at most one
// tick of the ms timebase, the timer sweep and that build (SLACK) more. Every
// frame is due T-IDENT-BURST after the one before it LEFT (its last byte), so
// a stalled MAC or a busy lane delays the rest of a burst and never shortens
// a gap (ID5i, ID7).
struct IdentifyPhase : NotifyBench {
  static constexpr uint64_t IDENT_MAC = 0x91E0F0010001ull;      // Table B.1
  static constexpr uint64_t IDENT_EID = 0x90E0F0FFFE010001ull;  // Table 7-180
  static constexpr long BURST = 150L * MS_CYC;                  // T-IDENT-BURST
  static constexpr long REARM = 1000L * MS_CYC;                 // T-IDENT-REARM
  //! one tick (100 clocks: a deadline counts from the next ms boundary after
  //! the departure), the sweep's walk to the identify slots (at most 91
  //! cycles, F08.4) and the job's build and serialization in this bench
  //! (measured at under 200 clocks, README section ID)
  static constexpr long SLACK = 400;

  using NotifyBench::NotifyBench;

  static std::vector<uint8_t> ident(uint16_t seq) {
    std::vector<uint8_t> pl(4, 0);
    putbe(&pl[0], 0x001A, 2);                  // CONTROL (Table 7-1)
    putbe(&pl[2], IDIX, 2);                    // the IDENTIFY control
    auto f = aecp_frame(IDENT_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, IDENT_EID,
                        seq, AEM_IDENTIFY_NOTIF, pl);
    f[36] |= 0x80;                             // u = 1 (9.3.2.1)
    return f;
  }
  void press(bool on) { io.d->identify_button_i = on ? 1 : 0; }
  std::vector<Seen> idents(size_t from) const { return to_mac(IDENT_MAC, from); }

  //! every frame of v[first..first+n) is byte-exact for sequence_id seq
  void burst_exact(const std::vector<Seen>& v, size_t first, uint16_t seq,
                   const char* tag) {
    for (size_t i = first; i < first + 3 && i < v.size(); ++i) {
      CHECK(v[i].f == ident(seq),
            "%s: frame %zu byte-exact: DA 91-E0-F0-01-00-01, controller "
            "90-E0-F0-FF-FE-01-00-01, u = 1, IDENTIFY_NOTIFICATION, CONTROL %u, "
            "sequence_id %u", tag, i - first + 1, unsigned(IDIX), unsigned(seq));
      if (v[i].f != ident(seq)) { dump("got", v[i].f); dump("exp", ident(seq)); }
    }
  }
  //! the two gaps of the burst at v[first] are T-IDENT-BURST, at most SLACK more
  void burst_spacing(const std::vector<Seen>& v, size_t first, const char* tag) {
    if (first + 2 < v.size())
      printf("  [i] %s: gaps %ld and %ld clocks\n", tag,
             static_cast<long>(v[first + 1].t - v[first].t),
             static_cast<long>(v[first + 2].t - v[first + 1].t));
    for (size_t k = first; k + 1 < first + 3 && k + 1 < v.size(); ++k) {
      const long gap = long(v[k + 1].t - v[k].t);
      CHECK(gap >= BURST && gap <= BURST + SLACK,
            "%s: frame %zu to %zu spaced %ld clocks, want T-IDENT-BURST %ld "
            "to %ld (150 ms at 100 clocks/ms)", tag, k - first + 1,
            k - first + 2, gap, BURST, BURST + SLACK);
    }
  }

  // ---- ID1: one press, one burst of three ------------------------------
  void one_press_sends_one_burst() {
    const size_t from = seen.size();
    press(true);
    run_ms(40);
    press(false);
    run_ms(1500);
    const auto v = idents(from);
    CHECK(v.size() == 3, "ID1: one press sends three IDENTIFY_NOTIFICATION "
          "frames (IEEE 7.5.1.2.1), got %zu", v.size());
    burst_exact(v, 0, 0, "ID1b");
    burst_spacing(v, 0, "ID1c");
    CHECK(seen.size() - from == v.size(),
          "ID1d: nothing else on the AECP wire (%zu other frames); no "
          "controller is registered, so the burst needs no registration",
          seen.size() - from - v.size());
  }

  // ---- ID2: a held button re-arms every T-IDENT-REARM ------------------
  void a_held_button_re_arms() {
    const size_t from = seen.size();
    press(true);
    run_ms(2500);
    press(false);
    run_ms(1500);
    const auto v = idents(from);
    CHECK(v.size() == 9, "ID2: held 2.5 s: three bursts of three (Figure "
          "7-142 re-enters IDENTIFY at timeout), got %zu", v.size());
    //! every burst that did arrive is graded, so a slow re-arm also fails
    //! its own spacing check, not only the count
    const size_t bursts = std::min<size_t>(v.size() / 3, 3);
    for (size_t b = 0; b < bursts; ++b) {
      burst_exact(v, 3 * b, uint16_t(1 + b), "ID2b");
      burst_spacing(v, 3 * b, "ID2c");
    }
    CHECK(bursts >= 2, "ID2d: at least two bursts to time the re-arm, got %zu", bursts);
    for (size_t b = 1; b < bursts; ++b) {
      const long gap = long(v[3 * b].t - v[3 * (b - 1)].t);
      printf("  [i] ID2d: burst %zu starts %ld clocks after burst %zu\n", b + 1, gap, b);
      CHECK(gap >= REARM && gap <= REARM + SLACK,
            "ID2d: burst %zu starts %ld clocks after burst %zu, want "
            "T-IDENT-REARM %ld to %ld (no faster than 1 s, from the first "
            "frame)", b + 1, gap, b, REARM, REARM + SLACK);
    }
    CHECK(seen.size() - from == 9,
          "ID2e: the release stopped the bursts: no fourth burst, nothing "
          "else on the wire (%zu frames)", seen.size() - from);
  }

  // ---- ID3: a release and a press inside a burst -----------------------
  void a_release_and_press_inside_a_burst() {
    const size_t from = seen.size();
    press(true);
    for (long c = 0; c < 20L * MS_CYC && idents(from).empty(); ++c) tick();
    press(false);
    run_ms(20);
    press(true);
    run_ms(1200);
    press(false);
    run_ms(1500);
    const auto v = idents(from);
    CHECK(v.size() == 6, "ID3: a burst the release did not cut, then one "
          "more for the new press, and none at the timeout after the second "
          "release, got %zu frames", v.size());
    if (v.size() < 6) return;
    burst_exact(v, 0, 4, "ID3b");
    burst_spacing(v, 0, "ID3c");
    burst_exact(v, 3, 5, "ID3d");
    burst_spacing(v, 3, "ID3e");
    const long gap = long(v[3].t - v[2].t);
    printf("  [i] ID3f: the new burst follows the third frame by %ld clocks\n", gap);
    CHECK(gap >= BURST && gap <= BURST + SLACK, "ID3f: the new press's burst "
          "starts T-IDENT-BURST after the running one's third frame left "
          "(WAITING, then IDENTIFY; never two frames closer), %ld clocks, want "
          "%ld to %ld", gap, BURST, BURST + SLACK);
  }

  // ---- ID4: the controller-to-entity forms start nothing ---------------
  void the_command_forms_start_nothing() {
    const size_t from = seen.size();
    std::vector<uint8_t> id_pl(4, 0);
    putbe(&id_pl[0], 0x001A, 2);
    const auto got = ask(CTLR_MAC, CTLR_EID, 0x6A00, AEM_IDENTIFY_NOTIF, id_pl);
    const auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_BAD_ARGUMENTS, EID,
                                 CTLR_EID, 0x6A00, AEM_IDENTIFY_NOTIF, id_pl);
    CHECK(got == want, "ID4: with the sequencer built, IDENTIFY_NOTIFICATION "
          "as a command still answers BAD_ARGUMENTS byte-exact (7.4.39.2, A6)");
    std::vector<uint8_t> on(5, 0);
    putbe(&on[0], 0x001A, 2);
    on[4] = 255;
    const auto s = ask(CTLR_MAC, CTLR_EID, 0x6A01, AEM_SET_CONTROL, on);
    CHECK(status_of(s) == AECP_SUCCESS && s.size() > 42 && s[42] == 255,
          "ID4b: SET_CONTROL IDENTIFY 255 is accepted (premise)");
    run_ms(600);
    std::vector<uint8_t> off(5, 0);
    putbe(&off[0], 0x001A, 2);
    (void)ask(CTLR_MAC, CTLR_EID, 0x6A02, AEM_SET_CONTROL, off);
    run_ms(200);
    CHECK(idents(from).empty(), "ID4c: neither the command form nor the "
          "IDENTIFY control (the controller-to-entity direction, Milan "
          "5.4.5.4) sends an identification notification, got %zu",
          idents(from).size());
  }

  //! run until `n` identify frames arrived since `from` (or 2 s passed)
  void wait_idents(size_t from, size_t n) {
    for (long c = 0; c < 2000L * MS_CYC && idents(from).size() < n; ++c) tick();
  }
  //! the MAC takes no byte for `ms`: tx_ready_i low, as a full MAC FIFO holds it
  void stall_mac(long ms) {
    io.mac_tx_ready = false;
    run_ms(ms);
    io.mac_tx_ready = true;
  }
  static long gap_of(const std::vector<Seen>& v, size_t k) {
    return long(v[k + 1].t - v[k].t);
  }

  // ---- ID5: a 15-row fan-out delays the burst by at most one job -------
  //! the fifteen controllers of this arm, row k at {FAN_MAC + k, FAN_EID + k}
  static constexpr uint64_t FAN_MAC = 0x020200BB0000ull;
  static constexpr uint64_t FAN_EID = 0x8888000000000100ull;
  static std::vector<uint8_t> clock_domain_name(const char* text) {
    std::vector<uint8_t> p(72, 0);
    putbe(&p[0], 0x0024, 2);                   // CLOCK_DOMAIN 0, name 0
    putbe(&p[6], CFGIX, 2);
    memcpy(&p[8], text, strlen(text));
    return p;
  }
  //! the SET_NAME notification row k receives at sequence_id seq
  static std::vector<uint8_t> fan_frame(unsigned k, uint16_t seq,
                                        const std::vector<uint8_t>& body) {
    auto f = aecp_frame(FAN_MAC + k, OWN_MAC, 1, AECP_SUCCESS, EID,
                        FAN_EID + k, seq, AEM_SET_NAME, body);
    f[36] |= 0x80;
    return f;
  }
  //! every row received exactly one frame since `from`, byte-exact at seq
  void fan_out_exact(size_t from, uint16_t seq, const std::vector<uint8_t>& body,
                     const char* tag) {
    int good = 0;
    for (unsigned k = 0; k < 15; ++k) {
      const auto v = to_mac(FAN_MAC + k, from);
      good += (v.size() == 1 && v[0].f == fan_frame(k, seq, body)) ? 1 : 0;
    }
    CHECK(good == 15, "%s: all fifteen rows received their SET_NAME "
          "notification byte-exact at their own sequence_id %u (%d of 15)",
          tag, unsigned(seq), good);
  }
  void a_fan_out_delays_the_burst_by_one_job() {
    int ok = 0;
    for (unsigned k = 0; k < 15; ++k)
      ok += register_controller(FAN_MAC + k, FAN_EID + k, uint16_t(0x6B00 + k)) ? 1 : 0;
    CHECK(ok == 15, "ID5: fifteen controllers registered (%d)", ok);
    const auto name = clock_domain_name("Identify Fan-Out One");
    const size_t from = seen.size();
    feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, 0x6B20, AEM_SET_NAME, name));
    for (long c = 0; c < 50L * MS_CYC && to_mac(FAN_MAC, from).empty(); ++c) tick();
    const size_t pressed_at = seen.size();
    press(true);
    run_ms(30);
    press(false);
    run_ms(1500);
    const auto v = idents(from);
    CHECK(v.size() == 3, "ID5b: the press during the fan-out sends one burst, "
          "got %zu", v.size());
    if (v.empty()) return;
    int before = 0;
    for (size_t i = pressed_at; i < seen.size() && seen[i].t < v[0].t; ++i)
      before += (unsolicited(seen[i].f) && da_of(seen[i].f) != IDENT_MAC) ? 1 : 0;
    CHECK(before <= 2, "ID5c: %d fan-out frames went between the press and "
          "the first identify frame, want at most two (the job in flight and "
          "the one the engine took)", before);
    burst_exact(v, 0, 6, "ID5d");
    burst_spacing(v, 0, "ID5e");
    fan_out_exact(from, 0, name, "ID5f");
    //! the identify job touched no row: the next fan-out is at sequence_id 1
    const auto name2 = clock_domain_name("Identify Fan-Out Two");
    const size_t from2 = seen.size();
    (void)ask(CTLR_MAC, CTLR_EID, 0x6B21, AEM_SET_NAME, name2);
    run_ms(100);
    fan_out_exact(from2, 1, name2, "ID5g");
    a_fan_out_at_frame_2_never_shortens_a_gap();
    ok = 0;
    for (unsigned k = 0; k < 15; ++k)
      ok += deregister_controller(FAN_MAC + k, FAN_EID + k, uint16_t(0x6B30 + k)) ? 1 : 0;
    CHECK(ok == 15, "ID5h: fifteen controllers deregistered (%d)", ok);
  }

  // ---- ID5i: a fan-out at frame 2's deadline never shortens a gap -------
  //! The fifteen rows of ID5 are still registered. A SET_NAME fan-out fed
  //! just before frame 2 is due holds the engine, so frame 2 leaves late;
  //! frame 3 is due T-IDENT-BURST after frame 2 LEFT, so the second gap is
  //! never short (a deadline chained from the first frame would leave it
  //! short by frame 2's delay). Sequence_ids 7, 8 and 9 follow ID5's 6.
  void a_fan_out_at_frame_2_never_shortens_a_gap() {
    static constexpr long OFFSETS[] = {
        BURST - 550, BURST - 300, BURST - 50};
    long min_gap = 1L << 40;
    long max_g12 = 0;
    int bursts = 0;
    unsigned n = 0;
    for (const long off : OFFSETS) {
      char text[32];
      snprintf(text, sizeof text, "Identify Contention %u", n);
      const auto body = clock_domain_name(text);
      const uint16_t seq = uint16_t(7 + n);
      const size_t from = seen.size();
      press(true);
      wait_idents(from, 1);
      press(false);
      if (idents(from).empty()) break;
      const uint64_t t1 = idents(from)[0].t;
      while (io.t < t1 + uint64_t(off)) tick();
      feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, uint16_t(0x6B40 + n),
                      AEM_SET_NAME, body));
      run_ms(1500);
      ++n;
      const auto v = idents(from);
      if (v.size() != 3) continue;
      ++bursts;
      burst_exact(v, 0, seq, "ID5j");
      printf("  [i] ID5i: fan-out at frame 1 + %ld clocks: gaps %ld and %ld\n", off,
             gap_of(v, 0), gap_of(v, 1));
      min_gap = std::min({min_gap, gap_of(v, 0), gap_of(v, 1)});
      max_g12 = std::max(max_g12, gap_of(v, 0));
    }
    CHECK(bursts == 3, "ID5i: three presses, each a burst of three, with a "
          "fan-out fed at frame 1 + %ld, %ld and %ld clocks (%d)", OFFSETS[0],
          OFFSETS[1], OFFSETS[2], bursts);
    CHECK(min_gap >= BURST, "ID5k: no gap shorter than T-IDENT-BURST: the "
          "smallest of the six is %ld clocks, want at least %ld", min_gap, BURST);
    //! anti-vacuity: some fan-out really held frame 2 past its uncontended
    //! departure (one tick, the sweep and the build: SLACK)
    CHECK(max_g12 > BURST + SLACK, "ID5l: a fan-out delayed frame 2 (largest "
          "gap 1->2 %ld clocks, more than %ld)", max_g12, BURST + SLACK);
  }

  // ---- ID6: a press before the restore goes out at the release ---------
  //! AECP is held from reset to the D3 terminal (PR #132): the burst pressed
  //! before the restore waits for the release, then keeps its spacing, and
  //! identifySequenceID restarted at 0 with the reset (7.5.1: "starts at zero
  //! (0) on power up or reboot"). The hold is before the first frame; ID7
  //! grades a stall between frames
  void a_press_before_the_restore_goes_out_at_the_release() {
    boot_to_idle(false);
    press(true);
    run_ms(30);
    press(false);
    run_ms(400);
    CHECK(seen.empty(), "ID6: nothing leaves while AECP is held (%zu frames)",
          seen.size());
    restore_walks();
    run_ms(1500);
    const auto v = idents(0);
    CHECK(v.size() == 3, "ID6b: the held press still sends its burst at the "
          "release, got %zu frames", v.size());
    burst_exact(v, 0, 0, "ID6c");
    burst_spacing(v, 0, "ID6d");
  }

  // ---- ID7: a TX stall mid-burst delays the rest, never bunches it -----
  //! The MAC takes no byte (tx_ready_i low) inside a burst. The engine
  //! retires a job at its lane grant, so a stall can hold a frame the engine
  //! has already let go; every later frame is due T-IDENT-BURST after the one
  //! before it LEFT (IEEE 7.5.1: "a 150 ms delay between transmissions"), and
  //! the next burst's first frame keeps the same gap after a third frame the
  //! stall pushed past the timeout. Sequence_ids 1 to 5 follow ID6's 0.
  //! true while an identify frame is part-way out to the MAC
  bool mid_ident_frame() const {
    return io.in_frame && io.cur.size() >= 6 && da_of(io.cur) == IDENT_MAC;
  }
  void a_tx_stall_mid_burst_never_bunches_it() {
    a_stall_inside_frame_1();
    a_stall_after_frame_1();
    a_stall_after_frame_2();
    a_stall_past_the_timeout();
  }
  //! ID7: the MAC stops for 400 ms in the middle of frame 1, which the
  //! engine retired at its grant: frame 2 waits for frame 1 to leave
  void a_stall_inside_frame_1() {
    const size_t from = seen.size();
    const uint64_t pressed = io.t;
    press(true);
    for (long c = 0; c < 50L * MS_CYC && !mid_ident_frame(); ++c) tick();
    press(false);
    stall_mac(400);
    run_ms(1500);
    const auto v = idents(from);
    CHECK(v.size() == 3, "ID7: a 400 ms MAC stall inside frame 1 still sends "
          "a burst of three, got %zu", v.size());
    if (v.size() == 3) {
      burst_exact(v, 0, 1, "ID7b");
      printf("  [i] ID7: stall inside frame 1: it left %ld clocks after the "
             "press; gaps %ld and %ld\n", long(v[0].t - pressed), gap_of(v, 0),
             gap_of(v, 1));
      CHECK(long(v[0].t - pressed) >= 400L * MS_CYC, "ID7c: the stall held "
            "frame 1 (it left %ld clocks after the press)", long(v[0].t - pressed));
      CHECK(gap_of(v, 0) >= BURST && gap_of(v, 0) <= BURST + SLACK,
            "ID7d: frame 2 leaves T-IDENT-BURST after frame 1 left, not after "
            "the engine retired it: %ld clocks, want %ld to %ld", gap_of(v, 0),
            BURST, BURST + SLACK);
      CHECK(gap_of(v, 1) >= BURST && gap_of(v, 1) <= BURST + SLACK,
            "ID7e: frame 3 keeps its gap, %ld clocks", gap_of(v, 1));
    }
  }
  //! ID7f: 400 ms between frames 1 and 2 (R420-1's probe): frame 3 is due
  //! T-IDENT-BURST after frame 2 left, not at a deadline from frame 1
  void a_stall_after_frame_1() {
    const size_t from = seen.size();
    press(true);
    wait_idents(from, 1);
    press(false);
    stall_mac(400);
    run_ms(1500);
    const auto v = idents(from);
    CHECK(v.size() == 3, "ID7f: a 400 ms MAC stall after frame 1 still sends "
          "a burst of three, got %zu", v.size());
    if (v.size() == 3) {
      burst_exact(v, 0, 2, "ID7g");
      printf("  [i] ID7f: stall after frame 1: gaps %ld and %ld clocks\n",
             gap_of(v, 0), gap_of(v, 1));
      CHECK(gap_of(v, 0) >= 400L * MS_CYC, "ID7h: the stall held frame 2: gap "
            "1->2 %ld clocks, at least the 400 ms stall", gap_of(v, 0));
      CHECK(gap_of(v, 1) >= BURST && gap_of(v, 1) <= BURST + SLACK,
            "ID7i: frame 3 leaves T-IDENT-BURST after frame 2 left, not at a "
            "deadline from frame 1: %ld clocks, want %ld to %ld", gap_of(v, 1),
            BURST, BURST + SLACK);
    }
  }
  //! ID7j: 250 ms after frame 2
  void a_stall_after_frame_2() {
    const size_t from = seen.size();
    press(true);
    wait_idents(from, 1);
    press(false);
    wait_idents(from, 2);
    stall_mac(250);
    run_ms(1500);
    const auto v = idents(from);
    CHECK(v.size() == 3, "ID7j: a 250 ms MAC stall after frame 2 still sends "
          "a burst of three, got %zu", v.size());
    if (v.size() == 3) {
      burst_exact(v, 0, 3, "ID7k");
      printf("  [i] ID7j: stall after frame 2: gaps %ld and %ld clocks\n",
             gap_of(v, 0), gap_of(v, 1));
      CHECK(gap_of(v, 0) >= BURST && gap_of(v, 0) <= BURST + SLACK,
            "ID7l: frame 2 kept its gap, %ld clocks", gap_of(v, 0));
      CHECK(gap_of(v, 1) >= 250L * MS_CYC, "ID7m: the stall held frame 3: gap "
            "2->3 %ld clocks, at least the 250 ms stall", gap_of(v, 1));
    }
  }
  //! ID7n: held, 900 ms after frame 1: the burst outlasts T-IDENT-REARM, so
  //! the timeout has passed when its third frame leaves. Released once the
  //! next burst is out, so no third burst is due
  void a_stall_past_the_timeout() {
    const size_t from = seen.size();
    press(true);
    wait_idents(from, 1);
    stall_mac(900);
    wait_idents(from, 6);
    press(false);
    run_ms(1500);
    const auto v = idents(from);
    CHECK(v.size() == 6, "ID7n: held through a 900 ms MAC stall after frame 1: "
          "the stretched burst and one more, got %zu", v.size());
    if (v.size() == 6) {
      burst_exact(v, 0, 4, "ID7o");
      burst_exact(v, 3, 5, "ID7o");
      printf("  [i] ID7n: gaps %ld, %ld | %ld | %ld, %ld clocks\n", gap_of(v, 0),
             gap_of(v, 1), gap_of(v, 2), gap_of(v, 3), gap_of(v, 4));
      CHECK(gap_of(v, 0) >= 900L * MS_CYC, "ID7p: the stall held frame 2: gap "
            "1->2 %ld clocks, at least the 900 ms stall", gap_of(v, 0));
      CHECK(gap_of(v, 1) >= BURST && gap_of(v, 1) <= BURST + SLACK,
            "ID7q: frame 3 leaves T-IDENT-BURST after frame 2 left, %ld clocks",
            gap_of(v, 1));
      CHECK(gap_of(v, 2) >= BURST && gap_of(v, 2) <= BURST + SLACK,
            "ID7r: the timeout passed during the stall, and the next burst "
            "starts T-IDENT-BURST after the third frame left, never at once: "
            "%ld clocks, want %ld to %ld", gap_of(v, 2), BURST, BURST + SLACK);
      burst_spacing(v, 3, "ID7s");
      CHECK(long(v[3].t - v[0].t) >= REARM, "ID7t: still no faster than "
            "T-IDENT-REARM from the first frame (%ld clocks)", long(v[3].t - v[0].t));
    }
  }

  // ---- ID8: no press is lost to a burst or to the gap after it ---------
  //! Figure 7-142 answers identifyButtonPressed in WAITING, and the
  //! T-IDENT-BURST gap after a burst's third frame only delays that answer
  //! (R420-2 F1, R421-2 F1): a press seen while the gap runs is latched, and
  //! its burst starts when the gap ends, however soon the button is let go;
  //! so is a new press after a release inside a burst (ID8o, ID8s). The
  //! gap's end is the IDENT-BURST expiry on the timer bus (gap_ends). Each
  //! arm starts from a burst of its own. ID8 and ID9 run after ID5, before
  //! ID6's reset, so sequence_ids 10 to 27 follow ID5i's 9.
  struct GapPress {
    std::vector<Seen> v;                       // the frames since the first press
    uint64_t gap_end = 0;                      // the expiry ending the third frame's gap
    long sweep = -1;                           // that expiry after its deadline's ms began
    long start = -1;                           // that expiry to the next first frame
  };
  long ref_start = -1;                         // ID8's start, the latched press's
  long ref_sweep = -1;                         // ID8's sweep, a timer-service constant

  //! the first IDENT-BURST expiry after clock t (0 when none came)
  uint64_t gap_end_after(uint64_t t) const {
    for (const uint64_t e : gap_ends)
      if (e > t) return e;
    return 0;
  }
  //! the ms deadline of the first IDENT-BURST arm after clock t (0: none)
  uint32_t gap_deadline_after(uint64_t t) const {
    for (const GapArm& a : gap_arms)
      if (a.t > t) return a.deadline;
    return 0;
  }
  //! the clock the timebase begins ms `ms` on, before or after the latest
  //! boundary (its prescaler is regular)
  int64_t clock_of_ms(uint32_t ms) const {
    return int64_t(edge_t) + (int64_t(ms) - int64_t(edge_ms)) * MS_CYC;
  }
  //! a 30 ms press and its burst; then, for `run`, wait for the arm of the
  //! gap after its third frame. Returns the third frame's clock (0: no burst)
  uint64_t lead_burst(size_t from) {
    press(true);
    run_ms(30);
    press(false);
    wait_idents(from, 3);
    const auto lead = idents(from);
    if (lead.size() != 3) return 0;
    for (long c = 0; c < 10L * MS_CYC && gap_deadline_after(lead[2].t) == 0; ++c) tick();
    return lead[2].t;
  }
  //! what the burst after the press shows: its frames, the gap's end, how
  //! far into its deadline's ms the expiry came, and the start after it
  GapPress gap_press_result(size_t from, uint64_t t3, uint32_t deadline) {
    run_ms(1000);
    GapPress g;
    g.v = idents(from);
    g.gap_end = gap_end_after(t3);
    if (g.gap_end != 0 && deadline != 0) g.sweep = long(int64_t(g.gap_end) - clock_of_ms(deadline));
    if (g.v.size() > 3 && g.gap_end != 0) g.start = long(g.v[3].t - g.gap_end);
    return g;
  }
  //! a burst, then a press of `hold_ms` made `lead_ms` after its third
  //! frame left, inside the gap
  GapPress press_in_gap(long lead_ms, long hold_ms) {
    const size_t from = seen.size();
    const uint64_t t3 = lead_burst(from);
    const uint32_t deadline = gap_deadline_after(t3);
    while (t3 != 0 && io.t < t3 + uint64_t(lead_ms * MS_CYC)) tick();
    press(true);
    run_ms(hold_ms);
    press(false);
    return gap_press_result(from, t3, deadline);
  }
  //! the gap from the first burst's third frame to the press's first frame
  void gap_after_third(const GapPress& g, const char* tag) {
    const long gap = gap_of(g.v, 2);
    printf("  [i] %s: the press's burst follows the third frame by %ld clocks, "
           "%ld after the gap ended\n", tag, gap, g.start);
    CHECK(gap >= BURST && gap <= BURST + SLACK, "%s: the press's burst starts "
          "T-IDENT-BURST after the third frame left, never sooner: %ld clocks, "
          "want %ld to %ld", tag, gap, BURST, BURST + SLACK);
  }

  //! ID8: 30 ms, made 2 ms after the third frame left: the press is over
  //! long before the gap ends, so only the latch can answer it
  void a_short_press_in_the_gap_is_latched() {
    const GapPress g = press_in_gap(2, 30);
    ref_sweep = g.sweep;
    printf("  [i] ID8: the gap's expiry came %ld clocks into its deadline's ms\n", g.sweep);
    CHECK(g.v.size() == 6, "ID8: a 30 ms press made 2 ms after a burst's "
          "third frame left (inside the T-IDENT-BURST gap) still sends one "
          "burst: %zu frames in all, want 3 + 3", g.v.size());
    if (g.v.size() != 6) return;
    burst_exact(g.v, 3, 11, "ID8b");
    gap_after_third(g, "ID8c");
    burst_spacing(g.v, 3, "ID8d");
    CHECK(g.start > 0 && g.start <= SLACK, "ID8e: the burst started when the "
          "gap ended: its first frame left %ld clocks after the IDENT-BURST "
          "expiry, want 1 to %ld", g.start, SLACK);
    ref_start = g.start;
  }
  //! ID8f: 200 ms, made 2 ms after the third frame left: still held when
  //! the gap ends, so the burst starts then, exactly as the latched one did
  void a_long_press_in_the_gap_starts_at_its_end() {
    const GapPress g = press_in_gap(2, 200);
    CHECK(g.v.size() == 6, "ID8f: a 200 ms press made 2 ms after a burst's "
          "third frame left sends one burst: %zu frames in all, want 3 + 3",
          g.v.size());
    if (g.v.size() != 6) return;
    burst_exact(g.v, 3, 13, "ID8g");
    gap_after_third(g, "ID8h");
    CHECK(g.start == ref_start, "ID8i: it starts at the gap's end as the "
          "latched 30 ms press did: %ld clocks after the expiry, ID8 %ld",
          g.start, ref_start);
  }

  //! a burst, then a 30 ms press timed so that its synchronised level is
  //! first seen by the sequencer k edges after the edge that sees the
  //! IDENT-BURST expiry (k = 0: the same edge, the gap's last). The expiry
  //! is predicted from the arm: ref_sweep clocks into the deadline's ms.
  //! The button is set before the step that ends on clock `at`; the two
  //! synchroniser flops take it on the next two edges, so the sequencer
  //! samples it on edge at + 3, and the expiry on edge gap_end + 1
  GapPress press_at_gap_end(long k, bool& on_time) {
    const size_t from = seen.size();
    const uint64_t t3 = lead_burst(from);
    const uint32_t deadline = gap_deadline_after(t3);
    const bool known = t3 != 0 && deadline != 0 && ref_sweep >= 0;
    const int64_t gap_end = known ? clock_of_ms(deadline) + ref_sweep : 0;
    while (known && int64_t(io.t) < gap_end - 2 + k && io.t < t3 + uint64_t(2 * BURST)) tick();
    press(true);
    run_ms(30);
    press(false);
    const GapPress g = gap_press_result(from, t3, deadline);
    on_time = known && int64_t(g.gap_end) == gap_end;
    return g;
  }
  //! ID8j: the press reaches the sequencer one edge before the gap's end,
  //! on it, and one and two edges after it. The first three start the
  //! burst on the same edge as the latched press (ID8); the last one edge
  //! later, which shows the measure resolves a single clock
  void a_press_at_the_gap_end_starts_with_it() {
    static constexpr long EDGES[] = {
        -1, 0, 1, 2};
    uint16_t seq = 15;
    for (const long k : EDGES) {
      bool on_time = false;
      const GapPress g = press_at_gap_end(k, on_time);
      CHECK(on_time, "ID8j: k = %ld: the gap ended on the clock predicted from "
            "its arm and ID8's sweep (%ld), so the press is placed against it",
            k, ref_sweep);
      CHECK(g.v.size() == 6, "ID8k: k = %ld: a press at the gap's end sends "
            "one burst, never none and never two: %zu frames in all, want 3 + 3",
            k, g.v.size());
      if (g.v.size() == 6) {
        burst_exact(g.v, 3, seq, "ID8l");
        gap_after_third(g, "ID8m");
        const long want = ref_start + std::max(0L, k - 1);
        printf("  [i] ID8n: k = %ld: the burst starts %ld clocks after the expiry\n",
               k, g.start);
        CHECK(g.start == want, "ID8n: k = %ld: the burst starts %ld clocks "
              "after the expiry, want %ld (ID8's start, plus the edges the "
              "press came after the gap's first free one)", k, g.start, want);
      }
      seq = uint16_t(seq + 2);
    }
  }

  //! the burst owed to a new press made inside the first burst: one more,
  //! byte-exact at seq + 1, starting at the gap's end as ID8's did
  void grade_owed_burst(const GapPress& g, uint16_t seq, const char* const tags[4],
                        const char* what) {
    CHECK(g.v.size() == 6, "%s: %s: one more burst: %zu frames in all, want 3 + 3",
          tags[0], what, g.v.size());
    if (g.v.size() != 6) return;
    burst_exact(g.v, 0, seq, tags[1]);
    burst_exact(g.v, 3, uint16_t(seq + 1), tags[1]);
    gap_after_third(g, tags[2]);
    CHECK(g.start == ref_start, "%s: it starts at the gap's end as ID8's latched "
          "press did: %ld clocks after the expiry, ID8 %ld", tags[3], g.start,
          ref_start);
  }
  //! ID8o: a release and a new press inside a burst, the button still held
  //! when the third frame leaves and let go 30 ms later, inside the gap
  void a_new_press_held_past_a_burst_is_latched() {
    static constexpr const char* TAGS[4] = {
        "ID8o", "ID8p", "ID8q", "ID8r"};
    const size_t from = seen.size();
    press(true);
    wait_idents(from, 1);
    press(false);
    run_ms(20);
    press(true);
    wait_idents(from, 3);
    const auto lead = idents(from);
    run_ms(30);
    press(false);
    grade_owed_burst(gap_press_result(from, lead.size() == 3 ? lead[2].t : 0, 0), 22,
                     TAGS, "a release and a new press inside a burst, let go 30 ms "
                     "after its third frame left (inside the gap)");
  }
  //! ID8s: a release and a new 30 ms press between frames 1 and 2, let go
  //! long before the burst ends: the press is latched like one in the gap
  void a_new_press_inside_a_burst_is_latched() {
    static constexpr const char* TAGS[4] = {
        "ID8s", "ID8t", "ID8u", "ID8v"};
    const size_t from = seen.size();
    press(true);
    wait_idents(from, 1);
    press(false);
    run_ms(20);
    press(true);
    run_ms(30);
    press(false);
    wait_idents(from, 3);
    const auto lead = idents(from);
    grade_owed_burst(gap_press_result(from, lead.size() == 3 ? lead[2].t : 0, 0), 24,
                     TAGS, "a release and a new 30 ms press between frames 1 "
                     "and 2, let go before the burst ended");
  }

  void a_press_in_the_gap_after_a_burst_is_never_lost() {
    a_short_press_in_the_gap_is_latched();
    a_long_press_in_the_gap_starts_at_its_end();
    a_press_at_the_gap_end_starts_with_it();
    a_new_press_held_past_a_burst_is_latched();
    a_new_press_inside_a_burst_is_latched();
  }

  // ---- ID9: a MAC stall on a frame's last byte ---------------------------
  //! (R421-2 S1) The MAC holds tx_ready_i low on the eof beat itself, so the
  //! frame is presented whole and its last byte is not taken: the frame has
  //! not left until the MAC takes that byte, and the next frame is due
  //! T-IDENT-BURST after it did. Sequence_ids 26 and 27 follow ID8's 25.
  //! The bench's hook drops ready on the first eof it sees, so it is armed
  //! once the identify frame is part-way out (frames are atomic on the MAC).
  //! Returns the clock the stall began on (0: the frame never got there)
  uint64_t stall_last_byte(long ms) {
    for (long c = 0; c < 300L * MS_CYC && !mid_ident_frame(); ++c) tick();
    if (!mid_ident_frame()) return 0;
    io.tx_eof_stalled = false;
    io.stall_tx_at_eof = true;
    for (long c = 0; c < 1000 && !io.tx_eof_stalled; ++c) tick();
    const uint64_t began = io.tx_eof_stalled ? io.t : 0;
    run_ms(ms);
    io.stall_tx_at_eof = false;
    io.mac_tx_ready = true;
    io.tx_eof_stalled = false;
    return began;
  }
  //! ID9: frame 1's last byte held 400 ms; ID9e: frame 2's
  void a_stall_on_the_last_byte_never_bunches_it() {
    for (unsigned n = 0; n < 2; ++n) {
      const char* const tag = n == 0 ? "ID9" : "ID9e";
      const size_t from = seen.size();
      press(true);
      if (n == 1) wait_idents(from, 1);
      const uint64_t began = stall_last_byte(400);
      press(false);
      run_ms(1500);
      const auto v = idents(from);
      CHECK(began != 0 && v.size() == 3, "%s: frame %u's last byte held 400 ms "
            "by the MAC (premise: the stall began, %d) still sends a burst of "
            "three, got %zu", tag, n + 1, began != 0 ? 1 : 0, v.size());
      if (v.size() != 3 || began == 0) continue;
      burst_exact(v, 0, uint16_t(26 + n), n == 0 ? "ID9b" : "ID9f");
      printf("  [i] %s: frame %u's last byte was held %ld clocks; gaps %ld and %ld "
             "clocks\n", tag, n + 1, long(v[n].t - began), gap_of(v, 0), gap_of(v, 1));
      CHECK(long(v[n].t - began) >= 400L * MS_CYC, "%s: frame %u left only when "
            "the MAC took its last byte, %ld clocks after it was held", n == 0 ? "ID9c"
            : "ID9g", n + 1, long(v[n].t - began));
      CHECK(gap_of(v, n) >= BURST && gap_of(v, n) <= BURST + SLACK, "%s: frame %u "
            "leaves T-IDENT-BURST after frame %u's last byte was taken, not after "
            "it was first presented: %ld clocks, want %ld to %ld", n == 0 ? "ID9d"
            : "ID9h", n + 2, n + 1, gap_of(v, n), BURST, BURST + SLACK);
    }
  }

  void run() {
    boot_to_idle(true);
    one_press_sends_one_burst();
    a_held_button_re_arms();
    a_release_and_press_inside_a_burst();
    the_command_forms_start_nothing();
    a_fan_out_delays_the_burst_by_one_job();
    a_press_in_the_gap_after_a_burst_is_never_lost();
    a_stall_on_the_last_byte_never_bunches_it();
    a_press_before_the_restore_goes_out_at_the_release();
    a_tx_stall_mid_burst_never_bunches_it();
  }
};

// ==== ID0. The same button with P-EN-IDENTIFY-NOTIFICATION = 0 ===========
// The default build: the sequencer does not exist, so a button held across
// two T-IDENT-REARM periods and pressed again puts nothing on the wire.
struct IdentifyOffPhase : NotifyBench {
  using NotifyBench::NotifyBench;
  void run() {
    boot_to_idle(true);
    const size_t from = seen.size();
    io.d->identify_button_i = 1;
    run_ms(2500);
    io.d->identify_button_i = 0;
    run_ms(100);
    io.d->identify_button_i = 1;
    run_ms(100);
    io.d->identify_button_i = 0;
    run_ms(1000);
    CHECK(seen.size() == from, "ID0: with the parameter at 0 the button puts "
          "no AECP frame on the wire (%zu)", seen.size() - from);
    CHECK(to_mac(IdentifyPhase::IDENT_MAC, 0).empty(),
          "ID0b: nothing is ever addressed to 91-E0-F0-01-00-01");
  }
};

// ==== NP. A wire-level push for each command class (issue #58) ===========
// Milan 5.4.5.2: each successful response to a command of the IEEE 7.5.2 list
// that modifies the state sends an unsolicited notification to every
// registered controller except the requester; 5.4.5.1: each copy carries that
// controller's own DA, controller_entity_id and sequence_id, and the entry's
// sequence_id counts up by one per notification sent to it. IEEE 7.5.2: the
// notification IS an unsolicited response to the command, so its body is the
// command's response format (7.4.7.1, 7.4.9.1, 7.4.15.1, 7.4.25.1, 7.4.21.1,
// 7.4.35.1 / 7.4.36.1).
//
// Two controllers are registered: the requester A and B. The per-entry
// sequence_id is modelled from the wire alone: the count of unsolicited
// frames addressed to that controller before this one.
struct PushPhase : NotifyBench {
  static constexpr uint64_t B_MAC = 0x0202C2C2C2C2ull;
  static constexpr uint64_t B_EID = CTLR2_EID;
  static constexpr uint32_t ACC_LAT_VALID = 0x20000000u;   // Table 7-145
  uint16_t seq = 0x6C00;

  using NotifyBench::NotifyBench;

  static std::vector<uint8_t> ti(uint16_t ty, uint16_t ix) {
    std::vector<uint8_t> p(4, 0);
    putbe(&p[0], ty, 2);
    putbe(&p[2], ix, 2);
    return p;
  }
  static std::vector<uint8_t> cfg_body(uint16_t ix) {
    std::vector<uint8_t> p(4, 0);
    putbe(&p[2], ix, 2);                        // reserved @24, index @26
    return p;
  }
  static std::vector<uint8_t> control_body(uint8_t value) {
    auto p = ti(0x001A, 0);
    p.push_back(value);                         // one CONTROL_LINEAR_UINT8
    return p;
  }
  //! 1722.1-2021 Figure 7-40, the whole 84-byte body: {type, index}, flags,
  //! stream_format, stream_id, msrp_accumulated_latency @48, then zeros
  static std::vector<uint8_t> stream_info_body(uint16_t ix, uint32_t latency) {
    std::vector<uint8_t> p(84, 0);
    putbe(&p[0], 0x0006, 2);
    putbe(&p[2], ix, 2);
    putbe(&p[4], ACC_LAT_VALID, 4);
    putbe(&p[24], latency, 4);
    return p;
  }
  //! the unsolicited frames of command_type `op` addressed to `mac` since
  //! `from`, with their log indices
  std::vector<size_t> pushes(uint64_t mac, unsigned op, size_t from) const {
    std::vector<size_t> v;
    for (size_t i = from; i < seen.size(); ++i)
      if (da_of(seen[i].f) == mac && unsolicited(seen[i].f) && ct_of(seen[i].f) == op)
        v.push_back(i);
    return v;
  }

  //! one state-changing SET from {req_mac, req_eid}: the solicited response
  //! byte-exact, exactly one unsolicited response of the same type at the
  //! other controller, byte-exact at its modelled sequence_id, and none at
  //! the requester
  void push_step(const char* tag, uint16_t op, const std::vector<uint8_t>& cmd,
                 const std::vector<uint8_t>& body, bool from_b = false) {
    const uint64_t req_mac = from_b ? B_MAC : CTLR_MAC;
    const uint64_t req_eid = from_b ? B_EID : CTLR_EID;
    const uint64_t other_mac = from_b ? CTLR_MAC : B_MAC;
    const uint64_t other_eid = from_b ? CTLR_EID : B_EID;
    const size_t from = seen.size();
    const uint16_t s = seq++;
    const auto r = ask(req_mac, req_eid, s, op, cmd);
    CHECK(r == aecp_frame(req_mac, OWN_MAC, 1, AECP_SUCCESS, EID, req_eid, s, op, body),
          "%s: the solicited response is SUCCESS, byte-exact (premise)", tag);
    run_ms(200);
    const auto at_other = pushes(other_mac, op, from);
    CHECK(at_other.size() == 1, "%s: exactly one unsolicited response of this "
          "command at the other registered controller, got %zu", tag, at_other.size());
    if (!at_other.empty()) {
      const size_t i = at_other[0];
      const unsigned want_seq = notified_before(other_mac, i);
      auto want = aecp_frame(other_mac, OWN_MAC, 1, AECP_SUCCESS, EID, other_eid,
                             uint16_t(want_seq), op, body);
      want[36] |= 0x80;
      CHECK(seen[i].f == want, "%s: it is byte-exact: u = 1, that controller's "
            "DA and entity_id, its own sequence_id %u, the response body (Milan "
            "5.4.5.1, IEEE 7.5.2)", tag, want_seq);
      if (seen[i].f != want) { dump("got", seen[i].f); dump("exp", want); }
    }
    CHECK(pushes(req_mac, op, from).empty(),
          "%s: the requester receives no unsolicited response (Milan 5.4.5.2)", tag);
  }
  //! a SET that changes nothing still answers SUCCESS and pushes nothing
  void no_push_step(const char* tag, uint16_t op, const std::vector<uint8_t>& cmd) {
    const size_t from = seen.size();
    const uint16_t s = seq++;
    const auto r = ask(CTLR_MAC, CTLR_EID, s, op, cmd);
    CHECK(status_of(r) == AECP_SUCCESS, "%s: the unchanged SET answers SUCCESS", tag);
    run_ms(200);
    CHECK(pushes(B_MAC, op, from).empty() && pushes(CTLR_MAC, op, from).empty(),
          "%s: a SET that modifies nothing pushes nothing (Milan 5.4.5.2)", tag);
  }

  void configuration_format_and_info() {
    push_step("NP1 SET_CONFIGURATION(1)", AEM_SET_CONFIGURATION, cfg_body(1), cfg_body(1));
    push_step("NP1b SET_CONFIGURATION(0)", AEM_SET_CONFIGURATION, cfg_body(0), cfg_body(0));
    const auto fmt = SetStreamFormatPhase::sf_pl(0x0005, 0, H::SFMT_MAIN_C);
    push_step("NP2 SET_STREAM_FORMAT", AEM_SET_STREAM_FORMAT, fmt, fmt);
    const auto info = stream_info_body(0, 1000000u);
    push_step("NP3 SET_STREAM_INFO", AEM_SET_STREAM_INFO, info, info);
  }
  void control_and_rate() {
    push_step("NP4 SET_CONTROL(IDENTIFY 255)", AEM_SET_CONTROL, control_body(255),
              control_body(255));
    push_step("NP4b SET_CONTROL(IDENTIFY 0)", AEM_SET_CONTROL, control_body(0),
              control_body(0));
    const auto rate = SamplingRateTools::rate_cmd(48000u);
    push_step("NP5 SET_SAMPLING_RATE(48000)", AEM_SET_SAMPLING_RATE, rate, rate);
  }
  void start_and_stop() {
    io.q_acmp.clear();
    feed(acmp_frame(CTLR_MAC, 6, 0, 0, CTLR_EID, T1_EID, EID, T1_UID, 0, 0, 0,
                    0x6CF0, 0, 0));
    run_ms(100);
    CHECK((io.d->acmp_bound_o & 1) && (io.d->aecp_strm_started_o & 1),
          "NP6: sink 0 bound with STREAMING_WAIT clear, so started (premise)");
    push_step("NP6 STOP_STREAMING", 0x0023, ti(0x0005, 0), ti(0x0005, 0));
    push_step("NP7 START_STREAMING", 0x0022, ti(0x0005, 0), ti(0x0005, 0));
  }
  void run() {
    boot_to_idle(true);
    CHECK(register_controller(CTLR_MAC, CTLR_EID, seq++)
              && register_controller(B_MAC, B_EID, seq++),
          "NP0: the requester and a second controller registered");
    configuration_format_and_info();
    control_and_rate();
    start_and_stop();
    //! the requester's own entry never moved while it was excluded: a SET
    //! from B reaches it at the count of what it was actually sent
    push_step("NP8 SET_CONTROL(IDENTIFY 255) from B", AEM_SET_CONTROL,
              control_body(255), control_body(255), true);
    no_push_step("NP9 SET_CONTROL(IDENTIFY 255) again", AEM_SET_CONTROL,
                 control_body(255));
    no_push_step("NP9b START_STREAMING again", 0x0022, ti(0x0005, 0));
  }
};

// ==== ST. STORM: the full registry and counter churn (issue #80) =========
// 09 section 3 STORM: fan-out to the full registry, counter churn at the rate
// limit, and no solicited deadline miss. Milan 5.3.4.2 sizes the list at 16;
// Milan Table 5.22 limits GET_COUNTERS to one notification per descriptor per
// second; 08 F08.1's T-BUDGET-AECP-WC (100 ms) and T-BUDGET-ACMP-RESP (50 ms)
// bound the solicited answers. The ms timebase is compressed (1 ms = 100
// clocks) but the engine's work is counted in real clocks, so the budgets are
// graded in clocks at the wrap's nominal P-CLK-HZ (1,000,001 Hz, clk_ms()):
// a hundred times stricter than the F01.5 default clock.
struct StormPhase : NotifyBench {
  static constexpr uint64_t ROW_MAC = 0x020200CC0000ull;
  static constexpr uint64_t ROW_EID = 0x9999000000000200ull;
  static constexpr unsigned N_ROWS = 16;                // Milan 5.3.4.2
  static constexpr unsigned N_DESC = 5;
  uint16_t seq = 0x6D00;
  long aecp_worst = 0;
  long acmp_worst = 0;

  using NotifyBench::NotifyBench;

  static uint16_t desc_type(unsigned d) {
    return d < 2 ? 0x0005 : d == 2 ? 0x0006 : d == 3 ? 0x0009 : 0x0024;
  }
  static uint16_t desc_index(unsigned d) { return d == 1 ? 1 : 0; }
  static std::vector<uint8_t> name(const char* text) {
    return IdentifyPhase::clock_domain_name(text);
  }
  //! a changed SET_NAME from {mac, eid}; the frames it pushed since `from`
  //! are byte-exact for each registered row but the requester, each at the
  //! count of what that row was sent before (Milan 5.4.5.1)
  int fan_out(uint64_t mac, uint64_t eid, const char* text, unsigned rows) {
    const auto body = name(text);
    const size_t from = seen.size();
    (void)ask(mac, eid, seq++, AEM_SET_NAME, body);
    run_ms(300);
    int good = 0;
    for (unsigned k = 0; k < rows; ++k) {
      if (ROW_MAC + k == mac) continue;
      const auto v = at_mac(ROW_MAC + k, from);
      if (v.size() != 1) continue;
      auto want = aecp_frame(ROW_MAC + k, OWN_MAC, 1, AECP_SUCCESS, EID, ROW_EID + k,
                             uint16_t(notified_before(ROW_MAC + k, v[0])), AEM_SET_NAME,
                             body);
      want[36] |= 0x80;
      good += (seen[v[0]].f == want) ? 1 : 0;
    }
    return good;
  }

  // ---- ST1: all 16 registered, one change, 15 frames ------------------
  void the_full_registry_fans_out() {
    int ok = 0;
    for (unsigned k = 0; k < 5; ++k) ok += register_controller(ROW_MAC + k, ROW_EID + k, seq++);
    const int wave_a = fan_out(CTLR_MAC, CTLR_EID, "Storm Wave A", 5);
    for (unsigned k = 5; k < 10; ++k) ok += register_controller(ROW_MAC + k, ROW_EID + k, seq++);
    const int wave_b = fan_out(CTLR_MAC, CTLR_EID, "Storm Wave B", 10);
    for (unsigned k = 10; k < N_ROWS; ++k) ok += register_controller(ROW_MAC + k, ROW_EID + k, seq++);
    CHECK(ok == 16 && wave_a == 5 && wave_b == 10,
          "ST1: 16 controllers registered in three waves, rows of each wave "
          "notified at different sequence_ids (%d, %d, %d)", ok, wave_a, wave_b);
    const size_t from = seen.size();
    const int good = fan_out(ROW_MAC + 15, ROW_EID + 15, "Storm Full Registry", N_ROWS);
    CHECK(good == 15, "ST1b: one change from row 15 reaches the other fifteen "
          "rows byte-exact, each at its own sequence_id (0, 1 or 2), %d of 15", good);
    CHECK(to_mac(ROW_MAC + 15, from).size() == 1,
          "ST1c: the requester receives only its solicited response");
  }

  // ---- ST2/ST3: counter churn above 1 Hz, solicited answers timed ------
  static constexpr uint64_t PROBE_MAC = 0x020200DD0001ull;   // not registered
  static constexpr uint64_t PROBE_EID = 0x9999000000000300ull;
  std::vector<std::pair<uint16_t, uint64_t>> aecp_probes;    // {seq, sent}
  std::vector<std::pair<uint16_t, uint64_t>> acmp_probes;

  void pulse(unsigned d) {
    io.d->ctr_change_desc_type_i = desc_type(d);
    io.d->ctr_change_desc_index_i = desc_index(d);
    io.d->ctr_change_i = 1;
    tick();
    io.d->ctr_change_i = 0;
    tick();
  }
  void probe(uint16_t s) {
    feed(aecp_frame(OWN_MAC, PROBE_MAC, 0, 0, EID, PROBE_EID, s, AEM_GET_CONFIGURATION, {}));
    aecp_probes.push_back({s, io.t - 4});
    feed(acmp_frame(PROBE_MAC, 10, 0, 0, PROBE_EID, 0, EID, 0, 0, 0, 0, s, 0, 0));
    acmp_probes.push_back({s, io.t - 4});
  }
  //! every descriptor's counters change every 100 ms (10 Hz); a solicited
  //! AECP and an ACMP command every 700 ms
  void churn(long ms) {
    const uint64_t t0 = io.t;
    long next_pulse = 0;
    long next_probe = 50;
    uint16_t s = 0x6E00;
    while (long(io.t - t0) < ms * MS_CYC) {
      const long now = long(io.t - t0);
      if (now >= next_pulse * MS_CYC) {
        for (unsigned d = 0; d < N_DESC; ++d) pulse(d);
        next_pulse += 100;
      } else if (now >= next_probe * MS_CYC) {
        probe(s++);
        next_probe += 700;
      } else {
        tick();
      }
    }
  }
  //! the first frame of each GET_COUNTERS round for descriptor d at row 0
  std::vector<uint64_t> rounds(unsigned d, size_t from) const {
    std::vector<uint64_t> v;
    for (size_t i : at_mac(ROW_MAC, from)) {
      const auto& f = seen[i].f;
      if (unsolicited(f) && f.size() >= 42 && ct_of(f) == 0x0029
          && ((f[38] << 8) | f[39]) == desc_type(d) && ((f[40] << 8) | f[41]) == desc_index(d))
        v.push_back(seen[i].t);
    }
    return v;
  }
  void the_rate_limit_holds_per_descriptor(size_t from, long window_ms) {
    for (unsigned d = 0; d < N_DESC; ++d) {
      const auto r = rounds(d, from);
      long closest = 1L << 40;
      for (size_t i = 1; i < r.size(); ++i) closest = std::min(closest, long(r[i] - r[i - 1]));
      printf("  [i] ST2: descriptor %04x:%u sent %zu rounds, closest %ld clocks apart\n",
             unsigned(desc_type(d)), unsigned(desc_index(d)), r.size(), closest);
      CHECK(r.size() >= 3 && long(r.size()) <= window_ms / 1000 + 1,
            "ST2: descriptor %04x:%u churned at 10 Hz for %ld ms emitted %zu "
            "rounds, want at least 3 and at most one per second",
            unsigned(desc_type(d)), unsigned(desc_index(d)), window_ms, r.size());
      CHECK(r.size() < 2 || closest >= 1000L * MS_CYC - MS_CYC,
            "ST2b: descriptor %04x:%u rounds %ld clocks apart, want at least "
            "1000 ms less the one tick the limiter reads (Milan Table 5.22)",
            unsigned(desc_type(d)), unsigned(desc_index(d)), closest);
    }
  }
  //! every GET_COUNTERS frame of the churn byte-exact at its row's sequence_id
  void every_counter_frame_is_exact(size_t from) {
    int frames = 0;
    int bad = 0;
    for (size_t i = from; i < seen.size(); ++i) {
      const auto& f = seen[i].f;
      if (!unsolicited(f) || f.size() < 42 || ct_of(f) != 0x0029) continue;
      const uint64_t mac = da_of(f);
      const uint16_t ty = uint16_t((f[38] << 8) | f[39]);
      const uint16_t ix = uint16_t((f[40] << 8) | f[41]);
      auto want = aecp_frame(mac, OWN_MAC, 1, AECP_SUCCESS, EID, ROW_EID + (mac - ROW_MAC),
                             uint16_t(notified_before(mac, i)), AEM_GET_COUNTERS,
                             UnsolicitedPhase::counter_body(ty, ix));
      want[36] |= 0x80;
      ++frames;
      bad += (f == want) ? 0 : 1;
    }
    CHECK(frames >= 16 * 3 * int(N_DESC) && bad == 0,
          "ST2c: %d GET_COUNTERS notifications, every one byte-exact at its "
          "row's own sequence_id (%d not)", frames, bad);
  }
  void solicited_answers_stay_inside_their_budgets() {
    long missing = 0;
    for (const auto& p : aecp_probes) {
      long lat = -1;
      for (const auto& s : seen)
        if (da_of(s.f) == PROBE_MAC && !unsolicited(s.f) && seq_of(s.f) == p.first) lat = long(s.t - p.second);
      if (lat < 0) ++missing;
      aecp_worst = std::max(aecp_worst, lat);
    }
    for (const auto& p : acmp_probes) {
      long lat = -1;
      for (const auto& s : seen_acmp)
        if (s.f.size() >= 64 && (s.f[15] & 0x0F) == 11 && ((s.f[62] << 8) | s.f[63]) == p.first)
          lat = long(s.t - p.second);
      if (lat < 0) ++missing;
      acmp_worst = std::max(acmp_worst, lat);
    }
    printf("  [i] ST3: worst solicited latency AECP %ld, ACMP %ld clocks\n", aecp_worst, acmp_worst);
    CHECK(missing == 0 && !aecp_probes.empty(), "ST3: every solicited probe under "
          "the storm was answered (%ld missing of %zu)", missing,
          aecp_probes.size() + acmp_probes.size());
    CHECK(aecp_worst <= clk_ms(100), "ST3b: GET_CONFIGURATION answered within "
          "T-BUDGET-AECP-WC under the storm: worst %ld clocks, bound %ld", aecp_worst, clk_ms(100));
    CHECK(acmp_worst <= clk_ms(50), "ST3c: GET_RX_STATE answered within "
          "T-BUDGET-ACMP-RESP under the storm: worst %ld clocks, bound %ld", acmp_worst, clk_ms(50));
  }

  void run() {
    boot_to_idle(true);
    the_full_registry_fans_out();
    //! ST1's SET_NAMEs are saved by the D3 writer a debounce later (issues
    //! #61, #83), and its ACQUIRE holds dispatch for up to one job: a round
    //! selected before it would leave late and the next on time. The churn
    //! grades the limiter, so it starts once that save is in the device
    long guard = 3 * 500L * MS_CYC;
    while (io.d->d3_unflushed_o && guard-- > 0) tick();
    CHECK(!io.d->d3_unflushed_o, "ST1: the fan-outs' name save has drained before the "
          "churn (premise)");
    //! and at the phase of the millisecond tick it always started at, one
    //! clock before a tick: ST2b reads the wire, and the limiter stamps a
    //! round when it selects it, so a probe's answer that lands before a
    //! round's first frame narrows that gap (a churn started 30 to 95 clocks
    //! later fails ST2b at main ddb3119d too; README limits)
    const uint32_t ms0 = io.d->dbg_now_ms_o;
    while (io.d->dbg_now_ms_o == ms0) tick();
    for (int c = 0; c < MS_CYC - 1; ++c) tick();
    const size_t from = seen.size();
    churn(3500);
    run_ms(1500);
    the_rate_limit_holds_per_descriptor(from, 5000);
    every_counter_frame_is_exact(from);
    solicited_answers_stay_inside_their_budgets();
  }
};

// ==== RN. RND: seeded registry, lock and SET churn against a model (#80) ==
// 09 section 3 RND: randomized multi-controller sessions against an
// independent model, no divergence. Twenty controllers (more than Milan
// 5.3.4.2's sixteen, so the list fills) send a seeded xorshift32 mix of
// REGISTER / DEREGISTER (IEEE 7.4.37 / 7.4.38, Milan 5.4.2.21 / 5.4.2.22),
// LOCK / UNLOCK (IEEE 7.4.2, Milan 5.3.4.1 / 5.4.2.2), SET_CONTROL and
// SET_CLOCK_SOURCE (lock-protected, notifying on a change) and GET_CONTROL.
// The model below knows the list (capacity 16, a refresh keeps its entry's
// sequence_id, a new entry starts at 0), the lock (one holder, keep-alive,
// the 60 s expiry the wrap compresses to 400 ms) and the pushes (every
// registered controller but the requester, per-entry sequence_id; an
// automatic unlock excludes nobody). Each step's solicited response and the
// set of unsolicited frames it caused are compared byte for byte; any other
// frame is a divergence too. The lock is never left in the ambiguous window
// around its expiry: once 250 ms have passed since its last LOCK, the holder
// either refreshes it or the bench waits 600 ms for the automatic unlock.
// The body of a lock-refused SET is issue #53's decision, so that response is
// graded on its status and length alone.
struct RndPhase : NotifyBench {
  static constexpr unsigned N_CTLR = 20;
  static constexpr uint64_t C_MAC = 0x020200EE0000ull;
  static constexpr uint64_t C_EID = 0xAAAA000000000400ull;
  static constexpr uint32_t SEED = 0xC6A46301u;
  static constexpr int STEPS = 720;
  static constexpr long LOCK_REFRESH = 250L * MS_CYC;

  struct Model {
    std::vector<bool> reg = std::vector<bool>(N_CTLR, false);
    std::vector<uint16_t> next_seq = std::vector<uint16_t>(N_CTLR, 0);
    int holder = -1;                            // -1: not locked
    uint64_t lock_at = 0;                       // clock of the holder's last LOCK
    uint8_t identify = 0;
    uint16_t clock_source = 0;
    unsigned count() const {
      unsigned n = 0;
      for (bool r : reg) n += r ? 1 : 0;
      return n;
    }
  };
  Model m;
  uint32_t rng = SEED;
  uint16_t seq = 0x7000;
  long divergences = 0;
  long frames_compared = 0;
  //! how often each arm ran, for the anti-vacuity checks
  long n_full = 0;
  long n_denied = 0;
  long n_expired = 0;
  long n_takes = 0;
  long n_pushes = 0;
  long n_refused_sets = 0;

  using NotifyBench::NotifyBench;

  uint32_t next() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  }
  static uint64_t mac(unsigned c) { return C_MAC + c; }
  static uint64_t eid(unsigned c) { return C_EID + c; }
  static std::vector<uint8_t> lock_body(uint32_t flags, uint64_t locked_id) {
    std::vector<uint8_t> b(16, 0);
    putbe(&b[0], flags, 4);
    putbe(&b[4], locked_id, 8);                 // ENTITY[0] @36 stays 0
    return b;
  }
  static std::vector<uint8_t> control_body(uint8_t v) {
    return PushPhase::control_body(v);
  }
  static std::vector<uint8_t> clock_source_body(uint16_t ix) {
    std::vector<uint8_t> b(8, 0);
    putbe(&b[0], 0x0024, 2);
    putbe(&b[4], ix, 2);                        // index @28, reserved @30
    return b;
  }
  //! the model's pushes: one frame per registered controller but `excl`
  std::vector<std::vector<uint8_t>> pushes_of(int excl, uint16_t op,
                                              const std::vector<uint8_t>& body) {
    std::vector<std::vector<uint8_t>> v;
    for (unsigned c = 0; c < N_CTLR; ++c) {
      if (!m.reg[c] || int(c) == excl) continue;
      auto f = aecp_frame(mac(c), OWN_MAC, 1, AECP_SUCCESS, EID, eid(c), m.next_seq[c]++, op, body);
      f[36] |= 0x80;
      v.push_back(f);
    }
    n_pushes += long(v.size());
    return v;
  }

  struct Expect {
    std::vector<uint8_t> rsp;                   // byte-exact unless status_only
    bool status_only = false;
    unsigned status = 0;
    int cdl = 0;
    std::vector<std::vector<uint8_t>> uns;
  };
  //! one frame compared: counted, and reported the first few times it differs
  void compare(bool same, const char* what, int step) {
    ++frames_compared;
    if (same) return;
    if (++divergences <= 8) printf("  RN step %d: %s diverges from the model\n", step, what);
  }
  //! wait for the response and every expected push (bounded), then a quiet
  //! window in which nothing else may arrive
  void settle(size_t from, uint64_t rmac, uint16_t s, size_t n_uns) {
    for (long c = 0; c < 150L * MS_CYC; ++c) {
      size_t uns = 0;
      bool rsp = false;
      for (size_t i = from; i < seen.size(); ++i) {
        uns += unsolicited(seen[i].f) ? 1 : 0;
        rsp = rsp || (da_of(seen[i].f) == rmac && !unsolicited(seen[i].f) && seq_of(seen[i].f) == s);
      }
      if ((rsp || rmac == 0) && uns >= n_uns) break;
      tick();
    }
    run_ms(5);
  }
  //! everything that arrived since `from` against the model's expectation
  void judge(size_t from, uint64_t rmac, uint16_t s, Expect& e, int step) {
    std::vector<std::vector<uint8_t>> got_uns;
    int responses = 0;
    for (size_t i = from; i < seen.size(); ++i) {
      const auto& f = seen[i].f;
      if (unsolicited(f)) {
        got_uns.push_back(f);
      } else if (rmac != 0 && da_of(f) == rmac && seq_of(f) == s) {
        ++responses;
        const int cdl = int(((f[16] & 0x07) << 8) | f[17]);
        compare(e.status_only ? (status_of(f) == e.status && cdl == e.cdl) : (f == e.rsp),
                "the solicited response", step);
      } else {
        compare(false, "an unexpected frame", step);
      }
    }
    if (rmac != 0) compare(responses == 1, "the response count", step);
    std::sort(got_uns.begin(), got_uns.end());
    std::sort(e.uns.begin(), e.uns.end());
    compare(got_uns == e.uns, "the set of unsolicited frames", step);
  }
  //! one command from controller c, judged against `e`
  void command(unsigned c, uint16_t op, const std::vector<uint8_t>& pl, Expect& e, int step) {
    const size_t from = seen.size();
    const uint16_t s = seq++;
    if (!e.status_only)
      e.rsp = aecp_frame(mac(c), OWN_MAC, 1, e.status, EID, eid(c), s, op, e.rsp);
    feed(aecp_frame(OWN_MAC, mac(c), 0, 0, EID, eid(c), s, op, pl));
    settle(from, mac(c), s, e.uns.size());
    judge(from, mac(c), s, e, step);
  }

  // ---- the model's arms: each fills an Expect, then runs the command ----
  void do_register(unsigned c, int step) {
    Expect e;
    e.rsp = std::vector<uint8_t>(4, 0);         // the flags echo
    if (m.reg[c] || m.count() < 16) {
      if (!m.reg[c]) m.next_seq[c] = 0;         // a new entry (5.4.2.21)
      m.reg[c] = true;
      e.status = AECP_SUCCESS;
    } else {
      e.status = 8;                             // NO_RESOURCES
      ++n_full;
    }
    command(c, 0x0024, std::vector<uint8_t>(4, 0), e, step);
  }
  void do_deregister(unsigned c, int step) {
    Expect e;
    m.reg[c] = false;
    e.status = AECP_SUCCESS;
    command(c, 0x0025, {}, e, step);
  }
  void do_lock(unsigned c, bool unlock, int step) {
    Expect e;
    const bool other = m.holder >= 0 && m.holder != int(c);
    if (other) {
      e.status = AECP_ENTITY_LOCKED;
      ++n_denied;
    } else if (!unlock) {
      e.status = AECP_SUCCESS;
      if (m.holder < 0) {
        m.holder = int(c);
        ++n_takes;
        e.uns = pushes_of(int(c), 0x0001, lock_body(0, eid(c)));
      }
      m.lock_at = io.t;
    } else {
      e.status = AECP_SUCCESS;
      if (m.holder == int(c)) {
        m.holder = -1;
        e.uns = pushes_of(int(c), 0x0001, lock_body(0, 0));
      }
    }
    e.rsp = lock_body(unlock ? 1 : 0, m.holder >= 0 ? eid(unsigned(m.holder)) : 0);
    command(c, 0x0001, lock_body(unlock ? 1 : 0, 0), e, step);
  }
  void do_set_control(unsigned c, uint8_t v, int step) {
    Expect e;
    if (m.holder >= 0 && m.holder != int(c)) {
      e.status_only = true;
      e.status = AECP_ENTITY_LOCKED;
      e.cdl = 17;
      ++n_refused_sets;
    } else {
      e.status = AECP_SUCCESS;
      e.rsp = control_body(v);
      if (v != m.identify) e.uns = pushes_of(int(c), AEM_SET_CONTROL, control_body(v));
      m.identify = v;
    }
    command(c, AEM_SET_CONTROL, control_body(v), e, step);
  }
  void do_set_clock_source(unsigned c, uint16_t ix, int step) {
    Expect e;
    if (m.holder >= 0 && m.holder != int(c)) {
      e.status_only = true;
      e.status = AECP_ENTITY_LOCKED;
      e.cdl = 20;
      ++n_refused_sets;
    } else {
      e.status = AECP_SUCCESS;
      e.rsp = clock_source_body(ix);
      if (ix != m.clock_source)
        e.uns = pushes_of(int(c), AEM_SET_CLOCK_SOURCE, clock_source_body(ix));
      m.clock_source = ix;
    }
    command(c, AEM_SET_CLOCK_SOURCE, clock_source_body(ix), e, step);
  }
  void do_get_control(unsigned c, int step) {
    Expect e;
    e.status = AECP_SUCCESS;
    e.rsp = control_body(m.identify);
    command(c, AEM_GET_CONTROL, PushPhase::ti(0x001A, 0), e, step);
  }
  //! the lock is old: its holder refreshes it, or the bench lets it expire
  void mind_the_lock(int step) {
    if (m.holder < 0 || io.t - m.lock_at < uint64_t(LOCK_REFRESH)) return;
    if (next() % 4 != 0) {
      do_lock(unsigned(m.holder), false, step);
      return;
    }
    Expect e;
    const size_t from = seen.size();
    e.uns = pushes_of(-1, 0x0001, lock_body(0, 0));   // Milan Table 5.22
    m.holder = -1;
    ++n_expired;
    run_ms(600);
    judge(from, 0, 0, e, step);
  }
  void one_step(int step) {
    mind_the_lock(step);
    const unsigned c = next() % N_CTLR;
    const unsigned op = next() % 100;
    if (op < 30) do_register(c, step);
    else if (op < 37) do_deregister(c, step);
    else if (op < 50) do_lock(c, false, step);
    else if (op < 58) do_lock(c, true, step);
    else if (op < 82) do_set_control(c, (next() & 1) ? 255 : 0, step);
    else if (op < 92) do_set_clock_source(c, uint16_t(next() % 3), step);
    else do_get_control(c, step);
  }
  void run() {
    boot_to_idle(true);
    const uint64_t t0 = io.t;
    for (int i = 0; i < STEPS; ++i) one_step(i);
    printf("  [i] RN: seed 0x%08X, %d steps, %ld frames compared in %ld ms; "
           "%ld NO_RESOURCES, %ld lock denials, %ld lock takes, %ld automatic "
           "unlocks, %ld lock-refused SETs, %ld pushes\n", unsigned(SEED), STEPS,
           frames_compared, long((io.t - t0) / MS_CYC), n_full, n_denied, n_takes,
           n_expired, n_refused_sets, n_pushes);
    CHECK(divergences == 0, "RN: %d seeded steps from %u controllers, zero "
          "divergence from the independent registry and lock model (%ld of "
          "%ld frame comparisons diverged)", STEPS, N_CTLR, divergences, frames_compared);
    CHECK(n_full >= 3 && n_denied >= 10 && n_takes >= 5 && n_expired >= 2
              && n_refused_sets >= 10 && n_pushes >= 200,
          "RN b: every arm was exercised (full %ld, denied %ld, takes %ld, "
          "expired %ld, refused %ld, pushes %ld)", n_full, n_denied, n_takes,
          n_expired, n_refused_sets, n_pushes);
    CHECK(io.t - t0 < 30000ULL * MS_CYC, "RN c: the session stays under the "
          "30 s floor of the controller monitor (Milan 5.4.5.3), %ld ms",
          long((io.t - t0) / MS_CYC));
  }
};

[[maybe_unused]] static void run_rnd(H& h) {
  const int checks0 = h.checks;
  const int fails0 = h.fails;
  RndPhase{h}.run();
  printf("RN: %d checks, %d failures\n", h.checks - checks0, h.fails - fails0);
}

[[maybe_unused]] static void run_pushes(H& h) {
  const int checks0 = h.checks;
  const int fails0 = h.fails;
  PushPhase{h}.run();
  printf("NP: %d checks, %d failures\n", h.checks - checks0, h.fails - fails0);
}

[[maybe_unused]] static void run_storm(H& h) {
  const int checks0 = h.checks;
  const int fails0 = h.fails;
  StormPhase{h}.run();
  printf("ST: %d checks, %d failures\n", h.checks - checks0, h.fails - fails0);
}

[[maybe_unused]] static void run_identify(H& h) {
  const int checks0 = h.checks;
  const int fails0 = h.fails;
#ifdef PP_TOP_EN_IDENT
  IdentifyPhase{h}.run();
#else
  IdentifyOffPhase{h}.run();
#endif
  printf("ID: %d checks, %d failures\n", h.checks - checks0, h.fails - fails0);
}
