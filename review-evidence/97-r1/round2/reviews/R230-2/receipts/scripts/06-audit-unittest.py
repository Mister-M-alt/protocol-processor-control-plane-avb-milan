#!/usr/bin/env python3
"""Run test_fixture_guards under a CPython audit hook that records every real
process creation (subprocess.Popen, os.exec*, os.posix_spawn, os.system,
os.fork). Proves the regression starts no compiler or Verilator process.
Run from a directory holding fixture_guards.py and test_fixture_guards.py."""
import os
import sys
import unittest

events = []
WATCH = {"subprocess.Popen", "os.exec", "os.posix_spawn", "os.spawn", "os.system",
         "os.fork", "os.forkpty", "os.startfile"}


def hook(event, args):
    if event in WATCH:
        events.append((event, repr(args)[:200]))


sys.dont_write_bytecode = True
sys.path.insert(0, os.getcwd())
sys.addaudithook(hook)
prog = unittest.main(module="test_fixture_guards", argv=["audit", "-v"], exit=False)
ok = prog.result.wasSuccessful() and prog.result.testsRun == 1
print(f"tests run={prog.result.testsRun} successful={prog.result.wasSuccessful()} "
      f"real process creations observed={len(events)}")
for e in events:
    print("  ", e)
print("RESULT:", "PASS" if ok and not events else "FAIL")
sys.exit(0 if ok and not events else 1)
