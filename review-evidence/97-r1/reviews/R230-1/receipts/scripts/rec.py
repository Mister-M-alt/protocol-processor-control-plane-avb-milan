#!/usr/bin/env python3
# R230 receipt runner: rec.py NAME CWD [VAR=VALUE ...] -- COMMAND...
# Runs COMMAND in CWD with the listed environment overrides (VAR= unsets VAR),
# writes receipts/NAME.log (stdout+stderr) and receipts/NAME.json (command,
# cwd, overrides, start, duration, exit code). Never interprets the result.
import datetime
import json
import os
import subprocess
import sys
import time
from pathlib import Path

RECEIPTS = Path(__file__).resolve().parent.parent


def main():
    name, cwd = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    sep = rest.index("--")
    overrides, command = rest[:sep], rest[sep + 1:]
    env = dict(os.environ)
    applied = {}
    for item in overrides:
        key, value = item.split("=", 1)
        if value == "":
            env.pop(key, None)
            applied[key] = None
        else:
            env[key] = value
            applied[key] = value
    log = RECEIPTS / f"{name}.log"
    start = datetime.datetime.now(datetime.timezone.utc)
    t0 = time.monotonic()
    with log.open("w") as out:
        rc = subprocess.run(command, cwd=cwd, env=env, stdout=out,
                            stderr=subprocess.STDOUT).returncode
    meta = {
        "name": name, "cwd": cwd, "command": command, "env_overrides": applied,
        "started_utc": start.isoformat(), "seconds": round(time.monotonic() - t0, 3),
        "exit_code": rc, "log": log.name,
        "inherited": {k: os.environ.get(k) for k in
                      ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "MAKEFLAGS", "CXX", "TMPDIR")},
    }
    (RECEIPTS / f"{name}.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(f"{name}: exit {rc} in {meta['seconds']} s -> {log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
