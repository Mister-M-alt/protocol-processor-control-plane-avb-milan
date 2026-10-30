#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Hold docs/INVENTORY.md to the Markdown files the tree has.

Every Markdown file has exactly one inventory row, and every row names a file
that exists. A row's status is one of the vocabulary below and its evidence is
not empty. A page the inventory calls superseded or obsolete carries its
banner near the top, and a page it calls current or generated does not, so
the row and the page cannot disagree about whether the page describes the tree.

The files are those git tracks plus untracked ones it does not ignore, so a new
page is held before it is added and build output never is.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INVENTORY = Path("docs/INVENTORY.md")
STATUSES = ("current", "generated", "stale", "superseded", "obsolete")
ROW = re.compile(r"^\| \[`([^`]+)`\]\([^)]+\) \|(.*)\|[ \t]*$", re.M)
# the banner a retired page opens with: a blockquote line in its first lines
BANNER = re.compile(r"^>.*\b(superseded|obsolete)\b", re.I | re.M)
BANNER_LINES = 20


def markdown_files(root: Path) -> set:
    """Every Markdown file git tracks or would add, as a repository-relative string."""
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
         "--exclude-standard", "--", "*.md"],
        capture_output=True, text=True, check=True).stdout
    return {p for p in out.split("\0") if p and (root / p).is_file()}


def check(root: Path) -> list:
    """Return one diagnostic per finding; an empty list is a pass."""
    inv = root / INVENTORY
    if not inv.is_file():
        return [f"INVENTORY MISSING: {INVENTORY}"]
    rows, bad = {}, []
    for path, rest in ROW.findall(inv.read_text(encoding="utf-8")):
        cells = [c.strip() for c in rest.split("|")]
        if path in rows:
            bad.append(f"DUPLICATE ROW: {path}")
            continue
        if len(cells) != 5:
            bad.append(f"MALFORMED ROW: {path}: {len(cells) + 1} cells, want 6")
            continue
        rows[path] = cells
    if not rows:
        return [f"INVENTORY EMPTY: {INVENTORY} has no file rows"]
    files = markdown_files(root)
    for path in sorted(files - rows.keys()):
        bad.append(f"NO ROW: {path}")
    for path in sorted(rows.keys() - files):
        bad.append(f"NO FILE: {path}")
    for path in sorted(rows.keys() & files):
        status, evidence = rows[path][3].strip("`* "), rows[path][4]
        if status not in STATUSES:
            bad.append(f"BAD STATUS: {path}: '{status}' is not one of {', '.join(STATUSES)}")
            continue
        if not evidence:
            bad.append(f"NO EVIDENCE: {path}")
        head = "\n".join((root / path).read_text(encoding="utf-8").splitlines()[:BANNER_LINES])
        bannered = bool(BANNER.search(head))
        if status in ("superseded", "obsolete") and not bannered:
            bad.append(f"NO BANNER: {path} is {status} but no quoted line of its first {BANNER_LINES} says superseded or obsolete")
        if status in ("current", "generated") and bannered:
            bad.append(f"BANNERED: {path} is {status} but a quoted line of its first {BANNER_LINES} calls it superseded or obsolete")
    return bad


def selftest() -> int:
    """Plant each fault in a small fixture repository and see it named.

    The fixture is the self-test's own, so it grades the checker and not this
    tree: a fault in the real inventory is the main check's to name.
    """
    pages = {
        "README.md": "# Start\n\nWhat this is.\n",
        "docs/guide.md": "# Guide\n\nHow to use it.\n",
        "docs/old.md": "# Old\n\n> **Historical, superseded** by [the guide](guide.md).\n",
    }
    row = {
        "README.md": "| [`README.md`](../README.md) | start | new reader | the repository | current | read |",
        "docs/guide.md": "| [`docs/guide.md`](guide.md) | use | integrator | the top | current | read |",
        "docs/old.md": "| [`docs/old.md`](old.md) | history | author | nothing | superseded | banner |",
    }
    head = "# Inventory\n\n| File | Purpose | Audience | Describes | Status | Evidence |\n|---|---|---|---|---|---|\n"

    def inventory(rows):
        return head + "".join(r + "\n" for r in rows)

    rows = [row[p] for p in pages] + ["| [`docs/INVENTORY.md`](INVENTORY.md) | this | author | every page | current | gate |"]
    guide = row["docs/guide.md"]

    def plant(inv=None, page=None, page_text=None, extra=None):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            subprocess.run(["git", "init", "-q", str(t)], check=True)
            for p, body in pages.items():
                (t / p).parent.mkdir(parents=True, exist_ok=True)
                (t / p).write_text(body, encoding="utf-8")
            (t / INVENTORY).write_text(inv if inv is not None else inventory(rows), encoding="utf-8")
            if page:
                (t / page).write_text(page_text, encoding="utf-8")
            if extra:
                (t / extra).parent.mkdir(parents=True, exist_ok=True)
                (t / extra).write_text("# new page\n", encoding="utf-8")
            return check(t)

    def swap(old, new):
        return inventory([new if r == old else r for r in rows])

    cases = [
        ("clean fixture", plant(), None),
        ("row dropped", plant(inv=inventory([r for r in rows if r != guide])), "NO ROW: docs/guide.md"),
        ("new page without a row", plant(extra="tb/x/README.md"), "NO ROW: tb/x/README.md"),
        ("row for a missing file", plant(inv=inventory(rows + [guide.replace("guide.md", "gone.md")])), "NO FILE: docs/gone.md"),
        ("duplicate row", plant(inv=inventory(rows + [guide])), "DUPLICATE ROW: docs/guide.md"),
        ("unknown status", plant(inv=swap(guide, guide.replace("| current |", "| fresh |"))), "BAD STATUS: docs/guide.md"),
        ("empty evidence", plant(inv=swap(guide, guide.replace("| read |", "| |"))), "NO EVIDENCE: docs/guide.md"),
        ("cell dropped", plant(inv=swap(guide, guide.replace(" read |", ""))), "MALFORMED ROW: docs/guide.md"),
        ("superseded page lost its banner", plant(page="docs/old.md", page_text="# Old\n\nIt describes the tree.\n"), "NO BANNER: docs/old.md"),
        ("current page carries a banner", plant(page="docs/guide.md", page_text="# Guide\n\n> Superseded by another page.\n"), "BANNERED: docs/guide.md"),
        ("inventory emptied", plant(inv="# Inventory\n"), "INVENTORY EMPTY"),
        ("inventory deleted", _missing(), "INVENTORY MISSING"),
    ]
    fails = 0
    for name, found, want in cases:
        ok = (not found) if want is None else any(f.startswith(want) for f in found)
        print(f"inventory selftest: {name}: {'caught' if want and ok else 'clean' if ok else 'MISSED'}")
        if not ok:
            fails += 1
            print("  got: " + ("; ".join(found) or "nothing"))
    print(f"inventory selftest: {len(cases)} cases, {'OK' if not fails else str(fails) + ' FAILED'}")
    return 1 if fails else 0


def _missing() -> list:
    """A repository with a page and no inventory at all."""
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        subprocess.run(["git", "init", "-q", str(t)], check=True)
        (t / "README.md").write_text("# Start\n", encoding="utf-8")
        return check(t)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true", help="plant each fault first and require it caught")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    bad = check(ROOT)
    for line in bad:
        print(f"INVENTORY FAIL: {line}")
    n = len(markdown_files(ROOT))
    print(f"inventory: {n} Markdown files, {'OK' if not bad else str(len(bad)) + ' FAILURES'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
