// SPDX-License-Identifier: CERN-OHL-W-2.0
// Milan 5.3.8.6/.8 and Table 5.22, through the real MAC/ADP/ACMP/SRP path.
// No record pokes or status injection: the peer drives protocol messages,
// and two unanswered probes drive the real timer/record timeout path.
struct InternalStreamInfoPhase {
  H& h;
  const milan::tb::Model<Vpp_top_wrap> model;
  H io;
  uint16_t sequence = 0x7900;
  uint16_t unsolicited_sequence = 0;
  static constexpr uint64_t BRIDGE0 = 0xB17D23456789ABCDull;
  static constexpr uint64_t BRIDGE1 = 0x9EAF1029384756C2ull;

  explicit InternalStreamInfoPhase(H& tally) : h(tally), io(model.get()) {}

  static uint64_t talker(unsigned sink) { return T1_EID + sink; }
  static uint64_t sid(unsigned sink) { return 0x123456789ABC0100ull + sink; }
  static uint64_t da(unsigned sink) { return 0x91E0F0000100ull + sink; }

  void boot() {
    // Power-cycle with erased media, so reset is checked independently of
    // the separate binding-restore behavior covered by the NVM suites.
    for (auto& region : io.nv_mem) std::fill(region.begin(), region.end(), 0xFF);
    io.nv_st = H::NvState::NV_IDLE;
    io.nv_done_lag = 0;
    io.reset();
    io.d->restore_go_i = 1;
    io.idle(5);
    io.d->restore_go_i = 0;
    unsigned budget = 400000;
    while (!io.d->restore_done_o && budget-- != 0) io.step();
    CHECK(io.d->restore_done_o && !io.d->restore_fail_o,
          "GI boot: blank NVM releases the real listener");
    io.d->link_up_i = 1;
    io.d->entity_enable_i = 1;
    io.run_ms(30);
    io.q_aecp.clear();
    io.q_acmp.clear();
  }

  void register_controller() {
    const auto seq = sequence++;
    io.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, seq,
                      0x0024, std::vector<uint8_t>(4, 0)));
    auto f = io.wait_any(io.q_aecp, 500);
    CHECK(f.size() >= 38 && fv_u64(f, 34, 2) == seq
          && ((f[16] >> 3) & 31) == 0,
          "GI register: controller registered for real notifications");
    unsolicited_sequence = 0;
  }

  std::vector<uint8_t> query(unsigned sink) {
    std::vector<uint8_t> body(4, 0);
    putbe(&body[0], 0x0005, 2);
    putbe(&body[2], sink, 2);
    const auto seq = sequence++;
    io.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, seq,
                      0x000F, body));
    for (int budget = 500 * MS_CYC; budget != 0; --budget) {
      auto found = std::find_if(io.q_aecp.begin(), io.q_aecp.end(),
          [seq](const std::vector<uint8_t>& f) {
            return f.size() >= 38 && !(f[36] & 0x80) && fv_u64(f, 34, 2) == seq;
          });
      if (found != io.q_aecp.end()) {
        auto f = *found;
        io.q_aecp.erase(found);
        return f;
      }
      io.step();
    }
    return {};
  }

  void check_frame(const std::vector<uint8_t>& f, const char* phase,
                   unsigned sink, uint8_t pb, uint8_t acmp,
                   uint8_t code, uint64_t bridge, bool uns, bool known = true) {
    const char* kind = uns ? "unsolicited" : "solicited";
    CHECK(f.size() == 94, "GI %s %s: complete Milan response", phase, kind);
    if (f.size() != 94) return;
    CHECK(f[90] >> 5 == pb, "GI %s %s: pbsta sink %u = %u, got %u",
          phase, kind, sink, pb, f[90] >> 5);
    CHECK((f[90] & 31) == acmp, "GI %s %s: acmpsta sink %u = %u, got %u",
          phase, kind, sink, acmp, f[90] & 31);
    CHECK(f[72] == code, "GI %s %s: failure code sink %u = %u, got %u",
          phase, kind, sink, code, f[72]);
    CHECK(fv_u64(f, 74, 8) == bridge,
          "GI %s %s: full failure bridge sink %u", phase, kind, sink);
    auto body = StreamInfoPhase::gsi_body(0x0005, sink, known,
                                        uint8_t((pb << 5) | acmp));
    body[34] = code;
    putbe(&body[36], bridge, 8);
    auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, known ? 0 : 2, EID, CTLR_EID,
                          uns ? unsolicited_sequence++ : sequence - 1,
                          0x000F, body);
    if (uns) want[36] |= 0x80;
    CHECK(f == want, "GI %s %s: byte-exact sink %u response", phase, kind, sink);
    if (f != want) { dump("GI got", f); dump("GI expected", want); }
  }

  void pair(const char* phase, unsigned sink, uint8_t pb, uint8_t acmp = 0,
            uint8_t code = 0, uint64_t bridge = 0, bool known = true,
            int wait_ms = 500) {
    auto uns = io.wait_any(io.q_aecp, wait_ms);
    check_frame(uns, phase, sink, pb, acmp, code, bridge, true, known);
    auto solicited = query(sink);
    check_frame(solicited, phase, sink, pb, acmp, code, bridge, false, known);
    CHECK(uns.size() == 94 && solicited.size() == 94
          && std::equal(uns.begin() + 38, uns.end(), solicited.begin() + 38),
          "GI %s: solicited and unsolicited bodies agree for sink %u", phase, sink);
  }

  void binding(unsigned sink, bool bind) {
    const auto seq = sequence++;
    io.feed(acmp_frame(CTLR_MAC, bind ? 6 : 8, 0, 0, CTLR_EID,
                      talker(sink), EID, T1_UID, sink, 0, 0, seq, 0, 0));
    auto f = io.wait_frame(io.q_acmp, 500, [seq](const std::vector<uint8_t>& r) {
      return r.size() == 70 && fv_u64(r, 62, 2) == seq
          && ((r[15] & 15) == 7 || (r[15] & 15) == 9);
    });
    CHECK(!f.empty() && ((f[16] >> 3) & 31) == 0,
          "GI binding: %s sink %u accepted", bind ? "bind" : "unbind", sink);
  }

  void discover(unsigned sink) {
    io.feed(adp_frame(0, T1_MAC + sink, talker(sink), 31, 0, GM0, DOM0,
                      0xBBB0000000000001ull, 8, TKCAP, 0, 0,
                      0x0000C588u, 0, 0));
  }

  std::vector<uint8_t> probe(unsigned sink, int wait_ms = 1600) {
    auto f = io.wait_frame(io.q_acmp, wait_ms,
        [sink](const std::vector<uint8_t>& r) {
          return r.size() == 70 && (r[15] & 15) == 0
              && fv_u64(r, 52, 2) == sink;
        });
    CHECK(!f.empty(), "GI probe: real PROBE_TX for sink %u", sink);
    return f;
  }

  void settle(unsigned sink, const std::vector<uint8_t>& p) {
    if (p.size() != 70) return;
    io.feed(acmp_frame(T1_MAC + sink, 1, 0, sid(sink), CTLR_EID,
                      talker(sink), EID, T1_UID, sink, da(sink), 0,
                      fv_u64(p, 62, 2), 0, 2));
    pair("COMPLETED", sink, 3);
  }

  void bind_without_talker(unsigned sink) {
    // Milan 5.5.3.5.3 starts probing immediately on a new BIND_RX.
    // With no discovered talker, 5.5.3.5.29 reaches PASSIVE after backoff.
    binding(sink, true);
    pair("BIND-ACTIVE", sink, 2);
    const auto first = probe(sink);
    const auto retry = probe(sink, 500);
    CHECK(!first.empty() && first == retry,
          "GI initial timeout: exact retry for sink %u", sink);
    pair("BIND-TIMEOUT", sink, 2, 7);
    pair("PASSIVE", sink, 1, 0, 0, 0, true, 5000);
  }

  void attribute(unsigned sink, uint8_t code, uint64_t bridge, bool failed,
                 int event = EV_NEW) {
    auto fv = fv_talker(sid(sink), da(sink), 2, 256, 1, 3, 1, 0x12345);
    if (failed) {
      const auto n = fv.size();
      fv.resize(n + 9);
      putbe(&fv[n], bridge, 8);
      fv[n + 8] = code;
    }
    io.feed(mrpdu_frame(true, T1_MAC + sink,
        {Msg{failed ? 2 : 1, failed ? 34 : 25, false,
             {Vec{false, 1, fv, {event}, {}}}}}));
  }

  void run() {
    std::vector<ImgEnt> entries{
        {CFGIX, 0x0000, 1, 312, 0, 312, 0},
        {CFGIX, 0x0005, 2, 140, 1, 144, 0}};
    io.dram = build_image(entries,
        {entity_descriptor(), stream_descriptor(0x0005, 0),
         stream_descriptor(0x0005, 1)}, {"Entity", "Input 0", "Input 1"}, 1);
    io.gsi_retired_stuck = true;
    boot();
    for (unsigned s = 0; s < 2; ++s)
      check_frame(query(s), "RESET", s, 0, 0, 0, 0, false);
    register_controller();
    bind_without_talker(0);
    bind_without_talker(1);
    binding(1, true);
    io.run_ms(100);
    CHECK(io.q_aecp.empty(), "GI repeat bind: no fabricated status transition");

    discover(0);
    pair("ACTIVE", 0, 2);
    check_frame(query(1), "OTHER-PASSIVE", 1, 1, 0, 0, 0, false);
    const auto first = probe(0);
    const auto retry = probe(0, 500);
    CHECK(!first.empty() && first == retry, "GI timeout: second probe is an exact retry");
    pair("TIMEOUT", 0, 2, 7);
    check_frame(query(1), "OTHER-PASSIVE", 1, 1, 0, 0, 0, false);

    // Hardware sink 2 exists, but the descriptor image deliberately stops
    // at 1. Its real ACTIVE record must not leak into either error body.
    binding(2, true);
    pair("MISSING", 2, 0, 0, 0, 0, false);
    binding(2, false);
    pair("MISSING-UNBIND", 2, 0, 0, 0, 0, false);
    pair("RETRY-CLEAR", 0, 2, 0, 0, 0, true, 5000);
    settle(0, probe(0));
    discover(1);
    pair("ACTIVE", 1, 2);
    settle(1, probe(1));

    attribute(0, 7, BRIDGE0, true);
    pair("FAILED-0", 0, 3, 0, 7, BRIDGE0);
    attribute(1, 11, BRIDGE1, true);
    pair("FAILED-1", 1, 3, 0, 11, BRIDGE1);
    check_frame(query(0), "DISTINCT-0", 0, 3, 0, 7, BRIDGE0, false);
    attribute(0, 9, BRIDGE0 ^ 0xFFFF000000000000ull, true);
    pair("FAILED-REFRESH", 0, 3, 0, 9, BRIDGE0 ^ 0xFFFF000000000000ull);
    attribute(0, 9, BRIDGE0 ^ 0xFFFF000000000000ull, true);
    io.run_ms(100);
    CHECK(io.q_aecp.empty(), "GI unchanged Failed: no fabricated failure transition");
    attribute(0, 0, 0, false);
    pair("ADVERTISE", 0, 3);
    attribute(0, 7, BRIDGE0, true);
    pair("FAILED-AGAIN", 0, 3, 0, 7, BRIDGE0);
    attribute(0, 7, BRIDGE0, true, EV_LV);
    pair("WITHDRAWAL", 0, 2);
    pair("WITHDRAWAL-PROBING", 0, 2);
    check_frame(query(1), "DISTINCT-1", 1, 3, 0, 11, BRIDGE1, false);
    binding(0, false);
    pair("DISABLED", 0, 0);
    // Reset with sink 1 still COMPLETED and registering a nonzero Failed.
    boot();
    for (unsigned s = 0; s < 2; ++s)
      check_frame(query(s), "RESET-LIVE", s, 0, 0, 0, 0, false);
    register_controller();
    bind_without_talker(1);
    binding(1, false);
    pair("DISABLED", 1, 0);
    io.run_ms(100);
    CHECK(io.q_aecp.empty(), "GI final: no duplicate notifications");
    CHECK(io.gsi_internal_leaks == 0,
          "GI internal seam: selectors 5/7 never requested from integrator");
  }
};
