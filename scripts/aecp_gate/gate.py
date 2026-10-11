# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The AECP command gate's three verdicts: every consumer against the table
(`check`), F06.14 regenerated (`write`), and tb/aecp_cmds' fresh sweep against
the table and the recorded decode.json (`observed`)."""
import json
from pathlib import Path

from .doc import check_doc, check_inventory, regenerated
from .fields import check_fields
from .field_docs import check_references, write_references
from .inputs import (DOC, ENGINE, NOTIFY, PP_PKG, RECORD, ROM_KEY, SCOREBOARD, TOP,
                     UCPU_PKG, GateError, Inputs, digests, load, read_json, ucode_of)
from .record import canonical, check_record, check_swept, dump_record, record_diff
from .rom import check_rom
from .rtl import (check_classifier, check_engine, check_notify, check_scoreboard,
                  check_status_codes, classifier, engine_consts, hazard_codes, lockprot,
                  notify_classes, status_codes, uns_kinds)
from .table import check_schema


def check(inp: Inputs) -> list:
    """Every disagreement, one line each; empty means the gate passes. The
    generator runs only on a table the schema accepts and whose entries the
    engine was observed to dispatch: those are the fields it reads."""
    t = inp.table
    p = check_schema(t)
    if p:
        return p
    p += check_fields(inp)
    if inp.record is None:
        inp.record = read_json(inp.root, RECORD)
    found, _, _, entries_ok = check_record(t, inp.record)
    found += check_swept(inp, rom=False)
    p += found
    p += check_engine(t, engine_consts(inp.texts[ENGINE]))
    default, cls = classifier(inp.texts[TOP])
    p += check_classifier(t, default, cls, hazard_codes(inp.texts[PP_PKG]))
    p += check_scoreboard(t, lockprot(inp.texts[SCOREBOARD]))
    p += check_notify(t, *notify_classes(inp.texts[NOTIFY]))
    p += check_status_codes(t, status_codes(inp.texts[UCPU_PKG]))
    p += check_doc(t, inp.texts[DOC])
    p += check_inventory(t, inp.texts[DOC])
    if not check_fields(inp):
        p += check_references(inp)
    if entries_ok:
        p += check_rom(t, ucode_of(inp))
        p += [x for x in check_swept(inp, rom=True) if ROM_KEY in x]
    return p


def write(root: Path) -> int:
    """Regenerate F06.14 from the table."""
    inp = load(root)
    p = check_schema(inp.table) + check_fields(inp)
    if p:
        print("\n".join(p))
        return 1
    doc = inp.texts[DOC]
    new = regenerated(inp.table, doc)
    if new != doc:
        (root / DOC).write_text(new, encoding="utf-8")
        print(f"aecp commands: rewrote F06.14 in {DOC}")
    else:
        print(f"aecp commands: F06.14 in {DOC} is current")
    write_references(inp)
    return 0


def observed(root: Path, path: Path, rewrite: bool) -> int:
    """tb/aecp_cmds's verdict: a fresh sweep held to the table and to the
    recorded decode.json (or, with `rewrite`, written as decode.json)."""
    inp = load(root, record=not rewrite)
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError(f"the sweep's output {path} cannot be read: {exc}") from exc
    uc = ucode_of(inp)
    rec = canonical(raw, uc, uns_kinds(inp.texts[PP_PKG]), digests(inp.swept, uc))
    if rewrite:
        (root / RECORD).write_text(dump_record(rec), encoding="utf-8")
        print(f"aecp commands: wrote {RECORD} from {raw['trials']} trials")
        inp.record = rec
    p = check_schema(inp.table)
    n, f = 1, bool(p)
    if not p:
        found, n2, f2, _ = check_record(inp.table, rec, "the fresh sweep")
        p, n, f = found, n + n2, f + f2
    stale = record_diff(rec, inp.record)
    p, n, f = p + stale, n + 1, f + bool(stale)
    for line in p:
        print(f"FAIL:{line}")
    print(f"aecp_cmds: {raw['trials']} trials swept; {n} checks: {n - f} PASS, {f} FAIL")
    return 1 if f else 0
