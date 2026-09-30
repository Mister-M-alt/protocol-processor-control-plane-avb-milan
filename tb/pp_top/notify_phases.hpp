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
// tick of the ms timebase plus that build (SLACK) more.
struct IdentifyPhase : NotifyBench {
  static constexpr uint64_t IDENT_MAC = 0x91E0F0010001ull;      // Table B.1
  static constexpr uint64_t IDENT_EID = 0x90E0F0FFFE010001ull;  // Table 7-180
  static constexpr long BURST = 150L * MS_CYC;                  // T-IDENT-BURST
  static constexpr long REARM = 1000L * MS_CYC;                 // T-IDENT-REARM
  //! one tick (100 clocks) plus the job's build and serialization in this
  //! bench (measured at under 200 clocks, README section ID)
  static constexpr long SLACK = 300;

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
  //! the two gaps of the burst at v[first] are T-IDENT-BURST, never less
  void burst_spacing(const std::vector<Seen>& v, size_t first, const char* tag) {
    if (first + 2 < v.size())
      printf("  [i] %s: gaps %ld and %ld clocks\n", tag,
             static_cast<long>(v[first + 1].t - v[first].t),
             static_cast<long>(v[first + 2].t - v[first + 1].t));
    for (size_t k = first; k + 1 < first + 3 && k + 1 < v.size(); ++k) {
      const long gap = long(v[k + 1].t - v[k].t);
      CHECK(gap >= BURST && gap <= BURST + SLACK,
            "%s: frame %zu to %zu spaced %ld clocks, want T-IDENT-BURST %ld "
            "to %ld (150 ms at 100 clocks/ms, never less)", tag, k - first + 1,
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
    CHECK(gap <= SLACK, "ID3f: the new press's burst follows the running "
          "one's third frame at once (WAITING, then IDENTIFY), %ld clocks "
          "later", gap);
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
    ok = 0;
    for (unsigned k = 0; k < 15; ++k)
      ok += deregister_controller(FAN_MAC + k, FAN_EID + k, uint16_t(0x6B30 + k)) ? 1 : 0;
    CHECK(ok == 15, "ID5h: fifteen controllers deregistered (%d)", ok);
  }

  // ---- ID6: a held engine delays a burst but never bunches it ----------
  //! AECP is held from reset to the D3 terminal (PR #132): the burst pressed
  //! before the restore waits for the release, then keeps its spacing, and
  //! identifySequenceID restarted at 0 with the reset (7.5.1: "starts at zero
  //! (0) on power up or reboot")
  void a_held_engine_never_bunches_a_burst() {
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

  void run() {
    boot_to_idle(true);
    one_press_sends_one_burst();
    a_held_button_re_arms();
    a_release_and_press_inside_a_burst();
    the_command_forms_start_nothing();
    a_fan_out_delays_the_burst_by_one_job();
    a_held_engine_never_bunches_a_burst();
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
