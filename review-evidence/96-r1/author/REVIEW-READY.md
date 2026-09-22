[A157] REVIEW READY (author handoff; manager validation and publication pending)

Commit: `ea93023fbdd31cbf718da0d8f1aab4d750bfd21a`
Base: `main` `424c688fa2205b934a7689a58f2aa766420f2326`
Branch: `95-expose-default-sr-vid`, clean and committed locally, one commit ahead of its remote, not pushed.

Changed:

- `hdl/top/protocol_processor_top.sv` declares `parameter logic [15:0] SRP_DOM_DEF_VID_P = 16'd2` (`:144`) and binds `.DOM_DEF_VID_P (SRP_DOM_DEF_VID_P)` on `u_srp` (`:2147`). `hdl/srp/` is untouched.
- Docs: F01.5 row `P-SRP-DOM-DEF-VID`; F10.2 cites the P-ID; 10 §11; integrator guide §2 and §8.
- `tb/pp_top`: section DV (20 checks) on a fresh model. The suite builds twice: the default build overrides nothing, and the fixture build (`0x5A3C`, verification only) runs DV alone. The four Domain class-D ports are passed through the wrap, and one summed canonical tally is printed. README gains section DV and mutation rows M25 to M31.

Validation, all compile jobs capped at 8:

- `tb/pp_top` `make clean && make`: rc 0, `1411 checks: 1411 PASS, 0 FAIL` (default 1391/0, fixture 20/0). The base measured 1371.
- `tb/srp_top`: 235/235. `tb/srp_encoder`: 180/180. `tb/timer_map`: 10 shapes OK, 3 guards OK, 1360/1360, on the rerun with a space-free job-capped wrapper; the first invocation failed in 0 s on my spaced `VERILATOR` value.
- `python3 scripts/check_upc_map.py`: PASS. `./scripts/lint_hdl.sh`: 37 LINT OK. `make check`: all OK. `python3 scripts/gen_matrix.py --check`: 86 rows, 0 untested.
- The individual hdl.yml docs-gates commands and `make stale`: OK.
- `./syn/yosys/run.sh`: 32 OK plus XILINX OK, output identical to base.
- `git diff --check`: clean. The 332 added lines carry 0 U+2014 and 0 U+2013.

Mutation controls, each in a scratch copy:

| Planted change | Default build | Fixture build |
|---|---|---|
| binding removed | 0/1391 FAIL | 13/20 FAIL |
| bound to literal 2 | not run | 13/20 FAIL |
| truncated to 12 bits | not run | 4/20 FAIL (wire) |
| bound to `DOM_DEF_PRIO_P` | 16/1391 FAIL | 13/20 FAIL |
| top default 3 | 18/1391 FAIL | 0/20 |
| wrap override removed | not run | 13/20 FAIL |
| control: child default 7 | green | green |

Acceptance criteria:

- **AC1 met**: a 16-bit named parameter defaults to 2 and drives the child directly. The default and startup semantics, and the preserved adoption, are documented in F01.5, 10 and the integrator guide. The top's own default is graded (M29); the child's default is no longer a source (M31).
- **AC2 met**: a top-level executable test covers the default and a distinct verification value through the real SRP path. Missing and misbound connections fail it (M25 to M28, M30).
- **AC3 met for author scope**: adoption and Link Down/Up remain covered (pp_top S1/S8, srp_top, srp_encoder D1 to D9) and are now also graded at top level with the parameter (DV4 to DV6). Focused regressions and the local donor gates pass.
- **AC4 not author scope**: two independent reviews, lens coverage and merge/containment are pending with the manager.

Open risks/questions: no unresolved implementation decision. Remaining gates:

- the complete `./scripts/run_suites.sh` sweep;
- `make -C tb/nvm_port figures` (untouched by this change; needs the PR #13 ref);
- Verilator v5.050, the CI pin (local runs used 5.052);
- all hosted runs, candidate validation, R223/R224 reviews, publication and merge.

This is author evidence, not an approval, and it does not claim the merge bar is met. No push, PR creation, review launch, act/Docker run, hardware operation, parent edit or merge was performed.
