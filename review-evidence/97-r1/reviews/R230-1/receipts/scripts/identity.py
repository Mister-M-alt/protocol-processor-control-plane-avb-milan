#!/usr/bin/env python3
# R230 identity and evidence-integrity checks (read-only on the review clone).
# identity.py REVIEW_CLONE EVIDENCE_REPO
#   REVIEW_CLONE  the detached review checkout
#   EVIDENCE_REPO a scratch repository that has fetched evidence commits
#                 77e2fb5 and 8b61b6c (never the review clone)
import hashlib
import json
import subprocess
import sys

HEAD = "1eb20dc4911880de10b745cc284e7dde306788b5"
BASE = "8452f564294300a82d56eed464276576f65f4d58"
TREE = "001d20069951396c5e056c5539c2af4217310a57"
EV_FINAL = "77e2fb5fdfbb4c02ed7335366733b41c279018cf"
EV_AUTHOR = "8b61b6cf8a6bd445ee5d54103813e5d72d04af98"
PREFIX = "review-evidence/97-r1/"
fails = []


def git(repo, *args, text=True):
    return subprocess.run(["git", "-C", repo, *args], check=True,
                          capture_output=True, text=text).stdout


def check(ok, what):
    print(("OK   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


def main():
    clone, ev = sys.argv[1], sys.argv[2]
    print("== review clone", clone)
    check(git(clone, "rev-parse", "HEAD").strip() == HEAD, "HEAD is " + HEAD)
    check(git(clone, "rev-parse", "HEAD^{tree}").strip() == TREE, "HEAD tree is " + TREE)
    check(git(clone, "rev-parse", "HEAD^@").split() == [BASE], "single parent is base " + BASE)
    check(git(clone, "merge-base", BASE, HEAD).strip() == BASE, "merge-base(base, head) is base")
    status = git(clone, "status", "--porcelain=v2", "--branch", "--ignored")
    print(status, end="")
    check("# branch.head (detached)" in status, "HEAD detached")
    check(not [l for l in status.splitlines() if not l.startswith("#")],
          "no tracked, untracked or ignored changes in the review clone")
    names = git(clone, "diff", "--name-status", BASE, HEAD)
    print(names, end="")
    check(sorted(names.split("\n")[:-1]) == sorted([
        "M\ttb/pp_top/Makefile", "M\ttb/pp_top/README.md",
        "A\ttb/pp_top/fixture_guards.py", "M\ttb/pp_top/sim_main.cpp"]),
        "exactly the four tb/pp_top files change")
    summary = git(clone, "diff", "--summary", BASE, HEAD)
    print(summary, end="")
    check(summary.strip() == "create mode 100644 tb/pp_top/fixture_guards.py",
          "only summary line is the new 100644 file (no mode change, delete or rename)")
    dc = subprocess.run(["git", "-C", clone, "diff", "--check", BASE, HEAD],
                        capture_output=True, text=True)
    check(dc.returncode == 0 and dc.stdout == "", "git diff --check base..head clean")
    for path in ("hdl", "docs", "scripts", ".github", "syn", "tb/pp_top/pp_top_wrap.sv"):
        check(git(clone, "diff", "--quiet", BASE, HEAD, "--", path) == "",
              f"no change under {path}")
    stat = git(clone, "diff", "--numstat", BASE, HEAD)
    print(stat, end="")
    ls = git(clone, "ls-files", "-s", "tb/pp_top")
    print(ls, end="")
    check(all(l.split()[0] == "100644" for l in ls.splitlines()), "all tb/pp_top files are 100644")

    print("== author source snapshot and patch vs head blobs")
    snap = json.loads(git(ev, "show", EV_FINAL + ":" + PREFIX + "author/receipts/source-snapshot.json"))
    for path, digest in snap["changed_files_sha256"].items():
        blob = subprocess.run(["git", "-C", clone, "show", HEAD + ":" + path],
                              check=True, capture_output=True).stdout
        check(hashlib.sha256(blob).hexdigest() == digest, f"snapshot sha256 matches head {path}")
    patch = subprocess.run(["git", "-C", ev, "show", EV_FINAL + ":" + PREFIX + "author/source.patch"],
                           check=True, capture_output=True).stdout
    diff = subprocess.run(["git", "-C", clone, "diff", BASE, HEAD], check=True,
                          capture_output=True).stdout
    check(patch == diff, "author source.patch is byte-identical to git diff base..head")

    print("== evidence commits")
    for c, parent in ((EV_AUTHOR, BASE), (EV_FINAL, EV_AUTHOR)):
        check(git(ev, "rev-parse", c + "^@").split() == [parent], f"{c[:7]} parent is {parent[:7]}")
    outside = [l for l in git(ev, "diff", "--name-only", BASE, EV_FINAL).splitlines()
               if not l.startswith(PREFIX)]
    check(outside == [], "evidence commits touch only " + PREFIX)
    files = git(ev, "ls-tree", "-r", "--name-only", EV_FINAL, PREFIX).splitlines()
    manifest = json.loads(git(ev, "show", EV_FINAL + ":" + PREFIX + "MANIFEST.json"))
    listed = {PREFIX + e["file"] for e in manifest}
    bad = []
    for e in manifest:
        blob = subprocess.run(["git", "-C", ev, "show", EV_FINAL + ":" + PREFIX + e["file"]],
                              check=True, capture_output=True).stdout
        if hashlib.sha256(blob).hexdigest() != e["published_sha256"]:
            bad.append(e["file"])
        if (e["original_sha256"] != e["published_sha256"]) != e["path_redacted"]:
            bad.append(e["file"] + " (path_redacted flag)")
    check(not bad, f"all {len(manifest)} MANIFEST entries match published sha256 and flags {bad}")
    unlisted = sorted(set(files) - listed)
    print("files not in MANIFEST:", unlisted)
    check(set(unlisted) <= {PREFIX + "MANIFEST.json", PREFIX + "README.md"},
          "only MANIFEST.json/README.md are outside the manifest")
    cand = json.loads(git(ev, "show", EV_FINAL + ":" + PREFIX + "manager/candidate.json"))
    check(cand == {"base": BASE, "head": HEAD, "head_tree": TREE, "candidate_tree": TREE, "clean": True},
          "manager candidate.json names this base/head/tree")
    res = json.loads(git(ev, "show", EV_FINAL + ":" + PREFIX + "manager/full-native/results.json"))
    check(res["head"] == HEAD and res["base"] == BASE and len(res["results"]) == 9
          and all(r["exit_code"] == 0 for r in res["results"]),
          "manager full-native results: 9 commands, all exit 0, this head")
    print("fails:", len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
