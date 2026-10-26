// SPDX-License-Identifier: CERN-OHL-W-2.0
// Issue #202: a MAAP conflict on a declaring source moves it to a new Stream
// ID at the next grant and declares at once. Expectations are written from
// the rule in 05 F05.12, never read from the DUT: the new ID keeps the
// slot's upper 48 bits and takes the first unique ID, counted up from a
// cursor that starts at P-N-STREAM-OUT (8), that is no source's current and
// no source's configured ID. Every path that declares an ID again after its
// withdrawal with another FirstValue either moves the ID or waits two
// LeaveAllTime periods (IEEE 802.1Q-2018 35.2.2.8, Table 10-7: 2 x 10 s):
// a disable of a declared source moves it; a PCP change keeps the ID and
// waits; and a move takes no ID declared within the quarantine, so an ID one
// source left is not declared by another inside the gap either. Included
// after Hn, like retry_cases.hpp.

//! Every declaration against the last withdrawal of the same ID, whichever
//! source made them: an ID declared again with another DA or VLAN must be
//! two LeaveAllTime periods after it (802.1Q-2018 35.2.2.8). The tests keep
//! one upper 48 bits, so the unique ID names the ID.
struct SidHistory {
  struct Last { bool declared; bool withdrawn; uint32_t ms; uint64_t da; int vlan; };
  std::vector<Last> by_uid = std::vector<Last>(65536, Last{false, false, 0, 0, 0});
  uint64_t open_sid[N_SRC] = {};          // what each source declares now
  bool     open_v[N_SRC] = {};
  int early = 0;                          // declarations inside the gap
  uint64_t early_sid = 0;
  uint32_t early_gap = 0;
  void see(const GateEv& g) {
    if (g.open) {
      Last& l = by_uid[g.sid & 0xFFFF];
      if (l.withdrawn && (l.da != g.da || l.vlan != g.vlan)
          && g.ms - l.ms < 2u * 10000u) {
        if (early++ == 0) { early_sid = g.sid; early_gap = g.ms - l.ms; }
      }
      l = Last{true, false, g.ms, g.da, g.vlan};
      open_sid[g.src] = g.sid; open_v[g.src] = true;
    } else if (open_v[g.src]) {
      Last& l = by_uid[open_sid[g.src] & 0xFFFF];
      l.withdrawn = true; l.ms = g.ms;
      open_v[g.src] = false;
    }
  }
  //! read every strobe the harness logged, then forget them
  void drain(Hn& h) {
    for (const auto& g : h.gates) see(g);
    h.gates.clear();
  }
};

struct SidChecks {
  int& checks;
  int& fails;
  //! the upper 48 bits every default slot shares (sid_of)
  static constexpr uint64_t UP = 0xAB00000000000000ull;
  //! IEEE 1722-2016 B.3, Table B.8: three probe intervals of 500 to 600 ms
  //! claim the new range; the bench's allocator answers after this walk
  static constexpr uint32_t WALK_MS = 1700;
  //! IEEE 802.1Q-2018 35.2.2.8 with Table 10-7's LeaveAllTime of 10 s: the
  //! least time between a withdrawal and a same-ID declaration of another
  //! FirstValue
  static constexpr uint32_t GAP_MS = 2 * 10000;
  void probe(Hn& h, int src, int status, uint64_t da, uint64_t sid,
             const char* tag, int vlan = VID);
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
  //! step the timebase to `ms` one second per clock, so the quarantine
  //! epochs see every boundary as the processor's millisecond count does
  static void advance_to(Hn& h, uint32_t ms);
  //! disable `src` and return it; the allocator grants at once. Returns
  //! true when the return declared (a move), false when it backed off
  bool disable_and_return(Hn& h, int src, SidHistory& hist);
  void fast_disables_wait_or_move_off_the_quarantine();
  //! the last gate strobe of `src` since the logs were cleared, or nullptr
  static const GateEv* last_open(const Hn& h, int src);
  void pcp_after_a_listener_withdrawal_waits();
  void pcp_after_a_lapse_on_a_moved_id_waits();
  void pcp_pending_holds_a_probe_open();
  void disable_moves_a_declared_id();
};

void SidChecks::probe(Hn& h, int src, int status, uint64_t da, uint64_t sid,
                      const char* tag, int vlan) {
  h.resps.clear();
  CHECK(h.send(MT_PROBE, src, C1, 0x600 + src, L1, 8, FL_FC),
        "%s src%d consumed", tag, src);
  CHECK(h.resps.size() == 1 && h.resps[0].status == status
        && h.resps[0].da == da && h.resps[0].sid == sid
        && h.resps[0].vlan == (status == ST_OK ? vlan : 0),
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

// The whole conflict path on one declaring source, timed in ms.
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

  // the move is for good: a disabled source keeps its new ID, so the
  // withdrawn one is never declared again with another DA. The new ID has
  // been declared, so the return grant moves the source once more.
  h.d->cfg_src_en_i = 0xFB; h.run(200);
  CHECK(h.sid_pub(2) == SID_NEW, "S1 the move survives a disable");
  h.d->cfg_src_en_i = 0xFF; h.run(400);
  CHECK(h.sid_pub(2) == (UP | 0x0009u),
        "S1 a declared moved ID moves again after a disable (got %016llx)",
        static_cast<unsigned long long>(h.sid_pub(2)));
  // a conflict with nothing declared moves nothing
  h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = 5;
  h.tick(); h.run(200);
  CHECK(h.sid_pub(5) == sid_of(5),
        "S1 a conflict with nothing declared keeps the Stream ID");
}

// A slot provisioned with the first candidate's value is skipped.
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

// Another source's configured ID stays excluded after it moved off it,
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

// An equal unique ID under another upper 48 bits is another ID, and every
// slot publishes its own whole 64-bit ID: after reset its configured
// slice, after a move its own upper 48 bits with the new unique ID.
void SidChecks::other_mac_is_no_collision() {
  Hn h; h.sid_cfg[7] = 0xCD00000000000008ull;
  h.sid_cfg[5] = 0x0123456789ABCDEFull;   // an arbitrary provisioned value
  h.configure_the_talker_and_release_reset(); h.run(400);
  for (int s = 0; s < N_SRC; ++s) h.da[s] = da_pool(s);
  bool own = true;
  for (int s = 0; s < N_SRC; ++s)
    if (h.sid_pub(s) != h.sid_cfg[s]) own = false;
  CHECK(own, "S4 after reset every slot publishes its own whole configured ID "
        "(slot 7 %016llx, slot 5 %016llx)",
        static_cast<unsigned long long>(h.sid_pub(7)),
        static_cast<unsigned long long>(h.sid_pub(5)));
  const uint64_t s0 = move_once(h, 0, "S4");
  CHECK(s0 == (UP | 0x0008u),
        "S4 an equal unique ID under another MAC is not a collision "
        "(got %016llx)", static_cast<unsigned long long>(s0));
  CHECK(h.sid_pub(7) == 0xCD00000000000008ull
        && h.sid_pub(5) == 0x0123456789ABCDEFull,
        "S4 a move leaves the other slots' whole IDs published");
  const uint64_t s7 = move_once(h, 7, "S4 src7");
  CHECK(s7 == 0xCD00000000000009ull && h.sid_pub(7) == s7,
        "S4 a slot under other upper 48 bits keeps them when it moves "
        "(got %016llx)", static_cast<unsigned long long>(h.sid_pub(7)));
  const uint64_t s5 = move_once(h, 5, "S4 src5");
  CHECK(s5 == 0x0123456789AB000Aull && h.sid_pub(5) == s5,
        "S4 an arbitrary provisioned slot keeps its upper 48 bits when it "
        "moves (got %016llx)", static_cast<unsigned long long>(h.sid_pub(5)));
  CHECK(all_distinct(h), "S4 every current ID distinct");
}

// Any conflict books the move once the current ID has been declared,
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

// One full lap of the 16-bit cursor, the moves spaced 5 ms apart: a conflict
// move waits for a MAAP claim, and at that pace each half of the unique IDs
// takes longer than the cursor guard's three quarantines, so no move waits.
// src1, src2 and src4 move once and declare; src0 then moves until the
// cursor wraps. Near the end src2 moves off its ID and at once again, the lap
// waits one quarantine less 1 ms, and src1 moves off its ID. Inside the lap
// no unique ID repeats, every move differs from the withdrawn and every
// current ID, and no ID is declared again with another DA inside two
// LeaveAllTime periods. The first move after the wrap skips every configured
// ID, the ID src1 left moments before and the one src2 left a quarantine
// before, both in their quarantine records (a younger move does not replace
// a record), and src4's current ID.
void SidChecks::cursor_lap() {
  Hn h; h.configure_the_talker_and_release_reset(); h.run(400);
  for (int s = 0; s < N_SRC; ++s) h.da[s] = da_pool(s);
  SidHistory hist;
  const uint64_t s1 = move_once(h, 1, "S5 src1");
  const uint64_t s2 = move_once(h, 2, "S5 src2");
  const uint64_t s4 = move_once(h, 4, "S5 src4");
  CHECK(s1 == (UP | 0x0008u) && s2 == (UP | 0x0009u) && s4 == (UP | 0x000Au),
        "S5 src1, src2 and src4 move to the first three candidates");
  // move_once has read its strobes: the history takes their declarations
  for (int s : {1, 2, 4})
    hist.see(GateEv{true, s, h.sid_pub(s), h.da[s], VID, h.d->now_ms_i});
  h.set_lsn(0, LSN_READY);                 // demand at every grant, no probes
  h.gates.clear();
  std::vector<bool> used(65536, false);
  for (int s = 0; s < N_SRC; ++s) used[h.sid_pub(s) & 0xFFFF] = true;
  bool fresh_ids = true, distinct = true, declared = true, left = false;
  uint64_t prev = h.sid_pub(0);
  uint64_t after_wrap = 0;
  int moves = 0;
  uint32_t now = h.d->now_ms_i;
  auto conflict = [&](int src) {
    h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = src;
    h.tick(); h.run(200);
    hist.drain(h); h.mreqs.clear();
    used[h.sid_pub(src) & 0xFFFF] = true;
  };
  for (; moves < 70000 && after_wrap == 0; ++moves) {
    now += 5; h.d->now_ms_i = now;
    h.d->maap_conflict_valid_i = 1; h.d->maap_conflict_src_i = 0;
    h.tick();
    uint64_t sid = 0;
    for (int i = 0; i < 200 && sid == 0; ++i) {
      h.tick();
      for (const auto& g : h.gates)
        if (g.open && g.src == 0) sid = g.sid;
    }
    hist.drain(h); h.mreqs.clear();
    if (sid == 0 || h.sid_pub(0) != sid) { declared = false; break; }
    if (sid == prev || !all_distinct(h)) distinct = false;
    const unsigned lo = unsigned(sid & 0xFFFF), plo = unsigned(prev & 0xFFFF);
    if (lo < plo && plo >= 0xFFF0u) after_wrap = sid;   // the cursor passed 0xFFFF
    else if (used[lo]) fresh_ids = false;
    used[lo] = true;
    prev = sid;
    if (!fresh_ids || !distinct) break;
    if (!left && lo >= 0xFFF0u) {
      // src2 leaves its ID and, declaring again, the next one at once; one
      // quarantine less 1 ms later src1 leaves its ID
      h.set_lsn(2, LSN_READY);
      conflict(2); conflict(2);
      advance_to(h, now + 29999); now = h.d->now_ms_i;
      conflict(1);
      left = true;
    }
  }
  printf("S5: %d moves of src0 up to and including the wrap\n", moves);
  CHECK(declared, "S5 every move re-declares at its grant (%d moves)", moves);
  CHECK(fresh_ids, "S5 no move repeats a unique ID inside one lap");
  CHECK(distinct, "S5 every move differs from the withdrawn and every current ID");
  REQ_TAG("REQ-ACMP-010", "DIR", "S5 no ID is declared again with another DA");
  CHECK(hist.early == 0,
        "S5 no ID is declared again with another DA inside two LeaveAllTime "
        "periods (%d, first %016llx after %u ms)", hist.early,
        static_cast<unsigned long long>(hist.early_sid), hist.early_gap);
  CHECK(after_wrap == (UP | 0x000Bu),
        "S5 after the cursor wraps a move skips every configured ID, the IDs "
        "other sources left inside the quarantine and another source's "
        "current ID (got %016llx after %d moves)",
        static_cast<unsigned long long>(after_wrap), moves);
}

void SidChecks::advance_to(Hn& h, uint32_t ms) {
  while (h.d->now_ms_i < ms) {
    h.d->now_ms_i = (ms - h.d->now_ms_i > 1000) ? h.d->now_ms_i + 1000 : ms;
    h.tick();
  }
  h.run(20);
}

bool SidChecks::disable_and_return(Hn& h, int src, SidHistory& hist) {
  const uint32_t all = 0xFFu, off = all & ~(1u << src);
  h.d->cfg_src_en_i = off;
  for (int i = 0; i < 400 && (h.mreqs.empty() || !h.mreqs.back().rel); ++i) h.tick();
  h.run(4);
  h.d->cfg_src_en_i = all;
  h.draws.clear(); h.arms.clear();
  bool opened = false;
  for (int i = 0; i < 400 && !opened && h.arms.empty(); ++i) {
    h.tick();
    for (const auto& g : h.gates)
      if (g.open && g.src == src) opened = true;
  }
  h.run(4);
  hist.drain(h); h.mreqs.clear();
  return opened;
}

// A declared source disabled and returned 65,000-odd times
// inside one millisecond, each return granted at once with another DA. The
// cursor crosses into the upper half of the unique IDs and on to its end; the
// return whose search would re-enter the lower half, left moments before,
// finds no ID: the source keeps its ID, and the new DA waits
// T-SRP-LEAVEALL2 from that grant. Three quarantine epochs later a return
// still waits; once the cursor left that half four epochs ago, a return
// moves again. No ID is declared again with another DA inside two
// LeaveAllTime periods.
void SidChecks::fast_disables_wait_or_move_off_the_quarantine() {
  Hn h; h.configure_the_talker_and_release_reset(); h.run(400);
  SidHistory hist;
  h.d->now_ms_i = 1000;
  h.set_lsn(3, LSN_READY);                 // demand at every return, no probes
  hist.drain(h);
  CHECK(hist.open_v[3] && hist.open_sid[3] == sid_of(3),
        "S11 src3 declares its configured ID");
  int moves = 0;
  bool distinct = true;
  uint64_t last = h.sid_pub(3);
  bool moved = true;
  for (; moves < 70000; ++moves) {
    moved = disable_and_return(h, 3, hist);
    if (!moved) break;
    if (h.sid_pub(3) == last || !all_distinct(h)) distinct = false;
    last = h.sid_pub(3);
  }
  printf("S11: %d disable-triggered moves before the guard\n", moves);
  CHECK(moves == 0xFFFF - 8 + 1 && last == (UP | 0xFFFFu),
        "S11 every return moves until the cursor ends the upper half "
        "(%d moves, last %016llx)", moves, static_cast<unsigned long long>(last));
  CHECK(distinct, "S11 every move differs from the withdrawn and every current ID");
  const uint32_t tg = h.d->now_ms_i;
  REQ_TAG("REQ-ACMP-010", "DIR", "S11 a return the cursor guard blocks keeps its ID and waits");
  CHECK(!moved && h.sid_pub(3) == last && h.draws.size() == 1
        && h.arms.size() == 1 && !h.arms[0].cancel
        && h.arms[0].dl == tg + 2 * h.draw_val && h.decl_mask() == 0,
        "S11 a return the cursor guard blocks keeps its ID and waits "
        "T-SRP-LEAVEALL2 (%zu draws, %zu arms, ID %016llx)",
        h.draws.size(), h.arms.size(), static_cast<unsigned long long>(h.sid_pub(3)));
  const uint64_t da_new = h.da[3] = da_pool(h.da_seq - 1);
  h.d->now_ms_i = tg + 1;
  probe(h, 3, ST_OK, da_new, last,
        "S11 PROBE_TX inside the gap answers the kept ID and the new DA");
  hist.drain(h);
  CHECK(!hist.open_v[3], "S11 nothing is declared inside the gap");
  advance_to(h, tg + 2 * h.draw_val);
  h.fire_expiry(3); h.run(20);
  hist.drain(h);
  CHECK(hist.open_v[3] && hist.open_sid[3] == last,
        "S11 after the gap the kept ID declares the new DA");
  // three epochs after the cursor left the lower half: still no ID
  advance_to(h, 100000);
  CHECK(!disable_and_return(h, 3, hist) && h.sid_pub(3) == last,
        "S11 three quarantine epochs after the cursor left the lower half a "
        "return still keeps its ID");
  advance_to(h, 125000);
  const bool back = disable_and_return(h, 3, hist);
  CHECK(back && h.sid_pub(3) == (UP | 0x0008u),
        "S11 four quarantine epochs after the cursor left the lower half a "
        "return moves again (got %016llx)",
        static_cast<unsigned long long>(h.sid_pub(3)));
  REQ_TAG("REQ-ACMP-010", "DIR", "S11 no ID is declared again with another DA");
  CHECK(hist.early == 0,
        "S11 no ID is declared again with another DA inside two LeaveAllTime "
        "periods (%d, first %016llx after %u ms)", hist.early,
        static_cast<unsigned long long>(hist.early_sid), hist.early_gap);
}

const GateEv* SidChecks::last_open(const Hn& h, int src) {
  const GateEv* g = nullptr;
  for (const auto& e : h.gates)
    if (e.open && e.src == src) g = &e;
  return g;
}

// A PCP change after the configured ID was withdrawn on a lapsed gate keeps
// the ID, so the next declaration waits the gap, counted from the change.
// Neither a returning listener nor a probe declares the changed FirstValue
// (here the Domain's new VID) under the same ID inside it.
void SidChecks::pcp_after_a_listener_withdrawal_waits() {
  Hn h; h.configure_the_talker_and_release_reset(); h.run(400);
  h.d->now_ms_i = 1000;
  h.set_lsn(0, LSN_READY);
  CHECK(h.decl_mask() == 0x01u, "S7 src0 declares on a registered listener");
  h.d->now_ms_i = 1010;
  h.gates.clear();
  h.set_lsn(0, LSN_NONE);
  CHECK(h.decl_mask() == 0 && h.gates.size() == 1 && !h.gates[0].open,
        "S7 src0 withdraws when the listener leaves");
  const uint32_t tw = h.gates.empty() ? 0 : h.gates[0].ms;
  h.gates.clear(); h.arms.clear(); h.draws.clear();
  const uint32_t tp = 1011;
  h.d->now_ms_i = tp; h.draw_val = 11000; h.d->srp_class_vid_i = VID + 1;
  h.d->srp_pcp_change_i = 1; h.tick(); h.run(40);
  REQ_TAG("REQ-ACMP-010", "DIR", "S7 a PCP change after a listener withdrawal arms T-SRP-LEAVEALL2");
  CHECK(h.draws.size() == 1 && h.arms.size() == 1 && !h.arms[0].cancel
        && h.arms[0].slot == TMR_BASE && h.arms[0].dl == tp + 2 * 11000
        && h.gates.empty(),
        "S7 a PCP change after a listener withdrawal arms T-SRP-LEAVEALL2 "
        "(%zu draws, %zu arms)", h.draws.size(), h.arms.size());
  h.arms.clear(); h.draws.clear();
  h.d->now_ms_i = tp + 1;
  h.set_lsn(0, LSN_READY);
  CHECK(h.gates.empty() && h.decl_mask() == 0,
        "S7 a returning listener does not declare the same ID inside the gap");
  h.d->now_ms_i = tp + 2;
  probe(h, 0, ST_OK, da_pool(0), sid_of(0),
        "S7 PROBE_TX inside the gap answers the kept ID and DA", VID + 1);
  CHECK(h.gates.empty() && h.decl_mask() == 0,
        "S7 a probe does not declare the same ID inside the gap");
  h.d->now_ms_i = tp + 2 * 11000;
  h.fire_expiry(0); h.run(20);
  const GateEv* g = last_open(h, 0);
  CHECK(g && g->sid == sid_of(0) && g->da == da_pool(0) && g->vlan == VID + 1,
        "S7 after the gap the same ID declares the changed FirstValue");
  REQ_TAG("REQ-ACMP-010", "DIR", "S7 two LeaveAllTime periods separate the withdrawal and the same-ID declaration");
  CHECK(g && g->ms - tw >= GAP_MS,
        "S7 two LeaveAllTime periods separate the withdrawal and the same-ID declaration"
        " (%u ms)", g ? g->ms - tw : 0u);
}

// The same on a conflict-moved ID declared by a probe and withdrawn when its
// freshness lapsed: the moved ID is kept through the PCP change and waits.
void SidChecks::pcp_after_a_lapse_on_a_moved_id_waits() {
  Hn h; h.configure_the_talker_and_release_reset(); h.run(400);
  for (int s = 0; s < N_SRC; ++s) h.da[s] = da_pool(s);
  const uint32_t t0 = 30000;
  h.d->now_ms_i = t0;
  const uint64_t mv = move_once(h, 2, "S8");
  CHECK(mv == (UP | 0x0008u) && h.sid_pub(2) == mv,
        "S8 src2 declares its moved ID (got %016llx)",
        static_cast<unsigned long long>(mv));
  h.d->now_ms_i = t0 + T_DAFRESH + 1;
  h.gates.clear();
  h.fire_expiry(2); h.run(40);
  CHECK(h.decl_mask() == 0 && h.gates.size() == 1 && !h.gates[0].open
        && h.gates[0].src == 2,
        "S8 the moved ID withdraws when its freshness lapses");
  const uint32_t tw = h.gates.empty() ? 0 : h.gates[0].ms;
  h.gates.clear(); h.arms.clear(); h.draws.clear();
  const uint32_t tp = tw + 1;
  h.d->now_ms_i = tp; h.draw_val = 10000; h.d->srp_class_vid_i = VID + 1;
  h.d->srp_pcp_change_i = 1; h.tick(); h.run(40);
  CHECK(h.draws.size() == 1 && h.arms.size() == 1 && !h.arms[0].cancel
        && h.arms[0].slot == TMR_BASE + 2 && h.arms[0].dl == tp + 2 * 10000
        && h.gates.empty(),
        "S8 a PCP change after a lapse arms T-SRP-LEAVEALL2 on the moved ID "
        "(%zu draws, %zu arms)", h.draws.size(), h.arms.size());
  h.arms.clear(); h.draws.clear();
  h.d->now_ms_i = tp + 1;
  probe(h, 2, ST_OK, h.da[2], mv,
        "S8 PROBE_TX inside the gap answers the moved ID and DA", VID + 1);
  CHECK(h.gates.empty() && h.decl_mask() == 0,
        "S8 a probe does not declare the moved ID inside the gap");
  h.d->now_ms_i = tp + 2;
  h.set_lsn(2, LSN_READY);
  CHECK(h.gates.empty() && h.decl_mask() == 0,
        "S8 a returning listener does not declare the moved ID inside the gap");
  h.d->now_ms_i = tp + 2 * 10000;
  h.fire_expiry(2); h.run(20);
  const GateEv* g = last_open(h, 2);
  CHECK(g && g->sid == mv && g->da == h.da[2] && g->vlan == VID + 1,
        "S8 after the gap the moved ID declares the changed FirstValue");
  CHECK(g && g->ms - tw >= GAP_MS,
        "S8 two LeaveAllTime periods separate the withdrawal and the moved "
        "ID's declaration (%u ms)", g ? g->ms - tw : 0u);
}

// A PCP change that arrives with a probe: the command is served ahead of
// the pending event, and must not declare the changed FirstValue under the
// withdrawn ID before the event has started the gap.
void SidChecks::pcp_pending_holds_a_probe_open() {
  Hn h; h.configure_the_talker_and_release_reset(); h.run(400);
  h.d->now_ms_i = 1000;
  h.set_lsn(5, LSN_READY);
  CHECK(h.decl_mask() == 0x20u, "S9 src5 declares on a registered listener");
  h.set_lsn(5, LSN_NONE);
  CHECK(h.decl_mask() == 0, "S9 src5 withdraws when the listener leaves");
  h.gates.clear(); h.arms.clear(); h.draws.clear(); h.resps.clear();
  h.d->now_ms_i = 1002; h.draw_val = 10000;
  h.d->srp_pcp_change_i = 1;               // in the probe's first cycle
  CHECK(h.send(MT_PROBE, 5, C1, 0x6A5, L1, 8, FL_FC), "S9 src5 consumed");
  h.run(40);
  REQ_TAG("REQ-ACMP-010", "DIR", "S9 a probe served with a PCP change pending does not declare the same ID");
  CHECK(h.gates.empty() && h.decl_mask() == 0,
        "S9 a probe served with a PCP change pending does not declare the same ID"
        " (%zu strobes)", h.gates.size());
  CHECK(h.draws.size() == 1, "S9 the pending PCP change then starts the gap");
}

// A disable of a source whose ID has been declared books the move: the
// release hands the DA back and the allocator may grant another one, so the
// source returns under a new Stream ID whatever DA it gets. A source that
// never declared keeps its configured ID.
void SidChecks::disable_moves_a_declared_id() {
  Hn h; h.configure_the_talker_and_release_reset(); h.run(400);
  h.d->now_ms_i = 1000;
  probe(h, 3, ST_OK, da_pool(3), sid_of(3), "S10 src3 declares its configured ID");
  CHECK(h.decl_mask() == 0x08u, "S10 src3 declaring");
  h.gates.clear(); h.mreqs.clear(); h.arms.clear();
  h.d->cfg_src_en_i = 0xF7; h.run(200);
  CHECK(h.gates.size() == 1 && !h.gates[0].open && h.gates[0].src == 3,
        "S10 the disable withdraws src3");
  CHECK(h.sid_pub(3) == sid_of(3), "S10 a disabled source keeps its ID while out");
  h.gates.clear();
  h.d->cfg_src_en_i = 0xFF; h.run(400);   // the allocator grants a fresh DA
  REQ_TAG("REQ-ACMP-010", "DIR", "S10 a declared source returns from a disable under a new Stream ID");
  CHECK(h.sid_pub(3) == (UP | 0x0008u),
        "S10 a declared source returns from a disable under a new Stream ID "
        "(got %016llx)", static_cast<unsigned long long>(h.sid_pub(3)));
  h.d->now_ms_i = 1500;                    // 500 ms after the withdrawal
  probe(h, 3, ST_OK, da_pool(8), UP | 0x0008u,
        "S10 PROBE_TX after the return answers the new ID and DA");
  const GateEv* g = last_open(h, 3);
  CHECK(g && g->sid == (UP | 0x0008u) && g->da == da_pool(8),
        "S10 the new DA is declared under the new ID, never the withdrawn one");
  // a block allocator gives the same DA back: the ID moves all the same
  h.block_count = N_SRC; h.block_base = da_pool(0);
  probe(h, 5, ST_OK, da_pool(5), sid_of(5), "S10 src5 declares its configured ID");
  h.d->cfg_src_en_i = 0xDF; h.run(200);
  h.d->cfg_src_en_i = 0xFF; h.run(400);
  CHECK(h.sid_pub(5) == (UP | 0x0009u),
        "S10 the move does not depend on the returned DA (got %016llx)",
        static_cast<unsigned long long>(h.sid_pub(5)));
  probe(h, 5, ST_OK, da_pool(5), UP | 0x0009u,
        "S10 PROBE_TX after the same DA returns answers the new ID");
  h.d->cfg_src_en_i = 0xBF; h.run(200);
  h.d->cfg_src_en_i = 0xFF; h.run(400);
  CHECK(h.sid_pub(6) == sid_of(6),
        "S10 a source that never declared returns under its configured ID");
  CHECK(all_distinct(h), "S10 every current ID distinct");
}

void Hn::check_sid_cases() {
  SidChecks sid{checks, fails};
  sid.conflict_moves_and_declares();
  sid.provisioned_collision();
  sid.configured_id_stays_excluded();
  sid.other_mac_is_no_collision();
  sid.declared_id_moves_after_a_withdrawal();
  sid.cursor_lap();
  sid.pcp_after_a_listener_withdrawal_waits();
  sid.pcp_after_a_lapse_on_a_moved_id_waits();
  sid.pcp_pending_holds_a_probe_open();
  sid.disable_moves_a_declared_id();
  sid.fast_disables_wait_or_move_off_the_quarantine();
}
