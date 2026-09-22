#!/usr/bin/env python3
# R230 fixture-value matrix: values.py PP_TOP_DIR VERILATOR
# Takes the Verilator flags and sources from the suite's own `make -n
# fixture-guards` line, verilates once into a scratch model dir, then
# syntax-compiles the real sim_main.cpp once per -DPP_TOP_SRP_DOM_DEF_VID value
# with the C locale and reports every error line. Independent of
# fixture_guards.py's matching logic; the classification below is by message.
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

WIRE = "SRP VID fixture must differ from product default 2 in the 16-bit wire value"
CLASS_D = "SRP VID fixture must differ from product default 2 in the 12-bit class-D value"
VALUES = [None, "5A3C", "5a3c", "0002", "2", "1002", "F002", "10002",
          "0003", "0000", "0FFF", "1005", "0005"]


def main():
    pp, verilator = sys.argv[1], sys.argv[2]
    env = {**os.environ, "LC_ALL": "C"}
    line = subprocess.run(["make", "-s", "-n", "fixture-guards", "VERILATOR=" + verilator, "CXX=g++"],
                          cwd=pp, check=True, capture_output=True, text=True, env=env).stdout
    argv = shlex.split(line.replace("\\\n", " "))
    vflags = argv[argv.index("--") + 1:]
    root = subprocess.run([verilator, "--getenv", "VERILATOR_ROOT"], check=True,
                          capture_output=True, text=True).stdout.strip()
    with tempfile.TemporaryDirectory(prefix="r230-values-") as tmp:
        subprocess.run([verilator, *vflags, "--Mdir", tmp], cwd=pp, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        base = ["g++", "-std=c++17", "-Wall", "-Wextra", "-fsyntax-only", "-I" + tmp,
                "-I" + root + "/include", "-I" + root + "/include/vltstd"]
        for v in VALUES:
            d = [] if v is None else ["-DPP_TOP_SRP_DOM_DEF_VID=0x" + v]
            r = subprocess.run([*base, *d, "sim_main.cpp"], cwd=pp, capture_output=True,
                               text=True, env=env)
            out = r.stdout + r.stderr
            errors = [l for l in out.splitlines() if "error:" in l]
            tags = []
            for l in errors:
                if WIRE in l:
                    tags.append("WIRE-16")
                elif CLASS_D in l:
                    tags.append("CLASS-D-12")
                else:
                    tags.append("OTHER: " + l.split("error:", 1)[1].strip())
            overflow = [l.split("warning:", 1)[1].strip() for l in out.splitlines()
                        if "warning:" in l and "sim_main.cpp:79" in l]
            label = "no override" if v is None else "0x" + v
            print(f"{label:12} rc={r.returncode} errors={len(errors)} {tags}"
                  + (f" sim_main.cpp:79 warning: {overflow}" if overflow else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
