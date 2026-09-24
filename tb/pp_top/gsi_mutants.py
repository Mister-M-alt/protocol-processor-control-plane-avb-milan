#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Build isolated GSI mutants and require a named response check to fail."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def mutations():
    """Each tuple names one exact source edit and its required failing check."""
    top = "hdl/top/protocol_processor_top.sv"
    return [
        ("failure-code-zero", top,
         "{gsi_data_i[63:16], gsi_fail_code_r, 8'd0}",
         "{gsi_data_i[63:16], 8'd0, 8'd0}", 1,
         "GI FAILED-0 solicited: failure code"),
        ("failure-bridge-zero", top,
         "4'd5: aecp_gsi_data_w = gsi_fail_bridge_r;",
         "4'd5: aecp_gsi_data_w = 64'd0;", 1,
         "GI FAILED-0 solicited: full failure bridge"),
        ("pbsta-zero", top,
         "{32'd0, gsi_status_r, 24'd0}",
         "{32'd0, 3'd0, gsi_status_r[4:0], 24'd0}", 1,
         "GI PASSIVE solicited: pbsta"),
        ("acmpsta-zero", top,
         "{32'd0, gsi_status_r, 24'd0}",
         "{32'd0, gsi_status_r[7:5], 5'd0, 24'd0}", 1,
         "GI TIMEOUT solicited: acmpsta"),
        ("wrong-sink", top,
         "SINK_IDX_W_C'(gsi_desc_index_o)",
         "SINK_IDX_W_C'(gsi_desc_index_o ^ 16'd1)", 3,
         "GI DISTINCT-0 solicited: full failure bridge"),
        ("integrator-path", top,
         "assign gsi_input_w = (gsi_kind_o == 2'd0)",
         "assign gsi_input_w = (1'b0 && gsi_kind_o == 2'd0)", 1,
         "GI internal seam: selectors 5/7 never requested"),
        ("missing-descriptor-leak", "hdl/aecp/KL_aecp_engine.sv",
         "assign gsi_missing_w = gstri_r && (resp_status_w == ST_NO_SUCH_DESC_C);",
         "assign gsi_missing_w = 1'b0;", 1,
         "GI MISSING solicited: pbsta"),
        ("status-notification-zero", top,
         "|| lstn_gsi_changed_r[k]", "|| 1'b0", 1,
         "GI PASSIVE unsolicited: complete Milan response"),
    ]


def run(command, cwd, log):
    """Run in the foreground, recording the command's own status and output."""
    with log.open("w") as stream:
        return subprocess.run(command, cwd=cwd, stdout=stream,
                              stderr=subprocess.STDOUT, check=False,
                              timeout=1800).returncode


def check_variant(tree, output, name, expected, verilator):
    """Compilation must pass; only a completed simulation can kill a mutant."""
    bench = tree / "tb/pp_top"
    build_rc = run(["make", "gsi-build", "VERILATOR=" + verilator], bench,
                   output / (name + "-build.log"))
    if build_rc != 0:
        raise RuntimeError(f"{name}: build failed ({build_rc}); not a detected mutant")
    log = output / (name + "-run.log")
    run_rc = run(["./obj_dir/Vpp_top_sim", "--gsi-internal-only"], bench, log)
    transcript = log.read_text()
    named = [line for line in transcript.splitlines()
             if line.startswith("FAIL: " + expected)] if expected else []
    complete = "[build default," in transcript
    passed = complete and ((run_rc == 1 and bool(named)) if expected
                           else (run_rc == 0 and "0 failures" in transcript))
    result = {"variant": name, "build_rc": build_rc, "run_rc": run_rc,
              "named_failures": named, "passed": passed}
    print(json.dumps(result), flush=True)
    if not passed:
        raise RuntimeError(f"{name}: missing expected verdict; see {log}")
    return result


def main():
    """Copy only sources to a temporary build tree; leave the lane untouched."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verilator", default="verilator")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:8])
    records = []
    with tempfile.TemporaryDirectory(prefix="pp-gsi-mutants-") as temp:
        tree = Path(temp)
        for directory in ("hdl", "tb/common", "tb/pp_top"):
            shutil.copytree(root / directory, tree / directory,
                            ignore=shutil.ignore_patterns("obj*", "*.hex", "__pycache__"))
        records.append(check_variant(tree, output, "golden", "", args.verilator))
        for name, filename, old, new, count, expected in mutations():
            path = tree / filename
            original = path.read_text()
            if original.count(old) != count:
                raise RuntimeError(f"{name}: expected {count} exact edit sites")
            try:
                path.write_text(original.replace(old, new))
                records.append(check_variant(tree, output, name, expected, args.verilator))
            finally:
                path.write_text(original)
        records.append(check_variant(tree, output, "restored", "", args.verilator))
    (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    print("GSI mutations: 8 detected by named checks; golden and restored PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
