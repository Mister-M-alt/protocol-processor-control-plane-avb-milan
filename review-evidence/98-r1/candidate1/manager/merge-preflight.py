#!/usr/bin/env python3
"""Read-only corrected-head PP101 preflight. Never merges or changes metadata."""
import argparse
import datetime
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--r233-report", required=True)
parser.add_argument("--r234-report", required=True)
args = parser.parse_args()
REPO = "Mister-M-alt/protocol-processor-control-plane-avb-milan"
HEAD = "bc997e7c00e50e3d59b97987950bfbf6cc442182"
CANDIDATE = "1fd06bf6e40d2290adf1987b51c1fabe30671098"
OLD_BASE = "8452f564294300a82d56eed464276576f65f4d58"
BASE = "c8214cfe827fb3fb50c9bfee415468a24b7c27b1"
TREE = "37a5cd807c5e8a289a7149892c1a1762dcd8279c"
CWD = "$VALIDATION"
ROOT = Path("$VALIDATION_STORAGE/donor98-manager-candidate1")
MANAGER = Path("$WORKSPACE_HOME/milan-fpga-management")

def command(*argv):
    return subprocess.check_output(argv, cwd=CWD, text=True).strip()

def github(*argv):
    return json.loads(command("gh", *argv, "--repo", REPO))

complete = json.loads((ROOT / "full-native/complete.json").read_text())
results = json.loads((ROOT / "full-native/results.json").read_text())
assert complete["head"] == results["head"] == CANDIDATE
assert results["base"] == BASE and complete["exit_code"] == 0
assert len(results["results"]) == 9
assert all(row["exit_code"] == 0 for row in results["results"])
assert command("git", "rev-parse", "HEAD") == CANDIDATE
assert not command("git", "status", "--porcelain=v1")
assert command("git", "ls-remote", "origin", "refs/heads/main").split()[0] == BASE
assert command("git", "diff", "--binary", OLD_BASE, HEAD) == command("git", "diff", "--binary", BASE, CANDIDATE)
assert command("git", "rev-parse", CANDIDATE + "^{tree}") == TREE
assert command("git", "merge-tree", "--write-tree", "--no-messages", BASE, HEAD) == TREE
integrity = json.loads((ROOT / "final-integrity.json").read_text())
assert integrity["head"] == CANDIDATE and integrity["tree"] == TREE and integrity["result"] == "PASS"
assert command("git", "ls-remote", "origin", "refs/heads/98-clarify-srp-domain-events").split()[0] == HEAD
pr = github("pr", "view", "101", "--json", "headRefOid,isDraft,state,mergeable,mergeStateStatus,statusCheckRollup")
assert pr["headRefOid"] == HEAD and not pr["isDraft"]
assert pr["state"] == "OPEN" and pr["mergeable"] == "MERGEABLE"
assert pr["mergeStateStatus"] == "CLEAN", pr["mergeStateStatus"]
checks = pr["statusCheckRollup"]
assert len(checks) == 6
assert {c["name"] for c in checks} == {"docs-gates", "suites", "portability"}
assert all(c["conclusion"] == "SUCCESS" for c in checks)
run_ids = {c["detailsUrl"].split("/runs/")[1].split("/")[0] for c in checks}
assert len(run_ids) == 2
runs = [github("run", "view", number, "--json", "headSha,event,status,conclusion,jobs,url") for number in sorted(run_ids)]
assert {run["event"] for run in runs} == {"push", "pull_request"}
for run in runs:
    assert run["headSha"] == HEAD and run["status"] == "completed" and run["conclusion"] == "success"
    assert {job["name"] for job in run["jobs"]} == {"docs-gates", "suites", "portability"}
    assert all(job["conclusion"] == "success" for job in run["jobs"])
pages = json.loads(command("gh", "api", "--paginate", "--slurp", f"repos/{REPO}/issues/101/comments"))
comments = [comment for page in pages for comment in page]
reviews = {}
for who, day, report_path in (("R233", "2026-09-19", args.r233_report), ("R234", "2026-09-21", args.r234_report)):
    matching = [c for c in comments if c["body"].startswith(f"[{who}]")]
    assert matching
    current = matching[-1]
    assert current["body"].splitlines()[0] == f"[{who}] POSITIVE - exact head {HEAD}"
    assert current["body"].strip() == Path(report_path).read_text().strip()
    key = f"101-r1-{who.lower()}"
    execution = json.loads((MANAGER / day / (key + "-execution.json")).read_text())
    assert execution["exit_code"] == 0 and execution.get("is_error") is False
    assert not execution.get("credit_exhausted", False)
    assert subprocess.run(["systemctl", "--user", "is-active", "--quiet", "milan-" + key]).returncode != 0
    reviews[who] = {"comment": current, "execution": execution}
result = {"checked_at": datetime.datetime.now().astimezone().isoformat(), "head": HEAD, "base": BASE, "tree": TREE, "candidate_commit": CANDIDATE, "old_hosted_base": OLD_BASE,
          "native": {"complete": complete, "results": results}, "pr": pr, "runs": runs, "reviews": reviews,
          "merge_performed": False, "authorization": "Standing automatic merge after two independent positives and complete gates",
          "limits": "Manager must verify reviewer-owned clean five-lens ledgers, no findings and no other review in flight. Historical PP continuity/negative-merge audit findings remain a disclosed nonzero baseline; current PR containment must be clean."}
(ROOT / "merge-preflight.json").write_text(json.dumps(result, indent=2) + "\n")
print(f"Mechanical preflight passed: {HEAD}; base {BASE}; candidate tree {TREE}. No merge performed.")
