Title: [A165] Show F10.2's Domain defaults restored at LINK_DOWN and declared at the next LINK_UP (#98)

---

[A165] Show F10.2's Domain defaults restored at LINK_DOWN and declared at the next LINK_UP

Closes #98

## Status

This is the author commit `88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf` on `98-clarify-srp-domain-events`, based on `main` `8452f564294300a82d56eed464276576f65f4d58`. It is documentation only: one Mermaid label line. Still pending: manager validation and the two independent reviews, R233 (internal) and R234 (external).

## Description

F10.2 labelled its `ADOPTED --> DEFAULTS` arc `LINK_DOWN then LINK_UP / back to defaults`. A reader of the figure alone could therefore place the revert, and its DOMAIN_CHANGE, at LINK_UP (R223-S1 on PR96). The RTL does two separate things on the two events (`hdl/srp/KL_srp_domain.sv`, unchanged):

- LINK_DOWN (`:155-165`) restores the default priority and VID and leaves ADOPTED. Nothing is declared while the link is down.
- LINK_UP (`:166-171`) declares the defaults, `New {6, DEF_PRIO_P, DEF_VID_P}`.

The arc (`docs/architecture/10_srp_engine.md:205`) now reads `LINK_DOWN / restore defaults, declared again on the next LINK_UP`. The states, the other four arcs and all prose are unchanged, and no arc is added. The figure now agrees with text that already said this: F01.5, 10 §11 and integrator guide §2 and §8.

F10.2 has no committed export, because GitHub renders the fence itself. None of the 34 diagram assets represents it, so nothing is regenerated.

## Evidence

- `make check` passes at the exact head: 41 Mermaid blocks rendered by mmdc, 18 WaveDrom, 807 links, the matrix gates, and nothing stale.
- An mmdc render of F10.2 at base and head differs only in this arc's label. Both events read as the RTL does.
- Existing executable evidence at the identical base tree (PR96 review receipts and hosted `main` run 35694588985):
  - DV5 grades the LINK_DOWN restore, and DV6 the LINK_UP declaration. Both pass in both pp_top builds.
  - Mutant R36 (LINK_DOWN branch) fails DV5, and mutant R37 (LINK_UP branch) fails DV6.
  - srp_encoder D9 covers the same two events.

  No new tests are added for this reversible documentation change.

## How to validate

```sh
git checkout 88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf
git diff 8452f564294300a82d56eed464276576f65f4d58 --stat   # 1 file, 1 insertion, 1 deletion
make check                                                  # all documentation gates OK (needs mmdc)
```

Then read the rendered F10.2 against `hdl/srp/KL_srp_domain.sv:155-171`.

## Out of scope

- No RTL, test, timer, parameter, interface, pin or other-figure change.
- `tb/srp_encoder/README.md:75-77` still quotes the old label in a recorded-interpretation note. Its interpretation already matches the new label.
- The arc still does not name DOMAIN_CHANGE, as before.

## Definition of Done

- [x] F10.2 shows default restoration at LINK_DOWN and declaration at LINK_UP (author evidence)
- [x] Other transitions unchanged; no generated asset exists or changed
- [x] `make check` and an actual mmdc render checked against `KL_srp_domain.sv:155-171`
- [ ] Donor full validation and the hosted `hdl` workflow on the exact head
- [ ] Internal cleared-context review (R233) positive
- [ ] External review (R234) positive, with all lenses clean
- [ ] Candidate merge validation and post-merge containment
