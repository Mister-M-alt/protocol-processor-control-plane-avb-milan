These receipts belong to R229-1, head `1eb20dc4911880de10b745cc284e7dde306788b5`, base `8452f564294300a82d56eed464276576f65f4d58`.

Independent execution:

- `01-fixture-guards.*`: permanent gate using the actual C++ bench and generated headers. `compiler/` contains each raw compiler response, command, return code and generated-header SHA-256 values.
- `02-remove-wire.*`, `03-remove-class-d.*`: one-assertion deletion controls; each gate must return make status 2. Patches and source hashes accompany the receipts.
- `04-extra-error.*` and `04-extra-error-actual.patch`: an unrelated `#error` alongside both required assertions at 0002. The gate must reject this extra error.
- `05-ordinary-builds.*`: unmodified exact-head default and pinned fixture executable builds, simulations and canonical tally.
- `06-full-refusal-*`: the actual overridden fixture build recipe selected from `make -n run`; separate fresh source copies, matching SV/C++ macros, actual `--cc --exe --build`, exact error sets and no fixture executable.
- `07-missing-binding.*`, `08-child-default.*`: sequential isolated runtime controls, with mutation patches and source hashes.
- `09-docs-checks.*`: independent `make -j1 links matrix modmatrix`.
- `source-review.json`, `generated-headers-check.json`, `scratch-source-differences.json`: source comparison and model-header checks.
- `initial-*`, `final-*`, `verify-integrity.py`: tracked content SHA-256, filesystem file kinds/modes, detached HEAD, raw index hash, staged-entry hash and clean-status checks.

`focused-results.json` and per-probe JSON retain argv, cwd, exact source head, wall time and return code. `verilator-invocations.jsonl` records requested and effective arguments. The external `probe-tools/verilator` wrapper replaces the repository's existing `-j 0` with `-j 8`; outer make uses `-j1`, and probes execute sequentially. Product sources are never edited in the review checkout.

To reproduce the focused experiments from this exact detached checkout, run:

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r1-r229/reproduce-focused.py
```

The standalone reproducer creates new scratch copies and a new `/tmp/r229-reproduction-receipts-*` output directory; it does not overwrite these review results. It requires the already installed tools recorded in `tool-versions.json` and the unchanged `probe-tools/` wrappers. It combines the commands actually run in `run-focused.py` and `run-remaining.py`, correcting the initial runner's non-unique mutation anchor. See `reviewer-runner-note.txt`: that reviewer harness failure occurred after probes 01–03 and before any probe-04 mutation/compilation. It is not a product failure. The consolidated reproducer was syntax checked; the underlying individual experiments supply the execution evidence.

Public evidence, attributed rather than independently executed:

- `issue95*.json`, `issue97*.json`, `pr100*.json`, `issue25.json`, `pr26.json`: GitHub REST snapshots used to establish acceptance, history and the separate portability lane.
- `public-evidence/`: all 94 blobs under `review-evidence/97-r1` at archive commit `77e2fb5fdfbb4c02ed7335366733b41c279018cf`. `evidence-tree.json` and `public-evidence-fetch.json` identify and hash each blob; `public-manifest-check.json` checks all 92 published manifest entries. Published redacted-path hashes, rather than the original unredacted hashes, are the comparison authority.
- `author-source-hash-check.json`: all four author-tested source hashes agree with the reviewed head.
- `hosted-runs.json`, `hosted-jobs-*.json`, `hosted-log-*.txt`, `hosted-merge-commit.json`: GitHub run/job status, raw logs, and the synthetic PR merge's tree identity. These are hosted observations, not R229 reruns.
- `hosted-log-fetch.json`: reproducible read-only `gh api --allow-escape-sequences .../actions/jobs/<id>/logs` requests. Initial requests without that flag were refused by gh because the logs contain terminal escapes; no job was rerun or changed.

All public reads used GitHub REST GET endpoints. To retrieve an archived evidence blob again, run `rtk proxy gh api -H 'Accept: application/vnd.github.raw+json' repos/Mister-M-alt/protocol-processor-control-plane-avb-milan/git/blobs/<blob-sha>` using its identity from `public-evidence-fetch.json`.
