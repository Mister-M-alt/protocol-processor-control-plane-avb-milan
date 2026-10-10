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
command, plus the µPC programs the engine dispatches it to. What it drives:

  GENERATED  F06.14 in docs/architecture/06_aecp_engine.md (`--write`), and
             gen_ucode.py's status codes, MVU command_type and DISPATCH map.
  GATED      everything below, because generating it would change an
             elaborated input, or a line a reviewed mutation patch anchors on:
    engine      KL_aecp_engine.sv OP_*_C / MVU_*_C / GDI_*_C opcodes, the
                gdi_allowed set, every GET_DYNAMIC_INFO record length, the
                unsolicited kind -> {command_type, body} map, and every
                UPC_*_C entry claimed by a command or the shared list;
    classifier  protocol_processor_top.sv hz_classify, opcode -> class, and
                pp_pkg's hazard codes;
    scoreboard  KL_pp_scoreboard.sv hz_is_lockprot = the lock commands' classes;
    notify      KL_aecp_notify.sv's class -> unsolicited kind case;
    status      ucpu_pkg.sv ST_*_C codes;
    rom         the generated µcode, walked from every entry: a lock-protected
                command reaches CHECK_LOCK and no other does, its NOTIFY_ENQ
                class is the table's, every status a path sets is in its set
                and every status in the set is one a path can set, every
                response it sends is the table's size, and no path falls into
                the ROM fill;
    8.1         06 section 8.1 lists exactly the served commands.

Every line of a failure names the command and the consumer that disagree.

Exit 0 = every consumer agrees. Exit 1 = a disagreement, each printed. Exit 2 =
an input is unreadable or a pattern found nothing (a stale pattern is itself a
failure). `--write` regenerates F06.14 from the table. `--selftest` plants one
defect per check in a copy of the inputs and requires each to fail by name.
"""
import argparse
import copy
import importlib.util
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = "hdl/aecp/ucode/aecp_commands.json"
UCODE = "hdl/aecp/ucode/gen_ucode.py"
ENGINE = "hdl/aecp/KL_aecp_engine.sv"
NOTIFY = "hdl/aecp/KL_aecp_notify.sv"
UCPU_PKG = "hdl/aecp/ucpu_pkg.sv"
PP_PKG = "hdl/common/pp_pkg.sv"
SCOREBOARD = "hdl/packet_engine/KL_pp_scoreboard.sv"
TOP = "hdl/top/protocol_processor_top.sv"
DOC = "docs/architecture/06_aecp_engine.md"
TEXTS = (ENGINE, NOTIFY, UCPU_PKG, PP_PKG, SCOREBOARD, TOP, DOC)

SCHEMA = "aecp-command-table/1"
BEGIN = ("<!-- BEGIN F06.14: generated from hdl/aecp/ucode/aecp_commands.json by "
         "scripts/check_aecp_commands.py --write; edit the table, not these rows -->")
END = "<!-- END F06.14 -->"
COLUMNS = ("Opcode", "Command", "Mandate", "Scope rule", "Class", "Lock-prot.",
           "GDI", "Oversize", "Notif", "Cmd cdl", "Resp. size", "Status set")
SECTIONS = ("aem", "mvu", "gdi", "tail")
#! the µCPU's response record: BUILD_HEADER owns bytes 0..11 and the cursor
#! starts at 12, so a program's length at SEND_RESPONSE IS the cdl (06 section 8)
HDR = 12
VAR = "variable"


class GateError(Exception):
    """An input that cannot be read: exit 2, never a pass."""


# ---------------------------------------------------------------------------
# inputs
# ---------------------------------------------------------------------------

@dataclass
class Ucode:
    """What the generator built: the ROM words, which words a program placed,
    the place() targets, the E_* entry points and DISPATCH."""

    rom: list
    occupied: set
    placed: set
    entry: dict
    dispatch: dict
    ops: dict


@dataclass
class Inputs:
    """Everything the gate reads, so the self-test can mutate a copy."""

    table: dict
    texts: dict
    ucode: Ucode


def load_ucode(path: Path) -> Ucode:
    """Run gen_ucode.py as a module (it reads aecp_commands.json beside it) and
    keep what it built, writing no bytecode next to it."""
    dont = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec = importlib.util.spec_from_file_location("gen_ucode_for_gate", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except Exception as exc:  # any failure to generate is a refusal, never a pass
        raise GateError(f"{UCODE} did not generate: {exc!r}") from exc
    finally:
        sys.dont_write_bytecode = dont
    for name in ("rom", "occupied", "placed", "DISPATCH", "OPS"):
        if not hasattr(mod, name):
            raise GateError(f"{UCODE} defines no {name}: the generator's shape has "
                            "changed under this gate")
    entry = {k[2:]: v for k, v in vars(mod).items()
             if k.startswith("E_") and isinstance(v, int)}
    return Ucode(list(mod.rom), set(mod.occupied), set(mod.placed), entry,
                 copy.deepcopy(mod.DISPATCH), dict(mod.OPS))


def load(root: Path) -> Inputs:
    """Read the table, the RTL and doc texts, and the generated ROM under `root`."""
    try:
        raw = (root / TABLE).read_text(encoding="utf-8")
        texts = {p: (root / p).read_text(encoding="utf-8") for p in TEXTS}
    except OSError as exc:
        raise GateError(f"cannot read an input: {exc}") from exc
    try:
        table = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise GateError(f"{TABLE} is not JSON: {exc}") from exc
    return Inputs(table, texts, load_ucode(root / UCODE))


def strip_sv(text: str) -> str:
    """SystemVerilog without its // and /* */ comments."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


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


# ---------------------------------------------------------------------------
# the RTL consumers
# ---------------------------------------------------------------------------

RE_C16 = re.compile(r"localparam\s+logic\s*\[15:0\]\s+((?:OP|GDI|MVU)_[A-Z0-9_]+_C)"
                    r"\s*=\s*16'h([0-9A-Fa-f]{4})\s*;")
RE_DECL = re.compile(r"\b(?:localparam|parameter)\b[^;]*?\b((?:OP|GDI)_[A-Z0-9_]+_C)\s*=")
RE_UPC = re.compile(r"localparam\s+logic\s*\[10:0\]\s+UPC_([A-Z0-9_]+)_C\s*=\s*11'd(\d+)\s*;")


def engine_facts(sv: str) -> dict:
    """The engine's command constants, entries, GET_DYNAMIC_INFO tables and
    unsolicited map, each parsed from comment-free text."""
    code = strip_sv(sv)
    consts = {}
    for m in RE_C16.finditer(code):
        consts[m.group(1)] = int(m.group(2), 16)
    unparsed = sorted({m.group(1) for m in RE_DECL.finditer(code)} - set(consts))
    upc = {m.group(1): int(m.group(2)) for m in RE_UPC.finditer(code)}
    allowed = block(code, r"function\s+automatic\s+logic\s+gdi_allowed\b",
                    r"\bendfunction\b", f"{ENGINE} gdi_allowed")
    m = re.search(r"unique\s+case\s*\(\s*command_type\s*\)(.*?):\s*gdi_allowed\s*=\s*1'b1",
                  allowed, re.S)
    if not m:
        raise GateError(f"{ENGINE}: the gdi_allowed member list is not parseable")
    gdi_allowed = set(re.findall(r"\b((?:OP|GDI)_[A-Z0-9_]+_C)\b", m.group(1)))
    sub = block(code, r"always_comb\s+begin\s*:\s*gdi_subcommand_decode\b.*?"
                r"unique\s+case\s*\(\s*g_rec_cmd_r\s*\)", r"\bendcase\b",
                f"{ENGINE} gdi_subcommand_decode")
    records = {}
    for lab, arm in case_arms(sub, r"OP_[A-Z0-9_]+_C"):
        lens = set(re.findall(r"g_sub_rlen_w\s*=\s*11'd(\d+)", arm))
        records[lab] = (int(lens.pop()) if len(lens) == 1 else None,
                        set(re.findall(r"\bUPC_([A-Z0-9_]+)_C\b", arm)))
    uns = block(code, r"always_comb\s+begin\s*:\s*uns_kind_map\b", r"\bendcase\b",
                f"{ENGINE} uns_kind_map")
    unsol = {}
    for lab, arm in case_arms(uns, r"PP_UNS_[A-Z0-9_]+_C"):
        unsol[lab] = (set(re.findall(r"\b(OP_[A-Z0-9_]+_C)\b", arm)),
                      set(re.findall(r"\bUPC_([A-Z0-9_]+)_C\b", arm)) - {"NOSEND"})
    if not consts or not upc or not records or not unsol:
        raise GateError(f"{ENGINE}: a pattern parsed nothing")
    return dict(consts=consts, unparsed=unparsed, upc=upc, gdi_allowed=gdi_allowed,
                records=records, unsol=unsol)


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


# ---------------------------------------------------------------------------
# the ROM consumer: every path of a program, abstractly
# ---------------------------------------------------------------------------

@dataclass
class Facts:
    """What every path from one entry does: (status, length) at each
    SEND_RESPONSE, whether it reaches CHECK_LOCK, the NOTIFY_ENQ classes, and
    any word of fill it runs into."""

    sends: set = field(default_factory=set)
    lock: bool = False
    notify: set = field(default_factory=set)
    fill: set = field(default_factory=set)


def walk(uc: Ucode, start: int, faces: bool) -> Facts:
    """Every path from `start` through the generated ROM, with the µCPU's own
    semantics (KL_aecp_ucpu.sv): the status register starts SUCCESS, a SET_STATUS
    replaces it, a taken CHECK_LOCK / CHECK_ARG / MAP_VALIDATE writes its refusal,
    BRANCH_IF_STATUS 0 splits on SUCCESS, and the length is the response cursor
    (12 + every field). With `faces`, a DESC_ADDR, READ_STATE or WRITE_STATE may
    also come back with its error status. Constants MOVEd into a register are
    tracked so a counted ITER loop and a COPY_BUFFER of a known length stay
    exact; anything else widens the length to `variable`."""
    n = {v: k for k, v in uc.ops.items()}
    width = {0: 1, 1: 2, 2: 4, 3: 8}
    facts = Facts()
    seen = {}
    work = [(start, frozenset({0}), HDR, ((0, 0),), None, False)]
    while work:
        pc, st, ln, regs, it, ovf = work.pop()
        key = (pc, st, regs, it, ovf)
        if key in seen and (seen[key] == ln or seen[key] == VAR):
            continue
        if key in seen:
            ln = VAR
        seen[key] = ln
        if not 0 <= pc < len(uc.rom) or pc not in uc.occupied:
            facts.fill.add(pc)
            continue
        w = uc.rom[pc]
        op = n.get(w >> 43)
        rd, ra, fmt, cnd, imm = (w >> 39) & 15, (w >> 35) & 15, (w >> 28) & 7, (w >> 24) & 15, w & 0xFFFFFF
        tgt = imm & 0x7FF
        rv = dict(regs)
        nxt = pc + 1

        def go(p, s=st, l=ln, r=None, i=it, o=ovf):
            work.append((p, frozenset(s), l, tuple(sorted((r if r is not None else rv).items())), i, o))

        if op == "END":
            continue
        if op == "BRANCH":
            go(tgt)
            continue
        if op == "BR_STATUS":
            if cnd == 0:
                if st - {0}:
                    go(tgt, s=st - {0})
                if st & {0}:
                    go(nxt, s=st & {0})
            elif cnd == 1 and it is not None and it[0] is not None:
                go(tgt if it[1] == it[0] else nxt)
            elif cnd == 4 and ovf is not None and ln != VAR:
                go(tgt if ovf else nxt)
            elif cnd in (1, 2, 3, 4):
                go(tgt)
                go(nxt)
            else:
                go(nxt)
            continue
        if op == "CHECK_LOCK":
            facts.lock = True
            go(tgt, s={3})
            go(nxt)
            continue
        if op == "CHECK_ARG":
            go(tgt, s={11 if cnd & 8 else 7})
            go(nxt)
            continue
        if op == "MAP_VALID":
            go(tgt, s={7})
            go(nxt)
            continue
        if op == "SET_STATUS":
            st = frozenset({imm & 31})
        elif op in ("DESC_ADDR", "READ_ST", "WRITE_ST") and faces:
            st = st | {{"DESC_ADDR": 2, "READ_ST": 7, "WRITE_ST": 10}[op]}
        if op == "MOVE":
            if ra == 0:
                rv[rd] = imm
            elif ra in rv:
                rv[rd] = rv[ra] & ((1 << (8 * width[fmt])) - 1)
            else:
                rv.pop(rd, None)
        elif op == "SHIFT_R":
            if ra in rv:
                rv[rd] = rv[ra] >> (imm & 63)
            else:
                rv.pop(rd, None)
        elif op in ("SET_MASKED", "READ_ST", "NAME_RD", "GATHER_EXT"):
            rv.pop(rd, None)
        elif op == "ITER_OPEN":
            #! a count the walk does not know leaves the index untracked too,
            #! so the loop widens to `variable` instead of unrolling 256 times
            it = (rv[ra] & 0xFF, 0) if ra in rv else (None, None)
        elif op == "ITER_NEXT" and it is not None and it[0] is not None:
            it = (it[0], (it[1] + 1) & 0xFF)
        elif op in ("BUILD_FLD", "APPEND") and ln != VAR:
            cap = 592 if (op == "APPEND" and cnd & 1) else 524
            if op == "APPEND" and ln + width[fmt] > cap:
                ovf = True
            else:
                ln += width[fmt]
        elif op == "READ_CTRS" and ln != VAR:
            ln += 16
        elif op == "COPY_BUF":
            count = None if ra not in rv else (
                (rv[ra] & 0xFFFF) - (imm & 0xFFF) if cnd & 1 else rv[ra] & 0xFFFF)
            ln = ln + count if (count is not None and ln != VAR and count >= 0) else VAR
        elif op == "SET_LENGTH":
            ln = imm & 0x7FF
        elif op == "SEND_RESP":
            for s in st:
                facts.sends.add((s, ln))
        elif op == "NOTIFY_ENQ":
            facts.notify.add(imm & 15)
        if ln == VAR:
            ovf = None
        go(nxt, s=st, l=ln, i=it, o=ovf)
    return facts


def entry_facts(uc: Ucode, at: int) -> tuple:
    """(definite, with-faces) facts from one entry."""
    return walk(uc, at, False), walk(uc, at, True)


# ---------------------------------------------------------------------------
# the table
# ---------------------------------------------------------------------------

REQUIRED = ("name", "message", "opcode", "rtl", "clause", "command", "response", "status",
            "hazard", "lock", "notify", "gdi", "oversize", "entries", "doc")


def opcode_of(text) -> int:
    """'0x0015' -> 0x15; anything else -> -1."""
    if isinstance(text, str) and re.fullmatch(r"0x[0-9A-F]{4}", text):
        return int(text, 16)
    return -1


def check_schema(t: dict) -> list:
    """The table's own shape: the fields every consumer reads, spelled once."""
    p = []
    if t.get("schema") != SCHEMA:
        p.append(f"  schema: {TABLE} declares {t.get('schema')!r}, this gate reads {SCHEMA!r}")
    codes = t.get("status_codes", {})
    hz = t.get("hazard_classes", {})
    cmds = t.get("commands", [])
    if not codes or not hz or not cmds:
        return p + ["  schema: status_codes, hazard_classes or commands is empty"]
    names, ops = {}, {}
    for i, c in enumerate(cmds):
        who = c.get("name", f"commands[{i}]")
        miss = [k for k in REQUIRED if k not in c]
        if miss:
            p.append(f"  schema: {who}: missing {', '.join(miss)}")
            continue
        if c["name"] in names:
            p.append(f"  schema: {who} is listed twice")
        names[c["name"]] = c
        if c["message"] not in ("AEM", "MVU"):
            p.append(f"  schema: {who}: message {c['message']!r} is neither AEM nor MVU")
        if c["message"] not in t.get("common_status", {}):
            p.append(f"  schema: {who}: message {c['message']!r} has no common_status row")
        op = opcode_of(c["opcode"])
        if op < 0:
            p.append(f"  schema: {who}: opcode {c['opcode']!r} is not 0xHHHH")
        elif (c["message"], op) in ops:
            p.append(f"  schema: {who}: opcode {c['opcode']} is also "
                     f"{ops[(c['message'], op)]}'s")
        ops[(c["message"], op)] = who
        for s in c["status"] + c.get("engine_status", []):
            if s not in codes:
                p.append(f"  schema: {who}: status {s} is not in status_codes")
        if c["hazard"] not in hz:
            p.append(f"  schema: {who}: hazard {c['hazard']} is not in hazard_classes")
        if not isinstance(c["lock"], bool) or not isinstance(c["gdi"], bool) \
                or not isinstance(c["oversize"], bool):
            p.append(f"  schema: {who}: lock, gdi and oversize must be true or false")
        for k in ("command", "response"):
            if not isinstance(c[k].get("cdl"), int):
                p.append(f"  schema: {who}: {k} has no integer cdl")
        if not c["entries"] or len(set(c["entries"])) != len(c["entries"]):
            p.append(f"  schema: {who}: entries must be a non-empty list without repeats")
        nt = c["notify"]
        if nt is not None and not (isinstance(nt, dict) and {"class", "kind", "body"} <= set(nt)):
            p.append(f"  schema: {who}: notify must be null or carry class, kind and body")
    for d in t.get("documented", []):
        if d.get("section") not in SECTIONS or len(d.get("cells", [])) != len(COLUMNS) - 2:
            p.append(f"  schema: documented row {d.get('opcode')!r}: needs a section of "
                     f"{SECTIONS} and {len(COLUMNS) - 2} cells")
    return p


# ---------------------------------------------------------------------------
# the checks
# ---------------------------------------------------------------------------

def check_engine(t: dict, eng: dict) -> list:
    """Opcodes, GET_DYNAMIC_INFO membership and record lengths, entries and the
    unsolicited map, against KL_aecp_engine.sv."""
    p = []
    consts = eng["consts"]
    for name in eng["unparsed"]:
        p.append(f"  engine: {name} is not written `localparam logic [15:0] {name} = "
                 "16'hXXXX;`, so its opcode cannot be held to the table")
    claimed = {}
    for c in t["commands"]:
        claimed[c["rtl"]] = c
        have = consts.get(c["rtl"])
        if have is None:
            p.append(f"  engine: {c['name']}: the table names {c['rtl']} and "
                     f"{ENGINE} declares no such constant")
        elif have != opcode_of(c["opcode"]):
            p.append(f"  engine: {c['name']}: the table says opcode {c['opcode']}, "
                     f"{ENGINE} {c['rtl']} = 0x{have:04X}")
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
    want = {c["rtl"] for c in t["commands"] if c["gdi"]} | \
           {d["rtl"] for d in t.get("documented", []) if d.get("gdi") and d.get("rtl")}
    for name in sorted(want - eng["gdi_allowed"]):
        p.append(f"  engine: gdi_allowed: the table allows {name} inside "
                 "GET_DYNAMIC_INFO and the engine refuses it")
    for name in sorted(eng["gdi_allowed"] - want):
        p.append(f"  engine: gdi_allowed: the engine allows {name} inside "
                 "GET_DYNAMIC_INFO and the table does not")
    for c in t["commands"]:
        rec = eng["records"].get(c["rtl"])
        if c["gdi"] and rec is None:
            p.append(f"  engine: {c['name']}: a GET_DYNAMIC_INFO member with no "
                     "gdi_subcommand_decode arm")
        elif rec is not None and not c["gdi"]:
            p.append(f"  engine: {c['name']}: gdi_subcommand_decode runs it inside "
                     "GET_DYNAMIC_INFO and the table says it is no member")
        elif rec is not None:
            rlen, upcs = rec
            if rlen != c["response"]["cdl"] - HDR:
                p.append(f"  engine: {c['name']}: its GET_DYNAMIC_INFO record is {rlen} "
                         f"bytes, the table's response is cdl {c['response']['cdl']} "
                         f"({c['response']['cdl'] - HDR} bytes)")
            for u in sorted(upcs - set(c["entries"])):
                p.append(f"  engine: {c['name']}: GET_DYNAMIC_INFO dispatches it to "
                         f"UPC_{u}_C, which is not one of its entries")
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
    kinds = {}
    for c in t["commands"]:
        nt = c["notify"]
        if not nt:
            continue
        kinds.setdefault(nt["kind"], []).append(c["name"])
        arm = eng["unsol"].get(nt["kind"])
        if arm is None:
            p.append(f"  engine: {c['name']}: uns_kind_map has no {nt['kind']} arm")
            continue
        if c["rtl"] not in arm[0]:
            p.append(f"  engine: {c['name']}: the {nt['kind']} job carries "
                     f"{sorted(arm[0])}, not {c['rtl']}")
        if arm[1] != {nt["body"]}:
            p.append(f"  engine: {c['name']}: the {nt['kind']} job's body is "
                     f"{sorted(arm[1])}, the table says {nt['body']}")
    for k in sorted(set(eng["unsol"]) - set(kinds)):
        p.append(f"  engine: uns_kind_map's {k} arm is no command's notification")
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


def size_text(s: dict, octets: bool) -> str:
    """A size rule as text: cdl, or AECPDU octets (12 + cdl)."""
    base = s["cdl"] + (HDR if octets else 0)
    out = str(base)
    if s.get("per"):
        out += f" + {s['per']}·N"
    if s.get("plus"):
        out += f" + {s['plus']}"
    return out


def fits(ln, rsp: dict, echo: bool) -> bool:
    """Whether one program's length at SEND_RESPONSE answers the table's
    response rule: a header-only echo, a variable or fixed instance of a
    variable rule, the fixed cdl, or the rule's failure stub."""
    base = rsp["cdl"]
    if ln == HDR and echo:
        return True
    if ln == VAR:
        return bool(rsp.get("per") or rsp.get("plus"))
    if rsp.get("stub") == ln:
        return True
    if rsp.get("plus"):
        return ln >= base
    if rsp.get("per"):
        return ln >= base and (ln - base) % rsp["per"] == 0
    return ln == base


def check_rom(t: dict, uc: Ucode) -> list:
    """Walk every entry of every command through the generated ROM."""
    p = []
    code_of = {r["code"]: n for n, r in t["status_codes"].items()}
    echo_shared = set(t.get("echo_entries", []))
    for c in t["commands"]:
        name, rsp = c["name"], c["response"]
        #! an engine-built answer (GET_DYNAMIC_INFO's aggregate) has statuses no
        #! program sets; they are named, and only such a command may name them
        common = set(t["common_status"].get(c["message"], [])) | set(c.get("engine_status", []))
        if c.get("engine_status") and not rsp.get("engine"):
            p.append(f"  rom: {name}: engine_status is only for an engine-built response")
        if uc.dispatch.get(name) != {e: uc.entry.get(e) for e in c["entries"]}:
            p.append(f"  rom: {name}: gen_ucode.py DISPATCH is "
                     f"{uc.dispatch.get(name)}, the table's entries resolve to "
                     f"{ {e: uc.entry.get(e) for e in c['entries']} }")
        definite, faced, lock, notes = set(), set(), False, set()
        for e in c["entries"]:
            at = uc.entry.get(e)
            if at is None or at not in uc.placed:
                p.append(f"  rom: {name}: entry {e} is no placed program in {UCODE}")
                continue
            d, m = entry_facts(uc, at)
            for pc in sorted(d.fill | m.fill):
                p.append(f"  rom: {name}: a path from E_{e} runs into ROM word {pc}, "
                         "which no program placed (the fill)")
            definite |= {s for s, _ in d.sends}
            faced |= {s for s, _ in m.sends}
            lock |= m.lock
            notes |= m.notify
            for s, ln in sorted(m.sends, key=str):
                if not fits(ln, rsp, e in echo_shared or bool(rsp.get("echo"))):
                    p.append(f"  rom: {name}: E_{e} answers {code_of.get(s, s)} at cdl "
                             f"{ln}, the table's response is {size_text(rsp, False)}"
                             f"{' (echo)' if rsp.get('echo') else ''}")
        for s in sorted(definite | faced):
            if s not in code_of:
                p.append(f"  rom: {name}: a path sets status {s}, which the table "
                         "does not know")
        must = {code_of[s] for s in definite if s in code_of}
        may = {code_of[s] for s in faced if s in code_of}
        listed = set(c["status"])
        for s in sorted(must - listed - common):
            p.append(f"  rom: {name}: status {s} is answered by a path of its programs "
                     "and missing from the table's status set")
        for s in sorted(listed - may - common):
            p.append(f"  rom: {name}: status {s} is in the table's status set and no "
                     "path of its programs can answer it")
        if lock != c["lock"]:
            p.append(f"  rom: {name}: the table says lock {str(c['lock']).lower()}, its "
                     f"programs {'do' if lock else 'never'} reach CHECK_LOCK")
        want = {c["notify"]["class"]} if c["notify"] and c["notify"]["class"] is not None else set()
        if notes != want:
            p.append(f"  rom: {name}: its programs enqueue notification class "
                     f"{sorted(notes) or 'none'}, the table says {sorted(want) or 'none'}")
        if c["notify"]:
            body = c["notify"]["body"]
            at = uc.entry.get(body)
            if at is None or at not in uc.placed:
                p.append(f"  rom: {name}: notification body {body} is no placed program")
            else:
                for s, ln in sorted(walk(uc, at, True).sends, key=str):
                    if not fits(ln, {k: v for k, v in rsp.items() if k != "stub"},
                                bool(rsp.get("echo"))):
                        p.append(f"  rom: {name}: its unsolicited body E_{body} is cdl "
                                 f"{ln}, the table's response is {size_text(rsp, False)}")
    return p


# ---------------------------------------------------------------------------
# the documents
# ---------------------------------------------------------------------------

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
        out += f", N {r['unit']}s"
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


def rows(t: dict) -> list:
    """F06.14's rows, generated: (opcode, command, cells...) per row."""
    out = []
    for sec in SECTIONS:
        group = []
        for c in t["commands"]:
            if {"AEM": "aem", "MVU": "mvu"}[c["message"]] != sec:
                continue
            op = c["opcode"] if c["message"] == "AEM" else f"MVU {c['opcode']}"
            cls = c["hazard"] + (f" {c['doc']['class']}" if c["doc"].get("class") else "")
            cmd = c["command"]
            cmd_text = size_text(cmd, False) + (f", at most {cmd['max']}" if cmd.get("max") else "")
            group.append((opcode_of(c["opcode"]), 0, [
                op, c["name"], c["doc"]["mandate"], c["doc"]["scope"], cls, yes(c["lock"]),
                yes(c["gdi"]), yes(c["oversize"]), notif_text(c), cmd_text, resp_text(c),
                ", ".join(c["status"])]))
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


def check(inp: Inputs) -> list:
    """Every disagreement, one line each; empty means the gate passes."""
    t = inp.table
    p = check_schema(t)
    if p:
        return p
    eng = engine_facts(inp.texts[ENGINE])
    default, cls = classifier(inp.texts[TOP])
    p += check_engine(t, eng)
    p += check_classifier(t, default, cls, hazard_codes(inp.texts[PP_PKG]))
    p += check_scoreboard(t, lockprot(inp.texts[SCOREBOARD]))
    p += check_notify(t, *notify_classes(inp.texts[NOTIFY]))
    p += check_status_codes(t, status_codes(inp.texts[UCPU_PKG]))
    p += check_rom(t, inp.ucode)
    p += check_doc(t, inp.texts[DOC])
    p += check_inventory(t, inp.texts[DOC])
    return p


# ---------------------------------------------------------------------------
# the self-test: one planted defect per check
# ---------------------------------------------------------------------------

def cmd(inp: Inputs, name: str) -> dict:
    """The table row of `name` in a mutated copy."""
    return next(c for c in inp.table["commands"] if c["name"] == name)


def sub(inp: Inputs, path: str, old: str, new: str) -> None:
    """Replace the one occurrence of `old` in an input text; a fixture that no
    longer matches is itself a self-test failure."""
    text = inp.texts[path]
    if text.count(old) != 1:
        raise GateError(f"selftest fixture: {old!r} occurs {text.count(old)} times in {path}")
    inp.texts[path] = text.replace(old, new)


def reachable(uc: Ucode, start: int) -> list:
    """Every ROM word a path from `start` can execute, in address order."""
    n = {v: k for k, v in uc.ops.items()}
    seen, work = set(), [start]
    while work:
        pc = work.pop()
        if pc in seen or pc not in uc.occupied:
            continue
        seen.add(pc)
        op = n.get(uc.rom[pc] >> 43)
        if op == "END":
            continue
        if op in ("BRANCH", "BR_STATUS", "CHECK_LOCK", "CHECK_ARG", "MAP_VALID"):
            work.append(uc.rom[pc] & 0x7FF)
        if op != "BRANCH":
            work.append(pc + 1)
    return sorted(seen)


def rom_op(inp: Inputs, entry: str, op: str, nth: int = 0) -> int:
    """The address of the nth `op` word a path from E_entry can execute."""
    code = inp.ucode.ops[op]
    hits = [pc for pc in reachable(inp.ucode, inp.ucode.entry[entry])
            if inp.ucode.rom[pc] >> 43 == code]
    if len(hits) <= nth:
        raise GateError(f"selftest fixture: fewer than {nth + 1} {op} from E_{entry}")
    return hits[nth]


def drop_all(inp: Inputs, entry: str, op: str) -> None:
    """Turn every `op` a path from E_entry can execute into a NOP (op 0)."""
    code = inp.ucode.ops[op]
    for pc in reachable(inp.ucode, inp.ucode.entry[entry]):
        if inp.ucode.rom[pc] >> 43 == code:
            inp.ucode.rom[pc] = 0


def into_fill(inp: Inputs, entry: str) -> None:
    """Make the word after E_entry's SEND_RESPONSE branch into the ROM fill."""
    free = max(set(range(len(inp.ucode.rom))) - inp.ucode.occupied)
    at = rom_op(inp, entry, "SEND_RESP") + 1
    inp.ucode.rom[at] = (inp.ucode.ops["BRANCH"] << 43) | free


def set_imm(inp: Inputs, at: int, imm: int) -> None:
    """Rewrite one ROM word's immediate."""
    inp.ucode.rom[at] = (inp.ucode.rom[at] & ~0xFFFFFF) | imm


def doc_cell(inp: Inputs, row: str, old: str, new: str) -> None:
    """Edit one cell of one F06.14 row (the doc-only drift)."""
    text = inp.texts[DOC]
    line = next(x for x in text.split("\n") if x.startswith(f"| {row} |"))
    if line.count(old) != 1:
        raise GateError(f"selftest fixture: {old!r} not once in F06.14 row {row}")
    inp.texts[DOC] = text.replace(line, line.replace(old, new))


def mutants() -> list:
    """(what, mutation, words each failure line must carry) per planted defect."""
    return [
        ("an opcode renumbered in the table only",
         lambda i: cmd(i, "GET_SAMPLING_RATE").update(opcode="0x001E"),
         ("engine: GET_SAMPLING_RATE", "0x001E")),
        ("a size changed in the doc table only",
         lambda i: doc_cell(i, "0x0015", "32 B (cdl 20)", "36 B (cdl 24)"),
         ("F06.14: row 0x0015 GET_SAMPLING_RATE, column Resp. size",)),
        ("an OP_*_C changed in the RTL only",
         lambda i: sub(i, ENGINE, "OP_GET_SAMP_RATE_C   = 16'h0015;",
                         "OP_GET_SAMP_RATE_C   = 16'h001E;"),
         ("engine: GET_SAMPLING_RATE", "OP_GET_SAMP_RATE_C = 0x001E")),
        ("a command missing from the table",
         lambda i: i.table["commands"].remove(cmd(i, "GET_AS_PATH")),
         ("OP_GET_AS_PATH_C = 0x0028 and the table has no command",)),
        ("an OP_*_C written in another form",
         lambda i: sub(i, ENGINE, "localparam logic [15:0] OP_GET_AS_PATH_C     = 16'h0028;",
                         "localparam int unsigned OP_GET_AS_PATH_C = 40;"),
         ("OP_GET_AS_PATH_C is not written",)),
        ("a GDI_*_C the table does not document",
         lambda i: i.table["documented"].remove(
             next(d for d in i.table["documented"] if d.get("rtl") == "GDI_GET_MEM_LEN_C")),
         ("GDI_GET_MEM_LEN_C = 0x0048 and the table documents no such",)),
        ("a GET_DYNAMIC_INFO member the engine refuses",
         lambda i: sub(i, ENGINE, "GDI_GET_SIGNAL_SEL_C, OP_GET_COUNTERS_C, GDI_GET_MEM_LEN_C,",
                         "GDI_GET_SIGNAL_SEL_C, GDI_GET_MEM_LEN_C,"),
         ("gdi_allowed: the table allows OP_GET_COUNTERS_C",)),
        ("a GET_DYNAMIC_INFO record length changed in the RTL",
         lambda i: sub(i, ENGINE, "g_sub_exec_w = 1'b1; g_sub_rlen_w = 11'd136;",
                         "g_sub_exec_w = 1'b1; g_sub_rlen_w = 11'd132;"),
         ("engine: GET_COUNTERS: its GET_DYNAMIC_INFO record is 132 bytes",)),
        ("a GET_DYNAMIC_INFO flag changed in the table",
         lambda i: cmd(i, "GET_NAME").update(gdi=False),
         ("gdi_allowed: the engine allows OP_GET_NAME_C",)),
        ("an entry the engine cannot dispatch to",
         lambda i: cmd(i, "GET_CLOCK_SOURCE")["entries"].append("SCFGRUN2"),
         ("GET_CLOCK_SOURCE: entry SCFGRUN2 has no UPC_SCFGRUN2_C",)),
        ("an engine entry no command claims",
         lambda i: cmd(i, "GET_CONTROL")["entries"].remove("NSUPP1")
           or cmd(i, "SET_CONTROL")["entries"].remove("NSUPP1"),
         ("UPC_NSUPP1_C is claimed by no command",)),
        ("an unsolicited job carrying another command_type",
         lambda i: sub(i, ENGINE, "PP_UNS_CFG_C:   begin uns_ct_w = OP_SET_CONFIG_C;",
                         "PP_UNS_CFG_C:   begin uns_ct_w = OP_GET_CONFIG_C;"),
         ("SET_CONFIGURATION: the PP_UNS_CFG_C job carries ['OP_GET_CONFIG_C']",)),
        ("an unsolicited body changed in the table",
         lambda i: cmd(i, "SET_CLOCK_SOURCE")["notify"].update(body="GSRATE"),
         ("SET_CLOCK_SOURCE: the PP_UNS_CLKS_C job's body is ['GCLKS']",)),
        ("a hazard class changed in the top",
         lambda i: sub(i, TOP, "16'h0010: begin                        // SET_NAME\n"
                         "          hz_class_w = 4'(PP_HZ_NAME_WR);",
                         "16'h0010: begin                        // SET_NAME\n"
                         "          hz_class_w = 4'(PP_HZ_STREAM_CFG);"),
         ("classifier: SET_NAME: the table says NAME_WR", "0x0010 STREAM_CFG")),
        ("a hazard class changed in the table",
         lambda i: cmd(i, "GET_COUNTERS").update(hazard="MAP_CFG"),
         ("classifier: GET_COUNTERS: the table says MAP_CFG",)),
        ("a classified opcode no command carries",
         lambda i: i.table["commands"].remove(cmd(i, "GET_DYNAMIC_INFO")),
         ("hz_classify names 0x004B",)),
        ("a hazard code changed in pp_pkg",
         lambda i: sub(i, PP_PKG, "PP_HZ_IDENTIFY    = 4'd8", "PP_HZ_IDENTIFY    = 4'd9"),
         ("classifier: hazard class IDENTIFY is 8 in the table and 9",)),
        ("a lock flag changed in the table",
         lambda i: cmd(i, "SET_CONTROL").update(lock=False),
         ("rom: SET_CONTROL: the table says lock false",)),
        ("CHECK_LOCK dropped from a program",
         lambda i: drop_all(i, "SCLKS", "CHECK_LOCK"),
         ("rom: SET_CLOCK_SOURCE: the table says lock true", "never reach CHECK_LOCK")),
        ("the scoreboard's lock-protected set changed",
         lambda i: sub(i, SCOREBOARD, "|| (c == HZ_NAME_WR_C)     ", ""),
         ("scoreboard:", "hz_is_lockprot")),
        ("a notification class changed in the table",
         lambda i: cmd(i, "SET_SAMPLING_RATE")["notify"].update(**{"class": 8}),
         ("rom: SET_SAMPLING_RATE: its programs enqueue notification class [5]",)),
        ("a NOTIFY_ENQ class changed in a program",
         lambda i: set_imm(i, rom_op(i, "SCTRL", "NOTIFY_ENQ"), 2),
         ("rom: SET_CONTROL: its programs enqueue notification class [2]",)),
        ("the notify block's class map changed",
         lambda i: sub(i, NOTIFY, "4'd8: pick_kind_w = PP_UNS_CLKS_C;",
                         "4'd8: pick_kind_w = PP_UNS_SRATE_C;"),
         ("notify: SET_CLOCK_SOURCE: class 8 is PP_UNS_SRATE_C",)),
        ("a status dropped from the table",
         lambda i: cmd(i, "SET_CLOCK_SOURCE")["status"].remove("ENTITY_LOCKED"),
         ("rom: SET_CLOCK_SOURCE: status ENTITY_LOCKED is answered",)),
        ("a status the programs cannot answer",
         lambda i: cmd(i, "ENTITY_AVAILABLE")["status"].append("NO_RESOURCES"),
         ("rom: ENTITY_AVAILABLE: status NO_RESOURCES is in the table",)),
        ("a refusal status changed in a program",
         lambda i: set_imm(i, rom_op(i, "SCFGRUN", "SET_STATUS"), 8),
         ("rom: SET_CONFIGURATION: status NO_RESOURCES is answered",)),
        ("a status code changed in ucpu_pkg",
         lambda i: sub(i, UCPU_PKG, "ST_NO_RESOURCES_C   = 5'd8;", "ST_NO_RESOURCES_C   = 5'd9;"),
         ("status: NO_RESOURCES is 8 in the table and ST_NO_RESOURCES_C = 9",)),
        ("a response size changed in the table",
         lambda i: cmd(i, "GET_CLOCK_SOURCE")["response"].update(cdl=24),
         ("rom: GET_CLOCK_SOURCE: E_GCLKS answers SUCCESS at cdl 20",)),
        ("a field dropped from a program",
         lambda i: i.ucode.rom.__setitem__(rom_op(i, "GSTRI", "BUILD_FLD", 3), 0),
         ("rom: GET_STREAM_INFO: E_GSTRI answers SUCCESS at cdl",)),
        ("an unsolicited body of another size",
         lambda i: i.ucode.rom.__setitem__(rom_op(i, "STRMUNS", "BUILD_FLD"), 0),
         ("unsolicited body E_STRMUNS is cdl 12",)),
        ("a path into the ROM fill",
         lambda i: into_fill(i, "EAVL"),
         ("rom: ENTITY_AVAILABLE: a path from E_EAVL runs into ROM word",)),
        ("DISPATCH out of step with the table",
         lambda i: i.ucode.dispatch["GET_CONFIGURATION"].update(GCFG=1056),
         ("rom: GET_CONFIGURATION: gen_ucode.py DISPATCH",)),
        ("section 8.1 missing a served command",
         lambda i: sub(i, DOC, "| 0x0002 ENTITY_AVAILABLE |", "| ENTITY_AVAILABLE |"),
         ("8.1: ENTITY_AVAILABLE (AEM 0x0002) is served",)),
        ("a row missing from F06.14",
         lambda i: setattr(i, "texts", {**i.texts, DOC: "\n".join(
             x for x in i.texts[DOC].split("\n") if not x.startswith("| 0x0029 |"))}),
         ("F06.14: row 0x0029 GET_COUNTERS is missing",)),
        ("two commands on one opcode",
         lambda i: cmd(i, "STOP_STREAMING").update(opcode="0x0022"),
         ("schema: STOP_STREAMING: opcode 0x0022 is also START_STREAMING's",)),
        ("a status the table does not define",
         lambda i: cmd(i, "GET_NAME")["status"].append("ENTITY_ACQUIRED"),
         ("schema: GET_NAME: status ENTITY_ACQUIRED is not in status_codes",)),
    ]


def selftest(root: Path) -> int:
    """The tree passes, and each planted defect fails with its words."""
    base = load(root)
    first = check(base)
    if first:
        print("AECP COMMAND GATE SELFTEST: FAIL - the unmutated tree does not pass:")
        print("\n".join(first))
        return 1
    cases = mutants()
    bad = 0
    for what, fn, words in cases:
        inp = Inputs(copy.deepcopy(base.table), dict(base.texts), copy.deepcopy(base.ucode))
        try:
            fn(inp)
            got = check(inp)
        except GateError as exc:
            got = [f"  (gate refused: {exc})"]
        hit = [g for g in got if all(w in g for w in words)]
        if not hit:
            print(f"SELFTEST FAIL: '{what}' was not caught by name {words}:")
            print("\n".join(got[:6]) or "  (no finding at all)")
            bad += 1
    print(f"aecp commands selftest: {len(cases)} planted defects, "
          f"{len(cases) - bad} caught by name, {'OK' if not bad else 'FAIL'}")
    return 1 if bad else 0


# ---------------------------------------------------------------------------

def write(root: Path) -> int:
    """Regenerate F06.14 from the table."""
    inp = load(root)
    p = check_schema(inp.table)
    if p:
        print("\n".join(p))
        return 1
    doc = inp.texts[DOC]
    _, a, b = doc_block(doc)
    new = doc[:a] + "\n" + "\n".join(render(inp.table)) + "\n" + doc[b:]
    if new != doc:
        (root / DOC).write_text(new, encoding="utf-8")
        print(f"aecp commands: rewrote F06.14 in {DOC}")
    else:
        print(f"aecp commands: F06.14 in {DOC} is current")
    return 0


def main() -> int:
    """The gate; `--selftest` first proves it can fail, `--write` regenerates."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true", help="plant each defect and require it caught")
    ap.add_argument("--write", action="store_true", help="regenerate F06.14 from the table")
    ap.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = ap.parse_args()
    try:
        if args.selftest:
            return selftest(args.root)
        if args.write:
            return write(args.root)
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
          f"{sum(len(c['entries']) for c in t['commands'])} entries walked, "
          f"F06.14 {len(rows(t))} rows, OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
