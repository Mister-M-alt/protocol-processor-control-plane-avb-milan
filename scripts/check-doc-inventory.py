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

Where a page states a count or a list that a file in the tree defines, the
statement is derived here from that file (DERIVED below): the 06 µISA count
and table from `ucpu_pkg.sv`'s operation enum, and the 09 §7 target table from
the Makefile's `check` prerequisites. A page that would only restate such a
count links to the defining file instead, and a µISA operation count stated on
any current page, or in a retired page's banner, other than 06's is a finding.
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


#! `    OP_SHIFT_R    = 5'd29   // logical right shift by imm[5:0]`: name, then comment
OP_MEMBER = re.compile(r"^[ \t]*OP_(\w+)[ \t]*=[ \t]*5'd\d+[ \t]*,?[ \t]*(?://[ \t]*(\S+))?", re.M)
OP_ENUM = re.compile(r"typedef enum logic \[4:0\] \{(.*?)\} ucpu_op_e;", re.S)
UISA_HEAD = re.compile(r"^\*\*µISA \((\d+) operations\)\*\*", re.M)
SPELLED = re.compile(r"[A-Z][A-Z_]+")


def uisa_ops(source: str) -> list:
    """The µISA's operation names as 06 spells them, in `ucpu_pkg.sv`'s enum order.

    A member whose comment opens with an upper-case name (`OP_BR_STATUS  // BRANCH_IF_STATUS`)
    is spelled that way in 06; any other member is spelled as its own name.
    """
    enum = OP_ENUM.search(source)
    if not enum:
        return []
    return [c if c and SPELLED.fullmatch(c) else name for name, c in OP_MEMBER.findall(enum.group(1))]


def uisa_findings(page: str, source: str) -> list:
    """06 §8's µISA count and table against `ucpu_pkg.sv`'s operation enum."""
    ops = uisa_ops(source)
    if not ops:
        return ["no operation enum (`typedef enum logic [4:0] {...} ucpu_op_e;`) parsed"]
    members = len(re.findall(r"\bOP_\w+", OP_ENUM.search(source).group(1)))
    if members != len(ops):
        return [f"unparsed enum member: {members} `OP_` names, {len(ops)} in the form `OP_X = 5'dN`"]
    head = UISA_HEAD.search(page)
    if not head:
        return ["no '**µISA (N operations)**' heading"]
    table = re.search(r"\n((?:\|.*\|[ \t]*\n)+)", page[head.end():])
    named = []
    for line in (table.group(1).splitlines()[2:] if table else []):
        cells = line.split("|")
        named += re.findall(r"`(\w+)`", cells[2]) if len(cells) > 3 else []
    bad = [] if int(head.group(1)) == len(ops) else [
        f"says {head.group(1)} operations, the enum declares {len(ops)}"]
    bad += [f"table lacks {op}" for op in ops if op not in named]
    bad += [f"table names {op}, which the enum does not declare" for op in named if op not in ops]
    bad += [f"table names {op} twice" for op in sorted(set(named)) if named.count(op) > 1]
    return bad


def target_findings(page: str, source: str) -> list:
    """09 §7's target table against the Makefile's `check:` prerequisites."""
    rule = re.search(r"^check:[ \t]*(.*)$", source, re.M)
    if not rule or not rule.group(1).split():
        return ["no `check:` rule with prerequisites parsed"]
    targets = rule.group(1).split()
    section = re.search(r"^## 7\..*?(?=^## |\Z)", page, re.M | re.S)
    named = re.findall(r"^\| `([\w-]+)` \|", section.group(0), re.M) if section else []
    if not named:
        return ["no §7 target table parsed"]
    bad = [f"table lacks `{t}`" for t in targets if t not in named]
    bad += [f"table names `{t}`, which `check` does not run" for t in named if t not in targets]
    bad += [f"table names `{t}` twice" for t in sorted(set(named)) if named.count(t) > 1]
    return bad


#: (page, the file that defines what it states, the comparison).
DERIVED = (
    (Path("docs/architecture/06_aecp_engine.md"), Path("hdl/aecp/ucpu_pkg.sv"), uisa_findings),
    (Path("docs/architecture/09_verification.md"), Path("Makefile"), target_findings),
)


#: a line naming the µISA with an operation count: `29-operation ISA`, `µISA (30 operations)`
UISA_COUNT = re.compile(r"(?=.*(?<![A-Za-z])ISA\b)(?=.*\b\d+[- ](?:operations?|ops)\b)")


def restated_uisa(path: str, text: str, retired: bool) -> list:
    """A µISA operation count stated anywhere but the heading DERIVED holds in 06 §8.

    A current page links 06 §8 instead. A retired page's body is history, but its
    opening banner says what the tree does, so the banner is held too.
    """
    lines = text.splitlines()
    span = range(len(lines))
    if retired:
        start = next((i for i, line in enumerate(lines) if line.startswith(">")), len(lines))
        end = next((i for i in range(start, len(lines)) if not lines[i].startswith(">")), len(lines))
        span = range(start, end)
    if path == str(DERIVED[0][0]):  # the derived heading itself, and nothing else on its line
        lines = [UISA_HEAD.sub("", line) for line in lines]
    return [f"RESTATED: {path}:{i + 1}: a µISA operation count; link 06 §8, whose count is derived"
            for i in span if UISA_COUNT.match(lines[i])]


def derived_findings(root: Path, derived: tuple) -> list:
    """Each DERIVED page's statement against its defining file; a missing file is a finding."""
    bad = []
    for page, source, compare in derived:
        missing = [str(p) for p in (page, source) if not (root / p).is_file()]
        if missing:
            bad.append(f"DERIVED MISSING: {', '.join(missing)}")
            continue
        texts = [(root / p).read_text(encoding="utf-8") for p in (page, source)]
        bad += [f"DERIVED: {page} against {source}: {f}" for f in compare(*texts)]
    return bad


def check(root: Path, derived: tuple = DERIVED) -> list:
    """Return one diagnostic per finding; an empty list is a pass."""
    bad = derived_findings(root, derived)
    inv = root / INVENTORY
    if not inv.is_file():
        return bad + [f"INVENTORY MISSING: {INVENTORY}"]
    rows = {}
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
        return bad + [f"INVENTORY EMPTY: {INVENTORY} has no file rows"]
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
        text = (root / path).read_text(encoding="utf-8")
        bad += restated_uisa(path, text, status in ("superseded", "obsolete"))
        head = "\n".join(text.splitlines()[:BANNER_LINES])
        bannered = bool(BANNER.search(head))
        if status in ("superseded", "obsolete") and not bannered:
            bad.append(f"NO BANNER: {path} is {status} but no quoted line of its first "
                       f"{BANNER_LINES} says superseded or obsolete")
        if status in ("current", "generated") and bannered:
            bad.append(f"BANNERED: {path} is {status} but a quoted line of its first "
                       f"{BANNER_LINES} calls it superseded or obsolete")
    return bad


#: The self-test's fixture: three pages, one retired under its banner, and their rows.
FIXTURE_PAGES = {
    "README.md": "# Start\n\nWhat this is.\n",
    "docs/guide.md": "# Guide\n\nHow to use it.\n",
    "docs/old.md": "# Old\n\n> **Historical, superseded** by [the guide](guide.md).\n",
}
GUIDE_ROW = "| [`docs/guide.md`](guide.md) | use | integrator | the top | current | read |"
FIXTURE_ROWS = [
    "| [`README.md`](../README.md) | start | new reader | the repository | current | read |",
    GUIDE_ROW,
    "| [`docs/old.md`](old.md) | history | author | nothing | superseded | banner |",
    "| [`docs/INVENTORY.md`](INVENTORY.md) | this | author | every page | current | gate |",
]
FIXTURE_HEAD = ("# Inventory\n\n| File | Purpose | Audience | Describes | Status | Evidence |\n"
                "|---|---|---|---|---|---|\n")


def _inventory(rows: list) -> str:
    """The fixture inventory page holding `rows`."""
    return FIXTURE_HEAD + "".join(r + "\n" for r in rows)


def _swap(old: str, new: str) -> str:
    """The fixture inventory with row `old` replaced by `new`."""
    return _inventory([new if r == old else r for r in FIXTURE_ROWS])


#: The derived checks' fixtures: an operation enum, the µISA table that matches it, a
#: `check` rule and the §7 table that matches it.
FIX_PKG = ("  typedef enum logic [4:0] {\n    OP_NOP    = 5'd0,\n"
           "    OP_BR_ST  = 5'd1,   // BRANCH_IF_STATUS\n    OP_SHR    = 5'd2    // logical shift\n"
           "  } ucpu_op_e;\n")
FIX_06 = ("**µISA (3 operations)** — the set\n\n| Group | Ops | Notes |\n|---|---|---|\n"
          "| Flow | `NOP`, `BRANCH_IF_STATUS` | |\n| Data | `SHR` | `imm` |\n\nAfter.\n")
FIX_MK = "check: lint links\n\nlint:\n\t@true\n"
FIX_09 = ("## 7. Gates\n\n| Target | Script | Asserts |\n|---|---|---|\n| `lint` | a | b |\n"
          "| `links` | c | d |\n\n## 8. Next\n\n| `other` | e | f |\n")


def _plant(inv: str | None = None, page: str | None = None, page_text: str = "",
           extra: str | None = None, with_inventory: bool = True, derived: tuple = ()) -> list:
    """Build the fixture in a scratch repository with one fault planted; return its findings."""
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        subprocess.run(["git", "init", "-q", str(t)], check=True)
        for p, body in FIXTURE_PAGES.items():
            (t / p).parent.mkdir(parents=True, exist_ok=True)
            (t / p).write_text(body, encoding="utf-8")
        if with_inventory:
            text = inv if inv is not None else _inventory(FIXTURE_ROWS)
            (t / INVENTORY).write_text(text, encoding="utf-8")
        if page:
            (t / page).write_text(page_text, encoding="utf-8")
        if extra:
            (t / extra).parent.mkdir(parents=True, exist_ok=True)
            (t / extra).write_text("# new page\n", encoding="utf-8")
        return check(t, derived)


def selftest() -> int:
    """Plant each fault in a small fixture repository and see it named.

    The fixture is the self-test's own, so it grades the checker and not this
    tree: a fault in the real inventory is the main check's to name.
    """
    gone = GUIDE_ROW.replace("guide.md", "gone.md")
    cases = [
        ("clean fixture", _plant(), None),
        ("row dropped", _plant(inv=_inventory([r for r in FIXTURE_ROWS if r != GUIDE_ROW])),
         "NO ROW: docs/guide.md"),
        ("new page without a row", _plant(extra="tb/x/README.md"), "NO ROW: tb/x/README.md"),
        ("row for a missing file", _plant(inv=_inventory(FIXTURE_ROWS + [gone])), "NO FILE: docs/gone.md"),
        ("duplicate row", _plant(inv=_inventory(FIXTURE_ROWS + [GUIDE_ROW])), "DUPLICATE ROW: docs/guide.md"),
        ("unknown status", _plant(inv=_swap(GUIDE_ROW, GUIDE_ROW.replace("| current |", "| fresh |"))),
         "BAD STATUS: docs/guide.md"),
        ("empty evidence", _plant(inv=_swap(GUIDE_ROW, GUIDE_ROW.replace("| read |", "| |"))),
         "NO EVIDENCE: docs/guide.md"),
        ("cell dropped", _plant(inv=_swap(GUIDE_ROW, GUIDE_ROW.replace(" read |", ""))),
         "MALFORMED ROW: docs/guide.md"),
        ("superseded page lost its banner",
         _plant(page="docs/old.md", page_text="# Old\n\nIt describes the tree.\n"), "NO BANNER: docs/old.md"),
        ("current page carries a banner",
         _plant(page="docs/guide.md", page_text="# Guide\n\n> Superseded by another page.\n"),
         "BANNERED: docs/guide.md"),
        ("current page restates the µISA count",
         _plant(page="docs/guide.md", page_text="# Guide\n\nThe µCPU runs a 3-operation ISA.\n"),
         "RESTATED: docs/guide.md:3"),
        ("retired page's banner restates the µISA count",
         _plant(page="docs/old.md", page_text="# Old\n\n> **Superseded.** The tree has a 3-operation ISA.\n"),
         "RESTATED: docs/old.md:3"),
        ("retired page's body keeps its historical count",
         _plant(page="docs/old.md", page_text="# Old\n\n> **Superseded.**\n\nIts µISA (2 operations).\n"), None),
        ("inventory emptied", _plant(inv="# Inventory\n"), "INVENTORY EMPTY"),
        ("inventory deleted", _plant(with_inventory=False), "INVENTORY MISSING"),
        ("µISA clean fixture", uisa_findings(FIX_06, FIX_PKG), None),
        ("µISA count stale", uisa_findings(FIX_06.replace("(3 ", "(2 "), FIX_PKG), "says 2 operations"),
        ("operation missing from the table",
         uisa_findings(FIX_06.replace(", `BRANCH_IF_STATUS`", ""), FIX_PKG), "table lacks BRANCH_IF_STATUS"),
        ("table names an undeclared operation",
         uisa_findings(FIX_06.replace("`SHR`", "`SHR`, `ROT`"), FIX_PKG), "table names ROT"),
        ("operation listed twice", uisa_findings(FIX_06.replace("`SHR`", "`SHR`, `NOP`"), FIX_PKG),
         "table names NOP twice"),
        ("enum member in another form", uisa_findings(FIX_06, FIX_PKG.replace("5'd2", "5'h2")),
         "unparsed enum member"),
        ("no µISA heading", uisa_findings(FIX_06.replace("**µISA", "µISA"), FIX_PKG), "no '**µISA"),
        ("no operation enum", uisa_findings(FIX_06, FIX_PKG.replace("ucpu_op_e", "op_e")),
         "no operation enum"),
        ("06's derived heading is not a restatement", restated_uisa(str(DERIVED[0][0]), FIX_06, False), None),
        ("06 restates the count beside its heading",
         restated_uisa(str(DERIVED[0][0]), FIX_06.replace("** —", "** (an ISA of 3 ops) —"), False),
         "RESTATED: docs/architecture/06_aecp_engine.md:1"),
        ("targets clean fixture", target_findings(FIX_09, FIX_MK), None),
        ("check runs a target the table lacks", target_findings(FIX_09, FIX_MK.replace("links", "links stale")),
         "table lacks `stale`"),
        ("table names a target check does not run", target_findings(FIX_09, FIX_MK.replace(" links", "")),
         "table names `links`"),
        ("no check rule", target_findings(FIX_09, FIX_MK.replace("check:", "all:")), "no `check:` rule"),
        ("defining file missing", _plant(derived=DERIVED), "DERIVED MISSING"),
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


def main() -> int:
    """Run the gate on this tree, or with `--selftest` grade it on its fixture; 1 on a finding."""
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
