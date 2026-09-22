[A165] Committed author handoff for donor issue #98

- Repository: Mister-M-alt/protocol-processor-control-plane-avb-milan
- Issue: #98, "Clarify LINK_DOWN reset and LINK_UP declaration in the SRP Domain state diagram". It comes from R223-S1 on PR96 and is linked from the parent PP audit.
- Assignment: https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773239576
- [A165] TAKEN: https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773306413. It was published before the first edit, and its text is `TAKEN.md`.
- [A165] REVIEW READY: https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773433155. It is the brief public handoff, and its text is `REVIEW-READY.md`, byte-identical (sha256 `e230d328…bfd1f`).
- Clone and branch: `$CANDIDATE`, on issue-linked branch `98-clarify-srp-domain-events`.

| | Commit | Tree |
|---|---|---|
| Old head (base, `main`) | `8452f564294300a82d56eed464276576f65f4d58` | `ec259f379560863e6ea49c6043353f0c11fe714d` (= PR96 reviewed head tree) |
| New head | `88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf` | `1a65be1c93e51c2ad94e467abc2452a047047bdc` |

Commit: `Show F10.2's Domain defaults restored at LINK_DOWN and declared at the next LINK_UP`. One line, no body, no trailers. Author and committer are hackerman-kl <hackerman-kl@kebag-logic.com> (the clone's configured identity).

State: the working tree is clean; the only ignored entry is `.venv-wavedrom/`. The branch is one commit ahead of `origin/98-clarify-srp-domain-events`, which is still `8452f56`. Nothing else was done: no push, PR, project change, merge or rebase. No review was started. No hardware, parent or pin was touched.

## The change

`docs/architecture/10_srp_engine.md:205`, inside the F10.2 Mermaid fence (`:200-207`), which is the figure's editable source:

```diff
-    ADOPTED --> DEFAULTS: LINK_DOWN then LINK_UP / back to defaults
+    ADOPTED --> DEFAULTS: LINK_DOWN / restore defaults, declared again on the next LINK_UP
```

That is the whole diff: 1 file, 1 insertion, 1 deletion. The states, the arc set, the other four arcs (`:202-204`, `:206`) and all prose are unchanged. No arc was added. The LINK_UP declaration keeps its existing home, the `[*] --> DEFAULTS: startup or LINK_UP / declare ...` arc at `:202`. The relabelled arc now names both events in order.

Material choices:
- The label names no literal value, per the `docs/README.md` §2 and `docs/diagrams/README.md` arc-label rule. "defaults" refers to the `:202` arc's priority 3 and `P-SRP-DOM-DEF-VID`.
- The words follow the existing prose: 10 §11 "declared ... on LINK_UP, restored on LINK_DOWN", and DV6 "declares the default again".
- `DOMAIN_CHANGE` is not added to the arc (see Observations, item 2).

## Why this wording: the RTL and the executable evidence

| Event | RTL (`hdl/srp/KL_srp_domain.sv`, unchanged, blob `96c57db1…`) | New label | Evidence |
|---|---|---|---|
| LINK_DOWN | `link_fall_w` (`:110`) takes the branch at `:155-165`. It loads `DEF_PRIO_P` and `DEF_VID_P` (`:158-159`), clears `adopted_r` (`:160`) and `declared_r` (`:161`), and flushes the queue and the pending latches (`:162-165`). If the FSM was adopted, it strobes DOMAIN_CHANGE (`:157`). The code comment says "nothing goes on the wire" (`:156`). | `LINK_DOWN / restore defaults` | DV5 (`tb/pp_top/sim_main.cpp:8369-8387`); srp_encoder D9 (`tb/srp_encoder/sim_main.cpp:691-702`) |
| LINK_UP | `link_rise_w` (`:109`) takes the branch at `:166-171`. It queues `New {CLASS_A_ID_C, DEF_PRIO_P, DEF_VID_P}` (`:168-170`) and sets `declared_r` (`:171`). The code comment says "startup lands here too" (`:167`), because reset clears `link_q_r` (`:119`). | `declared again on the next LINK_UP` | DV6 (`sim_main.cpp:8389-8400`); D9 (`tb/srp_encoder/sim_main.cpp:703-707`) |

"Next" is exact. After LINK_DOWN, `declared_r = 0` gates both the adoption capture (`:144`) and the periodic or LeaveAll re-join (`:150`, `:185`). The pending adoption and re-join latches were also cleared (`:164-165`). The FSM therefore queues nothing until `link_rise_w`, and at that edge it declares the defaults. DV5 grades the same point from outside: nothing on the MSRP wire for the 500 ms the link stays down (`:8384-8385`).

The unchanged arcs still match the RTL:
- the `[*]` arc: reset `:114-119` plus the LINK_UP declaration;
- adoption: `:144-149` and `:172-184`;
- re-join: `:150-152` and `:185-190`.

## Rendering inspection

mmdc 11.16.0 rendered F10.2 from the fence at each commit's git object, to SVG and to PNG. The files are in `render/`:
- `F10.2-base-8452f56.{mmd,svg,png}`
- `F10.2-head-88a4eb4.{mmd,svg,png}`

They are external factual output and are not committed.

- **Visual reading of the head PNG.** The arrow from ADOPTED to DEFAULTS reads "LINK_DOWN / restore defaults, declared again on the next LINK_UP". The initial arc still reads "startup or LINK_UP / declare Domain A with priority 3, VID P-SRP-DOM-DEF-VID". The other three labels are unchanged.
- **Base and head renders compared** (`logs/06`, `logs/07`):
  - Both renders have 2 states, 1 initial pseudostate, 5 transitions and 5 labels.
  - The node centres are identical.
  - Only `edge3`'s label differs, and in both renders `edge3` runs from ADOPTED to DEFAULTS.
- **Against the RTL.** The rendered arc places the restoration at the LINK_DOWN branch (`:155-165`) and the declaration at the LINK_UP branch (`:166-171`). That matches the RTL.
- **Crowding.** The relabelled arc's label box abuts the DEFAULTS self-loop label, and the arc passes behind that label. The base render has the same layout, so this crowding predates the change. All text is legible.

## Exports and generated assets

- **In the repository.** There are 34 committed diagram assets:
  - 3 draw.io sources and their 3 SVG exports;
  - 5 hand-authored SVGs and their 5 PNGs;
  - 18 WaveDrom SVGs.

  None is a representation of F10.2. A term scan of SVG and draw.io text and of PNG strings for `fig-10-domsm|F10.2|LINK_DOWN|LINK_UP|DEFAULTS|ADOPTED|back to defaults|DOMAIN_CHANGE` finds 0 hits. F10.2 is a Mermaid figure, and GitHub renders the fence natively (`docs/diagrams/README.md`: "Mermaid | yes, natively | fenced source only").
- **Nothing was regenerated.** Nothing needed regenerating, and nothing under `docs/diagrams` changed. `wavedrom-check` and `stale` pass.
- **Outside the repository.** The only generated images are the four renders in `render/`.
- **Tool environment, not an asset.** `make check` created the clone-local, gitignored `.venv-wavedrom/` with `wavedrom==2.0.3.post3` (`logs/02`).

## Validation (author)

| What | Result | Log |
|---|---|---|
| `make -j1 check` at base `8452f56` | exit 0: 41 mermaid + 18 wavedrom blocks, 18 wavedrom fresh, 807 links, 115 REQ / 17 GAP, 86 module rows / 0 untested, nothing stale | `logs/01` |
| `make -j1 check` on the edited worktree before commit | exit 0, same results | `logs/03` |
| `make -j1 check` at head `88a4eb4`, clean tree | exit 0, same results | `logs/04` |
| each `make check` target alone at head (`lint`, `wavedrom-check`, `links`, `matrix`, `modmatrix`, `stale`) | each exit 0 | `logs/09` |
| mmdc render of F10.2, base and head, SVG and PNG | 4 of 4 exit 0; inspected as above | `logs/05`-`07` |
| negative control: a broken F10.2 fence in a `git archive` scratch copy, then `./scripts/lint-diagrams.sh` | exit 1, `MERMAID FAIL` on 10's block 2, `Parse error on line 5` | `logs/08` |
| scope, identity and hash receipts | only `docs/architecture/10_srp_engine.md:205` differs | `logs/10` |

The Mermaid gate is a real render, not a placeholder. `scripts/lint-diagrams.sh` runs `mmdc` on every fence, and the negative control shows it rejects a broken F10.2. The renderer ran on every attempt; no step was refused or skipped.

Required donor image commands (`docs/README.md` §6):
- A Mermaid edit needs `make lint` and then `make check`. Both were run.
- No WaveDrom block, draw.io source or hand-authored SVG was touched, so `make wavedrom`, `make diagrams` and the rsvg PNG check do not apply.

## Evidence reused, not re-run

- **Base tree.** The base tree `ec259f37…` is the PR96 reviewed head tree.
- **DV5 and DV6 on that tree.** R223's receipt 02 gives, per build:
  - default build: `[build default, SRP_DOM_DEF_VID_P 0x0002] 1391 checks, 0 failures`;
  - fixture build: `[build fixture, SRP_DOM_DEF_VID_P 0x5a3c] 20 checks, 0 failures`;
  - canonical tally: `1411 checks: 1411 PASS, 0 FAIL`.

  DV1 to DV6 run in both builds. The bench prints failures only, so a pass shows as these totals.
- **Two mutants tie each check to one RTL branch.**
  - R36 changes only the LINK_DOWN branch (`:159` → `16'd2`). It fails DV5 x3 and DV6's class-D check: 4 of 20.
  - R37 changes only the LINK_UP branch (`:170` → `16'd2`). It fails DV2 x2 and DV6's wire check: 3 of 20.

  So DV5 grades the restoration at LINK_DOWN, and DV6 grades the declaration at LINK_UP.
- **Post-merge hosted main run.** `hdl` run 35694588985 ran at exactly `8452f56` and succeeded: docs-gates, suites and portability. Its log shows:
  - `PASS pp_top (1411 checks: 1411 PASS, 0 FAIL)`;
  - `PASS srp_encoder (180 checks: 180 PASS, 0 FAIL)`, which includes D9;
  - `PASS srp_top (235 checks: 235 PASS, 0 FAIL)`;
  - `suites: 14943 checks total, 0 failing`.
- **Source and hash check.** Everything above comes from evidence commit `29b2066b91cf4b0387edff43250b8fe79192232b`, path `review-evidence/96-r1/`. Each cited file's sha256 matches the published MANIFEST.json, and R223's `SHA256SUMS` where it has one (`logs/11`).
- **Why it still applies to head.**
  - Head changes one Markdown line, and every `hdl/` and `tb/` blob is identical (`logs/10` §6 and §8).
  - No build or suite reads the changed file. `gen_matrix.py` reads `hdl/**/*.sv` and `tb/*/Makefile`, and `check-matrix.py` reads 00. Only the doc gates parse `docs/**/*.md`, and all of them were run above.
  - I did not run any suite, `lint_hdl.sh`, Yosys, or a hosted, act or Docker job.

## Unchanged scope

The following are all unchanged:
- RTL and tests: all of `hdl/`, `tb/`, `syn/` and `scripts/`, the `Makefile` and `.github/`.
- Behaviour: timers, parameters (F01.5), interfaces and the product pin.
- Figures: every other figure, including F10.1 and F10.3 to F10.9.
- Documents: every other document, including 01 F01.5, 10 §2 and §11, the integrator guide and 00 REQ-SRP-004. Those already stated the LINK_DOWN restore and the LINK_UP declaration (`logs/10` §8).
- Other lanes: PP25/PR26 and the pp_top fixture lane (#97).

## Observations for the manager and reviewers (not changed)

1. `tb/srp_encoder/README.md:75-77`, under "Notes / interpretations recorded", quotes the old label verbatim: 'The revert edge reads "LINK_DOWN then LINK_UP / back to defaults": the class-D levels revert at LINK_DOWN (nothing is declared on a dead link); LINK_UP performs the default declaration.'
   - Its interpretation matches the new label.
   - Its quotation now describes the pre-#98 text.
   - It is a `tb/` note that copies figure text, and the assignment excludes test changes and broader documentation cleanup, so I left it as is. I flagged it in TAKEN. Updating it would be a one-line follow-up, if wanted.
2. The relabelled arc does not name DOMAIN_CHANGE, and neither did the old label. The RTL strobes it on this revert when the FSM was adopted (`:157`), and DV5 and D9 grade it. Adding it would give the arc a meaning beyond the timing-only scope.
3. F10.2 draws no arc for LINK_DOWN while already in DEFAULTS. The branch at `:155-165` runs there too, without DOMAIN_CHANGE, and the declaration that follows is carried by the `[*]` arc's "LINK_UP". This summary is as it was at base and outside scope.
4. The label crowding in the render is identical at base (see Rendering inspection).
5. `scripts/render-wavedrom.py` loses its "bootstrapping" message when stdout is redirected, because it calls `os.execv` without a flush. This tooling quirk was already there before this change and is outside scope. It is noted only to explain `logs/01`.
6. GitHub renders the fence with its own Mermaid version. The new label uses only ASCII letters, spaces, `/`, `,` and `_`, which the neighbouring labels already use.

## Remaining (manager-owned)

- donor full native and hosted validation;
- the two cold reviews, R233 (internal Opus) and R234 (external Codex), which I did not start;
- current-candidate and post-merge containment;
- publication, merge and project status.

## This folder

- `TAKEN.md`, `TAKEN-URL.txt`: the published TAKEN (byte-identical).
- `REVIEW-READY-URL.txt`: the published REVIEW READY (byte-identical to `REVIEW-READY.md`).
- `HANDOFF.md`, `REVIEW-READY.md`, `PR-BODY.md`, `COMMANDS.md`, `git-final.txt`.
- `scope-receipts.sh`, `public-dv-evidence.py`, `svg-labels.py`, `svg-edges.py`: the exact scripts run.
- `logs/01`-`11`: raw outputs.
- `render/`: the F10.2 sources as extracted, and their SVG and PNG renders at base and head.
- `SHA256SUMS`: every other file in this folder.
