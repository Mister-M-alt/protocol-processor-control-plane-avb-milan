#!/usr/bin/env python3
"""Analyse the 04 captures: compiler vs Verilator environment, caller inputs,
argv and diagnostic bytes across caller settings."""
import glob
import hashlib
import json
import os
import re

RC = "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r2-r230/receipts"
S = "/tmp/r230-100-r2-scratch"
CALLER_KEYS = ("LANG", "LANGUAGE", "LC_MESSAGES", "LOCPATH", "CPATH", "CPLUS_INCLUDE_PATH",
               "R230_MARKER", "SOURCE_DATE_EPOCH", "PATH", "HOME", "TMPDIR")
ok = True
outputs = {}
argvs = {}
for k in ("EN", "FR_LANGUAGE", "DE_LC_ALL", "MIXED"):
    d = f"{S}/capture/{k}"
    caller = json.load(open(f"{RC}/04-capture-{k}.json"))["env"]
    venvs = []
    for p in sorted(glob.glob(f"{d}/venv/*.env")):
        env = dict(line.split("=", 1) for line in open(p).read().splitlines() if "=" in line)
        venvs.append(env)
    recs = [json.load(open(p)) for p in sorted(glob.glob(f"{d}/cxx/*.json"))]
    print(f"== {k}: verilator calls={len(venvs)} compiler calls={len(recs)} "
          f"caller LC_ALL={caller.get('LC_ALL')!r}")
    # Both Verilator calls (--getenv query and model generation) see one environment.
    same_v = all(v == venvs[0] for v in venvs)
    print(f"   verilator environments identical across its calls: {same_v}")
    ok &= same_v and len(venvs) == 2 and len(recs) == 4
    ven = venvs[0]
    print(f"   verilator LC_ALL={ven.get('LC_ALL')!r} (caller {caller.get('LC_ALL')!r}): "
          f"{ven.get('LC_ALL') == caller.get('LC_ALL')}")
    ok &= ven.get("LC_ALL") == caller.get("LC_ALL")
    for i, r in enumerate(recs):
        env = r["env"]
        diff = sorted(set(env) ^ set(ven) | {x for x in set(env) & set(ven) if env[x] != ven[x]})
        good = env.get("LC_ALL") == "C" and diff == ["LC_ALL"]
        preserved = all(env.get(x) == caller.get(x) for x in CALLER_KEYS)
        print(f"   compiler call {i}: rc={r['rc']} LC_ALL={env.get('LC_ALL')!r} "
              f"keys differing from Verilator env={diff} caller inputs preserved={preserved}")
        ok &= good and preserved
        out = open(f"{d}/cxx/{i:02d}.out", "rb").read()
        tmp = next(a[2:] for a in r["argv"] if a.startswith("-I") and "pp-top-vid-guards-" in a)
        norm = out.replace(tmp.encode(), b"<TMP>")
        outputs.setdefault(i, {})[k] = norm
        argvs.setdefault(i, {})[k] = [("-I<TMP>" if a == "-I" + tmp else a) for a in r["argv"]]
print("== argv per case identical across settings (temp dir normalised):")
for i, per in sorted(argvs.items()):
    same = all(v == per["EN"] for v in per.values())
    ok &= same
    print(f"   case {i}: {same}  {' '.join(per['EN'])}")
print("== diagnostic bytes per case identical across settings (temp dir normalised):")
for i, per in sorted(outputs.items()):
    same = all(v == per["EN"] for v in per.values())
    ok &= same
    errs = [ln for ln in per["EN"].decode().splitlines() if "error:" in ln]
    print(f"   case {i}: identical={same} sha256={hashlib.sha256(per['EN']).hexdigest()[:16]} "
          f"bytes={len(per['EN'])} non-ascii={any(b > 127 for b in per['EN'])}")
    for e in errs:
        print(f"      {e}")
print("RESULT:", "PASS" if ok else "FAIL")
