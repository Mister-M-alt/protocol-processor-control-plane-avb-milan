[A165] TAKEN

Executor A165 (Opus), sole implementation author for #98. The isolated clone is verified clean on issue-linked branch `98-clarify-srp-domain-events` at base main `8452f564294300a82d56eed464276576f65f4d58`. Its tree is `ec259f379560863e6ea49c6043353f0c11fe714d`, the PR96 reviewed head tree.

Authority read:
- this issue and its settled scope;
- [R223-S1](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/pull/96#issuecomment-5772081981);
- `docs/README.md` §2 and §3 (one source, one home; the Mermaid fence is the editable source);
- 10 §2, §6.1 F10.2 and §11; 01 F01.5; integrator guide §2 and §8;
- `hdl/srp/KL_srp_domain.sv:155-171`;
- `tb/pp_top` DV5 and DV6.

Interpreted scope: relabel only the F10.2 arc `ADOPTED --> DEFAULTS` (`docs/architecture/10_srp_engine.md:205`), so that the defaults are restored at LINK_DOWN and declared at the next LINK_UP. States, the other four arcs and their meanings stay unchanged. There is no RTL, test, timer, parameter, interface, pin or other-figure change, and PP25/PR26 and the pp_top fixture lane are not touched.

Exports: I searched every committed SVG, PNG and draw.io asset. None is a generated representation of F10.2, which GitHub renders from the fence, so no export should change.

Scope note: `tb/srp_encoder/README.md:75-77` quotes the old arc label in a recorded-interpretation note. Its behaviour statement already matches the new label. It stays unchanged under the no-test, no-cleanup scope, and I will list it in the handoff.

Validation plan:
- `make check`;
- an mmdc rendering of the changed F10.2, compared against the RTL;
- source and scope receipts.

The existing DV5/DV6 evidence is reused and no tests are added. I make one local commit, with no push or PR. R233/R234 stay with the manager.

Blockers: none.
