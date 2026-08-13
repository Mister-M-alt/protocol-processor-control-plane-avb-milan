#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Check every relative markdown link and #anchor across the documentation."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINK = re.compile(r"\]\(([^)\s]+)\)")
EXPLICIT_ANCHOR = re.compile(r'<a\s+id="([^"]+)"')
HEADING = re.compile(r"^#{1,6}\s+(.*?)\s*$", re.M)
FENCED = re.compile(r"^```.*?^```", re.M | re.S)
INLINE_CODE = re.compile(r"`[^`\n]*`")


def strip_code(body: str) -> str:
    """Drop fenced blocks and inline code: links there are examples, not references."""
    return INLINE_CODE.sub("", FENCED.sub("", body))


def slug(text: str) -> str:
    """GitHub-style heading slug."""
    text = re.sub(r"`|\*|_", "", text.strip().lower())
    text = re.sub(r"[^\w\s-]", "", text)
    return re.sub(r"\s+", "-", text).strip("-")


def anchors_of(path: str) -> set:
    with open(path, encoding="utf-8") as fh:
        body = fh.read()
    found = set(EXPLICIT_ANCHOR.findall(body))
    for head in HEADING.findall(body):
        found.add(slug(head))
    return found


SKIP_DIRS = {".git", ".venv-wavedrom", "node_modules", "__pycache__"}


def md_files() -> list:
    """Every markdown file in the repository — docs/, the root, hdl/, tb/, syn/.

    The navigation is only coherent if the links OUT of the code trees are checked
    too: hdl/README.md and the tb/ suite notes point into docs/, and the guides
    point back at individual .sv modules.
    """
    out = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith("obj_")]
        out += [os.path.join(base, f) for f in files if f.endswith(".md")]
    return sorted(out)


def main() -> int:
    cache, problems, checked = {}, [], 0
    for src in md_files():
        with open(src, encoding="utf-8") as fh:
            body = strip_code(fh.read())
        for target in LINK.findall(body):
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            checked += 1
            path, _, anchor = target.partition("#")
            resolved = (
                src if not path
                else os.path.normpath(os.path.join(os.path.dirname(src), path))
            )
            rel_src = os.path.relpath(src, ROOT)
            if not os.path.exists(resolved):
                problems.append(f"{rel_src}: missing target -> {target}")
                continue
            if anchor and resolved.endswith(".md"):
                if resolved not in cache:
                    cache[resolved] = anchors_of(resolved)
                if anchor not in cache[resolved]:
                    problems.append(f"{rel_src}: missing anchor -> {target}")

    for line in problems:
        print(f"LINK FAIL: {line}")
    print(f"links: {checked} checked, "
          f"{'OK' if not problems else str(len(problems)) + ' FAILURES'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
