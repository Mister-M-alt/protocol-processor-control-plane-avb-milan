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
