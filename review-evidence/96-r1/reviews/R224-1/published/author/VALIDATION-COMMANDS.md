[A157] Author validation commands (donor issue #95)

Run from `$CANDIDATE` at `ea93023fbdd31cbf718da0d8f1aab4d750bfd21a` with a clean tree. Every shell command went through `rtk proxy`. Compile jobs were capped at 8 as follows:

- `MAKEFLAGS=-j8` and `VERILATOR_JOBS=8` exported. Nothing in the donor reads `VERILATOR_JOBS`.
- The suite Makefiles pass `--build -j 0`, which alone would take every visible CPU, so every suite `make` ran with `VERILATOR="verilator --build-jobs 8 --verilate-jobs 8"`. Their `VERILATOR ?=` hook accepts this, and explicit `--build-jobs` / `--verilate-jobs` take precedence over `-j 0`. A process sample during the fixture build showed the inner `make -C obj_vid -f Vpp_top_wrap.mk -j 8`, and Verilator reported "on 8 threads".

The exact sequence is `$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor95-author/validate.sh`, a copy of `/tmp/a157_validate.sh`:

```sh
export MAKEFLAGS=-j8 VERILATOR_JOBS=8
V="verilator --build-jobs 8 --verilate-jobs 8"
(cd tb/pp_top      && make clean; make VERILATOR="$V")   # both builds, one summed tally
(cd tb/srp_top     && make clean; make VERILATOR="$V")
(cd tb/srp_encoder && make clean; make VERILATOR="$V")
(cd tb/timer_map   && make clean; make VERILATOR=/tmp/a157_bin/verilator8)   # see note below
python3 scripts/check_upc_map.py                          # run_suites.sh's pre-gate
./scripts/lint_hdl.sh
make -j1 check
python3 scripts/gen_matrix.py --check
python3 scripts/check-links.py
python3 scripts/check-matrix.py
python3 scripts/render-wavedrom.py --check
make -j1 stale
./syn/yosys/run.sh                                        # read-only use; PR26 owns this file
```

timer_map note: `validate.sh` first ran this suite with the spaced `VERILATOR` value, and it failed in 0 s (`timer_map.log`: `/bin/sh: --build-jobs: command not found`, make Error 127). Its `shapes` recipe inlines `VERILATOR=$(VERILATOR) ./shape_elab.sh` as an unquoted environment prefix, so a value with spaces cannot pass through it; the lane is not at fault. It was rerun on the same head with the space-free wrapper `verilator8` (a copy is kept here; its body is `exec verilator --build-jobs 8 --verilate-jobs 8 "$@"`), and that run is `timer_map.rerun.log`.

Yosys baseline comparison: `./syn/yosys/run.sh` was also run on `git archive 424c688fa2205b934a7689a58f2aa766420f2326` in `/tmp/a157_base`. Its sorted output is identical to the head's, so the "Replacing memory" warnings are pre-existing (`yosys-base.log`).

Committed-range checks (base `424c688fa2205b934a7689a58f2aa766420f2326`):

```sh
git diff --check 424c688fa2205b934a7689a58f2aa766420f2326 HEAD
git diff -U0 424c688fa2205b934a7689a58f2aa766420f2326 HEAD | grep -P '^\+(?!\+\+)' | grep -cP '\x{2014}'   # U+2014 on added lines
git diff -U0 424c688fa2205b934a7689a58f2aa766420f2326 HEAD | grep -P '^\+(?!\+\+)' | grep -cP '\x{2013}'   # U+2013 on added lines
```

Mutation controls use `mutate.sh` (a copy of `$MUTATION_STORAGEate.sh`). It copies `hdl/`, `tb/common/` and the `tb/pp_top/` inputs into `$MUTATION_STORAGE/<id>/`, applies one `sed`, refuses a no-op, and builds with the commands `make -n run` prints from the scratch Makefile, so the flags cannot drift. The lane itself is never edited:

```sh
bash mutate.sh M25_binding_dropped       both    hdl/top/protocol_processor_top.sv '/\.DOM_DEF_VID_P    (SRP_DOM_DEF_VID_P),/d'
bash mutate.sh M26_bound_to_literal_2    fixture hdl/top/protocol_processor_top.sv "s/\.DOM_DEF_VID_P    (SRP_DOM_DEF_VID_P),/.DOM_DEF_VID_P    (16'd2),/"
bash mutate.sh M27_truncated_12b         fixture hdl/top/protocol_processor_top.sv "s/\.DOM_DEF_VID_P    (SRP_DOM_DEF_VID_P),/.DOM_DEF_VID_P    (16'(SRP_DOM_DEF_VID_P[11:0])),/"
bash mutate.sh M28_wrong_child_param     both    hdl/top/protocol_processor_top.sv "s/\.DOM_DEF_VID_P    (SRP_DOM_DEF_VID_P),/.DOM_DEF_PRIO_P   (SRP_DOM_DEF_VID_P[7:0]),/"
bash mutate.sh M29_top_default_3         both    hdl/top/protocol_processor_top.sv "s/parameter logic \[15:0\] SRP_DOM_DEF_VID_P   = 16'd2,/parameter logic [15:0] SRP_DOM_DEF_VID_P   = 16'd3,/"
bash mutate.sh M30_wrap_override_dropped fixture tb/pp_top/pp_top_wrap.sv '/\.SRP_DOM_DEF_VID_P (`PP_TOP_SRP_DOM_DEF_VID),/d'
bash mutate.sh M31_child_default_7       both    hdl/srp/KL_srp_top.sv "s/parameter logic \[15:0\] DOM_DEF_VID_P  = 16'd2,/parameter logic [15:0] DOM_DEF_VID_P  = 16'd7,/"
```

`fixture` builds and runs only the second (fixture) build; `both` runs the default build first. After the runs, each scratch tree's build inputs were compared with the committed head: exactly one file differs in each, the mutated one. Their diffs and run logs are under `logs/mutations/`.
