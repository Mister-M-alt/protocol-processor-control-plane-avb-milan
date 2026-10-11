# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The AECP command gate's self-test: the unmutated tree passes, and every
planted defect fails with its words. The in-memory defects are plants.py's;
the refusals made while the inputs are read, and the table defects the
generator reads, are planted in a copy of the input files, so the gate and the
generator run on them as they would on a real edit (R608-1 F2)."""
import copy
import json
import shutil
import tempfile
from collections.abc import Callable
from pathlib import Path

from .gate import check
from .inputs import (DOC, RECORD, SWEPT_RAW, SWEPT_SV, TABLE, TEXTS, UCODE, GateError, Inputs,
                     load, load_ucode)
from .plants import MUTANTS

#: a mutation of a copy of the input files, given the copy's root
FileEdit = Callable[[Path], object]


def _edit(d: Path, fn: Callable[[dict], object]) -> None:
    t = json.loads((d / TABLE).read_text(encoding="utf-8"))
    fn(t)
    (d / TABLE).write_text(json.dumps(t, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def edit_table(fn: Callable[[dict], object]) -> FileEdit:
    """A file mutation: load the copy's table, apply `fn`, write it back."""
    return lambda d: _edit(d, fn)


def named(t: dict, name: str) -> dict:
    """A table row by name, for a file mutation."""
    return next(c for c in t["commands"] if c["name"] == name)


#: (what, mutation of a copy of the input files, words) per refusal that
#: happens while the inputs are read, and per table defect the generator reads
FILE_MUTANTS = (
    ("a table that is not JSON",
     lambda d: (d / TABLE).write_text("{", encoding="utf-8"),
     ("gate refused", f"{TABLE} is not JSON")),
    ("a decode record that is not JSON",
     lambda d: (d / RECORD).write_text("[", encoding="utf-8"),
     ("gate refused", f"{RECORD} is not JSON")),
    ("an input that cannot be read",
     lambda d: (d / DOC).unlink(),
     ("gate refused", "cannot read an input")),
    ("a decode record that cannot be read",
     lambda d: (d / RECORD).unlink(),
     ("gate refused", "cannot read an input")),
    ("a swept input that cannot be read",
     lambda d: (d / "tb/aecp_cmds/sim_main.cpp").unlink(),
     ("gate refused", "cannot read an input")),
    ("a generator that does not run",
     lambda d: (d / UCODE).write_text("raise RuntimeError('planted')\n", encoding="utf-8"),
     ("gate refused", f"{UCODE} did not generate")),
    ("a generator of another shape",
     lambda d: (d / UCODE).write_text("rom = []\n", encoding="utf-8"),
     ("gate refused", f"{UCODE} defines no occupied")),
    ("a message type the generator cannot read, in the file (R608-1 p5e)",
     edit_table(lambda t: named(t, "GET_MILAN_INFO").update(message="AAU")),
     ("schema: GET_MILAN_INFO: message 'AAU' is neither AEM nor MVU",)),
    ("a message type the generator cannot read, in the file (R608-1 p5)",
     edit_table(lambda t: named(t, "GET_MILAN_INFO").update(message="AA")),
     ("schema: GET_MILAN_INFO: no command with message MVU",)),
    ("an entry the generator never placed, in the file (R608-1 p8)",
     edit_table(lambda t: named(t, "GET_CLOCK_SOURCE")["entries"].append("SCFGRUN2")),
     ("decode: GET_CLOCK_SOURCE: the table's entries name SCFGRUN2",)),
    ("a status code row the generator reads, gone from the file",
     edit_table(lambda t: t["status_codes"].pop("STREAM_IS_RUNNING")),
     ("schema: SET_CONFIGURATION: status STREAM_IS_RUNNING is not in status_codes",)),
    ("a status code the generator reads, not an integer in the file",
     edit_table(lambda t: t["status_codes"]["NO_RESOURCES"].update(code="8")),
     ("schema: status NO_RESOURCES: code must be an integer",)),
    ("a status code changed in the file only",
     edit_table(lambda t: t["status_codes"]["NO_RESOURCES"].update(code=9)),
     ("schema: status NO_RESOURCES is 9 in the table",)),
    ("the MVU row the generator reads, renamed in the file",
     edit_table(lambda t: named(t, "GET_MILAN_INFO").update(name="GET_MILAN_INFORMATION")),
     ("schema: GET_MILAN_INFO: no command with message MVU",)),
)

#: (what, mutation of a copy of the table, words) per field the generator
#: itself refuses: gen_ucode.py runs alone on the copy, as `make` runs it, so
#: its own refusal names the command and the field
GENERATOR_MUTANTS = (
    ("the generator: an entry it never placed",
     edit_table(lambda t: named(t, "GET_CLOCK_SOURCE")["entries"].append("SCFGRUN2")),
     ("GET_CLOCK_SOURCE: entries names SCFGRUN2", "placed no E_SCFGRUN2")),
    ("the generator: a status code row it reads, gone",
     edit_table(lambda t: t["status_codes"].pop("STREAM_IS_RUNNING")),
     ("status_codes gives no integer code for STREAM_IS_RUNNING",)),
    ("the generator: a status code it reads, not an integer",
     edit_table(lambda t: t["status_codes"]["BAD_ARGUMENTS"].update(code="7")),
     ("status_codes gives no integer code for BAD_ARGUMENTS",)),
    ("the generator: the MVU row it reads, gone",
     edit_table(lambda t: named(t, "GET_MILAN_INFO").update(message="AAU")),
     ("GET_MILAN_INFO: no command with message MVU",)),
    ("the generator: the MVU opcode it reads, not hexadecimal",
     edit_table(lambda t: named(t, "GET_MILAN_INFO").update(opcode="0")),
     ("GET_MILAN_INFO: no command with message MVU and an 0xHHHH opcode",)),
)


def copy_inputs(root: Path, d: Path) -> None:
    """The gate's input files under a fresh directory."""
    for p in (TABLE, UCODE, RECORD) + TEXTS + SWEPT_SV + SWEPT_RAW:
        (d / p).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / p, d / p)


def planted_copy(root: Path, fn: FileEdit) -> list:
    """Copy the gate's input files to a fresh directory, apply `fn` there, and
    run the whole gate, reading and generating included, on the copy."""
    with tempfile.TemporaryDirectory(prefix="aecp-commands-selftest-") as tmp:
        d = Path(tmp)
        copy_inputs(root, d)
        fn(d)
        try:
            return check(load(d))
        except GateError as exc:
            return [f"  (gate refused: {exc})"]


def generator_copy(root: Path, fn: FileEdit) -> list:
    """Copy the generator and its table, apply `fn`, and run the generator."""
    with tempfile.TemporaryDirectory(prefix="aecp-commands-selftest-") as tmp:
        d = Path(tmp)
        copy_inputs(root, d)
        fn(d)
        try:
            load_ucode(d / UCODE)
        except GateError as exc:
            return [f"  ({exc})"]
        return []


def missed(what: str, words: tuple, got: list) -> int:
    """1, printed, when no finding carries every one of `words`; else 0."""
    if any(all(w in g for w in words) for g in got):
        return 0
    print(f"SELFTEST FAIL: '{what}' was not caught by name {words}:")
    print("\n".join(got[:6]) or "  (no finding at all)")
    return 1


def in_memory(base: Inputs, fn: Callable[[Inputs], object]) -> list:
    """The gate's findings on a copy of `base` that `fn` has mutated."""
    inp = Inputs(copy.deepcopy(base.table), dict(base.texts), copy.deepcopy(base.ucode),
                 copy.deepcopy(base.record), base.root, dict(base.swept))
    try:
        fn(inp)
        return check(inp)
    except GateError as exc:
        return [f"  (gate refused: {exc})"]


def selftest(root: Path) -> int:
    """The tree passes, and each planted defect fails with its words."""
    base = load(root)
    first = check(base)
    if first:
        print("AECP COMMAND GATE SELFTEST: FAIL - the unmutated tree does not pass:")
        print("\n".join(first))
        return 1
    cases = MUTANTS + FILE_MUTANTS + GENERATOR_MUTANTS
    bad = 0
    for what, fn, words in MUTANTS:
        bad += missed(what, words, in_memory(base, fn))
    for what, fn, words in FILE_MUTANTS:
        bad += missed(what, words, planted_copy(root, fn))
    for what, fn, words in GENERATOR_MUTANTS:
        bad += missed(what, words, generator_copy(root, fn))
    print(f"aecp commands selftest: {len(cases)} planted defects, "
          f"{len(cases) - bad} caught by name, {'OK' if not bad else 'FAIL'}")
    return 1 if bad else 0
