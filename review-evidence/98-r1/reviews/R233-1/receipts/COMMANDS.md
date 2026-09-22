# R233-1 receipts: commands

Reviewer R233 (cold internal Opus), round R233-1, PR101 / issue #98, exact head
`bc997e7c00e50e3d59b97987950bfbf6cc442182`. Everything ran on 2026-09-22 (CEST).
Every shell command was prefixed with `rtk` (`rtk proxy <cmd>` where raw output was
kept). Tools: rtk 0.43.0, git, gh (read-only API calls), mmdc 11.16.0, node v22.23.2,
Python 3.14.7 (stdlib only), GNU make. Nothing was installed.

Paths:

- `REVIEW` = `$VALIDATION_STORAGE/reviews/r233-101-r1`. The assigned detached review clone. Only
  read-only git commands ran here (`rev-parse`, `status`, `ls-files`, `ls-tree`, `diff`,
  `hash-object`, `write-tree` from an unchanged index, `for-each-ref`). There was no fetch,
  checkout, build or write.
- `S` = `/tmp/r233-101-r1`. Isolated scratch. It holds `repo/`, a separate `git init`
  with the public commits fetched, plus worktrees and a `git archive` copy made from it.
- `B`=`8452f564294300a82d56eed464276576f65f4d58`, `F`=`88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf`,
  `H`=`bc997e7c00e50e3d59b97987950bfbf6cc442182`, `M`=`c8214cfe827fb3fb50c9bfee415468a24b7c27b1`
  (main after PR #100). `E1`=`94f47697f765e96a3c930dc09606fb7b559d3187` and
  `E2`=`c0b98764fb116b857cce0cfc389ad298c5948bed` (evidence branch `98-review-evidence`).
  `E96`=`29b2066b91cf4b0387edff43250b8fe79192232b` (PR96 evidence, R223 receipts).

| Receipt | Commands (abridged; the receipt header repeats the method) |
|---|---|
| 00, 99 | In `REVIEW`: `git rev-parse HEAD HEAD^{tree}`; `git status --porcelain=v2 --branch --untracked-files=all --ignored`; `git ls-files -s \| sha256sum`; `sha256sum .git/index`; `git ls-tree -r HEAD \| sha256sum`. Receipt 99 adds `git diff --cached --quiet`, `git diff --quiet`, a per-file `git hash-object --no-filters` against the index blob, the executable bit against the index mode, and `git for-each-ref`. |
| 10, 11, 12, 13, 14 | `gh api repos/<repo>/commits/$H/check-runs`; `.../commits/$H/status`; `.../actions/runs?head_sha=$H`; `.../actions/runs/<id>/jobs`; `gh api --allow-escape-sequences .../actions/jobs/<id>/logs` for the 6 jobs (saved under `hosted-logs/`), then `grep -a` of the verdict lines; `gh pr view 101 --json ...`; comment headers via `.../issues/{101,98}/comments`. |
| 20, 21, 22 | In `S/repo`: `git fetch --no-tags origin $E1 ...`; `git archive $E1 review-evidence/98-r1 \| tar -x`; `sha256sum` of every MANIFEST entry against `published_sha256`, with a consistency check on `path_redacted`; each author `SHA256SUMS` line checked against the file (published) or MANIFEST `original_sha256` (redacted); `cmp <(git diff $B $H) author/followup/8452f56..bc997e7.combined.diff`; `GIT_INDEX_FILE=$S/tmp-index git read-tree $F && git apply --cached bc997e7.patch && git write-tree`. |
| 30 | For `B`, `F` and `H`: `git show <rev>:docs/architecture/10_srp_engine.md \| awk` extracts the lines strictly between the first ` ```mermaid ` after anchor `fig-10-domsm` and the next closing fence, then `sha256sum`. |
| 31, 32 | `printf '{"args":["--no-sandbox","--disable-gpu"]}\n' > puppeteer.json` (the lint's own config); `mmdc -p puppeteer.json -i F10.2-<rev>.mmd -o r233-F10.2-<rev>.svg`; the same with `-o ....png -b white -s 2`; `sha256sum` against the author's `render/` files. |
| 33 | `python3 scripts/svg_edge_probe.py r233-F10.2-8452f56.svg r233-F10.2-bc997e7.svg` (R233's own probe; it does not reuse the author's scripts). |
| 34 | `python3 scripts/png_diff_bbox.py r233-F10.2-8452f56.png r233-F10.2-bc997e7.png` (a stdlib PNG decoder). |
| 40 | In worktree `S/wt-head` (`git worktree add --detach S/wt-head $H` from `S/repo`): `make -j1 lint`, `links`, `matrix`, `modmatrix` and `stale`, each alone. `wavedrom-check` was not run (see REPORT limits). |
| 41 | The concatenated ` ```wavedrom ` sources of every `docs/**/*.md` at `B` and `H` are hashed; `git rev-parse <rev>:docs/diagrams`; `git diff -U0 $B $H \| grep '^@@'`. |
| 42 | `git archive $H \| tar -x -C S/negctl`; `./scripts/lint-diagrams.sh` on the unmodified copy; `sed` turns line 205 `ADOPTED --> DEFAULTS:` into `ADOPTED --> --> DEFAULTS:`; `./scripts/lint-diagrams.sh` again. The copy was then deleted. |
| 43 | `git rev-list --parents $B..$H`; `git cat-file commit`; `git interpret-trailers --parse`; `git diff-tree -r`; `git diff --name-status/--numstat/--summary/--check`; a non-ASCII and trailing-space scan of the added lines; a mode+blob `git ls-tree -r` comparison of the other 220 entries; `100755` entries at `B` and `H`. |
| 44 | `git ls-files docs/diagrams` (svg/png/drawio); `grep -o -i` for Domain FSM terms in SVG/drawio; `strings \| grep` for PNGs. |
| 45, 46 | `git grep -n -I` at `H` (and `B` for contrast) for revert/restore wording near link events, the old label and the new label text. |
| 50 | `git merge-base --is-ancestor $B $M`; `git diff --name-status $B $M`; the path overlap with PR101; `git merge-tree --write-tree $M $H`; old-label grep and DV5/DV6 function bodies in the resulting tree. Informational only; not a candidate validation. |
| 51 | `git fetch $E2`; `git archive $E2 review-evidence/98-r1`; every `candidate1/` MANIFEST entry checked for presence and hash; `git check-ignore -v --no-index` for an absent log path. |
| 52 | `git fetch` of the evidence branch head `4640f5f8138418ee22bd4ae7ad5383303f6aa6cc`; `git archive` of only `review-evidence/98-r1/candidate1` and `MANIFEST.json` (the `reviews/R234-1/` folder was not extracted or opened); each candidate1 entry hashed against this MANIFEST and against the `c0b9876` MANIFEST; `grep` of the verdict lines of logs 01-09. |
| 61 | `git fetch $E96`; `git archive`; `sha256sum -c --quiet SHA256SUMS` in `reviews/R223-1/receipts`; 96-r1 MANIFEST hashes for receipts 02, 09 and the R36/R37 mutation folders; `grep` of their tallies. |

One command was not executed. A focused mutation probe of `tb/srp_encoder` (a control build
plus a mutant that defers the four revert lines of `KL_srp_domain.sv:157-160` from the
LINK_DOWN branch to the LINK_UP branch) was denied by the session's tool permission. No part
of it ran, and no mutant or build directory was created (checked with a file glob afterwards).
