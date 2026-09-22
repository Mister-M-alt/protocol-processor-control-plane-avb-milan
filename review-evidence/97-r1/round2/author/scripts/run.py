#!/usr/bin/env python3
"""Run one factual probe and retain its exact argv, status and unfiltered output."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shlex
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("name")
parser.add_argument("--cwd", required=True)
parser.add_argument("--expect", type=int, default=0)
parser.add_argument("command", nargs=argparse.REMAINDER)
args = parser.parse_args()
command = args.command[1:] if args.command[:1] == ["--"] else args.command
assert command[:2] == ["rtk", "proxy"], command
out = Path(__file__).resolve().parent.parent / "receipts"
log = out / (args.name + ".log")
assert not log.exists(), log
started = datetime.now(timezone.utc).isoformat()
start = time.monotonic()
with log.open("wb") as stream:
    result = subprocess.run(command, cwd=args.cwd, stdout=stream, stderr=subprocess.STDOUT)
record = dict(command=command, shell_command=shlex.join(command), cwd=args.cwd,
              started_utc=started, seconds=round(time.monotonic()-start,3),
              returncode=result.returncode, expected_returncode=args.expect)
(out / (args.name + ".json")).write_text(json.dumps(record, indent=2)+"\n")
print(json.dumps(record), flush=True)
print("\n".join(log.read_text(errors="replace").splitlines()[-14:]), flush=True)
raise SystemExit(0 if result.returncode == args.expect else 1)
