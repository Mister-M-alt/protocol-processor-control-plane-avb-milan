# R233-1 receipts index

These are the factual receipts for `../REPORT.md` (R233-1, PR101, exact head
`bc997e7c00e50e3d59b97987950bfbf6cc442182`). How each receipt was produced is in
`COMMANDS.md`. `SHA256SUMS` covers every file in this folder except itself.

| File | Content |
|---|---|
| 00-baseline-integrity.txt | the review clone's state at round start |
| 10-hosted-check-runs-bc997e7.json | check-runs API response at the head (6 runs) |
| 11-hosted-workflow-runs-bc997e7.json | workflow-runs API response at the head (2 runs) |
| 12-hosted-job-logs-extract.txt | verdict lines from the 6 hosted job logs |
| 13-live-state-at-report.txt | PR, main, evidence branch, comment headers and check runs at 13:25:50 |
| 14-live-state-update.txt | the same at 13:32:39: R234 and A10 comment header lines only, MERGEABLE/CLEAN |
| 20-evidence-manifest-verify.txt | evidence commit 94f4769: 62/62 MANIFEST hashes and redaction flags |
| 21-author-sha256sums-verify.txt | the author's initial and follow-up SHA256SUMS against the files and MANIFEST |
| 22-author-patch-reproduction.txt | the published diffs and patch against git objects; the patch rebuilds tree bd67eaa |
| 30-f10.2-fence-hashes.txt | the F10.2 fence at 8452f56, 88a4eb4 and bc997e7: bytes, sha256, diff |
| 31-independent-mmdc-render.txt | R233's mmdc renders (SVG and PNG, base and head) |
| 32-render-compare.txt | R233's renders against the author's: all 4 byte-identical |
| 33-svg-edge-probe.txt | edge → (source, target, label) for base and head renders |
| 34-png-diff-bbox.txt | differing-pixel count and bounding box, base vs head PNG |
| 40-head-docs-gates.txt | `make lint`, `links`, `matrix`, `modmatrix` and `stale` at bc997e7 |
| 41-wavedrom-and-scope.txt | WaveDrom sources and docs/diagrams unchanged; hunks; gate scan scopes |
| 42-mermaid-negative-control.txt | the lint passes unmodified and fails on a planted F10.2 break |
| 43-scope-hygiene.txt | commits, messages, trailers, diff-tree, check, ASCII, modes, unchanged blobs |
| 44-diagram-assets.txt | the 34 committed assets hold no depiction of F10.2 |
| 45-revert-wording-sweep.txt | every revert/restore statement near a link event in the tree |
| 46-label-text.txt | the old label (0 hits at head), the new label (1 hit), the fence and the README note |
| 50-new-main-merge-tree.txt | main advanced to c8214cf: disjoint paths, merge-tree 37a5cd8 (informational) |
| 51-candidate1-evidence.txt | evidence commit c0b9876: candidate1 files present, 9 raw logs absent |
| 52-candidate1-logs-4640f5f.txt | evidence commit 4640f5f: the 9 logs added, hashes match both MANIFESTs, verdict lines |
| 61-r223-public-receipts.txt | PR96 R223 receipts (commit 29b2066): SHA256SUMS, MANIFEST, R36/R37 |
| 99-final-integrity.txt | the review clone's state at finish (identical to 00) |
| render/ | the extracted fences (`.mmd`), R233's renders and the puppeteer config |
| hosted-logs/ | the 6 raw hosted job logs for runs 35706840515 and 35706849831 |
| scripts/ | `svg_edge_probe.py` and `png_diff_bbox.py` (stdlib only) |
| COMMANDS.md | how each receipt was produced |
