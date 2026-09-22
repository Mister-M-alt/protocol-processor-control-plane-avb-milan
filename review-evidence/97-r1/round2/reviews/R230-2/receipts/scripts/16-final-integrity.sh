#!/usr/bin/env bash
# Final state of the review clone: HEAD, detached, no tracked/untracked/ignored
# changes, index == HEAD tree at stage 0, every path's kind/mode/bytes equal to
# its HEAD blob, object store consistent; live refs unchanged.
set -u
R=$VALIDATION_STORAGE/reviews/r230-100-r2
cd "$R" || exit 1
echo "HEAD $(git rev-parse HEAD) tree $(git rev-parse 'HEAD^{tree}') symbolic-ref: $(git symbolic-ref -q HEAD || echo detached)"
echo "status --porcelain --ignored lines: $(git status --porcelain --ignored | wc -l)"
git status --porcelain --ignored | sed 's/^/  /'
python3 -B - <<'EOF'
import hashlib, os, stat, subprocess
tree = subprocess.check_output(["git", "ls-tree", "-r", "--full-tree", "HEAD"], text=True).splitlines()
idx = subprocess.check_output(["git", "ls-files", "-s"], text=True).splitlines()
t = {l.split("\t", 1)[1]: tuple(l.split("\t", 1)[0].split()) for l in tree}   # mode type blob
i = {l.split("\t", 1)[1]: tuple(l.split("\t", 1)[0].split()) for l in idx}    # mode blob stage
same = all(p in i and i[p][0] == t[p][0] and i[p][1] == t[p][2] and i[p][2] == "0" for p in t) and len(i) == len(t)
bad = []
modes = {}
for p, (mode, typ, blob) in t.items():
    modes[mode] = modes.get(mode, 0) + 1
    st = os.lstat(p)
    if mode == "120000":
        ok = stat.S_ISLNK(st.st_mode) and hashlib.sha1(b"blob %d\0" % len(os.readlink(p)) + os.readlink(p).encode()).hexdigest() == blob
    else:
        data = open(p, "rb").read()
        ok = (stat.S_ISREG(st.st_mode) and bool(st.st_mode & 0o100) == (mode == "100755")
              and hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest() == blob)
    if not ok:
        bad.append(p)
print(f"tree entries={len(t)} index entries={len(i)} index==HEAD tree at stage 0: {same}")
print(f"modes: {modes}; paths whose kind/exec bit/bytes differ from HEAD blob: {bad}")
EOF
echo "fsck: $(git fsck --full --no-progress 2>&1 | grep -v '^dangling' | wc -l) non-dangling messages"
echo "live refs ($(date -Is)):"
git ls-remote origin refs/heads/main refs/heads/97-assert-distinct-sr-vid-fixture refs/pull/100/head refs/pull/100/merge | sed 's/^/  /'
