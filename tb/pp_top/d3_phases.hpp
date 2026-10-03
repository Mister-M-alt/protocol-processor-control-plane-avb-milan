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

  // D3O7 (R391-2 S1, taken: the resident count is returned). The admission
  // counts an AECP record resident from its commit until its slot is
  // returned, by the engine or by the optional external drain. In CLOSED the
  // engine never takes one, so the drain (`aecp_txn_ready_i`, then
  // `aecp_rxs_free_i` with the popped record's slot) steals the held
  // command: the share is free again, so the next AECP command is held, not
  // dropped, and only the one after it is dropped and counted. A count that
  // never came back down would drop both.
  void o7_a_record_the_external_drain_returns_frees_the_share() {
    std::vector<uint8_t> bad = image;
    bad[0] ^= 0xFF;
    power_up(bad, false);
    const Boot b = boot_for(12000);
    x.feed(d3_read_entity_cmd(0xD390));
    x.idle(300);
    const bool held = x.d->dbg_aecp_head_o;
    const uint32_t before = x.snap(37);
    //! the head record names its RX slot while it waits in the queue
    const int slot = held ? int(x.d->aecp_txn_slot_o) : -1;
    x.d->aecp_txn_ready_i = 1;                    // the drain pops the head
    int pops = 0;
    for (int c = 0; c < 200 && x.d->dbg_aecp_head_o; ++c) {
      x.step();
      ++pops;
    }
    x.d->aecp_txn_ready_i = 0;
    const bool popped = !x.d->dbg_aecp_head_o && slot >= 0 && slot < int(RX_SLOTS);
    x.idle(20);
    x.d->aecp_rxs_free_i = 1;                     // ... and returns its slot
    x.d->aecp_rxs_free_slot_i = uint8_t(slot < 0 ? 0 : slot);
    x.step();
    x.d->aecp_rxs_free_i = 0;
    x.d->aecp_rxs_free_slot_i = 0;
    x.idle(20);
    x.feed(d3_read_entity_cmd(0xD391));
    x.idle(300);
    x.feed(d3_read_entity_cmd(0xD392));
    x.idle(300);
    const uint32_t after = x.snap(37);
    CHECK(b.closed > b.release && held && before == 0 && popped,
          "D3O7: in CLOSED the held AECP command is stolen by the external drain "
          "(slot %d, the head %s after %d clocks)", slot, popped ? "emptied" : "still held",
          pops);
    CHECK(after == before + 1 && x.d->dbg_aecp_head_o && x.q_aecp.empty()
              && x.d->dbg_d3_own_o,
          "D3O7: the returned slot frees the share: the next command held, the one "
          "after it dropped (%u counted), none answered", after - before);
  }

  void run() {
    o1_dispatch_is_held_from_reset();
    o2_an_unprovable_image_ends_closed();
    o3_a_silent_descriptor_memory_ends_closed();
    o4_a_late_image_is_proven_by_its_locate();
    o5_closed_admits_one_aecp_record();
    o6_a_slowed_restore_admits_one_aecp_record();
    o7_a_record_the_external_drain_returns_frees_the_share();
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

//! the D3 writer's records at this suite's shape (the wrap keeps the top's
//! defaults): one per persisted dynamic-state row, 1 + 1 + 1 + 8 + 8 + 8,
//! and one per name-table entry, DESC_NAME_ENTRIES_P's 32 (0x80 to 0x9F)
static constexpr int D3_SCALARS = 27;
static constexpr int D3_NAMES = 32;
static constexpr int D3_RECORDS = D3_SCALARS + D3_NAMES;

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
  // in all, each retry granted RETRY_BACKOFF_CYC_P cycles or more after the
  // failed attempt's error and within one relatch of it. The backoff is the
  // top's own derivation, never an override: 500 ms of the wrap's clock,
  // ceil(1,000,001 / 2) = 500,001 cycles (a product derivation of 5 ms, or a
  // count of the 500 bench ticks, is far short of it). While it waits the
  // writer holds neither dispatch nor the state bus: its ownership reads 0
  // in every cycle of it, and a READ_DESCRIPTOR sent into it is answered
  // byte-exact before the retry. Then the record is dropped with the
  // reset-sticky nvm_alarm_o: pending falls with it, no fourth attempt
  // follows, and a later successful write does not forgive it.
  static constexpr long BACKOFF = clk_ms(500);     //! the top's derivation
  //! a retry's grant follows its backoff by one relatch (ACQUIRE, the latch
  //! and its answer, the crc, the request): tens of cycles
  static constexpr long RELATCH = 64;
  void s10_bounded_attempts_then_the_sticky_alarm() {
    power_up();
    const size_t ops0 = x.nvm_ops.size();
    x.nv_err_region = 0x50;
    x.nv_err_writes = 1000;
    const bool a = set_ok(AEM_SET_STREAM_INFO, pl_ptof(0, 7000001));
    std::vector<long> grants;
    std::vector<long> errs;
    long owned = 0;
    long read_sent = -1;
    long read_answered = -1;
    std::vector<uint8_t> read_rsp;
    for (long c = 0; c < 3 * BACKOFF + 6 * WINDOW && !x.d->nvm_alarm_o; ++c) {
      const long at = long(x.t);
      x.step();
      if (x.d->dbg_d3_mgnt_o) grants.push_back(at + 1);
      if (x.d->dbg_d3_merr_o) errs.push_back(at + 1);
      const bool backoff = !errs.empty() && grants.size() == errs.size()
                           && long(x.t) - errs.back() < BACKOFF;
      if (backoff && x.d->dbg_d3_own_o) ++owned;
      if (backoff && read_sent < 0 && long(x.t) - errs.back() >= 1000) {
        x.q_aecp.clear();
        read_sent = long(x.t);
        x.feed(d3_read_entity_cmd(0xD510));
      }
      if (read_sent >= 0 && read_answered < 0 && !x.q_aecp.empty()) {
        read_answered = long(x.t);
        read_rsp = x.q_aecp.front();
      }
    }
    const bool dropped = settle(2 * WINDOW);
    const int failed = writes(ops0, 0x50, 3);
    CHECK(a && dropped && failed == 3 && grants.size() == 3 && writes(ops0, 0x50) == 0
              && x.d->nvm_alarm_o && !x.d->d3_unflushed_o,
          "D3S10 count: %d failed attempts of 0x50 (%zu grants), alarm %u, "
          "unflushed %u", failed, grants.size(), unsigned(x.d->nvm_alarm_o),
          unsigned(x.d->d3_unflushed_o));
    const long gap1 = grants.size() > 1 && !errs.empty() ? grants[1] - errs[0] : -1L;
    const long gap2 = grants.size() > 2 && errs.size() > 1 ? grants[2] - errs[1] : -1L;
    CHECK(gap1 >= BACKOFF && gap1 < BACKOFF + RELATCH && gap2 >= BACKOFF
              && gap2 < BACKOFF + RELATCH,
          "D3S10 timing: each retry granted after the derived %ld-cycle backoff "
          "and one relatch (%ld, %ld)", BACKOFF, gap1, gap2);
    CHECK(owned == 0 && read_sent >= 0 && read_answered > read_sent
              && grants.size() > 1 && read_answered < grants[1]
              && read_rsp == d3_read_entity_rsp(0xD510),
          "D3S10 backoff: %ld owned cycles inside the backoffs; a READ_DESCRIPTOR "
          "sent into the first is answered %ld cycles later, before the retry",
          owned, read_answered - read_sent);
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

// ---- the suite's image re-packed with one descriptor type rewritten -------
//! The suite's image re-packed by the same independent packer with one
//! change: every body of descriptor type `type` is rewritten by `edit`, and
//! its entry takes the new length. Every other descriptor and name is copied
//! out of the suite's image.
template <class Edit>
static std::vector<uint8_t> d3_image_repacked(const std::vector<uint8_t>& img,
                                              std::vector<ImgEnt> ents,
                                              uint16_t type, Edit edit) {
  auto rd16 = [&img](size_t o) { return uint16_t((img[o] << 8) | img[o + 1]); };
  std::vector<std::vector<uint8_t>> bodies;
  for (auto& e : ents) {
    const ImgEnt was = e;
    for (uint16_t k = 0; k < was.count; ++k) {
      const auto* at = &img[was.off + size_t(k) * was.stride];
      std::vector<uint8_t> body(at, at + was.len);
      if (was.type == type) {
        edit(body);
        e.len = uint16_t(body.size());
        e.stride = uint16_t((body.size() + 7) & ~size_t(7));
      }
      bodies.push_back(body);
    }
  }
  std::vector<std::string> names;
  const uint32_t nm_off = rd32(&img[16]);
  for (uint16_t i = 0; i < rd16(10); ++i) {
    const auto* at = &img[nm_off + 64u * i];
    names.emplace_back(at, std::find(at, at + 64, uint8_t(0)));
  }
  std::vector<const char*> name_ptrs;
  for (const auto& n : names) name_ptrs.push_back(n.c_str());
  return build_image(ents, bodies, name_ptrs, rd16(6));
}

// ---- an image whose AUDIO_UNIT lists more rates than the suite's two ----
//! The AUDIO_UNIT's sampling_rates list (count at 142, entries from 144,
//! IEEE 1722.1 Table 7-5) is `rates`, its descriptor 4 bytes longer a rate.
static std::vector<uint8_t> d3_image_with_rates(const std::vector<uint8_t>& img,
                                                std::vector<ImgEnt> ents,
                                                const std::vector<uint32_t>& rates) {
  return d3_image_repacked(img, std::move(ents), 0x0002, [&rates](std::vector<uint8_t>& body) {
    body.resize(144 + 4 * rates.size(), 0);
    putbe(&body[142], uint16_t(rates.size()), 2);
    for (size_t r = 0; r < rates.size(); ++r) putbe(&body[144 + 4 * r], rates[r], 4);
  });
}

// ---- an image whose CLOCK_DOMAIN lists more sources than the suite's three
//! The CLOCK_DOMAIN's clock_sources list (count at 74, entries from 76, IEEE
//! 1722.1-2021 Table 7-61) is the identity list 0..count-1 that 07 section
//! 3.1 L6 requires, its descriptor 2 bytes longer a source. The current
//! index at 70 stays the suite's 0, INTERNAL.
static std::vector<uint8_t> d3_image_with_sources(const std::vector<uint8_t>& img,
                                                  std::vector<ImgEnt> ents,
                                                  uint16_t count) {
  return d3_image_repacked(img, std::move(ents), 0x0024, [count](std::vector<uint8_t>& body) {
    body.resize(76 + 2 * size_t(count), 0);
    putbe(&body[74], count, 2);
    for (uint16_t s = 0; s < count; ++s) putbe(&body[76 + 2 * size_t(s)], s, 2);
  });
}

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

  //! the suite image's descriptor index, to re-pack it (D3R3b)
  const std::vector<ImgEnt> image_ents;

  D3RestorePhase(H& tally, const std::vector<uint8_t>& img,
                 const std::vector<ImgEnt>& ents)
      : h(tally), x(model.get()), image(img), image_ents(ents) {
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
    x.nv_gnt_at = -1;
    x.nv_rd_done_at = -1;
    x.nv_byte_at = -1;
    x.nv_hdr_every = 0;
    x.nv_byte_every = 0;
    x.nv_byte_wait = 0;
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
              && x.d->dbg_d3_refused_o == 0 && x.d->dbg_d3_blank_o == D3_RECORDS - 9,
          "D3R1: COMPLETE %ld cycles after the release, applied %u refused %u "
          "blank %u of %d", b.done - b.release, unsigned(x.d->dbg_d3_applied_o),
          unsigned(x.d->dbg_d3_refused_o), unsigned(x.d->dbg_d3_blank_o), D3_RECORDS);
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
              && d->dbg_d3_refused_o == 10 && d->dbg_d3_blank_o == D3_RECORDS - 11,
          "D3R2: COMPLETE, applied %u refused %u blank %u of %d",
          unsigned(d->dbg_d3_applied_o), unsigned(d->dbg_d3_refused_o),
          unsigned(d->dbg_d3_blank_o), D3_RECORDS);
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

  // D3R3b: the rate rule walks the AUDIO_UNIT's list as the SET program
  // does, entries 0 to count - 1 and at most eight of them, a lane (two
  // entries) at a time. Over an image listing ten rates, the eighth entry
  // (one lane past the first three) is accepted and the ninth, listed but
  // past the bound, is refused.
  void r3b_the_rate_walk_reaches_its_eighth_entry() {
    const std::vector<uint32_t> rates{32000, 44100, 48000, 88200, 96000,
                                      176400, 192000, 24000, 16000, 8000};
    const auto ten = d3_image_with_rates(image, image_ents, rates);
    const std::array<size_t, 2> picks{7, 8};
    for (const size_t k : picks) {
      x.dram = ten;
      x.erase_nvm();
      power_cycle();
      seed(0x02, d3_record(0x02, rates[k], 4));
      const Boot b = boot(6 * RS_TMO);
      const auto* d = x.d;
      const bool in_bound = k < 8;
      CHECK(b.done > b.release && !d->restore_fail_o
                && d->dbg_d3_applied_o == (in_bound ? 1u : 0u)
                && d->dbg_d3_refused_o == (in_bound ? 0u : 1u)
                && bool(d->dbg_dyn_rate_v_o) == in_bound
                && (!in_bound || d->dbg_dyn_rate_o == rates[k]),
            "D3R3b entry %zu of 10 (%u Hz): applied %u refused %u, rate %u valid %u",
            k, rates[k], unsigned(d->dbg_d3_applied_o), unsigned(d->dbg_d3_refused_o),
            unsigned(d->dbg_dyn_rate_o), unsigned(d->dbg_dyn_rate_v_o));
    }
    x.dram = image;
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
  // AECP running. With no descriptor debt owed, the roll-back still holds
  // both stores in reset for at least two cycles (parent D3 6.2).
  void r4_the_passes_agree_record_by_record() {
    fresh();
    seed(0x00, d3_record(0x00, 1, 2));
    seed(0x50, d3_record(0x50, 1500000, 4));
    const size_t ops0 = x.nvm_ops.size();
    bool erased = false;
    bool applied_before = false;
    long strobe = 0;
    long debt = 0;
    const Boot b = boot_with(6 * RS_TMO, [&] {
      if (!erased && reads_of(ops0, 0x50) == 2) {
        std::fill(x.nv_mem[0x50].begin(), x.nv_mem[0x50].begin() + 256, 0xFF);
        erased = true;
      }
      applied_before = applied_before || x.d->dbg_dyn_cfg_v_o;
      strobe += x.d->dbg_d3_rb_rst_o ? 1 : 0;
      debt += (x.d->dbg_d3_rb_rst_o && x.d->desc_mem_debt_o) ? 1 : 0;
    });
    CHECK(erased && applied_before && b.done > b.release && x.d->restore_fail_o
              && x.d->rs_cause_o == 5 && x.d->restore_rb_o && !x.d->restore_closed_o
              && rows_cleared() && x.d->dbg_img_valid_o && !x.d->dbg_d3_own_o
              && !x.d->restore_blank_o,
          "D3R4: the record whole in pass 0 and unframed in pass 1 aborts, cause "
          "%u, rolled back %u: every row at its default after the applied "
          "configuration", unsigned(x.d->rs_cause_o), unsigned(x.d->restore_rb_o));
    CHECK(strobe >= 2 && debt == 0,
          "D3R4 strobe: with no debt owed the roll-back holds both stores in "
          "reset %ld cycles, at least two", strobe);
  }

  // D3R4b: the other direction of the agreement. 0x50 is unframed (erased)
  // when pass 0 reads it and framed at rest before pass 1 does: a record
  // blank in one pass and whole in the other aborts too (cause 5), is never
  // applied, and the configuration applied before it is rolled back.
  void r4b_blank_then_whole_disagrees() {
    fresh();
    seed(0x00, d3_record(0x00, 1, 2));
    const size_t ops0 = x.nvm_ops.size();
    bool planted = false;
    bool applied_before = false;
    const Boot b = boot_with(6 * RS_TMO, [&] {
      if (!planted && reads_of(ops0, 0x50) == 1 && x.nv_st == H::NvState::NV_IDLE) {
        seed(0x50, d3_record(0x50, 1500000, 4));
        planted = true;
      }
      applied_before = applied_before || x.d->dbg_dyn_cfg_v_o;
    });
    CHECK(planted && applied_before && b.done > b.release && x.d->restore_fail_o
              && x.d->rs_cause_o == 5 && x.d->restore_rb_o && !x.d->restore_closed_o
              && rows_cleared() && (x.d->aecp_pt_offset_v_o & 1) == 0,
          "D3R4b: a record blank in pass 0 and whole in pass 1 aborts, cause %u, "
          "rolled back %u, the offset not applied (valid 0x%02x)",
          unsigned(x.d->rs_cause_o), unsigned(x.d->restore_rb_o),
          unsigned(x.d->aecp_pt_offset_v_o));
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

  // D3R5b: pass 1's header READ of 0x50 is never answered. The deadline
  // aborts pass 1 (cause 3) and rolls back to DEFAULTS, abandoning the
  // granted READ to the arbiter's drain; once the device ends it, a later
  // SET persists and nothing stays unflushed.
  void r5b_a_pass_1_read_held_past_its_deadline_is_drained() {
    const Boot b = faulted_boot(3, 0, true);
    const auto* d = x.d;
    CHECK(b.done > b.release && d->restore_fail_o && d->rs_cause_o == 3
              && d->restore_rb_o && !d->restore_closed_o && rows_cleared()
              && !d->dbg_d3_own_o,
          "D3R5b: pass 1's silent READ aborts at the deadline: DEFAULTS, cause %u, "
          "rolled back %u", unsigned(d->rs_cause_o), unsigned(d->restore_rb_o));
    const bool drained_busy = x.nv_st == H::NvState::NV_READ && x.d->dbg_nvm_drain_o;
    x.nv_rd_fault = false;
    x.idle(200);
    const size_t ops0 = x.nvm_ops.size();
    const bool set = ok(AEM_SET_STREAM_INFO, D3ServicePhase::pl_ptof(0, 4545454));
    for (long c = 0; c < 2 * WINDOW; ++c) x.step();
    int writes = 0;
    for (size_t i = ops0; i < x.nvm_ops.size(); i++)
      writes += x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == 0x50;
    CHECK(drained_busy && set && writes == 1 && !x.d->d3_unflushed_o
              && std::equal(x.nv_mem[0x50].begin(), x.nv_mem[0x50].begin() + 12,
                            d3_record(0x50, 4545454, 4).begin()),
          "D3R5b: once the device ends the drained pass-1 READ a later SET "
          "persists (%d WRITEs, unflushed %u)", writes, unsigned(x.d->d3_unflushed_o));
  }

  // D3R6: the restore meets an erased device (V10) and an unframed record:
  // both are that record's default, never a failure. Erased everywhere is a
  // blank restore; one saved record lost to a device error never is (H8).
  void r6_blank_is_honest() {
    fresh();
    Boot b = boot(6 * RS_TMO);
    CHECK(b.done > b.release && !x.d->restore_fail_o && x.d->restore_blank_o
              && x.d->dbg_d3_blank_o == D3_RECORDS,
          "D3R6: an erased device restores blank, not failed (%u of %d blank)",
          unsigned(x.d->dbg_d3_blank_o), D3_RECORDS);
    fresh();
    std::vector<uint8_t> junk(12, 0x5A);           // no F07.8 magic
    seed(0x50, junk);
    b = boot(6 * RS_TMO);
    CHECK(b.done > b.release && !x.d->restore_fail_o && x.d->restore_blank_o
              && x.d->dbg_d3_blank_o == D3_RECORDS,
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
    //! over an erased device every READ is one grant: the binding walk's eight
    //! and each pass's D3_RECORDS. Spaced so pass 0 ends before the bound
    //! and the bound falls midway through pass 1, still inside the per-wait
    //! deadline
    static_assert(AGG / (8 + D3_RECORDS + D3_RECORDS / 2) < RS_TMO,
                  "D3R13's grant spacing must stay inside the per-wait deadline");
    fresh();
    x.nv_gnt_every = static_cast<int>(AGG / (8 + D3_RECORDS + D3_RECORDS / 2));
    const Boot e = boot(AGG + 4 * RS_TMO);
    x.nv_gnt_every = 0;
    CHECK(e.done + 1 > AGG && e.done + 1 <= AGG + RS_TMO && d->restore_fail_o
              && d->rs_cause_o == 3 && d->restore_rb_o && !d->restore_closed_o
              && rows_cleared() && !d->dbg_d3_own_o,
          "D3R13 pass 1: rolled back to DEFAULTS at clock %ld, the bound %ld, "
          "cause %u, rolled back %u", e.done + 1, AGG, unsigned(d->rs_cause_o),
          unsigned(d->restore_rb_o));
  }

  //! the eight sinks' binding records, each saved bound and started, so the
  //! binding walk reads a header and a 20-byte payload per sink
  void seed_every_binding() {
    for (uint8_t k = 0; k < 8; ++k) {
      auto b = binding_record(0x00B0B0B0B0B0D400ULL + k, uint16_t(0x0D40 + k), CTLR_EID);
      b[3] = uint8_t(0x20 + k);
      seed(uint8_t(0x20 + k), reframe(b));
    }
  }
  //! READs of D3 records (every region but the binding block) from `from`
  int d3_reads_of(size_t from) const {
    int n = 0;
    for (size_t i = from; i < x.nvm_ops.size(); i++) {
      const auto& o = x.nvm_ops[i];
      n += (o.op == 0 || o.op == 4) && (o.region < 0x20 || o.region > 0x27);
    }
    return n;
  }

  // D3R14 (DR3a as clarified on processor issue #131: an aggregate expiry
  // never closes a provable image). The device answers each READ byte just
  // inside the per-wait deadline, slow per byte (the header probe's eight
  // bytes RS_TMO / 8 - 400 cycles apart, each payload byte RS_TMO - 1000:
  // the reviewers' probe D1), so with every sink's binding saved the
  // binding walk alone outlasts the bound while no wait trips its own
  // deadline. At the bound the binding walk takes its own per-wait path: it
  // fails whole (restore_cause_o 3), abandons its READ to the drain and
  // releases the listener. The D3 walk then proves the image with no record
  // READ and ends DEFAULTS, cause 3, within one per-wait deadline of the
  // bound, with the image valid, the enable released to ADP (graded on the
  // engine's input, as D3R1) and AECP released: a READ_DESCRIPTOR is
  // answered byte-exact, and once the device
  // ends the drained READ a later SET persists. With an image the store
  // cannot validate, the same boot ends CLOSED, cause 7, only after the
  // listener's release: the image could not be proven.
  void r14_an_aggregate_bound_before_the_image_proof() {
    struct Arm { bool provable; const char* what; };
    const std::array<Arm, 2> arms{{{true, "image valid"}, {false, "image refused"}}};
    for (const auto& a : arms) {
      x.dram = image;
      if (!a.provable) x.dram[0] ^= 0xFF;          // the store refuses the magic
      x.erase_nvm();
      power_cycle();
      seed_every_record();
      seed_every_binding();
      x.d->entity_enable_i = 1;
      x.d->link_up_i = 1;
      x.nv_hdr_every = static_cast<int>(RS_TMO / 8 - 400);
      x.nv_byte_every = static_cast<int>(RS_TMO - 1000);
      const size_t ops0 = x.nvm_ops.size();
      long c = 0;
      long bound = -1;                             //! the count first reads AGG - 1
      long bind_wait = 0;
      long d3_wait = 0;
      const Boot b = boot_with(AGG + 2 * RS_TMO, [&] {
        if (bound < 0 && x.d->dbg_d3_agg_o == uint32_t(AGG - 1)) bound = c;
        bind_wait = std::max(bind_wait, long(x.d->dbg_bind_wd_o));
        d3_wait = std::max(d3_wait, long(x.d->dbg_d3_wd_o));
        ++c;
      });
      const auto* d = x.d;
      const int d3_reads = d3_reads_of(ops0);
      const bool draining = d->dbg_nvm_drain_o;
      //! clocks counted as D3R13 does, the one that took restore_go_i the
      //! first: the count reads AGG - 1 in the AGG-th, the bound's own clock,
      //! and a level first seen at loop index c was registered by clock c + 1
      const long bound_clk = bound + 2;
      const long term_clk = (a.provable ? b.done : b.closed) + 1;
      CHECK(bound_clk == AGG && bind_wait < RS_TMO && b.release + 1 > bound_clk
                && d->restore_fail_o && d->restore_cause_o == 3
                && (d->acmp_bound_o & 0xFF) == 0 && draining && d3_reads == 0,
            "D3R14 %s: the binding walk (longest wait %ld of %ld) fails whole at the "
            "bound's clock %ld (cause %u, bound sinks 0x%02x), its READ drained %u, "
            "the listener released by clock %ld; %d D3 READs", a.what, bind_wait,
            RS_TMO, bound_clk, unsigned(d->restore_cause_o),
            unsigned(d->acmp_bound_o & 0xFF), unsigned(draining), b.release + 1, d3_reads);
      const bool in_time = term_clk > bound_clk && term_clk - bound_clk < RS_TMO;
      if (a.provable) {
        CHECK(b.done >= 0 && b.closed < 0 && in_time && d3_wait == 0 && d->restore_done_o
                  && d->rs_cause_o == 3 && !d->restore_rb_o && !d->restore_closed_o
                  && d->dbg_img_valid_o && d->dbg_d3_applied_o == 0 && rows_cleared()
                  && !d->dbg_d3_own_o && d->dbg_adp_enable_o && b.enable_early == 0
                  && (b.adp_first < 0 || b.adp_first > b.done),
              "D3R14 image valid: DEFAULTS by clock %ld, the D3 walk's longest wait %ld "
              "(no record READ requested), cause %u, rolled back %u, image valid %u, "
              "AECP owned %u, ADP enabled %u (%ld early enable cycles)", term_clk, d3_wait,
              unsigned(d->rs_cause_o), unsigned(d->restore_rb_o),
              unsigned(d->dbg_img_valid_o), unsigned(d->dbg_d3_own_o),
              unsigned(d->dbg_adp_enable_o), b.enable_early);
        x.q_aecp.clear();
        x.feed(d3_read_entity_cmd(0xD314));
        const auto got = x.wait_any(x.q_aecp, 50);
        x.nv_hdr_every = 0;
        x.nv_byte_every = 0;
        for (long k = 0; k < RS_TMO; ++k) x.step();
        const size_t ops1 = x.nvm_ops.size();
        const bool set = ok(AEM_SET_STREAM_INFO, D3ServicePhase::pl_ptof(0, 4141414));
        for (long k = 0; k < 2 * WINDOW; ++k) x.step();
        int writes = 0;
        for (size_t i = ops1; i < x.nvm_ops.size(); i++)
          writes += x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == 0x50;
        CHECK(got == d3_read_entity_rsp(0xD314) && set && writes == 1
                  && std::equal(x.nv_mem[0x50].begin(), x.nv_mem[0x50].begin() + 12,
                                d3_record(0x50, 4141414, 4).begin()),
              "D3R14 image valid: AECP answers a READ_DESCRIPTOR byte-exact, and once "
              "the device ends the drained binding READ a later SET persists (%d WRITEs)",
              writes);
      } else {
        CHECK(b.closed >= 0 && b.done < 0 && in_time && d->restore_closed_o
                  && d->rs_cause_o == 7 && !d->restore_done_o && d->dbg_d3_own_o
                  && !d->dbg_adp_enable_o,
              "D3R14 image refused: CLOSED by clock %ld, cause %u, AECP owned %u, ADP "
              "enabled %u", term_clk, unsigned(d->rs_cause_o), unsigned(d->dbg_d3_own_o),
              unsigned(d->dbg_adp_enable_o));
      }
      x.nv_hdr_every = 0;
      x.nv_byte_every = 0;
      x.d->entity_enable_i = 0;
      x.d->link_up_i = 0;
    }
    x.dram = image;
  }

  // ---- steering the device's grants onto a chosen clock --------------------
  //! Some cases must land one event on one exact clock about a million
  //! clocks after restore_go_i: the aggregate bound's own clock, or a set
  //! distance before it. The device withholds every grant, and steer(),
  //! called after every clock, releases each: the `target`-th command the
  //! device sees in the boot is granted on exactly harness cycle `at`, and
  //! every one before it after an equal share of the time left, so each
  //! single wait stays inside the per-wait deadline and none trips it. Every
  //! command after the target stays withheld until the case releases it.
  struct Steer {
    int target;               //! the command, 1-based, in the device's order
    long at;                  //! the harness cycle its grant must land on
    int seen = 0;             //! commands the device has seen
    long granted = -1;        //! the harness cycle the target was granted
    int region = -1;          //! the target command's region and offset
    int off = -1;
  };
  void steer(Steer& s) {
    const auto* d = x.d;
    if (s.granted < 0 && s.seen == s.target && x.nv_gnt_at < 0
        && x.nv_st != H::NvState::NV_IDLE)
      s.granted = x.nv_gnt_t;
    if (x.nv_st != H::NvState::NV_IDLE || !d->nvm_dev_req_o || x.nv_gnt_at >= 0
        || s.seen >= s.target)
      return;
    ++s.seen;
    const long now = long(x.t);
    if (s.seen == s.target) {
      s.region = int(d->nvm_dev_region_o);
      s.off = int(d->nvm_dev_offset_o);
      x.nv_gnt_at = s.at;
      return;
    }
    const long share = (s.at - now) / (s.target - s.seen + 1);
    x.nv_gnt_at = now + std::clamp(share, 1L, RS_TMO - 1000);
  }
  //! the device's harness cycle on which the aggregate count reads AGG - 1
  //! (the bound's own clock), from its reading `agg` now
  long bound_t(uint32_t agg) const { return long(x.t) + (AGG - 1 - long(agg)); }
  //! the device serves every command again at once
  void steer_release() {
    x.nv_gnt_every = 0;
    x.nv_gnt_hold = 0;
    x.nv_gnt_at = -1;
    x.nv_gnt_seen = false;
  }

  // D3R15 (R390-2 F3: the aggregate spans the roll-back). A pass-1 fault
  // that starts a roll-back shortly before the aggregate bound, with every
  // wait inside its deadline (the device's grants steered): the bound falls
  // inside the roll-back and ends it CLOSED on the bound's own clock, with
  // the pass-1 fault's cause (parent D3 section 6.3, as the clarification of
  // DR3a on issue #131 keeps it: a roll-back that cannot prove the image
  // again by the deadline). Two arms: the roll-back still waiting out the
  // descriptor memory's debt (the rate rule's AUDIO_UNIT fetch answers
  // 16,000 cycles late, so its watchdog aborts pass 1, cause 6, and the late
  // burst is owed across the bound), and its re-LOCATE still waiting for the
  // store's walk of the image (a DEVICE error on pass 1's header READ of
  // 0x02 100 cycles before the bound, cause 2). A counter that paused in the
  // roll-back would let both end DEFAULTS after the bound instead.
  void r15_the_aggregate_bound_inside_a_roll_back() {
    struct Arm { bool debt; int target; long before; unsigned cause; const char* what; };
    //! every scalar record saved: the binding walk's eight probes, pass 0's
    //! header and payload READs of all 27 scalar records and the header READ
    //! of each erased name record, then pass 1's of 0x00 and 0x02
    constexpr int PASS1 = 8 + 2 * D3_SCALARS + D3_NAMES;
    const std::array<Arm, 2> arms{{
        {true, PASS1 + 4, 10000, 6, "debt wait"},
        {false, PASS1 + 3, 100, 2, "re-LOCATE"}}};
    for (const auto& a : arms) {
      fresh();
      seed_every_record();
      if (a.debt) {
        x.dram_late_at = au_addr;
        x.dram_late_cycles = 16000;
      } else {
        x.nv_rd_region = 0x02;
        x.nv_rd_nth = 3;                           // pass 1's header READ of 0x02
        x.nv_rd_after = 0;
        x.nv_rd_silent = false;
        x.nv_rd_seen = 0;
      }
      x.nv_gnt_every = 1 << 30;                    // steer() grants each command
      Steer s{a.target, -1};
      long bound = -1;
      long closed_t = -1;
      long done_t = -1;
      bool strobe = false;
      bool debt = false;
      bool img = true;
      bool rolling = false;
      (void)boot_with(AGG + 4 * RS_TMO, [&] {
        const auto* d = x.d;
        if (s.at < 0) s.at = bound_t(d->dbg_d3_agg_o) - a.before;
        steer(s);
        if (bound < 0 && d->dbg_d3_agg_o == uint32_t(AGG - 1)) {
          bound = long(x.t);
          strobe = d->dbg_d3_rb_rst_o;
          debt = d->desc_mem_debt_o;
          img = d->dbg_img_valid_o;
          rolling = d->dbg_d3_fail_o && !d->dbg_d3_done_o && !d->dbg_d3_closed_o;
        }
        if (closed_t < 0 && d->restore_closed_o) closed_t = long(x.t);
        if (done_t < 0 && d->dbg_d3_done_o) done_t = long(x.t);
      });
      steer_release();
      x.dram_late_at = 0;
      x.nv_rd_region = -1;
      const auto* d = x.d;
      const bool premise = s.granted == s.at && s.region == 0x02
                           && s.off == (a.debt ? 8 : 0) && rolling
                           && (a.debt ? (strobe && debt) : (!strobe && !img));
      CHECK(premise,
            "D3R15 %s: the pass-1 fault's READ (region 0x%02x offset %d) granted on "
            "its steered cycle %u; at the bound the roll-back was under way (strobe "
            "%u, debt %u, image valid %u)", a.what, unsigned(s.region), s.off,
            unsigned(s.granted == s.at), unsigned(strobe), unsigned(debt), unsigned(img));
      //! the bound's own clock ends with the abort, so CLOSED is seen on the
      //! next one
      CHECK(bound >= 0 && closed_t == bound + 1 && done_t < 0 && d->restore_closed_o
                && d->rs_cause_o == a.cause && !d->restore_done_o && !d->restore_rb_o
                && d->dbg_d3_own_o,
            "D3R15 %s: CLOSED %ld clocks after the bound's own (want 0), cause %u, D3 "
            "done %ld clocks after the bound, rolled back %u, AECP owned %u", a.what,
            (closed_t < 0 || bound < 0) ? -1 : closed_t - bound - 1, unsigned(d->rs_cause_o),
            (done_t < 0 || bound < 0) ? -1 : done_t - bound, unsigned(d->restore_rb_o),
            unsigned(d->dbg_d3_own_o));
    }
    x.dram = image;
  }

  // D3R16 (R391-2 F1: the aggregate is inert after the terminal). Three
  // restores reach their terminal long before the bound: COMPLETE with
  // every record saved; DEFAULTS rolled back (the rate rule's AUDIO_UNIT
  // fetch errs, cause 6); CLOSED after a roll-back whose re-LOCATE meets a
  // silent memory (cause 2). A presentation offset is then SET where AECP
  // runs. Observed two per-wait deadlines past the bound, each keeps its
  // verdicts, its ownership and its rows (the SET value included), and no
  // roll-back strobe runs after the terminal. A count that kept running
  // would roll the COMPLETE restore back at the bound, reset the rows the
  // DEFAULTS one served, and turn the CLOSED one into DEFAULTS.
  void r16_the_aggregate_is_inert_after_the_terminal() {
    enum class End { COMPLETE, DEFAULTS, CLOSED };
    struct Arm { End end; const char* what; };
    const std::array<Arm, 3> arms{{{End::COMPLETE, "COMPLETE"}, {End::DEFAULTS, "DEFAULTS"},
                                   {End::CLOSED, "CLOSED"}}};
    for (const auto& a : arms) {
      fresh();
      if (a.end == End::COMPLETE) seed_every_record();
      if (a.end == End::DEFAULTS) {
        seed(0x02, d3_record(0x02, 96000, 4));
        x.dram_err_at = au_addr;
      }
      if (a.end == End::CLOSED) {
        seed(0x00, d3_record(0x00, 1, 2));
        seed(0x50, d3_record(0x50, 1500000, 4));
        x.nv_rd_region = 0x50;
        x.nv_rd_nth = 3;
        x.nv_rd_after = 0;
        x.nv_rd_silent = false;
        x.nv_rd_seen = 0;
      }
      long bound = -1;
      const Boot b = boot_with(8 * RS_TMO, [&] {
        if (bound < 0) bound = bound_t(x.d->dbg_d3_agg_o);
        if (a.end == End::CLOSED && x.d->dbg_d3_rb_rst_o) x.dram_silent = true;
      });
      x.dram_silent = false;
      x.dram_err_at = 0;
      x.nv_rd_region = -1;
      const bool closed = a.end == End::CLOSED;
      const bool set = closed || ok(AEM_SET_STREAM_INFO, D3ServicePhase::pl_ptof(0, 4040404));
      const auto* d = x.d;
      const bool done0 = d->restore_done_o;
      const bool fail0 = d->restore_fail_o;
      const bool rb0 = d->restore_rb_o;
      const bool closed0 = d->restore_closed_o;
      const unsigned cause0 = d->rs_cause_o;
      long strobes = 0;
      long own = 0;
      long cycles = 0;
      const long until = bound + 2 * RS_TMO;
      while (long(x.t) < until) {
        x.step();
        ++cycles;
        strobes += x.d->dbg_d3_rb_rst_o ? 1 : 0;
        own += x.d->dbg_d3_own_o ? 1 : 0;
      }
      const bool rows = closed ? true
                               : (x.d->aecp_pt_offset_o.at(0) == 4040404
                                  && (x.d->aecp_pt_offset_v_o & 1) != 0
                                  && (a.end != End::COMPLETE
                                      || (x.d->dbg_dyn_cfg_v_o && x.d->dbg_dyn_rate_v_o)));
      const bool reached = (closed ? b.closed : b.done) >= 0 && bound > 0
                           && long(x.t) > bound + RS_TMO;
      CHECK(reached && set && strobes == 0 && rows && d->restore_done_o == done0
                && d->restore_fail_o == fail0 && d->restore_rb_o == rb0
                && d->restore_closed_o == closed0 && d->rs_cause_o == cause0
                && done0 == !closed && closed0 == closed && bool(d->dbg_d3_own_o) == closed
                && (!closed || own == cycles),
            "D3R16 %s: two per-wait deadlines past the bound the verdicts hold (done %u "
            "fail %u rolled back %u closed %u cause %u), %ld roll-back strobe cycles after "
            "the terminal, the rows %s", a.what, unsigned(d->restore_done_o),
            unsigned(d->restore_fail_o), unsigned(d->restore_rb_o),
            unsigned(d->restore_closed_o), unsigned(d->rs_cause_o), strobes,
            rows ? "kept" : "lost");
    }
    x.dram = image;
  }

  // D3R17 (R391-2 F1: the aggregate never fires with an event in hand).
  // With every record saved and the device's grants steered, the writer's
  // arbiter grant of a pass-0 READ lands on the bound's own clock: the
  // aggregate waits for the next clock without an event (the device now
  // withholding that READ), ends DEFAULTS, cause 3, and abandons the READ it
  // owns to the drain; once the device ends it, a later SET persists. Firing
  // on the grant itself would leave that READ granted to a writer that has
  // stopped reading and no drain behind it: the port stays owned and no
  // later change is ever written.
  void r17_the_aggregate_waits_for_a_clock_without_an_event() {
    fresh();
    seed_every_record();
    x.nv_gnt_every = 1 << 30;                      // steer() grants each command
    //! the binding walk's eight probes, then pass 0's header and payload
    //! READs: command 60 is 0x56's payload READ, and the writer's grant of
    //! 0x57's header READ follows it by the latency `lat` measured on the
    //! earlier 4-byte payload READs of this boot
    Steer s{60, -1};
    long lat = -1;
    long gnt_t = -1;
    bool gnt_d3 = false;
    long bound = -1;
    bool in_hand = false;
    long done_t = -1;
    (void)boot_with(AGG + 4 * RS_TMO, [&] {
      const auto* d = x.d;
      if (x.nv_gnt_t != gnt_t) {                   // the device granted a command
        gnt_t = x.nv_gnt_t;
        gnt_d3 = x.nv_cur.op == 0 && x.nv_cur.len == 4
                 && (x.nv_cur.region < 0x20 || x.nv_cur.region > 0x27);
      }
      if (d->dbg_d3_mgnt_o && gnt_d3 && lat < 0) lat = long(x.t) - gnt_t;
      if (d->dbg_d3_mgnt_o) gnt_d3 = false;
      //! paced from the start on an estimate, exact once the latency is known
      //! (long before the target command)
      if (s.seen < s.target) s.at = bound_t(d->dbg_d3_agg_o) - (lat >= 0 ? lat : 40);
      steer(s);
      if (bound < 0 && d->dbg_d3_agg_o == uint32_t(AGG - 1)) {
        bound = long(x.t);
        in_hand = d->dbg_d3_mgnt_o && !d->dbg_d3_done_o;
      }
      if (done_t < 0 && d->dbg_d3_done_o) done_t = long(x.t);
    });
    const auto* d = x.d;
    const bool draining = d->dbg_nvm_drain_o;
    CHECK(s.granted == s.at && s.region == 0x56 && s.off == 8 && in_hand && done_t > bound
              && done_t - bound < RS_TMO && d->restore_fail_o && d->rs_cause_o == 3
              && !d->restore_rb_o && !d->restore_closed_o && rows_cleared() && draining,
          "D3R17: the writer's grant lands on the bound's own clock (%u, 0x%02x's READ "
          "steered %u, latency %ld); DEFAULTS %ld clocks later, cause %u, the READ in "
          "hand drained %u", unsigned(in_hand), unsigned(s.region),
          unsigned(s.granted == s.at), lat, done_t - bound, unsigned(d->rs_cause_o),
          unsigned(draining));
    steer_release();
    for (long c = 0; c < RS_TMO; ++c) x.step();
    const size_t ops0 = x.nvm_ops.size();
    const bool set = ok(AEM_SET_STREAM_INFO, D3ServicePhase::pl_ptof(0, 1717171));
    for (long c = 0; c < 2 * WINDOW; ++c) x.step();
    int writes = 0;
    for (size_t i = ops0; i < x.nvm_ops.size(); i++)
      writes += x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == 0x50;
    CHECK(set && writes == 1 && !x.d->d3_unflushed_o
              && std::equal(x.nv_mem[0x50].begin(), x.nv_mem[0x50].begin() + 12,
                            d3_record(0x50, 1717171, 4).begin()),
          "D3R17: once the device ends the READ granted on the bound's clock a later "
          "SET persists (%d WRITEs, unflushed %u)", writes, unsigned(x.d->d3_unflushed_o));
  }

  // D3R18 (R390-3 F1: an abort in the arbiter's issue cycle). The binding
  // walk registers its READ strobe, so the strobe is out in the walk's
  // first H_RS_STREAM clock, where no byte can be in hand yet. When agg_o
  // first reads 1 on exactly that clock, the walk abandons the READ in the
  // cycle the arbiter issues it, the arbiter still unowned. Every sink's
  // binding and every record saved, the headers at once and each payload
  // byte G clocks apart, so the fifth binding READ would start about 12,000
  // clocks before the bound; the done of the fourth record's payload READ
  // is held (nv_rd_done_at) so that the fifth strobe lands on agg_o's first
  // clock, from the done-to-strobe lag measured on the earlier records of
  // the same boot. The arbiter drains that READ from the next clock until
  // the device ends it, the walk fails whole (cause 3) and the restore ends
  // DEFAULTS; once the device is fast again the drain ends, the port is
  // idle and a later SET persists. An arbiter that arms the drain only for
  // a READ it already owns loses this abort: the port waits on the walk's
  // rready for ever and no later change is written.
  void r18_an_abort_in_the_arbiters_issue_cycle() {
    fresh();
    seed_every_record();
    seed_every_binding();
    x.d->entity_enable_i = 1;
    x.d->link_up_i = 1;
    constexpr int K = 4;                           //! the strobe to land, 0-based
    //! K records' 20-byte payloads fill the bound less about 12,000 clocks
    const int G = static_cast<int>((AGG - 12000) / (20 * K));
    x.nv_hdr_every = 0;
    x.nv_byte_every = G;
    std::vector<long> strobes;                     //! clocks of the binding READ strobes
    std::vector<long> dones;                       //! device dones of binding payloads
    size_t ops = x.nvm_ops.size();
    long lag = -1;                                 //! a payload's done to the next strobe
    long hold_at = -1;
    long first = -1;                               //! agg_o's first clock
    bool strobe_at = false;
    bool abort_at = false;
    unsigned own_at = 9;
    int strobe_k = -1;
    bool drain_next = false;
    unsigned own_next = 9;
    long bind_wait = 0;
    const Boot b = boot_with(AGG + 2 * RS_TMO, [&] {
      const auto* d = x.d;
      const long now = long(x.t);
      while (ops < x.nvm_ops.size()) {
        const auto& o = x.nvm_ops[ops++];
        if (o.op == 0 && o.region >= 0x20 && o.region < 0x28 && o.off != 0)
          dones.push_back(now - 1);                // the device's clock of it
      }
      if (d->dbg_bind_req_o) strobes.push_back(now);
      if (lag < 0 && strobes.size() == 3 && dones.size() == 2) lag = strobes[2] - dones[1];
      if (hold_at < 0 && lag > 0 && x.nv_st == H::NvState::NV_READ
          && x.nv_cur.region == 0x20 + K - 1 && x.nv_cur.off != 0) {
        hold_at = bound_t(d->dbg_d3_agg_o) + 1 - lag;
        x.nv_rd_done_at = hold_at;
      }
      if (first >= 0 && now == first + 1) {
        drain_next = d->dbg_nvm_drain_o;
        own_next = d->dbg_nvm_own_o;
      }
      if (first < 0 && d->dbg_d3_agg_fired_o) {
        first = now;
        strobe_at = d->dbg_bind_req_o;
        abort_at = d->dbg_bind_abort_o;
        own_at = d->dbg_nvm_own_o;
        strobe_k = int(strobes.size()) - 1;
      }
      bind_wait = std::max(bind_wait, long(d->dbg_bind_wd_o));
    });
    x.nv_rd_done_at = -1;
    const auto* d = x.d;
    CHECK(first >= 0 && strobe_at && abort_at && own_at == 0 && strobe_k == K && lag > 0
              && hold_at > 0 && bind_wait < RS_TMO,
          "D3R18: binding READ strobe %d on agg_o's first clock %ld (strobe %u, abort %u, "
          "arbiter owner %u), the done before it held to %ld (lag %ld, payload bytes %d "
          "apart, longest wait %ld of %ld)", strobe_k + 1, first, unsigned(strobe_at),
          unsigned(abort_at), own_at, hold_at, lag, G, bind_wait, RS_TMO);
    CHECK(drain_next && own_next == 1 && d->dbg_nvm_drain_o && d->restore_fail_o
              && d->restore_cause_o == 3 && b.done >= 0 && b.closed < 0 && d->restore_done_o
              && d->rs_cause_o == 3 && !d->restore_closed_o && !d->dbg_d3_own_o,
          "D3R18: the READ abandoned in its issue cycle is drained from the next clock "
          "(%u, owner %u), the binding walk fails whole (cause %u) and the restore ends "
          "DEFAULTS (cause %u, closed %u)", unsigned(drain_next), own_next,
          unsigned(d->restore_cause_o), unsigned(d->rs_cause_o),
          unsigned(d->restore_closed_o));
    x.nv_hdr_every = 0;
    x.nv_byte_every = 0;
    for (long k = 0; k < RS_TMO; ++k) x.step();
    const bool idle = !x.d->dbg_nvm_drain_o && !x.d->dbg_nvm_busy_o && x.d->dbg_nvm_own_o == 0;
    const size_t ops1 = x.nvm_ops.size();
    const bool set = ok(AEM_SET_STREAM_INFO, D3ServicePhase::pl_ptof(0, 1818181));
    long busy = 0;
    for (long k = 0; k < 2 * WINDOW; ++k) {
      x.step();
      busy += x.d->dbg_nvm_busy_o ? 1 : 0;
    }
    int writes = 0;
    for (size_t i = ops1; i < x.nvm_ops.size(); i++)
      writes += x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == 0x50;
    CHECK(idle && set && writes == 1 && busy < 1000 && !x.d->dbg_nvm_busy_o
              && x.d->dbg_nvm_own_o == 0 && !x.d->d3_unflushed_o
              && std::equal(x.nv_mem[0x50].begin(), x.nv_mem[0x50].begin() + 12,
                            d3_record(0x50, 1818181, 4).begin()),
          "D3R18: once the device ends the drained READ the port is idle (%u), and a later "
          "SET persists (%d WRITEs, the port busy %ld of %ld clocks, unflushed %u)",
          unsigned(idle), writes, busy, 2 * WINDOW, unsigned(x.d->d3_unflushed_o));
    x.d->entity_enable_i = 0;
    x.d->link_up_i = 0;
  }

  // ---- the aggregate's pre-proof variants (R390-3 F2, R391-3 F2) ----------
  //! power up for a pre-proof case: every record and every sink's binding
  //! saved; the image valid at reset, or (`late`) absent when the store
  //! walks at reset and loaded before PP_CTRL[1], so the D3 walk proves it
  //! with its own LOCATE (the product order of parent D3 section 8.1)
  void pre_proof_power_up(bool late) {
    x.dram = image;
    if (late) x.dram.clear();
    x.erase_nvm();
    power_cycle();
    if (late) {
      x.idle(2000);
      x.dram = image;
    }
    seed_every_record();
    seed_every_binding();
    x.d->entity_enable_i = 1;
    x.d->link_up_i = 1;
  }
  void pre_proof_power_down() {
    x.nv_hdr_every = 0;
    x.nv_byte_every = 0;
    x.nv_rd_done_at = -1;
    x.nv_byte_at = -1;
    x.d->entity_enable_i = 0;
    x.d->link_up_i = 0;
    x.dram = image;
  }
  //! what a pre-proof boot saw, every clock in the harness's own count
  struct PreProof {
    Boot b;
    long bound = -1;          //! the count reads AGG - 1: the bound's own clock
    long d27 = -1;            //! the device's done of 0x27's payload READ
    long held = -1;           //! the clock that done was held to (-1: none)
    long release = -1;        //! the binding walk's terminal released (go_i)
    long proof = -1;          //! the D3 walk proves the image (proof_w)
    long term = -1;           //! COMPLETE, DEFAULTS or CLOSED first seen
    bool img_at_go = false;   //! the image validated as the D3 walk started
    long bind_wait = 0;       //! the binding walk's longest wait
    int d3_reads = 0;         //! D3 record READs from the release on
  };
  //! one boot of a pre-proof case: binding payload bytes `g` clocks apart,
  //! the headers at once; once 0x27's payload READ (the binding walk's last)
  //! is granted, its done is held to `place(bound)` (-1: not held)
  template <class Place>
  PreProof pre_proof_boot(int g, Place place) {
    x.nv_hdr_every = 0;
    x.nv_byte_every = g;
    PreProof p;
    size_t ops = x.nvm_ops.size();
    size_t ops_rel = ops;
    bool asked = false;
    p.b = boot_with(AGG + 2 * RS_TMO, [&] {
      const auto* d = x.d;
      const long now = long(x.t);
      if (p.bound < 0) p.bound = bound_t(d->dbg_d3_agg_o);
      while (ops < x.nvm_ops.size()) {
        const auto& o = x.nvm_ops[ops++];
        if (o.op == 0 && o.region == 0x27 && o.off != 0) p.d27 = now - 1;
      }
      if (!asked && x.nv_st == H::NvState::NV_READ && x.nv_cur.region == 0x27
          && x.nv_cur.off != 0) {
        p.held = place(p.bound);
        x.nv_rd_done_at = p.held;
        asked = true;
      }
      if (p.release < 0 && d->dbg_lsn_released_o) {
        p.release = now;
        p.img_at_go = d->dbg_img_valid_o;
        ops_rel = x.nvm_ops.size();
        x.nv_byte_every = 0;                     // the D3 walk's reads at once
      }
      if (p.proof < 0 && d->dbg_d3_proof_o) p.proof = now;
      if (p.term < 0 && (d->restore_done_o || d->restore_closed_o)) p.term = now;
      p.bind_wait = std::max(p.bind_wait, long(d->dbg_bind_wd_o));
    });
    x.nv_rd_done_at = -1;
    x.nv_byte_every = 0;
    p.d3_reads = d3_reads_of(ops_rel);
    return p;
  }
  //! the fixed lags from the device's done of 0x27's payload READ to the
  //! image proof, measured on boots whose device is fast: `img` with the
  //! image valid (W_IMG, the clock after the release), `loc` with it loaded
  //! late (the LOCATE's answer in W_IMGLOC), and that LOCATE's own length
  struct ProofLags {
    long img = -1;
    long loc = -1;
    long locate = -1;
  };
  ProofLags proof_lags() {
    ProofLags l;
    for (const bool late : {false, true}) {
      pre_proof_power_up(late);
      const PreProof p = pre_proof_boot(0, [](long) { return -1L; });
      if (late) {
        l.loc = p.proof - p.d27;
        l.locate = p.proof - p.release;
      } else {
        l.img = p.proof - p.d27;
      }
      pre_proof_power_down();
    }
    return l;
  }
  //! a DEFAULTS end with no record read: cause 3, the image valid, AECP
  //! released, within one per-wait deadline of the bound
  bool defaults_unread(const PreProof& p) const {
    const auto* d = x.d;
    return p.b.done >= 0 && p.b.closed < 0 && p.term > p.bound && p.term - p.bound < RS_TMO
           && d->restore_done_o && d->restore_fail_o && d->rs_cause_o == 3
           && !d->restore_rb_o && !d->restore_closed_o && d->dbg_img_valid_o
           && !d->dbg_d3_own_o && d->dbg_d3_applied_o == 0 && p.d3_reads == 0;
  }
  void report_defaults(const char* what, const PreProof& p) {
    const auto* d = x.d;
    const char* end = d->restore_closed_o ? "CLOSED"
                      : !d->restore_done_o ? "no terminal"
                      : d->restore_fail_o  ? "DEFAULTS" : "COMPLETE";
    CHECK(defaults_unread(p),
          "%s: DEFAULTS with no record READ; the restore ends %s %ld clocks after "
          "the bound, cause %u, rolled back %u, image valid %u, AECP owned %u, %d D3 "
          "record READs", what, end, p.term < 0 ? -1 : p.term - p.bound,
          unsigned(d->rs_cause_o), unsigned(d->restore_rb_o),
          unsigned(d->dbg_img_valid_o), unsigned(d->dbg_d3_own_o), p.d3_reads);
  }
  //! the binding payload spacing that ends the binding walk's natural run
  //! about 12,000 clocks before the bound: its last done is then held
  static constexpr int PRE_PROOF_G = static_cast<int>((AGG - 12000) / (8 * 20));

  // D3R19 (R391-3 F2, R390-3 F2: the image proven by the writer's LOCATE
  // after the bound). The image is absent at reset and loaded before
  // PP_CTRL[1], the product order of parent D3 section 8.1, so the D3 walk
  // proves it with its own LOCATE of ENTITY 0 (the store walks the image,
  // hundreds of clocks). Past the bound: D3R14's per-byte device keeps the
  // binding walk reading past the bound, it fails whole there, and the
  // LOCATE then proves the image: DEFAULTS, cause 3, within one per-wait
  // deadline of the bound, with no D3 record READ. Inside the LOCATE: the
  // binding walk completes, its last done placed so the bound falls midway
  // through the LOCATE (the aggregate fires there, in a stalled wait, while
  // the image is not yet proven): the proof ends DEFAULTS the same way, the
  // image valid and AECP released. A proof's DEFAULTS taken only from
  // W_IMG reads every record and ends COMPLETE long past the bound; an
  // aggregate that aborts inside the LOCATE closes a provable image.
  void r19_the_image_proven_by_its_locate_after_the_bound(const ProofLags& lag) {
    {
      pre_proof_power_up(true);
      const bool absent = !x.d->dbg_img_valid_o;
      x.nv_hdr_every = static_cast<int>(RS_TMO / 8 - 400);
      x.nv_byte_every = static_cast<int>(RS_TMO - 1000);
      PreProof p;
      size_t ops_rel = x.nvm_ops.size();
      p.b = boot_with(AGG + 2 * RS_TMO, [&] {
        const auto* d = x.d;
        const long now = long(x.t);
        if (p.bound < 0) p.bound = bound_t(d->dbg_d3_agg_o);
        if (p.release < 0 && d->dbg_lsn_released_o) {
          p.release = now;
          p.img_at_go = d->dbg_img_valid_o;
          ops_rel = x.nvm_ops.size();
          x.nv_hdr_every = 0;                    // the drained READ ends at once
          x.nv_byte_every = 0;
        }
        if (p.proof < 0 && d->dbg_d3_proof_o) p.proof = now;
        if (p.term < 0 && (d->restore_done_o || d->restore_closed_o)) p.term = now;
        p.bind_wait = std::max(p.bind_wait, long(d->dbg_bind_wd_o));
      });
      p.d3_reads = d3_reads_of(ops_rel);
      CHECK(absent && !p.img_at_go && p.release > p.bound && p.proof > p.release
                && p.bind_wait < RS_TMO && x.d->restore_cause_o == 3,
            "D3R19 past the bound: no image at reset (%u) nor at the D3 walk's start "
            "(%u); the binding walk (longest wait %ld of %ld) fails whole at the bound "
            "(cause %u), released %ld clocks after it, the LOCATE proves the image %ld "
            "clocks after the release", unsigned(absent), unsigned(p.img_at_go),
            p.bind_wait, RS_TMO, unsigned(x.d->restore_cause_o), p.release - p.bound,
            p.proof - p.release);
      report_defaults("D3R19 past the bound", p);
      pre_proof_power_down();
    }
    {
      //! the bound midway through the LOCATE
      const long off = lag.locate / 2;
      pre_proof_power_up(true);
      const PreProof p = pre_proof_boot(PRE_PROOF_G, [&](long bound) {
        return bound + off - lag.loc;
      });
      CHECK(lag.loc > 0 && lag.locate > 2 && !p.img_at_go && p.d27 == p.held
                && p.release < p.bound && p.proof == p.bound + off
                && p.bind_wait < RS_TMO && x.d->restore_cause_o == 0,
            "D3R19 inside the LOCATE: the binding walk completes (0x27 done on its "
            "placed clock %u, longest wait %ld of %ld), the LOCATE starts %ld clocks "
            "before the bound and proves the image %ld clocks after it (want %ld of "
            "%ld; -1: never)", unsigned(p.d27 == p.held), p.bind_wait, RS_TMO,
            p.bound - p.release, p.proof < 0 ? -1 : p.proof - p.bound, off, lag.locate);
      report_defaults("D3R19 inside the LOCATE", p);
      pre_proof_power_down();
    }
  }

  // D3R20 (R390-3 F2, R391-3 F2: a proof on the bound's own clock). The
  // binding walk completes, its last done placed so the image is proven on
  // exactly the bound's own clock: in W_IMG with the image valid at reset
  // (nothing is awaited there, so the aggregate fires on that clock too),
  // and in W_IMGLOC with the image loaded late and the LOCATE's answer in
  // hand (so the aggregate has not fired). Either proof is past the bound
  // and ends DEFAULTS, cause 3, with no record READ. A rule that tests the
  // fired level instead of the count reaching the bound starts pass 0 on
  // that clock and reads records past it.
  void r20_a_proof_on_the_bounds_own_clock(const ProofLags& lag) {
    struct Arm { bool late; const char* what; };
    const std::array<Arm, 2> arms{{{false, "D3R20 W_IMG"}, {true, "D3R20 W_IMGLOC"}}};
    for (const auto& a : arms) {
      const long to_proof = a.late ? lag.loc : lag.img;
      pre_proof_power_up(a.late);
      const PreProof p = pre_proof_boot(PRE_PROOF_G, [&](long bound) {
        return bound - to_proof;
      });
      CHECK(to_proof > 0 && p.img_at_go == !a.late && p.d27 == p.held
                && p.proof == p.bound && p.bind_wait < RS_TMO && x.d->restore_cause_o == 0,
            "%s: the binding walk completes (0x27 done on its placed clock %u, longest "
            "wait %ld of %ld), the image valid at the D3 walk's start %u, proven %ld "
            "clocks after the bound (want 0; -1: never)", a.what, unsigned(p.d27 == p.held),
            p.bind_wait, RS_TMO, unsigned(p.img_at_go), p.proof < 0 ? -1 : p.proof - p.bound);
      report_defaults(a.what, p);
      pre_proof_power_down();
    }
  }

  // D3R21 (R391-3 F2: a binding byte in hand on the expiry clock). D3R14's
  // walk, every sink's binding saved and the image valid, the header bytes
  // RS_TMO / 8 - 400 clocks apart and each payload byte RS_TMO / 2, so the
  // binding walk outlasts the bound; one payload byte is placed so the
  // binding manager holds it on the bound's own clock, the clock the
  // aggregate fires. agg_o is a level from the next clock, so the walk
  // takes its own per-wait path at its next waiting clock: it fails whole
  // on the clock after the bound, and the D3 walk ends DEFAULTS. A one-clock
  // pulse would meet only the byte in hand, and the walk would read on.
  void r21_a_binding_byte_in_hand_on_the_expiry_clock() {
    pre_proof_power_up(false);
    x.nv_hdr_every = static_cast<int>(RS_TMO / 8 - 400);
    x.nv_byte_every = static_cast<int>(RS_TMO / 2);
    long bound = -1;
    long lag = -1;                                 //! a payload byte to the manager
    long moved = -1;                               //! the device's last byte
    bool moved_payload = false;
    int sent = x.nv_rd_sent;
    long placed = -1;
    bool in_hand = false;
    bool stream_next = false;
    long failed = -1;                              //! restore_cause_o first reads 3
    long bind_wait = 0;
    long term = -1;
    const Boot b = boot_with(AGG + 2 * RS_TMO, [&] {
      const auto* d = x.d;
      const long now = long(x.t);
      if (bound < 0) bound = bound_t(d->dbg_d3_agg_o);
      const bool payload = x.nv_st == H::NvState::NV_READ && x.nv_cur.off != 0;
      if (x.nv_rd_sent != sent) {
        sent = x.nv_rd_sent;
        moved = now - 1;
        moved_payload = payload;
        const long gap = bound - lag - moved;
        if (placed < 0 && lag > 0 && payload && x.nv_left >= 2 && gap > 10
            && gap <= RS_TMO - 1500) {
          placed = bound - lag;
          x.nv_byte_at = placed;
        }
      }
      if (lag < 0 && moved_payload && d->dbg_bind_rvalid_o) lag = now - moved;
      if (now == bound) in_hand = d->dbg_bind_rvalid_o;
      if (now == bound + 1) stream_next = !d->dbg_bind_rvalid_o;
      if (failed < 0 && d->restore_cause_o == 3) failed = now;
      if (term < 0 && (d->restore_done_o || d->restore_closed_o)) term = now;
      bind_wait = std::max(bind_wait, long(d->dbg_bind_wd_o));
    });
    const auto* d = x.d;
    CHECK(lag > 0 && placed == bound - lag && in_hand && stream_next && bind_wait < RS_TMO,
          "D3R21: a binding payload byte placed at the device %ld clocks before the "
          "bound (lag %ld) is in the binding manager's hand on the bound's own clock "
          "(%u), none on the next (%u); longest wait %ld of %ld", bound - placed, lag,
          unsigned(in_hand), unsigned(stream_next), bind_wait, RS_TMO);
    CHECK(failed == bound + 2 && d->restore_fail_o && d->restore_cause_o == 3
              && b.done >= 0 && b.closed < 0 && term - bound < RS_TMO && d->rs_cause_o == 3
              && d->dbg_img_valid_o && !d->dbg_d3_own_o,
          "D3R21: the binding walk fails whole at its next waiting clock, the one after "
          "the bound (cause 3 registered %ld clocks after the bound, want 2), and the "
          "restore ends DEFAULTS %ld clocks after the bound (cause %u)",
          failed < 0 ? -1 : failed - bound, term < 0 ? -1 : term - bound,
          unsigned(d->rs_cause_o));
    pre_proof_power_down();
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
    //! D3R14's device: the binding walk alone outlasts the bound
    fresh();
    seed_every_record();
    seed_every_binding();
    x.nv_hdr_every = static_cast<int>(RS_TMO / 8 - 400);
    x.nv_byte_every = static_cast<int>(RS_TMO - 1000);
    report("every READ byte just inside the per-wait deadline", measured(AGG + 4 * RS_TMO));
    x.nv_hdr_every = 0;
    x.nv_byte_every = 0;
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
  // aborts at the deadline, cause 3, on defaults, after exactly RS_TMO
  // stalled cycles: the top's ceil(CLK_HZ_P / 50), which a floor misses by
  // one at this clock.
  void r8_the_deadline_boundary() {
    const std::array<long, 2> holds{RS_TMO - 200, RS_TMO + 200};
    for (const long hold : holds) {
      fresh();
      seed(0x50, d3_record(0x50, 1500000, 4));
      bool armed = false;
      long wait = 0;
      const Boot b = boot_with(8 * RS_TMO, [&] {
        if (!armed && x.d->nvm_dev_req_o && x.d->nvm_dev_region_o == 0x50) {
          x.nv_gnt_hold = static_cast<int>(hold);
          armed = true;
        }
        wait = std::max(wait, long(x.d->dbg_d3_wd_o));
      });
      const bool late = hold > RS_TMO;
      CHECK(armed && b.done > b.release && x.d->restore_fail_o == late
              && x.d->rs_cause_o == (late ? 3u : 0u)
              && ((x.d->aecp_pt_offset_v_o & 1) != 0) == !late,
            "D3R8: a READ granted %ld cycles late: fail %u, cause %u",
            hold, unsigned(x.d->restore_fail_o), unsigned(x.d->rs_cause_o));
      if (late)
        CHECK(wait + 1 == RS_TMO,
              "D3R8 deadline: the walk aborted on the %ld-th stalled cycle, the "
              "derived deadline is %ld", wait + 1, RS_TMO);
    }
  }

  // D3R8b: the integrator's format judge is a watched wait too. It never
  // answers in pass 1 (the Milan-info face stuck), and the per-wait
  // deadline, not the aggregate, ends the walk: rolled back to DEFAULTS,
  // cause 3, within the deadline plus the walk to the judge.
  void r8b_a_silent_format_judge_is_watched() {
    fresh();
    seed(0x00, d3_record(0x00, 1, 2));
    seed(0x30, d3_record(0x30, H::SFMT_MAIN_C, 8));
    x.gsi_stuck = true;
    const Boot b = boot(8 * RS_TMO);
    x.gsi_stuck = false;
    const auto* d = x.d;
    CHECK(b.done > b.release && b.done - b.release < 2 * RS_TMO && d->restore_fail_o
              && d->rs_cause_o == 3 && d->restore_rb_o && rows_cleared()
              && !d->dbg_d3_own_o,
          "D3R8b: a silent judge ends DEFAULTS at %ld, the release at %ld, cause "
          "%u, rolled back %u", b.done, b.release, unsigned(d->rs_cause_o),
          unsigned(d->restore_rb_o));
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
    r3b_the_rate_walk_reaches_its_eighth_entry();
    r4_the_passes_agree_record_by_record();
    r4b_blank_then_whole_disagrees();
    r5_a_pass_0_fault_applies_nothing();
    r5b_a_pass_1_read_held_past_its_deadline_is_drained();
    r6_blank_is_honest();
    r7_a_rule_fetch_fault_aborts();
    r8_the_deadline_boundary();
    r8b_a_silent_format_judge_is_watched();
    r9_a_held_set_follows_the_restore();
    r10_a_late_burst_is_waited_out();
    r11_a_roll_back_keeps_the_restored_binding();
    r12_a_roll_back_that_cannot_prove_the_image_closes();
    r13_the_aggregate_bound();
    r14_an_aggregate_bound_before_the_image_proof();
    r15_the_aggregate_bound_inside_a_roll_back();
    r16_the_aggregate_is_inert_after_the_terminal();
    r17_the_aggregate_waits_for_a_clock_without_an_event();
    r18_an_abort_in_the_arbiters_issue_cycle();
    const ProofLags lags = proof_lags();
    r19_the_image_proven_by_its_locate_after_the_bound(lags);
    r20_a_proof_on_the_bounds_own_clock(lags);
    r21_a_binding_byte_in_hand_on_the_expiry_clock();
  }
};

// ==== D3C. the clock-source selection over a ten-source domain ============
//! Issue #141 (milan-fpga #629): the selectable set grows from INTERNAL and
//! CRF to one more INPUT_STREAM source per AAF input, ten on the 8x8 shape:
//! INTERNAL 0, CRF 1, AAF input k at 2 + k (07 section 3.1 L6). Neither the
//! SET program nor the restore rule changed for it: both accept an index
//! below the located CLOCK_DOMAIN's clock_sources_count, over a list of any
//! length. These arms grade that on a fresh model over the suite's image
//! re-packed with a ten-source identity list, so an index and a count past
//! the suite's three reach both. Like the suite's, the image carries no
//! CLOCK_SOURCE descriptor: neither path reads one. The arms run in the
//! order D3C1, D3C3's save, D3C2, D3C3's restore, D3C4, so the refusal is
//! graded against a row already saved and a refusal that stored anything
//! shows as a second save.
struct D3ClockSourcePhase : D3RestorePhase {
  //! the 8x8 shape's list, and one source shorter
  const std::vector<uint8_t> ten;
  const std::vector<uint8_t> nine;
  //! AAF input 7, the last of the ten sources
  static constexpr uint16_t LAST_AAF = 9;
  //! the device's operations before D3C1's SET, where D3C3's save is read
  size_t ops_at_set = 0;
  //! the saved record as the device held it after D3C3's save
  std::vector<uint8_t> saved;

  D3ClockSourcePhase(H& tally, const std::vector<uint8_t>& img,
                     const std::vector<ImgEnt>& ents)
      : D3RestorePhase(tally, img, ents),
        ten(d3_image_with_sources(img, ents, 10)),
        nine(d3_image_with_sources(img, ents, 9)) {
    seq = 0xDC00;
  }

  //! one command from a controller, and every AECP frame the entity sends in
  //! the `ms` that follow it, in the order sent; `unflushed` counts the
  //! cycles a D3 record read pending
  std::vector<std::vector<uint8_t>> exchange(uint64_t mac, uint64_t eid, uint16_t s,
                                             uint16_t op, const std::vector<uint8_t>& pl,
                                             int ms, long* unflushed = nullptr) {
    x.q_aecp.clear();
    x.feed(aecp_frame(OWN_MAC, mac, 0, 0, EID, eid, s, op, pl));
    for (long c = 0; c < long(ms) * MS_CYC; ++c) {
      x.step();
      if (unflushed != nullptr && x.d->d3_unflushed_o) ++*unflushed;
    }
    std::vector<std::vector<uint8_t>> got(x.q_aecp.begin(), x.q_aecp.end());
    x.q_aecp.clear();
    return got;
  }
  static std::vector<std::vector<uint8_t>> to(const std::vector<std::vector<uint8_t>>& got,
                                              uint64_t mac) {
    std::vector<std::vector<uint8_t>> mine;
    for (const auto& f : got)
      if (f.size() >= 6 && fv_u64(f, 0, 6) == mac) mine.push_back(f);
    return mine;
  }
  //! the first controller's response carrying `index` (IEEE 1722.1-2021
  //! Figure 7-47: SET_CLOCK_SOURCE's response and GET_CLOCK_SOURCE's share it)
  static std::vector<uint8_t> answer(int status, uint16_t s, uint16_t op, uint16_t index) {
    return aecp_frame(CTLR_MAC, OWN_MAC, 1, uint8_t(status), EID, CTLR_EID, s, op,
                      D3ServicePhase::pl_clk(index));
  }
  //! the unsolicited SET_CLOCK_SOURCE response the second controller's
  //! registration earns, its first: sequence 0
  static std::vector<uint8_t> note(uint16_t index) {
    auto f = aecp_frame(C2_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR2_EID, 0x0000,
                        AEM_SET_CLOCK_SOURCE, D3ServicePhase::pl_clk(index));
    f[36] |= 0x80;
    return f;
  }
  std::vector<std::vector<uint8_t>> get_clock_source(uint16_t s, long* unflushed = nullptr) {
    return exchange(CTLR_MAC, CTLR_EID, s, AEM_GET_CLOCK_SOURCE, ti(0x0024, 0), 100, unflushed);
  }
  static int status_of(const std::vector<uint8_t>& f) {
    return f.size() > 16 ? (f[16] >> 3) & 0x1F : -1;
  }

  // D3C1: SET_CLOCK_SOURCE(9), AAF input 7 and the last of ten sources, is
  // accepted and answered with the index it stored (IEEE 1722.1-2021
  // 7.4.23.1), sends the one unsolicited response 7.4.23 requires to a
  // registered second controller, moves each effect strobe once, and reads
  // back over GET_CLOCK_SOURCE, the row and the exported index. The SET
  // window ends inside the debounce, so the GET runs before the save.
  void c1_the_last_aaf_source_is_accepted() {
    x.dram = ten;
    x.erase_nvm();
    power_cycle();
    CHECK(x.boot_to_aecp(), "D3C1: both walks over an erased device release AECP (premise)");
    x.d->link_up_i = 1;
    const auto reg = exchange(C2_MAC, CTLR2_EID, seq++, AEM_REGISTER_UNSOL,
                              std::vector<uint8_t>(4, 0), 100);
    CHECK(reg.size() == 1 && status_of(reg[0]) == AECP_SUCCESS,
          "D3C1: the second controller registered for unsolicited notifications (premise)");
    const unsigned w0 = x.d->dbg_dyn_writes_o;
    const uint64_t m0 = x.nvm_marks;
    const uint64_t n0 = x.notify_enqs;
    ops_at_set = x.nvm_ops.size();
    const uint16_t s = seq++;
    const auto got = exchange(CTLR_MAC, CTLR_EID, s, AEM_SET_CLOCK_SOURCE,
                              D3ServicePhase::pl_clk(LAST_AAF), 300);
    const auto rsp = to(got, CTLR_MAC);
    const auto notes = to(got, C2_MAC);
    const auto want = answer(AECP_SUCCESS, s, AEM_SET_CLOCK_SOURCE, LAST_AAF);
    CHECK(rsp.size() == 1 && rsp[0] == want,
          "D3C1: SET_CLOCK_SOURCE(9) over the ten-source domain answers SUCCESS byte-exact, "
          "carrying 9 (%zu responses)", rsp.size());
    if (rsp.size() == 1 && rsp[0] != want) { dump("got", rsp[0]); dump("exp", want); }
    CHECK(notes.size() == 1 && notes[0] == note(LAST_AAF),
          "D3C1 notify: exactly one unsolicited SET_CLOCK_SOURCE carrying 9 reaches the "
          "second controller, sequence 0 (%zu frames)", notes.size());
    CHECK(x.d->dbg_dyn_writes_o == w0 + 1 && x.nvm_marks == m0 + 1 && x.notify_enqs == n0 + 1,
          "D3C1 effects: one store write, one NVM_MARK, one NOTIFY_ENQ (%u, %u, %u)",
          unsigned(x.d->dbg_dyn_writes_o - w0), unsigned(x.nvm_marks - m0),
          unsigned(x.notify_enqs - n0));
    const uint16_t g = seq++;
    const auto get = get_clock_source(g);
    CHECK(get.size() == 1 && get[0] == answer(AECP_SUCCESS, g, AEM_GET_CLOCK_SOURCE, LAST_AAF)
              && x.d->dbg_dyn_clk_v_o && x.d->dbg_dyn_clk_o == LAST_AAF
              && x.d->aecp_clk_src_index_o == LAST_AAF,
          "D3C1 readback: GET_CLOCK_SOURCE reads 9 byte-exact, the row holds 9 with its "
          "valid flag and the top exports 9 (row %u valid %u, export %u)",
          unsigned(x.d->dbg_dyn_clk_o), unsigned(x.d->dbg_dyn_clk_v_o),
          unsigned(x.d->aecp_clk_src_index_o));
  }

  // D3C3, the save: D3S1's grade for an AAF index. The accepted 9 becomes
  // exactly one ERASE and one WRITE of record 0x0A (clock source, CLOCK_DOMAIN
  // 0) after the debounce, carrying the byte-exact F07.8 frame of the u16
  // index, and no other record moves.
  void c3_the_aaf_index_is_saved() {
    for (long c = 0; c < 3 * WINDOW; ++c) x.step();
    const auto want = d3_record(0x0A, LAST_AAF, 2);
    int erases = 0;
    int writes = 0;
    int stray = 0;
    std::vector<uint8_t> wrote;
    for (size_t i = ops_at_set; i < x.nvm_ops.size(); i++) {
      const auto& op = x.nvm_ops[i];
      if (op.region != 0x0A) ++stray;
      else if (op.op == 2) ++erases;
      else if (op.op == 1) { ++writes; wrote = op.wr; }
    }
    saved.assign(x.nv_mem[0x0A].begin(), x.nv_mem[0x0A].begin() + want.size());
    CHECK(erases == 1 && writes == 1 && wrote == want && saved == want
              && !x.d->d3_unflushed_o && stray == 0,
          "D3C3 save: record 0x0A carrying 9 erased and written once, byte-exact, nothing "
          "unflushed, no other record (%d erases, %d writes, %d other operations)",
          erases, writes, stray);
  }

  // D3C2: SET_CLOCK_SOURCE(10), the count itself, is not on the list: it
  // answers BAD_ARGUMENTS (IEEE 1722.1-2021 7.2.32, Table 7-141) carrying the
  // 9 in force (7.4.23.1), and stores, marks, notifies and saves nothing.
  void c2_the_count_itself_is_refused() {
    const unsigned w0 = x.d->dbg_dyn_writes_o;
    const uint64_t m0 = x.nvm_marks;
    const uint64_t n0 = x.notify_enqs;
    const size_t ops0 = x.nvm_ops.size();
    long unflushed = 0;
    const uint16_t s = seq++;
    const auto got = exchange(CTLR_MAC, CTLR_EID, s, AEM_SET_CLOCK_SOURCE,
                              D3ServicePhase::pl_clk(10), 300, &unflushed);
    const auto rsp = to(got, CTLR_MAC);
    const auto notes = to(got, C2_MAC);
    const auto want = answer(AECP_BAD_ARGUMENTS, s, AEM_SET_CLOCK_SOURCE, LAST_AAF);
    CHECK(rsp.size() == 1 && rsp[0] == want,
          "D3C2: SET_CLOCK_SOURCE(10), the count, answers BAD_ARGUMENTS byte-exact carrying "
          "the 9 in force (%zu responses)", rsp.size());
    if (rsp.size() == 1 && rsp[0] != want) { dump("got", rsp[0]); dump("exp", want); }
    CHECK(notes.empty() && x.d->dbg_dyn_writes_o == w0 && x.nvm_marks == m0
              && x.notify_enqs == n0,
          "D3C2 effects: the refusal writes, marks and enqueues nothing (%u, %u, %u) and "
          "the second controller hears nothing (%zu frames)",
          unsigned(x.d->dbg_dyn_writes_o - w0), unsigned(x.nvm_marks - m0),
          unsigned(x.notify_enqs - n0), notes.size());
    const uint16_t g = seq++;
    const auto get = get_clock_source(g, &unflushed);
    CHECK(get.size() == 1 && get[0] == answer(AECP_SUCCESS, g, AEM_GET_CLOCK_SOURCE, LAST_AAF)
              && x.d->dbg_dyn_clk_v_o && x.d->dbg_dyn_clk_o == LAST_AAF
              && x.d->aecp_clk_src_index_o == LAST_AAF,
          "D3C2 readback: GET_CLOCK_SOURCE still reads 9, the row and the export hold 9 "
          "(row %u, export %u)", unsigned(x.d->dbg_dyn_clk_o),
          unsigned(x.d->aecp_clk_src_index_o));
    for (long c = 0; c < 2 * WINDOW; ++c) {
      x.step();
      if (x.d->d3_unflushed_o) ++unflushed;
    }
    CHECK(unflushed == 0 && x.nvm_ops.size() == ops0,
          "D3C2 saved: nothing pending and no device operation for two windows after the "
          "refusal (%ld pending cycles, %zu operations)", unflushed, x.nvm_ops.size() - ops0);
  }

  // D3C3, the restore: D3R1's grade for an AAF index. Across a power cycle
  // the walk applies record 0x0A whole and ends COMPLETE; the row reads 9
  // with its valid flag, GET_CLOCK_SOURCE reads it, and the top exports it.
  void c3_the_aaf_index_is_restored() {
    power_cycle();
    x.d->link_up_i = 1;
    const Boot b = boot(6 * RS_TMO);
    const auto* d = x.d;
    CHECK(b.cleared && b.done > b.release && !d->restore_fail_o && !d->restore_closed_o
              && d->dbg_d3_applied_o == 1 && d->dbg_d3_refused_o == 0
              && d->dbg_d3_blank_o == D3_RECORDS - 1,
          "D3C3 restore: COMPLETE from cleared rows with the saved record applied "
          "(applied %u refused %u blank %u of %d)", unsigned(d->dbg_d3_applied_o),
          unsigned(d->dbg_d3_refused_o), unsigned(d->dbg_d3_blank_o));
    const uint16_t g = seq++;
    const auto get = get_clock_source(g);
    CHECK(get.size() == 1 && get[0] == answer(AECP_SUCCESS, g, AEM_GET_CLOCK_SOURCE, LAST_AAF)
              && d->dbg_dyn_clk_v_o && d->dbg_dyn_clk_o == LAST_AAF
              && d->aecp_clk_src_index_o == LAST_AAF,
          "D3C3 restore: clock source 9 restored with its valid flag, GET reads it and the "
          "top exports it (row %u valid %u, export %u)", unsigned(d->dbg_dyn_clk_o),
          unsigned(d->dbg_dyn_clk_v_o), unsigned(d->aecp_clk_src_index_o));
  }

  // D3C4: the record D3C3 saved, restored over an image whose list is
  // shorter, at its count (nine sources) and above it (the suite's three).
  // The restore rule is the SET program's (07 section 5.3), so each walk
  // refuses the record and ends COMPLETE; the row stays at its reset value,
  // invalid, and GET reads the image's index 0.
  void c4_a_smaller_image_refuses_the_saved_index() {
    struct Arm { const char* what; const std::vector<uint8_t>* img; unsigned count; };
    for (const Arm& a : {Arm{"at the count", &nine, 9}, Arm{"above the count", &image, 3}}) {
      x.dram = *a.img;
      seed(0x0A, saved);
      power_cycle();
      x.d->link_up_i = 1;
      const Boot b = boot(6 * RS_TMO);
      const auto* d = x.d;
      CHECK(saved == d3_record(0x0A, LAST_AAF, 2) && b.done > b.release
                && !d->restore_fail_o && !d->restore_closed_o && d->dbg_d3_applied_o == 0
                && d->dbg_d3_refused_o == 1 && d->dbg_d3_blank_o == D3_RECORDS - 1,
            "D3C4 %s: the saved 9 over %u sources is refused, COMPLETE (applied %u "
            "refused %u blank %u of %d)", a.what, a.count, unsigned(d->dbg_d3_applied_o),
            unsigned(d->dbg_d3_refused_o), unsigned(d->dbg_d3_blank_o));
      const uint16_t g = seq++;
      const auto get = get_clock_source(g);
      CHECK(get.size() == 1 && get[0] == answer(AECP_SUCCESS, g, AEM_GET_CLOCK_SOURCE, 0)
                && !d->dbg_dyn_clk_v_o && d->dbg_dyn_clk_o == 0 && d->aecp_clk_src_index_o == 0,
            "D3C4 %s: the row stays unset and GET reads the image's 0 (row %u valid %u, "
            "export %u)", a.what, unsigned(d->dbg_dyn_clk_o), unsigned(d->dbg_dyn_clk_v_o),
            unsigned(d->aecp_clk_src_index_o));
    }
    x.dram = image;
  }

  // D3C5 (issue #52): a record 0x0A that cannot be restored falls back to
  // the image's index, 0, over the suite's three-source image: erased (blank),
  // corrupt (index 2, legal, with a crc that is not the crc of its bytes) and
  // read torn (the device ends pass 0's payload READ after one byte). The
  // first two end COMPLETE, the torn read DEFAULTS with cause 1;
  // in each the row stays unset, GET_CLOCK_SOURCE reads 0 and the top exports 0.
  void c5_an_unrestorable_record_keeps_the_image_index() {
    struct Arm { const char* what; bool erased; bool corrupt; bool torn; };
    for (const Arm& a : {Arm{"blank", true, false, false}, Arm{"corrupt", false, true, false},
                         Arm{"torn", false, false, true}}) {
      x.dram = image;
      x.erase_nvm();
      if (!a.erased) {
        auto rec = d3_record(0x0A, 2, 2);
        if (a.corrupt) rec[7] ^= 0x01;              // the crc's low byte
        seed(0x0A, rec);
      }
      power_cycle();
      if (a.torn) {
        x.nv_rd_region = 0x0A;
        x.nv_rd_nth = 2;                            // pass 0's payload READ
        x.nv_rd_after = 1;
        x.nv_rd_silent = false;
        x.nv_rd_seen = 0;
      }
      x.d->link_up_i = 1;
      const Boot b = boot(6 * RS_TMO);
      x.nv_rd_region = -1;
      const auto* d = x.d;
      const bool verdict = a.torn ? (d->restore_fail_o && d->rs_cause_o == 1 && !d->restore_rb_o)
                                  : (!d->restore_fail_o
                                     && d->dbg_d3_refused_o == (a.corrupt ? 1u : 0u));
      const uint16_t g = seq++;
      const auto get = get_clock_source(g);
      CHECK(b.done > b.release && verdict && !d->dbg_dyn_clk_v_o && d->dbg_dyn_clk_o == 0
                && d->aecp_clk_src_index_o == 0 && get.size() == 1
                && get[0] == answer(AECP_SUCCESS, g, AEM_GET_CLOCK_SOURCE, 0),
            "D3C5 %s: the image's index 0 stays in force: the row unset, GET reads 0 and "
            "the top exports 0 (fail %u cause %u refused %u, row %u valid %u, export %u)",
            a.what, unsigned(d->restore_fail_o), unsigned(d->rs_cause_o),
            unsigned(d->dbg_d3_refused_o), unsigned(d->dbg_dyn_clk_o),
            unsigned(d->dbg_dyn_clk_v_o), unsigned(d->aecp_clk_src_index_o));
    }
  }

  // D3C6 (issue #52): the saved index is in force before the entity is
  // enabled. With entity_enable_i requested from reset, the top exports the
  // restored 2 in every cycle the ADP engine's enable is high, the first
  // included, and GET_CLOCK_SOURCE then reads it.
  void c6_the_saved_index_precedes_the_enable() {
    x.dram = image;
    x.erase_nvm();
    seed(0x0A, d3_record(0x0A, 2, 2));
    power_cycle();
    x.d->link_up_i = 1;
    x.d->entity_enable_i = 1;
    long enabled = 0;
    long early = 0;
    long first = -1;
    long c = 0;
    const Boot b = boot_with(6 * RS_TMO, [&] {
      ++c;
      if (!x.d->dbg_adp_enable_o) return;
      ++enabled;
      if (first < 0) first = c;
      early += (x.d->aecp_clk_src_index_o != 2 || !x.d->dbg_dyn_clk_v_o) ? 1 : 0;
    });
    const uint16_t g = seq++;
    const auto get = get_clock_source(g);
    CHECK(b.done > b.release && first >= 0 && enabled > 0 && early == 0 && get.size() == 1
              && get[0] == answer(AECP_SUCCESS, g, AEM_GET_CLOCK_SOURCE, 2),
          "D3C6: the restored index 2 is exported in every cycle the ADP enable is high "
          "(first at %ld, %ld of %ld cycles not), and GET reads it", first, early, enabled);
    x.d->entity_enable_i = 0;
  }

  void run() {
    c1_the_last_aaf_source_is_accepted();
    c3_the_aaf_index_is_saved();
    c2_the_count_itself_is_refused();
    c3_the_aaf_index_is_restored();
    c4_a_smaller_image_refuses_the_saved_index();
    c5_an_unrestorable_record_keeps_the_image_index();
    c6_the_saved_index_precedes_the_enable();
  }
};

// ==== D3N. the user names: the name stage ==================================
//! Issues #61 and #83 (REQ-PER-001, REQ-AEM-011; Milan v1.2 5.3.13: every
//! user name "shall" be saved and restored after a power cycle). Each
//! writable name is one record, 0x80 + its name-table ordinal, the 64-byte
//! entry verbatim (07 section 5.2). The suite's image names eleven of the
//! store's 32 ordinals: ENTITY's entity_name 0 and group_name 1, CLOCK_DOMAIN
//! 0's 2, the streams' 3 to 6, AVB_INTERFACE 7, AUDIO_UNIT 8, the IDENTIFY
//! CONTROL 9 and the SIGNAL_MULTIPLEXER 10. The arms set five of them over
//! real SET_NAMEs: both ENTITY names (the group name a full 64 bytes, no
//! NUL), the clock domain's to the EMPTY name, the CONTROL's (its name, which
//! persists; its IDENTIFY value never does) and the last ordinal.
struct D3NamePhase : D3RestorePhase {
  struct Named {
    uint16_t type, index, name_index, ordinal;
    std::vector<uint8_t> name;
  };
  std::vector<Named> named;
  //! the image's names by ordinal, as the store's walk loads them
  std::vector<std::vector<uint8_t>> defaults;

  D3NamePhase(H& tally, const std::vector<uint8_t>& img, const std::vector<ImgEnt>& ents)
      : D3RestorePhase(tally, img, ents) {
    seq = 0xDB00;
    const uint32_t off = rd32(&img[16]);
    const uint16_t n = uint16_t((img[10] << 8) | img[11]);
    for (uint16_t k = 0; k < n; ++k)
      defaults.emplace_back(img.begin() + off + 64u * k, img.begin() + off + 64u * (k + 1));
    std::vector<uint8_t> full(64);
    for (size_t b = 0; b < full.size(); ++b) full[b] = uint8_t('!' + (b * 7) % 90);
    named = {{0x0000, 0, 0, 0, NamePhase::name64("D3N entity name, saved")},
             {0x0000, 0, 1, 1, full},
             {0x0024, 0, 0, 2, std::vector<uint8_t>(64, 0)},
             {0x001A, 0, 0, 9, NamePhase::name64("D3N identify control")},
             {0x0022, 0, 0, 10, NamePhase::name64("D3N last ordinal")}};
  }

  //! the framed record of name ordinal `ord` (07 section 5.2): the header
  //! {0x1722, layout 2, 0x80 + ord, length 64, crc16} and the entry verbatim
  static std::vector<uint8_t> name_record(uint16_t ord, const std::vector<uint8_t>& name) {
    std::vector<uint8_t> r = {0x17, 0x22, 0x02, uint8_t(0x80 + ord), 0x00, 0x40, 0x00, 0x00};
    r.insert(r.end(), name.begin(), name.end());
    const uint16_t c = d3_crc16(r);
    r[6] = uint8_t(c >> 8);
    r[7] = uint8_t(c);
    return r;
  }
  //! the store's name-table entry `ord`, read lane by lane through the tap
  std::vector<uint8_t> entry(uint16_t ord) {
    std::vector<uint8_t> n;
    for (uint16_t k = 0; k < 8; ++k) {
      x.d->dbg_name_lane_i = uint8_t(ord * 8 + k);
      x.d->eval();
      for (int b = 7; b >= 0; --b) n.push_back(uint8_t(x.d->dbg_name_o >> (8 * b)));
    }
    x.d->dbg_name_lane_i = 0;
    x.d->eval();
    return n;
  }
  bool set_name(const Named& n, const std::vector<uint8_t>& name) {
    return ok(AEM_SET_NAME, NamePhase::name_body(n.type, n.index, n.name_index, CFGIX, name));
  }
  //! GET_NAME answers `name` byte-exact (IEEE 1722.1-2021 7.4.18)
  bool get_reads(const Named& n, const std::vector<uint8_t>& name) {
    const uint16_t s = seq;
    const auto f = ask(AEM_GET_NAME, NamePhase::name_sel(n.type, n.index, n.name_index));
    return f == aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID, s, AEM_GET_NAME,
                           NamePhase::name_body(n.type, n.index, n.name_index, CFGIX, name));
  }
  //! READ_DESCRIPTOR(ENTITY 0) serves `entity` and `group` in its two name
  //! fields (IEEE 1722.1-2021 Table 7-2: entity_name at 48, group_name at 180)
  bool entity_reads(const std::vector<uint8_t>& entity, const std::vector<uint8_t>& group) {
    std::vector<uint8_t> rd(8, 0);
    putbe(&rd[0], CFGIX, 2);
    const auto f = ask(AEM_READ_DESCRIPTOR, rd);
    return f.size() >= 42 + 244
           && std::equal(entity.begin(), entity.end(), f.begin() + 42 + 48)
           && std::equal(group.begin(), group.end(), f.begin() + 42 + 180);
  }
  int ops_on(size_t from, uint8_t region, int op) const {
    int n = 0;
    for (size_t i = from; i < x.nvm_ops.size(); i++)
      n += (x.nvm_ops[i].op == op && x.nvm_ops[i].region == region) ? 1 : 0;
    return n;
  }

  // D3N1: a real SET_NAME of each of the five becomes exactly one ERASE and
  // one WRITE of its own record after the debounce, carrying the byte-exact
  // F07.8 frame of the entry the command left (the empty name too); no other
  // record id is touched and nothing stays unflushed.
  void n1_every_name_persists_its_record() {
    fresh();
    CHECK(x.boot_to_aecp(), "D3N1: both walks over an erased device release AECP (premise)");
    x.d->link_up_i = 1;
    const size_t ops0 = x.nvm_ops.size();
    bool accepted = true;
    for (const auto& n : named) accepted = set_name(n, n.name) && accepted;
    CHECK(accepted, "D3N1: every SET_NAME answered SUCCESS (premise)");
    for (long c = 0; c < 3 * WINDOW; ++c) x.step();
    int stray = 0;
    for (size_t i = ops0; i < x.nvm_ops.size(); i++) {
      bool listed = false;
      for (const auto& n : named) listed = listed || x.nvm_ops[i].region == 0x80 + n.ordinal;
      stray += listed ? 0 : 1;
    }
    for (const auto& n : named) {
      const uint8_t rid = uint8_t(0x80 + n.ordinal);
      const auto want = name_record(n.ordinal, n.name);
      std::vector<uint8_t> wrote;
      for (size_t i = ops0; i < x.nvm_ops.size(); i++)
        if (x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == rid) wrote = x.nvm_ops[i].wr;
      CHECK(ops_on(ops0, rid, 2) == 1 && ops_on(ops0, rid, 1) == 1 && wrote == want
                && std::equal(want.begin(), want.end(), x.nv_mem[rid].begin()),
            "D3N1 ordinal %u: record 0x%02x erased and written once, byte-exact (%d writes)",
            unsigned(n.ordinal), unsigned(rid), ops_on(ops0, rid, 1));
    }
    CHECK(stray == 0 && !x.d->d3_unflushed_o,
          "D3N1: no record outside the five names, nothing unflushed (%d operations)", stray);
  }

  // D3N2: a SET_NAME that names what the entry already holds writes no lane
  // (SET_NAME writes only the lanes that change), so it marks nothing
  // pending and the device sees no operation for two windows (parent DR2b).
  void n2_an_unchanged_name_writes_nothing() {
    const size_t ops0 = x.nvm_ops.size();
    long pulses = 0;
    long pending = 0;
    const bool same = set_name(named[0], named[0].name);
    for (long c = 0; c < 2 * WINDOW; ++c) {
      x.step();
      pulses += x.d->aecp_name_wr_o ? 1 : 0;
      pending += x.d->d3_unflushed_o ? 1 : 0;
    }
    CHECK(same && pulses == 0 && pending == 0 && x.nvm_ops.size() == ops0,
          "D3N2: an unchanged SET_NAME writes no lane (%ld pulses), nothing pending "
          "(%ld cycles) and no device operation (%zu)", pulses, pending,
          x.nvm_ops.size() - ops0);
  }

  // D3N3: the five come back across a power cycle. When the D3 walk proves
  // the image, the clock before it reads any record, every one of their
  // entries holds the image's name (the store's walk at reset reloads the
  // table, and may still be running at the admission gate's release, which
  // is why the proof waits for it); the walk ends COMPLETE
  // with exactly the five applied; GET_NAME reads each byte-exact (the empty
  // name and the full one included) and READ_DESCRIPTOR serves both ENTITY
  // names; the restore's name writes pulse no live write, and none becomes a
  // change (nothing pending, no device write, for two windows).
  void n3_every_name_survives_a_power_cycle() {
    power_cycle();
    bool cleared = false;
    bool sampled = false;
    long pulses = 0;
    const Boot b = boot_with(6 * RS_TMO, [&] {
      pulses += x.d->aecp_name_wr_o ? 1 : 0;
      if (sampled || !x.d->dbg_d3_proof_o) return;
      sampled = true;
      cleared = true;
      for (const auto& n : named) cleared = cleared && entry(n.ordinal) == defaults[n.ordinal];
    });
    x.d->link_up_i = 1;
    CHECK(sampled && cleared,
          "D3N3: every saved name's entry holds the image's name when the D3 walk "
          "proves the image");
    const auto* d = x.d;
    CHECK(b.done > b.release && !d->restore_fail_o && !d->restore_blank_o
              && d->dbg_d3_applied_o == named.size() && d->dbg_d3_refused_o == 0
              && d->dbg_d3_blank_o == D3_RECORDS - named.size(),
          "D3N3: COMPLETE, applied %u refused %u blank %u of %d",
          unsigned(d->dbg_d3_applied_o), unsigned(d->dbg_d3_refused_o),
          unsigned(d->dbg_d3_blank_o), D3_RECORDS);
    for (const auto& n : named)
      CHECK(entry(n.ordinal) == n.name && get_reads(n, n.name),
            "D3N3 ordinal %u: the entry holds the saved name and GET_NAME reads it "
            "byte-exact", unsigned(n.ordinal));
    CHECK(entity_reads(named[0].name, named[1].name),
          "D3N3: READ_DESCRIPTOR(ENTITY) serves both restored names");
    const size_t ops0 = x.nvm_ops.size();
    long pending = 0;
    for (long c = 0; c < 2 * WINDOW; ++c) {
      x.step();
      pending += x.d->d3_unflushed_o ? 1 : 0;
    }
    CHECK(pulses == 0 && pending == 0 && x.nvm_ops.size() == ops0,
          "D3N3: the restore's name writes are no live writes (%ld pulses of "
          "aecp_name_wr_o) and no changes (%ld pending cycles, %zu device operations)",
          pulses, pending, x.nvm_ops.size() - ops0);
  }

  // D3N4: SET_NAME's rule and the frame. A framed record for ordinal 20,
  // past the image's eleven names (no SET can name it), is refused by the
  // rule; ordinal 3's record with a crc that is not the crc of its bytes and
  // ordinal 4's carrying an 8-byte payload are refused by the frame; ordinal
  // 5's applies. COMPLETE with 1 applied and 3 refused, and GET_NAME reads
  // the image's names for 3 and 4 and the saved one for 5.
  void n4_the_rule_and_the_frame_refuse() {
    fresh();
    const auto past = NamePhase::name64("D3N past the image");
    seed(0x80 + 20, name_record(20, past));
    auto crc = name_record(3, NamePhase::name64("D3N corrupt"));
    crc[7] ^= 0x01;
    seed(0x83, crc);
    std::vector<uint8_t> shrt = {0x17, 0x22, 0x02, 0x84, 0x00, 0x08, 0x00, 0x00,
                                 'D', '3', 'N', ' ', 's', 'h', 'r', 't'};
    seed(0x84, reframe(shrt));
    const auto five = NamePhase::name64("D3N stream 6.0");
    seed(0x85, name_record(5, five));
    const Boot b = boot(6 * RS_TMO);
    x.d->link_up_i = 1;
    const auto* d = x.d;
    CHECK(b.done > b.release && !d->restore_fail_o && d->dbg_d3_applied_o == 1
              && d->dbg_d3_refused_o == 3 && entry(20) != past,
          "D3N4: COMPLETE, applied %u refused %u, ordinal 20 left alone",
          unsigned(d->dbg_d3_applied_o), unsigned(d->dbg_d3_refused_o));
    CHECK(get_reads({0x0005, 0, 0, 3, {}}, defaults[3]) && get_reads({0x0005, 1, 0, 4, {}}, defaults[4])
              && get_reads({0x0006, 0, 0, 5, {}}, five),
          "D3N4: GET_NAME reads the image's names for the refused 3 and 4, the saved "
          "one for 5");
  }

  // D3N5: a pass-1 abort after a name was applied rolls the name back with
  // the store. Ordinal 0's record applies; ordinal 10's, read whole in pass 0,
  // is erased before pass 1 reads it (cause 5); the roll-back resets the
  // descriptor store, which walks the image again, so the entry, GET_NAME and
  // READ_DESCRIPTOR all carry the image's entity_name.
  void n5_a_roll_back_restores_the_image_names() {
    fresh();
    seed(0x80, name_record(0, named[0].name));
    seed(0x8A, name_record(10, named[4].name));
    const size_t ops0 = x.nvm_ops.size();
    bool erased = false;
    bool applied = false;
    const Boot b = boot_with(6 * RS_TMO, [&] {
      if (!erased && reads_of(ops0, 0x8A) == 2) {
        std::fill(x.nv_mem[0x8A].begin(), x.nv_mem[0x8A].end(), 0xFF);
        erased = true;
      }
      applied = applied || x.d->dbg_d3_applied_o != 0;
    });
    x.d->link_up_i = 1;
    CHECK(erased && applied && b.done > b.release && x.d->restore_fail_o
              && x.d->rs_cause_o == 5 && x.d->restore_rb_o && !x.d->restore_closed_o,
          "D3N5: the name applied, then pass 1's disagreement rolled back (cause %u, "
          "rolled back %u) (premise)", unsigned(x.d->rs_cause_o), unsigned(x.d->restore_rb_o));
    CHECK(entry(0) == defaults[0] && get_reads(named[0], defaults[0])
              && entity_reads(defaults[0], defaults[1]),
          "D3N5: the roll-back left the image's entity_name in the entry, GET_NAME "
          "and READ_DESCRIPTOR");
  }

  // D3N6: a change to the name after its latch taints the WRITE in flight.
  // The device holds the record's WRITE request while a second SET_NAME
  // changes the same entry; the first WRITE carries the latched name, its
  // done clears nothing, and the record ends holding the second.
  void n6_a_change_during_the_write_taints_it() {
    fresh();
    CHECK(x.boot_to_aecp(), "D3N6: both walks release AECP (premise)");
    const auto a = NamePhase::name64("D3N taint first");
    const auto b2 = NamePhase::name64("D3N taint second");
    const size_t ops0 = x.nvm_ops.size();
    x.nv_gnt_hold = 1 << 30;
    const bool first = set_name(named[2], a);
    bool held = false;
    for (long c = 0; c < 2 * WINDOW && !held; ++c) {
      held = x.d->nvm_dev_req_o && x.d->nvm_dev_region_o == 0x82;
      if (!held) x.step();
    }
    const bool second = set_name(named[2], b2);
    x.nv_gnt_hold = 0;
    for (long c = 0; c < 3 * WINDOW; ++c) x.step();
    std::vector<uint8_t> wrote_first;
    for (size_t i = ops0; i < x.nvm_ops.size() && wrote_first.empty(); i++)
      if (x.nvm_ops[i].op == 1 && x.nvm_ops[i].region == 0x82) wrote_first = x.nvm_ops[i].wr;
    const auto want = name_record(2, b2);
    CHECK(first && held && second && ops_on(ops0, 0x82, 1) == 2
              && wrote_first == name_record(2, a)
              && std::equal(want.begin(), want.end(), x.nv_mem[0x82].begin())
              && !x.d->d3_unflushed_o,
          "D3N6 taint: %d WRITEs of 0x82, the first carrying the latched name, the "
          "record holding the change made during it", ops_on(ops0, 0x82, 1));
  }

  // D3N7: the image is loaded after the store's boot walk (D3O4's order), and
  // the device holds ordinal 0's record. The writer's LOCATE makes the store
  // walk the image before any record is read, so the name written back is
  // not overwritten by that walk: GET_NAME and READ_DESCRIPTOR read it.
  void n7_a_late_image_is_walked_before_the_names() {
    x.dram.clear();
    x.erase_nvm();
    seed(0x80, name_record(0, named[0].name));
    power_cycle();
    x.idle(2000);
    const bool invalid_before = !x.d->dbg_img_valid_o;
    x.dram = image;
    const Boot b = boot(6 * RS_TMO);
    x.d->link_up_i = 1;
    CHECK(invalid_before && b.done > b.release && !x.d->restore_fail_o
              && x.d->dbg_d3_applied_o == 1 && get_reads(named[0], named[0].name)
              && entity_reads(named[0].name, defaults[1]),
          "D3N7: an image loaded late is walked before the name is written back: "
          "GET_NAME and READ_DESCRIPTOR read the saved entity_name");
  }

  void run() {
    n1_every_name_persists_its_record();
    n2_an_unchanged_name_writes_nothing();
    n3_every_name_survives_a_power_cycle();
    n4_the_rule_and_the_frame_refuse();
    n5_a_roll_back_restores_the_image_names();
    n6_a_change_during_the_write_taints_it();
    n7_a_late_image_is_walked_before_the_names();
  }
};

// ==== D3K. every D3 record type cut mid-commit by rst_n ====================
//! Issues #61 and #83 (09 section 3 NVM: "every record type cut at least
//! once"). The device keeps each WRITE byte as it takes it (H's progressive
//! mode), so a cut leaves what a real cut leaves. For each record type the
//! device holds A, which the first boot restores, and a real SET of B starts
//! the record's ERASE and WRITE; rst_n is then pulsed with the device carried
//! at a cut point: on the ERASE's grant (A still at rest), at the WRITE's
//! grant (erased), after its 8-byte header, one byte short of the record, and
//! at a byte drawn from a fixed seed. The next boot's restore is never a
//! failure: an erased record or a torn header is UNFRAMED (blank), a torn
//! payload fails its crc16 (refused), and either keeps the image's value,
//! while A cut before its ERASE completed comes back. The device also holds
//! sink 0's binding at the cut (placed there once B is accepted, because a
//! bound sink refuses SET_CONFIGURATION and its own SET_STREAM_FORMAT with
//! STREAM_IS_RUNNING), and the boot after the cut restores it, probing
//! PASSIVE (PRB_W_AVAIL, Milan 5.5.3.5.2). Once UNBIND_RX frees sink 0, a
//! later SET of B persists over whatever the cut left.
struct D3CutPhase : D3RestorePhase {
  struct Row { const char* group; uint8_t rid; int plen; uint64_t a, b; };
  static constexpr uint64_t TALKER = 0x00B0B0B0B0B0D3C7ULL;
  static constexpr uint32_t SEED = 0xD3C0FFEE;
  std::mt19937 rng{SEED};

  D3CutPhase(H& tally, const std::vector<uint8_t>& img, const std::vector<ImgEnt>& ents)
      : D3RestorePhase(tally, img, ents) {
    seq = 0xDC80;
  }

  static std::vector<Row> rows() {
    return {{"cfg", 0x00, 2, 1, 0},
            {"rate", 0x02, 4, 48000, 96000},
            {"clks", 0x0A, 2, 1, 2},
            {"fmti", 0x30, 8, H::SFMT_ALT_C, H::SFMT_MAIN_C},
            {"fmto", 0x40, 8, H::SFMT_ALT_C, H::SFMT_MAIN_C},
            {"ptof", 0x50, 4, 1000000, 1500000},
            {"name", 0x80, 64, 0, 1}};
  }
  static std::vector<uint8_t> name_of(uint64_t v) {
    return NamePhase::name64(v ? "D3K name B" : "D3K name A");
  }
  //! the record of value `v`: the scalar's frame, or ENTITY 0's entity_name
  static std::vector<uint8_t> record(const Row& r, uint64_t v) {
    return r.rid == 0x80 ? D3NamePhase::name_record(0, name_of(v))
                         : d3_record(r.rid, v, r.plen);
  }
  bool set(const Row& r, uint64_t v) {
    using S = D3ServicePhase;
    switch (r.rid) {
      case 0x00: return ok(AEM_SET_CONFIGURATION, S::pl_cfg(uint16_t(v)));
      case 0x02: return ok(AEM_SET_SAMPLING_RATE, S::pl_rate(uint32_t(v)));
      case 0x0A: return ok(AEM_SET_CLOCK_SOURCE, S::pl_clk(uint16_t(v)));
      case 0x30: return ok(AEM_SET_STREAM_FORMAT, S::pl_fmt(0x0005, 0, v));
      case 0x40: return ok(AEM_SET_STREAM_FORMAT, S::pl_fmt(0x0006, 0, v));
      case 0x50: return ok(AEM_SET_STREAM_INFO, S::pl_ptof(0, uint32_t(v)));
      default:   return ok(AEM_SET_NAME, NamePhase::name_body(0x0000, 0, 0, CFGIX, name_of(v)));
    }
  }
  //! the group holds `v` with its valid flag, or (`v` absent) the image's
  //! value with the flag clear; a name is read back over GET_NAME
  bool holds(const Row& r, const uint64_t* v) {
    const auto* d = x.d;
    switch (r.rid) {
      case 0x00: return v ? (d->dbg_dyn_cfg_v_o && d->dbg_dyn_cfg_o == *v) : !d->dbg_dyn_cfg_v_o;
      case 0x02: return v ? (d->dbg_dyn_rate_v_o && d->dbg_dyn_rate_o == *v) : !d->dbg_dyn_rate_v_o;
      case 0x0A: return v ? (d->dbg_dyn_clk_v_o && d->dbg_dyn_clk_o == *v) : !d->dbg_dyn_clk_v_o;
      case 0x30: return v ? ((d->aecp_fmt_in_v_o & 1) && x.fmt_row(false, 0) == *v)
                          : !(d->aecp_fmt_in_v_o & 1);
      case 0x40: return v ? ((d->aecp_fmt_out_v_o & 1) && x.fmt_row(true, 0) == *v)
                          : !(d->aecp_fmt_out_v_o & 1);
      case 0x50: return v ? ((d->aecp_pt_offset_v_o & 1) && d->aecp_pt_offset_o.at(0) == *v)
                          : !(d->aecp_pt_offset_v_o & 1);
      default: {
        const uint16_t s = seq;
        const auto f = ask(AEM_GET_NAME, NamePhase::name_sel(0x0000, 0, 0));
        const auto want = v ? name_of(*v) : NamePhase::name64("PP Reference Entity");
        return f == aecp_frame(CTLR_MAC, OWN_MAC, 1, AECP_SUCCESS, EID, CTLR_EID, s,
                               AEM_GET_NAME, NamePhase::name_body(0x0000, 0, 0, CFGIX, want));
      }
    }
  }
  //! sink 0 bound to the saved talker and probing PASSIVE: GET_STREAM_INFO's
  //! probing_status (Milan v1.2 5.4.2.10, the top three bits of byte 90)
  bool binding_waits_for_its_talker() {
    const auto g = ask(AEM_GET_STREAM_INFO, ti(0x0005, 0));
    return (x.d->acmp_bound_o & 1) && g.size() == 94 && (g[90] >> 5) == 1;
  }

  //! one cut: A at rest and restored, B SET, rst_n at `where` (-1 on the
  //! ERASE's grant, else after `where` WRITE bytes), then the restore
  void cut(const Row& r, int where) {
    const int total = 8 + r.plen;
    fresh();
    x.nv_wr_progressive = true;
    seed(r.rid, record(r, r.a));
    const Boot b0 = boot(6 * RS_TMO);
    x.d->link_up_i = 1;
    const bool restored = b0.done > b0.release && holds(r, &r.a);
    const bool accepted = set(r, r.b);
    bool reached = false;
    for (long c = 0; c < 3 * WINDOW && !reached; ++c) {
      reached = x.nv_cur.region == r.rid
                && (where < 0 ? x.nv_st == H::NvState::NV_ERASE
                              : (x.nv_st == H::NvState::NV_WRITE
                                 && x.nv_cur.wr.size() == size_t(where)));
      if (!reached) x.step();
    }
    char at[40];
    if (where < 0) snprintf(at, sizeof at, "the ERASE");
    else snprintf(at, sizeof at, "byte %d of %d", where, total);
    CHECK(restored && accepted && reached,
          "D3K %s cut at %s: A restored, B accepted, the cut point reached (premise)",
          r.group, at);
    seed(0x20, binding_record(TALKER, 0x0DC7, CTLR_EID));
    power_cycle();
    const Boot b = boot(6 * RS_TMO);
    x.d->link_up_i = 1;
    const auto* d = x.d;
    const bool value = where < 0 ? holds(r, &r.a) : holds(r, nullptr);
    CHECK(b.done > b.release && !d->restore_fail_o && !d->restore_closed_o && value,
          "D3K %s cut at %s: the restore does not fail, and the group holds %s "
          "(fail %u cause %u, applied %u refused %u)", r.group, at,
          where < 0 ? "A" : "the image's value", unsigned(d->restore_fail_o),
          unsigned(d->rs_cause_o), unsigned(d->dbg_d3_applied_o),
          unsigned(d->dbg_d3_refused_o));
    CHECK(binding_waits_for_its_talker(),
          "D3K %s cut at %s: sink 0's saved binding restored, probing PASSIVE", r.group, at);
    //! UNBIND_RX (DISCONNECT_RX_COMMAND, 8) of sink 0, answered by its response (9)
    const uint16_t u = seq++;
    x.q_acmp.clear();
    x.feed(acmp_frame(CTLR_MAC, 8, 0, 0, CTLR_EID, TALKER, EID, 0x0DC7, 0, 0, 0, u, 0, 0));
    const auto un = x.wait_frame(x.q_acmp, 100, [u](const std::vector<uint8_t>& f) {
      return f.size() == 70 && (f[15] & 0x0F) == 9 && fv_u64(f, 62, 2) == u;
    });
    const bool again = !un.empty() && ((un[16] >> 3) & 0x1F) == 0 && set(r, r.b);
    for (long c = 0; c < 3 * WINDOW; ++c) x.step();
    const auto want = record(r, r.b);
    CHECK(again && std::equal(want.begin(), want.end(), x.nv_mem[r.rid].begin())
              && !d->d3_unflushed_o,
          "D3K %s cut at %s: once sink 0 is unbound, a later SET of B persists over what "
          "the cut left", r.group, at);
    x.nv_wr_progressive = false;
  }

  void run() {
    for (const auto& r : rows()) {
      const int total = 8 + r.plen;
      std::uniform_int_distribution<int> draw(1, total - 2);
      for (int where : {-1, 0, 8, total - 1, draw(rng)}) cut(r, where);
    }
  }
};

// ==== D3V. the volatile set across a power cycle ===========================
//! Issues #59 (REQ-NOT-005) and #62 (REQ-PER-002). Milan v1.2 5.3.4.1: "The
//! locked state is cleared by a power cycle"; 5.3.4.2: "The list of
//! registered controllers is cleared by a power cycle"; 5.3.12: IDENTIFY is 0
//! "after reset". D3R1 grades one registration; here two controllers
//! register, the second TIME_LIMITED (IEEE 1722.1-2021 7.4.37.2), the first
//! locks the entity and sets IDENTIFY, and the device holds a saved binding.
//! A power cycle (rst_n, the device carried) and both restore walks follow,
//! and every piece of the volatile set is gone while the binding comes back.
//! The wrap compresses the TIME_LIMITED window and the lock to 400 ms
//! (pp_top_wrap.sv), so everything before the cycle happens inside them; the
//! CONTROLLER_AVAILABLE monitor (Milan 5.4.5.3, 30 to 60 s) is not
//! compressed, and the watch after the cycle outlasts it.
struct D3VolatilePhase : D3RestorePhase {
  static constexpr uint64_t C3_MAC = 0x0202C3C3C3C3ULL;
  static constexpr uint64_t C3_EID = 0x7777000000000044ULL;
  static constexpr uint64_t TALKER = 0x00B0B0B0B0B0D3A1ULL;
  static constexpr uint16_t AEM_DEREGISTER_UNSOL = 0x0025;
  static constexpr int AECP_NO_RESOURCES = 8;
  //! the CONTROLLER_AVAILABLE monitor's longest draw (60 s) with U10's margin
  static constexpr int WATCH_MS = 66000;

  D3VolatilePhase(H& tally, const std::vector<uint8_t>& img,
                  const std::vector<ImgEnt>& ents)
      : D3RestorePhase(tally, img, ents) {
    seq = 0xDA00;
  }

  //! one command from a controller, then every AECP frame the entity sends
  //! in the `ms` that follow it, in the order sent
  std::vector<std::vector<uint8_t>> exchange(uint64_t mac, uint64_t eid, uint16_t s,
                                             uint16_t op, const std::vector<uint8_t>& pl,
                                             int ms) {
    x.q_aecp.clear();
    x.feed(aecp_frame(OWN_MAC, mac, 0, 0, EID, eid, s, op, pl));
    for (long c = 0; c < long(ms) * MS_CYC; ++c) x.step();
    std::vector<std::vector<uint8_t>> got(x.q_aecp.begin(), x.q_aecp.end());
    x.q_aecp.clear();
    return got;
  }
  //! the status of the solicited response to `mac` carrying `s`, -1 if none
  static int answer(const std::vector<std::vector<uint8_t>>& got, uint64_t mac, uint16_t s) {
    for (const auto& f : got)
      if (f.size() > 37 && fv_u64(f, 0, 6) == mac && (f[36] & 0x80) == 0
          && fv_u64(f, 34, 2) == s)
        return (f[16] >> 3) & 0x1F;
    return -1;
  }
  int status_from(uint64_t mac, uint64_t eid, uint16_t op, const std::vector<uint8_t>& pl) {
    const uint16_t s = seq++;
    return answer(exchange(mac, eid, s, op, pl, 30), mac, s);
  }
  //! every frame in `got` addressed to `mac`, solicited or not
  static int to(const std::vector<std::vector<uint8_t>>& got, uint64_t mac) {
    int n = 0;
    for (const auto& f : got) n += (f.size() >= 6 && fv_u64(f, 0, 6) == mac) ? 1 : 0;
    return n;
  }
  //! the unsolicited SET_CLOCK_SOURCE response a registered controller is
  //! sent (IEEE 1722.1-2021 7.4.23, 9.3.2.1 u = 1) at its sequence_id `s`
  static std::vector<uint8_t> note(uint64_t mac, uint64_t eid, uint16_t s, uint16_t index) {
    auto f = aecp_frame(mac, OWN_MAC, 1, AECP_SUCCESS, EID, eid, s, AEM_SET_CLOCK_SOURCE,
                        D3ServicePhase::pl_clk(index));
    f[36] |= 0x80;
    return f;
  }
  static bool has(const std::vector<std::vector<uint8_t>>& got, const std::vector<uint8_t>& want) {
    return std::find(got.begin(), got.end(), want) != got.end();
  }
  static std::vector<uint8_t> lock_pl(bool unlock) {
    std::vector<uint8_t> p(16, 0);
    p[3] = unlock ? 0x01 : 0x00;                  // IEEE 1722.1-2021 Table 7-133 UNLOCK
    return p;
  }
  static std::vector<uint8_t> register_pl(bool time_limited) {
    std::vector<uint8_t> p(4, 0);
    p[3] = time_limited ? 0x01 : 0x00;            // IEEE 1722.1-2021 Table 7-147
    return p;
  }

  //! the volatile set populated, the binding restored from the device: the
  //! premise of every check after the cycle
  void populate() {
    fresh();
    seed(0x20, binding_record(TALKER, 0x0DA1, CTLR_EID));
    CHECK(x.boot_to_aecp() && (x.d->acmp_bound_o & 1),
          "D3V1: the first boot restores sink 0's saved binding (premise)");
    x.d->link_up_i = 1;
    const int r1 = status_from(CTLR_MAC, CTLR_EID, AEM_REGISTER_UNSOL, register_pl(false));
    const int r2 = status_from(C2_MAC, CTLR2_EID, AEM_REGISTER_UNSOL, register_pl(true));
    const uint16_t s = seq++;
    const auto got = exchange(C3_MAC, C3_EID, s, AEM_SET_CLOCK_SOURCE,
                              D3ServicePhase::pl_clk(1), 30);
    CHECK(r1 == AECP_SUCCESS && r2 == AECP_SUCCESS && answer(got, C3_MAC, s) == AECP_SUCCESS
              && has(got, note(CTLR_MAC, CTLR_EID, 0, 1))
              && has(got, note(C2_MAC, CTLR2_EID, 0, 1)),
          "D3V1: both controllers registered, the second TIME_LIMITED, and a third "
          "controller's change notifies each at sequence_id 0 (premise)");
    const int lk = status_from(CTLR_MAC, CTLR_EID, AEM_LOCK_ENTITY, lock_pl(false));
    const int id = status_from(CTLR_MAC, CTLR_EID, AEM_SET_CONTROL,
                               D3ServicePhase::pl_identify(255));
    const int c2 = status_from(C2_MAC, CTLR2_EID, AEM_LOCK_ENTITY, lock_pl(false));
    CHECK(lk == AECP_SUCCESS && id == AECP_SUCCESS && c2 == AECP_ENTITY_LOCKED
              && x.d->dbg_lock_held_o && x.d->dbg_identify_o == 255,
          "D3V1: the first controller holds the lock (the second is refused "
          "ENTITY_LOCKED) and IDENTIFY reads 255 (premise)");
  }

  // D3V2-D3V4: the cycle. The device keeps sink 0's binding record; both
  // walks run from restore_go_i to their terminal. Lock and IDENTIFY are
  // graded in every cycle from restore_go_i on, so a value that survived the
  // reset and was cleared later still fails.
  void the_power_cycle() {
    power_cycle();
    long held = 0;
    long identifying = 0;
    const Boot b = boot_with(6 * RS_TMO, [&] {
      held += x.d->dbg_lock_held_o ? 1 : 0;
      identifying += x.d->dbg_identify_o != 0 ? 1 : 0;
    });
    x.d->link_up_i = 1;
    x.q_acmp.clear();
    x.feed(acmp_frame(C3_MAC, 10, 0, 0, C3_EID, 0, EID, 0, 0, 0, 0, 0xDA02, 0, 0));
    const auto g = x.wait_frame(x.q_acmp, 50, [](const std::vector<uint8_t>& f) {
      return f.size() > 15 && (f[15] & 0x0F) == 11;
    });
    CHECK(b.done > b.release && !x.d->restore_fail_o && (x.d->acmp_bound_o & 1)
              && g.size() > 50 && fv_u64(g, 34, 8) == TALKER,
          "D3V2: the binding preload still arrives: sink 0 bound to its saved "
          "talker after the cycle");
    const auto ctl = ask(AEM_GET_CONTROL, ti(0x001A, 0));
    CHECK(identifying == 0 && x.d->dbg_identify_o == 0 && ctl.size() > 42 && ctl[42] == 0,
          "D3V3: IDENTIFY reads 0 in every cycle from restore_go_i on (%ld cycles "
          "otherwise) and GET_CONTROL reads 0", identifying);
    CHECK(held == 0 && !x.d->dbg_lock_held_o,
          "D3V4: aecp_lock_held_o is 0 in every cycle from restore_go_i on (%ld "
          "cycles held)", held);
  }

  // D3V5, D3V6: the registry is empty. A third controller's change notifies
  // neither former controller, and for the monitor's longest draw nothing
  // at all reaches them: no CONTROLLER_AVAILABLE probe, no TIME_LIMITED
  // expiry DEREGISTER, no notification.
  void nothing_reaches_the_former_controllers() {
    const uint16_t s = seq++;
    const auto got = exchange(C3_MAC, C3_EID, s, AEM_SET_CLOCK_SOURCE,
                              D3ServicePhase::pl_clk(2), 30);
    CHECK(answer(got, C3_MAC, s) == AECP_SUCCESS && to(got, CTLR_MAC) == 0
              && to(got, C2_MAC) == 0,
          "D3V5: a third controller's change after the cycle notifies neither "
          "former controller (%d and %d frames)", to(got, CTLR_MAC), to(got, C2_MAC));
    int c1 = 0;
    int c2 = 0;
    for (long c = 0; c < long(WATCH_MS) * MS_CYC; ++c) {
      x.step();
      while (!x.q_aecp.empty()) {
        const auto& f = x.q_aecp.front();
        c1 += (f.size() >= 6 && fv_u64(f, 0, 6) == CTLR_MAC) ? 1 : 0;
        c2 += (f.size() >= 6 && fv_u64(f, 0, 6) == C2_MAC) ? 1 : 0;
        x.q_aecp.pop_front();
      }
    }
    CHECK(c1 == 0 && c2 == 0,
          "D3V6: for %d ms after the change no frame reaches either former "
          "controller: no CONTROLLER_AVAILABLE, no expiry DEREGISTER (%d and %d)",
          WATCH_MS, c1, c2);
  }

  // D3V7: the lock is free. LOCK_ENTITY from the second controller answers
  // SUCCESS, not ENTITY_LOCKED, and holds the lock; its UNLOCK frees it.
  void the_lock_is_free() {
    const int lk = status_from(C2_MAC, CTLR2_EID, AEM_LOCK_ENTITY, lock_pl(false));
    const bool held = x.d->dbg_lock_held_o;
    const int un = status_from(C2_MAC, CTLR2_EID, AEM_LOCK_ENTITY, lock_pl(true));
    CHECK(lk == AECP_SUCCESS && held && un == AECP_SUCCESS && !x.d->dbg_lock_held_o,
          "D3V7: LOCK_ENTITY from the second controller answers SUCCESS (status %d) "
          "and takes the lock; its UNLOCK frees it", lk);
  }

  // D3V8, D3V9: every row is free and a row restarts at sequence_id 0.
  // Sixteen controllers new to the entity all register (Milan 5.3.4.2's
  // sixteen); one of them leaves and the first former controller registers
  // again, and its first notification carries sequence_id 0 (Milan
  // 5.4.2.21: zero when a new entry is created), not the 1 its row would
  // have reached before the cycle.
  void every_row_is_free() {
    int ok_regs = 0;
    for (uint16_t k = 0; k < 16; ++k) {
      const uint64_t mac = 0x0202D5000000ULL + k;
      const uint64_t eid = 0x77770000000D5000ULL + k;
      ok_regs += status_from(mac, eid, AEM_REGISTER_UNSOL, register_pl(false)) == AECP_SUCCESS;
    }
    CHECK(ok_regs == 16, "D3V8: sixteen new controllers all register after the cycle "
          "(%d of 16 SUCCESS)", ok_regs);
    const int dr = status_from(0x0202D500000FULL, 0x77770000000D500FULL,
                               AEM_DEREGISTER_UNSOL, {});
    const int r1 = status_from(CTLR_MAC, CTLR_EID, AEM_REGISTER_UNSOL, register_pl(false));
    const uint16_t s = seq++;
    const auto got = exchange(C3_MAC, C3_EID, s, AEM_SET_CLOCK_SOURCE,
                              D3ServicePhase::pl_clk(0), 60);
    CHECK(dr == AECP_SUCCESS && r1 == AECP_SUCCESS && answer(got, C3_MAC, s) == AECP_SUCCESS
              && has(got, note(CTLR_MAC, CTLR_EID, 0, 0)),
          "D3V9: the first former controller registers again and its first "
          "notification carries sequence_id 0, byte-exact");
  }

  void run() {
    populate();
    the_power_cycle();
    nothing_reaches_the_former_controllers();
    the_lock_is_free();
    every_row_is_free();
  }
};
