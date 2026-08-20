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

WHAT IT COVERS, AND WHAT IT DOES NOT. Read this before trusting a green run.

  covered   the twelve `if (dev_err_i)` arm rows, and the arm COUNT read out of
            the RTL rather than assumed; every `fails N of M` claim in the file,
            which is M1, its sibling, M2, M3, M4, M5 and the three test probes;
            every row of the device-model result table; and every suite-size
            figure.
  NOT       the six-by-five pre-fix matrix. Those thirty cells are a historical
            record of check FORMS that no longer exist in the tree, so there is
            nothing here to re-run them against. They are argued, not measured,
            and they are the one part of this file that can still rot quietly.

That list is the honest version. Two earlier versions of this docstring said
"re-measure every figure" while the code reached a strict subset, which is the
same defect as the stale table: an instrument that closes a narrow class while
claiming the wide one. A reviewer caught it both times.

The coupling that keeps the list above true is CLAIM COVERAGE: every `fails N of
M` sentence in the README must be claimed by exactly one measurement below. A
new named mutation with a new count is a hard error until it is measured -- it
cannot be added silently, which is how M3 stayed ungated after the first
version of this gate was written specifically because M3 had gone stale.

`--check` is accepted for symmetry with the other gates but changes nothing:
this exits non-zero on any disagreement either way. `--write` is deliberately
NOT provided, because a figure nobody looked at is how the table went stale in
the first place.

It is slow: one Verilator build per arm, per mutation, per probe and per device
model, plus the baseline -- 26 builds, about three minutes. Run it when the
suite changes, not on every commit. `make -C tb/nvm_port figures`.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RTL = HERE.parent.parent / "hdl" / "packet_engine" / "KL_pp_nvm_port.sv"
SIM = HERE / "sim_main.cpp"
README = HERE / "README.md"

#: (line in the RTL, state name) for every `if (dev_err_i)` arm. Cross-checked
#: against the RTL below: a THIRTEENTH arm added anywhere used to leave this
#: gate printing "all figures agree" while the README's "twelve arms" silently
#: became false.
ARMS = [(185, "S_WEREQ"), (194, "S_WEWAIT"), (204, "S_WWREQ"), (214, "S_WHPUMP"),
        (227, "S_WDPUMP"), (237, "S_WWAIT"), (248, "S_RHREQ"), (258, "S_RHCOLL"),
        (276, "S_RHWAIT"), (303, "S_RPREQ"), (313, "S_RPPUMP"), (323, "S_RPWAIT")]

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

MODELS = [
    ("half-page",                 r"\| half-page \| \*\*(\d+) PASS, (\d+) FAIL\*\*", [_HALFPAGE]),
    ("page-buffered NOR",         r"\| page-buffered NOR \| \*\*(\d+) PASS, (\d+) FAIL\*\*", _PAGEBUF),
    ("lazy erase",                r"\| lazy erase \| (\d+) PASS, (\d+) FAIL", _LAZY),
    ("lazy erase + page-buffered",
     r"\| lazy erase \+ page-buffered \| (\d+) PASS, (\d+) FAIL", _PAGEBUF + _LAZY),
]

TALLY_RE = re.compile(r"(\d+) checks: (\d+) PASS, (\d+) FAIL")
#: Every sentence in the README that quotes a mutation/probe numerator. The
#: coverage check requires each to be claimed by exactly one MUTATIONS entry.
CLAIM_RE = re.compile(r"[Ff]ails\s+(?:only\s+)?\*?\*?\d+\s+of|now\s+fails\s+\d+")


def run_suite():
    """Build and run; return (total, passed, failed). Raises on a build error."""
    subprocess.run(["make", "-s", "clean"], cwd=HERE, check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p = subprocess.run(["make", "-s"], cwd=HERE, capture_output=True, text=True)
    m = None
    for line in (p.stdout + p.stderr).splitlines():
        hit = TALLY_RE.search(line)
        if hit:
            m = hit
    if not m:
        raise RuntimeError("no tally line; build failed:\n" + p.stdout[-2000:])
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def apply_edits(edits):
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
        return run_suite()
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

    Both directions, because one direction is not a coupling. MUTATIONS ->
    README catches a claim that was reworded or deleted; README -> MUTATIONS
    catches a claim that was ADDED, which is the direction the first version of
    this gate missed entirely and the direction M3 escaped through.
    """
    bad, spans = [], []
    for name, claim, _ in MUTATIONS:
        hits = list(re.finditer(claim, flat))
        if len(hits) != 1:
            bad.append(f"{name}: its README claim matches {len(hits)} times, "
                       "need exactly 1 -- the claim was reworded, moved or "
                       "deleted, so nothing checks this measurement any more")
        spans.extend((h.start(), h.end()) for h in hits)
    for m in CLAIM_RE.finditer(flat):
        if not any(s <= m.start() and m.end() <= e for s, e in spans):
            bad.append(f"unclaimed numerator {flat[m.start():m.end() + 6]!r} -- "
                       "every `fails N of M` in the README must be measured by "
                       "an entry in MUTATIONS. Add one; do not delete the claim.")
    return bad


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
    for name, claim, edits in MODELS:
        _, mp, mf = apply_edits(edits)
        hit = re.search(claim, flat)
        says = (int(hit.group(1)), int(hit.group(2))) if hit else None
        got = (mp, mf)
        print(f"  [{'ok ' if says == got else 'STALE'}] {name:<26} "
              f"measured={mp} PASS, {mf} FAIL  readme={says}")
        if says is None:
            bad.append(f"model '{name}': no result row found in the README")
        elif says != got:
            bad.append(f"model '{name}': README says {says[0]} PASS/{says[1]} "
                       f"FAIL, measured {mp} PASS/{mf} FAIL")

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
    print("NOT measured, and it says so at the top of this file: the six-by-five\n"
          "pre-fix matrix, whose check forms no longer exist to re-run.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
