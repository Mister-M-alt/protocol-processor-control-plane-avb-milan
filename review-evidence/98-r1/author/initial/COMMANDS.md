[A165] Exact commands and results for donor issue #98

Everything ran on 2026-09-22 (CEST). The working directory was the lane clone `$CANDIDATE`, except where a row says otherwise. Shell commands carried the `rtk` prefix. Every command whose output is recorded here ran as `rtk proxy <command>`, so the logs hold raw, unfiltered output. `D` is this folder, `$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor98-author`.

Tools: mmdc 11.16.0 (`$WORKSPACE_HOME/.local/bin/mmdc`, pre-existing), node v22.23.2, Python 3.14.7, rsvg-convert 2.62.3 (not needed). Nothing was installed globally. `make check` created the clone-local, gitignored `.venv-wavedrom/` itself (row 2).

| # | Time | Tree | Command | Result | Record |
|---|---|---|---|---|---|
| 0 | 10:12-10:16 | base | `rtk git status`; `git rev-parse HEAD HEAD^{tree}`; `rtk git fetch origin`; `git rev-parse origin/main`; `git ls-remote origin HEAD main 98-clarify-srp-domain-events` | Clean. HEAD, origin/main, remote HEAD and the remote branch are all `8452f564294300a82d56eed464276576f65f4d58`, tree `ec259f379560863e6ea49c6043353f0c11fe714d`. That tree equals PR96 head `ea93023`'s tree. | HANDOFF.md |
| 0b | 10:17 | base | `gh issue comment 98 --body-file D/TAKEN.md` | Published: https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773306413. The published body's sha256 `dbae1911…1e54` (1828 bytes) equals the local file's. | TAKEN.md, TAKEN-URL.txt |
| 1 | 10:17:31 | base, clean | `make -j1 check` | exit 0 in 28 s. Output: `lint: 41 mermaid + 18 wavedrom blocks checked, OK`; `wavedrom: 18 blocks checked, OK`; `links: 807 checked, OK`; `matrix: 115 REQ rows, 17 GAP findings, OK`; `matrix: OK (86 rows, 0 untested)`. `stale` prints nothing on success. | logs/01-base-make-check.log |
| 2 | 10:18 | n/a | `stat`, `cat pyvenv.cfg`, `pip freeze` and `git check-ignore -v` on `.venv-wavedrom` | Created at 10:17:56 by row 1: `scripts/render-wavedrom.py` builds it when `wavedrom` is not importable. It is clone-local, `include-system-site-packages = false`, and ignored by `.gitignore:12`. It holds `wavedrom==2.0.3.post3`, `svgwrite==1.4.3`, `six==1.17.0` and `PyYAML==6.0.3`. The script's "bootstrapping" message is absent from log 01 because it prints before `os.execv` without a flush. | logs/02-venv-wavedrom.txt |
| 3 | 10:18:42 | worktree with the uncommitted edit | `make -j1 check` | exit 0 in 25 s, with the same five lines as row 1. The worktree blob of the file is `b8dafc54…`. | logs/03-precommit-make-check.log |
| 3b | 10:19:15 | n/a | `git add docs/architecture/10_srp_engine.md && git commit -q -m "Show F10.2's Domain defaults restored at LINK_DOWN and declared at the next LINK_UP"` | Commit `88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf`, parent `8452f56…`, tree `1a65be1c93e51c2ad94e467abc2452a047047bdc`. `.git/hooks` holds only `*.sample` files. | logs/10-scope-receipts.txt §1-2 |
| 4 | 10:19:22 | head, clean | `make -j1 check` | exit 0 in 26 s, with the same five lines. The porcelain was empty before and after. | logs/04-head-make-check.log |
| 5 | 10:20:11 | base and head git objects | The F10.2 fence was extracted from `git show <rev>:docs/architecture/10_srp_engine.md` by anchor `fig-10-domsm`. Then `mmdc -p /tmp/a165-puppeteer.json -i F10.2-<rev>.mmd -o F10.2-<rev>.svg`, and the same with `-o F10.2-<rev>.png -b white -s 2`. The puppeteer config is the repository lint's own: `{"args":["--no-sandbox","--disable-gpu"]}`. | All 4 renders exit 0. The two fences differ only in fence line 5 (file line 205). | logs/05-mmdc-render.log, render/ |
| 6 | 10:20 | render/ | `python3 svg-labels.py render/F10.2-base-8452f56.svg render/F10.2-head-88a4eb4.svg` (run as a byte-identical `/tmp` copy) | Both renders have states `DEFAULTS` and `ADOPTED`, 1 initial pseudostate, 5 edge paths and 5 edge labels. Only edge label 4 differs. | logs/06-rendered-svg-labels.txt |
| 7 | 10:21 | render/ | `python3 svg-edges.py <base.svg> <head.svg>` | Node centres are identical in both renders. In each, `edge3` runs from ADOPTED to DEFAULTS. At head it carries `LINK_DOWN / restore defaults, declared again on the next LINK_UP`. My first, inline attempt stopped with `AttributeError` because its class filter also matched the transform-less `nodes` container. That was a bug in my probe, not a rendering finding. The saved script matches the first class token `node`. | logs/07-rendered-svg-edges.txt |
| 8 | 10:21 | scratch `/tmp/a165-negctl` = `git archive 88a4eb4` (the lane was not touched) | `sed` turns F10.2's `ADOPTED --> DEFAULTS:` into `ADOPTED --> --> DEFAULTS:`, then `./scripts/lint-diagrams.sh` runs | exit 1: `MERMAID FAIL: mmd-docs_architecture_10_srp_engine_md-2.txt`, `Parse error on line 5`, `lint: 41 mermaid + 18 wavedrom blocks checked, FAILURES`. The gate really renders F10.2 with mmdc. The scratch copy was deleted afterwards. | logs/08-lint-negative-control.log |
| 9 | 10:22:01 | head, clean | `make -j1 <target>` for `lint`, `wavedrom-check`, `links`, `matrix`, `modmatrix` and `stale`, one at a time | Each exits 0 with the lines of row 1. `stale` is silent, exit 0. | logs/09-head-per-gate.log |
| 10 | 10:23:20 | head | `bash D/scope-receipts.sh` | See the next table. | logs/10-scope-receipts.txt |
| 11 | 10:23:51 | public evidence commit `29b2066b91cf4b0387edff43250b8fe79192232b` | `python3 D/public-dv-evidence.py` | Every cited file matches its published MANIFEST.json sha256. Every R223 receipt among them also matches R223's `SHA256SUMS`. exit 0. | logs/11-public-dv-evidence.txt |

Scope receipt results (row 10):

- Commit and message:
  - `git rev-list --count 8452f56..HEAD` = 1.
  - The message is one line. `git interpret-trailers --parse` finds no trailer.
- Changed paths:
  - `git diff --name-status` gives only `M docs/architecture/10_srp_engine.md`, and `--numstat` gives `1 1`.
  - `--summary` is empty: no mode, create or delete.
  - `git diff --check` is clean.
  - `git diff-tree -r`: `:100644 100644 b6e54afc8479f0a8ab38aa8b96506a8de9bf2b8b b8dafc542930d607c32ab8be49329c7ca975f6b0 M`.
- The hunk:
  - `git diff -U0` has one hunk, `@@ -205 +205 @@`.
  - The added line has 0 U+2013 or U+2014 and 0 non-ASCII characters.
- File sha256: base `4ffc589a…1242`, head `6406ddce…ee60`.
- Every other path is identical, each with `git diff --quiet` exit 0:
  - `hdl`, `tb`, `syn`, `scripts`, `Makefile`, `.github`, `.gitignore`;
  - root `README.md` and `IEEE_1722_1_Hardware_Protocol_Processor.md`;
  - `docs/diagrams`, `docs/README.md`, `docs/guides`, `docs/traceability`;
  - 00 and the resource document;
  - architecture 01-09 and 11.
  - The tracked file list is identical.
- Exports:
  - `git diff --stat -- docs/diagrams` is empty.
  - 34 committed diagram assets; 0 of them contain F10.2 or Domain FSM terms, in SVG or drawio text or in PNG strings.
- The RTL and evidence blobs are identical at base and head: `KL_srp_domain.sv` `96c57db1…`, `tb/pp_top/sim_main.cpp` `e52f0fde…`, `tb/srp_encoder/sim_main.cpp` `eab9912e…`, `tb/srp_encoder/README.md` `2eae7395…`.
- Remote: `git ls-remote` still shows `main` and `98-clarify-srp-domain-events` at `8452f56…`. Nothing was pushed.

Not run by the author, by assignment: `./scripts/run_suites.sh` or any Verilator suite, `./scripts/lint_hdl.sh`, `./syn/yosys/run.sh`, `make -C tb/nvm_port figures`, hosted, act or Docker runs, and hardware. The change touches none of their inputs (HANDOFF.md, "Evidence reused").
