// SPDX-FileCopyrightText: 2026 Kebag Logic
// SPDX-License-Identifier: CERN-OHL-W-2.0
//
// The requirement tag a check carries (docs/architecture/09_verification.md
// section 8.10). One statement on its own line, directly above the check it
// labels:
//
//     REQ_TAG("REQ-NOT-001", "STORM", "ST1b");
//
// names the compliance-matrix row (docs/00_MILAN_COMPLIANCE_REVIEW.md section 6),
// the Ver category of the check, and the check's name as the check prints it.
// scripts/check-req-tags.py reads the tags out of the source; the statement is
// only the executed half of the evidence.
//
// With REQ_TAG_LOG unset or empty, which is every suite run but the gate's, the
// statement does nothing: no output, no file, no change to any check. With it
// set, the first time a tag runs in a process it appends one line to that file,
//
//     <suite> <file>:<line> <REQ> <CAT>
//
// where <suite> is the run's working directory (a suite's `make` runs in
// tb/<suite>) and <file> the source's base name. The gate requires every tag's
// line in the evidence of the same run_suites.sh run that reports its suite PASS.

#ifndef MILAN_TB_REQ_TAG_HPP
#define MILAN_TB_REQ_TAG_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>

#include <unistd.h>

namespace milan::tb {

//! Append the evidence line of a tag that ran, once per tag and process, if
//! REQ_TAG_LOG names a file.
inline void req_tag_record(const char* req, const char* cat, const char* file, int line) {
    static const char* const log = std::getenv("REQ_TAG_LOG");
    if (log == nullptr || *log == '\0') {
        return;
    }
    static std::set<std::pair<std::string, int>> seen;
    if (!seen.emplace(file, line).second) {
        return;
    }
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd) == nullptr) {
        return;  // no evidence line: the gate names the tag as never run
    }
    const char* suite = std::strrchr(cwd, '/');
    suite = suite != nullptr ? suite + 1 : cwd;
    const char* base = std::strrchr(file, '/');
    base = base != nullptr ? base + 1 : file;
    std::FILE* out = std::fopen(log, "a");
    if (out == nullptr) {
        return;
    }
    std::fprintf(out, "%s %s:%d %s %s\n", suite, base, line, req, cat);
    std::fclose(out);
}

}  // namespace milan::tb

//! The tag statement. `name` is read by the gate, never at run time.
#define REQ_TAG(req, cat, name) ::milan::tb::req_tag_record((req), (cat), __FILE__, __LINE__)

#endif  // MILAN_TB_REQ_TAG_HPP
