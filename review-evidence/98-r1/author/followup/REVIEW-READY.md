[A165] REVIEW READY (follow-up; manager validation, publication and the R233/R234 reviews pending)

Commit `bc997e7c00e50e3d59b97987950bfbf6cc442182`, tree `bd67eaac225513f84ecf3a228cf58a818e67b4da`. Its parent is the figure commit `88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf`, tree `1a65be1c93e51c2ad94e467abc2452a047047bdc`. The branch `98-clarify-srp-domain-events` is clean and two commits ahead of `main` `8452f564294300a82d56eed464276576f65f4d58`. It is not pushed. The commit message is one line with no trailers.

Changed: only `tb/srp_encoder/README.md:75`, as authorized by the [correction decision](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773480374).
- before: `- The revert edge reads "LINK_DOWN then LINK_UP / back to defaults": the`
- after: `- The F10.2 revert edge (ADOPTED to DEFAULTS) fires on LINK_DOWN: the`

The note now describes the edge instead of copying its label, so the F10.2 fence stays the label's only home (`docs/README.md` §3). The interpretation at `:76-77` is byte-identical: the defaults are restored at LINK_DOWN, nothing is declared on a dead link, and LINK_UP declares them. It matches `KL_srp_domain.sv:155-171` and D9, both unchanged.

Validation at the clean head:
- `make -j1 check`, run once: exit 0. It checked 41 mermaid (mmdc) + 18 wavedrom blocks, 18 wavedrom fresh, 807 links, 115 REQ / 17 GAP, 86 module rows with 0 untested, and nothing stale.
- `git diff --check` is clean against `88a4eb4` and against `main`.
- Scope: 1 insertion and 1 deletion, the mode is unchanged, and the new line is ASCII.
- Every other tracked path has the same mode and blob as at `88a4eb4`. That covers F10.2's file, `docs/diagrams`, the Makefile and gate scripts, `hdl/`, the rest of `tb/`, `syn/`, `.github/` and all 10 executable files.
- The old label no longer appears anywhere in the tree. The new label's text appears only at `10_srp_engine.md:205`.

Render evidence: not re-rendered. The F10.2 fence at `bc997e7` is byte-identical (sha256 `f16fb070…2a47`) to the source rendered at `88a4eb4` for the [first-round handoff](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773433155). That render and its RTL comparison therefore still apply, and they stay attributed to `88a4eb4`.

Against `main`, the combined head changes `10_srp_engine.md:205` and `tb/srp_encoder/README.md:75`. Acceptance is unchanged. Donor full validation, R233/R234, the candidate and containment stay with the manager. I did not push, open a PR, change the project, merge, start a review, run suites, run Docker or act, or touch hardware. This is author evidence, not approval.
