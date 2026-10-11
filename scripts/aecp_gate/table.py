# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The AECP command table's own shape: the fields every consumer reads, and
the fields gen_ucode.py reads, so a table it cannot use is refused by name
before it runs; and the size rules every consumer compares with."""
import re
from dataclasses import dataclass, field

from .inputs import COLUMNS, HDR, SCHEMA, SECTIONS, TABLE, UCODE

#! IEEE 1722.1-2021 Table 7-141, the AEM status codes: a code the table names
#! is held here, because a code no RTL constant carries (STREAM_IS_RUNNING)
#! reaches the ROM from the table alone
IEEE_STATUS = {"SUCCESS": 0, "NOT_IMPLEMENTED": 1, "NO_SUCH_DESCRIPTOR": 2,
               "ENTITY_LOCKED": 3, "ENTITY_ACQUIRED": 4, "NOT_AUTHENTICATED": 5,
               "AUTHENTICATION_DISABLED": 6, "BAD_ARGUMENTS": 7, "NO_RESOURCES": 8,
               "IN_PROGRESS": 9, "ENTITY_MISBEHAVING": 10, "NOT_SUPPORTED": 11,
               "STREAM_IS_RUNNING": 12}
REQUIRED = ("name", "message", "opcode", "rtl", "clause", "command", "response", "status",
            "hazard", "lock", "notify", "gdi", "oversize", "entries", "doc")


def opcode_of(text: object) -> int:
    """'0x0015' -> 0x15; anything else -> -1."""
    if isinstance(text, str) and re.fullmatch(r"0x[0-9A-F]{4}", text):
        return int(text, 16)
    return -1


def size_text(s: dict, octets: bool) -> str:
    """A size rule as text: cdl, or AECPDU octets (12 + cdl)."""
    base = s["cdl"] + (HDR if octets else 0)
    out = str(base)
    if s.get("per"):
        out += f" + {s['per']}·N"
    if s.get("plus"):
        out += f" + {s['plus']}"
    return out


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


def check_size(who: str, k: str, s: object) -> list:
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


@dataclass
class _Seen:
    """The command names and (message, opcode) pairs met so far."""

    names: dict = field(default_factory=dict)
    ops: dict = field(default_factory=dict)


def _status_rows(t: dict, codes: dict) -> list:
    """The status codes against Table 7-141, the common statuses and the
    unserved program."""
    p = []
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
    return p


def _identity(t: dict, c: dict, who: str, seen: _Seen) -> list:
    """A command's name, message type and opcode, each once."""
    p = []
    if c["name"] in seen.names:
        p.append(f"  schema: {who} is listed twice")
    seen.names[c["name"]] = c
    if c["message"] not in ("AEM", "MVU"):
        p.append(f"  schema: {who}: message {c['message']!r} is neither AEM nor MVU")
    if c["message"] not in t.get("common_status", {}):
        p.append(f"  schema: {who}: message {c['message']!r} has no common_status row")
    op = opcode_of(c["opcode"])
    if op < 0:
        p.append(f"  schema: {who}: opcode {c['opcode']!r} is not 0xHHHH")
    elif (c["message"], op) in seen.ops:
        p.append(f"  schema: {who}: opcode {c['opcode']} is also "
                 f"{seen.ops[(c['message'], op)]}'s")
    seen.ops[(c["message"], op)] = who
    return p


def _sizes(c: dict, who: str) -> list:
    """A command's two size rules, and an echo response's size is the command's."""
    p = []
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
    return p


def _command(t: dict, c: dict, who: str, seen: _Seen) -> list:
    """One command row, every field after its identity in table order."""
    p = _identity(t, c, who, seen)
    codes, hz = t.get("status_codes", {}), t.get("hazard_classes", {})
    for s in c["status"] + c.get("engine_status", []):
        if s not in codes:
            p.append(f"  schema: {who}: status {s} is not in status_codes")
    if c["hazard"] not in hz:
        p.append(f"  schema: {who}: hazard {c['hazard']} is not in hazard_classes")
    if not isinstance(c["lock"], bool) or not isinstance(c["gdi"], bool) \
            or not isinstance(c["oversize"], bool):
        p.append(f"  schema: {who}: lock, gdi and oversize must be true or false")
    p += _sizes(c, who)
    if not isinstance(c["entries"], list) or not c["entries"] \
            or len(set(c["entries"])) != len(c["entries"]) \
            or not all(isinstance(e, str) for e in c["entries"]):
        p.append(f"  schema: {who}: entries must be a non-empty list of names without repeats")
    nt = c["notify"]
    if nt is not None and not (isinstance(nt, dict) and {"class", "kind", "body"} <= set(nt)):
        p.append(f"  schema: {who}: notify must be null or carry class, kind and body")
    return p


def _documented(t: dict) -> list:
    """The F06.14 rows of commands the engine does not serve."""
    p = []
    for d in t.get("documented", []):
        if not isinstance(d, dict) or not {"section", "opcode", "name", "cells"} <= set(d) \
                or d["section"] not in SECTIONS or len(d["cells"]) != len(COLUMNS) - 2:
            p.append(f"  schema: documented row {d.get('opcode') if isinstance(d, dict) else d!r}: "
                     f"needs an opcode, a name, a section of {SECTIONS} and "
                     f"{len(COLUMNS) - 2} cells")
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
    p += _status_rows(t, codes)
    seen = _Seen()
    for i, c in enumerate(cmds):
        who = c.get("name", f"commands[{i}]")
        miss = [k for k in REQUIRED if k not in c]
        if miss:
            p.append(f"  schema: {who}: missing {', '.join(miss)}")
            continue
        p += _command(t, c, who, seen)
    if ("MVU", "GET_MILAN_INFO") not in {(c.get("message"), c.get("name")) for c in cmds}:
        p.append("  schema: GET_MILAN_INFO: no command with message MVU, whose opcode "
                 f"{UCODE} reads as its command_type")
    return p + _documented(t)
