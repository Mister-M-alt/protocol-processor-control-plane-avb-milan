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


def capacity(names: int) -> str:
    """The wrapper line that binds one build's name-table capacity."""
    return f"      .DESC_NAME_ENTRIES_P ({names}),\n"


def entries(line: str) -> int:
    """The name-table capacity a wrapper line binds."""
    return int(re.search(r"\((\d+)\)", line)[1])


# The parent binds DESC_NAME_ENTRIES_P to each population's exact generated
# name count (AEM_NAME_ENTRIES_C), so the last ordinal is the table's last
# entry. Every population runs at that capacity first.
NAMES = {1: 39, 8: 107}
# The first round's larger geometry follows as an extra run. Its line stays
# spelled out because external capacity probes rebind exactly this text.
EXTRA = "      .DESC_NAME_ENTRIES_P (128),\n"


def build(root: Path, work: Path, executable: str, line: str) -> Path:
    """Reuse the top bench's wiring at one test name capacity."""
    source = root / "tb/pp_top"
    wrapper = (source / "pp_top_wrap.sv").read_text()
    wrapper = replace_once(wrapper, "  protocol_processor_top #(\n",
                           "  protocol_processor_top #(\n" + line)
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
    """Run each population at its bound capacity, then at the extra geometry."""
    populations = (aaf,) if image else (1, 8)
    geometries = ([(population, capacity(NAMES[population])) for population in populations]
                  + [(population, EXTRA) for population in populations])
    binaries = {}
    total = 0
    failed = 0
    for population, line in geometries:
        size = entries(line)
        where = work / f"entries-{size}"
        if line not in binaries:
            where.mkdir(exist_ok=True)
            binaries[line] = build(root, where, executable, line)
        model = image
        if model is None:
            model = work / f"names-{population}.bin"
            if not model.exists():
                model.write_bytes(packer(root).build(fixture(population), lint=False)[0])
        command = [str(binaries[line]), str(model), str(population)]
        if measure:
            command.append("--measure")
        result = subprocess.run(command,
                                cwd=where, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        (work / f"names-{population}-{size}.log").write_text(result.stdout)
        print(f"population {population}, {size} name entries")
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
