# R223-1 receipts (PR96 at ea93023fbdd31cbf718da0d8f1aab4d750bfd21a)

All runs were made by R223 in scratch copies under `/tmp/r223-96-r1`, created with `git archive` of the head (`ea93023…`) and the base (`424c688…`). Nothing ran in the review clone.

| File | What it is |
|---|---|
| `00-identity.txt` | Head, tree, parent and merge-base; diff stat; `git diff --check`; modes of the changed paths; PR head and base via the API; the hosted runs with their suites log lines |
| `01-evidence-verification.txt` | Evidence commit `f41a2a1…` and its MANIFEST SHA-256 check; A10's candidate and full-native exit codes; A157's pp_top tallies |
| `02-head-pp_top-both-builds.log` | `make` in the head `tb/pp_top` (default and fixture builds, and the canonical tally) |
| `03-base-bench-on-head-rtl.log` | The base `tb/pp_top` run against the head `hdl/` |
| `04-head-bench-on-base-rtl.log` | The head `tb/pp_top` run against the base `hdl/`: the fixture build refuses (PINNOTFOUND) |
| `04b-base-pp_top.log` | The base `tb/pp_top` run against the base `hdl/` |
| `04c/04d-verilator-warnings-*.txt` | The sets of distinct Verilator warnings from the base and head bench builds |
| `05-tally-awk-probe.txt` | The Makefile's tally `awk` program fed synthetic tally files |
| `06-docs-gates.txt` | `check-links`, `check-matrix`, `gen_matrix --check` and `check_upc_map` on head |
| `07-yosys-probe.log` | sv2v and Yosys elaboration of `protocol_processor_top`: head at its default, head with `-chparam SRP_DOM_DEF_VID_P 23100`, base, and base with the same chparam |
| `08-lint-probe.log` | The donor lint flags on `protocol_processor_top` (at its default and with the `-G` fixture) and on `KL_srp_top` |
| `09-mutants.log` | Output of `scripts/run_mutants.sh`, which runs R00 and R25-R38 |
| `10-environment-and-job-cap.txt` | Tool versions, plus proof that the `verilator8` wrapper hands `-j 8` to the inner make and that every build ran on 8 threads |
| `mutations/<ID>/` | For each mutant: `mutation.diff`, `commands.txt` (the commands `make -n run` printed), and the build and run logs of each build it ran |
| `scripts/` | `verilator8`, `rmut.sh`, `run_mutants.sh`, `yosys_probe.sh` and `lint_probe.sh`, as run |
| `SHA256SUMS` | SHA-256 of every file listed above |

To reproduce:
1. Create `/tmp/r223-96-r1/{head,base}` with `git archive <sha> | tar -x -C …`.
2. Install `scripts/verilator8` as `/tmp/r223-96-r1/bin/verilator8`.
3. Run `make VERILATOR=/tmp/r223-96-r1/bin/verilator8` in `head/tb/pp_top`.
4. Run `bash scripts/run_mutants.sh`, `bash scripts/yosys_probe.sh` and `bash scripts/lint_probe.sh`.
