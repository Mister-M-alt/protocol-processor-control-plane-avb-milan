// SPDX-License-Identifier: CERN-OHL-W-2.0
// pp_top suite, section AI (issue #217): the ENTITY descriptor's
// available_index is ADP's. IEEE 1722.1-2021 Table 7-2: "the same as the
// available_index field in ATDECC Discovery Protocol", and 6.2.2.15: it is
// incremented after transmitting an ENTITY_AVAILABLE and reset to zero when
// transmitting an ENTITY_DEPARTING. So READ_DESCRIPTOR(ENTITY) carries the
// value of the last ENTITY_AVAILABLE sent: 0 after the first, N-1 after N,
// and 0 again after ENTITY_DEPARTING. Included by sim_main.cpp after section
// RBT; a fresh processor of its own (the AD pattern), so the count of
// advertisements is this section's alone.
//   AI1  before any ADPDU: the descriptor is the image, available_index 0
//   AI2  after each of four ENTITY_AVAILABLE, byte-exact own_avail(0..3) and
//        so on ADP's own cadence: the response is the image with that
//        frame's available_index at descriptor octet 36, every other byte
//        the image's (a whole-frame compare)
//   AI3  entity_enable_i low: one ENTITY_DEPARTING carrying 4, then the
//        descriptor's available_index is 0 again
//   AI4  enabled again: the first ENTITY_AVAILABLE carries 0 and so does the
//        descriptor; after the second, 1
// Every expectation is built here from the clause byte offsets, never read
// back from the DUT: the response's descriptor opens at AECPDU octet 28
// (7.4.5.2, after configuration_index and reserved), wire byte 42.

struct AvailIndexPhase {
  H& h;                                        // the tally
  const milan::tb::Model<Vpp_top_wrap> model;
  H io;
  uint16_t sequence = 0x7D00;

  explicit AvailIndexPhase(H& tally) : h(tally), io(model.get()) {}

  //! the ADPDU's available_index, wire bytes 50..53 (F04.5, 6.2.1.2)
  static uint32_t adp_index(const std::vector<uint8_t>& f) {
    return f.size() >= 54 ? ((uint32_t(f[50]) << 24) | (uint32_t(f[51]) << 16)
                             | (uint32_t(f[52]) << 8) | uint32_t(f[53]))
                          : 0xFFFFFFFFu;
  }
  static bool available(const std::vector<uint8_t>& f) {
    return f.size() > 15 && f[14] == 0xFA && (f[15] & 0x0F) == 0;
  }
  static bool departing(const std::vector<uint8_t>& f) {
    return f.size() > 15 && f[14] == 0xFA && (f[15] & 0x0F) == 1;
  }
  //! the descriptor's available_index in a READ_DESCRIPTOR(ENTITY) response:
  //! descriptor octet 36 at wire byte 42 + 36
  static uint32_t descriptor_index(const std::vector<uint8_t>& r) {
    return r.size() >= 82 ? ((uint32_t(r[78]) << 24) | (uint32_t(r[79]) << 16)
                             | (uint32_t(r[80]) << 8) | uint32_t(r[81]))
                          : 0xFFFFFFFFu;
  }
  //! the whole expected response: the image's ENTITY descriptor with
  //! available_index `aidx`, every other octet as the image holds it
  static std::vector<uint8_t> want(uint16_t sq, uint32_t aidx) {
    std::vector<uint8_t> pl(4, 0);
    putbe(&pl[0], CFGIX, 2);
    std::vector<uint8_t> ent = entity_descriptor();
    putbe(&ent[36], aidx, 4);
    pl.insert(pl.end(), ent.begin(), ent.end());
    return aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID, sq,
                      AEM_READ_DESCRIPTOR, pl);
  }
  //! READ_DESCRIPTOR(ENTITY 0) and its answer, graded whole against `aidx`
  void read_entity(uint32_t aidx, const char* tag, const char* when) {
    std::vector<uint8_t> pl(8, 0);
    putbe(&pl[0], CFGIX, 2);
    putbe(&pl[4], 0x0000, 2);                  // ENTITY
    const uint16_t sq = sequence++;
    io.q_aecp.clear();
    io.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, sq,
                       AEM_READ_DESCRIPTOR, pl));
    const auto r = io.wait_any(io.q_aecp, 600);
    const auto w = want(sq, aidx);
    REQ_TAG("REQ-ADP-011", "DIR", "READ_DESCRIPTOR(ENTITY) %s carries available_index");
    CHECK(r == w,
          "%s: READ_DESCRIPTOR(ENTITY) %s carries available_index %u, the image's "
          "other octets byte-exact (got %zu B, available_index %u)", tag, when, aidx,
          r.size(), descriptor_index(r));
    if (r != w && !r.empty()) { dump("got ", r); dump("want", w); }
  }

  //! a fresh processor: reset and both restore walks over a blank device,
  //! then the link up with the entity not yet enabled
  void boot() {
    Suite image(io);
    image.load_descriptor_image();
    io.reset();
    io.d->restore_go_i = 1;
    io.idle(5);
    io.d->restore_go_i = 0;
    long budget = 400000;
    while (!io.d->restore_done_o && budget-- > 0) io.step();
    CHECK(io.d->restore_done_o && !io.d->restore_fail_o,
          "AI boot: both restore walks end over the blank device");
    io.flush_all();
    io.q_aecp.clear();
    io.d->link_up_i = 1;
    io.idle(50);
  }

  //! the next own ENTITY_AVAILABLE inside `ms`, which must be own_avail(k)
  std::vector<uint8_t> next_available(uint32_t k, int ms, const char* tag) {
    const auto a = io.wait_frame(io.q_adp, ms, available);
    CHECK(a == own_avail(k),
          "%s: ENTITY_AVAILABLE %u byte-exact, available_index %u (got %zu B, %u)",
          tag, k + 1, k, a.size(), adp_index(a));
    return a;
  }

  void run() {
    boot();
    read_entity(0, "AI1", "before any ADPDU");
    io.d->entity_enable_i = 1;
    for (uint32_t k = 0; k < 4; ++k) {
      //! T-ADP-DELAY-START (0..2 s) for the first, then the 5 s cadence
      //! (Milan v1.2 5.6.3.5.2, 5.6.3.5.9)
      next_available(k, k == 0 ? 2600 : 9800, "AI2");
      read_entity(k, "AI2", "after that ENTITY_AVAILABLE");
    }
    //! SHUTDOWN (Milan v1.2 5.6.3.5.8): the DEPARTING carries the index the
    //! next ENTITY_AVAILABLE would have, 4, and 6.2.2.15 resets it to 0
    io.d->entity_enable_i = 0;
    const auto dep = io.wait_frame(io.q_adp, 400, departing);
    CHECK(departing(dep) && adp_index(dep) == 4,
          "AI3: one ENTITY_DEPARTING carrying available_index 4 (got %zu B, %u)",
          dep.size(), adp_index(dep));
    read_entity(0, "AI3", "after ENTITY_DEPARTING");
    io.d->entity_enable_i = 1;
    next_available(0, 4200, "AI4");
    read_entity(0, "AI4", "after the first ENTITY_AVAILABLE re-enabled");
    next_available(1, 9800, "AI4");
    read_entity(1, "AI4", "after the second ENTITY_AVAILABLE re-enabled");
  }
};

[[maybe_unused]] static void run_avail_index(H& h) {
  const int checks0 = h.checks;
  const int fails0 = h.fails;
  AvailIndexPhase{h}.run();
  printf("AI: %d checks, %d failures\n", h.checks - checks0, h.fails - fails0);
}
