#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Consistency checks for the compliance review: REQ rows, GAP ids, Ver vocabulary."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REVIEW = os.path.join(ROOT, "docs", "00_MILAN_COMPLIANCE_REVIEW.md")
VER_VOCAB = {"DIR", "MTXW", "TOL", "TIM", "RND", "STORM", "NVM", "—", "lint"}
REQ_ROW = re.compile(r"^\|\s*(REQ-[A-Z]+-\d{3})\s*\|(.*)$", re.M)
GAP_DEF = re.compile(r'<a id="(gap-\d+)">')
GAP_REF = re.compile(r"\(#(gap-\d+)\)")


def main() -> int:
    body = open(REVIEW, encoding="utf-8").read()
    problems, seen = [], {}

    for req, rest in REQ_ROW.findall(body):
        cols = [c.strip() for c in rest.split("|")]
        if req in seen:
            problems.append(f"duplicate {req}")
        seen[req] = cols
        # clause, requirement, mandate, coverage, finding, arch, doc, ver
        if len(cols) < 8:
            problems.append(f"{req}: only {len(cols)} columns")
            continue
        for idx, label in ((0, "clause"), (1, "requirement"), (5, "arch"), (6, "doc")):
            if not cols[idx]:
                problems.append(f"{req}: empty {label}")
        ver = cols[7].strip()
        if ver and ver not in VER_VOCAB:
            problems.append(f"{req}: unknown Ver category '{ver}'")

    defined = set(GAP_DEF.findall(body))
    referenced = set(GAP_REF.findall(body))
    for gap in sorted(referenced - defined):
        problems.append(f"{gap}: referenced but never defined")
    for gap in sorted(defined - referenced):
        problems.append(f"{gap}: defined but never referenced (needs a disposition row)")

    for line in problems:
        print(f"MATRIX FAIL: {line}")
    print(f"matrix: {len(seen)} REQ rows, {len(defined)} GAP findings, "
          f"{'OK' if not problems else str(len(problems)) + ' FAILURES'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
