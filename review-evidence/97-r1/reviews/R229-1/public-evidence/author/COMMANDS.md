# [A161] Command/result receipts for 1eb20dc4911880de10b745cc284e7dde306788b5

Every block records the exact executed command, cwd and task environment. Raw output is in the linked log. Negative exits are expected only where explicitly recorded.

## 01-fixture-guards

Cwd: `$CANDIDATE/tb/pp_top`

```sh
rtk proxy make -j8 fixture-guards
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:31:51.177616+00:00; duration 5.275 s; exit 0 (expected 0). Log: [raw output](receipts/01-fixture-guards.log).

## 02-default-fixture

Cwd: `/tmp/a161-issue97-rwxncqry/candidate/tb/pp_top`

```sh
rtk proxy make -j8
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:32:10.813340+00:00; duration 199.078 s; exit 0 (expected 0). Log: [raw output](receipts/02-default-fixture.log).

## 03-docs-check

Cwd: `$CANDIDATE`

```sh
rtk proxy make -j8 check
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:32:11.973907+00:00; duration 24.228 s; exit 0 (expected 0). Log: [raw output](receipts/03-docs-check.log).

## 04-hdl-lint

Cwd: `$CANDIDATE`

```sh
rtk proxy ./scripts/lint_hdl.sh
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:32:24.951347+00:00; duration 11.353 s; exit 0 (expected 0). Log: [raw output](receipts/04-hdl-lint.log).

## 05-refuse-0002-recipe

Cwd: `/tmp/a161-issue97-rwxncqry/05-refuse-0002/tb/pp_top`

```sh
rtk proxy make -n run VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator SRP_VID_FIXTURE=0002
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:35:50.267375+00:00; duration 0.027 s; exit 0 (expected 0). Log: [raw output](receipts/05-refuse-0002-recipe.log).

## 05-refuse-0002

Cwd: `/tmp/a161-issue97-rwxncqry/05-refuse-0002/tb/pp_top`

```sh
rtk proxy $WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator --cc --exe --build -j 0 --top-module pp_top_wrap -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM -CFLAGS '-std=c++17 -O2 -I/tmp/a161-issue97-rwxncqry/05-refuse-0002/tb/pp_top -Wall -Wextra' '+define+PP_TOP_SRP_DOM_DEF_VID=16'"'"'h0002' -CFLAGS -DPP_TOP_SRP_DOM_DEF_VID=0x0002 --Mdir obj_vid ../../hdl/common/pp_pkg.sv ../../hdl/srp/srp_pkg.sv ../../hdl/acmp/pp_acmp_pkg.sv ../../hdl/adp/pp_adp_pkg.sv ../../hdl/common/KL_pp_prng.sv ../../hdl/common/KL_pp_timer_service.sv ../../hdl/packet_engine/KL_pp_rx_validator.sv ../../hdl/packet_engine/KL_pp_rx_slots.sv ../../hdl/packet_engine/KL_pp_normalizer.sv ../../hdl/packet_engine/KL_pp_dispatch.sv ../../hdl/packet_engine/KL_pp_tx_slots.sv ../../hdl/packet_engine/KL_pp_release_merge.sv ../../hdl/packet_engine/KL_pp_tx_arbiter.sv ../../hdl/packet_engine/KL_pp_scoreboard.sv ../../hdl/packet_engine/KL_pp_event_router.sv ../../hdl/packet_engine/KL_pp_originator.sv ../../hdl/packet_engine/KL_pp_trace_ring.sv ../../hdl/packet_engine/KL_pp_side_port.sv ../../hdl/packet_engine/KL_pp_nvm_port.sv ../../hdl/aecp/ucpu_pkg.sv ../../hdl/aecp/KL_aecp_ucpu.sv ../../hdl/aecp/KL_aecp_desc_store.sv ../../hdl/aecp/KL_aecp_dyn_state.sv ../../hdl/aecp/KL_aecp_engine.sv ../../hdl/aecp/KL_aecp_resp_buf.sv ../../hdl/aecp/KL_aecp_notify.sv ../../hdl/aecp/KL_aecp_ca_originator.sv ../../hdl/adp/KL_adp_engine.sv ../../hdl/acmp/KL_pp_acmp_listener.sv ../../hdl/acmp/KL_acmp_talker.sv ../../hdl/acmp/KL_acmp_nvm_shadow.sv ../../hdl/maap/KL_pp_maap.sv ../../hdl/srp/KL_srp_decoder.sv ../../hdl/srp/KL_srp_domain.sv ../../hdl/srp/KL_srp_vlan.sv ../../hdl/srp/KL_srp_admission.sv ../../hdl/srp/KL_srp_talker_fsm.sv ../../hdl/srp/KL_srp_listener_fsm.sv ../../hdl/srp/KL_srp_encoder.sv ../../hdl/srp/KL_srp_top.sv ../../hdl/top/KL_mrp_strip.sv ../../hdl/top/protocol_processor_top.sv pp_top_wrap.sv sim_main.cpp -o Vpp_top_vid
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:35:50.364036+00:00; duration 6.791 s; exit 2 (expected 2). Log: [raw output](receipts/05-refuse-0002.log).

## 05-refuse-1002-recipe

Cwd: `/tmp/a161-issue97-rwxncqry/05-refuse-1002/tb/pp_top`

```sh
rtk proxy make -n run VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator SRP_VID_FIXTURE=1002
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:35:57.247174+00:00; duration 0.028 s; exit 0 (expected 0). Log: [raw output](receipts/05-refuse-1002-recipe.log).

## 05-refuse-1002

Cwd: `/tmp/a161-issue97-rwxncqry/05-refuse-1002/tb/pp_top`

```sh
rtk proxy $WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator --cc --exe --build -j 0 --top-module pp_top_wrap -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNUSEDPARAM -CFLAGS '-std=c++17 -O2 -I/tmp/a161-issue97-rwxncqry/05-refuse-1002/tb/pp_top -Wall -Wextra' '+define+PP_TOP_SRP_DOM_DEF_VID=16'"'"'h1002' -CFLAGS -DPP_TOP_SRP_DOM_DEF_VID=0x1002 --Mdir obj_vid ../../hdl/common/pp_pkg.sv ../../hdl/srp/srp_pkg.sv ../../hdl/acmp/pp_acmp_pkg.sv ../../hdl/adp/pp_adp_pkg.sv ../../hdl/common/KL_pp_prng.sv ../../hdl/common/KL_pp_timer_service.sv ../../hdl/packet_engine/KL_pp_rx_validator.sv ../../hdl/packet_engine/KL_pp_rx_slots.sv ../../hdl/packet_engine/KL_pp_normalizer.sv ../../hdl/packet_engine/KL_pp_dispatch.sv ../../hdl/packet_engine/KL_pp_tx_slots.sv ../../hdl/packet_engine/KL_pp_release_merge.sv ../../hdl/packet_engine/KL_pp_tx_arbiter.sv ../../hdl/packet_engine/KL_pp_scoreboard.sv ../../hdl/packet_engine/KL_pp_event_router.sv ../../hdl/packet_engine/KL_pp_originator.sv ../../hdl/packet_engine/KL_pp_trace_ring.sv ../../hdl/packet_engine/KL_pp_side_port.sv ../../hdl/packet_engine/KL_pp_nvm_port.sv ../../hdl/aecp/ucpu_pkg.sv ../../hdl/aecp/KL_aecp_ucpu.sv ../../hdl/aecp/KL_aecp_desc_store.sv ../../hdl/aecp/KL_aecp_dyn_state.sv ../../hdl/aecp/KL_aecp_engine.sv ../../hdl/aecp/KL_aecp_resp_buf.sv ../../hdl/aecp/KL_aecp_notify.sv ../../hdl/aecp/KL_aecp_ca_originator.sv ../../hdl/adp/KL_adp_engine.sv ../../hdl/acmp/KL_pp_acmp_listener.sv ../../hdl/acmp/KL_acmp_talker.sv ../../hdl/acmp/KL_acmp_nvm_shadow.sv ../../hdl/maap/KL_pp_maap.sv ../../hdl/srp/KL_srp_decoder.sv ../../hdl/srp/KL_srp_domain.sv ../../hdl/srp/KL_srp_vlan.sv ../../hdl/srp/KL_srp_admission.sv ../../hdl/srp/KL_srp_talker_fsm.sv ../../hdl/srp/KL_srp_listener_fsm.sv ../../hdl/srp/KL_srp_encoder.sv ../../hdl/srp/KL_srp_top.sv ../../hdl/top/KL_mrp_strip.sv ../../hdl/top/protocol_processor_top.sv pp_top_wrap.sv sim_main.cpp -o Vpp_top_vid
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:35:57.338346+00:00; duration 6.899 s; exit 2 (expected 2). Log: [raw output](receipts/05-refuse-1002.log).

## 06-missing-binding

Cwd: `/tmp/a161-issue97-rwxncqry/06-missing-binding/tb/pp_top`

```sh
rtk proxy make -j8
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:36:04.332784+00:00; duration 197.352 s; exit 2 (expected 2). Log: [raw output](receipts/06-missing-binding.log).

## 07-child-default

Cwd: `/tmp/a161-issue97-rwxncqry/07-child-default/tb/pp_top`

```sh
rtk proxy make -j8
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:39:21.801471+00:00; duration 196.338 s; exit 0 (expected 0). Log: [raw output](receipts/07-child-default.log).

## 08-remove-wire-guard

Cwd: `/tmp/a161-issue97-rwxncqry/08-remove-wire-guard/tb/pp_top`

```sh
rtk proxy make -j8 fixture-guards
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:42:38.246805+00:00; duration 5.118 s; exit 2 (expected 2). Log: [raw output](receipts/08-remove-wire-guard.log).

## 09-remove-class-d-guard

Cwd: `/tmp/a161-issue97-rwxncqry/09-remove-class-d-guard/tb/pp_top`

```sh
rtk proxy make -j8 fixture-guards
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:42:43.468233+00:00; duration 4.991 s; exit 2 (expected 2). Log: [raw output](receipts/09-remove-class-d-guard.log).

## 10-docs-final

Cwd: `$CANDIDATE`

```sh
rtk proxy make -j8 check
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:34:28.788165+00:00; duration 23.961 s; exit 0 (expected 0). Log: [raw output](receipts/10-docs-final.log).

## 11-source-checks

Cwd: `$CANDIDATE`

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/scripts/source_checks.py
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:35:43.682183+00:00; duration 0.153 s; exit 0 (expected 0). Log: [raw output](receipts/11-source-checks.log).

## 12-sequential-controls

Cwd: `$CANDIDATE`

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/scripts/probes.py
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:35:50.164846+00:00; duration 418.359 s; exit 0 (expected 0). Log: [raw output](receipts/12-sequential-controls.log).

## 13-tool-versions

Cwd: `$CANDIDATE`

```sh
rtk proxy bash -c 'verilator --version; g++ --version; python3 --version; make --version; git --version; rtk --version'
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:36:05.110577+00:00; duration 0.106 s; exit 0 (expected 0). Log: [raw output](receipts/13-tool-versions.log).

## 14-source-final

Cwd: `$CANDIDATE`

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/scripts/source_checks.py
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:39:54.244249+00:00; duration 0.164 s; exit 0 (expected 0). Log: [raw output](receipts/14-source-final.log).

## 15-commit

Cwd: `$CANDIDATE`

```sh
rtk proxy git commit -m 'Assert distinct SRP VID verification fixtures'
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:43:04.423186+00:00; duration 0.05 s; exit 0 (expected 0). Log: [raw output](receipts/15-commit.log).

## 16-final-verification

Cwd: `$CANDIDATE`

```sh
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/scripts/final_checks.py
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:43:04.582473+00:00; duration 0.362 s; exit 0 (expected 0). Log: [raw output](receipts/16-final-verification.log).

## 17-public-handoff

Cwd: `$CANDIDATE`

```sh
rtk proxy gh issue comment 97 --repo Mister-M-alt/protocol-processor-control-plane-avb-milan --body-file $WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/public/author-handoff-comment.md
```

```json
{
  "PATH": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin:$WORKSPACE_HOME/.codex/tmp/arg0/codex-arg0m9zHd6:$WORKSPACE_HOME/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/usr/lib/jvm/default/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl",
  "MAKEFLAGS": "-j8",
  "VERILATOR": "$WORKSPACE_HOME/milan-fpga-management/2026-09-22/donor97-author/bin/verilator"
}
```

Started 2026-09-22T06:43:45.946676+00:00; duration 1.888 s; exit 0 (expected 0). Log: [raw output](receipts/17-public-handoff.log).
