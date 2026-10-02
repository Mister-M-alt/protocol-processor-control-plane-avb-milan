# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The rules of the descriptor model lint: docs/architecture/07 §3.1, L1 to L11.

`model_lint.lint()` runs them; this module holds what they judge and how. A
rule reads the packed bytes at their IEEE 1722.1-2021 §7.2 wire offsets,
reads every variable part through the descriptor's own offset field, and
records a `Finding` of one check of `CHECKS`. A field it needs that lies
outside the descriptor is a finding of that check.
"""
import struct
from collections import defaultdict
from dataclasses import dataclass, field
from enum import IntEnum


class D(IntEnum):
    """IEEE 1722.1-2021 Table 7-1 descriptor types the rules read."""
    ENTITY = 0x0000
    CONFIGURATION = 0x0001
    AUDIO_UNIT = 0x0002
    STREAM_INPUT = 0x0005
    STREAM_OUTPUT = 0x0006
    AVB_INTERFACE = 0x0009
    CLOCK_SOURCE = 0x000A
    LOCALE = 0x000C
    STRINGS = 0x000D
    STREAM_PORT_INPUT = 0x000E
    STREAM_PORT_OUTPUT = 0x000F
    AUDIO_CLUSTER = 0x0014
    AUDIO_MAP = 0x0017
    CONTROL = 0x001A
    CLOCK_DOMAIN = 0x0024
    TIMING = 0x0026


#: IEEE 1722.1-2021 §7.2.2: the types a CONFIGURATION's descriptor_counts may
#: list ("the counts of the top level descriptors").
TOP_LEVEL = frozenset({0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                       0x0B, 0x0C, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
                       0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27})

CRF_MILAN = 0x041060010000BB80          # Milan v1.2 Table 7.1
IDENTIFY = 0x90E0F00000000001           # IEEE 1722.1-2021 Table 7-98
BUFFER_MIN_NS = 2_126_000               # Milan v1.2 §5.3.3.4
FORMATS_OFFSET = 138                    # IEEE 1722.1-2021 Table 7-8
FORMATS_MAX = 46                        # (508 - 138) // 8, IEEE §7.2
RATES_OFFSET = 144                      # IEEE 1722.1-2021 §7.2.3
RATES_MAX = 8                           # the SET_SAMPLING_RATE walk, 07 §3.1
SOURCES_OFFSET = 76                     # IEEE 1722.1-2021 §7.2.32
CLASS_A = 0x0002                        # stream_flags, IEEE Table 7-9
BASE_SPF = {5: 6, 7: 12, 9: 24}         # Milan v1.2 §6.2: nsr -> samples
BASE_RATE = {5: "48 kHz", 7: "96 kHz", 9: "192 kHz"}
BASE_CHANNELS = (1, 2, 4, 6, 8)         # Milan v1.2 §6.2
INTERNAL, EXTERNAL, INPUT_STREAM = 0x0000, 0x0001, 0x0002   # IEEE §7.2.9.2

MILAN = "Milan v1.2 "
IEEE = "IEEE 1722.1-2021 "
#: check -> (rule, clause, what the check requires)
CHECKS = {
    "entity-count": ("L1", MILAN + "§5.3.3.1; " + IEEE + "§7.4.5.2",
                     "exactly one ENTITY, in configuration 0"),
    "configurations-count": ("L1", IEEE + "§7.2.1", "ENTITY configurations_count equals the image's"),
    "current-configuration": ("L1", IEEE + "§7.2.1",
                              "ENTITY current_configuration names a configuration"),
    "configuration-descriptors": ("L1", MILAN + "§5.3.2; " + IEEE + "§7.2.2, §7.4.5.2",
                                  "one CONFIGURATION per configuration, in configuration 0"),
    "descriptor-counts": ("L1", IEEE + "§7.2.2",
                          "descriptor_counts equal the configuration's top-level counts"),
    "required-type": ("L1", MILAN + "§5.3.2, §5.3.3.5, §5.3.3.6, §5.3.3.11",
                      "an AVB_INTERFACE, a CLOCK_DOMAIN and a CLOCK_SOURCE per configuration"),
    "audio-unit-for-aaf": ("L1", MILAN + "§5.3.3.3", "an AUDIO_UNIT where a stream carries AAF"),
    "stream-port-for-aaf": ("L1", MILAN + "§5.3.3.7",
                            "a Stream Port in the direction of an AAF stream"),
    "port-cluster-minimum": ("L1", MILAN + "§5.3.3.8", "at least one AUDIO_CLUSTER per Stream Port"),
    "child-exists": ("L1", MILAN + "§5.3.2; " + IEEE + "§7.2.3, §7.2.8, §7.2.13",
                     "a child range names existing descriptors"),
    "single-parent": ("L1", MILAN + "§5.3.2", "no descriptor has two parents"),
    "has-parent": ("L1", MILAN + "§5.3.2",
                   "every Stream Port, AUDIO_CLUSTER and AUDIO_MAP has a parent"),
    "parent-order": ("L2", IEEE + "§7.2", "child indices follow the hierarchy in order"),
    "stream-presence": ("L3", MILAN + "§5.3.3.4", "a STREAM_INPUT or STREAM_OUTPUT per configuration"),
    "talker-base-format": ("L3", MILAN + "§6.3", "a Talker has a Stream Output with a Base format"),
    "listener-base-format": ("L3", MILAN + "§6.4", "a Listener has a Stream Input with a Base format"),
    "base-channel-completeness": ("L3", MILAN + "§6.4",
                                  "a Base rate on an input carries every Base channel count"),
    "base-rate-uniformity": ("L3", MILAN + "§6.4",
                             "Base-format inputs of a configuration share their Base rates"),
    "crf-format": ("L3", MILAN + "§7.3.1, §7.3.4", "a CRF stream lists 0x041060010000BB80"),
    "buffer-length": ("L4", MILAN + "§5.3.3.4", "STREAM_INPUT buffer_length >= 2126000 ns"),
    "class-a": ("L4", MILAN + "§5.3.3.4, §7.3.3", "stream_flags carries CLASS_A"),
    "format-family": ("L4", MILAN + "§5.3.3.4", "CRF and AAF never share a format list"),
    "current-format": ("L4", MILAN + "§5.3.3.4", "current_format is in the format list"),
    "format-count": ("L4", IEEE + "§7.2, §7.2.6 Table 7-8", "number_of_formats <= 46"),
    "stream-layout": ("L4", MILAN + "§5.3.3.4; " + IEEE + "§7.2.6 Table 7-8",
                      "formats at 138, no redundancy tail (non-redundant PAAD)"),
    "interface-index": ("L5", MILAN + "§5.3.3.5",
                        "the same AVB_INTERFACE index per physical port in every configuration"),
    "domain-source-offset": ("L6", IEEE + "§7.2.32; 06 §6.4", "clock_sources_offset 76"),
    "domain-source-count": ("L6", MILAN + "§5.3.3.6", "at least one CLOCK_SOURCE per CLOCK_DOMAIN"),
    "domain-source-length": ("L6", IEEE + "§7.2.32", "CLOCK_DOMAIN length 76 + 2 x count"),
    "domain-source-identity": ("L6", IEEE + "§7.4.23.1; 06 §6.4",
                               "clock_sources is the identity list 0..count-1"),
    "domain-source-exists": ("L6", IEEE + "§7.2.32", "clock_sources names existing CLOCK_SOURCEs"),
    "crf-input-source": ("L6", MILAN + "§5.3.3.6", "exactly one INPUT_STREAM source per CRF input"),
    "aaf-input-source": ("L6", MILAN + "§5.3.3.6",
                         "without a CRF input, exactly one INPUT_STREAM source at an AAF input"),
    "internal-source": ("L6", MILAN + "§5.3.3.6", "an INTERNAL source where a Stream Output exists"),
    "gptp-source-interfaces": ("L6", MILAN + "§5.3.3.6, §7.5.1",
                               "a gPTP media clock only with a single AVB_INTERFACE"),
    "input-port-maps": ("L7", MILAN + "§5.3.3.7, §5.3.3.9", "no AUDIO_MAP on a Stream Port Input"),
    "unique-mapping": ("L7", MILAN + "§5.3.3.9",
                       "at most one static mapping per output stream channel"),
    "cluster-channels": ("L7", MILAN + "§5.3.3.8", "AUDIO_CLUSTER channel_count 1"),
    "identify-index": ("L8", MILAN + "§5.3.3.10, §5.6.2",
                       "an IDENTIFY CONTROL at one index in every configuration"),
    "identify-driven": ("L8", MILAN + "§5.3.3.10, §5.6.2", "identify_index_i names that CONTROL"),
    "model-id-valid": ("L9", MILAN + "§5.3.3.1, §5.6.2", "entity_model_id neither 0 nor all ones"),
    "model-id-driven": ("L9", MILAN + "§5.6.2; " + IEEE + "§7.2.1",
                        "entity_model_id_i equals the ENTITY descriptor's"),
    "model-id-recorded": ("L9", IEEE + "§6.2.2.8", "the entity_model_id has a recorded digest"),
    "model-digest": ("L9", MILAN + "§5.3.1; " + IEEE + "§6.2.2.8",
                     "a structural change carries a new entity_model_id"),
    "rate-offset": ("L10", IEEE + "§7.2.3, §7.4.21.1; 07 §3.1", "sampling_rates_offset 144"),
    "rate-count": ("L10", "07 §3.1 L10", "sampling_rates_count <= 8"),
    "rate-empty": ("L10", MILAN + "§5.3.3.3", "at least one sampling rate"),
    "rate-length": ("L10", IEEE + "§7.2.3; 07 §3.1", "AUDIO_UNIT length 144 + 4 x count"),
    "current-rate": ("L10", MILAN + "§5.3.3.3", "current_sampling_rate is a listed word"),
    "talker-sources": ("L11", MILAN + "§5.3.3.1",
                       "talker_stream_sources is the most STREAM_OUTPUTs of any configuration"),
    "listener-sinks": ("L11", MILAN + "§5.3.3.1",
                       "listener_stream_sinks is the most STREAM_INPUTs of any configuration"),
    "talker-sources-driven": ("L11", MILAN + "§5.3.3.1, §5.6.2",
                              "talker_sources_i equals that maximum"),
    "listener-sinks-driven": ("L11", MILAN + "§5.3.3.1, §5.6.2",
                              "listener_sinks_i equals that maximum"),
}

Where = tuple[int, int | None, int | None]


def type_name(code: int) -> str:
    """The Table 7-1 name of a type the rules know, else its hex code."""
    try:
        return D(code).name
    except ValueError:
        return f"0x{code:04X}"


def where_text(where: Where) -> str:
    """`cfg C`, `cfg C TYPE` or `cfg C TYPE I`, the scope a finding names."""
    cfg, dtype, index = where
    if dtype is None:
        return f"cfg {cfg}"
    if index is None:
        return f"cfg {cfg} {type_name(dtype)}"
    return f"cfg {cfg} {type_name(dtype)} {index}"


@dataclass(frozen=True)
class Finding:
    """One check that failed on one scope."""
    check: str
    where: Where
    detail: str

    @property
    def rule(self) -> str:
        """The 07 §3.1 rule the check belongs to."""
        return CHECKS[self.check][0]

    def text(self) -> str:
        """The refusal line: rule, check, scope, detail, clause."""
        return (f"{self.rule} {self.check}: {where_text(self.where)}: "
                f"{self.detail} ({CHECKS[self.check][1]})")


@dataclass
class RuleContext:
    """The model as rules see it, and what they have found so far."""
    model: dict[int, dict[int, dict[int, bytes]]]
    adp: dict[str, int]
    findings: list[Finding] = field(default_factory=list)
    claims: dict[int, dict[tuple[int, int], list[tuple[int, int]]]] = field(default_factory=dict)
    ranges: dict[int, list[tuple[int, int, int, tuple[int, int]]]] = field(default_factory=dict)
    formats: dict[tuple[int, int, int], list[int] | None] = field(default_factory=dict)
    values: dict[str, int | None] = field(default_factory=dict)

    @property
    def cfgs(self) -> list[int]:
        """Configuration indices, ascending."""
        return sorted(self.model)

    def of(self, cfg: int, dtype: int) -> dict[int, bytes]:
        """{index: body} of one type in one configuration."""
        return self.model.get(cfg, {}).get(dtype, {})

    def bad(self, check: str, where: Where, detail: str) -> None:
        """Record a finding."""
        self.findings.append(Finding(check, where, detail))

    def read(self, check: str, where: Where, offset: int, size: int) -> int | None:
        """A big-endian field of the descriptor at `where`, or None and a
        finding of `check` when the descriptor is too short to hold it."""
        cfg, dtype, index = where
        body = self.of(cfg, dtype)[index]
        if offset + size > len(body):
            self.bad(check, where, f"is {len(body)} bytes; the field at {offset} "
                                   f"needs {offset + size}")
            return None
        return int.from_bytes(body[offset:offset + size], "big")

    def kind(self, cfg: int, dtype: int, index: int) -> set[str]:
        """The format families a stream lists ("AAF", "CRF", or other)."""
        return {_family(word) for word in self.formats.get((cfg, dtype, index)) or []}

    def streams(self, cfg: int, dtype: int, family: str) -> list[int]:
        """Indices of the streams of one direction whose list holds `family`."""
        return [i for i in sorted(self.of(cfg, dtype)) if family in self.kind(cfg, dtype, i)]


def _family(word: int) -> str:
    """AVTP subtype of a stream format word (IEEE 1722.1-2021 §7.3.2)."""
    subtype = (word >> 56) & 0x7F
    return {0x02: "AAF", 0x04: "CRF"}.get(subtype, f"subtype 0x{subtype:02X}")


def base_formats(word: int) -> set[tuple[int, int]]:
    """{(nsr, channels)} of the Milan v1.2 §6.2 Base formats a word advertises.

    The `ut` bit (AVTP Annex I.2.4, Milan v1.2 §5.3.3.4) makes one word cover
    every channel count from 1 to its channels_per_frame.
    """
    if word >> 56 != 0x02:
        return set()
    byte1 = (word >> 48) & 0xFF
    up_to, nsr = bool(byte1 & 0x10), byte1 & 0x0F
    encoding, depth = (word >> 40) & 0xFF, (word >> 32) & 0xFF
    channels, samples = (word >> 22) & 0x3FF, (word >> 12) & 0x3FF
    if nsr not in BASE_SPF or encoding != 0x02 or depth != 32 or samples != BASE_SPF[nsr]:
        return set()
    if up_to:
        return {(nsr, n) for n in BASE_CHANNELS if n <= channels}
    return {(nsr, channels)} if channels in BASE_CHANNELS else set()


def _covers(entry: int, current: int) -> bool:
    """Whether a format-list entry covers `current`: equal, or an AAF entry
    with the `ut` bit whose channel count is at least current's (AVTP Annex
    I.2.4, Milan v1.2 §5.3.3.4), all other fields equal."""
    if entry == current:
        return True
    up_to = 0x0010 << 48
    channels = 0x3FF << 22
    if entry >> 56 != 0x02 or not entry & up_to or current & up_to:
        return False
    return ((entry ^ up_to) & ~channels == current & ~channels
            and (current & channels) <= (entry & channels))


def _formats(ctx: RuleContext, where: Where) -> list[int] | None:
    """A stream's format list through its formats_offset, or None (L4 finding)."""
    offset = ctx.read("stream-layout", where, 82, 2)
    count = ctx.read("stream-layout", where, 84, 2)
    if offset is None or count is None:
        return None
    body = ctx.of(where[0], where[1])[where[2]]
    if offset + 8 * count > len(body):
        ctx.bad("stream-layout", where, f"{count} formats at {offset} run past "
                                        f"its {len(body)} bytes")
        return None
    return [int.from_bytes(body[offset + 8 * k:offset + 8 * k + 8], "big") for k in range(count)]


def _claim_range(ctx: RuleContext, parent: Where, child: int, span: tuple[int, int]) -> None:
    """Record that `parent` owns `child` descriptors base..base+count-1."""
    cfg, ptype, pidx = parent
    base, count = span
    if count == 0:
        return
    ctx.ranges.setdefault(cfg, []).append((child, base, count, (ptype, pidx)))
    claims = ctx.claims.setdefault(cfg, defaultdict(list))
    for k in range(base, base + count):
        if k not in ctx.of(cfg, child):
            ctx.bad("child-exists", parent, f"names {type_name(child)} {k}, which does not exist")
        else:
            claims[(child, k)].append((ptype, pidx))


def _claim_fields(ctx: RuleContext, where: Where, spans: tuple[tuple[int, int, int], ...]) -> bool:
    """Claim each (count offset, base offset, child type) range of `where`."""
    for count_at, base_at, child in spans:
        count = ctx.read("child-exists", where, count_at, 2)
        base = ctx.read("child-exists", where, base_at, 2)
        if count is None or base is None:
            return False
        _claim_range(ctx, where, child, (base, count))
    return True


def _survey_unit(ctx: RuleContext, cfg: int, unit: int) -> None:
    """One AUDIO_UNIT's ranges (§7.2.3), then those of the Stream Ports it owns
    (§7.2.13), in the order IEEE 1722.1-2021 §7.2 numbers them."""
    where = (cfg, D.AUDIO_UNIT, unit)
    ports = ((72, 74, D.STREAM_PORT_INPUT), (76, 78, D.STREAM_PORT_OUTPUT))
    if not _claim_fields(ctx, where, ((96, 98, D.CONTROL),) + ports):
        return
    for count_at, base_at, ptype in ports:
        base = ctx.of(cfg, D.AUDIO_UNIT)[unit][base_at:base_at + 2]
        count = ctx.of(cfg, D.AUDIO_UNIT)[unit][count_at:count_at + 2]
        first = int.from_bytes(base, "big")
        for port in range(first, first + int.from_bytes(count, "big")):
            if port in ctx.of(cfg, ptype):
                _claim_fields(ctx, (cfg, ptype, port), ((8, 10, D.CONTROL),
                                                        (12, 14, D.AUDIO_CLUSTER),
                                                        (16, 18, D.AUDIO_MAP)))


def survey(ctx: RuleContext) -> None:
    """Format lists and parent ranges, read once for every rule."""
    for cfg in ctx.cfgs:
        ctx.claims.setdefault(cfg, defaultdict(list))
        for dtype in (D.STREAM_INPUT, D.STREAM_OUTPUT):
            for index in sorted(ctx.of(cfg, dtype)):
                ctx.formats[(cfg, dtype, index)] = _formats(ctx, (cfg, dtype, index))
        for unit in sorted(ctx.of(cfg, D.AUDIO_UNIT)):
            _survey_unit(ctx, cfg, unit)
        for index in sorted(ctx.of(cfg, D.AVB_INTERFACE)):
            _claim_fields(ctx, (cfg, D.AVB_INTERFACE, index), ((98, 100, D.CONTROL),))


def _entity(ctx: RuleContext) -> bytes | None:
    """The ENTITY descriptor, after the L1 checks on where it is."""
    found = [(cfg, i) for cfg in ctx.cfgs for i in ctx.of(cfg, D.ENTITY)]
    for cfg, index in found:
        if cfg != 0:
            ctx.bad("entity-count", (cfg, D.ENTITY, index),
                    "an ENTITY outside configuration 0; the model has exactly one")
    if (0, 0) not in found:
        ctx.bad("entity-count", (0, D.ENTITY, None), "the model has no ENTITY descriptor")
        return None
    return ctx.of(0, D.ENTITY)[0]


def _rule_entity(ctx: RuleContext) -> None:
    """L1 on the ENTITY and the CONFIGURATION descriptors."""
    if _entity(ctx) is None:
        return
    where = (0, D.ENTITY, 0)
    count = ctx.read("configurations-count", where, 308, 2)
    current = ctx.read("current-configuration", where, 310, 2)
    if count is not None and count != len(ctx.cfgs):
        ctx.bad("configurations-count", where,
                f"configurations_count {count}; the image has {len(ctx.cfgs)} configurations")
    if count is not None and current is not None and current >= count:
        ctx.bad("current-configuration", where,
                f"current_configuration {current} is not below configurations_count {count}")
    for cfg in ctx.cfgs[1:]:
        for index in ctx.of(cfg, D.CONFIGURATION):
            ctx.bad("configuration-descriptors", (cfg, D.CONFIGURATION, index),
                    "a CONFIGURATION outside configuration 0")
    have = sorted(ctx.of(0, D.CONFIGURATION))
    if have != list(range(len(ctx.cfgs))):
        ctx.bad("configuration-descriptors", (0, D.CONFIGURATION, None),
                f"CONFIGURATION indices {have} for configurations {ctx.cfgs}")
    for index in have:
        if index in ctx.model:
            _descriptor_counts(ctx, index)


def _descriptor_counts(ctx: RuleContext, cfg: int) -> None:
    """CONFIGURATION `cfg`'s descriptor_counts against what the configuration holds."""
    where = (0, D.CONFIGURATION, cfg)
    count = ctx.read("descriptor-counts", where, 70, 2)
    offset = ctx.read("descriptor-counts", where, 72, 2)
    if count is None or offset is None:
        return
    body = ctx.of(0, D.CONFIGURATION)[cfg]
    if offset + 4 * count > len(body):
        ctx.bad("descriptor-counts", where, f"{count} counts at {offset} run past its "
                                            f"{len(body)} bytes")
        return
    declared: dict[int, int] = {}
    for k in range(count):
        dtype, n = struct.unpack_from(">HH", body, offset + 4 * k)
        if dtype in declared:
            ctx.bad("descriptor-counts", where, f"lists {type_name(dtype)} twice")
        declared[dtype] = n
    claims = ctx.claims.get(cfg, {})
    held = {dtype: len(members) for dtype, members in ctx.model[cfg].items() if dtype in TOP_LEVEL}
    held[D.CONTROL] = len([k for k in ctx.of(cfg, D.CONTROL) if not claims.get((D.CONTROL, k))])
    for dtype in sorted(set(declared) | {t for t, n in held.items() if n}):
        if dtype not in TOP_LEVEL:
            ctx.bad("descriptor-counts", where, f"lists {type_name(dtype)}, which is not a "
                                                "top-level type")
        elif declared.get(dtype, 0) != held.get(dtype, 0):
            ctx.bad("descriptor-counts", where,
                    f"{type_name(dtype)} {declared.get(dtype, 0)}; configuration {cfg} "
                    f"holds {held.get(dtype, 0)} at the top level")


def _rule_tree(ctx: RuleContext) -> None:
    """L1: cardinalities and the one-parent rule of the Milan v1.2 §5.3.2 tree."""
    for cfg in ctx.cfgs:
        for dtype in (D.AVB_INTERFACE, D.CLOCK_DOMAIN, D.CLOCK_SOURCE):
            if not ctx.of(cfg, dtype):
                ctx.bad("required-type", (cfg, dtype, None), f"configuration {cfg} has no "
                                                            f"{dtype.name}")
        aaf_in = ctx.streams(cfg, D.STREAM_INPUT, "AAF")
        aaf_out = ctx.streams(cfg, D.STREAM_OUTPUT, "AAF")
        if (aaf_in or aaf_out) and not ctx.of(cfg, D.AUDIO_UNIT):
            ctx.bad("audio-unit-for-aaf", (cfg, D.AUDIO_UNIT, None),
                    "a stream lists an AAF format and the configuration has no AUDIO_UNIT")
        claims = ctx.claims[cfg]
        for streams, ptype in ((aaf_in, D.STREAM_PORT_INPUT), (aaf_out, D.STREAM_PORT_OUTPUT)):
            owned = [k for k in ctx.of(cfg, ptype) if claims.get((ptype, k))]
            if streams and not owned:
                ctx.bad("stream-port-for-aaf", (cfg, ptype, None),
                        f"AAF streams {streams} and no {ptype.name} in an AUDIO_UNIT")
        for ptype in (D.STREAM_PORT_INPUT, D.STREAM_PORT_OUTPUT):
            for port in sorted(ctx.of(cfg, ptype)):
                clusters = ctx.read("port-cluster-minimum", (cfg, ptype, port), 12, 2)
                if clusters == 0:
                    ctx.bad("port-cluster-minimum", (cfg, ptype, port),
                            "number_of_clusters 0; a Stream Port contains at least one "
                            "AUDIO_CLUSTER")
        for (ctype, k), parents in sorted(claims.items()):
            if len(parents) > 1:
                names = ", ".join(f"{type_name(t)} {i}" for t, i in parents)
                ctx.bad("single-parent", (cfg, ctype, k), f"claimed by {names}")
        for ctype in (D.STREAM_PORT_INPUT, D.STREAM_PORT_OUTPUT, D.AUDIO_CLUSTER, D.AUDIO_MAP):
            for k in sorted(ctx.of(cfg, ctype)):
                if not claims.get((ctype, k)):
                    ctx.bad("has-parent", (cfg, ctype, k), "no descriptor's range claims it")


def _rule_order(ctx: RuleContext) -> None:
    """L2: child ranges ascend in the order IEEE 1722.1-2021 §7.2 walks the
    hierarchy, and configuration-level CONTROLs come before every other."""
    for cfg in ctx.cfgs:
        ends: dict[int, tuple[int, tuple[int, int]]] = {}
        for child, base, count, parent in ctx.ranges.get(cfg, []):
            if parent[0] == D.AVB_INTERFACE:
                continue
            if child in ends and base < ends[child][0]:
                prev = ends[child][1]
                ctx.bad("parent-order", (cfg, parent[0], parent[1]),
                        f"its {type_name(child)} range starts at {base}, before the end "
                        f"{ends[child][0]} of the range of {type_name(prev[0])} {prev[1]}")
            ends[child] = (max(base + count, ends.get(child, (0,))[0]), parent)
        claims = ctx.claims[cfg]
        top = [k for k in ctx.of(cfg, D.CONTROL) if not claims.get((D.CONTROL, k))]
        owned = [k for k in ctx.of(cfg, D.CONTROL)
                 if any(p[0] != D.AVB_INTERFACE for p in claims.get((D.CONTROL, k), []))]
        if top and owned and max(top) > min(owned):
            ctx.bad("parent-order", (cfg, D.CONTROL, max(top)),
                    f"a configuration-level CONTROL after CONTROL {min(owned)}, which a "
                    "unit or a port owns")


def _rule_streams(ctx: RuleContext) -> None:
    """L3: stream presence, Base formats (Milan v1.2 §6.3, §6.4) and the CRF word."""
    talker = listener = False
    base_out = base_in = False
    for cfg in ctx.cfgs:
        if not ctx.of(cfg, D.STREAM_INPUT) and not ctx.of(cfg, D.STREAM_OUTPUT):
            ctx.bad("stream-presence", (cfg, None, None), "no STREAM_INPUT and no STREAM_OUTPUT")
        # A stream carrying only CRF is a media clock, not audio (Milan §7).
        talker |= any(ctx.kind(cfg, D.STREAM_OUTPUT, i) != {"CRF"} for i in ctx.of(cfg, D.STREAM_OUTPUT))
        listener |= any(ctx.kind(cfg, D.STREAM_INPUT, i) != {"CRF"} for i in ctx.of(cfg, D.STREAM_INPUT))
        rates: dict[int, frozenset[int]] = {}
        for dtype in (D.STREAM_INPUT, D.STREAM_OUTPUT):
            for index in sorted(ctx.of(cfg, dtype)):
                words = ctx.formats.get((cfg, dtype, index)) or []
                where = (cfg, dtype, index)
                if "CRF" in ctx.kind(cfg, dtype, index) and CRF_MILAN not in words:
                    ctx.bad("crf-format", where, "lists CRF formats without 0x041060010000BB80")
                cover = set().union(*map(base_formats, words)) if words else set()
                if not cover:
                    continue
                if dtype == D.STREAM_OUTPUT:
                    base_out = True
                    continue
                base_in = True
                rates[index] = frozenset(nsr for nsr, _ in cover)
                for nsr in sorted(rates[index]):
                    missing = [n for n in BASE_CHANNELS if (nsr, n) not in cover]
                    if missing:
                        ctx.bad("base-channel-completeness", where,
                                f"Base {BASE_RATE[nsr]} without channel counts {missing}")
        if len(set(rates.values())) > 1:
            listed = "; ".join(f"STREAM_INPUT {i}: "
                               + ", ".join(BASE_RATE[n] for n in sorted(r)) for i, r in rates.items())
            ctx.bad("base-rate-uniformity", (cfg, D.STREAM_INPUT, None),
                    f"Base-format inputs with different Base rates ({listed})")
    if talker and not base_out:
        ctx.bad("talker-base-format", (0, D.STREAM_OUTPUT, None),
                "the entity is a Talker and no Stream Output lists a Base format")
    if listener and not base_in:
        ctx.bad("listener-base-format", (0, D.STREAM_INPUT, None),
                "the entity is a Listener and no Stream Input lists a Base format")


def _rule_stream_fields(ctx: RuleContext) -> None:
    """L4: every STREAM_INPUT/OUTPUT field Milan v1.2 §5.3.3.4 constrains."""
    for cfg in ctx.cfgs:
        for dtype in (D.STREAM_INPUT, D.STREAM_OUTPUT):
            for index in sorted(ctx.of(cfg, dtype)):
                where = (cfg, dtype, index)
                flags = ctx.read("class-a", where, 72, 2)
                if flags is not None and not flags & CLASS_A:
                    ctx.bad("class-a", where, f"stream_flags 0x{flags:04X} without CLASS_A")
                if dtype == D.STREAM_INPUT:
                    length = ctx.read("buffer-length", where, 128, 4)
                    if length is not None and length < BUFFER_MIN_NS:
                        ctx.bad("buffer-length", where, f"buffer_length {length} ns, "
                                                        f"below {BUFFER_MIN_NS}")
                words = ctx.formats.get(where)
                if words is None:
                    continue
                _stream_layout(ctx, where, len(words))
                if len(words) > FORMATS_MAX:
                    ctx.bad("format-count", where, f"number_of_formats {len(words)}, "
                                                   f"above {FORMATS_MAX}")
                if {"AAF", "CRF"} <= ctx.kind(cfg, dtype, index):
                    ctx.bad("format-family", where, "lists both CRF and AAF formats")
                current = ctx.read("current-format", where, 74, 8)
                if current is not None and not any(_covers(w, current) for w in words):
                    ctx.bad("current-format", where,
                            f"current_format 0x{current:016X} is not in its list")


def _stream_layout(ctx: RuleContext, where: Where, count: int) -> None:
    """The Table 7-8 layout with R = 0: formats at 138, then nothing."""
    offset = ctx.read("stream-layout", where, 82, 2)
    redundant = ctx.read("stream-layout", where, 132, 2)
    tail = ctx.read("stream-layout", where, 134, 2)
    body = ctx.of(where[0], where[1])[where[2]]
    end = FORMATS_OFFSET + 8 * count
    if offset is not None and offset != FORMATS_OFFSET:
        ctx.bad("stream-layout", where, f"formats_offset {offset}, not {FORMATS_OFFSET}")
    if redundant is not None and redundant != end:
        ctx.bad("stream-layout", where, f"redundant_offset {redundant}, not {end}")
    if tail:
        ctx.bad("stream-layout", where, f"number_of_redundant_streams {tail} on a "
                                        "non-redundant PAAD")
    if len(body) != end:
        ctx.bad("stream-layout", where, f"is {len(body)} bytes; {count} formats make {end}")


def _rule_interfaces(ctx: RuleContext) -> None:
    """L5: the same AVB_INTERFACE index for each physical port everywhere."""
    shapes = {}
    for cfg in ctx.cfgs:
        ports = {}
        for index in sorted(ctx.of(cfg, D.AVB_INTERFACE)):
            ports[index] = ctx.read("interface-index", (cfg, D.AVB_INTERFACE, index), 96, 2)
        shapes[cfg] = ports
    first = shapes[ctx.cfgs[0]]
    for cfg in ctx.cfgs[1:]:
        if shapes[cfg] != first:
            ctx.bad("interface-index", (cfg, D.AVB_INTERFACE, None),
                    f"index-to-port_number {shapes[cfg]}; configuration {ctx.cfgs[0]} has {first}")


def _rule_domains(ctx: RuleContext) -> None:
    """L6, first half: each CLOCK_DOMAIN's clock_sources list."""
    for cfg in ctx.cfgs:
        for index in sorted(ctx.of(cfg, D.CLOCK_DOMAIN)):
            where = (cfg, D.CLOCK_DOMAIN, index)
            body = ctx.of(cfg, D.CLOCK_DOMAIN)[index]
            offset = ctx.read("domain-source-offset", where, 72, 2)
            count = ctx.read("domain-source-count", where, 74, 2)
            if offset is None or count is None:
                continue
            if offset != SOURCES_OFFSET:
                ctx.bad("domain-source-offset", where,
                        f"clock_sources_offset {offset}; SET_CLOCK_SOURCE reads the list at 76")
            if count == 0:
                ctx.bad("domain-source-count", where, "clock_sources_count 0")
            if len(body) != SOURCES_OFFSET + 2 * count:
                ctx.bad("domain-source-length", where,
                        f"is {len(body)} bytes; {count} sources make {SOURCES_OFFSET + 2 * count}")
            if offset + 2 * count > len(body):
                continue
            listed = list(struct.unpack_from(f">{count}H", body, offset))
            if listed != list(range(count)):
                ctx.bad("domain-source-identity", where,
                        f"clock_sources {listed}, not 0..{count - 1}; the SET_CLOCK_SOURCE "
                        "range check is the membership test only for the identity list")
            for source in listed:
                if source not in ctx.of(cfg, D.CLOCK_SOURCE):
                    ctx.bad("domain-source-exists", where,
                            f"names CLOCK_SOURCE {source}, which does not exist")


def _rule_sources(ctx: RuleContext) -> None:
    """L6, second half: the CLOCK_SOURCE set Milan v1.2 §5.3.3.6 constructs."""
    for cfg in ctx.cfgs:
        kinds = {}
        for index in sorted(ctx.of(cfg, D.CLOCK_SOURCE)):
            where = (cfg, D.CLOCK_SOURCE, index)
            kinds[index] = tuple(ctx.read("crf-input-source", where, off, 2) for off in (72, 82, 84))
        at_input: dict[int, int] = defaultdict(int)
        for source_type, location_type, location in kinds.values():
            if source_type == INPUT_STREAM and location_type == D.STREAM_INPUT:
                at_input[location] += 1
        crf_in = ctx.streams(cfg, D.STREAM_INPUT, "CRF")
        aaf_in = ctx.streams(cfg, D.STREAM_INPUT, "AAF")
        for stream in crf_in:
            if at_input[stream] != 1:
                ctx.bad("crf-input-source", (cfg, D.STREAM_INPUT, stream),
                        f"a CRF input with {at_input[stream]} INPUT_STREAM sources")
        if aaf_in and not crf_in:
            total = sum(at_input[k] for k in aaf_in)
            if total != 1:
                ctx.bad("aaf-input-source", (cfg, D.CLOCK_SOURCE, None),
                        f"no CRF input, and {total} INPUT_STREAM sources at AAF inputs {aaf_in}")
        if ctx.of(cfg, D.STREAM_OUTPUT) and not any(k[0] == INTERNAL for k in kinds.values()):
            ctx.bad("internal-source", (cfg, D.CLOCK_SOURCE, None),
                    "Stream Outputs and no INTERNAL CLOCK_SOURCE")
        gptp = [i for i, k in kinds.items() if k[0] == EXTERNAL and k[1] == D.TIMING]
        if gptp and len(ctx.of(cfg, D.AVB_INTERFACE)) != 1:
            ctx.bad("gptp-source-interfaces", (cfg, D.CLOCK_SOURCE, gptp[0]),
                    f"a gPTP media clock source with {len(ctx.of(cfg, D.AVB_INTERFACE))} "
                    "AVB_INTERFACEs")


def _rule_maps(ctx: RuleContext) -> None:
    """L7: input ports own no maps, static mappings are unique, clusters are mono."""
    for cfg in ctx.cfgs:
        for port in sorted(ctx.of(cfg, D.STREAM_PORT_INPUT)):
            maps = ctx.read("input-port-maps", (cfg, D.STREAM_PORT_INPUT, port), 16, 2)
            if maps:
                ctx.bad("input-port-maps", (cfg, D.STREAM_PORT_INPUT, port),
                        f"number_of_maps {maps}; input mappings are dynamic")
        for index in sorted(ctx.of(cfg, D.AUDIO_CLUSTER)):
            channels = ctx.read("cluster-channels", (cfg, D.AUDIO_CLUSTER, index), 84, 2)
            if channels is not None and channels != 1:
                ctx.bad("cluster-channels", (cfg, D.AUDIO_CLUSTER, index),
                        f"channel_count {channels}")
        seen: dict[tuple[int, int], int] = {}
        for index in sorted(ctx.of(cfg, D.AUDIO_MAP)):
            where = (cfg, D.AUDIO_MAP, index)
            if not any(p[0] == D.STREAM_PORT_OUTPUT
                       for p in ctx.claims[cfg].get((D.AUDIO_MAP, index), [])):
                continue        # input maps are input-port-maps' finding
            offset = ctx.read("unique-mapping", where, 4, 2)
            count = ctx.read("unique-mapping", where, 6, 2)
            body = ctx.of(cfg, D.AUDIO_MAP)[index]
            if offset is None or count is None or offset + 8 * count > len(body):
                ctx.bad("unique-mapping", where, "its mappings run past the descriptor")
                continue
            for k in range(count):
                key = struct.unpack_from(">HH", body, offset + 8 * k)
                if key in seen:
                    ctx.bad("unique-mapping", where,
                            f"stream {key[0]} channel {key[1]} is also mapped by AUDIO_MAP "
                            f"{seen[key]}")
                seen.setdefault(key, index)


def _rule_identify(ctx: RuleContext) -> None:
    """L8: the primary IDENTIFY CONTROL, the index identify_index_i names."""
    held = []
    for cfg in ctx.cfgs:
        at = set()
        for index in sorted(ctx.of(cfg, D.CONTROL)):
            if ctx.read("identify-index", (cfg, D.CONTROL, index), 82, 8) == IDENTIFY:
                at.add(index)
        held.append(at)
    common = sorted(set.intersection(*held)) if held else []
    ctx.values["identify_index_i"] = common[0] if common else None
    if not common:
        ctx.bad("identify-index", (0, D.CONTROL, None),
                "no index holds an IDENTIFY CONTROL in every configuration, and the ADPDU "
                "advertises identify_control_index as valid")
    driven = ctx.adp.get("identify_index")
    if driven is not None and driven not in common:
        ctx.bad("identify-driven", (0, D.CONTROL, driven),
                f"identify_index_i {driven} is not an IDENTIFY CONTROL in every configuration")


def rule_identity(ctx: RuleContext, model_ids: dict[str, str] | None, digest: str) -> None:
    """L9: entity_model_id valid, equal to the driven input, and recorded."""
    if 0 not in ctx.of(0, D.ENTITY):
        return
    where = (0, D.ENTITY, 0)
    model_id = ctx.read("model-id-valid", where, 12, 8)
    ctx.values["entity_model_id_i"] = model_id
    if model_id is None:
        return
    if model_id in (0, (1 << 64) - 1):
        ctx.bad("model-id-valid", where, f"entity_model_id 0x{model_id:016X} is not a valid EUI-64")
    driven = ctx.adp.get("entity_model_id")
    if driven is not None and driven != model_id:
        ctx.bad("model-id-driven", where,
                f"entity_model_id_i 0x{driven:016X}; the ENTITY carries 0x{model_id:016X}")
    if model_ids is None:
        return
    recorded = {int(str(k), 16): str(v).lower() for k, v in model_ids.items()}
    if model_id not in recorded:
        ctx.bad("model-id-recorded", where,
                f"entity_model_id 0x{model_id:016X} has no recorded digest; record {digest}")
    elif recorded[model_id] != digest:
        ctx.bad("model-digest", where,
                f"digest {digest} differs from the {recorded[model_id]} recorded for "
                f"0x{model_id:016X}: the structure changed and the id did not")


def _rule_rates(ctx: RuleContext) -> None:
    """L10: the sampling_rates list SET_SAMPLING_RATE walks (07 §3.1)."""
    for cfg in ctx.cfgs:
        for index in sorted(ctx.of(cfg, D.AUDIO_UNIT)):
            where = (cfg, D.AUDIO_UNIT, index)
            body = ctx.of(cfg, D.AUDIO_UNIT)[index]
            offset = ctx.read("rate-offset", where, 140, 2)
            count = ctx.read("rate-count", where, 142, 2)
            current = ctx.read("current-rate", where, 136, 4)
            if offset is not None and offset != RATES_OFFSET:
                ctx.bad("rate-offset", where,
                        f"sampling_rates_offset {offset}; SET_SAMPLING_RATE reads the list at 144")
            if count is None:
                continue
            if count > RATES_MAX:
                ctx.bad("rate-count", where, f"sampling_rates_count {count}; SET_SAMPLING_RATE "
                                             f"consults the first {RATES_MAX}")
            if count == 0:
                ctx.bad("rate-empty", where, "sampling_rates_count 0")
            if len(body) != RATES_OFFSET + 4 * count:
                ctx.bad("rate-length", where,
                        f"is {len(body)} bytes; {count} rates make {RATES_OFFSET + 4 * count}")
            if offset is None or current is None or offset + 4 * count > len(body) or not count:
                continue
            rates = list(struct.unpack_from(f">{count}I", body, offset))
            if current not in rates:
                ctx.bad("current-rate", where,
                        f"current_sampling_rate 0x{current:08X} is not one of "
                        + ", ".join(f"0x{r:08X}" for r in rates))


def _rule_counts(ctx: RuleContext) -> None:
    """L11: ENTITY stream counts are the maxima over every configuration."""
    most_out = max(len(ctx.of(cfg, D.STREAM_OUTPUT)) for cfg in ctx.cfgs)
    most_in = max(len(ctx.of(cfg, D.STREAM_INPUT)) for cfg in ctx.cfgs)
    ctx.values["talker_sources_i"] = most_out
    ctx.values["listener_sinks_i"] = most_in
    rows = (("talker-sources", "talker_stream_sources", 24, most_out, "talker_sources", "OUTPUT"),
            ("listener-sinks", "listener_stream_sinks", 28, most_in, "listener_sinks", "INPUT"))
    entity = 0 in ctx.of(0, D.ENTITY)
    for check, name, offset, most, port, direction in rows:
        if entity:
            value = ctx.read(check, (0, D.ENTITY, 0), offset, 2)
            if value is not None and value != most:
                ctx.bad(check, (0, D.ENTITY, 0),
                        f"{name} {value}; the most STREAM_{direction}s of any configuration "
                        f"is {most}")
        driven = ctx.adp.get(port)
        if driven is not None and driven != most:
            ctx.bad(check + "-driven", (0, D.ENTITY, 0 if entity else None),
                    f"{port}_i {driven}; the most STREAM_{direction}s of any configuration "
                    f"is {most}")


RULES = (_rule_entity, _rule_tree, _rule_order, _rule_streams, _rule_stream_fields,
         _rule_interfaces, _rule_domains, _rule_sources, _rule_maps, _rule_identify,
         _rule_rates, _rule_counts)
