#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The descriptor packer's self-test gate (issues #38, #39, #60, #89).

It drives build() and the command-line packer only, on synthetic models and
on the two models beside the generator:
  * body/key agreement (BodyKeyTest), on layout-only models packed with the
    semantic lint off;
  * one negative case per existing layout refusal (LayoutRefusalTest);
  * the semantic lint (07 section 3.1, L1 to L12): milan_min.json packs, and
    every named mutation in lint_mutations.py is refused with its check while
    the same bytes pack with the lint off (LintTest);
  * waivers, the ADP report and check, the recorded digest (WaiverTest,
    IdentityTest), and the command line (CommandLineTest).
"""
import copy
import importlib.util
import json
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any
import unittest
from unittest import mock

import lint_mutations as mut


DESC = Path(__file__).resolve().parents[2] / "hdl/aecp/desc"
GENERATOR = DESC / "gen_desc_image.py"
spec = importlib.util.spec_from_file_location("gen_desc_image", GENERATOR)
gen_desc_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen_desc_image)
MILAN_MIN = json.loads((DESC / "milan_min.json").read_text(encoding="utf-8"))
EXAMPLE = json.loads((DESC / "example_milan_8.json").read_text(encoding="utf-8"))
RECORDED = json.loads((DESC / "model_ids.json").read_text(encoding="utf-8"))["models"]
#: The waiver the lint's tests carry; the reason names a tracking issue.
WAIVER = {"rule": "L1", "check": "port-cluster-minimum", "configuration": 0,
          "type": "STREAM_PORT_INPUT", "first": 0, "last": 0,
          "reason": "kebag-logic/milan-fpga#584: an input pool with no cluster"}


def run_cli(model: dict[str, Any], directory: Path,
            *options: str) -> subprocess.CompletedProcess[str]:
    """Write the model and run the CLI with `options`, capturing its status."""
    source = directory / "model.json"
    source.write_text(json.dumps(model), encoding="utf-8")
    return subprocess.run(
        [sys.executable, "-B", str(GENERATOR), "-i", str(source),
         "-o", str(directory / "image.bin"), "-m", str(directory / "image.map"),
         *options], capture_output=True, text=True)


def normalised(model: dict[str, Any]) -> dict[str, Any]:
    """A copy with every body as literal bytes and only the ENTITY named, so
    a mutation is a write at a wire offset and binds no name."""
    out = copy.deepcopy(model)
    for row in out["descriptors"]:
        row["bytes"] = gen_desc_image.descriptor_bytes(row).hex()
        row["type"] = gen_desc_image._type_code(row["type"])
        for key in ("fields", "pad_to"):
            row.pop(key, None)
        if row["type"] != mut.ENTITY:
            row.pop("name_index", None)
    return out


def digest(model: dict[str, Any]) -> str:
    """The model digest the lint computes and reports."""
    return gen_desc_image.model_lint.lint(gen_desc_image._grouped_descriptors(model)).digest


def refusal(model: dict[str, Any], **checks: Any) -> list[str]:
    """build()'s refusal lines for a model it must refuse."""
    try:
        gen_desc_image.build(model, **checks)
    except gen_desc_image.ImageError as exc:
        return str(exc).splitlines()
    raise AssertionError("build() accepted the model")


class BodyKeyTest(unittest.TestCase):
    # Named, numeric and hex keys, plus a numeric type outside the name table.
    KEYS = (("AUDIO_CLUSTER", 0x0014), (20, 0x0014),
            ("0x0014", 0x0014), (0x1234, 0x1234))

    def model(self, form: str, key: str | int, typ: int,
              body_type: int | None = None, body_index: int = 1) -> dict[str, Any]:
        """Build a dense tree with only the last body's type or index varied.

        Dense configurations and indices, no name binding, and short bodies
        keep unrelated refusals out of the experiment. The target is cfg 1,
        index 1 so the diagnostic cannot pass by always reporting zero.
        """
        descriptors = []
        for cfg, idx in ((0, 0), (1, 0), (1, 1)):
            bt, bi = typ, idx
            if (cfg, idx) == (1, 1):
                bt = typ if body_type is None else body_type
                bi = body_index
            desc = {"type": key, "pad_to": 7}
            # Also exercise the existing defaults and integer-string keys.
            if cfg:
                desc["configuration"] = "0x1"
            if idx:
                desc["index"] = "1"
            if form == "fields":
                # Field names are optional: the wire offsets are authoritative.
                desc["fields"] = [{"size": 2, "value": bt},
                                  {"size": 2, "value": bi},
                                  {"size": 2, "bytes": "cafe"}]
            else:
                desc["bytes"] = f"{bt:04x} {bi:04x} ca fe"
            descriptors.append(desc)
        return {"format": "kl-aem-image", "version": 1,
                "descriptors": descriptors}

    def cli(self, model: dict[str, Any], directory: Path) -> subprocess.CompletedProcess[str]:
        """Write the model and run the CLI, capturing its status and diagnostics."""
        return run_cli(model, directory, "--no-lint")

    def legal(self, form: str) -> None:
        """Prove all key spellings preserve legal body bytes through build and CLI."""
        for key, typ in self.KEYS:
            with self.subTest(key=key):
                model = self.model(form, key, typ)
                image, report = gen_desc_image.build(model, lint=False)
                # Two 16-byte directory rows after the 32-byte header, then
                # three 7-byte descriptors at 8-byte strides (07 section 3.3).
                self.assertEqual(len(image), 88)
                self.assertEqual(image[64:], b"".join(
                    bytes.fromhex(f"{typ:04x}{idx:04x}cafe0000")
                    for idx in (0, 0, 1)))
                with tempfile.TemporaryDirectory() as tmp:
                    directory = Path(tmp)
                    result = self.cli(model, directory)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual((directory / "image.bin").read_bytes(), image)
                    self.assertEqual((directory / "image.map").read_text(), report)

    def refused(self, form: str, mismatch: str) -> None:
        """Prove build and CLI refuse the mismatch with context and no output files."""
        for key, typ in self.KEYS:
            with self.subTest(key=key):
                # Change only the high byte, catching truncated comparisons.
                body_type = typ ^ 0x0100 if mismatch == "type" else typ
                body_index = 0x0101 if mismatch == "index" else 1
                model = self.model(form, key, typ, body_type, body_index)
                message = (f"cfg 1 directory key type 0x{typ:04X} index 1 "
                           f"disagrees with body type 0x{body_type:04X} "
                           f"index {body_index}")
                with self.assertRaises(gen_desc_image.ImageError) as caught:
                    gen_desc_image.build(model, lint=False)
                self.assertEqual(str(caught.exception), message)
                with tempfile.TemporaryDirectory() as tmp:
                    directory = Path(tmp)
                    result = self.cli(model, directory)
                    self.assertEqual(result.returncode, 1)
                    self.assertEqual(result.stderr, f"gen_desc_image: {message}\n")
                    self.assertEqual(result.stdout, "")
                    self.assertFalse((directory / "image.bin").exists())
                    self.assertFalse((directory / "image.map").exists())

    def test_legal_fields(self) -> None:
        """Matching field bodies are accepted unchanged by build and CLI."""
        self.legal("fields")

    def test_legal_bytes(self) -> None:
        """Matching literal byte bodies are accepted unchanged by build and CLI."""
        self.legal("bytes")

    def test_type_mismatch_fields(self) -> None:
        """A type-only mismatch in fields is refused by build and CLI."""
        self.refused("fields", "type")

    def test_type_mismatch_bytes(self) -> None:
        """A type-only mismatch in literal bytes is refused by build and CLI."""
        self.refused("bytes", "type")

    def test_index_mismatch_fields(self) -> None:
        """An index-only mismatch in fields is refused by build and CLI."""
        self.refused("fields", "index")

    def test_index_mismatch_bytes(self) -> None:
        """An index-only mismatch in literal bytes is refused by build and CLI."""
        self.refused("bytes", "index")


class LayoutRefusalTest(unittest.TestCase):
    """One negative case per layout refusal the packer had before the lint
    (issue #60 item 3), each on milan_min.json with one change."""

    def refused(self, model: dict[str, Any], message: str) -> None:
        """Both the lint-on and the lint-off build refuse with `message`."""
        for lint in (True, False):
            with self.subTest(lint=lint):
                with self.assertRaises(gen_desc_image.ImageError) as caught:
                    gen_desc_image.build(model, lint=lint)
                self.assertIn(message, str(caught.exception))

    def test_index_gap(self) -> None:
        """AUDIO_CLUSTER 3 renumbered 4: indices are not dense (L2)."""
        model = normalised(MILAN_MIN)
        row = mut.find(model, mut.AUDIO_CLUSTER, 3)
        row["index"] = 4
        mut.put(model, (mut.AUDIO_CLUSTER, 4, 0), 2, 4)
        self.refused(model, "type 0x0014 indices are not dense from 0: [0, 1, 2, 4]")

    def test_duplicate(self) -> None:
        """A second AUDIO_CLUSTER 0 is a duplicate key."""
        model = normalised(MILAN_MIN)
        model["descriptors"].append(dict(mut.find(model, mut.AUDIO_CLUSTER, 0)))
        self.refused(model, "duplicate descriptor cfg 0 type 0x0014 index 0")

    def test_mixed_names(self) -> None:
        """One unnamed AUDIO_CLUSTER in a named run."""
        model = copy.deepcopy(MILAN_MIN)
        row = next(r for r in model["descriptors"]
                   if r["type"] == "AUDIO_CLUSTER" and r["index"] == 1)
        del row["name_index"]
        self.refused(model, "cfg 0 type 0x0014 mixes named and unnamed descriptors")

    def test_entity_index(self) -> None:
        """A second, named ENTITY at index 1."""
        model = normalised(MILAN_MIN)
        model["descriptors"].append(dict(mut.find(model, mut.ENTITY, 0), index=1))
        mut.put(model, (mut.ENTITY, 1, 0), 2, 1)
        self.refused(model, "cfg 0 ENTITY must contain only index 0")

    def test_configuration_gap(self) -> None:
        """A descriptor in configuration 2 of a one-configuration model."""
        model = normalised(MILAN_MIN)
        model["descriptors"].append(dict(mut.find(model, mut.AUDIO_CLUSTER, 0),
                                         configuration=2))
        self.refused(model, "configuration indices are not dense from 0")


class LintTest(unittest.TestCase):
    """The positive model, the layout vector, and one refusal per check."""

    def test_milan_min_packs(self) -> None:
        """milan_min.json packs with the lint on and reports what it requires."""
        image, report = gen_desc_image.build(MILAN_MIN, model_ids=RECORDED)
        self.assertEqual(image[:4], b"AEMI")
        for line in ("semantic lint: on (07 §3.1 rules L1 to L12)",
                     "lint waivers applied: 0",
                     "  entity_model_id_i  0x020000FFFE00C801",
                     "  talker_sources_i   1", "  listener_sinks_i   2",
                     "  identify_index_i   0",
                     "model digest sha256 " + RECORDED["0x020000FFFE00C801"]
                     + " (IEEE 1722.1-2021 §6.2.2.8, recorded digest checked)"):
            self.assertIn(line + "\n", report)

    def test_normalised_and_two_configurations_pack(self) -> None:
        """The mutations' starting points are positive cases too."""
        model = normalised(MILAN_MIN)
        self.assertIn("lint waivers applied: 0", gen_desc_image.build(model, model_ids=RECORDED)[1])
        mut.second_configuration(model)
        self.assertIn("semantic lint: on", gen_desc_image.build(model)[1])

    def test_example_is_a_layout_vector(self) -> None:
        """example_milan_8.json packs with the lint off, and is no Milan model:
        the lint refuses it, though not for its Annex C streams (R434-2 F1)."""
        _, report = gen_desc_image.build(EXAMPLE, lint=False)
        self.assertTrue(report.endswith("\nsemantic lint: off\n"))
        lines = refusal(EXAMPLE)
        self.assertFalse(any(line.startswith("L4 stream-layout:") for line in lines), lines)

    def test_every_check_has_a_mutation(self) -> None:
        """One negative case per refusal: no check of the lint goes untested."""
        self.assertEqual(sorted({m.check for m in mut.MUTATIONS}),
                         sorted(gen_desc_image.model_lint.CHECKS))

    def test_mutations(self) -> None:
        """Each mutation is refused with its check, on the arm its detail names,
        and packs with the lint off."""
        for mutation in mut.MUTATIONS:
            with self.subTest(mutation=mutation.name):
                model = normalised(MILAN_MIN)
                mutation.edit(model)
                checks = {"adp": mutation.adp or None,
                          "model_ids": RECORDED if mutation.recorded else None}
                rule = gen_desc_image.model_lint.CHECKS[mutation.check][0]
                lines = refusal(model, **checks)
                self.assertTrue(any(line.startswith(f"{rule} {mutation.check}: ")
                                    and mutation.detail in line for line in lines), lines)
                self.assertEqual(gen_desc_image.build(model, lint=False)[0][:4], b"AEMI")

    def test_boundaries_pack(self) -> None:
        """The caps accept their own value: 46 formats (IEEE 1722.1-2021 Table
        7-8), 8 sampling rates (07 §3.1 L10), a 508-octet descriptor (§7.2, L12),
        8 redundant streams in Annex C (Milan v1.2 Table C.1; which streams they
        name is not linted); and a CRF input's source beside one at an AAF input
        (Milan v1.2 §5.3.3.6 limits only the CRF input's)."""
        formats, rates, sources, longest, redundant = (normalised(MILAN_MIN) for _ in range(5))
        mut.annex_c(redundant, (mut.STREAM_OUTPUT, 0, 0), tuple(range(1, 9)))
        mut.set_formats(formats, (mut.STREAM_INPUT, 0, 0), [mut.BASE_IN] * 46)
        mut.set_rates(rates, [48000, 96000, 192000, 44100, 88200, 176400, 32000, 24000])
        source = mut.body(sources, mut.CLOCK_SOURCE, 1)
        struct.pack_into(">H", source, 84, 0)
        mut.add(sources, (mut.CLOCK_SOURCE, 2, 0), source)
        mut.set_sources(sources, [0, 1, 2])
        mut.add(longest, (mut.CONTROL, 1, 0), mut.control(longest) + bytes(508 - 113))
        for model in (formats, rates, sources, longest, redundant):
            self.assertIn("lint waivers applied: 0", gen_desc_image.build(model)[1])

    def test_fixed_extents(self) -> None:
        """Each fixed-size Milan-subset type two octets long is refused with its
        §7.2 extent (L12), and so is a mappings_offset other than 8."""
        for dtype, size in ((mut.ENTITY, 312), (mut.AVB_INTERFACE, 102),
                            (mut.CLOCK_SOURCE, 86), (mut.STREAM_PORT_INPUT, 20),
                            (mut.STREAM_PORT_OUTPUT, 20), (mut.AUDIO_CLUSTER, 90)):
            name = gen_desc_image.model_lint.type_name(dtype)
            with self.subTest(type=name):
                model = normalised(MILAN_MIN)
                mut.store(model, (dtype, 0, 0), mut.body(model, dtype, 0) + bytes(2))
                self.assertIn(f"L12 descriptor-extent: cfg 0 {name} 0: is {size + 2} bytes; "
                              f"§7.2 makes {size}", "\n".join(refusal(model)))
        model = normalised(MILAN_MIN)
        mut.offset_moved(model, (mut.AUDIO_MAP, 0), 4, 8)
        self.assertIn("L12 descriptor-extent: cfg 0 AUDIO_MAP 0: the offset at 4 is 16, not 8",
                      "\n".join(refusal(model)))

    def test_short_descriptor(self) -> None:
        """A field a rule needs past the descriptor's end is that rule's finding."""
        model = normalised(MILAN_MIN)
        mut.store(model, (mut.CLOCK_SOURCE, 0, 0), mut.body(model, mut.CLOCK_SOURCE, 0)[:80])
        self.assertIn("L6 crf-input-source: cfg 0 CLOCK_SOURCE 0: is 80 bytes; the field at 82 "
                      "needs 84 (Milan v1.2 §5.3.3.6)", refusal(model))


class ConformingModelTest(unittest.TestCase):
    """Standard-conforming models pack: L1's top-level counts and L2's order
    follow every IEEE 1722.1-2021 §7.2 owner, and L5 compares physical ports."""

    def packs(self, model: dict[str, Any]) -> None:
        """The model packs with the lint on and no waiver."""
        self.assertIn("lint waivers applied: 0", gen_desc_image.build(model)[1])

    def test_unit_and_port_controls_in_walk_order(self) -> None:
        """AUDIO_UNIT 0 owns CONTROL 1 and its input port CONTROL 2 (§7.2's walk);
        neither is top-level, so descriptor_counts still says one CONTROL."""
        model = normalised(MILAN_MIN)
        for index in (1, 2):
            mut.add(model, (mut.CONTROL, index, 0), mut.control(model), top=False)
        mut.own_controls(model, (mut.AUDIO_UNIT, 0), 96, 1)
        mut.own_controls(model, (mut.STREAM_PORT_INPUT, 0), 8, 2)
        self.packs(model)

    def test_jack_control_is_not_top_level(self) -> None:
        """JACK_INPUT 0 owns CONTROL 1 (§7.2.7): descriptor_counts lists the
        jack and the one configuration-level CONTROL (R434-1 E10)."""
        model = normalised(MILAN_MIN)
        jack = bytearray(78)
        struct.pack_into(">HH", jack, 74, 1, 1)
        mut.add(model, (mut.JACK_INPUT, 0, 0), jack)
        mut.list_count(model, mut.JACK_INPUT, 1)
        mut.add(model, (mut.CONTROL, 1, 0), mut.control(model), top=False)
        self.packs(model)

    def test_other_control_owners(self) -> None:
        """A CONTROL an AVB_INTERFACE (§7.2.8), a CONTROL_BLOCK (§7.2.33), a
        PTP_INSTANCE (§7.2.35) or AUDIO_UNIT 0's External Port Input (§7.2.3,
        §7.2.15) owns is not top-level: descriptor_counts still says one CONTROL
        (R434-2 S1)."""
        def _block(model: dict[str, Any]) -> None:
            """CONTROL_BLOCK 0, counted at the top, owns CONTROL 1."""
            block = bytearray(82)
            struct.pack_into(">HH", block, 70, 1, 1)
            mut.add(model, (0x0025, 0, 0), block)
            mut.list_count(model, 0x0025, 1)

        def _ptp(model: dict[str, Any]) -> None:
            """PTP_INSTANCE 0, counted at the top, owns CONTROL 1."""
            instance = bytearray(90)
            struct.pack_into(">HH", instance, 82, 1, 1)
            mut.add(model, (0x0027, 0, 0), instance)
            mut.list_count(model, 0x0027, 1)

        def _external(model: dict[str, Any]) -> None:
            """AUDIO_UNIT 0 owns EXTERNAL_PORT_INPUT 0, which owns CONTROL 1."""
            port = bytearray(24)
            struct.pack_into(">HH", port, 8, 1, 1)
            mut.add(model, (0x0010, 0, 0), port, top=False)
            mut.put(model, (mut.AUDIO_UNIT, 0, 0), 80, 1)

        owners = (("AVB_INTERFACE", lambda m: mut.own_controls(m, (mut.AVB_INTERFACE, 0), 98, 1)),
                  ("CONTROL_BLOCK", _block), ("PTP_INSTANCE", _ptp),
                  ("EXTERNAL_PORT_INPUT", _external))
        for owner, edit in owners:
            with self.subTest(owner=owner):
                model = normalised(MILAN_MIN)
                edit(model)
                mut.add(model, (mut.CONTROL, 1, 0), mut.control(model), top=False)
                self.packs(model)

    def test_identify_value_type_flags(self) -> None:
        """An IDENTIFY whose control_value_type carries the r or the u flag is
        still CONTROL_LINEAR_UINT8: IEEE 1722.1-2021 §7.3.6.1 keeps value_type
        in the low 14 bits (R435-2 F2, R434-2 S3)."""
        for flag in (0x8000, 0x4000):
            with self.subTest(flag=flag):
                model = normalised(MILAN_MIN)
                mut.put(model, (mut.CONTROL, 0, 0), 80, flag | 0x0001)
                self.packs(model)

    def test_interface_first_in_a_later_configuration(self) -> None:
        """Configuration 1 adds AVB_INTERFACE 1 (port 2), which configuration 0
        lacks: Milan v1.2 §5.3.3.5 binds an index to a physical port (R434-2 S1)."""
        model = normalised(MILAN_MIN)
        mut.second_configuration(model)
        mut.second_interface(model, 1)
        self.packs(model)

    def test_unit_signal_selector_is_not_top_level(self) -> None:
        """A SIGNAL_SELECTOR AUDIO_UNIT 0 owns (§7.2.3) is not counted at the top."""
        model = normalised(MILAN_MIN)
        selector = bytearray(96)
        struct.pack_into(">H", selector, 80, 96)
        mut.add(model, (0x001B, 0, 0), selector)
        mut.put(model, (mut.AUDIO_UNIT, 0, 0), 100, 1)
        self.packs(model)

    def test_second_interface_optional_per_configuration(self) -> None:
        """Configuration 0 holds ports 1 and 2, configuration 1 only port 1 at
        the same index (R435-1 probe C), or ports 1 and 3: Milan v1.2 §5.3.3.5
        binds an index to a physical port, not a port to an index."""
        model = normalised(MILAN_MIN)
        mut.second_interface(model)
        mut.second_configuration(model)
        mut.drop(model, mut.AVB_INTERFACE, 1, 1)
        mut.set_count(model, mut.AVB_INTERFACE, 1, 1)
        self.packs(model)
        model = normalised(MILAN_MIN)
        mut.second_interface(model)
        mut.second_configuration(model)
        mut.put(model, (mut.AVB_INTERFACE, 1, 1), 96, 3)
        self.packs(model)       # configuration 1 holds port 3 where configuration 0 holds port 2

    def test_annex_c_layout(self) -> None:
        """Every stream re-laid in Milan v1.2 Annex C Table C.1 with R = 0 packs,
        and so does a redundant pair of Stream Outputs in Annex C, each naming
        the other (R = 1): §5.3.3.4 allows Annex C for any stream and requires
        it for a redundant pair (R434-2 F1, R435-2 F1)."""
        model = normalised(MILAN_MIN)
        for at in ((mut.STREAM_INPUT, 0, 0), (mut.STREAM_INPUT, 1, 0), (mut.STREAM_OUTPUT, 0, 0)):
            mut.annex_c(model, at)
        self.packs(model)
        model = normalised(MILAN_MIN)
        mut.redundant_outputs(model)
        self.packs(model)

    def test_single_level_ranges_have_no_order(self) -> None:
        """The output port's clusters numbered before the input port's: §7.2
        orders only multi-level types (R435-1 probe B)."""
        model = normalised(MILAN_MIN)
        mut.put(model, (mut.STREAM_PORT_INPUT, 0, 0), 14, 2)
        mut.put(model, (mut.STREAM_PORT_OUTPUT, 0, 0), 14, 0)
        self.packs(model)


class WaiverTest(unittest.TestCase):
    """A waiver names one check on one scope, is reported, and is refused
    when it is malformed or stale."""

    def waived(self, waivers: list[dict[str, Any]]) -> dict[str, Any]:
        """milan_min with input port 0 cluster-less (an L1 finding) and `waivers`."""
        model = normalised(MILAN_MIN)
        mut.input_port_without_clusters(model)
        model["lint_waivers"] = waivers
        return model

    def test_waiver_applies_and_is_reported(self) -> None:
        """The waived finding packs, and the report names the waiver and its reason."""
        _, report = gen_desc_image.build(self.waived([WAIVER]))
        self.assertIn("lint waivers applied: 1\n  waiver L1 port-cluster-minimum: cfg 0 "
                      "STREAM_PORT_INPUT 0: 1 finding(s) waived; reason: "
                      "kebag-logic/milan-fpga#584", report)

    def test_without_the_waiver_the_rule_refuses(self) -> None:
        """Removing the waiver brings back the L1 refusal."""
        lines = refusal(self.waived([]))
        self.assertEqual(lines[0].split(" (")[0], "L1 port-cluster-minimum: cfg 0 "
                         "STREAM_PORT_INPUT 0: number_of_clusters 0; a Stream Port "
                         "contains at least one AUDIO_CLUSTER")

    def test_waiver_on_a_fixed_model_is_stale(self) -> None:
        """The same waiver on a model whose port has clusters is refused."""
        model = normalised(MILAN_MIN)
        model["lint_waivers"] = [WAIVER]
        self.assertEqual(refusal(model), [
            "lint waiver 0 (L1 port-cluster-minimum: cfg 0 STREAM_PORT_INPUT 0) is "
            "stale: its check passes for STREAM_PORT_INPUT 0; remove or narrow it"])

    def test_waiver_past_the_descriptors_is_stale(self) -> None:
        """A range naming a port that does not exist is refused."""
        lines = refusal(self.waived([dict(WAIVER, last=1)]))
        self.assertEqual(lines, ["lint waiver 0 (L1 port-cluster-minimum: cfg 0 "
                                 "STREAM_PORT_INPUT 0..1) is stale: STREAM_PORT_INPUT 1 "
                                 "does not exist; remove or narrow it"])

    def test_waiver_excuses_only_its_check(self) -> None:
        """Another L1 check on the same model still refuses."""
        model = self.waived([WAIVER])
        mut.put(model, (mut.STREAM_PORT_OUTPUT, 0, 0), 16, 0)
        self.assertEqual([line.split(":")[0] for line in refusal(model)], ["L1 has-parent"])

    def test_waiver_excuses_only_its_scope(self) -> None:
        """Two stereo clusters, one waived: the other is refused."""
        model = normalised(MILAN_MIN)
        for index in (2, 3):
            mut.put(model, (mut.AUDIO_CLUSTER, index, 0), 84, 2)
        model["lint_waivers"] = [dict(WAIVER, rule="L7", check="cluster-channels",
                                      type="AUDIO_CLUSTER", first=3, last=3)]
        lines = refusal(model)
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith("L7 cluster-channels: cfg 0 AUDIO_CLUSTER 2:"))

    def test_waiver_scope_is_its_type(self) -> None:
        """A STREAM_PORT_INPUT 0 waiver does not excuse STREAM_PORT_OUTPUT 0:
        the output port is refused and the waiver is stale."""
        model = normalised(MILAN_MIN)
        mut.output_port_without_clusters(model)
        model["lint_waivers"] = [WAIVER]
        self.assertEqual([line.split(" (Milan")[0] for line in refusal(model)], [
            "L1 port-cluster-minimum: cfg 0 STREAM_PORT_OUTPUT 0: number_of_clusters 0; a "
            "Stream Port contains at least one AUDIO_CLUSTER",
            "lint waiver 0 (L1 port-cluster-minimum: cfg 0 STREAM_PORT_INPUT 0) is stale: its "
            "check passes for STREAM_PORT_INPUT 0; remove or narrow it"])

    def test_waiver_scope_is_its_configuration(self) -> None:
        """A configuration 0 waiver does not excuse the same port in
        configuration 1: that port is refused and the waiver is stale."""
        model = normalised(MILAN_MIN)
        mut.second_configuration(model)
        mut.input_port_without_clusters(model, 1)
        model["lint_waivers"] = [WAIVER]
        self.assertEqual([line.split(" (Milan")[0] for line in refusal(model)], [
            "L1 port-cluster-minimum: cfg 1 STREAM_PORT_INPUT 0: number_of_clusters 0; a "
            "Stream Port contains at least one AUDIO_CLUSTER",
            "lint waiver 0 (L1 port-cluster-minimum: cfg 0 STREAM_PORT_INPUT 0) is stale: its "
            "check passes for STREAM_PORT_INPUT 0; remove or narrow it"])

    def test_waiver_naming_a_missing_configuration_is_stale(self) -> None:
        """A waiver for configuration 1 of a one-configuration model is refused."""
        lines = refusal(self.waived([WAIVER, dict(WAIVER, configuration=1)]))
        self.assertEqual(lines, ["lint waiver 1 (L1 port-cluster-minimum: cfg 1 "
                                 "STREAM_PORT_INPUT 0) is stale: configuration 1 does not "
                                 "exist; remove or narrow it"])

    def test_configuration_scope(self) -> None:
        """A check that names a configuration takes a waiver without a range."""
        model = normalised(MILAN_MIN)
        mut.set_formats(model, (mut.STREAM_OUTPUT, 0, 0), [0x0205021800806000],
                        0x0205021800806000)
        model["lint_waivers"] = [{"rule": "L3", "check": "talker-base-format",
                                  "configuration": 0, "type": "STREAM_OUTPUT",
                                  "reason": "owner/repo#1: a test of the scope"}]
        self.assertIn("waiver L3 talker-base-format: cfg 0 STREAM_OUTPUT: 1 finding(s)",
                      gen_desc_image.build(model)[1])

    def test_malformed_waivers(self) -> None:
        """Each malformed waiver is refused with what is wrong with it."""
        cases = ((dict(WAIVER, reason="no tracking issue"),
                  "carries no reason naming a tracking issue (repo#N)"),
                 (dict(WAIVER, rule="L2"), "names rule L2, and port-cluster-minimum is a L1"),
                 (dict(WAIVER, check="cluster-minimum"), "names no check 'cluster-minimum'"),
                 ({k: v for k, v in WAIVER.items() if k != "configuration"},
                  "missing keys ['configuration']"),
                 ({k: v for k, v in WAIVER.items() if k != "last"},
                  "gives one of first and last"),
                 (dict(WAIVER, type="NOT_A_TYPE"), "unknown descriptor type 'NOT_A_TYPE'"),
                 (dict(WAIVER, scope="all"), "unknown keys ['scope']"),
                 (["L1", "port-cluster-minimum"], "is not an object"),
                 (dict(WAIVER, first=1, last=0), "has the empty range 1..0"),
                 (dict(WAIVER, first=0.2, last=0.9), "has a first that is not an integer: 0.2"),
                 (dict(WAIVER, configuration=True),
                  "has a configuration that is not an integer: True"),
                 (dict(WAIVER, reason=["x/y#1"]), "has a reason that is not a string"),
                 (dict(WAIVER, rule=1), "has a rule that is not a string: 1"),
                 (dict(WAIVER, type=True), "has a type that is neither a name nor a code"))
        for waiver, message in cases:
            with self.subTest(message=message):
                lines = refusal(self.waived([waiver]))
                self.assertTrue(lines[0].startswith("lint waiver 0 "), lines)
                self.assertIn(message, lines[0])

    def test_overlapping_waivers_are_refused(self) -> None:
        """A second waiver of the same check that shares a descriptor with an
        earlier one is refused: each finding has exactly one waiver."""
        for second in (dict(WAIVER), dict(WAIVER, reason="owner/repo#2: the same port again")):
            with self.subTest(second=second["reason"]):
                self.assertEqual(refusal(self.waived([WAIVER, second])), [
                    "lint waiver 1 (L1 port-cluster-minimum: cfg 0 STREAM_PORT_INPUT 0) overlaps "
                    "lint waiver 0 (L1 port-cluster-minimum: cfg 0 STREAM_PORT_INPUT 0); one "
                    "waiver per descriptor"])

    def test_partly_overlapping_waivers_are_refused(self) -> None:
        """Waivers of AUDIO_CLUSTER 1..2 and 2..3 share cluster 2 only: the
        second is refused, so cluster 3's finding stands (R434-2 F2)."""
        model = normalised(MILAN_MIN)
        for index in (1, 2, 3):
            mut.put(model, (mut.AUDIO_CLUSTER, index, 0), 84, 2)
        stereo = dict(WAIVER, rule="L7", check="cluster-channels", type="AUDIO_CLUSTER")
        model["lint_waivers"] = [dict(stereo, first=1, last=2), dict(stereo, first=2, last=3)]
        self.assertEqual([line.split(" (Milan")[0] for line in refusal(model)], [
            "lint waiver 1 (L7 cluster-channels: cfg 0 AUDIO_CLUSTER 2..3) overlaps lint "
            "waiver 0 (L7 cluster-channels: cfg 0 AUDIO_CLUSTER 1..2); one waiver per descriptor",
            "L7 cluster-channels: cfg 0 AUDIO_CLUSTER 3: channel_count 2"])

    def test_waivers_are_a_list(self) -> None:
        """A lint_waivers that is not a list is refused, lint on or off."""
        model = self.waived([])
        model["lint_waivers"] = WAIVER
        for lint in (True, False):
            with self.subTest(lint=lint):
                with self.assertRaises(gen_desc_image.ImageError) as caught:
                    gen_desc_image.build(model, lint=lint)
                self.assertEqual(str(caught.exception), "lint_waivers is not a list")

    def test_lint_off_reports_unevaluated_waivers(self) -> None:
        """With the lint off the waivers are counted, not judged."""
        _, report = gen_desc_image.build(self.waived([WAIVER]), lint=False)
        self.assertTrue(report.endswith("\nsemantic lint: off (1 lint waivers not evaluated)\n"))


#: IEEE 1722.1-2021 §6.2.2.8's fixed-offset exclusions, one entry per type:
#: (type, length, the excluded fields as (name, start, end), offsets of the
#: structural octets beside them). Offsets are those of the §7.2 tables.
#: ENTITY's entity_id and entity_model_id are beyond the clause (model_lint.py).
FIXED_EXCLUSIONS = (
    (0x0000, 312, (("entity_id", 4, 12), ("entity_model_id", 12, 20), ("available_index", 36, 40),
                   ("association_id", 40, 48), ("entity_name", 48, 112),
                   ("firmware_version", 116, 180), ("group_name", 180, 244),
                   ("serial_number", 244, 308), ("current_configuration", 310, 312)),
     (3, 20, 35, 112, 115, 309)),
    (0x0002, 148, (("current_sampling_rate", 136, 140),), (135, 140)),
    (0x0005, 146, (("current_format", 74, 82),), (73, 82)),
    (0x0006, 146, (("current_format", 74, 82),), (73, 82)),
    (0x0009, 102, (("mac_address", 70, 76), ("clock_identity", 78, 86), ("priority1", 86, 87),
                   ("clock_class", 87, 88), ("offset_scaled_log_variance", 88, 90),
                   ("clock_accuracy", 90, 91), ("priority2", 91, 92), ("domain_number", 92, 93),
                   ("log_sync_interval", 93, 94), ("log_announce_interval", 94, 95),
                   ("log_pdelay_interval", 95, 96)), (69, 76, 77, 96)),
    (0x000A, 86, (("clock_source_flags", 70, 72), ("clock_source_identifier", 74, 82)),
     (69, 72, 73, 82)),
    (0x000B, 108, (("length", 92, 100),), (91, 100)),
    (0x0015, 121, (("current_format_specific", 85, 89), ("current_sampling_rate", 93, 97),
                   ("current_aspect_ratio", 101, 103), ("current_size", 107, 111),
                   ("current_color_space", 115, 117)),
     (84, 89, 92, 97, 100, 103, 106, 111, 114, 117)),
    (0x0016, 104, (("current_format", 84, 92), ("current_sampling_rate", 96, 100)),
     (83, 92, 95, 100)),
    (0x001B, 96, (("current_signal_type", 84, 86), ("current_signal_index", 86, 88),
                  ("current_signal_output", 88, 90)), (83, 90, 95)),
    (0x0024, 78, (("clock_source_index", 70, 72),), (69, 72)),
)

#: Where a CONTROL, MIXER, MATRIX and SIGNAL_TRANSCODER keep control_value_type,
#: values_offset and number_of_values, and where their value_details start
#: (IEEE 1722.1-2021 §7.2.22, §7.2.24, §7.2.25, §7.2.31); a MIXER has no count.
VALUE_FIELDS = {0x1A: (80, 94, 96, 104), 0x1C: (80, 86, None, 88),
                0x1D: (80, 94, 96, 102), 0x23: (80, 82, 84, 100)}

#: §6.2.2.8's value_details exclusions, per type and value family: (what,
#: type, control_value_type, number_of_values, value_details length, the octet
#: edited counted from the value_details start, whether the clause excludes it).
#: The layouts are Tables 7-122 (linear entry 5V + 4: min, max, step, default,
#: current, unit, string), 7-123 (selector: current, default, options, unit),
#: 7-124 (array: min, max, step, default, unit, string, current[]) and 7-126.
VALUE_EXCLUSIONS = (
    ("CONTROL linear UINT16 current[1]", 0x1A, 0x0003, 2, 28, 14 + 8, True),
    ("CONTROL linear UINT16 default[1]", 0x1A, 0x0003, 2, 28, 14 + 6, False),
    ("CONTROL linear UINT16 unit[0]", 0x1A, 0x0003, 2, 28, 10, False),
    ("CONTROL linear UINT16 string[1]", 0x1A, 0x0003, 2, 28, 14 + 12, False),
    ("CONTROL linear DOUBLE current[0]", 0x1A, 0x0009, 1, 44, 32, True),
    ("CONTROL linear INT8 current (the clause starts at UINT8)", 0x1A, 0x0000, 1, 9, 4, False),
    ("MIXER linear current", 0x1C, 0x0003, 1, 14, 8, True),
    ("MIXER linear default", 0x1C, 0x0003, 1, 14, 6, False),
    ("MATRIX linear current", 0x1D, 0x0003, 1, 14, 8, True),
    ("SIGNAL_TRANSCODER linear current", 0x23, 0x0003, 1, 14, 8, True),
    ("SIGNAL_TRANSCODER linear step", 0x23, 0x0003, 1, 14, 4, False),
    ("CONTROL selector UINT8 current", 0x1A, 0x000B, 3, 7, 0, True),
    ("CONTROL selector UINT8 default", 0x1A, 0x000B, 3, 7, 1, False),
    ("CONTROL selector UINT8 option[2]", 0x1A, 0x000B, 3, 7, 4, False),
    ("CONTROL selector UINT8 unit", 0x1A, 0x000B, 3, 7, 5, False),
    ("CONTROL selector STRING current", 0x1A, 0x0014, 2, 10, 1, True),
    ("CONTROL selector INT8 current (the clause starts at UINT8)", 0x1A, 0x000A, 2, 6, 0, False),
    ("MATRIX selector UINT16 current", 0x1D, 0x000D, 2, 10, 0, True),
    ("SIGNAL_TRANSCODER selector current", 0x23, 0x000B, 2, 6, 0, True),
    ("MIXER selector current (the clause names MIXER for linear only)", 0x1C, 0x000B, 1, 5, 0,
     False),
    ("CONTROL array UINT16 current[0]", 0x1A, 0x0018, 2, 16, 12, True),
    ("CONTROL array UINT16 current[1]", 0x1A, 0x0018, 2, 16, 15, True),
    ("CONTROL array UINT16 default", 0x1A, 0x0018, 2, 16, 6, False),
    ("CONTROL array UINT16 unit", 0x1A, 0x0018, 2, 16, 8, False),
    ("CONTROL array UINT16 string", 0x1A, 0x0018, 2, 16, 11, False),
    ("CONTROL array INT8 current[0] (the clause starts at UINT8)", 0x1A, 0x0015, 1, 9, 8, False),
    ("MATRIX array UINT8 current[0]", 0x1D, 0x0016, 1, 9, 8, True),
    ("SIGNAL_TRANSCODER array UINT8 current[0]", 0x23, 0x0016, 1, 9, 8, True),
    ("MIXER array current[0] (the clause names MIXER for linear only)", 0x1C, 0x0016, 1, 9, 8,
     False),
    ("CONTROL bode current_frequency[0]", 0x1A, 0x0020, 2, 72, 48, True),
    ("CONTROL bode current_magnitude[0]", 0x1A, 0x0020, 2, 72, 55, True),
    ("CONTROL bode current_phase[1]", 0x1A, 0x0020, 2, 72, 71, True),
    ("CONTROL bode phase_default", 0x1A, 0x0020, 2, 72, 44, False),
    ("CONTROL UTF8 value", 0x1A, 0x001F, 1, 16, 6, True),
    ("CONTROL SMPTE_TIME value", 0x1A, 0x0021, 1, 10, 9, True),
    ("CONTROL SAMPLE_RATE value", 0x1A, 0x0022, 1, 4, 0, True),
    ("CONTROL GPTP_TIME value", 0x1A, 0x0023, 1, 10, 5, True),
    ("CONTROL VENDOR value", 0x1A, 0x3FFE, 1, 12, 11, True),
    ("CONTROL signal_output, the octet before a UTF8 value", 0x1A, 0x001F, 1, 16, -1, False),
    ("MATRIX UTF8 value (the clause names CONTROL only)", 0x1D, 0x001F, 1, 8, 2, False),
)


def _same_digest(data: bytes, offset: int) -> bool:
    """Whether flipping one octet of a lone descriptor leaves the model digest."""
    edited = bytearray(data)
    edited[offset] ^= 0x5A
    dtype = struct.unpack_from(">H", data, 0)[0]
    return (gen_desc_image.model_lint.model_digest({0: {dtype: {0: bytes(data)}}})
            == gen_desc_image.model_lint.model_digest({0: {dtype: {0: bytes(edited)}}}))


def _valued(dtype: int, value_type: int, count: int, length: int) -> bytes:
    """A descriptor of a valued type with `count` values of `value_type` in
    `length` octets of value_details right after its fixed fields."""
    type_at, offset_at, count_at, start = VALUE_FIELDS[dtype]
    data = bytearray(start + length)
    struct.pack_into(">H", data, 0, dtype)
    struct.pack_into(">H", data, type_at, value_type)
    struct.pack_into(">H", data, offset_at, start)
    if count_at is not None:
        struct.pack_into(">H", data, count_at, count)
    return bytes(data)


class IdentityTest(unittest.TestCase):
    """The ADP values the lint reports and checks, and the recorded digest."""

    def test_driven_values_agree(self) -> None:
        """Driven values equal to the model's pass, and the report says so."""
        adp = {"entity_model_id": 0x020000FFFE00C801, "talker_sources": 1,
               "listener_sinks": 2, "identify_index": 0}
        _, report = gen_desc_image.build(MILAN_MIN, adp=adp, model_ids=RECORDED)
        self.assertEqual(report.count("(driven value checked)"), 4)

    def test_checks_need_the_lint(self) -> None:
        """A check asked for with the lint off, or an unknown one, is refused."""
        for checks in ({"lint": False, "adp": {"talker_sources": 1}},
                       {"lint": False, "model_ids": RECORDED},
                       {"adp": {"talker_source": 1}}):
            with self.subTest(checks=checks):
                with self.assertRaises(gen_desc_image.ImageError):
                    gen_desc_image.build(MILAN_MIN, **checks)

    def test_malformed_checks_are_refused(self) -> None:
        """A malformed recorded-digest map or driven value is an ImageError
        naming it, never another exception (R434-1 S1, R435-1 S1)."""
        digest_hex = RECORDED["0x020000FFFE00C801"]
        cases = (({"model_ids": {"not-hex": digest_hex}}, "model_ids key 'not-hex' is not a "
                  "hexadecimal entity_model_id"),
                 ({"model_ids": [digest_hex]}, "model_ids is not an object of entity_model_id: digest"),
                 ({"model_ids": {"0x020000FFFE00C801": "digest"}}, "model_ids digest 'digest' "
                  "for 0x020000FFFE00C801 is not a SHA-256 hex digest"),
                 ({"adp": {"entity_model_id": "0x020000FFFE00C801"}},
                  "adp entity_model_id '0x020000FFFE00C801' is not a non-negative integer"),
                 ({"adp": {"talker_sources": True}}, "adp talker_sources True is not a "
                  "non-negative integer"))
        for checks, message in cases:
            with self.subTest(message=message):
                with self.assertRaises(gen_desc_image.ImageError) as caught:
                    gen_desc_image.build(MILAN_MIN, **checks)
                self.assertEqual(str(caught.exception), message)

    def test_excluded_fields_keep_the_digest(self) -> None:
        """IEEE 1722.1-2021 6.2.2.8 exclusions and the unit identity move nothing."""
        model = normalised(MILAN_MIN)
        for at, offset, value, size in (((mut.ENTITY, 0, 0), 4, 0x0200_00FF_FE00_0099, 8),
                                        ((mut.ENTITY, 0, 0), 116, 0x312E, 2),
                                        ((mut.ENTITY, 0, 0), 244, 0x41, 1),
                                        ((mut.AVB_INTERFACE, 0, 0), 70, 0x0200_0000_0099, 6),
                                        ((mut.CLOCK_SOURCE, 1, 0), 74, 7, 8),
                                        ((mut.STREAM_INPUT, 0, 0), 4, 0x41, 1),
                                        ((mut.CONTROL, 0, 0), 108, 1, 1)):
            mut.put(model, at, offset, value, size)
        gen_desc_image.build(model, model_ids=RECORDED)

    def test_selector_structure_moves_the_digest(self) -> None:
        """A selector CONTROL's option is structure and its current is not
        (IEEE 1722.1-2021 §6.2.2.8, Table 7-123): under a recorded digest the
        option change is refused and the current change packs (R435-1 F3)."""
        model = normalised(MILAN_MIN)
        mut.add(model, (mut.CONTROL, 1, 0), mut.selector_control([1, 2, 3]))
        record = {"0x020000FFFE00C801": digest(model)}
        moved, current = copy.deepcopy(model), copy.deepcopy(model)
        mut.store(moved, (mut.CONTROL, 1, 0), mut.selector_control([1, 2, 7]))
        mut.store(current, (mut.CONTROL, 1, 0), mut.selector_control([1, 2, 3], current=2))
        self.assertNotEqual(digest(moved), record["0x020000FFFE00C801"])
        self.assertTrue(refusal(moved, model_ids=record)[0].startswith("L9 model-digest: "))
        gen_desc_image.build(current, model_ids=record)

    def test_object_name_is_the_clause(self) -> None:
        """§6.2.2.8 excludes object_name "in all descriptors": octets 4 to 67 of
        every Table 7-1 type that has one keep the digest, and its
        localized_description moves it; a type without one (LOCALE, STRINGS, a
        Port, a map, MATRIX_SIGNAL) is hashed there (ENTITY: FIXED_EXCLUSIONS)."""
        named = set(range(0x01, 0x0C)) | {0x14, 0x15, 0x16, 0x1A, 0x1B, 0x1C, 0x1D} \
            | set(range(0x1F, 0x29))
        for dtype in range(0x01, 0x29):
            data = bytearray(128)
            struct.pack_into(">H", data, 0, dtype)
            with self.subTest(type=gen_desc_image.model_lint.type_name(dtype)):
                self.assertEqual([_same_digest(bytes(data), at) for at in (4, 67, 68)],
                                 [dtype in named, dtype in named, False])

    def test_fixed_exclusions_are_the_clause(self) -> None:
        """§6.2.2.8 field by field, for every fixed-offset field it names: the
        first and the last octet of each keep the digest, and each structural
        octet beside them moves it."""
        for dtype, length, fields, beside in FIXED_EXCLUSIONS:
            data = bytearray(length)
            struct.pack_into(">H", data, 0, dtype)
            for name, start, end in fields:
                with self.subTest(field=f"{gen_desc_image.model_lint.type_name(dtype)} {name}"):
                    self.assertTrue(_same_digest(bytes(data), start))
                    self.assertTrue(_same_digest(bytes(data), end - 1))
            for offset in beside:
                with self.subTest(beside=f"{gen_desc_image.model_lint.type_name(dtype)} {offset}"):
                    self.assertFalse(_same_digest(bytes(data), offset))

    def test_exclusions_are_the_clause(self) -> None:
        """§6.2.2.8 field by field, for every value family it names: each
        excluded current value keeps the digest, and each neighbour the clause
        does not name (a limit, a default, an option, a unit, a string, a value
        of a family or type the clause leaves out) moves it (R435-2 F2)."""
        for what, dtype, value_type, count, length, offset, excluded in VALUE_EXCLUSIONS:
            with self.subTest(what=what):
                start = VALUE_FIELDS[dtype][3]
                self.assertEqual(_same_digest(_valued(dtype, value_type, count, length),
                                              start + offset), excluded)

    def test_record_is_current(self) -> None:
        """model_ids.json records milan_min.json's digest as packed today."""
        result = gen_desc_image.model_lint.lint(gen_desc_image._grouped_descriptors(MILAN_MIN))
        self.assertEqual(RECORDED, {"0x020000FFFE00C801": result.digest})


class CommandLineTest(unittest.TestCase):
    """The lint's options at the command line: refusals write nothing."""

    def test_milan_min(self) -> None:
        """The positive model with every check, the map holding the report."""
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            result = run_cli(MILAN_MIN, directory, "--model-ids", str(DESC / "model_ids.json"),
                             "--adp-entity-model-id", "0x020000FFFE00C801",
                             "--adp-talker-sources", "1", "--adp-listener-sinks", "2",
                             "--adp-identify-index", "0")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("(driven value checked)", (directory / "image.map").read_text())

    def test_refusal_writes_nothing(self) -> None:
        """A refused model exits 1 naming the check, and leaves no file."""
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            result = run_cli(MILAN_MIN, directory, "--adp-listener-sinks", "3")
            self.assertEqual(result.returncode, 1)
            self.assertTrue(result.stderr.startswith(
                "gen_desc_image: L11 listener-sinks-driven: cfg 0 ENTITY 0:"), result.stderr)
            self.assertFalse((directory / "image.bin").exists())
            self.assertFalse((directory / "image.map").exists())

    def test_model_ids_file_without_models(self) -> None:
        """A --model-ids file with no 'models' object exits 1 and writes nothing."""
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "ids.json").write_text('{"model": {}}', encoding="utf-8")
            result = run_cli(MILAN_MIN, directory, "--model-ids", str(directory / "ids.json"))
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stderr, f"gen_desc_image: --model-ids {directory / 'ids.json'}"
                                            ": no 'models' object\n")
            self.assertFalse((directory / "image.bin").exists())

    def test_import_by_path_touches_no_search_path(self) -> None:
        """Loading the packer by its path loads the lint by path too: nothing is
        added to sys.path and no module named model_lint or model_rules can
        shadow a consumer's own (R435-1 S3)."""
        probe = ("import importlib.util, sys\n"
                 f"spec = importlib.util.spec_from_file_location('packer', {str(GENERATOR)!r})\n"
                 "module = importlib.util.module_from_spec(spec)\n"
                 "before = list(sys.path)\n"
                 "spec.loader.exec_module(module)\n"
                 "print(sys.path == before, 'model_lint' in sys.modules, 'model_rules' in sys.modules,"
                 " module.model_lint.model_rules.CHECKS is module.model_lint.CHECKS)\n")
        result = subprocess.run([sys.executable, "-B", "-c", probe], capture_output=True,
                                text=True, check=False)
        self.assertEqual(result.stdout.split(), ["True", "False", "False", "True"], result.stderr)

    def test_one_guarded_loader(self) -> None:
        """model_lint and model_rules load through the packer's one loader, and
        a module it cannot locate is an ImportError naming it (R435-2 S1)."""
        self.assertIs(gen_desc_image.model_lint.beside, gen_desc_image._beside)
        with mock.patch.object(importlib.util, "spec_from_file_location", return_value=None):
            with self.assertRaises(ImportError) as caught:
                gen_desc_image._beside("model_rules")
        self.assertIn("no model_rules.py beside ", str(caught.exception))

    def test_example_needs_no_lint(self) -> None:
        """The layout vector packs with --no-lint and is refused without it."""
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.assertEqual(run_cli(EXAMPLE, directory).returncode, 1)
            result = run_cli(EXAMPLE, directory, "--no-lint")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("semantic lint: off", (directory / "image.map").read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
