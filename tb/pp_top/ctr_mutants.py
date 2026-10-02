#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant reviewed GET_COUNTERS face arms in scratch and require named check failures.

Lane C7's campaign (issues #44 and #79): each arm is an explicit patch in
ctr_mutations/, applied with git apply to a scratch copy of the tree; this
driver reads only simulation logs, never production source. Eleven arms break
the processor's half of the counters face (the type gate, the face's index,
the block's beat order, the locate, the notification decode, its windows and
the strobe's wiring). Two break the harness's integrator store instead, the
half the processor cannot hold: they show section K's checks refuse a store
that counts a domain-only strobe as a grandmaster change, or whose link edge
detector resets up. Every arm runs the cycle-bounded `counters` target (K9 to
K16). Its positive control runs first and must pass; an arm is KILLED only when
its simulation completed (a tally was printed), failed, and printed the
required check. A build failure or a missing tally never counts as a kill.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATCHES = Path(__file__).resolve().parent / "ctr_mutations"
TARGET = "counters"

# arm (= its patch), the check that must fail (its label prefix)
MUTANTS = [
    # the processor: GET_COUNTERS on the face
    ("ctr-avb-not-supported",
     "K9: AVB_INTERFACE 0 after boot is byte-exact"),
    ("ctr-ckd-not-supported", "K16: media clock locked"),
    ("ctr-index-from-type", "K9: AVB_INTERFACE 0 after boot is byte-exact"),
    ("ctr-block-beats-swapped",
     "K11: AVB_INTERFACE 0 byte-exact at block offsets 0, 4, 20"),
    ("ctr-locate-ignored", "K12: AVB_INTERFACE 1 answers NO_SUCH_DESCRIPTOR"),
    # the processor: the Table 5.22 push from the change strobe
    ("ctr-notify-avb-dropped",
     "K13: one unsolicited GET_COUNTERS of AVB_INTERFACE 0"),
    ("ctr-notify-avb-as-clock",
     "K13: one unsolicited GET_COUNTERS of AVB_INTERFACE 0"),
    ("ctr-notify-ckd-dropped", "K16: CLOCK_DOMAIN 0 is pushed"),
    ("ctr-notify-one-window", "K15: STREAM_INPUT 0 is pushed at once"),
    ("ctr-notify-no-window", "K14: nothing more for AVB_INTERFACE 0"),
    ("ctr-change-type-from-index",
     "K13: one unsolicited GET_COUNTERS of AVB_INTERFACE 0"),
    # the integrator's store the harness plays
    ("store-counts-domain-strobes",
     "K11: a domain-only gm_change_i is not a grandmaster change"),
    ("store-link-detector-resets-up",
     "K9: AVB_INTERFACE 0 after boot is byte-exact"),
]

SUITES = ("common", "pp_top")
IGNORE = shutil.ignore_patterns("obj_*", "*.hex", "__pycache__")


def run(tree: Path, log: Path) -> tuple[int, str]:
    """Build and run the cycle-bounded counters target, keeping its whole log."""
    with log.open("w") as stream:
        result = subprocess.run(["make", "-C", str(tree / "tb" / "pp_top"), TARGET],
                                stdout=stream, stderr=subprocess.STDOUT, check=False)
    return result.returncode, log.read_text()


def restore(tree: Path) -> None:
    """Put the scratch RTL and bench back to the tree's, and drop the ROM images.

    Two arms plant into the bench rather than the RTL, so both are restored,
    and with fresh timestamps: a restored file keeping its original one would
    be older than the object an earlier arm built from its patched copy, and
    make would keep that object, so a bench arm would outlive its own run. The
    ROM images are dropped for the same reason: make would keep an image an
    earlier arm generated from a patched generator.
    """
    shutil.copytree(ROOT / "hdl", tree / "hdl", dirs_exist_ok=True,
                    copy_function=shutil.copy)
    shutil.copytree(ROOT / "tb" / "pp_top", tree / "tb" / "pp_top",
                    dirs_exist_ok=True, ignore=IGNORE, copy_function=shutil.copy)
    for image in ("ucode.hex", "ltn_rom.hex"):
        (tree / "tb" / "pp_top" / image).unlink(missing_ok=True)


def plant(tree: Path, patch: str) -> None:
    """Restore the scratch copy and apply one explicit patch, refusing drift."""
    restore(tree)
    path = str(PATCHES / (patch + ".patch"))
    subprocess.run(["git", "apply", "--check", path], cwd=tree, check=True)
    subprocess.run(["git", "apply", path], cwd=tree, check=True)


def failures_of(contents: str) -> list[str]:
    """The failing-check lines of one simulation log, in the order printed."""
    return [line for line in contents.splitlines() if line.startswith("FAIL:")]


def completed(contents: str) -> bool:
    """A run that printed its tally: the build's own checks line."""
    return "checks" in contents


def campaign(tree: Path, output: Path, selected: list[tuple[str, str]]) -> tuple[int, int]:
    """Run the positive control, then require each arm's own named check."""
    rc, contents = run(tree, output / f"control-{TARGET}.log")
    ok = rc == 0 and completed(contents) and not failures_of(contents)
    print(f"control pp_top {TARGET}: rc={rc} {'PASS' if ok else 'FAIL'}", flush=True)
    if not ok:
        print(contents[-4000:])
        return 0, 1
    passed = 1
    total = 1
    for arm, expected in selected:
        plant(tree, arm)
        rc, contents = run(tree, output / f"{arm}.log")
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
    restore(tree)
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
    with tempfile.TemporaryDirectory(prefix="ctr-mutants-") as tmp:
        tree = Path(tmp)
        shutil.copytree(ROOT / "hdl", tree / "hdl")
        for suite in SUITES:
            shutil.copytree(ROOT / "tb" / suite, tree / "tb" / suite, ignore=IGNORE)
        passed, total = campaign(tree, args.output, selected)
    print(f"{total} checks: {passed} PASS, {total - passed} FAIL")
    return int(passed != total)


if __name__ == "__main__":
    raise SystemExit(main())
