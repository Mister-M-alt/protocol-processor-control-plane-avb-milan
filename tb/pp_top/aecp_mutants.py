#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant reviewed AECP deadline and hazard-class arms in scratch and require named check failures.

Lane C5a's campaign (issues #81, #57 and #84): each arm is an explicit patch in
mutations/, applied with git apply to a scratch copy of the tree of its own;
this driver reads only simulation logs, never production source. Every suite it
runs is cycle-bounded. A positive control of each (suite, target) pair runs
first, each in its own copy, and must pass; an arm is KILLED only when its
simulation completed (a tally was printed), failed, and printed the required
check. A build failure or a missing tally never counts as a kill. `--jobs N`
runs up to N copies at once; the results are read in the declared order.
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
    # issue #81 (GAP-07): the deadline read at admission and the kill face
    ("dl-kill-tied-off", "dl-kill-tied-off", "pp_top", "deadline",
     "DL1: the stalled GET_COUNTERS is answered ENTITY_MISBEHAVING"),
    ("dl-released-before-queued", "dl-released-before-queued", "pp_top",
     "deadline", "DL1: the key stayed held"),
    ("dl-armed-at-admission", "dl-armed-at-admission", "pp_top", "deadline",
     "DL3: the queued GET_MILAN_INFO"),
    ("dl-boot-hold-not-exempt", "dl-boot-hold-not-exempt", "pp_top", "d3",
     "D3O6: at the terminal the held command is answered byte-exact"),
    ("dl-gdi-runs-on", "dl-gdi-runs-on", "pp_top", "deadline",
     "DL4: the batch past its deadline is voided"),
    ("dl-edit-preempted", "dl-edit-preempted", "pp_top", "deadline",
     "DL6: the edit answers its own SUCCESS"),
    ("dl-registry-preempted", "dl-registry-preempted", "pp_top", "deadline",
     "DL10 REGISTER_UNSOLICITED_NOTIFICATION past its deadline answers its "
     "own SUCCESS"),
    ("dl-lock-preempted", "dl-lock-preempted", "pp_top", "deadline",
     "DL10 LOCK_ENTITY past its deadline answers its own SUCCESS"),
    ("dl-kill-ack-keeps-owner", "dl-kill-ack-keeps-owner", "pp_top",
     "deadline", "DL1: the honoured kill ended the AECP owner"),
    ("dl-preempt-after-effect", "dl-preempt-after-effect", "ucpu", "run",
     "P20c the program's own SUCCESS"),
    ("dl-preempt-after-effect-top", "dl-preempt-after-effect", "pp_top",
     "deadline", "DL2: past its NAME_WR the SET_NAME answers its own SUCCESS"),
    ("ucpu-preempt-cuts-a-wait", "ucpu-preempt-cuts-a-wait", "ucpu", "run",
     "P20e the waiting locate was answered"),
    ("ucpu-preempt-keeps-the-body", "ucpu-preempt-keeps-the-body", "ucpu",
     "run", "P20f header only"),
    ("ucpu-preempt-repeats", "ucpu-preempt-repeats", "ucpu", "run",
     "P20a completes: one redirect"),
    ("dlkill-always-misbehaving", "dlkill-always-misbehaving", "ucpu", "run",
     "P20d ENTITY_LOCKED kept"),
    # issue #81 acceptance 4: T-LOCK-UNLOCK and T-NOTIF-TIMELIMITED at the
    # top's own defaults (section TD, the sixth build), one side of the
    # window each
    ("td-lock-default-59s", "td-lock-default-59s", "pp_top", "timer-defaults",
     "TD1: T-LOCK-UNLOCK at its default"),
    ("td-tl-default-301s", "td-tl-default-301s", "pp_top", "timer-defaults",
     "TD2: T-NOTIF-TIMELIMITED at its default"),
    # issue #57 (REQ-MVU-005): the MVU answer under the deadline, and its
    # latency against T-AECP-RESP (section TB, the fifth build)
    ("dl-mvu-forced-status-10", "dl-mvu-forced-status-10", "pp_top",
     "deadline", "DL3: the queued GET_MILAN_INFO"),
    # every message type but AEM's under the deadline (IEEE Table 9-2)
    ("dl-non-aem-forced-status-10", "dl-non-aem-forced-status-10", "pp_top",
     "deadline", "DL9 ADDRESS_ACCESS past its deadline: NOT_IMPLEMENTED"),
    # ... and when its response memory fails (REQ-MVU-005, Table 5.19)
    ("mvu-fault-status-10", "mvu-fault-status-10", "pp_top", "deadline",
     "DL8 a response-memory read error: GET_MILAN_INFO answers MVU "
     "NOT_IMPLEMENTED"),
    ("mvu-echo-slot-std", "mvu-echo-slot-std", "pp_top", "deadline",
     "DL8 a read error under a command padded to 540 payload bytes: "
     "GET_MILAN_INFO answers MVU NOT_IMPLEMENTED"),
    ("mvu-silent", "mvu-silent", "pp_top", "budget",
     "TB1 GET_MILAN_INFO at the suite latency"),
    ("fanout-never-ends", "fanout-never-ends", "pp_top", "budget",
     "TB3 GET_MILAN_INFO behind the fan-out is answered as it is idle"),
    # issue #81 acceptance 3: ACMP inside T-BUDGET-ACMP-RESP under AECP load
    ("acmp-waits-for-aecp", "acmp-waits-for-aecp", "pp_top", "budget",
     "TB5 GET_RX_STATE beside the oversize READ_DESCRIPTOR"),
    # issue #84 (GAP-10): the nine F03.7 classes at the scoreboard
    ("hz-stub-restored", "hz-stub-restored", "pp_top", "hazards",
     "HZ1 SET_CONFIGURATION: admitted"),
    ("hz-setcfg-not-barrier", "hz-setcfg-not-barrier", "pp_top", "hazards",
     "HZ2: SET_CONFIGURATION is refused and latches the drain"),
    ("hz-lock-not-lockop", "hz-lock-not-lockop", "pp_top", "hazards",
     "HZ4: LOCK_ENTITY is not admitted while an ACMP stream step holds"),
    ("hz-stream-key-none", "hz-stream-key-none", "pp_top", "hazards",
     "HZ5 START_STREAMING on the held sink's key: not admitted"),
    ("hz-reads-keyed-none", "hz-reads-keyed-none", "pp_top", "hazards",
     "HZ6 GET_STREAM_INFO on the held sink's key: not admitted"),
    ("hz-clock-as-ro", "hz-clock-as-ro", "pp_top", "hazards",
     "HZ1 SET_SAMPLING_RATE AUDIO_UNIT 0: admitted"),
    ("hz-clock-as-lock", "hz-clock-as-lock", "pp_top", "hazards",
     "HZ8 SET_SAMPLING_RATE (CLOCK_CFG): admitted beside"),
    ("hz-name-as-ro", "hz-name-as-ro", "pp_top", "hazards",
     "HZ1 SET_NAME CLOCK_DOMAIN 0: admitted"),
    ("hz-registry-as-ro", "hz-registry-as-ro", "pp_top", "hazards",
     "HZ1 REGISTER_UNSOLICITED_NOTIFICATION: admitted"),
    ("hz-identify-as-ro", "hz-identify-as-ro", "pp_top", "hazards",
     "HZ1 SET_CONTROL CONTROL 0 (identify): admitted"),
    ("hz-acmp-reads-as-steps", "hz-acmp-reads-as-steps", "pp_top", "hazards",
     "HZ1 ACMP GET_RX_STATE sink 1: admitted"),
    ("hz-barrier-no-priority", "hz-barrier-no-priority", "pp_top", "hazards",
     "HZ3: both the barrier and the queued ACMP command are answered"),
    ("hz-foreign-target-classified", "hz-foreign-target-classified", "pp_top",
     "hazards", "HZ1 a SET_CONFIGURATION for another entity_id: admitted"),
    ("hz-response-classified", "hz-response-classified", "pp_top", "hazards",
     "HZ1 an AEM_RESPONSE arriving as input: admitted"),
    # issue #84 acceptance 2: every class and key pair that can conflict at
    # this top, on the listener's and the talker's keys, one arm each
    ("hz-name-key-none", "hz-name-key-none", "pp_top", "hazards",
     "HZ9a SET_NAME on STREAM_INPUT 1 vs a held GET_RX_STATE of sink 1: "
     "not admitted"),
    ("hz-name-key-none-talker", "hz-name-key-none", "pp_top", "hazards",
     "HZ9d a GET_TX_STATE of source 1 vs a held SET_NAME on STREAM_OUTPUT "
     "1: not admitted"),
    ("hz-name-key-none-held", "hz-name-key-none", "pp_top", "hazards",
     "HZ9f a GET_RX_STATE of sink 1 vs a held SET_NAME on STREAM_INPUT 1: "
     "not admitted"),
    ("hz-name-as-stream", "hz-name-as-stream", "pp_top", "hazards",
     "HZ9c SET_NAME on STREAM_INPUT 1 vs a held UNBIND_RX of sink 1: "
     "admitted beside"),
    ("hz-stream-key-none-vs-read", "hz-stream-key-none", "pp_top", "hazards",
     "HZ10a STOP_STREAMING on STREAM_INPUT 1 vs a held GET_RX_STATE of "
     "sink 1: not admitted"),
    ("hz-stream-key-none-talker-read", "hz-stream-key-none", "pp_top",
     "hazards", "HZ10b a GET_TX_STATE of source 1 vs a held STOP_STREAMING "
     "on STREAM_OUTPUT 1: not admitted"),
    ("hz-stream-key-none-talker-step", "hz-stream-key-none", "pp_top",
     "hazards", "HZ10c a DISCONNECT_TX of source 1 vs a held "
     "STOP_STREAMING on STREAM_OUTPUT 1: not admitted"),
    ("hz-talker-keyed-as-listener", "hz-talker-keyed-as-listener", "pp_top",
     "hazards", "HZ10c a DISCONNECT_TX of source 1 vs a held "
     "STOP_STREAMING on STREAM_OUTPUT 1: not admitted"),
    ("hz-reads-keyed-none-talker", "hz-reads-keyed-none", "pp_top", "hazards",
     "HZ10e a DISCONNECT_TX of source 1 vs a held GET_STREAM_INFO on "
     "STREAM_OUTPUT 1: not admitted"),
    ("hz-setcfg-not-barrier-talker", "hz-setcfg-not-barrier", "pp_top",
     "hazards", "HZ11a a GET_TX_STATE of source 1 vs a held "
     "SET_CONFIGURATION: not admitted"),
    ("hz-lock-not-lockop-talker", "hz-lock-not-lockop", "pp_top", "hazards",
     "HZ11b a DISCONNECT_TX of source 1 vs a held LOCK_ENTITY: not "
     "admitted"),
    ("hz-map-as-ro", "hz-map-as-ro", "pp_top", "hazards",
     "HZ7 ADD_AUDIO_MAPPINGS beside any stream step: not admitted"),
    ("hz-map-as-ro-talker", "hz-map-as-ro", "pp_top", "hazards",
     "HZ11d a DISCONNECT_TX of source 1 vs a held ADD_AUDIO_MAPPINGS: not "
     "admitted"),
    ("hz-clock-key-none", "hz-clock-key-none", "pp_top", "hazards",
     "HZ12a SET_SAMPLING_RATE (CLOCK_CFG) naming STREAM_INPUT 1 vs a held "
     "GET_RX_STATE of sink 1: not admitted"),
    ("hz-clock-key-none-talker", "hz-clock-key-none", "pp_top", "hazards",
     "HZ12a a GET_TX_STATE of source 1 vs a held SET_SAMPLING_RATE "
     "(CLOCK_CFG) naming STREAM_OUTPUT 1: not admitted"),
    ("hz-identify-key-none", "hz-identify-key-none", "pp_top", "hazards",
     "HZ12b SET_CONTROL (IDENTIFY) naming STREAM_INPUT 1 vs a held "
     "GET_RX_STATE of sink 1: not admitted"),
    ("hz-identify-key-none-talker", "hz-identify-key-none", "pp_top",
     "hazards", "HZ12b a GET_TX_STATE of source 1 vs a held SET_CONTROL "
     "(IDENTIFY) naming STREAM_OUTPUT 1: not admitted"),
    ("hz-map-key-none", "hz-map-key-none", "pp_top", "hazards",
     "HZ12c ADD_AUDIO_MAPPINGS (MAP_CFG) naming STREAM_INPUT 1 vs a held "
     "GET_RX_STATE of sink 1: not admitted"),
    ("hz-map-key-none-talker", "hz-map-key-none", "pp_top", "hazards",
     "HZ12c a GET_TX_STATE of source 1 vs a held ADD_AUDIO_MAPPINGS "
     "(MAP_CFG) naming STREAM_OUTPUT 1: not admitted"),
    # issue #84, R419-2 F5: a GET_DYNAMIC_INFO waits for and holds back a
    # listener step of a sink its records name, and still runs beside a read
    ("hz-gdi-key-none", "hz-gdi-key-none", "pp_top", "hazards",
     "HZ13a GET_DYNAMIC_INFO with a GET_STREAM_INFO record of STREAM_INPUT 1 "
     "vs a held UNBIND_RX of sink 1: not admitted"),
    ("hz-gdi-key-none-held", "hz-gdi-key-none", "pp_top", "hazards",
     "HZ13c an UNBIND_RX of sink 1 vs a held GET_DYNAMIC_INFO with a "
     "GET_STREAM_INFO record of STREAM_INPUT 1: not admitted"),
    ("hz-gdi-key-none-no-stream", "hz-gdi-key-none", "pp_top", "hazards",
     "HZ8 GET_DYNAMIC_INFO naming no stream (MAP_CFG's cross-lock, the "
     "accepted over-serialization): not admitted"),
    ("hz-gdi-as-barrier", "hz-gdi-as-barrier", "pp_top", "hazards",
     "HZ13b GET_DYNAMIC_INFO with a GET_STREAM_INFO record of STREAM_INPUT 1 "
     "vs a held GET_RX_STATE of sink 1 (two reads): admitted beside"),
]

SUITES = ("common", "ucpu", "pp_top")


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
    """Copy the tree to a scratch directory of its own, plant the patch if any, and run.

    The copy carries no generated ROM image (`*.hex` is not copied), so every
    build generates its ROMs from the generators as planted.
    """
    patch, suite, target, log = job
    with tempfile.TemporaryDirectory(prefix="aecp-mutants-") as tmp:
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


def completed(contents: str) -> bool:
    """A run that printed its tally: pp_top's per-section line or a suite tally."""
    return "checks" in contents


def campaign(output: Path, selected: list[tuple], jobs: int) -> tuple[int, int]:
    """Run the positive controls, then require each arm's own named check."""
    passed = 0
    total = 0
    pairs = sorted({(m[2], m[3]) for m in selected})
    units = [(None, suite, target, output / f"control-{suite}-{target}.log")
             for suite, target in pairs]
    with in_order(trial, units, jobs) as results:
        for (suite, target), (rc, contents) in zip(pairs, results):
            ok = rc == 0 and completed(contents) and not failures_of(contents)
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
            ok = rc != 0 and completed(contents) and bool(named)
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
