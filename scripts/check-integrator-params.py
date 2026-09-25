#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Keep the complete guide/diagram parameter inventories equal to the RTL top.

Only overridable parameters in the module header count, not derived localparams,
instance bindings, or names mentioned in comments. The guide inventory is the
first column of its section 2 table, not incidental mentions elsewhere. Diagram
21's SVG is its editable master; only visible text in its inventory group counts.
"""
import argparse
from collections import Counter
import re
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
TOP = ROOT / "hdl/top/protocol_processor_top.sv"
GUIDE = ROOT / "docs/guides/integrator.md"
DIAGRAM = ROOT / "docs/diagrams/21-integration-faces.svg"


def top_parameters(path: Path) -> list[str]:
    """Read overridable parameter names from the top's module header."""
    # Mask strings and comments before looking for declaration syntax. In
    # particular, punctuation inside a ROM filename must not split the header.
    body = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/', " ",
                  path.read_text(encoding="utf-8"), flags=re.S)
    start = re.search(r"\bmodule\s+protocol_processor_top\b.*?#\s*\(",
                      body, re.S)
    if not start:
        raise ValueError("cannot find protocol_processor_top parameter header")
    # Split only outer commas; defaults and derived localparams can contain
    # nested calls, casts, concatenations and packed dimensions.
    declarations, buf, depth = [], [], 0
    for char in body[start.end():]:
        if char == ")" and depth == 0:
            declarations.append("".join(buf))
            break
        if char == "," and depth == 0:
            declarations.append("".join(buf))
            buf = []
            continue
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        buf.append(char)
    else:
        raise ValueError("unterminated parameter header")

    names, kind = [], None
    for declaration in declarations:
        keyword = re.match(r"\s*(parameter|localparam)\b", declaration)
        if keyword:
            kind = keyword[1]
        name = re.search(r"\b([A-Za-z_]\w*)\s*=", declaration)
        if kind is None or not name:
            raise ValueError(f"cannot parse declaration {declaration.strip()!r}")
        if kind == "parameter":
            names.append(name[1])
    return names


def guide_parameters(path: Path) -> list[str]:
    """Read parameter names from the guide's section 2 inventory table."""
    body = path.read_text(encoding="utf-8")
    section = re.search(r"^## 2\. Parameters\b.*?(?=^## |\Z)", body, re.M | re.S)
    if not section:
        raise ValueError("cannot find section 2 parameter inventory")
    # Ignore example tables inside fenced blocks and commented-out rows.
    table = re.sub(r"^```.*?^```[^\n]*|<!--.*?-->", "", section[0],
                   flags=re.M | re.S)
    names = []
    for column in re.findall(r"^\|([^|]+)\|", table, re.M):
        names.extend(re.findall(r"`([A-Za-z_]\w*)`", column))
    return names


def diagram_parameters(path: Path) -> list[str]:
    """Read parameter names from diagram 21's visible inventory group."""
    root = ET.parse(path).getroot()
    groups = root.findall(".//*[@id='integration-parameters']")
    if len(groups) != 1:
        raise ValueError("expected one integration-parameters inventory group")
    visible = " ".join("".join(node.itertext())
                       for node in groups[0].iter("{http://www.w3.org/2000/svg}text"))
    return re.findall(r"\b[A-Za-z_]\w*_P\b", visible)


def main() -> int:
    """Compare the guide and diagram inventories against the RTL declarations."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--top", type=Path, default=TOP)
    parser.add_argument("--guide", type=Path, default=GUIDE,
                        help="alternate guide, e.g. the pre-change version")
    parser.add_argument("--diagram", type=Path, default=DIAGRAM)
    args = parser.parse_args()
    inventories, problems = {}, []
    for label, read, path in (("top", top_parameters, args.top),
                              ("guide", guide_parameters, args.guide),
                              ("diagram", diagram_parameters, args.diagram)):
        try:
            names = read(path)
        except (OSError, ValueError, ET.ParseError) as exc:
            problems.append(f"{label}: {exc}")
            continue
        inventories[label] = set(names)
        if not names:
            problems.append(f"{label}: parsed no parameters")
        for name, count in sorted(Counter(names).items()):
            if count > 1:
                problems.append(f"{label}: duplicate {name} ({count} occurrences)")

    if inventories.get("top"):
        for label in ("guide", "diagram"):
            if label not in inventories:
                continue
            for name in sorted(inventories["top"] - inventories[label]):
                problems.append(f"{label}: missing {name}")
            for name in sorted(inventories[label] - inventories["top"]):
                problems.append(f"{label}: extra {name}")
    for problem in problems:
        print(f"PARAMETERS FAIL: {problem}")
    counts = ", ".join(f"{label} {len(names)}" for label, names in inventories.items())
    print(f"parameters: {counts}, "
          f"{'OK' if not problems else str(len(problems)) + ' FAILURES'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
