# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The documents the AECP command table drives: F06.14, generated between its
markers in docs/architecture/06_aecp_engine.md, and 06 section 8.1, which must
list exactly the served commands."""
import re

from .inputs import COLUMNS, DOC, SECTIONS, GateError
from .table import opcode_of, size_text

BEGIN = ("<!-- BEGIN F06.14: generated from hdl/aecp/ucode/aecp_commands.json by "
         "scripts/check_aecp_commands.py --write; edit the table, not these rows -->")
END = "<!-- END F06.14 -->"


def yes(flag: bool) -> str:
    """A table flag as F06.14 prints it."""
    return "yes" if flag else "no"


def resp_text(c: dict) -> str:
    """F06.14's Resp. size cell: AECPDU octets, then the cdl rule."""
    r, note = c["response"], c["doc"].get("size")
    out = f"{size_text(r, True)} B{' echo' if r.get('echo') else ''} (cdl {size_text(r, False)}"
    if r.get("max"):
        out += f", at most {r['max']}"
    if r.get("per"):
        out += f", N {r['unit']}"
    if r.get("stub"):
        out += f"; failure stub cdl {r['stub']}"
    out += ")"
    return out + (f"; {note}" if note else "")


def notif_text(c: dict) -> str:
    """F06.14's Notif cell: the class a program enqueues, or the engine's event."""
    nt = c["notify"]
    if not nt:
        return "—"
    how = f"class {nt['class']}" if nt["class"] is not None else "event"
    return f"{how}: {nt['doc']}"


def _served_row(c: dict) -> list:
    """One served command's F06.14 cells."""
    op = c["opcode"] if c["message"] == "AEM" else f"MVU {c['opcode']}"
    cls = c["hazard"] + (f" {c['doc']['class']}" if c["doc"].get("class") else "")
    cmd = c["command"]
    cmd_text = size_text(cmd, False) + (f", at most {cmd['max']}" if cmd.get("max") else "")
    return [op, c["name"], c["doc"]["mandate"], c["doc"]["scope"], cls, yes(c["lock"]),
            yes(c["gdi"]), yes(c["oversize"]), notif_text(c), cmd_text, resp_text(c),
            ", ".join(c["status"])]


def rows(t: dict) -> list:
    """F06.14's rows, generated: (opcode, command, cells...) per row."""
    out = []
    for sec in SECTIONS:
        group = []
        for c in t["commands"]:
            if {"AEM": "aem", "MVU": "mvu"}[c["message"]] == sec:
                group.append((opcode_of(c["opcode"]), 0, _served_row(c)))
        for d in t.get("documented", []):
            if d["section"] == sec:
                key = opcode_of(d["opcode"].split()[-1].split("/")[0])
                group.append((key, 1, [d["opcode"], d["name"]] + list(d["cells"])))
        out += [r for _, _, r in sorted(group, key=lambda g: (g[0], g[1]))]
    return out


def render(t: dict) -> list:
    """F06.14 as the lines between its markers."""
    lines = ["| " + " | ".join(COLUMNS) + " |", "|" + "---|" * len(COLUMNS)]
    return lines + ["| " + " | ".join(r) + " |" for r in rows(t)]


def doc_block(doc: str) -> tuple:
    """(lines between the F06.14 markers, start offset, end offset)."""
    a, b = doc.find(BEGIN), doc.find(END)
    if a < 0 or b < a:
        raise GateError(f"{DOC}: the F06.14 markers are missing; restore them, then "
                        "run --write")
    start = a + len(BEGIN)
    return doc[start:b].strip("\n").split("\n"), start, b


def regenerated(t: dict, doc: str) -> str:
    """`doc` with F06.14 regenerated from `t`."""
    _, a, b = doc_block(doc)
    return doc[:a] + "\n" + "\n".join(render(t)) + "\n" + doc[b:]


def cells(line: str) -> list:
    """A Markdown table row's cells."""
    return [x.strip() for x in line.strip().strip("|").split("|")]


def check_doc(t: dict, doc: str) -> list:
    """F06.14 is what the table generates, row by row and cell by cell."""
    p = []
    have, _, _ = doc_block(doc)
    want = render(t)
    if have[:2] != want[:2]:
        p.append(f"  F06.14: the header is not {want[0]}")
    if len(have) != len(want):
        p.append(f"  F06.14: {len(have) - 2} rows where the table generates {len(want) - 2}")
    hrows = {tuple(cells(x)[:2]): cells(x) for x in have[2:]}
    wrows = {tuple(cells(x)[:2]): cells(x) for x in want[2:]}
    for key in [k for k in wrows if k not in hrows]:
        p.append(f"  F06.14: row {key[0]} {key[1]} is missing (run --write)")
    for key in [k for k in hrows if k not in wrows]:
        p.append(f"  F06.14: row {key[0]} {key[1]} is not in the table")
    for key in [k for k in wrows if k in hrows]:
        for col, a, b in zip(COLUMNS, hrows[key], wrows[key]):
            if a != b:
                p.append(f"  F06.14: row {key[0]} {key[1]}, column {col}, reads "
                         f"'{a}' where the table gives '{b}'")
        if len(hrows[key]) != len(wrows[key]):
            p.append(f"  F06.14: row {key[0]} {key[1]} has {len(hrows[key])} cells, "
                     f"not {len(wrows[key])}")
    order_h = [k for k in (tuple(cells(x)[:2]) for x in have[2:]) if k in wrows]
    order_w = [k for k in wrows if k in hrows]
    if not p and order_h != order_w:
        p.append("  F06.14: the rows are out of the generated order (run --write)")
    return p


def check_inventory(t: dict, doc: str) -> list:
    """06 section 8.1 lists exactly the served commands."""
    sec = re.search(r"### 8\.1 Realization status.*?\n(\|.*?)\n\n", doc, re.S)
    if not sec:
        raise GateError(f"{DOC}: section 8.1's table is not parseable")
    aem, mvu = set(), set()
    for line in sec.group(1).split("\n")[2:]:
        first = cells(line)[0]
        ops = {int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4})", first)}
        (mvu if first.startswith("MVU") else aem).update(ops)
    p = []
    for msg, have in (("AEM", aem), ("MVU", mvu)):
        want = {opcode_of(c["opcode"]): c["name"] for c in t["commands"] if c["message"] == msg}
        for op in sorted(set(want) - have):
            p.append(f"  8.1: {want[op]} ({msg} 0x{op:04X}) is served and section 8.1 "
                     "does not list it")
        for op in sorted(have - set(want)):
            p.append(f"  8.1: section 8.1 lists {msg} 0x{op:04X} and the table serves "
                     "no such command")
    return p
