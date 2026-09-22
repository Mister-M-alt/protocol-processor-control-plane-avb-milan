import hashlib
import json
from pathlib import Path
import subprocess

out = Path(__file__).resolve().parents[1]
base = "8452f564294300a82d56eed464276576f65f4d58"
def git(*args):
    return subprocess.check_output(["rtk", "proxy", "git", *args], text=True)
head = git("rev-parse", "HEAD").strip()
assert git("rev-parse", "HEAD^").strip() == base
assert git("branch", "--show-current").strip() == "97-assert-distinct-sr-vid-fixture"
assert not git("status", "--porcelain=v2", "--ignored")
message = git("show", "-s", "--format=%B", "HEAD").strip()
assert "\n" not in message and "\u2014" not in message
expected = {"tb/pp_top/Makefile", "tb/pp_top/README.md", "tb/pp_top/sim_main.cpp", "tb/pp_top/fixture_guards.py"}
assert set(git("diff", "--name-only", base, head).splitlines()) == expected
subprocess.run(["rtk", "proxy", "git", "diff", "--check", base, head], check=True)
snapshot = json.loads((out / "receipts/source-snapshot.json").read_text())
for name, sha in snapshot["changed_files_sha256"].items():
    assert hashlib.sha256(Path(name).read_bytes()).hexdigest() == sha
    assert hashlib.sha256(git("show", head+":"+name).encode()).hexdigest() == sha
invocations = [json.loads(line) for line in (out / "receipts/verilator-invocations.jsonl").read_text().splitlines()]
for item in invocations:
    assert item["argv"][-6:] == ["-j", "8", "--build-jobs", "8", "--verilate-jobs", "8"]
    assert "-j0" not in item["argv"]
for name in ("01-fixture-guards", "02-default-fixture", "04-hdl-lint", "05-refuse-0002", "05-refuse-1002", "06-missing-binding", "07-child-default", "08-remove-wire-guard", "09-remove-class-d-guard", "10-docs-final", "12-sequential-controls"):
    receipt = json.loads((out / "receipts" / (name+".json")).read_text())
    assert receipt["exit_code"] == receipt["expected_exit_code"], name
print("Committed head: " + head)
print("One-line commit without trailer: " + message)
print("Scope: four pp_top files only; parent is assigned base")
print("Clean source including ignored files: PASS")
print("Committed content matches tested snapshot hashes: PASS")
print(f"Explicit Verilator job caps: PASS ({len(invocations)} calls)")
print("All expected test/control exits verified: PASS")
