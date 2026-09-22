[R234] Reproduction and receipt map

Run from this report directory; all shell invocations start with `rtk`.
Python subprocess commands also use `rtk proxy`. No package installation is
performed. `review-probes.py` uses existing mmdc 11.16.0 and temporary archives;
it runs make with one job and performs no RTL compilation.

```sh
rtk proxy python3 fetch-evidence.py
rtk proxy python3 verify-public-evidence.py
rtk proxy python3 review-probes.py $VALIDATION_STORAGE/reviews/r234-101-r1
rtk proxy python3 snapshot-public.py
rtk proxy python3 final-integrity.py $VALIDATION_STORAGE/reviews/r234-101-r1
```

`fetch-evidence.py` uses the immutable URLs in `download-items.json` for the
selected public author/manager archive artifacts, historical factual PR96
execution receipts, and parent workflow documents. Public review reports are
not in that download set. Initial issue/decision/PR metadata, tree listings and
status snapshots are retained in `public/`; their source URLs, observation
times and SHA256 digests are recorded in `receipts/downloads.jsonl`.
`snapshot-public.py` reads live status, so replay can legitimately observe later
results. The recorded snapshots, not a later replay, support this report.

Receipt | Meaning
---|---
receipts/candidate.diff, candidate-history.txt | Exact two-line diff and both implementation commits
receipts/scope.json | All 222 tree entries compared; only two files changed from base; one from 88a4eb4; modes preserved; fence and file identity
receipts/probe-commands.jsonl | Executed probe argv, working directory, time, exit status and output file
receipts/archive-hashes.json | 42 downloaded files checked against published MANIFEST hashes; original hashes checked where unredacted
receipts/archive-continuity.json | 46 author archive blobs/modes unchanged between c5f372ed and 94f47697; both PNG digests match author SHA256SUMS and original/published manifest fields
receipts/history96-hashes.json | Six selected historical factual logs/diffs independently checked against their manifest and applicable SHA256SUMS
receipts/public-evidence-summary.json | Nine manager command completions, suite arithmetic and lint/Yosys result counts
receipts/svg-structure.json, independent-render.json | Independent structural comparison of all figure nodes, edges and labels
render/ | Final frozen-head source, independent SVG and native-size PNG; Puppeteer config
receipts/visual-inspection.md | Actual visual inspection observations
receipts/docs-focused.log | Independent lint, links, requirement matrix and module matrix pass
receipts/docs-negative-control.log | Scratch-only malformed F10.2 rejected; inner lint exit 1, make exit 2
receipts/stale.log, diff-check.log | Silent successful gates; exit codes in probe command log
receipts/hosted-final-summary.json | Precise live hosted snapshot and advanced main; no new-main candidate claim
receipts/final-integrity.json, final-tracked-files.json | Direct physical blob and executable-mode checks, exact index equality and clean detached checkout
receipts/probe-development.txt | Disclosed reviewer SVG-selector bug corrected before successful probe execution

Independent focused command in the disposable exact-head archive:

```sh
rtk proxy make -j1 lint links matrix modmatrix
```

Result: 41 Mermaid renders, 18 WaveDrom JSON parses, 807 links, 115 REQ rows,
17 GAP records, 86 module rows / zero untested. `make -j1 stale` and diff hygiene
also passed against the preserved checkout. No independent full `make check`
is claimed: system Python lacks WaveDrom and its donor helper would bootstrap
an installation. The manager's hash-verified final-head `make check` supplies
the separate 18-render freshness result. No source, test or package was changed.

Public archive text may contain neutralized path tokens. Its published hash is
the applicable comparison; an original pre-redaction SHA256 is not asserted to
match redacted text. Original private artifacts were neither needed nor read.
