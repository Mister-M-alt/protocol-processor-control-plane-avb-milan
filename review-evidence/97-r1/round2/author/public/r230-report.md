[R230] NEGATIVE - exact head 1eb20dc4911880de10b745cc284e7dde306788b5

Reviewer R230 (cold independent external Opus), round R230-1. PR100 (`97-assert-distinct-sr-vid-fixture`) for issue #97. Base main `8452f564294300a82d56eed464276576f65f4d58`; head tree `001d20069951396c5e056c5539c2af4217310a57`. Public evidence archive `77e2fb5fdfbb4c02ed7335366733b41c279018cf/review-evidence/97-r1`. Author results are attributed to A161 and native/hosted results to A10, except where I reproduced them below. Everything I ran is in `receipts/` beside this report, and `receipts/SHA256SUMS` lists every receipt file.

## Verdict

NEGATIVE, because one MINOR finding is open: R230-M1 (Robustness, Tests).

The new permanent `fixture-guards` gate decides its verdict by matching English compiler text. Under a GCC that prints translated diagnostics, the gate rejects correct compiler behaviour. The default pp_top `make` then stops before either build, while base passes in the same environment.

- Conformance, RTL and Docs are clean at this head.
- Robustness and Tests are not clean, because of R230-M1.
- There are two optional SUGGESTIONs, R230-S1 (Docs) and R230-S2 (Docs, Tests).
- On substance the change does what issue #97 settled:
  - it refuses default-equivalent fixtures at both observable widths;
  - it restricts nothing in the product;
  - the M25 and M31 controls keep their recorded outcomes.

## Inputs read

- Issue #97: the body, with its settled scope, acceptance and decision, and four comments: A10 READY TAKEOVER (the assignment), A161 TAKEN, A161 MATERIAL DECISIONS and A161 AUTHOR HANDOFF.
- Issue #95: the body and its acceptance.
- The PR96 R223 review, including R223-S2, which is the origin of #97.
- PR100: the body and four comments: A161 REVIEW READY, A10 VALIDATION RUNNING, A10 REVIEW READY (native and hosted), and A10 INDEPENDENT REVIEW START. The PR has no review or inline comments.
- Documentation:
  - `docs/README.md` §2 (single-source rules, which bind architecture documents 01-10) and §6.
  - `hdl/README.md` rules 2-4.
  - `docs/guides/hdl-engineer.md` §6-§8.
  - `docs/guides/integrator.md:66`.
  - `01_overview.md:166` (the F01.5 row for P-SRP-DOM-DEF-VID).
  - `10_srp_engine.md:196-212` (F10.2) and `:364-367` (§11).
  - 00 REQ-NET-002 (`:410`) and REQ-SRP-004 (`:422`).
  - `.github/workflows/hdl.yml` and `scripts/run_suites.sh`.
- `tb/pp_top`:
  - `Makefile`, `README.md`, `fixture_guards.py` and `pp_top_wrap.sv:332-335`;
  - `sim_main.cpp:75-87`, the DV section (`:8242-8406`) and the build epilogue (`:9054-9072`).
- RTL:
  - `protocol_processor_top.sv:137-144,2147`;
  - `KL_srp_top.sv:80,323`;
  - `KL_srp_domain.sv:42`.
- The whole diff `8452f56..1eb20dc` (4 files, +90/-2) and the whole evidence archive.

## Identity and evidence (receipts 00, 01, 16)

- The review clone is detached at `1eb20dc`.
  - Its tree is `001d200`.
  - Its single parent, and its merge-base with base, is `8452f56`.
  - It has no tracked, untracked or ignored changes.
- The diff changes exactly four files:
  - `tb/pp_top/Makefile`, `README.md` and `sim_main.cpp`, all modified;
  - `fixture_guards.py`, new, mode 100644 (like the other `python3`-invoked helpers).
- There is no mode change, deletion or rename, and `git diff --check` is clean. Nothing under `hdl/`, `docs/`, `scripts/`, `.github/` or `syn/` changes, and neither does `pp_top_wrap.sv`.
- Author artifacts:
  - the author's `source.patch` is byte-identical to `git diff base..head`;
  - the four file hashes in `source-snapshot.json` match the head blobs.
- Evidence archive:
  - Evidence commit `8b61b6c` (the author archive linked from the PR body) has parent `8452f56`, and `77e2fb5` has parent `8b61b6c`.
  - Both touch only `review-evidence/97-r1/`.
  - All 92 MANIFEST entries match their published SHA-256, and their `path_redacted` flags are consistent.
- Manager records:
  - `manager/candidate.json` names this base, head and tree, with `clean: true`.
  - `full-native/results.json` records nine commands, all exit 0, at this head. Its `04.log` shows `PASS pp_top (1411 checks: 1411 PASS, 0 FAIL)` and `suites: 14943 checks total, 0 failing`.
  - I did not re-run these.
- Live state: PR100 is OPEN, not draft, MERGEABLE, head `1eb20dc`, base `main@8452f56`. The refs `main`, the branch, `pull/100/head` and `pull/100/merge` (`1a1f476`) match.
- Hosted runs:
  - `hdl` run 35696606554 (push, which checks out `1eb20dc`) and run 35696611002 (pull_request, which checks out merge `1a1f476`, tree `001d200`, identical to head) both succeeded.
  - All six jobs (docs-gates, suites and portability, on both events) succeeded.
  - Both suites logs show Verilator 5.050, `PASS pp_top (1411 checks: 1411 PASS, 0 FAIL)` and `suites: 14943 checks total, 0 failing`.
  - `run` depends on `fixture-guards`, so the hosted pp_top pass implies the gate passed with Verilator 5.050 and the runner's g++.

## What I ran

All work ran in scratch copies under `/tmp/r230-100-r1`: a `--no-hardlinks` clone for the head, and `git archive` exports for mutants and base.

- The outer `make` ran serially.
- Every Verilator call went through `receipts/scripts/verilator8`, which strips `-j`, `--build-jobs` and `--verilate-jobs` and pins all three at 8. `verilator-invocations.jsonl` shows 37 invocations, all capped, and the logs show `on 8 threads`.
- `TMPDIR` pointed into the scratch area, so the gate's temporary model directories could be audited.
- Toolchain: Verilator 5.052, GCC 16.2.1, Python 3.14.7 and GNU Make 4.4.1. clang++ is not installed.

| Receipt | Probe | Result |
|---|---|---|
| 02 | head `make fixture-guards` | rc 0. Default and 5A3C compile. 0002 is refused with both diagnostics and 1002 with the class-D diagnostic only. `fixture guards: 4 cases PASS`. No temp dir left behind, and the scratch clone's `git status --ignored` stays empty. |
| 03 | the same gate with `LANGUAGE=de`, with `LANGUAGE=fr`, and with `LANGUAGE=de LC_ALL=C` | de and fr: GCC prints both intended diagnostics for 0002, but the gate reports `FAIL: fixture 0002: unexpected compiler result 1` and make exits 2. `LC_ALL=C`: 4 cases PASS. See R230-M1. |
| 04 | head `make`, both builds | Gate PASS, then `[build default, 0x0002] 1391 checks, 0 failures` and `[build fixture, 0x5a3c] 20 checks, 0 failures`. The last line is `1411 checks: 1411 PASS, 0 FAIL`, and `build_tally.txt` holds `1391 0` and `20 0`. Only ignored outputs are created. |
| 05 | head `make SRP_VID_FIXTURE=0002`, then `=1002`, each with a fresh `obj_vid` | The default build still runs (1391/0). The fixture build then fails in `sim_main.o`: 0002 gives both static assertions (`sim_main.cpp:81`, `:83`) and 1002 only `:83`. make exits 2, no `obj_vid/Vpp_top_vid` exists, and no canonical line is printed. Verilator accepted the SV override and generated the model, so only the C++ bench refused. |
| 06 | six gate mutants, G1-G6, run through `make fixture-guards` (the diffs are in `*.mutation.txt`) | Every mutant fails the gate (rc 2):<br>• G1, the wire guard removed: 0002 shows the class-D diagnostic only.<br>• G2, the class-D guard removed: 0002 shows the wire diagnostic only.<br>• G3, the class-D mask widened to `0xFFFFu`: 1002 compiles.<br>• G4, the two diagnostics swapped: 1002 is refused with the wire text.<br>• G5, the guards moved outside the fixture branch: the default build is refused.<br>• G6, an unrelated error added in the fixture branch: 5A3C fails. |
| 07 | value matrix: the real `sim_main.cpp`, `-fsyntax-only`, `LC_ALL=C`, flags taken from the suite's own `make -n fixture-guards` | These compile: no override, 5A3C, 5a3c, 0003, 0000, 0FFF and 1005.<br>Refused at both widths: 0002, 2 and 10002. The last truncates to the `uint16_t` 0x0002.<br>Class-D only: 1002 and F002.<br>0005 is refused only by the existing DV4 guard. |
| 08 | head M25 (top binding line removed) and M31 (`KL_srp_top` default 2 changed to 7) | M25: default 1391/0, fixture 13 of 20 failing (every value check of DV1-DV6), make rc 2. M31: 1391/0 and 20/0, 1411 PASS. The edits are byte-identical to A161's patches and match README M25/M31. |
| 09 | base `8452f56` with M25, run with `make SRP_VID_FIXTURE=0002` and `=1002` | 0002: `1411 checks: 1411 PASS, 0 FAIL`. The fixture build is blind to the dropped binding, which is the issue #97 premise.<br>1002: only the 4 wire checks fail (DV2 x2, DV4, DV6). The other 9 value checks, on the 12-bit faces, pass despite the dropped binding. |
| 10 | clean head `make` and clean base `make`, both with `LANGUAGE=de` | Head: rc 2 at `Makefile:68: fixture-guards`, and no build directory is created. Base: 1391/0 and 20/0, `1411 checks: 1411 PASS, 0 FAIL`. |
| 11 | donor zero-tolerance lint flags on `protocol_processor_top` at the default, `16'h0002`, `16'h1002` and `16'h5A3C` | Exit 0 with 0 warnings at all four. No assertion on the VID parameter in the top, `KL_srp_top` or `KL_srp_domain`. |
| 12 | `check-links.py`, `check-matrix.py`, `gen_matrix.py --check`, `check_upc_map.py`, `make stale`, `py_compile` of the new script | All pass: 807 links; 115 REQ rows and 17 GAP findings; 86 module rows with 0 untested; UPC map PASS. The diff adds no U+2014. |
| 13 | `make -n run` with the fixture given as an environment prefix and as a make variable | The environment prefix is ignored (the recipe still carries 5A3C). The make variable gives 0002. See R230-S1. |
| 14, 15 | tool versions, job cap and locales; minimal `static_assert` under GCC's message catalogs | Only `C`, `C.utf8`, `en_US.utf8` and `POSIX` locales exist here. The stock `gcc 16.2.1` package ships 20 `gcc.mo` catalogs, de and fr among them. |
| 16 | final state of the review clone | HEAD `1eb20dc`, detached, and clean including ignored files. The 223 index entries equal the HEAD tree entries, all at stage 0. Every path has its mode's kind, executable bit and HEAD blob bytes (213 x 100644, 10 x 100755). `git fsck --full` is clean. |

## Findings

### R230-M1: MINOR (Robustness, Tests)

- **Artifacts:**
  - `tb/pp_top/fixture_guards.py:39-49`: each case runs the compiler in the caller's locale. `found` counts only error lines that also contain the English phrase `static assertion` (`:44`). Any mismatch returns 1 (`:48-52`).
  - `tb/pp_top/Makefile:54`: `fixture-guards` is a prerequisite of the default `run` target. It therefore gates every `make` of the suite, and through `scripts/run_suites.sh:30`, the sweep.
- **Requirement:**
  - Issue #97 acceptance: "default and 5A3C builds pass unchanged".
  - `hdl-engineer.md` §6: `cd tb/<suite> && make`, exit 0 = PASS.
  - The README's new contract (`:531-535`) says the gate fails only for the wrong diagnostic set or an unrelated compiler error.
- **Evidence:**
  - The `static assertion failed` prefix is a translatable GCC message. The assertion strings are not translated.
    - With `LANGUAGE=de`, GCC 16.2.1 prints `error: statische Assertion fehlgeschlagen: SRP VID fixture must differ ...`.
    - With `LANGUAGE=fr`, it prints `error: l'assertion statique a échoué : ...` (receipt 15).
  - In the real gate, GCC emits exactly the two intended diagnostics for 0002, yet the gate fails (receipt 03). The same gate passes with `LC_ALL=C`.
  - The clean head `make` exits 2 at the gate before either build under `LANGUAGE=de`. The base `make` passes 1411/0 in the same environment (receipt 10).
- **Impact:**
  - For a developer or consumer whose GCC prints translated diagnostics, the pp_top suite goes red although the compiler behaved exactly as intended. That needs a non-English `LANG`/`LANGUAGE` plus installed GCC catalogs, which the stock Arch `gcc` package ships (receipt 15).
  - Under `run_suites.sh:30`, the sweep then counts `FAIL pp_top`. I did not run the sweep.
  - The message blames an "unexpected compiler result".
  - This environmental dependency is new; before this PR nothing in the suite parsed compiler text.
  - The gate fails closed: it can never pass a default-equivalent fixture.
  - Hosted CI, A10's native run and my English runs are unaffected.
- **Required outcome:** make the gate's verdict independent of the compiler's message language. Two ways to do that:
  - run the compile with `LC_ALL=C`; receipt 03 shows this suffices even with `LANGUAGE=de` set;
  - classify error lines by the two unique assertion messages, without requiring the English prefix.

  Keep the exact expected sets, the error-count check and the fail-closed behaviour.
- **Verification:**
  - `LANGUAGE=de make fixture-guards` and `LANGUAGE=fr make fixture-guards`, with `LC_ALL` unset and GCC catalogs installed, print the four expected outcomes and exit 0.
  - The G1 and G2 removal mutants still fail.
  - pp_top `make` gives 1411 PASS, and the hosted suites stay green.

### R230-S1: SUGGESTION (Docs)

- **Where:** `tb/pp_top/README.md:525`: "`SRP_VID_FIXTURE=0002` fails both assertions; `SRP_VID_FIXTURE=1002` fails ...".
- **Evidence:** `Makefile:15` assigns `SRP_VID_FIXTURE`, and a makefile assignment overrides an environment variable. As a shell prefix (`SRP_VID_FIXTURE=0002 make`) the value is ignored and the recipe keeps 5A3C. As a make variable (`make SRP_VID_FIXTURE=0002`) it applies (receipt 13).
- **Impact:** none on the build. A reader who uses the prefix form sees a green 5A3C run instead of the documented refusal.
- **Suggested outcome, optional:** write `make SRP_VID_FIXTURE=0002` and `make SRP_VID_FIXTURE=1002`.
- **Verification:** a reading, and `make -n run` as in receipt 13.

### R230-S2: SUGGESTION (Docs, Tests)

- **Where:** the README mutation record (`tb/pp_top/README.md:182-232`) and `hdl-engineer.md:186-189` ("If you add checks, add to it").
- **Evidence:** the PR adds two compile-time guards and a four-case gate. The proof that removing either guard turns `make fixture-guards` red exists in A161's receipts 08/09 and in my G1-G6 (receipt 06). It is not in the tree, and the evidence README says the archive branch must never be merged.
- **Suggested outcome, optional:** record the guard mutants in the mutation record, with their gate outcome and date:
  - wire guard removed;
  - class-D guard removed;
  - class-D mask widened to 16 bits.
- **Verification:** a README diff, and rerunning the gate mutants as in receipt 06.

## Lens results

- **Conformance: PASS, clean.**
  - Artifacts:
    - `sim_main.cpp:78-87` and `:8257-8262`; DV1-DV6 (`:8298-8405`), unchanged.
    - `01_overview.md:166`, `integrator.md:66` and `10_srp_engine.md:202,364-367`.
    - REQ-SRP-004 and REQ-NET-002.
    - Receipts 04, 05, 07 and 09.
  - The product behaviour graded by the bench is unchanged: 1391/0 by default and 20/0 with the fixture, the same counts as at PR96.
  - The bench sees the parameter at exactly two widths:
    - 16 bits only in the MSRP SRclassVID (DV2 x2, DV4 Lv, DV6);
    - 12 bits on the class-D ports, snapshot word 10, GET_DOMAIN and ACMP `stream_vlan_id`.
  - The two guards assert distinctness from 2 at exactly those widths, after conversion to the existing `uint16_t` expectation (`0x10002` is refused at both widths).
  - Receipt 09 reproduces why each guard is needed. At base with the binding dropped:
    - fixture 0002 is fully blind (1411 PASS);
    - fixture 1002 leaves all 9 twelve-bit value checks passing.
  - At head, both values are refused at compile time, before any simulation (receipt 05).
  - The policy is consistent with F01.5 ("any other value is a verification fixture ..., not a product profile") and with the integrator guide ("Keep it at 2 in a product build"). The bench policy is stricter than F01.5, not in conflict with it.
  - No normative conflict found.
- **RTL: PASS, clean.**
  - Artifacts:
    - `protocol_processor_top.sv:144,2147`, `KL_srp_top.sv:80,323`, `KL_srp_domain.sv:42` and `pp_top_wrap.sv:332-335`.
    - Receipts 00, 05, 08 and 11.
  - No RTL or wrap byte changes.
  - The refusal is bench-only, not a product-parameter restriction:
    - the VID parameter carries no RTL assertion;
    - the product top lints clean at the default and at 0002, 1002 and 5A3C;
    - in the refused builds Verilator accepted the SV override and only `sim_main.cpp` failed.
  - The guard's reference value 2 equals all three RTL defaults that a dropped binding can fall back to (top `:144`, `KL_srp_top:80`, `KL_srp_domain:42`).
  - M25 and M31 keep their recorded outcomes.
- **Robustness: NOT CLEAN, R230-M1 open.**
  - Artifacts: `fixture_guards.py:21-56`, `Makefile:54,65-69,80`, and receipts 02, 03, 05, 06, 10 and 14.
  - Checked clean:
    - Every gate mutant G1-G6 fails closed, including an unrelated error (G6) and a guard leaking into the no-override build (G5).
    - The temporary model directory is removed after every passing and failing run: none of the 16 gate runs left one in the scratch `TMPDIR`.
    - The gate writes nothing to the source tree and does not touch `obj_dir` or `obj_vid`.
    - A refused fixture build leaves no fixture executable and no canonical tally line.
    - The two builds' Verilator warning count is unchanged at 46 for head and base; the gate's own verilation repeats the bench's 23 existing warnings.
  - Open: R230-M1.
- **Tests: NOT CLEAN, R230-M1 open; R230-S2 optional.**
  - Artifacts: `sim_main.cpp:81-84`, `fixture_guards.py:24-26,43-49`, `Makefile:54`, `run_suites.sh:30,34`, and receipts 02 and 04-09.
  - The gate runs in every `make`, and so in the sweep and in the hosted suites.
  - Its four cases match the README, and it detects each change:
    - removal of either guard (G1, G2);
    - width collapse (G3);
    - swapped diagnostics (G4);
    - scope leak (G5);
    - an unrelated error (G6).
  - The canonical tally is unchanged (1411 = 1391 + 20, last line). The gate's own summary line does not match the tally pattern.
  - The DV runtime checks are unchanged, and M25/M31 reproduce.
  - Open: R230-M1.
- **Docs: PASS, clean; R230-S1 and R230-S2 optional.**
  - Artifacts:
    - `README.md:19-21,507-538`.
    - The new comments: `Makefile:65-66`, the `fixture_guards.py:3` docstring and `sim_main.cpp:80`.
    - The PR body; receipts 02-07, 12 and 13.
  - Every new README statement matches a receipt:
    - the widths;
    - the 0002 and 1002 refusal sets;
    - "verification-bench assertions only";
    - the gate's four cases;
    - unrelated errors fail it;
    - no source or `obj_dir`/`obj_vid` reuse;
    - it does not replace the builds or the M25-M31 controls.
  - The docs gates pass.
  - No architecture or guide text needed a change: the product contract is unchanged, and the fixture policy lives with the bench.

## Issue #97 acceptance

- **Compile-time refusal at either observable width; default, 5A3C, RTL, runtime policy and shipping values preserved:** met (receipts 00, 04, 05, 07, 11).
- **Default and 5A3C builds pass unchanged:** met with English or C diagnostics, which covers receipt 04, A161, A10 and hosted. It does not hold under a localized GCC (receipt 10). That is R230-M1.
- **0002, and a value whose low 12 bits are 2, fail compilation with a clear diagnostic:** met for 0002, 1002 and F002 (receipts 05, 07).
- **Missing-binding and child-default controls keep their recorded outcomes:** met (receipt 08).
- **Focused pp_top and documented donor gates:**
  - I reproduced the focused pp_top runs and the Python docs gates.
  - The full native bar and the hosted bar are A10's. I verified their identity and did not re-run them.
- **Decision: distinctness at both widths, no new product-parameter restriction:** met (receipts 07, 11).
- **Two independent reviews and merge containment:** this review is NEGATIVE. R229 and containment are outside it.

## Reviewer-owned lens, round and head ledger

| Round | Head | Lens | Result | Open findings (any severity) | Primary artifacts |
|---|---|---|---|---|---|
| R230-1 | 1eb20dc4911880de10b745cc284e7dde306788b5 | Conformance | PASS, clean | none | `sim_main.cpp:78-87,8257-8405`; F01.5 `01_overview.md:166`; receipts 04, 05, 07, 09 |
| R230-1 | 1eb20dc4911880de10b745cc284e7dde306788b5 | RTL | PASS, clean | none | `protocol_processor_top.sv:144,2147`; `KL_srp_top.sv:80`; `KL_srp_domain.sv:42`; receipts 00, 08, 11 |
| R230-1 | 1eb20dc4911880de10b745cc284e7dde306788b5 | Robustness | NOT CLEAN | R230-M1 (MINOR) | `fixture_guards.py:39-49`; `Makefile:54`; receipts 03, 06, 10 |
| R230-1 | 1eb20dc4911880de10b745cc284e7dde306788b5 | Tests | NOT CLEAN | R230-M1 (MINOR); R230-S2 (SUGGESTION) | `fixture_guards.py:24-49`; `sim_main.cpp:81-84`; receipts 02, 04-09 |
| R230-1 | 1eb20dc4911880de10b745cc284e7dde306788b5 | Docs | PASS, clean | R230-S1, R230-S2 (SUGGESTION) | `tb/pp_top/README.md:523-538`; receipts 12, 13 |

Open MINOR: R230-M1, under Robustness and Tests. No BLOCKER or MAJOR.

## Existing unrelated observations (not findings, not counted against any lens)

- **O1 (existing, outside the #97 acceptance):** the DV4 guard `static_assert(SRP_DEF_VID != ADOPT_VID, ...)` (`sim_main.cpp:8262`) compares 16 bits only.
  - A fixture such as 0x1005 passes it and both new guards (receipt 07). Its 12-bit default then equals ADOPT_VID 5, so DV4's class-D VID cannot visibly move.
  - DV4 also grades the ADOPTED flag, the DOMAIN_CHANGE count and the wire Lv/New frame.
  - I did not simulate 0x1005. Issue #97 settled distinctness from the product default only, so I raise nothing.
- **O2:** PR96's R223-S1 (the F10.2 arc label, `10_srp_engine.md:205`), R223-X1 (the Yosys `tops` list, in PR26's portability lane) and R223-X2 remain as recorded there. PR100 touches none of them, and none is in its scope.

## Limitations

- **Not re-run by me:**
  - the complete `scripts/run_suites.sh` sweep;
  - `scripts/lint_hdl.sh` over every module;
  - `make check` with its mermaid and wavedrom tooling;
  - `syn/yosys/run.sh`;
  - `make -C tb/nvm_port figures`;
  - any hosted, Docker or act workflow.

  For these I rely on A10's full-native receipts and the two hosted runs, whose identity and hashes I verified above.
- **Toolchain:**
  - The local Verilator is 5.052. The Verilator 5.050 result comes from the hosted logs.
  - clang++ is not installed, so I exercised the gate with GCC 16.2.1 only.
- **Locale reproduction:** I reproduced the localized output with `LANGUAGE=de` and `LANGUAGE=fr` under `LANG=en_US.UTF-8`. Only C and en_US locales are generated on this host, so I did not test a native `LANG=de_DE.UTF-8`.
- **Specifications:** the Milan v1.2 and IEEE texts are not in the repository. I judged Conformance against:
  - the issue texts;
  - the 00 compliance rows;
  - F01.5, F10.2 and 10 §11;
  - the integrator guide;
  - executable behaviour.
- **Mutation coverage:** among the recorded controls I re-ran M25 and M31, as the acceptance names. I did not re-run M26-M30.
- **Host and scratch:**
  - The host is shared. Builds ran one at a time with Verilator capped at 8, so wall-clock times are not reference figures.
  - All scratch lived under `/tmp/r230-100-r1`.
  - Nothing was committed, pushed or published.
  - The review clone is unchanged (receipt 16).

## Pending merge obligations (separate from the verdict)

1. Resolve R230-M1 on a new exact head, then get an independent re-review.
2. R229's independent review.
3. Two positive reviews with clean five-lens coverage.
4. Current-candidate validation, merge, actual-tree containment and post-merge hosted validation.

Parent submodule integration remains a separate lane.

R230-1 FINISHED
