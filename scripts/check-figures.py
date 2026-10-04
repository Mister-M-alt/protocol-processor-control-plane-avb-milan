#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Hold every file under docs/diagrams/ to a figure class of docs/README.md section 3.

The classes and what this gate asks of each:

- draw.io: `src/<name>.drawio` with its export `<name>.svg` (freshness is
  `make stale`);
- WaveDrom: `wavedrom/<anchor>.svg`, rendered from the ```wavedrom block under
  `<a id="<anchor>">` in a document (freshness is `make wavedrom-check`; this
  gate fails a render whose block is gone);
- hand-authored SVG: a top-level `<name>.svg` with no `.drawio` source, listed
  in the hand-authored inventory of docs/diagrams/README.md. The SVG is its own
  source, so it must parse as XML with an `<svg>` root and a `viewBox`, carry no
  raster or embedded document (`<image>`, `<foreignObject>`), and be linked from
  a Markdown page.

Any other file there (a PNG export, say) is a format section 3 does not list
and fails. The files are those git tracks plus untracked ones it does not ignore.
"""
import argparse
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIAGRAMS = Path("docs/diagrams")
INVENTORY = DIAGRAMS / "README.md"
SVG_NS = "{http://www.w3.org/2000/svg}"
HAND_SECTION = "## Inventory (hand-authored SVG)"
FIRST_CELL = re.compile(r"^\|\s*`([^`]+\.svg)`\s*\|", re.M)
ANCHOR = re.compile(r'<a id="(fig-[a-z0-9-]+)"></a>')
MD_LINK = re.compile(r"\]\(([^)\s#]+)(?:#[^)\s]*)?\)")


def listed_files(root: Path, *trees: str) -> list:
    """Tracked and untracked-but-not-ignored files under `trees`."""
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
         "--exclude-standard", "--", *trees],
        check=True, capture_output=True).stdout.decode("utf-8")
    return sorted({Path(n) for n in out.split("\0")
                   if n and (root / n).is_file()}, key=str)


def hand_inventory(root: Path) -> list:
    """The file names in the hand-authored inventory table, in table order."""
    body = (root / INVENTORY).read_text(encoding="utf-8")
    at = body.find(HAND_SECTION)
    if at < 0:
        raise ValueError(f"{INVENTORY}: no '{HAND_SECTION}' section")
    section = body[at + len(HAND_SECTION):]
    end = section.find("\n## ")
    names = FIRST_CELL.findall(section if end < 0 else section[:end])
    if not names:
        raise ValueError(f"{INVENTORY}: the hand-authored inventory lists no SVG")
    return names


def wavedrom_anchors(root: Path, docs: list) -> set:
    """Anchors that head a ```wavedrom block, as render-wavedrom.py names them."""
    found = set()
    for rel in docs:
        anchor = None
        for line in (root / rel).read_text(encoding="utf-8").splitlines():
            m = ANCHOR.search(line)
            if m:
                anchor = m.group(1)
            if line.rstrip() == "```wavedrom" and anchor:
                found.add(anchor)
    return found


def linked_targets(root: Path, docs: list) -> set:
    """Every repository path a Markdown link or image in `docs` resolves to."""
    targets = set()
    for rel in docs:
        for target in MD_LINK.findall((root / rel).read_text(encoding="utf-8")):
            if "://" in target or target.startswith("mailto:"):
                continue
            path = (root / rel).parent / target
            try:
                targets.add(path.resolve().relative_to(root.resolve()))
            except ValueError:
                continue
    return targets


def svg_problems(path: Path) -> list:
    """What keeps a hand-authored SVG from being its own editable source."""
    try:
        svg = ET.parse(path).getroot()
    except ET.ParseError as exc:
        return [f"not well-formed XML ({exc})"]
    out = []
    if svg.tag != f"{SVG_NS}svg":
        out.append(f"root element is {svg.tag}, not an SVG-namespace <svg>")
    if "viewBox" not in svg.attrib:
        out.append("no viewBox on the <svg> root")
    for tag in ("image", "foreignObject"):
        if svg.find(f".//{SVG_NS}{tag}") is not None or svg.find(f".//{tag}") is not None:
            out.append(f"carries <{tag}>: a raster or embedded document is not editable source")
    return out


def check(root: Path) -> int:
    """Print one line per file outside a listed class; 1 on any finding."""
    problems, counts = [], {"draw.io": 0, "WaveDrom": 0, "hand-authored": 0}
    try:
        hand = hand_inventory(root)
    except (OSError, ValueError) as exc:
        print(f"FIGURE FAIL: {exc}")
        print("figures: inventory unreadable, FAILURES")
        return 1
    files = listed_files(root, str(DIAGRAMS))
    docs = [p for p in listed_files(root, ".") if p.suffix == ".md"]
    anchors, linked = wavedrom_anchors(root, docs), linked_targets(root, docs)
    names = {p.relative_to(DIAGRAMS).as_posix() for p in files}
    for name in sorted(set(hand) - names):
        problems.append(f"{INVENTORY}: lists {name}, which is not in {DIAGRAMS}")
    for rel in files:
        name = rel.relative_to(DIAGRAMS).as_posix()
        if name == "README.md":
            continue
        if name.startswith("src/") and name.endswith(".drawio") and "/" not in name[4:]:
            export = Path(name[4:]).with_suffix(".svg").as_posix()
            if export not in names:
                problems.append(f"{rel}: draw.io source without its export {DIAGRAMS / export}")
            counts["draw.io"] += 1
        elif name.startswith("wavedrom/") and name.endswith(".svg") and "/" not in name[9:]:
            if name[9:-4] not in anchors:
                problems.append(f"{rel}: WaveDrom render whose ```wavedrom block "
                                f"<a id=\"{name[9:-4]}\"> is in no document")
            counts["WaveDrom"] += 1
        elif "/" not in name and name.endswith(".svg"):
            if f"src/{name[:-4]}.drawio" in names:
                continue
            if name not in hand:
                problems.append(f"{rel}: SVG with no .drawio source, not in the "
                                f"hand-authored inventory of {INVENTORY}")
                continue
            problems += [f"{rel}: {p}" for p in svg_problems(root / rel)]
            if rel not in linked:
                problems.append(f"{rel}: hand-authored figure no Markdown page links")
            counts["hand-authored"] += 1
        else:
            problems.append(f"{rel}: not a figure format docs/README.md section 3 lists")
    for line in problems:
        print(f"FIGURE FAIL: {line}")
    summary = ", ".join(f"{n} {k}" for k, n in counts.items())
    print(f"figures: {summary}, "
          f"{'OK' if not problems else str(len(problems)) + ' FAILURES'}")
    return 1 if problems else 0


GOOD_SVG = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10">'
            '<rect width="10" height="10" fill="#fff"/></svg>\n')
INVENTORY_BODY = ("# Diagrams\n\n" + HAND_SECTION + "\n\n| File | Figure |\n|---|---|\n"
                  "| `20-a.svg` | a |\n\n## Commands\n")
BASE_TREE = {
    "docs/diagrams/README.md": INVENTORY_BODY,
    "docs/diagrams/20-a.svg": GOOD_SVG,
    "docs/diagrams/01-b.svg": GOOD_SVG,
    "docs/diagrams/src/01-b.drawio": "<mxfile/>\n",
    "docs/diagrams/wavedrom/fig-x.svg": GOOD_SVG,
    "docs/page.md": ("![a](diagrams/20-a.svg)\n\n<a id=\"fig-x\"></a>\n\n"
                     "```wavedrom\n{}\n```\n"),
}
# (files changed from BASE_TREE, None = deleted; expected finding substrings)
SELFTEST_CASES = (
    ({}, []),
    ({"docs/diagrams/20-a.png": "PNG"}, ["20-a.png: not a figure format"]),
    ({"docs/diagrams/25-c.svg": GOOD_SVG}, ["25-c.svg: SVG with no .drawio source"]),
    ({"docs/diagrams/20-a.svg": "<svg"}, ["20-a.svg: not well-formed XML"]),
    ({"docs/diagrams/20-a.svg": GOOD_SVG.replace("<rect", '<image href="x.png"/><rect')},
     ["20-a.svg: carries <image>"]),
    ({"docs/diagrams/20-a.svg": GOOD_SVG.replace(' viewBox="0 0 10 10"', "")},
     ["20-a.svg: no viewBox"]),
    ({"docs/page.md": "<a id=\"fig-x\"></a>\n\n```wavedrom\n{}\n```\n"},
     ["20-a.svg: hand-authored figure no Markdown page links"]),
    ({"docs/diagrams/20-a.svg": None}, ["lists 20-a.svg, which is not in"]),
    ({"docs/page.md": "![a](diagrams/20-a.svg)\n"},
     ["wavedrom/fig-x.svg: WaveDrom render whose"]),
    ({"docs/diagrams/01-b.svg": None},
     ["src/01-b.drawio: draw.io source without its export"]),
)


def selftest() -> int:
    """Run the gate over planted trees: each planted file caught, the base clean."""
    failures = 0
    for number, (changes, expect) in enumerate(SELFTEST_CASES, start=1):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            for rel, body in {**BASE_TREE, **changes}.items():
                if body is None:
                    continue
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(body, encoding="utf-8")
            proc = subprocess.run([sys.executable, __file__, "--root", str(root)],
                                  capture_output=True, text=True)
            found = [l for l in proc.stdout.splitlines() if l.startswith("FIGURE FAIL")]
            caught = all(any(e in l for l in found) for e in expect)
            want_rc = 1 if expect else 0
            if not caught or len(found) != len(expect) or proc.returncode != want_rc:
                failures += 1
                print(f"SELFTEST FAIL: case {number}: want {expect} rc {want_rc}, "
                      f"got rc {proc.returncode}\n{proc.stdout}")
    print(f"figures selftest: {len(SELFTEST_CASES)} cases, "
          f"{'OK' if not failures else str(failures) + ' FAILURES'}")
    return 1 if failures else 0


def main() -> int:
    """`--selftest` proves the gate on planted trees; otherwise check `--root`."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    return selftest() if args.selftest else check(args.root)


if __name__ == "__main__":
    sys.exit(main())
