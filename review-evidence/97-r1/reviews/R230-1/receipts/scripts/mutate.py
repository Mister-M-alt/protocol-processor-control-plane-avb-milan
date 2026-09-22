#!/usr/bin/env python3
# R230 scratch-copy mutator: mutate.py REPO COMMIT DEST NAME
# Exports COMMIT of REPO with `git archive` into DEST (which must not exist),
# applies the named one-site edit below, refuses unless the old text occurs
# exactly once, and prints the unified diff plus before/after sha256. The
# review clone is only read. NAME "none" exports without an edit.
import difflib
import hashlib
import subprocess
import sys
from pathlib import Path

WIRE = ('static_assert(SRP_DEF_VID != 2,\n'
        '              "SRP VID fixture must differ from product default 2 in the 16-bit wire value");\n')
CLASS_D = ('static_assert((SRP_DEF_VID & 0x0FFFu) != 2,\n'
           '              "SRP VID fixture must differ from product default 2 in the 12-bit class-D value");\n')
W_MSG = '"SRP VID fixture must differ from product default 2 in the 16-bit wire value"'
D_MSG = '"SRP VID fixture must differ from product default 2 in the 12-bit class-D value"'
SIM = "tb/pp_top/sim_main.cpp"
TOP = "hdl/top/protocol_processor_top.sv"
SRPTOP = "hdl/srp/KL_srp_top.sv"

MUTANTS = {
    # the two guards, one at a time
    "G1-remove-wire-guard": (SIM, WIRE, ""),
    "G2-remove-class-d-guard": (SIM, CLASS_D, ""),
    # the class-D guard collapsed onto the wire width
    "G3-class-d-mask-16bit": (SIM, "(SRP_DEF_VID & 0x0FFFu) != 2", "(SRP_DEF_VID & 0xFFFFu) != 2"),
    # the two diagnostics exchanged between the guards
    "G4-swap-diagnostics": (SIM, WIRE + CLASS_D,
                            WIRE.replace(W_MSG, D_MSG) + CLASS_D.replace(D_MSG, W_MSG)),
    # both guards also applied to the no-override (product default) build
    "G5-guards-outside-fixture-branch": (
        SIM,
        "#ifdef PP_TOP_SRP_DOM_DEF_VID\nconstexpr uint16_t    SRP_DEF_VID = PP_TOP_SRP_DOM_DEF_VID;\n"
        "//! A default-equivalent fixture cannot expose a dropped binding (issue #97).\n" + WIRE + CLASS_D +
        "#else\nconstexpr uint16_t    SRP_DEF_VID = 2;\n#endif\n",
        "#ifdef PP_TOP_SRP_DOM_DEF_VID\nconstexpr uint16_t    SRP_DEF_VID = PP_TOP_SRP_DOM_DEF_VID;\n"
        "#else\nconstexpr uint16_t    SRP_DEF_VID = 2;\n#endif\n"
        "//! A default-equivalent fixture cannot expose a dropped binding (issue #97).\n" + WIRE + CLASS_D),
    # an unrelated compile error inside the fixture branch only
    "G6-unrelated-error-in-fixture-branch": (
        SIM, CLASS_D + "#else\n", CLASS_D + "constexpr int r230_unrelated = undeclared_r230;\n#else\n"),
    # issue #95 controls (README M25, M31)
    "M25-binding-removed": (TOP, "      .DOM_DEF_VID_P    (SRP_DOM_DEF_VID_P),\n", ""),
    "M31-child-default-7": (SRPTOP, "DOM_DEF_VID_P  = 16'd2", "DOM_DEF_VID_P  = 16'd7"),
}


def main():
    repo, commit, dest, name = sys.argv[1:5]
    dest = Path(dest)
    if dest.exists():
        raise SystemExit(f"refusing: {dest} exists")
    dest.mkdir(parents=True)
    tar = subprocess.run(["git", "-C", repo, "archive", commit], check=True, capture_output=True).stdout
    subprocess.run(["tar", "-x", "-C", str(dest)], input=tar, check=True)
    print(f"exported {commit} of {repo} to {dest}")
    if name == "none":
        return 0
    path, old, new = MUTANTS[name]
    f = dest / path
    text = f.read_text()
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"refusing: old text occurs {n} times in {path}")
    before = hashlib.sha256(text.encode()).hexdigest()
    mutated = text.replace(old, new)
    f.write_text(mutated)
    after = hashlib.sha256(mutated.encode()).hexdigest()
    sys.stdout.writelines(difflib.unified_diff(text.splitlines(True), mutated.splitlines(True),
                                               "a/" + path, "b/" + path))
    print(f"mutant {name}: {path} sha256 {before} -> {after}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
