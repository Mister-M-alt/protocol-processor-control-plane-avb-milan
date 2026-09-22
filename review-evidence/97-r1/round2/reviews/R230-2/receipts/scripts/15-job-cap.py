#!/usr/bin/env python3
"""Audit verilator-invocations.jsonl: every non-query Verilator call must carry
exactly -j 8 --build-jobs 8 --verilate-jobs 8 and no other job setting."""
import json

RC = "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r2-r230/receipts"
recs = [json.loads(l) for l in open(f"{RC}/verilator-invocations.jsonl")]
work = [r for r in recs if not r["query"]]
bad = []
original_j = set()
for r in work:
    e = r["effective"]
    js = [(e[i], e[i + 1]) for i in range(len(e) - 1)
          if e[i] in ("-j", "--build-jobs", "--verilate-jobs")]
    if sorted(js) != sorted([("-j", "8"), ("--build-jobs", "8"), ("--verilate-jobs", "8")]):
        bad.append(r)
    if any(a.startswith("--build-jobs=") or a.startswith("--verilate-jobs=") for a in e):
        bad.append(r)
    a = r["argv"]
    original_j |= {f"{a[i]} {a[i + 1]}" for i in range(len(a) - 1) if a[i] == "-j"}
print(f"invocations={len(recs)} queries={len(recs) - len(work)} work={len(work)} "
      f"work-with-exact-8-caps={len(work) - len(bad)} violations={len(bad)}")
print(f"job settings requested by the callers and replaced: {sorted(original_j)}")
print("RESULT:", "PASS" if not bad else "FAIL")
