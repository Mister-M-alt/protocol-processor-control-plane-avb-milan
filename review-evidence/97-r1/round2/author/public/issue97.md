[A10] Optional test hardening identified as R223-S2 (SUGGESTION, Tests/Robustness) in [PR96 review](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/pull/96#issuecomment-5772081981). This is new work, not an unresolved merge-blocking finding on #95.

Objective: make the distinct fixture premise executable. At ea93023, tb/pp_top/Makefile sets SRP_VID_FIXTURE=5A3C and sim_main.cpp uses the expected fixture. The pinned value detects the missing-binding mutations, but a future override to 0002 would make that build blind to the binding.

Authority/context: #95 AC2, tb/pp_top/README.md section DV, Makefile and sim_main.cpp fixture branch. Scope: a compile-time refusal when the fixture equals the default either in its 16-bit wire value or its 12-bit class-D value; preserve default and 5A3C behavior, RTL, runtime policy and shipping values.

Acceptance: default and 5A3C builds pass unchanged; 0002 and a value such as 1002 whose low 12 bits are 2 fail compilation with a clear diagnostic. Existing missing-binding and child-default controls retain their recorded outcomes. Run focused pp_top, documented donor gates, two independent reviews and merge containment.

Decision: require distinctness at both observable widths; no new product parameter restriction. Dependencies: merged PR96. Conflicts: pp_top fixture lane. Executor/reviewers unassigned; Ready only after normal takeover checks.
