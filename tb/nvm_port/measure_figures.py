#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""
Re-measure the figures this suite's README quotes, and diff them against it.

WHY THIS EXISTS. The README carries a device-error coverage table, a set of
mutation counts, probe counts and a device-model result table. Four review
rounds found them stale, and each time the figures that had been corrected were
the ones somebody thought to check while the rest drifted. Splitting one check
into two moves every row that reaches it; replacing an array-negative check with
a bus check moves every row whose mutant wedges earlier. There is no edit to
this suite that reliably leaves the numbers alone, so hand-maintaining them is
the wrong shape.

WHAT IT COVERS. Every figure in the README, and the covered set is DERIVED
rather than asserted: each `N of M` and `N PASS, M FAIL` in the file is a claim
by default, satisfied only by a measurement here or by an explicit entry in
WAIVERS whose reason is printed on every clean run. Twelve arm rows plus the arm
COUNT read from the RTL; eleven mutations and probes; seven device-model result
rows; and all thirty cells of the pre-fix matrix.

Three earlier versions each closed a narrower class than they claimed, and the
progression is the useful part. The first checked only denominators and
result-row sums, so seven of eight falsifications walked past it. The second
added a table of named mutations and still missed M3 -- the very numerator that
had gone stale -- because nothing required the table to cover the file's own
claims. The third coupled the table to the file but matched ONE PHRASE, "fails
N of M", so three figures already present were invisible: "the same mutations
FAIL 68 of 90", "that check FAILS while the header check PASSES, 2 of 90", and
a prose restatement of a model row. A wider vocabulary would have been the same
mistake with a longer list. Inverting the default is what ended it.

The third version also declared the thirty matrix cells unmeasurable "because
the forms no longer exist in the tree". That was a cost choice dressed as an
impossibility -- the README's own text says "re-deriving it is re-running it" --
and re-injecting all six forms costs five builds. They are measured now.

Inverting the default paid for itself immediately: on its first run it found
that the coincident-completion figure could not be re-derived, because its
recipe had never been committed. Unverifiable, not fabricated -- a different
defect with a different fix, so the recipe is now `_COINCIDENT` and the number
is measured rather than asserted.

`--check` is accepted for symmetry with the other gates but changes nothing:
this exits non-zero on any disagreement either way. `--write` is deliberately
NOT provided, because a figure nobody looked at is how the table went stale in
the first place.

It is slow: one Verilator build per arm, mutation, probe and device model, five
for the matrix, plus the baseline -- 36 builds, about four minutes. It runs in
CI and should be run after any change to this suite; it is deliberately not part
of `run`. `make -C tb/nvm_port figures`.
"""

import argparse
import atexit
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parent
README = SRC / "README.md"          # read only, never written

# Every mutation below edits a WORKING COPY, not the checkout. The `finally`
# restores in place correctly, but for the ~4 minutes a sweep runs the tracked
# sources on disk are wrong, and anything else reading them gets a false answer:
# during one run a concurrent read reported 11 arms and "M4: anchor occurs 0
# times", both false, both artefacts of reading mid-sweep. Copying removes the
# class rather than narrowing the window.
_WORK = Path(tempfile.mkdtemp(prefix="nvm_figures."))
atexit.register(shutil.rmtree, _WORK, True)
HERE = _WORK / "tb" / "nvm_port"
shutil.copytree(SRC, HERE, ignore=shutil.ignore_patterns("obj_dir", "__pycache__"))
_rtl_src = SRC.parent.parent / "hdl" / "packet_engine"
shutil.copytree(_rtl_src, _WORK / "hdl" / "packet_engine")
RTL = _WORK / "hdl" / "packet_engine" / "KL_pp_nvm_port.sv"
SIM = HERE / "sim_main.cpp"

#: (line in the RTL, state name) for every `if (dev_err_i)` arm. Cross-checked
#: against the RTL below: a THIRTEENTH arm added anywhere used to leave this
#: gate printing "all figures agree" while the README's "twelve arms" silently
#: became false.
ARMS = [(185, "S_WEREQ"), (194, "S_WEWAIT"), (204, "S_WWREQ"), (214, "S_WHPUMP"),
        (227, "S_WDPUMP"), (237, "S_WWAIT"), (248, "S_RHREQ"), (258, "S_RHCOLL"),
        (276, "S_RHWAIT"), (303, "S_RPREQ"), (313, "S_RPPUMP"), (323, "S_RPWAIT")]

#: The coincident-completion model. Unlike every other model here it varies the
#: HANDSHAKE, not what the array retains: the device raises `dev_done_i` on the
#: same clock edge that moves a pump's final byte. `KL_pp_nvm_port.sv:151-153`
#: says the sticky `done_seen_r` latch exists for exactly this device, so it is
#: a documented contract freedom, not a broken peer -- and the port handles it,
#: 90 PASS 0 FAIL on pristine RTL. Its whole interest is that deleting the latch
#: is INVISIBLE without it.
#:
#: The load-bearing edit is the fourth. Setting the model's own `d_done` cannot
#: produce this: `dut->dev_done_i = d_done;` is driven at the top of the tick,
#: so it presents a cycle late, the port is already in a WAIT state reading
#: `dev_done_i` directly, and the latch is bypassed. Four attempts to tune a
#: completion delay all gave 90/0, because the delay is counted in ticks and the
#: coincidence lives inside one. Writing `dut->dev_done_i` directly after the
#: first `eval()` -- when the DUT's outputs for this cycle have settled, so the
#: final-byte handshake is visible -- and re-evaluating before the posedge is
#: the only thing that lands it on the right edge.
#:
#: The `fail_cur` guard is load-bearing too: without it the model pulses `done`
#: on the byte an armed error is about to land on, and T17/T18 stop testing what
#: they name.
_COINCIDENT = [
    (SIM, "  int  d_st = 0;                          // 0 idle, 1 data, 2 completion timer",
          "  int  d_st = 0;                          // 0 idle, 1 data, 2 completion timer\n"
          "  bool coinc = false;      // this tick's final byte carries `done` with it"),
    (SIM, "        if (fail_cur && d_bytes == err_after_bytes) { err_ctr = 2; d_st = 2; }\n"
          "        else if (d_bytes == d_cur.len) { done_ctr = op_delay; d_st = 2; }",
          "        if (fail_cur && d_bytes == err_after_bytes) { err_ctr = 2; d_st = 2; }\n"
          "        else if (d_bytes == d_cur.len) {\n"
          "          if (coinc) { d_busy = false; d_st = 0; }\n"
          "          else { done_ctr = op_delay; d_st = 2; } }"),
    (SIM, "          if (fail_cur && d_bytes == err_after_bytes) { err_ctr = 2; d_st = 2; }\n"
          "          else if (d_bytes == d_cur.len) { done_ctr = op_delay; d_st = 2; }",
          "          if (fail_cur && d_bytes == err_after_bytes) { err_ctr = 2; d_st = 2; }\n"
          "          else if (d_bytes == d_cur.len) {\n"
          "            if (coinc) { d_busy = false; d_st = 0; }\n"
          "            else { done_ctr = op_delay; d_st = 2; } }"),
    (SIM, "    dut->clk_i = 0; dut->eval();",
          "    dut->clk_i = 0; dut->eval();\n"
          "    coinc = false;\n"
          "    if (d_st == 1 && !(fail_cur && err_after_bytes >= 0\n"
          "                       && d_bytes + 1 == err_after_bytes)) {\n"
          "      bool lw = (d_cur.op == OP_WRITE && d_bytes + 1 == d_cur.len\n"
          "                 && dut->dev_wready_i && dut->dev_wvalid_o);\n"
          "      bool lr = (d_cur.op == OP_READ && d_bytes + 1 == d_cur.len\n"
          "                 && dut->dev_rvalid_i && dut->dev_rready_o);\n"
          "      if (lw || lr) { dut->dev_done_i = 1; dut->dev_busy_i = 0;\n"
          "                      coinc = true; dut->eval(); }\n"
          "    }"),
]


#: Every `fails N of M` claim in the README, and how to reproduce it.
#:
#: `claim` is a regex that must match the README EXACTLY ONCE and must capture
#: the claimed count. Keying on the claim text rather than on a bare name is
#: what makes the coverage check below possible: a claim nobody listed here is
#: detectable precisely because every listed claim points at its own sentence.
#:
#: `edits` is a list of (file, old, new). Every `old` must occur exactly once --
#: see `apply_edits`. Two of these anchors were one-line strings that appear
#: twice in the file, and a count-1 replace silently patched whichever came
#: first; they carry a line of context now so the ambiguity is impossible
#: rather than merely unlikely.
MUTATIONS = [
    # ---- mutations of the RTL ------------------------------------------------
    ("M1", r"rewritten to `S_WWREQ`.*?\*\*Fails (\d+) of", [
        (RTL, "                state_r <= S_WEREQ;",
              "                state_r <= S_WWREQ;")]),
    ("M1-sibling", r"so the ERASE is REQUESTED but never awaited, \*\*fails\s+only (\d+) of", [
        (RTL, "          end else if (dev_gnt_i) begin\n"
              "            state_r <= S_WEWAIT;\n          end",
              "          end else if (dev_gnt_i) begin\n"
              "            state_r <= S_WWREQ;\n          end")]),
    ("M2", r"\*\*M2\*\*.*?\*\*fails (\d+) of", [
        (RTL, "(hdr_r[0] == MAGIC_HI_C) && (hdr_r[1] == MAGIC_LO_C)", "1'b1")]),
    ("M3", r"\*\*M3\*\*.*?\*\*fails (\d+) of", [
        (RTL, "            if (bcnt_r == (plen_r - 16'd1)) state_r <= S_WWAIT;",
              "            if (bcnt_r == plen_r) state_r <= S_WWAIT;"),
        (RTL, "            if (bcnt_r == (plen_r - 16'd1)) state_r <= S_RPWAIT;",
              "            if (bcnt_r == plen_r) state_r <= S_RPWAIT;")]),
    ("M4", r"\*\*M4\*\*.*?\*\*fails (\d+) of", [
        (RTL, "        S_WDPUMP: begin\n          if (dev_err_i) begin",
              "        S_WDPUMP: begin\n          if (1'b0) begin")]),
    ("M5", r"\*\*M5\*\*.*?\*\*fails (\d+) of", [
        (RTL, "        S_WWAIT: begin\n          if (dev_err_i) begin",
              "        S_WWAIT: begin\n          if (1'b0) begin")]),
    # ---- mutations of the TEST (the README calls these probes) ---------------
    ("probe-armearly", r"fails before its first byte moves, fails (\d+) of", [
        (SIM, "    std::vector<uint8_t> torn = frame(1, pattern(16, 0x22));\n"
              "    h.arm_err(1, 5);",
              "    std::vector<uint8_t> torn = frame(1, pattern(16, 0x22));\n"
              "    h.arm_err(1, -1);")]),
    ("probe-nodevice", r"the port the T16 prose names as the threat \S+ fails (\d+) of", [
        (SIM, "    h.arm_err(1, 5);\n    rc = h.commit(1, torn);\n    h.disarm_err();",
              "    rc = 1;")]),
    ("probe-t18", r"replacing a T18 arm with a bare `r = 1`, port never touched: now\s+fails (\d+)", [
        (SIM, "    int r = h.restore(5);", "    int r = 1;")]),
    # The retraction at "On checks that cannot fail alone": T15's tear moved
    # into the completion window, so the survival check fails while the
    # header-agreement check beside it passes. Its numerator was invisible to
    # the first inverted-default pass because it reads "FAILS ... PASSES, 2 of
    # 90" -- no "fails N of M" anywhere in the sentence.
    # The `done_seen_r` escape: green on pristine both ways, and only the
    # coincident model exposes it. This was an UNVERIFIABLE figure -- the
    # recipe lived in a scratch run that was never committed, so it could not
    # be re-derived. That is a different defect from a fabricated one and it
    # gets a different fix: commit the recipe, not retract the number.
    ("done_seen_r/coincident",
     r"under a coincident-completion model the same\s+mutations fail (\d+) of", 
     _COINCIDENT + [(RTL, "      if (dev_done_i) done_seen_r <= 1'b1;\n", "")]),
    ("retraction", r"that check FAILS while the header check PASSES, (\d+) of", [
        (SIM, "    h.arm_err(1, 12);                 // op 0 = ERASE, op 1 = WRITE: cut at 12 B",
              "    h.arm_err(1, static_cast<int>(replacement.size()));")]),
]

#: The device-model result table. Each varies what the ARRAY retains and must
#: not redden a check about the PORT; the README's claim for each is a
#: `N PASS, F FAIL` row. Measuring these closes a shape the size/sum checks are
#: structurally blind to: "90 PASS, 0 FAIL" -> "45 PASS, 45 FAIL" preserves the
#: sum, so nothing below caught it until these ran.
_HALFPAGE = (SIM,
             "        d_stall = wstall;\n"
             "        if (fail_cur && d_bytes == err_after_bytes) { err_ctr = 2; d_st = 2; }",
             "        d_stall = wstall;\n"
             "        if (fail_cur && d_bytes == err_after_bytes) {\n"
             "          for (int k = 0; k < 4 && d_bytes - 1 - k >= 0; ++k)\n"
             "            store[d_cur.region % N_REGIONS]\n"
             "                 [(d_cur.offset + d_bytes - 1 - k) % REG_BYTES] = 0xFF;\n"
             "          err_ctr = 2; d_st = 2;\n        }")
_PAGEBUF = [
    (SIM, "  std::vector<uint8_t> sent;",
          "  std::vector<uint8_t> sent;\n  std::vector<uint8_t> pagebuf;"),
    (SIM, "        sent.push_back(dut->dev_wdata_o);\n"
          "        store[d_cur.region % N_REGIONS][(d_cur.offset + d_bytes) % REG_BYTES] =\n"
          "            dut->dev_wdata_o;\n        ++d_bytes;",
          "        sent.push_back(dut->dev_wdata_o);\n"
          "        pagebuf.push_back(dut->dev_wdata_o);\n        ++d_bytes;"),
    (SIM, "        else if (d_bytes == d_cur.len) { done_ctr = op_delay; d_st = 2; }\n"
          "      } else if (d_stall > 0) --d_stall;\n    }\n\n    // read byte delivery",
          "        else if (d_bytes == d_cur.len) {\n"
          "          for (size_t k = 0; k < pagebuf.size(); ++k)\n"
          "            store[d_cur.region % N_REGIONS][(d_cur.offset + k) % REG_BYTES] =\n"
          "                pagebuf[k];\n"
          "          pagebuf.clear();\n          done_ctr = op_delay; d_st = 2;\n        }\n"
          "      } else if (d_stall > 0) --d_stall;\n    }\n\n    // read byte delivery"),
    (SIM, "        d_stall = wstall;\n"
          "        if (fail_cur && d_bytes == err_after_bytes) { err_ctr = 2; d_st = 2; }",
          "        d_stall = wstall;\n"
          "        if (fail_cur && d_bytes == err_after_bytes) {\n"
          "          pagebuf.clear(); err_ctr = 2; d_st = 2;\n        }"),
]
_LAZY = [(SIM, "        int r = d_cur.region % N_REGIONS;\n"
               "        memset(store[r], 0xFF, REG_BYTES);\n"
               "        ++erase_count[r];",
               "        int r = d_cur.region % N_REGIONS;\n"
               "        ++erase_count[r];")]

#: A model measurement may be quoted in more than one place. `claims` is a
#: LIST, and every listed site must agree with the one measurement: the prose
#: sentence at "Model probe" restates the half-page row, and a falsification of
#: the prose alone survived a gate that anchored on the table row only.

MODELS = [
    ("pristine", [r"\| pristine \| \*\*(\d+) PASS, (\d+) FAIL\*\*"], []),
    ("half-page", [r"\| half-page \| \*\*(\d+) PASS, (\d+) FAIL\*\*",
                   r"The half-page model is (\d+) PASS, (\d+) FAIL"], [_HALFPAGE]),
    ("page-buffered NOR", [r"\| page-buffered NOR \| \*\*(\d+) PASS, (\d+) FAIL\*\*"], _PAGEBUF),
    ("lazy erase", [r"\| lazy erase \| (\d+) PASS, (\d+) FAIL"], _LAZY),
    ("lazy erase + page-buffered",
     [r"\| lazy erase \+ page-buffered \| (\d+) PASS, (\d+) FAIL"], _PAGEBUF + _LAZY),
    ("coincident completion",
     [r"\| coincident completion \| \*\*(\d+) PASS, (\d+) FAIL\*\*"], _COINCIDENT),
]

#: Models whose columns the pre-fix matrix carries, in the table's own order.
#: NOT simply "every model": the matrix is about what the array RETAINS, and
#: the coincident model varies the handshake instead. Deriving the columns from
#: MODELS produced a six-wide measurement against a five-wide table on the run
#: that added `pristine` -- the gate caught it, which is the point.
MATRIX_MODELS = ["pristine", "half-page", "page-buffered NOR", "lazy erase",
                 "lazy erase + page-buffered"]

#: The six-by-five pre-fix matrix. Each row is a check FORM that was removed
#: from the suite because it read the array where it should not; the table
#: records which device model exposed each. Two earlier versions of this script
#: declared these thirty cells unmeasurable "because the forms no longer exist
#: in the tree". That was a cost choice dressed as an impossibility, and the
#: README's own text contradicted it: "re-deriving it is re-running it." The
#: forms are recoverable from `ba403c6~1`, `62d96d6~1` and `dc354be~1`, and
#: re-injecting all six beside the current checks costs FIVE builds -- one
#: fewer than MODELS already spends.
#:
#: (README row label, anchor, code injected before it). Order matters: the two
#: T17 forms must be injected BEFORE the good commit, because both pre-fix
#: spellings restored the TORN region and anchoring after `h.commit(3, rec)`
#: measures the wrong array.
MATRIX_FORMS = [
    ("T16 byte comparison",
     "    CHECK(h.sent.size() == 5 && std::equal(h.sent.begin(), h.sent.end(),",
     '    CHECK(std::equal(torn.begin(), torn.begin() + 5, h.store[1])\n'
     '              && h.store[1][5] == 0xFF,\n'
     '          "MX T16 byte comparison");\n'),
    ("T17 restore vs the record",
     "    h.ops.clear();\n    rc = h.commit(3, rec);\n"
     "    CHECK(rc == 0 && h.store_match(3, rec),",
     '    {\n'
     '      std::vector<uint8_t> probe_arr(h.store[3], h.store[3] + late.size());\n'
     '      h.ops.clear(); int pr = h.restore(3);\n'
     '      CHECK(pr == 0 && h.rbytes == late, "MX T17 restore vs the record");\n'
     '      h.ops.clear(); pr = h.restore(3);\n'
     '      CHECK(pr == 0 && h.rbytes.size() == late.size() && h.rbytes == probe_arr,\n'
     '            "MX T17 restore vs the array");\n'
     '    }\n'),
    ("T15 branch pin", "    bool hdr_intact =",
     '    CHECK(crc_rejects && h.rbytes.size() == whole.size(),\n'
     '          "MX T15 branch pin");\n'
     '    CHECK(!h.store_match(7, whole) && !h.store_match(7, replacement),\n'
     '          "MX T15 cut was real");\n'
     '    CHECK(refused || crc_rejects,\n'
     '          "MX T15 #70 property, unconditioned");\n'),
]
#: Rows carried by an injection above rather than owning an anchor of their own.
MATRIX_ROWS = ["T16 byte comparison", "T17 restore vs the record",
               "T17 restore vs the array", "T15 branch pin",
               "T15 cut was real", "T15 #70 property, unconditioned"]


def readme_matrix():
    """{row: [pass/FAIL per model]} exactly as the README's matrix claims."""
    out, cols = {}, None
    for line in README.read_text().splitlines():
        if line.startswith("| pre-fix form |"):
            cols = [c.strip() for c in line.strip("|").split("|")][1:]
        elif cols and line.startswith("|") and not line.startswith("|---"):
            cells = [c.strip() for c in line.strip("|").split("|")]
            if cells[0] in MATRIX_ROWS:
                out[cells[0]] = ["FAIL" if "FAIL" in c else "pass" for c in cells[1:]]
            elif out:
                break
    return out, (cols or [])


def measure_matrix():
    """{row: [pass/FAIL per model]} measured: inject all six, run every model."""
    inject = [(SIM, anchor, code + anchor) for _, anchor, code in MATRIX_FORMS]
    got = {r: [] for r in MATRIX_ROWS}
    by_name = {n: e for n, _c, e in MODELS}
    for mname in MATRIX_MODELS:
        failed = apply_edits(inject + list(by_name[mname]), want_fail_names=True)
        for r in MATRIX_ROWS:
            got[r].append("FAIL" if any(f"MX {r}" in f for f in failed) else "pass")
    return got


TALLY_RE = re.compile(r"(\d+) checks: (\d+) PASS, (\d+) FAIL")

#: EVERY figure in the README. Not "every figure phrased the way I expected" --
#: that was the previous version and it was a recogniser, matching one
#: vocabulary ("fails N of M") and blind to everything else. Three figures
#: already in the file sat outside it: "the same mutations FAIL 68 of 90"
#: (fail, not fails), "that check FAILS while the header check PASSES, 2 of 90"
#: (numerator not adjacent to any verb), and a prose restatement of a model row.
#: Seven plausible phrasings of a future claim were silent too -- `reddens 22 of
#: 90`, `FAILS 22 of 90`, `fails twenty-two of 90`, `| M6 | 22 |`, `22 of 90
#: checks go red`. A wider vocabulary would have been the same mistake with a
#: longer list, so the default is inverted: this matches the SHAPE of a figure,
#: and anything it finds is a claim that must be measured or waived out loud.
#: A spelled-out numerator is still a numerator: "fails twenty-two of 90" was
#: the one phrasing of seven that survived the first inverted-default pass.
#: Enumerating English number words is a recogniser too, but unlike the verbs it
#: is a genuinely CLOSED class -- there is no eighth way to write twenty-two --
#: so the list cannot grow behind the gate's back the way "fails/FAILS/reddens/
#: failed/go red" did.
_ONES = ("one two three four five six seven eight nine ten eleven twelve "
         "thirteen fourteen fifteen sixteen seventeen eighteen nineteen").split()
_TENS = "twenty thirty forty fifty sixty seventy eighty ninety".split()
_WORDS = "|".join(sorted(
    _ONES + _TENS + [f"{a}-{b}" for a in _TENS for b in _ONES[:9]] + ["zero"],
    key=len, reverse=True))
FIGURE_RE = re.compile(
    r"\d+ of \d+|\d+ PASS, \d+ FAIL|\b\d+/\d+\b"
    rf"|\b(?:{_WORDS}) of \d+")

#: (regex, reason). A figure this file does not measure must be listed here with
#: why, and the reason is PRINTED on every clean run. The exclusion list is now
#: derived from the file rather than asserted in a docstring: a claim that is
#: neither measured nor waived is a hard error, so it cannot be added silently
#: and it cannot be quietly dropped from the covered set either.
WAIVERS = [
    (r"\b90/90\b",
     "suite tally shorthand; both halves are the suite size, which the size "
     "check already pins against the measured total"),
]


def run_suite(want_fail_names=False):
    """Build and run; return (total, passed, failed), or the failing names."""
    subprocess.run(["make", "-s", "clean"], cwd=HERE, check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p = subprocess.run(["make", "-s"], cwd=HERE, capture_output=True, text=True)
    m, names = None, []
    for line in (p.stdout + p.stderr).splitlines():
        hit = TALLY_RE.search(line)
        if hit:
            m = hit
        elif line.startswith("FAIL: "):
            names.append(line[6:])
    if not m:
        raise RuntimeError("no tally line; build failed:\n" + p.stdout[-2000:])
    if want_fail_names:
        return names
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def apply_edits(edits, want_fail_names=False):
    """Apply every edit, run, restore byte-identically. Returns the tally.

    An `old` that occurs zero times means this table is stale, which is the
    class the gate exists to catch. An `old` that occurs MORE than once is
    refused for the same reason at one remove: a count-1 replace would still
    measure something, just not necessarily the thing the README names, and a
    measurement of the wrong site is worse than a missing one because it looks
    like coverage.
    """
    saved = {p: p.read_text() for p, _, _ in edits}
    try:
        for path, old, new in edits:
            text = path.read_text()
            n = text.count(old)
            if n != 1:
                raise RuntimeError(
                    f"{path.name}: anchor occurs {n} times, need exactly 1 -- "
                    "this table is stale or the anchor is ambiguous, which is "
                    f"the class this gate exists to catch\n    {old!r:.120}")
            path.write_text(text.replace(old, new, 1))
        return run_suite(want_fail_names)
    finally:
        for path, text in saved.items():
            path.write_text(text)


def with_arm_disabled(lineno):
    """Force one `if (dev_err_i)` arm false, run, restore. Returns fail count.

    Line-indexed, NOT text-anchored, and deliberately does not go through
    `apply_edits`: all twelve arms are the same twelve characters, so the
    uniqueness rule that protects every other edit here would reject all of
    them. The line number IS the identity, and the guard below is the
    corresponding staleness check.
    """
    original = RTL.read_text()
    lines = original.splitlines(keepends=True)
    idx = lineno - 1
    if idx >= len(lines) or "if (dev_err_i) begin" not in lines[idx]:
        raise RuntimeError(
            f"{RTL.name}:{lineno} is not an `if (dev_err_i)` arm any more -- "
            "the ARMS table in this script is stale, which is exactly the "
            "class it exists to catch")
    try:
        lines[idx] = lines[idx].replace("if (dev_err_i) begin", "if (1'b0) begin")
        RTL.write_text("".join(lines))
        return run_suite()[2]
    finally:
        RTL.write_text(original)


def rtl_arm_count():
    """How many `if (dev_err_i)` arms the RTL actually has, counted not assumed."""
    return RTL.read_text().count("if (dev_err_i) begin")


def flat_readme():
    return " ".join(README.read_text().split())


def readme_arm_rows():
    """{state: count} as the README's coverage table currently claims."""
    rows = {}
    for line in README.read_text().splitlines():
        m = re.match(r"\|\s*(\d+)\s*\|\s*`(S_\w+)`\s*\|\s*\**(\d+)", line)
        if m:
            rows[m.group(2)] = int(m.group(3))
    return rows


def claim_coverage(flat):
    """Problems with the claim<->measurement correspondence, as strings.

    Both directions, because one direction is not a coupling. Measurement ->
    README catches a claim reworded or deleted; README -> measurement catches a
    claim ADDED, which is the direction M3 escaped through.

    The README -> measurement half now works by SHAPE, not by phrase. Every
    `N of M` and `N PASS, M FAIL` in the file is a claim by default; it is
    satisfied only by falling inside a measurement's own claim span or by an
    explicit waiver. That inversion is what makes the covered set derivable
    instead of asserted, and it found a figure nobody could reproduce on its
    first run.
    """
    bad, spans = [], []
    for name, claims, _ in _all_claims():
        for claim in claims:
            hits = list(re.finditer(claim, flat))
            if len(hits) != 1:
                bad.append(f"{name}: its README claim matches {len(hits)} times, "
                           "need exactly 1 -- the claim was reworded, moved or "
                           "deleted, so nothing checks this measurement any more")
            spans.extend((h.start(), h.end()) for h in hits)
    for pat, _reason in WAIVERS:
        spans.extend((h.start(), h.end()) for h in re.finditer(pat, flat))
    for m in FIGURE_RE.finditer(flat):
        # OVERLAP, not containment: a claim regex captures the numerator and
        # ends at "... of", while the figure shape runs on through the
        # denominator. Containment rejected all eleven real claims.
        if not any(s < m.end() and m.start() < e for s, e in spans):
            ctx = flat[max(0, m.start() - 60):m.end() + 20]
            bad.append(f"UNCLAIMED FIGURE {flat[m.start():m.end()]!r} -- every "
                       "figure in the README must be measured by an entry here "
                       "or waived in WAIVERS with a reason. Do not delete the "
                       f"claim to silence this.\n      ...{ctx}...")
    return bad


def _all_claims():
    """(name, [claim regexes], edits) over mutations and models alike."""
    return ([(n, [c], e) for n, c, e in MUTATIONS]
            + [(n, cs, e) for n, cs, e in MODELS])


def readme_figures(total, flat):
    """Suite-size claims that disagree with `total`, as human-readable strings.

    A SIZE claim ("90 checks", "1 of 90", "the 90-check suite") is stale if it
    is not `total`. A RESULT row ("89 PASS, 1 FAIL") is stale if pass + fail is
    not `total` -- 89 is a good number in a five-model table and must not be
    flagged, while "82 PASS, 1 FAIL" must be.

    Note what this canNOT see, and why MODELS exists: "45 PASS, 45 FAIL"
    preserves the sum. Sums are a weak test and are the second line of defence
    here, not the first.
    """
    bad = []
    for pat in (r"of (\d+)\b", r"(\d+) checks\b", r"(\d+)-check\b"):
        for n in {int(x) for x in re.findall(pat, flat)}:
            if n > 40 and n != total:
                bad.append(f"suite size quoted as {n}, measured {total}")
    for pas, fail in re.findall(r"(\d+) PASS, (\d+) FAIL", flat):
        if int(pas) + int(fail) != total:
            bad.append(f"result row '{pas} PASS, {fail} FAIL' sums to "
                       f"{int(pas) + int(fail)}, suite is {total}")
    return sorted(set(bad))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="accepted for symmetry with the other gates; this "
                         "exits non-zero on a disagreement with or without it")
    ap.parse_args()

    total, passed, failed = run_suite()
    print(f"baseline: {total} checks, {passed} PASS, {failed} FAIL")
    if failed:
        print("baseline is not green; fix that before trusting any figure below")
        return 1

    flat = flat_readme()
    bad = readme_figures(total, flat)
    bad.extend(claim_coverage(flat))

    rtl_arms = rtl_arm_count()
    if rtl_arms != len(ARMS):
        bad.append(f"the RTL has {rtl_arms} `if (dev_err_i)` arms; this gate's "
                   f"ARMS table lists {len(ARMS)}. A new arm is invisible to "
                   f"every row below until it is added here.")

    claimed = readme_arm_rows()
    if len(claimed) != len(ARMS):
        bad.append(f"README table has {len(claimed)} rows; the RTL has {len(ARMS)} arms")

    print("\narm coverage (measured vs README):")
    for lineno, state in ARMS:
        got = with_arm_disabled(lineno)
        says = claimed.get(state)
        print(f"  [{'ok ' if says == got else 'STALE'}] {lineno:>4} {state:<10} "
              f"measured={got:<4} readme={says}")
        if says != got:
            bad.append(f"{state}: README says {says}, measured {got}")

    print("\nmutations and probes (measured vs README):")
    for name, claim, edits in MUTATIONS:
        got = apply_edits(edits)[2]
        hit = re.search(claim, flat)
        says = int(hit.group(1)) if hit else None
        print(f"  [{'ok ' if says == got else 'STALE'}] {name:<16} "
              f"measured={got:<4} readme={says}")
        if says is not None and says != got:
            bad.append(f"{name}: README says {says}, measured {got}")

    print("\ndevice models (measured vs README):")
    for name, claims, edits in MODELS:
        _, mp, mf = apply_edits(edits)
        got = (mp, mf)
        for claim in claims:
            hit = re.search(claim, flat)
            says = (int(hit.group(1)), int(hit.group(2))) if hit else None
            print(f"  [{'ok ' if says == got else 'STALE'}] {name:<26} "
                  f"measured={mp} PASS, {mf} FAIL  readme={says}")
            if says is None:
                bad.append(f"model '{name}': a claim site vanished from the README")
            elif says != got:
                bad.append(f"model '{name}': README says {says[0]} PASS/{says[1]} "
                           f"FAIL, measured {mp} PASS/{mf} FAIL")

    print("\npre-fix matrix (measured vs README), 5 builds:")
    says_m, cols = readme_matrix()
    got_m = measure_matrix()
    if len(says_m) != len(MATRIX_ROWS):
        bad.append(f"README matrix has {len(says_m)} of {len(MATRIX_ROWS)} known rows")
    for row in MATRIX_ROWS:
        says, got = says_m.get(row), got_m[row]
        print(f"  [{'ok ' if says == got else 'STALE'}] {row:<34} "
              f"measured={' '.join(got)}")
        if says != got:
            bad.append(f"matrix row '{row}': README says {says}, measured {got}")

    if bad:
        print("\nfigures disagree with the tree:")
        for b in bad:
            print("  -", b)
        print("\nRe-measure and update README.md. Do NOT edit only the rows you\n"
              "changed: four review rounds found staleness in the rows nobody\n"
              "thought to check.")
        # Always non-zero. The first version returned 0 without --check, so a
        # caller that forgot the flag got a clean exit over a stale file.
        return 1

    print("\nall measured figures agree with the tree")
    if WAIVERS:
        print("\nNOT measured (waived, with the reason, so the exclusion set is\n"
              "derived from the file rather than asserted in a comment):")
        for pat, reason in WAIVERS:
            print(f"  - /{pat}/: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
