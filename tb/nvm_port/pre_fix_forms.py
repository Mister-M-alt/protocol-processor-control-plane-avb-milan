# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""
The pre-fix matrix's check forms, and the pins that hold each injected form to
the form git recorded before it was removed.

`measure_figures.py` injects MATRIX_FORMS beside the current checks and runs
every array model over them; the cells it measures are only evidence if each
injection IS the removed code. This module owns what is injected and the three
checks that say so: `git_verbatim` (every pinned predicate is in its source
revision and still injected), `pin_completeness` (every injected condition line
is pinned) and `line_multiplicity` (no injected line occurs more often than in
git). It reads no RTL and runs no build; the figures gate imports it.
"""

import re
import subprocess
from pathlib import Path

SRC = Path(__file__).resolve().parent

#: The six-by-five pre-fix matrix. Each row is a check FORM that was removed
#: from the suite because it read the array where it should not; the table
#: records which device model exposed each. Two earlier versions of this script
#: declared these thirty cells unmeasurable "because the forms no longer exist
#: in the tree". That was a cost choice dressed as an impossibility, and the
#: README's own text contradicted it: "re-deriving it is re-running it." The
#: forms are recoverable from `ba403c6~1`, `62d96d6~1` and `dc354be~1`, and
#: re-injecting all six beside the current checks costs FIVE builds -- one
#: fewer than MODELS already spends.
#:
#: (README row label, anchor, code injected before it). Order matters: the two
#: T17 forms must be injected BEFORE the good commit, because both pre-fix
#: spellings restored the TORN region and anchoring after `h.commit(3, rec)`
#: measures the wrong array.
MATRIX_FORMS = [
    ("T16 byte comparison",
     "  CHECK(h.sent.size() == 5 && std::equal(h.sent.begin(), h.sent.end(),",
     '    CHECK(std::equal(torn.begin(), torn.begin() + 5, h.store[1])\n'
     '              && h.store[1][5] == 0xFF,\n'
     '          "MX T16 byte comparison");\n'),
    ("T17 restore vs the record",
     "  h.ops.clear();\n  rc = h.commit(3, rec);\n"
     "  CHECK(rc == 0 && h.store_match(3, rec),",
     # Byte-faithful to the removed code, and pinned as such below. The
     # earlier version renamed `rc` to `pr`, MERGED the two original checks
     # into one conjunction, and captured the array BEFORE the restore across
     # two restores instead of after it across one. All thirty cells agreed
     # either way, which is exactly why it needed a pin rather than a re-run:
     # a reconstruction and a re-reconstruction share their errors.
     '    h.ops.clear();\n'
     '    rc = h.restore(3);\n'
     '    std::vector<uint8_t> in_array(h.store[3], h.store[3] + late.size());\n'
     '    CHECK(rc == 0 && h.rbytes == late,\n'
     '          "MX T17 restore vs the record");\n'
     '    CHECK(rc == 0 && h.rbytes.size() == late.size(),\n'
     '          "MX T17 restore vs the record length");\n'
     '    CHECK(h.rbytes == in_array,\n'
     '          "MX T17 restore vs the array");\n'),
    ("T15 branch pin", "  bool hdr_intact =",
     '    CHECK(crc_rejects && h.rbytes.size() == whole.size(),\n'
     '          "MX T15 branch pin");\n'
     '    CHECK(!h.store_match(7, whole) && !h.store_match(7, replacement),\n'
     '          "MX T15 cut was real");\n'
     '    CHECK(refused || crc_rejects,\n'
     '          "MX T15 #70 property, unconditioned");\n'),
]
#: Rows carried by an injection above rather than owning an anchor of their own.
MATRIX_ROWS = ["T16 byte comparison", "T17 restore vs the record",
               "T17 restore vs the array", "T15 branch pin",
               "T15 cut was real", "T15 #70 property, unconditioned"]


#: The one assumption the thirty matrix cells rest on that had no check behind
#: it: that each injected form IS the form that was removed. Re-running proves
#: nothing here -- two independent reconstructions share whatever transcription
#: error both made, and both agreed on all thirty cells while one of them had
#: merged two checks and moved an array capture. Git is the third party.
#:
#: (matrix row, revision, predicate that must appear VERBATIM in that file).
#:
#: The row is not decoration. The first version of this pin had five predicates
#: and all five were T17 -- the row where a defect had already been found and
#: fixed. The four rows where nothing had been found were the four with no pin,
#: and swapping T16's predicate for a DIFFERENT one was caught by nothing: not
#: by the pin, and not by the cell comparison either, since all thirty cells
#: stayed identical. An instrument that covers exactly where a defect was
#: already found is the same failure this gate has now made in five different
#: costumes, so `git_verbatim` requires every row to carry at least one pin.
GIT_FORMS = [
    ("T17 restore vs the record", "dc354be~1", "CHECK(rc == 0 && h.rbytes == late,"),
    ("T17 restore vs the record", "dc354be~1", "rc = h.restore(3);"),
    ("T17 restore vs the array", "62d96d6~1",
     "std::vector<uint8_t> in_array(h.store[3], h.store[3] + late.size());"),
    ("T17 restore vs the array", "62d96d6~1", "CHECK(rc == 0 && h.rbytes.size() == late.size(),"),
    ("T17 restore vs the array", "62d96d6~1", "CHECK(h.rbytes == in_array,"),
    ("T16 byte comparison", "dc354be~1", "CHECK(std::equal(torn.begin(), torn.begin() + 5, h.store[1])"),
    ("T16 byte comparison", "dc354be~1", "&& h.store[1][5] == 0xFF,"),
    ("T15 branch pin", "dc354be~1", "CHECK(crc_rejects && h.rbytes.size() == whole.size(),"),
    ("T15 cut was real", "dc354be~1",
     "CHECK(!h.store_match(7, whole) && !h.store_match(7, replacement),"),
    ("T15 #70 property, unconditioned", "dc354be~1", "CHECK(refused || crc_rejects,"),
]


def git_verbatim() -> list[str]:
    """Problems where an injected predicate is not what git records."""
    bad = []
    # Every row must carry a pin. Without this the pin covers whichever row
    # somebody last found a bug in, which is precisely the coverage shape that
    # let the T16 predicate be swapped for a different one unnoticed.
    for row in MATRIX_ROWS:
        if not any(r == row for r, _rev, _p in GIT_FORMS):
            bad.append(f"git-form pin: matrix row '{row}' has NO pinned "
                       "predicate, so nothing checks that its injected form is "
                       "the form that was removed")
    for row, rev, pred in GIT_FORMS:
        blob = subprocess.run(["git", "show", f"{rev}:tb/nvm_port/sim_main.cpp"],
                              cwd=SRC, capture_output=True, text=True)
        if blob.returncode:
            bad.append(f"git-form pin: cannot read {rev} -- the revision the "
                       "matrix injection was reconstructed from is gone")
        elif pred not in blob.stdout:
            bad.append(f"git-form pin [{row}]: {pred!r:.60} is NOT in {rev}. "
                       "The injected form is not the removed form, so the "
                       "matrix row measures something the suite never had.")
        elif not any(pred in code for _r, _a, code in MATRIX_FORMS):
            bad.append(f"git-form pin [{row}]: {pred!r:.60} is in {rev} but no "
                       "longer in any injection -- the pin has gone slack")
    return bad


def _check_condition_lines(code):
    """Every line of an injected CHECK that carries CONDITION, not message.

    By paren balance, after stripping the message LITERAL. The first version
    used `'"MX ' in line` as a terminator, which meant a condition sharing a
    line with its message was never collected at all -- a one-line
    `CHECK(cond, "MX row");` yielded nothing, not a partial result. Folding
    T16's second condition onto its message line, altering it, and dropping the
    now-unmatched pin passed all three rules with all thirty cells identical.
    The rule this function implements was simply false whenever a condition
    shared a line with a message.
    """
    lines, out = code.splitlines(), []
    i = 0
    while i < len(lines):
        if "CHECK(" not in lines[i] or lines[i].strip().startswith("//"):
            i += 1
            continue
        depth, body = 0, []
        while i < len(lines):
            raw = lines[i]
            # drop the message literal but keep its comma, so a condition
            # folded onto the same line is still seen
            stripped = re.sub(r'"(?:[^"\\]|\\.)*"', "", raw)
            body.append(stripped)
            depth += stripped.count("(") - stripped.count(")")
            i += 1
            if depth <= 0:
                break
        for b in body:
            b = b.strip().rstrip(";").rstrip(",").strip()
            if b and b not in ("CHECK(", ")"):
                out.append(b)
    return out


def pin_completeness() -> list[str]:
    """Every condition line of every injected CHECK must be pinned to git.

    NOT "at least one pin per row", which was the rule this replaces and was
    the weak default wearing new clothes. T16's form spans two lines and so
    carried two pins; deleting the second left the row compliant, `git_verbatim`
    clean, and re-opened the exact escape the pin had been added to close --
    swapping `h.store[1][5] == 0xFF` for a DIFFERENT predicate, invisible to the
    cell comparison because all thirty cells stay identical.

    A minimum COUNT would have been the same mistake again: the three forms
    carry one, three and three checks, so any threshold is arbitrary. The rule
    is read off the artifact instead -- what must be pinned is determined by
    what is injected, so it cannot drift out of proportion with it.
    """
    pinned = [pred for _row, _rev, pred in GIT_FORMS]
    bad = []
    for row, _anchor, code in MATRIX_FORMS:
        for line in _check_condition_lines(code):
            if not any(line in pred or pred.strip() in line for pred in pinned):
                bad.append(f"pin completeness [{row}]: the condition line "
                           f"{line!r:.60} is injected but not pinned to any "
                           "revision, so it can be swapped for a different "
                           "predicate and nothing will notice")
    return bad


def line_multiplicity() -> list[str]:
    """No injected line may appear more times than in its source revision.

    This closes the structural hole the two other mechanisms share. The cell
    comparison catches structure that moves a result; the git pin catches
    predicate text; a structural change that does NEITHER -- prepending a
    second restore, say -- leaves predicates verbatim and cells byte-identical
    and passes both. Counting occurrences catches it, because the duplicate is
    the structure.

    Deliberately NOT the shape first proposed for this, which was diffing the
    injected span against the git hunk: T17's form merges removals from two
    revisions and T15's from three, so that rule false-positives on the landed
    forms unless given a splice budget of three or more -- an arbitrary number,
    which is how the waiver cap got its 4.
    """
    revs = sorted({rev for _r, rev, _p in GIT_FORMS})
    blobs = {}
    for rev in revs:
        r = subprocess.run(["git", "show", f"{rev}:tb/nvm_port/sim_main.cpp"],
                           cwd=SRC, capture_output=True, text=True)
        if not r.returncode:
            blobs[rev] = r.stdout
    bad = []
    for row, _anchor, code in MATRIX_FORMS:
        # The `"MX ..."` labels are renamed on purpose, so `measure_matrix`
        # can find each form by name in the run's FAIL lines. They are the one
        # thing here that legitimately does not exist in git.
        # Was `len(strip()) > 12`, a threshold introduced while removing
        # thresholds. Inert today, but its boundary sat on a load-bearing line:
        # `refused = 1;` is exactly twelve characters and valid in T15's scope.
        # Brace-only lines are what actually needed excluding.
        for line in {l.strip() for l in code.splitlines()
                     if l.strip() not in ("", "{", "}") and '"MX ' not in l}:
            got = sum(1 for l in code.splitlines() if l.strip() == line)
            allowed = max((sum(1 for l in b.splitlines() if l.strip() == line)
                           for b in blobs.values()), default=0)
            if got > allowed:
                bad.append(f"line multiplicity [{row}]: {line!r:.55} appears "
                           f"{got}x in the injection but at most {allowed}x in "
                           "any source revision -- the injected form has extra "
                           "structure the removed form did not have")
    return bad
