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


def mutations() -> list[tuple[str, str, str, str, int, str]]:
    """Each tuple names one exact source edit and its required failing check."""
    top = "hdl/top/protocol_processor_top.sv"
    srp = "hdl/srp/KL_srp_listener_fsm.sv"
    lstn = "hdl/acmp/KL_pp_acmp_listener.sv"
    return [
        ("latency-trigger-removed", top,
         " || srp_evt_tk_latency_chg_w[k]", "", 1,
         "GI LATENCY-CHANGE: exactly one unsolicited response"),
        *[("latency-cmp-" + name, srp,
           "(lat_r[s] != evt_acc_latency_i)",
           f"(lat_r[s][{bits}] != evt_acc_latency_i[{bits}])", 1,
           f"GI LATENCY-WALK-ONE bit {missed} step: exactly one unsolicited response")
          for name, bits, missed in (
              ("low8", "7:0", 8),
              ("low16", "15:0", 16),
              ("high16", "31:16", 0),
              ("low31", "30:0", 31),
              ("high2", "31:30", 0),
          )],
        ("latency-cmp-bit31-dropped", srp,
         "(lat_r[s] != evt_acc_latency_i)",
         "((lat_r[s] & 32'h7fffffff) != (evt_acc_latency_i & 32'h7fffffff))", 1,
         "GI LATENCY-WALK-ONE bit 31 step: exactly one unsolicited response"),
        ("failure-code-zero", top,
         "{gsi_data_i[63:16], gsi_fail_code_w, 8'd0}",
         "{gsi_data_i[63:16], 8'd0, 8'd0}", 1,
         "GI FAILED-0 solicited: failure code"),
        ("failure-bridge-zero", top,
         "4'd5: aecp_gsi_data_w = gsi_fail_bridge_w;",
         "4'd5: aecp_gsi_data_w = 64'd0;", 1,
         "GI FAILED-0 solicited: full failure bridge"),
        ("pbsta-zero", top,
         "{32'd0, gsi_status_w, 24'd0}",
         "{32'd0, 3'd0, gsi_status_w[4:0], 24'd0}", 1,
         "GI PASSIVE solicited: pbsta"),
        ("acmpsta-zero", top,
         "{32'd0, gsi_status_w, 24'd0}",
         "{32'd0, gsi_status_w[7:5], 5'd0, 24'd0}", 1,
         "GI TIMEOUT solicited: acmpsta"),
        ("wrong-sink", top,
         "assign gsi_sink_w = SINK_IDX_W_C'(gsi_desc_index_o);",
         "assign gsi_sink_w = SINK_IDX_W_C'(gsi_desc_index_o ^ 16'd1);", 1,
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
        ("rebind-started-trigger-removed", lstn,
         "act_strt_chg_o <= cellmut_r && bnd_was_r && rec_r.f_bound\n",
         "act_strt_chg_o <= cellmut_r && bnd_was_r && rec_r.f_bound\n"
         "                            && !apend_r[ACT_A2_C]\n", 1,
         "GI REBIND-SW: exactly one unsolicited response"),
        ("failure-change-strobe-removed", srp,
         "if (ind_fchg_w[s]) evt_tk_fail_chg_o[s]   <= 1'b1;",
         "if (1'b0) evt_tk_fail_chg_o[s]   <= 1'b1;", 1,
         "GI FAILED-REFRESH unsolicited: complete Milan response"),
        ("failure-change-redeclares", srp,
         "&& ((reg_r[s] == R_MT_C) || (rtype_r[s] != rx_is_failed_w));",
         "&& ((reg_r[s] == R_MT_C) || (rtype_r[s] != rx_is_failed_w)"
         " || ind_fchg_w[s]);", 1,
         "GI FAILED-REFRESH wire: a changed FailureInformation sends no"),
        ("index-guard-removed", top,
         "if (32'(gsi_desc_index_o) < N_STREAM_IN_P) begin",
         "if (1'b1) begin", 1,
         "GI INDEX-GUARD solicited: pbsta sink 9"),
        ("bridge-gate-removed", top,
         "if (srp_tk_reg_state_w[gsi_sink_w] == 2'd2) begin",
         "if (1'b1) begin", 1,
         "GI ADVERTISE unsolicited: full failure bridge"),
    ]


def run(command: list[str], cwd: Path, log: Path) -> int:
    """Run in the foreground, recording the command's own status and output."""
    with log.open("w") as stream:
        return subprocess.run(command, cwd=cwd, stdout=stream,
                              stderr=subprocess.STDOUT, check=False,
                              timeout=1800).returncode


def check_variant(tree: Path, output: Path, name: str, expected: str,
                  verilator: str) -> dict[str, object]:
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


def main() -> int:
    """Copy only sources to a temporary build tree; leave the lane untouched."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verilator", default="verilator")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    # Cap the parallel builds at eight CPUs where the platform can pin them;
    # elsewhere the builds run unpinned with the same verdicts.
    if hasattr(os, "sched_setaffinity") and hasattr(os, "sched_getaffinity"):
        os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:8])
    records: list[dict[str, object]] = []
    variants = mutations()
    with tempfile.TemporaryDirectory(prefix="pp-gsi-mutants-") as temp:
        tree = Path(temp)
        for directory in ("hdl", "tb/common", "tb/pp_top"):
            shutil.copytree(root / directory, tree / directory,
                            ignore=shutil.ignore_patterns("obj*", "*.hex", "__pycache__"))
        records.append(check_variant(tree, output, "golden", "", args.verilator))
        for name, filename, old, new, count, expected in variants:
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
    print(f"GSI mutations: {len(variants)} detected by named checks; "
          "golden and restored PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
