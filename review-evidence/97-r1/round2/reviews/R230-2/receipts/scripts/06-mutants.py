#!/usr/bin/env python3
"""Apply one named edit to a copy of the exact-head fixture_guards.py.

Usage: 06-mutants.py <name> <source fixture_guards.py> <output path>
Each edit replaces an exact anchor that must occur once; otherwise it aborts
without writing. The unified diff of every mutant is printed for the receipt."""
import difflib
import sys

L_ENV = '        compiler_env = os.environ.copy()\n'
L_LC = '        compiler_env["LC_ALL"] = "C"\n'
L_RUNENV = '                env=compiler_env,\n'
L_VRUN = '            [args.verilator, *vflags, "--Mdir", tmp], check=True\n'
L_FAIL = '                return 1\n'

EDITS = {
    # Normalisation lost or made ineffective: the regression must fail.
    "N1-no-env-argument": [(L_RUNENV, "")],
    "N2-no-lc-all-override": [(L_LC, "")],
    "N3-lang-instead-of-lc-all": [(L_LC, '        compiler_env["LANG"] = "C"\n')],
    "N4-lc-messages-instead-of-lc-all": [(L_LC, '        compiler_env["LC_MESSAGES"] = "C"\n')],
    "N5-drop-inherited-environment": [(L_ENV, '        compiler_env = {}\n')],
    "N6-mutate-caller-environment": [(L_ENV, '        compiler_env = os.environ\n')],
    "N7-verilator-also-normalised": [
        (L_VRUN, '            [args.verilator, *vflags, "--Mdir", tmp], check=True,\n'
                 '            env={**os.environ, "LC_ALL": "C"},\n')],
    "N8-global-lc-all-before-verilator": [
        ('    with tempfile.TemporaryDirectory(prefix="pp-top-vid-guards-") as tmp:\n',
         '    os.environ["LC_ALL"] = "C"\n'
         '    with tempfile.TemporaryDirectory(prefix="pp-top-vid-guards-") as tmp:\n'),
        (L_RUNENV, "")],
    # Equivalent implementations: the regression should not reject them.
    "E1-dict-merge": [(L_ENV, '        compiler_env = {**os.environ, "LC_ALL": "C"}\n'),
                      (L_LC, "")],
    "E2-dict-kwargs": [(L_ENV, '        compiler_env = dict(os.environ, LC_ALL="C")\n'),
                       (L_LC, "")],
    # Outside the regression's stated scope (locale/argv plumbing): classification.
    "C1-lenient-verdict": [(L_FAIL, '                pass\n')],
}


def main():
    name, src, out = sys.argv[1:4]
    text = open(src).read()
    new = text
    for old, rep in EDITS[name]:
        if new.count(old) != 1:
            sys.exit(f"anchor not found exactly once for {name}: {old!r}")
        new = new.replace(old, rep)
    open(out, "w").write(new)
    sys.stdout.writelines(difflib.unified_diff(
        text.splitlines(True), new.splitlines(True),
        "a/tb/pp_top/fixture_guards.py", "b/tb/pp_top/fixture_guards.py"))


if __name__ == "__main__":
    main()
