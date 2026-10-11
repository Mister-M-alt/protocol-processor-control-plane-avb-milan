#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant each guard's weaker forms, and the bench's own defects; require named failures.

Every guard in guards.py's GUARDS gets five arms, each planted in a scratch copy of
hdl/ of its own and graded by guards.py on that copy with `--only` the guard:
  error     its `$fatal(1, ` becomes `$error(`
  warning   its `$fatal(1, ` becomes `$warning(`
  initial   its `if` becomes `initial if`, the guard's old placement in the listener
  tightened its boundary moves by one to refuse the nearest passing value
  loosened  its boundary moves by one to admit the violating value
Other arms break the inventory, message check and narrow deadline warning allowance.

An arm is KILLED only when guards.py exits non-zero and prints the arm's own FAIL line,
naming the guard; the line shows which front ends caught it. A positive control on an
unplanted copy runs first and must pass. `--jobs N` runs up to N copies at once; the
results are read in the declared order.
"""
import argparse
import dataclasses
from collections.abc import Callable
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tb" / "common"))
sys.path.insert(0, str(HERE))
from mutant_pool import add_jobs_argument, in_order  # noqa: E402
import guards  # noqa: E402


@dataclasses.dataclass(frozen=True)
class Arm:
    """One planted defect, the guard it is graded on and the FAIL lines that kill it."""
    name: str
    guard: str
    plant: Callable[[Path], None]
    expect: tuple[str, ...]


def replace_once(path: Path, old: str, new: str) -> None:
    """Edit one file of the scratch tree, refusing drift."""
    text = path.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f"`{old}` is not once in {path}")
    path.write_text(text.replace(old, new))


def site_of(tree: Path, name: str) -> guards.Site:
    """The guard's severity task in the scratch tree."""
    found = [s for s in guards.inventory(tree) if f"{s.module}.{s.block}" == name]
    if len(found) != 1:
        raise RuntimeError(f"{name}: {len(found)} severity tasks in the scratch tree")
    return found[0]


def severity(name: str, task: str) -> Callable[[Path], None]:
    """Turn the guard's $fatal(1, ...) into `task`(...), on its own line."""
    def plant(tree: Path) -> None:
        """Edit only the located guard's task in this isolated source tree."""
        site = site_of(tree, name)
        path = tree / site.path
        lines = path.read_text().split("\n")
        if lines[site.line - 1].count("$fatal(1, ") != 1:
            raise RuntimeError(f"{name}: no `$fatal(1, ` at {site.path}:{site.line}")
        lines[site.line - 1] = lines[site.line - 1].replace("$fatal(1, ", f"{task}(")
        path.write_text("\n".join(lines))
    return plant


def initial(name: str) -> Callable[[Path], None]:
    """Move the guard into an initial block: `if (...)` becomes `initial if (...)`."""
    def plant(tree: Path) -> None:
        """Find this guard's opening conditional and move it to time zero."""
        site = site_of(tree, name)
        path = tree / site.path
        lines = path.read_text().split("\n")
        for k in range(site.line - 1, -1, -1):
            if re.match(r"^\s*if\s*\(", lines[k]):
                lines[k] = re.sub(r"^(\s*)if", r"\1initial if", lines[k], count=1)
                path.write_text("\n".join(lines))
                return
        raise RuntimeError(f"{name}: no `if` opens the guard above {site.path}:{site.line}")
    return plant


def delete_line_step(tree: Path) -> None:
    """Delete KL_aecp_engine's gen_g_line_step whole: at 577 the store's own guard,
    one level down, is then the first refusal, at another guard's line."""
    path = tree / "hdl/aecp/KL_aecp_engine.sv"
    text, n = re.subn(r"\n  if \(\(LINE_BYTES_P % 8\) != 0\) begin : gen_g_line_step\n.*?\n  end\n",
                      "\n", path.read_text(), count=1, flags=re.S)
    if n != 1:
        raise RuntimeError(f"gen_g_line_step is not in {path}")
    path.write_text(text)


# Each tuple is (old condition term, tightened term, loosened term). These are
# independent defect descriptions, not thresholds derived from GUARDS or RTL.
# Multi-term owner limits both constrain the selected output-stream boundary.
BOUNDARIES = {
    "KL_acmp_nvm_shadow.g_backoff_check": (("RETRY_BACKOFF_CYC_P < 1", "RETRY_BACKOFF_CYC_P < 2",
                                           "RETRY_BACKOFF_CYC_P < 0"),),
    "KL_acmp_nvm_shadow.g_rs_tmo_check": (("RS_TMO_CYC_P < 1", "RS_TMO_CYC_P < 2", "RS_TMO_CYC_P < 0"),),
    "KL_pp_acmp_listener.g_strm_tmo_check": (("STRM_TIMEOUT_CYC_P == 0", "STRM_TIMEOUT_CYC_P <= 1",
                                             "STRM_TIMEOUT_CYC_P < 0"),),
    "KL_aecp_desc_store.gen_g_line_align": (("(LINE_BYTES_P % 8) != 0", "(LINE_BYTES_P % 8) >= 0",
                                            "(LINE_BYTES_P % 8) > 1"),),
    "KL_aecp_desc_store.gen_g_beats": (("LINE_LANES_C > 511", "LINE_LANES_C >= 511", "LINE_LANES_C > 512"),),
    "KL_aecp_desc_store.gen_g_names": (("NAME_ENTRIES_P > 1024", "NAME_ENTRIES_P >= 1024",
                                       "NAME_ENTRIES_P > 1025"),),
    "KL_aecp_desc_store.gen_g_base_align": (("DESC_BASE_P[2:0] != 3'd0", "DESC_BASE_P[2:0] >= 3'd0",
                                            "DESC_BASE_P[2:0] > 3'd1"),),
    "KL_aecp_engine.gen_g_frame_fit": (("FRAME_MAX_C > TX_OVERSIZE_BYTES_P", "FRAME_MAX_C >= TX_OVERSIZE_BYTES_P",
                                       "FRAME_MAX_C > TX_OVERSIZE_BYTES_P + 1"),),
    "KL_aecp_engine.gen_g_resp_cap_fit": (("RESP_BUF_C < ucpu_pkg::RESP_CAP_C", "RESP_BUF_C <= ucpu_pkg::RESP_CAP_C",
                                          "RESP_BUF_C < ucpu_pkg::RESP_CAP_C - 1"),),
    "KL_aecp_engine.gen_g_line_step": (("(LINE_BYTES_P % 8) != 0", "(LINE_BYTES_P % 8) >= 0",
                                       "(LINE_BYTES_P % 8) > 1"),),
    "KL_aecp_engine.gen_g_gamap_page_fit": (("RESP_BUF_C < 24 + 8 * ucpu_pkg::GAMAP_PAGE_MAX_C",
                                            "RESP_BUF_C <= 24 + 8 * ucpu_pkg::GAMAP_PAGE_MAX_C",
                                            "RESP_BUF_C < 24 + 8 * ucpu_pkg::GAMAP_PAGE_MAX_C - 1"),),
    "KL_aecp_engine.gen_g_line_ceiling": (("RESP_BUF_C > RESP_CURSOR_BYTES_C", "RESP_BUF_C >= RESP_CURSOR_BYTES_C",
                                          "RESP_BUF_C > RESP_CURSOR_BYTES_C + 1"),),
    "KL_aecp_notify.g_rows_guard": (("N_CTRL_P < 2", "N_CTRL_P < 3", "N_CTRL_P < 1"),),
    "KL_aecp_nvm_writer.g_shape_check": (("N_NAME_P > 128", "N_NAME_P >= 128", "N_NAME_P > 129"),),
    "KL_aecp_nvm_writer.g_backoff_check": (("RETRY_BACKOFF_CYC_P < 1", "RETRY_BACKOFF_CYC_P < 2",
                                           "RETRY_BACKOFF_CYC_P < 0"),),
    "KL_aecp_nvm_writer.g_rs_tmo_check": (("RS_TMO_CYC_P < 1", "RS_TMO_CYC_P < 2", "RS_TMO_CYC_P < 0"),),
    "KL_aecp_nvm_writer.g_rs_agg_check": (("RS_AGG_CYC_P < 1", "RS_AGG_CYC_P < 2", "RS_AGG_CYC_P < 0"),),
    "KL_aecp_resp_buf.gen_g_base_align": (("RESP_BASE_P[2:0] != 3'd0", "RESP_BASE_P[2:0] >= 3'd0",
                                          "RESP_BASE_P[2:0] > 3'd1"),),
    "KL_aecp_resp_buf.gen_g_size": (("RESP_BYTES_P <= 16", "RESP_BYTES_P <= 17", "RESP_BYTES_P < 16"),),
    "KL_aecp_resp_buf.gen_g_beats": (("LANES_C > 511", "LANES_C >= 511", "LANES_C > 512"),),
    "KL_aecp_ucpu.gen_g_d8_cap": (("RESP_D8_CAP_BYTES_P > 1024", "RESP_D8_CAP_BYTES_P >= 1024",
                                  "RESP_D8_CAP_BYTES_P > 1025"),),
    "KL_pp_nvm_port.g_tmo_check": (("MEM_TIMEOUT_CYC_P < 1", "MEM_TIMEOUT_CYC_P < 2", "MEM_TIMEOUT_CYC_P < 0"),),
    "KL_pp_nvm_port.g_maxp_check": (("MAX_PAYLOAD_P > MAXP_BOUND_C", "MAX_PAYLOAD_P >= MAXP_BOUND_C",
                                    "MAX_PAYLOAD_P > MAXP_BOUND_C + 1"),),
    "KL_pp_originator.g_inflight_check": (("INFLIGHT_P > IFL_N_C", "INFLIGHT_P >= IFL_N_C",
                                           "INFLIGHT_P > IFL_N_C + 1"),),
    "KL_pp_release_merge.g_slot_count_check": (("N_SLOTS_P < 1", "N_SLOTS_P < 2", "N_SLOTS_P < 0"),),
    "KL_srp_encoder.g_depth_check": (("WORST_BYTES_C > TX_STD_BYTES_P", "WORST_BYTES_C >= TX_STD_BYTES_P",
                                      "WORST_BYTES_C > TX_STD_BYTES_P + 1"),),
    "protocol_processor_top.gen_g_avb_if": (("N_AVB_IF_P > 32'd2", "N_AVB_IF_P >= 32'd2",
                                             "N_AVB_IF_P > 32'd3"),),
    "protocol_processor_top.gen_g_tmr_aw": (("TMR_SLOTS_C > (32'd1 << TMR_AW_C)",
                                             "TMR_SLOTS_C >= (32'd1 << TMR_AW_C)",
                                             "TMR_SLOTS_C > ((32'd1 << TMR_AW_C) + 1)"),),
    "protocol_processor_top.gen_g_tmr_fit": (("N_STREAM_IN_P > TMR_SLOTS_C", "N_STREAM_IN_P >= TMR_SLOTS_C",
                                              "N_STREAM_IN_P > TMR_SLOTS_C + 1"),),
    "protocol_processor_top.gen_g_tmr_overlap": (("TMR_TKR_BASE_C     < TMR_LSTN_BASE_C      + N_STREAM_IN_P",
                                                  "TMR_TKR_BASE_C     <= TMR_LSTN_BASE_C      + N_STREAM_IN_P",
                                                  "TMR_TKR_BASE_C     < TMR_LSTN_BASE_C      + N_STREAM_IN_P - 1"),),
    "protocol_processor_top.gen_g_owner_overlap": (
        ("OWN_SRPTK_END_C > 32'(PP_OWN_TKR_C)", "OWN_SRPTK_END_C >= 32'(PP_OWN_TKR_C)",
         "OWN_SRPTK_END_C > 32'(PP_OWN_TKR_C) + 1"),
        ("OWN_TKR_END_C   > 32'(PP_OWN_SRP_LS_C)", "OWN_TKR_END_C   >= 32'(PP_OWN_SRP_LS_C)",
         "OWN_TKR_END_C   > 32'(PP_OWN_SRP_LS_C) + 1")),
}


def boundary(name: str, side: int) -> Callable[[Path], None]:
    """Move this guard's fixed boundary by one, without touching sibling guards."""
    def plant(tree: Path) -> None:
        """Replace only condition terms in the block owning the located task."""
        site = site_of(tree, name)
        path = tree / site.path
        lines = path.read_text().split("\n")
        start = next(k for k in range(site.line - 1, -1, -1)
                     if re.match(r"^\s*if\s*\(", lines[k]))
        condition = "\n".join(lines[start:site.line - 1])
        for terms in BOUNDARIES[name]:
            if condition.count(terms[0]) != 1:
                raise RuntimeError(f"{name}: boundary term `{terms[0]}` is not once in its condition")
            condition = condition.replace(terms[0], terms[side])
        lines[start:site.line - 1] = condition.split("\n")
        path.write_text("\n".join(lines))
    return plant


def arms() -> list[Arm]:
    """Five arms per guard, then one arm per other check of the bench."""
    out = []
    for g in guards.GUARDS:
        violating = "FAIL: GUARD "
        for kind, plant in (("error", severity(g.name, "$error")),
                            ("warning", severity(g.name, "$warning")),
                            ("initial", initial(g.name))):
            out.append(Arm(f"{g.name}:{kind}", g.name, plant, (violating,)))
        out.append(Arm(f"{g.name}:tightened", g.name, boundary(g.name, 1), ("FAIL: ELAB ",)))
        out.append(Arm(f"{g.name}:loosened", g.name, boundary(g.name, 2), (violating,)))
    merge = "hdl/packet_engine/KL_pp_release_merge.sv"
    shadow = "hdl/acmp/KL_acmp_nvm_shadow.sv"
    out += [
        Arm("bench:unlisted", "KL_pp_release_merge.g_slot_count_check",
            lambda t: replace_once(t / merge, "  logic [N_SLOTS_P-1:0] pending_r;",
                                   "  if (N_SLOTS_P > 64) begin : g_unlisted_check\n"
                                   '    $fatal(1, "KL_pp_release_merge: unlisted");\n'
                                   "  end\n  logic [N_SLOTS_P-1:0] pending_r;"),
            ("FAIL: inventory KL_pp_release_merge.g_unlisted_check",)),
        Arm("bench:deleted", "KL_aecp_engine.gen_g_line_step", delete_line_step,
            ("FAIL: inventory KL_aecp_engine.gen_g_line_step",
             "FAIL: GUARD ")),
        Arm("bench:message", "KL_pp_nvm_port.g_tmo_check",
            lambda t: replace_once(t / "hdl/packet_engine/KL_pp_nvm_port.sv",
                                   "is outside 1 to 2147483647", "is not within 1 to 2147483647"),
            ("FAIL: GUARD ",)),
        Arm("bench:deadline_user", "KL_acmp_nvm_shadow.g_rs_tmo_check",
            lambda t: replace_once(t / shadow, "  if (RS_TMO_CYC_P < 1)",
                                   "  if (RS_TMO_CYC_P == 1) $warning(\"planted deadline warning\");\n"
                                   "  if (RS_TMO_CYC_P < 1)"), ("FAIL: ELAB ",)),
        Arm("bench:deadline_other_warning", "KL_acmp_nvm_shadow.g_rs_tmo_check",
            lambda t: replace_once(t / shadow, "  if (RS_TMO_CYC_P < 1)",
                                   "  logic [-1:0] planted_bad_range;\n  if (RS_TMO_CYC_P < 1)"),
            ("FAIL: ELAB ",)),
    ]
    return out


def trial(job: tuple[Arm | None, list[str], Path]) -> tuple[int, str]:
    """Copy hdl/ to a scratch tree of its own, plant the arm if any, and grade it."""
    arm, bench, log = job
    with tempfile.TemporaryDirectory(prefix="elab-guards-mutants-") as tmp:
        tree = Path(tmp)
        shutil.copytree(ROOT / "hdl", tree / "hdl")
        only = []
        if arm is not None:
            arm.plant(tree)
            only = ["--only", arm.guard]
        with log.open("w") as stream:
            result = subprocess.run([sys.executable, str(HERE / "guards.py"), "--root",
                                     str(tree), "--jobs", "1", *bench, *only],
                                    stdout=stream, stderr=subprocess.STDOUT, check=False)
        return result.returncode, log.read_text()


def killed(arm: Arm, rc: int, contents: str) -> tuple[bool, list[str]]:
    """Non-zero, a tally, and every expected FAIL line, each naming the arm's guard."""
    lines = [line for line in contents.splitlines() if line.startswith("FAIL:")]
    named = [line for line in lines if arm.guard in line or
             (arm.name == "bench:unlisted" and "g_unlisted_check" in line)]
    every = all(any(line.startswith(prefix) for line in named) for prefix in arm.expect)
    if arm.name.endswith((":tightened", ":loosened")):
        frontends = {line.split()[1] for line in contents.splitlines()
                     if line.startswith(("GUARD ", "ELAB "))}
        frontends |= {line.split()[2] for line in lines if line.startswith(("FAIL: GUARD ", "FAIL: ELAB "))}
        kind = "ELAB" if arm.name.endswith(":tightened") else "GUARD"
        every = every and bool(frontends) and all(
            any(re.match(rf"FAIL: {kind}\s+{name}\s", line) for line in named) for name in frontends)
    return rc != 0 and "checks:" in contents and every, named


def main() -> int:
    """Select arms, isolate every write in scratch trees, fail on any unproven arm."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--only", default="", help="comma-separated arm names")
    parser.add_argument("--verilator", default="verilator")
    parser.add_argument("--frontend", action="append",
                        choices=("verilator", "yosys", "xsim", "synth"))
    add_jobs_argument(parser)
    args = parser.parse_args()
    selected = arms()
    if args.only:
        wanted = set(args.only.split(","))
        unknown = wanted - {a.name for a in selected}
        if unknown:
            print(f"unknown arm(s): {', '.join(sorted(unknown))}", file=sys.stderr)
            return 2
        selected = [a for a in selected if a.name in wanted]
    if args.output.exists():
        print(f"refusing to write into the existing {args.output}", file=sys.stderr)
        return 2
    args.output.mkdir(parents=True)
    bench = ["--verilator", args.verilator]
    for name in args.frontend or ():
        bench += ["--frontend", name]

    with in_order(trial, [(None, bench, args.output / "control.log")], 1) as results:
        rc, contents = next(results)
    ok = rc == 0 and " 0 FAIL" in contents
    print(f"control: rc={rc} {'PASS' if ok else 'FAIL'}", flush=True)
    if not ok:
        print(contents[-4000:])
        return 1
    passed = 0
    units = [(arm, bench, args.output / f"{arm.name.replace(':', '-')}.log") for arm in selected]
    with in_order(trial, units, args.jobs) as results:
        for arm, (rc, contents) in zip(selected, results):
            ok, named = killed(arm, rc, contents)
            passed += ok
            print(f"{arm.name}: rc={rc} {'KILLED' if ok else 'UNPROVEN'}", flush=True)
            for line in named:
                print(f"    {line[:200]}", flush=True)
            if not ok:
                print(contents[-2500:], flush=True)
    print(f"elab guard mutants: {passed} of {len(selected)} killed")
    return int(passed != len(selected))


if __name__ == "__main__":
    raise SystemExit(main())
