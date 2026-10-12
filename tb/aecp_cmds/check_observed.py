#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Run the existing observed-command gate with its requirement execution tag.

The check and its 176 verdicts remain scripts/check_aecp_commands.py's. The tag
records that this suite reached that check during the same run_suites.sh run.
REQ-VER-002 remains a lint row: this directed observation labels its check but
does not change the matrix's verification category or require a waiver.
"""
import importlib.util
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SPEC = importlib.util.spec_from_file_location("req_tag", HERE.parent / "common/req_tag.py")
TAG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TAG)
REQ_TAG = TAG.REQ_TAG


class ObservedCommands:
    """The directed sweep's existing table and recorded-observation comparisons."""

    def test_sweep(self, observation: Path) -> int:
        """Preserve the command gate's output and exit status, and record execution."""
        REQ_TAG("REQ-VER-002", "DIR", "ObservedCommands.test_sweep")
        return subprocess.run([sys.executable, "-B", str(ROOT / "scripts/check_aecp_commands.py"),
                               "--observed", str(observation)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(ObservedCommands().test_sweep(Path(sys.argv[1])))
