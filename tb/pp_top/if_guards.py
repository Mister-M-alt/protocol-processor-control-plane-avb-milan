#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Elaborate the real top across N_AVB_IF_P and require the legal range.

P-N-AVB-INTERFACES is the redundancy seam (issue #69): the shipping build has
one interface, so nothing else would notice the two-interface elaboration rot.
Each case lints protocol_processor_top, from the sources the Makefile passes
after `--`, with the flags of scripts/lint_hdl.sh and one count override: a
legal count, one interface or Milan's redundant pair, must lint clean with no
warning at all, and a refused one must fail with the top's own message naming
N_AVB_IF_P, as a $fatal. Verilator reports an elaboration $fatal as a USERFATAL
warning, which a -Wno-fatal simulation build carries past, so the verdict is
taken from a lint that tolerates no warning.
"""
import argparse
import subprocess

#: the count, then the refusal it must print (None: it must lint clean)
CASES = (
    (1, None),                                          # the default, one interface
    (2, None),                                          # Milan's redundant pair
    (0, "N_AVB_IF_P=0 is outside 1 to 2"),              # one under the floor
    (3, "N_AVB_IF_P=3 is outside 1 to 2"),              # one over the ceiling
)
LINT = ("--lint-only", "-Wall", "-Wno-DECLFILENAME", "-Wno-UNUSEDSIGNAL",
        "-Wno-UNUSEDPARAM", "--top-module", "protocol_processor_top")


def findings_of(output: str) -> list[str]:
    """Verilator's warning and error lines, without its closing summary."""
    return [line for line in output.splitlines()
            if line.startswith(("%Warning", "%Error")) and "Exiting due to" not in line]


def main() -> int:
    """Lint each case; 0 = every legal count clean and every other refused by name."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verilator", default="verilator")
    parser.add_argument("sources", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    sources = args.sources[1:] if args.sources[:1] == ["--"] else args.sources
    failing = 0
    for count, refusal in CASES:
        result = subprocess.run(
            [args.verilator, *LINT, f"-GN_AVB_IF_P={count}", *sources],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
        findings = findings_of(result.stdout)
        if refusal is None:
            ok = result.returncode == 0 and not findings
            outcome = "lints clean" if ok else "must lint clean"
        else:
            named = [f for f in findings if "USERFATAL" in f and refusal in f]
            ok = result.returncode != 0 and bool(named)
            outcome = f"refused: {refusal}" if ok else f"must be refused: {refusal}"
        if ok:
            print(f"if guard {count}: {outcome}")
        else:
            failing += 1
            print(f"FAIL: if guard {count}: {outcome} (verilator rc {result.returncode})")
            for finding in findings[:6]:
                print(f"    {finding}")
    #: "checks", as the ADP campaign reads a completed run by it; not the canonical
    #: tally shape, so run_suites.sh still reads the Makefile's summed line
    print(f"if guards: {len(CASES)} checks, {failing} failing")
    return int(failing != 0)


if __name__ == "__main__":
    raise SystemExit(main())
