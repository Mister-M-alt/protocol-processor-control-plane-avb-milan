#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Generate name-only descriptor fixtures with the fixed parent populations.

These are synthetic descriptor bodies for the name path, not shipping models.
The shipping parent images can be passed to run.py separately. The C++ oracle
enumerates the semantic keys independently; it never reads this file or the
generated image directory to decide an ordinal.
"""

import importlib.util
from pathlib import Path
from types import ModuleType


def packer(root: Path) -> ModuleType:
    """Load the processor's image packer without changing the import path."""
    path = root / "hdl/aecp/desc/gen_desc_image.py"
    spec = importlib.util.spec_from_file_location("name_image_packer", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fixture(aaf: int) -> dict:
    """Name populations of the pinned 1x1 TDM8 and 8x8 diagnostic models."""
    groups = ((0, 1, 312), (1, 1, 74), (2, 1, 148),
              (5, aaf + 1, 146), (6, aaf + 1, 146), (9, 1, 102),
              (10, aaf + 2, 86), (20, 25 if aaf == 1 else 72, 90),
              (26, 1, 113), (36, 1, 96))
    names = []
    descriptors = []
    for kind, count, length in groups:
        for index in range(count):
            base = len(names)
            body = bytearray(length)
            body[:2] = kind.to_bytes(2, "big")
            body[2:4] = index.to_bytes(2, "big")
            for slot in range(2 if kind == 0 else 1):
                name = f"factory {kind:04x}/{index}/{slot}"
                names.append(name)
                offset = (48 if slot == 0 else 180) if kind == 0 else 4
                body[offset:offset + 64] = name.encode().ljust(64, b"\0")
            if kind == 0:
                body[308:310] = b"\0\1"
            if kind == 2:
                body[136:140] = (48000).to_bytes(4, "big")
                body[140:144] = b"\0\x90\0\1"
                body[144:148] = (48000).to_bytes(4, "big")
            descriptors.append({"configuration": 0, "type": kind,
                                "index": index, "name_index": base,
                                "bytes": body.hex()})
    return {"format": "kl-aem-image", "version": 1,
            "names": names, "descriptors": descriptors}
