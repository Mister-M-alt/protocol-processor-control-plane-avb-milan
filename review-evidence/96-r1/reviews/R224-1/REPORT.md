[R224] POSITIVE - exact head ea93023fbdd31cbf718da0d8f1aab4d750bfd21a

Independent external review R224-1 of Mister-M-alt/protocol-processor-control-plane-avb-milan PR96 / issue95, against base `424c688fa2205b934a7689a58f2aa766420f2326`. No in-scope BLOCKER, MAJOR, MINOR, or SUGGESTION findings. All five applicable lenses are clean for this change.

The review reconstructed the contract from [issue95 and its public decisions](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/95), the [parent #400 pre-implementation decision](https://github.com/kebag-logic/milan-fpga/issues/400#issuecomment-5770922734), and [A10's exact-head REVIEW READY](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/pull/96#issuecomment-5771668737). I read the pinned documentation authority, HDL rules, relevant requirements, parameter registry, interfaces, SRP state semantics, verification documentation, complete eight-file diff, and single-commit history. The isolated checkout contained no AGENTS.md or CONTRIBUTING.md; `$WORKSPACE_HOME/.codex/RTK.md` applied. No author/reviewer collaboration or private reasoning material was used.

PASS Conformance — `docs/00_MILAN_COMPLIANCE_REVIEW.md:410,422`, `docs/architecture/01_overview.md:166`, and `hdl/top/protocol_processor_top.sv:144,2147`: checked issue95 AC1–AC3 against REQ-NET-002 / REQ-SRP-004. The named parameter is 16 bits, defaults to 2, and directly drives the child's existing default parameter. The product policy remains 2; `0x5A3C` is explicitly a verification fixture. Runtime adoption and Link Down/Up remain effective. The settled contract does not require rejecting alternate fixtures at donor elaboration; shipping-value refusal belongs to parent #400. No stronger acceptance condition was introduced.

PASS RTL — `hdl/top/protocol_processor_top.sv:144,2147`, `hdl/srp/KL_srp_top.sv:323`, and `hdl/srp/KL_srp_domain.sv`: traced `SRP_DOM_DEF_VID_P` through `u_srp.DOM_DEF_VID_P` to `u_domain.DEF_VID_P`, including reset, Link-Up declaration, and Link-Down restoration. The complete production RTL diff is the top declaration/comment and named child binding; `hdl/srp/` is byte-identical to base. The Domain state and wire value remain 16 bits; `class_a_vid_o` intentionally selects `[11:0]`. The existing priority, timers, adoption, MVRP, packet format, and microprogram implementation are unchanged. Reviewer mutation/control receipts below independently exercise this physical path.

PASS Robustness — `tb/pp_top/sim_main.cpp:8237–8402`, `tb/srp_encoder/sim_main.cpp:578–707`, `srp_encoder.log`, and `probes/warm_reset_and_isolation/`: checked cold reset, silence before link and while down, differing two-class Domain adoption, exactly one change strobe, restoration, and re-declaration. Existing D5/D7/D8 checks cover identical-domain no-op, non-covering/empty vectors, and preservation of adoption across periodic/LeaveAll events. Reviewer probes additionally passed all 20 DV checks at both `0x0000` and `0xFFFF`, and 64 checks across two fresh models with reset after adoption. These boundary values test transport width only; they are not proposed shipping VLAN profiles.

PASS Tests — `tb/pp_top/Makefile:53–63`, `tb/pp_top/pp_top_wrap.sv:332–335,531–534`, `tb/pp_top/sim_main.cpp:9042–9072`, `pp_top.log`, `binding-probe-results.json`, and `tally-probe-results.json`: checked actual default and alternate builds, independent expected values, model isolation, mutation sensitivity, exit propagation, and aggregation. The first build does not override the new top parameter; the second defines the fixture in both SV and C++. Expectations come from the requested build value, never DUT readback. Full-frame comparisons and the explicit wire decoder observe all 16 bits; ports, snapshot word 10, GET_DOMAIN, and GET_TX_STATE correctly observe the low 12 bits. DV checks accrue to the owning harness's final tally despite running on a separate model.

PASS Docs — `docs/README.md` §2, F01.5, F10.2 / SRP §11, integrator §§2/8, and `tb/pp_top/README.md` DV / M25–M31: checked parameter identity, width, default, startup/link semantics, adoption, fixture-only policy, observations, and evidence claims against implementation. The architecture's value is registered in F01.5; F10.2 and §11 cite the P-ID. The guide and bench README are outside the explicitly stated architecture-only single-source rule. Local receipts show 807 links, 115 requirement rows / 17 recorded gaps, 86 module-matrix rows / zero untested, 41 Mermaid / 18 WaveDrom syntax blocks, and staleness checks passing. No new normative documentation conflict was found.

Reviewer-owned executable receipts:

| Run | Result |
|---|---|
| Actual `tb/pp_top` Makefile, default plus `0x5A3C` | 1,391 + 20 = 1,411 checks, zero failures; exactly one canonical tally |
| Actual `tb/srp_top` / `tb/srp_encoder` | 235 / 180 checks, zero failures |
| Missing top-to-child binding, fixture build | Exit 1; 13 failures / 20 checks |
| Binding truncated to 12 bits, fixture build | Exit 1; four failures / 20 checks, all wire observations; 12-bit observations pass |
| Child default changed to 7, binding intact, fixture build | Exit 0; 20 checks, zero failures |
| Width boundaries `0x0000` / `0xFFFF` | Exit 0; 20 checks each, zero failures |
| Repeated fresh-model / reset-after-adoption probe | Exit 0; 64 checks, zero failures |
| Synthetic Makefile controls, without RTL compilation | Correct sum; either failed binary stops the recipe; missing or extra tally rows fail; stale rows are cleared |

The focused suites ran in this review checkout. Mutations and extra probes ran only in separate `/tmp/r224-pr96-*` copies created from the pinned Git archive; no product fixes were written. `probe_binding.py` and `probe_tally.py` reproduce the probes, with per-case diffs, commands, logs, and statuses under `probes/` and `tally-probes/`. All ten real Verilator invocations used effective `-j 8`, with builds sequential and outer make `-j1`; `verilator-invocations.jsonl` records the executed arguments. Local Verilator was 5.052. One initial synthetic probe had a reviewer-generated Python escaping error, corrected before its seven cases passed; this is recorded in `reviewer-probe-setup.txt` and is not a product failure.

Published evidence was verified separately. At [immutable evidence commit f41a2a13615fe321def5d896d0b1035695ed87b5](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/tree/f41a2a13615fe321def5d896d0b1035695ed87b5/review-evidence/96-r1), all 65 downloaded Git blob identities and all 63 manifest-listed published SHA256 hashes matched. A157's M25–M31 diffs and run logs agree with their reported outcomes; the literal-binding, wrong-parameter, changed-top-default, and removed-wrapper-override cases remain attributed to A157, not represented as my executions. The published failed initial timer_map command was not counted as passing evidence.

A10's full native receipts record all nine commands successful, including 37 linted modules, all 30 donor suites, documentation gates, Yosys, and NVM figures. I recomputed the suite inventory and total: 14,943 checks, zero failing suites, including pp_top's complete 1,411. I did not rerun the full donor sweep, full lint, timer-map shape sweep, Yosys, NVM figures, or any parent sweep. GitHub API records independently confirm successful docs-gates, suites, and portability jobs for both [push run 35689949228](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/actions/runs/35689949228) and [PR run 35689952257](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/actions/runs/35689952257), each identifying the reviewed head. Downloaded push logs also confirm checkout identity, Verilator 5.050, pp_top 1,411, total 14,943, and the documentation/portability results. Those hosted executions are not reviewer reruns.

`evidence-identity.json` records an independent `git merge-tree --write-tree` calculation against the specified base: candidate and head trees both equal `ec259f379560863e6ea49c6043353f0c11fe714d`, matching A10's candidate receipt. `final-integrity.json` and before/after manifests confirm all 222 tracked files retain identical bytes, source kinds, filesystem modes, and index entries. This includes the known tracked `tb/dyn_state/shape_tally.txt`; nothing was blindly restored. The pp_top generated ROMs match fresh generator output. Git status is clean and HEAD remains exactly `ea93023fbdd31cbf718da0d8f1aab4d750bfd21a`.

Reviewer-owned coverage ledger, with no inherited approval:

| Reviewer | Round | Exact head | Lens | Coverage | Open in-scope findings |
|---|---|---|---|---|---|
| R224 | R224-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | Conformance | PASS | None |
| R224 | R224-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | RTL | PASS | None |
| R224 | R224-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | Robustness | PASS | None |
| R224 | R224-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | Tests | PASS | None |
| R224 | R224-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | Docs | PASS | None |

Limitations and scope: this is a review of issue95's interface extension, not whole-product certification. No existing unrelated defect was newly established, and existing repository gaps are not declared resolved by this verdict. DV3 observes existing implementation behavior where F05.11 leaves non-declaring stream fields undefined; it is not a new normative requirement. No in-scope defect was deferred or waived by another issue. Python WaveDrom was unavailable locally, so I did not invoke full `make check` or install dependencies; its render check is supported by verified A10 and hosted receipts, while the listed local documentation checks were executed. Standards conclusions use the public issue decisions and pinned requirement documentation; no separate standards PDF audit is claimed.

This supplies R224's one independent positive. Issue95 AC4 still requires the other independent positive and the manager's merge/containment workflow before the parent pin changes. No public write, commit, push, merge, hardware operation, privileged operation, dependency installation, or subagent was performed.

R224-1 FINISHED
