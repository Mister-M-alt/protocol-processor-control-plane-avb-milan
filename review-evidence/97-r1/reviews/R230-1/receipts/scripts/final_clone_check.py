#!/usr/bin/env python3
# R230 final state of the review clone (read-only): HEAD, cleanliness, index
# entries == HEAD tree entries (mode, object, path, stage 0), on-disk kind and
# executable bit per mode, blob content per path, and object-store fsck.
# final_clone_check.py REVIEW_CLONE
import hashlib
import os
import stat
import subprocess
import sys

HEAD = "1eb20dc4911880de10b745cc284e7dde306788b5"
TREE = "001d20069951396c5e056c5539c2af4217310a57"


def git(repo, *a):
    return subprocess.run(["git", "-C", repo, *a], check=True, capture_output=True).stdout


def main():
    clone = sys.argv[1]
    fails = []

    def check(ok, what):
        print(("OK   " if ok else "FAIL ") + what)
        if not ok:
            fails.append(what)

    check(git(clone, "rev-parse", "HEAD").decode().strip() == HEAD, "HEAD " + HEAD)
    check(git(clone, "rev-parse", "HEAD^{tree}").decode().strip() == TREE, "tree " + TREE)
    st = git(clone, "status", "--porcelain=v2", "--branch", "--ignored", "--untracked-files=all").decode()
    print(st, end="")
    check("# branch.head (detached)" in st, "detached HEAD")
    check(not [l for l in st.splitlines() if not l.startswith("#")], "no modified, staged, untracked or ignored path")
    tree = {}
    for rec in git(clone, "ls-tree", "-r", "-z", "HEAD").split(b"\0"):
        if rec:
            meta, path = rec.split(b"\t", 1)
            mode, _typ, obj = meta.split()
            tree[path] = (mode, obj)
    index = {}
    stages = set()
    for rec in git(clone, "ls-files", "-s", "-z").split(b"\0"):
        if rec:
            meta, path = rec.split(b"\t", 1)
            mode, obj, stage = meta.split()
            index[path] = (mode, obj)
            stages.add(stage)
    check(stages == {b"0"}, "index has only stage-0 entries")
    check(index == tree, f"index entries == HEAD tree entries ({len(index)} paths, mode+object+path)")
    kinds = {b"100644": 0, b"100755": 0, b"120000": 0, b"160000": 0}
    bad = []
    for path, (mode, obj) in tree.items():
        kinds[mode] = kinds.get(mode, 0) + 1
        p = os.path.join(clone, path.decode())
        s = os.lstat(p)
        if mode == b"120000":
            ok = stat.S_ISLNK(s.st_mode) and hashlib.sha1(
                b"blob %d\0" % len(os.readlink(p).encode()) + os.readlink(p).encode()).hexdigest() == obj.decode()
        elif mode in (b"100644", b"100755"):
            data = open(p, "rb").read()
            ok = (stat.S_ISREG(s.st_mode)
                  and bool(s.st_mode & stat.S_IXUSR) == (mode == b"100755")
                  and hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest() == obj.decode())
        else:
            ok = stat.S_ISDIR(s.st_mode)
        if not ok:
            bad.append(path.decode())
    print("tracked kinds:", {k.decode(): v for k, v in kinds.items() if v})
    check(not bad, f"every tracked path has its mode's kind/exec bit and its HEAD blob bytes {bad[:5]}")
    fsck = subprocess.run(["git", "-C", clone, "fsck", "--full", "--no-progress", "--no-dangling"],
                          capture_output=True, text=True)
    print(fsck.stdout + fsck.stderr, end="")
    check(fsck.returncode == 0, "git fsck --full clean")
    print("fails:", len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
