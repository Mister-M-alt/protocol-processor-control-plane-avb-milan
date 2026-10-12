# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The command table's field registry: exhaustive paths and executable claims.

[] identifies command/documented rows; * identifies named map members. Container
entries describe shape only; their children each have their own classification.
The registry is data beside the exported table, never inferred from this code.
"""
from .inputs import FIELDS, Inputs, read_json

MAPS = {"status_codes", "hazard_classes", "shared_entries"}


def field_paths(value: object, path: str = "") -> set:
    """Every field present, folding row indices and named maps into patterns."""
    found = set()
    if isinstance(value, dict):
        for key, child in value.items():
            part = "*" if path in MAPS else key
            nested = f"{path}.{part}" if path else part
            found.add(nested)
            found.update(field_paths(child, nested))
    elif isinstance(value, list):
        for child in value:
            if isinstance(child, dict):
                found.update(field_paths(child, path + "[]"))
    return found


def registry(inp: Inputs) -> list:
    """Read the registry once, allowing the self-test to plant a registry copy."""
    if inp.registry is None:
        inp.registry = read_json(inp.root, FIELDS)
    return inp.registry


def check_fields(inp: Inputs) -> list:
    """Every table field is registered exactly once, with an actionable status."""
    entries = registry(inp)
    if not isinstance(entries, list) or not all(isinstance(e, dict) for e in entries):
        return ["  fields: registry must be a list of field objects"]
    p, seen = [], set()
    for entry in entries:
        path = entry.get("path")
        if not isinstance(path, str) or not path:
            p.append("  fields: registry entry needs a path")
            continue
        if path in seen:
            p.append(f"  fields: {path}: duplicate registry entry")
        seen.add(path)
        status = entry.get("status")
        if status not in ("held", "held to nothing"):
            p.append(f"  fields: {path}: status must be held or held to nothing")
        if not isinstance(entry.get("reason"), str) or not entry["reason"]:
            p.append(f"  fields: {path}: needs a reason describing its scope")
        if status == "held" and not _claim(entry):
            p.append(f"  fields: {path}: held needs consumer, check and a named drift probe")
        elif status == "held" and not _targets_field(inp.table, entry):
            p.append(f"  fields: {path}: probe must change this table field")
    actual = field_paths(inp.table)
    for path in sorted(actual - seen):
        p.append(f"  fields: {path}: table field missing from registry")
    for path in sorted(seen - actual):
        p.append(f"  fields: {path}: registry entry has no such table field")
    if inp.table.get("fields") != "aecp_fields.json":
        p.append("  fields: fields: export must name aecp_fields.json")
    return p


def _claim(entry: dict) -> bool:
    """A held field names its reader, finding words and concrete table edits."""
    probe = entry.get("probe")
    return (isinstance(entry.get("consumer"), str) and bool(entry["consumer"])
            and isinstance(entry.get("check"), str) and bool(entry["check"])
            and isinstance(probe, dict) and bool(probe.get("edits"))
            and isinstance(probe.get("words"), list) and bool(probe["words"])
            and all(isinstance(w, str) and w for w in probe["words"])
            and isinstance(probe["edits"], list)
            and all(isinstance(e, dict) and isinstance(e.get("path"), list)
                    and e["path"] and "value" in e for e in probe["edits"]))


def plant_field(inp: Inputs, probe: dict) -> None:
    """Apply a registry probe to the copy's table; no executable data is read."""
    for edit in probe["edits"]:
        owner = inp.table
        for key in edit["path"][:-1]:
            owner = owner[key]
        owner[edit["path"][-1]] = edit["value"]


def _targets_field(table: dict, entry: dict) -> bool:
    """A held claim must target its own existing field; the sweep rejects no-ops."""
    targeted = False
    for edit in entry["probe"]["edits"]:
        value, path = table, ""
        try:
            for key in edit["path"]:
                value = value[key]
                if isinstance(key, int):
                    path += "[]"
                else:
                    part = "*" if path in MAPS else key
                    path = f"{path}.{part}" if path else part
        except (KeyError, IndexError, TypeError):
            return False
        targeted |= path == entry["path"]
    return targeted
