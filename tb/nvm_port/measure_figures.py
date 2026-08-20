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

#: (line in the RTL, state name) for every `if (dev_err_i)` arm.
ARMS = [(185, "S_WEREQ"), (194, "S_WEWAIT"), (204, "S_WWREQ"), (214, "S_WHPUMP"),
        (227, "S_WDPUMP"), (237, "S_WWAIT"), (248, "S_RHREQ"), (258, "S_RHCOLL"),
        (276, "S_RHWAIT"), (303, "S_RPREQ"), (313, "S_RPPUMP"), (323, "S_RPWAIT")]

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

    if bad:
        print("\nfigures disagree with the tree:")
        for b in bad:
            print("  -", b)
        print("\nRe-measure and update README.md. Do NOT edit only the rows you\n"
              "changed: four review rounds found staleness in the rows nobody\n"
              "thought to check.")
        return 1 if args.check else 0

    print("\nall figures agree with the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
