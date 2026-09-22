#!/usr/bin/env python3
"""Real subprocesses, no mocks: import the exact-head fixture_guards, run main()
in this process with verilator8 and cxx-record, and check that (a) the caller's
os.environ is unchanged afterwards, (b) Verilator received exactly the caller
environment, (c) every compiler call received exactly caller + LC_ALL=C.
Run from <scratch>/head/tb/pp_top with the MIXED caller setting (run.py)."""
import glob
import io
import json
import os
import shutil
import sys
from contextlib import redirect_stdout

RC = "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r2-r230/receipts"
S = "/tmp/r230-100-r2-scratch"
d = f"{S}/capture/inprocess"
shutil.rmtree(d, ignore_errors=True)
os.makedirs(f"{d}/cxx")
os.makedirs(f"{d}/venv")
os.environ["R230_CXX_DIR"] = f"{d}/cxx"
os.environ["R230_ENV_DUMP_DIR"] = f"{d}/venv"
sys.dont_write_bytecode = True
sys.path.insert(0, os.getcwd())
import fixture_guards  # noqa: E402  (exact-head module from the scratch clone)

import shlex  # noqa: E402
import subprocess  # noqa: E402
# The gate's exact argv as the Makefile builds it (make -n), split as sh would.
dry = subprocess.check_output(["make", "-n", "fixture-guards",
                               f"VERILATOR={RC}/scripts/verilator8",
                               f"CXX={RC}/scripts/cxx-record"], text=True)
line = next(l for l in dry.replace("\\\n", " ").splitlines()
            if l.startswith("python3 fixture_guards.py"))
argv = shlex.split(line)
sys.argv = argv[argv.index("fixture_guards.py"):]
print("gate argv from make -n:", " ".join(sys.argv[:6]), "...", len(sys.argv), "words")
before = dict(os.environ)
buf = io.StringIO()
with redirect_stdout(buf):
    rc = fixture_guards.main()
after = dict(os.environ)
print(buf.getvalue(), end="")
print(f"main() returned {rc}")
print(f"caller os.environ unchanged after main(): {before == after}")
venvs = [dict(l.split('=', 1) for l in open(p).read().splitlines() if '=' in l)
         for p in sorted(glob.glob(f"{d}/venv/*.env"))]
print(f"verilator calls: {len(venvs)}; each received exactly the caller environment: "
      f"{all(v == before for v in venvs)}")
recs = [json.load(open(p)) for p in sorted(glob.glob(f"{d}/cxx/*.json"))]
expect = {**before, "LC_ALL": "C"}
print(f"compiler calls: {len(recs)}; each received exactly caller + LC_ALL=C: "
      f"{all(r['env'] == expect for r in recs)}; caller LC_ALL={before.get('LC_ALL')!r}")
ok = (rc == 0 and before == after and len(venvs) == 2 and all(v == before for v in venvs)
      and len(recs) == 4 and all(r["env"] == expect for r in recs))
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
