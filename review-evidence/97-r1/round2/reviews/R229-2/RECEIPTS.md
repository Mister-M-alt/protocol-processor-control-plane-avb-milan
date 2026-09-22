R229-2 factual receipts for PR100 / issue97

Reviewed head: `5c45845ad15bd7995f20c81d7fd61501e5ca9d7e`; tree: `8768f7e640ee62fd62862bec7117155fcab3ddbb`.

Reproduce focused execution using installed GCC (with German/French catalogs for the prior-failure demonstrations), Verilator, GNU Make and Python. Choose an output directory that does not already exist:

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r2-r229/reproduce-focused.py $VALIDATION_STORAGE/reviews/r229-100-r2 /tmp/r229-r2-fresh-receipts
```

The runner creates source archives in fresh temporary directories, records exact argv/cwd/environment overrides and raw outputs, uses sequential outer `make -j1`, and caps every Verilator work invocation at eight jobs. Its wrappers record effective Verilator commands and actual compiler argv, selected inherited inputs, return codes and diagnostics. Unit-test mutants run under outer `LANG=C LANGUAGE=C LC_ALL=C`, mock subprocesses, and fail before any compiler or Verilator runs. The source clone is read-only. Scratch paths are recorded in `focused/scratch.txt`; generated build products remain there for inspection.

Verify these completed receipts and the unchanged clone:

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r2-r229/verify-receipts.py $VALIDATION_STORAGE/reviews/r229-100-r2 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r2-r229
```

`verification-summary.json` is the successful result. `reviewer-runner-note.txt` records one pre-mutation anchor error and the resumed execution. No test or product result was repaired or discarded. `scope-and-retention.json` records exact changed/unchanged Git artifacts and the basis for retaining R229-1 RTL/M25/M31 coverage. `delta.patch` is correction-only; `pr.patch` includes the complete PR against main.

`focused/results.json` indexes all 18 completed probes. `focused/01-*` are expected failures at the prior head. `02-*`/`03-*` are passing corrected-head locale gates; `04-*` through `12-*` are expected-negative controls; `13-*` records build wiring; `14-*` is the successful two-build runtime run; `15-*`/`16-*` are documentation and diff checks. Compiler logs are in `focused/compiler/`. `scratch-source-differences.json` and `job-cap-check.json` validate mutation scope and compile caps.

Public inputs were retrieved read-only with `gh api` and immutable raw GitHub URLs. `issue95.json`, `issue97.json`, `issue97-comments.json`, `pr100.json`, `comments.json` and `parent-public-AGENTS.md` are public context snapshots. `downloaded-evidence.json` lists the exact immutable URL, Git blob ID and SHA-256 for every downloaded archive artifact. Files under `public-evidence/` preserve published bytes. The outer published manifest distinguishes original and path-neutralized hashes; use its `published_sha256` for downloaded file validation. No private lane file was consulted.

Hosted metadata can be retrieved using these read-only endpoints, substituting either run ID `35708824838` or `35708829801`, and the job IDs in REPORT.md:

```sh
rtk gh api repos/Mister-M-alt/protocol-processor-control-plane-avb-milan/actions/runs/35708824838
rtk gh api 'repos/Mister-M-alt/protocol-processor-control-plane-avb-milan/actions/runs/35708824838/jobs?per_page=100'
rtk gh api --allow-escape-sequences repos/Mister-M-alt/protocol-processor-control-plane-avb-milan/actions/jobs/106684179182/logs
rtk gh api repos/Mister-M-alt/protocol-processor-control-plane-avb-milan/git/commits/6439edc9e2a2fa06c4f9ce2e35d4ccaa273e321d
```

Raw hosted logs, metadata and checkout identity observations are saved as `hosted-*`. Those logs and the manager's completed nine-command native record are external execution evidence; the local focused receipts are reviewer execution. Old `public-evidence/reviews/R229-1/07-*` and `08-*` retain their original round/head attribution.

The report is the reviewer verdict. Receipts do not independently authorize a merge or substitute for the other review, current-candidate checks, or post-merge containment. The evidence branch must never merge.
