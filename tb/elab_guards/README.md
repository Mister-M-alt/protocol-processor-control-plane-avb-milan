<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Elaboration guards: every guard stops every front end (issue #151)

A guard on parameters exists to stop the build. The front ends this project uses do
not agree on which severity tasks do that: Verilator lint reports all four as
warnings, sv2v from 0.0.13 lowers a `$error` to an `initial $display` that Yosys never
runs, Vivado builds through a `$warning`, and Verilator lint and xelab never run an
`initial` block. Only a module-scope `$fatal(1, ...)` is refused by all of them. The
measurement and the rule are in the
[HDL engineer guide §2.1](../../docs/guides/hdl-engineer.md#21-elaboration-guards-stop-every-front-end).

A refusal that names the guard is therefore not enough: a module-scope `$error` is
refused by name in Verilator lint, and Yosys builds straight through it under the sv2v
after the pinned one. This suite grades the class.

```sh
make                 # Verilator lint, plus sv2v + Yosys when both are on PATH
make vivado          # Vivado xelab and synth_design -rtl (slow; not in `make`)
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
  `ERROR` line. For Verilator this is `scripts/lint_hdl.sh`'s zero tolerance.

And one inventory check per guard: every elaboration severity task under `hdl/`
(`$fatal`, `$error`, `$warning`, `$info`, in any scope, outside comments) must be the one
task of a guard in `GUARDS`, and every guard in `GUARDS` must be in `hdl/`. A new guard
with no cases fails `make` until it has them.

The tally is `elab guards: N checks: N PASS, 0 FAIL`: 31 inventory checks and two per
guard per front end, so 93 with Verilator alone and 155 with Yosys as well.

## The cases

Each guard is elaborated in its own module except the top's five. The passing case is
the nearest set that lints clean. Three deadlines pass at 2, not 1: at 1 they
elaborate, but a compare against `N - 1 = 0` is constant and Verilator `-Wall` reports
it (`UNSIGNED`), a lint finding and not a guard.

Three of the top's guards check the timer-slot map against itself, and no parameter
set reaches them alone: `TMR_AW_C` is `$clog2(TMR_SLOTS_C)`, every base is the running
sum of the extents before it, and `TMR_SLOTS_C` is the map's own end (only
`N_AVB_IF_P = 0` reaches `gen_g_tmr_fit`, and `gen_g_avb_if` refuses it first). They
hold the map's derivation in place, should it ever be edited. For them the violating
case is a shape planted in a scratch copy of `hdl/`, never in the tree: one derived
value moved by one (`plant` in `GUARDS`), so the guard's condition is what refuses it.
Their passing case is the default shape.

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
