# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The RTL consumers of the AECP command table: their declared constants only.
What the engine decodes and dispatches is observed (tb/aecp_cmds, record.py),
never read off its text. A pattern that parses nothing refuses (exit 2): a
stale pattern is itself a failure."""
import re

from .inputs import ENGINE, NOTIFY, PP_PKG, SCOREBOARD, TOP, UCPU_PKG, GateError, strip_sv
from .table import opcode_of

RE_C16 = re.compile(r"localparam\s+logic\s*\[15:0\]\s+((?:OP|GDI|MVU)_[A-Z0-9_]+_C)"
                    r"\s*=\s*16'h([0-9A-Fa-f]{4})\s*;")
RE_DECL = re.compile(r"\b(?:localparam|parameter)\b[^;]*?\b((?:OP|GDI)_[A-Z0-9_]+_C)\s*=")
RE_UPC = re.compile(r"localparam\s+logic\s*\[10:0\]\s+UPC_([A-Z0-9_]+)_C\s*=\s*11'd(\d+)\s*;")


def block(text: str, start: str, end: str, what: str) -> str:
    """The text from the first `start` regex to the next `end` regex."""
    m = re.search(start, text, re.S)
    if not m:
        raise GateError(f"{what}: '{start}' not found - the pattern has gone stale")
    e = re.search(end, text[m.end():])
    if not e:
        raise GateError(f"{what}: no '{end}' after '{start}'")
    return text[m.end():m.end() + e.start()]


def case_arms(body: str, label: str) -> list:
    """(labels, arm text) per case item of `body` whose label matches `label`;
    an arm runs to the next label or to `default`."""
    starts = [(m.start(), m.end(), m.group(1))
              for m in re.finditer(rf"(?m)^\s*({label})\s*:", body)]
    stop = re.search(r"(?m)^\s*default\s*:", body)
    arms = []
    for i, (_, end, lab) in enumerate(starts):
        nxt = starts[i + 1][0] if i + 1 < len(starts) else (stop.start() if stop else len(body))
        arms.append((lab, body[end:nxt]))
    return arms


def engine_consts(sv: str) -> dict:
    """The engine's declared command constants and µPC entry names."""
    code = strip_sv(sv)
    consts = {m.group(1): int(m.group(2), 16) for m in RE_C16.finditer(code)}
    unparsed = sorted({m.group(1) for m in RE_DECL.finditer(code)} - set(consts))
    upc = {m.group(1): int(m.group(2)) for m in RE_UPC.finditer(code)}
    if not consts or not upc:
        raise GateError(f"{ENGINE}: a pattern parsed nothing")
    return dict(consts=consts, unparsed=unparsed, upc=upc)


def classifier(sv: str) -> tuple:
    """protocol_processor_top's hz_classify: (default class, {opcode: class})
    for an AEM_COMMAND, an arm without its own class keeping the default."""
    #! the block's first `endcase` closes the opcode case: the ACMP arm above it
    #! has none, and its first class assignment is the default every arm keeps
    body = block(strip_sv(sv), r"always_comb\s+begin\s*:\s*hz_classify\b",
                 r"\bendcase\b", f"{TOP} hz_classify")
    d = re.search(r"hz_class_w\s*=\s*4'\(PP_HZ_([A-Z_]+)\)", body)
    case = re.search(r"unique\s+case\s*\(\s*hz_opcode_w\s*\)(.*)$", body, re.S)
    if not d or not case:
        raise GateError(f"{TOP}: hz_classify's default or its opcode case is not parseable")
    out = {}
    for lab, arm in case_arms(case.group(1), r"(?:16'h[0-9A-Fa-f]{4}\s*,\s*)*16'h[0-9A-Fa-f]{4}"):
        cls = set(re.findall(r"PP_HZ_([A-Z_]+)", arm))
        got = next(iter(cls)) if len(cls) == 1 else (d.group(1) if not cls else None)
        for op in re.findall(r"16'h([0-9A-Fa-f]{4})", lab):
            out[int(op, 16)] = got
    if not out:
        raise GateError(f"{TOP}: hz_classify's opcode case parsed nothing")
    return d.group(1), out


def hazard_codes(pkg: str) -> dict:
    """pp_pkg's hazard-class enum."""
    out = {m.group(1): int(m.group(2))
           for m in re.finditer(r"\bPP_HZ_([A-Z_]+)\s*=\s*4'd(\d+)", strip_sv(pkg))}
    if not out:
        raise GateError(f"{PP_PKG}: no PP_HZ_* enum parsed")
    return out


def uns_kinds(pkg: str) -> dict:
    """pp_pkg's unsolicited job kinds: code -> PP_UNS_*_C name."""
    out = {int(m.group(2)): m.group(1) for m in re.finditer(
        r"localparam\s+logic\s*\[3:0\]\s+(PP_UNS_[A-Z0-9_]+_C)\s*=\s*4'd(\d+)\s*;", strip_sv(pkg))}
    if not out:
        raise GateError(f"{PP_PKG}: no PP_UNS_*_C kind parsed")
    return out


def lockprot(sv: str) -> set:
    """KL_pp_scoreboard's hz_is_lockprot class set."""
    code = strip_sv(sv)
    alias = {m.group(1): m.group(2) for m in
             re.finditer(r"\b(HZ_[A-Z_]+_C)\s*=\s*pp_pkg::PP_HZ_([A-Z_]+)\s*;", code)}
    body = block(code, r"function\s+automatic\s+logic\s+hz_is_lockprot\b",
                 r"\bendfunction\b", f"{SCOREBOARD} hz_is_lockprot")
    names = set(re.findall(r"\b(HZ_[A-Z_]+_C)\b", body))
    if not names or not names <= set(alias):
        raise GateError(f"{SCOREBOARD}: hz_is_lockprot is not parseable")
    return {alias[n] for n in names}


def notify_classes(sv: str) -> tuple:
    """KL_aecp_notify: ({class: unsolicited kind}, the classes it queues)."""
    code = strip_sv(sv)
    case = block(code, r"unique\s+case\s*\(\s*cmdq_class_r\s*\[[^\]]*\]\s*\)", r"\bendcase\b",
                 f"{NOTIFY} emit_pick")
    out = {int(m.group(1)): m.group(2) for m in
           re.finditer(r"4'd(\d+)\s*:\s*pick_kind_w\s*=\s*(PP_UNS_[A-Z0-9_]+_C)\s*;", case)}
    ok = re.search(r"cmd_class_ok_w\s*=\s*ev_cmd_class_i\s+inside\s*\{([^}]*)\}", code)
    if not out or not ok:
        raise GateError(f"{NOTIFY}: the command-class map is not parseable")
    return out, {int(x) for x in re.findall(r"4'd(\d+)", ok.group(1))}


def status_codes(pkg: str) -> dict:
    """ucpu_pkg's status register codes."""
    out = {m.group(1): int(m.group(2)) for m in
           re.finditer(r"localparam\s+logic\s*\[4:0\]\s+(ST_[A-Z_]+_C)\s*=\s*5'd(\d+)\s*;",
                       strip_sv(pkg))}
    if not out:
        raise GateError(f"{UCPU_PKG}: no ST_*_C parsed")
    return out


def _gdi_consts(t: dict, consts: dict) -> list:
    """The engine's GDI_*_C against the table's documented GET_DYNAMIC_INFO
    members, and every other declared opcode against the served commands."""
    p = []
    claimed = {c["rtl"] for c in t["commands"]}
    docs = {d["rtl"]: d for d in t.get("documented", []) if d.get("rtl")}
    for name, val in sorted(consts.items()):
        if name.startswith("MVU_PID_"):
            continue
        if name.startswith("GDI_"):
            d = docs.get(name)
            if d is None:
                p.append(f"  engine: {ENGINE} declares {name} = 0x{val:04X} and the "
                         "table documents no such GET_DYNAMIC_INFO member")
            elif opcode_of(d["opcode"]) != val:
                p.append(f"  engine: {d['name']}: the table says {d['opcode']}, "
                         f"{ENGINE} {name} = 0x{val:04X}")
        elif name not in claimed:
            p.append(f"  engine: {ENGINE} decodes {name} = 0x{val:04X} and the table "
                     "has no command for it")
    for name in sorted(set(docs) - set(consts)):
        p.append(f"  engine: {docs[name]['name']}: the table names {name} and {ENGINE} "
                 "declares no such constant")
    return p


def check_engine(t: dict, eng: dict) -> list:
    """The engine's declared opcodes and µPC entry names against the table."""
    p = []
    consts = eng["consts"]
    for name in eng["unparsed"]:
        p.append(f"  engine: {name} is not written `localparam logic [15:0] {name} = "
                 "16'hXXXX;`, so its opcode cannot be held to the table")
    for c in t["commands"]:
        have = consts.get(c["rtl"])
        if have is None:
            p.append(f"  engine: {c['name']}: the table names {c['rtl']} and "
                     f"{ENGINE} declares no such constant")
        elif have != opcode_of(c["opcode"]):
            p.append(f"  engine: {c['name']}: the table says opcode {c['opcode']}, "
                     f"{ENGINE} {c['rtl']} = 0x{have:04X}")
    p += _gdi_consts(t, consts)
    bodies = {c["notify"]["body"] for c in t["commands"] if c["notify"]}
    entries = {e for c in t["commands"] for e in c["entries"]}
    shared = set(t.get("shared_entries", {}))
    for c in t["commands"]:
        for e in c["entries"]:
            if e not in eng["upc"]:
                p.append(f"  engine: {c['name']}: entry {e} has no UPC_{e}_C, so the "
                         "engine cannot dispatch to it")
    for e in sorted(set(eng["upc"]) - entries - bodies - shared):
        p.append(f"  engine: UPC_{e}_C is claimed by no command, notification body or "
                 "shared entry of the table")
    return p


def check_classifier(t: dict, default: str, cls: dict, codes: dict) -> list:
    """The top's hazard classes and pp_pkg's codes."""
    p = []
    for name, code in sorted(t["hazard_classes"].items()):
        if codes.get(name) != code:
            p.append(f"  classifier: hazard class {name} is {code} in the table and "
                     f"{codes.get(name)} in {PP_PKG}")
    for name in sorted(set(codes) - set(t["hazard_classes"])):
        p.append(f"  classifier: {PP_PKG} declares PP_HZ_{name} and the table does not")
    aem = {opcode_of(c["opcode"]): c for c in t["commands"] if c["message"] == "AEM"}
    for op, c in sorted(aem.items()):
        got = cls.get(op, default)
        if got != c["hazard"]:
            p.append(f"  classifier: {c['name']}: the table says {c['hazard']}, "
                     f"{TOP} hz_classify gives 0x{op:04X} {got}")
    for op in sorted(set(cls) - set(aem)):
        p.append(f"  classifier: {TOP} hz_classify names 0x{op:04X}, which no table "
                 "command carries")
    for c in t["commands"]:
        if c["message"] != "AEM" and c["hazard"] != default:
            p.append(f"  classifier: {c['name']}: a non-AEM command takes the "
                     f"classifier's default {default}, not {c['hazard']}")
    return p


def check_scoreboard(t: dict, prot: set) -> list:
    """The scoreboard's lock-protected classes are the lock commands' classes."""
    want = {c["hazard"] for c in t["commands"] if c["lock"]}
    if prot == want:
        return []
    return [f"  scoreboard: {SCOREBOARD} hz_is_lockprot is {sorted(prot)}, the table's "
            f"lock-protected commands are {sorted(want)}"]


def check_notify(t: dict, cmap: dict, queued: set) -> list:
    """The notify block's class -> kind map against the table's classes."""
    p = []
    for c in t["commands"]:
        nt = c["notify"]
        if not nt or nt["class"] is None:
            continue
        k = nt["class"]
        if k in cmap and cmap[k] != nt["kind"]:
            p.append(f"  notify: {c['name']}: class {k} is {cmap[k]} in {NOTIFY}, "
                     f"the table says {nt['kind']}")
        if (k in cmap) != (k in queued):
            p.append(f"  notify: {NOTIFY} queues classes {sorted(queued)} and maps "
                     f"{sorted(cmap)}: class {k} ({c['name']}) is in one only")
        if k not in cmap and nt["kind"] in cmap.values():
            p.append(f"  notify: {c['name']}: class {k} is not in {NOTIFY}'s map, yet "
                     f"its kind {nt['kind']} is another class's")
    for k, kind in sorted(cmap.items()):
        if not any(c["notify"] and c["notify"]["class"] == k for c in t["commands"]):
            p.append(f"  notify: {NOTIFY} maps class {k} to {kind} and no table "
                     "command enqueues it")
    return p


def check_status_codes(t: dict, pkg: dict) -> list:
    """ucpu_pkg's ST_*_C codes are the table's."""
    p = []
    rows = {r["rtl"]: (n, r["code"]) for n, r in t["status_codes"].items() if r.get("rtl")}
    for rtl, (name, code) in sorted(rows.items()):
        if pkg.get(rtl) != code:
            p.append(f"  status: {name} is {code} in the table and {rtl} = "
                     f"{pkg.get(rtl)} in {UCPU_PKG}")
    for rtl in sorted(set(pkg) - set(rows)):
        p.append(f"  status: {UCPU_PKG} declares {rtl} and the table has no status for it")
    return p
