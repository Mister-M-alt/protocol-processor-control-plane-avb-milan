#!/usr/bin/env python3
"""[A165] donor #98: cite the existing public DV5/DV6 evidence exactly.

Read-only. Reads blobs from the public evidence commit (branch
origin/96-review-evidence) in the lane clone, hashes each cited file, checks
the hash against the published MANIFEST.json (and, for R223 receipts, the
reviewer's receipts/SHA256SUMS), and prints the cited lines.
"""
import hashlib
import json
import re
import subprocess
import sys

EVID = "29b2066b91cf4b0387edff43250b8fe79192232b"
ROOT = "review-evidence/96-r1"


def blob(path: str) -> bytes:
    return subprocess.run(["git", "show", f"{EVID}:{ROOT}/{path}"],
                          check=True, capture_output=True).stdout


manifest = {e["file"]: e for e in json.loads(blob("MANIFEST.json"))}
sums = {}
for line in blob("reviews/R223-1/receipts/SHA256SUMS").decode().splitlines():
    h, _, name = line.partition("  ")
    sums[name.strip().lstrip("*").removeprefix("./")] = h

CITED = [
    ("reviews/R223-1/receipts/02-head-pp_top-both-builds.log",
     r"^\[build (default|fixture)|^\d+ checks: "),
    ("reviews/R223-1/receipts/mutations/R36_child_revert_literal/mutation.diff",
     r"^[-+] "),
    ("reviews/R223-1/receipts/mutations/R36_child_revert_literal/run_fixture.log",
     r"FAIL|checks"),
    ("reviews/R223-1/receipts/mutations/R37_child_linkup_decl_literal/mutation.diff",
     r"^[-+] "),
    ("reviews/R223-1/receipts/mutations/R37_child_linkup_decl_literal/run_fixture.log",
     r"FAIL|checks"),
    ("manager/postmerge/hosted-main.log",
     r"PASS (pp_top|srp_encoder|srp_top) |suites: \d+ checks|Run actions/checkout@v4.*Z 8452f564294300a82d56eed464276576f65f4d58$"),
]

ok = True
print(f"evidence commit {EVID} (origin/96-review-evidence), root {ROOT}/")
for path, pat in CITED:
    data = blob(path)
    h = hashlib.sha256(data).hexdigest()
    m = manifest.get(path)
    man = m["published_sha256"] if m else None
    rel = path.removeprefix("reviews/R223-1/receipts/")
    s = sums.get(rel) if path.startswith("reviews/R223-1/receipts/") else None
    verdict = []
    if man is not None:
        verdict.append("MANIFEST match" if man == h else "MANIFEST MISMATCH")
        ok &= man == h
    else:
        verdict.append("not in MANIFEST")
    if s is not None:
        verdict.append("SHA256SUMS match" if s == h else "SHA256SUMS MISMATCH")
        ok &= s == h
    print(f"\n== {path}\n   sha256 {h} ({', '.join(verdict)})")
    for n, line in enumerate(data.decode(errors="replace").splitlines(), 1):
        if re.search(pat, line):
            print(f"   {n:5d}: {line}")
print("\nall cited hashes consistent" if ok else "\nHASH INCONSISTENCY")
sys.exit(0 if ok else 1)
