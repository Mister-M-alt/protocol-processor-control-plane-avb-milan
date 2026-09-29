#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Apply reviewed MAAP patch arms in scratch and require named simulation failures.

No behavioral oracle reads production source. Each patch is explicit reviewable
input to git apply; the driver reads only simulation logs. Every suite it runs
is cycle-bounded (the MAAP walk waits on compressed-ms budgets, the validator
feeds finite frames, the pp_top MP section waits on DUT-cycle budgets). A build
failure, a missing tally or a clean run never counts as a kill.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATCHES = Path(__file__).resolve().parent / "mutations"

# label, suite, make target, required failing assertion prefix. A label may
# appear on several rows: the same planted defect must then fail every suite.
MUTANTS = [
    ('fit-compare-forced-true', 'maap', 'run', 'U17:'),
    ('fit-compare-off-by-one', 'maap', 'run', 'U17:'),
    ('seed-clamp-removed', 'maap', 'run', 'U18:'),
    ('release-keeps-draw-mark', 'maap', 'run', 'U17b:'),
    ('validator-maap-version-1-only', 'rx_validator', 'run', 'F28'),
    ('validator-maap-version-1-only', 'pp_top', 'maap-internal', 'MP7:'),
    ('compare-mac-forward', 'maap', 'run', 'U7:'),
    ('compare-mac-forward', 'pp_top', 'maap-internal', 'MP4:'),
    ('probe-rprobe-never-yields', 'maap', 'run', 'U19:'),
    ('defend-rdefend-ignored', 'maap', 'run', 'U21:'),
    ('defend-rdefend-no-tiebreak', 'maap', 'run', 'U20:'),
    ('probe-rannounce-tiebreak', 'maap', 'run', 'U22:'),
    ('yield-reuses-range', 'maap', 'run', 'U19:'),
    ('ival-sends-after-release', 'maap', 'run', 'U23:'),
    ('post-publishes-after-release', 'maap', 'run', 'U24:'),
    ('tx-path-absorbs-release', 'maap', 'run', 'U25:'),
    ('off-waits-for-an-edge', 'maap', 'run', 'U26:'),
    ('rx-release-returns-to-idle', 'maap', 'run', 'U26:'),
    ('seed-rearmed-on-idle-release-only', 'maap', 'run', 'U27:'),
    ('seed-clamp-off-by-one', 'maap', 'run', 'U18b:'),
]

# the two tally shapes: a suite's canonical line, and one pp_top build's line
TALLY = re.compile(r"(\d+) checks: \d+ PASS, (\d+) FAIL|\] (\d+) checks, (\d+) failures")


def run(tree: Path, suite: str, target: str, log: Path) -> tuple[int, str]:
    """Build and run one cycle-bounded suite target, keeping its full receipt."""
    with log.open("w") as stream:
        result = subprocess.run(["make", "-C", str(tree / "tb" / suite), target],
                                stdout=stream, stderr=subprocess.STDOUT, check=False)
    return result.returncode, log.read_text()


def tally(contents: str) -> tuple[int, int] | None:
    """The last tally line as (checks, failures), or None if the run never finished."""
    found = TALLY.findall(contents)
    if not found:
        return None
    last = found[-1]
    return (int(last[0]), int(last[1])) if last[0] else (int(last[2]), int(last[3]))


def plant(tree: Path, label: str) -> None:
    """Restore the scratch RTL and apply one explicit patch, refusing drift."""
    shutil.copytree(ROOT / "hdl", tree / "hdl", dirs_exist_ok=True)
    patch = str(PATCHES / (label + ".patch"))
    subprocess.run(["git", "apply", "--check", patch], cwd=tree, check=True)
    subprocess.run(["git", "apply", patch], cwd=tree, check=True)


def controls(tree: Path, output: Path, selected: list[tuple]) -> tuple[int, int]:
    """Every suite target the arms use must first pass unmutated."""
    passed = 0
    total = 0
    for suite, target in sorted({(m[1], m[2]) for m in selected}):
        shutil.copytree(ROOT / "hdl", tree / "hdl", dirs_exist_ok=True)
        rc, contents = run(tree, suite, target, output / f"control-{suite}-{target}.log")
        counted = tally(contents)
        ok = rc == 0 and counted is not None and counted[1] == 0
        total += 1
        passed += ok
        print(f"control {suite} {target}: rc={rc} tally={counted} {'PASS' if ok else 'FAIL'}",
              flush=True)
        if not ok:
            print(contents[-4000:], flush=True)
    return passed, total


def arms(tree: Path, output: Path, selected: list[tuple]) -> tuple[int, int]:
    """Each arm must finish its simulation red with its own named assertion."""
    passed = 0
    total = 0
    for label, suite, target, expected in selected:
        plant(tree, label)
        rc, contents = run(tree, suite, target, output / f"{label}-{suite}.log")
        counted = tally(contents)
        failures = [line for line in contents.splitlines() if line.startswith("FAIL:")]
        named = [line for line in failures if line[len("FAIL:"):].strip().startswith(expected)]
        ok = rc != 0 and counted is not None and counted[1] > 0 and bool(named)
        total += 1
        passed += ok
        verdict = "KILLED" if ok else "UNPROVEN"
        print(f"{label} [{suite}]: rc={rc} tally={counted} named={len(named)} {verdict}",
              flush=True)
        for line in named[:3]:
            print(f"    {line}", flush=True)
        if not ok:
            print(contents[-2500:], flush=True)
    return passed, total


def main() -> int:
    """Select patch arms, isolate all writes, and fail on any unproven result."""
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
    with tempfile.TemporaryDirectory(prefix="maap-mutants-") as tmp:
        tree = Path(tmp)
        shutil.copytree(ROOT / "hdl", tree / "hdl")
        for suite in sorted({"common"} | {m[1] for m in selected}):
            shutil.copytree(ROOT / "tb" / suite, tree / "tb" / suite,
                            ignore=shutil.ignore_patterns("obj_*", "__pycache__", "*.hex"))
        control_passed, control_total = controls(tree, args.output, selected)
        if control_passed != control_total:
            print(f"{control_total} checks: {control_passed} PASS, "
                  f"{control_total - control_passed} FAIL")
            return 1
        arm_passed, arm_total = arms(tree, args.output, selected)
    passed = control_passed + arm_passed
    total = control_total + arm_total
    print(f"{total} checks: {passed} PASS, {total - passed} FAIL")
    return int(passed != total)


if __name__ == "__main__":
    raise SystemExit(main())
