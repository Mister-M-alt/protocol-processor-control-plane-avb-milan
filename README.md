<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Protocol Processor — AVB/Milan Control Plane

Hardware architecture for a compact, deterministic **IEEE 1722.1-2021 (ATDECC) protocol
processor** implementing the control plane of a **non-redundant Milan v1.2 PAAD**
(Professional Audio AVB Device): ADP discovery, Milan ACMP binding/probing, and
AECP/AEM + Milan Vendor Unique command execution — as reusable, HDL-agnostic
architecture documentation. No RTL yet: this repository is the specification a
VHDL/SystemVerilog/other implementation is written against.

## Contents

| Path | What it is |
|---|---|
| [`IEEE_1722_1_Hardware_Protocol_Processor.md`](IEEE_1722_1_Hardware_Protocol_Processor.md) | Original concept document (input; reviewed and superseded by `docs/`) |
| [`docs/README.md`](docs/README.md) | Conventions, reading order, audiences — **start here** |
| [`docs/00_MILAN_COMPLIANCE_REVIEW.md`](docs/00_MILAN_COMPLIANCE_REVIEW.md) | Review of the original document against Milan v1.2 (gap register + compliance matrix) |
| [`docs/architecture/`](docs/architecture/) | The architecture: overview, interfaces, packet engine, ADP/ACMP/AECP engines, memory, timing, verification |
| [`docs/diagrams/`](docs/diagrams/) | Editable diagram sources (draw.io) and committed SVG exports |
| [`Makefile`](Makefile) | `make diagrams` / `make lint` / `make stale` — regenerate and validate all figures |

All diagrams are editable text: Mermaid and WaveDrom sources are embedded in the
Markdown; the three richest pictures are draw.io files under `docs/diagrams/src/`.

## Specifications targeted

- AVnu **Milan Specification, Consolidated v1.2** (Final, 2023-11-30) — non-redundant PAAD
- **IEEE Std 1722.1-2021** (ATDECC), with IEEE 1722-2016, IEEE 802.1AS-2011, IEEE 802.1Q-2014

The specification PDFs are not distributed in this repository (copyrighted documents).

## License

Licensed under the **CERN Open Hardware Licence Version 2 — Weakly Reciprocal**
(CERN-OHL-W-2.0). See [`LICENSE`](LICENSE).
