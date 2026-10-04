#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Apply reviewed patch arms in scratch and require named simulation failures.

No behavioral oracle reads production source. Each patch is explicit reviewable
input to git apply; the driver reads only simulation logs. SRP top has a global
DUT-cycle guard; the encoder and stream-FSM suites have finite cycle-bounded
walks. A budget exit, missing tally or build failure never counts as a kill.
Every control and every arm builds and runs in a scratch copy of its own;
`--jobs N` runs up to N copies at once, and the results are read in the
declared order.
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

# label, suite, group, required failing assertion prefix.
MUTANTS = [
    ('bad-listener-length', 'srp_top', 'phases', 'K12:'),
    ('repeated-action', 'srp_top', 'congestion', 'N4:'),
    ('expiry-event', 'srp_top', 'phases', 'K2:'),
    ('before-slot', 'srp_top', 'edge', 'L1:'),
    ('talker-no-own', 'srp_top', 'phases', 'K2:'),
    ('listener-no-own', 'srp_top', 'phases', 'K11:'),
    ('talker-lost-txla', 'srp_stream_fsms', 'suite', 'T QA txla=2:'),
    ('listener-lost-txla', 'srp_stream_fsms', 'suite', 'L QA txla=2:'),
    ('talker-strict-lv', 'srp_top', 'phases', 'K2:'),
    ('listener-strict-lv', 'srp_top', 'phases', 'K11:'),
    ('listener-sid-ignored', 'srp_top', 'phases', 'K5:'),
    ('talker-no-renewal', 'srp_top', 'phases', 'K7:'),
    ('listener-no-renewal', 'srp_top', 'phases', 'K7:'),
    ('talker-no-expiry', 'srp_top', 'phases', 'K8:'),
    ('listener-no-expiry', 'srp_top', 'phases', 'K8:'),
    ('pending-peer-ignored', 'srp_top', 'peer', 'M1:'),
    ('encoder-peer-ignored', 'srp_top', 'peer', 'M6:'),
    ('mvrp-supersedes-msrp', 'srp_top', 'peer', 'M1:'),
    ('reset-retains-intent', 'srp_top', 'phases', 'K10:'),
    ('full-drain-missing', 'srp_top', 'congestion', 'N6:'),
    ('already-full-missed', 'srp_top', 'congestion', 'N10:'),
    ('round-completion-lost', 'srp_top', 'congestion', 'N13:'),
    ('own-flags-missing', 'srp_top', 'congestion', 'N5:'),
    ('expiry-congestion', 'srp_top', 'congestion', 'N1:'),
    ('missing-registrar-edge', 'srp_top', 'edge', 'L3:'),
    ('empty-canceled-pdu', 'srp_top', 'peer', 'M11:'),
    ('reserved-slot-not-reused', 'srp_top', 'peer', 'M12:'),
    ('renewal-congestion', 'srp_top', 'congestion', 'N7:'),
    ('leaveall-expiry-lost', 'srp_top', 'peer', 'M9:'),
    ('expiry-outranks-peer', 'srp_top', 'peer', 'M10:'),
    ('repeated-expiry-queued', 'srp_top', 'congestion', 'N12:'),
    ('receive-priority-lost', 'srp_top', 'edge', 'L2:'),
    ('action-omitted', 'srp_top', 'congestion', 'N3:'),
    ('preparation-before-slot', 'srp_top', 'peer', 'M5:'),
    ('table-one-short', 'srp_top', 'congestion', 'N9:'),
    ('my-expiry-pulse', 'srp_top', 'phases', 'K2:'),
    ('my-join-edge-peer', 'srp_top', 'guards', 'O3:'),
    ('my-cancel-eats-new-intent', 'srp_top', 'guards', 'O2:'),
    ('my-edge-cancel-lost', 'srp_top', 'peer', 'M6:'),
    ('my-la-outranks-leave', 'srp_top', 'edge', 'L2:'),
    ('my-drop-join-during-wait', 'srp_top', 'guards', 'O6:'),
    ('my-no-walk-at-sLA', 'srp_top', 'congestion', 'N6:'),
    ('my-reuse-flags-ignore-cancel', 'srp_top', 'guards', 'O4:'),
    ('r-expiry-pulse-restored', 'srp_top', 'phases', 'K2:'),
    ('r-cancel-latch-dropped', 'srp_top', 'guards', 'O1:'),
    ('r-cancel-consumes-new-intent', 'srp_top', 'guards', 'O2:'),
    ('r-join-start-guard-dropped', 'srp_top', 'guards', 'O3:'),
    ('r-sink-receive-priority-lost', 'srp_top', 'guards', 'O5:'),
    ('r-join-coalesce-dropped', 'srp_top', 'guards', 'O6:'),
    ('r-la-only-emission-lost', 'srp_top', 'peer', 'M12:'),
    ('r-collect-blocks-pushes', 'srp_top', 'congestion', 'N6:'),
    ('r-accept-edge-drain-lost', 'srp_top', 'congestion', 'N10:'),
    ('r-reuse-without-action', 'srp_top', 'peer', 'M12:'),
    ('r-mvrp-start-during-prepare', 'srp_encoder', '', 'O8:'),
    ('canceled-content-never-drains', 'srp_top', 'guards', 'O7:'),
    # issue #108: Table 10-5 rLA! restarts the leavealltimer and goes Passive
    ('expiry-only-redraw', 'srp_top', 'restart', 'P1:'),
    ('mvrp-expiry-only-redraw', 'srp_top', 'restart', 'P2:'),
    ('stale-expiry-honoured', 'srp_top', 'peer', 'M10:'),
    ('mvrp-stale-expiry-honoured', 'srp_top', 'restart', 'P6:'),
    ('mvrp-passive-lost', 'srp_top', 'restart', 'P4:'),
    ('mvrp-flag-at-expiry', 'srp_top', 'restart', 'P5:'),
    # issue #108: the stale-expiry guard holds whatever the arm-path latency
    ('rearm-at-issue', 'srp_top', 'armdelay', 'P8:'),
    ('r-rearm-no-inflight', 'srp_top', 'peer', 'M10:'),
    ('r-rearm-no-inflight', 'srp_top', 'restart', 'P6:'),
    ('r-rearm-no-deadline', 'srp_top', 'peer', 'M10:'),
    ('r-rearm-no-deadline', 'srp_top', 'armdelay', 'P8:'),
    ('r-flag-ignores-edge-peer', 'srp_top', 'restart', 'P7:'),
    # issue #64: Milan v1.2 Table 4.3 timer grading
    ('join-ms-400', 'srp_top', 'timers', 'Q1:'),
    ('periodic-ms-3000', 'srp_top', 'timers', 'Q2:'),
    ('draw-kind-0', 'srp_top', 'timers', 'Q3:'),
    # issue #65: MVRP join before the stream, both halves
    ('licence-ignores-join', 'srp_top', 'join', 'R1:'),
    ('licence-ignores-join', 'srp_stream_fsms', 'suite', 'T not ACTIVE while the VID'),
    ('join-sent-at-handover', 'srp_top', 'join', 'R1:'),
    ('count-up-unsends', 'srp_top', 'join', 'R2:'),
    ('listener-lane-cut', 'srp_top', 'join', 'R4:'),
    ('join-sent-at-handover', 'srp_encoder', '', 'W2 New VID 7 handed over'),
    ('count-up-unsends', 'srp_encoder', '', 'W4 a second user'),
    ('tx-strobe-any-app', 'srp_encoder', '', 'W1 no strobe for an MSRP'),
    # issue #230: the storage paths at both elaboration arms. The timer-arm
    # FIFOs (store_main.cpp, sources/sinks 1/1, 2/2, 3/5, 9/9) ...
    ('tf-heads-swapped', 'srp_top', 'storage', 'TF1:'),
    ('tf-head-at-write-pointer', 'srp_top', 'storage', 'TF1:'),
    ('tf-listener-push-dropped', 'srp_top', 'storage', 'TF2:'),
    ('tf-ls-written-at-tk-pointer', 'srp_top', 'storage', 'TF2:'),
    ('tf-tk-head-read-ahead', 'srp_top', 'storage', 'TF1:'),
    ('tf-ls-head-reads-tk-ram', 'srp_top', 'storage', 'TF2:'),
    ('tf-tk-write-at-rptr', 'srp_top', 'storage', 'TF1:'),
    ('tf-full-guard-31', 'srp_top', 'storage', 'TF4:'),
    ('tf-tk-write-ignores-full', 'srp_top', 'storage', 'TF4:'),
    ('tf-ls-write-ignores-full', 'srp_top', 'storage', 'TF5:'),
    # ... the walk records (walk_main.cpp, the same four shapes) ...
    ('walk-record-written-on-close', 'srp_stream_fsms', 'walk', 'WK3:'),
    ('wtsp-first-open-only', 'srp_stream_fsms', 'walk', 'WK2:'),
    ('wtsp-read-at-gate-source', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-read-at-source-0', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-latency-field-shifted', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-latency-shifted', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-rank-dropped', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-prio-rank-swapped', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-ram-first-open-only', 'srp_stream_fsms', 'walk', 'WK2:'),
    ('wid-ram-read-at-gate-source', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-ram-read-neighbour', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-flops-da-of-gate-source', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-flops-sid-of-source-0', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-flops-vid-of-source-0', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('talker-vid-unreset', 'srp_stream_fsms', 'walk', 'WK5:'),
    ('wsid-ram-first-settle-only', 'srp_stream_fsms', 'walk', 'WK8:'),
    ('wsid-ram-written-on-teardown', 'srp_stream_fsms', 'walk', 'WK7:'),
    ('wsid-flops-of-control-sink', 'srp_stream_fsms', 'walk', 'WK6:'),
    ('wsid-flops-read-sink-0', 'srp_stream_fsms', 'walk', 'WK6:'),
    ('wtsp-write-ignores-ready', 'srp_stream_fsms', 'walk', 'WK9:'),
    ('wsid-write-ignores-ready', 'srp_stream_fsms', 'walk', 'WK10:'),
    # ... and the admission slopes (the admission suite, 1 to 8 sources)
    ('slope-stored-at-stage-2-index', 'srp_admission', '',
     'round publishes the greedy walk over every current declaration'),
    ('slope-stored-at-source-0', 'srp_admission', '',
     'round publishes the greedy walk over every current declaration'),
    ('slope-store-source-0-only', 'srp_admission', '',
     'round publishes the greedy walk over every current declaration'),
    ('slope-read-source-0', 'srp_admission', '',
     'round publishes the greedy walk over every current declaration'),
]


def run(tree: Path, suite: str, group: str, log: Path) -> tuple[int, str]:
    """Build and run a cycle-bounded suite, preserving its complete receipt."""
    with log.open("w") as stream:
        result = subprocess.run(
            ["make", "-C", str(tree / "tb" / suite), "RUN_ARGS=" + group],
            stdout=stream, stderr=subprocess.STDOUT, check=False)
    return result.returncode, log.read_text()


def plant(tree: Path, label: str) -> None:
    """Apply one explicit patch to the scratch RTL, refusing drift."""
    subprocess.run(["git", "apply", "--check", str(PATCHES / (label + ".patch"))],
                   cwd=tree, check=True)
    subprocess.run(["git", "apply", str(PATCHES / (label + ".patch"))],
                   cwd=tree, check=True)


def trial(job: tuple[str | None, str, str]) -> tuple[int, str]:
    """Copy the RTL and the suites to a scratch tree of its own, plant the arm if any, and run.

    The receipt is kept inside the copy and returned: a label that runs on two
    suites has one receipt name, so the caller writes the receipts in the
    declared order, the later over the earlier, as a serial run leaves them.
    """
    label, suite, group = job
    with tempfile.TemporaryDirectory(prefix="srp-leaveall-") as tmp:
        tree = Path(tmp)
        shutil.copytree(ROOT / "hdl", tree / "hdl")
        for name in ("common", "srp_top", "srp_stream_fsms", "srp_encoder", "srp_admission"):
            shutil.copytree(ROOT / "tb" / name, tree / "tb" / name,
                            ignore=shutil.ignore_patterns("obj_*", "__pycache__"))
        if label is not None:
            plant(tree, label)
        return run(tree, suite, group, tree / "receipt.log")


def campaign(output: Path, selected: list[tuple], jobs: int) -> tuple[int, int, set[str]]:
    """Run positive controls first, then require each arm's own named assertion."""
    passed = 0
    total = 0
    covered: set[str] = set()
    pairs = sorted({(m[1], m[2]) for m in selected})
    with in_order(trial, [(None, suite, group) for suite, group in pairs], jobs) as results:
        for (suite, group), (rc, contents) in zip(pairs, results):
            (output / f"control-{suite}-{group}.log").write_text(contents)
            ok = rc == 0 and " 0 FAIL" in contents
            total += 1
            passed += ok
            print(f"control {suite} {group}: rc={rc} {'PASS' if ok else 'FAIL'}", flush=True)
            if not ok:
                print(contents[-4000:])
                return passed, total, covered
    units = [(label, suite, group) for label, suite, group, _ in selected]
    with in_order(trial, units, jobs) as results:
        for (label, suite, group, expected), (rc, contents) in zip(selected, results):
            (output / f"{label}.log").write_text(contents)
            failures = [line for line in contents.splitlines() if line.startswith("FAIL:")]
            ok = (rc != 0 and "checks:" in contents and "CYCLE_BUDGET" not in contents
                  and any(expected in line for line in failures))
            total += 1
            passed += ok
            tags = sorted({line.split(":", 2)[1].strip() for line in failures})
            if ok:
                covered.update(tags)
            verdict = "KILLED" if ok else "UNPROVEN"
            print(f"{label}: rc={rc} failures={len(failures)} {verdict} tags={','.join(tags)}",
                  flush=True)
            if not ok:
                print(contents[-2500:], flush=True)
    return passed, total, covered


def main() -> int:
    """Select patch arms, isolate all writes, and fail on any unproven result."""
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
    passed, total, covered = campaign(args.output, selected, args.jobs)
    if not args.only:
        expected = {f"{group}{i}" for group, count in
                    [("K", 12), ("L", 4), ("M", 12), ("N", 13), ("O", 8), ("P", 8), ("Q", 4), ("R", 4),
                     ("TF", 5), ("WK", 10)]
                    for i in range(1, count + 1)}
        missing = expected - covered
        total += 1
        passed += not missing
        print(f"assertion coverage: {len(expected - missing)}/{len(expected)}; "
              f"missing={sorted(missing)}")
    print(f"{total} checks: {passed} PASS, {total - passed} FAIL")
    return int(passed != total)


if __name__ == "__main__":
    raise SystemExit(main())
