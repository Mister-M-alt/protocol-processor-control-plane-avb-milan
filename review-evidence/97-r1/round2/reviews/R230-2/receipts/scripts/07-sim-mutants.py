#!/usr/bin/env python3
"""Apply one named edit to a copy of the exact-head tb/pp_top/sim_main.cpp.

Usage: 07-sim-mutants.py <name> <source sim_main.cpp> <output path>
Anchors must occur exactly once; the unified diff is printed for the receipt."""
import difflib
import sys

WIRE = ('static_assert(SRP_DEF_VID != 2,\n'
        '              "SRP VID fixture must differ from product default 2 in the 16-bit wire value");\n')
CLSD = ('static_assert((SRP_DEF_VID & 0x0FFFu) != 2,\n'
        '              "SRP VID fixture must differ from product default 2 in the 12-bit class-D value");\n')
ELSE = '#else\nconstexpr uint16_t    SRP_DEF_VID = 2;\n#endif\n'
MW = "SRP VID fixture must differ from product default 2 in the 16-bit wire value"
MC = "SRP VID fixture must differ from product default 2 in the 12-bit class-D value"

EDITS = {
    "G1-wire-guard-removed": [(WIRE, "")],
    "G2-class-d-guard-removed": [(CLSD, "")],
    "G3-class-d-mask-widened": [("(SRP_DEF_VID & 0x0FFFu) != 2", "(SRP_DEF_VID & 0xFFFFu) != 2")],
    "G4-diagnostics-swapped": [(WIRE + CLSD, WIRE.replace(MW, MC) + CLSD.replace(MC, MW))],
    "G5-guards-outside-fixture-branch": [(WIRE + CLSD, ""), (ELSE, ELSE + WIRE + CLSD)],
    "G6-unrelated-error-all-fixtures": [(CLSD, CLSD + "#error R230 unrelated compiler error control\n")],
    "G7-unrelated-error-0002-alongside": [(CLSD, CLSD + "#if PP_TOP_SRP_DOM_DEF_VID == 0x0002\n"
                                          "#error R230 unrelated compiler error control\n#endif\n")],
    "G8-unrelated-error-default-branch": [(ELSE, "#else\nconstexpr uint16_t    SRP_DEF_VID = 2;\n"
                                           "int r230_unrelated = r230_undeclared;\n#endif\n")],
    "G9-extra-static-assertion-0002": [(CLSD, CLSD + 'static_assert(SRP_DEF_VID != 2, '
                                        '"R230 unrelated static assertion text");\n')],
}


def main():
    name, src, out = sys.argv[1:4]
    text = open(src).read()
    new = text
    for old, rep in EDITS[name]:
        if new.count(old) != 1:
            sys.exit(f"anchor not found exactly once for {name}: {old[:60]!r}")
        new = new.replace(old, rep)
    open(out, "w").write(new)
    sys.stdout.writelines(difflib.unified_diff(
        text.splitlines(True), new.splitlines(True),
        "a/tb/pp_top/sim_main.cpp", "b/tb/pp_top/sim_main.cpp"))


if __name__ == "__main__":
    main()
