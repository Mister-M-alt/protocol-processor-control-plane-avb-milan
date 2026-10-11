#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Every elaboration guard in hdl/ stops every front end, and stops it as a $fatal.

A guard is a module-scope `if (<condition>) begin : <name> $fatal(1, ...); end` on
parameters (the HDL engineer guide's section 2.1, issue #151). The front ends treat
the elaboration severities differently, and only a module-scope `$fatal` stops all of
them (the guide's measurement): Verilator lint reports every severity as a warning,
sv2v from 0.0.13 lowers a `$error` to an `initial $display` that Yosys never runs,
Vivado carries a `$warning` past, and Verilator lint and xelab never see a guard in an
`initial` block. So a refusal that names the guard is not proof of the class: a
module-scope `$error` is refused by name in Verilator lint and missed by Yosys under
the sv2v after the pinned one.

This bench grades the class, guard by guard. For every guard in GUARDS:
  - its violating case must be refused by every front end that runs, in that front
    end's FATAL words, at the guard's own line;
  - its nearest passing case must elaborate with nothing to report.
And the inventory, so no guard escapes: every elaboration severity task under hdl/
must belong to a guard in GUARDS, and every guard in GUARDS must be under hdl/.

Front ends (`--frontend`, repeatable; each needs only its own tools on PATH):
  verilator  scripts/lint_hdl.sh's flags; the refusal is %Warning-USERFATAL
  yosys      sv2v + Yosys as syn/yosys/run.sh runs them; the refusal is Yosys's
             `FATAL` (sv2v 0.0.12 passes $fatal through) or its `$finish`
             (sv2v from 0.0.13 lowers $fatal to $display and $finish)
  xsim       Vivado xvlog + xelab; the refusal is [VRFC 10-8279] $fatal
  synth      Vivado synth_design -rtl; the refusal is [Synth 8-6058], which is
             Vivado's synthesis word for $fatal and $error alike
Default: verilator, and yosys when sv2v and yosys are both on PATH.
"""
import argparse
import concurrent.futures
import dataclasses
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

#: scripts/lint_hdl.sh's flags, so a refusal here is a refusal there
LINT = ("--lint-only", "-Wall", "-Wno-DECLFILENAME", "-Wno-UNUSEDSIGNAL",
        "-Wno-UNUSEDPARAM")
#: syn/ooc/protocol_processor_ooc.tcl's part
PART = "xc7a100tfgg484-2"

Params = tuple[tuple[str, int], ...]


@dataclasses.dataclass(frozen=True)
class Guard:
    """One guard, the case that must trip it and the nearest case that must not."""
    module: str         # the module that declares it
    block: str          # its generate block's name
    bad: Params         # the violating parameter set ...
    words: str          # ... and the text Verilator's refusal must carry
    good: Params        # the nearest passing set
    top: str = ""       # the module elaborated, when it is not `module`
    #: (old, new) once in the module's file, for a guard no parameter set reaches
    #: alone: a violating shape, planted in a scratch copy, never in the tree
    plant: tuple[str, str] = ()

    @property
    def name(self) -> str:
        return f"{self.module}.{self.block}"

    @property
    def elaborated(self) -> str:
        return self.top or self.module


TOP = "protocol_processor_top"
#: The passing case is the nearest set that lints clean. Three deadlines pass at 2,
#: not 1: at 1 they elaborate, but `x >= 32'(N - 1)` compares against 0 and Verilator
#: -Wall calls it constant (UNSIGNED), which is a lint finding and not a guard.
GUARDS = (
    Guard("KL_acmp_nvm_shadow", "g_backoff_check", (("RETRY_BACKOFF_CYC_P", 0),),
          "RETRY_BACKOFF_CYC_P must be at least 1", (("RETRY_BACKOFF_CYC_P", 1),)),
    Guard("KL_acmp_nvm_shadow", "g_rs_tmo_check", (("RS_TMO_CYC_P", 0),),
          "RS_TMO_CYC_P must be at least 1", (("RS_TMO_CYC_P", 2),)),
    Guard("KL_pp_acmp_listener", "g_strm_tmo_check", (("STRM_TIMEOUT_CYC_P", 0),),
          "STRM_TIMEOUT_CYC_P must be greater than zero", (("STRM_TIMEOUT_CYC_P", 1),)),
    Guard("KL_aecp_desc_store", "gen_g_line_align", (("LINE_BYTES_P", 580),),
          "LINE_BYTES_P=580 must be a multiple of 8", (("LINE_BYTES_P", 576),)),
    Guard("KL_aecp_desc_store", "gen_g_beats", (("IDX_ENTRIES_P", 256),),
          "a burst exceeds the 9-bit mem_req_beats_o field", (("IDX_ENTRIES_P", 255),)),
    Guard("KL_aecp_desc_store", "gen_g_names", (("NAME_ENTRIES_P", 1025),),
          "NAME_ENTRIES_P=1025 must be in 1..1024", (("NAME_ENTRIES_P", 1024),)),
    Guard("KL_aecp_desc_store", "gen_g_base_align", (("DESC_BASE_P", 0x2000_0004),),
          "DESC_BASE_P=20000004 must be 8-byte aligned", (("DESC_BASE_P", 0x2000_0008),)),
    Guard("KL_aecp_engine", "gen_g_frame_fit", (("TX_OVERSIZE_BYTES_P", 617),),
          "a maximum AECP response (618 B) exceeds the oversize slot (617 B)",
          (("TX_OVERSIZE_BYTES_P", 618),)),
    Guard("KL_aecp_engine", "gen_g_resp_cap_fit", (("LINE_BYTES_P", 504),),
          "response buffer (520 B) is smaller than GET_DYNAMIC_INFO limit (524 B)",
          (("LINE_BYTES_P", 576),)),
    Guard("KL_aecp_engine", "gen_g_line_step", (("LINE_BYTES_P", 580),),
          "DESC_LINE_BYTES_P=580 is not a multiple of 8", (("LINE_BYTES_P", 576),)),
    Guard("KL_aecp_engine", "gen_g_gamap_page_fit", (("LINE_BYTES_P", 568),),
          "DESC_LINE_BYTES_P=568 is below 576", (("LINE_BYTES_P", 576),)),
    Guard("KL_aecp_engine", "gen_g_line_ceiling", (("LINE_BYTES_P", 1016),),
          "DESC_LINE_BYTES_P=1016 is above 1008", (("LINE_BYTES_P", 1008),)),
    Guard("KL_aecp_notify", "g_rows_guard", (("N_IF_P", 2), ("N_CTRL_P", 1)),
          "need N_IF_P=2 a power of two and N_CTRL_P=1 above 1",
          (("N_IF_P", 2), ("N_CTRL_P", 2))),
    Guard("KL_aecp_nvm_writer", "g_shape_check", (("N_NAME_P", 129),),
          "a group outgrows its record-id block", (("N_NAME_P", 128),)),
    Guard("KL_aecp_nvm_writer", "g_backoff_check", (("RETRY_BACKOFF_CYC_P", 0),),
          "RETRY_BACKOFF_CYC_P must be at least 1", (("RETRY_BACKOFF_CYC_P", 1),)),
    Guard("KL_aecp_nvm_writer", "g_rs_tmo_check", (("RS_TMO_CYC_P", 0),),
          "RS_TMO_CYC_P must be at least 1", (("RS_TMO_CYC_P", 2),)),
    Guard("KL_aecp_nvm_writer", "g_rs_agg_check", (("RS_AGG_CYC_P", 0),),
          "RS_AGG_CYC_P must be at least 1", (("RS_AGG_CYC_P", 2),)),
    Guard("KL_aecp_resp_buf", "gen_g_base_align", (("RESP_BASE_P", 0x2010_0004),),
          "RESP_BASE_P=20100004 must be 8-byte aligned", (("RESP_BASE_P", 0x2010_0008),)),
    Guard("KL_aecp_resp_buf", "gen_g_size", (("RESP_BYTES_P", 16),),
          "RESP_BYTES_P=16 leaves no payload above the header record",
          (("RESP_BYTES_P", 24),)),
    Guard("KL_aecp_resp_buf", "gen_g_beats", (("RESP_BYTES_P", 4089),),
          "a read burst of 512 beats exceeds the 9-bit mem_req_beats_o field",
          (("RESP_BYTES_P", 4088),)),
    Guard("KL_aecp_ucpu", "gen_g_d8_cap", (("RESP_D8_CAP_BYTES_P", 1025),),
          "RESP_D8_CAP_BYTES_P=1025 outside 524..1024", (("RESP_D8_CAP_BYTES_P", 1024),)),
    Guard("KL_pp_nvm_port", "g_tmo_check", (("MEM_TIMEOUT_CYC_P", 0),),
          "MEM_TIMEOUT_CYC_P=0 is outside 1 to 2147483647", (("MEM_TIMEOUT_CYC_P", 1),)),
    Guard("KL_pp_nvm_port", "g_maxp_check", (("MAX_PAYLOAD_P", 65528),),
          "MAX_PAYLOAD_P=65528 is above 65527", (("MAX_PAYLOAD_P", 65527),)),
    Guard("KL_pp_originator", "g_inflight_check", (("INFLIGHT_P", 17),),
          "INFLIGHT_P must be 1..16", (("INFLIGHT_P", 16),)),
    Guard("KL_pp_release_merge", "g_slot_count_check", (("N_SLOTS_P", 0),),
          "N_SLOTS_P must be positive", (("N_SLOTS_P", 1),)),
    Guard("KL_srp_encoder", "g_depth_check", (("DEPTH_P", 13),),
          "DEPTH_P drain cannot fit a standard TX slot", (("DEPTH_P", 12),)),
    Guard(TOP, "gen_g_avb_if", (("N_AVB_IF_P", 3),),
          "N_AVB_IF_P=3 is outside 1 to 2", (("N_AVB_IF_P", 2),)),
    # TMR_AW_C is $clog2(TMR_SLOTS_C), so 2^TMR_AW_C covers TMR_SLOTS_C at every
    # shape that elaborates: the guard holds the width to the map, should either move
    Guard(TOP, "gen_g_tmr_aw", (), "F08.4: TMR_AW_C=6 cannot index P-TIMER-SLOTS=91", (),
          plant=("$clog2(TMR_SLOTS_C),", "$clog2(TMR_SLOTS_C) - 32'd1,")),
    # TMR_SLOTS_C is the map's own end; only N_AVB_IF_P = 0, refused by gen_g_avb_if
    # first, reaches this one by parameter
    Guard(TOP, "gen_g_tmr_fit", (), "F08.4: slot map ends at 91 but P-TIMER-SLOTS=90", (),
          plant=("= TMR_MAP_C.srp_end,", "= TMR_MAP_C.srp_end - 32'd1,")),
    # every base is the running sum of the extents before it (pp_pkg::pp_timer_map),
    # so each comparison is an equality at every shape
    Guard(TOP, "gen_g_tmr_overlap", (), "F08.4: timer-slot groups OVERLAP at SI=8 SO=8", (),
          plant=("= TMR_MAP_C.tkr;", "= TMR_MAP_C.tkr - 32'd1;")),
    Guard(TOP, "gen_g_owner_overlap", (("N_STREAM_IN_P", 16), ("N_STREAM_OUT_P", 17)),
          "F08.4: owner tags OVERLAP at SI=16 SO=17",
          (("N_STREAM_IN_P", 16), ("N_STREAM_OUT_P", 16))),
)

TASK = re.compile(r"\$(fatal|error|warning|info)\b")
LABEL = re.compile(r"\bbegin\s*:\s*(\w+)")
MODULE = re.compile(r"^\s*module\s+(\w+)")


@dataclasses.dataclass(frozen=True)
class Site:
    """Where a guard's severity task is: its file, line, module and block."""
    path: str
    line: int
    module: str
    block: str


def labelled(lines: list[str], line: int) -> tuple[str, str]:
    """The module and the nearest block label at or above `line` (1-based)."""
    module = block = ""
    for text in lines[:line]:
        code = text.split("//", 1)[0]
        if found := MODULE.match(code):
            module, block = found.group(1), ""
        for found in LABEL.finditer(code):
            block = found.group(1)
    return module, block


def inventory(root: Path) -> list[Site]:
    """Every elaboration severity task under hdl/, outside comments."""
    sites = []
    for path in sorted((root / "hdl").rglob("*.sv")):
        lines = path.read_text().splitlines()
        for number, text in enumerate(lines, 1):
            if TASK.search(text.split("//", 1)[0]):
                module, block = labelled(lines, number)
                sites.append(Site(path.relative_to(root).as_posix(), number, module, block))
    return sites


def sources(root: Path) -> list[str]:
    """Packages first, then every other file, as lint_hdl.sh and run.sh pass them."""
    pkgs = sorted(p.relative_to(root).as_posix() for p in (root / "hdl").rglob("*_pkg.sv"))
    rest = sorted(p.relative_to(root).as_posix() for p in (root / "hdl").rglob("*.sv")
                  if not p.name.endswith("_pkg.sv"))
    return pkgs + rest


def roms(root: Path, into: Path) -> None:
    """The two ROM images the tops read by relative name, as run.sh generates them."""
    subprocess.run([sys.executable, "gen_ucode.py", "-o", str(into / "ucode.hex")],
                   cwd=root / "hdl/aecp/ucode", stdout=subprocess.DEVNULL, check=True)
    subprocess.run([sys.executable, "gen_ltn_rom.py", "-o", str(into / "ltn_rom.hex")],
                   cwd=root / "hdl/acmp/rom", stdout=subprocess.DEVNULL, check=True)


@dataclasses.dataclass
class Case:
    """One elaboration: a guard, violating or passing, in one tree."""
    guard: Guard
    violating: bool
    tree: Path
    site: Site | None

    @property
    def params(self) -> Params:
        return self.guard.bad if self.violating else self.guard.good

    @property
    def label(self) -> str:
        shown = " ".join(f"{k}={v:#x}" if v > 0xFFFFFF else f"{k}={v}"
                         for k, v in self.params)
        if self.violating and self.guard.plant:
            shown = "planted " + (shown or "shape")
        return f"{self.guard.name} {shown or 'defaults'}"


@dataclasses.dataclass
class Verdict:
    """One check's outcome, with the front end's own words."""
    ok: bool
    words: str


def run(cmd: list[str], cwd: Path) -> tuple[int, str]:
    """Combined output and the tool's own status."""
    result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", check=False)
    return result.returncode, result.stdout


def grade(case: Case, rc: int, out: str, refusal: re.Pattern, where) -> Verdict:
    """Violating: refused, in the FATAL words, at this guard. Passing: rc 0, quiet."""
    if not case.violating:
        noisy = [line for line in out.splitlines() if re.match(r"^(%Warning|%Error|ERROR)", line)]
        if rc == 0 and not noisy:
            return Verdict(True, "elaborates")
        return Verdict(False, f"rc {rc}: " + (noisy[0] if noisy else "no output"))
    if rc == 0:
        return Verdict(False, "elaborated; the guard did not stop it")
    for line in out.splitlines():
        found = refusal.search(line)
        if found and where(found) == (case.guard.module, case.guard.block):
            return Verdict(True, line.strip())
    # quote the guard's own diagnostic when it printed one, else the first
    said = [line.strip() for line in out.splitlines() if re.search(r"%Warning|%Error|ERROR", line)]
    own = [line for line in said if case.site and (f"{case.site.path}:{case.site.line}" in line
                                                   or case.guard.words in line)]
    first = (own or said or [f"rc {rc}"])[0]
    return Verdict(False, f"not refused as a $fatal of this guard: {first}")


class Verilator:
    """scripts/lint_hdl.sh's lint, with the case's parameters."""
    name = "verilator"
    REFUSAL = re.compile(r"^%(?:Warning|Error)-USERFATAL: (\S+?):(\d+):\d+: (.*)$")

    def __init__(self, verilator: str):
        self.verilator = verilator

    def prepare(self, tree: Path) -> None:
        pass

    def check(self, case: Case) -> Verdict:
        cmd = [self.verilator, *LINT, "--top-module", case.guard.elaborated,
               *(f"-G{k}={v}" for k, v in case.params), *sources(case.tree)]
        rc, out = run(cmd, case.tree)

        def where(found):
            site = case.site
            if site and (found.group(1), int(found.group(2))) == (site.path, site.line) \
                    and case.guard.words in found.group(3):
                return (site.module, site.block)
            return None
        return grade(case, rc, out, self.REFUSAL, where)


class Yosys:
    """sv2v once per tree, then the elaboration syn/yosys/run.sh runs per top."""
    name = "yosys"
    REFUSAL = re.compile(r"all\.v:(\d+): ERROR: (FATAL\b.*|System task `\$finish' executed\.)")

    def __init__(self):
        self.lowered: dict[Path, tuple[Path, list[str]]] = {}

    def prepare(self, tree: Path) -> None:
        work = Path(tempfile.mkdtemp(prefix="elab-guards-yosys-"))
        with (work / "all.v").open("w") as out:
            subprocess.run(["sv2v", *sources(tree)], cwd=tree, stdout=out, check=True)
        roms(tree, work)
        self.lowered[tree] = (work, (work / "all.v").read_text().splitlines())

    def check(self, case: Case) -> Verdict:
        work, lines = self.lowered[case.tree]
        top = case.guard.elaborated
        sets = " ".join(f"-set {k} {v}" for k, v in case.params)
        script = (f"read_verilog -defer all.v; {f'chparam {sets} {top}; ' if sets else ''}"
                  f"hierarchy -check -top {top}; proc; opt_clean")
        rc, out = run(["yosys", "-q", "-p", script], work)
        return grade(case, rc, out, self.REFUSAL,
                     lambda found: labelled(lines, int(found.group(1))))

    def close(self) -> None:
        for work, _ in self.lowered.values():
            shutil.rmtree(work, ignore_errors=True)


class Xsim:
    """xvlog once per tree, then xelab with the case's parameters on the top."""
    name = "xsim"
    REFUSAL = re.compile(r"^ERROR: \[VRFC 10-8279\] \$fatal : .* \[(\S+):(\d+)\]$")

    def __init__(self):
        self.work: dict[Path, Path] = {}

    def prepare(self, tree: Path) -> None:
        work = Path(tempfile.mkdtemp(prefix="elab-guards-xsim-"))
        rc, out = run(["xvlog", "-sv", *(str(tree / s) for s in sources(tree))], work)
        if rc != 0:
            raise RuntimeError(f"xvlog refused the tree {tree}:\n{out[-2000:]}")
        self.work[tree] = work

    def check(self, case: Case) -> Verdict:
        work = self.work[case.tree]
        snap = re.sub(r"\W", "_", case.label)
        cmd = ["xelab", "-debug", "off", *(a for k, v in case.params
                                            for a in ("-generic_top", f"{k}={v}")),
               f"work.{case.guard.elaborated}", "-s", snap]
        rc, out = run(cmd, work)
        return grade(case, rc, out, self.REFUSAL, lambda found: self.where(case, found))

    @staticmethod
    def where(case: Case, found) -> tuple[str, str] | None:
        site = case.site
        if site and Path(found.group(1)) == case.tree / site.path \
                and int(found.group(2)) == site.line:
            return (site.module, site.block)
        return None

    def close(self) -> None:
        for work in self.work.values():
            shutil.rmtree(work, ignore_errors=True)


class Synth:
    """One Vivado session per tree runs synth_design -rtl for each case in turn."""
    name = "synth"
    REFUSAL = re.compile(r"^ERROR: \[Synth 8-6058\] Synth Error: .* \[(\S+):(\d+)\]$")

    def __init__(self):
        self.out: dict[tuple[Path, str], tuple[int, str]] = {}

    def run_tree(self, tree: Path, cases: list[Case]) -> None:
        work = Path(tempfile.mkdtemp(prefix="elab-guards-synth-"))
        try:
            roms(tree, work)
            tcl = ["set_param general.maxThreads 4"]
            tcl += [f"read_verilog -sv {{{tree / s}}}" for s in sources(tree)]
            for i, case in enumerate(cases):
                generics = " ".join(f"-generic {k}={v}" for k, v in case.params)
                tcl += [f'puts "@@begin {i}"',
                        f"set rc [catch {{synth_design -rtl -top {case.guard.elaborated} "
                        f"-part {PART} {generics}}}]",
                        "catch {close_design}",
                        f'puts "@@end {i} rc=$rc"']
            (work / "cases.tcl").write_text("\n".join(tcl) + "\n")
            _, log = run(["vivado", "-mode", "batch", "-nojournal", "-nolog",
                          "-source", "cases.tcl"], work)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        for i, case in enumerate(cases):
            found = re.search(rf"^@@begin {i}\n(.*?)^@@end {i} rc=(\d+)$", log, re.S | re.M)
            self.out[(tree, case.label)] = ((int(found.group(2)), found.group(1)) if found
                                            else (-1, f"no result in the Vivado log:\n{log[-1500:]}"))

    def check(self, case: Case) -> Verdict:
        rc, out = self.out[(case.tree, case.label)]
        return grade(case, rc, out, self.REFUSAL, lambda found: Xsim.where(case, found))


def planted(root: Path, guard: Guard, path: str, into: Path) -> Path:
    """A scratch copy of hdl/ with the guard's violating shape planted, exactly once."""
    shutil.copytree(root / "hdl", into / "hdl")
    target = into / path
    text = target.read_text()
    old, new = guard.plant
    if text.count(old) != 1:
        raise RuntimeError(f"{guard.name}: the plant `{old}` is not once in {path}")
    target.write_text(text.replace(old, new))
    return into


def main() -> int:
    """Inventory, then every guard's two cases through every selected front end."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT,
                        help="the tree whose hdl/ is graded (default: this checkout)")
    parser.add_argument("--verilator", default=os.environ.get("VERILATOR", "verilator"))
    parser.add_argument("--frontend", action="append",
                        choices=("verilator", "yosys", "xsim", "synth"))
    parser.add_argument("--only", default="", help="comma-separated guard names")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    root = args.root.resolve()
    frontends = args.frontend or (["verilator"] + (
        ["yosys"] if shutil.which("sv2v") and shutil.which("yosys") else []))
    if not args.frontend and "yosys" not in frontends:
        print("YOSYS SKIP (sv2v or yosys is not on PATH)")

    checks = failing = 0

    def verdict(ok: bool, text: str) -> None:
        nonlocal checks, failing
        checks += 1
        failing += not ok
        print(("" if ok else "FAIL: ") + text, flush=True)

    sites = inventory(root)
    by_name: dict[str, list[Site]] = {}
    for site in sites:
        by_name.setdefault(f"{site.module}.{site.block}", []).append(site)
    known = {g.name for g in GUARDS}
    for name, found in sorted(by_name.items()):
        if name not in known:
            for site in found:
                verdict(False, f"inventory {name}: {site.path}:{site.line} is a severity "
                               "task no guard in GUARDS accounts for")
    for guard in GUARDS:
        found = by_name.get(guard.name, [])
        verdict(len(found) == 1,
                f"inventory {guard.name}: " + (f"{found[0].path}:{found[0].line}" if len(found) == 1
                                               else f"{len(found)} severity tasks, not 1"))

    only = set(filter(None, args.only.split(",")))
    unknown = only - known
    if unknown:
        print(f"unknown guard(s): {', '.join(sorted(unknown))}", file=sys.stderr)
        return 2
    selected = [g for g in GUARDS if not only or g.name in only]
    scratch = Path(tempfile.mkdtemp(prefix="elab-guards-"))
    try:
        cases = []
        for guard in selected:
            site = by_name.get(guard.name, [None])[0]
            bad_tree = root
            if guard.plant and site:
                bad_tree = planted(root, guard, site.path, scratch / guard.name)
            cases += [Case(guard, True, bad_tree, site), Case(guard, False, root, site)]
        trees = sorted({c.tree for c in cases})
        for name in frontends:
            if name == "verilator":
                engine = Verilator(args.verilator)
            elif name == "yosys":
                engine = Yosys()
            elif name == "xsim":
                engine = Xsim()
            else:
                engine = Synth()
            try:
                if name == "synth":
                    for tree in trees:
                        engine.run_tree(tree, [c for c in cases if c.tree == tree])
                else:
                    for tree in trees:
                        engine.prepare(tree)
                jobs = 1 if name == "xsim" else args.jobs
                with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as pool:
                    results = list(pool.map(engine.check, cases))
            finally:
                if hasattr(engine, "close"):
                    engine.close()
            for case, result in zip(cases, results):
                kind = "GUARD" if case.violating else "ELAB "
                verdict(result.ok, f"{kind} {name} {case.label}: {result.words}")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print(f"elab guards: {checks} checks: {checks - failing} PASS, {failing} FAIL")
    return int(failing != 0)


if __name__ == "__main__":
    raise SystemExit(main())
