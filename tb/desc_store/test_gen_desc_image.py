#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The descriptor packer's self-test gate (issues #38, #39, #60, #89).

It drives build() and the command-line packer only, on synthetic models and
on the two models beside the generator:
  * body/key agreement (BodyKeyTest), on layout-only models packed with the
    semantic lint off;
  * one negative case per existing layout refusal (LayoutRefusalTest);
  * the semantic lint (07 section 3.1, L1 to L11): milan_min.json packs, and
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
        for line in ("semantic lint: on (07 §3.1 rules L1 to L11)",
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
        """example_milan_8.json packs with the lint off, and is no Milan model."""
        _, report = gen_desc_image.build(EXAMPLE, lint=False)
        self.assertTrue(report.endswith("\nsemantic lint: off\n"))
        lines = refusal(EXAMPLE)
        self.assertTrue(any(line.startswith("L4 stream-layout:") for line in lines))

    def test_every_check_has_a_mutation(self) -> None:
        """One negative case per refusal: no check of the lint goes untested."""
        self.assertEqual(sorted({m.check for m in mut.MUTATIONS}),
                         sorted(gen_desc_image.model_lint.CHECKS))

    def test_mutations(self) -> None:
        """Each mutation is refused with its check and packs with the lint off."""
        for mutation in mut.MUTATIONS:
            with self.subTest(mutation=mutation.name):
                model = normalised(MILAN_MIN)
                mutation.edit(model)
                checks = {"adp": mutation.adp or None,
                          "model_ids": RECORDED if mutation.recorded else None}
                rule = gen_desc_image.model_lint.CHECKS[mutation.check][0]
                lines = refusal(model, **checks)
                self.assertTrue(any(line.startswith(f"{rule} {mutation.check}: ")
                                    for line in lines), lines)
                self.assertEqual(gen_desc_image.build(model, lint=False)[0][:4], b"AEMI")


class OwnershipTest(unittest.TestCase):
    """L1's top-level counts and L2's order follow every IEEE 1722.1-2021 §7.2
    owner: conforming models that own descriptors outside the Units pack."""

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

    def test_unit_signal_selector_is_not_top_level(self) -> None:
        """A SIGNAL_SELECTOR AUDIO_UNIT 0 owns (§7.2.3) is not counted at the top."""
        model = normalised(MILAN_MIN)
        selector = bytearray(96)
        struct.pack_into(">H", selector, 80, 96)
        mut.add(model, (0x001B, 0, 0), selector)
        mut.put(model, (mut.AUDIO_UNIT, 0, 0), 100, 1)
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
                 (dict(WAIVER, scope="all"), "unknown keys ['scope']"))
        for waiver, message in cases:
            with self.subTest(message=message):
                lines = refusal(self.waived([waiver]))
                self.assertTrue(lines[0].startswith("lint waiver 0 "), lines)
                self.assertIn(message, lines[0])

    def test_lint_off_reports_unevaluated_waivers(self) -> None:
        """With the lint off the waivers are counted, not judged."""
        _, report = gen_desc_image.build(self.waived([WAIVER]), lint=False)
        self.assertTrue(report.endswith("\nsemantic lint: off (1 lint waivers not evaluated)\n"))


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
