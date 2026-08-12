// SPDX-License-Identifier: CERN-OHL-W-2.0
// F08.4 timer-slot map + 02 §5 event-router map suite.
//
// The expectations below are the DOCUMENT, re-derived here from the 08 §5
// F08.4 table and the 02 §5 source catalog — never read back from the RTL.
// What is graded, per shape:
//
//   1. every base equals the independent re-derivation;
//   2. the groups are PAIRWISE DISJOINT — the property the literal map lost,
//      checked as real interval overlap over every pair, not as a spot check;
//   3. the map fits P-TIMER-SLOTS and the derived index width can address it;
//   4. the 8x8 shape still reproduces the historical literal map EXACTLY
//      (0/1/9/17/25/57/61/66/73/81, 89 slots; event sources 0/8/16/17/18/19/
//      27/28, 29 total) — the shipping shape must not move;
//   5. THE REGRESSION, at 9 sinks and 9 sources: the two collisions the
//      literal map had are named and shown gone —
//        listener sink SI-1 vs the talker base   (distinct owner tags, so the
//                                                 loser's deadline was lost)
//        SRP talker SO-1 vs the SRP listener base (no owner discrimination,
//                                                 so the expiry was
//                                                 MISDELIVERED)
//        event TK_ATTR_UNREGISTERED{SI-1} vs ADP EVT_TK_DISCOVERED (the
//                                                 router carries no owner
//                                                 tag at all);
//   6. the owner-tag allocation is disjoint at every legal shape, and the
//      first shape past it is named (that shape is refused at elaboration —
//      shape_elab.sh proves the refusal, this proves WHERE it starts).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "Vtimer_map_wrap.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// ---- F01.5 shape constants (the document, not the DUT) -----------------
static const unsigned N_IF = 1, N_CTRL = 16, CA_POOL = 4;
static const unsigned SRP_CAD_SLOTS = 7;   // JOIN x2 + LEAVEALL x2 + PERIODIC
                                           // + Domain and MVRP VID registrars
static const unsigned SINGLETONS = 5;      // LOCK, IDENT-BURST, IDENT-REARM,
                                           // CTR-OBSERVE, NVM-DEBOUNCE

// ---- 08 §5 F08.4 owner-tag allocation ----------------------------------
static const unsigned OWN_LSTN = 0x20, OWN_SRP_TK = 0x40, OWN_TKR = 0x50,
                      OWN_SRP_LS = 0x60, OWN_SRP_CAD = 0x80;

// map field order, mirroring the wrap's port order
enum { M_ADV, M_NOADP, M_LSTN, M_TKR, M_REGMON, M_CAPOOL, M_SINGLE,
       M_BASE_END, M_SRP_CAD, M_SRP_TK, M_SRP_LS, M_SRP_END, M_SLOT_AW };
enum { E_TKREG, E_TKUNR, E_DISC, E_DEP, E_DOMAIN, E_LSNCHG, E_GM, E_LINK,
       E_NSRC, E_SRCW };

static unsigned clog2_of(unsigned v) {
  unsigned w = 0;
  while ((1u << w) < v) ++w;
  return w;
}

// one half-open interval of indices owned by one group
struct Grp { const char* name; unsigned base, count; };

// pairwise overlap over a group list — the property, not a spot check
static void check_disjoint(const char* tag, const std::vector<Grp>& g) {
  for (size_t i = 0; i < g.size(); ++i) {
    for (size_t j = i + 1; j < g.size(); ++j) {
      if (g[i].count == 0 || g[j].count == 0) { ++checks; continue; }
      bool ok = (g[i].base + g[i].count <= g[j].base)
                || (g[j].base + g[j].count <= g[i].base);
      CHECK(ok, "%s: %s [%u,%u) OVERLAPS %s [%u,%u)", tag,
            g[i].name, g[i].base, g[i].base + g[i].count,
            g[j].name, g[j].base, g[j].base + g[j].count);
    }
  }
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* d = new Vtimer_map_wrap;
  d->eval();

  const int N_SHAPE = 10;
  int ix_8x8 = -1, ix_9x9 = -1;

  for (int g = 0; g < N_SHAPE; ++g) {
    const unsigned si = d->si_o[g], so = d->so_o[g];
    char tag[64];
    snprintf(tag, sizeof tag, "SI=%u SO=%u", si, so);
    if (si == 8 && so == 8) ix_8x8 = g;
    if (si == 9 && so == 9) ix_9x9 = g;

    // ---- 1. the F08.4 running sum, re-derived from the table ----------
    const unsigned e_adv    = 0;
    const unsigned e_noadp  = e_adv    + N_IF;
    const unsigned e_lstn   = e_noadp  + si;
    const unsigned e_tkr    = e_lstn   + si;
    const unsigned e_regmon = e_tkr    + so;
    const unsigned e_capool = e_regmon + 2 * N_CTRL * N_IF;
    const unsigned e_single = e_capool + CA_POOL;
    const unsigned e_bend   = e_single + SINGLETONS;
    const unsigned e_cad    = e_bend;
    const unsigned e_srptk  = e_cad    + SRP_CAD_SLOTS;
    const unsigned e_srpls  = e_srptk  + so;
    const unsigned e_end    = e_cad    + (SRP_CAD_SLOTS + si + so) * N_IF;

    const uint32_t* m = &d->map_o[g][0];
    CHECK(m[M_ADV]      == e_adv,    "%s adp_adv %u want %u",    tag, m[M_ADV], e_adv);
    CHECK(m[M_NOADP]    == e_noadp,  "%s adp_noadp %u want %u",  tag, m[M_NOADP], e_noadp);
    CHECK(m[M_LSTN]     == e_lstn,   "%s lstn %u want %u",       tag, m[M_LSTN], e_lstn);
    CHECK(m[M_TKR]      == e_tkr,    "%s tkr %u want %u",        tag, m[M_TKR], e_tkr);
    CHECK(m[M_REGMON]   == e_regmon, "%s regmon %u want %u",     tag, m[M_REGMON], e_regmon);
    CHECK(m[M_CAPOOL]   == e_capool, "%s capool %u want %u",     tag, m[M_CAPOOL], e_capool);
    CHECK(m[M_SINGLE]   == e_single, "%s single %u want %u",     tag, m[M_SINGLE], e_single);
    CHECK(m[M_BASE_END] == e_bend,   "%s base_end %u want %u",   tag, m[M_BASE_END], e_bend);
    CHECK(m[M_SRP_CAD]  == e_cad,    "%s srp_cad %u want %u",    tag, m[M_SRP_CAD], e_cad);
    CHECK(m[M_SRP_TK]   == e_srptk,  "%s srp_tk %u want %u",     tag, m[M_SRP_TK], e_srptk);
    CHECK(m[M_SRP_LS]   == e_srpls,  "%s srp_ls %u want %u",     tag, m[M_SRP_LS], e_srpls);
    CHECK(m[M_SRP_END]  == e_end,    "%s srp_end %u want %u",    tag, m[M_SRP_END], e_end);

    // ---- 2. pairwise disjointness of every SLOT group ------------------
    check_disjoint(tag, {
        {"adp_adv",   m[M_ADV],     N_IF},
        {"adp_noadp", m[M_NOADP],   si},
        {"lstn",      m[M_LSTN],    si},
        {"tkr",       m[M_TKR],     so},
        {"regmon",    m[M_REGMON],  2 * N_CTRL * N_IF},
        {"capool",    m[M_CAPOOL],  CA_POOL},
        {"single",    m[M_SINGLE],  SINGLETONS},
        {"srp_cad",   m[M_SRP_CAD], SRP_CAD_SLOTS},
        {"srp_tk",    m[M_SRP_TK],  so},
        {"srp_ls",    m[M_SRP_LS],  si}});

    // ---- 3. the map fits, and the index width can address it ----------
    CHECK(m[M_SRP_LS] + si <= m[M_SRP_END],
          "%s last group ends at %u past P-TIMER-SLOTS %u", tag,
          m[M_SRP_LS] + si, m[M_SRP_END]);
    CHECK(m[M_SLOT_AW] == clog2_of(m[M_SRP_END]),
          "%s slot width %u want %u", tag, m[M_SLOT_AW],
          clog2_of(m[M_SRP_END]));
    CHECK(m[M_SRP_END] <= (1u << m[M_SLOT_AW]),
          "%s %u slots cannot be indexed by %u bits", tag, m[M_SRP_END],
          m[M_SLOT_AW]);

    // ---- 1b/2b. the 02 §5 event-router source map ---------------------
    const unsigned f_reg = 0, f_unr = f_reg + si, f_disc = f_unr + si,
                   f_dep = f_disc + 1, f_dom = f_dep + 1, f_lsn = f_dom + 1,
                   f_gm = f_lsn + so, f_link = f_gm + 1, f_n = f_link + 1;
    const uint32_t* e = &d->evr_o[g][0];
    CHECK(e[E_TKREG]  == f_reg,  "%s evr tk_reg %u want %u",   tag, e[E_TKREG], f_reg);
    CHECK(e[E_TKUNR]  == f_unr,  "%s evr tk_unreg %u want %u", tag, e[E_TKUNR], f_unr);
    CHECK(e[E_DISC]   == f_disc, "%s evr adp_disc %u want %u", tag, e[E_DISC], f_disc);
    CHECK(e[E_DEP]    == f_dep,  "%s evr adp_dep %u want %u",  tag, e[E_DEP], f_dep);
    CHECK(e[E_DOMAIN] == f_dom,  "%s evr domain %u want %u",   tag, e[E_DOMAIN], f_dom);
    CHECK(e[E_LSNCHG] == f_lsn,  "%s evr lsn_chg %u want %u",  tag, e[E_LSNCHG], f_lsn);
    CHECK(e[E_GM]     == f_gm,   "%s evr gm %u want %u",       tag, e[E_GM], f_gm);
    CHECK(e[E_LINK]   == f_link, "%s evr link %u want %u",     tag, e[E_LINK], f_link);
    CHECK(e[E_NSRC]   == f_n,    "%s evr n_src %u want %u",    tag, e[E_NSRC], f_n);
    CHECK(e[E_SRCW]   == clog2_of(f_n), "%s evr src width %u want %u", tag,
          e[E_SRCW], clog2_of(f_n));
    check_disjoint(tag, {
        {"evr tk_reg",   e[E_TKREG],  si},
        {"evr tk_unreg", e[E_TKUNR],  si},
        {"evr adp_disc", e[E_DISC],   1},
        {"evr adp_dep",  e[E_DEP],    1},
        {"evr domain",   e[E_DOMAIN], 1},
        {"evr lsn_chg",  e[E_LSNCHG], so},
        {"evr gm_chg",   e[E_GM],     1},
        {"evr link",     e[E_LINK],   1}});
    // the ACMP listener decodes the event KIND by index range alone, so the
    // whole TK block must be one contiguous run ending at adp_dep
    CHECK(e[E_DEP] + 1 == e[E_DOMAIN],
          "%s TK block is not contiguous: dep %u, domain %u", tag,
          e[E_DEP], e[E_DOMAIN]);

    // ---- 6. owner tags: disjoint at every shape the guard admits -------
    // ADP publishes its SLOT as its owner tag, so its slot block is also an
    // owner block. The listener/talker filter BY owner; SRP by slot but with
    // owner bases that must not be mistaken for theirs.
    const bool own_ok = (m[M_LSTN] <= OWN_LSTN)
                        && (OWN_LSTN + si <= OWN_SRP_TK)
                        && (OWN_SRP_TK + so <= OWN_TKR)
                        && (OWN_TKR + so <= OWN_SRP_LS)
                        && (OWN_SRP_LS + si <= OWN_SRP_CAD)
                        && (OWN_SRP_CAD + SRP_CAD_SLOTS <= 256);
    CHECK(own_ok, "%s owner tags overlap on the 8-bit expiry bus", tag);
    if (own_ok) {
      check_disjoint(tag, {
          {"own adp",     m[M_ADV],    m[M_LSTN]},
          {"own lstn",    OWN_LSTN,    si},
          {"own srp_tk",  OWN_SRP_TK,  so},
          {"own tkr",     OWN_TKR,     so},
          {"own srp_ls",  OWN_SRP_LS,  si},
          {"own srp_cad", OWN_SRP_CAD, SRP_CAD_SLOTS}});
    }
  }

  // ---- 4. the shipping 8x8 shape reproduces the historical map --------
  CHECK(ix_8x8 >= 0, "the 8x8 default shape is not in the table");
  if (ix_8x8 >= 0) {
    const uint32_t* m = &d->map_o[ix_8x8][0];
    const unsigned want[12] = {0, 1, 9, 17, 25, 57, 61, 66, 66, 73, 81, 89};
    static const char* nm[12] = {"adp_adv", "adp_noadp", "lstn", "tkr",
                                 "regmon", "capool", "single", "base_end",
                                 "srp_cad", "srp_tk", "srp_ls", "srp_end"};
    for (int k = 0; k < 12; ++k)
      CHECK(m[k] == want[k], "8x8 %s moved: %u want %u (the literal map that "
            "shipped)", nm[k], m[k], want[k]);
    CHECK(m[M_SLOT_AW] == 7, "8x8 slot width %u want 7", m[M_SLOT_AW]);
    const uint32_t* e = &d->evr_o[ix_8x8][0];
    const unsigned ewant[9] = {0, 8, 16, 17, 18, 19, 27, 28, 29};
    for (int k = 0; k < 9; ++k)
      CHECK(e[k] == ewant[k], "8x8 evr field %d moved: %u want %u", k,
            e[k], ewant[k]);
  }

  // ---- 5. THE REGRESSION: the 9x9 shape the literals aliased ----------
  CHECK(ix_9x9 >= 0, "the 9x9 shape is not in the table");
  if (ix_9x9 >= 0) {
    const uint32_t* m = &d->map_o[ix_9x9][0];
    const uint32_t* e = &d->evr_o[ix_9x9][0];
    const unsigned si = 9, so = 9;
    // the OLD literal map, written out so the collision is legible
    const unsigned lit_lstn = 9, lit_tkr = 17, lit_srp_tk = 73,
                   lit_srp_ls = 81, lit_unr = 8, lit_disc = 16;
    // the premise: with the literals, these three pairs SHARED an index
    CHECK(lit_lstn + (si - 1) == lit_tkr,
          "premise: literal listener sink 8 landed on the talker base");
    CHECK(lit_srp_tk + (so - 1) == lit_srp_ls,
          "premise: literal SRP talker 8 landed on the SRP listener base");
    CHECK(lit_unr + (si - 1) == lit_disc,
          "premise: literal event TK_ATTR_UNREGISTERED{8} landed on "
          "ADP EVT_TK_DISCOVERED");
    CHECK(m[M_LSTN] + si <= m[M_TKR],
          "9x9 listener sink %u (slot %u) still collides with the talker "
          "base %u — a SILENTLY LOST deadline", si - 1, m[M_LSTN] + si - 1,
          m[M_TKR]);
    CHECK(m[M_SRP_TK] + so <= m[M_SRP_LS],
          "9x9 SRP talker %u (slot %u) still collides with the SRP listener "
          "base %u — a MISDELIVERED expiry", so - 1, m[M_SRP_TK] + so - 1,
          m[M_SRP_LS]);
    CHECK(e[E_TKUNR] + si <= e[E_DISC],
          "9x9 event TK_ATTR_UNREGISTERED{%u} (source %u) still collides "
          "with ADP EVT_TK_DISCOVERED at %u", si - 1, e[E_TKUNR] + si - 1,
          e[E_DISC]);
    // and the whole 9x9 map still fits an addressable timer
    CHECK(m[M_SRP_END] == 94, "9x9 P-TIMER-SLOTS %u want 94", m[M_SRP_END]);
    CHECK(m[M_SLOT_AW] == 7, "9x9 slot width %u want 7", m[M_SLOT_AW]);
    CHECK(e[E_NSRC] == 32, "9x9 event sources %u want 32", e[E_NSRC]);
  }

  // ---- 6b. where the owner-tag space runs out (named, not guessed) ----
  // The talker owner base is 0x50 and SRP's listener base 0x60, so 16 is the
  // last legal P-N-STREAM-OUT; the listener base is 0x20 and ADP publishes
  // its slot as its owner, so IF + SI must stay under 0x20.
  CHECK(OWN_TKR + 16 <= OWN_SRP_LS, "16 sources must be legal");
  CHECK(OWN_TKR + 17 > OWN_SRP_LS, "17 sources must be the first refused");
  CHECK(N_IF + 31 <= OWN_LSTN, "31 sinks must fit under the listener tags");
  CHECK(N_IF + 32 > OWN_LSTN, "32 sinks must be the first refused by ADP");

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete d;
  return fails ? 1 : 0;
}
