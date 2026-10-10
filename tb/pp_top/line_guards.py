#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Elaborate the real top across DESC_LINE_BYTES_P and require the legal range.

The response buffer is the integrator's reservation, 16 + DESC_LINE_BYTES_P
bytes, and KL_aecp_engine refuses a line that is not a multiple of 8 from 576
to 1008 (issue #50). Each case lints protocol_processor_top, from the sources
the Makefile passes after `--`, with the flags of scripts/lint_hdl.sh and one
line override: a legal line must lint clean, and a refused one must fail with
the engine's own message naming DESC_LINE_BYTES_P, the parameter an
integrator sets, as a $fatal. Verilator reports an elaboration $fatal as a
USERFATAL warning, which a -Wno-fatal simulation build carries past, so the
verdict is taken from a lint that tolerates no warning.
"""
import argparse
import subprocess

#: the line, then the refusal it must print (None: it must lint clean)
CASES = (
    (576, None),                                        # the floor and the default
    (584, None),                                        # the line build's fixture
    (1008, None),                                       # the ceiling
    (568, "DESC_LINE_BYTES_P=568 is below 576"),        # one step under the floor
    (1016, "DESC_LINE_BYTES_P=1016 is above 1008"),     # one step over the ceiling
    (580, "DESC_LINE_BYTES_P=580 is not a multiple of 8"),
)
LINT = ("--lint-only", "-Wall", "-Wno-DECLFILENAME", "-Wno-UNUSEDSIGNAL",
        "-Wno-UNUSEDPARAM", "--top-module", "protocol_processor_top")


def findings_of(output: str) -> list[str]:
    """Verilator's warning and error lines, without its closing summary."""
    return [line for line in output.splitlines()
            if line.startswith(("%Warning", "%Error")) and "Exiting due to" not in line]


def main() -> int:
    """Lint each case; 0 = every legal line clean and every other refused by name."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verilator", default="verilator")
    parser.add_argument("sources", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    sources = args.sources[1:] if args.sources[:1] == ["--"] else args.sources
    failing = 0
    for line, refusal in CASES:
        result = subprocess.run(
            [args.verilator, *LINT, f"-GDESC_LINE_BYTES_P={line}", *sources],
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
            print(f"line guard {line}: {outcome}")
        else:
            failing += 1
            print(f"FAIL: line guard {line}: {outcome} (verilator rc {result.returncode})")
            for finding in findings[:6]:
                print(f"    {finding}")
    print(f"line guards: {len(CASES)} cases, {failing} failing")
    return int(failing != 0)


if __name__ == "__main__":
    raise SystemExit(main())
