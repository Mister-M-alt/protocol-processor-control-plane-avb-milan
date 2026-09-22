[A166] Author package ready for manager validation and independent review scheduling

Exact local head `5c45845ad15bd7995f20c81d7fd61501e5ca9d7e`, tree `8768f7e640ee62fd62862bec7117155fcab3ddbb`, parent `1eb20dc4911880de10b745cc284e7dde306788b5`, main base `8452f564294300a82d56eed464276576f65f4d58`. Source is clean and committed on `97-assert-distinct-sr-vid-fixture`; no push was performed.

Scope is accepted MINOR R230-M1 (Robustness, Tests) only. The compiler subprocess uses a copied environment with LC_ALL=C. All original compiler arguments, fixture values, diagnostic sets and strict error counts are retained. A required catalog-independent regression protects this normalization and environment/argument isolation. Three existing pp_top files plus one new test comprise the correction; 220 other files are byte/mode-identical to the starting tree, including RTL, wrapper, sim_main.cpp and workflows.

Author evidence is complete: German/French pre-correction failures and corrected passes; both guard-removal mutants rejected; an extra unrelated 0002 error rejected despite both expected errors; deletion of locale normalization rejected by the permanent regression; default/5A3C 1411 PASS; full documentation check, UPC map, syntax/diff and exact source scope checks. Builds were capped at eight jobs, with sequential mutations in disposable scratch. Commands, logs, source hashes and patches are indexed by `HANDOFF.md`, `COMMANDS.md`, `RESULTS.json` and `MANIFEST.json`.

This file does not start a review or declare the PR merge-ready. The manager must publish and validate this corrected exact head, enforce the complete native and hosted bars, and obtain two independent positive reviews with clean reviewer-owned Conformance, RTL, Robustness, Tests and Docs ledgers before current-candidate/merge/post-merge containment. Prior R229/R230 coverage at 1eb20dc does not establish coverage of this corrected head. R230-M1 closure belongs to independent re-review. No private traces or reasoning are included; the package contains public authority and factual source/command/result evidence only.

See `REMAINING-GATES.md` for limitations and remaining obligations. Optional R230-S1/S2 are not adopted.

[Public author handoff](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/pull/100#issuecomment-5773841434). Disposable scratch was removed after source/artifact verification; cleanup is recorded in `receipts/scratch-cleanup.json`.
