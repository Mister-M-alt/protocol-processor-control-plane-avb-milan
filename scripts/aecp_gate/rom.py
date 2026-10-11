# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The ROM consumer of the AECP command table: every path of every program
the generator placed, walked abstractly with the µCPU's own semantics
(KL_aecp_ucpu.sv), and held to the table's lock, notification, status set and
response size."""
import math
from dataclasses import dataclass, field
from typing import NamedTuple

from .inputs import HDR, UCODE, Ucode
from .table import fits_exact, size_text

#! distinct exact lengths a walk state keeps before it widens to a loop's
#! fixed part plus whole records
WIDEN = 4
#! a field's byte width by its µop format
WIDTH = {0: 1, 1: 2, 2: 4, 3: 8}
#! the µops that branch to their immediate's target as well as falling through
BRANCHES = ("BRANCH", "BR_STATUS", "CHECK_LOCK", "CHECK_ARG", "MAP_VALID")


@dataclass(frozen=True)
class Var:
    """A length a path cannot fix: `base` bytes, the part every instance has,
    plus whole records of `per` bytes (a loop of unknown count), or plus any
    number of bytes when `per` is None (a copy of unknown length)."""

    base: int
    per: int | None = None

    def __add__(self, n: int) -> "Var":
        return Var(self.base + n, self.per)

    def __str__(self) -> str:
        return f"{self.base} + {self.per}·N" if self.per else f"{self.base} + a copy"


#: a length on a path: exact, or a Var once a loop or a copy makes it unknown
Length = int | Var


def covers(v: Var | None, ln: Length) -> bool:
    """Whether the instances of `v` include every instance of `ln`."""
    if v is None:
        return False
    if isinstance(ln, Var):
        if ln.base < v.base or v.per is None:
            return ln.base >= v.base
        return ln.per is not None and ln.per % v.per == 0 and (ln.base - v.base) % v.per == 0
    return ln >= v.base and (v.per is None or (ln - v.base) % v.per == 0)


def join(a: Var | None, b: Var) -> Var:
    """The least Var holding both."""
    if a is None:
        return b
    if a.per is None or b.per is None:
        return Var(min(a.base, b.base))
    return Var(min(a.base, b.base), math.gcd(a.per, b.per, abs(a.base - b.base)))


def absorb(seen: dict, key: tuple, ln: Length, ovf: bool | None) -> tuple | None:
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


class _State(NamedTuple):
    """One point of one path: the µPC, the statuses the register may hold, the
    response length, the constants MOVEd into registers (sorted pairs), the
    ITER (count, index) when known, the overflow flag and the length at the
    first COPY_BUFFER."""

    pc: int
    st: frozenset
    ln: Length
    regs: tuple
    it: tuple | None
    ovf: bool | None
    cp: int | None


class _Word(NamedTuple):
    """One µop's fields (ucpu_pkg.sv encoding)."""

    op: str | None
    rd: int
    ra: int
    fmt: int
    cnd: int
    imm: int


def _decode(uc: Ucode, names: dict, pc: int) -> _Word:
    w = uc.rom[pc]
    return _Word(names.get(w >> 43), (w >> 39) & 15, (w >> 35) & 15, (w >> 28) & 7,
                 (w >> 24) & 15, w & 0xFFFFFF)


def _branches(w: _Word, s: _State, facts: Facts) -> list | None:
    """The successors of a branching µop, in the order the walk explores them;
    None when `w` falls through to the next word alone."""
    tgt, nxt = w.imm & 0x7FF, s.pc + 1
    if w.op == "BRANCH":
        return [s._replace(pc=tgt)]
    if w.op == "BR_STATUS":
        if w.cnd == 0:
            out = [s._replace(pc=tgt, st=s.st - {0})] if s.st - {0} else []
            return out + ([s._replace(pc=nxt, st=s.st & {0})] if s.st & {0} else [])
        if w.cnd == 1 and s.it is not None and s.it[0] is not None:
            return [s._replace(pc=tgt if s.it[1] == s.it[0] else nxt)]
        if w.cnd == 4 and s.ovf is not None and not isinstance(s.ln, Var):
            return [s._replace(pc=tgt if s.ovf else nxt)]
        if w.cnd in (1, 2, 3, 4):
            return [s._replace(pc=tgt), s._replace(pc=nxt)]
        return [s._replace(pc=nxt)]
    refusal = {"CHECK_LOCK": 3, "CHECK_ARG": 11 if w.cnd & 8 else 7, "MAP_VALID": 7}.get(w.op)
    if refusal is None:
        return None
    facts.lock |= w.op == "CHECK_LOCK"
    return [s._replace(pc=tgt, st=frozenset({refusal})), s._replace(pc=nxt)]


def _registers(w: _Word, rv: dict, it: tuple | None) -> tuple | None:
    """Track the constants a µop leaves in registers (`rv`, in place) and the
    ITER count and index; returns the ITER state after `w`."""
    if w.op == "MOVE":
        if w.ra == 0:
            rv[w.rd] = w.imm
        elif w.ra in rv:
            rv[w.rd] = rv[w.ra] & ((1 << (8 * WIDTH[w.fmt])) - 1)
        else:
            rv.pop(w.rd, None)
    elif w.op == "SHIFT_R":
        if w.ra in rv:
            rv[w.rd] = rv[w.ra] >> (w.imm & 63)
        else:
            rv.pop(w.rd, None)
    elif w.op in ("SET_MASKED", "READ_ST", "NAME_RD", "GATHER_EXT"):
        rv.pop(w.rd, None)
    elif w.op == "ITER_OPEN":
        #! a count the walk does not know leaves the index untracked too,
        #! so the loop is one state the walk revisits and widens
        it = (rv[w.ra] & 0xFF, 0) if w.ra in rv else (None, None)
    elif w.op == "ITER_NEXT" and it is not None and it[0] is not None:
        it = (it[0], (it[1] + 1) & 0xFF)
    return it


def _length(w: _Word, s: _State, rv: dict) -> tuple:
    """(length, overflow, copy start) after a µop that builds the response."""
    ln, ovf, cp = s.ln, s.ovf, s.cp
    var = isinstance(ln, Var)
    if w.op in ("BUILD_FLD", "APPEND"):
        cap = 592 if (w.op == "APPEND" and w.cnd & 1) else 524
        if w.op == "APPEND" and not var and ln + WIDTH[w.fmt] > cap:
            ovf = True
        else:
            ln = ln + WIDTH[w.fmt]
    elif w.op == "READ_CTRS":
        ln = ln + 16
    elif w.op == "COPY_BUF":
        count = None if w.ra not in rv else (
            (rv[w.ra] & 0xFFFF) - (w.imm & 0xFFF) if w.cnd & 1 else rv[w.ra] & 0xFFFF)
        if cp is None:
            cp = ln.base if var else ln
        if count is not None and count >= 0:
            ln = ln + count
        else:
            ln = Var(ln.base if var else ln)
    elif w.op == "SET_LENGTH":
        ln = w.imm & 0x7FF
    return ln, ovf, cp


def _straight(w: _Word, s: _State, faces: bool, facts: Facts) -> _State:
    """The one successor of a µop that falls through to the next word."""
    st = s.st
    if w.op == "SET_STATUS":
        st = frozenset({w.imm & 31})
    elif w.op in ("DESC_ADDR", "READ_ST", "WRITE_ST") and faces:
        st = st | {{"DESC_ADDR": 2, "READ_ST": 7, "WRITE_ST": 10}[w.op]}
    rv = dict(s.regs)
    it = _registers(w, rv, s.it)
    ln, ovf, cp = _length(w, s, rv)
    if w.op == "SEND_RESP":
        for code in st:
            facts.sends.add((code, ln, cp))
    elif w.op == "NOTIFY_ENQ":
        facts.notify.add(w.imm & 15)
    if isinstance(ln, Var):
        ovf = None
    return _State(s.pc + 1, frozenset(st), ln, tuple(sorted(rv.items())), it, ovf, cp)


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
    names = {v: k for k, v in uc.ops.items()}
    facts = Facts()
    seen = {}
    work = [_State(start, frozenset({0}), HDR, ((0, 0),), None, False, None)]
    while work:
        s = work.pop()
        got = absorb(seen, (s.pc, s.st, s.regs, s.it, s.cp), s.ln, s.ovf)
        if got is None:
            continue
        s = s._replace(ln=got[0], ovf=got[1])
        if not 0 <= s.pc < len(uc.rom) or s.pc not in uc.occupied:
            facts.fill.add(s.pc)
            continue
        w = _decode(uc, names, s.pc)
        if w.op == "END":
            continue
        branched = _branches(w, s, facts)
        work += branched if branched is not None else [_straight(w, s, faces, facts)]
    return facts


def entry_facts(uc: Ucode, at: int) -> tuple:
    """(definite, with-faces) facts from one entry."""
    return walk(uc, at, False), walk(uc, at, True)


def reachable(uc: Ucode, start: int) -> list:
    """Every ROM word a path from `start` can execute, in address order."""
    names = {v: k for k, v in uc.ops.items()}
    seen, work = set(), [start]
    while work:
        pc = work.pop()
        if pc in seen or pc not in uc.occupied:
            continue
        seen.add(pc)
        op = names.get(uc.rom[pc] >> 43)
        if op == "END":
            continue
        if op in BRANCHES:
            work.append(uc.rom[pc] & 0x7FF)
        if op != "BRANCH":
            work.append(pc + 1)
    return sorted(seen)


def fits(ln: Length, rsp: dict, echo: bool, status: int = 0, cp: int | None = None) -> bool:
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


@dataclass
class _Paths:
    """What every entry of one command does, summed: the statuses its paths
    set (definitely, and with the faces' errors), CHECK_LOCK, the NOTIFY_ENQ
    classes and the (length, copy start) of every SUCCESS."""

    definite: set = field(default_factory=set)
    faced: set = field(default_factory=set)
    lock: bool = False
    notes: set = field(default_factory=set)
    success: set = field(default_factory=set)


def _entries(t: dict, c: dict, uc: Ucode, paths: _Paths) -> list:
    """Walk each entry of `c`: its fill, and every answer's size."""
    p = []
    name, rsp = c["name"], c["response"]
    code_of = {r["code"]: n for n, r in t["status_codes"].items()}
    echo_shared = set(t.get("echo_entries", []))
    for e in c["entries"]:
        at = uc.entry.get(e)
        if at is None or at not in uc.placed:
            p.append(f"  rom: {name}: entry {e} is no placed program in {UCODE}")
            continue
        d, m = entry_facts(uc, at)
        for pc in sorted(d.fill | m.fill):
            p.append(f"  rom: {name}: a path from E_{e} runs into ROM word {pc}, "
                     "which no program placed (the fill)")
        paths.definite |= {s for s, _, _ in d.sends}
        paths.faced |= {s for s, _, _ in m.sends}
        paths.lock |= m.lock
        paths.notes |= m.notify
        paths.success |= {(ln, cp) for s, ln, cp in m.sends if s == 0}
        for s, ln, cp in sorted(m.sends, key=str):
            if not fits(ln, rsp, e in echo_shared or bool(rsp.get("echo")), s, cp):
                copy = f" (its copy starting at {cp})" if cp is not None and rsp.get("plus") else ""
                p.append(f"  rom: {name}: E_{e} answers {code_of.get(s, s)} at cdl "
                         f"{ln}{copy}, "
                         f"the table's response is {size_text(rsp, False)}"
                         f"{' (echo)' if rsp.get('echo') else ''}")
    return p


def _statuses(t: dict, c: dict, paths: _Paths) -> list:
    """The variable part some SUCCESS builds, and the status set both ways."""
    p = []
    name, rsp = c["name"], c["response"]
    code_of = {r["code"]: n for n, r in t["status_codes"].items()}
    common = set(t["common_status"].get(c["message"], [])) | set(c.get("engine_status", []))
    #! a variable rule is a promise of a variable part: some SUCCESS path
    #! must build it, or the records were never emitted
    if (rsp.get("per") or rsp.get("plus")) and not rsp.get("echo") and not rsp.get("engine") \
            and not any(isinstance(ln, Var) and fits(ln, rsp, False, 0, cp)
                        for ln, cp in paths.success):
        p.append(f"  rom: {name}: its programs answer SUCCESS only at cdl "
                 f"{', '.join(sorted({str(ln) for ln, _ in paths.success})) or 'nothing'}, never "
                 f"{size_text(rsp, False)}: no path builds the variable part")
    for s in sorted(paths.definite | paths.faced):
        if s not in code_of:
            p.append(f"  rom: {name}: a path sets status {s}, which the table "
                     "does not know")
    must = {code_of[s] for s in paths.definite if s in code_of}
    may = {code_of[s] for s in paths.faced if s in code_of}
    listed = set(c["status"])
    for s in sorted(must - listed - common):
        p.append(f"  rom: {name}: status {s} is answered by a path of its programs "
                 "and missing from the table's status set")
    for s in sorted(listed - may - common):
        p.append(f"  rom: {name}: status {s} is in the table's status set and no "
                 "path of its programs can answer it")
    return p


def _lock_and_notify(c: dict, uc: Ucode, paths: _Paths) -> list:
    """CHECK_LOCK, the NOTIFY_ENQ class, and the unsolicited body's size."""
    p = []
    name, rsp = c["name"], c["response"]
    if paths.lock != c["lock"]:
        p.append(f"  rom: {name}: the table says lock {str(c['lock']).lower()}, its "
                 f"programs {'do' if paths.lock else 'never'} reach CHECK_LOCK")
    want = {c["notify"]["class"]} if c["notify"] and c["notify"]["class"] is not None else set()
    if paths.notes != want:
        p.append(f"  rom: {name}: its programs enqueue notification class "
                 f"{sorted(paths.notes) or 'none'}, the table says {sorted(want) or 'none'}")
    if not c["notify"]:
        return p
    body = c["notify"]["body"]
    at = uc.entry.get(body)
    if at is None or at not in uc.placed:
        return p + [f"  rom: {name}: notification body {body} is no placed program"]
    for s, ln, cp in sorted(walk(uc, at, True).sends, key=str):
        if not fits(ln, {k: v for k, v in rsp.items() if k != "stub"},
                    bool(rsp.get("echo")), 0, cp):
            p.append(f"  rom: {name}: its unsolicited body E_{body} is cdl "
                     f"{ln}, the table's response is {size_text(rsp, False)}")
    return p


def check_rom(t: dict, uc: Ucode) -> list:
    """Walk every entry of every command through the generated ROM."""
    p = []
    for c in t["commands"]:
        #! an engine-built answer (GET_DYNAMIC_INFO's aggregate) has statuses no
        #! program sets; they are named, and only such a command may name them
        if c.get("engine_status") and not c["response"].get("engine"):
            p.append(f"  rom: {c['name']}: engine_status is only for an engine-built response")
        paths = _Paths()
        p += _entries(t, c, uc, paths)
        p += _statuses(t, c, paths)
        p += _lock_and_notify(c, uc, paths)
    return p
