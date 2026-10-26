// SPDX-License-Identifier: CERN-OHL-W-2.0
// Issue #202: a MAAP conflict on a declaring source moves it to a new Stream
// ID at the next grant and declares at once. Expectations are written from
// the rule in 05 F05.12, never read from the DUT: the new ID keeps the
// slot's upper 48 bits and takes the first unique ID, counted up from a
// cursor that starts at P-N-STREAM-OUT (8), that is no source's current and
// no source's configured ID. Included after Hn, like retry_cases.hpp.
struct SidChecks {
  int& checks;
  int& fails;
  //! the upper 48 bits every default slot shares (sid_of)
  static constexpr uint64_t UP = 0xAB00000000000000ull;
  //! IEEE 1722-2016 B.3, Table B.8: three probe intervals of 500 to 600 ms
  //! claim the new range; the bench's allocator answers after this walk
  static constexpr uint32_t WALK_MS = 1700;
  void probe(Hn& h, int src, int status, uint64_t da, uint64_t sid,
             const char* tag);
  void tx_state(Hn& h, int src, uint64_t da, uint64_t sid, const char* tag);
  //! src declares on a probe, then a conflict hits it; the grant follows
  //! at once (auto_grant). Returns the Stream ID the grant declared.
  uint64_t move_once(Hn& h, int src, const char* tag);
  bool all_distinct(const Hn& h) const;
  void conflict_moves_and_declares();
  void provisioned_collision();
  void configured_id_stays_excluded();
  void other_mac_is_no_collision();
  void declared_id_moves_after_a_withdrawal();
  void cursor_lap();
};

void SidChecks::probe(Hn& h, int src, int status, uint64_t da, uint64_t sid,
                      const char* tag) {
  h.resps.clear();
  CHECK(h.send(MT_PROBE, src, C1, 0x600 + src, L1, 8, FL_FC),
        "%s src%d consumed", tag, src);
  CHECK(h.resps.size() == 1 && h.resps[0].status == status
        && h.resps[0].da == da && h.resps[0].sid == sid
        && h.resps[0].vlan == (status == ST_OK ? VID : 0),
        "%s src%d status/tuple (status %u sid %016llx)", tag, src,
        h.resps.empty() ? 99u : unsigned(h.resps[0].status),
        static_cast<unsigned long long>(h.resps.empty() ? 0 : h.resps[0].sid));
  h.resps.clear();
}

void SidChecks::tx_state(Hn& h, int src, uint64_t da, uint64_t sid,
                         const char* tag) {
  h.resps.clear();
  CHECK(h.send(MT_GTXS, src, C1, 0x680 + src, 0, 0, 0),
        "%s src%d consumed", tag, src);
  CHECK(h.resps.size() == 1 && h.resps[0].status == ST_OK
        && h.resps[0].da == da && h.resps[0].sid == sid,
        "%s src%d stream_id/dest_mac (sid %016llx)", tag, src,
        static_cast<unsigned long long>(h.resps.empty() ? 0 : h.resps[0].sid));
  h.resps.clear();
}

uint64_t SidChecks::move_once(Hn& h, int src, const char* tag) {
  probe(h, src, ST_OK, h.da[src], h.sid_pub(src), tag);
  CHECK(((h.decl_mask() >> src) & 1) == 1, "%s src%d declaring", tag, src);
  h.gates.clear();
  h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = src;
  h.tick();
  h.run(120);
  uint64_t sid = 0;
  for (const auto& g : h.gates)
    if (g.open && g.src == src) sid = g.sid;
  CHECK(sid != 0, "%s src%d re-declared at the grant", tag, src);
  for (const auto& g : h.gates)
    if (g.open && g.src == src) h.da[src] = g.da;
  h.gates.clear(); h.arms.clear(); h.mreqs.clear(); h.resps.clear();
  return sid;
}

bool SidChecks::all_distinct(const Hn& h) const {
  for (int a = 0; a < N_SRC; ++a)
    for (int b = a + 1; b < N_SRC; ++b)
      if (h.sid_pub(a) == h.sid_pub(b)) return false;
  return true;
}

// S1: the whole conflict path on one declaring source, timed in ms.
void SidChecks::conflict_moves_and_declares() {
  Hn h; h.configure_the_talker_and_release_reset(); h.run(400);
  for (int s = 0; s < N_SRC; ++s) h.da[s] = da_pool(s);
  const uint32_t tc = 1000;
  h.d->now_ms_i = tc;
  probe(h, 2, ST_OK, da_pool(2), sid_of(2), "S1 before the conflict");
  CHECK(h.decl_mask() == 0x04u, "S1 src2 declaring before the conflict");
  h.auto_grant = false;                    // the claim walk holds the grant
  h.gates.clear(); h.arms.clear(); h.mreqs.clear(); h.draws.clear();
  h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = 2;
  h.tick();
  h.run(120);                              // time is held at tc
  CHECK(h.gates.size() == 1 && !h.gates[0].open && h.gates[0].src == 2
        && h.decl_mask() == 0, "S1 the conflict withdraws at once");
  REQ_TAG("REQ-ACMP-010", "DIR", "S1 no T-SRP-LEAVEALL2");
  CHECK(h.draws.empty() && h.arms.empty(),
        "S1 no T-SRP-LEAVEALL2 is drawn (%zu) or armed (%zu)",
        h.draws.size(), h.arms.size());
  CHECK(h.mreqs.size() == 1 && h.mreqs[0].src == 2 && !h.mreqs[0].rel,
        "S1 ALLOC_DA offered in the conflict's millisecond");
  h.gates.clear(); h.arms.clear();

  // the allocator is still probing: no valid address, no new ID yet
  h.d->now_ms_i = tc + WALK_MS / 2;
  REQ_TAG("REQ-ACMP-005", "DIR", "S1 PROBE_TX before the grant");
  probe(h, 2, ST_DMAC_FAIL, 0, 0, "S1 PROBE_TX before the grant");
  tx_state(h, 2, 0, sid_of(2), "S1 GET_TX_STATE before the grant");
  CHECK(h.sid_pub(2) == sid_of(2) && h.gates.empty(),
        "S1 src_stream_id_o holds the withdrawn ID until the grant");
  h.arms.clear();

  const uint64_t DA_NEW = 0x91E0F00007A0ull;
  const uint64_t SID_NEW = UP | 0x0008u;   // the first candidate is free
  h.d->now_ms_i = tc + WALK_MS;
  h.inject_rsp(true, DA_NEW);
  h.run(80);
  const bool opened = h.gates.size() == 1 && h.gates[0].open
                      && h.gates[0].src == 2;
  CHECK(opened && h.gates[0].da == DA_NEW && h.decl_mask() == 0x04u,
        "S1 the grant declares at once with the new DA");
  CHECK(opened && h.gates[0].sid != sid_of(2),
        "S1 the declaration carries a new Stream ID");
  CHECK(opened && h.gates[0].sid == SID_NEW,
        "S1 the new Stream ID follows the rule (got %016llx)",
        static_cast<unsigned long long>(opened ? h.gates[0].sid : 0));
  REQ_TAG("REQ-ACMP-010", "TIM", "S1 conflict to new declaration under 2 s");
  CHECK(opened && h.gates[0].ms - tc < 2000,
        "S1 conflict to new declaration under 2 s (%u ms)",
        opened ? h.gates[0].ms - tc : 0u);
  CHECK(h.sid_pub(2) == SID_NEW, "S1 src_stream_id_o follows the move");
  bool others = true;
  for (int s = 0; s < N_SRC; ++s)
    if (s != 2 && h.sid_pub(s) != sid_of(s)) others = false;
  CHECK(others, "S1 no other source's Stream ID moves");
  h.gates.clear(); h.auto_grant = true;
  REQ_TAG("REQ-ACMP-005", "DIR", "S1 PROBE_TX after the grant");
  probe(h, 2, ST_OK, DA_NEW, SID_NEW, "S1 PROBE_TX after the grant");
  REQ_TAG("REQ-ACMP-008", "DIR", "S1 GET_TX_STATE after the grant");
  tx_state(h, 2, DA_NEW, SID_NEW, "S1 GET_TX_STATE after the grant");

  // the move is for good: a disable and re-enable keep the new ID, so the
  // withdrawn one is never declared again with another DA
  h.d->cfg_src_en_i = 0xFB; h.run(200);
  h.d->cfg_src_en_i = 0xFF; h.run(400);
  CHECK(h.sid_pub(2) == SID_NEW, "S1 the move survives a disable");
  // a conflict with nothing declared moves nothing
  h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = 5;
  h.tick(); h.run(200);
  CHECK(h.sid_pub(5) == sid_of(5),
        "S1 a conflict with nothing declared keeps the Stream ID");
}

// S2: a slot provisioned with the first candidate's value is skipped.
void SidChecks::provisioned_collision() {
  Hn h; h.sid_cfg[7] = UP | 0x0008u;
  h.configure_the_talker_and_release_reset(); h.run(400);
  for (int s = 0; s < N_SRC; ++s) h.da[s] = da_pool(s);
  const uint64_t was = h.sid_pub(0);
  const uint64_t sid = move_once(h, 0, "S2");
  CHECK(sid == (UP | 0x0009u) && h.sid_pub(0) == sid,
        "S2 a provisioned collision with the first candidate is skipped "
        "(got %016llx)", static_cast<unsigned long long>(sid));
  CHECK(sid != was && all_distinct(h),
        "S2 the new ID differs from the withdrawn one and every current ID");
}

// S3: another source's configured ID stays excluded after it moved off it,
// so a move never re-declares an ID that source withdrew moments before.
void SidChecks::configured_id_stays_excluded() {
  Hn h; h.sid_cfg[7] = UP | 0x0009u;
  h.configure_the_talker_and_release_reset(); h.run(400);
  for (int s = 0; s < N_SRC; ++s) h.da[s] = da_pool(s);
  const uint64_t s7 = move_once(h, 7, "S3 src7");
  CHECK(s7 == (UP | 0x0008u), "S3 src7 takes the first candidate");
  const uint64_t s0 = move_once(h, 0, "S3 src0");
  CHECK(s0 == (UP | 0x000Au),
        "S3 a move never takes another source's configured ID after that "
        "source moved off it (got %016llx)", static_cast<unsigned long long>(s0));
  CHECK(all_distinct(h), "S3 every current ID distinct");
}

// S4: an equal unique ID under another upper 48 bits is another ID.
void SidChecks::other_mac_is_no_collision() {
  Hn h; h.sid_cfg[7] = 0xCD00000000000008ull;
  h.configure_the_talker_and_release_reset(); h.run(400);
  for (int s = 0; s < N_SRC; ++s) h.da[s] = da_pool(s);
  const uint64_t s0 = move_once(h, 0, "S4");
  CHECK(s0 == (UP | 0x0008u),
        "S4 an equal unique ID under another MAC is not a collision "
        "(got %016llx)", static_cast<unsigned long long>(s0));
}

// S6: any conflict books the move once the current ID has been declared,
// also after the declaration was withdrawn on a lapsed gate: the new DA is
// never declared under an ID that was on the wire with the old one. An ID
// not declared since its move is not moved again.
void SidChecks::declared_id_moves_after_a_withdrawal() {
  Hn h; h.configure_the_talker_and_release_reset(); h.run(400);
  for (int s = 0; s < N_SRC; ++s) h.da[s] = da_pool(s);
  // the event path declares: a Listener registers, then leaves
  h.set_lsn(4, LSN_READY);
  CHECK(h.decl_mask() == 0x10u, "S6 src4 declares on a registered listener");
  h.set_lsn(4, LSN_NONE);
  CHECK(h.decl_mask() == 0, "S6 src4 withdraws when the listener leaves");
  h.gates.clear();
  h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = 4;
  h.tick(); h.run(200);
  CHECK(h.sid_pub(4) == (UP | 0x0008u) && h.gates.empty(),
        "S6 a conflict after a listener withdrawal moves the ID at the grant "
        "(got %016llx)", static_cast<unsigned long long>(h.sid_pub(4)));
  h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = 4;
  h.tick(); h.run(200);
  CHECK(h.sid_pub(4) == (UP | 0x0008u),
        "S6 an ID not declared since its move is not moved again");
  // the command path declares: a probe, then its freshness lapses
  h.d->now_ms_i = 2000;
  h.gates.clear(); h.mreqs.clear();
  probe(h, 4, ST_OK, da_pool(9), UP | 0x0008u, "S6 PROBE_TX answers the moved ID");
  CHECK(h.gates.size() == 1 && h.gates[0].open && h.gates[0].sid == (UP | 0x0008u),
        "S6 the next declaration carries the moved ID");
  h.d->now_ms_i = 2000 + T_DAFRESH + 1;
  h.fire_expiry(4); h.run(40);
  CHECK(h.decl_mask() == 0, "S6 src4 withdraws when its freshness lapses");
  h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = 4;
  h.tick(); h.run(200);
  CHECK(h.sid_pub(4) == (UP | 0x0009u),
        "S6 a conflict after a freshness lapse moves the ID (got %016llx)",
        static_cast<unsigned long long>(h.sid_pub(4)));
}

// S5: one full lap of the 16-bit cursor. src1 moves once, then src0 moves
// until the cursor wraps: inside the lap no unique ID repeats, every move
// differs from the withdrawn and every current ID, and the first move after
// the wrap skips every configured ID and src1's moved one.
void SidChecks::cursor_lap() {
  Hn h; h.configure_the_talker_and_release_reset(); h.run(400);
  for (int s = 0; s < N_SRC; ++s) h.da[s] = da_pool(s);
  const uint64_t s1 = move_once(h, 1, "S5 src1");
  CHECK(s1 == (UP | 0x0008u), "S5 src1 moves to the first candidate");
  h.set_lsn(0, LSN_READY);                 // demand at every grant, no probes
  h.gates.clear();
  std::vector<bool> used(65536, false);
  for (int s = 0; s < N_SRC; ++s) used[h.sid_pub(s) & 0xFFFF] = true;
  bool fresh_ids = true, distinct = true, declared = true;
  uint64_t prev = h.sid_pub(0);
  uint64_t after_wrap = 0;
  int moves = 0;
  for (; moves < 70000 && after_wrap == 0; ++moves) {
    h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = 0;
    h.tick();
    uint64_t sid = 0;
    for (int i = 0; i < 200 && sid == 0; ++i) {
      h.tick();
      for (const auto& g : h.gates)
        if (g.open && g.src == 0) sid = g.sid;
    }
    h.gates.clear(); h.mreqs.clear();
    if (sid == 0 || h.sid_pub(0) != sid) { declared = false; break; }
    if (sid == prev || !all_distinct(h)) distinct = false;
    const unsigned lo = unsigned(sid & 0xFFFF), plo = unsigned(prev & 0xFFFF);
    if (lo < plo && plo >= 0xFFF0u) after_wrap = sid;   // the cursor passed 0xFFFF
    else if (used[lo]) fresh_ids = false;
    used[lo] = true;
    prev = sid;
    if (!fresh_ids || !distinct) break;
  }
  printf("S5: %d moves of src0 up to and including the wrap\n", moves);
  CHECK(declared, "S5 every move re-declares at its grant (%d moves)", moves);
  CHECK(fresh_ids, "S5 no move repeats a unique ID inside one lap");
  CHECK(distinct, "S5 every move differs from the withdrawn and every current ID");
  CHECK(after_wrap == (UP | 0x0009u),
        "S5 after the cursor wraps a move skips every configured ID and "
        "another source's current ID (got %016llx after %d moves)",
        static_cast<unsigned long long>(after_wrap), moves);
}

void Hn::check_sid_cases() {
  SidChecks sid{checks, fails};
  sid.conflict_moves_and_declares();
  sid.provisioned_collision();
  sid.configured_id_stays_excluded();
  sid.other_mac_is_no_collision();
  sid.declared_id_moves_after_a_withdrawal();
  sid.cursor_lap();
}
