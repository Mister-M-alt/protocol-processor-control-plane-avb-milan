#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The packer gate's check-suppression proof: each lint check, dropped alone,
fails the gate.

For every check of the lint's CHECKS, one child process replaces the lint's
finding recorder (RuleContext.bad) with one that drops that check's findings,
then runs the whole gate, test_gen_desc_image.py, in that process. The gate
must fail. A control run drops nothing and must pass. No file is written and
nothing outside the child process changes.

usage: lint_suppression.py [--jobs N]     the proof; exit 0 when the control
                                          passes and every check is killed
       lint_suppression.py --check NAME   one run (NONE: the control)
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import io
from pathlib import Path
import subprocess
import sys
from typing import Any
import unittest

import test_gen_desc_image as gate

LINT = gate.gen_desc_image.model_lint
FAILED = 3          # a child's status when the gate failed, so a crash cannot pass for it


def run_one(check: str) -> int:
    """Run the gate with `check`'s findings dropped; print the failing tests.
    Exit status 0 when the gate passes, FAILED when it fails (a crash is 1)."""
    if check != "NONE" and check not in LINT.CHECKS:
        raise SystemExit(f"lint_suppression: no check {check!r}")
    record = LINT.RuleContext.bad

    def _dropping(self: Any, name: str, where: Any, detail: str) -> None:
        if name != check:
            record(self, name, where, detail)

    LINT.RuleContext.bad = _dropping
    suite = unittest.defaultTestLoader.loadTestsFromModule(gate)
    result = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
    failing = sorted({test.id().split(".", 1)[1] for test, _ in result.failures + result.errors})
    print(f"{len(failing)} failing: {', '.join(failing)}")
    return 0 if result.wasSuccessful() else FAILED


def child(check: str) -> tuple[str, int, str]:
    """(check, exit status, failing tests) of one run in its own process."""
    proc = subprocess.run([sys.executable, "-B", str(Path(__file__).resolve()), "--check", check],
                          capture_output=True, text=True, check=False,
                          cwd=Path(__file__).resolve().parent)
    return check, proc.returncode, (proc.stdout + proc.stderr).strip()


def main() -> int:
    """The proof over every check, or one run with --check."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", help="run the gate once with this check dropped")
    parser.add_argument("--jobs", type=int, default=8, help="child processes at once")
    args = parser.parse_args()
    if args.check:
        return run_one(args.check)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        runs = list(pool.map(child, ["NONE", *sorted(LINT.CHECKS)]))
    ok = runs[0][1] == 0
    print(f"control (nothing dropped): {'passes' if ok else 'FAILS'}: {runs[0][2]}")
    killed = 0
    for check, status, failing in runs[1:]:
        killed += status == FAILED
        verdict = {FAILED: "killed", 0: "SURVIVED"}.get(status, f"ERROR (exit {status})")
        print(f"{check}: {verdict}: {failing}")
    print(f"{killed} of {len(runs) - 1} checks killed")
    return 0 if ok and killed == len(runs) - 1 else 1


if __name__ == "__main__":
    sys.exit(main())
