[A165] REVIEW READY (author handoff; manager validation, publication and the R233/R234 reviews pending)

Commit: `88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf`, tree `1a65be1c93e51c2ad94e467abc2452a047047bdc`
Base: `main` `8452f564294300a82d56eed464276576f65f4d58`, tree `ec259f379560863e6ea49c6043353f0c11fe714d`
Branch: `98-clarify-srp-domain-events`. It is clean and committed locally, one commit ahead of its remote, and not pushed. The commit message is one line with no trailers.

Changed: only `docs/architecture/10_srp_engine.md:205`, the F10.2 arc `ADOPTED --> DEFAULTS`:
- before: `LINK_DOWN then LINK_UP / back to defaults`
- after: `LINK_DOWN / restore defaults, declared again on the next LINK_UP`

The diff is 1 insertion and 1 deletion. States, the other four arcs and all prose are byte-identical. No arc was added. The LINK_UP declaration stays on the existing `[*]` arc (`:202`).

RTL check (`hdl/srp/KL_srp_domain.sv`, unchanged):
- The LINK_DOWN branch `:155-165` loads the defaults, clears ADOPTED and `declared_r`, and puts nothing on the wire.
- The LINK_UP branch `:166-171` queues `New {6, DEF_PRIO_P, DEF_VID_P}`.
- While the link is down, `declared_r = 0` blocks adoption and re-join (`:144`, `:150`, `:185`). So the next LINK_UP is the next declaration.

Validation:
- `make -j1 check`, rc 0 at base, on the pre-commit worktree and at the clean head. It checked 41 mermaid (mmdc) + 18 wavedrom blocks, 18 wavedrom fresh, 807 links, 115 REQ / 17 GAP, 86 module rows with 0 untested, and nothing stale. Each of the six targets also passes alone at head.
- mmdc 11.16.0 rendered F10.2 at base and head as SVG and PNG. In both, `edge3` runs from ADOPTED to DEFAULTS. Only its label changed, and it now reads as above. States, node positions and the other four labels are identical. The rendering matches the two RTL branches.
- Negative control on a scratch `git archive`: a broken F10.2 fence fails `scripts/lint-diagrams.sh` (rc 1, parse error at fence line 5). The Mermaid gate is real.
- Scope: `git diff --name-status` lists only this file. `hdl/`, `tb/`, `syn/`, `scripts/`, `.github/`, `docs/diagrams/` and every other document are identical. `git diff --check` is clean. The added line is plain ASCII.

Exports: 34 committed diagram assets, none of which represents F10.2 (0 term hits in SVG, draw.io or PNG). GitHub renders the fence natively, so there was nothing to regenerate and no asset changed. The only generated images are the external renders in the author folder.

Executable evidence (reused, not re-run; its sha256 matches published MANIFEST.json and SHA256SUMS):
- The base tree equals the PR96 reviewed head tree. R223 receipt 02 records default 1391/0, fixture 20/0 and 1411/1411, with DV1 to DV6 in both builds.
- R36 changes only the LINK_DOWN branch (`:159`). It fails DV5 x3 and DV6's class-D check.
- R37 changes only the LINK_UP branch (`:170`). It fails DV2 x2 and DV6's wire check.
- Hosted main run 35694588985 at exactly `8452f56` succeeded: pp_top 1411/0, srp_encoder 180/0 (D9 covers LINK_DOWN and LINK_UP), srp_top 235/0, and 14943 checks with 0 failing.
- The head changes no input of any build or suite.

Issue #98 acceptance:
- **Met, restore and declare events:** the diagram shows default restoration at LINK_DOWN and declaration at LINK_UP, in the source and in the render.
- **Met, generated assets:** no generated asset of F10.2 exists; none changed; `wavedrom-check` and `stale` pass.
- **Met, other transitions:** their meanings are unchanged, byte-identical in the source and identical in the render.
- **Met, checks and comparison:** `make check` (with Mermaid and links) passes, and the rendered figure was compared against `KL_srp_domain.sv:155-171`.
- **Not author scope, review/merge workflow:** donor full validation, R233 and R234, candidate and containment stay with the manager.

Not changed, flagged:
- `tb/srp_encoder/README.md:75-77` still quotes the old label in a recorded-interpretation note. Its interpretation matches the new label. It is a `tb/` note and out of this scope.
- The arc does not name DOMAIN_CHANGE, and it did not before. The RTL strobes it on this revert (`:157`), and DV5 grades it.

I did not push, open a PR, change the project, merge, start a review, run suites, run Docker or act, or touch hardware or the parent. This is author evidence, not approval.
