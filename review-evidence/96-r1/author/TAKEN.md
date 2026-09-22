[A157] TAKEN

Branch: `95-expose-default-sr-vid`, cut from main `424c688fa2205b934a7689a58f2aa766420f2326`. One author slot; reviewers R223 (internal) and R224 (external) as assigned by [A10].

Authoritative references: this issue and its settled acceptance; parent kebag-logic/milan-fpga#400 and its 2026-09-22 pre-implementation decision (vid row); Milan v1.2 4.2.7.2.1; `docs/README.md` §2 (P-IDs live only in 01 §7 F01.5, parameter values only in F01.5); `docs/architecture/01_overview.md` §7 F01.5; `docs/architecture/10_srp_engine.md` §6.1 F10.2 and §11; `docs/guides/integrator.md` §2 and §8; `hdl/top/protocol_processor_top.sv` (KL_srp_top instance); `hdl/srp/KL_srp_top.sv` `DOM_DEF_VID_P`; `hdl/srp/KL_srp_domain.sv` (reset, LINK_UP declaration and LINK_DOWN revert all read `DEF_VID_P`).

Interpreted scope: add one public `protocol_processor_top` parameter, `logic [15:0] SRP_DOM_DEF_VID_P = 16'd2` (P-ID `P-SRP-DOM-DEF-VID`), bound explicitly to `KL_srp_top.DOM_DEF_VID_P`. Register it in F01.5 and document it in the integrator guide and the SRP engine page (F10.2 cites the P-ID instead of repeating the value, per the single-source rule). `hdl/srp/` is not touched: no width, default, timer, priority, adoption, MVRP, frame-format or microprogram change. The shipping default stays 2; a non-2 value exists only as a verification fixture proving propagation.

Test plan: a focused Domain-default phase in `tb/pp_top` drives the real top on a fresh model. The suite Makefile builds it twice: once overriding nothing (expects the top's own default 2) and once with a verification-only fixture override. Each build grades the reset value, the LINK_UP Domain declaration on the MSRP wire byte-exact (the full 16-bit SRclassVID), the class-D port, snapshot word 10 and GET_DOMAIN, the GET_TX_STATE stream VLAN a controller sees, adoption of a bridge Domain (Lv of the default then New of the adopted), LINK_DOWN revert and LINK_UP re-declaration. Mutation controls in a scratch copy: binding removed, bound to a literal 2, truncated, and the top default changed.

Validation plan (compile jobs capped at 8): `tb/pp_top` (both builds), `tb/srp_top`, `tb/srp_encoder` (child-level Domain adoption and link revert), `tb/timer_map` (real-top elaboration sweep), `./scripts/lint_hdl.sh`, `make check`, `python3 scripts/gen_matrix.py --check`, and a read-only run of `./syn/yosys/run.sh`. PR26 and its `syn/yosys/run.sh` are not edited. The full sweep, hosted gates, reviews and publication stay with the manager.

Blockers: none.
