#!/usr/bin/env python3
"""Sequential scratch-only controls for the accepted R230-M1 correction."""
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

out = Path(__file__).resolve().parent.parent
fixed = Path(json.loads((out / "scratch.json").read_text())["fixed"])
scratch = fixed.parent
mutant = scratch / "mutant"
wire = 'static_assert(SRP_DEF_VID != 2,\n              "SRP VID fixture must differ from product default 2 in the 16-bit wire value");\n'
class_d = 'static_assert((SRP_DEF_VID & 0x0FFFu) != 2,\n              "SRP VID fixture must differ from product default 2 in the 12-bit class-D value");\n'
variants = [
    ("05-remove-wire", "sim_main.cpp", wire, "", "fixture-guards"),
    ("06-remove-class-d", "sim_main.cpp", class_d, "", "fixture-guards"),
    ("07-unrelated-error", "sim_main.cpp", wire,
     wire + '#if PP_TOP_SRP_DOM_DEF_VID == 0x0002\n#error A166 unrelated compiler error control\n#endif\n', "fixture-guards"),
    ("08-remove-locale", "fixture_guards.py", '        compiler_env["LC_ALL"] = "C"\n', "", "fixture-guards"),
]
records = []
for name, filename, old, new, target in variants:
    assert not mutant.exists(), mutant
    shutil.copytree(fixed, mutant)
    path = mutant / "tb/pp_top" / filename
    before = path.read_text()
    assert before.count(old) == 1, (name, repr(old))
    after = before.replace(old, new)
    path.write_text(after)
    patch = "".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                         fromfile="a/tb/pp_top/"+filename,
                                         tofile="b/tb/pp_top/"+filename))
    (out / "mutants" / (name + ".patch")).write_text(patch)
    differences = []
    for source in fixed.rglob("*"):
        if source.is_file():
            rel = source.relative_to(fixed)
            changed = mutant / rel
            if source.read_bytes() != changed.read_bytes() or source.stat().st_mode != changed.stat().st_mode:
                differences.append(str(rel))
    assert differences == ["tb/pp_top/"+filename], differences
    record = dict(name=name, differences=differences,
                  before_sha256=hashlib.sha256(before.encode()).hexdigest(),
                  mutant_sha256=hashlib.sha256(after.encode()).hexdigest())
    command = ["rtk", "proxy", "python3", str(out / "scripts/run.py"),
               "--cwd", str(mutant / "tb/pp_top"), "--expect", "2", name, "--",
               "rtk", "proxy", "env", "-u", "LC_ALL", "LANG=en_US.UTF-8", "LANGUAGE=de",
               "make", "-j1", "VERILATOR="+str(out / "scripts/verilator8"), target]
    subprocess.run(command, check=True)
    log = (out / "receipts" / (name + ".log")).read_text()
    if name == "08-remove-locale":
        assert "FAILED (failures=2)" in log
        assert "fixture guard default" not in log
    else:
        assert "FAIL: fixture 0002: unexpected compiler result 1" in log
        errors = [line for line in log.splitlines() if "error:" in line]
        assert len(errors) == (3 if name == "07-unrelated-error" else 1), errors
        record["compiler_error_lines"] = errors
    records.append(record)
    (out / "mutants/results.json").write_text(json.dumps(records, indent=2)+"\n")
    shutil.rmtree(mutant)
print("All four sequential mutants failed at the intended gate; disposable copies removed.")
