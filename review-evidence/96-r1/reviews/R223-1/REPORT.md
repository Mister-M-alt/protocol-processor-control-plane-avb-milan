[R223] POSITIVE - exact head ea93023fbdd31cbf718da0d8f1aab4d750bfd21a

Reviewer R223 (cold independent internal Opus), round R223-1. PR96 (`95-expose-default-sr-vid`) for issue #95; base main `424c688fa2205b934a7689a58f2aa766420f2326`; head tree `ec259f379560863e6ea49c6043353f0c11fe714d`. Public evidence `f41a2a13615fe321def5d896d0b1035695ed87b5/review-evidence/96-r1`: author receipts are attributed to A157 and manager receipts to A10, except where reproduced below. Everything I ran is in `receipts/` beside this report; `receipts/SHA256SUMS` lists every receipt file.

## Verdict

POSITIVE. No BLOCKER, MAJOR or MINOR finding. Two SUGGESTIONs, R223-S1 and R223-S2, neither blocking. All five lenses (Conformance, RTL, Robustness, Tests, Docs) are clean at the exact head. Issue #95 acceptance 1 to 3 is met. Acceptance 4 still needs this review, R224's review, and the manager's merge and containment bar.

## Inputs read

- Issue #95: the body (settled acceptance and decision) and three comments: A10 READY TAKEOVER, A157 TAKEN, A157 MATERIAL DECISIONS.
- PR96: the body and four comments: A157 REVIEW READY, A10 VALIDATION AND REVIEW SCHEDULE, A10 REVIEW READY, A10 INDEPENDENT REVIEW ROUND START.
- Documentation:
  - docs/README.md §2 (single-source rule "Parameter values only in F01.5", scoped to architecture documents 01-10) and §3.
  - hdl/README.md rules 2 to 4.
  - 01 §7 F01.5; 10 §6.1 F10.2 and §11; 02 F02.10.
  - Integrator guide §2 and §8.
  - 00 REQ-SRP-004 and REQ-NET-002.
- RTL: `hdl/top/protocol_processor_top.sv`, `hdl/srp/KL_srp_top.sv` and `hdl/srp/KL_srp_domain.sv`, plus every consumer of the class-A VID in the top.
- The full diff `424c688..ea93023` (8 files, +332/-6) and the whole evidence tree.

## Identity and evidence checks (receipts 00, 01, 10)

- The review clone is detached at `ea93023fbdd31cbf718da0d8f1aab4d750bfd21a`.
  - Its parent, and its merge-base with main, is `424c688fa2205b934a7689a58f2aa766420f2326`.
  - Its tree `ec259f37…` equals A10's `head_tree` and `candidate_tree` in `manager/candidate.json`.
  - The PR API reports headRefOid `ea93023…`, baseRefOid `424c688…`, OPEN and MERGEABLE.
- `git diff --check 424c688..ea93023` is clean. `git diff --summary` is empty: no mode change, create or delete. All eight changed paths are mode 100644 at both base and head.
- I fetched evidence commit `f41a2a1…` into a scratch repository.
  - All 63 MANIFEST entries match their published SHA-256, and every `path_redacted` flag is consistent.
  - A10's `full-native/results.json` records exit code 0 for all nine commands at this head.
  - Its `04.log` shows `PASS pp_top (1411 checks: 1411 PASS, 0 FAIL)` and 14943 checks in total.
  - I did not re-run those manager gates.
- Hosted `hdl` runs 35689949228 (push) and 35689952257 (pull_request) both have headSha `ea93023…`, and all three jobs (docs-gates, suites, portability) succeeded. The pull_request suites log shows:
  - Verilator 5.050, the CI pin;
  - `PASS pp_top (1411 checks: 1411 PASS, 0 FAIL)`;
  - `suites: 14943 checks total, 0 failing`.

## What I ran

All runs used scratch copies under `/tmp/r223-96-r1`, made with `git archive`. The toolchain was Verilator 5.052, Yosys 0.66 and sv2v 0.0.13. Builds ran one at a time. Compile jobs were capped at 8 by `receipts/scripts/verilator8` (`--build-jobs 8 --verilate-jobs 8`). Receipt 10 shows the resulting inner `make -j 8` and `on 8 threads` in all 25 of my Verilator builds.

| Receipt | Probe | Result |
|---|---|---|
| 02 | head `tb/pp_top` `make`, both builds | Default build: `[build default, SRP_DOM_DEF_VID_P 0x0002] 1391 checks, 0 failures`. Fixture build: `[build fixture, SRP_DOM_DEF_VID_P 0x5a3c] 20 checks, 0 failures`. Last line `1411 checks: 1411 PASS, 0 FAIL`; rc 0. `obj_dir/build_tally.txt` holds `1391 0` and `20 0`. |
| 03 | base bench (`424c688` `tb/pp_top`) on the head `hdl/` | `1371 checks: 1371 PASS, 0 FAIL`. Every pre-existing check sees the default behaviour unchanged. |
| 04 | head bench on the base `hdl/` (issue reproduction) | The default build passes 1391/0: by design it cannot see the missing binding. The fixture build stops with `%Error-PINNOTFOUND: pp_top_wrap.sv:334:8: Parameter not found: 'SRP_DOM_DEF_VID_P'` and make exits 2, so the suite fails on the pre-fix RTL. |
| 04b-04d | base `tb/pp_top` alone; warning sets compared | Base: 1371/0. Head adds no Verilator warning. It removes base's four PINMISSING warnings, for `srp_class_a_prio_o`, `srp_class_a_vid_o`, `srp_domain_adopted_o` and `srp_domain_change_o`, which the wrap now connects. |
| 05 | the Makefile's tally `awk` on synthetic tally files | Two lines give the canonical sum. Zero, one or three lines give `FAIL: N build tallies, expected 2`, rc 1. |
| 06 | `check-links.py`, `check-matrix.py`, `gen_matrix.py --check` and `check_upc_map.py` on head | All OK: 807 links; 115 REQ rows; 86 rows with 0 untested; UPC map PASS. |
| 07 | sv2v, then Yosys `hierarchy -check -top protocol_processor_top` | Head at default: `$paramod\KL_srp_domain\DEF_PRIO_P=8'00000011\DEF_VID_P=16'0000000000000010`, identical to base. Head with `-chparam SRP_DOM_DEF_VID_P 23100`: `…\DEF_VID_P=16'0101101000111100`, so all 16 bits of 0x5A3C reach the Domain FSM. Base with the same chparam: `ERROR: Can't find object for defparam`. sv2v carries `parameter [15:0] SRP_DOM_DEF_VID_P = 16'd2` and `.DOM_DEF_VID_P(SRP_DOM_DEF_VID_P)` through unchanged. |
| 08 | the donor lint flags (`-Wall`, zero tolerance) | 0 warnings for `protocol_processor_top` at its default and with `-GSRP_DOM_DEF_VID_P=16'h5A3C`. 0 warnings for `KL_srp_top`. |
| 09 | 15 mutants through `receipts/scripts/rmut.sh` | See the table below. Each mutant is a one-line edit in its own scratch copy. The runner refuses unless exactly one input differs from head, and takes its build commands from `make -n run`. |

Mutation results. Each build's DV section has 20 checks, 13 of which compare values. A dash means that build was not run for that mutant.

| ID | Planted change | Default build | Fixture build | A157 record |
|---|---|---|---|---|
| R00 | control: no edit | - | 0/20 | - |
| R25 | top binding line removed | 0/1391 | 13/20 | M25, identical |
| R26 | top bound to `16'd2` | - | 13/20 | M26, identical |
| R27 | top bound to `16'(SRP_DOM_DEF_VID_P[11:0])` | - | 4/20: DV2 x2, DV4, DV6 (the 16-bit wire checks) | M27, identical |
| R28 | `.DOM_DEF_PRIO_P (SRP_DOM_DEF_VID_P[7:0])`, VID left unbound | 16/1391 | 13/20 | M28, identical |
| R29 | top default changed to 3 | 18/1391 | 0/20 | M29, identical |
| R30 | wrap fixture override removed | - | 13/20 | M30, identical |
| R31 | control: child default changed to 7, binding intact | 0/1391 | 0/20 | M31, identical |
| R32 | new: top bound to `16'(SRP_DOM_DEF_VID_P[7:0])` | - | 13/20 | - |
| R33 | new: top bound byte-swapped | - | 13/20 | - |
| R34 | new: top parameter narrowed to `logic [11:0]` | 0/1391 | 4/20 (the wire checks) | - |
| R35 | new: `KL_srp_domain` reset loads `16'd2` | - | 6/20: DV1 x3, DV2 class-D, DV3, DV4 | - |
| R36 | new: `KL_srp_domain` LINK_DOWN revert loads `16'd2` | - | 4/20: DV5 x3, DV6 class-D | - |
| R37 | new: `KL_srp_domain` LINK_UP declaration uses `16'd2` | - | 3/20: DV2 x2, DV6 | - |
| R38 | new: `KL_srp_top` inner binding is `.DEF_VID_P (16'd2)` | - | 13/20 | - |

## Lens results

- **Conformance: PASS.**
  - Artifacts:
    - `hdl/srp/KL_srp_domain.sv:113-193`, unchanged. The reset (:116), the LINK_DOWN revert (:155-165) and the LINK_UP declaration (:166-171) all read `DEF_VID_P`.
    - `hdl/top/protocol_processor_top.sv:144,2147`.
    - 00 REQ-SRP-004 and REQ-NET-002 (Milan §4.2.7.2.1): priority 3 and default VID 2 at startup and link-up, with adoption and re-declaration of a differing Class A Domain.
    - Receipts 02, 03 and 07.
  - The product default stays 2, both at the top and in the child.
  - S1, DV2 and DV6 grade `New {6,3,2}` byte-exact at startup and LINK_UP.
  - A certified two-class bridge Domain `{5,2,5}` with NumberOfValues 2 is still adopted over both 2 and 0x5A3C: DV4 in both builds, and S8.
  - LINK_DOWN reverts to the default (DV5).
  - `hdl/srp/` is untouched, so no timer, priority, adoption, MVRP, frame-format or microprogram change.
  - All 1371 pre-existing checks pass on the head RTL.
  - DV3 grades the talker's `stream_vlan_id` for a source that is not declaring. F05.11 leaves that value undefined, as the PR states; the check shows the talker reads the same class-D VID.
  - Non-2 values are documented as verification-only, which matches the settled decision.
  - No normative conflict found.
- **RTL: PASS.**
  - Artifacts:
    - `protocol_processor_top.sv:137-144`: `parameter logic [15:0] SRP_DOM_DEF_VID_P = 16'd2`. It sits before the derived localparams, and its banner cites the P-ID and F01.5.
    - `protocol_processor_top.sv:2147`: an explicit named binding, `.DOM_DEF_VID_P (SRP_DOM_DEF_VID_P)`, at the same 16-bit width as `KL_srp_top.sv:80`.
    - `hdl/srp/` is unchanged.
  - Adding a parameter mid-list is safe. The wrap is the only instantiation of the top in the tree and uses named overrides; `tb/timer_map` overrides by name with `-G`.
  - No other default-VID source exists in `hdl/`. The only `16'd2` VID literals are the three parameter defaults, at `KL_srp_domain.sv:42`, `KL_srp_top.sv:80` and `protocol_processor_top.sv:144`.
  - Yosys derives `DEF_VID_P` = 2 at the default and 0x5A3C with the chparam, and base refuses the chparam (receipt 07). Lint is clean at both values (receipt 08).
  - Every mutant that breaks the binding path is detected: R25-R28, R32-R34 and R38.
- **Robustness: PASS.**
  - Artifacts:
    - `tb/pp_top/Makefile:54-63`: the tally file is reset, both builds run, and `awk` requires `NR == 2`.
    - `tb/pp_top/sim_main.cpp:9062-9071`: each build prints its own line, and the binary exits 1 on any failure or when it cannot record its tally.
    - Receipts 04 and 05.
  - The suite fails closed on the pre-fix RTL (PINNOTFOUND, make exit 2).
  - A missing or extra build tally fails the suite.
  - The canonical line comes last, and it is the only line matching `run_suites.sh`'s pattern, which reads the last match (`tail -1`).
  - `.gitignore`'s `obj_*/` covers `obj_vid/` and `obj_dir/`, and `clean` removes `obj_vid`.
  - The fixture value gives each plausible binding fault a distinct reading (R25-R28, R32-R34):
    - missing binding or a literal: 2;
    - 8-bit or 12-bit truncation: 0x003C or 0x0A3C;
    - byte swap: 0x3C5A;
    - routed to the priority parameter: priority reads 4.
  - The value is synthesis-time only: no runtime register and no clock crossing.
- **Tests: PASS.**
  - Artifacts:
    - `sim_main.cpp:8237-8401` (section DV), `:78-82`, `:8452` and `:9049-9057`.
    - `pp_top_wrap.sv:332-335`.
    - Receipts 02-04 and 09.
  - DV drives the real top on a fresh model, the same pattern as the existing `InternalMaapPhase`.
    - All DUT traffic in DV goes through `h2` and `d2`.
    - Its CHECKs count into the main tally.
  - Coverage in both builds:
    - reset (DV1);
    - LINK_UP declaration (DV2);
    - the controller-visible talker VLAN (DV3);
    - adoption (DV4);
    - LINK_DOWN revert (DV5);
    - re-declaration at the next LINK_UP (DV6).
  - The expected value comes from the same Makefile variable as the wrap's define. It is never read back from the DUT, and the wrap overrides nothing in the default build.
  - Mutation sensitivity is reproduced: R25-R31 match A157's M25-M31 exactly.
  - I extended it:
    - R35-R37 show that each of the Domain FSM's three reads of the parameter is graded on its own: reset, LINK_UP declaration and LINK_DOWN revert.
    - R38 shows the child's inner binding is covered.
    - The controls R00 and R31 stay green.
  - The default build alone cannot see the binding (R25: default 0/1391). That is the stated reason for the second build, and the second build detects it.
  - Open item: R223-S2, a SUGGESTION.
- **Docs: PASS.**
  - Artifacts:
    - `docs/architecture/01_overview.md:166` (the F01.5 row).
    - `10_srp_engine.md:202` (F10.2 now cites the P-ID) and `:366-368` (§11).
    - `docs/guides/integrator.md:66` (§2) and `:260` (§8).
    - `tb/pp_top/README.md:19-21, 164-168, 210-216, 229-232, 493-543`.
    - Receipt 06.
  - Among architecture documents 01-10, the value 2 now appears only in F01.5, as docs/README §2 requires. 00 is exempt from that rule, and the operator guide's "VLAN 2" holds for every product build.
  - The P-ID mirrors the RTL name, as the other F01.5 rows do.
  - The integrator text matches the RTL:
    - the default is declared at start-up and on every link-up;
    - link-down restores both defaults;
    - a bridge's Domain is still adopted.
  - The README's DV and M25-M31 statements match the counts I reproduced.
  - The links, matrix and module-matrix gates pass.
  - Open item: R223-S1, a SUGGESTION.

## Findings

### R223-S1: SUGGESTION (Docs)
- Where: `docs/architecture/10_srp_engine.md:205`, the F10.2 arc `ADOPTED --> DEFAULTS: LINK_DOWN then LINK_UP / back to defaults`. The PR did not change this wording.
- Requirement: F10.2 is the normative home of the Domain FSM (docs/README §3). Every other statement puts the revert at LINK_DOWN:
  - this PR's new F01.5 row (`01_overview.md:166`, which cites F10.2);
  - 10 §11 (`:366-368`);
  - the integrator guide (`:66`, `:260`);
  - the RTL (`KL_srp_domain.sv:155-165`);
  - DV5, which grades it.
- Impact: none on the product. A reader of F10.2 alone could put the revert and its DOMAIN_CHANGE strobe at LINK_UP rather than LINK_DOWN. The class-D outputs would then differ while the link is down.
- Suggested outcome, optional and not required for #95: relabel the arc so the revert happens at LINK_DOWN and the re-declaration at LINK_UP, in this PR or a follow-up.
- Verification: `make check` (mermaid lint and links), and a reading of F10.2 against `KL_srp_domain.sv:155-171`.

### R223-S2: SUGGESTION (Tests, Robustness)
- Where: `tb/pp_top/Makefile:15` (`SRP_VID_FIXTURE = 5A3C`) and `tb/pp_top/sim_main.cpp:78-79`.
- Requirement: issue #95 AC2 needs a distinct verification-only value. The pinned 0x5A3C meets it: every binding mutant, R25-R38, is detected.
- Impact: none at the pinned value. Distinctness rests only on that constant and the README prose. A later edit, or an override such as `make SRP_VID_FIXTURE=0002`, would leave the fixture build green but blind to a dropped binding.
- Suggested outcome, optional: add a compile-time assertion in the fixture branch that the fixture differs from 2, on both the 16-bit wire field and the 12-bit class-D value. The property would then check itself.
- Verification: a fixture build with `SRP_VID_FIXTURE=0002` should then fail to compile.

Considered and not raised:
- The 0x5A3C fixture puts a non-zero top nibble in the wire SRclassVID. It is verification-only under the settled acceptance and is documented in README section DV.
- The top has no elaboration guard against non-2 values. That was a settled decision, deferred to parent #400.
- The priority stays a literal 3 in F10.2 and is not a top parameter. The issue excludes priority changes.

## Existing unrelated defects and observations

None of these is an in-scope finding, and none is counted against any lens.

- X1, portability-gate coverage (pre-existing): the `tops` list in `syn/yosys/run.sh` omits `protocol_processor_top`, `KL_srp_top`, `KL_srp_admission`, `KL_acmp_nvm_shadow` and `KL_mrp_strip`.
  - The Yosys gate therefore never elaborates the file this PR changes, although hdl/README rule 2 asks Yosys to elaborate every module.
  - The file belongs to open PR26's lane, and this lane was told not to edit it.
  - My probe (receipt 07) covers this change: sv2v and Yosys elaborate the head top at its default and with the fixture.
- X2, documentation (pre-existing): integrator guide §2 omits the existing top parameters `N_AUDIO_UNIT_P`, `N_CLK_DOMAIN_P`, `N_CONTROL_P`, `REG_TL_TIMEOUT_MS_P` and `LOCK_TIMEOUT_MS_P`. Diagram 21's parameter box is also a partial list. The PR adds its own parameter to §2 and, consistent with that practice, leaves diagram 21 as a summary.

## Acceptance mapping (issue #95)

- AC1, met:
  - the parameter at `:144` and the binding at `:2147`;
  - the F01.5 row;
  - 10 §11;
  - integrator guide §2 and §8.
- AC2, met.
  - The fixture build observes the parameter through the real top. The 16-bit value 0x5A3C appears in the MSRP Domain declaration. Its low 12 bits, 0xA3C, appear on:
    - the class-D ports;
    - snapshot word 10;
    - GET_DOMAIN;
    - the ACMP talker's VLAN.
  - Planted missing or misbound connections fail: R25, R26, R28, R30, R32, R33 and R38. The default build alone does not catch them (R25).
- AC3, met for behaviour: DV4-DV6 at both values, S1 and S8 in the default build, and the base bench passing 1371/1371 on the head RTL. The complete donor gates rest on A10's receipts and the hosted runs; I verified their identity but did not re-run them.
- AC4: this report is one of the two required independent reviews. R224 and the merge and containment bar are outside this review.

## Reviewer-owned lens, round and head ledger

| Round | Head | Lens | Result | Open findings (any severity) | Primary artifacts |
|---|---|---|---|---|---|
| R223-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | Conformance | PASS, clean | none | receipts 02, 03, 07; `KL_srp_domain.sv:113-193` |
| R223-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | RTL | PASS, clean | none | `protocol_processor_top.sv:144,2147`; receipts 07, 08, 09 |
| R223-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | Robustness | PASS, clean | R223-S2 (SUGGESTION) | `tb/pp_top/Makefile:54-63`; receipts 04, 05, 09 |
| R223-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | Tests | PASS, clean | R223-S2 (SUGGESTION) | `sim_main.cpp:8237-8401`; receipts 02-04, 09 |
| R223-1 | ea93023fbdd31cbf718da0d8f1aab4d750bfd21a | Docs | PASS, clean | R223-S1 (SUGGESTION) | `01_overview.md:166`; `10_srp_engine.md:202,366`; `integrator.md:66,260`; receipt 06 |

No MINOR, MAJOR or BLOCKER is open under any lens.

## Limitations

- Not re-run by me:
  - the complete `scripts/run_suites.sh` sweep;
  - `scripts/lint_hdl.sh` over every module;
  - `make check` with its wavedrom and mermaid tooling;
  - `syn/yosys/run.sh`;
  - `make -C tb/nvm_port figures`;
  - the `tb/srp_top`, `tb/srp_encoder` and `tb/timer_map` suites;
  - any hosted, Docker or act workflow.

  For these I rely on A10's full-native receipts and the hosted runs, whose identity and hashes are verified above.
- My local Verilator is 5.052. The 5.050 CI-pin result comes from the hosted log (pp_top 1411 PASS).
- The Milan v1.2 and IEEE 802.1Q texts are not in the repository (copyrighted). The Conformance lens was judged against the issue text, the donor's 00 compliance matrix, the architecture documents and the executable behaviour.
- I measured the default build only for R25, R28, R29, R31 and R34. For the fixture-only mutants it was not run.
- The host is shared with other lanes. My builds ran one at a time, each capped at 8 jobs, so the wall-clock times in the logs are not reference figures.
- Review clone at finish: detached at `ea93023fbdd31cbf718da0d8f1aab4d750bfd21a`, and `git status --porcelain=v2 --ignored` is empty. Nothing was committed, pushed or published. All mutants and builds lived in `/tmp/r223-96-r1`.

R223-1 FINISHED
