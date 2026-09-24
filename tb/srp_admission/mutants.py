#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Clean controls and stale-evaluation mutants, built only in temporary trees."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main() -> int:
    """Run controls and mutants; return 0 only if every expected outcome holds."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[2]
    original = (repo / "hdl/srp/KL_srp_admission.sv").read_text()
    anchor = " && slope_valid_r[aidx_r]"
    if original.count(anchor) != 2:
        raise RuntimeError("expected exactly the fit/refusal validity guards")
    failed = 0
    checks = 0
    with tempfile.TemporaryDirectory(prefix="pp112-mutants-") as tmp:
        tree = Path(tmp)
        (tree / "hdl/srp").mkdir(parents=True)
        shutil.copytree(repo / "tb/common", tree / "tb/common")
        shutil.copytree(repo / "tb/srp_admission", tree / "tb/srp_admission",
                        ignore=shutil.ignore_patterns("obj_*", "__pycache__"))
        for n in (2, 8):
            for label, source in (("control", original),
                                  ("stale-evaluation", original.replace(anchor, ""))):
                (tree / "hdl/srp/KL_srp_admission.sv").write_text(source)
                log = args.output / f"{label}-{n}.log"
                with log.open("w") as stream:
                    result = subprocess.run(["make", "run", f"N={n}"],
                                            cwd=tree / "tb/srp_admission",
                                            stdout=stream, stderr=subprocess.STDOUT,
                                            timeout=1200, check=False)
                contents = log.read_text()
                if label == "control":
                    passed = result.returncode == 0 and "0 FAIL" in contents
                else:
                    passed = (result.returncode != 0 and "checks:" in contents and
                              "FAIL: refused current TSpec never pulses a grant" in contents)
                checks += 1
                failed += not passed
                print(f"{label} N={n}: rc={result.returncode} "
                      f"{'PASS' if passed else 'FAIL'}", flush=True)
    print(f"{checks} checks: {checks - failed} PASS, {failed} FAIL")
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
