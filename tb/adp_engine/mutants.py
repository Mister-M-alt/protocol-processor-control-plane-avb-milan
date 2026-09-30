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
    # the D3 writer (PR #132) on the state bus: the flag follows its restore write
    # and its roll-back's reset
    ("cfg-valid-ucpu-bus", "cfg-valid-ucpu-bus", "pp_top", "adp-config", "AD5"),
    ("cfg-valid-hard-reset", "cfg-valid-hard-reset", "pp_top", "adp-config", "AD6"),
    # review R406-1 F-1: the flag's own reset
    ("cfg-valid-no-reset", "cfg-valid-no-reset", "pp_top", "adp-config", "AD7"),
    # issue #41: the boot gate over the full T-ADP-DELAY span
    ("gate-enable-dropped", "gate-enable-dropped", "adp_engine", "run", "P12"),
    ("gate-enable-dropped-top", "gate-enable-dropped", "pp_top", "adp-config", "AD0"),
    # issue #85: the MTXW walk of F04.2 and F04.3 (the first two are the issue's named mutations)
    ("walk-down-answers-discover", "walk-down-answers-discover", "adp_engine", "run",
     "P13 F04.2 RCV_ADP_DISCOVER(eid 0) x DOWN"),
    ("walk-delay-ignores-link-down", "walk-delay-ignores-link-down", "adp_engine", "run",
     "P13 F04.2 LINK_DOWN x DELAY"),
    ("walk-down-answers-gm-change", "walk-down-answers-gm-change", "adp_engine", "run",
     "P13 F04.2 GM_CHANGE x DOWN"),
    ("walk-down-shutdown-departs", "walk-down-shutdown-departs", "adp_engine", "run",
     "P13 F04.2 SHUTDOWN x DOWN"),
    ("walk-delay-answers-discover", "walk-delay-answers-discover", "adp_engine", "run",
     "P13 F04.2 RCV_ADP_DISCOVER(eid 0) x DELAY"),
    ("walk-delay-shutdown-silent", "walk-delay-shutdown-silent", "adp_engine", "run",
     "P13 F04.2 SHUTDOWN x DELAY"),
    ("walk-stale-draw-arms", "walk-stale-draw-arms", "adp_engine", "run",
     "P13 F04.2 LINK_DOWN x DELAY(draw in flight)"),
    ("walk-departing-keeps-index", "walk-departing-keeps-index", "adp_engine", "run",
     "P13 F04.2 SHUTDOWN x WAITING"),
    ("walk-foreign-discover-answered", "walk-foreign-discover-answered", "adp_engine", "run",
     "P13 F04.2 ENTITY_DISCOVER(foreign eid) x WAITING"),
    ("walk-link-down-keeps-timer", "walk-link-down-keeps-timer", "adp_engine", "run",
     "P13 F04.2 LINK_DOWN x WAITING"),
    ("disc-fresh-checks-gm", "disc-fresh-checks-gm", "adp_engine", "run",
     "P13 F04.3 AVAILABLE(GM mismatch, index > last) x TK_DISCOVERED"),
    ("disc-not-discovered-checks-index", "disc-not-discovered-checks-index", "adp_engine", "run",
     "P13 F04.3 AVAILABLE(match, index <= last) x TK_NOT_DISCOVERED"),
    ("disc-not-discovered-checks-interface", "disc-not-discovered-checks-interface", "adp_engine", "run",
     "P13 F04.3 AVAILABLE(interface_index differs) x TK_NOT_DISCOVERED"),
    ("disc-restart-not-rediscovered", "disc-restart-not-rediscovered", "adp_engine", "run",
     "P13 F04.3 AVAILABLE(match, index <= last) x TK_DISCOVERED"),
    ("disc-departing-ignores-interface", "disc-departing-ignores-interface", "adp_engine", "run",
     "P13 F04.3 DEPARTING(interface_index differs) x TK_DISCOVERED"),
    ("disc-stray-noadp-departs", "disc-stray-noadp-departs", "adp_engine", "run",
     "P13 F04.3 TMR_NO_ADP x TK_NOT_DISCOVERED"),
    ("disc-unbind-keeps-timer", "disc-unbind-keeps-timer", "adp_engine", "run",
     "P13 F04.3 UNBIND x TK_DISCOVERED"),
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
    """The failing-check lines of one simulation log, in the order printed."""
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
