# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Generate and check the four field-contract references from one registry."""
import textwrap
from pathlib import Path

from .fields import registry
from .inputs import REFERENCES, GateError, Inputs

BEGIN = "BEGIN AECP FIELD REGISTRY"
END = "END AECP FIELD REGISTRY"


def reference(entries: list) -> str:
    """A complete reference, with explicit limits beside each held claim."""
    lines = ["Generated from hdl/aecp/ucode/aecp_fields.json; edit the registry.",
             "[] means table rows; * means named map members. Held means only the stated scope.",
             "Rendering inputs generate F06.14; rendering equality is not behavioral proof.", ""]
    for entry in entries:
        line = f"- {entry['path']}: {entry['status']}. {entry['reason']}"
        if entry["status"] == "held":
            line += f" Consumer: {entry['consumer']}. Check: {entry['check']}."
        lines.append(textwrap.fill(line, width=108, subsequent_indent="  ",
                                   break_long_words=False, break_on_hyphens=False))
    return "\n".join(lines)


def arch(entries: list) -> str:
    """REQ-VER-002's compact Arch contract, including the trusted exemptions."""
    held = sum(e["status"] == "held" for e in entries)
    free = [e["path"] for e in entries if e["status"] == "held to nothing"
            and e["path"] in ("common_status", "commands[].engine_status", "commands[].oversize")]
    return (f"Registry: {len(entries)} fields, {held} held within their declared scope; "
            f"{len(entries) - held} held to nothing (containers, rendering and explicit limits). "
            "Generated references: gate docstring, 09 §8.10 and PR field reference. "
            f"Explicitly held to nothing: {', '.join(free)}. Status-path claims trust these "
            "status exemptions. Every held field has a standing named mutation. "
            "F06.14 and ROM status/MVU data are generated; decode/dispatch and sizes are "
            "observed or gated. Echo command cdl with a declared floor is held only by echo "
            "equality (IDENTIFY_NOTIFICATION also by its unsolicited body); mapping per is "
            "held exactly by accepted-offset gcd. Six documented GDI rows have semantic "
            "opcode/rtl/gdi fields; name is diagnostic and section/cells render. Still "
            "hand-written: ROM programs and layout, field values/order, ACMP ROM, descriptor "
            "image, F05.3, F08.1 and reference models. GDI ceilings, opcodes above 0x007F "
            "and unswept outcomes are unproven; no golden model or stimulus generator.")


def replace_block(text: str, body: str, where: str) -> str:
    """Replace only the marked body, refusing missing or duplicate markers."""
    if text.count(BEGIN) != 1 or text.count(END) != 1:
        raise GateError(f"fields: {where}: needs exactly one pair of registry markers")
    start = text.index("\n", text.index(BEGIN)) + 1
    stop = text.rfind("\n", 0, text.index(END)) + 1
    if stop < start:
        raise GateError(f"fields: {where}: registry markers are reversed")
    return text[:start] + body + "\n" + text[stop:]


def reference_text(entries: list, path: str, text: str) -> str:
    """The reference appropriate to a source docstring, document or PR draft."""
    if path == REFERENCES[2]:
        # The Arch cell is delimited within its single Markdown row.
        begin, end = "<!-- AECP ARCH -->", "<!-- /AECP ARCH -->"
        if text.count(begin) != 1 or text.count(end) != 1 or text.index(end) < text.index(begin):
            raise GateError(f"fields: {path}: needs the AECP Arch markers")
        a, b = text.index(begin) + len(begin), text.index(end)
        return text[:a] + arch(entries) + text[b:]
    return replace_block(text, reference(entries), path)


def check_references(inp: Inputs) -> list:
    """The tracked references are exactly the registry's rendered contract."""
    return [f"  fields: {path}: registry reference differs (run --write)"
            for path in REFERENCES
            if reference_text(registry(inp), path, inp.texts[path]) != inp.texts[path]]


def write_references(inp: Inputs) -> None:
    """Regenerate the docstring, section 09 and REQ-VER-002 Arch references."""
    for path in REFERENCES:
        new = reference_text(registry(inp), path, inp.texts[path])
        if new != inp.texts[path]:
            (inp.root / path).write_text(new, encoding="utf-8")


def pr_reference(inp: Inputs, path: Path, write: bool) -> list:
    """Generate or check a review draft outside the repository by explicit path."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GateError(f"fields: PR body: cannot read draft: {exc}") from exc
    new = reference_text(registry(inp), "PR body", text)
    if write:
        path.write_text(new, encoding="utf-8")
        return []
    return [] if new == text else ["  fields: PR body: registry reference differs (run --write --pr-body)"]
