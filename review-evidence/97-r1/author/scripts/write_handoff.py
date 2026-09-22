import json
from pathlib import Path
import subprocess

out = Path(__file__).resolve().parents[1]
head = subprocess.check_output(["rtk", "proxy", "git", "rev-parse", "HEAD"], text=True).strip()
base = "8452f564294300a82d56eed464276576f65f4d58"
records = []
for path in sorted((out / "receipts").glob("*.json")):
    record = json.loads(path.read_text())
    if "argv" in record:
        records.append(record)
assert all(r["exit_code"] == r["expected_exit_code"] for r in records)
assert not subprocess.check_output(["rtk", "proxy", "git", "status", "--porcelain=v2", "--ignored"], text=True)

validation = """| Evidence | Result |
|---|---|
| Normal `make -j8` in disposable candidate | rc 0; default 1391 checks / 0 failures, 5A3C fixture 20 / 0; canonical 1411 PASS / 0 FAIL |
| `make -j8 fixture-guards` | rc 0; no override and 5A3C compile; 0002 gives both width assertions, 1002 gives only the class-D assertion |
| Full fixture compile recipe, `SRP_VID_FIXTURE=0002` | expected rc 2; both named static assertions; no fixture executable |
| Full fixture compile recipe, `SRP_VID_FIXTURE=1002` | expected rc 2; class-D static assertion only; no fixture executable |
| Remove actual top-to-child VID binding, M25 | expected make rc 2; default 1391 / 0, fixture 20 / 13 failures, all in DV |
| Child `DOM_DEF_VID_P` default 2 to 7 with binding intact, M31 | rc 0; default 1391 / 0, fixture 20 / 0; canonical 1411 PASS / 0 FAIL |
| Remove each new assertion separately | expected rc 2 from `make fixture-guards`; the new regression detects either missing guard |
| `make -j8 check`, final documentation text | rc 0; 41 Mermaid and 18 WaveDrom blocks, 807 links, 115 REQ rows / 17 GAP findings, 86 module rows / 0 untested, freshness/staleness pass |
| `./scripts/lint_hdl.sh` | rc 0; 37 LINT OK, zero tolerance |
| Source/format checks | Python syntax and `git diff --check` pass; no new U+2014; only four pp_top files; tested snapshot hashes match committed content |
"""

mapping = """| Settled issue97 acceptance | Implementation and evidence |
|---|---|
| Refuse a verification fixture equal to default 2 at either observable width | Two fixture-only `static_assert`s in `sim_main.cpp`, using the existing `uint16_t` expectation and its `0x0FFFu` mask |
| Default and pinned 5A3C remain green | Normal two-build pp_top run: 1391 + 20 checks, 0 failures; default recipe carries no fixture define |
| 0002 and 1002 fail compilation clearly | Actual full fixture compile recipes refuse both values with width-specific diagnostics; the permanent four-case syntax-compile gate validates their exact diagnostic sets |
| Preserve missing-binding sensitivity and child-default control | M25 reproduces 13/20 fixture failures with default green; M31 remains 1391 + 20 green |
| Preserve product RTL/default/runtime policy and independent expectations | Only the Makefile, README, new compile-check script and five fixture-branch lines change; no RTL or wrapper edits, no DUT-readback-derived expectations, no runtime CHECK edits |
| Donor gates, two reviews and containment | Focused author gates pass; complete native/hosted bar, R229/R230 and merge/containment remain manager-owned |
"""

limits = """No unresolved implementation decision. Local tools: Verilator 5.052, GCC 16.2.1, Python 3.14.7, GNU Make 4.4.1. The workflow pins Verilator v5.050; that hosted result is still required. The new compile checker was exercised with GCC, not separately with Clang. Normative review used the donor compliance rows, F01.5, F10.2 and the settled public issues; copyrighted specification PDFs are not shipped in this repository and were not read here.

The focused compile gate checks the actual C++ bench against temporary generated headers. It does not simulate its four cases; the separate normal and mutated real-top executions provide runtime evidence. Raw logs retain existing bench Verilator warnings; the separate zero-tolerance HDL lint passes.

Full donor suites, Yosys portability, historical nvm_port figures, hosted jobs, two independent reviews, candidate merge validation and post-merge containment were not performed by this author. No push, PR creation, review launch, merge, parent pin/workflow/other-checkout edit, hardware, Docker/act or privileged action occurred. No agents were used.

One receipt-runner launch initially exited 2 because `--cwd` followed an argparse REMAINDER positional. It ran no test. `receipts/00-launch-error.txt` records it; the corrected invocation and every actual test outcome are retained. Expected-negative compile and mutation failures are not implementation failures.
"""

(out / "HANDOFF.md").write_text(f"""# [A161] Author handoff for donor issue97

Head: `{head}`. Base: `{base}`. Branch: `97-assert-distinct-sr-vid-fixture`. Checkout: `$CANDIDATE`. One local author commit; source is clean, including ignored build artifacts. No push or PR.

Public issue: https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/97

The fixture branch now refuses default-equivalent values at the 16-bit wire and 12-bit class-D widths. A permanent `fixture-guards` prerequisite compiles the real bench in four configurations with disposable model headers and requires the intended diagnostic set. The README states this verification contract. Existing runtime checks, default build, fixture 5A3C, RTL, wrap and canonical tally are preserved.

## Acceptance mapping

{mapping}
## Author validation

{validation}
All full builds and fixture/RTL mutations ran sequentially in disposable scratch trees. Every Verilator invocation used `-j 8 --build-jobs 8 --verilate-jobs 8`; `MAKEFLAGS=-j8` and `make -j8` also bounded make. `receipts/verilator-invocations.jsonl` records the effective argument vectors. Source snapshot hashes tie the tested files to this head.

Exact cwd, environment, command vector, start time, duration, exit status and log path are in each command receipt JSON and in `COMMANDS.md`. `scripts/probes.py` derives the negative full-build commands from `make -n run SRP_VID_FIXTURE=...`, selects the fixture recipe and verifies its diagnostics. No source checkout mutation was needed. Every mutation has its patch and before/after hashes.

## Open limitations and remaining gates

{limits}
See `REMAINING-GATES.md` for the exact mandatory native and hosted commands from `.github/workflows/hdl.yml` and the public two-independent-review completion bar. R229/R230 have not been started. `PR-BODY.md` is an unpublished draft; `REVIEW-READY.md` is author evidence, not an approval.
""")
(out / "PR-BODY.md").write_text(f"""# Assert distinct SRP VID verification fixtures

Closes #97

A future `SRP_VID_FIXTURE=0002` override could leave the verification build blind to a dropped top-level VID binding. A value such as `1002` also aliases the product default on the 12-bit class-D faces. The fixture branch now refuses both cases at compile time with width-specific diagnostics. The no-override default build and pinned `5A3C` behavior remain unchanged.

`make fixture-guards`, required by the normal pp_top run, compiles the actual bench with no override, 5A3C, 0002 and 1002 using temporary generated headers. It requires the intended positive results and exact assertion-diagnostic sets; unrelated compiler errors fail it. The pp_top README documents the contract. No product RTL/default/runtime behavior, DUT-derived expectations or existing runtime checks change.

Author head: `{head}`; base: `{base}`.

## Validation

{validation}
All compile jobs were capped at 8; fixture and RTL controls ran sequentially in disposable scratch copies. Exact receipts are in the author handoff bundle and remain local until manager publication.

## Remaining completion bar

Complete native donor sweep, Yosys and historical nvm_port figures; exact-head hosted docs-gates, suites and portability with pinned Verilator v5.050; R229/R230 independent positive reviews and clean five-lens ledgers; candidate and post-merge validation/containment. Local author simulation used Verilator 5.052. The parent submodule pin is unchanged.

This is an unpublished PR draft. Author evidence does not claim the merge bar is met.
""")
(out / "REVIEW-READY.md").write_text(f"""# [A161] REVIEW READY: author handoff only

Exact head: `{head}`. Base: `{base}`. Branch: `97-assert-distinct-sr-vid-fixture`. Committed and clean locally; not pushed. Manager full validation/publication and review launch remain pending.

Review scope is the four pp_top test/documentation files. Read the frozen public issue97 acceptance, assignment, R223-S2 observation, donor README/docs guide/HDL engineer guide and relevant F01.5/F10.2 requirements. Public sources are preserved under `public/`. No private reasoning is included.

{mapping}
{validation}
Assigned reviewers: R229 cold internal Codex and R230 external Opus. Neither has been started by the author. Both must independently reconstruct the change from public authority and factual artifacts and publish exact-head Conformance, RTL, Robustness, Tests and Docs coverage. Author checks are not independent review or approval.

`HANDOFF.md` records limitations. `COMMANDS.md`, `receipts/`, `scripts/`, `source.patch` and `MANIFEST.json` provide reproducible commands, raw outcomes, mutation patches and hashes. `REMAINING-GATES.md` lists exact remaining mandatory commands and merge/containment discipline. No merge readiness is claimed.
""")
command_text = [f"# [A161] Command/result receipts for {head}", "", "Every block records the exact executed command, cwd and task environment. Raw output is in the linked log. Negative exits are expected only where explicitly recorded.", ""]
for r in records:
    command_text += ["## " + r["name"], "", "Cwd: `"+r["cwd"]+"`", "", "```sh", r["shell_command"], "```", "", "```json", json.dumps(r["environment"], indent=2), "```", "", f"Started {r['started_utc']}; duration {r['seconds']} s; exit {r['exit_code']} (expected {r['expected_exit_code']}). Log: [raw output](receipts/{r['name']}.log).", ""]
(out / "COMMANDS.md").write_text("\n".join(command_text))
print("Handoff documents written for " + head)
