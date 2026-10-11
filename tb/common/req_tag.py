# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The requirement tag a Python check carries: req_tag.hpp's statement, for a test.

One statement on its own line at the head of the test method it labels,

    REQ_TAG("REQ-MDL-001", "DIR", "LintTest.test_mutations")

names the compliance-matrix row, the Ver category of the check and the check's
name. scripts/check-req-tags.py reads the tags out of the source. With
REQ_TAG_LOG unset or empty the call does nothing; set, the first time a tag runs
in a process it appends `<suite> <file>:<line> <REQ> <CAT>` to that file, the
same evidence line req_tag.hpp writes (docs/architecture/09_verification.md
section 8.10). A test loads this file by path, never through `sys.path`.
"""

import os
import sys
from pathlib import Path

_SEEN: set[tuple[str, int]] = set()


def REQ_TAG(req: str, cat: str, name: str) -> None:
    """Append the evidence line of the calling tag, once, if REQ_TAG_LOG names a file.

    Spelled as req_tag.hpp's macro is, so one pattern reads both languages.
    `name` is read by the gate, never at run time.
    """
    del name
    log = os.environ.get("REQ_TAG_LOG", "")
    if not log:
        return
    caller = sys._getframe(1)  # the tag's own file and line, as __FILE__/__LINE__ give
    key = (caller.f_code.co_filename, caller.f_lineno)
    if key in _SEEN:
        return
    _SEEN.add(key)
    with Path(log).open("a", encoding="utf-8") as out:
        out.write(f"{Path.cwd().name} {Path(key[0]).name}:{key[1]} {req} {cat}\n")
