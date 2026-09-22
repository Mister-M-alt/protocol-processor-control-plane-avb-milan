import hashlib
import json
from pathlib import Path
import subprocess

out = Path(__file__).resolve().parents[1]
subprocess.run(["rtk", "proxy", "git", "diff", "--check"], check=True)
source = Path("tb/pp_top/fixture_guards.py")
compile(source.read_text(), str(source), "exec")
print("Python syntax: PASS")
diff = subprocess.check_output(["rtk", "proxy", "git", "diff", "--unified=0"], text=True)
added = "\n".join(line[1:] for line in diff.splitlines() if line.startswith("+") and not line.startswith("+++"))
added += source.read_text()
assert "\u2014" not in added
print("Added text: no new U+2014")
changed = subprocess.check_output(["rtk", "proxy", "git", "diff", "--name-only"], text=True).splitlines()
changed += subprocess.check_output(["rtk", "proxy", "git", "ls-files", "--others", "--exclude-standard"], text=True).splitlines()
expected = {"tb/pp_top/Makefile", "tb/pp_top/README.md", "tb/pp_top/sim_main.cpp", "tb/pp_top/fixture_guards.py"}
assert set(changed) == expected, changed
print("Scope: exactly the four pp_top test/documentation files")
snapshot = json.loads((out / "receipts/source-snapshot.json").read_text())
for name, sha in snapshot["changed_files_sha256"].items():
    assert hashlib.sha256(Path(name).read_bytes()).hexdigest() == sha, name
print("Source matches the tested scratch snapshot")
invocations = [json.loads(line) for line in (out / "receipts/verilator-invocations.jsonl").read_text().splitlines()]
for item in invocations:
    assert item["argv"][-6:] == ["-j", "8", "--build-jobs", "8", "--verilate-jobs", "8"]
    assert "-j0" not in item["argv"]
print(f"Verilator invocation cap: PASS ({len(invocations)} calls)")
