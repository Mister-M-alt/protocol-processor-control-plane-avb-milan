# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The semantic lint's negative cases: one named mutation of milan_min.json each.

Every mutation edits a copy of the minimal Milan model that
test_gen_desc_image.py has normalised to literal `bytes` bodies, with only the
ENTITY named, so an edit is a write at an IEEE 1722.1-2021 §7.2 wire offset
and no name binding stands in its way. The gate requires the named check in
the refusal, and requires the same mutated model to pack with the lint off,
so every refusal it counts is the lint's and not a layout refusal.

Nothing here reads a file: the gate hands each mutation the model.
"""
import struct
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

Model = dict[str, Any]

ENTITY, CONFIGURATION, AUDIO_UNIT = 0x0000, 0x0001, 0x0002
STREAM_INPUT, STREAM_OUTPUT, AVB_INTERFACE = 0x0005, 0x0006, 0x0009
CLOCK_SOURCE, STREAM_PORT_INPUT, STREAM_PORT_OUTPUT = 0x000A, 0x000E, 0x000F
AUDIO_CLUSTER, AUDIO_MAP, CONTROL, CLOCK_DOMAIN = 0x0014, 0x0017, 0x001A, 0x0024
TIMING, EXTERNAL, JACK_INPUT = 0x0026, 0x0001, 0x0007
MUTE = 0x90E0F00000000002        # a CONTROL type that is not IDENTIFY (IEEE Table 7-98)
BASE_IN = 0x0215022002006000     # 48 kHz, up to 8 channels (Milan v1.2 Table 6.2)
CRF = 0x041060010000BB80         # Milan v1.2 §7.3.4 Table 7.1


def find(model: Model, dtype: int, index: int, cfg: int = 0) -> dict[str, Any]:
    """The descriptor row at (cfg, type, index)."""
    for row in model["descriptors"]:
        if (row.get("configuration", 0), row["type"], row["index"]) == (cfg, dtype, index):
            return row
    raise KeyError(f"no descriptor cfg {cfg} type 0x{dtype:04X} index {index}")


def body(model: Model, dtype: int, index: int, cfg: int = 0) -> bytearray:
    """A copy of one descriptor's bytes."""
    return bytearray.fromhex(find(model, dtype, index, cfg)["bytes"])


def store(model: Model, at: tuple[int, int, int], data: bytes) -> None:
    """Replace the bytes of the descriptor at (type, index, cfg)."""
    find(model, at[0], at[1], at[2])["bytes"] = bytes(data).hex()


def put(model: Model, at: tuple[int, int, int], offset: int, value: int,
        size: int = 2) -> None:
    """Write one big-endian field of the descriptor at (type, index, cfg)."""
    data = body(model, *at)
    data[offset:offset + size] = value.to_bytes(size, "big")
    store(model, at, data)


def add(model: Model, at: tuple[int, int, int], data: bytes, top: bool = True) -> None:
    """Add a descriptor at (type, index, cfg), fixing its key bytes, and count
    it in its CONFIGURATION when its type is listed there and `top` (no other
    descriptor owns it)."""
    dtype, index, cfg = at
    data = bytearray(data)
    struct.pack_into(">HH", data, 0, dtype, index)
    model["descriptors"].append({"configuration": cfg, "type": dtype, "index": index,
                                 "bytes": bytes(data).hex()})
    if not top:
        return
    counts = body(model, CONFIGURATION, cfg)
    for k in range(struct.unpack_from(">H", counts, 70)[0]):
        listed, count = struct.unpack_from(">HH", counts, 74 + 4 * k)
        if listed == dtype:
            struct.pack_into(">H", counts, 76 + 4 * k, count + 1)
            store(model, (CONFIGURATION, cfg, 0), counts)


def set_count(model: Model, dtype: int, count: int, cfg: int = 0) -> None:
    """Set the count configuration `cfg`'s descriptor_counts gives `dtype`."""
    data = body(model, CONFIGURATION, cfg)
    for k in range(struct.unpack_from(">H", data, 70)[0]):
        if struct.unpack_from(">H", data, 74 + 4 * k)[0] == dtype:
            struct.pack_into(">H", data, 76 + 4 * k, count)
    store(model, (CONFIGURATION, cfg, 0), data)


def list_count(model: Model, dtype: int, count: int, cfg: int = 0) -> None:
    """Append a (type, count) pair to configuration `cfg`'s descriptor_counts."""
    data = body(model, CONFIGURATION, cfg) + struct.pack(">HH", dtype, count)
    struct.pack_into(">H", data, 70, struct.unpack_from(">H", data, 70)[0] + 1)
    store(model, (CONFIGURATION, cfg, 0), data)


def control(model: Model) -> bytes:
    """A CONTROL body like CONTROL 0 that is a MUTE, not an IDENTIFY."""
    data = body(model, CONTROL, 0)
    struct.pack_into(">Q", data, 82, MUTE)
    return bytes(data)


def selector_control(options: list[int], current: int = 0, index: int = 1) -> bytes:
    """CONTROL `index` as a CONTROL_SELECTOR_UINT8 (IEEE 1722.1-2021 Table
    7-123): current, default, the options, then the unit."""
    data = bytearray(104)
    struct.pack_into(">HH", data, 0, CONTROL, index)
    struct.pack_into(">HQ", data, 80, 0x000B, MUTE)
    struct.pack_into(">HHH", data, 94, 104, len(options), 0xFFFF)
    return bytes(data) + bytes([current, options[0], *options]) + bytes(2)


def own_controls(model: Model, owner: tuple[int, int], count_at: int, base: int) -> None:
    """Give a configuration-0 descriptor one CONTROL, `base`, through its
    number_of_controls / base_control pair at `count_at`."""
    put(model, (owner[0], owner[1], 0), count_at, 1)
    put(model, (owner[0], owner[1], 0), count_at + 2, base)


def drop(model: Model, dtype: int, index: int, cfg: int = 0) -> None:
    """Remove one descriptor."""
    model["descriptors"].remove(find(model, dtype, index, cfg))


def set_formats(model: Model, at: tuple[int, int, int], words: list[int],
                current: int | None = None) -> None:
    """Rewrite a stream's format list in the Table 7-8 layout, R = 0."""
    data = body(model, *at)[:138]
    struct.pack_into(">HH", data, 82, 138, len(words))
    struct.pack_into(">H", data, 132, 138 + 8 * len(words))
    if current is not None:
        struct.pack_into(">Q", data, 74, current)
    store(model, at, bytes(data) + b"".join(w.to_bytes(8, "big") for w in words))


def annex_c(model: Model, at: tuple[int, int, int], streams: tuple[int, ...] = ()) -> None:
    """Re-lay a Table 7-8 stream in Milan v1.2 Annex C Table C.1: no `timing`,
    the formats at 136, then `streams` as its two-octet redundant_streams."""
    data = body(model, *at)
    count = struct.unpack_from(">H", data, 84)[0]
    head = data[:136]
    struct.pack_into(">H", head, 82, 136)
    struct.pack_into(">HH", head, 132, 136 + 8 * count, len(streams))
    store(model, at, bytes(head) + data[138:138 + 8 * count]
          + struct.pack(f">{len(streams)}H", *streams))


def redundant_outputs(model: Model) -> None:
    """STREAM_OUTPUT 1 on AVB_INTERFACE 1 (port 2) beside STREAM_OUTPUT 0, both
    in Annex C and each naming the other: a redundant pair (Milan v1.2 Annex C)."""
    second_interface(model)
    output = body(model, STREAM_OUTPUT, 0)
    struct.pack_into(">H", output, 126, 1)
    add(model, (STREAM_OUTPUT, 1, 0), output)
    put(model, (ENTITY, 0, 0), 24, 2)
    annex_c(model, (STREAM_OUTPUT, 0, 0), (1,))
    annex_c(model, (STREAM_OUTPUT, 1, 0), (0,))


def set_rates(model: Model, rates: list[int], current: int = 48000) -> None:
    """Rewrite AUDIO_UNIT 0's sampling-rate list at 144."""
    data = body(model, AUDIO_UNIT, 0)[:144]
    struct.pack_into(">IHH", data, 136, current, 144, len(rates))
    store(model, (AUDIO_UNIT, 0, 0), bytes(data) + struct.pack(f">{len(rates)}I", *rates))


def set_sources(model: Model, sources: list[int], extra: bytes = b"") -> None:
    """Rewrite CLOCK_DOMAIN 0's clock_sources list at 76."""
    data = body(model, CLOCK_DOMAIN, 0)[:76]
    struct.pack_into(">HH", data, 72, 76, len(sources))
    store(model, (CLOCK_DOMAIN, 0, 0),
          bytes(data) + struct.pack(f">{len(sources)}H", *sources) + extra)


def second_configuration(model: Model) -> None:
    """Configuration 1 as a copy of configuration 0: a positive control."""
    for row in [r for r in model["descriptors"] if r["type"] not in (ENTITY, CONFIGURATION)]:
        model["descriptors"].append(dict(row, configuration=1))
    add_config = body(model, CONFIGURATION, 0)
    struct.pack_into(">H", add_config, 2, 1)
    model["descriptors"].append({"configuration": 0, "type": CONFIGURATION, "index": 1,
                                 "bytes": bytes(add_config).hex()})
    put(model, (ENTITY, 0, 0), 308, 2)


def input_port_without_clusters(model: Model, cfg: int = 0) -> None:
    """Stream Port Input 0 of configuration `cfg` with no AUDIO_CLUSTER; its
    two clusters leave the tree."""
    put(model, (STREAM_PORT_INPUT, 0, cfg), 12, 0)
    for index in (0, 1):
        drop(model, AUDIO_CLUSTER, index, cfg)
    for old, new in ((2, 0), (3, 1)):
        row = find(model, AUDIO_CLUSTER, old, cfg)
        row["index"] = new
        put(model, (AUDIO_CLUSTER, new, cfg), 2, new)
    put(model, (STREAM_PORT_OUTPUT, 0, cfg), 14, 0)


def output_port_without_clusters(model: Model) -> None:
    """Stream Port Output 0 with no AUDIO_CLUSTER; its two clusters leave the tree."""
    put(model, (STREAM_PORT_OUTPUT, 0, 0), 12, 0)
    for index in (2, 3):
        drop(model, AUDIO_CLUSTER, index)


def _crf_input_becomes_aaf(model: Model) -> None:
    """STREAM_INPUT 1 advertises the Base format instead of CRF."""
    set_formats(model, (STREAM_INPUT, 1, 0), [BASE_IN], 0x0205022002006000)


def _no_crf_and_no_aaf_source(model: Model) -> None:
    """No CRF input, and the INPUT_STREAM source becomes EXTERNAL."""
    _crf_input_becomes_aaf(model)
    put(model, (CLOCK_SOURCE, 1, 0), 72, EXTERNAL)


def second_interface(model: Model, cfg: int = 0) -> None:
    """AVB_INTERFACE 1, physical port 2, beside AVB_INTERFACE 0 (port 1)."""
    interface = body(model, AVB_INTERFACE, 0, cfg)
    struct.pack_into(">H", interface, 96, 2)
    add(model, (AVB_INTERFACE, 1, cfg), interface)


def _port_moves_in_configuration_1(model: Model) -> None:
    """Configuration 1 holds both ports, with port 1 at index 1 and port 2 at
    index 0: port 1 moves from the index configuration 0 gives it."""
    second_configuration(model)
    second_interface(model, 1)
    put(model, (AVB_INTERFACE, 0, 1), 96, 2)
    put(model, (AVB_INTERFACE, 1, 1), 96, 1)


def _gptp_with_two_interfaces(model: Model) -> None:
    """A gPTP media clock source (Milan v1.2 §7.5.2) beside a second interface."""
    source = body(model, CLOCK_SOURCE, 0)
    struct.pack_into(">HH", source, 70, 0, EXTERNAL)
    struct.pack_into(">HH", source, 82, TIMING, 0)
    add(model, (CLOCK_SOURCE, 2, 0), source)
    second_interface(model)


def _identify_moves(model: Model) -> None:
    """Two configurations whose IDENTIFY CONTROLs sit at different indices."""
    second_configuration(model)
    add(model, (CONTROL, 1, 1), body(model, CONTROL, 0, 1))
    put(model, (CONTROL, 0, 1), 82, 0x90E0F00000000002, 8)


def _second_talker_in_configuration_1(model: Model) -> None:
    """Configuration 1 with two Stream Outputs, ENTITY still saying one."""
    second_configuration(model)
    add(model, (STREAM_OUTPUT, 1, 1), body(model, STREAM_OUTPUT, 0, 1))


@dataclass(frozen=True)
class Mutation:
    """One negative case: the edit, the check it must trip, the text that
    names the arm of that check (a check with several arms has one mutation
    per arm), and the checks the build runs with (driven ADP values; the
    recorded digests)."""
    name: str
    check: str
    edit: Callable[[Model], None]
    adp: dict[str, int] = field(default_factory=dict)
    recorded: bool = False
    detail: str = ""


def _p(at: tuple[int, int], offset: int, value: int, size: int = 2) -> Callable[[Model], None]:
    """An edit writing one field of a configuration-0 descriptor."""
    return lambda model: put(model, (at[0], at[1], 0), offset, value, size)


def _port_control_before_unit_control(model: Model) -> None:
    """AUDIO_UNIT 0 owns CONTROL 2 and its Stream Port Input 0 owns CONTROL 1:
    IEEE 1722.1-2021 §7.2 numbers the Unit's CONTROLs before its Ports'."""
    for index in (1, 2):
        add(model, (CONTROL, index, 0), control(model), top=False)
    own_controls(model, (AUDIO_UNIT, 0), 96, 2)
    own_controls(model, (STREAM_PORT_INPUT, 0), 8, 1)


def _configuration_control_after_unit_control(model: Model) -> None:
    """AUDIO_UNIT 0 owns CONTROL 1, and CONTROL 2 is the configuration's own:
    §7.2 numbers the configuration's CONTROLs first."""
    add(model, (CONTROL, 1, 0), control(model), top=False)
    own_controls(model, (AUDIO_UNIT, 0), 96, 1)
    add(model, (CONTROL, 2, 0), control(model))


def _entity_in_configuration_1(model: Model) -> None:
    """A second ENTITY, in configuration 1 (Milan v1.2 §5.3.3.1: exactly one)."""
    second_configuration(model)
    model["descriptors"].append(dict(find(model, ENTITY, 0), configuration=1))


def _configuration_in_configuration_1(model: Model) -> None:
    """CONFIGURATION 0 copied into configuration 1, beside configuration 0's two."""
    second_configuration(model)
    add(model, (CONFIGURATION, 0, 1), body(model, CONFIGURATION, 0))


def _counts_list(model: Model, extra: bytes, count: int) -> None:
    """CONFIGURATION 0's descriptor_counts with `extra` pairs appended and a
    descriptor_counts_count raised by `count`."""
    data = body(model, CONFIGURATION, 0) + extra
    struct.pack_into(">H", data, 70, struct.unpack_from(">H", data, 70)[0] + count)
    store(model, (CONFIGURATION, 0, 0), data)


def _two_sources_at_crf_input(model: Model) -> None:
    """A second INPUT_STREAM CLOCK_SOURCE at the CRF input (STREAM_INPUT 1)."""
    add(model, (CLOCK_SOURCE, 2, 0), body(model, CLOCK_SOURCE, 1))
    set_sources(model, [0, 1, 2])


def _a_source_at_each_aaf_input(model: Model) -> None:
    """No CRF input, and an INPUT_STREAM source at each of the two AAF inputs."""
    _crf_input_becomes_aaf(model)
    source = body(model, CLOCK_SOURCE, 1)
    struct.pack_into(">H", source, 84, 0)
    add(model, (CLOCK_SOURCE, 2, 0), source)
    set_sources(model, [0, 1, 2])


def _mapping_repeated_in_a_second_map(model: Model) -> None:
    """AUDIO_MAP 1 on the output port repeats AUDIO_MAP 0's mappings
    (Milan v1.2 §5.3.3.9: unique across all AUDIO_MAPs)."""
    add(model, (AUDIO_MAP, 1, 0), body(model, AUDIO_MAP, 0))
    put(model, (STREAM_PORT_OUTPUT, 0, 0), 16, 2)


def offset_moved(model: Model, at: tuple[int, int], offset_at: int, gap: int) -> None:
    """A configuration-0 descriptor's variable part moved `gap` octets on, its
    offset field following it: the list still reads, the offset is not §7.2's."""
    data = body(model, at[0], at[1])
    base = struct.unpack_from(">H", data, offset_at)[0]
    struct.pack_into(">H", data, offset_at, base + gap)
    store(model, (at[0], at[1], 0), data[:base] + bytes(gap) + data[base:])


def _long_audio_map(model: Model) -> None:
    """AUDIO_MAP 0 with 64 distinct mappings: 520 octets (R435-1 probe I)."""
    data = body(model, AUDIO_MAP, 0)[:8]
    struct.pack_into(">H", data, 6, 64)
    store(model, (AUDIO_MAP, 0, 0),
          data + b"".join(struct.pack(">HHHH", 0, k, 0, 0) for k in range(64)))


def _no_streams(model: Model) -> None:
    """Every STREAM_INPUT and STREAM_OUTPUT removed."""
    for dtype, index in ((STREAM_INPUT, 0), (STREAM_INPUT, 1), (STREAM_OUTPUT, 0)):
        drop(model, dtype, index)


MUTATIONS = (
    Mutation("no ENTITY", "entity-count", lambda m: drop(m, ENTITY, 0),
             detail="no ENTITY descriptor"),
    Mutation("an ENTITY in configuration 1", "entity-count", _entity_in_configuration_1,
             detail="cfg 1 ENTITY 0: an ENTITY outside configuration 0"),
    Mutation("configurations_count 2", "configurations-count", _p((ENTITY, 0), 308, 2)),
    Mutation("current_configuration 1", "current-configuration", _p((ENTITY, 0), 310, 1)),
    Mutation("no CONFIGURATION", "configuration-descriptors",
             lambda m: drop(m, CONFIGURATION, 0), detail="CONFIGURATION indices []"),
    Mutation("a CONFIGURATION in configuration 1", "configuration-descriptors",
             _configuration_in_configuration_1,
             detail="cfg 1 CONFIGURATION 0: a CONFIGURATION outside configuration 0"),
    Mutation("descriptor_counts says 3 STREAM_INPUTs", "descriptor-counts",
             _p((CONFIGURATION, 0), 80, 3), detail="STREAM_INPUT 3; configuration 0 holds 2"),
    Mutation("descriptor_counts lists CONTROL twice", "descriptor-counts",
             lambda m: _counts_list(m, struct.pack(">HH", CONTROL, 0), 1),
             detail="lists CONTROL twice"),
    Mutation("descriptor_counts lists STREAM_PORT_INPUT", "descriptor-counts",
             lambda m: _counts_list(m, struct.pack(">HH", STREAM_PORT_INPUT, 1), 1),
             detail="lists STREAM_PORT_INPUT, which is not a top-level type"),
    Mutation("descriptor_counts_count one past the list", "descriptor-counts",
             lambda m: _counts_list(m, b"", 1), detail="8 counts at 74 run past"),
    Mutation("no AVB_INTERFACE", "required-type", lambda m: drop(m, AVB_INTERFACE, 0)),
    Mutation("no AUDIO_UNIT", "audio-unit-for-aaf", lambda m: drop(m, AUDIO_UNIT, 0)),
    Mutation("AUDIO_UNIT owns no output port", "stream-port-for-aaf",
             _p((AUDIO_UNIT, 0), 76, 0)),
    Mutation("input port with no cluster", "port-cluster-minimum",
             input_port_without_clusters),
    Mutation("output port names AUDIO_CLUSTER 4", "child-exists",
             _p((STREAM_PORT_OUTPUT, 0), 12, 3)),
    Mutation("two ports share AUDIO_CLUSTER 2", "single-parent",
             _p((STREAM_PORT_INPUT, 0), 12, 3)),
    Mutation("AUDIO_MAP 0 has no port", "has-parent", _p((STREAM_PORT_OUTPUT, 0), 16, 0)),
    Mutation("the input port's CONTROL numbered before its unit's", "parent-order",
             _port_control_before_unit_control,
             detail="STREAM_PORT_INPUT 0: its CONTROL range starts at 1, before the end 3"),
    Mutation("a configuration-level CONTROL after a unit-owned one", "parent-order",
             _configuration_control_after_unit_control,
             detail="CONTROL 2: a configuration-level CONTROL after CONTROL 1"),
    Mutation("no stream at all", "stream-presence", _no_streams),
    Mutation("output format 24-bit", "talker-base-format",
             lambda m: set_formats(m, (STREAM_OUTPUT, 0, 0), [0x0205021800806000],
                                   0x0205021800806000)),
    Mutation("input format 24-bit", "listener-base-format",
             lambda m: set_formats(m, (STREAM_INPUT, 0, 0), [0x0215021802006000],
                                   0x0215021802006000)),
    Mutation("input Base format up to 6 channels", "base-channel-completeness",
             lambda m: set_formats(m, (STREAM_INPUT, 0, 0), [0x0215022001806000],
                                   0x0205022001806000)),
    Mutation("second Base input at 96 kHz", "base-rate-uniformity",
             lambda m: set_formats(m, (STREAM_INPUT, 1, 0), [0x021702200200C000],
                                   0x020702200200C000)),
    Mutation("CRF input at 44.1 kHz only", "crf-format",
             lambda m: set_formats(m, (STREAM_INPUT, 1, 0), [0x041060010000AC44],
                                   0x041060010000AC44)),
    Mutation("CRF input lists the Milan word and 44.1 kHz, runs at 44.1 kHz", "crf-format",
             lambda m: set_formats(m, (STREAM_INPUT, 1, 0), [CRF, 0x041060010000AC44],
                                   0x041060010000AC44)),
    Mutation("buffer_length 2125999 ns", "buffer-length",
             _p((STREAM_INPUT, 0), 128, 2_125_999, 4)),
    Mutation("output without CLASS_A", "class-a", _p((STREAM_OUTPUT, 0), 72, 0)),
    Mutation("CRF input lists an AAF format", "format-family",
             lambda m: set_formats(m, (STREAM_INPUT, 1, 0), [CRF, BASE_IN])),
    Mutation("output current_format 96 kHz", "current-format",
             _p((STREAM_OUTPUT, 0), 74, 0x020702200080C000, 8)),
    Mutation("input current_format carries ut, under a wider ut entry", "current-format",
             _p((STREAM_INPUT, 0), 74, 0x0215022000806000, 8),
             detail="current_format 0x0215022000806000 is not in its list"),
    Mutation("input with 47 formats", "format-count",
             lambda m: set_formats(m, (STREAM_INPUT, 0, 0), [BASE_IN] * 47)),
    Mutation("output with a redundant stream", "stream-layout",
             _p((STREAM_OUTPUT, 0), 134, 1), detail="number_of_redundant_streams 1"),
    Mutation("Table 7-8 output naming one redundant stream, its tail present", "stream-layout",
             lambda m: (put(m, (STREAM_OUTPUT, 0, 0), 134, 1),
                        store(m, (STREAM_OUTPUT, 0, 0), body(m, STREAM_OUTPUT, 0) + bytes(2))),
             detail="number_of_redundant_streams 1 in the Table 7-8 layout"),
    Mutation("Annex C output naming nine redundant streams", "stream-layout",
             lambda m: annex_c(m, (STREAM_OUTPUT, 0, 0), tuple(range(9))),
             detail="number_of_redundant_streams 9, above 8"),
    Mutation("Annex C output with Table 7-8's redundant_offset", "stream-layout",
             lambda m: (annex_c(m, (STREAM_OUTPUT, 0, 0)), put(m, (STREAM_OUTPUT, 0, 0), 132, 146)),
             detail="redundant_offset 146, not 144"),
    Mutation("Annex C output counting one redundant stream it lacks", "stream-layout",
             lambda m: (annex_c(m, (STREAM_OUTPUT, 0, 0)), put(m, (STREAM_OUTPUT, 0, 0), 134, 1)),
             detail="is 144 bytes; 1 formats and 1 redundant streams make 146"),
    Mutation("output formats_offset 146, the list moved there", "stream-layout",
             lambda m: (store(m, (STREAM_OUTPUT, 0, 0), body(m, STREAM_OUTPUT, 0)[:138] + bytes(8)
                              + body(m, STREAM_OUTPUT, 0)[138:]),
                        put(m, (STREAM_OUTPUT, 0, 0), 82, 146)),
             detail="formats_offset 146, not 138"),
    Mutation("output redundant_offset 0", "stream-layout", _p((STREAM_OUTPUT, 0), 132, 0),
             detail="redundant_offset 0, not 146"),
    Mutation("output 8 bytes past its formats, offsets consistent", "stream-layout",
             lambda m: store(m, (STREAM_OUTPUT, 0, 0), body(m, STREAM_OUTPUT, 0) + bytes(8)),
             detail="is 154 bytes; 1 formats make 146"),
    Mutation("output number_of_formats 2 with one format", "stream-layout",
             _p((STREAM_OUTPUT, 0), 84, 2), detail="2 formats at 138 run past its 146 bytes"),
    Mutation("configuration 1 moves port 1 to AVB_INTERFACE 1", "interface-index",
             _port_moves_in_configuration_1),
    Mutation("clock_sources_offset 78", "domain-source-offset", _p((CLOCK_DOMAIN, 0), 72, 78)),
    Mutation("no clock source in the domain", "domain-source-count",
             lambda m: set_sources(m, [])),
    Mutation("domain one word too long", "domain-source-length",
             lambda m: set_sources(m, [0, 1], b"\0\0")),
    Mutation("clock_sources [1, 0]", "domain-source-identity", lambda m: set_sources(m, [1, 0])),
    Mutation("domain names CLOCK_SOURCE 2", "domain-source-exists",
             lambda m: set_sources(m, [0, 1, 2])),
    Mutation("CRF source at the AAF input", "crf-input-source", _p((CLOCK_SOURCE, 1), 84, 0),
             detail="a CRF input with 0 INPUT_STREAM sources"),
    Mutation("two INPUT_STREAM sources at the CRF input", "crf-input-source",
             _two_sources_at_crf_input, detail="a CRF input with 2 INPUT_STREAM sources"),
    Mutation("no CRF input and no INPUT_STREAM source", "aaf-input-source",
             _no_crf_and_no_aaf_source, detail="no CRF input, and 0 INPUT_STREAM sources"),
    Mutation("no CRF input, an INPUT_STREAM source at each AAF input", "aaf-input-source",
             _a_source_at_each_aaf_input, detail="no CRF input, and 2 INPUT_STREAM sources"),
    Mutation("no INTERNAL source", "internal-source", _p((CLOCK_SOURCE, 0), 72, EXTERNAL)),
    Mutation("gPTP media clock with two interfaces", "gptp-source-interfaces",
             _gptp_with_two_interfaces),
    Mutation("input port owns AUDIO_MAP 0", "input-port-maps",
             _p((STREAM_PORT_INPUT, 0), 16, 1)),
    Mutation("stream 0 channel 0 mapped twice", "unique-mapping", _p((AUDIO_MAP, 0), 18, 0),
             detail="AUDIO_MAP 0: stream 0 channel 0 is also mapped by AUDIO_MAP 0"),
    Mutation("AUDIO_MAP 1 repeats AUDIO_MAP 0", "unique-mapping",
             _mapping_repeated_in_a_second_map,
             detail="AUDIO_MAP 1: stream 0 channel 0 is also mapped by AUDIO_MAP 0"),
    Mutation("AUDIO_MAP 0 number_of_mappings 3 with two", "unique-mapping",
             _p((AUDIO_MAP, 0), 6, 3), detail="its mappings run past the descriptor"),
    Mutation("stereo AUDIO_CLUSTER 3", "cluster-channels", _p((AUDIO_CLUSTER, 3), 84, 2)),
    Mutation("no IDENTIFY CONTROL", "identify-index", _p((CONTROL, 0), 82, MUTE, 8)),
    Mutation("IDENTIFY at index 0, then 1", "identify-index", _identify_moves),
    Mutation("identify_index_i 1", "identify-driven", lambda m: None, {"identify_index": 1}),
    Mutation("IDENTIFY maximum 1", "identify-format", _p((CONTROL, 0), 105, 1, 1),
             detail="maximum 1, not 255"),
    Mutation("entity_model_id 0", "model-id-valid", _p((ENTITY, 0), 12, 0, 8),
             detail="0x0000000000000000"),
    Mutation("entity_model_id all ones", "model-id-valid", _p((ENTITY, 0), 12, (1 << 64) - 1, 8),
             detail="0xFFFFFFFFFFFFFFFF"),
    Mutation("entity_model_id_i differs", "model-id-driven", lambda m: None,
             {"entity_model_id": 0x020000FFFE00C802}),
    Mutation("an unrecorded entity_model_id", "model-id-recorded",
             _p((ENTITY, 0), 12, 0x020000FFFE00C802, 8), recorded=True),
    Mutation("buffer_length edited under the recorded id", "model-digest",
             _p((STREAM_INPUT, 0), 128, 3_000_000, 4), recorded=True),
    Mutation("IDENTIFY reset_time edited under the recorded id", "model-digest",
             _p((CONTROL, 0), 90, 1_000_000, 4), recorded=True),
    Mutation("sampling_rates_offset 143", "rate-offset", _p((AUDIO_UNIT, 0), 140, 143)),
    Mutation("nine sampling rates", "rate-count", lambda m: set_rates(m, [48000] * 9)),
    Mutation("no sampling rate", "rate-empty", lambda m: set_rates(m, [])),
    Mutation("one word past the rate list", "rate-length",
             lambda m: (set_rates(m, [48000]), store(m, (AUDIO_UNIT, 0, 0),
                                                     body(m, AUDIO_UNIT, 0) + bytes(4)))),
    Mutation("current_sampling_rate 96000", "current-rate", _p((AUDIO_UNIT, 0), 136, 96000, 4)),
    Mutation("talker_stream_sources 2", "talker-sources", _p((ENTITY, 0), 24, 2),
             detail="talker_stream_sources 2; the most STREAM_OUTPUTs of any configuration is 1"),
    Mutation("configuration 1 has two talkers", "talker-sources",
             _second_talker_in_configuration_1,
             detail="talker_stream_sources 1; the most STREAM_OUTPUTs of any configuration is 2"),
    Mutation("listener_stream_sinks 1", "listener-sinks", _p((ENTITY, 0), 28, 1)),
    Mutation("talker_sources_i 2", "talker-sources-driven", lambda m: None,
             {"talker_sources": 2}),
    Mutation("listener_sinks_i 3", "listener-sinks-driven", lambda m: None,
             {"listener_sinks": 3}),
    Mutation("AUDIO_CLUSTER 3 four octets long", "descriptor-extent",
             lambda m: store(m, (AUDIO_CLUSTER, 3, 0), body(m, AUDIO_CLUSTER, 3) + bytes(4)),
             detail="AUDIO_CLUSTER 3: is 94 bytes; §7.2 makes 90"),
    Mutation("CONFIGURATION descriptor_counts at 78", "descriptor-extent",
             lambda m: offset_moved(m, (CONFIGURATION, 0), 72, 4),
             detail="CONFIGURATION 0: the offset at 72 is 78, not 74"),
    Mutation("AUDIO_MAP 0 eight octets past its mappings", "descriptor-extent",
             lambda m: store(m, (AUDIO_MAP, 0, 0), body(m, AUDIO_MAP, 0) + bytes(8)),
             detail="AUDIO_MAP 0: is 32 bytes; 2 entries of 8 make 24"),
    Mutation("AUDIO_MAP 0 with 64 mappings, 520 octets", "descriptor-maximum", _long_audio_map,
             detail="AUDIO_MAP 0: is 520 bytes, above 508"),
)
