#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The M9 sweep's opcode list is the engine's opcode list; this makes it so.

WHY THIS EXISTS. `KL_aecp_engine` reads AECPDU @22..@23 as `opcode`, and on
every message type but AEM_COMMAND those two bytes are not a command_type at
all (a VENDOR_UNIQUE protocol_id head, an AV/C length). Every dispatch arm
therefore carries the `aem_w` message-type guard, and `tb/pp_top` section M9
sends each opcode the engine names on every non-AEM message type to prove the
guard is there (issue #83). The sweep can only test the arms it lists: issue
#76 found `kOpcodes` at 23 of the 30 opcodes the engine decodes, because the
seven arms that landed after the list was written never joined it, and
removing the guard from any of them left the suite green.

So: parse the engine's `OP_*_C` localparams (the opcodes its two decode stages
name; the `GDI_*_C` whitelist members are not dispatch arms and are not
parsed) and the `kOpcodes` initializer in `tb/pp_top/sim_main.cpp`, and refuse
any difference in either direction. A new arm that is not in the sweep fails
here, before any suite runs; so does a sweep entry the engine no longer names.

Exit 0 = the two sets are equal. Exit 1 = they are not, or a parse found
nothing (a stale pattern is itself a failure). `--selftest` runs the parser
and the comparison on fixed fixtures, including three that must fail.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "hdl" / "aecp" / "KL_aecp_engine.sv"
BENCH = ROOT / "tb" / "pp_top" / "sim_main.cpp"

#! `localparam logic [15:0] OP_NAME_C = 16'h0004;`
RE_ENGINE = re.compile(
    r"localparam\s+logic\s*\[15:0\]\s+(OP_[A-Z0-9_]+_C)\s*=\s*16'h([0-9A-Fa-f]{4})\s*;")
#! `static const uint16_t kOpcodes[] = { 0x0000, ... };`
RE_BENCH = re.compile(r"static\s+const\s+uint16_t\s+kOpcodes\[\]\s*=\s*\{([^}]*)\}")
RE_HEX = re.compile(r"0x([0-9A-Fa-f]{1,4})\b")


def engine_opcodes(text: str) -> dict[int, str]:
    """opcode -> the OP_*_C names that carry it (one opcode, one name)."""
    found: dict[int, str] = {}
    for m in RE_ENGINE.finditer(text):
        found[int(m.group(2), 16)] = m.group(1)
    return found


def bench_opcodes(text: str) -> list[int]:
    """The kOpcodes initializer, comments removed, in written order."""
    m = RE_BENCH.search(text)
    if m is None:
        return []
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [int(h, 16) for h in RE_HEX.findall(body)]


def compare(engine: dict[int, str], bench: list[int]) -> list[str]:
    """Every disagreement, one line each; empty means the gate passes."""
    if not engine or not bench:
        return ["  parsed nothing: the engine or the kOpcodes pattern has gone "
                "stale, which is itself a failure"]
    problems = []
    for op in sorted(set(bench)):
        if bench.count(op) > 1:
            problems.append(f"  0x{op:04X} is listed {bench.count(op)} times in kOpcodes")
    for op, name in sorted(engine.items()):
        if op not in bench:
            problems.append(f"  {name} = 0x{op:04X} is decoded by the engine and "
                            "missing from kOpcodes: its aem_w guard is untested")
    for op in sorted(set(bench) - set(engine)):
        problems.append(f"  0x{op:04X} is in kOpcodes and the engine names no "
                        "OP_*_C for it")
    return problems


def selftest() -> int:
    """The parser and the comparison on fixed text, three fixtures failing."""
    eng = ("localparam logic [15:0] OP_READ_DESCRIPTOR_C = 16'h0004;\n"
           "localparam logic [15:0] GDI_GET_VIDEO_FMT_C   = 16'h000B;\n"
           "  localparam logic [15:0] OP_SET_NAME_C = 16'h0010;\n")
    good = "static const uint16_t kOpcodes[] = {\n  0x0004, // x 0x0099\n 0x0010,\n};"
    cases = [
        ("equal sets pass", good, True),
        ("a missing engine opcode fails",
         "static const uint16_t kOpcodes[] = { 0x0004, };", False),
        ("an extra bench opcode fails",
         "static const uint16_t kOpcodes[] = { 0x0004, 0x0010, 0x000B, };", False),
        ("a duplicate fails",
         "static const uint16_t kOpcodes[] = { 0x0004, 0x0010, 0x0010, };", False),
        ("no initializer fails", "static const uint16_t kOther[] = { 0x0004 };", False),
    ]
    parsed = engine_opcodes(eng)
    bad = 0
    if sorted(parsed) != [0x0004, 0x0010]:
        print(f"SELFTEST FAIL: the engine parse gave {sorted(parsed)}")
        bad += 1
    for what, bench, want in cases:
        ok = not compare(parsed, bench_opcodes(bench))
        if ok != want:
            print(f"SELFTEST FAIL: {what}")
            bad += 1
    print(f"M9 OPCODE GATE SELFTEST: {len(cases) + 1 - bad} of {len(cases) + 1} PASS")
    return 1 if bad else 0


def main() -> int:
    """The gate itself; `--selftest` first proves it can fail."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true",
                    help="run the fixtures instead of the tree")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for path in (ENGINE, BENCH):
        if not path.exists():
            print(f"M9 OPCODE GATE: cannot read {path}", file=sys.stderr)
            return 1
    engine = engine_opcodes(ENGINE.read_text(encoding="utf-8"))
    bench = bench_opcodes(BENCH.read_text(encoding="utf-8"))
    problems = compare(engine, bench)
    if problems:
        print("M9 OPCODE GATE: FAIL - tb/pp_top M9's kOpcodes is not the engine's "
              "OP_*_C set\n")
        print("\n".join(problems))
        return 1
    print(f"M9 OPCODE GATE: PASS ({len(engine)} engine opcodes, all swept by M9)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
