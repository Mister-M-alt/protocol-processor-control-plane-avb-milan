#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Clean controls and admission mutants, built only in temporary trees.

Every run of a suite on a control or a mutant builds in a temporary tree of its
own. `--jobs N` runs up to N of them at once; the results are read in the
declared order.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
ADMISSION = "hdl/srp/KL_srp_admission.sv"

sys.path.insert(0, str(REPO / "tb" / "common"))
from mutant_pool import add_jobs_argument, in_order  # noqa: E402

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


def trial(job: tuple[str, str, tuple[str, ...], Path]) -> tuple[int, str]:
    """Build a temporary tree of its own with this admission source and run one suite."""
    source, suite, command, log = job
    with tempfile.TemporaryDirectory(prefix="pp112-mutants-") as tmp:
        tree = Path(tmp)
        build_tree(tree)
        (tree / ADMISSION).write_text(source)
        return run_suite(tree, suite, command, log)


def campaign(output: Path, chosen: list[tuple], jobs: int) -> tuple[int, int]:
    """Run the controls and every chosen mutant; return (checks, failures)."""
    original = (REPO / ADMISSION).read_text()
    variants = [("control", original, {})]
    for label, edits, expects in chosen:
        source = original
        for anchor, replacement, count in edits:
            if source.count(anchor) != count:
                raise RuntimeError(f"{label}: expected {count} copies of {anchor!r}")
            source = source.replace(anchor, replacement)
        variants.append((label, source, expects))
    runs = [(label, name, expects.get(suite) if expects else None)
            for label, _, expects in variants for name, suite, _ in SUITES]
    units = [(source, suite, command, output / f"{label}-{name}.log")
             for label, source, _ in variants for name, suite, command in SUITES]
    checks = 0
    failed = 0
    with in_order(trial, units, jobs) as results:
        for (label, name, expect), (status, contents) in zip(runs, results):
            passed = judge(expect, status, contents)
            checks += 1
            failed += not passed
            print(f"{label} {name}: rc={status} {'PASS' if passed else 'FAIL'}", flush=True)
    return checks, failed


def main() -> int:
    """Run controls and mutants; return 0 only if every expected outcome holds."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--only", nargs="*", default=None)
    add_jobs_argument(parser)
    args = parser.parse_args()
    unknown = sorted(set(args.only or ()) - {m[0] for m in MUTANTS})
    if unknown:
        parser.error("unknown mutant(s): " + " ".join(unknown))
    chosen = [m for m in MUTANTS if not args.only or m[0] in args.only]
    args.output.mkdir(parents=True, exist_ok=True)
    checks, failed = campaign(args.output.resolve(), chosen, args.jobs)
    print(f"{checks} checks: {checks - failed} PASS, {failed} FAIL")
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
