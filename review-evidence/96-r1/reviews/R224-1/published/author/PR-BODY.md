Title: [A157] Expose the SRP Domain default VID as a top parameter and grade its binding in a second pp_top build (#95)

---

[A157] Expose the SRP Domain default VID as a top parameter and grade its binding in a second pp_top build

Closes #95

## Status

Author implementation at `ea93023fbdd31cbf718da0d8f1aab4d750bfd21a` on `95-expose-default-sr-vid`, based on `main` `424c688fa2205b934a7689a58f2aa766420f2326`. This is an interface extension only: one new `protocol_processor_top` parameter carrying the child's existing default, with no port, width or behaviour change. Still pending: hosted gates, the full sweep, and the two independent reviews (R223 internal, R224 external).

## Description

`protocol_processor_top` instantiated `KL_srp_top` without binding its 16-bit `DOM_DEF_VID_P`. The Class A Domain default was therefore the child's own 2, and nothing outside the processor could drive it. Parent kebag-logic/milan-fpga#400 needs a physical parameter path for its generated VID, not an equality check.

- `SRP_DOM_DEF_VID_P` (`logic [15:0]`, default `16'd2`, F01.5 `P-SRP-DOM-DEF-VID`) is declared on the top and bound explicitly: `.DOM_DEF_VID_P (SRP_DOM_DEF_VID_P)` (`hdl/top/protocol_processor_top.sv:144`, `:2147`). The Domain FSM reads it three times: as its reset value, in every LINK_UP declaration, and in the LINK_DOWN revert (10 §6.1 F10.2). A bridge's differing Class A Domain is still adopted at run time.
- `hdl/srp/` is unchanged: the width, the child's own default, timers, priority, adoption, MVRP, frame format and microprograms are all as before.
- Milan v1.2 §4.2.7.2.1 fixes the product value at 2, and F01.5, the integrator guide and the parameter banner say so. Any other value is a verification fixture, not a product profile. Refusing other shipping defaults belongs to the parent builder, per the #400 decision, so the donor adds no elaboration guard.
- Documentation: a new F01.5 row; F10.2 now cites the P-ID instead of the literal (`docs/README.md` §2 single-source rule); 10 §11; integrator guide §2 (parameter row) and §8 (link-down restores the defaults).

## The test

`tb/pp_top` now builds twice. The first build overrides nothing and grades the top's own default across every section, plus the new section DV. The second build overrides the top with the verification-only fixture `0x5A3C` and runs DV alone. The child's own default is also 2, so only the second build can see a dropped or misbound connection. Each binary prints its own per-build line, and the Makefile prints the one canonical tally, summed over both builds.

Section DV runs 20 checks per build on a fresh model of the real top:

- **DV1** reset value on the class-D ports, snapshot word 10 and GET_DOMAIN; nothing declared before the link.
- **DV2** LINK_UP `New {6, 3, default}` byte-exact, with all 16 bits of SRclassVID.
- **DV3** the GET_TX_STATE stream VLAN.
- **DV4** a certified two-class bridge Domain is still adopted: `Lv {6, 3, default}` + `New {6, 3, 5}` byte-exact, one DOMAIN_CHANGE.
- **DV5** LINK_DOWN restores the default with one DOMAIN_CHANGE and declares nothing while down.
- **DV6** LINK_UP re-declares the default.

The fixture gives a different value under every plausible fault: missing or literal (2), 8- or 12-bit truncation (0x003C, 0x0A3C), byte swap (0x3C5A), or priority misroute (reads 4). Its low 12 bits are a legal VID; only the 16-bit wire field shows the top nibble.

Mutation record (`tb/pp_top/README.md` M25 to M31), each run in a scratch copy of the tree:

| # | Planted change | Default build | Fixture build |
|---|---|---|---|
| M25 | binding line removed | 0 of 1391 FAIL | 13 of 20 FAIL |
| M26 | bound to the literal `16'd2` | not run | 13 of 20 FAIL |
| M27 | truncated to 12 bits | not run | 4 of 20 FAIL (the 16-bit wire checks) |
| M28 | bound to `DOM_DEF_PRIO_P` instead | 16 of 1391 FAIL | 13 of 20 FAIL |
| M29 | top default 2 changed to 3 | 18 of 1391 FAIL | 0 of 20 |
| M30 | wrap fixture override removed | not run | 13 of 20 FAIL |
| M31 | control: child default changed to 7, binding intact | 0 FAIL | 0 FAIL |

Existing coverage is unchanged and passing: pp_top S1 and S8 (default declaration, certified adoption), `tb/srp_top` (235) and `tb/srp_encoder` (180, Domain D1 to D9 including the link revert).

## How to get into the same state

```sh
git fetch origin 95-expose-default-sr-vid
git checkout ea93023fbdd31cbf718da0d8f1aab4d750bfd21a
```

## How to validate

```sh
(cd tb/pp_top && make)          # 1411 checks: 1411 PASS, 0 FAIL (1391 default + 20 fixture)
(cd tb/srp_top && make)         # 235 checks: 235 PASS, 0 FAIL
(cd tb/srp_encoder && make)     # 180 checks: 180 PASS, 0 FAIL
(cd tb/timer_map && make)       # 10 shapes OK, 3 guards OK, 1360 checks: 1360 PASS, 0 FAIL
./scripts/lint_hdl.sh           # 37 LINT OK
make check                      # all documentation gates OK
python3 scripts/gen_matrix.py --check   # 86 rows, 0 untested
./syn/yosys/run.sh              # 32 YOSYS OK + XILINX OK; output identical to base
./scripts/run_suites.sh         # full sweep (not run by the author)
```

The author's local results above used Verilator 5.052; CI pins v5.050 and is the reference for that version. Exact commands, logs and the mutation runner are kept with the author handoff; self-test evidence belongs in a PR comment.

## Known limitations / out of scope

- The fixture is verification-only. No shipping build may declare it, and the donor does not refuse it at elaboration.
- DV3 grades the GET_TX_STATE `stream_vlan_id` of a non-declaring source. F05.11 leaves that field undefined; this talker answers the SR-class VID, as S10 and MP3 already grade.
- `./obj_dir/Vpp_top_sim` alone now prints a per-build line; the canonical tally comes from `make`.
- Not run by the author: the complete `run_suites.sh` sweep, the `nvm_port` figures gate (untouched), Verilator v5.050, and any hosted run.
- Parent #400 binds `SRP_DOM_DEF_VID_P` from its generated declaration after this merges; the parent pin is unchanged here.

## Definition of Done

- [x] Issue #95 acceptance 1 to 3 met at `ea93023` (author evidence)
- [x] Self-checking top-level test in both builds, with mutation controls
- [x] Normative registry (F01.5) and integrator documentation updated
- [ ] Complete donor sweep and hosted `hdl` workflow green on the exact head
- [ ] Self-test evidence posted as a PR comment
- [ ] Internal cleared-context review (R223) positive
- [ ] External review (R224) positive, with all lenses covered clean
- [ ] Candidate merge validation and post-merge containment

🤖 Generated with [Claude Code](https://claude.com/claude-code)
