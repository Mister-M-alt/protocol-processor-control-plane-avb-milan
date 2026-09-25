#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Remove only the request hold; require the completed late-byte assertion to fail."""
import argparse
from pathlib import Path
import subprocess


def main() -> int:
    """Read CLI options and return 0 only when the completed byte check kills the mutant."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    suite = Path(__file__).resolve().parent
    rtl = suite.parents[1] / "hdl/aecp/KL_aecp_desc_mem_guard.sv"
    source = rtl.read_text()
    hold = " && !owed_r"
    if source.count(hold) != 2:
        raise SystemExit("REFUSED: expected exactly the two request hold arms")
    mutant = out / "KL_aecp_desc_mem_guard.no_hold.sv"
    mutant.write_text(source.replace(hold, ""))
    log = out / "mutant-no-hold.log"
    cmd = ["make", "-C", str(suite), "run", f"GUARD_SRC={mutant}",
           "OBJ_DIR=obj_mutant", "ARGS=--late-only"]
    with log.open("w") as stream:
        # The late-only case bounds boot and every read by BOUND cycles;
        # its remaining idle steps have fixed counts, even if memory stalls.
        result = subprocess.run(cmd, stdout=stream, stderr=subprocess.STDOUT)
    text = log.read_text()
    failures = [line for line in text.splitlines() if line.startswith("FAIL:")]
    expected = "FAIL: third locate late_beats_never_served: STREAM_INPUT received another burst's bytes"
    detected = (result.returncode == 2 and failures == [expected]
                and "late case completed:" in text and "18 checks: 17 PASS, 1 FAIL" in text
                and "third locate bytes: 00060000deadbeef cafef00d01234567" in text)
    print(f"hold-deleted mutant: make rc={result.returncode}; completed byte assertion detected={detected}")
    print(f"log: {log}")
    return 0 if detected else 1


if __name__ == "__main__":
    raise SystemExit(main())
