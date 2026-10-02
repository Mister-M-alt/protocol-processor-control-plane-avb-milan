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
TIMING, EXTERNAL = 0x0026, 0x0001
BASE_IN = 0x0215022002006000     # 48 kHz, up to 8 channels (Milan v1.2 Table 6.2)
CRF = 0x041060010000BB80         # Milan v1.2 Table 7.1


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


def add(model: Model, at: tuple[int, int, int], data: bytes) -> None:
    """Add a descriptor at (type, index, cfg), fixing its key bytes, and count
    it in its CONFIGURATION when the type is top-level there."""
    dtype, index, cfg = at
    data = bytearray(data)
    struct.pack_into(">HH", data, 0, dtype, index)
    model["descriptors"].append({"configuration": cfg, "type": dtype, "index": index,
                                 "bytes": bytes(data).hex()})
    counts = body(model, CONFIGURATION, cfg)
    for k in range(struct.unpack_from(">H", counts, 70)[0]):
        listed, count = struct.unpack_from(">HH", counts, 74 + 4 * k)
        if listed == dtype:
            struct.pack_into(">H", counts, 76 + 4 * k, count + 1)
            store(model, (CONFIGURATION, cfg, 0), counts)


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


def input_port_without_clusters(model: Model) -> None:
    """Stream Port Input 0 with no AUDIO_CLUSTER; its two clusters leave the tree."""
    put(model, (STREAM_PORT_INPUT, 0, 0), 12, 0)
    for index in (0, 1):
        drop(model, AUDIO_CLUSTER, index)
    for old, new in ((2, 0), (3, 1)):
        row = find(model, AUDIO_CLUSTER, old)
        row["index"] = new
        put(model, (AUDIO_CLUSTER, new, 0), 2, new)
    put(model, (STREAM_PORT_OUTPUT, 0, 0), 14, 0)


def _crf_input_becomes_aaf(model: Model) -> None:
    """STREAM_INPUT 1 advertises the Base format instead of CRF."""
    set_formats(model, (STREAM_INPUT, 1, 0), [BASE_IN], 0x0205022002006000)


def _no_crf_and_no_aaf_source(model: Model) -> None:
    """No CRF input, and the INPUT_STREAM source becomes EXTERNAL."""
    _crf_input_becomes_aaf(model)
    put(model, (CLOCK_SOURCE, 1, 0), 72, EXTERNAL)


def _gptp_with_two_interfaces(model: Model) -> None:
    """A gPTP media clock source (Milan v1.2 §7.5.2) beside a second interface."""
    source = body(model, CLOCK_SOURCE, 0)
    struct.pack_into(">HH", source, 70, 0, EXTERNAL)
    struct.pack_into(">HH", source, 82, TIMING, 0)
    add(model, (CLOCK_SOURCE, 2, 0), source)
    interface = body(model, AVB_INTERFACE, 0)
    struct.pack_into(">H", interface, 96, 2)
    add(model, (AVB_INTERFACE, 1, 0), interface)


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
    """One negative case: the edit, the check it must trip, and the checks
    the build runs with (driven ADP values; the recorded digests)."""
    name: str
    check: str
    edit: Callable[[Model], None]
    adp: dict[str, int] = field(default_factory=dict)
    recorded: bool = False


def _p(at: tuple[int, int], offset: int, value: int, size: int = 2) -> Callable[[Model], None]:
    """An edit writing one field of a configuration-0 descriptor."""
    return lambda model: put(model, (at[0], at[1], 0), offset, value, size)


def _no_streams(model: Model) -> None:
    """Every STREAM_INPUT and STREAM_OUTPUT removed."""
    for dtype, index in ((STREAM_INPUT, 0), (STREAM_INPUT, 1), (STREAM_OUTPUT, 0)):
        drop(model, dtype, index)


MUTATIONS = (
    Mutation("no ENTITY", "entity-count", lambda m: drop(m, ENTITY, 0)),
    Mutation("configurations_count 2", "configurations-count", _p((ENTITY, 0), 308, 2)),
    Mutation("current_configuration 1", "current-configuration", _p((ENTITY, 0), 310, 1)),
    Mutation("no CONFIGURATION", "configuration-descriptors",
             lambda m: drop(m, CONFIGURATION, 0)),
    Mutation("descriptor_counts says 3 STREAM_INPUTs", "descriptor-counts",
             _p((CONFIGURATION, 0), 80, 3)),
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
    Mutation("output clusters before input clusters", "parent-order",
             lambda m: (put(m, (STREAM_PORT_INPUT, 0, 0), 14, 2),
                        put(m, (STREAM_PORT_OUTPUT, 0, 0), 14, 0))),
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
    Mutation("buffer_length 2125999 ns", "buffer-length",
             _p((STREAM_INPUT, 0), 128, 2_125_999, 4)),
    Mutation("output without CLASS_A", "class-a", _p((STREAM_OUTPUT, 0), 72, 0)),
    Mutation("CRF input lists an AAF format", "format-family",
             lambda m: set_formats(m, (STREAM_INPUT, 1, 0), [CRF, BASE_IN])),
    Mutation("output current_format 96 kHz", "current-format",
             _p((STREAM_OUTPUT, 0), 74, 0x020702200080C000, 8)),
    Mutation("input with 47 formats", "format-count",
             lambda m: set_formats(m, (STREAM_INPUT, 0, 0), [BASE_IN] * 47)),
    Mutation("output with a redundant stream", "stream-layout",
             _p((STREAM_OUTPUT, 0), 134, 1)),
    Mutation("configuration 1 moves the port to AVB_INTERFACE 0 port 2", "interface-index",
             lambda m: (second_configuration(m), put(m, (AVB_INTERFACE, 0, 1), 96, 2))),
    Mutation("clock_sources_offset 78", "domain-source-offset", _p((CLOCK_DOMAIN, 0), 72, 78)),
    Mutation("no clock source in the domain", "domain-source-count",
             lambda m: set_sources(m, [])),
    Mutation("domain one word too long", "domain-source-length",
             lambda m: set_sources(m, [0, 1], b"\0\0")),
    Mutation("clock_sources [1, 0]", "domain-source-identity", lambda m: set_sources(m, [1, 0])),
    Mutation("domain names CLOCK_SOURCE 2", "domain-source-exists",
             lambda m: set_sources(m, [0, 1, 2])),
    Mutation("CRF source at the AAF input", "crf-input-source", _p((CLOCK_SOURCE, 1), 84, 0)),
    Mutation("no CRF input and no INPUT_STREAM source", "aaf-input-source",
             _no_crf_and_no_aaf_source),
    Mutation("no INTERNAL source", "internal-source", _p((CLOCK_SOURCE, 0), 72, EXTERNAL)),
    Mutation("gPTP media clock with two interfaces", "gptp-source-interfaces",
             _gptp_with_two_interfaces),
    Mutation("input port owns AUDIO_MAP 0", "input-port-maps",
             _p((STREAM_PORT_INPUT, 0), 16, 1)),
    Mutation("stream 0 channel 0 mapped twice", "unique-mapping", _p((AUDIO_MAP, 0), 18, 0)),
    Mutation("stereo AUDIO_CLUSTER 3", "cluster-channels", _p((AUDIO_CLUSTER, 3), 84, 2)),
    Mutation("no IDENTIFY CONTROL", "identify-index",
             _p((CONTROL, 0), 82, 0x90E0F00000000002, 8)),
    Mutation("IDENTIFY at index 0, then 1", "identify-index", _identify_moves),
    Mutation("identify_index_i 1", "identify-driven", lambda m: None, {"identify_index": 1}),
    Mutation("entity_model_id 0", "model-id-valid", _p((ENTITY, 0), 12, 0, 8)),
    Mutation("entity_model_id all ones", "model-id-valid", _p((ENTITY, 0), 12, (1 << 64) - 1, 8)),
    Mutation("entity_model_id_i differs", "model-id-driven", lambda m: None,
             {"entity_model_id": 0x020000FFFE00C802}),
    Mutation("an unrecorded entity_model_id", "model-id-recorded",
             _p((ENTITY, 0), 12, 0x020000FFFE00C802, 8), recorded=True),
    Mutation("buffer_length edited under the recorded id", "model-digest",
             _p((STREAM_INPUT, 0), 128, 3_000_000, 4), recorded=True),
    Mutation("sampling_rates_offset 143", "rate-offset", _p((AUDIO_UNIT, 0), 140, 143)),
    Mutation("nine sampling rates", "rate-count", lambda m: set_rates(m, [48000] * 9)),
    Mutation("no sampling rate", "rate-empty", lambda m: set_rates(m, [])),
    Mutation("one word past the rate list", "rate-length",
             lambda m: (set_rates(m, [48000]), store(m, (AUDIO_UNIT, 0, 0),
                                                     body(m, AUDIO_UNIT, 0) + bytes(4)))),
    Mutation("current_sampling_rate 96000", "current-rate", _p((AUDIO_UNIT, 0), 136, 96000, 4)),
    Mutation("talker_stream_sources 2", "talker-sources", _p((ENTITY, 0), 24, 2)),
    Mutation("configuration 1 has two talkers", "talker-sources",
             _second_talker_in_configuration_1),
    Mutation("listener_stream_sinks 1", "listener-sinks", _p((ENTITY, 0), 28, 1)),
    Mutation("talker_sources_i 2", "talker-sources-driven", lambda m: None,
             {"talker_sources": 2}),
    Mutation("listener_sinks_i 3", "listener-sinks-driven", lambda m: None,
             {"listener_sinks": 3}),
)
