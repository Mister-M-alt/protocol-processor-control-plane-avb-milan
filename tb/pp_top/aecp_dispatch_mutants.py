#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant the AECP dispatch-and-response arms in scratch; require named failures.

Each arm is an explicit patch in aecp_dispatch_mutations/, applied with git apply to a
scratch copy of the tree; this driver reads only simulation and lint logs,
never production source. Every simulation it runs is cycle-bounded. A
positive control of each make target runs first and must pass; an arm is
KILLED only when its target completed (a build's tally line, or the line
guards' summary, was printed), failed, and printed the required check. A
build failure or a missing tally never counts as a kill. Every control and
every arm builds in a scratch copy of its own, and the generated ROMs and
every model directory are deleted before each build, so a microcode arm can
never leave its ROM behind for another. `--jobs N` runs up to N copies at
once; the results are read in the declared order.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATCHES = Path(__file__).resolve().parent / "aecp_dispatch_mutations"

sys.path.insert(0, str(ROOT / "tb" / "common"))
from mutant_pool import add_jobs_argument, in_order  # noqa: E402

#: arm, patch, make target, the check that must fail (its label prefix)
MUTANTS = [
    # issue #76: the aem_w message-type guard of each arm M9 did not sweep
    ("m9-guard-set-stream-format", "m9-guard-set-stream-format", "aecp-dispatch",
     "M9: mt=4 word 0008"),
    ("m9-guard-set-stream-info", "m9-guard-set-stream-info", "aecp-dispatch",
     "M9: mt=4 word 000E"),
    ("m9-guard-set-name", "m9-guard-set-name", "aecp-dispatch", "M9: mt=4 word 0010"),
    ("m9-guard-get-name", "m9-guard-get-name", "aecp-dispatch", "M9: mt=4 word 0011"),
    ("m9-guard-add-mappings", "m9-guard-add-mappings", "aecp-dispatch", "M9: mt=4 word 002C"),
    ("m9-guard-remove-mappings", "m9-guard-remove-mappings", "aecp-dispatch",
     "M9: mt=4 word 002D"),
    ("m9-guard-dynamic-info", "m9-guard-dynamic-info", "aecp-dispatch", "M9: mt=4 word 004B"),
    # issue #74: a SUCCESS arm for REBOOT, the first opcode REQ-FWX-001 names
    ("a5b-reboot-success-arm", "a5b-reboot-success-arm", "aecp-dispatch",
     "A5b: REBOOT (7.4.43, Figure 7-68): the response is not the echoed command"),
    # issue #53: every CHECK_LOCK of the three SETs replaced with NOP, on the
    # main path and on the locate-miss path, and the lane base's microcode
    # (the zero-bodied ENTITY_LOCKED the issue records)
    ("lk-ssrate-lock-nop", "lk-ssrate-lock-nop", "aecp-dispatch",
     "LK1 unset rate row, foreign SET_SAMPLING_RATE(48000) carries the image's 96000: "
     "ENTITY_LOCKED byte-exact"),
    ("lk-ssrate-miss-lock-nop", "lk-ssrate-miss-lock-nop", "aecp-dispatch",
     "LK4 foreign SET_SAMPLING_RATE on AUDIO_UNIT 3 (absent): ENTITY_LOCKED byte-exact"),
    ("lk-sclks-lock-nop", "lk-sclks-lock-nop", "aecp-dispatch",
     "LK1 unset clock-source row, foreign SET_CLOCK_SOURCE(1) carries the image's 2: "
     "ENTITY_LOCKED byte-exact"),
    ("lk-sclks-miss-lock-nop", "lk-sclks-miss-lock-nop", "aecp-dispatch",
     "LK4 foreign SET_CLOCK_SOURCE on CLOCK_DOMAIN 3 (absent): ENTITY_LOCKED byte-exact"),
    ("lk-sctrl-lock-nop", "lk-sctrl-lock-nop", "aecp-dispatch",
     "LK1 IDENTIFY at its reset 0, foreign SET_CONTROL(255) carries 0: "
     "ENTITY_LOCKED byte-exact"),
    ("lk-sctrl-miss-lock-nop", "lk-sctrl-miss-lock-nop", "aecp-dispatch",
     "LK4 foreign SET_CONTROL on CONTROL 3 (absent): ENTITY_LOCKED byte-exact"),
    ("lk-prefix-zero-body", "lk-prefix-zero-body", "aecp-dispatch",
     "LK3 foreign SET_SAMPLING_RATE(96000) carries the stored 48000: "
     "ENTITY_LOCKED byte-exact"),
    # R416-1 F1: SET_CONTROL's out-of-range BAD_ARGUMENTS back on the lane
    # base's zero-bodied stub, while IDENTIFY holds 255
    ("sctrl-badarg-zero-body", "sctrl-badarg-zero-body", "aecp-dispatch",
     "LK3b IDENTIFY at 255, the holder's SET_CONTROL(128) carries 255: "
     "BAD_ARGUMENTS byte-exact"),
    # issue #50 (and #82's oversize path): the engine's Delta-8 request, its
    # comparator and the top's routing of it; the GET_AUDIO_MAP page, its
    # buffer-wide APPEND and its cap
    ("ov-oversize-never", "ov-oversize-never", "aecp-dispatch",
     "OV1 AUDIO_MAP 0 (576 B, the whole line: cdl 592, frame 618): "
     "the 576-byte descriptor, byte-exact"),
    ("ov-oversize-at-576", "ov-oversize-at-576", "aecp-dispatch",
     "OV4 CLOCK_DOMAIN 0 (534 B: frame 576, the standard slot's own size): "
     "one TX-slot grant"),
    ("ov-top-oversize-dropped", "ov-top-oversize-dropped", "aecp-dispatch",
     "OV1 AUDIO_MAP 0 (576 B, the whole line: cdl 592, frame 618): "
     "the 576-byte descriptor, byte-exact"),
    ("pg-append-524", "pg-append-524", "aecp-dispatch",
     "PG2 a 63-mapping page: SUCCESS above cdl 524 (528), a standard slot: "
     "number_of_mappings"),
    ("pg-cap-dropped", "pg-cap-dropped", "aecp-dispatch",
     "PG7 a 72-mapping page: NO_RESOURCES, no record claimed: number_of_mappings"),
    ("pg-cap-off-by-one", "pg-cap-off-by-one", "aecp-dispatch",
     "PG7 a 72-mapping page: NO_RESOURCES, no record claimed: byte-exact"),
    # issue #82: READ_DESCRIPTOR's current-value overlays, the seam that
    # dispatches them, the configuration guard, the rows they read and the
    # TAIL copy they end with
    ("rd-base-no-overlay", "rd-base-no-overlay", "aecp-dispatch",
     "RD1 AUDIO_UNIT 0 after SET_SAMPLING_RATE(48000): READ_DESCRIPTOR byte-exact"),
    ("rd-au-image-only", "rd-au-image-only", "aecp-dispatch",
     "RD1 AUDIO_UNIT 0 after SET_SAMPLING_RATE(48000): READ_DESCRIPTOR byte-exact"),
    ("rd-au-unset-overlays", "rd-au-unset-overlays", "aecp-dispatch",
     "RD0 AUDIO_UNIT 0, rate unset: READ_DESCRIPTOR byte-exact"),
    ("rd-cd-image-only", "rd-cd-image-only", "aecp-dispatch",
     "RD1 CLOCK_DOMAIN 0 after SET_CLOCK_SOURCE(1): READ_DESCRIPTOR byte-exact"),
    ("rd-str-unset-overlays", "rd-str-unset-overlays", "aecp-dispatch",
     "RD0 STREAM_INPUT 0, format unset: READ_DESCRIPTOR byte-exact"),
    ("rd-so-reads-input-row", "rd-so-reads-input-row", "aecp-dispatch",
     "RD1 STREAM_OUTPUT 1 after SET_STREAM_FORMAT: READ_DESCRIPTOR byte-exact"),
    ("rd-cfg-any", "rd-cfg-any", "aecp-dispatch",
     "RD2 configuration 1's CLOCK_DOMAIN 0 keeps its image bytes: "
     "the 534-byte descriptor, byte-exact"),
    ("rd-tail-uncut", "rd-tail-uncut", "aecp-dispatch",
     "RD1 AUDIO_UNIT 0 after SET_SAMPLING_RATE(48000): READ_DESCRIPTOR byte-exact"),
    # R416-1 S1: the STREAM programs' too-short guard, the one a SET can reach
    ("rd-str-short-guard-nop", "rd-str-short-guard-nop", "aecp-dispatch",
     "RD4 STREAM_OUTPUT 1 of 80 bytes with a set row: READ_DESCRIPTOR byte-exact"),
    # R416-1 F2, R417-1 F1: the DESC_LINE_BYTES_P range and the response
    # buffer as the 16 + line reservation, at the range's edges (line-guards)
    # and at the line build's non-default 584-byte line (aecp-line)
    ("line-floor-rounded", "line-floor-rounded", "line-guards", "line guard 568"),
    ("line-ceiling-dropped", "line-ceiling-dropped", "line-guards", "line guard 1016"),
    ("line-buffer-fixed-592", "line-buffer-fixed-592", "aecp-line",
     "OV1 AUDIO_MAP 0 (584 B, the whole line: cdl 600, frame 626): "
     "the 584-byte descriptor, byte-exact"),
    ("rb-rounded-buffer-no-page-cap", "rb-rounded-buffer-no-page-cap", "aecp-line",
     "RB no response byte written at or past RESP_BASE_P + 16 + DESC_LINE_BYTES_P"),
    # issue #141: SET_CLOCK_SOURCE's range check over a ten-source domain,
    # graded on a fresh model in section D3C (the d3 target): the bound fixed
    # at the suite list's three, and the bound made inclusive
    ("sclks-bound-three", "sclks-bound-three", "d3",
     "D3C1: SET_CLOCK_SOURCE(9) over the ten-source domain answers SUCCESS"),
    ("sclks-bound-inclusive", "sclks-bound-inclusive", "d3",
     "D3C2: SET_CLOCK_SOURCE(10), the count, answers BAD_ARGUMENTS"),
]

#: the scratch tree: the RTL and the two bench directories the targets build
TREES = (("hdl",), ("tb", "common"), ("tb", "pp_top"))
#: generated in the bench directory by the targets; never carried between arms
GENERATED = ("obj_dir", "obj_vid", "obj_line", "ucode.hex", "ltn_rom.hex")
#: what each target prints once it has run to its end
COMPLETE = {"aecp-dispatch": "[build default,", "aecp-line": "[build line,",
            "line-guards": "line guards: ", "d3": "[build default,"}


def run(tree: Path, target: str, verilator: str, log: Path) -> tuple[int, str]:
    """Build and run one cycle-bounded bench target, keeping its whole log."""
    bench = tree / "tb" / "pp_top"
    for name in GENERATED:
        path = bench / name
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            path.unlink()
    with log.open("w") as stream:
        result = subprocess.run(["make", "-C", str(bench), target, "VERILATOR=" + verilator],
                                stdout=stream, stderr=subprocess.STDOUT, check=False)
    return result.returncode, log.read_text()


def plant(tree: Path, patch: str) -> None:
    """Apply one explicit patch to the scratch RTL, refusing drift."""
    path = str(PATCHES / (patch + ".patch"))
    subprocess.run(["git", "apply", "--check", path], cwd=tree, check=True)
    subprocess.run(["git", "apply", path], cwd=tree, check=True)


def trial(job: tuple[str | None, str, str, Path]) -> tuple[int, str]:
    """Copy the trees to a scratch directory of its own, plant the patch if any, and run."""
    patch, target, verilator, log = job
    with tempfile.TemporaryDirectory(prefix="aecp-dispatch-mutants-") as tmp:
        tree = Path(tmp)
        for parts in TREES:
            shutil.copytree(ROOT.joinpath(*parts), tree.joinpath(*parts),
                            ignore=shutil.ignore_patterns("obj_*", "*.hex", "__pycache__"))
        if patch is not None:
            plant(tree, patch)
        return run(tree, target, verilator, log)


def failures_of(contents: str) -> list[str]:
    """The failing-check lines of one simulation log, in the order printed."""
    return [line for line in contents.splitlines() if line.startswith("FAIL:")]


def completed(contents: str, target: str) -> bool:
    """The target printed its tally or summary: it ran to its end."""
    return COMPLETE[target] in contents


def campaign(output: Path, selected: list[tuple], verilator: str, jobs: int) -> list[dict]:
    """Run the positive controls, then require each arm's own named check."""
    records: list[dict] = []
    targets = sorted({m[2] for m in selected})
    units = [(None, target, verilator, output / f"control-{target}.log") for target in targets]
    with in_order(trial, units, jobs) as results:
        for target, (rc, contents) in zip(targets, results):
            ok = rc == 0 and completed(contents, target) and not failures_of(contents)
            records.append({"arm": f"control {target}", "rc": rc,
                            "verdict": "PASS" if ok else "FAIL"})
            print(f"control {target}: rc={rc} {'PASS' if ok else 'FAIL'}", flush=True)
            if not ok:
                print(contents[-4000:], flush=True)
                return records
    units = [(patch, target, verilator, output / f"{arm}.log")
             for arm, patch, target, _ in selected]
    with in_order(trial, units, jobs) as results:
        for (arm, patch, target, expected), (rc, contents) in zip(selected, results):
            failures = failures_of(contents)
            named = [line for line in failures
                     if line[len("FAIL:"):].strip().startswith(expected)]
            ok = rc != 0 and completed(contents, target) and bool(named)
            verdict = "KILLED" if ok else "UNPROVEN"
            records.append({"arm": arm, "patch": patch, "target": target, "rc": rc,
                            "failures": len(failures), "named": len(named), "verdict": verdict})
            print(f"{arm}: rc={rc} failures={len(failures)} named={len(named)} {verdict}",
                  flush=True)
            for line in failures[:12]:
                print(f"    {line}", flush=True)
            if not ok:
                print(contents[-2500:], flush=True)
    return records


def main() -> int:
    """Select arms, isolate every write in scratch trees, fail on any unproven arm."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--only", default="")
    parser.add_argument("--verilator", default="verilator")
    add_jobs_argument(parser)
    args = parser.parse_args()
    requested = set(args.only.split(",")) if args.only else {m[0] for m in MUTANTS}
    unknown = requested - {m[0] for m in MUTANTS}
    if unknown:
        parser.error(f"unknown mutation arms: {sorted(unknown)}")
    selected = [m for m in MUTANTS if m[0] in requested]
    args.output.mkdir(parents=True, exist_ok=True)
    records = campaign(args.output, selected, args.verilator, args.jobs)
    (args.output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    total = len(records)
    passed = sum(1 for r in records if r["verdict"] in ("PASS", "KILLED"))
    print(f"{total} checks: {passed} PASS, {total - passed} FAIL")
    return int(passed != total or total != len(selected) + len({m[2] for m in selected}))


if __name__ == "__main__":
    raise SystemExit(main())
