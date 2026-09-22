#!/usr/bin/env python3
"""Run one probe command in a fixed base environment and record it as a receipt.

Writes <receipts>/<name>.log (merged stdout/stderr) and <name>.json (command,
working directory, complete environment given to the command, exit code,
duration, start/end time). The base environment is explicit so that every
receipt says exactly which caller settings the command received:

  PATH=/usr/local/bin:/usr/bin:/bin  HOME=$HOME  LANG=en_US.UTF-8
  TMPDIR=<scratch>/tmp  LOCPATH=<scratch>/locales  (scratch en_US/de_DE/fr_FR)

--set K=V adds or replaces a variable; --unset K removes one.
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import time

RECEIPTS = "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r2-r230/receipts"
SCRATCH = "/tmp/r230-100-r2-scratch"
BASE = {
    "PATH": "/usr/local/bin:/usr/bin:/bin",
    "HOME": os.environ.get("HOME", "$WORKSPACE_HOME"),
    "LANG": "en_US.UTF-8",
    "TMPDIR": SCRATCH + "/tmp",
    "LOCPATH": SCRATCH + "/locales",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--cwd", required=True)
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--unset", action="append", default=[])
    ap.add_argument("--expect-rc", type=int, default=None)
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    env = dict(BASE)
    for kv in a.set:
        k, v = kv.split("=", 1)
        env[k] = v
    for k in a.unset:
        env.pop(k, None)
    start = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    t0 = time.monotonic()
    with open(os.path.join(RECEIPTS, a.name + ".log"), "wb") as log:
        rc = subprocess.run(cmd, cwd=a.cwd, env=env, stdout=log,
                            stderr=subprocess.STDOUT, check=False).returncode
    secs = round(time.monotonic() - t0, 1)
    end = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    rec = {"name": a.name, "cwd": a.cwd, "cmd": cmd, "env": env, "rc": rc,
           "expected_rc": a.expect_rc,
           "as_expected": None if a.expect_rc is None else rc == a.expect_rc,
           "seconds": secs, "start": start, "end": end}
    with open(os.path.join(RECEIPTS, a.name + ".json"), "w", encoding="utf-8") as f:
        json.dump(rec, f, indent=1)
        f.write("\n")
    print(f"{a.name}: rc={rc} expected={a.expect_rc} {secs}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
