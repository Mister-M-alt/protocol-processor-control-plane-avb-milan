[A166] AUTHOR HANDOFF: R230-M1 correction committed locally

Head `5c45845ad15bd7995f20c81d7fd61501e5ca9d7e`, tree `8768f7e640ee62fd62862bec7117155fcab3ddbb`, parent `1eb20dc4911880de10b745cc284e7dde306788b5`, main base `8452f564294300a82d56eed464276576f65f4d58`. Clean branch `97-assert-distinct-sr-vid-fixture`; not pushed.

The compiler receives a copied environment with only LC_ALL=C overridden. Compiler selection/arguments, fixture values, exact diagnostic sets, total-error count and unrelated-error refusal remain unchanged. A required standard-library regression protects locale/environment isolation without installed catalogs. Four pp_top files only; all 220 other files retain exact bytes/modes, including RTL, wrapper, runtime bench, workflows and Yosys.

Verified with explicit eight-job Verilator caps and sequential scratch mutations: pre-correction German/French failures reproduced with LC_ALL unset and installed GCC catalogs; corrected gates pass; removing either guard, adding an unrelated error alongside both expected errors, and removing locale normalization all fail at the intended gate. Default + 5A3C passes 1,411 checks. Documentation, UPC map, syntax/diff and committed-tree scope checks pass.

Factual `HANDOFF.md`, `REVIEW-READY.md`, exact commands/results/mutant patches and integrity proofs: `$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author`. Local tools: GCC 16.2.1 / Verilator 5.052. No full sweep or hosted run by this author; manager owns complete native/hosted validation, cold independent re-review with two positive clean five-lens reviews, current-candidate and merge/post-merge containment. Optional S1/S2 excluded. No review start, project-state change, push or merge. Bounded author work finished.
