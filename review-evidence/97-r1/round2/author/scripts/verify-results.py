#!/usr/bin/env python3
"""Check the factual pass/fail conditions retained by this author run."""
import json
from pathlib import Path
import re

out = Path(__file__).resolve().parent.parent
receipts = out / "receipts"
results = {}
for name in ["01-base-german", "02-base-french"]:
    meta = json.loads((receipts / (name+".json")).read_text())
    log = (receipts / (name+".log")).read_text()
    errors = [l for l in log.splitlines() if "error:" in l]
    assert meta["returncode"] == 2 and len(errors) == 2
    assert "FAIL: fixture 0002: unexpected compiler result 1" in log
    prefix = "statische Assertion fehlgeschlagen" if name.endswith("german") else "l'assertion statique a échoué"
    assert all(prefix in l for l in errors)
    results[name] = dict(returncode=2, error_lines=errors)
for name in ["03-fixed-german", "04-fixed-french"]:
    meta = json.loads((receipts / (name+".json")).read_text())
    log = (receipts / (name+".log")).read_text()
    outcomes = [l for l in log.splitlines() if l.startswith("fixture guard ")]
    assert meta["returncode"] == 0 and len(outcomes) == 4
    assert "fixture guards: 4 cases PASS" in log
    assert "Ran 1 test" in log and "\nOK\n" in log
    results[name] = dict(returncode=0, outcomes=outcomes, locale_regression="PASS")
for item in json.loads((out / "mutants/results.json").read_text()):
    name = item["name"]
    meta = json.loads((receipts / (name+".json")).read_text())
    assert meta["returncode"] == 2
    results[name] = item | dict(returncode=2)
name = "09-default-and-5a3c"
meta = json.loads((receipts / (name+".json")).read_text())
log = (receipts / (name+".log")).read_text()
assert meta["returncode"] == 0
assert re.findall(r"^\d+ checks: \d+ PASS, \d+ FAIL$", log, re.M) == ["1411 checks: 1411 PASS, 0 FAIL"]
assert "[build default, SRP_DOM_DEF_VID_P 0x0002] 1391 checks, 0 failures" in log
assert "[build fixture, SRP_DOM_DEF_VID_P 0x5a3c] 20 checks, 0 failures" in log
fixed = Path(json.loads((out / "scratch.json").read_text())["fixed"])
tally = (fixed / "tb/pp_top/obj_dir/build_tally.txt").read_text()
assert tally.splitlines() == ["1391 0", "20 0"]
(receipts / "09-build-tally.txt").write_text(tally)
results[name] = dict(returncode=0, default_checks=1391, fixture_checks=20, failures=0, canonical_tally="1411 checks: 1411 PASS, 0 FAIL")
for name in ["10-docs", "11-upc-map", "12-precommit-scope"]:
    meta = json.loads((receipts / (name+".json")).read_text())
    assert meta["returncode"] == 0
    results[name] = dict(returncode=0)
invocations = [json.loads(l) for l in (receipts / "verilator-invocations.jsonl").read_text().splitlines()]
work = []
for row in invocations:
    args = row["effective"]
    if "--getenv" in args:
        continue
    for flag in ["-j", "--build-jobs", "--verilate-jobs"]:
        assert args.count(flag) == 1 and args[args.index(flag)+1] == "8"
    work.append(row)
results["job_cap"] = dict(invocations=len(invocations), work_invocations=len(work),
                          verilator_jobs=8, build_jobs=8, verilate_jobs=8, outer_suite_make_jobs=1,
                          mutations="sequential, disposable scratch")
(out / "RESULTS.json").write_text(json.dumps(results, indent=2)+"\n")
print(json.dumps(results, indent=2))
