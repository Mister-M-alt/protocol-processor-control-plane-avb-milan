#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""
Re-measure every figure this suite's README quotes, and diff them against it.

WHY THIS EXISTS. The README carries a device-error coverage table and a set of
mutation counts. Four separate review rounds found them stale, and each time the
figures that had been corrected were the ones somebody thought to check while
the rest drifted. Splitting one check into two moves every row that reaches it;
replacing an array-negative check with a bus check moves every row whose mutant
wedges earlier. There is no edit to this suite that reliably leaves the numbers
alone, so hand-maintaining them is the wrong shape.

This script runs the mutations, reads the counts out of the suite's own output,
and compares them to what the README says. `--check` exits non-zero on any
disagreement and names the row; `--write` is deliberately NOT provided, because
a figure nobody looked at is how the table went stale in the first place.

It is slow: one Verilator build per arm plus the baseline, so roughly fifteen
builds. Run it when the suite changes, not on every commit.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RTL = HERE.parent.parent / "hdl" / "packet_engine" / "KL_pp_nvm_port.sv"
README = HERE / "README.md"

#: (line in the RTL, state name) for every `if (dev_err_i)` arm. This list is
#: cross-checked against the RTL below: a THIRTEENTH arm added anywhere used to
#: leave the gate printing "all figures agree" while the README's "twelve arms"
#: silently became false.
ARMS = [(185, "S_WEREQ"), (194, "S_WEWAIT"), (204, "S_WWREQ"), (214, "S_WHPUMP"),
        (227, "S_WDPUMP"), (237, "S_WWAIT"), (248, "S_RHREQ"), (258, "S_RHCOLL"),
        (276, "S_RHWAIT"), (303, "S_RPREQ"), (313, "S_RPPUMP"), (323, "S_RPWAIT")]

#: Named mutations the README quotes a NUMERATOR for, and how to reproduce each.
#: A `sed`-style (file, old, new) edit; RTL paths are relative to the repo root.
#: WHY: the first version of this gate checked only denominators ("of 90") and
#: result-row sums. Seven of eight falsifications walked past it, including
#: reverting M3 to the exact stale value the gate had been written after
#: finding. A gate that cannot see the number it exists to protect is worse
#: than none, because it retires the suspicion.
MUTATIONS = [
    ("M1", RTL, "                state_r <= S_WEREQ;", "                state_r <= S_WWREQ;"),
    ("M2", RTL, "(hdr_r[0] == MAGIC_HI_C) && (hdr_r[1] == MAGIC_LO_C)", "1'b1"),
    ("M4", RTL, "        S_WDPUMP: begin\n          if (dev_err_i) begin",
                "        S_WDPUMP: begin\n          if (1'b0) begin"),
    ("M5", RTL, "        S_WWAIT: begin\n          if (dev_err_i) begin",
                "        S_WWAIT: begin\n          if (1'b0) begin"),
]

TALLY_RE = re.compile(r"(\d+) checks: (\d+) PASS, (\d+) FAIL")


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


def with_arm_disabled(lineno):
    """Force one `if (dev_err_i)` arm false, run, restore. Returns fail count."""
    original = RTL.read_text()
    try:
        lines = original.splitlines(keepends=True)
        idx = lineno - 1
        if "if (dev_err_i) begin" not in lines[idx]:
            raise RuntimeError(
                f"{RTL.name}:{lineno} is not an `if (dev_err_i)` arm any more — "
                "the ARMS table in this script is stale, which is exactly the "
                "class it exists to catch")
        lines[idx] = lines[idx].replace("if (dev_err_i) begin", "if (1'b0) begin")
        RTL.write_text("".join(lines))
        return run_suite()[2]
    finally:
        RTL.write_text(original)


def rtl_arm_count():
    """How many `if (dev_err_i)` arms the RTL actually has, counted not assumed."""
    return RTL.read_text().count("if (dev_err_i) begin")


def with_edit(path, old, new):
    """Apply one textual edit, run, restore. Returns the fail count."""
    original = path.read_text()
    try:
        if old not in original:
            raise RuntimeError(
                f"{path.name}: mutation anchor not found -- this table is stale, "
                "which is the class this gate exists to catch")
        path.write_text(original.replace(old, new, 1))
        return run_suite()[2]
    finally:
        path.write_text(original)


def readme_numerators():
    """{name: claimed fail count} for every `**Mx** ... fails N of M` claim."""
    out = {}
    flat = " ".join(README.read_text().split())
    for name in (m[0] for m in MUTATIONS):
        hit = re.search(rf"\*\*{name}\*\*.*?[Ff]ails \*?\*?(\d+) of", flat)
        if hit:
            out[name] = int(hit.group(1))
    return out


def readme_arm_rows():
    """{state: count} as the README's coverage table currently claims."""
    rows = {}
    for line in README.read_text().splitlines():
        m = re.match(r"\|\s*(\d+)\s*\|\s*`(S_\w+)`\s*\|\s*\**(\d+)", line)
        if m:
            rows[m.group(2)] = int(m.group(3))
    return rows


def readme_figures(total):
    """Suite-size claims that disagree with `total`, as human-readable strings.

    Two shapes, and they need different tests. A SIZE claim ("90 checks",
    "1 of 90", "the 90-check suite") is stale if it is not `total`. A RESULT
    row ("89 PASS, 1 FAIL") is stale if pass + fail is not `total` -- 89 is a
    perfectly good number in a five-model table and must not be flagged, while
    "82 PASS, 1 FAIL" must be.

    The narrow first version of this function checked only "of N" and
    "N checks," and missed three of the five figures a review found stale
    AFTER this gate had reported the arm table clean. Widening it naively then
    false-positived on the model rows, which is why the two shapes are split.
    """
    flat = " ".join(README.read_text().split())
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
                    help="exit non-zero if the README disagrees with a measurement")
    args = ap.parse_args()

    total, passed, failed = run_suite()
    print(f"baseline: {total} checks, {passed} PASS, {failed} FAIL")
    if failed:
        print("baseline is not green; fix that before trusting any figure below")
        return 1

    bad = []

    bad.extend(readme_figures(total))

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
        mark = "ok " if says == got else "STALE"
        print(f"  [{mark}] {lineno:>4} {state:<10} measured={got:<4} readme={says}")
        if says != got:
            bad.append(f"{state}: README says {says}, measured {got}")

    says_n = readme_numerators()
    print("\nnamed mutations (measured vs README):")
    for name, path, old, new in MUTATIONS:
        got = with_edit(path, old, new)
        claim = says_n.get(name)
        mark = "ok " if claim == got else "STALE"
        print(f"  [{mark}] {name:<4} measured={got:<4} readme={claim}")
        if claim is None:
            bad.append(f"{name}: no `fails N of M` claim found in the README")
        elif claim != got:
            bad.append(f"{name}: README says {claim}, measured {got}")

    if bad:
        print("\nfigures disagree with the tree:")
        for b in bad:
            print("  -", b)
        print("\nRe-measure and update README.md. Do NOT edit only the rows you\n"
              "changed: four review rounds found staleness in the rows nobody\n"
              "thought to check.")
        # Always non-zero. The first version returned 0 without --check,
        # so a caller that forgot the flag got a clean exit over a stale file.
        return 1

    print("\nall figures agree with the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
