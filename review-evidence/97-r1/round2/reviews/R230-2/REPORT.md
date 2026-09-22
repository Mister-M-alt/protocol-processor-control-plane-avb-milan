[R230] POSITIVE - exact head 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e

Reviewer R230 (cold independent external Opus), round R230-2. PR100 (`97-assert-distinct-sr-vid-fixture`) for issue #97.

- Head `5c45845ad15bd7995f20c81d7fd61501e5ca9d7e`, tree `8768f7e640ee62fd62862bec7117155fcab3ddbb`.
- Single parent `1eb20dc4911880de10b745cc284e7dde306788b5`, the head R230-1 reviewed.
- Base main `8452f564294300a82d56eed464276576f65f4d58`.
- Published evidence: branch `97-review-evidence` at `290d5a2ceb23a5cb4eac41257ac331d63babeaf3`. Author round 2 (A166) is under `review-evidence/97-r1/round2/author`, first published at `4d00509`. Manager round 2 (A10) is under `review-evidence/97-r1/round2/manager`.

Attribution: author results belong to A166 (round 2) and A161 (round 1). Native results belong to A10, and hosted results to GitHub Actions runs `35708824838` and `35708829801`. The only exceptions are results I reproduced below. Everything I ran is in `receipts/` beside this report, and `receipts/SHA256SUMS` lists every receipt file.

## Verdict

POSITIVE. No BLOCKER, MAJOR or MINOR finding is open at this head.

- **R230-M1 (MINOR; Robustness, Tests) is resolved at this head.** See its disposition below.
- **All five lenses are clean.** Conformance, RTL, Robustness, Tests and Docs were each applied afresh in this round at `5c45845`, and each is clean. No earlier coverage is banked.
- **No new finding.**
- **Optional suggestions stay optional.** R230-S1 and R230-S2 are SUGGESTIONs from R230-1. They were not adopted and are unchanged at this head. They remain optional, are not acceptance conditions, and do not affect coverage.
- **Product behaviour is unchanged.**
  - The correction touches four `tb/pp_top` files.
  - The product RTL, the wrapper, the runtime bench (`sim_main.cpp`) and the documents that state the parameter policy are byte-identical to `1eb20dc`.

## R230-M1 disposition

### The original finding

- Raised in R230-1 at `1eb20dc`, under Robustness and Tests.
- `fixture_guards.py` ran the compiler in the caller's locale and matched English text.
- Under a GCC that prints translated diagnostics, the gate rejected correct compiler behaviour. The default pp_top `make` then stopped before either build.

### The correction at `5c45845`

- `tb/pp_top/fixture_guards.py:38-47`:
  - copies the inherited environment;
  - overrides only `LC_ALL=C`;
  - passes the result to each of the four compiler subprocesses.
- Verilator's model generation (`:30-32`) and `--getenv` query (`:22-24`) keep the caller's environment.
- The expected diagnostic sets, the error-count check and the fail-closed verdict (`:48-57`) are unchanged.
- `tb/pp_top/Makefile:67,71-72,83` adds `fixture-guards-test`, a new regression (`test_fixture_guards.py`), and makes it a prerequisite of `fixture-guards`, and therefore of `run`.

### Required outcome and verification, as stated in R230-1

| Required | Evidence at this head | Result |
|---|---|---|
| With `LANGUAGE=de` and `LANGUAGE=fr`, `LC_ALL` unset and GCC catalogs installed, the gate prints the four expected outcomes and exits 0 | Receipt 03. This environment translates GCC's output (receipt 02), and the prior head fails in each localized setting. Five further UTF-8 settings also pass: German and French `LANG`, an inherited `LC_ALL=de_DE.UTF-8`, `LC_MESSAGES=fr_FR.UTF-8`, and a mixed German/French set. So do Latin-1 German and Latin-9 French locales (receipt 13) | met |
| The verdict is independent of the message language | Receipt 04: the diagnostic bytes of each case are identical under EN, FR_LANGUAGE, DE_LC_ALL and MIXED, and every compiler call ran with `LC_ALL=C` | met |
| The exact expected sets, the error-count check and fail-closed behaviour are kept | Receipt 07: G1-G9 each fail at the intended case under localized callers, including an unrelated error alongside both expected ones (G7) and a third, unrelated static assertion (G9). Receipt 06d | met |
| G1 and G2 still fail | Receipt 07, under DE_LC_ALL, FR_LANG and MIXED | met |
| pp_top `make` gives 1411 PASS | Receipt 08: German and French callers each give 1391 + 20 checks, `1411 checks: 1411 PASS, 0 FAIL`. A10's native run and the hosted runs, whose identity I verified in receipt 01 | met |
| The hosted suites stay green | Receipt 01: all six jobs of the push and pull_request runs succeeded at this head | met |

Disposition: **RESOLVED at `5c45845ad15bd7995f20c81d7fd61501e5ca9d7e`.** Robustness and Tests are clean in this round (ledger below).

## Inputs read

- **Issue #97:** the body (scope, acceptance, decision) and its four comments: A10 READY TAKEOVER, A161 TAKEN, A161 MATERIAL DECISIONS and A161 AUTHOR HANDOFF.
- **PR100:**
  - the body and the diff;
  - both commits, whose messages are one line each with no trailers;
  - these comments: A161 REVIEW READY; A10 VALIDATION RUNNING; A10 REVIEW READY; A10 INDEPENDENT REVIEW START; my R230-1 report; A10 R230-M1 ACCEPTED; A166 TAKEN, MATERIAL DECISIONS and AUTHOR HANDOFF; A10 CORRECTED HEAD PUBLISHED; A10 CORRECTED-HEAD FULL BAR PASS; A10 CORRECTED COLD REVIEW ROUND START.
- **Read only as first lines (comment headers), to keep this review independent:** R229's two review comments and the manager note on R229-2's publication.
- **Not read:** R229's archived evidence, and any private lane directory, including the local path named in A166's handoff.
- **Review discipline:** the parent `kebag-logic/milan-fpga` `AGENTS.md` §6-§8 (branch `dev`, blob `b9d500c`). The donor has no AGENTS or CONTRIBUTING file.
- **Donor documentation:**
  - `docs/README.md` §6;
  - `docs/guides/hdl-engineer.md` §2 and §6-§8;
  - `tb/pp_top/README.md` (`:19-21`, `:500-567`, and mutation rows M25-M31 at `:210-216`);
  - `docs/architecture/01_overview.md:166` (F01.5);
  - `docs/guides/integrator.md:66`;
  - `docs/architecture/10_srp_engine.md:198,366-369`;
  - `docs/00_MILAN_COMPLIANCE_REVIEW.md:410,422`.
- **Build and CI files:** `.github/workflows/hdl.yml`, `scripts/run_suites.sh`, `scripts/lint_hdl.sh` and the root `Makefile`.
- **Source:**
  - `tb/pp_top/Makefile`, `fixture_guards.py` and `test_fixture_guards.py`;
  - `tb/pp_top/sim_main.cpp:75-87,8257-8262`;
  - the VID parameter lines of `protocol_processor_top.sv`, `KL_srp_top.sv` and `KL_srp_domain.sv`.
- **Evidence:**
  - `review-evidence/97-r1/round2/` at `290d5a2`. Every file was hash-verified against `MANIFEST.json`. I read these files:
    - author: `IDENTITY.json`, `RESULTS.json`, `COMMANDS.md`, `mutants/`, `receipts/runner-note.txt`, `receipts/08-remove-locale.log`, and both source patches;
    - manager: `candidate.json`, `full-native.json`, `full-native/results.json`, `full-native/complete.json`, `final-integrity.json`, `hosted.json`, and the logs `full-native/01.log` and `04.log`;
  - A161's round-1 M25 and M31 patches (`review-evidence/97-r1/author/receipts/06-missing-binding.patch`, `07-child-default.patch`).

## Identity, scope and published evidence (receipts 00, 01, 16)

### The review clone

- Detached at `5c45845`, tree `8768f7e`, single parent `1eb20dc`.
- `8452f56` is both its merge-base with base and an ancestor.
- There were no tracked, untracked or ignored changes at the start (receipt 00) and none at the end (receipt 16).

### The correction `1eb20dc..5c45845` changes exactly four files

| File | Change |
|---|---|
| `tb/pp_top/Makefile` | +5/-2 |
| `tb/pp_top/README.md` | +6 |
| `tb/pp_top/fixture_guards.py` | +5 |
| `tb/pp_top/test_fixture_guards.py` | new, +64, mode 100644 |

- There are no other mode changes, deletions or renames, and `git diff --check` is clean.
- 220 of the 224 tree entries have the same mode and blob as at `1eb20dc`. These include `hdl/` (tree `abf21fe`), `syn/`, `scripts/`, `.github/`, `docs/`, the root `Makefile`, `tb/pp_top/sim_main.cpp` (`6b400db`) and `tb/pp_top/pp_top_wrap.sv` (`c755a16`).
- The whole PR, `8452f56..5c45845`, changes five `tb/pp_top` files (+168/-2) and adds no non-ASCII line.

### Live state (12:56 CEST)

- PR100 is OPEN, not draft, MERGEABLE, head `5c45845`, base `main@8452f56`.
- `refs/pull/100/merge` is `6439edc`, with parents `8452f56` and `5c45845` and tree `8768f7e`, which is the head tree.

### Evidence archive

- `4d00509` (A166) and `290d5a2` (A10) touch only `review-evidence/97-r1/round2/**` and `review-evidence/97-r1/MANIFEST.json`. Neither is an ancestor of the head.
- The `MANIFEST.json` round-2 section lists 101 entries for the 101 files present. Every published SHA-256 matches, and the 24 `path_redacted` flags are consistent.
- A166's patches are byte-identical to the real diffs: `source.patch` to `git diff 1eb20dc..5c45845`, and `source-vs-main.patch` to `git diff 8452f56..5c45845`.

### A166's recorded results (author facts; I reproduced their substance in receipts 03, 06, 07 and 08)

- Before the correction, German and French gates give rc 2 while the compiler prints both intended diagnostics.
- After it, both give rc 0 with the four outcomes, and the regression passes.
- Mutants 05-08 give rc 2.
- Default + 5A3C gives 1411 PASS.
- Docs, UPC map and scope checks give rc 0, and every Verilator call was capped at 8.

### A10's native results (not re-run by me)

- `candidate.json` names this head, base and tree, with `candidate_equals_head_tree: true`.
- `full-native/results.json` records nine commands, all exit 0, at this head:
  - `verilator --version` (5.052);
  - `lint_hdl.sh`;
  - `run_suites.sh`, whose `04.log` shows `PASS pp_top (1411 checks: 1411 PASS, 0 FAIL)` and `suites: 14943 checks total, 0 failing`;
  - `make -j1 check`;
  - `gen_matrix.py --check`;
  - the Yosys portability run;
  - nvm_port figures;
  - `git diff --check`.
- `complete.json` gives exit 0, finished 11:18:22+02:00.
- `final-integrity.json` records both checkouts clean at this head and tree.

### Hosted runs (live API; not re-run by me)

- The `hdl` push run `35708824838` and pull_request run `35708829801` are the only runs for this head. Both are attempt 1 and completed/success with `head_sha` `5c45845`.
- All six jobs (docs-gates, suites and portability, on both events) completed/success.
- The suites logs:
  - The push run checked out `5c45845`. The PR run checked out `6439edc`.
  - Both show Verilator 5.050, `PASS pp_top (1411 checks: 1411 PASS, 0 FAIL)` and `suites: 14943 checks total, 0 failing`.
- A10's hosted snapshot recorded `isDraft: true` when it was taken. The PR is now ready for review (not a draft).

## What I ran

### Scratch and setup

- Everything ran under `/tmp/r230-100-r2-scratch`:
  - a `--no-hardlinks` clone detached at the head;
  - `git archive` exports of the prior head, of base (receipt 13 only), a mutant tree, and the M25 and M31 trees. The M25 and M31 trees were removed afterwards.
- `receipts/scripts/setup.sh` records the setup commands.

### Locales

- This host installs only the `C`, `C.utf8`, `en_US.utf8` and `POSIX` locales.
- I generated `en_US.UTF-8`, `de_DE.UTF-8`, `fr_FR.UTF-8`, `de_DE.ISO-8859-1`, `fr_FR.ISO-8859-15@euro` and `ja_JP.EUC-JP` with glibc `localedef` into a scratch `LOCPATH`. This needed no privileges.
- GCC 16.2.1's stock package ships 20 `gcc.mo` catalogs, de, fr and ja among them.

### Controlled environment

- Every probe ran through `receipts/scripts/run.py`, which starts from an explicit base environment:
  - `PATH=/usr/local/bin:/usr/bin:/bin`;
  - `HOME`;
  - `LANG=en_US.UTF-8`;
  - `TMPDIR` and `LOCPATH` inside the scratch area.
- It then adds the setting's variables. Each JSON receipt records the complete environment, the working directory, the command, the exit code and the duration.

### Caller settings (`scripts/settings.sh`)

| Setting | Variables added to the base environment |
|---|---|
| EN | none |
| DE_LANGUAGE | `LANGUAGE=de` |
| FR_LANGUAGE | `LANGUAGE=fr` |
| DE_LANG | `LANG=de_DE.UTF-8` |
| FR_LANG | `LANG=fr_FR.UTF-8` |
| DE_LC_ALL | `LC_ALL=de_DE.UTF-8`, an inherited non-C `LC_ALL` |
| FR_LC_MESSAGES | `LC_MESSAGES=fr_FR.UTF-8` |
| MIXED | `LC_ALL=fr_FR.UTF-8`, `LANG=de_DE.UTF-8`, `LANGUAGE=de:fr`, `LC_MESSAGES=de_DE.UTF-8` |

### Job caps

- The outer `make` ran serially (`-j1`). The one exception is 06e, a `make -j8` that stops at the regression before any compile.
- Every Verilator call went through `receipts/scripts/verilator8`. It strips `-j`, `--build-jobs` and `--verilate-jobs` and pins all three at 8.
- Receipt 15:
  - 134 calls were logged: 76 work calls, all with exactly `-j 8 --build-jobs 8 --verilate-jobs 8`, and 58 `--getenv` queries.
  - The callers' `-j 0` was replaced.
  - Verilation reports show `on 8 threads`.

### Toolchain

- Verilator 5.052, GCC 16.2.1, Python 3.14.7 and GNU Make 4.4.1.
- Receipt 12 also used the existing uv-managed CPython 3.11.15 and 3.12.13.
- clang++ is not installed.

## Receipts

| Receipt | Probe | Result |
|---|---|---|
| 00 | identity, live refs, diff scope, unchanged-artifact check | As above. 4 changed paths; 220 of 224 entries identical to `1eb20dc`. |
| 01 | published evidence and live hosted state | 101 of 101 manifest entries verified. Author patches byte-identical. 9 of 9 native commands rc 0. Hosted 6 of 6 jobs success, attempt 1, head `5c45845` (pull_request on `6439edc`, head tree). Verilator 5.050. 1411 pp_top checks and 14,943 suite checks, 0 failing. |
| 02 | `g++ -fsyntax-only` of a one-line `static_assert` under each setting | All seven localized settings print `statische Assertion fehlgeschlagen` or `l'assertion statique a échoué`; EN prints English. `LC_ALL=C` with `LANGUAGE=de`, `LC_ALL=C` with a mixed German/French set, `LC_ALL=POSIX` and `LC_ALL=C.UTF-8` with `LANGUAGE=de` all print English. |
| 03 | real `make fixture-guards`, prior head against this head, 8 settings | Prior head: EN rc 0. All 7 localized settings give rc 2 with `FAIL: fixture 0002: unexpected compiler result 1`, although GCC printed both intended diagnostics in German or French.<br>This head: 8 of 8 give rc 0. Each shows `Ran 1 test ... OK`, the four outcomes and `fixture guards: 4 cases PASS`.<br>No temporary directory is left behind, and the clone stays clean including ignored files. |
| 04 | real processes, no mocks: `make fixture-guards` with the compiler replaced by `cxx-record`, which runs `/usr/bin/g++` unchanged and records its environment, argv and output, and Verilator by `verilator8`, which dumps its environment. Settings EN, FR_LANGUAGE, DE_LC_ALL and MIXED, plus extra caller inputs: `CPATH`, `CPLUS_INCLUDE_PATH`, `SOURCE_DATE_EPOCH` and a marker variable | Per setting: 2 Verilator calls with identical environments that keep the caller's `LC_ALL` (unset, `de_DE.UTF-8` or `fr_FR.UTF-8`). 4 compiler calls whose environment differs from Verilator's only in `LC_ALL=C`. Every caller input is preserved: `LANG`, `LANGUAGE`, `LC_MESSAGES`, `LOCPATH`, `CPATH`, `CPLUS_INCLUDE_PATH`, the marker, `SOURCE_DATE_EPOCH`, `PATH`, `HOME` and `TMPDIR`.<br>Compiler argv is identical across settings. Per case, the diagnostic bytes are identical across settings (sha256 prefixes: `540890b1…` for the no-override and 5A3C cases, `1caea53a…` for 0002, `a56c5307…` for 1002). Artifacts are in `04-capture/`. |
| 05 | in-process: exact-head `fixture_guards.main()` with real subprocesses, argv taken from `make -n`, MIXED caller | `main()` returns 0. The caller's `os.environ` is unchanged afterwards. Both Verilator calls received exactly the caller environment. All 4 compiler calls received exactly the caller environment plus `LC_ALL=C` (caller `LC_ALL=fr_FR.UTF-8`). Artifacts are in `05-capture/`. |
| 06a/b | `make fixture-guards-test` under EN, DE_LC_ALL and MIXED; the test under a CPython audit hook with `PATH=/nonexistent` | Passes in all three settings. Audit hook: 1 test run, 0 process creations. The regression needs no compiler, Verilator or message catalog. |
| 06c | the regression against 11 edits of `fixture_guards.py` (diffs in `06c-mutants.diff`) | Fails (rc 1, both subtests) for N1-N8:<br>• N1: the `env` argument removed;<br>• N2: the `LC_ALL` override removed;<br>• N3: `LANG=C` instead;<br>• N4: `LC_MESSAGES=C` instead;<br>• N5: inherited environment dropped;<br>• N6: caller environment mutated;<br>• N7: Verilator also normalised;<br>• N8: global `LC_ALL` set before Verilator.<br>Passes (rc 0) for the equivalent rewrites E1 (`{**os.environ, "LC_ALL": "C"}`) and E2 (`dict(os.environ, LC_ALL="C")`). Passes for C1 (lenient verdict), which is outside the regression's stated scope; see observation O2. |
| 06d | the real gate, bypassing the regression (command exactly as `make -n` builds it), under DE_LC_ALL | Head: `4 cases PASS`. N1-N4: rc 1 at 0002, with German `statische Assertion fehlgeschlagen` lines. The regression's failures therefore match real defects. N5-N8 break the documented isolation contract and were not run through the real gate. |
| 06e | N2 in a full tree, `make -j8`, the default target, DE_LANGUAGE | rc 2 at `Makefile:72: fixture-guards-test`. The ROM generators ran; the gate, Verilator and both builds did not. No `obj_dir` or `obj_vid`. |
| 07 | G1-G9 `sim_main.cpp` mutants through `make fixture-guards`: all under DE_LC_ALL, and G1, G2 and G7 also under FR_LANG and MIXED (diffs in `07-sim-mutants.diff`) | All 15 runs give rc 2, with the regression OK and the compiler output in English:<br>• G1 (wire guard removed): 0002 shows the class-D diagnostic only.<br>• G2 (class-D guard removed): the wire diagnostic only.<br>• G3 (mask widened): 1002 compiles.<br>• G4 (messages swapped): 1002 carries the wire text.<br>• G5 (guards moved out of the fixture branch): the default build is refused.<br>• G6 (`#error` in every fixture build): 5A3C fails.<br>• G7 (`#error` for 0002 alongside both assertions): 3 errors against 2 expected.<br>• G8 (undeclared name in the default branch): the default build fails.<br>• G9 (third static assertion for 0002): 3 errors.<br>The mutant tree was restored to the head bytes; no temporary directory is left behind. |
| 08 | full `make`, the default target, at the head: DE_LC_ALL and FR_LANG | Both rc 0. Regression OK, `fixture guards: 4 cases PASS`, `[build default, …0x0002] 1391 checks, 0 failures`, `[build fixture, …0x5a3c] 20 checks, 0 failures`. Last line `1411 checks: 1411 PASS, 0 FAIL`, and `build_tally.txt` holds `1391 0` and `20 0`. `run_suites.sh`'s tally regex extracts the same line. Only ignored outputs are created; clean after `make clean`. |
| 09 | M25 (A161's patch removing the top's `.DOM_DEF_VID_P` binding) under DE_LANGUAGE; M31 (A161's patch changing `KL_srp_top`'s default to 7) under FR_LC_MESSAGES; each in its own export of this head | M25: gate PASS, default 1391/0, and the fixture build fails 13 of 20 (every value check of DV1-DV6); make rc 2.<br>M31: 1391/0 and 20/0, `1411 checks: 1411 PASS, 0 FAIL`.<br>Both match `README.md:210,216`. |
| 10 | `lint_hdl.sh`'s zero-tolerance flags on `protocol_processor_top` at the default and at `-G…=16'h0002`, `16'h1002` and `16'h5A3C`; RTL defaults and assertions; the real 0002 and 1002 fixture build recipes | Lint: rc 0 with 0 `%Warning`/`%Error` lines at all four values. The defaults are 2 at `protocol_processor_top.sv:144`, `KL_srp_top.sv:80` and `KL_srp_domain.sv:42`, and no RTL assertion mentions the VID.<br>Fixture builds: rc 2. The model header is generated, only `sim_main.o` fails, and there is no `Vpp_top_vid`. 0002 shows `:81` and `:83`; 1002 shows `:83` only. |
| 11 | docs gates that need no install; syntax; `diff --check` | `check-links.py`: 807, OK. `check-matrix.py`: 115 REQ rows and 17 GAP findings, OK. `gen_matrix.py --check`: 86 rows, 0 untested. `check_upc_map.py`: PASS. `make stale`: rc 0. Both new Python files parse. `diff --check` is clean against the prior head and against base. |
| 12 | regression plus gate under CPython 3.11.15 and 3.12.13, DE_LC_ALL | Both rc 0: `Ran 1 test … OK`, 4 cases PASS. |
| 13 | legacy caller encodings | `de_DE.ISO-8859-1` and `fr_FR.ISO-8859-15@euro`: prior head rc 2, this head rc 0.<br>`ja_JP.EUC-JP` at both heads: rc 2 with a `UnicodeDecodeError` while decoding the 0002 output (see observation O1).<br>Base `8452f56`: `make ltn_rom.hex` fails under EUC-JP, Latin-1 and Latin-9 (`UnicodeEncodeError` on U+2014 in `gen_ltn_rom.py`) and passes under `de_DE.UTF-8`. Under EUC-JP, `gen_matrix.py --check` also fails with a `UnicodeDecodeError`. |
| 14 | conformance sources at the head, and a width value matrix (the real `sim_main.cpp`, `-fsyntax-only`, `LC_ALL=C`, one generated model) | These compile: no override, 5A3C, 5a3c, 0003, 0000, 0FFF and 1005.<br>Refused at both widths: 0002, 2 and 10002.<br>Class-D only: 1002 and F002.<br>No other error appears for any value. |
| 15 | Verilator job-cap audit (`verilator-invocations.jsonl`) | PASS, as described under "Job caps". |
| 16 | final state of the review clone | HEAD `5c45845`, detached. Status is empty including ignored files. The 224 index entries equal the HEAD tree at stage 0. Every path matches its HEAD blob in kind, executable bit and bytes (214 x 100644, 10 x 100755). `git fsck --full` is clean. The live refs are unchanged. |

## Findings

**None new at this head.**

These carried SUGGESTIONs from R230-1 were not adopted, are unchanged and remain optional:

- **R230-S1 (Docs):** `tb/pp_top/README.md:525` still writes the environment-prefix form `SRP_VID_FIXTURE=0002`. `Makefile:15` still overrides it; only `make SRP_VID_FIXTURE=…` applies.
- **R230-S2 (Docs, Tests):** the README mutation record (`:182-232`) still has no rows for the guards' mutants. The README prose (`:544`) now states that removing the locale override fails the regression.

Neither is a condition of issue #97 or of R230-M1, and neither affects lens coverage.

## Lens results (artifact-based, this round, this head)

```text
[R230] PASS Conformance — tb/pp_top/sim_main.cpp:78-87 (blob 6b400db, unchanged since 1eb20dc); docs/architecture/01_overview.md:166 (F01.5); docs/guides/integrator.md:66; docs/architecture/10_srp_engine.md:198,366-369; docs/00_MILAN_COMPLIANCE_REVIEW.md:410,422; receipts 03, 08, 09, 10, 14 — issue #97 acceptance re-checked at 5c45845: default + 5A3C builds pass 1391 + 20 = 1411 also under German and French callers (the R230-M1 gap), 0002/2/10002 are refused at both widths and 1002/F002 at class-D only, M25/M31 keep their recorded outcomes; product policy (2 in every product build, other values verification fixtures) unchanged and not restricted.
[R230] PASS RTL — hdl/top/protocol_processor_top.sv:144,2147; hdl/srp/KL_srp_top.sv:80,323; hdl/srp/KL_srp_domain.sv:42; tb/pp_top/pp_top_wrap.sv (blob c755a16); receipts 00, 09, 10, 14 — no RTL or wrapper byte changed since base; product top lints with zero warnings at default, 0002, 1002 and 5A3C; no RTL assertion on the VID; the guard reference 2 equals all three RTL defaults; the real 0002/1002 fixture recipes generate the model and fail only in sim_main.o; M25 bites 13/20, M31 stays green.
[R230] PASS Robustness — tb/pp_top/fixture_guards.py:22-60; tb/pp_top/Makefile:54,67-72; receipts 02, 03, 04, 05, 07, 08, 13 — gate verdict is independent of the caller's message language in 8 UTF-8 settings (LANGUAGE lists, LANG, LC_MESSAGES, inherited non-C LC_ALL, mixed) and in Latin-1/Latin-9 German/French locales; compiler environment = caller environment + LC_ALL=C only, Verilator environment and caller os.environ untouched; diagnostic bytes identical across settings; every gate mutant and unrelated error fails closed under localized callers; no temporary-directory or source residue.
[R230] PASS Tests — tb/pp_top/test_fixture_guards.py:14-60; tb/pp_top/Makefile:67,71-72,83; receipts 03, 05, 06, 07, 08, 12 — the regression runs in every make through the prerequisite chain, starts no real process (audit hook, PATH without tools), passes under EN/DE/MIXED callers and CPython 3.11/3.12/3.14, fails for all eight normalisation/isolation mutants N1-N8 (N1-N4 also shown to break the real gate under an inherited German LC_ALL), accepts equivalent rewrites E1/E2, and a failing regression stops make before any build; the gate's four cases still detect G1-G9; canonical tally 1411 unchanged and readable by run_suites.sh.
[R230] PASS Docs — tb/pp_top/README.md:523-544; tb/pp_top/fixture_guards.py:38; PR100 body; receipts 03, 04, 05, 06, 11 — each new README sentence matches a receipt (LC_ALL=C for the compiler subprocess only; other environment inputs and compiler arguments preserved; mocked regression needs no catalogs; required by fixture-guards; removing the override fails it); docs gates pass; no other document references the gate or LC_ALL; R230-S1/S2 unchanged and optional.
```

## Issue #97 acceptance, at this head

- **Compile-time refusal at either observable width; default, 5A3C, RTL, runtime policy and shipping values preserved:** met (receipts 00, 10, 14).
- **Default and 5A3C builds pass unchanged:**
  - met, now also under German and French callers (receipt 08), which is the case R230-M1 was about;
  - English results: A10's native run and the hosted runs (receipt 01).
- **0002, and a value whose low 12 bits are 2, fail compilation with a clear diagnostic:** met (receipts 10, 14; the gate, receipt 03).
- **Missing-binding and child-default controls keep their recorded outcomes:** met (receipt 09).
- **Focused pp_top and the documented donor gates:**
  - reproduced by me: focused pp_top (receipts 08, 09) and the no-install docs gates (receipt 11);
  - from A10 and GitHub Actions: the complete native and hosted bar, whose identity and logs I verified and did not re-run (receipt 01).
- **Decision: distinctness at both widths, no new product-parameter restriction:** met (receipts 10, 14).
- **A10's R230-M1 decision** (fix the compiler subprocess locale; keep both diagnostic sets, the unrelated-error check and fail-closed behaviour; verify German and French, both removal mutants and default/5A3C; S1/S2 excluded; no RTL or policy change): met (receipts 00, 03, 06, 07, 08).
- **Two independent reviews and merge containment:** this review is POSITIVE. Everything else is listed under pending obligations.

## Reviewer-owned lens, covering-round and head ledger

| Lens | Covering round | Head | Result | Open findings (any severity) | Primary artifacts |
|---|---|---|---|---|---|
| Conformance | R230-2 | 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e | PASS, clean | none | `sim_main.cpp:78-87`; F01.5 `01_overview.md:166`; receipts 03, 08, 09, 10, 14 |
| RTL | R230-2 | 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e | PASS, clean | none | `protocol_processor_top.sv:144,2147`; `KL_srp_top.sv:80`; `KL_srp_domain.sv:42`; receipts 00, 09, 10 |
| Robustness | R230-2 | 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e | PASS, clean | none (R230-M1 resolved) | `fixture_guards.py:22-60`; `Makefile:54,67-72`; receipts 02-05, 07, 08, 13 |
| Tests | R230-2 | 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e | PASS, clean | R230-S2 (SUGGESTION, optional); R230-M1 resolved | `test_fixture_guards.py:14-60`; `Makefile:67,71-72`; receipts 03, 05-08, 12 |
| Docs | R230-2 | 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e | PASS, clean | R230-S1, R230-S2 (SUGGESTION, optional) | `tb/pp_top/README.md:523-544`; receipts 04-06, 11 |

- All five lenses were applied in R230-2 against this head. None is banked from an earlier round.
- **History:** R230-1 at `1eb20dc` left Conformance, RTL and Docs clean and Robustness and Tests not clean (R230-M1). `5c45845` changed files in the Robustness, Tests and Docs scopes. Those R230-1 rows are therefore superseded by the rows above.
- No BLOCKER, MAJOR or MINOR finding is open under any lens.

## Observations (not findings)

**O1: the gate decodes compiler output in the caller's encoding.**

- Location: `fixture_guards.py:43-47`, `text=True`. This has been present since `1eb20dc` and was not changed by the correction.
- Behaviour under `LC_ALL=C`: GCC 16.2.1 still prints a UTF-8 bullet (U+2022) in the note under each failed static assertion.
- Under `ja_JP.EUC-JP`, both heads stop at 0002 with a `UnicodeDecodeError` (receipt 13). The failure is closed; no bad fixture can pass.
- Why not raised:
  - It concerns output encoding, not message language.
  - German and French Latin-1 and Latin-9 callers pass.
  - The tree already does not run under these locales. In a fresh checkout, base pp_top `make` fails under every non-UTF-8 locale I tried, because its `ltn_rom.hex` prerequisite raises `UnicodeEncodeError` under EUC-JP, Latin-1 and Latin-9. The documented gate `gen_matrix.py --check` also fails under EUC-JP.
  - The only way base passes and the gate fails is an EUC-JP-class caller whose ROM images were generated earlier under a UTF-8 locale.

**O2: the new regression covers only locale, environment and argv plumbing, as its README paragraph says.**

- A deliberately lenient verdict (C1) passes the regression.
- The gate's classification is exercised by its four real cases on every `make`, and by the G1-G9 mutants (receipt 07) and A166's mutants 05-07. C1 is in receipt 06c.

**O3: the new test raises no Python floor.**

- It uses a parenthesized multi-item `with` (documented from CPython 3.10) and `mock` `call.args`/`call.kwargs` (3.8).
- pp_top `make` already needs Python 3.9 or later whenever it generates `ucode.hex`: `gen_ucode.py` evaluates `list[int]` annotations at definition time (`:354`) and has no `__future__` import.
- CPython 3.11, 3.12 and 3.14 pass locally (receipt 12), and so does the hosted ubuntu-24.04 runner (receipt 01). 3.9 and 3.10 are not available here.

**O4: carried from R230-1 and unchanged by this PR.**

- The DV4 guard `sim_main.cpp:8262` compares only 16 bits (1005 compiles, receipt 14).
- PR96's R223-S1, R223-X1 and R223-X2 remain as recorded there.

## Limitations

- **Not re-run by me:**
  - the complete `scripts/run_suites.sh` sweep;
  - `scripts/lint_hdl.sh` over every module;
  - `make check`'s diagram lint and WaveDrom check (`render-wavedrom.py` bootstraps a virtual environment on first run, which counts as an install);
  - `syn/yosys/run.sh`;
  - `make -C tb/nvm_port figures`;
  - any hosted, Docker or act workflow.

  For these I rely on A10's native receipts and the two hosted runs whose identity I verified (receipt 01).
- **What the hosted logs show:** only `run_suites.sh`'s per-suite summary. That the regression and the gate ran on the hosted runner follows from the Makefile prerequisite chain and the `PASS pp_top` line. The logs do not print it.
- **Toolchain:**
  - The local Verilator is 5.052. The Verilator 5.050 result comes only from the hosted logs.
  - clang++ is not installed, so the gate was exercised with GCC 16.2.1 only.
- **Locales:**
  - German, French and legacy locales were generated into a scratch `LOCPATH`, not installed system-wide.
  - The GCC catalogs are the stock package's.
  - No non-UTF-8 multibyte encoding other than EUC-JP was tried.
- **Mutation coverage:** M25 and M31 were reproduced with A161's published patches. M26-M30 were not re-run.
- **Specifications:** the Milan v1.2 and IEEE texts are not in the repository. Conformance was judged against:
  - the issue text;
  - F01.5, F10.2 and 10 §11;
  - the integrator guide;
  - the 00 compliance rows;
  - executable behaviour.
- **Run history:**
  - Receipt 05's first attempt stopped inside my harness before the gate ran: its argv extraction matched the `make -n` unittest line. The corrected harness produced the published receipt; `scripts/05-run.sh` records this.
  - Receipts 08, 12 and 13 were first run as inline commands and then regenerated from their scripts; the published files are the script runs.
  - `verilator-invocations.jsonl` is append-only, so it also covers the superseded runs. Every one of them was capped.
- **Host and review clone:**
  - The host is shared. Builds ran serially with Verilator capped at 8 threads, so durations are not reference figures.
  - In the review clone only remote-tracking refs were fetched. The working tree, the index and HEAD are untouched (receipt 16).
  - Nothing was committed, pushed or published by me.

## Pending merge obligations (separate from this verdict)

1. **R229-2.** A PR comment whose first line reads `[R229] POSITIVE - exact head 5c45845…` exists (id 5774922964). I did not read its content. Whether it supplies a clean, reviewer-owned five-lens ledger at this head is for the manager to confirm.
2. **Two positive independent reviews** with clean five-lens coverage at the merge candidate, or at an ancestor of it that no lens-scoped change has touched since. This report supplies R230's ledger at `5c45845`.
3. **Current-candidate validation immediately before merge.** At 12:56 CEST main was `8452f56`, and `refs/pull/100/merge` (`6439edc`) carried the head tree. If main moves, the candidate must be re-validated.
4. **Merge, then containment.** The merge needs explicit maintainer authorization. After it: actual-tree containment, post-merge hosted validation, and closure of issue #97 through `Closes #97`.
5. **Separate lanes.** The `97-review-evidence` branch must never merge. Parent submodule integration is its own lane.

R230-2 FINISHED
