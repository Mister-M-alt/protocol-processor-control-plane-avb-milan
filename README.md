<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Protocol Processor — AVB/Milan Control Plane

Hardware architecture for a compact, deterministic **IEEE 1722.1-2021 (ATDECC) protocol
processor** implementing the control plane of a **non-redundant Milan v1.2 PAAD**
(Professional Audio AVB Device): ADP discovery, Milan ACMP binding/probing, and
AECP/AEM + Milan Vendor Unique command execution — as reusable, HDL-agnostic
architecture documentation, plus the beginning of its SystemVerilog
implementation under `hdl/`.

**Architecture revision 2.0** (2026-08-11): the post-review,
implementation-backed architecture — P0 conformance corrections adversarially
verified against the standards PDFs, the in-scope SRP engine
([10](docs/architecture/10_srp_engine.md)), every sizing width stated
([F01.5](docs/architecture/01_overview.md)), and the first measured RTL (the
1,068-LUT µCPU skeleton, [`syn/ooc/`](syn/ooc/README.md)). Revision 1.x is the
original concept document, retained below as reviewed input. The reference
platform consumes this repository as a git submodule and cuts over by direct
substitution at parity (owner decision 10,
[review §8](docs/00_MILAN_COMPLIANCE_REVIEW.md)).

## Contents

| Path | What it is |
|---|---|
| [`IEEE_1722_1_Hardware_Protocol_Processor.md`](IEEE_1722_1_Hardware_Protocol_Processor.md) | Original concept document (input; reviewed and superseded by `docs/`) |
| [`docs/README.md`](docs/README.md) | Conventions, reading order, audiences — **start here** |
| [`docs/00_MILAN_COMPLIANCE_REVIEW.md`](docs/00_MILAN_COMPLIANCE_REVIEW.md) | Review of the original document against Milan v1.2 (gap register + compliance matrix) |
| [`docs/architecture/`](docs/architecture/) | The architecture: overview, interfaces, packet engine, ADP/ACMP/AECP/SRP engines, memory, timing, verification |
| [`docs/10_RESOURCE_AND_EFFORT.md`](docs/10_RESOURCE_AND_EFFORT.md) | Resource-savings and implementation-effort analysis against the reference platform (measured anchors, four scenarios, phase plan) |
| [`hdl/`](hdl/README.md) | Home of the SystemVerilog implementation; consumed by the reference platform as a git submodule (layout, rules, consumption contract) |
| [`docs/diagrams/`](docs/diagrams/) | Editable diagram sources (draw.io) and committed SVG exports |
| [`Makefile`](Makefile) | `make diagrams` regenerates all draw.io and WaveDrom SVGs; `make check` validates figures, links and the compliance matrix |

All diagrams are editable text: Mermaid and WaveDrom sources are embedded in the
Markdown; the three richest pictures are draw.io files under `docs/diagrams/src/`.

## Specifications targeted

- AVnu **Milan Specification, Consolidated v1.2** (Final, 2023-11-30) — non-redundant PAAD
- **IEEE Std 1722.1-2021** (ATDECC), with IEEE 1722-2016, IEEE 802.1AS-2011, IEEE 802.1Q-2014

The specification PDFs are not distributed in this repository (copyrighted documents).

## License

Licensed under the **CERN Open Hardware Licence Version 2 — Weakly Reciprocal**
(CERN-OHL-W-2.0). See [`LICENSE`](LICENSE).
