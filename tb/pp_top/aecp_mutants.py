#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant reviewed AECP deadline and hazard-class arms in scratch and require named check failures.

Lane C5a's campaign (issues #81, #57 and #84): each arm is an explicit patch in
mutations/, applied with git apply to a scratch copy of the tree; this driver
reads only simulation logs, never production source. Every suite it runs is
cycle-bounded. A positive control of each (suite, target) pair runs first and
must pass; an arm is KILLED only when its simulation completed (a tally was
printed), failed, and printed the required check. A build failure or a missing
tally never counts as a kill.
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
    ("dl-preempt-after-effect", "dl-preempt-after-effect", "ucpu", "run",
     "P19c the program's own SUCCESS"),
    ("dl-preempt-after-effect-top", "dl-preempt-after-effect", "pp_top",
     "deadline", "DL2: past its NAME_WR the SET_NAME answers its own SUCCESS"),
    ("ucpu-preempt-cuts-a-wait", "ucpu-preempt-cuts-a-wait", "ucpu", "run",
     "P19e the waiting locate was answered"),
    ("ucpu-preempt-keeps-the-body", "ucpu-preempt-keeps-the-body", "ucpu",
     "run", "P19f header only"),
    ("ucpu-preempt-repeats", "ucpu-preempt-repeats", "ucpu", "run",
     "P19a completes"),
    ("dlkill-always-misbehaving", "dlkill-always-misbehaving", "ucpu", "run",
     "P19d ENTITY_LOCKED kept"),
    # issue #57 (REQ-MVU-005): the MVU answer under the deadline, and its
    # latency against T-AECP-RESP (section TB, the third build)
    ("dl-mvu-forced-status-10", "dl-mvu-forced-status-10", "pp_top",
     "deadline", "DL3: the queued GET_MILAN_INFO"),
    # ... and when its response memory fails (REQ-MVU-005, Table 5.19)
    ("mvu-fault-status-10", "mvu-fault-status-10", "pp_top", "deadline",
     "DL8 a response-memory read error: GET_MILAN_INFO answers MVU "
     "NOT_IMPLEMENTED"),
    ("mvu-echo-slot-std", "mvu-echo-slot-std", "pp_top", "deadline",
     "DL8 a read error under a 540-byte command: GET_MILAN_INFO answers MVU "
     "NOT_IMPLEMENTED"),
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
     "HZ HZ5 START_STREAMING on the held sink's key: not admitted"),
    ("hz-reads-keyed-none", "hz-reads-keyed-none", "pp_top", "hazards",
     "HZ HZ6 GET_STREAM_INFO on the held sink's key: not admitted"),
    ("hz-clock-as-ro", "hz-clock-as-ro", "pp_top", "hazards",
     "HZ1 SET_SAMPLING_RATE AUDIO_UNIT 0: admitted"),
    ("hz-clock-as-lock", "hz-clock-as-lock", "pp_top", "hazards",
     "HZ HZ8 SET_SAMPLING_RATE (CLOCK_CFG): admitted beside"),
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
     "HZ HZ9a SET_NAME on STREAM_INPUT 1 vs a held GET_RX_STATE of sink 1: "
     "not admitted"),
    ("hz-name-key-none-talker", "hz-name-key-none", "pp_top", "hazards",
     "HZ HZ9d a GET_TX_STATE of source 1 vs a held SET_NAME on STREAM_OUTPUT "
     "1: not admitted"),
    ("hz-name-key-none-held", "hz-name-key-none", "pp_top", "hazards",
     "HZ HZ9f a GET_RX_STATE of sink 1 vs a held SET_NAME on STREAM_INPUT 1: "
     "not admitted"),
    ("hz-name-as-stream", "hz-name-as-stream", "pp_top", "hazards",
     "HZ HZ9c SET_NAME on STREAM_INPUT 1 vs a held UNBIND_RX of sink 1: "
     "admitted beside"),
    ("hz-stream-key-none-vs-read", "hz-stream-key-none", "pp_top", "hazards",
     "HZ HZ10a STOP_STREAMING on STREAM_INPUT 1 vs a held GET_RX_STATE of "
     "sink 1: not admitted"),
    ("hz-stream-key-none-talker-read", "hz-stream-key-none", "pp_top",
     "hazards", "HZ HZ10b a GET_TX_STATE of source 1 vs a held STOP_STREAMING "
     "on STREAM_OUTPUT 1: not admitted"),
    ("hz-stream-key-none-talker-step", "hz-stream-key-none", "pp_top",
     "hazards", "HZ HZ10c a DISCONNECT_TX of source 1 vs a held "
     "STOP_STREAMING on STREAM_OUTPUT 1: not admitted"),
    ("hz-talker-keyed-as-listener", "hz-talker-keyed-as-listener", "pp_top",
     "hazards", "HZ HZ10c a DISCONNECT_TX of source 1 vs a held "
     "STOP_STREAMING on STREAM_OUTPUT 1: not admitted"),
    ("hz-reads-keyed-none-talker", "hz-reads-keyed-none", "pp_top", "hazards",
     "HZ HZ10e a DISCONNECT_TX of source 1 vs a held GET_STREAM_INFO on "
     "STREAM_OUTPUT 1: not admitted"),
    ("hz-setcfg-not-barrier-talker", "hz-setcfg-not-barrier", "pp_top",
     "hazards", "HZ HZ11a a GET_TX_STATE of source 1 vs a held "
     "SET_CONFIGURATION: not admitted"),
    ("hz-lock-not-lockop-talker", "hz-lock-not-lockop", "pp_top", "hazards",
     "HZ HZ11b a DISCONNECT_TX of source 1 vs a held LOCK_ENTITY: not "
     "admitted"),
    ("hz-map-as-ro", "hz-map-as-ro", "pp_top", "hazards",
     "HZ HZ7 ADD_AUDIO_MAPPINGS beside any stream step: not admitted"),
    ("hz-map-as-ro-talker", "hz-map-as-ro", "pp_top", "hazards",
     "HZ HZ11d a DISCONNECT_TX of source 1 vs a held ADD_AUDIO_MAPPINGS: not "
     "admitted"),
    ("hz-clock-key-none", "hz-clock-key-none", "pp_top", "hazards",
     "HZ HZ12a SET_SAMPLING_RATE (CLOCK_CFG) naming STREAM_INPUT 1 vs a held "
     "GET_RX_STATE of sink 1: not admitted"),
    ("hz-clock-key-none-talker", "hz-clock-key-none", "pp_top", "hazards",
     "HZ HZ12a a GET_TX_STATE of source 1 vs a held SET_SAMPLING_RATE "
     "(CLOCK_CFG) naming STREAM_OUTPUT 1: not admitted"),
    ("hz-identify-key-none", "hz-identify-key-none", "pp_top", "hazards",
     "HZ HZ12b SET_CONTROL (IDENTIFY) naming STREAM_INPUT 1 vs a held "
     "GET_RX_STATE of sink 1: not admitted"),
    ("hz-identify-key-none-talker", "hz-identify-key-none", "pp_top",
     "hazards", "HZ HZ12b a GET_TX_STATE of source 1 vs a held SET_CONTROL "
     "(IDENTIFY) naming STREAM_OUTPUT 1: not admitted"),
    ("hz-map-key-none", "hz-map-key-none", "pp_top", "hazards",
     "HZ HZ12c ADD_AUDIO_MAPPINGS (MAP_CFG) naming STREAM_INPUT 1 vs a held "
     "GET_RX_STATE of sink 1: not admitted"),
    ("hz-map-key-none-talker", "hz-map-key-none", "pp_top", "hazards",
     "HZ HZ12c a GET_TX_STATE of source 1 vs a held ADD_AUDIO_MAPPINGS "
     "(MAP_CFG) naming STREAM_OUTPUT 1: not admitted"),
]

SUITES = ("common", "ucpu", "pp_top")


def run(tree: Path, suite: str, target: str, log: Path) -> tuple[int, str]:
    """Build and run one cycle-bounded suite target, keeping its whole log."""
    with log.open("w") as stream:
        result = subprocess.run(["make", "-C", str(tree / "tb" / suite), target],
                                stdout=stream, stderr=subprocess.STDOUT, check=False)
    return result.returncode, log.read_text()


def forget_generated_roms(tree: Path) -> None:
    """Drop every ROM image the suites generate, so the next build regenerates it.

    The restored generators keep their original timestamps, which are older
    than an image an earlier arm generated from a patched generator: without
    this, make would keep that stale image for every later arm.
    """
    for suite in SUITES:
        for image in ("ucode.hex", "ltn_rom.hex"):
            (tree / "tb" / suite / image).unlink(missing_ok=True)


def plant(tree: Path, patch: str) -> None:
    """Restore the scratch RTL and apply one explicit patch, refusing drift."""
    shutil.copytree(ROOT / "hdl", tree / "hdl", dirs_exist_ok=True)
    forget_generated_roms(tree)
    path = str(PATCHES / (patch + ".patch"))
    subprocess.run(["git", "apply", "--check", path], cwd=tree, check=True)
    subprocess.run(["git", "apply", path], cwd=tree, check=True)


def failures_of(contents: str) -> list[str]:
    """The failing-check lines of one simulation log, in the order printed."""
    return [line for line in contents.splitlines() if line.startswith("FAIL:")]


def completed(contents: str) -> bool:
    """A run that printed its tally: pp_top's per-section line or a suite tally."""
    return "checks" in contents


def campaign(tree: Path, output: Path, selected: list[tuple]) -> tuple[int, int]:
    """Run the positive controls, then require each arm's own named check."""
    passed = 0
    total = 0
    for suite, target in sorted({(m[2], m[3]) for m in selected}):
        rc, contents = run(tree, suite, target, output / f"control-{suite}-{target}.log")
        ok = rc == 0 and completed(contents) and not failures_of(contents)
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
    with tempfile.TemporaryDirectory(prefix="aecp-mutants-") as tmp:
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
