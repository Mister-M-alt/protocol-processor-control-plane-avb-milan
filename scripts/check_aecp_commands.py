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

Every line of a failure names the command and the consumer that disagree.

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
import copy
import hashlib
import importlib.util
import json
import math
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = "hdl/aecp/ucode/aecp_commands.json"
UCODE = "hdl/aecp/ucode/gen_ucode.py"
RECORD = "tb/aecp_cmds/decode.json"
ENGINE = "hdl/aecp/KL_aecp_engine.sv"
NOTIFY = "hdl/aecp/KL_aecp_notify.sv"
UCPU_PKG = "hdl/aecp/ucpu_pkg.sv"
PP_PKG = "hdl/common/pp_pkg.sv"
SCOREBOARD = "hdl/packet_engine/KL_pp_scoreboard.sv"
TOP = "hdl/top/protocol_processor_top.sv"
DOC = "docs/architecture/06_aecp_engine.md"
TEXTS = (ENGINE, NOTIFY, UCPU_PKG, PP_PKG, SCOREBOARD, TOP, DOC)
#! what tb/aecp_cmds' sweep is made of: the SystemVerilog it elaborates (held
#! by its code, comments and spacing ignored), the bench and the descriptor
#! image's inputs (by their bytes), and the generated ROM. decode.json records
#! each digest, so a record swept from another engine is refused by name.
SWEPT_SV = (PP_PKG, UCPU_PKG, "hdl/aecp/KL_aecp_ucpu.sv", "hdl/aecp/KL_aecp_desc_store.sv",
            "hdl/aecp/KL_aecp_dyn_state.sv", "hdl/aecp/KL_aecp_nvm_writer.sv",
            "hdl/aecp/KL_aecp_resp_buf.sv", ENGINE, "tb/aecp_cmds/aecp_cmds_wrap.sv")
SWEPT_RAW = ("tb/aecp_cmds/sim_main.cpp", "hdl/aecp/desc/milan_min.json",
             "hdl/aecp/desc/gen_desc_image.py")
ROM_KEY = "ucode.hex (generated)"

SCHEMA = "aecp-command-table/1"
RECORD_SCHEMA = "aecp-decode-record/1"
BEGIN = ("<!-- BEGIN F06.14: generated from hdl/aecp/ucode/aecp_commands.json by "
         "scripts/check_aecp_commands.py --write; edit the table, not these rows -->")
END = "<!-- END F06.14 -->"
COLUMNS = ("Opcode", "Command", "Mandate", "Scope rule", "Class", "Lock-prot.",
           "GDI", "Oversize", "Notif", "Cmd cdl", "Resp. size", "Status set")
SECTIONS = ("aem", "mvu", "gdi", "tail")
#! IEEE 1722.1-2021 Table 7-141, the AEM status codes: a code the table names
#! is held here, because a code no RTL constant carries (STREAM_IS_RUNNING)
#! reaches the ROM from the table alone
IEEE_STATUS = {"SUCCESS": 0, "NOT_IMPLEMENTED": 1, "NO_SUCH_DESCRIPTOR": 2,
               "ENTITY_LOCKED": 3, "ENTITY_ACQUIRED": 4, "NOT_AUTHENTICATED": 5,
               "AUTHENTICATION_DISABLED": 6, "BAD_ARGUMENTS": 7, "NO_RESOURCES": 8,
               "IN_PROGRESS": 9, "ENTITY_MISBEHAVING": 10, "NOT_SUPPORTED": 11,
               "STREAM_IS_RUNNING": 12}
#! the µCPU's response record: BUILD_HEADER owns bytes 0..11 and the cursor
#! starts at 12, so a program's length at SEND_RESPONSE IS the cdl (06 section 8)
HDR = 12
#! outcomes of a trial that are not a program: an answer the engine built
#! without one, a command it dropped, a trial that never ended
OUTCOMES = ("engine", "drop", "timeout")
#! distinct exact lengths a walk state keeps before it widens to a loop's
#! fixed part plus whole records
WIDEN = 4


class GateError(Exception):
    """An input that cannot be read: exit 2, never a pass."""


# ---------------------------------------------------------------------------
# inputs
# ---------------------------------------------------------------------------

@dataclass
class Ucode:
    """What the generator built: the ROM words, which words a program placed,
    the place() targets, and the E_* entry points."""

    rom: list
    occupied: set
    placed: set
    entry: dict
    ops: dict


@dataclass
class Inputs:
    """Everything the gate reads, so the self-test can mutate a copy. The
    generator runs only once the table has passed every check that does not
    need it (`ucode_of`); a copy made without the decode record reads it from
    `root` when it is needed."""

    table: dict
    texts: dict
    ucode: Ucode = None
    record: dict = None
    root: Path = ROOT
    swept: dict = None


def load_ucode(path: Path) -> Ucode:
    """Run gen_ucode.py as a module (it reads aecp_commands.json beside it) and
    keep what it built, writing no bytecode next to it."""
    dont = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec = importlib.util.spec_from_file_location("gen_ucode_for_gate", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except (Exception, SystemExit) as exc:  # any failure to generate is a refusal
        why = str(exc) if isinstance(exc, SystemExit) else repr(exc)
        raise GateError(f"{UCODE} did not generate: {why}") from exc
    finally:
        sys.dont_write_bytecode = dont
    for name in ("rom", "occupied", "placed", "OPS"):
        if not hasattr(mod, name):
            raise GateError(f"{UCODE} defines no {name}: the generator's shape has "
                            "changed under this gate")
    entry = {k[2:]: v for k, v in vars(mod).items()
             if k.startswith("E_") and isinstance(v, int)}
    return Ucode(list(mod.rom), set(mod.occupied), set(mod.placed), entry, dict(mod.OPS))


def read_json(root: Path, rel: str) -> dict:
    """One JSON input; unreadable or not JSON is a refusal."""
    try:
        raw = (root / rel).read_text(encoding="utf-8")
    except OSError as exc:
        raise GateError(f"cannot read an input: {exc}") from exc
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise GateError(f"{rel} is not JSON: {exc}") from exc


def load(root: Path, record: bool = True) -> Inputs:
    """Read the table, the decode record (unless it is about to be written)
    and the RTL and doc texts under `root`."""
    table = read_json(root, TABLE)
    record = read_json(root, RECORD) if record else {}
    try:
        texts = {p: (root / p).read_text(encoding="utf-8") for p in TEXTS}
        swept = {p: (root / p).read_bytes() for p in SWEPT_SV + SWEPT_RAW}
    except OSError as exc:
        raise GateError(f"cannot read an input: {exc}") from exc
    return Inputs(table, texts, None, record, root, swept)


def digests(swept: dict, uc: Ucode) -> dict:
    """The sweep's inputs, digested: path -> sha256. SystemVerilog by its
    code alone, so a comment does not ask for a new sweep; the ROM as the
    ucode.hex the suites load."""
    out = {}
    for path in SWEPT_SV + SWEPT_RAW:
        data = swept[path]
        if path in SWEPT_SV:
            data = " ".join(strip_sv(data.decode("utf-8")).split()).encode("utf-8")
        out[path] = hashlib.sha256(data).hexdigest()
    out[ROM_KEY] = hashlib.sha256("".join(f"{w:012x}\n" for w in uc.rom)
                                  .encode("ascii")).hexdigest()
    return out


def check_swept(inp: Inputs, rom: bool) -> list:
    """decode.json is a sweep of this tree's engine, bench and (with `rom`)
    ROM: a record swept from anything else is refused by name."""
    if inp.swept is None:
        inp.swept = {p: (inp.root / p).read_bytes() for p in SWEPT_SV + SWEPT_RAW}
    kept = inp.record.get("sweep", {}).get("inputs", {})
    have = digests(inp.swept, inp.ucode if rom else Ucode([], set(), set(), {}, {}))
    p = []
    for path in SWEPT_SV + SWEPT_RAW + ((ROM_KEY,) if rom else ()):
        if kept.get(path) != have[path]:
            p.append(f"  decode: {RECORD} was swept from another {path} (sha256 "
                     f"{str(kept.get(path))[:12]}, this tree's is {have[path][:12]}): "
                     "sweep this engine with make -C tb/aecp_cmds record")
    return p


def ucode_of(inp: Inputs) -> Ucode:
    """The generator's output, run on first use."""
    if inp.ucode is None:
        inp.ucode = load_ucode(inp.root / UCODE)
    return inp.ucode


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
# the RTL consumers: declared constants only - what the engine decodes and
# dispatches is observed (tb/aecp_cmds), never read off its text
# ---------------------------------------------------------------------------

RE_C16 = re.compile(r"localparam\s+logic\s*\[15:0\]\s+((?:OP|GDI|MVU)_[A-Z0-9_]+_C)"
                    r"\s*=\s*16'h([0-9A-Fa-f]{4})\s*;")
RE_DECL = re.compile(r"\b(?:localparam|parameter)\b[^;]*?\b((?:OP|GDI)_[A-Z0-9_]+_C)\s*=")
RE_UPC = re.compile(r"localparam\s+logic\s*\[10:0\]\s+UPC_([A-Z0-9_]+)_C\s*=\s*11'd(\d+)\s*;")


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


# ---------------------------------------------------------------------------
# the ROM consumer: every path of a program, abstractly
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Var:
    """A length a path cannot fix: `base` bytes, the part every instance has,
    plus whole records of `per` bytes (a loop of unknown count), or plus any
    number of bytes when `per` is None (a copy of unknown length)."""

    base: int
    per: int = None

    def __add__(self, n: int) -> "Var":
        return Var(self.base + n, self.per)

    def __str__(self) -> str:
        return f"{self.base} + {self.per}·N" if self.per else f"{self.base} + a copy"


def covers(v: Var, ln) -> bool:
    """Whether the instances of `v` include every instance of `ln`."""
    if v is None:
        return False
    if isinstance(ln, Var):
        if ln.base < v.base or v.per is None:
            return ln.base >= v.base
        return ln.per is not None and ln.per % v.per == 0 and (ln.base - v.base) % v.per == 0
    return ln >= v.base and (v.per is None or (ln - v.base) % v.per == 0)


def join(a: Var, b: Var) -> Var:
    """The least Var holding both."""
    if a is None:
        return b
    if a.per is None or b.per is None:
        return Var(min(a.base, b.base))
    return Var(min(a.base, b.base), math.gcd(a.per, b.per, abs(a.base - b.base)))


def absorb(seen: dict, key, ln, ovf):
    """(length, overflow) to explore `key` with, or None when an earlier visit
    already holds it. A state keeps up to WIDEN exact lengths, so branches that
    join with different lengths stay exact; past that it is a loop of unknown
    count, and the state widens to its smallest length - the fixed part - plus
    whole records of the step between them. A widened state's overflow flag is
    unknown, so the flag is no part of the state."""
    ex, var = seen.get(key, (frozenset(), None))
    if not isinstance(ln, Var):
        if (ln, ovf) in ex or covers(var, ln):
            return None
        lens = {x for x, _ in ex}
        if ln in lens or len(lens) < WIDEN:
            seen[key] = (ex | {(ln, ovf)}, var)
            return ln, ovf
        vals = sorted(lens | {ln})
        step = 0
        for x in vals[1:]:
            step = math.gcd(step, x - vals[0])
        ln = Var(vals[0], step)
    elif covers(var, ln):
        return None
    var = join(var, ln)
    seen[key] = (ex, var)
    return var, None


@dataclass
class Facts:
    """What every path from one entry does: (status, length, the length at its
    first COPY_BUFFER or None) at each SEND_RESPONSE, whether it reaches
    CHECK_LOCK, the NOTIFY_ENQ classes, and any word of fill it runs into."""

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
    exact; a loop of unknown count keeps its fixed part and its step (`absorb`),
    and a path remembers the length its first COPY_BUFFER starts at, the fixed
    part before a descriptor's bytes."""
    n = {v: k for k, v in uc.ops.items()}
    width = {0: 1, 1: 2, 2: 4, 3: 8}
    facts = Facts()
    seen = {}
    work = [(start, frozenset({0}), HDR, ((0, 0),), None, False, None)]
    while work:
        pc, st, ln, regs, it, ovf, cp = work.pop()
        got = absorb(seen, (pc, st, regs, it, cp), ln, ovf)
        if got is None:
            continue
        ln, ovf = got
        if not 0 <= pc < len(uc.rom) or pc not in uc.occupied:
            facts.fill.add(pc)
            continue
        w = uc.rom[pc]
        op = n.get(w >> 43)
        rd, ra, fmt, cnd, imm = (w >> 39) & 15, (w >> 35) & 15, (w >> 28) & 7, (w >> 24) & 15, w & 0xFFFFFF
        tgt = imm & 0x7FF
        rv = dict(regs)
        nxt = pc + 1
        var = isinstance(ln, Var)

        def go(p, s=st, l=ln, r=None, i=it, o=ovf, c=cp):
            work.append((p, frozenset(s), l, tuple(sorted((r if r is not None else rv).items())),
                         i, o, c))

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
            elif cnd == 4 and ovf is not None and not var:
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
            #! so the loop is one state the walk revisits and widens
            it = (rv[ra] & 0xFF, 0) if ra in rv else (None, None)
        elif op == "ITER_NEXT" and it is not None and it[0] is not None:
            it = (it[0], (it[1] + 1) & 0xFF)
        elif op in ("BUILD_FLD", "APPEND"):
            cap = 592 if (op == "APPEND" and cnd & 1) else 524
            if op == "APPEND" and not var and ln + width[fmt] > cap:
                ovf = True
            else:
                ln = ln + width[fmt]
        elif op == "READ_CTRS":
            ln = ln + 16
        elif op == "COPY_BUF":
            count = None if ra not in rv else (
                (rv[ra] & 0xFFFF) - (imm & 0xFFF) if cnd & 1 else rv[ra] & 0xFFFF)
            if cp is None:
                cp = ln.base if var else ln
            if count is not None and count >= 0:
                ln = ln + count
            else:
                ln = Var(ln.base if var else ln)
        elif op == "SET_LENGTH":
            ln = imm & 0x7FF
        elif op == "SEND_RESP":
            for s in st:
                facts.sends.add((s, ln, cp))
        elif op == "NOTIFY_ENQ":
            facts.notify.add(imm & 15)
        if isinstance(ln, Var):
            ovf = None
        go(nxt, s=st, l=ln, i=it, o=ovf, c=cp)
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


def check_size(who: str, k: str, s) -> list:
    """One command or response size rule."""
    if not isinstance(s, dict) or not isinstance(s.get("cdl"), int):
        return [f"  schema: {who}: {k} has no integer cdl"]
    p = []
    if s.get("per") is not None and (not isinstance(s["per"], int) or not s.get("unit")):
        p.append(f"  schema: {who}: {k} per must be an integer with its unit")
    if "floor" in s and (k != "command" or not isinstance(s["floor"], int)
                         or not HDR <= s["floor"] < s["cdl"]):
        p.append(f"  schema: {who}: {k} floor must be a command's integer from {HDR} "
                 "below its cdl")
    return p


def check_schema(t: dict) -> list:
    """The table's own shape: the fields every consumer reads, spelled once,
    and the fields gen_ucode.py reads, so a table it cannot use is refused by
    name before it runs."""
    p = []
    if t.get("schema") != SCHEMA:
        p.append(f"  schema: {TABLE} declares {t.get('schema')!r}, this gate reads {SCHEMA!r}")
    codes = t.get("status_codes", {})
    hz = t.get("hazard_classes", {})
    cmds = t.get("commands", [])
    if not codes or not hz or not cmds:
        return p + ["  schema: status_codes, hazard_classes or commands is empty"]
    for name, row in codes.items():
        code = row.get("code") if isinstance(row, dict) else None
        if not isinstance(code, int):
            p.append(f"  schema: status {name}: code must be an integer")
        elif IEEE_STATUS.get(name) != code:
            p.append(f"  schema: status {name} is {code} in the table, IEEE 1722.1-2021 "
                     f"Table 7-141 gives {IEEE_STATUS.get(name, 'no such status')}")
    for msg, names in t.get("common_status", {}).items():
        for s in names:
            if s not in codes:
                p.append(f"  schema: common_status {msg}: status {s} is not in status_codes")
    if t.get("unserved") not in t.get("shared_entries", {}):
        p.append(f"  schema: unserved {t.get('unserved')!r} is no shared entry")
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
        bad = False
        for k in ("command", "response"):
            got = check_size(who, k, c[k])
            bad |= bool(got)
            p += got
        rsp, cmd_ = c["response"], c["command"]
        if not bad and rsp.get("echo") and (rsp["cdl"], rsp.get("per")) != \
                (cmd_["cdl"], cmd_.get("per")):
            p.append(f"  schema: {who}: an echo response is the command's own size: "
                     f"response cdl {size_text(rsp, False)}, command cdl "
                     f"{size_text(cmd_, False)}")
        if not isinstance(c["entries"], list) or not c["entries"] \
                or len(set(c["entries"])) != len(c["entries"]) \
                or not all(isinstance(e, str) for e in c["entries"]):
            p.append(f"  schema: {who}: entries must be a non-empty list of names without repeats")
        nt = c["notify"]
        if nt is not None and not (isinstance(nt, dict) and {"class", "kind", "body"} <= set(nt)):
            p.append(f"  schema: {who}: notify must be null or carry class, kind and body")
    if ("MVU", "GET_MILAN_INFO") not in {(c.get("message"), c.get("name")) for c in cmds}:
        p.append("  schema: GET_MILAN_INFO: no command with message MVU, whose opcode "
                 f"{UCODE} reads as its command_type")
    for d in t.get("documented", []):
        if not isinstance(d, dict) or not {"section", "opcode", "name", "cells"} <= set(d) \
                or d["section"] not in SECTIONS or len(d["cells"]) != len(COLUMNS) - 2:
            p.append(f"  schema: documented row {d.get('opcode') if isinstance(d, dict) else d!r}: "
                     f"needs an opcode, a name, a section of {SECTIONS} and "
                     f"{len(COLUMNS) - 2} cells")
    return p


# ---------------------------------------------------------------------------
# the decode record: what tb/aecp_cmds observed, by program name
# ---------------------------------------------------------------------------

def ranges(nums) -> str:
    """[12, 13, 14, 20] -> '12-14, 20'."""
    out, nums = [], sorted(set(nums))
    i = 0
    while i < len(nums):
        j = i
        while j + 1 < len(nums) and nums[j + 1] == nums[j] + 1:
            j += 1
        out.append(f"{nums[i]}" if i == j else f"{nums[i]}-{nums[j]}")
        i = j + 1
    return ", ".join(out)


def hexranges(nums) -> str:
    """[3, 5, 6, 7] -> '0x0003, 0x0005-0x0007'."""
    return ", ".join("-".join(f"0x{int(x):04X}" for x in r.split("-"))
                     for r in ranges(nums).split(", ") if r)


def unranges(text: str) -> list:
    """'12-14, 0x0020' -> [12, 13, 14, 32]."""
    out = []
    for r in str(text).split(","):
        r = r.strip()
        if not r:
            continue
        a, _, b = r.partition("-")
        lo, hi = int(a, 0), int(b or a, 0)
        out += range(lo, hi + 1)
    return out


def classify(pairs) -> dict:
    """The answers one program gave, by status: one length, 'echo' when the
    response always had the command's own length, or every length seen."""
    by = {}
    for cmd_cdl, ans in pairs:
        if ans == "none":
            by.setdefault("none", [])
            continue
        s, r = ans.split(":")
        by.setdefault(s, []).append((cmd_cdl, int(r)))
    out = {}
    for s, lst in sorted(by.items(), key=lambda x: (x[0] == "none", int(x[0]) if x[0] != "none" else 0)):
        if s == "none":
            out[s] = "no frame"
            continue
        lens = sorted({r for _, r in lst})
        if len(lens) == 1:
            out[s] = str(lens[0])
        elif all(r == c for c, r in lst):
            out[s] = "echo"
        else:
            out[s] = ", ".join(map(str, lens))
    return out


def canonical(raw: dict, uc: Ucode, kinds: dict, inputs: dict) -> dict:
    """The bench's raw output (µPC addresses) as the decode record (names),
    with the digests of what was swept."""
    name = {at: e for e, at in uc.entry.items()}

    def prog(o: str) -> str:
        return "+".join(name.get(int(x), f"µPC {x}") if x.isdigit() else x
                        for x in o.split("+"))

    decode = {}
    for key, outs in raw["decode"].items():
        bucket, op = key.rsplit(" ", 1)
        progs = {}
        for o, by_cdl in outs.items():
            cdls = sorted(int(c) for c in by_cdl)
            pairs = [(int(c), a) for c, ans in by_cdl.items() for a in ans]
            progs[prog(o)] = {"floor": cdls[0], "cdls": ranges(cdls), "answers": classify(pairs)}
        decode.setdefault(bucket, {})[int(op, 16)] = dict(sorted(progs.items()))
    groups = {}
    for bucket, by_op in sorted(decode.items()):
        sigs = {}
        for op, progs in sorted(by_op.items()):
            sigs.setdefault(json.dumps(progs, sort_keys=True), (progs, []))[1].append(op)
        groups[bucket] = [{"opcodes": hexranges(ops), "programs": progs}
                          for progs, ops in sorted(sigs.values(), key=lambda g: g[1][0])]
    def record(a: str, ct: int) -> tuple:
        """One aggregate answer as (status, the record's length), or the
        aggregate's own cdl when it carries no whole record."""
        s, c = (int(x) for x in a.split(":"))
        if ct < 0:
            return s, f"cdl {c}"
        return (s, c - HDR - 8) if c >= HDR + 8 else (s, f"aggregate cdl {c}")

    members, refused, empty = {}, {}, {}
    for ct, outs in sorted(raw["gdi"].items(), key=lambda x: int(x[0])):
        ct = int(ct)
        lens = {}
        for o, ans in outs.items():
            got = sorted({record(a, ct) for a in ans if a != "none"}, key=str)
            lens[prog(o.removeprefix("batch:")) if o.startswith("batch:")
                 else ("copied" if o == "engine" and ct >= 0 else prog(o))] = \
                (", ".join(f"{s}:{n}" for s, n in got) or "no frame",
                 o.startswith("batch:") or o == "engine")
        if ct < 0:
            empty = {k: v for k, (v, _) in lens.items()}
        elif not any(member for _, member in lens.values()):
            refused.setdefault("+".join(sorted(lens)), []).append(ct)
        else:
            members[f"0x{ct:04X}"] = {k: v for k, (v, _) in sorted(lens.items())}
    uns = {}
    for k, outs in sorted(raw["unsolicited"].items(), key=lambda x: int(x[0])):
        uns[kinds.get(int(k), f"kind {k}")] = {prog(o): sorted(v) for o, v in sorted(outs.items())}
    return {
        "schema": RECORD_SCHEMA,
        "about": [
            "SPDX-License-Identifier: CERN-OHL-W-2.0",
            "What KL_aecp_engine decodes and dispatches, observed by tb/aecp_cmds (issue #73). "
            "Regenerate with: make -C tb/aecp_cmds record. The suite holds a fresh sweep to this "
            "file; scripts/check_aecp_commands.py (make check) holds aecp_commands.json to it.",
            "decode: per message bucket, groups of opcodes the engine treats alike; per program "
            "(the E_<name> the uCPU was handed): floor = the shortest cdl it was dispatched at, "
            "cdls = every swept cdl it was dispatched at, answers = status code -> the response "
            "cdls seen ('echo' = always the command's own cdl). 'engine' = answered without a "
            "program.",
            "gdi: GET_DYNAMIC_INFO with one record of each command_type: members -> the program "
            "each record ran and the aggregate's status:record length (copied = a record "
            "answered by copying its data); refused -> the program that refused the whole "
            "command; empty -> the aggregate with no record.",
            "unsolicited: per job kind, the program and the response's command_type:cdl.",
        ],
        "sweep": {
            "trials": raw["trials"], "timeouts": raw["timeouts"],
            "opcodes": "0x0000-0x007F on every bucket; MVU: command_type 0x0000-0x007F",
            "cdl": "12-100 with descriptor_type @24; 16, 24, 96 running; the other templates at "
                   "their own lengths (tb/aecp_cmds/sim_main.cpp); every program replayed down to 12",
            "descriptor_types": "0x0000-0x0028, 0xFFFF",
            "build": "EN_IDENTIFY_NOTIF_P = 1 (the IDENTIFY_NOTIFICATION job has its program)",
            "inputs": inputs,
        },
        "decode": groups,
        "gdi": {"members": members,
                "refused": {k: hexranges(v) for k, v in sorted(refused.items())},
                "empty": empty},
        "unsolicited": uns,
    }


def dump_record(rec: dict) -> str:
    """decode.json's text: one line per program, group and job kind."""
    def one(x) -> str:
        return json.dumps(x, ensure_ascii=False)
    lines = ["{"]
    keys = list(rec)
    for i, k in enumerate(keys):
        tail = "," if i < len(keys) - 1 else ""
        v = rec[k]
        if k == "decode":
            lines.append(f'  "decode": {{')
            for j, (b, groups) in enumerate(v.items()):
                lines.append(f"    {one(b)}: [")
                for gi, g in enumerate(groups):
                    lines.append(f'      {{"opcodes": {one(g["opcodes"])}, "programs": {{')
                    progs = list(g["programs"].items())
                    for pi, (pn, info) in enumerate(progs):
                        lines.append(f"        {one(pn)}: {one(info)}"
                                     f"{',' if pi < len(progs) - 1 else ''}")
                    lines.append(f"      }}}}{',' if gi < len(groups) - 1 else ''}")
                lines.append(f"    ]{',' if j < len(v) - 1 else ''}")
            lines.append(f"  }}{tail}")
        elif k in ("gdi", "unsolicited", "sweep"):
            lines.append(f"  {one(k)}: {{")
            items = list(v.items())
            for j, (a, b) in enumerate(items):
                if isinstance(b, dict) and k in ("gdi", "sweep"):
                    lines.append(f"    {one(a)}: {{")
                    sub = list(b.items())
                    for si, (c, d) in enumerate(sub):
                        lines.append(f"      {one(c)}: {one(d)}{',' if si < len(sub) - 1 else ''}")
                    lines.append(f"    }}{',' if j < len(items) - 1 else ''}")
                else:
                    lines.append(f"    {one(a)}: {one(b)}{',' if j < len(items) - 1 else ''}")
            lines.append(f"  }}{tail}")
        elif k == "about":
            lines.append('  "about": [')
            for j, a in enumerate(v):
                lines.append(f"    {one(a)}{',' if j < len(v) - 1 else ''}")
            lines.append(f"  ]{tail}")
        else:
            lines.append(f"  {one(k)}: {one(v)}{tail}")
    return "\n".join(lines + ["}"]) + "\n"


def fits_exact(n: int, rsp: dict, status: int = 0) -> bool:
    """Whether one exact response cdl is an instance of the rule; a rule with a
    failure stub answers every failure at exactly the stub."""
    base = rsp["cdl"]
    if rsp.get("stub") is not None and status:
        return n == rsp["stub"]
    if rsp.get("plus"):
        return n >= base
    if rsp.get("per"):
        return n >= base and (n - base) % rsp["per"] == 0
    return n == base


def answer_lens(spec: str) -> list:
    """'echo', '20' or '20, 28' -> ['echo'] / [20] / [20, 28]."""
    return ["echo"] if spec == "echo" else [int(x) for x in spec.split(",")]


def check_record(t: dict, rec: dict, where: str = RECORD) -> tuple:
    """The table against what the engine was observed to decode and dispatch:
    (findings, checks made, checks failed, whether every command's entries are
    observed - the field the generator reads)."""
    p, n, f, mark = [], 0, 0, 0

    def done() -> None:
        """One check made: it failed if it added a finding."""
        nonlocal n, f, mark
        n, f, mark = n + 1, f + (len(p) > mark), len(p)

    if rec.get("schema") != RECORD_SCHEMA:
        return [f"  decode: {where} declares {rec.get('schema')!r}, this gate reads "
                f"{RECORD_SCHEMA!r}"], 1, 1, False
    try:
        groups = {(b, op): g["programs"] for b, gs in rec["decode"].items()
                  for g in gs for op in unranges(g["opcodes"])}
        gdi, uns = rec["gdi"], rec["unsolicited"]
        members, empty = gdi["members"], gdi["empty"]
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        return [f"  decode: {where} is not a decode record: {exc!r}"], 1, 1, False
    entries_ok = True
    code_of = {r["code"]: s for s, r in t["status_codes"].items()}
    echo_shared = set(t.get("echo_entries", []))
    served = set()
    if rec.get("sweep", {}).get("timeouts"):
        p.append(f"  decode: {where}: {rec['sweep']['timeouts']} trials never ended")
    done()
    for c in t["commands"]:
        name, cmd_, rsp = c["name"], c["command"], c["response"]
        key = (c["message"], opcode_of(c["opcode"]))
        at = f"{c['message']} {c['opcode']}"
        served.add(key)
        progs = groups.get(key)
        if progs is None:
            p.append(f"  decode: {name}: {where} has no {at}: the sweep never sent it")
            entries_ok = False
            done()
            continue
        disp = {k for k in progs if k not in OUTCOMES}
        want = set(c["entries"])
        for e in sorted(want - disp):
            p.append(f"  decode: {name}: the table's entries name {e}, which the engine "
                     f"never dispatches {at} to ({where})")
            entries_ok = False
        for e in sorted(disp - want):
            p.append(f"  decode: {name}: the engine dispatches {at} to {e}, which is not "
                     f"one of the table's entries ({where})")
            entries_ok = False
        done()
        if ("engine" in progs) != bool(rsp.get("engine")):
            p.append(f"  decode: {name}: the engine "
                     f"{'answers' if 'engine' in progs else 'never answers'} {at} without a "
                     f"program, the table says its response is "
                     f"{'engine' if rsp.get('engine') else 'program'}-built")
        for bad in ("drop", "timeout"):
            if bad in progs:
                p.append(f"  decode: {name}: the bench saw {at} "
                         f"{'dropped' if bad == 'drop' else 'never finish'} at cdl "
                         f"{progs[bad]['cdls']}")
        done()
        first = "engine" if rsp.get("engine") else c["entries"][0]
        floor = cmd_.get("floor", cmd_["cdl"])
        got = progs.get(first, {}).get("floor")
        how = "answers without a program" if first == "engine" else f"dispatches to {first}"
        if got is not None and got != floor:
            p.append(f"  decode: {name}: the shortest {at} the engine {how} "
                     f"is cdl {got}, the table's "
                     f"{'floor' if 'floor' in cmd_ else 'command cdl'} is {floor}")
        if got is not None and cmd_.get("per"):
            odd = [x for x in unranges(progs[first]["cdls"]) if (x - floor) % cmd_["per"]]
            if odd:
                did = (f"answers {at} without a program" if first == "engine"
                       else f"dispatches {at} to {first}")
                p.append(f"  decode: {name}: the engine {did} at cdl "
                         f"{ranges(odd)}, which is no {size_text(cmd_, False)}")
        done()
        allowed = set(c["status"]) | set(t["common_status"].get(c["message"], [])) \
            | set(c.get("engine_status", []))
        for pn, info in sorted(progs.items()):
            echo_ok = pn in echo_shared or bool(rsp.get("echo"))
            who = "its engine-built answer" if pn == "engine" else pn
            sent = set(unranges(info["cdls"]))
            for s, spec in info["answers"].items():
                if s == "none":
                    if pn not in OUTCOMES:
                        p.append(f"  decode: {name}: {at} dispatched to {pn} sent no frame")
                    continue
                st = code_of.get(int(s), f"status {s}")
                if st not in allowed:
                    p.append(f"  decode: {name}: {who} answered {st}, which is not in the "
                             "table's status set")
                for ln in answer_lens(spec):
                    if ln == "echo":
                        if not echo_ok:
                            p.append(f"  decode: {name}: {who} answered {st} at the command's "
                                     f"own length, and the table's response is no echo")
                    elif not fits_exact(ln, rsp, int(s)) and not (echo_ok and ln in sent):
                        p.append(f"  decode: {name}: {who} answered {st} at cdl {ln}, the "
                                 f"table's response is {size_text(rsp, False)}")
        done()
    unserved = t.get("unserved")
    for b, gs in sorted(rec["decode"].items()):
        for g in gs:
            ops = [op for op in unranges(g["opcodes"]) if (b, op) not in served]
            if not ops:
                continue
            for pn in sorted(set(g["programs"]) - {unserved}):
                p.append(f"  decode: {b} {hexranges(ops)} is no table command and the engine "
                         f"dispatches it to {pn}, not only {unserved}")
            done()
    #! GET_DYNAMIC_INFO: its members, what each record runs, how long it is
    docs = {opcode_of(d["opcode"]): d["name"] for d in t.get("documented", []) if d.get("gdi")}
    cmds = {opcode_of(c["opcode"]): c for c in t["commands"] if c["message"] == "AEM"}
    want = {op for op, c in cmds.items() if c["gdi"]} | set(docs)
    have = {int(k, 16) for k in members}
    for op in sorted(want - have):
        who = cmds[op]["name"] if op in cmds else docs[op]
        p.append(f"  decode: {who}: the table makes it a GET_DYNAMIC_INFO member and the "
                 "engine refuses a record of it")
    for op in sorted(have - want):
        who = cmds[op]["name"] if op in cmds else f"0x{op:04X}"
        p.append(f"  decode: {who}: the engine runs a record of it inside GET_DYNAMIC_INFO "
                 "and the table says it is no member")
    done()
    for op in sorted(want & have):
        c = cmds.get(op)
        if c is None:
            continue
        m = members[f"0x{op:04X}"]
        ran = {k for k in m if k not in ("copied",) + OUTCOMES}
        if c["entries"][0] not in ran:
            p.append(f"  decode: {c['name']}: inside GET_DYNAMIC_INFO the engine never "
                     f"dispatches it to {c['entries'][0]}")
        for e in sorted(ran - set(c["entries"])):
            p.append(f"  decode: {c['name']}: inside GET_DYNAMIC_INFO the engine dispatches "
                     f"it to {e}, which is not one of its entries")
        for e in sorted(ran):
            for tok in m[e].split(", "):
                s, _, ln = tok.partition(":")
                if not s.isdigit() or s != "0" or not ln.isdigit():
                    p.append(f"  decode: {c['name']}: inside GET_DYNAMIC_INFO its {e} record "
                             f"leaves the aggregate {code_of.get(int(s), s) if s.isdigit() else s}"
                             f" at {ln or 'no frame'}, not SUCCESS with the record")
                elif int(ln) != c["response"]["cdl"] - HDR:
                    p.append(f"  decode: {c['name']}: inside GET_DYNAMIC_INFO its {e} record "
                             f"is {ln} bytes, the table's response is cdl "
                             f"{c['response']['cdl']} ({c['response']['cdl'] - HDR} bytes)")
        done()
    for c in t["commands"]:
        if c["response"].get("engine"):
            got = empty.get("engine")
            if got != f"0:cdl {c['response']['cdl']}":
                p.append(f"  decode: {c['name']}: with no record the engine answers {got}, "
                         f"the table's response is SUCCESS at {size_text(c['response'], False)}")
            done()
    #! the unsolicited jobs: kind -> command_type and body
    for c in t["commands"]:
        nt = c["notify"]
        if not nt:
            continue
        got = uns.get(nt["kind"])
        if got is None:
            p.append(f"  decode: {c['name']}: the engine runs no {nt['kind']} job")
            done()
            continue
        pairs = {(x.split(":")[0], pn) for pn, v in got.items() for x in v if x != "none"}
        if (c["opcode"], nt["body"]) not in pairs:
            p.append(f"  decode: {c['name']}: the {nt['kind']} job answers "
                     f"{', '.join(f'{ct} with {pn}' for ct, pn in sorted(pairs)) or 'nothing'}, "
                     f"the table says {c['opcode']} with {nt['body']}")
        rule = {k: v for k, v in c["response"].items() if k != "stub"}
        for pn, v in got.items():
            for x in v:
                ct, _, ln = x.partition(":")
                if x != "none" and ct == c["opcode"] and pn == nt["body"] \
                        and not (fits_exact(int(ln), rule) or (rule.get("echo") and int(ln) == HDR)):
                    p.append(f"  decode: {c['name']}: its unsolicited {pn} is cdl {ln}, the "
                             f"table's response is {size_text(c['response'], False)}")
        done()
    bodies = {(c["notify"]["kind"], c["opcode"], c["notify"]["body"])
              for c in t["commands"] if c["notify"]}
    for k, got in sorted(uns.items()):
        for pn, v in sorted(got.items()):
            for x in v:
                ct = x.split(":")[0]
                if x != "none" and (k, ct, pn) not in bodies:
                    p.append(f"  decode: the engine's {k} job answers {ct} with {pn}, which "
                             "is no table command's notification")
        done()
    return p, n, f, entries_ok


def flat(rec: dict) -> dict:
    """A decode record as path -> value, with the groups opened per opcode, so
    two records compare entry by entry."""
    out = {}
    for b, gs in rec.get("decode", {}).items():
        for g in gs:
            for op in unranges(g["opcodes"]):
                for pn, info in g["programs"].items():
                    out[f"decode {b} 0x{op:04X} {pn}"] = json.dumps(info, sort_keys=True)
    for part in ("gdi", "unsolicited", "sweep"):
        for k, v in rec.get(part, {}).items():
            if part in ("gdi", "sweep") and isinstance(v, dict):
                for a, b in v.items():
                    out[f"{part} {k} {a}"] = json.dumps(b, sort_keys=True)
            else:
                out[f"{part} {k}"] = json.dumps(v, sort_keys=True)
    out["schema"] = rec.get("schema")
    return out


def record_diff(fresh: dict, kept: dict) -> list:
    """Every entry where a fresh sweep and the recorded file differ."""
    a, b = flat(fresh), flat(kept)
    p = []
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            p.append(f"  decode: {RECORD} is not what the engine does: {k}: observed "
                     f"{a.get(k, 'nothing')}, recorded {b.get(k, 'nothing')} "
                     "(make -C tb/aecp_cmds record)")
    return p


# ---------------------------------------------------------------------------
# the checks
# ---------------------------------------------------------------------------

def check_engine(t: dict, eng: dict) -> list:
    """The engine's declared opcodes and µPC entry names against the table."""
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


def size_text(s: dict, octets: bool) -> str:
    """A size rule as text: cdl, or AECPDU octets (12 + cdl)."""
    base = s["cdl"] + (HDR if octets else 0)
    out = str(base)
    if s.get("per"):
        out += f" + {s['per']}·N"
    if s.get("plus"):
        out += f" + {s['plus']}"
    return out


def fits(ln, rsp: dict, echo: bool, status: int = 0, cp: int = None) -> bool:
    """Whether one program's length at SEND_RESPONSE answers the table's
    response rule: a header-only echo, the fixed cdl, the rule's failure stub,
    an exact instance of a per-record rule, a loop whose fixed part is the
    rule's base and whose step is its record, or a descriptor rule's answer
    whose copy starts at the rule's base."""
    base = rsp["cdl"]
    if ln == HDR and echo:
        return True
    if rsp.get("plus") and not (rsp.get("stub") is not None and status):
        return cp == base and (isinstance(ln, Var) or ln >= base)
    if isinstance(ln, Var):
        return bool(rsp.get("per")) and ln.base == base and ln.per == rsp["per"]
    return fits_exact(ln, rsp, status)


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
        definite, faced, lock, notes, success = set(), set(), False, set(), set()
        for e in c["entries"]:
            at = uc.entry.get(e)
            if at is None or at not in uc.placed:
                p.append(f"  rom: {name}: entry {e} is no placed program in {UCODE}")
                continue
            d, m = entry_facts(uc, at)
            for pc in sorted(d.fill | m.fill):
                p.append(f"  rom: {name}: a path from E_{e} runs into ROM word {pc}, "
                         "which no program placed (the fill)")
            definite |= {s for s, _, _ in d.sends}
            faced |= {s for s, _, _ in m.sends}
            lock |= m.lock
            notes |= m.notify
            success |= {(ln, cp) for s, ln, cp in m.sends if s == 0}
            for s, ln, cp in sorted(m.sends, key=str):
                if not fits(ln, rsp, e in echo_shared or bool(rsp.get("echo")), s, cp):
                    p.append(f"  rom: {name}: E_{e} answers {code_of.get(s, s)} at cdl "
                             f"{ln}{f' (its copy starting at {cp})' if cp is not None and rsp.get('plus') else ''}, "
                             f"the table's response is {size_text(rsp, False)}"
                             f"{' (echo)' if rsp.get('echo') else ''}")
        #! a variable rule is a promise of a variable part: some SUCCESS path
        #! must build it, or the records were never emitted
        if (rsp.get("per") or rsp.get("plus")) and not rsp.get("echo") and not rsp.get("engine") \
                and not any(isinstance(ln, Var) and fits(ln, rsp, False, 0, cp)
                            for ln, cp in success):
            p.append(f"  rom: {name}: its programs answer SUCCESS only at cdl "
                     f"{', '.join(sorted({str(ln) for ln, _ in success})) or 'nothing'}, never "
                     f"{size_text(rsp, False)}: no path builds the variable part")
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
                for s, ln, cp in sorted(walk(uc, at, True).sends, key=str):
                    if not fits(ln, {k: v for k, v in rsp.items() if k != "stub"},
                                bool(rsp.get("echo")), 0, cp):
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


def check(inp: Inputs) -> list:
    """Every disagreement, one line each; empty means the gate passes. The
    generator runs only on a table the schema accepts and whose entries the
    engine was observed to dispatch: those are the fields it reads."""
    t = inp.table
    p = check_schema(t)
    if p:
        return p
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
    if entries_ok:
        p += check_rom(t, ucode_of(inp))
        p += [x for x in check_swept(inp, rom=True) if ROM_KEY in x]
    return p


# ---------------------------------------------------------------------------
# the self-test: one planted defect per finding and per refusal
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
    uc = ucode_of(inp)
    code = uc.ops[op]
    hits = [pc for pc in reachable(uc, uc.entry[entry]) if uc.rom[pc] >> 43 == code]
    if len(hits) <= nth:
        raise GateError(f"selftest fixture: fewer than {nth + 1} {op} from E_{entry}")
    return hits[nth]


def nop(inp: Inputs, entry: str, op: str, nth: int = 0) -> None:
    """Turn the nth `op` a path from E_entry can execute into a NOP (op 0)."""
    ucode_of(inp).rom[rom_op(inp, entry, op, nth)] = 0


def drop_all(inp: Inputs, entry: str, op: str) -> None:
    """Turn every `op` a path from E_entry can execute into a NOP (op 0)."""
    uc = ucode_of(inp)
    code = uc.ops[op]
    for pc in reachable(uc, uc.entry[entry]):
        if uc.rom[pc] >> 43 == code:
            uc.rom[pc] = 0


def into_fill(inp: Inputs, entry: str) -> None:
    """Make the word after E_entry's SEND_RESPONSE branch into the ROM fill."""
    uc = ucode_of(inp)
    free = max(set(range(len(uc.rom))) - uc.occupied)
    at = rom_op(inp, entry, "SEND_RESP") + 1
    uc.rom[at] = (uc.ops["BRANCH"] << 43) | free


def set_imm(inp: Inputs, at: int, imm: int) -> None:
    """Rewrite one ROM word's immediate."""
    uc = ucode_of(inp)
    uc.rom[at] = (uc.rom[at] & ~0xFFFFFF) | imm


def sub_all(inp: Inputs, path: str, old: str, new: str) -> None:
    """Replace every occurrence of `old` in an input text, at least one."""
    if old not in inp.texts[path]:
        raise GateError(f"selftest fixture: {old!r} does not occur in {path}")
    inp.texts[path] = inp.texts[path].replace(old, new)


def doc_cell(inp: Inputs, row: str, old: str, new: str) -> None:
    """Edit one cell of one F06.14 row (the doc-only drift)."""
    text = inp.texts[DOC]
    line = next(x for x in text.split("\n") if x.startswith(f"| {row} |"))
    if line.count(old) != 1:
        raise GateError(f"selftest fixture: {old!r} not once in F06.14 row {row}")
    inp.texts[DOC] = text.replace(line, line.replace(old, new))


def doc_rows(inp: Inputs, fn) -> None:
    """Rewrite F06.14's rows as a list: `fn(rows)` returns the new list."""
    have, a, b = doc_block(inp.texts[DOC])
    new = have[:2] + fn(have[2:])
    inp.texts[DOC] = inp.texts[DOC][:a] + "\n" + "\n".join(new) + "\n" + inp.texts[DOC][b:]


def row_at(rows_: list, opcode: str) -> int:
    """The index of the F06.14 row of `opcode`."""
    return next(k for k, x in enumerate(rows_) if x.startswith(f"| {opcode} |"))


def swap(rows_: list, a: str, b: str) -> list:
    """Two F06.14 rows exchanged."""
    i, j = row_at(rows_, a), row_at(rows_, b)
    rows_[i], rows_[j] = rows_[j], rows_[i]
    return rows_


def table_and_doc(inp: Inputs, fn) -> None:
    """A table change with F06.14 regenerated from it, as `--write` would."""
    fn(inp.table)
    inp.texts[DOC] = regenerated(inp.table, inp.texts[DOC])


def group(inp: Inputs, bucket: str, op: int) -> dict:
    """The programs of one opcode in the decode record, split out of its group
    so a plant touches that opcode alone."""
    gs = inp.record["decode"][bucket]
    g = next(g for g in gs if op in unranges(g["opcodes"]))
    rest = [x for x in unranges(g["opcodes"]) if x != op]
    if rest:
        g["opcodes"] = hexranges(rest)
        g = {"opcodes": f"0x{op:04X}", "programs": copy.deepcopy(g["programs"])}
        gs.append(g)
    return g["programs"]


def rename(d: dict, old: str, new: str) -> None:
    """One key of a dict renamed."""
    d[new] = d.pop(old)


def mutants() -> list:
    """(what, mutation, words each failure line must carry) per planted defect."""
    aem = lambda i, op: group(i, "AEM", op)  # noqa: E731
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
        ("a command missing from the table, as the engine decodes it",
         lambda i: i.table["commands"].remove(cmd(i, "GET_AS_PATH")),
         ("decode: AEM 0x0028 is no table command", "GASP")),
        ("an OP_*_C written in another form",
         lambda i: sub(i, ENGINE, "localparam logic [15:0] OP_GET_AS_PATH_C     = 16'h0028;",
                         "localparam int unsigned OP_GET_AS_PATH_C = 40;"),
         ("OP_GET_AS_PATH_C is not written",)),
        ("a GDI_*_C the table does not document",
         lambda i: i.table["documented"].remove(
             next(d for d in i.table["documented"] if d.get("rtl") == "GDI_GET_MEM_LEN_C")),
         ("GDI_GET_MEM_LEN_C = 0x0048 and the table documents no such",)),
        # --- the observed decode and dispatch (tb/aecp_cmds/decode.json) ---
        ("a success arm redirected to another program (R608-1 q1, as recorded)",
         lambda i: rename(aem(i, 0x0016), "SCLKS", "GCLKS"),
         ("decode: SET_CLOCK_SOURCE: the table's entries name SCLKS",)),
        ("a success arm redirected to another program, the other line",
         lambda i: rename(aem(i, 0x0016), "SCLKS", "GCLKS"),
         ("decode: SET_CLOCK_SOURCE: the engine dispatches AEM 0x0016 to GCLKS",)),
        ("a decode keyed on another opcode (R608-1 q2, as recorded)",
         lambda i: (aem(i, 0x0017).clear(), aem(i, 0x0017).update(
             copy.deepcopy(aem(i, 0x0016))), aem(i, 0x0016).clear(),
             aem(i, 0x0016).update({"NOTIMPL": {"floor": 12, "cdls": "12-100",
                                                "answers": {"1": "echo"}}})),
         ("decode: GET_CLOCK_SOURCE: the engine dispatches AEM 0x0017 to SCLKS",)),
        ("an entry the engine cannot dispatch to",
         lambda i: cmd(i, "GET_CLOCK_SOURCE")["entries"].append("SCFGRUN2"),
         ("GET_CLOCK_SOURCE: entry SCFGRUN2 has no UPC_SCFGRUN2_C",)),
        ("an entry the engine never dispatches the command to",
         lambda i: cmd(i, "GET_CLOCK_SOURCE")["entries"].append("SCFGRUN"),
         ("decode: GET_CLOCK_SOURCE: the table's entries name SCFGRUN",)),
        ("an engine entry no command claims",
         lambda i: (cmd(i, "GET_CONTROL")["entries"].remove("NSUPP1"),
                    cmd(i, "SET_CONTROL")["entries"].remove("NSUPP1")),
         ("UPC_NSUPP1_C is claimed by no command",)),
        ("a command cdl off the engine's decode floor",
         lambda i: table_and_doc(i, lambda t: next(
             c for c in t["commands"] if c["name"] == "GET_SAMPLING_RATE")["command"]
             .update(cdl=20)),
         ("decode: GET_SAMPLING_RATE: the shortest AEM 0x0015 the engine dispatches to "
          "GSRATE is cdl 16, the table's command cdl is 20",)),
        ("a decode floor moved in the engine (as recorded)",
         lambda i: aem(i, 0x0016)["SCLKS"].update(floor=16),
         ("decode: SET_CLOCK_SOURCE: the shortest AEM 0x0016", "is cdl 16")),
        ("a declared floor dropped from the table",
         lambda i: table_and_doc(i, lambda t: next(
             c for c in t["commands"] if c["name"] == "REGISTER_UNSOLICITED_NOTIFICATION")
             ["command"].pop("floor")),
         ("decode: REGISTER_UNSOLICITED_NOTIFICATION: the shortest AEM 0x0024",
          "the table's command cdl is 16")),
        ("a per-record command accepted between records (as recorded)",
         lambda i: aem(i, 0x002C)["AMADD"].update(cdls="20, 24, 28, 36"),
         ("decode: ADD_AUDIO_MAPPINGS: the engine dispatches AEM 0x002C to AMADD at cdl 24",)),
        ("a response the bench saw at another size (as recorded)",
         lambda i: aem(i, 0x0016)["SCLKS"]["answers"].update({"0": "24"}),
         ("decode: SET_CLOCK_SOURCE: SCLKS answered SUCCESS at cdl 24",)),
        ("a response the bench saw at the command's length on a fixed rule (as recorded)",
         lambda i: aem(i, 0x0029)["GCTRS"]["answers"].update({"0": "echo"}),
         ("decode: GET_COUNTERS: GCTRS answered SUCCESS at the command's own length",)),
        ("a status the bench saw outside the set (as recorded)",
         lambda i: aem(i, 0x0007)["GCFG"]["answers"].update({"8": "16"}),
         ("decode: GET_CONFIGURATION: GCFG answered NO_RESOURCES",)),
        ("a program that sent no frame (as recorded)",
         lambda i: aem(i, 0x0007)["GCFG"]["answers"].update({"none": "no frame"}),
         ("decode: GET_CONFIGURATION: AEM 0x0007 dispatched to GCFG sent no frame",)),
        ("a command answered without a program (as recorded)",
         lambda i: aem(i, 0x0002).update(engine={"floor": 12, "cdls": "12",
                                                 "answers": {"0": "32"}}),
         ("decode: ENTITY_AVAILABLE: the engine answers AEM 0x0002 without a program",)),
        ("a command the bench saw dropped (as recorded)",
         lambda i: aem(i, 0x0002).update(drop={"floor": 12, "cdls": "12",
                                               "answers": {"none": "no frame"}}),
         ("decode: ENTITY_AVAILABLE: the bench saw AEM 0x0002 dropped",)),
        ("a served command the sweep never sent (as recorded)",
         lambda i: i.record["decode"].pop("MVU"),
         ("decode: GET_MILAN_INFO: tb/aecp_cmds/decode.json has no MVU 0x0000",)),
        ("an unserved opcode decoded (as recorded)",
         lambda i: aem(i, 0x0003).update(GCFG={"floor": 12, "cdls": "12-100",
                                               "answers": {"0": "16"}}),
         ("decode: AEM 0x0003 is no table command", "GCFG")),
        ("another message type decoded as AEM (as recorded)",
         lambda i: group(i, "AVC", 0x0016).update(SCLKS={"floor": 24, "cdls": "24, 100",
                                                         "answers": {"0": "20"}}),
         ("decode: AVC 0x0016 is no table command", "SCLKS")),
        ("an engine source changed after the sweep",
         lambda i: i.swept.update({ENGINE: i.swept[ENGINE].replace(
             b"upc_r <= UPC_SCLKS_C;", b"upc_r <= UPC_GCLKS_C;")}),
         (f"decode: {RECORD} was swept from another {ENGINE}",
          "sweep this engine with make -C tb/aecp_cmds record")),
        ("the bench changed after the sweep",
         lambda i: i.swept.update({"tb/aecp_cmds/sim_main.cpp":
                                   i.swept["tb/aecp_cmds/sim_main.cpp"] + b"\n"}),
         (f"decode: {RECORD} was swept from another tb/aecp_cmds/sim_main.cpp",)),
        ("the ROM changed after the sweep",
         lambda i: ucode_of(i).rom.__setitem__(
             max(set(range(len(ucode_of(i).rom))) - ucode_of(i).occupied), 1),
         (f"decode: {RECORD} was swept from another {ROM_KEY}",)),
        ("a trial that never ended (as recorded)",
         lambda i: i.record["sweep"].update(timeouts=1),
         ("decode: tb/aecp_cmds/decode.json: 1 trials never ended",)),
        ("a record of another schema",
         lambda i: i.record.update(schema="aecp-decode-record/0"),
         ("decode: tb/aecp_cmds/decode.json declares 'aecp-decode-record/0'",)),
        ("a record without its parts",
         lambda i: i.record.pop("gdi"),
         ("decode: tb/aecp_cmds/decode.json is not a decode record",)),
        ("a GET_DYNAMIC_INFO member the engine refuses (as recorded)",
         lambda i: i.record["gdi"]["members"].pop("0x0029"),
         ("decode: GET_COUNTERS: the table makes it a GET_DYNAMIC_INFO member",)),
        ("a GET_DYNAMIC_INFO flag changed in the table",
         lambda i: table_and_doc(i, lambda t: next(
             c for c in t["commands"] if c["name"] == "GET_NAME").update(gdi=False)),
         ("decode: GET_NAME: the engine runs a record of it inside GET_DYNAMIC_INFO",)),
        ("a GET_DYNAMIC_INFO record length changed (as recorded)",
         lambda i: i.record["gdi"]["members"]["0x0029"].update(GCTRS="0:132"),
         ("decode: GET_COUNTERS: inside GET_DYNAMIC_INFO its GCTRS record is 132 bytes",)),
        ("a GET_DYNAMIC_INFO member that never runs its program (as recorded)",
         lambda i: i.record["gdi"]["members"]["0x0007"].pop("GCFG"),
         ("decode: GET_CONFIGURATION: inside GET_DYNAMIC_INFO the engine never dispatches it to GCFG",)),
        ("a GET_DYNAMIC_INFO record dispatched outside the entries (as recorded)",
         lambda i: i.record["gdi"]["members"]["0x0015"].update(GCLKS="0:8"),
         ("decode: GET_SAMPLING_RATE: inside GET_DYNAMIC_INFO the engine dispatches it to GCLKS",)),
        ("GET_DYNAMIC_INFO's empty aggregate at another size (as recorded)",
         lambda i: i.record["gdi"]["empty"].update(engine="0:cdl 16"),
         ("decode: GET_DYNAMIC_INFO: with no record the engine answers 0:cdl 16",)),
        ("a GET_DYNAMIC_INFO record that voids the aggregate (as recorded)",
         lambda i: i.record["gdi"]["members"]["0x0029"].update(GCTRS="10:aggregate cdl 12"),
         ("decode: GET_COUNTERS: inside GET_DYNAMIC_INFO its GCTRS record leaves the "
          "aggregate ENTITY_MISBEHAVING at aggregate cdl 12",)),
        ("GET_DYNAMIC_INFO's response cdl changed in the table",
         lambda i: table_and_doc(i, lambda t: next(
             c for c in t["commands"] if c["name"] == "GET_DYNAMIC_INFO")["response"]
             .update(cdl=16)),
         ("decode: GET_DYNAMIC_INFO: with no record the engine answers 0:cdl 12",)),
        ("an unsolicited job carrying another command_type (as recorded)",
         lambda i: i.record["unsolicited"]["PP_UNS_CFG_C"].update(GCFG=["0x0007:16"]),
         ("decode: SET_CONFIGURATION: the PP_UNS_CFG_C job answers",)),
        ("an unsolicited job no table command claims (as recorded)",
         lambda i: i.record["unsolicited"]["PP_UNS_CFG_C"].update(GCFG=["0x0007:16"]),
         ("decode: the engine's PP_UNS_CFG_C job answers 0x0007 with GCFG",)),
        ("an unsolicited body changed in the table",
         lambda i: cmd(i, "SET_CLOCK_SOURCE")["notify"].update(body="GSRATE"),
         ("decode: SET_CLOCK_SOURCE: the PP_UNS_CLKS_C job answers",)),
        ("an unsolicited kind the engine does not run",
         lambda i: cmd(i, "SET_CLOCK_SOURCE")["notify"].update(kind="PP_UNS_CLOCK_C"),
         ("decode: SET_CLOCK_SOURCE: the engine runs no PP_UNS_CLOCK_C job",)),
        ("an unsolicited job at another size (as recorded)",
         lambda i: i.record["unsolicited"]["PP_UNS_CLKS_C"].update(GCLKS=["0x0016:24"]),
         ("decode: SET_CLOCK_SOURCE: its unsolicited GCLKS is cdl 24",)),
        ("an unsolicited arm no command claims",
         lambda i: cmd(i, "GET_AVB_INFO").update(notify=None),
         ("decode: the engine's PP_UNS_AVB_C job answers 0x0027 with GAVB",)),
        # --- the other consumers ---
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
         lambda i: nop(i, "GSTRI", "BUILD_FLD", 3),
         ("rom: GET_STREAM_INFO: E_GSTRI answers SUCCESS at cdl",)),
        # --- every size held: the variable programs (R609-1) ---
        ("GET_AUDIO_MAP's response cdl 20, F06.14 regenerated",
         lambda i: table_and_doc(i, lambda t: next(
             c for c in t["commands"] if c["name"] == "GET_AUDIO_MAP")["response"]
             .update(cdl=20)),
         ("rom: GET_AUDIO_MAP: E_GAMAP answers SUCCESS at cdl 24 + 8·N",
          "the table's response is 20 + 8·N")),
        ("GET_AUDIO_MAP's response cdl 28, F06.14 regenerated",
         lambda i: table_and_doc(i, lambda t: next(
             c for c in t["commands"] if c["name"] == "GET_AUDIO_MAP")["response"]
             .update(cdl=28)),
         ("rom: GET_AUDIO_MAP: E_GAMAP answers SUCCESS at cdl 24 + 8·N",
          "the table's response is 28 + 8·N")),
        ("a header field NOPed in E_GAMAP",
         lambda i: nop(i, "GAMAP", "BUILD_FLD"),
         ("rom: GET_AUDIO_MAP: E_GAMAP answers SUCCESS at cdl 22",)),
        ("a header field NOPed in E_GAVB",
         lambda i: nop(i, "GAVB", "BUILD_FLD"),
         ("rom: GET_AVB_INFO: E_GAVB answers SUCCESS at cdl 28",)),
        ("a header field NOPed in E_GASP",
         lambda i: nop(i, "GASP", "BUILD_FLD"),
         ("rom: GET_AS_PATH: E_GASP answers SUCCESS at cdl 14",)),
        ("a header field NOPed in E_RDESC",
         lambda i: nop(i, "RDESC", "BUILD_FLD"),
         ("rom: READ_DESCRIPTOR: E_RDESC answers SUCCESS at cdl 14 + a copy",)),
        ("the record APPEND NOPed in E_GAMAP",
         lambda i: nop(i, "GAMAP", "APPEND"),
         ("rom: GET_AUDIO_MAP: its programs answer SUCCESS only at cdl 24",
          "no path builds the variable part")),
        ("the record APPEND NOPed in E_GAVB",
         lambda i: nop(i, "GAVB", "APPEND"),
         ("rom: GET_AVB_INFO: its programs answer SUCCESS only at cdl 32",)),
        ("the record APPEND NOPed in E_GASP",
         lambda i: nop(i, "GASP", "APPEND"),
         ("rom: GET_AS_PATH: its programs answer SUCCESS only at cdl 16",)),
        ("a per-record response read as a fixed one",
         lambda i: table_and_doc(i, lambda t: next(
             c for c in t["commands"] if c["name"] == "GET_AS_PATH")["response"]
             .pop("per")),
         ("rom: GET_AS_PATH: E_GASP answers SUCCESS at cdl 16 + 8·N, the table's response is 16",)),
        ("an echo row's response off its command size",
         lambda i: table_and_doc(i, lambda t: next(
             c for c in t["commands"] if c["name"] == "ACQUIRE_ENTITY")["response"]
             .update(cdl=32)),
         ("schema: ACQUIRE_ENTITY: an echo response is the command's own size",)),
        ("an echo row's command off its response size",
         lambda i: table_and_doc(i, lambda t: next(
             c for c in t["commands"] if c["name"] == "ADD_AUDIO_MAPPINGS")["command"]
             .update(cdl=24)),
         ("schema: ADD_AUDIO_MAPPINGS: an echo response is the command's own size",)),
        ("an unsolicited body of another size",
         lambda i: nop(i, "STRMUNS", "BUILD_FLD"),
         ("unsolicited body E_STRMUNS is cdl 12",)),
        ("a path into the ROM fill",
         lambda i: into_fill(i, "EAVL"),
         ("rom: ENTITY_AVAILABLE: a path from E_EAVL runs into ROM word",)),
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
        ("a status code off IEEE Table 7-141 (no RTL constant holds it)",
         lambda i: i.table["status_codes"]["STREAM_IS_RUNNING"].update(code=13),
         ("schema: status STREAM_IS_RUNNING is 13 in the table, IEEE 1722.1-2021 "
          "Table 7-141 gives 12",)),
        ("a status code that is not an integer",
         lambda i: i.table["status_codes"]["NO_RESOURCES"].update(code="8"),
         ("schema: status NO_RESOURCES: code must be an integer",)),
        ("a common status the table does not define",
         lambda i: i.table["common_status"]["AEM"].append("BUSY"),
         ("schema: common_status AEM: status BUSY is not in status_codes",)),
        ("an unserved program that is no shared entry",
         lambda i: i.table.update(unserved="NOTIMPL2"),
         ("schema: unserved 'NOTIMPL2' is no shared entry",)),
        ("a table of another schema",
         lambda i: i.table.update(schema="aecp-command-table/2"),
         (f"schema: {TABLE} declares 'aecp-command-table/2'",)),
        ("an empty part of the table",
         lambda i: i.table.update(hazard_classes={}),
         ("schema: status_codes, hazard_classes or commands is empty",)),
        ("a command without a field",
         lambda i: cmd(i, "GET_NAME").pop("hazard"),
         ("schema: GET_NAME: missing hazard",)),
        ("a command listed twice",
         lambda i: i.table["commands"].append(copy.deepcopy(cmd(i, "GET_NAME"))),
         ("schema: GET_NAME is listed twice",)),
        ("a message type that is neither AEM nor MVU",
         lambda i: cmd(i, "GET_MILAN_INFO").update(message="AAU"),
         ("schema: GET_MILAN_INFO: message 'AAU' is neither AEM nor MVU",)),
        ("the MVU row the generator reads, gone",
         lambda i: cmd(i, "GET_MILAN_INFO").update(message="AAU"),
         ("schema: GET_MILAN_INFO: no command with message MVU",)),
        ("a message type without its common statuses",
         lambda i: i.table["common_status"].pop("MVU"),
         ("schema: GET_MILAN_INFO: message 'MVU' has no common_status row",)),
        ("an opcode not written 0xHHHH",
         lambda i: cmd(i, "GET_NAME").update(opcode="0x11"),
         ("schema: GET_NAME: opcode '0x11' is not 0xHHHH",)),
        ("a hazard class the table does not define",
         lambda i: cmd(i, "GET_NAME").update(hazard="NAME_RD"),
         ("schema: GET_NAME: hazard NAME_RD is not in hazard_classes",)),
        ("a flag that is not true or false",
         lambda i: cmd(i, "GET_NAME").update(lock="no"),
         ("schema: GET_NAME: lock, gdi and oversize must be true or false",)),
        ("a size without an integer cdl",
         lambda i: cmd(i, "GET_NAME")["response"].update(cdl="84"),
         ("schema: GET_NAME: response has no integer cdl",)),
        ("a per-record size without its unit",
         lambda i: cmd(i, "GET_AS_PATH")["response"].pop("unit"),
         ("schema: GET_AS_PATH: response per must be an integer with its unit",)),
        ("a floor at or above the command cdl",
         lambda i: cmd(i, "ACQUIRE_ENTITY")["command"].update(floor=28),
         ("schema: ACQUIRE_ENTITY: command floor must be",)),
        ("an entry listed twice",
         lambda i: cmd(i, "GET_NAME")["entries"].append("GNAME"),
         ("schema: GET_NAME: entries must be a non-empty list of names without repeats",)),
        ("a notification without its body",
         lambda i: cmd(i, "SET_NAME")["notify"].pop("body"),
         ("schema: SET_NAME: notify must be null or carry class, kind and body",)),
        ("an unserved F06.14 row short of a cell",
         lambda i: i.table["documented"][0]["cells"].pop(),
         ("schema: documented row '0x0003': needs",)),
        ("an unserved F06.14 row without its name (R609-1 S2)",
         lambda i: i.table["documented"][0].pop("name"),
         ("schema: documented row '0x0003': needs",)),
        ("a constant the engine does not declare",
         lambda i: cmd(i, "GET_NAME").update(rtl="OP_GET_NAMES_C"),
         ("engine: GET_NAME: the table names OP_GET_NAMES_C", "declares no such constant")),
        ("a GDI_*_C renumbered in the table",
         lambda i: next(d for d in i.table["documented"]
                        if d.get("rtl") == "GDI_GET_VIDEO_FMT_C").update(opcode="0x000C"),
         ("engine: GET_VIDEO_FORMAT: the table says 0x000C", "GDI_GET_VIDEO_FMT_C = 0x000B")),
        ("a GDI_*_C the engine does not declare",
         lambda i: next(d for d in i.table["documented"]
                        if d.get("rtl") == "GDI_GET_VIDEO_FMT_C")
                   .update(rtl="GDI_GET_VIDEO_FORMAT_C"),
         ("engine: GET_VIDEO_FORMAT: the table names GDI_GET_VIDEO_FORMAT_C",)),
        ("a hazard code pp_pkg adds",
         lambda i: sub(i, PP_PKG, "PP_HZ_IDENTIFY    = 4'd8",
                         "PP_HZ_IDENTIFY    = 4'd8,\n    PP_HZ_SPARE       = 4'd9"),
         (f"classifier: {PP_PKG} declares PP_HZ_SPARE and the table does not",)),
        ("a non-AEM command off the classifier's default",
         lambda i: cmd(i, "GET_MILAN_INFO").update(hazard="MAP_CFG"),
         ("classifier: GET_MILAN_INFO: a non-AEM command takes the classifier's default",)),
        ("a class the notify block maps and does not queue",
         lambda i: sub(i, NOTIFY, "4'd5, 4'd7, 4'd8, 4'd9};", "4'd5, 4'd7, 4'd8};"),
         ("notify:", "class 9 (START_STREAMING) is in one only")),
        ("a class off the notify block's map whose kind another class carries",
         lambda i: cmd(i, "SET_NAME")["notify"].update(**{"class": 6}),
         ("notify: SET_NAME: class 6 is not in", "its kind PP_UNS_NAME_C is another class's")),
        ("a class the notify block maps and no command enqueues",
         lambda i: sub(i, NOTIFY, "        4'd9: pick_kind_w = PP_UNS_STRM_C;\n",
                         "        4'd9: pick_kind_w = PP_UNS_STRM_C;\n"
                         "        4'd10: pick_kind_w = PP_UNS_STRM_C;\n"),
         ("notify:", "maps class 10 to PP_UNS_STRM_C and no table command enqueues it")),
        ("a ucpu_pkg status the table does not name",
         lambda i: i.table["status_codes"]["NO_RESOURCES"].update(rtl=None),
         (f"status: {UCPU_PKG} declares ST_NO_RESOURCES_C and the table has no status for it",)),
        ("an engine-built status on a program-built response",
         lambda i: cmd(i, "GET_NAME").update(engine_status=["SUCCESS"]),
         ("rom: GET_NAME: engine_status is only for an engine-built response",)),
        ("an entry the generator never placed",
         lambda i: ucode_of(i).entry.pop("EAVL"),
         ("rom: ENTITY_AVAILABLE: entry EAVL is no placed program",)),
        ("a status code the table does not know set in a program",
         lambda i: set_imm(i, rom_op(i, "SCFGRUN", "SET_STATUS"), 13),
         ("rom: SET_CONFIGURATION: a path sets status 13, which the table does not know",)),
        ("a notification body the generator never placed",
         lambda i: ucode_of(i).entry.pop("STRMUNS"),
         ("rom: START_STREAMING: notification body STRMUNS is no placed program",)),
        ("F06.14's header changed",
         lambda i: sub(i, DOC, "| Opcode | Command | Mandate |", "| Op | Command | Mandate |"),
         ("F06.14: the header is not",)),
        ("a row repeated in F06.14",
         lambda i: doc_rows(i, lambda r: r + [r[row_at(r, "0x0029")]]),
         ("F06.14: 42 rows where the table generates 41",)),
        ("a row in F06.14 the table does not generate",
         lambda i: doc_cell(i, "0x0029", "| GET_COUNTERS |", "| GET_COUNTER |"),
         ("F06.14: row 0x0029 GET_COUNTER is not in the table",)),
        ("an F06.14 row with an extra cell",
         lambda i: doc_rows(i, lambda r: [x + " extra |" if x.startswith("| 0x0029 |") else x
                                          for x in r]),
         ("F06.14: row 0x0029 GET_COUNTERS has 13 cells, not 12",)),
        ("F06.14 rows out of order",
         lambda i: doc_rows(i, lambda r: swap(r, "0x0029", "0x002B")),
         ("F06.14: the rows are out of the generated order",)),
        ("section 8.1 listing a command the engine does not serve",
         lambda i: sub(i, DOC, "| 0x0002 ENTITY_AVAILABLE |",
                         "| 0x0002 ENTITY_AVAILABLE, 0x0003 CONTROLLER_AVAILABLE |"),
         ("8.1: section 8.1 lists AEM 0x0003 and the table serves no such command",)),
        ("a parsed block renamed (a stale pattern)",
         lambda i: sub(i, TOP, "always_comb begin : hz_classify",
                         "always_comb begin : hz_classifier"),
         ("gate refused", "hz_classify", "the pattern has gone stale")),
        ("a parsed block with no end",
         lambda i: sub_all(i, NOTIFY, "endcase", "endcas"),
         ("gate refused", f"{NOTIFY} emit_pick: no ")),
        ("the engine's entries written in another form",
         lambda i: sub_all(i, ENGINE, "localparam logic [10:0] UPC_",
                             "localparam logic [11:0] UPC_"),
         ("gate refused", f"{ENGINE}: a pattern parsed nothing")),
        ("hz_classify's opcode case written in another form",
         lambda i: sub(i, TOP, "unique case (hz_opcode_w)", "case (hz_opcode_w)"),
         ("gate refused", "hz_classify's default or its opcode case is not parseable")),
        ("hz_classify's opcode labels written in another form",
         lambda i: sub_all(i, TOP, "16'h", "16'H"),
         ("gate refused", "hz_classify's opcode case parsed nothing")),
        ("pp_pkg's hazard codes written in another form",
         lambda i: sub_all(i, PP_PKG, "= 4'd", "= 4'h"),
         ("gate refused", "no PP_HZ_* enum parsed")),
        ("hz_is_lockprot naming a class without its alias",
         lambda i: sub(i, SCOREBOARD,
                       "localparam logic [3:0] HZ_NAME_WR_C     = pp_pkg::PP_HZ_NAME_WR;", ""),
         ("gate refused", "hz_is_lockprot is not parseable")),
        ("the notify block's queued classes written in another form",
         lambda i: sub(i, NOTIFY, "assign cmd_class_ok_w = ev_cmd_class_i inside {",
                         "assign cmd_class_ok_w = (ev_cmd_class_i) inside {"),
         ("gate refused", "the command-class map is not parseable")),
        ("ucpu_pkg's status codes written in another form",
         lambda i: sub_all(i, UCPU_PKG, "localparam logic [4:0] ST_", "localparam logic [5:0] ST_"),
         ("gate refused", "no ST_*_C parsed")),
        ("F06.14's end marker gone",
         lambda i: sub(i, DOC, END, ""),
         ("gate refused", "the F06.14 markers are missing")),
        ("section 8.1's heading renamed",
         lambda i: sub(i, DOC, "### 8.1 Realization status", "### 8.1 Realisation status"),
         ("gate refused", "section 8.1's table is not parseable")),
    ]


def edit_table(fn):
    """A file mutation: load the copy's table, apply `fn`, write it back."""
    def go(d: Path) -> None:
        t = json.loads((d / TABLE).read_text(encoding="utf-8"))
        fn(t)
        (d / TABLE).write_text(json.dumps(t, indent=2, ensure_ascii=False) + "\n",
                               encoding="utf-8")
    return go


def named(t: dict, name: str) -> dict:
    """A table row by name, for a file mutation."""
    return next(c for c in t["commands"] if c["name"] == name)


def file_mutants() -> list:
    """(what, mutation of a copy of the input files, words) per refusal that
    happens while the inputs are read, and per table defect the generator reads:
    planted in the file, so the gate and the generator run on it as they would
    on a real edit (R608-1 F2)."""
    return [
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
    ]


def generator_mutants() -> list:
    """(what, mutation of a copy of the table, words) per field the generator
    itself refuses: run gen_ucode.py alone on the copy, as `make` does, so its
    own refusal names the command and the field."""
    return [
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
    ]


def copy_inputs(root: Path, d: Path) -> None:
    """The gate's input files under a fresh directory."""
    for p in (TABLE, UCODE, RECORD) + TEXTS + SWEPT_SV + SWEPT_RAW:
        (d / p).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / p, d / p)


def planted_copy(root: Path, fn) -> list:
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


def generator_copy(root: Path, fn) -> list:
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


def selftest(root: Path) -> int:
    """The tree passes, and each planted defect fails with its words."""
    base = load(root)
    first = check(base)
    if first:
        print("AECP COMMAND GATE SELFTEST: FAIL - the unmutated tree does not pass:")
        print("\n".join(first))
        return 1
    cases = mutants() + file_mutants() + generator_mutants()
    bad = 0
    for what, fn, words in mutants():
        inp = Inputs(copy.deepcopy(base.table), dict(base.texts), copy.deepcopy(base.ucode),
                     copy.deepcopy(base.record), base.root, dict(base.swept))
        try:
            fn(inp)
            got = check(inp)
        except GateError as exc:
            got = [f"  (gate refused: {exc})"]
        bad += missed(what, words, got)
    for what, fn, words in file_mutants():
        bad += missed(what, words, planted_copy(root, fn))
    for what, fn, words in generator_mutants():
        bad += missed(what, words, generator_copy(root, fn))
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
    new = regenerated(inp.table, doc)
    if new != doc:
        (root / DOC).write_text(new, encoding="utf-8")
        print(f"aecp commands: rewrote F06.14 in {DOC}")
    else:
        print(f"aecp commands: F06.14 in {DOC} is current")
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
