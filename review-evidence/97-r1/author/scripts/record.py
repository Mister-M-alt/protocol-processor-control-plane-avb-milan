#!/usr/bin/env python3
import argparse
import datetime
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
p = argparse.ArgumentParser()
p.add_argument("name")
p.add_argument("--cwd", required=True)
p.add_argument("--expect", type=int, default=0)
p.add_argument("command", nargs=argparse.REMAINDER)
a = p.parse_args()
cmd = a.command[1:] if a.command[:1] == ["--"] else a.command
assert cmd[0] == "rtk", cmd
out = Path(__file__).resolve().parents[1]
env = dict(os.environ)
env["PATH"] = str(out / "bin") + os.pathsep + env["PATH"]
env["MAKEFLAGS"] = "-j8"
env["VERILATOR"] = str(out / "bin/verilator")
started = datetime.datetime.now(datetime.timezone.utc).isoformat()
t0 = time.monotonic()
log = out / "receipts" / (a.name + ".log")
print(f"START {a.name}: cwd={a.cwd} {shlex.join(cmd)}", flush=True)
with log.open("w") as f:
    result = subprocess.run(cmd, cwd=a.cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
record = {"name": a.name, "started_utc": started, "seconds": round(time.monotonic()-t0, 3),
          "cwd": a.cwd, "argv": cmd, "shell_command": shlex.join(cmd),
          "environment": {key: env[key] for key in ("PATH", "MAKEFLAGS", "VERILATOR")},
          "exit_code": result.returncode, "expected_exit_code": a.expect, "log": str(log)}
(out / "receipts" / (a.name + ".json")).write_text(json.dumps(record, indent=2) + "\n")
print(f"END {a.name}: rc={result.returncode}, expected={a.expect}, seconds={record['seconds']}, log={log}", flush=True)
print("\n".join(log.read_text().splitlines()[-12:]), flush=True)
raise SystemExit(0 if result.returncode == a.expect else 1)
