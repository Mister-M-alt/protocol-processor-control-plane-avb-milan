// SPDX-FileCopyrightText: 2026 Kebag Logic
// SPDX-License-Identifier: CERN-OHL-W-2.0
//
// KL_aecp_dyn_state — the AECP dynamic-state store.
//
// WHAT THIS SUITE PROVES. That the store the whole SET_* family is about to
// be built on keeps a setting, keeps it PER DESCRIPTOR, tells a reader
// whether a setting exists at all, and refuses an index the shape does not
// have. Every check drives the real state port with the same handshake the
// microCPU drives (request held until ready/rvalid), because a store that
// only works when poked in a convenient order is a store that will fail in
// microcode.
//
// THE ONE THAT MATTERS MOST is the VALID FLAG (region 0x2). It is what lets a
// GET fall back to the descriptor image, so "unwritten reads back invalid"
// and "written reads back valid" are the two halves of the contract that
// every GET_* microprogram will branch on. A store that answered a plausible
// zero with the flag set would make every GET report a setting nobody made.
//
// Clauses: Milan v1.2 5.3.5.1 (sampling rate), 5.3.7.1/5.3.8.1 (formats),
// 5.3.7.6 (presentation offset), 5.3.8.7 (started/stopped), 5.3.11.1 (clock
// source), 5.3.12 (IDENTIFY, volatile, 0 after reset), 5.4.2.5 (current
// configuration).
#include <cstdio>
#include <cstdint>
#include "VKL_aecp_dyn_state.h"
#include "verilated.h"

static long checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// field selectors — mirror KL_aecp_dyn_state.sv's SEL_*_C
enum { SEL_CFG = 0, SEL_RATE = 1, SEL_CLKSRC = 2, SEL_FMTIN = 3,
       SEL_FMTOUT = 4, SEL_PTOFF = 5, SEL_START = 6, SEL_IDENT = 7 };
// regions
static const uint32_t RGN_DYN  = 0x1u << 16;   // the value
static const uint32_t RGN_DYNV = 0x2u << 16;   // the valid flag

#ifdef SHAPE_1x1
static const int NSI = 1, NSO = 1;
static const char* SHAPE = "1x1";
#else
static const int NSI = 2, NSO = 2;
static const char* SHAPE = "2x2";
#endif

static VKL_aecp_dyn_state* d;

static void tick() { d->clk_i = 0; d->eval(); d->clk_i = 1; d->eval(); }

static void reset() {
  d->rst_n = 0; d->st_req_i = 0; d->st_we_i = 0; d->st_addr_i = 0;
  d->st_wdata_i = 0; d->desc_index_i = 0;
  for (int i = 0; i < 4; i++) tick();
  d->rst_n = 1;
  tick();
}

//! address = region | selector<<3.  The DESCRIPTOR INDEX does not ride the
//! address — it is a separate port, which is the whole point of the design.
static uint32_t addr(uint32_t rgn, int sel) {
  return rgn | (uint32_t(sel) << 3);
}

static void wr(int sel, uint16_t index, uint64_t val) {
  d->st_addr_i    = addr(RGN_DYN, sel);
  d->desc_index_i = index;
  d->st_wdata_i   = val;
  d->st_we_i      = 1;
  d->st_req_i     = 1;
  // hold the request until ready, exactly as the microCPU does
  int guard = 32;
  while (guard-- > 0) { d->eval(); if (d->st_ready_o) break; tick(); }
  tick();
  d->st_req_i = 0; d->st_we_i = 0;
  d->eval();
}

static uint64_t rd(uint32_t rgn, int sel, uint16_t index) {
  d->st_addr_i    = addr(rgn, sel);
  d->desc_index_i = index;
  d->st_we_i      = 0;
  d->st_req_i     = 1;
  uint64_t got = 0;
  int guard = 32;
  while (guard-- > 0) {
    tick();
    d->eval();
    if (d->st_rvalid_o) { got = d->st_rdata_o; break; }
  }
  d->st_req_i = 0;
  d->eval();
  return got;
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  d = new VKL_aecp_dyn_state;
  reset();

  printf("== KL_aecp_dyn_state, shape %s ==\n", SHAPE);

  // ---- A: everything reads INVALID out of reset -------------------------
  // The image is the authority until a controller says otherwise, so every
  // valid flag must be clear and every value zero. A store that came up
  // "valid" would make every GET report a setting nobody made.
  {
    const int sels[] = {SEL_CFG, SEL_RATE, SEL_CLKSRC, SEL_FMTIN,
                        SEL_FMTOUT, SEL_PTOFF, SEL_START, SEL_IDENT};
    for (int s : sels) {
      CHECK(rd(RGN_DYNV, s, 0) == 0,
            "A: selector %d reads VALID out of reset", s);
      CHECK(rd(RGN_DYN, s, 0) == 0,
            "A: selector %d reads non-zero out of reset", s);
    }
    CHECK(d->identify_o == 0,
          "A: Milan 5.3.12 makes IDENTIFY 0 after reset, got %u",
          (unsigned)d->identify_o);
    CHECK(d->dirty_o == 0, "A: dirty is clear before any write");
  }

  // ---- B: a write is kept, and the flag goes valid -----------------------
  {
    wr(SEL_RATE, 0, 96000);
    CHECK(rd(RGN_DYN,  SEL_RATE, 0) == 96000,
          "B: sampling rate did not read back, got %llu",
          (unsigned long long)rd(RGN_DYN, SEL_RATE, 0));
    CHECK(rd(RGN_DYNV, SEL_RATE, 0) == 1,
          "B: the valid flag did not set on a write");
  }

  // ---- C: settings are PER DESCRIPTOR ------------------------------------
  // The bug this catches is the one the design exists to avoid: an address
  // that carries the field but not the index, so every stream shares one row.
  if (NSI >= 2) {
    wr(SEL_FMTIN, 0, 0x0205022000406000ull);   // 48 kHz 1 ch  (Milan Table 6.2)
    wr(SEL_FMTIN, 1, 0x0205022002006000ull);   // 48 kHz 8 ch
    CHECK(rd(RGN_DYN, SEL_FMTIN, 0) == 0x0205022000406000ull,
          "C: input 0's format was clobbered by input 1's write");
    CHECK(rd(RGN_DYN, SEL_FMTIN, 1) == 0x0205022002006000ull,
          "C: input 1's format did not read back");
    //! ...and the flags are per descriptor too
    wr(SEL_PTOFF, 1, 250000);
    CHECK(rd(RGN_DYNV, SEL_PTOFF, 1) == 1, "C: output 1's offset is valid");
    CHECK(rd(RGN_DYNV, SEL_PTOFF, 0) == 0,
          "C: writing output 1 must NOT validate output 0");
  }

  // ---- D: an index the shape does not have is REFUSED --------------------
  // Milan sizes these arrays by the entity model. An index past the end must
  // read invalid and its write must be dropped and counted — never aliased
  // onto row zero, which would silently set the wrong stream's format.
  {
    const uint16_t oob = uint16_t(NSI + 4);
    const uint16_t oob0 = d->dbg_oob_o;
    wr(SEL_FMTIN, oob, 0xDEADBEEFCAFEF00Dull);
    CHECK(d->dbg_oob_o == uint16_t(oob0 + 1),
          "D: an out-of-range write was not counted (%u -> %u)",
          (unsigned)oob0, (unsigned)d->dbg_oob_o);
    CHECK(rd(RGN_DYNV, SEL_FMTIN, oob) == 0,
          "D: an out-of-range read claims to be valid");
    CHECK(rd(RGN_DYN, SEL_FMTIN, oob) == 0,
          "D: an out-of-range read returned data");
    CHECK(rd(RGN_DYN, SEL_FMTIN, 0) == 0x0205022000406000ull ||
          NSI < 2,
          "D: the out-of-range write ALIASED onto row 0");
  }

  // ---- E: dirty marks the persisted set, and only it ---------------------
  // Milan 5.3.13 & friends make these fields non-volatile; 5.3.12 makes the
  // IDENTIFY value volatile. Raising dirty for IDENTIFY would commit flash
  // every time a front panel blinked.
  {
    reset();
    CHECK(d->dirty_o == 0, "E: dirty is clear after reset");
    wr(SEL_IDENT, 0, 255);
    CHECK(d->identify_o == 255,
          "E: the IDENTIFY value did not reach its face, got %u",
          (unsigned)d->identify_o);
    CHECK(d->dirty_o == 0,
          "E: a volatile IDENTIFY write marked the store dirty");
    wr(SEL_CLKSRC, 0, 2);
    CHECK(d->dirty_o == 1,
          "E: a persisted clock-source write did NOT mark the store dirty");
  }

  // ---- F: the live face tracks the store ---------------------------------
  // These are the ports the fabric acts on, so a setting that reads back
  // over AECP but never reaches the face would be a setting in name only.
  {
    reset();
    wr(SEL_CFG, 0, 3);
    CHECK(d->cur_config_o == 3,
          "F: current_configuration face is %u", (unsigned)d->cur_config_o);
    wr(SEL_CLKSRC, 0, 1);
    CHECK(d->clk_src_index_o == 1,
          "F: clock_source_index face is %u", (unsigned)d->clk_src_index_o);
    wr(SEL_PTOFF, 0, 2000000);
    CHECK(d->pt_offset_o == 2000000,
          "F: presentation-offset face is %u", (unsigned)d->pt_offset_o);
    wr(SEL_START, 0, 1);
    CHECK((d->strm_started_o & 1u) == 1u,
          "F: input 0's started bit did not reach the face");
    if (NSI >= 2) {
      CHECK(((d->strm_started_o >> 1) & 1u) == 0u,
            "F: starting input 0 also started input 1");
      wr(SEL_START, 1, 1);
      CHECK(((d->strm_started_o >> 1) & 1u) == 1u,
            "F: input 1's started bit did not reach the face");
      wr(SEL_START, 0, 0);
      CHECK((d->strm_started_o & 1u) == 0u, "F: input 0 did not stop");
      CHECK(((d->strm_started_o >> 1) & 1u) == 1u,
            "F: stopping input 0 also stopped input 1");
    }
  }

  // ---- G: a read never disturbs the store --------------------------------
  // The valid-flag read happens on EVERY GET, so a read with side effects
  // would corrupt state on the most common path in the system.
  {
    const uint16_t wr0 = d->dbg_writes_o;
    for (int i = 0; i < 4; i++) { rd(RGN_DYN, SEL_CFG, 0); rd(RGN_DYNV, SEL_CFG, 0); }
    CHECK(d->dbg_writes_o == wr0, "G: a read was counted as a write");
    CHECK(d->cur_config_o == 3, "G: a read changed current_configuration");
  }

  //! NOT the canonical tally shape: this binary is ONE SHAPE of the suite,
  //! and run_suites.sh takes the LAST matching line, so a per-shape tally
  //! here would silently drop the other shape's checks from the total. The
  //! Makefile sums the two and prints the one canonical line.
  printf("[shape %s] %ld checks, %ld failures\n", SHAPE, checks, fails);
  FILE* acc = fopen("shape_tally.txt", "a");
  if (acc) { fprintf(acc, "%ld %ld\n", checks, fails); fclose(acc); }
  delete d;
  return fails ? 1 : 0;
}
