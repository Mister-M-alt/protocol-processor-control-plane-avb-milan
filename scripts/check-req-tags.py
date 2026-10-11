#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Requirement-to-test traceability: every REQ row is traced, waived, or needs no check.

docs/architecture/09_verification.md section 2: every row's Ver category expands
to at least one tagged check, and an uncovered REQ-ID is a failure. This gate
holds that rule over the compliance matrix (docs/00_MILAN_COMPLIANCE_REVIEW.md
section 6) and the suites under tb/.

Tags. A check carries one statement per row it verifies, on its own line
directly above it (tb/common/req_tag.hpp; tb/common/req_tag.py for a Python
test, at the head of the test method):

    REQ_TAG("REQ-NOT-001", "STORM", "ST1b");

It names the row, the Ver category of the check it labels, and the check's name.
In C++ the name must appear in the check statement that follows the tags; in
Python it must be Class.method of the test the tag heads. Mentions of a REQ-ID
in prose are not tags.

Rows. DIR, MTXW, TOL, TIM, RND, STORM and NVM rows need a check; lint (a CI
gate, 09 section 7) and an em dash (no dynamic verification) need none. A row
that needs a check is traced by a tag of its own Ver category. A tag of another
category labels its check and traces nothing. A row with no such tag needs a
waiver in 09 section 8.10 naming one of the row's own Finding GAPs and a reason;
a waiver on a traced row is stale. Either failure fails the gate.

Records. Each row's Cov cell ends with its state ("traced", "waived GAP-nn",
"no check"), and the 09 section 8.10 coverage table lists the traced and the
no-check rows per Ver category. Both must equal what the gate finds.

The two halves. Without --suites, the static half above runs; `make check` runs
it, and needs no simulator. With --suites LOG --evidence FILE the executed half
runs as well, on one run of scripts/run_suites.sh made with REQ_TAG_LOG=FILE and
its output saved as LOG. Every tag's suite must PASS in LOG, and the tag's
evidence line must be in FILE, so a tag on a check that never runs fails by name.
An evidence line that no tag accounts for fails as stray (a stale file).

--table prints each row's REQ, Ver, state, file:line and check name, in the
order a reviewer samples them. --selftest plants each fault in a scratch tree
and requires the gate to name it.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parent.parent
REVIEW = Path("docs/00_MILAN_COMPLIANCE_REVIEW.md")
VERIFY = Path("docs/architecture/09_verification.md")
NEEDS_CHECK = ("DIR", "MTXW", "TOL", "TIM", "RND", "STORM", "NVM")
NO_CHECK = ("lint", "—")
SOURCES = (".cpp", ".hpp", ".h", ".py")
DEFINITIONS = ("tb/common/req_tag.hpp", "tb/common/req_tag.py")
REQ_ROW = re.compile(r"^\|\s*(REQ-[A-Z]+-\d{3})\s*\|(.*)$", re.M)
GAP_REF = re.compile(r"\(#gap-(\d+)\)")
GAP_ID = re.compile(r"\bGAP-(\d{2})\b")
REQ_ID = re.compile(r"\bREQ-[A-Z]+-\d{3}\b")
TAG = re.compile(r'^(\s*)REQ_TAG\("(REQ-[A-Z]+-\d{3})", "([A-Z]+)", "([^"\\]+)"\);?\s*$')
COV = re.compile(r"^([CPAI])(?:; (.*))?$")
EVIDENCE = re.compile(r"^(\S+) (\S+):(\d+) (REQ-[A-Z]+-\d{3}) ([A-Z]+)$")
SUITE_PASS = re.compile(r"^PASS (\S+) \(")
SUITES_DONE = re.compile(r"^suites: \d+ checks total, 0 failing$", re.M)
WAIVER_ANCHOR = '<a id="req-waivers"></a>'
WAIVER_HEADER = "| REQ | Ver | GAP | Reason |"
RECORD_ANCHOR = '<a id="req-coverage"></a>'
RECORD_HEADER = "| State | Ver | Rows |"
#: lines after the tags that may hold a C++ check's statement
CHECK_LINES = 8


class Row(NamedTuple):
    """One compliance-matrix row: its Ver, Cov cell and Finding GAPs."""
    ver: str
    cov: str
    gaps: tuple[str, ...]


class Tag(NamedTuple):
    """One tag statement and where it stands."""
    req: str
    cat: str
    name: str
    path: str
    line: int

    @property
    def suite(self) -> str:
        """The suite directory the tag is in: tb/<suite>/..."""
        return self.path.split("/")[1]

    @property
    def key(self) -> tuple[str, str, int]:
        """The (suite, file, line) its evidence line names."""
        return self.suite, self.path.rsplit("/", 1)[-1], self.line

    @property
    def where(self) -> str:
        """file:line, as the findings and the table print it."""
        return f"{self.path}:{self.line}"


class Waiver(NamedTuple):
    """One row of the 09 section 8.10 waiver table."""
    req: str
    ver: str
    gap: str
    reason: str


def read_rows(review: str) -> dict[str, Row]:
    """Every REQ row of the matrix, by REQ-ID."""
    rows = {}
    for req, rest in REQ_ROW.findall(review):
        cols = [c.strip() for c in rest.split("|")]
        if len(cols) >= 8:
            gaps = tuple(f"GAP-{int(n):02d}" for n in GAP_REF.findall(cols[4]))
            rows[req] = Row(cols[7], cols[3], gaps)
    return rows


def table_after(body: str, anchor: str, header: str,
                problems: list[str]) -> list[list[str]] | None:
    """The cells of each row of the table that follows `anchor`; None if absent."""
    at = body.find(anchor)
    if at < 0:
        problems.append(f"NO TABLE {anchor}: {VERIFY} lacks it")
        return None
    lines = body[at:].splitlines()[1:]
    while lines and not lines[0].startswith("|"):
        lines.pop(0)
    if not lines or lines[0].strip() != header:
        problems.append(f"NO TABLE {anchor}: the table after it must open with '{header}'")
        return None
    rows = []
    for line in lines[2:]:
        if not line.startswith("|"):
            break
        rows.append([c.strip() for c in line.strip().strip("|").split("|")])
    return rows


def read_waivers(verify: str, rows: dict[str, Row], problems: list[str]) -> dict[str, Waiver]:
    """The waiver table, every waiver held to its row; findings go to `problems`."""
    waivers: dict[str, Waiver] = {}
    for cells in table_after(verify, WAIVER_ANCHOR, WAIVER_HEADER, problems) or []:
        cells += [""] * (4 - len(cells))
        req, ver, gap_cell, reason = cells[:4]
        gaps = [f"GAP-{n}" for n in GAP_ID.findall(gap_cell)]
        row = rows.get(req)
        if row is None:
            problems.append(f"UNKNOWN REQ {req or '(empty)'}: a waiver names no matrix row")
            continue
        if req in waivers:
            problems.append(f"DUPLICATE WAIVER {req}: the row is waived twice")
            continue
        if not gaps:
            problems.append(f"WAIVER WITHOUT GAP {req}: the waiver names no GAP")
        elif len(gaps) > 1 or gaps[0] not in row.gaps:
            problems.append(f"WAIVER NAMES ANOTHER ROW'S GAP {req}: '{gap_cell}', "
                            f"the row's own: {', '.join(row.gaps) or 'none'}")
        if not reason:
            problems.append(f"WAIVER WITHOUT REASON {req}: the waiver gives no reason")
        if ver != row.ver:
            problems.append(f"WAIVER VER {req}: the waiver says '{ver}', the row '{row.ver}'")
        if row.ver not in NEEDS_CHECK:
            problems.append(f"WAIVER ON A ROW NEEDING NO CHECK {req}: Ver '{row.ver}'")
        waivers[req] = Waiver(req, ver, gaps[0] if gaps else "", reason)
    return waivers


def source_files(root: Path) -> list[str]:
    """The tb/ sources git tracks, plus untracked ones it does not ignore."""
    run = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
                          "--exclude-standard", "--", "tb"],
                         capture_output=True, text=True, check=True)
    return sorted({p for p in run.stdout.split("\0")
                   if p.endswith(SOURCES) and p not in DEFINITIONS and (root / p).is_file()})


def enclosing(lines: list[str], at: int, indent: int, keyword: str) -> tuple[str, int]:
    """The name and indentation of the nearest `keyword` block above line `at`
    that is indented less than `indent`."""
    for line in reversed(lines[:at]):
        stripped = line.lstrip()
        depth = len(line) - len(stripped)
        if stripped and depth < indent:
            if stripped.startswith(keyword + " "):
                return re.split(r"[\s(:]", stripped[len(keyword) + 1:], maxsplit=1)[0], depth
            indent = depth
    return "", 0


def name_at_check(path: str, lines: list[str], at: int, name: str) -> bool:
    """Whether a tag on line index `at` names the check it stands above."""
    if path.endswith(".py"):
        indent = len(lines[at]) - len(lines[at].lstrip())
        method, depth = enclosing(lines, at, indent, "def")
        owner, _ = enclosing(lines, at, depth, "class")
        return bool(method) and name == f"{owner}.{method}"
    nxt = at + 1
    while nxt < len(lines) and TAG.match(lines[nxt]):
        nxt += 1
    text = []
    for line in lines[nxt:nxt + CHECK_LINES]:
        text.append(line)
        if line.rstrip().endswith(";"):
            break
    return name in "\n".join(text)


def read_tags(root: Path, rows: dict[str, Row], problems: list[str]) -> list[Tag]:
    """Every tag under tb/, each held to the matrix and to its check."""
    tags: list[Tag] = []
    for path in source_files(root):
        lines = (root / path).read_text(encoding="utf-8", errors="replace").splitlines()
        for at, line in enumerate(lines):
            if "REQ_TAG(" not in line:
                continue
            match = TAG.match(line)
            where = f"{path}:{at + 1}"
            if not match:
                problems.append(f"MALFORMED TAG {where}: a tag is one statement on its own line, "
                                'REQ_TAG("REQ-AREA-nnn", "CAT", "check name")')
                continue
            tag = Tag(match[2], match[3], match[4], path, at + 1)
            if path.count("/") < 2 or tag.suite == "common":
                problems.append(f"TAG OUTSIDE A SUITE {where}: tags live in tb/<suite>/")
            if tag.req not in rows:
                problems.append(f"UNKNOWN REQ {tag.req} {where}: no matrix row has that ID")
            if tag.cat not in NEEDS_CHECK:
                problems.append(f"UNKNOWN CATEGORY {tag.cat} {where}: one of {' '.join(NEEDS_CHECK)}")
            if not name_at_check(path, lines, at, tag.name):
                problems.append(f'NAME NOT AT CHECK {where}: "{tag.name}" is not in the check '
                                "it stands above")
            if any(t[:4] == tag[:4] for t in tags):
                problems.append(f"DUPLICATE TAG {tag.req} {where}: the same tag twice")
            tags.append(tag)
    return tags


def row_states(rows: dict[str, Row], tags: list[Tag], waivers: dict[str, Waiver],
               problems: list[str]) -> dict[str, str]:
    """Each row's state: traced, waived GAP-nn, no check; or a finding."""
    states = {}
    for req, row in rows.items():
        own = any(t.req == req and t.cat == row.ver for t in tags)
        if row.ver in NO_CHECK:
            states[req] = "no check"
        elif row.ver not in NEEDS_CHECK:
            problems.append(f"UNKNOWN VER {req}: '{row.ver}'")
        elif own:
            states[req] = "traced"
            if req in waivers:
                problems.append(f"STALE WAIVER {req}: the row has a tagged {row.ver} check")
        elif req in waivers:
            if waivers[req].gap:  # a waiver naming no GAP is already a finding
                states[req] = f"waived {waivers[req].gap}"
        else:
            problems.append(f"UNCOVERED {req} ({row.ver}): no tagged {row.ver} check and no waiver")
    return states


def check_records(rows: dict[str, Row], states: dict[str, str], verify: str,
                  problems: list[str]) -> None:
    """The Cov column and the 09 section 8.10 coverage table against `states`."""
    for req, row in rows.items():
        match = COV.match(row.cov)
        want = states.get(req)
        if want and (not match or match[2] != want):
            problems.append(f"COV RECORD {req}: Cov reads '{row.cov}', the gate finds "
                            f"'{row.cov[:1]}; {want}'")
    table = table_after(verify, RECORD_ANCHOR, RECORD_HEADER, problems)
    if table is None:
        return
    listed: dict[tuple[str, str], set[str]] = {}
    for cells in table:
        cells += [""] * (3 - len(cells))
        listed.setdefault((cells[0], cells[1]), set()).update(REQ_ID.findall(cells[2]))
    found: dict[tuple[str, str], set[str]] = {}
    for req, state in states.items():
        if not state.startswith("waived"):
            found.setdefault((state, rows[req].ver), set()).add(req)
    failing = set(rows) - set(states)  # already a finding of their own
    for key in sorted(set(listed) | set(found)):
        missing = sorted(found.get(key, set()) - listed.get(key, set()))
        extra = sorted(listed.get(key, set()) - found.get(key, set()) - failing)
        if missing or extra:
            problems.append(f"COVERAGE RECORD {key[0]} {key[1]}: 09 section 8.10 lacks "
                            f"{', '.join(missing) or 'nothing'}, lists {', '.join(extra) or 'nothing'}"
                            " beyond what the gate finds")


def check_executed(tags: list[Tag], suites: str, evidence: str, problems: list[str]) -> int:
    """The executed half: the run passed every tag's suite and ran every tag."""
    if not SUITES_DONE.search(suites):
        problems.append("SUITES RUN INCOMPLETE: no 'suites: N checks total, 0 failing' line")
    passed = {m[1] for m in map(SUITE_PASS.match, suites.splitlines()) if m}
    ran: dict[tuple[str, str, int], tuple[str, str]] = {}
    for line in evidence.splitlines():
        match = EVIDENCE.match(line)
        if not match:
            problems.append(f"MALFORMED EVIDENCE '{line}': want '<suite> <file>:<line> <REQ> <CAT>'")
            continue
        ran[(match[1], match[2], int(match[3]))] = (match[4], match[5])
    keys = {t.key: t for t in tags}
    for tag in tags:
        if tag.suite not in passed:
            problems.append(f"SUITE NOT PASSED {tag.suite} {tag.where}: the run reports no "
                            f"'PASS {tag.suite}'")
        elif ran.get(tag.key) != (tag.req, tag.cat):
            problems.append(f'DEAD TAG {tag.req} {tag.where}: "{tag.name}" never ran in this run')
    for key, (req, cat) in sorted(ran.items()):
        if key not in keys:
            problems.append(f"STRAY EVIDENCE {key[0]} {key[1]}:{key[2]} {req} {cat}: no tag "
                            "stands there in this tree")
    return len(ran)


def print_table(rows: dict[str, Row], tags: list[Tag], states: dict[str, str]) -> None:
    """REQ, Ver, state, then file:line and check name of each of its tags."""
    print("| REQ | Ver | State | file:line | Check | Tag category |")
    print("|---|---|---|---|---|---|")
    for req, row in rows.items():
        own = [t for t in tags if t.req == req]
        state = states.get(req, "UNCOVERED")
        if not own:
            print(f"| {req} | {row.ver} | {state} | | | |")
        for tag in own:
            print(f"| {req} | {row.ver} | {state} | `{tag.where}` | {tag.name} | {tag.cat} |")


def check(root: Path, suites: Path | None, evidence: Path | None, table: bool) -> int:
    """The gate over the tree at `root`; 1 on any finding."""
    problems: list[str] = []
    review = (root / REVIEW).read_text(encoding="utf-8")
    verify = (root / VERIFY).read_text(encoding="utf-8")
    rows = read_rows(review)
    if not rows:
        print(f"REQ TAG FAIL: NO ROWS: {REVIEW} gave no REQ row")
        return 1
    waivers = read_waivers(verify, rows, problems)
    tags = read_tags(root, rows, problems)
    states = row_states(rows, tags, waivers, problems)
    check_records(rows, states, verify, problems)
    ran = None
    if suites is not None and evidence is not None:
        ran = check_executed(tags, suites.read_text(encoding="utf-8"),
                             evidence.read_text(encoding="utf-8") if evidence.exists() else "",
                             problems)
    if table:
        print_table(rows, tags, states)
    for line in problems:
        print(f"REQ TAG FAIL: {line}")
    count = {s: sum(1 for v in states.values() if v.startswith(s))
             for s in ("traced", "waived", "no check")}
    verdict = "OK" if not problems else f"{len(problems)} FAILURES"
    print(f"req-tags: {len(rows)} REQ rows: {count['traced']} traced, {count['waived']} waived, "
          f"{count['no check']} need no check; {len(tags)} tags in {len({t.path for t in tags})} "
          f"files, {verdict}")
    if ran is not None:
        print(f"req-tags executed: {ran} evidence lines, {len({t.suite for t in tags})} suites, "
              f"{verdict}")
    return 1 if problems else 0


# ---- the self-test -----------------------------------------------------------

SELFTEST_REVIEW = """## 6. Compliance matrix (F00.1)

| REQ | Clause | Requirement | Mand | Cov | Finding | Arch | Doc | Ver |
|---|---|---|---|---|---|---|---|---|
| REQ-AAA-001 | c | r | shall | A; traced | [GAP-01](#gap-01) | a | d | DIR |
| REQ-AAA-002 | c | r | shall | C; traced | [GAP-02](#gap-02) | a | d | RND |
| REQ-AAA-003 | c | r | shall | A; waived GAP-02 | [GAP-02](#gap-02) | a | d | STORM |
| REQ-AAA-004 | c | r | design | A; no check | [GAP-03](#gap-03) | a | d | lint |
| REQ-AAA-005 | c | r | shall | P; traced | [GAP-01](#gap-01) | a | d | TIM |
"""
SELFTEST_VERIFY = """### 8.10 Requirement tags

<a id="req-coverage"></a>

| State | Ver | Rows |
|---|---|---|
| traced | DIR | REQ-AAA-001 |
| traced | RND | REQ-AAA-002 |
| traced | TIM | REQ-AAA-005 |
| no check | lint | REQ-AAA-004 |

<a id="req-waivers"></a>

| REQ | Ver | GAP | Reason |
|---|---|---|---|
| REQ-AAA-003 | STORM | GAP-02 | no storm check of this row |
"""
SELFTEST_CPP = """int run() {
  REQ_TAG("REQ-AAA-001", "DIR", "D1");
  CHECK(x == 1, "D1: one");
  REQ_TAG("REQ-AAA-002", "RND", "R1");
  REQ_TAG("REQ-AAA-003", "DIR", "R1");
  CHECK(y == 2,
        "R1: a seeded session");
  CHECK(z == 3, "Z1: untagged");
}
"""
SELFTEST_PY = '''class SomeTest(unittest.TestCase):
    """A test."""

    def test_timing(self) -> None:
        """The timer."""
        REQ_TAG("REQ-AAA-005", "TIM", "SomeTest.test_timing")
        self.assertTrue(True)
'''
SELFTEST_SUITES = """PASS one (3 checks: 3 PASS, 0 FAIL)
PASS two (1 checks: 1 PASS, 0 FAIL)
suites: 4 checks total, 0 failing
"""
SELFTEST_EVIDENCE = """one sim_main.cpp:2 REQ-AAA-001 DIR
one sim_main.cpp:4 REQ-AAA-002 RND
one sim_main.cpp:5 REQ-AAA-003 DIR
two test_two.py:6 REQ-AAA-005 TIM
"""
CPP = "tb/one/sim_main.cpp"
PY = "tb/two/test_two.py"
FILES = {str(REVIEW): SELFTEST_REVIEW, str(VERIFY): SELFTEST_VERIFY, CPP: SELFTEST_CPP,
         PY: SELFTEST_PY, "suites": SELFTEST_SUITES, "evidence": SELFTEST_EVIDENCE}
DROP_001 = ('  REQ_TAG("REQ-AAA-001", "DIR", "D1");\n', "")
#: (file, old text, new text) edits to the fixture, and the findings they must
#: give: the assignment's five planted defects first, then every other finding
SELFTEST_CASES: tuple[tuple[tuple[tuple[str, str, str], ...], list[str]], ...] = (
    ((), []),
    (((CPP,) + DROP_001, ("evidence", "one sim_main.cpp:2 REQ-AAA-001 DIR\n", ""),
      ("evidence", ":4 ", ":3 "), ("evidence", ":5 ", ":4 ")),
     ["UNCOVERED REQ-AAA-001 (DIR)"]),
    (((CPP, '"REQ-AAA-001", "DIR"', '"REQ-ZZZ-001", "DIR"'),
      ("evidence", "REQ-AAA-001 DIR", "REQ-ZZZ-001 DIR")),
     ["UNKNOWN REQ REQ-ZZZ-001 tb/one/sim_main.cpp:2", "UNCOVERED REQ-AAA-001 (DIR)"]),
    ((("evidence", "one sim_main.cpp:2 REQ-AAA-001 DIR\n", ""),),
     ["DEAD TAG REQ-AAA-001 tb/one/sim_main.cpp:2"]),
    (((str(VERIFY), "| STORM | GAP-02 |", "| STORM | |"),),
     ["WAIVER WITHOUT GAP REQ-AAA-003"]),
    (((str(REVIEW), "| a | d | TIM |\n", "| a | d | TIM |\n| REQ-AAA-006 | c | r | shall | "
       "A | [GAP-01](#gap-01) | a | d | NVM |\n"),),
     ["UNCOVERED REQ-AAA-006 (NVM)"]),
    (((CPP, '"REQ-AAA-002", "RND"', '"REQ-AAA-002", "STORM"'),
      ("evidence", "REQ-AAA-002 RND", "REQ-AAA-002 STORM")),
     ["UNCOVERED REQ-AAA-002 (RND)"]),
    (((CPP, '"REQ-AAA-001", "DIR"', '"REQ-AAA-001", "FOO"'),
      ("evidence", "REQ-AAA-001 DIR", "REQ-AAA-001 FOO")),
     ["UNKNOWN CATEGORY FOO tb/one/sim_main.cpp:2", "UNCOVERED REQ-AAA-001 (DIR)"]),
    (((CPP, '"DIR", "D1")', '"DIR", "D2")'),), ["NAME NOT AT CHECK tb/one/sim_main.cpp:2"]),
    (((PY, '"SomeTest.test_timing"', '"SomeTest.test_other"'),),
     ["NAME NOT AT CHECK tb/two/test_two.py:6"]),
    (((CPP, '  REQ_TAG("REQ-AAA-001", "DIR", "D1");', '  REQ_TAG("REQ-AAA-001", "DIR", "D1"); f();'),
      ("evidence", "one sim_main.cpp:2 REQ-AAA-001 DIR\n", "")),
     ["MALFORMED TAG tb/one/sim_main.cpp:2", "UNCOVERED REQ-AAA-001 (DIR)"]),
    (((str(VERIFY), "| GAP-02 | no storm check of this row |", "| GAP-02 | |"),),
     ["WAIVER WITHOUT REASON REQ-AAA-003"]),
    (((str(VERIFY), "| STORM | GAP-02 |", "| STORM | GAP-01 |"),
      (str(REVIEW), "A; waived GAP-02", "A; waived GAP-01")),
     ["WAIVER NAMES ANOTHER ROW'S GAP REQ-AAA-003"]),
    (((str(VERIFY), "| REQ-AAA-003 | STORM | GAP-02 | no storm check of this row |\n",
       "| REQ-AAA-003 | STORM | GAP-02 | no storm check of this row |\n"
       "| REQ-AAA-004 | lint | GAP-03 | a lint row |\n"),),
     ["WAIVER ON A ROW NEEDING NO CHECK REQ-AAA-004"]),
    (((str(VERIFY), "| REQ-AAA-003 | STORM | GAP-02 | no storm check of this row |\n",
       "| REQ-AAA-003 | STORM | GAP-02 | no storm check of this row |\n"
       "| REQ-AAA-001 | DIR | GAP-01 | a stale waiver |\n"),),
     ["STALE WAIVER REQ-AAA-001"]),
    (((str(REVIEW), "| P; traced |", "| P |"),), ["COV RECORD REQ-AAA-005"]),
    (((str(VERIFY), "| traced | TIM | REQ-AAA-005 |\n", ""),), ["COVERAGE RECORD traced TIM"]),
    ((("suites", "PASS two (1 checks: 1 PASS, 0 FAIL)\n", "FAIL two\n"),
      ("suites", "0 failing", "1 failing")),
     ["SUITES RUN INCOMPLETE", "SUITE NOT PASSED two tb/two/test_two.py:6"]),
    ((("evidence", "two test_two.py:6", "two test_two.py:6 REQ-AAA-005 TIM\ntwo test_two.py:9"),),
     ["STRAY EVIDENCE two test_two.py:9 REQ-AAA-005 TIM"]),
    ((("evidence", "one sim_main.cpp:2 REQ-AAA-001 DIR\n", "one sim_main.cpp:2\n"),),
     ["MALFORMED EVIDENCE 'one sim_main.cpp:2'", "DEAD TAG REQ-AAA-001 tb/one/sim_main.cpp:2"]),
    ((("tb/common/x.cpp", "", '  REQ_TAG("REQ-AAA-001", "DIR", "D1");\n  CHECK(a, "D1");\n'),),
     ["TAG OUTSIDE A SUITE tb/common/x.cpp:1", "SUITE NOT PASSED common tb/common/x.cpp:1"]),
    (((PY, '        self.assertTrue(True)', '        REQ_TAG("REQ-AAA-005", "TIM", "SomeTest.test_timing")'
       '\n        self.assertTrue(True)'),
      ("evidence", "two test_two.py:6 REQ-AAA-005 TIM\n",
       "two test_two.py:6 REQ-AAA-005 TIM\ntwo test_two.py:7 REQ-AAA-005 TIM\n")),
     ["DUPLICATE TAG REQ-AAA-005 tb/two/test_two.py:7"]),
    (((str(REVIEW), "| a | d | TIM |", "| a | d | XYZ |"),), ["UNKNOWN VER REQ-AAA-005"]),
    (((str(VERIFY), "| no storm check of this row |\n",
       "| no storm check of this row |\n| REQ-AAA-003 | STORM | GAP-02 | again |\n"),),
     ["DUPLICATE WAIVER REQ-AAA-003"]),
    (((str(VERIFY), "| no storm check of this row |\n",
       "| no storm check of this row |\n| REQ-ZZZ-009 | DIR | GAP-01 | no row |\n"),),
     ["UNKNOWN REQ REQ-ZZZ-009"]),
    (((str(VERIFY), "| REQ-AAA-003 | STORM |", "| REQ-AAA-003 | DIR |"),), ["WAIVER VER REQ-AAA-003"]),
    (((str(VERIFY), '<a id="req-waivers"></a>', ""),),
     ['NO TABLE <a id="req-waivers"></a>', "UNCOVERED REQ-AAA-003 (STORM)"]),
    (((str(VERIFY), "| State | Ver | Rows |", "| State | Rows |"),),
     ['NO TABLE <a id="req-coverage"></a>']),
    (((str(REVIEW), SELFTEST_REVIEW, "## 6. Compliance matrix (F00.1)\n"),), ["NO ROWS"]),
)
FINDING = re.compile(r"^REQ TAG FAIL: (.*?)(?:: |$)", re.M)


def planted(edits: Iterable[tuple[str, str, str]]) -> dict[str, str]:
    """The fixture with `edits` applied; a file absent from it starts empty."""
    files = dict(FILES)
    for name, old, new in edits:
        body = files.get(name, "")
        if old and old not in body:
            raise SystemExit(f"SELFTEST BROKEN: '{old}' is not in {name}")
        files[name] = body.replace(old, new) if old else body + new
    return files


def selftest() -> int:
    """Run the gate, both halves, over the fixture and each planted fault."""
    failures = 0
    for number, (edits, expect) in enumerate(SELFTEST_CASES, start=1):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            for rel, body in planted(edits).items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(body, encoding="utf-8")
            proc = subprocess.run([sys.executable, __file__, "--root", str(root),
                                   "--suites", str(root / "suites"),
                                   "--evidence", str(root / "evidence")],
                                  capture_output=True, text=True, check=False)
            got = sorted(FINDING.findall(proc.stdout))
            if got != sorted(expect) or proc.returncode != (1 if expect else 0):
                failures += 1
                print(f"SELFTEST FAIL: case {number}: want {sorted(expect)}, got {got} "
                      f"rc {proc.returncode}\n{proc.stdout}{proc.stderr}")
    print(f"req-tags selftest: {len(SELFTEST_CASES)} cases, "
          f"{'OK' if not failures else str(failures) + ' FAILURES'}")
    return 1 if failures else 0


def main() -> int:
    """`--selftest` proves the gate on planted trees; otherwise check `--root`."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--suites", type=Path, help="scripts/run_suites.sh output of the run")
    parser.add_argument("--evidence", type=Path, help="the REQ_TAG_LOG file of the same run")
    parser.add_argument("--table", action="store_true", help="print the coverage table")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if (args.suites is None) != (args.evidence is None):
        parser.error("--suites and --evidence go together")
    return check(args.root, args.suites, args.evidence, args.table)


if __name__ == "__main__":
    sys.exit(main())
