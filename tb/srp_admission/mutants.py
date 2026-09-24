#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Clean controls and admission mutants, built only in temporary trees."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
ADMISSION = "hdl/srp/KL_srp_admission.sv"

#: The suites every tree runs: (log name, suite directory, make arguments).
SUITES = (
    ("admission-2", "tb/srp_admission", ("make", "run", "N=2")),
    ("admission-8", "tb/srp_admission", ("make", "run", "N=8")),
    ("srp-top", "tb/srp_top", ("make", "run")),
)

#: The discard of a round that met a pending source.
PENDING = "assign pend_w   = !invalid_w[aidx_r] && !slope_valid_r[aidx_r];"

#: Each mutant: (label, edits as (anchor, replacement, anchor count), and the
#: named check it must fail in each suite directory). Removing only the fit/
#: refusal validity terms is equivalent (the discard never publishes the
#: round), so the stale-evaluation mutant removes both protections.
MUTANTS = (
    ("stale-evaluation",
     ((" && slope_valid_r[aidx_r]", "", 2), (PENDING, "assign pend_w   = 1'b0;", 1)), {
         "tb/srp_admission": "FAIL: refused current TSpec never pulses a grant",
         "tb/srp_top": "FAIL: H: grow has no grant pulse",
     }),
    ("pending-absent", ((PENDING, "assign pend_w   = 1'b0;", 1),), {
        "tb/srp_admission":
            "FAIL: refused source never granted while a lower source re-declares",
        "tb/srp_top": "FAIL: I: refused source",
    }),
    # A discarded round strobing round_done_o would age the optimistic window
    # while a verdict is held, so the window could close before it publishes.
    ("discarded-round-strobes",
     (("          round_done_o <= 1'b1;\n        end\n",
       "        end\n        round_done_o <= 1'b1;\n", 1),), {
         "tb/srp_admission":
             "FAIL: round publishes the greedy walk over every current declaration",
         "tb/srp_top": "FAIL: J: no Failed while",
     }),
)


def build_tree(tree: Path) -> None:
    """Copy the RTL and the two admission benches, without build products."""
    ignore = shutil.ignore_patterns("obj_*", "__pycache__")
    shutil.copytree(REPO / "hdl", tree / "hdl")
    for suite in ("tb/common", "tb/srp_admission", "tb/srp_top"):
        shutil.copytree(REPO / suite, tree / suite, ignore=ignore)


def run_suite(tree: Path, suite: str, command: tuple[str, ...], log: Path) -> tuple[int, str]:
    """Run one suite in the tree; return its exit status and its log text."""
    with log.open("w") as stream:
        # The twelve legs use fixed source/case loops and cycle observations;
        # decoder drains and the recurring timer cadence are not mutated.
        result = subprocess.run(list(command), cwd=tree / suite, stdout=stream,
                                stderr=subprocess.STDOUT, check=False)
    return result.returncode, log.read_text()


def judge(expect: str | None, status: int, contents: str) -> bool:
    """A control passes clean; a mutant is killed only by its named check."""
    if expect is None:
        return status == 0 and " 0 FAIL" in contents
    return status != 0 and "checks:" in contents and expect in contents


def campaign(tree: Path, output: Path) -> tuple[int, int]:
    """Run the controls and every mutant; return (checks, failures)."""
    original = (REPO / ADMISSION).read_text()
    variants = [("control", original, {})]
    for label, edits, expects in MUTANTS:
        source = original
        for anchor, replacement, count in edits:
            if source.count(anchor) != count:
                raise RuntimeError(f"{label}: expected {count} copies of {anchor!r}")
            source = source.replace(anchor, replacement)
        variants.append((label, source, expects))
    checks = 0
    failed = 0
    for label, source, expects in variants:
        (tree / ADMISSION).write_text(source)
        for name, suite, command in SUITES:
            expect = expects.get(suite) if expects else None
            status, contents = run_suite(tree, suite, command, output / f"{label}-{name}.log")
            passed = judge(expect, status, contents)
            checks += 1
            failed += not passed
            print(f"{label} {name}: rc={status} {'PASS' if passed else 'FAIL'}", flush=True)
    return checks, failed


def main() -> int:
    """Run controls and mutants; return 0 only if every expected outcome holds."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pp112-mutants-") as tmp:
        tree = Path(tmp)
        build_tree(tree)
        checks, failed = campaign(tree, args.output.resolve())
    print(f"{checks} checks: {checks - failed} PASS, {failed} FAIL")
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
