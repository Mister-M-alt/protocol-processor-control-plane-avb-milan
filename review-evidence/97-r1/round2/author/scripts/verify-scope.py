#!/usr/bin/env python3
"""Verify exact correction scope and untouched file bytes/modes."""
import ast
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile

out = Path(__file__).resolve().parent.parent
root = Path("$CANDIDATE")
phase = sys.argv[1]
base = "1eb20dc4911880de10b745cc284e7dde306788b5"
main = "8452f564294300a82d56eed464276576f65f4d58"
modified = ["tb/pp_top/Makefile", "tb/pp_top/README.md", "tb/pp_top/fixture_guards.py"]
added = ["tb/pp_top/test_fixture_guards.py"]
def git(*args):
    return subprocess.check_output(["rtk", "proxy", "git", *args], cwd=root)
def snapshot():
    entries = {}
    for base_dir, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d != ".git")
        for name in sorted(files):
            path = Path(base_dir) / name
            if path.name == ".git":
                continue
            data = os.readlink(path).encode() if path.is_symlink() else path.read_bytes()
            entries[str(path.relative_to(root))] = dict(
                mode=oct(stat.S_IMODE(path.lstat().st_mode)),
                kind="symlink" if path.is_symlink() else "file",
                bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    return entries
initial = json.loads((out / "snapshots/initial-files.json").read_text())
current = snapshot()
assert sorted(set(current) - set(initial)) == added
assert not (set(initial) - set(current))
assert sorted(k for k in initial if initial[k] != current[k]) == modified
assert all(current[k]["mode"] == initial[k]["mode"] for k in initial)
assert current[added[0]]["mode"] == "0o644"
fixed = Path(json.loads((out / "scratch.json").read_text())["fixed"])
for rel in current:
    path = fixed / rel
    assert path.read_bytes() == (root / rel).read_bytes(), rel
    assert stat.S_IMODE(path.stat().st_mode) == stat.S_IMODE((root / rel).stat().st_mode), rel
fixture = (root / "tb/pp_top/fixture_guards.py").read_text()
for line in ["import os\n", "        # The diagnostic checks below require the compiler's English wording.\n",
             "        compiler_env = os.environ.copy()\n", '        compiler_env["LC_ALL"] = "C"\n',
             "                env=compiler_env,\n"]:
    assert fixture.count(line) == 1
    fixture = fixture.replace(line, "")
assert fixture.encode() == git("show", base+":tb/pp_top/fixture_guards.py")
for rel in ["tb/pp_top/fixture_guards.py", *added]:
    ast.parse((root / rel).read_text(), filename=rel)
patch = git("diff", base, "--", *modified)
new_em_dash = [line for line in patch.decode().splitlines() if line.startswith("+") and "\u2014" in line]
assert not new_em_dash
assert "\u2014" not in (root / added[0]).read_text()
assert not git("diff", "--check")
assert not git("diff", "--cached", "--check")
untouched = sorted(k for k in initial if k not in modified)
protected = [k for k in untouched if k.startswith(("hdl/", "docs/", "scripts/", ".github/", "syn/"))
             or k in ("tb/pp_top/sim_main.cpp", "tb/pp_top/pp_top_wrap.sv")]
head, tree = git("rev-parse", "HEAD", "HEAD^{tree}").decode().splitlines()
record = dict(phase=phase, head=head, tree=tree, correction_base=base, main_base=main,
              modified=modified, added=added, changed_modes=[], untouched_files=untouched,
              untouched_count=len(untouched), protected_files=protected,
              scratch_source_matches=True, original_guard_logic_byte_identical_after_removing_locale_lines=True,
              python_syntax="PASS", diff_check="PASS", new_em_dash_count=0)
if phase == "final":
    assert git("rev-parse", "HEAD^").decode().strip() == base
    assert not git("status", "--porcelain=v1", "--untracked-files=all")
    assert not git("ls-files", "--others", "--ignored", "--exclude-standard")
    changed = git("diff", "--name-only", base, "HEAD").decode().splitlines()
    assert sorted(changed) == sorted(modified+added)
    archive = git("archive", "HEAD")
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        files = {m.name:m for m in tar.getmembers() if m.isfile()}
        assert set(files) == set(current)
        for rel, member in files.items():
            assert tar.extractfile(member).read() == (root / rel).read_bytes(), rel
            assert bool(member.mode & 0o111) == bool((root / rel).stat().st_mode & 0o111), rel
    record["committed_tree_bytes_and_executable_modes_match"] = True
    record["clean_including_ignored"] = True
    (out / "source.patch").write_bytes(git("diff", base, "HEAD"))
    (out / "source-vs-main.patch").write_bytes(git("diff", main, "HEAD"))
    (out / "snapshots/final-tree.txt").write_bytes(git("ls-tree", "-r", "HEAD"))
    (out / "snapshots/final-index.txt").write_bytes(git("ls-files", "--stage"))
    (out / "snapshots/final-status.txt").write_bytes(git("status", "--porcelain=v1", "--untracked-files=all"))
(out / ("snapshots/"+phase+"-files.json")).write_text(json.dumps(current, indent=2)+"\n")
(out / ("receipts/"+phase+"-scope.json")).write_text(json.dumps(record, indent=2)+"\n")
print(json.dumps({k:v for k,v in record.items() if k not in ("untouched_files", "protected_files")}, indent=2))
