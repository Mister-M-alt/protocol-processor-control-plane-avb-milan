[A165] Exact commands and results for the donor issue #98 follow-up

Everything ran on 2026-09-22 (CEST). The working directory was the lane clone `$CANDIDATE`, except where a row says otherwise. Shell commands carried the `rtk` prefix. Every command whose output is recorded here ran as `rtk proxy <command>`, so the logs hold raw, unfiltered output.

Names used below:
- `D`: this folder, `$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor98-followup-author`.
- `O`: the first-round packet, `$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor98-author`. It was only read, never written.

Tools: git 2.55.0, gh 2.101.0, jq 1.8.2, GNU Make 4.4.1, Python 3.14.7, node v22.23.2, mmdc 11.16.0 (used by `make check`) and rtk 0.43.0. Nothing was installed. `make check` reused the clone-local, gitignored `.venv-wavedrom/` from the first round.

| # | Time | Tree | Command | Result | Record |
|---|---|---|---|---|---|
| 0 | 10:32-10:35 | `88a4eb4`, clean | `rtk git status`; `git rev-parse HEAD`; `git branch --show-current`; `git log --oneline -5` | Clean at `88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf` on `98-clarify-srp-domain-events`, one commit ahead of its remote. | git-final.txt |
| 0b | 10:32-10:35 | n/a | `gh issue view 98 --json ...`; `gh api .../issues/comments/<id>` for 5773239576, 5773306413, 5773433155 and 5773480374; `gh api --paginate .../issues/98/comments` | 5773480374, the A10 correction decision, is the newest comment. It authorizes only the correction to the `tb/srp_encoder/README.md` quote. `../donor98-quote-decision.md` has the same text. | HANDOFF.md |
| 0c | 10:32-10:35 | `88a4eb4` | Read `docs/README.md`, F10.2 (`10_srp_engine.md:180-229`), `tb/srp_encoder/README.md`, `Makefile` and `scripts/check-links.py:38-56`. Grep `scripts/check-links.py` and `scripts/lint-diagrams.sh` for the files they scan (`lint-diagrams.sh:28`). Read `git show HEAD:hdl/srp/KL_srp_domain.sv` (`:105-192`) and `git show HEAD:tb/srp_encoder/sim_main.cpp` (`:685-710`). | Authority and evidence for the wording (HANDOFF.md). `links` walks every `.md` file, `tb/` included. `lint` scans only `docs/**/*.md` and the root README. | HANDOFF.md |
| 0d | 10:32-10:35 | `O` | `find O -maxdepth 3` (names only). Read O's HANDOFF.md, COMMANDS.md, SHA256SUMS, git-final.txt, TAKEN-URL.txt, REVIEW-READY-URL.txt, logs/04, logs/05, render/F10.2-head-88a4eb4.mmd and scope-receipts.sh. PR-BODY.md was read at 10:41. | The first round's factual evidence. No CLI event or reasoning record was opened. | HANDOFF.md |
| 0e | 10:32-10:35 | `O` | `sha256sum -c O/SHA256SUMS`, first run from the lane | Could not open any of the 29 listed files. The paths are relative to O and I ran it from the lane, so this was my path error, not a packet finding. | none |
| 0f | 10:32-10:35 | `O` | `sed "s\|  \|  O/\|" O/SHA256SUMS \| sha256sum -c --strict` | 29 of 29 OK, exit 0. Re-checked in row 5, §11. | logs/02 §11 |
| 1 | 10:36:10 | n/a | `gh issue comment 98 --body-file D/TAKEN.md` | Published: https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773538937. `gh api ... \| jq -j .body \| sha256sum` gives `7f199555…26c2`, equal to the local file (1049 bytes). A first comparison through `--jq .body` differed only by the newline that `--jq` appends. | TAKEN.md, TAKEN-URL.txt |
| 2 | 10:36 | worktree | The agent's file-edit tool replaced one line, `tb/srp_encoder/README.md:75`. Then `git diff --stat`, `git diff -U1`, `git diff --check`, and `awk` for line lengths at `:74-78`. | 1 file, 1 insertion, 1 deletion. `diff --check` exit 0. The new line is 69 characters (`:76` is 74) and has 0 non-ASCII characters. | logs/02 §3-4 |
| 3 | 10:36:53 | n/a | `git add tb/srp_encoder/README.md && git commit -q -m "Describe F10.2's revert edge in the srp_encoder README instead of quoting its old label"` | Commit `bc997e7c00e50e3d59b97987950bfbf6cc442182`, parent `88a4eb4…`, tree `bd67eaac225513f84ecf3a228cf58a818e67b4da`. `.git/hooks` holds only `*.sample` files. | logs/02 §1-2 |
| 4 | 10:37:00 | `bc997e7`, clean | `make -j1 check`, the only run | exit 0 in 25 s. Output: `lint: 41 mermaid + 18 wavedrom blocks checked, OK`; `wavedrom: 18 blocks checked, OK`; `links: 807 checked, OK`; `matrix: 115 REQ rows, 17 GAP findings, OK`; `matrix: OK (86 rows, 0 untested)`. `stale` prints nothing on success. The porcelain was empty before and after. | logs/01-head-make-check.log |
| 5 | 10:39:04 | `bc997e7` | `bash D/scope-receipts.sh` | exit 0. See the next list. | logs/02-scope-receipts.txt |
| 6 | 10:39:27 | `/tmp` index | `git format-patch -1 HEAD --stdout > D/bc997e7.patch`; `git diff 88a4eb4 HEAD > D/88a4eb4..bc997e7.diff`; `git diff 8452f56 HEAD > D/8452f56..bc997e7.combined.diff`; then, with `GIT_INDEX_FILE=/tmp/a165-f98-index`, `git read-tree 88a4eb4`, `git apply --cached [--check] D/bc997e7.patch` and `git write-tree` | The patch applies to `88a4eb4` and gives tree `bd67eaac…`, equal to the head tree. The saved diff equals `git diff` (`cmp` exit 0). The temporary index was removed, and the lane porcelain stayed empty. | logs/03-patch-reproduces-tree.txt |
| 7 | 10:39:36 | `bc997e7` | `git rev-parse`, `git log -1`, `git status -sb --ignored`, `git diff --stat`, `git ls-remote` | Final identity and remote state. | git-final.txt |
| 8 | 10:40:25 | n/a | `gh issue comment 98 --body-file D/REVIEW-READY.md` | Published: https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773588874. `jq -j .body \| sha256sum` gives `40b13570…4a34`, equal to the local file (2700 bytes). | REVIEW-READY.md, REVIEW-READY-URL.txt |
| 9 | after HANDOFF.md | `D`, `O` | `sha256sum` over every file in D except `SHA256SUMS`, then `sha256sum -c` of D and O, `git status`, and `git rev-parse HEAD` | See HANDOFF.md, "Final state". | SHA256SUMS |

Scope receipt results (row 5, `logs/02`):

- **Identity:** HEAD `bc997e7…`, tree `bd67eaac…`. `HEAD^` is `88a4eb4…`. `git rev-list --count` gives 1 from `88a4eb4` and 2 from `8452f56`.
- **Message:** one non-empty line. `git interpret-trailers --parse` finds no trailer.
- **Paths, `88a4eb4..HEAD`:**
  - `--name-status` gives only `M tb/srp_encoder/README.md`, and `--numstat` gives `1 1`.
  - `--summary` is empty.
  - `diff-tree`: `:100644 100644 2eae73950323db4cddaf9b263bd5eeee0ca13573 11092a208b3248690e1d1fd723c2d66c8f269b90 M`.
  - `git diff --check` exits 0, both from `88a4eb4` and from `8452f56`.
  - The file's sha256 is `a4af61b4…77df` at `88a4eb4` and `654ed423…0bf1` at head.
- **Hunk:** one hunk, `@@ -75 +75 @@`. The added line has 0 U+2013/U+2014 and 0 non-ASCII characters. `:76-77` and every line except `:75` are byte-identical at `88a4eb4` and head.
- **Stale quote gone:**
  - `git grep -F 'LINK_DOWN then LINK_UP'` finds only `tb/srp_encoder/README.md:75` at `88a4eb4`, and nothing at head (exit 1).
  - `'back to defaults'` and `'revert edge reads'` find nothing at head (exit 1).
  - The new label's text `restore defaults, declared again on the next LINK_UP` has exactly one hit at head, `docs/architecture/10_srp_engine.md:205`.
- **F10.2 source:**
  - The blob of `10_srp_engine.md` is `b8dafc54…` at both `88a4eb4` and head; the sha256 is `6406ddce…ee60` at both.
  - The fence extracted by anchor `fig-10-domsm` has sha256 `f16fb070…2a47` at `88a4eb4` and at head. It is byte-identical (`cmp`) to O's `render/F10.2-head-88a4eb4.mmd`.
  - The base fence `b05461fa…0ff` is byte-identical to O's `render/F10.2-base-8452f56.mmd`.
- **Everything else identical:**
  - `git diff --quiet 88a4eb4 HEAD -- . ':(exclude)tb/srp_encoder/README.md'` exits 0, and so does `-- ':(exclude)*.md'`.
  - Mode and blob of every tracked path except the note are identical. The tracked file list is identical, with 222 paths.
  - Each of these path sets exits 0: `hdl`, `syn`, `scripts`, `Makefile`, `.github`, `.gitignore`, `README.md`, `IEEE_1722_1_Hardware_Protocol_Processor.md`, `docs`, `docs/diagrams`, `docs/diagrams/src`, `docs/diagrams/wavedrom`, `docs/architecture/10_srp_engine.md`, and `tb` without the note.
  - The renderer and gate scripts are all `same`: `Makefile`, `lint-diagrams.sh`, `render-wavedrom.py`, `check-links.py`, `check-matrix.py` and `gen_matrix.py`.
  - All 10 `100755` files are `same`.
  - The note's mode is 100644 at `88a4eb4` and at head.
- **Assets:** `git diff --stat -- docs/diagrams` is empty. There are 34 committed diagram assets.
- **RTL and evidence blobs** are identical at `88a4eb4` and head: `KL_srp_domain.sv` `96c57db1…`, `tb/srp_encoder/sim_main.cpp` `eab9912e…`, `tb/pp_top/sim_main.cpp` `e52f0fde…`.
- **Remote:** `git ls-remote` shows `main` and `98-clarify-srp-domain-events` still at `8452f56…`. Nothing was pushed.
- **O is intact:** 29 of 29 OK.

Not run, by assignment:
- a second `make check` and the per-target runs;
- an mmdc re-render and the Mermaid negative control;
- `./scripts/run_suites.sh` or any Verilator suite, `./scripts/lint_hdl.sh`, and `./syn/yosys/run.sh`;
- hosted, act or Docker runs, and hardware.

None of their inputs changed (HANDOFF.md, "Evidence reused").
