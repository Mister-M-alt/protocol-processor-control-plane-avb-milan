#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant reviewed ADP patch arms in scratch and require named check failures.

Each arm is an explicit patch in mutations/, applied with git apply to a
scratch copy of the tree; this driver reads only simulation logs, never
production source. Every suite it runs is cycle-bounded. A positive control
of each (suite, target) pair runs first and must pass; an arm is KILLED only
when its simulation completed (a tally was printed), failed, and printed the
required check. A build failure or a missing tally never counts as a kill.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATCHES = Path(__file__).resolve().parent / "mutations"

# arm, patch, suite, make target, the check that must fail (its label prefix)
MUTANTS = [
    # issue #40: current_configuration_index across SET_CONFIGURATION
    ("cfg-read-live", "cfg-read-live", "adp_engine", "run", "P11c"),
    ("cfg-dependent-field", "cfg-dependent-field", "adp_engine", "run", "P11b"),
    ("cfg-dependent-field-top", "cfg-dependent-field", "pp_top", "adp-config", "AD2"),
    ("cfg-frozen-at-top", "cfg-frozen-at-top", "pp_top", "adp-config", "AD2"),
    ("cfg-overlay-only", "cfg-overlay-only", "pp_top", "adp-config", "AD1"),
    ("cfg-nonzero-for-valid", "cfg-nonzero-for-valid", "pp_top", "adp-config", "AD2"),
    ("cfg-valid-not-sticky", "cfg-valid-not-sticky", "pp_top", "adp-config", "AD2"),
    ("cfg-valid-any-selector", "cfg-valid-any-selector", "pp_top", "adp-config", "AD1b"),
    # issue #41: the boot gate over the full T-ADP-DELAY span
    ("gate-enable-dropped", "gate-enable-dropped", "adp_engine", "run", "P12"),
    ("gate-enable-dropped-top", "gate-enable-dropped", "pp_top", "adp-config", "AD0"),
]

SUITES = ("common", "adp_engine", "pp_top")


def run(tree: Path, suite: str, target: str, log: Path) -> tuple[int, str]:
    """Build and run one cycle-bounded suite target, keeping its whole log."""
    with log.open("w") as stream:
        result = subprocess.run(["make", "-C", str(tree / "tb" / suite), target],
                                stdout=stream, stderr=subprocess.STDOUT, check=False)
    return result.returncode, log.read_text()


def plant(tree: Path, patch: str) -> None:
    """Restore the scratch RTL and apply one explicit patch, refusing drift."""
    shutil.copytree(ROOT / "hdl", tree / "hdl", dirs_exist_ok=True)
    path = str(PATCHES / (patch + ".patch"))
    subprocess.run(["git", "apply", "--check", path], cwd=tree, check=True)
    subprocess.run(["git", "apply", path], cwd=tree, check=True)


def failures_of(contents: str) -> list[str]:
    return [line for line in contents.splitlines() if line.startswith("FAIL:")]


def campaign(tree: Path, output: Path, selected: list[tuple]) -> tuple[int, int]:
    """Run the positive controls, then require each arm's own named check."""
    passed = 0
    total = 0
    for suite, target in sorted({(m[2], m[3]) for m in selected}):
        rc, contents = run(tree, suite, target, output / f"control-{suite}-{target}.log")
        ok = rc == 0 and "checks" in contents and not failures_of(contents)
        total += 1
        passed += ok
        print(f"control {suite} {target}: rc={rc} {'PASS' if ok else 'FAIL'}", flush=True)
        if not ok:
            print(contents[-4000:])
            return passed, total
    for arm, patch, suite, target, expected in selected:
        plant(tree, patch)
        rc, contents = run(tree, suite, target, output / f"{arm}.log")
        failures = failures_of(contents)
        named = [line for line in failures if line[len("FAIL:"):].strip().startswith(expected)]
        ok = rc != 0 and "checks" in contents and bool(named)
        total += 1
        passed += ok
        verdict = "KILLED" if ok else "UNPROVEN"
        print(f"{arm}: rc={rc} failures={len(failures)} named={len(named)} {verdict}",
              flush=True)
        for line in failures:
            print(f"    {line}", flush=True)
        if not ok:
            print(contents[-2500:], flush=True)
    shutil.copytree(ROOT / "hdl", tree / "hdl", dirs_exist_ok=True)
    return passed, total


def main() -> int:
    """Select arms, isolate every write in a scratch tree, fail on any unproven arm."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--only", default="")
    args = parser.parse_args()
    requested = set(args.only.split(",")) if args.only else {m[0] for m in MUTANTS}
    unknown = requested - {m[0] for m in MUTANTS}
    if unknown:
        parser.error(f"unknown mutation arms: {sorted(unknown)}")
    selected = [m for m in MUTANTS if m[0] in requested]
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="adp-mutants-") as tmp:
        tree = Path(tmp)
        shutil.copytree(ROOT / "hdl", tree / "hdl")
        for suite in SUITES:
            shutil.copytree(ROOT / "tb" / suite, tree / "tb" / suite,
                            ignore=shutil.ignore_patterns("obj_*", "*.hex", "__pycache__"))
        passed, total = campaign(tree, args.output, selected)
    print(f"{total} checks: {passed} PASS, {total - passed} FAIL")
    return int(passed != total)


if __name__ == "__main__":
    raise SystemExit(main())
