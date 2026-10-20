// SPDX-License-Identifier: CERN-OHL-W-2.0
// Milan 5.3.8.6/.8 and Table 5.22, through the real MAC/ADP/ACMP/SRP path.
// No record pokes or status injection: the peer drives protocol messages,
// and two unanswered probes drive the real timer/record timeout path. The
// harness integrator folds the published binding/started view into the
// flags word (H::gsi_fold_sw), so STREAMING_WAIT is graded byte-exact too.
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
                   uint8_t code, uint64_t bridge, bool uns, bool known = true,
                   bool sw = false, uint32_t latency = 0) {
    const char* kind = uns ? "unsolicited" : "solicited";
    CHECK(f.size() == 94, "GI %s %s: complete Milan response", phase, kind);
    if (f.size() != 94) return;
    REQ_TAG("REQ-ACMP-023", "DIR", "pbsta sink");
    CHECK(f[90] >> 5 == pb, "GI %s %s: pbsta sink %u = %u, got %u",
          phase, kind, sink, pb, f[90] >> 5);
    REQ_TAG("REQ-ACMP-023", "DIR", "acmpsta sink");
    CHECK((f[90] & 31) == acmp, "GI %s %s: acmpsta sink %u = %u, got %u",
          phase, kind, sink, acmp, f[90] & 31);
    REQ_TAG("REQ-NET-003", "DIR", "failure code sink");
    CHECK(f[72] == code, "GI %s %s: failure code sink %u = %u, got %u",
          phase, kind, sink, code, f[72]);
    CHECK(fv_u64(f, 74, 8) == bridge,
          "GI %s %s: full failure bridge sink %u", phase, kind, sink);
    auto body = StreamInfoPhase::gsi_body(0x0005, sink, known,
                                        uint8_t((pb << 5) | acmp));
    putbe(&body[44], pb == 3 ? 2 : 0, 2);
    body[34] = code;
    putbe(&body[36], bridge, 8);
    if (io.gsi_fold_latency && known) putbe(&body[24], latency, 4);
    if (sw) body[7] |= 0x08;                      // STREAMING_WAIT
    auto want = aecp_frame(CTLR_MAC, OWN_MAC, 1, known ? 0 : 2, EID, CTLR_EID,
                          uns ? unsolicited_sequence++ : sequence - 1,
                          0x000F, body);
    if (uns) want[36] |= 0x80;
    CHECK(f == want, "GI %s %s: byte-exact sink %u response", phase, kind, sink);
    if (f != want) { dump("GI got", f); dump("GI expected", want); }
  }

  void pair(const char* phase, unsigned sink, uint8_t pb, uint8_t acmp = 0,
            uint8_t code = 0, uint64_t bridge = 0, bool known = true,
            int wait_ms = 500, bool sw = false) {
    auto uns = io.wait_any(io.q_aecp, wait_ms);
    check_frame(uns, phase, sink, pb, acmp, code, bridge, true, known, sw);
    auto solicited = query(sink);
    check_frame(solicited, phase, sink, pb, acmp, code, bridge, false, known,
                sw);
    CHECK(uns.size() == 94 && solicited.size() == 94
          && std::equal(uns.begin() + 38, uns.end(), solicited.begin() + 38),
          "GI %s: solicited and unsolicited bodies agree for sink %u", phase, sink);
  }

  void binding(unsigned sink, bool bind, uint64_t tk = 0,
               uint16_t flags = 0) {
    const auto seq = sequence++;
    io.feed(acmp_frame(CTLR_MAC, bind ? 6 : 8, 0, 0, CTLR_EID,
                      tk ? tk : talker(sink), EID, T1_UID, sink, 0, 0, seq,
                      flags, 0));
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
                 int event = EV_NEW, uint32_t latency = 0x12345) {
    auto fv = fv_talker(sid(sink), da(sink), 2, 256, 1, 3, 1, latency);
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

  // Every Listener declaration (attribute 3) the processor transmits for this
  // sink's stream with the New event over `ms`, each vector value checked.
  int listener_news(unsigned sink, int ms) {
    int n = 0;
    for (int t = 0; t < ms; ++t) {
      io.run_ms(1);
      while (!io.q_msrp.empty()) {
        const auto p = parse_mrpdu(io.q_msrp.front());
        io.q_msrp.pop_front();
        if (!p.msrp) continue;
        for (const auto& v : p.vecs) {
          if (v.type != 3 || v.fv.size() < 8) continue;
          const uint64_t first = fv_u64(v.fv, 0, 8);
          for (size_t j = 0; j < v.ev.size(); ++j)
            if (first + j == sid(sink) && v.ev[j] == EV_NEW) ++n;
        }
      }
    }
    return n;
  }

  // Unsolicited GET_STREAM_INFO responses naming STREAM_INPUT `sink`, queued
  // or arriving within `ms`; other frames stay queued.
  std::vector<std::vector<uint8_t>> input_uns(unsigned sink, int ms) {
    std::vector<std::vector<uint8_t>> got;
    auto take = [&]() {
      for (auto it = io.q_aecp.begin(); it != io.q_aecp.end();) {
        const auto& f = *it;
        if (f.size() == 94 && (f[36] & 0x80) && fv_u64(f, 38, 2) == 0x0005
            && fv_u64(f, 40, 2) == sink) {
          got.push_back(f);
          it = io.q_aecp.erase(it);
        } else {
          ++it;
        }
      }
    };
    take();
    for (int t = 0; t < ms; ++t) { io.run_ms(1); take(); }
    return got;
  }

  // Table 5.22 started/stopped through F05.3 "BIND_RX new source" in
  // PRB_W_RESP: a re-bind naming ANOTHER talker with STREAMING_WAIT runs A2
  // without A10, keeps pbsta/acmpsta at ACTIVE/0 and stops the sink. It is
  // a committed change of a visible field, so exactly one unsolicited
  // response reports it, carrying the new STREAMING_WAIT.
  void rebind_from_probing(unsigned sink) {
    binding(sink, true);
    pair("REBIND-BOUND", sink, 2);
    probe(sink);                                 // now PRB_W_RESP
    const bool started = (io.d->aecp_strm_started_o >> sink) & 1;
    binding(sink, true, talker(sink) + 0x55, 0x0008);
    const auto uns = input_uns(sink, 150);
    CHECK(started && !((io.d->aecp_strm_started_o >> sink) & 1),
          "GI REBIND-SW: the re-bind with STREAMING_WAIT stopped sink %u",
          sink);
    CHECK(uns.size() == 1,
          "GI REBIND-SW: exactly one unsolicited response for sink %u, "
          "got %zu", sink, uns.size());
    const auto pushed = uns.empty() ? std::vector<uint8_t>{} : uns[0];
    check_frame(pushed, "REBIND-SW", sink, 2, 0, 0, 0, true, true, true);
    const auto solicited = query(sink);
    check_frame(solicited, "REBIND-SW", sink, 2, 0, 0, 0, false, true, true);
    CHECK(pushed.size() == 94 && solicited.size() == 94
          && std::equal(pushed.begin() + 38, pushed.end(),
                        solicited.begin() + 38),
          "GI REBIND-SW: solicited and unsolicited bodies agree");
    // the new talker never answers: the double timeout still reports
    pair("REBIND-TIMEOUT", sink, 2, 7, 0, 0, true, 700, true);
    binding(sink, false);
    pair("REBIND-UNBIND", sink, 0);
  }

  // An index at or past N_STREAM_IN_P must never be narrowed onto a
  // hardware sink: STREAM_INPUT 9 of a ten-input image aliases sink 1 in
  // the three-bit sink index. Solicited only: notifications are raised per
  // hardware sink, so no unsolicited response can name an index past the
  // shape.
  void index_guard(unsigned alias, uint8_t alias_pb) {
    check_frame(query(9), "INDEX-GUARD", 9, 0, 0, 0, 0, false);
    check_frame(query(alias), "INDEX-GUARD-ALIAS", alias, alias_pb, 0, 0, 0,
                false);
  }

  // Two settled, registered streams: sink 0 Advertise, sink 1 Failed.
  // Change only the latency bytes of a real Talker JoinIn refresh; grade
  // every emitted response and the solicited read through the same gather.
  void latency_refresh(unsigned sink, uint32_t latency, uint32_t other_latency,
                       bool changed, const char* step = "LATENCY-CHANGE") {
    const unsigned other = sink ^ 1;
    const uint8_t code = sink == 1 ? 11 : 0;
    const uint64_t bridge = sink == 1 ? BRIDGE1 : 0;
    attribute(sink, code, bridge, sink == 1, EV_JOININ, latency);
    const auto uns = input_uns(sink, 100);
    if (changed) {
      CHECK(uns.size() == 1,
            "GI %s: exactly one unsolicited response for sink %u, got %zu",
            step, sink, uns.size());
      const auto pushed = uns.empty() ? std::vector<uint8_t>{} : uns[0];
      check_frame(pushed, step, sink, 3, 0, code, bridge,
                  true, true, false, latency);
    } else {
      CHECK(uns.empty(), "GI LATENCY-SAME: unchanged refresh sends no response for sink %u (%s)",
            sink, step);
    }
    check_frame(query(sink), "LATENCY-READ", sink, 3, 0, code, bridge,
                false, true, false, latency);
    CHECK(input_uns(other, 100).empty(),
          "GI LATENCY-OTHER: sink %u refresh never notifies sink %u", sink, other);
    check_frame(query(other), "LATENCY-OTHER", other, 3, 0,
                other == 1 ? 11 : 0, other == 1 ? BRIDGE1 : 0,
                false, true, false, other_latency);
    CHECK(io.q_aecp.empty(), "GI LATENCY: no duplicate or unrelated response");
  }

  // Start and finish at the existing zero/all-ones latches. Returning to
  // the baseline between walking values makes EACH transition differ in
  // only the named bit, in both directions. Repeat every committed value.
  void latency_walks() {
    const int before_checks = h.checks;
    const int before_fails = h.fails;
    for (unsigned walk = 0; walk < 2; ++walk) {
      const unsigned sink = walk;
      const uint32_t baseline = walk == 0 ? 0u : UINT32_MAX;
      const uint32_t other_latency = ~baseline;
      for (unsigned bit = 0; bit < 32; ++bit) {
        // Keep both real peers discovered and registered through the long
        // sweep. This unchanged refresh is graded before the isolated step,
        // so a registration event cannot substitute for its latency event.
        discover(0);
        discover(1);
        latency_refresh(sink ^ 1, other_latency, baseline, false,
                        "LATENCY-WALK-PEER-REFRESH");
        const uint32_t value = baseline ^ (uint32_t{1} << bit);
        for (unsigned returning = 0; returning < 2; ++returning) {
          const uint32_t latency = returning ? baseline : value;
          char step[80];
          snprintf(step, sizeof(step), "LATENCY-WALK-%s bit %u %s",
                   walk == 0 ? "ONE" : "ZERO", bit,
                   returning ? "return" : "step");
          latency_refresh(sink, latency, other_latency, true, step);
          latency_refresh(sink, latency, other_latency, false, step);
        }
      }
    }
    printf("GI latency walks added: %d checks, %d failures\n",
           h.checks - before_checks, h.fails - before_fails);
  }

  void latency_changes() {
    io.gsi_fold_latency = true;
    check_frame(query(0), "LATENCY-BEFORE", 0, 3, 0, 0, 0,
                false, true, false, 0x12345);
    check_frame(query(1), "LATENCY-BEFORE", 1, 3, 0, 11, BRIDGE1,
                false, true, false, 0x12345);
    latency_refresh(0, 0x81234567, 0x12345, true);
    latency_refresh(0, 0x81234567, 0x12345, false);
    latency_refresh(1, 0xABCDEF01, 0x81234567, true);
    latency_refresh(1, 0xABCDEF01, 0x81234567, false);
    latency_refresh(0, 0, 0xABCDEF01, true);
    latency_refresh(1, 0xFFFFFFFF, 0, true);
    latency_walks();
    io.gsi_fold_latency = false;
  }

  static std::vector<uint8_t> image(uint16_t inputs) {
    static const char* const kNames[] = {
        "Entity", "Input 0", "Input 1", "Input 2", "Input 3", "Input 4",
        "Input 5", "Input 6", "Input 7", "Input 8", "Input 9"};
    std::vector<ImgEnt> entries{
        {CFGIX, 0x0000, 1, 312, 0, 312, 0},
        {CFGIX, 0x0005, inputs, 140, 1, 144, 0}};
    std::vector<std::vector<uint8_t>> bodies{entity_descriptor()};
    for (uint16_t i = 0; i < inputs; ++i)
      bodies.push_back(stream_descriptor(0x0005, i));
    return build_image(entries, bodies,
                       std::vector<const char*>(kNames, kNames + 1 + inputs),
                       1);
  }

  void vlan_retention() {
    boot();
    binding(0, true);
    const auto sent = probe(0);
    if (sent.size() != 70) return;
    // Milan 5.3.8.9, Table 5.38 and 5.4.2.10. Use upper VLAN bits
    // deliberately, and derive response identity from the actual probe.
    io.feed(acmp_frame(T1_MAC, 1, 0, sid(0), fv_u64(sent, 26, 8),
                      talker(0), EID, T1_UID, 0, da(0), 0,
                      fv_u64(sent, 62, 2), 0, 0xF123));
    io.run_ms(10);
    const auto info = query(0);
    CHECK(info.size() == 94 && fv_u64(info, 82, 2) == 0xF123,
          "VLAN168 GET_STREAM_INFO retains all received bits");
    CHECK((io.d->acmp_bound_vlan_o[0] & 0xFFFu) == 0x123,
          "VLAN168 parent VID remains the low 12 bits");
    io.q_acmp.clear();
    io.feed(acmp_frame(CTLR_MAC, 10, 0, 0, CTLR_EID, 0, EID, 0, 0,
                      0, 0, 0x1680, 0, 0));
    const auto state = io.wait_frame(io.q_acmp, 400,
        [](const std::vector<uint8_t>& f) {
          return f.size() == 70 && (f[15] & 15) == 11;
        });
    CHECK(state.size() == 70 && fv_u64(state, 66, 2) == 0xF123,
          "VLAN168 GET_RX_STATE retains all received bits");
    binding(0, false);
    const auto cleared = query(0);
    CHECK(cleared.size() == 94 && fv_u64(cleared, 82, 2) == 0,
          "VLAN168 unbind clears the full stored VLAN");
  }

  // ---- Table 5.22 started/stopped by START/STOP_STREAMING (issue #192) ----
  // Milan 5.4.2.19 and 5.4.2.20: START/STOP_STREAMING change exactly the
  // started state of a bound input, which Table 5.22 lists among the
  // GET_STREAM_INFO causes. That notification answers no command, so every
  // registered controller receives it, the requester included (5.4.5.1);
  // the command's own unsolicited response reaches the others alone
  // (5.4.5.2). A (CTLR) is the requester and B a second registered
  // controller. Each controller's sequence_id is modelled from the wire: the
  // count of unsolicited frames it was sent before.
  static constexpr uint64_t B_MAC = 0x0202C2C2C2C2ull;
  uint16_t b_unsolicited = 0;

  struct Uns {
    std::vector<uint8_t> f;
    uint16_t seq;
  };

  // every AECP frame transmitted within `ms`, in order; none left queued
  std::vector<std::vector<uint8_t>> drain_aecp(int ms) {
    std::vector<std::vector<uint8_t>> got;
    for (int t = 0; t <= ms; ++t) {
      while (!io.q_aecp.empty()) {
        got.push_back(io.q_aecp.front());
        io.q_aecp.pop_front();
      }
      if (t < ms) io.run_ms(1);
    }
    return got;
  }

  // the unsolicited frames to `mac` in arrival order, each with the
  // sequence_id it must carry: that controller's next one (5.4.5.1)
  static std::vector<Uns> unsolicited_to(
      const std::vector<std::vector<uint8_t>>& got, uint64_t mac,
      uint16_t& next) {
    std::vector<Uns> v;
    for (const auto& f : got)
      if (f.size() >= 42 && (f[36] & 0x80) && fv_u64(f, 0, 6) == mac)
        v.push_back(Uns{f, next++});
    return v;
  }

  static std::vector<Uns> stream_info_of(const std::vector<Uns>& v,
                                         unsigned sink) {
    std::vector<Uns> s;
    for (const auto& u : v)
      if ((fv_u64(u.f, 36, 2) & 0x7FFF) == 0x000F
          && fv_u64(u.f, 38, 2) == 0x0005 && fv_u64(u.f, 40, 2) == sink)
        s.push_back(u);
    return s;
  }

  static size_t count_type(const std::vector<Uns>& v, uint16_t op) {
    size_t n = 0;
    for (const auto& u : v)
      if ((fv_u64(u.f, 36, 2) & 0x7FFF) == op) ++n;
    return n;
  }

  // the unsolicited GET_STREAM_INFO a settled (COMPLETED) sink owes `mac`
  static std::vector<uint8_t> settled_info(uint64_t mac, uint64_t eid,
                                           uint16_t seq, unsigned sink,
                                           bool stopped, uint8_t code,
                                           uint64_t bridge) {
    auto body = StreamInfoPhase::gsi_body(0x0005, sink, true, 3 << 5);
    putbe(&body[44], 2, 2);
    body[34] = code;
    putbe(&body[36], bridge, 8);
    if (stopped) body[7] |= 0x08;                 // STREAMING_WAIT
    auto want = aecp_frame(mac, OWN_MAC, 1, 0, EID, eid, seq, 0x000F, body);
    want[36] |= 0x80;
    return want;
  }

  void register_b() {
    const auto seq = sequence++;
    io.feed(aecp_frame(OWN_MAC, B_MAC, 0, 0, EID, CTLR2_EID, seq,
                       0x0024, std::vector<uint8_t>(4, 0)));
    auto f = io.wait_any(io.q_aecp, 500);
    CHECK(f.size() >= 38 && fv_u64(f, 0, 6) == B_MAC
          && fv_u64(f, 34, 2) == seq && ((f[16] >> 3) & 31) == 0,
          "GI SN register: a second controller B registered");
    b_unsolicited = 0;
  }

  // START_STREAMING (0x0022) or STOP_STREAMING (0x0023) from A
  void streaming(uint16_t op, unsigned sink) {
    std::vector<uint8_t> body(4, 0);
    putbe(&body[0], 0x0005, 2);
    putbe(&body[2], sink, 2);
    io.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, sequence++,
                       op, body));
  }

  // A's solicited answers to `n` commands from sequence_id `first` on
  size_t answered(const std::vector<std::vector<uint8_t>>& got,
                  uint16_t first, unsigned n) const {
    size_t ok = 0;
    for (const auto& f : got)
      if (f.size() >= 38 && !(f[36] & 0x80) && fv_u64(f, 0, 6) == CTLR_MAC
          && static_cast<uint16_t>(fv_u64(f, 34, 2) - first) < n
          && ((f[16] >> 3) & 31) == 0)
        ++ok;
    return ok;
  }

  // exactly one GET_STREAM_INFO for `sink` among `v`, byte-exact at its
  // controller's own sequence_id, carrying the final state
  void one_info(const char* step, const char* who, const std::vector<Uns>& v,
                uint64_t mac, uint64_t eid, unsigned sink, bool stopped,
                uint8_t code = 0, uint64_t bridge = 0) {
    const auto s = stream_info_of(v, sink);
    CHECK(s.size() == 1, "GI %s: exactly one GET_STREAM_INFO for sink %u at "
          "%s, got %zu", step, sink, who, s.size());
    if (s.empty()) return;
    const auto want = settled_info(mac, eid, s[0].seq, sink, stopped, code,
                                   bridge);
    CHECK(s[0].f == want, "GI %s: %s's sink %u notification is byte-exact: "
          "u = 1, its DA, entity_id and sequence_id %u, STREAMING_WAIT %s",
          step, who, sink, s[0].seq, stopped ? "set" : "clear");
    if (s[0].f != want) { dump("GI got", s[0].f); dump("GI expected", want); }
  }

  // the solicited read after a step agrees with what was pushed
  void read_back(const char* step, unsigned sink, bool stopped,
                 uint8_t code = 0, uint64_t bridge = 0) {
    const auto f = query(sink);
    check_frame(f, step, sink, 3, 0, code, bridge, false, true, stopped);
  }

  // one START/STOP_STREAMING from A on sink 0, nothing else in flight
  void started_step(const char* step, uint16_t op, bool stopped) {
    const uint16_t first = sequence;
    const bool was = (io.d->aecp_strm_started_o & 1) != 0;
    streaming(op, 0);
    const auto got = drain_aecp(150);
    const auto at_a = unsolicited_to(got, CTLR_MAC, unsolicited_sequence);
    const auto at_b = unsolicited_to(got, B_MAC, b_unsolicited);
    CHECK(answered(got, first, 1) == 1
          && was == stopped && ((io.d->aecp_strm_started_o & 1) != 0) != stopped,
          "GI %s: A's command answered SUCCESS and moved sink 0's started "
          "state (premise)", step);
    one_info(step, "A, the requester", at_a, CTLR_MAC, CTLR_EID, 0, stopped);
    one_info(step, "B", at_b, B_MAC, CTLR2_EID, 0, stopped);
    CHECK(count_type(at_a, op) == 0 && count_type(at_b, op) == 1,
          "GI %s: the command's own unsolicited response reaches B once and "
          "A never (Milan 5.4.5.2)", step);
    CHECK(got.size() == 4, "GI %s: nothing else is sent, got %zu frames",
          step, got.size());
    read_back(step, 0, stopped);
  }

  // STOP_STREAMING on sink 0 coinciding with sink 0's first Talker
  // registration, another GET_STREAM_INFO cause. The engine takes no command
  // while a notification is pending, so the STOP commits with the notify
  // block idle and its round would be claimed at once. The integrator's
  // GET_AVB_INFO change, raised on the clock the engine takes the STOP,
  // holds the block with a round of its own until the STOP has answered, so
  // the STOP's change and the registration both pend before sink 0's round
  // is claimed: one notification each, carrying both changes.
  void started_with_registration() {
    const char* step = "STOP-WITH-REGISTRATION";
    const uint16_t first = sequence;
    const auto taken = io.d->dbg_aecp_cmd_o;
    streaming(0x0023, 0);
    for (long b = 50L * MS_CYC; b != 0 && io.d->dbg_aecp_cmd_o == taken; --b)
      io.step();
    const bool before = io.d->dbg_aecp_cmd_o != taken
                        && (io.d->aecp_strm_started_o & 1) != 0;
    io.d->gsi_avb_chg_i = 1;
    io.step();
    io.d->gsi_avb_chg_i = 0;
    attribute(0, 7, BRIDGE0, true);
    const auto got = drain_aecp(250);
    const auto at_a = unsolicited_to(got, CTLR_MAC, unsolicited_sequence);
    const auto at_b = unsolicited_to(got, B_MAC, b_unsolicited);
    CHECK(before && answered(got, first, 1) == 1
          && (io.d->aecp_strm_started_o & 1) == 0,
          "GI %s: the GET_AVB_INFO change came after the engine took the STOP "
          "and before it stopped sink 0, which it then did (premise)", step);
    CHECK(count_type(at_a, 0x0027) == 1 && count_type(at_b, 0x0027) == 1,
          "GI %s: the GET_AVB_INFO change reached A and B once each "
          "(premise)", step);
    one_info(step, "A, the requester", at_a, CTLR_MAC, CTLR_EID, 0, true, 7,
             BRIDGE0);
    one_info(step, "B", at_b, B_MAC, CTLR2_EID, 0, true, 7, BRIDGE0);
    CHECK(got.size() == 6, "GI %s: nothing else is sent, got %zu frames",
          step, got.size());
    read_back(step, 0, true, 7, BRIDGE0);
  }

  void started_notify() {
    io.dram = image(2);
    boot();
    register_controller();
    discover(0);
    binding(0, true);
    pair("SN-BIND", 0, 2);
    settle(0, probe(0));
    register_b();
    started_step("STOP-BY-A", 0x0023, true);
    started_step("START-BY-A", 0x0022, false);
    started_with_registration();
    io.run_ms(100);
    CHECK(io.q_aecp.empty(), "GI SN final: no late or duplicate notification");
  }

  void run() {
    io.dram = image(2);
    io.gsi_retired_stuck = true;
    io.gsi_fold_sw = true;
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
    // Milan 5.5.3.5.30 step 2 and .10 preserve the timeout status.
    // With no status change, Table 5.22 supplies no notification to wait for.
    const auto next_probe = probe(0, 5000);
    check_frame(query(0), "RETRY-RETAIN", 0, 2, 7, 0, 0, false);
    settle(0, next_probe);
    discover(1);
    pair("ACTIVE", 1, 2);
    settle(1, probe(1));

    io.q_msrp.clear();
    attribute(0, 7, BRIDGE0, true);
    pair("FAILED-0", 0, 3, 0, 7, BRIDGE0);
    attribute(1, 11, BRIDGE1, true);
    pair("FAILED-1", 1, 3, 0, 11, BRIDGE1);
    check_frame(query(0), "DISTINCT-0", 0, 3, 0, 7, BRIDGE0, false);
    // wire control: the fresh Failed registration DOES declare New, so the
    // silence checked after FAILED-REFRESH is not a blind detector
    CHECK(listener_news(0, 1000) >= 1,
          "GI FAILED-0 wire: a fresh Failed registration declares Listener New");
    attribute(0, 9, BRIDGE0 ^ 0xFFFF000000000000ull, true);
    pair("FAILED-REFRESH", 0, 3, 0, 9, BRIDGE0 ^ 0xFFFF000000000000ull);
    CHECK(listener_news(0, 600) == 0,
          "GI FAILED-REFRESH wire: a changed FailureInformation sends no "
          "Listener New");
    attribute(0, 9, BRIDGE0 ^ 0xFFFF000000000000ull, true);
    io.run_ms(100);
    CHECK(io.q_aecp.empty(), "GI unchanged Failed: no fabricated failure transition");
    attribute(0, 0, 0, false);
    pair("ADVERTISE", 0, 3);
    latency_changes();
    attribute(0, 7, BRIDGE0, true);
    pair("FAILED-AGAIN", 0, 3, 0, 7, BRIDGE0);
    attribute(0, 7, BRIDGE0, true, EV_LV);
    pair("WITHDRAWAL", 0, 2);
    pair("WITHDRAWAL-PROBING", 0, 2);
    check_frame(query(1), "DISTINCT-1", 1, 3, 0, 11, BRIDGE1, false);
    binding(0, false);
    pair("DISABLED", 0, 0);
    // Reset with sink 1 still COMPLETED and registering a nonzero Failed.
    // The image now carries STREAM_INPUT 0..9, past the eight hardware sinks.
    io.dram = image(10);
    boot();
    for (unsigned s = 0; s < 2; ++s)
      check_frame(query(s), "RESET-LIVE", s, 0, 0, 0, 0, false);
    register_controller();
    rebind_from_probing(0);
    bind_without_talker(1);
    index_guard(1, 1);
    binding(1, false);
    pair("DISABLED", 1, 0);
    io.run_ms(100);
    CHECK(io.q_aecp.empty(), "GI final: no duplicate notifications");
    CHECK(io.gsi_internal_leaks == 0,
          "GI internal seam: selectors 5/7 never requested from integrator");
    vlan_retention();
    started_notify();
  }
};
