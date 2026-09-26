#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Require a command-decode name-write export to fail the acceptance tests."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(command: list[str], cwd: Path, log: Path) -> int:
    """Run in the foreground and return the process status, saving its output."""
    with log.open("w") as stream:
        # The name-write harness bounds its waits in DUT cycles; only its
        # completed simulation verdict can kill the command-decode mutant.
        return subprocess.run(command, cwd=cwd, stdout=stream,
                              stderr=subprocess.STDOUT, check=False).returncode


def main() -> int:
    """Require golden/restored passes and named mutant failures in a temporary tree."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    if hasattr(os, "sched_getaffinity"):
        os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:8])
    results = []
    with tempfile.TemporaryDirectory(prefix="pp-name-mutant-") as temp:
        tree = Path(temp)
        for directory in ("hdl", "tb/common", "tb/pp_top"):
            shutil.copytree(root / directory, tree / directory,
                            ignore=shutil.ignore_patterns("obj*", "*.hex", "__pycache__"))
        engine = tree / "hdl/aecp/KL_aecp_engine.sv"
        original = engine.read_text()
        connection = "      .name_wr_o         (name_wr_o),"
        anchor = "  logic [15:0] store_fetch_nc_w, store_rowr_nc_w, store_dlen_nc_w;"
        assert original.count(connection) == original.count(anchor) == 1
        mutant = original.replace(connection, "      .name_wr_o         (),")
        mutant = mutant.replace(anchor, anchor +
            "\n  assign name_wr_o = txn_valid_i && txn_ready_o && sname_w;")
        for name, source in (("golden", original), ("decode", mutant),
                             ("restored", original)):
            engine.write_text(source)
            bench = tree / "tb/pp_top"
            build_rc = run(["make", "gsi-build"], bench, output / (name + "-build.log"))
            if build_rc:
                raise RuntimeError(f"{name}: build failed; this cannot kill a mutant")
            log = output / (name + "-run.log")
            run_rc = run(["./obj_dir/Vpp_top_sim", "--name-writes-only"], bench, log)
            transcript = log.read_text()
            failures = [line for line in transcript.splitlines() if line.startswith("FAIL:")]
            complete = "[build default," in transcript and "NW:" in transcript
            if name == "decode":
                required = ("NW EIGHT: accepted lane pulse count",
                            "NW LOCKED: accepted lane pulse count",
                            "NW ABORT: accepted lane pulse count")
                passed = complete and run_rc == 1 and all(
                    any(line.startswith("FAIL: " + check) for line in failures)
                    for check in required)
            else:
                passed = complete and run_rc == 0 and not failures
            result = dict(variant=name, build_rc=build_rc, run_rc=run_rc,
                          passed=passed, failures=failures)
            results.append(result)
            print(json.dumps(result), flush=True)
            if not passed:
                raise RuntimeError(f"{name}: missing required verdict; see {log}")
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print("Name-write mutation: decode killed; golden and restored PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
