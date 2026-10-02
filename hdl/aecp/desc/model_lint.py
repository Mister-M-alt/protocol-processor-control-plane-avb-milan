# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Semantic lint of a descriptor model: docs/architecture/07 §3.1, L1 to L11.

`gen_desc_image.build()` runs it by default, after its layout checks and
before it renders the image, so one check guards every consumer of the packer
(issue #89). The opt-out, `build(..., lint=False)` or `--no-lint`, is for
layout vectors and for deliberate negative cases another checker must name;
the layout report then says so.

The rules are in model_rules.py. They judge the packed bytes at their
IEEE 1722.1-2021 §7.2 wire offsets and read every variable part through the
descriptor's own offset field. A field a rule needs that lies outside the
descriptor is a finding of that rule. A finding names its rule, one check of
`CHECKS`, the descriptor and the clause:

    L1 port-cluster-minimum: cfg 0 STREAM_PORT_INPUT 3: number_of_clusters 0
    ... (Milan v1.2 §5.3.3.8)

A waiver (`lint_waivers` in the packed document) excuses one check on a named
scope, never a whole rule, and carries a reason naming its tracking issue:

    {"rule": "L1", "check": "port-cluster-minimum", "configuration": 0,
     "type": "STREAM_PORT_INPUT", "first": 0, "last": 7,
     "reason": "owner/repo#123: what is wrong and who tracks it"}

`first` and `last` bound an inclusive index range; a check that names a
configuration takes neither. Applied waivers are listed in the report. A
malformed waiver is refused, and so is a stale one: any descriptor of its
scope missing, or passing its check.

The report gains the values the integrator drives on `entity_model_id_i`,
`talker_sources_i`, `listener_sinks_i` and `identify_index_i`, which stay
integrator inputs, and the model digest: SHA-256 over every descriptor with
the IEEE 1722.1-2021 §6.2.2.8 exclusions, entity_id and entity_model_id
zeroed. `adp` refuses a driven value that disagrees with the model;
`model_ids` refuses a recorded entity_model_id whose digest moved (L9).
"""
import hashlib
import re
import struct
from collections import defaultdict
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from typing import Any

from model_rules import (CHECKS, RULES, D, Finding, RuleContext, Where, rule_identity,
                         survey, type_name, where_text)

ISSUE_REF = re.compile(r"[\w.-]+(?:/[\w.-]+)?#\d+")

#: Types with no object_name at bytes 4..67 (IEEE 1722.1-2021 §7.2): the
#: digest zeroes that span in every other type.
UNNAMED = frozenset({0x00, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13,
                     0x17, 0x18, 0x19})


@dataclass(frozen=True)
class Waiver:
    """One check excused on one scope, with the reason that tracks it."""
    number: int
    check: str
    where: Where
    last: int | None
    reason: str

    def label(self) -> str:
        """`L1 port-cluster-minimum: cfg 0 STREAM_PORT_INPUT 0..7`."""
        scope = where_text(self.where)
        if self.last is not None and self.last != self.where[2]:
            scope += f"..{self.last}"
        return f"{CHECKS[self.check][0]} {self.check}: {scope}"

    def indices(self) -> list[int | None]:
        """The descriptor indices the waiver names, or [None] for a configuration."""
        first = self.where[2]
        if first is None or self.last is None:
            return [None]
        return list(range(first, self.last + 1))

    def covers(self, finding: Finding) -> bool:
        """Whether this waiver excuses the finding."""
        cfg, dtype, index = finding.where
        if finding.check != self.check or (cfg, dtype) != self.where[:2]:
            return False
        return index in self.indices()


@dataclass
class LintResult:
    """What `lint()` hands the packer: the refusals and the report lines."""
    refusals: list[str]
    report: list[str]
    digest: str


def _zero(body: bytearray, start: int, end: int) -> None:
    """Zero body[start:end], clipped to the descriptor."""
    end = min(end, len(body))
    if start < end:
        body[start:end] = bytes(end - start)


def _structural(dtype: int, body: bytes) -> bytes:
    """A descriptor with the IEEE 1722.1-2021 §6.2.2.8 exclusions zeroed, plus
    the per-unit identity (entity_id) and entity_model_id itself."""
    out = bytearray(body)
    if dtype not in UNNAMED:
        _zero(out, 4, 68)
    spans = {D.ENTITY: ((4, 20), (36, 112), (116, 308), (310, 312)),
             D.AUDIO_UNIT: ((136, 140),),
             D.STREAM_INPUT: ((74, 82),), D.STREAM_OUTPUT: ((74, 82),),
             D.CLOCK_SOURCE: ((70, 72), (74, 82)),
             D.CLOCK_DOMAIN: ((70, 72),),
             D.AVB_INTERFACE: ((70, 76), (78, 96))}
    for start, end in spans.get(dtype, ()):
        _zero(out, start, end)
    if dtype == D.CONTROL and len(out) >= 98:
        value_type = struct.unpack_from(">H", out, 80)[0] & 0x3FFF
        offset, count = struct.unpack_from(">HH", out, 94)
        width = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 8, 7: 8, 8: 4, 9: 8}.get(value_type)
        if width is None:
            _zero(out, offset, len(out))
        for k in range(count if width else 0):
            at = offset + k * (5 * width + 4) + 4 * width
            _zero(out, at, at + width)
    return bytes(out)


def model_digest(model: dict[int, dict[int, dict[int, bytes]]]) -> str:
    """SHA-256 of the model's structure: every descriptor in (configuration,
    type, index) order, each keyed and length-prefixed, exclusions zeroed."""
    digest = hashlib.sha256()
    for cfg in sorted(model):
        for dtype in sorted(model[cfg]):
            for index, body in sorted(model[cfg][dtype].items()):
                digest.update(struct.pack(">HHHH", cfg, dtype, index, len(body)))
                digest.update(_structural(dtype, body))
    return digest.hexdigest()


def _parse_waiver(number: int, item: Any, type_code: Callable[[Any], int]) -> Waiver:
    """One `lint_waivers` entry, or ValueError saying what is wrong with it."""
    if not isinstance(item, dict):
        raise ValueError("is not an object")
    allowed = {"rule", "check", "configuration", "type", "first", "last", "reason"}
    unknown = sorted(set(item) - allowed)
    missing = sorted({"rule", "check", "configuration", "type", "reason"} - set(item))
    if unknown or missing:
        raise ValueError(f"unknown keys {unknown}, missing keys {missing}")
    check = str(item["check"])
    if check not in CHECKS:
        raise ValueError(f"names no check {check!r}")
    if str(item["rule"]) != CHECKS[check][0]:
        raise ValueError(f"names rule {item['rule']}, and {check} is a {CHECKS[check][0]} check")
    reason = str(item["reason"]).strip()
    if not ISSUE_REF.search(reason):
        raise ValueError("carries no reason naming a tracking issue (repo#N)")
    if ("first" in item) != ("last" in item):
        raise ValueError("gives one of first and last")
    first = int(item["first"]) if "first" in item else None
    last = int(item["last"]) if "last" in item else None
    if first is not None and last is not None and not 0 <= first <= last:
        raise ValueError(f"has the empty range {first}..{last}")
    return Waiver(number, check, (int(item["configuration"]), type_code(item["type"]), first),
                  last, reason)


def _apply(ctx: RuleContext, waivers: list[Waiver]) -> tuple[list[Finding], list[str], list[str]]:
    """(findings no waiver covers, report lines of applied waivers, stale refusals)."""
    covered: dict[int, set[int | None]] = defaultdict(set)
    excused: dict[int, int] = defaultdict(int)
    left = []
    for finding in ctx.findings:
        hits = [w for w in waivers if w.covers(finding)]
        for waiver in hits:
            covered[waiver.number].add(finding.where[2])
            excused[waiver.number] += 1
        if not hits:
            left.append(finding)
    applied, stale = [], []
    for waiver in waivers:
        cfg, dtype, _ = waiver.where
        problems = []
        for index in waiver.indices():
            if cfg not in ctx.model:
                problems.append(f"configuration {cfg} does not exist")
            elif index is not None and index not in ctx.of(cfg, dtype):
                problems.append(f"{type_name(dtype)} {index} does not exist")
            elif index not in covered[waiver.number]:
                scope = f"{type_name(dtype)} {index}" if index is not None else f"configuration {cfg}"
                problems.append(f"its check passes for {scope}")
        if problems:
            stale.append(f"lint waiver {waiver.number} ({waiver.label()}) is stale: "
                         + "; ".join(problems) + "; remove or narrow it")
        else:
            applied.append(f"  waiver {waiver.label()}: {excused[waiver.number]} finding(s) "
                           f"waived; reason: {waiver.reason}")
    return left, applied, stale


def _report(ctx: RuleContext, applied: list[str], digest: str, recorded: bool) -> list[str]:
    """The lint's part of the layout report."""
    lines = ["semantic lint: on (07 §3.1 rules L1 to L11)"]
    lines += [f"lint waivers applied: {len(applied)}"] + applied
    lines.append("ADP inputs this model requires (Milan v1.2 §5.3.3.1, §5.6.2):")
    for port in ("entity_model_id_i", "talker_sources_i", "listener_sinks_i", "identify_index_i"):
        value = ctx.values.get(port)
        shown = "-" if value is None else (f"0x{value:016X}" if port == "entity_model_id_i"
                                           else str(value))
        checked = "  (driven value checked)" if ctx.adp.get(port[:-2]) is not None else ""
        lines.append(f"  {port:<18} {shown}{checked}")
    checked = ", recorded digest checked" if recorded else ""
    lines.append(f"model digest sha256 {digest} (IEEE 1722.1-2021 §6.2.2.8{checked})")
    return lines


def lint(groups: dict[tuple[int, int], dict[int, tuple[bytes, int]]], *,
         waivers: Iterable[Any] = (), adp: dict[str, int] | None = None,
         model_ids: dict[str, str] | None = None,
         type_code: Callable[[Any], int] = int) -> LintResult:
    """Every rule over the packer's descriptor groups, then the waivers.

    `groups` is `gen_desc_image._grouped_descriptors()`'s
    {(configuration, type): {index: (body, name_index)}}. `adp` holds driven
    values to check: entity_model_id, talker_sources, listener_sinks and
    identify_index. `model_ids` maps a recorded entity_model_id (hex) to its
    digest. `type_code` resolves a waiver's type name.
    """
    model: dict[int, dict[int, dict[int, bytes]]] = defaultdict(dict)
    for (cfg, dtype), members in groups.items():
        model[cfg][dtype] = {index: body for index, (body, _) in members.items()}
    ctx = RuleContext(dict(model), dict(adp or {}))
    digest = model_digest(ctx.model)
    survey(ctx)
    for rule in RULES:
        rule(ctx)
    rule_identity(ctx, model_ids, digest)
    parsed, refusals = [], []
    for number, item in enumerate(waivers):
        try:
            parsed.append(_parse_waiver(number, item, type_code))
        except (ValueError, TypeError, KeyError) as exc:
            refusals.append(f"lint waiver {number} {exc}")
    left, applied, stale = _apply(ctx, parsed)
    refusals += [finding.text() for finding in left] + stale
    return LintResult(refusals, _report(ctx, applied, digest, model_ids is not None), digest)
