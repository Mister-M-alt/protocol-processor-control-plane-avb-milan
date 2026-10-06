#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant reviewed ADP patch arms in scratch and require named check failures.

Each arm is an explicit patch in mutations/, applied with git apply to a
scratch copy of the tree of its own; this driver reads only simulation logs,
never production source. Every suite it runs is cycle-bounded. A positive
control of each (suite, target) pair runs first, each in its own copy, and must
pass; an arm is KILLED only when its simulation completed (a tally was printed),
failed, and printed the required check. A build failure or a missing tally never
counts as a kill. `--jobs N` runs up to N copies at once; the results are read
in the declared order.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATCHES = Path(__file__).resolve().parent / "mutations"

sys.path.insert(0, str(ROOT / "tb" / "common"))
from mutant_pool import add_jobs_argument, in_order  # noqa: E402

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
    # issue #85 item 3: one arm per F04.3 arc, each required to fail that arc's own check
    ("arc-bind-keeps-discovered", "arc-bind-keeps-discovered", "adp_engine", "run",
     "P13 F04.3 arc [*] -> TK_NOT_DISCOVERED (sink bound)"),
    ("arc-discover-no-noadp-arm", "arc-discover-no-noadp-arm", "adp_engine", "run",
     "P13 F04.3 arc NOT -> DISCOVERED (GM and domain match)"),
    ("arc-not-discovered-no-guard", "arc-not-discovered-no-guard", "adp_engine", "run",
     "P13 F04.3 arc NOT -> NOT (GM or domain mismatch)"),
    ("arc-fresh-no-rearm", "arc-fresh-no-rearm", "adp_engine", "run",
     "P13 F04.3 arc DISCOVERED -> DISCOVERED (index > last)"),
    ("arc-fresh-no-store", "arc-fresh-no-store", "adp_engine", "run",
     "P13 F04.3 arc DISCOVERED -> DISCOVERED (index > last)"),
    ("arc-fresh-store-plus-one", "arc-fresh-store-plus-one", "adp_engine", "run",
     "P13 F04.3 arc DISCOVERED -> DISCOVERED (index > last)"),
    ("arc-fresh-store-max", "arc-fresh-store-max", "adp_engine", "run",
     "P13 F04.3 arc DISCOVERED -> DISCOVERED (index > last)"),
    ("arc-restart-detector-off-by-one", "arc-restart-detector-off-by-one", "adp_engine", "run",
     "P13 F04.3 arc DISCOVERED -> DISCOVERED (index <= last, GM matches: restart pair)"),
    ("arc-restart-skips-guard", "arc-restart-skips-guard", "adp_engine", "run",
     "P13 F04.3 arc DISCOVERED -> NOT (index <= last, GM mismatch)"),
    ("arc-departing-silent", "arc-departing-silent", "adp_engine", "run",
     "P13 F04.3 arc DISCOVERED -> NOT (DEPARTING, interface matches)"),
    ("arc-noadp-expiry-silent", "arc-noadp-expiry-silent", "adp_engine", "run",
     "P13 F04.3 arc DISCOVERED -> NOT (T-ADP-NOADP expiry)"),
    # issue #69: the redundancy seam at two AVB interfaces, each arm collapsing the
    # interface count, index or sample; graded by this suite's second build (section
    # IF) and by tb/pp_top's seventh build (section IF) and its if-guards lint
    ("if-ingress-collapsed", "if-ingress-collapsed", "adp_engine", "interfaces",
     "IF5: ENTITY_DISCOVER on interface 1"),
    ("if-egress-collapsed", "if-egress-collapsed", "adp_engine", "interfaces", "IF3"),
    ("if-pdu-index-collapsed", "if-pdu-index-collapsed", "adp_engine", "interfaces", "IF3"),
    ("if-gm-sample-collapsed", "if-gm-sample-collapsed", "adp_engine", "interfaces", "IF3"),
    ("if-top-count-collapsed", "if-top-count-collapsed", "pp_top", "interfaces", "IF1"),
    ("if-top-count-collapsed-lint", "if-top-count-collapsed", "pp_top", "if-guards",
     "if guard 2"),
    ("if-top-ingress-collapsed", "if-top-ingress-collapsed", "pp_top", "interfaces",
     "IF2: ENTITY_DISCOVER received on interface 1"),
    ("if-top-ingress-live", "if-top-ingress-live", "pp_top", "interfaces",
     "IF2: ENTITY_DISCOVER received on interface 1"),
    ("if-top-range-unguarded", "if-top-range-unguarded", "pp_top", "if-guards", "if guard 3"),
    ("if-link-collapsed", "if-link-collapsed", "adp_engine", "interfaces", "IF1"),
    ("if-gm-slice-reversed", "if-gm-slice-reversed", "adp_engine", "interfaces", "IF2"),
    ("if-link-fall-collapsed", "if-link-fall-collapsed", "adp_engine", "interfaces", "IF7"),
    ("if-aidx-reset-by-index", "if-aidx-reset-by-index", "adp_engine", "interfaces", "IF0"),
    ("if-ingress-forced-one", "if-ingress-forced-one", "adp_engine", "interfaces",
     "IF5: ENTITY_DISCOVER on interface 0"),
    ("if-top-range-floor-off-by-one", "if-top-range-floor-off-by-one", "pp_top", "if-guards",
     "if guard 1"),
    ("if-top-range-floor-dropped", "if-top-range-floor-dropped", "pp_top", "if-guards",
     "if guard 0"),
]

SUITES = ("common", "adp_engine", "pp_top")


def run(tree: Path, suite: str, target: str, log: Path) -> tuple[int, str]:
    """Build and run one cycle-bounded suite target, keeping its whole log."""
    with log.open("w") as stream:
        result = subprocess.run(["make", "-C", str(tree / "tb" / suite), target],
                                stdout=stream, stderr=subprocess.STDOUT, check=False)
    return result.returncode, log.read_text()


def plant(tree: Path, patch: str) -> None:
    """Apply one explicit patch to the scratch RTL, refusing drift."""
    path = str(PATCHES / (patch + ".patch"))
    subprocess.run(["git", "apply", "--check", path], cwd=tree, check=True)
    subprocess.run(["git", "apply", path], cwd=tree, check=True)


def trial(job: tuple[str | None, str, str, Path]) -> tuple[int, str]:
    """Copy the tree to a scratch directory of its own, plant the patch if any, and run."""
    patch, suite, target, log = job
    with tempfile.TemporaryDirectory(prefix="adp-mutants-") as tmp:
        tree = Path(tmp)
        shutil.copytree(ROOT / "hdl", tree / "hdl")
        for name in SUITES:
            shutil.copytree(ROOT / "tb" / name, tree / "tb" / name,
                            ignore=shutil.ignore_patterns("obj_*", "*.hex", "__pycache__"))
        if patch is not None:
            plant(tree, patch)
        return run(tree, suite, target, log)


def failures_of(contents: str) -> list[str]:
    """The failing-check lines of one simulation log, in the order printed."""
    return [line for line in contents.splitlines() if line.startswith("FAIL:")]


def campaign(output: Path, selected: list[tuple], jobs: int) -> tuple[int, int]:
    """Run the positive controls, then require each arm's own named check."""
    passed = 0
    total = 0
    pairs = sorted({(m[2], m[3]) for m in selected})
    units = [(None, suite, target, output / f"control-{suite}-{target}.log")
             for suite, target in pairs]
    with in_order(trial, units, jobs) as results:
        for (suite, target), (rc, contents) in zip(pairs, results):
            ok = rc == 0 and "checks" in contents and not failures_of(contents)
            total += 1
            passed += ok
            print(f"control {suite} {target}: rc={rc} {'PASS' if ok else 'FAIL'}", flush=True)
            if not ok:
                print(contents[-4000:])
                return passed, total
    units = [(patch, suite, target, output / f"{arm}.log")
             for arm, patch, suite, target, _ in selected]
    with in_order(trial, units, jobs) as results:
        for (arm, patch, suite, target, expected), (rc, contents) in zip(selected, results):
            failures = failures_of(contents)
            named = [line for line in failures
                     if line[len("FAIL:"):].strip().startswith(expected)]
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
    return passed, total


def main() -> int:
    """Select arms, isolate every write in scratch trees, fail on any unproven arm."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--only", default="")
    add_jobs_argument(parser)
    args = parser.parse_args()
    requested = set(args.only.split(",")) if args.only else {m[0] for m in MUTANTS}
    unknown = requested - {m[0] for m in MUTANTS}
    if unknown:
        parser.error(f"unknown mutation arms: {sorted(unknown)}")
    selected = [m for m in MUTANTS if m[0] in requested]
    args.output.mkdir(parents=True, exist_ok=True)
    passed, total = campaign(args.output, selected, args.jobs)
    print(f"{total} checks: {passed} PASS, {total - passed} FAIL")
    return int(passed != total)


if __name__ == "__main__":
    raise SystemExit(main())
