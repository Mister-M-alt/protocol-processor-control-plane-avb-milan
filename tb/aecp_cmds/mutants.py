#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The standing controls of tb/aecp_cmds: plant each edit, sweep, require the named failure.

Each arm is one exact edit to the engine (or the generator) that the AECP command
table must catch through what the engine is observed to do, never through its
text: R608-1's q1 (SET_CLOCK_SOURCE's success arm redirected to UPC_GCLKS_C) and
q2 (its decode keyed on the GET opcode) first, then a moved length floor, the
MVU sub-decode, the message-type guard, an unserved opcode decoded, a
per-record command accepted between records, the GET_DYNAMIC_INFO membership,
record length and member dispatch, the unsolicited kind map, and a program's
response size. The golden, every arm and the final restored run each build and
sweep in a temporary source copy of their own (`--jobs N` at once); an arm is
detected only when the suite fails and prints a FAIL line with its words.
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

ENGINE = "hdl/aecp/KL_aecp_engine.sv"
UCODE = "hdl/aecp/ucode/gen_ucode.py"

#: one variant to run: its name, its exact edit (file, old, new, sites) or None
#: for an unmutated copy, the words its FAIL line must carry, the output
#: directory and the Verilator to build with
Variant = tuple[str, tuple[str, str, str, int] | None, str, Path, str]


def mutations() -> list[tuple[str, str, str, str, int, str]]:
    """Each tuple names one exact source edit and the words of its named failure."""
    return [
        ("q1-set-clock-source-to-gclks", ENGINE,
         "else                                    upc_r <= UPC_SCLKS_C;",
         "else                                    upc_r <= UPC_GCLKS_C;", 1,
         "decode: SET_CLOCK_SOURCE: the table's entries name SCLKS, which the engine "
         "never dispatches AEM 0x0016 to"),
        ("q2-set-clock-source-decoded-on-get", ENGINE,
         "&& (txn_w.opcode == OP_SET_CLOCK_SRC_C);",
         "&& (txn_w.opcode == OP_GET_CLOCK_SRC_C);", 1,
         "decode: GET_CLOCK_SOURCE: the engine dispatches AEM 0x0017 to SCLKS"),
        ("floor-set-clock-source-16", ENGINE,
         "if (cmd_r.cdl < 11'd20)                 upc_r <= UPC_BADARG4_C;",
         "if (cmd_r.cdl < 11'd16)                 upc_r <= UPC_BADARG4_C;", 1,
         "decode: SET_CLOCK_SOURCE: the shortest AEM 0x0016 the engine dispatches to "
         "SCLKS is cdl 16"),
        ("mvu-sub-decode-other-command-type", ENGINE,
         "&& (desc_ty_r  == MVU_GET_MILAN_INFO_C);",
         "&& (desc_ty_r  == 16'h0001);", 1,
         "decode: GET_MILAN_INFO: the table's entries name MVUINFO, which the engine "
         "never dispatches MVU 0x0000 to"),
        ("mvu-protocol-id-tail-unchecked", ENGINE,
         "&& (pid_lo_r   == 2'b11)", "&& 1'b1", 1,
         "decode: MVU/wrong-protocol-id 0x0000 is no table command and the engine "
         "dispatches it to MVUINFO"),
        ("message-type-guard-dropped", ENGINE,
         "assign sclks_w  = aem_w", "assign sclks_w  = (txn_w.protocol == PP_PROTO_AEM)", 1,
         "decode: AVC 0x0016 is no table command and the engine dispatches it to SCLKS"),
        ("unserved-opcode-decoded", ENGINE,
         "                    && (txn_w.opcode == OP_GET_CONFIG_C);",
         "                    && ((txn_w.opcode == OP_GET_CONFIG_C)"
         " || (txn_w.opcode == 16'h0005));", 1,
         "decode: AEM 0x0005 is no table command and the engine dispatches it to GCFG"),
        ("mappings-accepted-between-records", ENGINE,
         "if (({8'd0, cmd_r.cdl}\n                   != (19'd20 + ({3'd0, desc_ty_r} << 3)))",
         "if (({8'd0, cmd_r.cdl}\n                   < (19'd20 + ({3'd0, desc_ty_r} << 3)))", 1,
         "decode: ADD_AUDIO_MAPPINGS: the engine dispatches AEM 0x002C to AMADD at cdl"),
        ("mappings-four-byte-record", ENGINE,
         "                   != (19'd20 + ({3'd0, desc_ty_r} << 3)))",
         "                   != (19'd20 + ({3'd0, desc_ty_r} << 2)))", 1,
         "decode: ADD_AUDIO_MAPPINGS: command per is 8, the gcd of accepted offsets from floor 20 is 4"),
        ("mappings-sixteen-byte-record", ENGINE,
         "                   != (19'd20 + ({3'd0, desc_ty_r} << 3)))",
         "                   != (19'd20 + ({3'd0, desc_ty_r} << 4)))", 1,
         "decode: ADD_AUDIO_MAPPINGS: command per is 8, the gcd of accepted offsets from floor 20 is 0"),
        ("gdi-member-refused", ENGINE,
         "GDI_GET_SIGNAL_SEL_C, OP_GET_COUNTERS_C, GDI_GET_MEM_LEN_C,",
         "GDI_GET_SIGNAL_SEL_C, GDI_GET_MEM_LEN_C,", 1,
         "decode: GET_COUNTERS: the table makes it a GET_DYNAMIC_INFO member and the "
         "engine refuses a record of it"),
        ("gdi-record-length", ENGINE,
         "g_sub_exec_w = 1'b1; g_sub_rlen_w = 11'd136;",
         "g_sub_exec_w = 1'b1; g_sub_rlen_w = 11'd132;", 1,
         "decode: GET_COUNTERS: inside GET_DYNAMIC_INFO its GCTRS record leaves the "
         "aggregate ENTITY_MISBEHAVING"),
        ("gdi-member-dispatch", ENGINE,
         "? UPC_GSRATE_C : UPC_TIZ4NS_C;", "? UPC_GCLKS_C : UPC_TIZ4NS_C;", 1,
         "decode: GET_SAMPLING_RATE: inside GET_DYNAMIC_INFO the engine dispatches it to "
         "GCLKS"),
        ("unsolicited-command-type", ENGINE,
         "PP_UNS_CFG_C:   begin uns_ct_w = OP_SET_CONFIG_C;",
         "PP_UNS_CFG_C:   begin uns_ct_w = OP_GET_CONFIG_C;", 1,
         "decode: SET_CONFIGURATION: the PP_UNS_CFG_C job answers"),
        ("unsolicited-body", ENGINE,
         "uns_upc_w = UPC_GCLKS_C;   end", "uns_upc_w = UPC_GSRATE_C;  end", 1,
         "decode: SET_CLOCK_SOURCE: the PP_UNS_CLKS_C job answers"),
        ("response-field-dropped", UCODE,
         "    u('BUILD_FLD', ra=2, fmt=FMT_W),             # reserved             @30\n"
         "    u('SEND_RESP'),\n    u('END'),\n])\n\n# --- the shared wrong-target refusals",
         "    u('NOP'),\n"
         "    u('SEND_RESP'),\n    u('END'),\n])\n\n# --- the shared wrong-target refusals", 1,
         "decode: GET_CLOCK_SOURCE: GCLKS answered SUCCESS at cdl 18"),
    ]


def run(command: list[str], cwd: Path, log: Path) -> int:
    """Run in the foreground, recording the command's own status and output."""
    with log.open("w") as stream:
        # every trial of the sweep has its own clock budget (sim_main.cpp)
        return subprocess.run(command, cwd=cwd, stdout=stream,
                              stderr=subprocess.STDOUT, check=False).returncode


def check_variant(variant: Variant) -> dict[str, object]:
    """Copy only sources to a tree of its own, plant the edit, build, sweep and
    judge. Compilation and the sweep must complete; only the gate's verdict can
    kill an arm."""
    name, edit, expected, output, verilator = variant
    root = Path(__file__).resolve().parents[2]
    log = output / (name + "-run.log")
    with tempfile.TemporaryDirectory(prefix="aecp-cmds-mutants-") as temp:
        tree = Path(temp)
        for directory in ("hdl", "scripts", "tb/common", "tb/aecp_cmds"):
            shutil.copytree(root / directory, tree / directory,
                            ignore=shutil.ignore_patterns("obj*", "*.hex", "image.*",
                                                          "__pycache__"))
        (tree / "docs/architecture").mkdir(parents=True)
        for filename in ("docs/architecture/06_aecp_engine.md",
                         "docs/architecture/09_verification.md", "docs/00_MILAN_COMPLIANCE_REVIEW.md"):
            shutil.copyfile(root / filename, tree / filename)
        if edit is not None:
            filename, old, new, count = edit
            path = tree / filename
            original = path.read_text()
            if original.count(old) != count:
                raise RuntimeError(f"{name}: expected {count} exact edit sites")
            path.write_text(original.replace(old, new))
        bench = tree / "tb/aecp_cmds"
        build_rc = run(["make", "build", "VERILATOR=" + verilator], bench,
                       output / (name + "-build.log"))
        if build_rc != 0:
            raise RuntimeError(f"{name}: build failed ({build_rc}); not a detected arm")
        sweep_rc = run(["./obj_dir/Vaecp_cmds", "obj_dir/observed.json"], bench,
                       output / (name + "-sweep.log"))
        if sweep_rc != 0:
            raise RuntimeError(f"{name}: the sweep did not complete ({sweep_rc}); see "
                               f"{output / (name + '-sweep.log')}")
        run_rc = run(["python3", "-B", "../../scripts/check_aecp_commands.py",
                      "--observed", "obj_dir/observed.json"], bench, log)
    transcript = log.read_text()
    named = [line for line in transcript.splitlines()
             if line.startswith("FAIL:") and expected in line] if expected else []
    tally = [line for line in transcript.splitlines() if " checks: " in line]
    passed = bool(tally) and ((run_rc == 1 and bool(named)) if expected
                              else (run_rc == 0 and tally[-1].endswith(" 0 FAIL")))
    return {"variant": name, "build_rc": build_rc, "sweep_rc": sweep_rc, "run_rc": run_rc,
            "named_failures": named, "passed": passed}


def report(result: dict[str, object], output: Path) -> dict[str, object]:
    """Print one variant's result; anything but its expected verdict stops the campaign."""
    print(json.dumps(result), flush=True)
    if not result["passed"]:
        log = output / f"{result['variant']}-run.log"
        raise RuntimeError(f"{result['variant']}: missing expected verdict; see {log}")
    return result


def main() -> int:
    """Copy only sources to temporary trees; leave the lane untouched."""
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
    print(f"AECP command decode controls: {len(chosen)} of {len(chosen)} detected by "
          "their named failures; golden and restored PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
