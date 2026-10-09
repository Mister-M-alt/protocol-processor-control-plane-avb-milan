#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Build the real processor with the shared wire harness, outside the tree."""

import argparse
import re
from pathlib import Path
import subprocess
import tempfile

from fixture import fixture, packer


def replace_once(text: str, old: str, new: str) -> str:
    """Refuse a stale harness adaptation instead of silently testing it."""
    if text.count(old) != 1:
        raise RuntimeError(f"expected one harness anchor: {old!r}")
    return text.replace(old, new)


def build(root: Path, work: Path, executable: str) -> Path:
    """Reuse the top bench's wiring; only the test name capacity is larger."""
    source = root / "tb/pp_top"
    wrapper = (source / "pp_top_wrap.sv").read_text()
    wrapper = replace_once(wrapper, "  protocol_processor_top #(\n",
                           "  protocol_processor_top #(\n"
                           "      .DESC_NAME_ENTRIES_P (128),\n")
    wrapper = replace_once(wrapper, "input  wire   [7:0] dbg_name_lane_i",
                           "input  wire   [9:0] dbg_name_lane_i")
    (work / "pp_top_wrap.sv").write_text(wrapper)
    command = ["make", "-j16", "-f", str(source / "Makefile"),
               f"HDL={root / 'hdl'}", f"VERILATOR={executable}",
               f"CPP={root / 'tb/name_state/sim_main.cpp'}", "gsi-build"]
    with (work / "build.log").open("w") as log:
        result = subprocess.run(command, cwd=work, stdout=log, stderr=log,
                                check=False)
    if result.returncode:
        raise RuntimeError(f"build failed ({result.returncode}): {work / 'build.log'}")
    return work / "obj_dir/Vpp_top_sim"


def run(root: Path, work: Path, executable: str, image: Path | None,
        aaf: int, measure: bool) -> int:
    """Run both synthetic populations, or one supplied generated image."""
    binary = build(root, work, executable)
    populations = (aaf,) if image else (1, 8)
    total = 0
    failed = 0
    for population in populations:
        model = image
        if model is None:
            generated = packer(root).build(fixture(population), lint=False)
            model = work / f"names-{population}.bin"
            model.write_bytes(generated[0])
        command = [str(binary), str(model), str(population)]
        if measure:
            command.append("--measure")
        result = subprocess.run(command,
                                cwd=work, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        (work / f"names-{population}.log").write_text(result.stdout)
        print(result.stdout, end="")
        tally = re.search(r"(\d+) checks: (\d+) PASS, (\d+) FAIL", result.stdout)
        if not tally:
            return result.returncode or 2
        total += int(tally[1])
        failed += int(tally[3])
        if result.returncode and not int(tally[3]):
            return result.returncode
    print(f"{total} checks: {total - failed} PASS, {failed} FAIL")
    return 1 if failed else 0


def main() -> int:
    """Select an external build directory and execute the requested images."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verilator", default="verilator")
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument("--work", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--aaf", type=int, choices=(1, 8), default=1)
    parser.add_argument("--measure", action="store_true")
    args = parser.parse_args()
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        return run(args.root.resolve(), args.work.resolve(), args.verilator,
                   args.image.resolve() if args.image else None, args.aaf, args.measure)
    with tempfile.TemporaryDirectory(prefix="name-state-") as directory:
        return run(args.root.resolve(), Path(directory), args.verilator,
                   args.image.resolve() if args.image else None, args.aaf, args.measure)


if __name__ == "__main__":
    raise SystemExit(main())
