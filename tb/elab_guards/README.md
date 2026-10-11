<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Elaboration guards: every guard stops every front end (issue #151)

A guard on parameters exists to stop the build. The front ends this project uses do
not agree on which severity tasks do that. Verilator reports `$fatal`, `$error` and
`$warning` as warnings (`USERFATAL`, `USERERROR`, `USERWARN`); `$info` is an `-Info`
note with exit status 0. sv2v from 0.0.13 lowers a `$error` to an `initial $display`
that Yosys never runs, Vivado builds through a `$warning`, and Verilator lint and
xelab never run an
`initial` block. Only a module-scope `$fatal(1, ...)` is refused by all of them. The
measurement and the rule are in the
[HDL engineer guide §2.1](../../docs/guides/hdl-engineer.md#21-elaboration-guards-stop-every-front-end).

A refusal that names the guard is therefore not enough: a module-scope `$error` is
refused by name in Verilator lint, and Yosys builds straight through it under the sv2v
after the pinned one. This suite grades the class.

```sh
make                 # Verilator lint, plus sv2v + Yosys when both are on PATH
make vivado          # Vivado xelab and synth_design -rtl (slow; not in `make`)
make mutants         # the suite's own proof (below)
python3 guards.py --only KL_pp_nvm_port.g_tmo_check --frontend yosys
```

## What it proves

For each of the 31 guards in `guards.py`'s `GUARDS`, two cases through each front end
that runs:

- **the violating case is refused as a `$fatal` of that guard**, in the front end's own
  words, at the guard's own line:

  | Front end | Run as | The refusal it must print |
  |---|---|---|
  | `verilator` | `scripts/lint_hdl.sh`'s flags, `--top-module`, `-G` | `%Warning-USERFATAL: <file>:<the guard's line>:<col>: <the guard's words>` |
  | `yosys` | sv2v over the tree, then `read_verilog -defer; chparam; hierarchy -check -top; proc; opt_clean`, as `syn/yosys/run.sh` runs a top | `all.v:<n>: ERROR: FATAL: .` (sv2v 0.0.12) or ``all.v:<n>: ERROR: System task `$finish' executed.`` (sv2v 0.0.13), where line `<n>` of the lowered file is inside the guard's block |
  | `xsim` | `xvlog` over the tree, then `xelab -generic_top` | `ERROR: [VRFC 10-8279] $fatal : ... [<file>:<the guard's line>]` |
  | `synth` | `synth_design -rtl -generic`, one Vivado session per tree | `ERROR: [Synth 8-6058] Synth Error: ... [<file>:<the guard's line>]` |

  Verilator's line must also carry the guard's message with its values (the `words`
  column of `GUARDS`). Yosys 0.66 prints a `$fatal(1, ...)` as `FATAL: .`, taking the
  finish number for the text, so its verdict locates the guard by line instead.
- **the nearest passing case elaborates**: exit status 0 with no `%Warning`, `%Error` or
  `ERROR` line, except the three measured deadline comparisons below. The exact
  `scripts/lint_hdl.sh` flags are retained, with no warning suppression.

And one inventory check per guard: every elaboration severity task under `hdl/`
(`$fatal`, `$error`, `$warning`, `$info`, in any scope, outside comments) must be the one
task of a guard in `GUARDS`, and every guard in `GUARDS` must be in `hdl/`. A new guard
with no cases fails `make` until it has them.

The tally is `elab guards: N checks: N PASS, 0 FAIL`: 31 inventory checks and two per
guard per front end, so 93 with Verilator alone and 155 with Yosys as well.

## The cases

Each guard is elaborated in its own module except the top's five. The two sides
straddle a boundary by one byte, beat, slot, count, cycle or alignment remainder.
`gen_g_size` grades 16/17 bytes. The three deadline guards grade 0/1 cycles. At 1,
Verilator flags the counter's comparison against zero as `UNSIGNED`; the bench
accepts only those recorded expressions at their source locations, exit 1, and
exactly the matching warning-count exit line. Every USER diagnostic, other warning,
other error or other process status still fails. Yosys and both Vivado legs must
elaborate those passing cases with exit 0.

Some derived limits cannot be reached one unit apart in the shipping context.
`setup` applies these fixed context edits to both sides in scratch copies of the
whole HDL tree. No guard condition or message is changed by a fixture.

| Guard | Scratch context | Violating / nearest passing value of the guarded quantity |
|---|---|---|
| engine `gen_g_resp_cap_fit` | `RESP_BUF_C = LINE_BYTES_P - 52`, page size 62 so the independent page floor does not mask the passing side; line 575/576 | response buffer 523/524 bytes, original response cap 524 |
| engine `gen_g_gamap_page_fit` | `RESP_BUF_C = 520 + LINE_BYTES_P / 8`, line 568/576 | response buffer 591/592 bytes, original page floor 592 |
| engine `gen_g_line_ceiling` | `RESP_BUF_C = 898 + LINE_BYTES_P / 8`, line 1016/1008 | response buffer 1025/1024 bytes, original cursor ceiling 1024 |
| top `gen_g_tmr_aw` | 7 address bits; 128 slots in the passing context, 129 planted in the violating context | 129/128 slots |
| top `gen_g_tmr_fit` | map end 91; only violating side plants slot count 90 | 90/91 slots |
| top `gen_g_tmr_overlap` | only violating side moves the talker base down by one | one-slot overlap / exact adjacency |

The other cases use public parameters directly. Descriptor burst counts use
4088/4096-byte lines, giving 511/512 beats. Encoder capacity uses 557/558 bytes
at the default depth. Original full-tree default and shipping elaborations remain
covered by the processor and parent gates. These fixtures test the boundary
conditions, not an additional shipping configuration.

## Seams

- A `-Wno-fatal` Verilator simulation build carries every severity past, `$fatal`
  included, so a suite that overrides a parameter into a guard's range still builds.
  The Verilator gate is a lint: this suite's, and `scripts/lint_hdl.sh`'s at the
  defaults.
- The Vivado legs need Vivado on PATH and are slow, so `make` does not run them.
- Yosys stops at its first error, so a violating case must reach its own guard first.
  Where one trips two (the engine's response-cap case also trips its page-fit guard),
  the graded guard is the one Yosys reaches first. The other front ends are graded on
  the guard's own line among everything they print.

## Mutation record

`make mutants` plants each arm in a scratch copy of `hdl/` of its own, runs `guards.py
--only` the arm's guard on it, and counts the arm KILLED only when the bench exits
non-zero and prints the arm's own FAIL line naming that guard. A control on an
unplanted copy runs first and must pass.

| Arms | Planted | The check that fails |
|---|---|---|
| 31 `error` | the guard's `$fatal(1, ` becomes `$error(` | GUARD: Verilator `USERERROR`; Yosys `ERROR: <message>` under sv2v 0.0.12, nothing under 0.0.13 |
| 31 `warning` | `$fatal(1, ` becomes `$warning(` | GUARD: Verilator `USERWARN`; Yosys builds |
| 31 `initial` | the guard's `if` becomes `initial if` | GUARD: Verilator builds; Yosys `Can't resolve task name` under sv2v 0.0.12 |
| `bench:unlisted` | a new `$fatal` guard in `KL_pp_release_merge` with no case | inventory, naming it |
| `bench:deleted` | `KL_aecp_engine.gen_g_line_step` deleted | inventory, and GUARD: at 577 the store's own guard refuses one level down, at another guard's line |
| `bench:message` | `KL_pp_nvm_port.g_tmo_check`'s words changed | GUARD (Verilator's words) |
| 31 `tightened` | one-unit stricter bound (explicit terms in `BOUNDARIES`) | ELAB, naming the nearest passing case, in **every** selected front end |
| 31 `loosened` | one-unit looser bound | GUARD, naming the violating case, in **every** selected front end |
| `bench:deadline_user` | a `$warning` on the deadline's passing side | ELAB: the UNSIGNED exception cannot admit a USER diagnostic |
| `bench:deadline_other_warning` | an ascending packed range beside the deadline | ELAB: the UNSIGNED exception cannot admit another warning class |

There are 160 arms: five per guard plus the five bench checks above. Alignment
mutants shrink the allowed remainder from 0 to none, or expand it from 0 to 0..1.
The owner-tag mutant moves both output-stream limits by one; either unchanged
comparison would otherwise still refuse the violating shape. A boundary arm is
unproven if any selected front end misses its named failure.

The hosted `suites` job runs the lint leg. The hosted `portability` job runs the
Yosys leg explicitly, using its installed sv2v release, even when Verilator is not
installed in that job.
