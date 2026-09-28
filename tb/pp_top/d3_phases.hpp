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

  void run() {
    o1_dispatch_is_held_from_reset();
    o2_an_unprovable_image_ends_closed();
    o3_a_silent_descriptor_memory_ends_closed();
    o4_a_late_image_is_proven_by_its_locate();
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

  // D3S10: a record whose every WRITE fails is attempted 1 + RETRY_MAX_P = 3
  // times, then dropped with the sticky alarm: pending falls with it, no
  // fourth attempt follows, and a later successful write does not forgive
  // it. (The ruled DR2c spacing is graded where it lands.)
  void s10_bounded_attempts_then_the_sticky_alarm() {
    power_up();
    const size_t ops0 = x.nvm_ops.size();
    x.nv_err_region = 0x50;
    x.nv_err_writes = 1000;
    const bool a = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 7000001));
    const bool dropped = settle(3 * WINDOW);
    const int failed = writes(ops0, 0x50, 3);
    CHECK(a && dropped && failed == 3 && writes(ops0, 0x50) == 0
              && x.d->nvm_alarm_o && !x.d->d3_unflushed_o,
          "D3S10: %d failed attempts of 0x50, alarm %u, unflushed %u",
          failed, unsigned(x.d->nvm_alarm_o), unsigned(x.d->d3_unflushed_o));
    x.idle(static_cast<int>(2 * WINDOW));
    CHECK(writes(ops0, 0x50, 3) == 3, "D3S10: no fourth attempt (%d)",
          writes(ops0, 0x50, 3));
    x.nv_err_region = -1;
    const bool b = set_ok(AEM_SET_STREAM_INFO, pl_ptof(1, 7000002));
    CHECK(b && settle(3 * WINDOW) && held(0x51, 4) == d3_record(0x51, 7000002, 4)
              && x.d->nvm_alarm_o,
          "D3S10: a later successful write leaves the alarm set");
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
