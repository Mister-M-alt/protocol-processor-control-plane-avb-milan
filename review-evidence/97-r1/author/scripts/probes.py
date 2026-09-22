#!/usr/bin/env python3
"""Sequential disposable issue97 controls; every subprocess command starts rtk."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

OUT = Path(__file__).resolve().parents[1]
SCRATCH = Path((OUT / "scratch-path.txt").read_text().strip())
SOURCE = SCRATCH / "candidate"
WRAPPER = str(OUT / "bin/verilator")
RUNNER = str(OUT / "scripts/record.py")


def record(name, cwd, command, expected=0):
    subprocess.run(["rtk", "proxy", "python3", RUNNER, "--cwd", str(cwd),
                    "--expect", str(expected), name, "--", *command], check=True)
    return (OUT / "receipts" / (name + ".log")).read_text()


def copy(name, mutation=None):
    dest = SCRATCH / name
    shutil.copytree(SOURCE, dest, ignore=shutil.ignore_patterns("obj_dir", "obj_vid", "*.hex"))
    details = {"name": name, "path": str(dest), "mutation": mutation}
    if mutation:
        filename, before, after = mutation
        file = dest / filename
        original = file.read_text()
        assert original.count(before) == 1, filename
        changed = original.replace(before, after)
        file.write_text(changed)
        details.update(before_sha256=hashlib.sha256(original.encode()).hexdigest(),
                       after_sha256=hashlib.sha256(changed.encode()).hexdigest())
        (OUT / "receipts" / (name + ".patch")).write_text("".join(difflib.unified_diff(
            original.splitlines(True), changed.splitlines(True),
            fromfile="a/" + filename, tofile="b/" + filename)))
    (OUT / "receipts" / (name + "-mutation.json")).write_text(json.dumps(details, indent=2)+"\n")
    return dest


def main():
    wire = "SRP VID fixture must differ from product default 2 in the 16-bit wire value"
    class_d = "SRP VID fixture must differ from product default 2 in the 12-bit class-D value"
    for value, messages in (("0002", (wire, class_d)), ("1002", (class_d,))):
        name = "05-refuse-" + value
        tree = copy(name)
        cwd = tree / "tb/pp_top"
        dry = record(name + "-recipe", cwd, ["rtk", "proxy", "make", "-n", "run",
                     "VERILATOR=" + WRAPPER, "SRP_VID_FIXTURE=" + value])
        commands = [shlex.split(line) for line in dry.replace("\\\n", " ").splitlines()
                    if line.startswith(WRAPPER + " ") and "Vpp_top_vid" in line]
        assert len(commands) == 1, commands
        log = record(name, cwd, ["rtk", "proxy", *commands[0]], expected=2)
        errors = [line for line in log.splitlines() if "error:" in line]
        for message in messages:
            assert any("static assertion" in line and message in line for line in errors), log
        if value == "1002":
            assert not any(wire in line for line in errors), log
        assert not (cwd / "obj_vid/Vpp_top_vid").exists()
        print(f"VERIFIED {name}: fixture compilation refused with expected diagnostics", flush=True)

    name = "06-missing-binding"
    tree = copy(name, ("hdl/top/protocol_processor_top.sv",
                       "      .DOM_DEF_VID_P    (SRP_DOM_DEF_VID_P),\n", ""))
    log = record(name, tree / "tb/pp_top", ["rtk", "proxy", "make", "-j8"], expected=2)
    assert "[build default, SRP_DOM_DEF_VID_P 0x0002] 1391 checks, 0 failures" in log
    assert "[build fixture, SRP_DOM_DEF_VID_P 0x5a3c] 20 checks, 13 failures" in log
    assert sum(line.startswith("FAIL: DV") for line in log.splitlines()) == 13
    print("VERIFIED missing binding: default 1391/0, fixture 20/13", flush=True)

    name = "07-child-default"
    tree = copy(name, ("hdl/srp/KL_srp_top.sv",
                       "DOM_DEF_VID_P  = 16'd2", "DOM_DEF_VID_P  = 16'd7"))
    log = record(name, tree / "tb/pp_top", ["rtk", "proxy", "make", "-j8"])
    assert "[build default, SRP_DOM_DEF_VID_P 0x0002] 1391 checks, 0 failures" in log
    assert "[build fixture, SRP_DOM_DEF_VID_P 0x5a3c] 20 checks, 0 failures" in log
    assert "1411 checks: 1411 PASS, 0 FAIL" in log
    print("VERIFIED child default 7 with binding intact: both builds green", flush=True)

    for name, guard in (("08-remove-wire-guard", "static_assert(SRP_DEF_VID != 2,\n              \"" + wire + "\");\n"),
                        ("09-remove-class-d-guard", "static_assert((SRP_DEF_VID & 0x0FFFu) != 2,\n              \"" + class_d + "\");\n")):
        tree = copy(name, ("tb/pp_top/sim_main.cpp", guard, ""))
        log = record(name, tree / "tb/pp_top", ["rtk", "proxy", "make", "-j8", "fixture-guards"], expected=2)
        assert "FAIL: fixture 0002: unexpected compiler result" in log
        print(f"VERIFIED {name}: focused coverage detects missing assertion", flush=True)
    print("All sequential controls verified", flush=True)


if __name__ == "__main__":
    main()
