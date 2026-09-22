[A165] Committed author handoff for the donor issue #98 follow-up: the srp_encoder README quotation

- Repository: Mister-M-alt/protocol-processor-control-plane-avb-milan
- Issue: #98, "Clarify LINK_DOWN reset and LINK_UP declaration in the SRP Domain state diagram".
- Authority for this follow-up: [A10] DIRECT DOCUMENTATION REFERENCE CORRECTION, https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773480374. The same text is in `../donor98-quote-decision.md`.
  - It authorizes one correction: the copied quotation of the old F10.2 label in `tb/srp_encoder/README.md:75-77`.
  - That note's LINK_DOWN restore and LINK_UP declaration interpretation must be kept.
  - The issue acceptance is unchanged.
- First round, the figure commit `88a4eb4`:
  - Its factual packet is `$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor98-author/` (`HANDOFF.md`, `COMMANDS.md`, `render/`, `logs/`, `SHA256SUMS`).
  - Its public [REVIEW READY](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773433155).
  - This follow-up only read that packet. Its 29 checksums still verify (`logs/04`).
- [A165] TAKEN (follow-up): https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773538937. It was published before the edit. Its text is `TAKEN.md`, byte-identical (sha256 `7f199555…26c2`).
- [A165] REVIEW READY (follow-up): https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773588874. Its text is `REVIEW-READY.md`, byte-identical (sha256 `40b13570…4a34`).
- Clone and branch: `$CANDIDATE`, on `98-clarify-srp-domain-events`.

| | Commit | Tree |
|---|---|---|
| Main base | `8452f564294300a82d56eed464276576f65f4d58` | `ec259f379560863e6ea49c6043353f0c11fe714d` |
| Old head (the first-round figure commit) | `88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf` | `1a65be1c93e51c2ad94e467abc2452a047047bdc` |
| New head | `bc997e7c00e50e3d59b97987950bfbf6cc442182` | `bd67eaac225513f84ecf3a228cf58a818e67b4da` |

The commit message is `Describe F10.2's revert edge in the srp_encoder README instead of quoting its old label`: one line, no body, no trailers. Author and committer are hackerman-kl <hackerman-kl@kebag-logic.com>, the clone's configured identity. In `bc997e7.patch` the subject wraps over two header lines; that is `format-patch` header folding, and the message itself is one line (`logs/02` §2).

State (`logs/04`):
- The working tree is clean. The only ignored entry is `.venv-wavedrom/`, and there is no stash.
- The branch is two commits ahead of `origin/98-clarify-srp-domain-events`, which is still `8452f56`, and no PR exists for it.
- Nothing else was done: no push, PR, project change, merge or rebase. No review was started. No hardware, parent or pin was touched.

## The change

The change is at `tb/srp_encoder/README.md:75`, under "Notes / interpretations recorded":

```diff
-- The revert edge reads "LINK_DOWN then LINK_UP / back to defaults": the
+- The F10.2 revert edge (ADOPTED to DEFAULTS) fires on LINK_DOWN: the
   class-D levels revert at LINK_DOWN (nothing is declared on a dead link);
   LINK_UP performs the default declaration.
```

The diff is 1 file, 1 insertion and 1 deletion. The mode stays 100644. The new line is ASCII and 69 characters long. `:76-77` and every other line are byte-identical (`logs/02` §3-4).

## Why this wording

- **A description, not a quote.** The assignment allowed an accurate quotation of the new label or a description without the literal label. I chose the description.
  - `docs/README.md` §3 gives each figure one home: "Reuse is a relative link, never a copy."
  - A copied label is exactly what went stale here.
  - After this change, the new label's text exists only in the F10.2 fence, at `10_srp_engine.md:205` (`logs/02` §5).
- **The edge is named by its two states, ADOPTED and DEFAULTS, not by its label.** "F10.2" names the figure that the dropped quotation pointed to. The notes already cite F10.2 (`:71`), and the README already uses ADOPTED (`:42`).
- **"fires on LINK_DOWN"** states the edge's trigger, as the label does (`LINK_DOWN / …`). It does not make LINK_UP part of the trigger, so the old "LINK_DOWN then LINK_UP" reading cannot come back.
- **The interpretation is byte-identical** (`:76-77`): the class-D levels revert at LINK_DOWN, nothing is declared on a dead link, and LINK_UP performs the default declaration.

The note agrees with every unchanged source:

| Statement in the note | F10.2 (`10_srp_engine.md`) | RTL (`hdl/srp/KL_srp_domain.sv`, blob `96c57db1…`) | Suite (`tb/srp_encoder/sim_main.cpp`, blob `eab9912e…`) |
|---|---|---|---|
| the revert edge from ADOPTED to DEFAULTS fires on LINK_DOWN | `:205` `ADOPTED --> DEFAULTS: LINK_DOWN / …` | `link_fall_w` (`:110`) takes `:155-165`; `adopted_r <= 0` (`:160`) | D9 `:694-699`: after link down, DOMAIN_CHANGE fires, the levels are 3/2 and `dom_adopted_o == 0` |
| the class-D levels revert at LINK_DOWN | "restore defaults" | `:158-159` load `DEF_PRIO_P` and `DEF_VID_P` | D9 `:697-698` "levels back to the defaults" |
| nothing is declared on a dead link | "declared again on the next LINK_UP" | `:156` "nothing goes on the wire"; `declared_r <= 0` (`:161`) | D9 `:700-702` "nothing declared while the link is down" |
| LINK_UP performs the default declaration | `:202` `[*] --> DEFAULTS: startup or LINK_UP / declare …`; `:205` "declared again on the next LINK_UP" | `link_rise_w` (`:109`) takes `:166-171`: `New {CLASS_A_ID_C, DEF_PRIO_P, DEF_VID_P}` | D9 `:703-707` "LINK_UP re-declares the defaults (New)" |

The suite summary in the same README, which is unchanged, says the same thing (`:42-43`): "revert happens on LINK_DOWN only, LINK_UP re-declares the defaults".

## Validation (author)

| What | Result | Log |
|---|---|---|
| `make -j1 check`, run once, at the clean head `bc997e7` | exit 0 in 25 s: 41 mermaid + 18 wavedrom blocks, 18 wavedrom fresh, 807 links, 115 REQ / 17 GAP, 86 module rows / 0 untested, nothing stale. The porcelain was empty before and after. | `logs/01` |
| `git diff --check`, for `88a4eb4..bc997e7` and `8452f56..bc997e7` | exit 0 for both | `logs/02` §3 |
| scope, identity, hash and fence receipts | see "Unchanged scope" and "Render evidence" | `logs/02` |
| the saved patch applied to `88a4eb4` in a `/tmp` index | gives tree `bd67eaac…`, the head tree | `logs/03` |
| final state, remote, PR list, first-round packet checksums | clean, not pushed, no PR, 29 of 29 OK | `logs/04` |

Which gates cover the changed file:
- `links` walks every Markdown file in the repository, `tb/` included (`scripts/check-links.py:38-56`), so it scanned this README. The README has no links.
- `lint` renders the Mermaid and WaveDrom blocks in `docs/**/*.md` and the root README (`scripts/lint-diagrams.sh:28`). The tb README has no block. F10.2 is among the 41 Mermaid blocks it rendered at head.
- `matrix` reads 00, `modmatrix` reads `hdl/**/*.sv` and `tb/*/Makefile`, `wavedrom-check` walks `docs/`, and `stale` compares the draw.io sources with their exports. None of them reads the changed file, and all pass.

Donor image commands (`docs/README.md` §6): none apply. No Mermaid fence, WaveDrom block, draw.io source or hand-authored SVG was touched.

## Render evidence (first round, attributed, not re-rendered)

The F10.2 render proof comes from the first round. It was made at `88a4eb4` and is kept in `$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor98-author/`:
- the head renders: `render/F10.2-head-88a4eb4.mmd` (sha256 `f16fb070…2a47`), `.svg` (`b2edf478…3873`) and `.png` (`5533d609…fb8f`), with the base renders at `8452f56` beside them;
- `logs/05-mmdc-render.log`: mmdc 11.16.0, 4 of 4 renders exit 0;
- `logs/06` and `logs/07`: the base and head comparison;
- `logs/08`, the negative control: a broken F10.2 fence fails `scripts/lint-diagrams.sh`.

That proof applies unchanged to `bc997e7` (`logs/02` §6-7):
- **The fence.** Extracted by anchor `fig-10-domsm`, the F10.2 fence at `bc997e7` has sha256 `f16fb070…2a47`, and `cmp` finds it byte-identical to the rendered `render/F10.2-head-88a4eb4.mmd`. The same extraction at `8452f56` also matches the rendered base source byte for byte, so it is the same extraction the first round rendered.
- **The file.** `docs/architecture/10_srp_engine.md` is the same blob at `88a4eb4` and at head: `b8dafc54…`, sha256 `6406ddce…ee60`.
- **The renderer.** `Makefile`, `scripts/lint-diagrams.sh` (`de470321…`) and the other gate scripts are the same blobs.
- **The RTL.** The RTL the render was compared against is the same blob, `96c57db1…`.

No image was re-rendered. The render, and its comparison against `KL_srp_domain.sv:155-171`, stay attributed to `88a4eb4`.

## Unchanged scope

Every tracked path except `tb/srp_encoder/README.md` has the same mode and blob at `88a4eb4` and `bc997e7`. The tracked file list is also the same, 222 paths (`logs/02` §7). That covers:
- **Documents.** The figure and every document: all of `docs/`, including F10.2's file, 01 F01.5 and the guides.
- **Masters and exports.** All of `docs/diagrams`: 3 draw.io sources and 34 committed assets, including `wavedrom/`.
- **Renderer and gates.** `Makefile` and `scripts/`: `lint-diagrams.sh`, `render-wavedrom.py`, `check-links.py`, `check-matrix.py`, `gen_matrix.py` and the rest.
- **Executable bytes.** All 10 `100755` files, plus `hdl/`, the rest of `tb/`, `syn/` and `.github/`.
- **Behaviour.** RTL, tests, timers, parameters, interfaces and the product pin.

Against `main`, the combined head changes exactly two lines:
- `docs/architecture/10_srp_engine.md:205` (`88a4eb4`);
- `tb/srp_encoder/README.md:75` (`bc997e7`).

`git diff --check 8452f56 bc997e7` is clean. The combined diff is in `8452f56..bc997e7.combined.diff`.

## Evidence reused, not re-run

- The first round cited executable evidence: R223 receipts, mutants R36 and R37, and hosted main run 35694588985 with srp_encoder 180/0, which includes D9. This commit leaves that evidence's inputs unchanged. It changes one Markdown line that no build or suite reads, and every `hdl/` and `tb/` source blob is identical. See the first-round HANDOFF.md, "Evidence reused, not re-run".
- Not run, by assignment:
  - suites, `lint_hdl.sh` and Yosys;
  - hosted, act and Docker runs, and hardware;
  - a second `make check`, the per-target runs, an mmdc re-render and the negative control.

## Issue #98 acceptance (unchanged)

- **Restore and declare events.** Met at `88a4eb4` and unchanged at `bc997e7`, where the fence is byte-identical. The tb note now agrees with the figure and no longer carries a stale quote.
- **Generated assets match their master.** No generated asset of F10.2 exists. `docs/diagrams` is unchanged, and `wavedrom-check` and `stale` pass at `bc997e7`.
- **Other transitions unchanged.** The fence is byte-identical to `88a4eb4`'s, whose other four arcs were byte-identical to base.
- **Checks and comparison.** `make check`, with Mermaid and links, passes at `bc997e7`. The comparison of the rendered figure against `KL_srp_domain.sv:155-171` still holds (see Render evidence).
- **Review and merge workflow.** Owned by the manager.

## Notes for the manager and reviewers

1. The first-round `PR-BODY.md` describes `88a4eb4` alone. Three places are outdated for the combined head:
   - the Status paragraph ("one Mermaid label line");
   - "How to validate" (`git checkout 88a4eb4`, "1 file, 1 insertion, 1 deletion");
   - the "Out of scope" bullet saying that `tb/srp_encoder/README.md:75-77` still quotes the old label.

   I left that packet untouched and wrote no new PR body, because publication is owned by the manager.
2. The decision adopts none of the first round's other observations, so none changed here:
   - DOMAIN_CHANGE is not named on the arc;
   - no LINK_DOWN arc is drawn out of DEFAULTS;
   - the render's labels are crowded;
   - the render-wavedrom message is lost without a flush;
   - GitHub renders with its own Mermaid version.

## Remaining (manager-owned)

- donor full native and hosted validation on the final combined head;
- the two cold reviews, R233 (internal) and R234 (external), which I did not start;
- current-candidate and post-merge containment;
- publication, merge and project status.

## This folder

- `TAKEN.md` and `TAKEN-URL.txt`: the published follow-up TAKEN, byte-identical.
- `REVIEW-READY.md` and `REVIEW-READY-URL.txt`: the published follow-up REVIEW READY, byte-identical.
- `HANDOFF.md`, `COMMANDS.md` and `git-final.txt`.
- The patches:
  - `bc997e7.patch`: `git format-patch -1`;
  - `88a4eb4..bc997e7.diff`: this commit;
  - `8452f56..bc997e7.combined.diff`: the combined head against `main`.
- `scope-receipts.sh`: the exact script run.
- `logs/01`-`04`: raw outputs.
- `SHA256SUMS`: every other file in this folder.
