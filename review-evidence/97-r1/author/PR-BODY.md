# Assert distinct SRP VID verification fixtures

Closes #97

A future `SRP_VID_FIXTURE=0002` override could leave the verification build blind to a dropped top-level VID binding. A value such as `1002` also aliases the product default on the 12-bit class-D faces. The fixture branch now refuses both cases at compile time with width-specific diagnostics. The no-override default build and pinned `5A3C` behavior remain unchanged.

`make fixture-guards`, required by the normal pp_top run, compiles the actual bench with no override, 5A3C, 0002 and 1002 using temporary generated headers. It requires the intended positive results and exact assertion-diagnostic sets; unrelated compiler errors fail it. The pp_top README documents the contract. No product RTL/default/runtime behavior, DUT-derived expectations or existing runtime checks change.

Author head: `1eb20dc4911880de10b745cc284e7dde306788b5`; base: `8452f564294300a82d56eed464276576f65f4d58`.

## Validation

| Evidence | Result |
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

All compile jobs were capped at 8; fixture and RTL controls ran sequentially in disposable scratch copies. Exact receipts are in the author handoff bundle and remain local until manager publication.

## Remaining completion bar

Complete native donor sweep, Yosys and historical nvm_port figures; exact-head hosted docs-gates, suites and portability with pinned Verilator v5.050; R229/R230 independent positive reviews and clean five-lens ledgers; candidate and post-merge validation/containment. Local author simulation used Verilator 5.052. The parent submodule pin is unchanged.

This is an unpublished PR draft. Author evidence does not claim the merge bar is met.
