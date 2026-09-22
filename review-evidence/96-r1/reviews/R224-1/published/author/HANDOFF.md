[A157] Committed author handoff for donor issue #95

Repository: Mister-M-alt/protocol-processor-control-plane-avb-milan
Issue: #95, a prerequisite of kebag-logic/milan-fpga#400
Implementation head: `ea93023fbdd31cbf718da0d8f1aab4d750bfd21a`
Base: `main` at `424c688fa2205b934a7689a58f2aa766420f2326`
Branch: `95-expose-default-sr-vid` (issue-linked, created by the manager)
Checkout: `$CANDIDATE`
State: clean. One local commit ahead of `origin/95-expose-default-sr-vid`. Nothing was pushed, no PR opened, no review launched, nothing rebased or merged. The parent's submodule pin, the parent checkouts, open PR26 and its `syn/yosys/run.sh` were not touched.

Public trail: [A10] assignment https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/95 (status moved Ready to In progress at 2026-09-22T04:10:46Z, before the first edit); [A157] TAKEN https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/95#issuecomment-5771249686; [A157] material decisions https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/95#issuecomment-5771529672 (text in `DECISIONS.md`).

Commit: `Expose the SRP Domain default VID as a top parameter and grade its binding in a second pp_top build`. One line, no body, no trailers; author hackerman-kl <hackerman-kl@kebag-logic.com>.

## Changed files

| File | Change |
|---|---|
| `hdl/top/protocol_processor_top.sv` | `:137-144` new public `parameter logic [15:0] SRP_DOM_DEF_VID_P = 16'd2`, banner citing `P-SRP-DOM-DEF-VID` (F01.5), 10 §6.1 F10.2 and Milan §4.2.7.2.1. `:2147` explicit binding `.DOM_DEF_VID_P (SRP_DOM_DEF_VID_P)` on `u_srp`. Nothing else in the RTL changes. |
| `docs/architecture/01_overview.md` | F01.5 row `P-SRP-DOM-DEF-VID`: default 2, 16-bit, =2 in every product build, other values verification-only; affects the F10.2 startup/LINK_UP declaration and LINK_DOWN revert; adoption preserved. |
| `docs/architecture/10_srp_engine.md` | F10.2 initial arc cites the P-ID instead of the literal 2 (single-source rule, `docs/README.md` §2). §11 lists the parameter and its semantics. |
| `docs/guides/integrator.md` | §2 parameter row (keep 2 in a product build; adoption still applies). §8 Domain class-D row names the default and the link-down restore. |
| `tb/pp_top/pp_top_wrap.sv` | Four Domain class-D ports passed through by name (`srp_class_a_prio_o`, `srp_class_a_vid_o`, `srp_domain_adopted_o`, `srp_domain_change_o`). The fixture override applies only under `` `ifdef PP_TOP_SRP_DOM_DEF_VID ``, so the default build overrides nothing. |
| `tb/pp_top/sim_main.cpp` | `SRP_DEF_VID` expectation (2, or the compiled fixture); `H::domain_changes` strobe counter; new `DomainDefaultPhase` (section DV, 20 checks) on a fresh model; `Suite::run()` runs DV last; `main()` runs DV alone in the fixture build and prints a per-build line plus its tally record. |
| `tb/pp_top/Makefile` | `SRP_VID_FIXTURE = 5A3C`; the second build (`obj_vid/Vpp_top_vid`) gets `+define+PP_TOP_SRP_DOM_DEF_VID=16'h5A3C` for the wrap and `-DPP_TOP_SRP_DOM_DEF_VID=0x5A3C` for the C++; one summed canonical tally line (guarded: exactly two build tallies); `clean` removes `obj_vid`. |
| `tb/pp_top/README.md` | Tally note, DV bullet, mutation rows M25 to M31 and a "Section DV" chapter (why two builds, the fixture rationale, DV1 to DV6). |

`hdl/srp/` is untouched: the child's width, its own default of 2 and all engine, timer, priority, adoption, MVRP, frame-format and microprogram behaviour are unchanged. 8 files changed, 332 insertions, 6 deletions.

## Acceptance mapping

| Issue acceptance | Evidence at `ea93023` |
|---|---|
| 1. A named 16-bit top parameter defaults to 2 and drives `KL_srp_top.DOM_DEF_VID_P` directly; the interface documentation gives default/startup semantics and preserves network adoption | `protocol_processor_top.sv:144` and `:2147`. F01.5 row, F10.2, 10 §11, integrator §2 and §8. Default build DV1 to DV6 grade 2 at reset, LINK_UP, LINK_DOWN and after the bounce. M29 (top default changed to 3) fails 18 default-build checks, so the default build grades the top's own default. M31 (child default changed to 7, binding intact) stays green in both builds, so the child's default is no longer a source. |
| 2. A top-level executable test observes the default and a distinct verification-only value through the actual instantiated SRP path; a planted missing or misbound connection fails it; a child-only test is insufficient | Section DV drives the real `protocol_processor_top` through its MAC stream, class-D ports, side-port snapshot, svc GET_DOMAIN and ACMP talker, in the default build (2) and the fixture build (0x5A3C). Planted defects: M25 binding removed, 13 of 20 FAIL; M26 bound to a literal 2, 13 of 20; M27 truncated to 12 bits, 4 of 20 (the 16-bit wire checks); M28 bound to `DOM_DEF_PRIO_P`, 16 of 1391 default plus 13 of 20 fixture; M30 fixture override removed, 13 of 20. |
| 3. Existing Domain adoption and Link Down/Up behaviour remain covered; focused top/SRP regressions and the mandatory donor gates run on the exact candidate | Unchanged and passing: pp_top S1 (`New {6,3,2}`) and S8 (certified two-class adoption), `tb/srp_top` 235/235, and `tb/srp_encoder` 180/180 (Domain D1 to D9, including LINK_DOWN revert and LINK_UP re-declare). Added at top level: DV4 adoption over the parameter, DV5 LINK_DOWN revert with one DOMAIN_CHANGE and no declaration while down, DV6 LINK_UP re-declaration. Donor gates are in the validation table below; the full sweep and hosted gates are listed as remaining. |
| 4. Two independent positive reviews, clean lens coverage, donor merge/containment before the parent pin moves | Not author scope. Pending with the manager: R223 (internal) and R224 (external). |

## Author validation (head `ea93023`, clean tree, compile jobs capped at 8)

| Command | Result | Log (`logs/validation/`) |
|---|---|---|
| `cd tb/pp_top && make clean && make` | rc 0, 191 s. `[build default, SRP_DOM_DEF_VID_P 0x0002] 1391 checks, 0 failures`; `[build fixture, SRP_DOM_DEF_VID_P 0x5a3c] 20 checks, 0 failures`; canonical `1411 checks: 1411 PASS, 0 FAIL`. The base `424c688` measured `1371 checks: 1371 PASS, 0 FAIL` in 178 s | `pp_top.log`, `pp_top-baseline-424c688.log` |
| `cd tb/srp_top && make clean && make` | rc 0, `235 checks: 235 PASS, 0 FAIL` | `srp_top.log` |
| `cd tb/srp_encoder && make clean && make` | rc 0, `180 checks: 180 PASS, 0 FAIL` | `srp_encoder.log` |
| `cd tb/timer_map && make clean && make` | First attempt rc 2 in 0 s, an invocation error of mine: a spaced `VERILATOR` value cannot pass the `shapes` recipe's env prefix. Rerun with the `verilator8` wrapper: rc 0, 10 legal shapes `SHAPE OK`, 3 over-large `GUARD OK`, `1360 checks: 1360 PASS, 0 FAIL` | `timer_map.log`, `timer_map.rerun.log` |
| `python3 scripts/check_upc_map.py` | `UPC MAP GATE: PASS (56 engine constants, 80 entry points, all agree)` | `check_upc_map.log` |
| `./scripts/lint_hdl.sh` | rc 0, 37 `LINT OK`, nothing else | `lint_hdl.log` |
| `make check` | rc 0: 41 mermaid + 18 wavedrom blocks OK, wavedrom fresh, 807 links OK, 115 REQ rows / 17 GAP OK, module matrix 86 rows / 0 untested, nothing stale | `make_check.log` |
| `python3 scripts/gen_matrix.py --check` | `matrix: OK (86 rows, 0 untested)`; `MODULE_MATRIX.md` unchanged, since no Makefile source list changed | `gen_matrix.log` |
| `python3 scripts/check-links.py`, `check-matrix.py`, `render-wavedrom.py --check`, `make stale` | all rc 0 | `check_links.log`, `check_matrix.log`, `wavedrom_check.log`, `make_stale.log` |
| `./syn/yosys/run.sh` (run, not edited) | rc 0: 32 `YOSYS OK`, `YOSYS XILINX OK KL_aecp_engine`. Output identical (sorted) to the same gate at base `424c688` | `yosys.log`, `yosys-base.log` |
| `git diff --check 424c688..HEAD` | clean | `diff-check.log` |
| U+2014 / U+2013 on the 332 added lines | 0 / 0 | `em-dash.log`, `added-lines.txt` |
| `git status` before and after | 0 changed paths both times | `SUMMARY` |

Exact invocations: `VALIDATION-COMMANDS.md`, `validate.sh`, `mutate.sh`.

## Mutation controls

Each control ran in its own scratch copy under `$MUTATION_STORAGE/`; the lane was never edited. Afterwards each scratch tree's build inputs were compared with the committed head, and exactly one file differs in each.

| # | Planted change | Default build (1391) | Fixture build (20) |
|---|---|---|---|
| M25 | binding line removed | 0 FAIL (blind by construction) | 13 FAIL: every value check of DV1 to DV6 |
| M26 | `.DOM_DEF_VID_P (16'd2)` | not run (same value as M25 there) | 13 FAIL |
| M27 | `.DOM_DEF_VID_P (16'(SRP_DOM_DEF_VID_P[11:0]))` | not run | 4 FAIL: DV2 byte-exact, DV2 decoded SRclassVID, DV4, DV6 (the 16-bit wire field) |
| M28 | `.DOM_DEF_PRIO_P (SRP_DOM_DEF_VID_P[7:0])`, VID unbound | 16 FAIL (S1 x2, S2, S7, S8, 11 in DV) | 13 FAIL |
| M29 | top default `16'd3` | 18 FAIL (S1 x2, S8, T0, MP3, 13 in DV) | 0 FAIL (override in force) |
| M30 | wrap fixture override removed | not run (the define is absent there) | 13 FAIL |
| M31 | control: child default `16'd7`, binding intact | 0 FAIL | 0 FAIL |

The diffs and per-build logs are in `logs/mutations/<id>/`.

## `.github/workflows/hdl.yml` required commands (read, not changed)

The workflow pins Verilator `v5.050`, built from source, and runs on push and pull_request:

- `docs-gates`: `python3 -m pip install wavedrom`; `python3 scripts/check-links.py`; `python3 scripts/check-matrix.py`; `python3 scripts/render-wavedrom.py --check`; `make stale`.
- `suites`: `verilator --version`; `./scripts/lint_hdl.sh`; `./scripts/run_suites.sh`; `python3 scripts/gen_matrix.py --check`; `git fetch --no-tags origin refs/pull/13/head` then `make -C tb/nvm_port figures`.
- `portability`: install `yosys` (apt) and the latest `sv2v` release; `./syn/yosys/run.sh`.

Run here: every docs-gates command, `lint_hdl.sh`, `gen_matrix.py --check` and `./syn/yosys/run.sh`. The suites of `run_suites.sh` affected by this change were run (pp_top, srp_top, srp_encoder, timer_map), and so was its `check_upc_map.py` pre-gate.

Not run here (remaining gates): the complete `./scripts/run_suites.sh` sweep, which is manager-owned; `make -C tb/nvm_port figures`, which this change does not affect since `tb/nvm_port` and its RTL are untouched, and which needs the PR #13 ref; Verilator `v5.050` itself, since local runs used 5.052; and all hosted runs, since nothing was pushed. The parent's hosted gates and candidate validation also remain.

## Environment

Verilator 5.052 (the CI pin is v5.050; the donor floor in `hdl/README.md` is 5.050 or later), Python 3.14.7, Yosys 0.66, sv2v v0.0.13, mmdc 11.16.0, g++ 16.1.1. `make check` bootstrapped the git-ignored `.venv-wavedrom/` in the lane from PyPI, as `render-wavedrom.py` documents for its first run. Cgroup `milan-donor95-a157.service`: CPUs 96-127, MemoryMax 12 GiB. Compile jobs were capped at 8 via `MAKEFLAGS=-j8`, `VERILATOR_JOBS=8` and `--build-jobs 8 --verilate-jobs 8`; a process sample showed the inner `make ... -j 8`.

## Known limits and observations

- The fixture `0x5A3C` exists only to prove propagation. It is not a VID any product build may declare (Milan §4.2.7.2.1 fixes 2), and no elaboration guard refuses non-2 values in the donor; refusing other shipping defaults is the parent builder's job, per the #400 decision.
- DV3 grades the GET_TX_STATE `stream_vlan_id` of a source that is not declaring. F05.11 leaves that field undefined; this talker answers the SR-class VID, which S10 and MP3 already grade.
- `./obj_dir/Vpp_top_sim` now ends with a per-build line (`[build default, ...] N checks, F failures`); the canonical line comes from the Makefile, summed over both builds. The per-build tally record goes to git-ignored `obj_dir/build_tally.txt`.
- pp_top wall time rose from 178 s to 191 s: a second ~15 s Verilator build, 0.8 s of fixture run, and DV's 3.4 s of simulated time in the default build.
- Pre-existing, out of scope and left unchanged: `tb/dyn_state/shape_tally.txt` is tracked and rewritten when that suite runs, so a full sweep can dirty a tree. The check counts stated in `tb/timer_map/README.md` (1187) and `tb/srp_top/README.md` (230) predate their current tallies (1360 and 235). None of these is touched by this lane.
- Parent follow-up (kebag-logic/milan-fpga#400, after this donor merges): bind `SRP_DOM_DEF_VID_P` from the generated declaration and validate tag/default agreement there.
