// SPDX-License-Identifier: CERN-OHL-W-2.0
// Section D3: the processor's D3 saved-state writer (KL_aecp_nvm_writer,
// inside KL_aecp_engine; parent D3 contract sections 3, 6.2 and 8.1),
// through the real top. Every case runs on a FRESH model, so the main DUT's
// tuned timeline is untouched, and reaches the writer only through the
// top's own faces: AECP commands on the MAC stream, restore_go_i, the NVM
// device face and the descriptor memory. The wrap's dbg_d3_* taps are read
// to grade, never to drive.

//! READ_DESCRIPTOR(ENTITY 0) of configuration 0: the command every case
//! holds across the boot, and the byte-exact answer the image gives
static std::vector<uint8_t> d3_read_entity_cmd(uint16_t seq) {
  std::vector<uint8_t> rd(8, 0);
  putbe(&rd[0], CFGIX, 2);
  return aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, seq,
                    AEM_READ_DESCRIPTOR, rd);
}

static std::vector<uint8_t> d3_read_entity_rsp(uint16_t seq) {
  std::vector<uint8_t> epl(4, 0);
  putbe(&epl[0], CFGIX, 2);
  const auto ent = entity_descriptor();
  epl.insert(epl.end(), ent.begin(), ent.end());
  return aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID, seq,
                    AEM_READ_DESCRIPTOR, epl);
}

// ==== D3O. ownership: the dispatch hold from reset to the D3 terminal =====
struct D3OwnershipPhase {
  H& h;
  const milan::tb::Model<Vpp_top_wrap> model;
  H x;
  const std::vector<uint8_t>& image;

  //! what one boot observed, cycle by cycle, from restore_go_i on
  struct Boot {
    long release = -1;        //! first cycle the admission gate had released
    long done = -1;           //! first cycle the writer read done
    long own_fall = -1;       //! first cycle the writer read not owning
    long closed = -1;         //! first cycle the writer read closed
    long busy = -1;           //! first cycle the engine had a command in flight
    long busy_owned = 0;      //! cycles the engine was busy while owned
    long dram_reqs = 0;       //! descriptor requests, release to terminal
  };

  D3OwnershipPhase(H& tally, const std::vector<uint8_t>& img)
      : h(tally), x(model.get()), image(img) {}

  void power_up(const std::vector<uint8_t>& dram, bool silent) {
    x.dram = dram;
    x.dram_silent = silent;
    x.erase_nvm();
    x.reset();
    x.q_aecp.clear();
  }

  //! pulse restore_go_i and follow the boot for `cycles`
  Boot boot_for(long cycles) {
    Boot b;
    uint64_t reqs_at_release = 0;
    x.d->restore_go_i = 1;
    for (long c = 0; c < cycles; ++c) {
      if (c == 5) x.d->restore_go_i = 0;
      x.step();
      if (b.release < 0 && x.d->dbg_lsn_released_o) {
        b.release = c;
        reqs_at_release = x.dram_reqs;
      }
      if (b.done < 0 && x.d->dbg_d3_done_o) {
        b.done = c;
        b.dram_reqs = static_cast<long>(x.dram_reqs - reqs_at_release);
      }
      if (b.own_fall < 0 && !x.d->dbg_d3_own_o) b.own_fall = c;
      if (b.closed < 0 && x.d->dbg_d3_closed_o) {
        b.closed = c;
        b.dram_reqs = static_cast<long>(x.dram_reqs - reqs_at_release);
      }
      if (b.busy < 0 && x.d->dbg_aecp_busy_o) b.busy = c;
      if (x.d->dbg_d3_own_o && x.d->dbg_aecp_busy_o) ++b.busy_owned;
    }
    x.d->restore_go_i = 0;
    return b;
  }

  // D3O1: a command fed before the walk is held in its queue, nothing runs
  // while the writer owns the bus, the writer starts only at the binding
  // walk's end, and the held command is served after the terminal.
  void o1_dispatch_is_held_from_reset() {
    power_up(image, false);
    x.feed(d3_read_entity_cmd(0xD301));
    long owned = 0;
    long busy = 0;
    for (int c = 0; c < 2000; ++c) {
      x.step();
      owned += x.d->dbg_d3_own_o ? 1 : 0;
      busy += x.d->dbg_aecp_busy_o ? 1 : 0;
    }
    CHECK(owned == 2000 && busy == 0 && x.q_aecp.empty()
              && x.d->dbg_aecp_head_o && !x.d->dbg_d3_done_o,
          "D3O1: without the walk the writer owns every cycle (%ld of 2000), "
          "the engine runs nothing (%ld busy) and the command waits in its "
          "queue", owned, busy);
    const Boot b = boot_for(3000);
    CHECK(b.release >= 0 && b.done > b.release && b.own_fall == b.done
              && b.busy > b.own_fall && b.busy_owned == 0,
          "D3O1: released at %ld, terminal at %ld, own fell at %ld, the held "
          "command taken at %ld, %ld busy cycles while owned",
          b.release, b.done, b.own_fall, b.busy, b.busy_owned);
    CHECK(!x.d->dbg_d3_fail_o && !x.d->dbg_d3_closed_o && x.d->dbg_d3_cause_o == 0
              && b.dram_reqs == 0,
          "D3O1: a validated image is proven without a LOCATE (%ld requests "
          "from the release to the terminal), COMPLETE", b.dram_reqs);
    const auto got = x.wait_any(x.q_aecp, 50);
    CHECK(got == d3_read_entity_rsp(0xD301),
          "D3O1: the command held across the boot is answered byte-exact");
    if (got != d3_read_entity_rsp(0xD301)) dump("got", got);
  }

  // D3O2: an image the store cannot validate ends CLOSED, cause 7: the
  // writer keeps the bus for ever, no command is served, and the listener's
  // release at the binding walk's end is not taken back.
  void o2_an_unprovable_image_ends_closed() {
    std::vector<uint8_t> bad = image;
    bad[0] ^= 0xFF;                               // the "AEMI" magic
    power_up(bad, false);
    const Boot b = boot_for(12000);
    CHECK(b.closed > b.release && b.done < 0 && x.d->dbg_d3_fail_o
              && x.d->dbg_d3_cause_o == 7 && x.d->dbg_d3_own_o
              && b.dram_reqs > 0,
          "D3O2: CLOSED at %ld after the release at %ld, cause %u, own %u, "
          "%ld descriptor requests", b.closed, b.release,
          unsigned(x.d->dbg_d3_cause_o), unsigned(x.d->dbg_d3_own_o),
          b.dram_reqs);
    x.feed(d3_read_entity_cmd(0xD302));
    long busy = 0;
    for (int c = 0; c < 30000; ++c) {
      x.step();
      busy += x.d->dbg_aecp_busy_o ? 1 : 0;
    }
    CHECK(busy == 0 && x.q_aecp.empty() && x.d->dbg_aecp_head_o
              && x.d->dbg_d3_own_o && x.d->dbg_lsn_released_o,
          "D3O2: CLOSED holds AECP (%ld busy cycles, the command still queued) "
          "and leaves the listener released", busy);
  }

  // D3O3: a descriptor memory that accepts and never answers: the store's
  // own watchdog answers the LOCATE with an error, so the walk ends CLOSED,
  // cause 7, inside the restore deadline, never hangs.
  void o3_a_silent_descriptor_memory_ends_closed() {
    power_up(image, true);
    const Boot b = boot_for(12000);
    CHECK(b.closed > b.release && b.closed - b.release < 2 * 4096 + 64
              && x.d->dbg_d3_cause_o == 7 && b.done < 0,
          "D3O3: CLOSED %ld cycles after the release, cause %u",
          b.closed - b.release, unsigned(x.d->dbg_d3_cause_o));
    x.dram_silent = false;
  }

  // D3O4: an image loaded after the store's boot walk (the silicon order
  // before the firmware change of D3 section 5.3) is proven by the writer's
  // LOCATE, which makes the store walk it again, and the held command runs.
  void o4_a_late_image_is_proven_by_its_locate() {
    power_up(std::vector<uint8_t>{}, false);
    x.idle(2000);
    const bool invalid_before = !x.d->dbg_img_valid_o;
    x.dram = image;
    x.feed(d3_read_entity_cmd(0xD304));
    const Boot b = boot_for(6000);
    CHECK(invalid_before && b.done > b.release && b.dram_reqs > 0
              && !x.d->dbg_d3_fail_o && x.d->dbg_img_valid_o,
          "D3O4: an image loaded late is walked at the writer's LOCATE (%ld "
          "requests from the release to the terminal), COMPLETE at %ld",
          b.dram_reqs, b.done);
    const auto got = x.wait_any(x.q_aecp, 50);
    CHECK(got == d3_read_entity_rsp(0xD304),
          "D3O4: the held command is answered from the proven image");
  }

  // ---- the AECP hold admission (processor issue #131 ruling) -------------
  //! the top's RX_SLOTS_P: the wrap keeps the F01.5 default, and the
  //! snapshot's shape word (word 1, [15:8]) is graded to say so
  static constexpr uint32_t RX_SLOTS = 4;
  //! GET_RX_STATE of sink 0 from the controller: live ACMP listener work
  static std::vector<uint8_t> get_rx_state(uint16_t seq) {
    return acmp_frame(CTLR_MAC, 10, 0, 0, CTLR_EID, 0, EID, 0, 0, 0, 0, seq, 0, 0);
  }
  //! cycles from the end of its feed to its GET_RX_STATE_RESPONSE, -1 if
  //! none within `ms`
  long rx_state_latency(uint16_t seq, int ms) {
    x.q_acmp.clear();
    x.feed(get_rx_state(seq));
    const long t0 = long(x.t);
    const auto g = x.wait_frame(x.q_acmp, ms, [seq](const std::vector<uint8_t>& f) {
      return f.size() > 63 && (f[15] & 0x0F) == 11 && fv_u64(f, 62, 2) == seq;
    });
    return g.empty() ? -1 : long(x.t) - t0;
  }
  //! AECP responses on the wire carrying sequence id `seq`
  int answered(uint16_t seq) const {
    int n = 0;
    for (const auto& r : x.q_aecp) n += (r.size() >= 38 && fv_u64(r, 34, 2) == seq) ? 1 : 0;
    return n;
  }

  // D3O5: CLOSED holds AECP for ever, and still at most ONE AECP record
  // occupies the shared ingress. Of RX_SLOTS_P + 2 AECP commands the first
  // is held and every further one is dropped at the slot gate and counted
  // (snapshot word 37, the side port's own read). A GET_RX_STATE after each
  // is answered in exactly the cycles it takes with no AECP traffic, none
  // of the AECP commands is answered, and after 2,000 ms the listener
  // still answers in that time.
  void o5_closed_admits_one_aecp_record() {
    std::vector<uint8_t> bad = image;
    bad[0] ^= 0xFF;
    power_up(bad, false);
    const Boot b = boot_for(12000);
    const uint32_t shape = x.snap(1);
    const long idle_lat = rx_state_latency(0xD350, 50);
    long worst = 0;
    bool same = true;
    for (uint32_t k = 0; k < RX_SLOTS + 2; ++k) {
      x.feed(d3_read_entity_cmd(uint16_t(0xD360 + k)));
      const long lat = rx_state_latency(uint16_t(0xD351 + k), 50);
      same = same && lat == idle_lat;
      worst = std::max(worst, lat);
    }
    CHECK(b.closed > b.release && ((shape >> 8) & 0xFF) == RX_SLOTS && idle_lat > 0
              && same,
          "D3O5: in CLOSED each GET_RX_STATE after each of %u AECP commands is "
          "answered in %ld cycles, the idle latency (worst %ld)",
          RX_SLOTS + 2, idle_lat, worst);
    int replies = 0;
    for (uint32_t k = 0; k < RX_SLOTS + 2; ++k) replies += answered(uint16_t(0xD360 + k));
    const uint32_t dropped = x.snap(37);
    CHECK(dropped == RX_SLOTS + 1 && replies == 0 && x.d->dbg_aecp_head_o
              && x.d->dbg_d3_own_o,
          "D3O5: one AECP command held, %u dropped at the slot gate and counted, "
          "%d answered", dropped, replies);
    x.run_ms(2000);
    const long late = rx_state_latency(0xD35F, 50);
    CHECK(late == idle_lat && x.snap(37) == RX_SLOTS + 1,
          "D3O5: after 2,000 ms in CLOSED the listener answers in %ld cycles", late);
  }

  // D3O6: the same during a restore slowed inside its per-wait deadline:
  // the D3 walk's next grant waits 15,000 cycles, and six AECP commands
  // arrive after the listener's release. A GET_RX_STATE is answered in the
  // idle latency long before the D3 terminal; at the terminal the one held
  // command is answered byte-exact and the five dropped ones never are; a
  // command after the terminal is served and nothing more is dropped.
  void o6_a_slowed_restore_admits_one_aecp_record() {
    power_up(image, false);
    x.d->restore_go_i = 1;
    for (long c = 0; c < 20000 && !x.d->dbg_lsn_released_o; ++c) {
      if (c == 5) x.d->restore_go_i = 0;
      x.step();
    }
    x.d->restore_go_i = 0;
    x.nv_gnt_hold = 15000;                        // the D3 walk's next grant
    const long idle_lat = rx_state_latency(0xD380, 50);
    for (int k = 0; k < 6; ++k) x.feed(d3_read_entity_cmd(uint16_t(0xD370 + k)));
    const long lat = rx_state_latency(0xD381, 50);
    const bool walking = !x.d->dbg_d3_done_o;
    for (long c = 0; c < 40000 && !x.d->dbg_d3_done_o; ++c) x.step();
    x.idle(3000);
    int dropped_replies = 0;
    for (int k = 1; k < 6; ++k) dropped_replies += answered(uint16_t(0xD370 + k));
    std::vector<uint8_t> held;
    for (const auto& r : x.q_aecp)
      if (r.size() >= 38 && fv_u64(r, 34, 2) == 0xD370) held = r;
    const uint32_t dropped = x.snap(37);
    CHECK(idle_lat > 0 && lat == idle_lat && walking,
          "D3O6: during the slowed walk a GET_RX_STATE behind six AECP "
          "commands is answered in %ld cycles, the idle latency %ld", lat, idle_lat);
    CHECK(x.d->dbg_d3_done_o && held == d3_read_entity_rsp(0xD370)
              && dropped_replies == 0 && dropped == 5,
          "D3O6: at the terminal the held command is answered byte-exact, %u "
          "dropped and counted, %d of them answered", dropped, dropped_replies);
    x.q_aecp.clear();
    x.feed(d3_read_entity_cmd(0xD37F));
    const auto after = x.wait_any(x.q_aecp, 50);
    CHECK(after == d3_read_entity_rsp(0xD37F) && x.snap(37) == 5,
          "D3O6: after the terminal an AECP command is served, no drop counted");
  }

  void run() {
    o1_dispatch_is_held_from_reset();
    o2_an_unprovable_image_ends_closed();
    o3_a_silent_descriptor_memory_ends_closed();
    o4_a_late_image_is_proven_by_its_locate();
    o5_closed_admits_one_aecp_record();
    o6_a_slowed_restore_admits_one_aecp_record();
  }
};

// ---- the D3 record oracle, from the F07.8 framing and the saved-state
// allocation (07 section 5.2; parent FASTCONNECT section 4.2), never the RTL
static uint16_t d3_crc16(const std::vector<uint8_t>& frame) {
  uint16_t c = 0xFFFF;                    // CCITT-FALSE: init FFFF, poly 1021
  for (size_t i = 0; i < frame.size(); i++) {
    if (i == 6 || i == 7) continue;       // over the header without its crc
    c = static_cast<uint16_t>(c ^ (frame[i] << 8));
    for (int b = 0; b < 8; b++) {
      c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021)
                       : static_cast<uint16_t>(c << 1);
    }
  }
  return c;
}

//! the whole framed record: {0x1722, layout 2, id, length, crc16} and the
//! value's low `plen` bytes, big-endian
static std::vector<uint8_t> d3_record(uint8_t rid, uint64_t value, int plen) {
  std::vector<uint8_t> r = {0x17, 0x22, 0x02, rid, 0x00,
                            static_cast<uint8_t>(plen), 0x00, 0x00};
  for (int j = plen - 1; j >= 0; --j)
    r.push_back(static_cast<uint8_t>(value >> (8 * j)));
  const uint16_t c = d3_crc16(r);
  r[6] = static_cast<uint8_t>(c >> 8);
  r[7] = static_cast<uint8_t>(c);
  return r;
}

//! one persisted row the service phase sets, and what its record must hold
struct D3Row {
  const char* group;
  uint8_t rid;
  int plen;
  uint64_t value;
};

// ==== D3S. the writer in service: triggers, latch and the clear rule ======
struct D3ServicePhase {
  H& h;
  const milan::tb::Model<Vpp_top_wrap> model;
  H x;
  const std::vector<uint8_t>& image;
  uint16_t seq = 0xD500;
  //! T-NVM-DEBOUNCE at the wrap's 1 ms = 100 clk
  static constexpr long WINDOW = 500L * MS_CYC;
  static constexpr uint32_t FLAG_ACC_LAT_VALID = 0x20000000u;

  D3ServicePhase(H& tally, const std::vector<uint8_t>& img)
      : h(tally), x(model.get()), image(img) {}

  void power_up() {
    x.dram = image;
    x.dram_silent = false;
    x.nv_done_at = -1;
    x.nv_err_region = -1;
    x.nv_gnt_hold = 0;
    x.erase_nvm();
    x.reset();
    CHECK(x.boot_to_aecp(), "D3S: both walks over an erased device release AECP");
    x.d->link_up_i = 1;
    x.q_aecp.clear();
  }

  // ---- the commands (IEEE 1722.1-2021 section 7.4 bodies) ----------------
  static std::vector<uint8_t> pl_cfg(uint16_t ix) {
    std::vector<uint8_t> p(4, 0);
    putbe(&p[2], ix, 2);
    return p;
  }
  static std::vector<uint8_t> pl_rate(uint32_t rate) {
    std::vector<uint8_t> p(8, 0);
    putbe(&p[0], 0x0002, 2);
    putbe(&p[4], rate, 4);
    return p;
  }
  static std::vector<uint8_t> pl_clk(uint16_t source) {
    std::vector<uint8_t> p(8, 0);
    putbe(&p[0], 0x0024, 2);
    putbe(&p[4], source, 2);
    return p;
  }
  static std::vector<uint8_t> pl_fmt(uint16_t ty, uint16_t ix, uint64_t fmt) {
    std::vector<uint8_t> p(12, 0);
    putbe(&p[0], ty, 2);
    putbe(&p[2], ix, 2);
    putbe(&p[4], fmt, 8);
    return p;
  }
  static std::vector<uint8_t> pl_ptof(uint16_t ix, uint32_t latency) {
    std::vector<uint8_t> p(84, 0);
    putbe(&p[0], 0x0006, 2);
    putbe(&p[2], ix, 2);
    putbe(&p[4], FLAG_ACC_LAT_VALID, 4);
    putbe(&p[24], latency, 4);
    return p;
  }
  static std::vector<uint8_t> pl_identify(uint8_t value) {
    std::vector<uint8_t> p(5, 0);
    putbe(&p[0], 0x001A, 2);
    p[4] = value;
    return p;
  }

  std::vector<uint8_t> frame(uint16_t op, const std::vector<uint8_t>& pl,
                             uint16_t s) const {
    return aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, s, op, pl);
  }
  //! one command on the MAC stream and its SUCCESS response
  bool set_ok(uint16_t op, const std::vector<uint8_t>& pl) {
    const uint16_t s = seq++;
    x.q_aecp.clear();
    x.feed(frame(op, pl, s));
    const auto f = x.wait_frame(x.q_aecp, 100, [s](const std::vector<uint8_t>& r) {
      return r.size() >= 38 && fv_u64(r, 34, 2) == s;
    });
    return f.size() > 16 && ((f[16] >> 3) & 0x1F) == AECP_SUCCESS;
  }

  // ---- the device ------------------------------------------------------------
  int writes(size_t from, uint8_t region, int op = 1) const {
    int n = 0;
    for (size_t i = from; i < x.nvm_ops.size(); i++)
      if (x.nvm_ops[i].op == op && x.nvm_ops[i].region == region) n++;
    return n;
  }
  //! the bytes of the last completed WRITE to `region` since `from`
  std::vector<uint8_t> last_write(size_t from, uint8_t region) const {
    std::vector<uint8_t> w;
    for (size_t i = from; i < x.nvm_ops.size(); i++)
      if (x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == region) w = x.nvm_ops[i].wr;
    return w;
  }
  std::vector<uint8_t> held(uint8_t region, int plen) const {
    return std::vector<uint8_t>(x.nv_mem[region].begin(),
                                x.nv_mem[region].begin() + 8 + plen);
  }
  //! run until no D3 record is unflushed and the port is quiet, or `limit`
  bool settle(long limit) {
    for (long c = 0; c < limit; ++c) {
      x.step();
      if (!x.d->d3_unflushed_o && x.nv_st == H::NvState::NV_IDLE
          && !x.d->nvm_dev_req_o && c > 16) return true;
    }
    return false;
  }
  //! the device holds a WRITE/ERASE request for `region` (its grant withheld)
  bool wait_request(uint8_t region, long limit) {
    for (long c = 0; c < limit; ++c) {
      if (x.d->nvm_dev_req_o && x.d->nvm_dev_region_o == region) return true;
      x.step();
    }
    return false;
  }

  //! every group at its first and last declared index, one SET each
  std::vector<D3Row> s1_rows() const {
    return {
      {"cfg",  0x00, 2, 0x0001},                  // SET_CONFIGURATION 1 of 2
      {"rate", 0x02, 4, 48000},                   // the image's list: 48k, 96k
      {"clks", 0x0A, 2, 2},                       // of three clock sources
      {"fmti", 0x30, 8, H::SFMT_ALT_C},
      {"fmti", 0x31, 8, H::SFMT_MAIN_C},
      {"fmto", 0x40, 8, H::SFMT_ALT_C},
      {"fmto", 0x41, 8, H::SFMT_MAIN_C},
      {"ptof", 0x50, 4, 1000000},
      {"ptof", 0x51, 4, 2500000},
    };
  }
  bool set_row(const D3Row& r) {
    const uint16_t ix = r.rid & 0x0F;
    switch (r.rid & 0xF0) {
      case 0x00:
        if (r.rid == 0x00) return set_ok(AEM_SET_CONFIGURATION, pl_cfg(uint16_t(r.value)));
        if (r.rid < 0x0A) return set_ok(AEM_SET_SAMPLING_RATE, pl_rate(uint32_t(r.value)));
        return set_ok(AEM_SET_CLOCK_SOURCE, pl_clk(uint16_t(r.value)));
      case 0x30: return set_ok(AEM_SET_STREAM_FORMAT, pl_fmt(0x0005, ix, r.value));
      case 0x40: return set_ok(AEM_SET_STREAM_FORMAT, pl_fmt(0x0006, ix, r.value));
      default:   return set_ok(AEM_SET_STREAM_INFO, pl_ptof(ix, uint32_t(r.value)));
    }
  }

  // D3S1: a real SET of every group, first and last declared index, becomes
  // exactly one ERASE + WRITE of its own record after the debounce, carrying
  // the byte-exact F07.8 frame of the value the command left; no other D3
  // record id is touched. Deleting one group's trigger fails its row.
  void s1_every_group_persists_its_record() {
    power_up();
    const size_t ops0 = x.nvm_ops.size();
    bool accepted = true;
    for (const auto& r : s1_rows()) accepted = set_row(r) && accepted;
    CHECK(accepted, "D3S1: every SET answered SUCCESS");
    CHECK(settle(3 * WINDOW), "D3S1: the burst drained, nothing unflushed");
    for (const auto& r : s1_rows()) {
      const auto want = d3_record(r.rid, r.value, r.plen);
      CHECK(writes(ops0, r.rid) == 1 && writes(ops0, r.rid, 2) == 1
                && last_write(ops0, r.rid) == want && held(r.rid, r.plen) == want,
            "D3S1 %s: record 0x%02x written once, byte-exact (%d writes)",
            r.group, r.rid, writes(ops0, r.rid));
    }
    int stray = 0;
    for (size_t i = ops0; i < x.nvm_ops.size(); i++) {
      const uint8_t g = x.nvm_ops[i].region;
      bool listed = false;
      for (const auto& r : s1_rows()) listed = listed || (r.rid == g);
      if (!listed) ++stray;
    }
    CHECK(stray == 0, "D3S1: no record outside the nine rows set (%d ops)", stray);
  }

  // D3S2: the pending export. d3_unflushed_o rises in the cycle after the
  // store's accepting cycle and falls in the cycle after the port's done of
  // the record's WRITE; it is 1 in every cycle between.
  void s2_unflushed_follows_the_record() {
    power_up();
    const uint16_t w0 = x.d->dbg_dyn_writes_o;
    const uint16_t s = seq++;
    x.feed(frame(AEM_SET_STREAM_INFO, pl_ptof(1, 777000), s));
    long accept = -1;
    long rise = -1;
    long done = -1;
    long fall = -1;
    long holes = 0;
    for (long c = 0; c < 2 * WINDOW && fall < 0; ++c) {
      const long at = long(x.t);
      x.step();
      if (accept < 0 && x.d->dbg_dyn_writes_o != w0) accept = at;
      if (rise < 0 && x.d->d3_unflushed_o) rise = at + 1;
      if (done < 0 && rise >= 0 && x.d->dbg_d3_mdone_o) done = at + 1;
      if (rise >= 0 && fall < 0 && !x.d->d3_unflushed_o) fall = at + 1;
      if (rise >= 0 && done < 0 && !x.d->d3_unflushed_o) ++holes;
    }
    CHECK(accept >= 0 && rise == accept + 1 && done > rise && fall == done + 1
              && holes == 0,
          "D3S2: accepted at %ld, unflushed from %ld, the port's done at %ld, "
          "clear from %ld", accept, rise, done, fall);
  }

  // D3S3: two changes to one row in one window coalesce into one WRITE that
  // carries the later value.
  void s3_one_window_one_write() {
    power_up();
    const size_t ops0 = x.nvm_ops.size();
    const bool ok = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 1111111))
                    && set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 2222222));
    CHECK(ok && settle(3 * WINDOW) && writes(ops0, 0x50) == 1
              && held(0x50, 4) == d3_record(0x50, 2222222, 4),
          "D3S3: two changes in one window, %d WRITE of 0x50 carrying the later",
          writes(ops0, 0x50));
  }

  // D3S4: a change after the latch taints the write in flight. The device
  // withholds the grant of the WRITE's ERASE, the row changes meanwhile, and
  // the tainted done clears nothing: a second WRITE carries the new value.
  void s4_a_change_during_the_write_taints_it() {
    power_up();
    const size_t ops0 = x.nvm_ops.size();
    x.nv_gnt_hold = 1 << 30;
    const bool a = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 3000001));
    const bool held_req = wait_request(0x50, 2 * WINDOW);
    const bool b = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 3000002));
    x.nv_gnt_hold = 0;
    const bool drained = settle(3 * WINDOW);
    std::vector<uint8_t> first;
    for (size_t i = ops0; i < x.nvm_ops.size() && first.empty(); i++)
      if (x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == 0x50) first = x.nvm_ops[i].wr;
    CHECK(a && b && held_req && drained && writes(ops0, 0x50) == 2
              && first == d3_record(0x50, 3000001, 4)
              && held(0x50, 4) == d3_record(0x50, 3000002, 4),
          "D3S4 taint: %d WRITEs of 0x50, the first carrying the latched value, "
          "the last the change made during it", writes(ops0, 0x50));
  }

  //! cycles from the end of feed() to the store's accepting cycle
  long write_latency(const std::vector<uint8_t>& pl) {
    const uint16_t w0 = x.d->dbg_dyn_writes_o;
    const uint16_t s = seq++;
    x.q_aecp.clear();
    x.feed(frame(AEM_SET_STREAM_INFO, pl, s));
    const long t0 = long(x.t);
    for (long c = 0; c < 5000; ++c) {
      const long at = long(x.t);
      x.step();
      if (x.d->dbg_dyn_writes_o != w0) return at - t0;
    }
    return -1;
  }

  // D3S5: a change on the WRITE's done edge wins. The same command's latency
  // to the store is measured first; the held WRITE's device completion is
  // then placed so that the port's done and the store's accepting cycle
  // coincide (the premise is graded), and the record is rewritten with the
  // newer value.
  void s5_a_change_on_the_done_edge_wins() {
    power_up();
    const long lw = write_latency(pl_ptof(0, 4000000));
    const bool calm = settle(3 * WINDOW);
    const size_t ops0 = x.nvm_ops.size();
    x.nv_done_at = 1L << 40;                      // hold the WRITE's completion
    const bool a = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 4000001));
    bool full = false;
    for (long c = 0; c < 2 * WINDOW && !full; ++c) {
      x.step();
      full = x.nv_st == H::NvState::NV_WRITE && x.nv_left == 0
             && x.nv_cur.region == 0x50;
    }
    const uint16_t w0 = x.d->dbg_dyn_writes_o;
    const uint16_t s = seq++;
    x.feed(frame(AEM_SET_STREAM_INFO, pl_ptof(0, 4000002), s));
    x.nv_done_at = long(x.t) + lw - 1;            // the port's done lands on it
    long accept = -1;
    long done = -1;
    for (long c = 0; c < 5000 && (accept < 0 || done < 0); ++c) {
      const long at = long(x.t);
      x.step();
      if (accept < 0 && x.d->dbg_dyn_writes_o != w0) accept = at;
      if (done < 0 && x.d->dbg_d3_mdone_o) done = at + 1;
    }
    x.nv_done_at = -1;
    CHECK(lw > 1 && calm && a && full && accept >= 0 && accept == done,
          "D3S5 premise: the change accepted at %ld, the WRITE's done at %ld",
          accept, done);
    CHECK(settle(3 * WINDOW) && writes(ops0, 0x50) == 2
              && held(0x50, 4) == d3_record(0x50, 4000002, 4),
          "D3S5 same edge: the change on the done edge is written (%d WRITEs)",
          writes(ops0, 0x50));
  }

  // D3S6: a record's done clears that record alone, named by group AND
  // index. While 0x50's WRITE is held, the second output's offset (same
  // group, 0x51) and the clock source (another group, index 0) change;
  // both are written after it.
  void s6_the_clear_names_group_and_index() {
    power_up();
    const size_t ops0 = x.nvm_ops.size();
    x.nv_gnt_hold = 1 << 30;
    const bool a = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 5000001));
    const bool held_req = wait_request(0x50, 2 * WINDOW);
    const bool b = set_ok(AEM_SET_STREAM_INFO, pl_ptof(1, 5000002));
    const bool c = set_ok(AEM_SET_CLOCK_SOURCE, pl_clk(1));
    x.nv_gnt_hold = 0;
    const bool drained = settle(4 * WINDOW);
    CHECK(a && b && c && held_req && drained
              && held(0x50, 4) == d3_record(0x50, 5000001, 4)
              && held(0x51, 4) == d3_record(0x51, 5000002, 4),
          "D3S6 group: 0x51 changed during 0x50's WRITE is written after it "
          "(%d WRITEs of 0x51)", writes(ops0, 0x51));
    CHECK(drained && held(0x0A, 2) == d3_record(0x0A, 1, 2),
          "D3S6 index: the clock source changed during 0x50's WRITE is written "
          "after it (%d WRITEs of 0x0A)", writes(ops0, 0x0A));
  }

  // D3S7: IDENTIFY is volatile (Milan 5.3.12): SET_CONTROL on it raises no
  // pending, writes no record and moves no NVM operation.
  void s7_identify_is_never_persisted() {
    power_up();
    const size_t ops0 = x.nvm_ops.size();
    const bool ok = set_ok(AEM_SET_CONTROL, pl_identify(255));
    long pending = 0;
    for (long c = 0; c < 2 * WINDOW; ++c) {
      x.step();
      pending += x.d->d3_unflushed_o ? 1 : 0;
    }
    CHECK(ok && pending == 0 && x.nvm_ops.size() == ops0 && x.d->dbg_d3_dirty_o == 0,
          "D3S7: IDENTIFY set, %ld pending cycles, %zu NVM operations",
          pending, x.nvm_ops.size() - ops0);
  }

  // D3S8 (DR2b): an identical rewrite after convergence writes nothing; a
  // row becoming valid at its reset value still changes its projection and
  // is written.
  void s8_only_a_proven_unchanged_projection_is_suppressed() {
    power_up();
    const bool a = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 6000000));
    const bool calm = settle(3 * WINDOW);
    const size_t ops0 = x.nvm_ops.size();
    const bool b = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 6000000));
    long pending = 0;
    for (long c = 0; c < 2 * WINDOW; ++c) {
      x.step();
      pending += x.d->d3_unflushed_o ? 1 : 0;
    }
    CHECK(a && b && calm && pending == 0 && x.nvm_ops.size() == ops0,
          "D3S8 unchanged: the identical rewrite, %ld pending cycles, %zu "
          "NVM operations", pending, x.nvm_ops.size() - ops0);
    const bool c = set_ok(AEM_SET_CLOCK_SOURCE, pl_clk(0));
    CHECK(c && settle(3 * WINDOW) && writes(ops0, 0x0A) == 1
              && held(0x0A, 2) == d3_record(0x0A, 0, 2),
          "D3S8 validity: clock source 0 on the unset row is written (%d)",
          writes(ops0, 0x0A));
  }

  // D3S10 (DR2c): a record whose every WRITE fails is attempted three times
  // in all, each retry starting RETRY_BACKOFF_CYC_P cycles or more after the
  // failed attempt's error (the wrap's 50,000: 500 ms of its compressed
  // time), then dropped with the reset-sticky nvm_alarm_o: pending falls
  // with it, no fourth attempt follows, and a later successful write does
  // not forgive it.
  static constexpr long BACKOFF = 50000;           //! the wrap's override
  void s10_bounded_attempts_then_the_sticky_alarm() {
    power_up();
    const size_t ops0 = x.nvm_ops.size();
    x.nv_err_region = 0x50;
    x.nv_err_writes = 1000;
    const bool a = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 7000001));
    std::vector<long> grants;
    std::vector<long> errs;
    for (long c = 0; c < 6 * WINDOW && !x.d->nvm_alarm_o; ++c) {
      const long at = long(x.t);
      x.step();
      if (x.d->dbg_d3_mgnt_o) grants.push_back(at + 1);
      if (x.d->dbg_d3_merr_o) errs.push_back(at + 1);
    }
    const bool dropped = settle(2 * WINDOW);
    const int failed = writes(ops0, 0x50, 3);
    CHECK(a && dropped && failed == 3 && grants.size() == 3 && writes(ops0, 0x50) == 0
              && x.d->nvm_alarm_o && !x.d->d3_unflushed_o,
          "D3S10 count: %d failed attempts of 0x50 (%zu grants), alarm %u, "
          "unflushed %u", failed, grants.size(), unsigned(x.d->nvm_alarm_o),
          unsigned(x.d->d3_unflushed_o));
    const bool spaced = grants.size() == 3 && errs.size() >= 2
                        && grants[1] - errs[0] >= BACKOFF
                        && grants[2] - errs[1] >= BACKOFF;
    CHECK(spaced,
          "D3S10 timing: each retry granted RETRY_BACKOFF_CYC_P cycles or more "
          "after the failed attempt's error (%ld, %ld)",
          grants.size() > 1 && !errs.empty() ? grants[1] - errs[0] : -1L,
          grants.size() > 2 && errs.size() > 1 ? grants[2] - errs[1] : -1L);
    x.idle(static_cast<int>(3 * BACKOFF));
    CHECK(writes(ops0, 0x50, 3) == 3, "D3S10 count: no fourth attempt (%d)",
          writes(ops0, 0x50, 3));
    x.nv_err_region = -1;
    const bool b = set_ok(AEM_SET_STREAM_INFO, pl_ptof(1, 7000002));
    CHECK(b && settle(3 * WINDOW) && held(0x51, 4) == d3_record(0x51, 7000002, 4)
              && x.d->nvm_alarm_o,
          "D3S10 revocation: a later successful write leaves the alarm set");
  }

  // D3S11: the latch waits for a running command. A READ_DESCRIPTOR whose
  // descriptor fetch the memory answers 3,000 cycles late is running when
  // the debounce closes; the writer owns the bus from ACQUIRE and latches
  // only after that command retires (graded every cycle by D3S9), and the
  // value it writes is the one the SET left.
  void s11_the_latch_waits_for_a_running_command() {
    power_up();
    const size_t ops0 = x.nvm_ops.size();
    const long t0 = long(x.t);
    const bool a = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 8000001));
    while (long(x.t) < t0 + WINDOW - 1500) x.step();
    x.dram_delay_next = 3000;
    x.q_aecp.clear();
    x.feed(d3_read_entity_cmd(seq++));
    const auto rd = x.wait_any(x.q_aecp, 100);
    const bool drained = settle(2 * WINDOW);
    CHECK(a && !rd.empty() && drained && x.d3_own_max > 500
              && held(0x50, 4) == d3_record(0x50, 8000001, 4)
              && writes(ops0, 0x50) == 1,
          "D3S11: the writer owned %ld cycles waiting for the running command, "
          "then wrote the SET's value", x.d3_own_max);
  }

  //! D3S9: across every case, no command started while the writer owned the
  //! state bus and no latch overlapped a running program
  void s9_the_hold_graded_every_cycle() {
    CHECK(x.d3_start_owned == 0 && x.d3_latch_busy == 0,
          "D3S9: %ld commands taken while owned, %ld latch cycles over a "
          "running program; the longest service ownership %ld cycles",
          x.d3_start_owned, x.d3_latch_busy, x.d3_own_max);
  }

  void run() {
    s1_every_group_persists_its_record();
    s2_unflushed_follows_the_record();
    s3_one_window_one_write();
    s4_a_change_during_the_write_taints_it();
    s5_a_change_on_the_done_edge_wins();
    s6_the_clear_names_group_and_index();
    s7_identify_is_never_persisted();
    s8_only_a_proven_unchanged_projection_is_suppressed();
    s10_bounded_attempts_then_the_sticky_alarm();
    s11_the_latch_waits_for_a_running_command();
    s9_the_hold_graded_every_cycle();
  }
};

// ==== D3R. the restore transaction: both passes, the rules, the verdicts ===
struct D3RestorePhase {
  H& h;
  const milan::tb::Model<Vpp_top_wrap> model;
  H x;
  const std::vector<uint8_t>& image;
  uint32_t au_addr = 0;            //! the AUDIO_UNIT descriptor in the image
  uint16_t seq = 0xD600;
  static constexpr long WINDOW = 500L * MS_CYC;
  //! the top's NVM_RS_TMO_CYC_P and NVM_RS_AGG_CYC_P, both derived from the
  //! wrap's clock (DR3a: 20 ms per wait, 1,000 ms in all)
  static constexpr long RS_TMO = clk_ms(20);
  static constexpr long AGG = clk_ms(1000);

  D3RestorePhase(H& tally, const std::vector<uint8_t>& img,
                 const std::vector<ImgEnt>& ents)
      : h(tally), x(model.get()), image(img) {
    for (const auto& e : ents)
      if (e.type == 0x0002) au_addr = DESC_BASE + e.off;
  }

  // ---- the device and the commands, as D3S uses them ----------------------
  void seed(uint8_t rid, const std::vector<uint8_t>& rec) {
    std::fill(x.nv_mem[rid].begin(), x.nv_mem[rid].end(), 0xFF);
    std::copy(rec.begin(), rec.end(), x.nv_mem[rid].begin());
  }
  std::vector<uint8_t> ask(uint16_t op, const std::vector<uint8_t>& pl) {
    const uint16_t s = seq++;
    x.q_aecp.clear();
    x.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, s, op, pl));
    return x.wait_frame(x.q_aecp, 100, [s](const std::vector<uint8_t>& r) {
      return r.size() >= 38 && fv_u64(r, 34, 2) == s;
    });
  }
  static std::vector<uint8_t> ti(uint16_t ty, uint16_t ix) {
    std::vector<uint8_t> p(4, 0);
    putbe(&p[0], ty, 2);
    putbe(&p[2], ix, 2);
    return p;
  }
  //! a reset that keeps the device (a power cycle: the flash is carried).
  //! The device face is reset with the processor, as the product couples
  //! the two resets: a request still presented as the reset began may have
  //! been granted by the model, and no walk issues one before its go.
  void power_cycle() {
    x.nv_rd_region = -1;
    x.nv_rd_fault = false;
    x.nv_gnt_hold = 0;
    x.nv_gnt_every = 0;
    x.nv_gnt_seen = false;
    x.nv_st = H::NvState::NV_IDLE;
    x.reset();
    x.nv_st = H::NvState::NV_IDLE;
    x.q_aecp.clear();
    x.q_adp.clear();
  }
  void fresh() {
    x.dram = image;
    x.erase_nvm();
    power_cycle();
  }

  template <class Wide>
  static bool wide_zero(const Wide& w, int words) {
    for (int i = 0; i < words; i++)
      if (w.at(i) != 0) return false;
    return true;
  }
  //! every persisted row at its reset value with its valid flag clear
  bool rows_cleared() const {
    const auto* d = x.d;
    return d->dbg_dyn_cfg_o == 0 && !d->dbg_dyn_cfg_v_o && d->dbg_dyn_rate_o == 0
           && !d->dbg_dyn_rate_v_o && d->dbg_dyn_clk_o == 0 && !d->dbg_dyn_clk_v_o
           && d->aecp_fmt_in_v_o == 0 && d->aecp_fmt_out_v_o == 0
           && d->aecp_pt_offset_v_o == 0 && wide_zero(d->aecp_fmt_in_o, 16)
           && wide_zero(d->aecp_fmt_out_o, 16) && wide_zero(d->aecp_pt_offset_o, 8);
  }

  //! what one boot observed from restore_go_i: the rows at the D3 walk's
  //! start (the admission gate's release), the terminal, the first ADPDU
  struct Boot {
    bool cleared = false;
    long release = -1;
    long done = -1;
    long closed = -1;
    long adp_first = -1;
    long aecp_first = -1;    //! first AECP response on the wire
    long pending = 0;        //! cycles a D3 record read unflushed
    long enable_early = 0;   //! cycles ADP was enabled before both terminals
    long done_early = 0;     //! cycles restore_done_o led the D3 terminal
  };
  //! the per-cycle facts every boot records
  void observe(Boot& b, long c) {
    const auto* d = x.d;
    const bool both = d->dbg_d3_done_o && d->dbg_lsn_released_o;
    if (b.done < 0 && d->restore_done_o) b.done = c;
    if (b.closed < 0 && d->restore_closed_o) b.closed = c;
    if (b.adp_first < 0 && !x.q_adp.empty()) b.adp_first = c;
    if (b.aecp_first < 0 && !x.q_aecp.empty()) b.aecp_first = c;
    b.pending += d->d3_unflushed_o ? 1 : 0;
    b.enable_early += (d->dbg_adp_enable_o && !both) ? 1 : 0;
    b.done_early += (d->restore_done_o && !both) ? 1 : 0;
  }
  Boot boot(long cycles) {
    Boot b;
    x.d->restore_go_i = 1;
    for (long c = 0; c < cycles; ++c) {
      if (c == 5) x.d->restore_go_i = 0;
      x.step();
      if (b.release < 0 && x.d->dbg_lsn_released_o) {
        b.release = c;
        b.cleared = rows_cleared();
      }
      observe(b, c);
      if (b.done >= 0 && c > b.done + 3000) break;
    }
    x.d->restore_go_i = 0;
    return b;
  }

  bool ok(uint16_t op, const std::vector<uint8_t>& pl) {
    const auto f = ask(op, pl);
    return f.size() > 16 && ((f[16] >> 3) & 0x1F) == AECP_SUCCESS;
  }

  // ---- the volatile set (Milan 5.3.4.1, 5.3.4.2, 5.3.12) ------------------
  static constexpr uint64_t C2_MAC = 0x0202C2C2C2C2ULL;
  static constexpr uint16_t AEM_LOCK_ENTITY = 0x0001;
  static constexpr uint16_t AEM_REGISTER_UNSOL = 0x0024;
  //! a SET_CLOCK_SOURCE from the second controller, answered SUCCESS
  bool c2_sets_clock(uint16_t source) {
    const uint16_t s = seq++;
    x.q_aecp.clear();
    x.feed(aecp_frame(OWN_MAC, C2_MAC, 0, 0, EID, CTLR2_EID, s,
                      AEM_SET_CLOCK_SOURCE, D3ServicePhase::pl_clk(source)));
    const auto f = x.wait_frame(x.q_aecp, 100, [s](const std::vector<uint8_t>& r) {
      return r.size() >= 38 && fv_u64(r, 34, 2) == s;
    });
    return f.size() > 16 && ((f[16] >> 3) & 0x1F) == AECP_SUCCESS;
  }
  //! unsolicited responses (u = 1) addressed to the first controller within
  //! the next `ms`, the frames already queued included
  int unsolicited_to_ctlr(int ms) {
    for (long c = 0; c < ms * MS_CYC; ++c) x.step();
    int n = 0;
    for (const auto& r : x.q_aecp)
      n += (r.size() > 36 && fv_u64(r, 0, 6) == CTLR_MAC && (r[36] & 0x80) != 0) ? 1 : 0;
    return n;
  }
  //! populate the volatile set before the saved-set cycle: the first
  //! controller registered (proved by a notification the second one's
  //! change sends it), then the entity locked by it and IDENTIFY set
  bool populate_volatile() {
    const bool reg = ok(AEM_REGISTER_UNSOL, std::vector<uint8_t>(4, 0));
    const bool c2 = c2_sets_clock(1);
    const int notes = unsolicited_to_ctlr(20);
    const bool lock = ok(AEM_LOCK_ENTITY, std::vector<uint8_t>(16, 0));
    const bool ident = ok(AEM_SET_CONTROL, D3ServicePhase::pl_identify(255));
    return reg && c2 && notes >= 1 && lock && ident && x.d->dbg_lock_held_o
           && x.d->dbg_identify_o == 255;
  }
  //! the nine rows D3S1 sets: every group at its first and last declared
  //! index, saved through real SETs and a flush
  bool save_all_rows() {
    using S = D3ServicePhase;
    bool all = ok(AEM_SET_CONFIGURATION, S::pl_cfg(1));
    all = ok(AEM_SET_SAMPLING_RATE, S::pl_rate(48000)) && all;
    all = ok(AEM_SET_CLOCK_SOURCE, S::pl_clk(2)) && all;
    all = ok(AEM_SET_STREAM_FORMAT, S::pl_fmt(0x0005, 0, H::SFMT_ALT_C)) && all;
    all = ok(AEM_SET_STREAM_FORMAT, S::pl_fmt(0x0005, 1, H::SFMT_MAIN_C)) && all;
    all = ok(AEM_SET_STREAM_FORMAT, S::pl_fmt(0x0006, 0, H::SFMT_ALT_C)) && all;
    all = ok(AEM_SET_STREAM_FORMAT, S::pl_fmt(0x0006, 1, H::SFMT_MAIN_C)) && all;
    all = ok(AEM_SET_STREAM_INFO, S::pl_ptof(0, 1000000)) && all;
    all = ok(AEM_SET_STREAM_INFO, S::pl_ptof(1, 2500000)) && all;
    for (long c = 0; c < 2 * WINDOW; ++c) x.step();
    return all && !x.d->d3_unflushed_o;
  }

  //! the crc recomputed after a test edits a header or payload byte
  static std::vector<uint8_t> reframe(std::vector<uint8_t> r) {
    const uint16_t c = d3_crc16(r);
    r[6] = static_cast<uint8_t>(c >> 8);
    r[7] = static_cast<uint8_t>(c);
    return r;
  }

  // D3R1: every group saved through real SETs comes back across a power
  // cycle. When the D3 walk starts, every row reads its reset value with
  // its valid flag clear; the walk ends COMPLETE with exact counts; the
  // enable requested from reset reaches ADP only after the combined
  // terminal; and no restore write becomes a change (no pending, no WRITE).
  void r1_every_group_survives_a_power_cycle() {
    fresh();
    CHECK(x.boot_to_aecp() && populate_volatile(),
          "D3R1: registered, locked and identifying before the save (premise)");
    CHECK(save_all_rows(), "D3R1: nine rows saved through real SETs");
    power_cycle();
    x.d->link_up_i = 1;
    x.d->entity_enable_i = 1;                     // requested from reset (W14)
    const Boot b = boot(6 * RS_TMO);
    CHECK(b.cleared, "D3R1: every row at its reset value, valid clear, when the D3 walk starts");
    CHECK(b.done > b.release && !x.d->restore_fail_o && !x.d->restore_blank_o
              && !x.d->restore_closed_o && x.d->dbg_d3_applied_o == 9
              && x.d->dbg_d3_refused_o == 0 && x.d->dbg_d3_blank_o == 18,
          "D3R1: COMPLETE %ld cycles after the release, applied %u refused %u "
          "blank %u of 27", b.done - b.release, unsigned(x.d->dbg_d3_applied_o),
          unsigned(x.d->dbg_d3_refused_o), unsigned(x.d->dbg_d3_blank_o));
    //! graded on the ADP engine's enable input, every cycle: ADP's own
    //! 0-2 s start delay would hide an early enable from the wire
    CHECK(b.enable_early == 0 && b.done_early == 0 && x.d->dbg_adp_enable_o,
          "D3R1: the enable requested from reset reaches ADP only at the "
          "combined terminal (%ld early enable cycles, %ld early done cycles)",
          b.enable_early, b.done_early);
    const size_t ops0 = x.nvm_ops.size();
    long pending = b.pending;
    for (long c = 0; c < 2 * WINDOW; ++c) {
      x.step();
      pending += x.d->d3_unflushed_o ? 1 : 0;
    }
    int writes = 0;
    for (size_t i = ops0; i < x.nvm_ops.size(); i++) writes += x.nvm_ops[i].op != 0;
    CHECK(pending == 0 && writes == 0,
          "D3R1: no restore write is a change (%ld pending cycles, %d device writes)",
          pending, writes);
    CHECK(b.adp_first < 0 || b.adp_first > b.done,
          "D3R1: no ADPDU before the terminal");
    r1_readback();
    r1_volatile_set_is_gone();
  }

  //! the volatile set, populated before the same cycle, is absent after it:
  //! IDENTIFY reads 0, the lock is free (the second controller's SET is
  //! accepted) and the registry is empty (that change notifies nobody)
  void r1_volatile_set_is_gone() {
    const auto g = ask(AEM_GET_CONTROL, ti(0x001A, 0));
    CHECK(x.d->dbg_identify_o == 0 && g.size() > 42 && g[42] == 0,
          "D3R1 volatile: IDENTIFY reads 0 after the restore (%u)",
          unsigned(x.d->dbg_identify_o));
    const bool unlocked = !x.d->dbg_lock_held_o;
    const bool c2 = c2_sets_clock(1);
    const int notes = unsolicited_to_ctlr(20);
    CHECK(unlocked && c2, "D3R1 volatile: the lock is free after the restore");
    CHECK(notes == 0, "D3R1 volatile: the registry is empty after the restore "
          "(%d notifications)", notes);
  }

  //! each group's value AND valid flag, then a real GET of it
  void r1_readback() {
    const auto* d = x.d;
    auto g = ask(AEM_GET_CONFIGURATION, {});
    CHECK(d->dbg_dyn_cfg_v_o && d->dbg_dyn_cfg_o == 1 && g.size() >= 42
              && fv_u64(g, 40, 2) == 1,
          "D3R1 cfg: configuration 1 restored with its valid flag, GET reads it");
    g = ask(AEM_GET_SAMPLING_RATE, ti(0x0002, 0));
    CHECK(d->dbg_dyn_rate_v_o && d->dbg_dyn_rate_o == 48000 && g.size() >= 46
              && rd32(&g[42]) == 48000,
          "D3R1 rate: 48000 restored with its valid flag, GET reads it");
    g = ask(AEM_GET_CLOCK_SOURCE, ti(0x0024, 0));
    CHECK(d->dbg_dyn_clk_v_o && d->dbg_dyn_clk_o == 2 && g.size() >= 44
              && fv_u64(g, 42, 2) == 2,
          "D3R1 clks: clock source 2 restored with its valid flag, GET reads it");
    const auto gi0 = ask(AEM_GET_STREAM_FORMAT, ti(0x0005, 0));
    const auto gi1 = ask(AEM_GET_STREAM_FORMAT, ti(0x0005, 1));
    CHECK(d->aecp_fmt_in_v_o == 0x03 && x.fmt_row(false, 0) == H::SFMT_ALT_C
              && x.fmt_row(false, 1) == H::SFMT_MAIN_C && gi0.size() >= 50
              && gi1.size() >= 50 && fv_u64(gi0, 42, 8) == H::SFMT_ALT_C
              && fv_u64(gi1, 42, 8) == H::SFMT_MAIN_C,
          "D3R1 fmti: both input formats restored with their valid flags, GET "
          "reads them (valid 0x%02x)", unsigned(d->aecp_fmt_in_v_o));
    const auto go0 = ask(AEM_GET_STREAM_FORMAT, ti(0x0006, 0));
    const auto go1 = ask(AEM_GET_STREAM_FORMAT, ti(0x0006, 1));
    CHECK(d->aecp_fmt_out_v_o == 0x03 && x.fmt_row(true, 0) == H::SFMT_ALT_C
              && x.fmt_row(true, 1) == H::SFMT_MAIN_C && go0.size() >= 50
              && go1.size() >= 50 && fv_u64(go0, 42, 8) == H::SFMT_ALT_C
              && fv_u64(go1, 42, 8) == H::SFMT_MAIN_C,
          "D3R1 fmto: both output formats restored with their valid flags, GET "
          "reads them (valid 0x%02x)", unsigned(d->aecp_fmt_out_v_o));
    const auto gs0 = ask(AEM_GET_STREAM_INFO, ti(0x0006, 0));
    const auto gs1 = ask(AEM_GET_STREAM_INFO, ti(0x0006, 1));
    CHECK(d->aecp_pt_offset_v_o == 0x03 && d->aecp_pt_offset_o.at(0) == 1000000
              && d->aecp_pt_offset_o.at(1) == 2500000 && gs0.size() >= 66
              && gs1.size() >= 66 && rd32(&gs0[62]) == 1000000
              && rd32(&gs1[62]) == 2500000,
          "D3R1 ptof: both presentation offsets restored with their valid "
          "flags, GET_STREAM_INFO reads them (valid 0x%02x)",
          unsigned(d->aecp_pt_offset_v_o));
  }

  // D3R2: framed records a SET program would refuse keep their defaults and
  // the walk goes on: configuration past configurations_count, a rate off
  // the AUDIO_UNIT's list, a clock source index equal to the count, formats
  // the integrator judges unsupported, an offset with bit 31 set. Records
  // whose frame fails (crc, layout version, another record's id, a u64 group
  // carrying four bytes) are refused before any rule. Their neighbour still
  // applies, and nothing aborts.
  void r2_the_set_rules_and_the_frame_refuse() {
    fresh();
    seed(0x00, d3_record(0x00, 5, 2));
    seed(0x02, d3_record(0x02, 44100, 4));
    seed(0x0A, d3_record(0x0A, 3, 2));
    seed(0x30, d3_record(0x30, 0xDEADBEEF00C0FFEEull, 8));
    seed(0x40, d3_record(0x40, 0x0205022001406000ull, 8));
    seed(0x50, d3_record(0x50, 0x80000005u, 4));
    seed(0x51, d3_record(0x51, 1600000, 4));
    auto crc = d3_record(0x31, H::SFMT_MAIN_C, 8);
    crc[9] ^= 0x01;                                // payload changed, crc kept
    seed(0x31, crc);
    auto ver = d3_record(0x41, H::SFMT_MAIN_C, 8);
    ver[2] = 0x03;
    seed(0x41, reframe(ver));
    seed(0x37, d3_record(0x36, H::SFMT_MAIN_C, 8));
    seed(0x47, d3_record(0x47, 0x00001234u, 4));
    const Boot b = boot(6 * RS_TMO);
    const auto* d = x.d;
    CHECK(b.done > b.release && !d->restore_fail_o && d->dbg_d3_applied_o == 1
              && d->dbg_d3_refused_o == 10 && d->dbg_d3_blank_o == 16,
          "D3R2: COMPLETE, applied %u refused %u blank %u of 27",
          unsigned(d->dbg_d3_applied_o), unsigned(d->dbg_d3_refused_o),
          unsigned(d->dbg_d3_blank_o));
    CHECK(!d->dbg_dyn_cfg_v_o && !d->dbg_dyn_rate_v_o && !d->dbg_dyn_clk_v_o
              && d->aecp_fmt_in_v_o == 0 && d->aecp_fmt_out_v_o == 0
              && d->aecp_pt_offset_v_o == 0x02 && d->aecp_pt_offset_o.at(1) == 1600000,
          "D3R2: only the neighbour's offset restored (valid 0x%02x)",
          unsigned(d->aecp_pt_offset_v_o));
  }

  // D3R3: the other side of every rule: configuration 0, the rate list's
  // second entry (96000) and clock source 0 are accepted.
  void r3_the_set_rules_accept() {
    fresh();
    seed(0x00, d3_record(0x00, 0, 2));
    seed(0x02, d3_record(0x02, 96000, 4));
    seed(0x0A, d3_record(0x0A, 0, 2));
    const Boot b = boot(6 * RS_TMO);
    const auto* d = x.d;
    CHECK(b.done > b.release && !d->restore_fail_o && d->dbg_d3_applied_o == 3
              && d->dbg_dyn_cfg_v_o && d->dbg_dyn_cfg_o == 0 && d->dbg_dyn_rate_v_o
              && d->dbg_dyn_rate_o == 96000 && d->dbg_dyn_clk_v_o
              && d->dbg_dyn_clk_o == 0,
          "D3R3: configuration 0, rate 96000 and clock source 0 restored with "
          "their valid flags (applied %u)", unsigned(d->dbg_d3_applied_o));
  }

  //! a boot like boot(), running `hook` after every clock
  template <class Hook>
  Boot boot_with(long cycles, Hook hook) {
    Boot b;
    x.d->restore_go_i = 1;
    for (long c = 0; c < cycles; ++c) {
      if (c == 5) x.d->restore_go_i = 0;
      x.step();
      hook();
      if (b.release < 0 && x.d->dbg_lsn_released_o) b.release = c;
      observe(b, c);
      if ((b.done >= 0 || b.closed >= 0) && c > std::max(b.done, b.closed) + 3000) break;
    }
    x.d->restore_go_i = 0;
    return b;
  }
  int reads_of(size_t from, uint8_t region) const {
    int n = 0;
    for (size_t i = from; i < x.nvm_ops.size(); i++)
      if (x.nvm_ops[i].op == 0 && x.nvm_ops[i].region == region) n++;
    return n;
  }

  // D3R4: the passes agree record by record. 0x50 is read whole in pass 0
  // (its header and payload READs), then erased at rest; pass 1 meets an
  // unframed record, which aborts the restore (cause 5) after the
  // configuration record was applied. The abort rolls both stores back:
  // DEFAULTS, every row at its reset value, the image walked again and
  // AECP running.
  void r4_the_passes_agree_record_by_record() {
    fresh();
    seed(0x00, d3_record(0x00, 1, 2));
    seed(0x50, d3_record(0x50, 1500000, 4));
    const size_t ops0 = x.nvm_ops.size();
    bool erased = false;
    bool applied_before = false;
    const Boot b = boot_with(6 * RS_TMO, [&] {
      if (!erased && reads_of(ops0, 0x50) == 2) {
        std::fill(x.nv_mem[0x50].begin(), x.nv_mem[0x50].begin() + 256, 0xFF);
        erased = true;
      }
      applied_before = applied_before || x.d->dbg_dyn_cfg_v_o;
    });
    CHECK(erased && applied_before && b.done > b.release && x.d->restore_fail_o
              && x.d->rs_cause_o == 5 && x.d->restore_rb_o && !x.d->restore_closed_o
              && rows_cleared() && x.d->dbg_img_valid_o && !x.d->dbg_d3_own_o
              && !x.d->restore_blank_o,
          "D3R4: the record whole in pass 0 and unframed in pass 1 aborts, cause "
          "%u, rolled back %u: every row at its default after the applied "
          "configuration", unsigned(x.d->rs_cause_o), unsigned(x.d->restore_rb_o));
  }

  //! seed a configuration and an offset, arm one fault on 0x50's READs, boot
  Boot faulted_boot(int nth, int after, bool silent) {
    fresh();
    seed(0x00, d3_record(0x00, 1, 2));
    seed(0x50, d3_record(0x50, 1500000, 4));
    x.nv_rd_region = 0x50;
    x.nv_rd_nth = nth;
    x.nv_rd_after = after;
    x.nv_rd_silent = silent;
    x.nv_rd_seen = 0;
    return boot(6 * RS_TMO);
  }

  // D3R5: a transport fault in pass 0 has applied nothing: the restore ends
  // done and failed on defaults, never blank, and AECP runs. A DEVICE error
  // on 0x50's header (cause 2), a payload torn after two bytes (cause 1),
  // and a header READ the device never answers, abandoned at the deadline
  // to the arbiter's drain (cause 3); once the device ends that read, a
  // later SET persists.
  void r5_a_pass_0_fault_applies_nothing() {
    struct Arm { int nth; int after; bool silent; unsigned cause; const char* what; };
    const Arm arms[] = {{1, 0, false, 2, "device error on the header"},
                        {2, 2, false, 1, "payload torn after two bytes"},
                        {1, 0, true, 3, "the header never answered"}};
    for (const auto& a : arms) {
      const Boot b = faulted_boot(a.nth, a.after, a.silent);
      const auto* d = x.d;
      CHECK(b.done > b.release && d->restore_fail_o && !d->restore_blank_o
              && !d->restore_closed_o && d->rs_cause_o == a.cause
              && d->dbg_d3_applied_o == 0 && rows_cleared() && !d->dbg_d3_own_o,
            "D3R5 %s: DEFAULTS, cause %u, applied %u, rows at their defaults",
            a.what, unsigned(d->rs_cause_o), unsigned(d->dbg_d3_applied_o));
    }
    // the drained read ends when the device answers it; the port is free
    const bool drained_busy = x.nv_st == H::NvState::NV_READ;
    x.nv_rd_fault = false;
    x.idle(200);
    const size_t ops0 = x.nvm_ops.size();
    const bool set = ok(AEM_SET_STREAM_INFO, D3ServicePhase::pl_ptof(0, 4242424));
    for (long c = 0; c < 2 * WINDOW; ++c) x.step();
    int writes = 0;
    for (size_t i = ops0; i < x.nvm_ops.size(); i++)
      writes += x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == 0x50;
    CHECK(drained_busy && set && writes == 1
              && std::equal(x.nv_mem[0x50].begin(), x.nv_mem[0x50].begin() + 12,
                            d3_record(0x50, 4242424, 4).begin()),
          "D3R5: once the device ends the drained read a later SET persists");
  }

  // D3R6: the restore meets an erased device (V10) and an unframed record:
  // both are that record's default, never a failure. Erased everywhere is a
  // blank restore; one saved record lost to a device error never is (H8).
  void r6_blank_is_honest() {
    fresh();
    Boot b = boot(6 * RS_TMO);
    CHECK(b.done > b.release && !x.d->restore_fail_o && x.d->restore_blank_o
              && x.d->dbg_d3_blank_o == 27,
          "D3R6: an erased device restores blank, not failed (%u of 27 blank)",
          unsigned(x.d->dbg_d3_blank_o));
    fresh();
    std::vector<uint8_t> junk(12, 0x5A);           // no F07.8 magic
    seed(0x50, junk);
    b = boot(6 * RS_TMO);
    CHECK(b.done > b.release && !x.d->restore_fail_o && x.d->restore_blank_o
              && x.d->dbg_d3_blank_o == 27,
          "D3R6: an unframed record is that record's default, never a failure");
    //! every READ of the one saved record ends in the device's error, in
    //! both passes, so the pass agreement cannot stand in for the verdict
    fresh();
    seed(0x50, d3_record(0x50, 1500000, 4));
    x.nv_rd_region = 0x50;
    x.nv_rd_nth = 1;
    x.nv_rd_after = 0;
    x.nv_rd_silent = false;
    b = boot_with(6 * RS_TMO, [&] { x.nv_rd_seen = 0; });
    CHECK(b.done > b.release && x.d->restore_fail_o && !x.d->restore_blank_o
              && x.d->rs_cause_o == 2,
          "D3R6: the one saved record lost to a device error is a failure "
          "(cause %u), never a blank first boot", unsigned(x.d->rs_cause_o));
  }

  // D3R7: a descriptor read the rule needs that fails is a transport
  // failure, never a refused value: the AUDIO_UNIT fetch of the rate rule
  // answers an error beat in pass 1, the restore aborts (cause 6) and rolls
  // back to DEFAULTS.
  void r7_a_rule_fetch_fault_aborts() {
    fresh();
    seed(0x02, d3_record(0x02, 96000, 4));
    x.dram_err_at = au_addr;
    const Boot b = boot(6 * RS_TMO);
    CHECK(au_addr != 0 && x.dram_err_at == 0 && b.done > b.release
              && x.d->restore_fail_o && x.d->rs_cause_o == 6 && x.d->restore_rb_o
              && x.d->dbg_d3_refused_o == 0 && rows_cleared(),
          "D3R7: the rule's descriptor fetch errs: cause %u, refused %u, rolled "
          "back %u", unsigned(x.d->rs_cause_o), unsigned(x.d->dbg_d3_refused_o),
          unsigned(x.d->restore_rb_o));
  }

  // D3R10: the rule's AUDIO_UNIT fetch answers late. 4,000 cycles is inside
  // the store's 4,096-cycle watchdog: COMPLETE. At 5,000 and 16,000 the
  // store answers its watchdog's error, the restore aborts (cause 6) and
  // rolls back; both stores stay in reset while the memory still owes the
  // abandoned burst (the guard's debt, which the stores' reset does not
  // clear), leave it only after the late burst, and the re-walked image is
  // proven: DEFAULTS. At 30,000 the debt outlasts the restore deadline:
  // CLOSED.
  void r10_a_late_burst_is_waited_out() {
    struct Arm { int late; bool rolled; bool closed; };
    const Arm arms[] = {{4000, false, false}, {5000, true, false},
                        {16000, true, false}, {30000, false, true}};
    for (const auto& a : arms) {
      fresh();
      seed(0x02, d3_record(0x02, 96000, 4));
      x.dram_late_at = au_addr;
      x.dram_late_cycles = a.late;
      long rb_rise = -1;
      long rb_fall = -1;
      long debt_fall = -1;
      long debt_owed_in_rb = 0;
      long c = 0;
      const Boot b = boot_with(8 * RS_TMO, [&] {
        ++c;
        const bool rb = x.d->dbg_d3_rb_rst_o;
        if (rb && rb_rise < 0) rb_rise = c;
        if (!rb && rb_rise >= 0 && rb_fall < 0) rb_fall = c;
        if (rb) debt_owed_in_rb += x.d->desc_mem_debt_o ? 1 : 0;
        if (rb_rise >= 0 && debt_fall < 0 && !x.d->desc_mem_debt_o) debt_fall = c;
      });
      const bool as_expected =
          a.closed ? (b.closed >= 0 && b.done < 0 && x.d->rs_cause_o == 6)
          : a.rolled ? (b.done >= 0 && x.d->restore_rb_o && x.d->rs_cause_o == 6
                        && rows_cleared() && debt_owed_in_rb > 1
                        && debt_fall >= 0 && rb_fall > debt_fall - 1)
                     : (b.done >= 0 && !x.d->restore_fail_o && x.d->dbg_dyn_rate_v_o);
      CHECK(as_expected,
            "D3R10 %d: done %ld closed %ld cause %u rolled back %u; the stores "
            "in reset from %ld to %ld, the debt owed %ld cycles of it, fell at %ld",
            a.late, b.done, b.closed, unsigned(x.d->rs_cause_o),
            unsigned(x.d->restore_rb_o), rb_rise, rb_fall, debt_owed_in_rb, debt_fall);
    }
  }

  //! a binding record for sink 0 (05 section 5, 20-byte payload, valid and
  //! started), built from the framing, not the RTL
  static std::vector<uint8_t> binding_record(uint64_t talker, uint16_t uid,
                                             uint64_t ctlr) {
    std::vector<uint8_t> r(28, 0);
    r[0] = 0x17; r[1] = 0x22; r[2] = 0x02; r[3] = 0x20; r[5] = 20;
    r[8] = 0x03;
    putbe(&r[10], uid, 2);
    putbe(&r[12], talker, 8);
    putbe(&r[20], ctlr, 8);
    return reframe(r);
  }

  // D3R11: a D3 roll-back leaves the binding walk's work alone: the
  // binding restored for sink 0 stays bound, the listener answers it, and
  // its record is not rewritten. The D3 walk fails in pass 1 on a DEVICE
  // error on 0x50's header after the configuration was applied.
  void r11_a_roll_back_keeps_the_restored_binding() {
    fresh();
    constexpr uint64_t TALKER = 0x00B0B0B0B0B0D311ULL;
    const auto bind = binding_record(TALKER, 0x0D31, CTLR_EID);
    seed(0x20, bind);
    seed(0x00, d3_record(0x00, 1, 2));
    seed(0x50, d3_record(0x50, 1500000, 4));
    x.nv_rd_region = 0x50;
    x.nv_rd_nth = 3;                              // pass 1's header READ
    x.nv_rd_after = 0;
    x.nv_rd_silent = false;
    x.nv_rd_seen = 0;
    const size_t ops0 = x.nvm_ops.size();
    const Boot b = boot(6 * RS_TMO);
    x.q_acmp.clear();
    x.feed(acmp_frame(CTLR_MAC, 10, 0, 0, CTLR_EID, 0, EID, 0, 0, 0, 0, 0xD311, 0, 0));
    const auto g = x.wait_frame(x.q_acmp, 50, [](const std::vector<uint8_t>& f) {
      return f.size() > 15 && (f[15] & 0x0F) == 11;
    });
    int rewrites = 0;
    for (size_t i = ops0; i < x.nvm_ops.size(); i++)
      rewrites += x.nvm_ops[i].op != 0 && x.nvm_ops[i].region == 0x20;
    CHECK(b.done > b.release && x.d->restore_rb_o && x.d->rs_cause_o == 2
              && (x.d->acmp_bound_o & 1) && g.size() > 50 && fv_u64(g, 34, 8) == TALKER
              && rewrites == 0,
          "D3R11: rolled back (cause %u), sink 0 still bound to its restored "
          "talker, %d rewrites of its record", unsigned(x.d->rs_cause_o), rewrites);
  }

  // D3R12: a roll-back whose re-walk cannot prove the image ends CLOSED:
  // the descriptor memory falls silent at the pass-1 abort, the store's
  // watchdog answers the re-LOCATE with an error, and the entity stays
  // held: no done, AECP owned, ADP never enabled.
  void r12_a_roll_back_that_cannot_prove_the_image_closes() {
    fresh();
    seed(0x00, d3_record(0x00, 1, 2));
    seed(0x50, d3_record(0x50, 1500000, 4));
    x.nv_rd_region = 0x50;
    x.nv_rd_nth = 3;
    x.nv_rd_after = 0;
    x.nv_rd_silent = false;
    x.nv_rd_seen = 0;
    x.d->entity_enable_i = 1;
    x.d->link_up_i = 1;
    const Boot b = boot_with(8 * RS_TMO, [&] {
      if (x.d->dbg_d3_rb_rst_o) x.dram_silent = true;
    });
    x.dram_silent = false;
    CHECK(b.closed > b.release && b.done < 0 && x.d->restore_fail_o
              && !x.d->restore_done_o && x.d->dbg_d3_own_o && !x.d->dbg_adp_enable_o
              && x.d->rs_cause_o == 2,
          "D3R12: the re-walk cannot prove the image: CLOSED %ld, cause %u, the "
          "entity held", b.closed, unsigned(x.d->rs_cause_o));
    x.d->entity_enable_i = 0;
  }

  //! every D3 record framed, so each costs its header and payload READs
  void seed_every_record() {
    seed(0x00, d3_record(0x00, 1, 2));
    seed(0x02, d3_record(0x02, 48000, 4));
    seed(0x0A, d3_record(0x0A, 1, 2));
    for (uint8_t k = 0; k < 8; ++k) {
      seed(uint8_t(0x30 + k), d3_record(uint8_t(0x30 + k), H::SFMT_MAIN_C, 8));
      seed(uint8_t(0x40 + k), d3_record(uint8_t(0x40 + k), H::SFMT_MAIN_C, 8));
      seed(uint8_t(0x50 + k), d3_record(uint8_t(0x50 + k), 1000000u + k, 4));
    }
  }

  // D3R13 (DR3a, ratified as an enforced bound): a device that grants every
  // request, the binding walk's included, 200 cycles inside the per-wait
  // deadline trips no wait's deadline, and the restore still ends at the
  // aggregate bound. Every record saved puts the bound in pass 0: DEFAULTS
  // registered by the AGG-th clock counting the one that took restore_go_i
  // as the first, cause 3, nothing applied, the READ in hand abandoned to
  // the drain, and once the device ends it a later SET persists. Over an
  // erased device the bound falls in pass 1, which rolls back to DEFAULTS
  // within one per-wait deadline of it. Without the counter the first walk
  // runs to about 2.3 million clocks.
  void r13_the_aggregate_bound() {
    fresh();
    seed_every_record();
    x.nv_gnt_every = static_cast<int>(RS_TMO - 200);
    long d3_wait = 0;
    const Boot b = boot_with(AGG + 4 * RS_TMO, [&] {
      d3_wait = std::max(d3_wait, long(x.d->dbg_d3_wd_o));
    });
    const auto* d = x.d;
    CHECK(b.done + 1 == AGG && d->restore_fail_o && d->rs_cause_o == 3
              && !d->restore_rb_o && !d->restore_closed_o
              && d->dbg_d3_applied_o == 0 && rows_cleared() && !d->dbg_d3_own_o
              && d3_wait < RS_TMO,
          "D3R13 pass 0: DEFAULTS at clock %ld of the aggregate %ld, cause %u, "
          "rolled back %u, applied %u, the longest wait %ld of %ld",
          b.done + 1, AGG, unsigned(d->rs_cause_o), unsigned(d->restore_rb_o),
          unsigned(d->dbg_d3_applied_o), d3_wait, RS_TMO);
    x.nv_gnt_every = 0;
    const bool draining = x.d->dbg_nvm_drain_o;
    for (long c = 0; c < RS_TMO; ++c) x.step();
    const size_t ops0 = x.nvm_ops.size();
    const bool set = ok(AEM_SET_STREAM_INFO, D3ServicePhase::pl_ptof(0, 4343434));
    for (long c = 0; c < 2 * WINDOW; ++c) x.step();
    int writes = 0;
    for (size_t i = ops0; i < x.nvm_ops.size(); i++)
      writes += x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == 0x50;
    CHECK(draining && set && writes == 1
              && std::equal(x.nv_mem[0x50].begin(), x.nv_mem[0x50].begin() + 12,
                            d3_record(0x50, 4343434, 4).begin()),
          "D3R13 pass 0: the READ in hand at the bound was drained, and once "
          "the device ends it a later SET persists");
    fresh();
    x.nv_gnt_every = static_cast<int>(RS_TMO - 200);
    const Boot e = boot(AGG + 4 * RS_TMO);
    x.nv_gnt_every = 0;
    CHECK(e.done + 1 > AGG && e.done + 1 <= AGG + RS_TMO && d->restore_fail_o
              && d->rs_cause_o == 3 && d->restore_rb_o && !d->restore_closed_o
              && rows_cleared() && !d->dbg_d3_own_o,
          "D3R13 pass 1: rolled back to DEFAULTS at clock %ld, the bound %ld, "
          "cause %u, rolled back %u", e.done + 1, AGG, unsigned(d->rs_cause_o),
          unsigned(d->restore_rb_o));
  }

  // ---- DR3a: restore durations and the longest waits (informational) ----
  // The parent D3 contract's DR3a had the processor lane MEASURE its
  // per-wait (20 ms) and aggregate (1,000 ms) candidates; the manager
  // ratified both, the aggregate as an enforced bound (D3R13 grades it).
  // These lines are printed, never graded: every figure is clk_i cycles of
  // this bench from restore_go_i, with this bench's device and memory
  // models (their latencies are named per line).
  struct Span {
    long release = -1;        //! go to the admission gate's release
    long terminal = -1;       //! go to COMPLETE, DEFAULTS or CLOSED
    uint32_t bind_wait = 0;   //! the binding walk's longest wait (cycles)
    uint32_t d3_wait = 0;     //! the D3 walk's longest wait (cycles)
    long rec_max = 0;         //! the longest D3 port operation, grant to end
  };
  Span measured(long cycles) {
    Span s;
    long gnt_at = -1;
    long now = 0;
    const Boot b = boot_with(cycles, [&] {
      const auto* d = x.d;
      s.bind_wait = std::max(s.bind_wait, uint32_t(d->dbg_bind_wd_o));
      s.d3_wait = std::max(s.d3_wait, uint32_t(d->dbg_d3_wd_o));
      if (d->dbg_d3_mgnt_o) gnt_at = now;
      if ((d->dbg_d3_mdone_o || d->dbg_d3_merr_o) && gnt_at >= 0) {
        s.rec_max = std::max(s.rec_max, now - gnt_at);
        gnt_at = -1;
      }
      ++now;
    });
    s.release = b.release;
    s.terminal = b.done >= 0 ? b.done : b.closed;
    return s;
  }
  void report(const char* what, const Span& s) {
    const auto* d = x.d;
    const char* term = d->restore_closed_o ? "CLOSED"
                       : !d->restore_fail_o ? "COMPLETE" : "DEFAULTS";
    std::printf("DR3a %-46s release %7ld  terminal %8ld  %-8s cause %u  "
                "longest wait: binding %6u D3 %6u  longest D3 record op %4ld\n",
                what, s.release, s.terminal, term, unsigned(d->rs_cause_o),
                s.bind_wait, s.d3_wait, s.rec_max);
  }
  void dr3a_measurements() {
    std::printf("DR3a: cycles from restore_go_i; NVM model grants at once and "
                "streams a byte a cycle; CLK_HZ %ld, RS_TMO %ld, AGG %ld\n",
                CLK_HZ, RS_TMO, AGG);
    const std::array<int, 2> lats{31, 143};
    for (const int lat : lats) {
      x.dram_lat = lat;
      char what[96];
      fresh();
      std::snprintf(what, sizeof what, "erased device, DRAM %d cyc", lat);
      report(what, measured(6 * RS_TMO));
      fresh();
      x.boot_to_aecp();
      save_all_rows();
      power_cycle();
      std::snprintf(what, sizeof what, "nine records saved, DRAM %d cyc", lat);
      report(what, measured(6 * RS_TMO));
      //! the store's own image walk from reset (header, index, names)
      fresh();
      long img = -1;
      for (long c = 0; c < 4 * RS_TMO && img < 0; ++c) {
        x.step();
        if (x.d->dbg_img_valid_o) img = c;
      }
      std::printf("DR3a image walk from reset, DRAM %d cyc: %ld cycles\n", lat, img);
    }
    x.dram_lat = 31;
    fresh();
    seed(0x00, d3_record(0x00, 1, 2));
    seed(0x50, d3_record(0x50, 1500000, 4));
    x.nv_rd_region = 0x50;
    x.nv_rd_nth = 1;
    x.nv_rd_after = 0;
    x.nv_rd_silent = false;
    x.nv_rd_seen = 0;
    report("pass 0: DEVICE error on a header", measured(6 * RS_TMO));
    fresh();
    seed(0x02, d3_record(0x02, 96000, 4));
    x.dram_err_at = au_addr;
    report("pass 1: rule fetch error, roll-back", measured(6 * RS_TMO));
    x.dram_err_at = 0;
    fresh();
    seed(0x02, d3_record(0x02, 96000, 4));
    x.dram_late_at = au_addr;
    x.dram_late_cycles = 16000;
    report("pass 1: rule fetch 16000 late, roll-back on debt", measured(8 * RS_TMO));
    x.dram_late_at = 0;
    fresh();
    x.nv_gnt_hold = 1 << 30;
    report("NVM device silent from its first read", measured(8 * RS_TMO));
    x.nv_gnt_hold = 0;
    x.dram = image;
    x.dram[0] ^= 0xFF;                            // the store refuses the magic
    x.erase_nvm();
    power_cycle();
    report("descriptor image refused (magic)", measured(8 * RS_TMO));
    x.dram = image;
    x.dram_silent = true;
    x.erase_nvm();
    power_cycle();
    report("descriptor memory silent", measured(8 * RS_TMO));
    x.dram_silent = false;
    fresh();
    seed_every_record();
    x.nv_gnt_every = static_cast<int>(RS_TMO - 200);
    report("every grant 200 inside the per-wait deadline", measured(AGG + 4 * RS_TMO));
    x.nv_gnt_every = 0;
    //! DR2a, this producer's share: one real SET, no other traffic, from
    //! the record reading pending to the port's done of its WRITE (the
    //! backend's window, where the integrator's own debounce begins)
    fresh();
    x.boot_to_aecp();
    x.q_aecp.clear();
    x.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, seq++, AEM_SET_STREAM_INFO,
                      D3ServicePhase::pl_ptof(0, 1234567)));
    long rise = -1;
    long fall = -1;
    for (long c = 0; c < 4 * WINDOW && fall < 0; ++c) {
      x.step();
      if (rise < 0 && x.d->d3_unflushed_o) rise = c;
      if (rise >= 0 && fall < 0 && !x.d->d3_unflushed_o) fall = c;
    }
    std::printf("DR2a one SET_STREAM_INFO: pending %ld cycles, acceptance to the "
                "window's done (%ld ticks of this bench's %ld-cycle ms)\n",
                fall - rise, (fall - rise) / MS_CYC, long(MS_CYC));
  }

  // D3R8: the deadline counts cycles without progress. 0x50's header READ
  // is granted by the device RS_TMO - 200 cycles late and the restore
  // completes with it applied; granted RS_TMO + 200 late, the restore
  // aborts at the deadline, cause 3, on defaults.
  void r8_the_deadline_boundary() {
    const std::array<long, 2> holds{RS_TMO - 200, RS_TMO + 200};
    for (const long hold : holds) {
      fresh();
      seed(0x50, d3_record(0x50, 1500000, 4));
      bool armed = false;
      const Boot b = boot_with(8 * RS_TMO, [&] {
        if (!armed && x.d->nvm_dev_req_o && x.d->nvm_dev_region_o == 0x50) {
          x.nv_gnt_hold = static_cast<int>(hold);
          armed = true;
        }
      });
      const bool late = hold > RS_TMO;
      CHECK(armed && b.done > b.release && x.d->restore_fail_o == late
              && x.d->rs_cause_o == (late ? 3u : 0u)
              && ((x.d->aecp_pt_offset_v_o & 1) != 0) == !late,
            "D3R8: a READ granted %ld cycles late: fail %u, cause %u",
            hold, unsigned(x.d->restore_fail_o), unsigned(x.d->rs_cause_o));
    }
  }

  // D3R9: a SET that waits through the restore wins by coming later. The
  // offset saved as A is restored, then the SET of B held since before the
  // walk runs, and the next flush saves B.
  void r9_a_held_set_follows_the_restore() {
    fresh();
    CHECK(x.boot_to_aecp() && ok(AEM_SET_STREAM_INFO, D3ServicePhase::pl_ptof(0, 1234567)),
          "D3R9: offset A set and answered");
    for (long c = 0; c < 2 * WINDOW; ++c) x.step();
    power_cycle();
    const uint16_t s = seq++;
    x.feed(aecp_frame(OWN_MAC, CTLR_MAC, 0, 0, EID, CTLR_EID, s,
                      AEM_SET_STREAM_INFO, D3ServicePhase::pl_ptof(0, 7654321)));
    x.idle(3000);                       // far longer than the SET takes when served
    const Boot b = boot(6 * RS_TMO);
    const auto f = x.wait_frame(x.q_aecp, 100, [s](const std::vector<uint8_t>& r) {
      return r.size() >= 38 && fv_u64(r, 34, 2) == s;
    });
    CHECK(b.done > b.release && !f.empty() && x.d->aecp_pt_offset_o.at(0) == 7654321
              && x.d->dbg_d3_applied_o == 1 && b.aecp_first > b.done,
          "D3R9: the held SET answered after the restore applied A, and its B "
          "is in force (row %u)", unsigned(x.d->aecp_pt_offset_o.at(0)));
    for (long c = 0; c < 2 * WINDOW; ++c) x.step();
    CHECK(std::equal(x.nv_mem[0x50].begin(), x.nv_mem[0x50].begin() + 12,
                     d3_record(0x50, 7654321, 4).begin()),
          "D3R9: the next flush saves B over the restored A");
  }

  void run() {
    r1_every_group_survives_a_power_cycle();
    r2_the_set_rules_and_the_frame_refuse();
    r3_the_set_rules_accept();
    r4_the_passes_agree_record_by_record();
    r5_a_pass_0_fault_applies_nothing();
    r6_blank_is_honest();
    r7_a_rule_fetch_fault_aborts();
    r8_the_deadline_boundary();
    r9_a_held_set_follows_the_restore();
    r10_a_late_burst_is_waited_out();
    r11_a_roll_back_keeps_the_restored_binding();
    r12_a_roll_back_that_cannot_prove_the_image_closes();
    r13_the_aggregate_bound();
  }
};
