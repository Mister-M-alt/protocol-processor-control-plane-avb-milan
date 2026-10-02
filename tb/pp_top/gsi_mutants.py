#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Build isolated GSI mutants and require a named response check to fail.

The golden, every mutant and the final restored run each build in a temporary
source copy of their own. `--jobs N` runs up to N of them at once; the golden
runs first and must pass, and the results are read in the declared order.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from mutant_pool import add_jobs_argument, in_order  # noqa: E402

#: one variant to build: its name, its exact edit (file, old, new, sites) or
#: None for an unmutated copy, its required failing check, the output
#: directory and the Verilator to build with
Variant = tuple[str, tuple[str, str, str, int] | None, str, Path, str]


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
        # Each fixed variant runs gsi-internal-only: boot, query and frame
        # waits have cycle budgets; the other loops consume finite inputs.
        return subprocess.run(command, cwd=cwd, stdout=stream,
                              stderr=subprocess.STDOUT, check=False).returncode


def check_variant(variant: Variant) -> dict[str, object]:
    """Copy only sources to a build tree of its own and plant the edit, if any.

    Compilation must pass; only a completed simulation can kill a mutant.
    """
    name, edit, expected, output, verilator = variant
    root = Path(__file__).resolve().parents[2]
    log = output / (name + "-run.log")
    with tempfile.TemporaryDirectory(prefix="pp-gsi-mutants-") as temp:
        tree = Path(temp)
        for directory in ("hdl", "tb/common", "tb/pp_top"):
            shutil.copytree(root / directory, tree / directory,
                            ignore=shutil.ignore_patterns("obj*", "*.hex", "__pycache__"))
        if edit is not None:
            filename, old, new, count = edit
            path = tree / filename
            original = path.read_text()
            if original.count(old) != count:
                raise RuntimeError(f"{name}: expected {count} exact edit sites")
            path.write_text(original.replace(old, new))
        bench = tree / "tb/pp_top"
        build_rc = run(["make", "gsi-build", "VERILATOR=" + verilator], bench,
                       output / (name + "-build.log"))
        if build_rc != 0:
            raise RuntimeError(f"{name}: build failed ({build_rc}); not a detected mutant")
        run_rc = run(["./obj_dir/Vpp_top_sim", "--gsi-internal-only"], bench, log)
    transcript = log.read_text()
    named = [line for line in transcript.splitlines()
             if line.startswith("FAIL: " + expected)] if expected else []
    complete = "[build default," in transcript
    passed = complete and ((run_rc == 1 and bool(named)) if expected
                           else (run_rc == 0 and "0 failures" in transcript))
    return {"variant": name, "build_rc": build_rc, "run_rc": run_rc,
            "named_failures": named, "passed": passed}


def report(result: dict[str, object], output: Path) -> dict[str, object]:
    """Print one variant's result; anything but its expected verdict stops the campaign."""
    print(json.dumps(result), flush=True)
    if not result["passed"]:
        log = output / f"{result['variant']}-run.log"
        raise RuntimeError(f"{result['variant']}: missing expected verdict; see {log}")
    return result


def main() -> int:
    """Copy only sources to temporary build trees; leave the lane untouched."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verilator", default="verilator")
    parser.add_argument("--only", nargs="*", default=None)
    add_jobs_argument(parser)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    variants = mutations()
    unknown = sorted(set(args.only or ()) - {v[0] for v in variants})
    if unknown:
        parser.error("unknown mutant(s): " + " ".join(unknown))
    chosen = [v for v in variants if not args.only or v[0] in args.only]
    # Cap the parallel builds at eight CPUs where the platform can pin them;
    # elsewhere the builds run unpinned with the same verdicts.
    if hasattr(os, "sched_setaffinity") and hasattr(os, "sched_getaffinity"):
        os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:8])
    records = [report(check_variant(("golden", None, "", output, args.verilator)), output)]
    units: list[Variant] = [(name, (filename, old, new, count), expected, output, args.verilator)
                            for name, filename, old, new, count, expected in chosen]
    units.append(("restored", None, "", output, args.verilator))
    with in_order(check_variant, units, args.jobs) as results:
        records += [report(result, output) for result in results]
    (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    print(f"GSI mutations: {len(chosen)} detected by named checks; "
          "golden and restored PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
