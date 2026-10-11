# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""tb/aecp_cmds' decode record: what the elaborated engine was observed to
decode and dispatch, by program name. Written from a sweep (`canonical`,
`dump_record`), pinned to its inputs by digest (`check_swept`), compared with a
fresh sweep (`record_diff`), and held to the table (`check_record`)."""
import copy
import json
from math import gcd
from collections.abc import Iterable

from .inputs import (HDR, OUTCOMES, RECORD, RECORD_SCHEMA, ROM_KEY, SWEPT_RAW, SWEPT_SV,
                     Inputs, Ucode, digests)
from .table import fits_exact, opcode_of, size_text

ABOUT = [
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
]
SWEEP = {
    "opcodes": "0x0000-0x007F on every bucket; MVU: command_type 0x0000-0x007F",
    "cdl": "12-100 with descriptor_type @24; 16, 24, 96 running; the other templates at "
           "their own lengths (tb/aecp_cmds/sim_main.cpp); every program replayed down to 12",
    "descriptor_types": "0x0000-0x0028, 0xFFFF",
    "build": "EN_IDENTIFY_NOTIF_P = 1 (the IDENTIFY_NOTIFICATION job has its program)",
}


def ranges(nums: Iterable[int]) -> str:
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


def hexranges(nums: Iterable[int]) -> str:
    """[3, 5, 6, 7] -> '0x0003, 0x0005-0x0007'."""
    return ", ".join("-".join(f"0x{int(x):04X}" for x in r.split("-"))
                     for r in ranges(nums).split(", ") if r)


def unranges(text: object) -> list:
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


def classify(pairs: list) -> dict:
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


def _prog(name: dict, o: str) -> str:
    """A raw outcome ('810', '810+812', 'engine') by program name."""
    return "+".join(name.get(int(x), f"µPC {x}") if x.isdigit() else x for x in o.split("+"))


def _decode_groups(raw: dict, name: dict) -> dict:
    """Per message bucket, the opcodes the engine treats alike, grouped."""
    decode = {}
    for key, outs in raw["decode"].items():
        bucket, op = key.rsplit(" ", 1)
        progs = {}
        for o, by_cdl in outs.items():
            cdls = sorted(int(c) for c in by_cdl)
            pairs = [(int(c), a) for c, ans in by_cdl.items() for a in ans]
            progs[_prog(name, o)] = {"floor": cdls[0], "cdls": ranges(cdls),
                                     "answers": classify(pairs)}
        decode.setdefault(bucket, {})[int(op, 16)] = dict(sorted(progs.items()))
    groups = {}
    for bucket, by_op in sorted(decode.items()):
        sigs = {}
        for op, progs in sorted(by_op.items()):
            sigs.setdefault(json.dumps(progs, sort_keys=True), (progs, []))[1].append(op)
        groups[bucket] = [{"opcodes": hexranges(ops), "programs": progs}
                          for progs, ops in sorted(sigs.values(), key=lambda g: g[1][0])]
    return groups


def _aggregate(a: str, ct: int) -> tuple:
    """One aggregate answer as (status, the record's length), or the
    aggregate's own cdl when it carries no whole record."""
    s, c = (int(x) for x in a.split(":"))
    if ct < 0:
        return s, f"cdl {c}"
    return (s, c - HDR - 8) if c >= HDR + 8 else (s, f"aggregate cdl {c}")


def _gdi(raw: dict, name: dict) -> dict:
    """GET_DYNAMIC_INFO: the members, the refused command_types, the empty
    aggregate."""
    members, refused, empty = {}, {}, {}
    for key, outs in sorted(raw["gdi"].items(), key=lambda x: int(x[0])):
        ct = int(key)
        lens = {}
        for o, ans in outs.items():
            got = sorted({_aggregate(a, ct) for a in ans if a != "none"}, key=str)
            lens[_prog(name, o.removeprefix("batch:")) if o.startswith("batch:")
                 else ("copied" if o == "engine" and ct >= 0 else _prog(name, o))] = \
                (", ".join(f"{s}:{n}" for s, n in got) or "no frame",
                 o.startswith("batch:") or o == "engine")
        if ct < 0:
            empty = {k: v for k, (v, _) in lens.items()}
        elif not any(member for _, member in lens.values()):
            refused.setdefault("+".join(sorted(lens)), []).append(ct)
        else:
            members[f"0x{ct:04X}"] = {k: v for k, (v, _) in sorted(lens.items())}
    return {"members": members,
            "refused": {k: hexranges(v) for k, v in sorted(refused.items())},
            "empty": empty}


def canonical(raw: dict, uc: Ucode, kinds: dict, inputs: dict) -> dict:
    """The bench's raw output (µPC addresses) as the decode record (names),
    with the digests of what was swept."""
    name = {at: e for e, at in uc.entry.items()}
    uns = {}
    for k, outs in sorted(raw["unsolicited"].items(), key=lambda x: int(x[0])):
        uns[kinds.get(int(k), f"kind {k}")] = {_prog(name, o): sorted(v)
                                               for o, v in sorted(outs.items())}
    return {
        "schema": RECORD_SCHEMA,
        "about": list(ABOUT),
        "sweep": {"trials": raw["trials"], "timeouts": raw["timeouts"], **SWEEP, "inputs": inputs},
        "decode": _decode_groups(raw, name),
        "gdi": _gdi(raw, name),
        "unsolicited": uns,
    }


def _one(x: object) -> str:
    return json.dumps(x, ensure_ascii=False)


def _comma(i: int, n: int) -> str:
    return "," if i < n - 1 else ""


def _dump_decode(v: dict) -> list:
    """The decode part: one line per program."""
    lines = ['  "decode": {']
    for j, (b, groups) in enumerate(v.items()):
        lines.append(f"    {_one(b)}: [")
        for gi, g in enumerate(groups):
            lines.append(f'      {{"opcodes": {_one(g["opcodes"])}, "programs": {{')
            progs = list(g["programs"].items())
            for pi, (pn, info) in enumerate(progs):
                lines.append(f"        {_one(pn)}: {_one(info)}{_comma(pi, len(progs))}")
            lines.append(f"      }}}}{_comma(gi, len(groups))}")
        lines.append(f"    ]{_comma(j, len(v))}")
    return lines


def _dump_object(k: str, v: dict) -> list:
    """The gdi, unsolicited or sweep part: one line per member or job kind."""
    lines = [f"  {_one(k)}: {{"]
    items = list(v.items())
    for j, (a, b) in enumerate(items):
        if isinstance(b, dict) and k in ("gdi", "sweep"):
            lines.append(f"    {_one(a)}: {{")
            sub = list(b.items())
            for si, (c, d) in enumerate(sub):
                lines.append(f"      {_one(c)}: {_one(d)}{_comma(si, len(sub))}")
            lines.append(f"    }}{_comma(j, len(items))}")
        else:
            lines.append(f"    {_one(a)}: {_one(b)}{_comma(j, len(items))}")
    return lines


def dump_record(rec: dict) -> str:
    """decode.json's text: one line per program, group and job kind."""
    lines = ["{"]
    keys = list(rec)
    for i, k in enumerate(keys):
        tail = _comma(i, len(keys))
        v = rec[k]
        if k == "decode":
            lines += _dump_decode(v) + [f"  }}{tail}"]
        elif k in ("gdi", "unsolicited", "sweep"):
            lines += _dump_object(k, v) + [f"  }}{tail}"]
        elif k == "about":
            lines.append('  "about": [')
            lines += [f"    {_one(a)}{_comma(j, len(v))}" for j, a in enumerate(v)]
            lines.append(f"  ]{tail}")
        else:
            lines.append(f"  {_one(k)}: {_one(v)}{tail}")
    return "\n".join(lines + ["}"]) + "\n"


def answer_lens(spec: str) -> list:
    """'echo', '20' or '20, 28' -> ['echo'] / [20] / [20, 28]."""
    return ["echo"] if spec == "echo" else [int(x) for x in spec.split(",")]


class _Checks:
    """check_record's findings and its tally: a check is made by `done`, and it
    failed if it added a finding since the last one."""

    def __init__(self) -> None:
        self.p: list = []
        self.n = 0
        self.f = 0
        self.mark = 0

    def done(self) -> None:
        """One check made: it failed if it added a finding."""
        self.n, self.f, self.mark = self.n + 1, self.f + (len(self.p) > self.mark), len(self.p)


def _dispatch(c: dict, progs: dict, at: str, where: str, ck: _Checks) -> bool:
    """The programs the opcode reached are the entries, both ways, and the
    engine builds the response exactly when the table says so; returns whether
    the entries are what the engine dispatches to."""
    name, rsp = c["name"], c["response"]
    disp = {k for k in progs if k not in OUTCOMES}
    want = set(c["entries"])
    for e in sorted(want - disp):
        ck.p.append(f"  decode: {name}: the table's entries name {e}, which the engine "
                    f"never dispatches {at} to ({where})")
    for e in sorted(disp - want):
        ck.p.append(f"  decode: {name}: the engine dispatches {at} to {e}, which is not "
                    f"one of the table's entries ({where})")
    ck.done()
    if ("engine" in progs) != bool(rsp.get("engine")):
        ck.p.append(f"  decode: {name}: the engine "
                    f"{'answers' if 'engine' in progs else 'never answers'} {at} without a "
                    f"program, the table says its response is "
                    f"{'engine' if rsp.get('engine') else 'program'}-built")
    for bad in ("drop", "timeout"):
        if bad in progs:
            ck.p.append(f"  decode: {name}: the bench saw {at} "
                        f"{'dropped' if bad == 'drop' else 'never finish'} at cdl "
                        f"{progs[bad]['cdls']}")
    ck.done()
    return want == disp


def _floor(c: dict, progs: dict, at: str, ck: _Checks) -> None:
    """The shortest command dispatched to the first entry is the command cdl or
    its declared floor, and a per-record command is accepted at whole records
    only."""
    name, cmd_ = c["name"], c["command"]
    first = "engine" if c["response"].get("engine") else c["entries"][0]
    floor = cmd_.get("floor", cmd_["cdl"])
    got = progs.get(first, {}).get("floor")
    how = "answers without a program" if first == "engine" else f"dispatches to {first}"
    if got is not None and got != floor:
        ck.p.append(f"  decode: {name}: the shortest {at} the engine {how} "
                    f"is cdl {got}, the table's "
                    f"{'floor' if 'floor' in cmd_ else 'command cdl'} is {floor}")
    if got is not None and cmd_.get("per"):
        step = gcd(*(x - floor for x in unranges(progs[first]["cdls"])))
        if step != cmd_["per"]:
            ck.p.append(f"  decode: {name}: command per is {cmd_['per']}, "
                        f"the gcd of accepted offsets from floor {floor} is {step}")
        odd = [x for x in unranges(progs[first]["cdls"]) if (x - floor) % cmd_["per"]]
        if odd:
            did = (f"answers {at} without a program" if first == "engine"
                   else f"dispatches {at} to {first}")
            ck.p.append(f"  decode: {name}: the engine {did} at cdl "
                        f"{ranges(odd)}, which is no {size_text(cmd_, False)}")
    ck.done()


def _answers(t: dict, c: dict, progs: dict, at: str, ck: _Checks) -> None:
    """Every response the bench saw has a status of the set and the table's size."""
    name, rsp = c["name"], c["response"]
    code_of = {r["code"]: s for s, r in t["status_codes"].items()}
    echo_shared = set(t.get("echo_entries", []))
    allowed = set(c["status"]) | set(t["common_status"].get(c["message"], [])) \
        | set(c.get("engine_status", []))
    for pn, info in sorted(progs.items()):
        echo_ok = pn in echo_shared or bool(rsp.get("echo"))
        who = "its engine-built answer" if pn == "engine" else pn
        sent = set(unranges(info["cdls"]))
        for s, spec in info["answers"].items():
            if s == "none":
                if pn not in OUTCOMES:
                    ck.p.append(f"  decode: {name}: {at} dispatched to {pn} sent no frame")
                continue
            st = code_of.get(int(s), f"status {s}")
            if st not in allowed:
                ck.p.append(f"  decode: {name}: {who} answered {st}, which is not in the "
                            "table's status set")
            for ln in answer_lens(spec):
                if ln == "echo":
                    if not echo_ok:
                        ck.p.append(f"  decode: {name}: {who} answered {st} at the command's "
                                    f"own length, and the table's response is no echo")
                elif not fits_exact(ln, rsp, int(s)) and not (echo_ok and ln in sent):
                    ck.p.append(f"  decode: {name}: {who} answered {st} at cdl {ln}, the "
                                f"table's response is {size_text(rsp, False)}")
    ck.done()


def _served(t: dict, c: dict, groups: dict, where: str, ck: _Checks) -> bool:
    """One served command against what its opcode was observed to do; returns
    whether its entries are what the engine dispatches to."""
    at = f"{c['message']} {c['opcode']}"
    progs = groups.get((c["message"], opcode_of(c["opcode"])))
    if progs is None:
        ck.p.append(f"  decode: {c['name']}: {where} has no {at}: the sweep never sent it")
        ck.done()
        return False
    ok = _dispatch(c, progs, at, where, ck)
    _floor(c, progs, at, ck)
    _answers(t, c, progs, at, ck)
    return ok


def _unserved(t: dict, rec: dict, served: set, ck: _Checks) -> None:
    """Every opcode the table does not serve reaches only the unserved program."""
    unserved = t.get("unserved")
    for b, gs in sorted(rec["decode"].items()):
        for g in gs:
            ops = [op for op in unranges(g["opcodes"]) if (b, op) not in served]
            if not ops:
                continue
            for pn in sorted(set(g["programs"]) - {unserved}):
                ck.p.append(f"  decode: {b} {hexranges(ops)} is no table command and the "
                            f"engine dispatches it to {pn}, not only {unserved}")
            ck.done()


def _gdi_record(t: dict, c: dict, m: dict, ck: _Checks) -> None:
    """One GET_DYNAMIC_INFO member: the program its record runs and its length."""
    code_of = {r["code"]: s for s, r in t["status_codes"].items()}
    ran = {k for k in m if k not in ("copied",) + OUTCOMES}
    if c["entries"][0] not in ran:
        ck.p.append(f"  decode: {c['name']}: inside GET_DYNAMIC_INFO the engine never "
                    f"dispatches it to {c['entries'][0]}")
    for e in sorted(ran - set(c["entries"])):
        ck.p.append(f"  decode: {c['name']}: inside GET_DYNAMIC_INFO the engine dispatches "
                    f"it to {e}, which is not one of its entries")
    for e in sorted(ran):
        for tok in m[e].split(", "):
            s, _, ln = tok.partition(":")
            if not s.isdigit() or s != "0" or not ln.isdigit():
                ck.p.append(f"  decode: {c['name']}: inside GET_DYNAMIC_INFO its {e} record "
                            f"leaves the aggregate {code_of.get(int(s), s) if s.isdigit() else s}"
                            f" at {ln or 'no frame'}, not SUCCESS with the record")
            elif int(ln) != c["response"]["cdl"] - HDR:
                ck.p.append(f"  decode: {c['name']}: inside GET_DYNAMIC_INFO its {e} record "
                            f"is {ln} bytes, the table's response is cdl "
                            f"{c['response']['cdl']} ({c['response']['cdl'] - HDR} bytes)")
    ck.done()


def _gdi_members(t: dict, gdi: dict, ck: _Checks) -> None:
    """GET_DYNAMIC_INFO: its members, what each record runs, how long it is,
    and the aggregate with no record."""
    members, empty = gdi["members"], gdi["empty"]
    docs = {opcode_of(d["opcode"]): d["name"] for d in t.get("documented", []) if d.get("gdi")}
    cmds = {opcode_of(c["opcode"]): c for c in t["commands"] if c["message"] == "AEM"}
    want = {op for op, c in cmds.items() if c["gdi"]} | set(docs)
    have = {int(k, 16) for k in members}
    for op in sorted(want - have):
        who = cmds[op]["name"] if op in cmds else docs[op]
        ck.p.append(f"  decode: {who}: the table makes it a GET_DYNAMIC_INFO member and the "
                    "engine refuses a record of it")
    for op in sorted(have - want):
        who = cmds[op]["name"] if op in cmds else f"0x{op:04X}"
        ck.p.append(f"  decode: {who}: the engine runs a record of it inside GET_DYNAMIC_INFO "
                    "and the table says it is no member")
    ck.done()
    for op in sorted(want & have):
        if op in cmds:
            _gdi_record(t, cmds[op], members[f"0x{op:04X}"], ck)
    for c in t["commands"]:
        if c["response"].get("engine"):
            got = empty.get("engine")
            if got != f"0:cdl {c['response']['cdl']}":
                ck.p.append(f"  decode: {c['name']}: with no record the engine answers {got}, "
                            f"the table's response is SUCCESS at {size_text(c['response'], False)}")
            ck.done()


def _unsolicited(t: dict, uns: dict, ck: _Checks) -> None:
    """Every unsolicited job: its kind's command_type, body and size."""
    for c in t["commands"]:
        nt = c["notify"]
        if not nt:
            continue
        got = uns.get(nt["kind"])
        if got is None:
            ck.p.append(f"  decode: {c['name']}: the engine runs no {nt['kind']} job")
            ck.done()
            continue
        pairs = {(x.split(":")[0], pn) for pn, v in got.items() for x in v if x != "none"}
        if (c["opcode"], nt["body"]) not in pairs:
            ck.p.append(f"  decode: {c['name']}: the {nt['kind']} job answers "
                        f"{', '.join(f'{ct} with {pn}' for ct, pn in sorted(pairs)) or 'nothing'}, "
                        f"the table says {c['opcode']} with {nt['body']}")
        rule = {k: v for k, v in c["response"].items() if k != "stub"}
        for pn, v in got.items():
            for x in v:
                ct, _, ln = x.partition(":")
                if x != "none" and ct == c["opcode"] and pn == nt["body"] \
                        and not (fits_exact(int(ln), rule) or (rule.get("echo") and int(ln) == HDR)):
                    ck.p.append(f"  decode: {c['name']}: its unsolicited {pn} is cdl {ln}, the "
                                f"table's response is {size_text(c['response'], False)}")
        ck.done()
    bodies = {(c["notify"]["kind"], c["opcode"], c["notify"]["body"])
              for c in t["commands"] if c["notify"]}
    for k, got in sorted(uns.items()):
        for pn, v in sorted(got.items()):
            for x in v:
                ct = x.split(":")[0]
                if x != "none" and (k, ct, pn) not in bodies:
                    ck.p.append(f"  decode: the engine's {k} job answers {ct} with {pn}, which "
                                "is no table command's notification")
        ck.done()


def check_record(t: dict, rec: dict, where: str = RECORD) -> tuple:
    """The table against what the engine was observed to decode and dispatch:
    (findings, checks made, checks failed, whether every command's entries are
    observed - the field the generator reads)."""
    if rec.get("schema") != RECORD_SCHEMA:
        return [f"  decode: {where} declares {rec.get('schema')!r}, this gate reads "
                f"{RECORD_SCHEMA!r}"], 1, 1, False
    try:
        groups = {(b, op): g["programs"] for b, gs in rec["decode"].items()
                  for g in gs for op in unranges(g["opcodes"])}
        gdi, uns = rec["gdi"], rec["unsolicited"]
        gdi = {"members": gdi["members"], "empty": gdi["empty"]}
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        return [f"  decode: {where} is not a decode record: {exc!r}"], 1, 1, False
    ck = _Checks()
    if rec.get("sweep", {}).get("timeouts"):
        ck.p.append(f"  decode: {where}: {rec['sweep']['timeouts']} trials never ended")
    ck.done()
    entries_ok = True
    served = set()
    for c in t["commands"]:
        served.add((c["message"], opcode_of(c["opcode"])))
        entries_ok &= _served(t, c, groups, where, ck)
    _unserved(t, rec, served, ck)
    _gdi_members(t, gdi, ck)
    _unsolicited(t, uns, ck)
    return ck.p, ck.n, ck.f, entries_ok


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


def split_group(rec: dict, bucket: str, op: int) -> dict:
    """The programs of one opcode in a decode record, split out of its group
    so an edit touches that opcode alone (the self-test's plants)."""
    gs = rec["decode"][bucket]
    g = next(g for g in gs if op in unranges(g["opcodes"]))
    rest = [x for x in unranges(g["opcodes"]) if x != op]
    if rest:
        g["opcodes"] = hexranges(rest)
        g = {"opcodes": f"0x{op:04X}", "programs": copy.deepcopy(g["programs"])}
        gs.append(g)
    return g["programs"]
