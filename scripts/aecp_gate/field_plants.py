# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Standing defects of the field registry and its generated references."""
from .inputs import REFERENCES, Inputs
from .field_docs import pr_reference
from pathlib import Path
import tempfile


def draft_probe(inp: Inputs, absent: bool) -> list:
    """A stale or unreadable external PR reference must fail by name."""
    with tempfile.TemporaryDirectory(prefix="aecp-draft-selftest-") as tmp:
        path = Path(tmp) / "PR-BODY.md"
        if not absent:
            path.write_text("<!-- BEGIN AECP FIELD REGISTRY -->\nstale\n"
                            "<!-- END AECP FIELD REGISTRY -->\n", encoding="utf-8")
        return pr_reference(inp, path, False)


def entry(inp: Inputs, path: str = "commands[].opcode") -> dict:
    """One registry entry in the already copied inputs."""
    return next(e for e in inp.registry if e["path"] == path)


def text_edit(inp: Inputs, path: str, old: str, new: str) -> None:
    """An exact edit to a copied reference text."""
    inp.texts[path] = inp.texts[path].replace(old, new)


MUTANTS = (
    ("an unregistered table field", lambda i: i.table.update(unknown=1),
     ("fields: unknown: table field missing from registry",)),
    ("an unregistered nested field", lambda i: i.table["commands"][0]["command"].update(extra=1),
     ("fields: commands[].command.extra: table field missing from registry",)),
    ("a registry entry for no field", lambda i: i.registry.append(
        dict(path="absent", status="held to nothing", reason="planted")),
     ("fields: absent: registry entry has no such table field",)),
    ("a duplicate registry entry", lambda i: i.registry.append(dict(entry(i))),
     ("fields: commands[].opcode: duplicate registry entry",)),
    ("a registry which is no list", lambda i: setattr(i, "registry", {}),
     ("fields: registry must be a list of field objects",)),
    ("a registry row which is no object", lambda i: i.registry.append("bad"),
     ("fields: registry must be a list of field objects",)),
    ("a registry entry without a path", lambda i: entry(i).pop("path"),
     ("fields: registry entry needs a path",)),
    ("an unknown registry status", lambda i: entry(i).update(status="maybe"),
     ("fields: commands[].opcode: status must be held or held to nothing",)),
    ("a registry entry without a reason", lambda i: entry(i).pop("reason"),
     ("fields: commands[].opcode: needs a reason",)),
    ("a held field without a consumer", lambda i: entry(i).pop("consumer"),
     ("fields: commands[].opcode: held needs consumer, check and a named drift probe",)),
    ("a held field without a check", lambda i: entry(i).pop("check"),
     ("fields: commands[].opcode: held needs consumer, check and a named drift probe",)),
    ("a held field without a probe", lambda i: entry(i).pop("probe"),
     ("fields: commands[].opcode: held needs consumer, check and a named drift probe",)),
    ("a held field without named words", lambda i: entry(i)["probe"].update(words=[]),
     ("fields: commands[].opcode: held needs consumer, check and a named drift probe",)),
    ("a held field probe without edits", lambda i: entry(i)["probe"].update(edits=[]),
     ("fields: commands[].opcode: held needs consumer, check and a named drift probe",)),
    ("a held field probe without a value", lambda i: entry(i)["probe"]["edits"][0].pop("value"),
     ("fields: commands[].opcode: held needs consumer, check and a named drift probe",)),
    ("a held field probe of another field", lambda i: entry(i)["probe"].update(
        edits=[dict(path=["schema"], value="other")]),
     ("fields: commands[].opcode: probe must change this table field",)),
    ("a held field probe of an absent field", lambda i: entry(i)["probe"].update(
        edits=[dict(path=["absent"], value="other")]),
     ("fields: commands[].opcode: probe must change this table field",)),
    ("a missing docstring registry marker", lambda i: text_edit(
        i, REFERENCES[0], "BEGIN AECP FIELD REGISTRY", "BEGIN REMOVED"),
     ("fields:", REFERENCES[0], "needs exactly one pair of registry markers")),
    ("reversed registry markers", lambda i: text_edit(
        i, REFERENCES[0], "BEGIN AECP FIELD REGISTRY", "TEMP") or text_edit(
        i, REFERENCES[0], "END AECP FIELD REGISTRY", "BEGIN AECP FIELD REGISTRY") or text_edit(
        i, REFERENCES[0], "TEMP", "END AECP FIELD REGISTRY"),
     ("fields:", REFERENCES[0], "registry markers are reversed")),
    ("missing Arch markers", lambda i: text_edit(i, REFERENCES[2], "<!-- AECP ARCH -->", ""),
     ("fields:", REFERENCES[2], "needs the AECP Arch markers")),
) + tuple(
    (f"a stale registry reference in {path}",
     lambda i, path=path: text_edit(i, path, "held to nothing", "not classified"),
     ("fields:", path, "registry reference differs"))
    for path in REFERENCES
)
