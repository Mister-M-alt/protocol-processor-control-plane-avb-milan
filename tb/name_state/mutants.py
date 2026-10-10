#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Require completed runs and named name-value failures for planted defects."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from fixture import fixture, packer
from run import NAMES, build, capacity, replace_once


def controls(root: Path) -> list[tuple]:
    """Reuse existing defect definitions; grade the complete inventory checks.

    Each control names the populations it is graded in, each at its bound
    name capacity, and the name-value assertion that must fail there.
    """
    path = root / "tb/pp_top/d3_mutants.py"
    spec = importlib.util.spec_from_file_location("saved_state_controls", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    by_name = {control.name: control for control in module.MUTANTS}
    named = (
        ("TRG_name", "N3 saved ordinal", "inventory"),
        ("RPL_name", "N5 restored ordinal", "inventory"),
        ("name_record_id_shifted", "N3 saved ordinal", "inventory"),
        ("name_entry_shifted", "N5 restored ordinal", "inventory"),
        ("name_empty_refused", "N5 restored ordinal 2", "inventory"),
        ("name_lanes_partial", "N5 restored ordinal 1", "inventory"),
        ("latch_ignores_program", "N7 capture:", "capture"),
        ("name_taint_ignored", "D3N6 taint:", "taint"),
        ("names_before_the_image", "D3N7:", "healing"),
        ("store_not_rolled_back", "D3N5: the roll-back left", "rollback"),
        ("identify_survives_reset", "N8 CONTROL name", "inventory"),
        ("rollback_ignores_debt", "N9 debt names:", "debt"),
    )
    result = [(name, by_name[name].edits, ((1, check, case),)) for name, check, case in named]
    result.append(("pending_clears_other_name", ((
        "hdl/aecp/KL_aecp_nvm_writer.sv",
        "    if (done_ok_w || giveup_w) clr_w[hand_r] = 1'b1;",
        "    if (done_ok_w || giveup_w) clr_w = '1;"),), ((1, "N6 pending:", "pending"),)))
    result.append(("image_names_zeroed", ((
        "hdl/aecp/KL_aecp_desc_store.sv",
        "      name_wdata_w = mem_rsp_data_i;",
        "      name_wdata_w = 64'd0;"),), ((1, "N1 default ordinal", "inventory"),)))
    result.append(("live_name_lane_dropped", ((
        "hdl/aecp/KL_aecp_desc_store.sv",
        "      name_we_w    = name_wr_o;",
        "      name_we_w    = name_wr_o && (st_addr_i[5:3] != 3'd7);"),),
        ((1, "N2 SET and GET ordinal", "inventory"),)))
    # The trigger drops the table's last entry, which only a capacity equal
    # to the population (the bound geometry) puts at the last ordinal.
    result.append(("name_table_last_entry_dropped", ((
        "hdl/aecp/KL_aecp_nvm_writer.sv",
        "    if (nchg_i && (32'(nchg_ord_i) < N_NAME_P)) begin",
        "    if (nchg_i && (32'(nchg_ord_i) < N_NAME_P - 1)) begin"),),
        tuple((population, f"N3 saved ordinal {names - 1}", "inventory")
              for population, names in NAMES.items())))
    return result


def judge(control: tuple, root: Path, output: Path, executable: str) -> dict:
    """Build one isolated source copy; compiler failures cannot kill a defect."""
    name, edits, grades = control
    work = output / name
    work.mkdir()
    tree = work / "source"
    skip = shutil.ignore_patterns("obj*", "*.hex", "__pycache__")
    for directory in ("hdl", "tb/common", "tb/pp_top", "tb/name_state"):
        shutil.copytree(root / directory, tree / directory, ignore=skip)
    for filename, old, new in edits:
        path = tree / filename
        path.write_text(replace_once(path.read_text(), old, new))
    runs = []
    for population, assertion, case in grades:
        where = work / f"names-{population}"
        where.mkdir()
        try:
            binary = build(tree, where, executable, capacity(NAMES[population]))
        except RuntimeError as error:
            return {"name": name, "verdict": "BUILD_FAILED", "reason": str(error)}
        image = where / f"names-{population}.bin"
        image.write_bytes(packer(tree).build(fixture(population), lint=False)[0])
        result = subprocess.run([str(binary), str(image), str(population), case],
                                cwd=where, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        (where / "run.log").write_text(result.stdout)
        failures = [line.removeprefix("FAIL: ") for line in result.stdout.splitlines()
                    if line.startswith("FAIL: ")]
        completed = bool(re.search(r"\d+ checks: \d+ PASS, \d+ FAIL", result.stdout))
        # An assertion ending in an ordinal names that ordinal alone.
        named = re.compile(re.escape(assertion) + r"(?!\d)")
        killed = result.returncode == 1 and completed and any(
            named.match(line) for line in failures)
        runs.append({"population": population, "entries": NAMES[population],
                     "case": case, "assertion": assertion, "run_rc": result.returncode,
                     "completed": completed, "failures": failures, "killed": killed})
    if not edits:
        verdict = "PASS" if all(run["run_rc"] == 0 and run["completed"]
                                for run in runs) else "BROKEN"
    else:
        verdict = "KILLED" if all(run["killed"] for run in runs) else "SURVIVED"
    print(f"{name}: {verdict}", flush=True)
    return {"name": name, "build_rc": 0, "runs": runs, "verdict": verdict}


def main() -> int:
    """Run the golden first, then independent controls with bounded concurrency."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--verilator", default="verilator")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--only", nargs="*")
    args = parser.parse_args()
    root = args.root.resolve()
    output = args.output.resolve()
    if output.exists():
        parser.error(f"output directory already exists: {output}")
    output.mkdir(parents=True)
    selected = controls(root)
    if args.only:
        known = {control[0] for control in selected}
        if set(args.only) - known:
            parser.error("unknown control")
        selected = [control for control in selected if control[0] in args.only]
    golden = judge(("golden", (), tuple((population, "", "all") for population in NAMES)),
                   root, output, args.verilator)
    records = [golden]
    if golden["verdict"] == "PASS":
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            records += list(pool.map(
                lambda control: judge(control, root, output, args.verilator), selected))
    (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    return 0 if len(records) == len(selected) + 1 and all(
        row["verdict"] in ("PASS", "KILLED") for row in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
