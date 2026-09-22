[A166] Exact executed commands and results

All commands use RTK. `scripts/verilator8` is an external author-only wrapper; `receipts/verilator-invocations.jsonl` preserves original and effective arguments. Each work invocation explicitly sets `-j 8 --build-jobs 8 --verilate-jobs 8`; outer pp_top make uses `-j1`. Mutations ran sequentially. The docs check uses a pre-existing WaveDrom environment; nothing was installed. `receipts/runner-note.txt` records the corrected pre-execution anchor error in the external mutation runner.

### 01-base-german

Working directory: `/tmp/a166-pr100-m1-hos8lcje/base/tb/pp_top`

```sh
rtk proxy env -u LC_ALL LANG=en_US.UTF-8 LANGUAGE=de make -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verilator8 fixture-guards
```

Observed exit 2; expected 2; 4.351 seconds. [Raw output](receipts/01-base-german.log); [metadata](receipts/01-base-german.json).

### 02-base-french

Working directory: `/tmp/a166-pr100-m1-hos8lcje/base/tb/pp_top`

```sh
rtk proxy env -u LC_ALL LANG=en_US.UTF-8 LANGUAGE=fr make -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verilator8 fixture-guards
```

Observed exit 2; expected 2; 4.31 seconds. [Raw output](receipts/02-base-french.log); [metadata](receipts/02-base-french.json).

### 03-fixed-german

Working directory: `/tmp/a166-pr100-m1-hos8lcje/fixed/tb/pp_top`

```sh
rtk proxy env -u LC_ALL LANG=en_US.UTF-8 LANGUAGE=de make -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verilator8 fixture-guards
```

Observed exit 0; expected 0; 5.358 seconds. [Raw output](receipts/03-fixed-german.log); [metadata](receipts/03-fixed-german.json).

### 04-fixed-french

Working directory: `/tmp/a166-pr100-m1-hos8lcje/fixed/tb/pp_top`

```sh
rtk proxy env -u LC_ALL LANG=en_US.UTF-8 LANGUAGE=fr make -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verilator8 fixture-guards
```

Observed exit 0; expected 0; 5.444 seconds. [Raw output](receipts/04-fixed-french.log); [metadata](receipts/04-fixed-french.json).

### 05-remove-wire

Working directory: `/tmp/a166-pr100-m1-hos8lcje/mutant/tb/pp_top`

```sh
rtk proxy env -u LC_ALL LANG=en_US.UTF-8 LANGUAGE=de make -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verilator8 fixture-guards
```

Observed exit 2; expected 2; 4.662 seconds. [Raw output](receipts/05-remove-wire.log); [metadata](receipts/05-remove-wire.json).

### 06-remove-class-d

Working directory: `/tmp/a166-pr100-m1-hos8lcje/mutant/tb/pp_top`

```sh
rtk proxy env -u LC_ALL LANG=en_US.UTF-8 LANGUAGE=de make -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verilator8 fixture-guards
```

Observed exit 2; expected 2; 4.647 seconds. [Raw output](receipts/06-remove-class-d.log); [metadata](receipts/06-remove-class-d.json).

### 07-unrelated-error

Working directory: `/tmp/a166-pr100-m1-hos8lcje/mutant/tb/pp_top`

```sh
rtk proxy env -u LC_ALL LANG=en_US.UTF-8 LANGUAGE=de make -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verilator8 fixture-guards
```

Observed exit 2; expected 2; 4.514 seconds. [Raw output](receipts/07-unrelated-error.log); [metadata](receipts/07-unrelated-error.json).

### 08-remove-locale

Working directory: `/tmp/a166-pr100-m1-hos8lcje/mutant/tb/pp_top`

```sh
rtk proxy env -u LC_ALL LANG=en_US.UTF-8 LANGUAGE=de make -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verilator8 fixture-guards
```

Observed exit 2; expected 2; 0.106 seconds. [Raw output](receipts/08-remove-locale.log); [metadata](receipts/08-remove-locale.json).

### 09-default-and-5a3c

Working directory: `/tmp/a166-pr100-m1-hos8lcje/fixed/tb/pp_top`

```sh
rtk proxy env -u LC_ALL LANG=en_US.UTF-8 LANGUAGE=de make -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verilator8 run
```

Observed exit 0; expected 0; 195.527 seconds. [Raw output](receipts/09-default-and-5a3c.log); [metadata](receipts/09-default-and-5a3c.json).

### 10-docs

Working directory: `$CANDIDATE`

```sh
rtk proxy env PATH=$WORKSPACE_HOME/milan-fpga-management/tools/fpga-gptp-wavedrom/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0KKu8Bg:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl make -j8 check
```

Observed exit 0; expected 0; 25.357 seconds. [Raw output](receipts/10-docs.log); [metadata](receipts/10-docs.json).

### 11-upc-map

Working directory: `$CANDIDATE`

```sh
rtk proxy python3 scripts/check_upc_map.py
```

Observed exit 0; expected 0; 0.04 seconds. [Raw output](receipts/11-upc-map.log); [metadata](receipts/11-upc-map.json).

### 12-precommit-scope

Working directory: `$CANDIDATE`

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verify-scope.py precommit
```

Observed exit 0; expected 0; 0.153 seconds. [Raw output](receipts/12-precommit-scope.log); [metadata](receipts/12-precommit-scope.json).

### 13-results

Working directory: `$CANDIDATE`

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verify-results.py
```

Observed exit 0; expected 0; 0.037 seconds. [Raw output](receipts/13-results.log); [metadata](receipts/13-results.json).

### 14-final-scope

Working directory: `$CANDIDATE`

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-m1-author/scripts/verify-scope.py final
```

Observed exit 0; expected 0; 0.359 seconds. [Raw output](receipts/14-final-scope.log); [metadata](receipts/14-final-scope.json).
