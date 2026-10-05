#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Fail on any P- or T- ID used under docs/, hdl/ or tb/ that has no master row.

docs/README.md section 2 gives every synthesis-time parameter one row in F01.5
(docs/architecture/01_overview.md section 7) and every timing constant one row
in F08.1 (docs/architecture/08_timing.md section 2). This gate holds the ID half
of that rule: an ID a document, a module or a suite cites must be one of those
rows. Whether a value is copied outside its table is not checked here.

The first cell of each master-table row defines its IDs. A cell may list several
(`P-EN-MVU-SUID / P-EN-MVU-MCR`) or abbreviate a sibling by its last segment
(`T-BUDGET-AECP-TYP / -WC`). A use is read the same way, and these forms are
the only ones that reach past the ID's last segment:

- `T-MRP-*` names a family and needs a row in it;
- `T-NVM-{RS-DEADLINE, RS-AGGREGATE}` needs a row for each member, and braces
  holding anything but uppercase segments fail;
- `T-ADP-` at the end of a line continues with the next line's first word,
  after any comment leader (`T-ADP-DELAY-START`);
- `/ -WC` after an ID names the sibling with that last segment;
- `T-ADP-DELAY(-START)` names the ID and the ID with the optional segment,
  and a parenthesis after an ID that opens with a hyphen but holds anything
  but uppercase segments fails.

Anything else after a hyphen is prose: `P-TX-shaped` uses `P-TX`.
`P-RX-SLOTS-1` reads as `P-RX-SLOTS` minus one when no row carries the `-1`.
`P-ID` and `T-ID` are the registry's column names, not IDs.

The files scanned are those git tracks plus untracked ones it does not ignore,
so a new file is held before it is added and build output never is.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ("docs", "hdl", "tb")
PARAMS = Path("docs/architecture/01_overview.md")
TIMING = Path("docs/architecture/08_timing.md")
# a P- or T- ID that is not the tail of a longer hyphenated word (MAAP-..., gPTP-...)
ID = re.compile(r"(?<![A-Za-z0-9_-])([PT]-[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*)")
REGISTRY_WORDS = {"P-ID", "T-ID"}
ROW = re.compile(r"^\|([^|\n]*)\|", re.M)
SIBLING = re.compile(r"[ \t]*/[ \t]*-([A-Z][A-Z0-9]*)")
BRACES = re.compile(r"-\{([^{}\n]*)\}")
SEGMENTS = re.compile(r"[A-Z0-9]+(?:-[A-Z0-9]+)*")
# a line break inside an ID: the next line's text resumes after any comment leader
NEXT_LINE = r"[ \t]*\r?\n[ \t]*(?:(?://+|#+|--|\*|;+|>)[ \t]*)?"
LINE_BREAK = re.compile(r"-" + NEXT_LINE + r"([A-Z0-9]+(?:-[A-Z0-9]+)*)(?![A-Za-z0-9_])")
OPTIONAL = re.compile(r"\(-(?:" + NEXT_LINE + r")?([A-Z0-9]+(?:-[A-Z0-9]+)*)\)")


def table_section(body: str, start: str, what: str) -> str:
    """The text from `start` to the end of the first table after it."""
    at = body.find(start)
    if at < 0:
        raise ValueError(f"cannot find {what} ({start!r})")
    lines, in_table = [], False
    for line in body[at:].splitlines()[1:]:
        if line.startswith("|"):
            in_table = True
            lines.append(line)
        elif in_table:
            break
    if not lines:
        raise ValueError(f"{what} has no table")
    return "\n".join(lines)


def defined_ids(table: str, prefix: str, what: str) -> dict:
    """ID -> 1-based table row for every ID the first column defines."""
    rows, problems = {}, []
    for number, cell in enumerate(ROW.findall(table), start=1):
        if set(cell.strip()) <= set("-: "):
            continue
        names = [m for m in ID.findall(cell) if m.startswith(prefix)]
        if names:
            stem = names[0].rsplit("-", 1)[0]
            names += [f"{stem}-{tail}" for tail in SIBLING.findall(cell)]
        for name in names:
            if name in REGISTRY_WORDS:
                continue
            if name in rows:
                problems.append(f"{what}: {name} has two rows")
            rows[name] = number
    if not rows:
        problems.append(f"{what}: no {prefix} rows parsed")
    if problems:
        raise ValueError("; ".join(problems))
    return rows


def scanned_files(root: Path) -> list:
    """Tracked and untracked-but-not-ignored files under the scanned trees."""
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
         "--exclude-standard", "--", *SCAN_DIRS],
        check=True, capture_output=True).stdout.decode("utf-8")
    names = sorted({n for n in out.split("\0") if n})
    return [Path(n) for n in names if (root / n).is_file()]


def uses(text: str) -> Iterator[tuple[int, str, str]]:
    """(line, token, kind) per ID use; kind is 'id', 'family' or 'list' (a bad list)."""
    for match in ID.finditer(text):
        token, end = match.group(1), match.end()
        line = text.count("\n", 0, match.start()) + 1
        if token in REGISTRY_WORDS:
            continue
        # Resolve the continuation before interpreting any suffix on its last word.
        # Otherwise a wrapped optional member is lost behind the original match.
        broken = LINE_BREAK.match(text, end)
        while broken:
            token, end = f"{token}-{broken.group(1)}", broken.end()
            broken = LINE_BREAK.match(text, end)
        if text.startswith("-*", end):
            yield line, token, "family"
        elif text.startswith("-{", end):
            braces = BRACES.match(text, end)
            members = [m.strip() for m in braces.group(1).split(",")] if braces else [""]
            if all(SEGMENTS.fullmatch(m) for m in members):
                for member in members:
                    yield line, f"{token}-{member}", "id"
            else:
                yield line, f"{token}-{{...}}", "list"
        else:
            yield line, token, "id"
            sibling = SIBLING.match(text, end)
            if sibling:
                yield line, f"{token.rsplit('-', 1)[0]}-{sibling.group(1)}", "id"
            optional = OPTIONAL.match(text, end)
            if optional:
                yield line, f"{token}-{optional.group(1)}", "id"
            elif text.startswith("(-", end):
                yield line, f"{token}(-...)", "list"


def resolves(token: str, kind: str, rows: set) -> bool:
    """True when the use names a row, a family with a row, or a row minus one."""
    if kind == "family":
        return token in rows or any(r.startswith(token + "-") for r in rows)
    return token in rows or (token.endswith("-1") and token[:-2] in rows)


def check(root: Path) -> int:
    """Print one line per unresolved use and a summary; 1 on any finding."""
    try:
        params = defined_ids(table_section(
            (root / PARAMS).read_text(encoding="utf-8"),
            "## 7. Parameter master table (F01.5)", "F01.5"), "P-", "F01.5")
        timing = defined_ids(table_section(
            (root / TIMING).read_text(encoding="utf-8"),
            '<a id="fig-08-constants"></a>', "F08.1"), "T-", "F08.1")
    except (OSError, ValueError) as exc:
        print(f"ID FAIL: {exc}")
        print("ids: master tables unreadable, FAILURES")
        return 1
    problems, used, files = [], set(), scanned_files(root)
    for rel in files:
        data = (root / rel).read_bytes()
        if b"\0" in data[:8192]:
            continue
        for line, token, kind in uses(data.decode("utf-8", errors="replace")):
            rows = params if token.startswith("P-") else timing
            table = "F01.5" if token.startswith("P-") else "F08.1"
            used.add(token)
            if kind == "list":
                problems.append(f"{rel}:{line}: {token} is not a list of ID segments")
            elif not resolves(token, kind, set(rows)):
                shown = f"{token}-*" if kind == "family" else token
                problems.append(f"{rel}:{line}: {shown} has no {table} row")
    for line in problems:
        print(f"ID FAIL: {line}")
    print(f"ids: {len(files)} files, {len(used)} distinct IDs used, "
          f"F01.5 {len(params)} P-IDs, F08.1 {len(timing)} T-IDs, "
          f"{'OK' if not problems else str(len(problems)) + ' FAILURES'}")
    return 1 if problems else 0


SELFTEST_PARAMS = """## 7. Parameter master table (F01.5)

| P-ID | Default |
|---|---|
| P-ONE | 1 |
| P-RX-SLOTS × P-RX-SLOT-BYTES | 4 × 576 |
| P-EN-A / P-EN-B | 0 / 0 |

## 8. Next
"""
SELFTEST_TIMING = """<a id="fig-08-constants"></a>**F08.1**

| T-ID | Value |
|---|---|
| T-MRP-JOIN | 200 ms |
| T-MRP-LEAVEALL | 10 s |
| T-NVM-RS-DEADLINE | 20 ms |
| T-ADP-DELAY | 0-4 s |
| T-ADP-DELAY-START | 0-2 s |
| T-BUDGET-AECP-TYP / -WC | 20 / 100 ms |
"""
CASE = "tb/case/README.md"
# (files planted beside the two master pages, expected findings: the token of
# each unresolved use, else the message): the forms the tree uses, a stray in
# each scanned tree, each abbreviated form, then the master-table faults
SELFTEST_CASES = (
    ({CASE: "P-ONE, P-RX-SLOT-BYTES, P-EN-B, T-BUDGET-AECP-WC; the P-ID and T-IDs"}, []),
    ({CASE: "T-MRP-* and T-MRP-{JOIN, LEAVEALL} and T-MRP-{JOIN,LEAVEALL}"}, []),
    ({CASE: "(T-MRP-\n//  JOIN) and 0..P-RX-SLOTS-1, MAAP-ANNOUNCE, gPTP-T-X"}, []),
    ({CASE: "T-NVM-{RS-DEADLINE}, a P-ONE-based bound, T-BUDGET-AECP-TYP / -WC"}, []),
    ({CASE: "T-ADP-DELAY(-START), (T-ADP-DELAY(-\n//  START)); the label T-MRP-JOIN(B)"}, []),
    ({CASE: "T-ADP-\n// DELAY(-START), T-ADP-\n// DELAY(-\n// START), "
            "T-ADP-\n// DELAY-\n// START"}, []),
    ({CASE: "P-MISSING-1 has no base row"}, ["P-MISSING-1"]),
    ({CASE: "T-ADP-DELAY(-\n// STRT) has no optional member row"}, ["T-ADP-DELAY-STRT"]),
    ({CASE: "T-ADP-\n// DELAY(-STRT) has no optional member row"}, ["T-ADP-DELAY-STRT"]),
    ({CASE: "T-ADP-\n// DELAY(-\n// STRT) has no optional member row"}, ["T-ADP-DELAY-STRT"]),
    ({"docs/case.md": "a stray P-NOT-A-ROW in a document"}, ["P-NOT-A-ROW"]),
    ({"hdl/case.sv": "// a stray T-NOT-A-ROW in a module comment"}, ["T-NOT-A-ROW"]),
    ({CASE: "a stray T-NOT-A-ROW in a suite"}, ["T-NOT-A-ROW"]),
    ({CASE: "T-MRP-{JOIN, NOPE} names one missing member"}, ["T-MRP-NOPE"]),
    ({CASE: "T-NVM-{RS-DEADLINE, RS-TYPO} names one missing member"}, ["T-NVM-RS-TYPO"]),
    ({CASE: "T-MRP-{join} is not a list of IDs"}, ["T-MRP-{...}"]),
    ({CASE: "the P-RX-shaped pool is no family use"}, ["P-RX"]),
    ({CASE: "(T-MRP-\n//  NOPE) continues on the next line"}, ["T-MRP-NOPE"]),
    ({CASE: "T-BUDGET-AECP-TYP / -XX names a sibling with no row"}, ["T-BUDGET-AECP-XX"]),
    ({CASE: "T-ADP-DELAY(-STRT) names an optional segment with no row"}, ["T-ADP-DELAY-STRT"]),
    ({CASE: "T-ADP-DELAY(-start) is not an ID segment"}, ["T-ADP-DELAY(-...)"]),
    ({CASE: "T-NOFAMILY-* names a family with no row"}, ["T-NOFAMILY-*"]),
    ({CASE: "P-RX-SLOT-2 is not P-RX-SLOTS minus two"}, ["P-RX-SLOT-2"]),
    ({CASE: "P-RX-SLOTS-2 is not P-RX-SLOTS minus one"}, ["P-RX-SLOTS-2"]),
    ({CASE: "P-ONE-X is no row under P-ONE"}, ["P-ONE-X"]),
    ({CASE: "P-EN-C is no sibling of P-EN-A / P-EN-B"}, ["P-EN-C"]),
    ({str(PARAMS): SELFTEST_PARAMS + "\n| P-ID |\n|---|\n| P-LATER |\n"}, ["P-LATER"]),
    ({str(PARAMS): SELFTEST_PARAMS.replace("| P-ONE | 1 |", "| P-ONE | 1 |\n| P-ONE | 2 |")},
     ["F01.5: P-ONE has two rows"]),
    ({str(TIMING): SELFTEST_TIMING.split("|---|---|")[0] + "|---|---|\n"},
     ["F08.1: no T- rows parsed"]),
    ({str(PARAMS): SELFTEST_PARAMS.replace(" (F01.5)", "")},
     ["cannot find F01.5 ('## 7. Parameter master table (F01.5)')"]),
)
USE_FAIL = re.compile(r"ID FAIL: \S+:\d+: (\S+) ")


def findings(stdout: str) -> list:
    """Per `ID FAIL` line, the token of an unresolved use or else the message."""
    out = []
    for line in stdout.splitlines():
        if line.startswith("ID FAIL: "):
            m = USE_FAIL.match(line)
            out.append(m.group(1) if m else line[len("ID FAIL: "):])
    return out


def selftest() -> int:
    """Run the gate over planted trees: every stray caught, every form passed."""
    failures = 0
    for number, (files, expect) in enumerate(SELFTEST_CASES, start=1):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            for rel, body in {str(PARAMS): SELFTEST_PARAMS,
                              str(TIMING): SELFTEST_TIMING, **files}.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(body, encoding="utf-8")
            proc = subprocess.run([sys.executable, __file__, "--root", str(root)],
                                  capture_output=True, text=True)
            got = findings(proc.stdout)
            want_rc = 1 if expect else 0
            if got != expect or proc.returncode != want_rc:
                failures += 1
                print(f"SELFTEST FAIL: case {number}: want {expect} rc {want_rc}, "
                      f"got {got} rc {proc.returncode}\n{proc.stdout}")
    print(f"ids selftest: {len(SELFTEST_CASES)} cases, "
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
