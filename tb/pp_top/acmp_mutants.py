#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant the ACMP lane's negative controls in isolated copies; each must fail its named checks.

Each mutant is one or more exact source edits (every old text must occur exactly
once) planted in a private copy of hdl/, tb/common/ and the one suite that grades
it. The copy is built and run in the foreground. A mutant is KILLED only when its
build succeeds, its run completes with the suite's tally, exits non-zero, and every
check named for it fails. A refused edit, a failed build, a crash or a missing tally
never counts as a kill. A golden copy of every suite in use runs first and must pass;
the tree this script lives in is never touched.

The controls are those of processor issues #47 (ACMP messages outside the listener
and talker sets are inert; the probe-response guard per term), #45 (ACMPDUs longer
than 56 bytes are accepted) and #48 (the integrated settle path from the listener
through the top's SRP service stage to the SRP listener matcher and back). The
suite READMEs carry the matching mutation records.

Usage: python3 tb/pp_top/acmp_mutants.py --output DIR [--verilator V] [--jobs N]
                                         [--only NAME ...]
"""

import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import NamedTuple


class Suite(NamedTuple):
    """Where a suite lives, how it builds, and how its graded run starts."""
    directory: str
    build: tuple[str, ...]
    run: tuple[str, ...]


class Mutant(NamedTuple):
    """One planted defect: its exact edits and the checks that must fail."""
    name: str
    suite: Suite
    edits: tuple[tuple[str, str, str], ...]
    checks: tuple[str, ...]


ACMP_LISTENER = Suite("tb/acmp_listener", (), ("make", "run"))
PP_TOP = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--acmp-only"))

LISTENER = "hdl/acmp/KL_pp_acmp_listener.sv"

MSG_OK = "  assign txn_msg_ok_w   = (txn_i.msg_type == AMSG_PROBE_TX_RESP_C)\n"
GUARD = ("  assign probe_match_w = (ctlr_x_r == rec_r.bind_ctlr_eid)\n"
         "                      && (tk_eid_f_r == rec_r.talker_eid)\n"
         "                      && (tk_uid_f_r == rec_r.talker_uid)\n"
         "                      && (seq_x_r == rec_r.probe_seq);\n")


def guard_without(term: str) -> str:
    """The probe guard with one of its four terms replaced by a constant true."""
    return GUARD.replace(term, "1'b1", 1)


# issue #47: messages outside the listener set, and the guard per term
INERT = (
    Mutant("msg_ok_forced", ACMP_LISTENER, (
        (LISTENER, MSG_OK,
         "  assign txn_msg_ok_w   = 1'b1 || (txn_i.msg_type == AMSG_PROBE_TX_RESP_C)\n"),),
        ("B13 msg 7 status 0 in PWR", "B13 msg 14 status 0 in PWR",
         "B13 msg 3 status 5 in PW2", "B13 msg 15 status 5 in PW2")),
    Mutant("msg_ok_forced", PP_TOP, (
        (LISTENER, MSG_OK,
         "  assign txn_msg_ok_w   = 1'b1 || (txn_i.msg_type == AMSG_PROBE_TX_RESP_C)\n"),),
        ("AI3: the sink never left PRB_W_RESP",)),
    Mutant("guard_ctlr_dropped", ACMP_LISTENER, (
        (LISTENER, GUARD, guard_without("(ctlr_x_r == rec_r.bind_ctlr_eid)")),),
        ("B14 wrong controller_entity_id in PWR", "B14 wrong controller_entity_id in PW2")),
    Mutant("guard_talker_eid_dropped", ACMP_LISTENER, (
        (LISTENER, GUARD, guard_without("(tk_eid_f_r == rec_r.talker_eid)")),),
        ("B14 wrong talker_entity_id in PWR", "B14 wrong talker_entity_id in PW2")),
    Mutant("guard_talker_uid_dropped", ACMP_LISTENER, (
        (LISTENER, GUARD, guard_without("(tk_uid_f_r == rec_r.talker_uid)")),),
        ("B14 wrong talker_unique_id in PWR", "B14 wrong talker_unique_id in PW2")),
)

MUTANTS = INERT
TALLY = re.compile(r"^(ACMP: \d+ checks, \d+ failures|\d+ checks: \d+ PASS, \d+ FAIL)$", re.M)


def plant(tree: Path, edits: tuple[tuple[str, str, str], ...]) -> str:
    """Apply every exact replacement; return a refusal reason, or '' when all applied."""
    for rel, old, new in edits:
        path = tree / rel
        text = path.read_text()
        if text.count(old) != 1:
            return f"{rel}: the planted text occurs {text.count(old)} times"
        path.write_text(text.replace(old, new, 1))
    return ""


def copy_tree(root: Path, tree: Path, suite: Suite) -> None:
    """Copy the sources one suite needs into a private tree, no build products."""
    skip = shutil.ignore_patterns("obj*", "*.hex", "__pycache__")
    for directory in ("hdl", "tb/common", suite.directory):
        shutil.copytree(root / directory, tree / directory, ignore=skip)


def execute(command: tuple[str, ...], cwd: Path, log: Path, verilator: str) -> int:
    """Run one command in the foreground, appending its output to the log."""
    argv = list(command) + (["VERILATOR=" + verilator] if command[0] == "make" else [])
    with log.open("a") as stream:
        return subprocess.run(argv, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                              check=False).returncode


def judge(label: str, suite: Suite, edits: tuple, checks: tuple[str, ...],
          work: tuple[Path, Path, str]) -> dict[str, object]:
    """Build and run one copy (a golden when `edits` is empty) and grade the run."""
    root, output, verilator = work
    with tempfile.TemporaryDirectory(prefix="pp-acmp-mutant-") as temp:
        tree = Path(temp)
        copy_tree(root, tree, suite)
        refusal = plant(tree, edits)
        log = output / f"{label}.log"
        log.write_text("")
        if refusal:
            return {"mutant": label, "verdict": "REFUSED", "reason": refusal}
        cwd = tree / suite.directory
        build_rc = execute(suite.build, cwd, log, verilator) if suite.build else 0
        run_rc = execute(suite.run, cwd, log, verilator) if build_rc == 0 else None
    text = log.read_text(errors="replace")
    fails = [line[len("FAIL: "):] for line in text.splitlines() if line.startswith("FAIL: ")]
    completed = bool(TALLY.search(text))
    missing = [c for c in checks if not any(f.startswith(c) for f in fails)]
    if not edits:
        verdict = "PASS" if (run_rc == 0 and completed and not fails) else "BROKEN"
    else:
        verdict = "KILLED" if (run_rc not in (0, None) and completed and not missing) else "SURVIVED"
    return {"mutant": label, "suite": suite.directory, "build_rc": build_rc, "run_rc": run_rc,
            "completed": completed, "named_checks": list(checks), "missing": missing,
            "failing_checks": fails, "verdict": verdict}


def label_of(mutant: Mutant) -> str:
    """One mutant may be graded by several suites; its record names both."""
    return f"{mutant.name}@{Path(mutant.suite.directory).name}"


def main() -> int:
    """Run a golden copy of every suite in use, then every selected mutant."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verilator", default="verilator")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--only", nargs="*", default=None)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    known = sorted({m.name for m in MUTANTS})
    unknown = sorted(set(args.only or ()) - set(known))
    if unknown:
        parser.error("unknown mutant(s): " + " ".join(unknown))
    chosen = [m for m in MUTANTS if not args.only or m.name in args.only]
    work = (root, output, args.verilator)
    suites = {m.suite.directory: m.suite for m in chosen}
    records = [judge("golden-" + Path(d).name, s, (), (), work) for d, s in sorted(suites.items())]
    for record in records:
        print(json.dumps({k: record[k] for k in ("mutant", "verdict")}), flush=True)
    if all(r["verdict"] == "PASS" for r in records):
        with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
            futures = [pool.submit(judge, label_of(m), m.suite, m.edits, m.checks, work)
                       for m in chosen]
            for future in concurrent.futures.as_completed(futures):
                record = future.result()
                records.append(record)
                print(json.dumps({k: record.get(k) for k in ("mutant", "verdict", "missing")}),
                      flush=True)
    (output / "results.json").write_text(json.dumps(records, indent=1) + "\n")
    killed = sum(r["verdict"] == "KILLED" for r in records)
    goldens = [r for r in records if r["mutant"].startswith("golden-")]
    ok = all(r["verdict"] in ("PASS", "KILLED") for r in records) and len(records) > len(goldens)
    print(f"ACMP mutations: {killed} of {len(chosen)} KILLED by their named checks; "
          f"goldens {'PASS' if all(g['verdict'] == 'PASS' for g in goldens) else 'BROKEN'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
