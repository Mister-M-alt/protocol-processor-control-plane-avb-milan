# Remaining manager gates

Author scope is focused pp_top coverage and relevant documentation/lint/format checks. The manager owns the complete native bar and hosted validation on the exact committed head, then R229/R230 cold independent reviews, candidate validation, merge and containment. No review or approval is claimed by these author receipts.

The authoritative workflow is `.github/workflows/hdl.yml` at the reported head; its unchanged copy is `public/hdl.yml`. It runs on both `push` and `pull_request` and pins `VERILATOR_VERSION: v5.050`. Local author receipts use Verilator 5.052, so they do not replace the pinned hosted result.

## Complete native donor bar

From the donor root, with supported tools and a job-capped Verilator on PATH, the manager must establish the full bar on this head. These are the documented command bodies with the required RTK prefix:

```sh
rtk proxy verilator --version
rtk proxy ./scripts/lint_hdl.sh
rtk proxy ./scripts/run_suites.sh
rtk proxy make -j8 check
rtk proxy python3 scripts/gen_matrix.py --check
rtk proxy ./syn/yosys/run.sh
rtk proxy git fetch --no-tags origin refs/pull/13/head
rtk proxy make -j8 -C tb/nvm_port figures
rtk proxy git diff --check
```

Author results already cover lint and documentation, including matrix freshness. Full suites, Yosys and the historical NVM figures gate were intentionally left to the manager. `syn/yosys/run.sh` is unchanged and belongs to PR26. `MAKEFLAGS=-j8` alone does not bound the donor Makefiles' Verilator `-j 0`; the supplied `bin/verilator` wrapper explicitly sets `-j 8 --build-jobs 8 --verilate-jobs 8`. Use the same limit for future local compilation.

## Exact hosted job command bodies

After the workflow's existing tool setup, all three jobs remain required on the exact head:

- `docs-gates`: `python3 scripts/check-links.py`; `python3 scripts/check-matrix.py`; `python3 scripts/render-wavedrom.py --check`; `make stale`. The job installs the `wavedrom` Python package first.
- `suites`: set PATH to the pinned Verilator installation; `verilator --version`; `./scripts/lint_hdl.sh`; `./scripts/run_suites.sh`; `python3 scripts/gen_matrix.py --check`; `git fetch --no-tags origin refs/pull/13/head`; `make -C tb/nvm_port figures`. Full history is required by the historical figures gate.
- `portability`: after Yosys and sv2v installation, `./syn/yosys/run.sh`.

The workflow's cache-miss Verilator setup builds v5.050 from source. No hosted toolchain setup, privileged command, hosted workflow, Docker/act operation or hardware action was performed by the author. The documentation gate created a local WaveDrom virtual environment, which was removed after validation. No workflow change is included.

## Public lane completion

The issue assignment names R229 (cold internal Codex) and R230 (external Opus). Both independent positive reviews must cover the exact head with clean Conformance, RTL, Robustness, Tests and Docs ledgers. No finding may remain open and no review may remain in flight before candidate validation and merge. The manager must validate the current candidate merge tree and perform actual-tree comparison, direct containment and post-merge hosted validation. The parent submodule pin remains a separate task.

Authority: issue97 assignment, issue95 acceptance 4, and the public parent completion bar at https://github.com/kebag-logic/milan-fpga/pull/503#issuecomment-5764802190. Author instructions explicitly stop before pushing, opening a PR, starting reviewers or merging.
