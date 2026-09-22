[A166] Bounded author handoff: accepted R230-M1 only

Source lane: `$CANDIDATE`.
Branch: `97-assert-distinct-sr-vid-fixture`.
Head: `5c45845ad15bd7995f20c81d7fd61501e5ca9d7e`.
Tree: `8768f7e640ee62fd62862bec7117155fcab3ddbb`.
Correction parent: `1eb20dc4911880de10b745cc284e7dde306788b5`.
Main base: `8452f564294300a82d56eed464276576f65f4d58`.
Commit: `Normalize fixture guard compiler diagnostics to C` (one line; no trailers or new U+2014).

The compiler subprocess receives a copy of the inherited environment with only `LC_ALL` replaced by `C`. Its compiler selection, arguments, four cases, exact diagnostic sets, total-error count and refusal of unrelated errors are unchanged. `fixture-guards-test`, required by `fixture-guards`, uses standard-library mocks to verify all four compiler invocations with LC_ALL absent and pre-set, preservation of other environment/argv inputs, and isolation from the caller and Verilator. It requires no message catalogs. The README documents only this correction and regression.

Authority: [accepted decision](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/pull/100#issuecomment-5773649948), [independent report](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/pull/100#issuecomment-5773649718), [TAKEN before source edits](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/pull/100#issuecomment-5773679315), and [material decisions](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/pull/100#issuecomment-5773736429). Saved public inputs and selected factual archive receipts are under `public/`.

| Check | Observed result | Receipts |
|---|---|---|
| Pre-correction 1eb20dc, German/French, LC_ALL unset | Both exit 2 at 0002 with exactly two intended translated assertion errors | 01, 02 |
| Corrected German/French, LC_ALL unset | Each exits 0: default and 5A3C compile; 0002 has both errors; 1002 has class-D only | 03, 04 |
| Remove wire guard; remove class-D guard | Each exits 2 at 0002 with exactly the remaining assertion error | 05, 06 |
| Add unrelated error only for 0002 | Exits 2 with both intended errors plus the injected third error | 07 |
| Remove LC_ALL normalization | Required permanent regression fails both inherited-locale subcases before compilation; make exits 2 | 08 |
| Default plus 5A3C run under German caller inputs | Exit 0; 1391 + 20 checks, 0 failures; one canonical `1411 checks: 1411 PASS, 0 FAIL` | 09 |
| `make -j8 check` | Exit 0: 41 Mermaid, 18 WaveDrom, 807 links, 115 REQ rows / 17 GAP findings, 86 module rows / 0 untested, freshness/staleness pass | 10 |
| UPC map | Exit 0: 56 engine constants, 80 entry points agree | 11 |
| Syntax, diff, source/tested-copy and final committed-tree scope | Pass, no mode changes or new U+2014; clean including ignored files | 12, 14 |

Full commands, working directories, raw outputs and return codes are in `COMMANDS.md` and `receipts/`. Exact mutant patches and before/after hashes are in `mutants/`; executable/ROM/tally hashes are in `receipts/09-tested-artifacts.json`. `RESULTS.json` checks the receipts. All 10 working Verilator invocations explicitly cap jobs/build-jobs/verilate-jobs at 8; all pp_top builds and mutations ran sequentially. Documentation used an already installed WaveDrom environment. Local tools: Verilator 5.052, GCC 16.2.1, Python 3.14.7, Make 4.4.1.

Untouched artifact proof: the initial snapshot has 223 files and no ignored artifacts. Exactly three existing files changed (`tb/pp_top/fixture_guards.py`, `Makefile`, `README.md`); one mode-100644 test was added. All 220 other file bytes, kinds and modes remain identical, including every RTL, wrapper, runtime bench, existing fixture/default/assertion/expectation, workflow, documentation outside the pp_top README, and Yosys file. Removing just the five added locale-related lines makes the helper byte-identical to its parent. `snapshots/initial-files.json`, `snapshots/final-files.json`, `receipts/final-scope.json` and the raw tree/index snapshots retain the exact proofs. All tested source bytes/modes match the committed source tree. `source.patch` is the correction-only diff; `source-vs-main.patch` includes the original issue97 work.

No push, PR/review start, project-state change or merge was performed. Optional R230-S1/S2 and adjacent work were not adopted. Parent integration is unchanged and separate. Full native/hosted validation, two cold independent positive reviews with clean five-lens coverage, current-candidate validation and merge/post-merge containment remain manager-owned. The focused author receipts are not independent review or approval. See `REMAINING-GATES.md` for exact limitations, including no new full sweep, alternate compiler, hosted 5.050, or M25/M31 runtime-control rerun. This completes the bounded author task.

[Public author handoff](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/pull/100#issuecomment-5773841434). Disposable scratch was removed after source/artifact verification; cleanup is recorded in `receipts/scratch-cleanup.json`.
