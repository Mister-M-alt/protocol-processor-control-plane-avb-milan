#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The AECP command model is written once; this holds every consumer to it.

WHY THIS EXISTS (issue #73, REQ-VER-002). Each command the AECP engine serves
was written down in several places that nothing connected: its opcode in the
engine's `OP_*_C` localparams, the top's hazard classifier and the M9 sweep; its
sizes, statuses, lock and notify behaviour in the µprograms of
`hdl/aecp/ucode/gen_ucode.py` and again, by hand, in F06.14. They had already
drifted: F06.14 gave SET/GET_SAMPLING_RATE and SET/GET_CLOCK_SOURCE a 36-octet
response where the engine answers 32 (cdl 20), and GET_AUDIO_MAP 32 + 8·N where
it answers 36 + 8·N (cdl 24 + 8·N).

`hdl/aecp/ucode/aecp_commands.json` is now the one source: opcode, command and
response sizes, status set, hazard class, lock and notify flags per served
command, and every µPC program the engine dispatches it to. What it drives:

  GENERATED  F06.14 in docs/architecture/06_aecp_engine.md (`--write`), and
             gen_ucode.py's status codes and MVU command_type (ROM bytes).
  OBSERVED   what the elaborated engine decodes and dispatches. tb/aecp_cmds
             sweeps KL_aecp_engine alone and records it in
             tb/aecp_cmds/decode.json; the suite holds a fresh sweep to that
             record and to the table, and this gate holds the table to the
             record. Per served command: the programs the engine dispatches its
             opcode to on every outcome of the sweep, the MVU sub-decode
             included, are exactly its `entries`; the shortest command it
             dispatches to its first entry is its command cdl (or its declared
             `floor`); a per-record command is accepted only at cdl + k·per;
             every response the bench saw fits its response rule and carries a
             status of its set. Every opcode the table does not serve reaches
             only the `unserved` program, on every message type swept. The
             GET_DYNAMIC_INFO members, their programs and record lengths, its
             empty aggregate, and every unsolicited job's command_type, body
             and size are the table's. decode.json carries the digest of every
             input the sweep was made of (the engine's SystemVerilog by its
             code, the bench, the image's inputs, the ROM), so an engine
             changed without a new sweep fails this gate by file name and the
             suite by command name.
  GATED      everything below, because generating it would change an
             elaborated input, or a line a reviewed mutation patch anchors on:
    engine      KL_aecp_engine.sv's OP_*_C / MVU_*_C / GDI_*_C values (the
                lines the M9 gate and the parent inventory read), and every
                UPC_*_C entry name claimed by a command or the shared list;
    classifier  protocol_processor_top.sv hz_classify, opcode -> class, and
                pp_pkg's hazard codes;
    scoreboard  KL_pp_scoreboard.sv hz_is_lockprot = the lock commands' classes;
    notify      KL_aecp_notify.sv's class -> unsolicited kind case;
    status      ucpu_pkg.sv ST_*_C codes, and the table's codes are IEEE
                1722.1-2021 Table 7-141's;
    rom         the generated µcode, walked from every entry: a lock-protected
                command reaches CHECK_LOCK and no other does, its NOTIFY_ENQ
                class is the table's, every status a path sets is in its set
                and every status in the set is one a path can set, and every
                response it sends has the table's size - a fixed rule exactly,
                a per-record rule as its fixed part plus whole records (the
                walk keeps the fixed part where a loop widens it, and a path
                must build the per-record part), a descriptor rule as its
                fixed part plus the copy; no path falls into the ROM fill;
    8.1         06 section 8.1 lists exactly the served commands.
  The table itself: an echo row's response size is its command size.

  HELD TO NOTHING (said here, in 09 section 8.10 and in REQ-VER-002):
    ACQUIRE_ENTITY's and REGISTER_UNSOLICITED_NOTIFICATION's command cdl
    (the engine accepts both from cdl 12, so only the echo rule ties them to
    their response); GET_DYNAMIC_INFO's 524 ceilings; the decode of opcodes
    above 0x007F and of outcomes no stimulus of the sweep reaches; the
    µprograms' field VALUES and order; the ROM layout (check_upc_map.py's);
    and every row of F06.14 the table only documents.

Every line of a failure names the consumer, and the command that disagrees
with it - or, where no command is involved, the opcode, the table field or the
file.

The checks live in scripts/aecp_gate/, one module per consumer (its docstring
lists them); this file is the entry point.

Exit 0 = every consumer agrees. Exit 1 = a disagreement, each printed. Exit 2 =
an input is unreadable or a pattern found nothing (a stale pattern is itself a
failure). `--write` regenerates F06.14 from the table. `--selftest` plants one
defect for every finding this gate can print and for every refusal (the table
defects the generator reads in a file copy, so the generator runs on them) and
requires each to be caught by name. `--observed FILE` is tb/aecp_cmds's step:
it holds a fresh sweep to the table and to decode.json (`--write-record`
rewrites decode.json from it).
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from aecp_gate.doc import rows  # noqa: E402
from aecp_gate.gate import check, observed, write  # noqa: E402
from aecp_gate.inputs import ROOT, TABLE, GateError, Inputs, load  # noqa: E402
from aecp_gate.plants import rom_op  # noqa: E402
from aecp_gate.selftest import selftest  # noqa: E402

#: the gate's API for a tool that loads this file as a module (the reviews'
#: ROM size probe does): read the inputs, plant a defect in an Inputs copy
#: (rom_op finds a ROM word to plant it in), and check the copy
__all__ = ["GateError", "Inputs", "check", "load", "main", "rom_op"]


def main() -> int:
    """The gate; `--selftest` first proves it can fail, `--write` regenerates,
    `--observed` is tb/aecp_cmds's verdict."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true", help="plant each defect and require it caught")
    ap.add_argument("--write", action="store_true", help="regenerate F06.14 from the table")
    ap.add_argument("--observed", type=Path, help="tb/aecp_cmds's sweep output to hold")
    ap.add_argument("--write-record", action="store_true",
                    help="with --observed: rewrite tb/aecp_cmds/decode.json from it")
    ap.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = ap.parse_args()
    try:
        if args.selftest:
            return selftest(args.root)
        if args.write:
            return write(args.root)
        if args.observed:
            return observed(args.root, args.observed, args.write_record)
        inp = load(args.root)
        problems = check(inp)
    except GateError as exc:
        print(f"AECP COMMAND GATE: REFUSED - {exc}", file=sys.stderr)
        return 2
    if problems:
        print("AECP COMMAND GATE: FAIL - a consumer disagrees with "
              f"{TABLE}\n")
        print("\n".join(problems))
        return 1
    t = inp.table
    print(f"aecp commands: {len(t['commands'])} commands, "
          f"{sum(len(c['entries']) for c in t['commands'])} entries walked and observed, "
          f"F06.14 {len(rows(t))} rows, OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
