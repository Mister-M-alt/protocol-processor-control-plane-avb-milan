#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Body/key agreement at the public build and command-line entry points."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


GENERATOR = (Path(__file__).resolve().parents[2]
             / "hdl/aecp/desc/gen_desc_image.py")
spec = importlib.util.spec_from_file_location("gen_desc_image", GENERATOR)
gen_desc_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen_desc_image)


class BodyKeyTest(unittest.TestCase):
    # Named, numeric and hex keys, plus a numeric type outside the name table.
    KEYS = (("AUDIO_CLUSTER", 0x0014), (20, 0x0014),
            ("0x0014", 0x0014), (0x1234, 0x1234))

    def model(self, form, key, typ, body_type=None, body_index=1):
        """Only the last body's type or index changes in a refusal probe.

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

    def cli(self, model, directory):
        source = directory / "model.json"
        source.write_text(json.dumps(model), encoding="utf-8")
        return subprocess.run(
            [sys.executable, "-B", str(GENERATOR), "-i", str(source),
             "-o", str(directory / "image.bin"),
             "-m", str(directory / "image.map")],
            capture_output=True, text=True, timeout=60)

    def legal(self, form):
        for key, typ in self.KEYS:
            with self.subTest(key=key):
                model = self.model(form, key, typ)
                image, report = gen_desc_image.build(model)
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

    def refused(self, form, mismatch):
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
                    gen_desc_image.build(model)
                self.assertEqual(str(caught.exception), message)
                with tempfile.TemporaryDirectory() as tmp:
                    directory = Path(tmp)
                    result = self.cli(model, directory)
                    self.assertEqual(result.returncode, 1)
                    self.assertEqual(result.stderr, f"gen_desc_image: {message}\n")
                    self.assertEqual(result.stdout, "")
                    self.assertFalse((directory / "image.bin").exists())
                    self.assertFalse((directory / "image.map").exists())

    def test_legal_fields(self):
        self.legal("fields")

    def test_legal_bytes(self):
        self.legal("bytes")

    def test_type_mismatch_fields(self):
        self.refused("fields", "type")

    def test_type_mismatch_bytes(self):
        self.refused("bytes", "type")

    def test_index_mismatch_fields(self):
        self.refused("fields", "index")

    def test_index_mismatch_bytes(self):
        self.refused("bytes", "index")


if __name__ == "__main__":
    unittest.main(verbosity=2)
