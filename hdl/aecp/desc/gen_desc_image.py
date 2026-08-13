#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Descriptor-image generator for KL_aecp_desc_store (docs/architecture/07 §3.3).

Turns a vendor-neutral JSON description of an AEM descriptor tree into the FLAT
MEMORY IMAGE the store fetches over its read-only memory master — a header, a
per-(configuration, type) index map, the concatenated descriptors in
(configuration, type, index) order, and the 64-byte name table of 07 §3.4.

The image lives in the integrator's main memory (the reference platform puts it
in DDR3), NOT in a $readmemh ROM: the consumer's model is already 22,561 bytes
at the 8x8 shape and grows with every stream, descriptor and localized string,
which is the wrong thing to spend RAMB tiles on.  Nothing here knows what the
memory is.

=============================================================================
INPUT FORMAT  (JSON; see example_milan_8.json)
=============================================================================

{
  "format": "kl-aem-image", "version": 1,
  "names": [ "entity name", "group name", ... ],   // 64-byte overlay entries
  "descriptors": [
    { "configuration": 0,            // configuration_index this belongs to
      "type": "ENTITY",              // name from TYPES, or 5 / "0x0005"
      "index": 0,                    // descriptor_index, dense from 0 per type
      "name_index": 0,               // optional: first name-table entry, else -1
      "pad_to": 312,                 // optional: zero-pad to this exact length
      "fields": [                    // ordered; concatenated big-endian
        {"name": "descriptor_type", "size": 2, "value": "0x0000"},
        {"name": "entity_name",     "size": 64, "string": "..."},
        {"name": "raw",             "size": 6,  "bytes": "001b92fffe01"},
        {"name": "reserved",        "size": 8}          // value defaults to 0
      ] },
    { "configuration": 0, "type": "AUDIO_MAP", "index": 0,
      "bytes": "00170000..." }       // alternative: the whole descriptor as hex
  ]
}

A field carries exactly one of `value` (big-endian unsigned integer, int or
"0x.." string), `string` (UTF-8, zero-padded/truncated to `size`), `bytes` (hex,
zero-padded/truncated to `size`), or nothing (zeros).

=============================================================================
OUTPUT IMAGE LAYOUT  (all multi-byte fields BIG-ENDIAN, the 1722.1 wire order)
=============================================================================

  header @0x00, 32 bytes = 4 x 64-bit beats
    +0x00 u32 magic            0x41454D49  ("AEMI")
    +0x04 u16 layout_version   1
    +0x06 u16 n_config         number of configurations
    +0x08 u16 n_entries        index-map entries (all configurations)
    +0x0A u16 n_names          64-byte name-table entries
    +0x0C u32 index_off        byte offset of the index-map array
    +0x10 u32 names_off        byte offset of the name table
    +0x14 u32 image_bytes      total image length
    +0x18 u16 desc_max_len     longest descriptor in the image
    +0x1A u16 reserved         0
    +0x1C u32 checksum         chosen so the eight u32 words sum to 0xFFFFFFFF

  The checksum is what separates "software has not loaded the image yet" from
  "silently wrong": uninitialised DRAM is not a recognisable zero, so the store
  refuses to serve anything until magic + version + checksum all agree.

  index map @index_off, n_entries x 16 bytes, sorted by (config, type)
    +0x00 u16 config_index
    +0x02 u16 descriptor_type
    +0x04 u16 count        descriptors of this type in this configuration
    +0x06 u16 elem_len     byte length of EACH of them
    +0x08 u32 elem_off     byte offset of index 0 (8-byte aligned)
    +0x0C u16 name_base    name-table entry of index 0 (0xFFFF = unnamed)
    +0x0E u16 elem_stride  elem_len rounded UP to a multiple of 8

  LAYOUT-VERSION-1 CONSTRAINT: every descriptor of one (configuration, type)
  has the SAME length, so a locate is elem_off + index*elem_stride with no
  second indirection and no DRAM round trip beyond the one that fetches the
  descriptor.  Milan §6.3/§6.4 rate-completeness and configuration-uniformity
  already force this for the stream descriptors; the generator REFUSES an input
  that violates it rather than emitting a layout the store cannot address.

  The STRIDE is separate from the LENGTH because a descriptor length is rarely
  a multiple of 8 (07 §3.2: CONFIGURATION 74+4n, AVB_INTERFACE 102,
  CLOCK_SOURCE 86) and the store fetches 64-bit beats: with a bare
  base + index*elem_len, descriptor 1 of such a type would start mid-beat and
  the whole line buffer would be byte-shifted.  Padding the STRIDE keeps every
  descriptor 8-aligned while elem_len stays the true wire length.

  descriptors, then the name table (n_names x 64 bytes), 8-byte aligned.

=============================================================================
USAGE
=============================================================================
  gen_desc_image.py -i example_milan_8.json -o image.bin [-m image.map]
"""
import argparse
import json
import sys

# IEEE 1722.1-2021 Table 7-2 descriptor types (the ones 07 §3.1 names, plus the
# handful a Milan PAAD may still carry).  A numeric type is always accepted.
TYPES = {
    "ENTITY": 0x0000, "CONFIGURATION": 0x0001, "AUDIO_UNIT": 0x0002,
    "VIDEO_UNIT": 0x0003, "SENSOR_UNIT": 0x0004,
    "STREAM_INPUT": 0x0005, "STREAM_OUTPUT": 0x0006,
    "JACK_INPUT": 0x0007, "JACK_OUTPUT": 0x0008,
    "AVB_INTERFACE": 0x0009, "CLOCK_SOURCE": 0x000A,
    "MEMORY_OBJECT": 0x000B, "LOCALE": 0x000C, "STRINGS": 0x000D,
    "STREAM_PORT_INPUT": 0x000E, "STREAM_PORT_OUTPUT": 0x000F,
    "EXTERNAL_PORT_INPUT": 0x0010, "EXTERNAL_PORT_OUTPUT": 0x0011,
    "AUDIO_CLUSTER": 0x0014, "AUDIO_MAP": 0x0017,
    "CONTROL": 0x001A, "SIGNAL_SELECTOR": 0x001B,
    "CLOCK_DOMAIN": 0x0024,
}

MAGIC = 0x41454D49
LAYOUT_VERSION = 1
HDR_BYTES = 32
IDX_BYTES = 16
NAME_BYTES = 64
NAME_NONE = 0xFFFF


class ImageError(Exception):
    pass


def _u(value):
    """int, or a decimal/hex string."""
    if isinstance(value, bool):
        raise ImageError("bool is not a descriptor value")
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def _type_code(spec):
    if isinstance(spec, str) and spec in TYPES:
        return TYPES[spec]
    try:
        return _u(spec)
    except (ValueError, ImageError):
        raise ImageError(f"unknown descriptor type {spec!r}")


def field_bytes(fld):
    size = _u(fld["size"])
    if size < 0:
        raise ImageError(f"negative size in field {fld.get('name')}")
    given = [k for k in ("value", "string", "bytes") if k in fld]
    if len(given) > 1:
        raise ImageError(f"field {fld.get('name')!r} sets {given} — pick one")
    if "value" in fld:
        val = _u(fld["value"])
        if val < 0 or val >= (1 << (8 * size)):
            raise ImageError(f"field {fld.get('name')!r} value does not fit "
                             f"{size} bytes")
        return val.to_bytes(size, "big")
    if "string" in fld:
        raw = str(fld["string"]).encode("utf-8")[:size]
        return raw + b"\x00" * (size - len(raw))
    if "bytes" in fld:
        raw = bytes.fromhex("".join(str(fld["bytes"]).split()))
        if len(raw) > size:
            raise ImageError(f"field {fld.get('name')!r} has {len(raw)} bytes "
                             f"for size {size}")
        return raw + b"\x00" * (size - len(raw))
    return b"\x00" * size


def descriptor_bytes(desc):
    if "bytes" in desc and "fields" in desc:
        raise ImageError("a descriptor sets both 'bytes' and 'fields'")
    if "bytes" in desc:
        body = bytes.fromhex("".join(str(desc["bytes"]).split()))
    else:
        body = b"".join(field_bytes(f) for f in desc.get("fields", []))
    if "pad_to" in desc:
        want = _u(desc["pad_to"])
        if len(body) > want:
            raise ImageError(f"descriptor is {len(body)} bytes, pad_to {want}")
        body += b"\x00" * (want - len(body))
    if len(body) < 4:
        raise ImageError("a descriptor is at least its type+index (4 bytes)")
    return body


def build(model, line_bytes=576):
    """model (parsed JSON) -> (image bytes, human-readable map)."""
    if model.get("format") != "kl-aem-image":
        raise ImageError("input is not a kl-aem-image document")
    if _u(model.get("version", 0)) != LAYOUT_VERSION:
        raise ImageError(f"input version {model.get('version')} != "
                         f"{LAYOUT_VERSION}")

    # ---- group by (configuration, type), ordered by (config, type, index) ---
    groups = {}
    for desc in model.get("descriptors", []):
        cfg = _u(desc.get("configuration", 0))
        typ = _type_code(desc["type"])
        idx = _u(desc.get("index", 0))
        body = descriptor_bytes(desc)
        nidx = _u(desc.get("name_index", NAME_NONE))
        key = (cfg, typ)
        if idx in groups.setdefault(key, {}):
            raise ImageError(f"duplicate descriptor cfg {cfg} type "
                             f"0x{typ:04X} index {idx}")
        groups[key][idx] = (body, nidx)
    if not groups:
        raise ImageError("the model declares no descriptors")

    entries = []
    for (cfg, typ) in sorted(groups):
        members = groups[(cfg, typ)]
        want = sorted(members)
        if want != list(range(len(want))):
            raise ImageError(f"cfg {cfg} type 0x{typ:04X} indices are not "
                             f"dense from 0: {want} (07 §3.1 rule L2)")
        # A type does NOT have to be one uniform run. A Milan end-station puts
        # its media sink and its CRF sink both under STREAM_INPUT, and 07
        # §3.2's format list makes them different lengths (AAF advertising two
        # formats is 148 bytes, CRF advertising one is 140) — so REFUSING a
        # mixed-length type refused every real build of this device, not a
        # corner case. It is also not a model defect to fix upstream: 1722.1
        # §7.2.6 sizes a stream descriptor by its own number_of_formats, and a
        # CRF sink cannot advertise AAF formats to pad itself level.
        #
        # The layout is unchanged and still version 1. A type with mixed
        # lengths is emitted as SEVERAL index entries, one per maximal run of
        # equal-length descriptors, in ascending index order — each internally
        # uniform, so the "locate = elem_off + i*stride, no second
        # indirection" property survives intact. What absorbs the split is the
        # SCAN: entries for one (cfg, type) are contiguous and ordered, so the
        # store accumulates the counts it has walked past and subtracts that
        # running base from the key. A type that is uniform emits exactly one
        # entry and the running base stays zero — byte-identical to before,
        # which is why no version bump is owed and old images still load.
        runs = []
        for i in want:
            body, nidx = members[i]
            if runs and len(runs[-1]["bodies"][0]) == len(body):
                runs[-1]["bodies"].append(body)
            else:
                runs.append({"first": i, "name_base": nidx, "bodies": [body]})
        for run in runs:
            elem_len = len(run["bodies"][0])
            if elem_len > line_bytes:
                raise ImageError(
                    f"cfg {cfg} type 0x{typ:04X} index {run['first']} is "
                    f"{elem_len} bytes, over the {line_bytes}-byte store line "
                    f"buffer — the store would answer NO_SUCH_DESCRIPTOR for it")
            entries.append({"cfg": cfg, "type": typ, "count": len(run["bodies"]),
                            "elem_len": elem_len, "name_base": run["name_base"],
                            "stride": (elem_len + 7) & ~7,
                            "first": run["first"],
                            "bodies": run["bodies"]})

    names = [str(n) for n in model.get("names", [])]
    n_config = len({e["cfg"] for e in entries})
    if sorted({e["cfg"] for e in entries}) != list(range(n_config)):
        raise ImageError("configuration indices are not dense from 0")

    # ---- lay the image out --------------------------------------------------
    index_off = HDR_BYTES
    cursor = index_off + IDX_BYTES * len(entries)
    cursor = (cursor + 7) & ~7
    blobs = []
    for ent in entries:
        ent["elem_off"] = cursor
        for body in ent["bodies"]:
            blobs.append((cursor, body))
            cursor += ent["stride"]

    names_off = cursor
    cursor += NAME_BYTES * len(names)
    image_bytes = (cursor + 7) & ~7
    desc_max = max(e["elem_len"] for e in entries)

    img = bytearray(image_bytes)
    for off, body in blobs:
        img[off:off + len(body)] = body
    for i, nm in enumerate(names):
        raw = nm.encode("utf-8")[:NAME_BYTES]
        img[names_off + i * NAME_BYTES:
            names_off + i * NAME_BYTES + len(raw)] = raw

    for i, ent in enumerate(entries):
        at = index_off + i * IDX_BYTES
        img[at:at + IDX_BYTES] = (
            ent["cfg"].to_bytes(2, "big") + ent["type"].to_bytes(2, "big")
            + ent["count"].to_bytes(2, "big")
            + ent["elem_len"].to_bytes(2, "big")
            + ent["elem_off"].to_bytes(4, "big")
            + (ent["name_base"] & 0xFFFF).to_bytes(2, "big")
            + ent["stride"].to_bytes(2, "big"))

    head = (MAGIC.to_bytes(4, "big") + LAYOUT_VERSION.to_bytes(2, "big")
            + n_config.to_bytes(2, "big") + len(entries).to_bytes(2, "big")
            + len(names).to_bytes(2, "big") + index_off.to_bytes(4, "big")
            + names_off.to_bytes(4, "big") + image_bytes.to_bytes(4, "big")
            + desc_max.to_bytes(2, "big") + b"\x00\x00")
    words = [int.from_bytes(head[i:i + 4], "big") for i in range(0, 28, 4)]
    checksum = (0xFFFFFFFF - (sum(words) & 0xFFFFFFFF)) & 0xFFFFFFFF
    img[0:HDR_BYTES] = head + checksum.to_bytes(4, "big")

    report = [
        f"magic AEMI  layout_version {LAYOUT_VERSION}",
        f"configurations {n_config}  index entries {len(entries)}  "
        f"names {len(names)}",
        f"index_off 0x{index_off:06X}  names_off 0x{names_off:06X}  "
        f"image_bytes {image_bytes}  desc_max_len {desc_max}",
        f"checksum 0x{checksum:08X}",
        "",
        # `first` is printed because a mixed-length type occupies several rows
        # and two rows with the same cfg+type would otherwise read as a
        # duplicate entry rather than as the index ranges they are.
        "  cfg  type    first  count  elem_len  stride  elem_off   name_base",
    ]
    for ent in entries:
        nm = "-" if ent["name_base"] == NAME_NONE else str(ent["name_base"])
        report.append(f"  {ent['cfg']:>3}  0x{ent['type']:04X}  "
                      f"{ent['first']:>5}  "
                      f"{ent['count']:>5}  {ent['elem_len']:>8}  "
                      f"{ent['stride']:>6}  "
                      f"0x{ent['elem_off']:06X}  {nm:>9}")
    return bytes(img), "\n".join(report) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-i", "--input", required=True, help="model JSON")
    ap.add_argument("-o", "--out", required=True, help="flat memory image")
    ap.add_argument("-m", "--map", help="human-readable layout report")
    ap.add_argument("--line-bytes", type=int, default=576,
                    help="store line-buffer size to validate against (576)")
    args = ap.parse_args()
    with open(args.input, encoding="utf-8") as fh:
        model = json.load(fh)
    try:
        img, report = build(model, args.line_bytes)
    except ImageError as exc:
        print(f"gen_desc_image: {exc}", file=sys.stderr)
        return 1
    with open(args.out, "wb") as fh:
        fh.write(img)
    if args.map:
        with open(args.map, "w", encoding="utf-8") as fh:
            fh.write(report)
    print(f"{args.out}: {len(img)} bytes")
    print(report, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
