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
(`T-BUDGET-AECP-TYP / -WC`). A use may name a family instead of one ID:
`T-MRP-*` (and `T-ADP-` broken across a line) needs a row in that family, and
`T-MRP-{JOIN, LEAVEALL}` needs each member. `P-RX-SLOTS-1` reads as
`P-RX-SLOTS` minus one when no row carries the numeric segment. `P-ID` and
`T-ID` are the registry's column names, not IDs.

The files scanned are those git tracks plus untracked ones it does not ignore,
so a new file is held before it is added and build output never is.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ("docs", "hdl", "tb")
PARAMS = Path("docs/architecture/01_overview.md")
TIMING = Path("docs/architecture/08_timing.md")
# a P- or T- ID that is not the tail of a longer hyphenated word (MAAP-..., gPTP-...)
ID = re.compile(r"(?<![A-Za-z0-9_-])([PT]-[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*)")
REGISTRY_WORDS = {"P-ID", "T-ID"}
ROW = re.compile(r"^\|([^|\n]*)\|", re.M)
SIBLING = re.compile(r"/\s*-([A-Z][A-Z0-9]*)")
BRACES = re.compile(r"-\{([A-Z0-9, ]+)\}")


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


def uses(text: str):
    """(line, token, kind) per ID use; kind is 'id', 'family' or a brace list."""
    for match in ID.finditer(text):
        token, end = match.group(1), match.end()
        line = text.count("\n", 0, match.start()) + 1
        if token in REGISTRY_WORDS:
            continue
        braces = BRACES.match(text, end)
        if braces:
            for member in braces.group(1).split(","):
                yield line, f"{token}-{member.strip()}", "id"
        elif text.startswith("-", end):
            yield line, token, "family"
        else:
            yield line, token, "id"


def resolves(token: str, kind: str, rows: set) -> bool:
    """True when the use names a row, a family with a row, or a row minus n."""
    if kind == "family":
        return token in rows or any(r.startswith(token + "-") for r in rows)
    if token in rows:
        return True
    stem, _, tail = token.rpartition("-")
    return tail.isdigit() and stem in rows


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
            if not resolves(token, kind, set(rows)):
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
| T-BUDGET-AECP-TYP / -WC | 20 / 100 ms |
"""
# (file text, expected unresolved tokens): the forms the tree uses, then strays
SELFTEST_CASES = (
    ("P-ONE, P-RX-SLOT-BYTES, P-EN-B, T-BUDGET-AECP-WC; the P-ID and T-IDs", []),
    ("T-MRP-* and T-MRP-{JOIN, LEAVEALL} and T-MRP-{JOIN,LEAVEALL}", []),
    ("(T-MRP-\n//  JOIN) and 0..P-RX-SLOTS-1, MAAP-ANNOUNCE, gPTP-T-X", []),
    ("a stray P-NOT-A-ROW in a document", ["P-NOT-A-ROW"]),
    ("// a stray T-NOT-A-ROW in a module comment", ["T-NOT-A-ROW"]),
    ("T-MRP-{JOIN, NOPE} names one missing member", ["T-MRP-NOPE"]),
    ("T-NOFAMILY-* names a family with no row", ["T-NOFAMILY-*"]),
    ("P-RX-SLOT-2 is not P-RX-SLOTS minus two", ["P-RX-SLOT-2"]),
    ("P-EN-C is no sibling of P-EN-A / P-EN-B", ["P-EN-C"]),
)


def selftest() -> int:
    """Run the gate over planted trees: every stray caught, every form passed."""
    failures = 0
    for number, (text, expect) in enumerate(SELFTEST_CASES, start=1):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            for rel, body in ((PARAMS, SELFTEST_PARAMS),
                              (TIMING, SELFTEST_TIMING),
                              (Path("tb/case/README.md"), text)):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(body, encoding="utf-8")
            got = []
            proc = subprocess.run([sys.executable, __file__, "--root", str(root)],
                                  capture_output=True, text=True)
            for line in proc.stdout.splitlines():
                m = re.match(r"ID FAIL: tb/case/README\.md:\d+: (\S+) has no", line)
                if m:
                    got.append(m.group(1))
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
