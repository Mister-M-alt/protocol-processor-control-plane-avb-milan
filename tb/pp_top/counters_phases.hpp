// SPDX-License-Identifier: CERN-OHL-W-2.0
// pp_top suite, section K's AVB_INTERFACE and CLOCK_DOMAIN half (lane C7,
// processor issues #44 and #79), on a fresh processor of its own so the main
// run's clock is untouched. The counters are the integrator's (integrator
// guide section 7.1), and this bench's harness plays that integrator: its
// store counts the very link_up_i edges and gm_change_i identity changes it
// drives into the processor, and the section pulses ctr_change_i whenever one
// of those counts moves, as the guide asks. Every expectation is written here
// from IEEE 1722.1-2021 Figure 7-67 and Tables 7-152/7-153 (AVB_INTERFACE:
// LINK_UP at block offset 0, LINK_DOWN at 4, GPTP_GM_CHANGED at 20) and
// 7-154/7-155 (CLOCK_DOMAIN: LOCKED at 0, UNLOCKED at 4), with the Milan v1.2
// Table 5.13 and 5.15 masks and the Table 5.1 / 5.7 invariants; never read
// back from the DUT.
//   K9   AVB_INTERFACE 0 after boot: the boot's link-up, counted once
//   K10  link flaps: LINK_UP = LINK_DOWN or LINK_DOWN + 1 at every sample
//   K11  grandmaster changes counted, a domain-only strobe not; the distinct
//        counts byte-exact at block offsets 0, 4 and 20
//   K12  AVB_INTERFACE 1, absent from the image: NO_SUCH_DESCRIPTOR, and the
//        face is not asked
//   K13  a change strobe pushes the counts of its moment to every registered
//        controller (Milan Table 5.22)
//   K14  changes inside the second after a push go out once, a second after
//        it, with the latest counts
//   K15  AVB_INTERFACE and STREAM_INPUT are throttled apart
//   K16  CLOCK_DOMAIN 0: LOCKED = UNLOCKED or UNLOCKED + 1 byte-exact, and
//        its push
//   K17  a change strobe for an object with no notification slot pushes
//        nothing, and a slotted one still does
// Included by sim_main.cpp after the notification sections, whose bench it
// shares.

struct InterfaceCountersPhase : NotifyBench {
  static constexpr uint64_t B_MAC = 0x0202C2C2C2C2ull;
  static constexpr uint64_t B_EID = CTLR2_EID;
  static constexpr uint16_t DT_SI = 0x0005;
  static constexpr uint16_t DT_SO = 0x0006;
  //! the top's N_STREAM_IN_P and N_STREAM_OUT_P, which this bench leaves at
  //! their defaults: the first Stream Input and Stream Output index past them
  static constexpr uint16_t SI_PAST = 8;
  static constexpr uint16_t SO_PAST = 8;
  static constexpr uint16_t DT_AVB = 0x0009;
  static constexpr uint16_t DT_CKD = 0x0024;
  static constexpr uint32_t MASK_AVB = 0x00000023u;   // Milan Table 5.13
  static constexpr uint32_t MASK_CKD = 0x00000003u;   // Milan Table 5.15
  //! the processor's own sequence_ids for the commands of this section
  uint16_t seq = 0xD200;

  using NotifyBench::NotifyBench;

  //! a Figure 7-67 body from counts written here: {type, index},
  //! counters_valid, then quadlet n at block byte 4n, every other quadlet 0
  static std::vector<uint8_t> body(uint16_t ty, uint16_t ix, uint32_t mask,
                                   const std::vector<std::pair<int, uint32_t>>& quads) {
    std::vector<uint8_t> b(136, 0);
    putbe(&b[0], ty, 2);
    putbe(&b[2], ix, 2);
    putbe(&b[4], mask, 4);
    for (const auto& q : quads) putbe(&b[8 + 4 * q.first], q.second, 4);
    return b;
  }
  static std::vector<uint8_t> avb_body(uint32_t up, uint32_t down, uint32_t gm) {
    return body(DT_AVB, 0, MASK_AVB, {{0, up}, {1, down}, {5, gm}});
  }
  static std::vector<uint8_t> ckd_body(uint32_t locked, uint32_t unlocked) {
    return body(DT_CKD, 0, MASK_CKD, {{0, locked}, {1, unlocked}});
  }
  //! quadlet n of a GET_COUNTERS frame's block (frame byte 46 + 4n)
  static uint32_t quadlet(const std::vector<uint8_t>& f, int n) {
    const int at = 46 + 4 * n;
    return f.size() >= size_t(at + 4) ? uint32_t(fv_u64(f, at, 4)) : 0xFFFFFFFFu;
  }

  //! one GET_COUNTERS from the first controller; its solicited response
  std::vector<uint8_t> get(uint16_t ty, uint16_t ix) {
    std::vector<uint8_t> p(4, 0);
    putbe(&p[0], ty, 2);
    putbe(&p[2], ix, 2);
    return ask(CTLR_MAC, CTLR_EID, ++seq, AEM_GET_COUNTERS, p);
  }
  //! the solicited answer to the last get(), byte-exact
  std::vector<uint8_t> answer(uint8_t status, const std::vector<uint8_t>& b) const {
    return aecp_frame(CTLR_MAC, OWN_MAC, 1, status, EID, CTLR_EID, seq,
                      AEM_GET_COUNTERS, b);
  }
  //! the unsolicited GET_COUNTERS of `body` the controller at `mac` is owed
  //! at log index `at`: its sequence_id is the entry's own count of
  //! notifications before it (Milan 5.4.5.1)
  std::vector<uint8_t> push_of(uint64_t mac, uint64_t eid, size_t at,
                               const std::vector<uint8_t>& b) const {
    auto f = aecp_frame(mac, OWN_MAC, 1, AECP_SUCCESS, EID, eid,
                        uint16_t(notified_before(mac, at)), AEM_GET_COUNTERS, b);
    f[36] |= 0x80;
    return f;
  }
  //! the GET_COUNTERS pushes to `mac` for {ty, ix} since log index `from`
  std::vector<size_t> pushes(uint64_t mac, uint16_t ty, uint16_t ix, size_t from) const {
    std::vector<size_t> v;
    for (size_t i = from; i < seen.size(); ++i) {
      const auto& f = seen[i].f;
      if (da_of(f) == mac && unsolicited(f) && ct_of(f) == AEM_GET_COUNTERS
          && f.size() >= 42 && fv_u64(f, 38, 2) == ty && fv_u64(f, 40, 2) == ix)
        v.push_back(i);
    }
    return v;
  }
  //! every unsolicited GET_COUNTERS to `mac` since log index `from`, of any object
  size_t any_pushes(uint64_t mac, size_t from) const {
    size_t n = 0;
    for (size_t i = from; i < seen.size(); ++i) {
      const auto& f = seen[i].f;
      if (da_of(f) == mac && unsolicited(f) && ct_of(f) == AEM_GET_COUNTERS) ++n;
    }
    return n;
  }
  //! the harness clock, in ms, of the frame at log index i (its last byte)
  uint64_t ms_at(size_t i) const { return seen[i].t / MS_CYC; }

  //! the integrator's duty: one change strobe for a descriptor whose counts moved
  void strobe(uint16_t ty, uint16_t ix) {
    io.d->ctr_change_desc_type_i = ty;
    io.d->ctr_change_desc_index_i = ix;
    io.d->ctr_change_i = 1;
    tick();
    io.d->ctr_change_i = 0;
    io.d->ctr_change_desc_type_i = 0;
    io.d->ctr_change_desc_index_i = 0;
    tick();
  }
  void set_link(bool up) {
    io.d->link_up_i = up ? 1 : 0;
    run_ms(5);
  }
  //! publish a gPTP pair and raise the ADP/GET_AVB_INFO strobe (guide section 6)
  void publish_gm(uint64_t gm, uint8_t domain) {
    io.d->gm_id_i = gm;
    io.d->gptp_domain_i = domain;
    io.d->gm_change_i = 1;
    tick();
    io.d->gm_change_i = 0;
    run_ms(5);
  }

  void run() {
    boot_to_idle(true);
    k9_the_boot_link_up_is_counted_once();
    k10_link_flaps_keep_the_invariant();
    k11_grandmaster_changes_count_and_a_domain_strobe_does_not();
    k12_a_second_interface_is_its_own_object();
    CHECK(register_controller(CTLR_MAC, CTLR_EID, ++seq)
          && register_controller(B_MAC, B_EID, ++seq),
          "K13: both controllers register for notifications");
    run_ms(1200);
    const size_t first = k13_a_change_strobe_pushes_the_counts_of_its_moment();
    k14_changes_inside_the_second_go_out_once_and_late(first);
    k15_interface_and_stream_input_are_throttled_apart();
    k16_clock_domain_counts_and_pushes();
    k17_a_strobe_without_a_slot_pushes_nothing();
    CHECK(deregister_controller(CTLR_MAC, CTLR_EID, ++seq)
          && deregister_controller(B_MAC, B_EID, ++seq),
          "K17: both controllers deregister");
  }

  // ---- K9: the boot's link-up is LINK_UP 1, nothing else moved -----------
  void k9_the_boot_link_up_is_counted_once() {
    const auto f = get(DT_AVB, 0);
    const auto want = answer(AECP_SUCCESS, avb_body(1, 0, 0));
    CHECK(!f.empty() && f == want,
          "K9: AVB_INTERFACE 0 after boot is byte-exact: mask 0x23, LINK_UP 1, "
          "LINK_DOWN 0, GPTP_GM_CHANGED 0");
    if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
  }

  // ---- K10: four flaps, the invariant at every sample ---------------------
  // Milan v1.2 Table 5.1: "either LINK_UP=LINK_DOWN (... the link is
  // currently down), or LINK_UP=LINK_DOWN+1 (... currently up)". Each sample
  // is taken in the link state it grades, from the wire values alone.
  void k10_link_flaps_keep_the_invariant() {
    struct Counts {
      uint32_t up;
      uint32_t down;
    };
    const Counts want[4] = {{1, 1}, {2, 1}, {2, 2}, {3, 2}};
    for (int k = 0; k < 4; ++k) {
      const bool up = (k % 2) == 1;
      set_link(up);
      const auto f = get(DT_AVB, 0);
      const uint32_t lu = quadlet(f, 0);
      const uint32_t ld = quadlet(f, 1);
      const bool invariant = up ? (lu == ld + 1) : (lu == ld);
      CHECK(!f.empty() && f == answer(AECP_SUCCESS, avb_body(want[k].up, want[k].down, 0))
                && invariant,
            "K10: link %s (flap %d): LINK_UP %u, LINK_DOWN %u, want %u and %u, "
            "and LINK_UP = LINK_DOWN%s", up ? "up" : "down", k + 1, lu, ld,
            want[k].up, want[k].down, up ? " + 1" : "");
    }
  }

  // ---- K11: GM changes, and the domain-only strobe that is not one --------
  // Table 5.1 counts grandmaster changes; gm_change_i is also the ADP duty
  // for a domain-only change (guide section 6), which must move nothing.
  // The counts end distinct (3, 2, 5), so a quadlet landing at another's
  // offset cannot pass: #44 acceptance 3, block offsets 0, 4 and 20.
  void k11_grandmaster_changes_count_and_a_domain_strobe_does_not() {
    for (uint64_t k = 1; k <= 5; ++k) publish_gm(0x00A1A2A3A4A5A600ull + k, DOM0);
    auto f = get(DT_AVB, 0);
    CHECK(quadlet(f, 5) == 5, "K11: five grandmaster changes, GPTP_GM_CHANGED %u",
          quadlet(f, 5));
    publish_gm(0x00A1A2A3A4A5A605ull, uint8_t(DOM0 ^ 0x01));   // domain only
    f = get(DT_AVB, 0);
    CHECK(quadlet(f, 5) == 5,
          "K11: a domain-only gm_change_i is not a grandmaster change, "
          "GPTP_GM_CHANGED %u", quadlet(f, 5));
    publish_gm(0x00A1A2A3A4A5A605ull, DOM0);                  // and back
    f = get(DT_AVB, 0);
    const auto want = answer(AECP_SUCCESS, avb_body(3, 2, 5));
    CHECK(!f.empty() && f == want,
          "K11: AVB_INTERFACE 0 byte-exact at block offsets 0, 4, 20: LINK_UP 3, "
          "LINK_DOWN 2, GPTP_GM_CHANGED 5, mask 0x23, every other quadlet 0");
    if (!f.empty() && f != want) { dump("got", f); dump("exp", want); }
  }

  // ---- K12: AVB_INTERFACE 1 is another object, and the image has none -----
  void k12_a_second_interface_is_its_own_object() {
    const uint64_t reads0 = io.ctr_reads;
    const auto f = get(DT_AVB, 1);
    CHECK(!f.empty() && f == answer(AECP_NO_SUCH_DESCRIPTOR, body(DT_AVB, 1, 0, {}))
              && io.ctr_reads == reads0,
          "K12: AVB_INTERFACE 1 answers NO_SUCH_DESCRIPTOR with the zero body, "
          "and the face is not asked (%llu reads)",
          static_cast<unsigned long long>(io.ctr_reads - reads0));
  }

  // ---- K13: one link flap, one strobe, one push of that moment's counts ---
  size_t k13_a_change_strobe_pushes_the_counts_of_its_moment() {
    const size_t from = seen.size();
    set_link(false);                            // LINK_DOWN 3
    strobe(DT_AVB, 0);
    run_ms(300);
    const auto a = pushes(CTLR_MAC, DT_AVB, 0, from);
    const auto b = pushes(B_MAC, DT_AVB, 0, from);
    const auto counts = avb_body(3, 3, 5);
    CHECK(a.size() == 1 && b.size() == 1
              && seen[a[0]].f == push_of(CTLR_MAC, CTLR_EID, a[0], counts)
              && seen[b[0]].f == push_of(B_MAC, B_EID, b[0], counts),
          "K13: one unsolicited GET_COUNTERS of AVB_INTERFACE 0 at each "
          "controller within 300 ms, byte-exact with LINK_UP 3, LINK_DOWN 3 "
          "(got %zu and %zu)", a.size(), b.size());
    return a.empty() ? seen.size() : a[0];
  }

  // ---- K14: two more changes in that second: one push, late, the latest ---
  void k14_changes_inside_the_second_go_out_once_and_late(size_t first) {
    const size_t from = seen.size();
    set_link(true);                             // LINK_UP 4
    strobe(DT_AVB, 0);
    publish_gm(0x00A1A2A3A4A5A606ull, DOM0);    // GPTP_GM_CHANGED 6
    strobe(DT_AVB, 0);
    const uint64_t t0 = ms_at(first);
    while (io.t / MS_CYC < t0 + 850) run_ms(10);
    CHECK(pushes(CTLR_MAC, DT_AVB, 0, from).empty() && pushes(B_MAC, DT_AVB, 0, from).empty(),
          "K14: nothing more for AVB_INTERFACE 0 inside 850 ms of the push");
    run_ms(450);
    const auto a = pushes(CTLR_MAC, DT_AVB, 0, from);
    const auto b = pushes(B_MAC, DT_AVB, 0, from);
    const auto counts = avb_body(4, 3, 6);
    CHECK(a.size() == 1 && b.size() == 1 && ms_at(a[0]) >= t0 + 900
              && seen[a[0]].f == push_of(CTLR_MAC, CTLR_EID, a[0], counts)
              && seen[b[0]].f == push_of(B_MAC, B_EID, b[0], counts),
          "K14: the two changes go out as one push per controller, %llu ms after "
          "the first, byte-exact with the latest counts LINK_UP 4, "
          "GPTP_GM_CHANGED 6 (got %zu and %zu)",
          a.empty() ? 0ull : static_cast<unsigned long long>(ms_at(a[0]) - t0),
          a.size(), b.size());
    run_ms(1200);
    CHECK(pushes(CTLR_MAC, DT_AVB, 0, from).size() == 1,
          "K14: and nothing after it: coalesced, never replayed");
  }

  // ---- K15: a Stream Input's push is not held by the interface's window ---
  // K14's last second has passed, so a new interface change goes out at
  // once and opens a new window; inside it, an interface change waits for
  // that window while a Stream Input change, in a window of its own, does not
  void k15_interface_and_stream_input_are_throttled_apart() {
    const size_t from = seen.size();
    set_link(false);                            // LINK_DOWN 4
    strobe(DT_AVB, 0);
    run_ms(300);
    const auto opened = pushes(CTLR_MAC, DT_AVB, 0, from);
    CHECK(opened.size() == 1
              && seen[opened[0]].f == push_of(CTLR_MAC, CTLR_EID, opened[0],
                                              avb_body(4, 4, 6)),
          "K15: a change a second after the last push goes out at once with "
          "LINK_DOWN 4 (%zu)", opened.size());
    const uint64_t t0 = opened.empty() ? io.t / MS_CYC : ms_at(opened[0]);
    const size_t mid = seen.size();
    set_link(true);                             // LINK_UP 5, held by the window
    strobe(DT_AVB, 0);
    strobe(DT_SI, 0);
    run_ms(300);
    const auto si = pushes(CTLR_MAC, DT_SI, 0, mid);
    CHECK(si.size() == 1 && pushes(CTLR_MAC, DT_AVB, 0, mid).empty()
              && seen[si[0]].f
                     == push_of(CTLR_MAC, CTLR_EID, si[0],
                                UnsolicitedPhase::counter_body(io, DT_SI, 0)),
          "K15: STREAM_INPUT 0 is pushed at once while AVB_INTERFACE 0 waits for "
          "its own second (%zu, %zu)", si.size(),
          pushes(CTLR_MAC, DT_AVB, 0, mid).size());
    while (io.t / MS_CYC < t0 + 1300) run_ms(10);
    const auto a = pushes(CTLR_MAC, DT_AVB, 0, mid);
    CHECK(a.size() == 1 && ms_at(a[0]) >= t0 + 900
              && seen[a[0]].f == push_of(CTLR_MAC, CTLR_EID, a[0], avb_body(5, 4, 6)),
          "K15: ... and AVB_INTERFACE 0 then goes out, a second after its window "
          "opened, with LINK_UP 5");
  }

  // ---- K16: CLOCK_DOMAIN 0, from a lock level the processor never sees ----
  void k16_clock_domain_counts_and_pushes() {
    struct Counts {
      uint32_t locked;
      uint32_t unlocked;
    };
    const Counts want[3] = {{1, 0}, {1, 1}, {2, 1}};
    for (int k = 0; k < 3; ++k) {
      io.mclk_locked = (k % 2) == 0;
      run_ms(5);
      const auto f = get(DT_CKD, 0);
      const uint32_t l = quadlet(f, 0);
      const uint32_t u = quadlet(f, 1);
      CHECK(!f.empty() && f == answer(AECP_SUCCESS, ckd_body(want[k].locked, want[k].unlocked))
                && (io.mclk_locked ? l == u + 1 : l == u),
            "K16: media clock %s: LOCKED %u, UNLOCKED %u, want %u and %u, "
            "and LOCKED = UNLOCKED%s", io.mclk_locked ? "locked" : "unlocked", l,
            u, want[k].locked, want[k].unlocked, io.mclk_locked ? " + 1" : "");
    }
    const size_t from = seen.size();
    strobe(DT_CKD, 0);
    run_ms(300);
    const auto a = pushes(CTLR_MAC, DT_CKD, 0, from);
    const auto b = pushes(B_MAC, DT_CKD, 0, from);
    CHECK(a.size() == 1 && b.size() == 1
              && seen[a[0]].f == push_of(CTLR_MAC, CTLR_EID, a[0], ckd_body(2, 1))
              && seen[b[0]].f == push_of(B_MAC, B_EID, b[0], ckd_body(2, 1)),
          "K16: CLOCK_DOMAIN 0 is pushed to both controllers byte-exact (%zu, %zu)",
          a.size(), b.size());
  }

  // ---- K17: a strobe the notification block keeps no slot for ------------
  // The block keeps one slot per Stream Input and Stream Output index of the
  // shape, one for AVB_INTERFACE 0 and one for CLOCK_DOMAIN 0 (06 section
  // 6.6); a strobe naming any other object is ignored (integrator guide
  // section 7.1), never folded onto a slot it does not name. Each strobe is
  // graded alone, for longer than the one-second window K13 to K16 left
  // open, so a strobe that marked any kept slot would push inside the wait.
  void k17_a_strobe_without_a_slot_pushes_nothing() {
    struct Unslotted {
      uint16_t ty;
      uint16_t ix;
      const char* name;
    };
    const Unslotted none[4] = {{DT_AVB, 1, "AVB_INTERFACE 1"},
                               {DT_CKD, 1, "CLOCK_DOMAIN 1"},
                               {DT_SI, SI_PAST, "STREAM_INPUT 8"},
                               {DT_SO, SO_PAST, "STREAM_OUTPUT 8"}};
    for (const auto& u : none) {
      const size_t from = seen.size();
      strobe(u.ty, u.ix);
      run_ms(1500);
      const size_t a = any_pushes(CTLR_MAC, from);
      const size_t b = any_pushes(B_MAC, from);
      CHECK(a == 0 && b == 0,
            "K17: a ctr_change_i for %s, which has no slot, pushes nothing in "
            "1.5 s (got %zu and %zu)", u.name, a, b);
    }
    // the silence above is the decode's: a slotted strobe still pushes
    const size_t from = seen.size();
    set_link(false);                            // LINK_DOWN 5
    strobe(DT_AVB, 0);
    run_ms(300);
    const auto a = pushes(CTLR_MAC, DT_AVB, 0, from);
    const auto b = pushes(B_MAC, DT_AVB, 0, from);
    const auto counts = avb_body(5, 5, 6);
    CHECK(a.size() == 1 && b.size() == 1 && any_pushes(CTLR_MAC, from) == 1
              && seen[a[0]].f == push_of(CTLR_MAC, CTLR_EID, a[0], counts)
              && seen[b[0]].f == push_of(B_MAC, B_EID, b[0], counts),
          "K17: and AVB_INTERFACE 0's own strobe still pushes it at once, "
          "byte-exact with LINK_DOWN 5 (got %zu and %zu)", a.size(), b.size());
  }
};

//! section K's interface half on a fresh model; `--counters-only` runs it alone
[[maybe_unused]] static void run_counters(H& h) {
  const int checks0 = h.checks;
  const int fails0 = h.fails;
  InterfaceCountersPhase{h}.run();
  printf("K-AVB: %d checks, %d failures\n", h.checks - checks0, h.fails - fails0);
}
