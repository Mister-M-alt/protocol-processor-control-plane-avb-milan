// SPDX-License-Identifier: CERN-OHL-W-2.0
// Issue #128: expectations use only the documented ports and time bounds.
// Included after Hn so the same RX/command BFM also serves these fresh boots.
struct RetryChecks {
  int& checks;
  int& fails;
  static constexpr uint32_t retry_ms = 100;
  static constexpr int sweep_cycles = N_SRC * (MAAP_TMO + 64);
  static constexpr uint64_t poison = 0x91e0f000deadULL;
  void probe(Hn& h, int src, int status, uint64_t da, const char* tag);
  static void conflict(Hn& h, int src);
  void late_availability();
  void paced_refusal();
  void fair_absence();
  void obsolete_grants();
  void late_responses();
  void preserved_backoff();
  void block_changes();
  void retry_wrap();
};

void RetryChecks::probe(Hn& h, int src, int status, uint64_t da, const char* tag) {
    h.resps.clear();
    CHECK(h.send(MT_PROBE, src, C1, 200 + src, L1, 8, FL_FC),
          "%s src%d consumed", tag, src);
    CHECK(h.last_send_cyc <= MAAP_TMO + 64,
          "%s src%d response budget", tag, src);
    CHECK(h.resps.size() == 1 && h.resps[0].status == status
          && h.resps[0].da == da
          && h.resps[0].sid == (status == ST_OK ? sid_of(src) : 0)
          && h.resps[0].vlan == (status == ST_OK ? VID : 0),
          "%s src%d status/tuple", tag, src);
}
void RetryChecks::conflict(Hn& h, int src) {
    h.d->maap_conflict_valid_i = 1;
    h.d->maap_conflict_src_i = src;
    h.tick();
}

void RetryChecks::late_availability() {
  // R1: startup refusal, then availability at the last millisecond before
  // the retry boundary. Every enabled source acquires without a probe.
  {
    Hn h; h.grant_ok = false; h.block_count = N_SRC;
    h.configure_the_talker_and_release_reset(); h.run(sweep_cycles);
    CHECK(h.mreqs.size() == N_SRC, "R1 one refused startup attempt per source");
    h.grant_ok = true; h.d->now_ms_i = 199; h.run(sweep_cycles);
    CHECK(h.mreqs.size() == N_SRC, "R1 no retry before 100 ms");
    h.d->now_ms_i = 200; h.run(sweep_cycles);
    CHECK(h.mreqs.size() == 2 * N_SRC, "R1 automatic bounded acquisition");
    CHECK(h.gates.empty() && h.decl_mask() == 0 && h.arms.empty(),
          "R1 acquired addresses neither declare nor borrow timer slots");
    for (int s = 0; s < N_SRC; ++s)
      probe(h, s, ST_OK, da_pool(s), "R1 first probe after bound");
    CHECK(h.decl_mask() == 0xff, "R1 every probed source declares");
    h.d->now_ms_i = 200 + T_DAFRESH;
    for (int s = 0; s < N_SRC; ++s) { h.fire_expiry(s); h.run(12); }
    CHECK(h.decl_mask() == 0, "R1 freshness expiry still withdraws every source");
    CHECK(h.mreqs.size() == 2 * N_SRC, "R1 owned addresses are never reallocated");
  }
}

void RetryChecks::paced_refusal() {
  // R2: permanent refusal plus repeated probes/listener changes cannot
  // turn the retry round into a request storm. Disabled sources stay quiet.
  {
    Hn h; h.grant_ok = false; h.block_count = N_SRC;
    h.configure_the_talker_and_release_reset(); h.run(sweep_cycles);
    h.d->cfg_src_en_i = 0x55; h.run(100);
    for (int round = 1; round <= 3; ++round) {
      h.d->now_ms_i = 100 + round * retry_ms; h.run(sweep_cycles);
      const auto before = h.mreqs.size();
      CHECK(before == static_cast<size_t>(N_SRC + round * 4), "R2 one attempt per enabled source per round");
      for (int s = 0; s < N_SRC; s += 2) {
        probe(h, s, ST_DMAC_FAIL, 0, "R2 honest refusal");
        probe(h, s, ST_DMAC_FAIL, 0, "R2 repeated probe");
        h.set_lsn(s, LSN_READY); h.set_lsn(s, LSN_NONE);
      }
      h.run(sweep_cycles);
      CHECK(h.mreqs.size() == before, "R2 demand cannot bypass retry pacing");
      CHECK(h.decl_mask() == 0 && h.gates.empty(), "R2 no declaration without ownership");
    }
    h.d->cfg_src_en_i = 0;
    h.run(100); h.d->now_ms_i += retry_ms; h.run(sweep_cycles);
    const auto before = h.mreqs.size();
    h.set_lsn(7, LSN_READY); h.d->now_ms_i += retry_ms; h.run(sweep_cycles);
    CHECK(h.mreqs.size() == before, "R2 disabled listener event cannot allocate");
    probe(h, 7, ST_TK_UNKNOWN, 0, "R2 disabled probe");
    h.grant_ok = true; h.d->cfg_src_en_i = 0xff; h.run(sweep_cycles);
    for (int s = 0; s < N_SRC; ++s)
      probe(h, s, ST_OK, da_pool(s), "R2 re-enable recovers");
  }
}

void RetryChecks::fair_absence() {
  // R3: ready tied low, with continuously presented PROBE_TX traffic.
  // Accelerate ms so a request spans retry rounds. Round robin must serve
  // every source before revisiting one, even though src0 is probed nonstop.
  {
    Hn h; h.configure_the_talker_and_release_reset();
    h.d->maap_req_ready_i = 0;
    probe(h, 0, ST_DMAC_FAIL, 0, "R3 absent allocator");
    h.d->txn_valid_i = 1;
    int since_response = 0;
    int max_gap = 0;
    for (int cycle = 0; cycle < 3 * sweep_cycles; ++cycle) {
      h.d->now_ms_i += 1;
      const auto n = h.resps.size(); h.tick();
      ++since_response;
      if (h.resps.size() != n) {
        if (since_response > max_gap) max_gap = since_response;
        since_response = 0;
      }
    }
    h.d->txn_valid_i = 0; h.run(MAAP_TMO + 100);
    unsigned seen = 0;
    for (int i = 0; i < N_SRC && i < static_cast<int>(h.offered.size()); ++i)
      seen |= 1u << h.offered[i].src;
    CHECK(seen == 0xff && h.offered.size() >= N_SRC,
          "R3 every source gets an attempt under continuous commands");
    CHECK(max_gap <= MAAP_TMO + 64 && h.resps.size() > 20,
          "R3 commands never starve behind retries (gap=%d)", max_gap);
    CHECK(h.hold_last == MAAP_TMO, "R3 absent request respects accept bound");
    CHECK(h.decl_mask() == 0, "R3 absent allocator never declares");
  }
}

void RetryChecks::obsolete_grants() {
  // R4: successful but obsolete grants, arriving either after disable/
  // re-enable or after conflict. No old DA may enter a record or gate.
  for (int moved = 0; moved < 2; ++moved) {
    Hn h; h.auto_grant = false;
    h.configure_the_talker_and_release_reset(); h.d->cfg_src_en_i = 1;
    h.run(60);
    CHECK(h.mreqs.size() == 1, "R4 pending allocation accepted");
    if (moved) { conflict(h, 0); h.run(40); }
    else {
      h.d->cfg_src_en_i = 0; h.run(40);
      h.d->cfg_src_en_i = 1; h.run(40);
    }
    h.inject_rsp(true, poison); h.run(40);
    probe(h, 0, ST_DMAC_FAIL, 0, "R4 obsolete grant rejected");
    CHECK(h.decl_mask() == 0 && h.gates.empty(), "R4 obsolete grant cannot declare");
    CHECK(h.mreqs.size() == 2 && h.mreqs.back().rel,
          "R4 obsolete successful allocation released before retry");
    h.inject_rsp(false, 0); h.run(40);
    h.d->now_ms_i += retry_ms; h.run(40);
    CHECK(h.mreqs.size() == 3 && !h.mreqs.back().rel,
          "R4 reacquires after obsolete release");
    h.inject_rsp(true, da_pool(77)); h.run(40);
    probe(h, 0, ST_OK, da_pool(77), "R4 fresh grant");
  }
}

void RetryChecks::late_responses() {
  // R5: expiry wraps modulo 2^32; a timed-out response is swallowed before
  // the retry's own response. Persistent silence saturates stale credits
  // safely rather than attributing an untagged grant to the wrong source.
  {
    Hn h; h.auto_grant = false;
    h.configure_the_talker_and_release_reset(); h.d->cfg_src_en_i = 1;
    h.d->rst_n = 0; h.d->now_ms_i = 0xfffffff0u; h.run(5);
    h.d->rst_n = 1; h.run(60);
    h.d->now_ms_i = static_cast<uint32_t>(0xfffffff0u + MAAP_RSP_MS - 1); h.run(60);
    CHECK(h.mreqs.size() == 1, "R5 no premature response timeout across wrap");
    h.d->now_ms_i += 1; h.run(60);
    CHECK(h.mreqs.size() == 2, "R5 timeout retries automatically across wrap");
    h.inject_rsp(true, poison); h.run(40);
    probe(h, 0, ST_DMAC_FAIL, 0, "R5 stale timeout response");
    h.inject_rsp(true, da_pool(88)); h.run(40);
    probe(h, 0, ST_OK, da_pool(88), "R5 retry response");
  }
  {
    Hn h; h.auto_grant = false;
    h.configure_the_talker_and_release_reset(); h.run(60);
    for (int i = 0; i < 4; ++i) {
      h.d->now_ms_i += MAAP_RSP_MS; h.run(100);
    }
    CHECK(h.mreqs.size() == 3, "R5 silent accepts stop at stale-credit capacity");
    probe(h, 7, ST_DMAC_FAIL, 0, "R5 saturated silence");
    h.inject_rsp(true, poison); h.run(100);
    CHECK(h.mreqs.size() == 4, "R5 stale drain releases capacity for another source");
    CHECK(h.decl_mask() == 0, "R5 stale drain does not grant ownership");
  }
}

void RetryChecks::preserved_backoff() {
  // R6: retry rounds cannot consume or shorten the conflict/PCP backoff.
  for (int pcp = 0; pcp < 2; ++pcp) {
    Hn h; h.configure_the_talker_and_release_reset(); h.d->cfg_src_en_i = 1;
    h.run(100); probe(h, 0, ST_OK, da_pool(0), "R6 before backoff");
    if (pcp) { h.d->srp_pcp_change_i = 1; h.tick(); }
    else conflict(h, 0);
    h.run(40);
    const auto allocs = h.mreqs.size();
    const auto arms_before = h.arms.size();
    CHECK(!h.arms.empty() && h.arms.back().dl == 20100,
          "R6 full two-LeaveAll backoff armed");
    for (int ms = 200; ms < 20100; ms += 100) {
      h.d->now_ms_i = ms; h.run(100);
    }
    CHECK(h.mreqs.size() == allocs && h.decl_mask() == 0
          && h.arms.size() == arms_before, "R6 retries leave backoff and timer untouched");
    h.d->now_ms_i = 20099;
    probe(h, 0, pcp ? ST_OK : ST_DMAC_FAIL, pcp ? da_pool(0) : 0,
          "R6 probe during backoff");
    h.d->now_ms_i = 20100; h.fire_expiry(0); h.run(100);
    CHECK(h.decl_mask() == 1, "R6 backoff expiry redeclares with fresh demand");
    CHECK(h.mreqs.size() == allocs + (pcp ? 0 : 1),
          "R6 only conflict needs a new address");
    probe(h, 0, ST_OK, da_pool(pcp ? 0 : 1), "R6 after backoff");
  }
}

void RetryChecks::block_changes() {
  // R7: a block server may refuse high sources while serving low ones.
  // A changed block/count fans out conflicts; a newly valid block does not.
  {
    Hn h; h.block_count = 2;
    h.configure_the_talker_and_release_reset(); h.run(sweep_cycles);
    auto get_da = [&](int s, uint64_t expected) {
      h.resps.clear();
      CHECK(h.send(MT_GTXS, s, C1, 300 + s, 0, 0, 0), "R7 query consumed src%d", s);
      CHECK(h.resps.size() == 1 && h.resps[0].da == expected,
            "R7 block mapping src%d", s);
    };
    for (int s = 0; s < N_SRC; ++s) get_da(s, s < 2 ? da_pool(s) : 0);
    h.block_count = N_SRC;
    for (int s = 0; s < N_SRC; ++s) conflict(h, s);
    h.d->now_ms_i += retry_ms; h.run(sweep_cycles);
    for (int s = 0; s < N_SRC; ++s) get_da(s, da_pool(s));
    CHECK(h.decl_mask() == 0, "R7 count growth does not create demand");
    h.block_count = 2; h.block_base += 0x100;
    for (int s = 0; s < N_SRC; ++s) conflict(h, s);
    h.d->now_ms_i += retry_ms; h.run(sweep_cycles);
    const auto before = h.mreqs.size();
    for (int s = 0; s < N_SRC; ++s)
      probe(h, s, s < 2 ? ST_OK : ST_DMAC_FAIL,
            s < 2 ? h.block_base + s : 0, "R7 shrink and block move");
    h.run(sweep_cycles);
    CHECK(h.mreqs.size() == before, "R7 out-of-block probes cannot storm");
    CHECK(h.decl_mask() == 3, "R7 only in-block sources declare");
  }
}

void RetryChecks::retry_wrap() {
  // R8: pin both sides of the retry boundary when the timebase wraps.
  {
    Hn h; h.grant_ok = false;
    h.configure_the_talker_and_release_reset();
    h.d->rst_n = 0; h.d->now_ms_i = 0xfffffff0u; h.run(5);
    h.d->rst_n = 1; h.run(sweep_cycles);
    h.d->now_ms_i = 83; h.run(sweep_cycles);
    CHECK(h.mreqs.size() == N_SRC, "R8 wrap retry bound minus one");
    h.d->now_ms_i = 84; h.run(sweep_cycles);
    CHECK(h.mreqs.size() == 2 * N_SRC, "R8 wrap retry at bound");
  }
}

void Hn::check_retry_cases() {
  RetryChecks retry{checks, fails};
  retry.late_availability();
  retry.paced_refusal();
  retry.fair_absence();
  retry.obsolete_grants();
  retry.late_responses();
  retry.preserved_backoff();
  retry.block_changes();
  retry.retry_wrap();
}
