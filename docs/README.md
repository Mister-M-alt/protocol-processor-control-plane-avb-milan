<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Documentation Guide — Conventions, Reading Order, Audiences

This directory specifies the architecture of an IEEE 1722.1-2021 protocol processor
implementing the control plane of a **non-redundant Milan v1.2 PAAD**. Read this page
first: it defines the conventions every other document relies on.

## 1. Reading order

```
docs/README.md (this page)
   └─► 00_MILAN_COMPLIANCE_REVIEW.md      why the architecture looks the way it does
        └─► architecture/01_overview.md    scope, top-level, parameters, Milan↔IEEE deltas
             └─► 02_interfaces.md          every external contract
                  └─► 03_packet_engine.md  shared RX/TX datapath
                       └─► 04_adp_engine.md ─► 05_acmp_engine.md   (04 before 05: ACMP
                            └─► 10_srp_engine.md                    consumes ADP events;
                                 └─► 06_aecp_engine.md              10 with/after 05 — it
                                      └─► 07_memory_maps.md         serves 05's srp calls)
                                           └─► 08_timing.md ─► 09_verification.md
```

| Role | Path |
|---|---|
| Implementer (RTL) | 01 → 02 → 03 → 04 → 05 → 10 → 06 → 07 → 08 (00 on second pass) |
| Verifier | 00 §6 matrix → 01 → 08 → 09 → the F05.3 / F06.14 behavior tables |
| System integrator | 01 → 02 → 07 §5 (persistence) → 02 §7 (side-port) |
| Compliance reviewer | 00 only, following its links into the architecture |

## 2. Identifier registries

Every normative artifact has a stable ID. IDs never change meaning; new ones are appended.

| Prefix | Meaning | Defined in |
|---|---|---|
| `F<doc>.<n>` | Figure/table of documentation rank (e.g. `F05.3`) | the host document |
| `REQ-<AREA>-<nnn>` | Requirement row of the compliance matrix | 00 §6 |
| `GAP-<nn>` | Review finding | 00 §5 |
| `T-<ENGINE>-<NAME>` | Timing constant (e.g. `T-ACMP-CMD`) | **08 §2 only** |
| `P-<NAME>` | Synthesis-time parameter (e.g. `P-N-CONTROLLERS`) | **01 §7 only** |
| `A<n>` | Listener-state-machine action primitive | 05 §6.3 legend |
| `Δ<n>` | Milan-overrides-IEEE delta | **01 §6 only** |

**Single-source rules** — a value lives in exactly one table; everywhere else references the ID:
- Timing values only in `F08.1`. FSM arcs are labeled `T-…`, never `200 ms`.
- Parameter values only in `F01.5`.
- External status signal names only in `F02.10`.
- Milan↔IEEE deltas only in `F01.4`; documents cite `Δn`.
- Record/memory layouts only in 07; engine docs link to them.

Scope of those rules: they bind the **architecture** documents (01–09). Three
deliberate exceptions: the compliance review (00) quotes spec requirement text
*including its values* — that is its job; tick-generation rates belong to the clocking
contract (02 §2 with `F08.2`); and PDU field constants such as ADP `valid_time` belong
to their field-sourcing table. `make check` enforces the rest.

## 3. Figures: one source, one home

- Every figure exists in **exactly one** host document, preceded by an explicit anchor:
  `<a id="fig-05-listener-matrix"></a>`. Reuse is a relative link, never a copy.
- **Mermaid** (block/FSM/sequence/flow): fenced ` ```mermaid ` blocks — the fence *is* the
  editable source. Style: one diagram per fence; no `%%{init}%%` theming; stable
  lower-kebab node IDs; no HTML labels; `stateDiagram-v2` for FSMs, `sequenceDiagram`
  for on-the-wire flows, `flowchart` for datapaths/decision trees, `classDiagram` only
  for the descriptor tree.
- **WaveDrom** (waveforms and PDU/register bit layouts): GitHub does **not** render
  WaveDrom natively, so every block appears as a committed SVG
  (`docs/diagrams/wavedrom/<anchor>.svg`, rendered by `make wavedrom` via the Python
  `wavedrom` package) with the fenced ` ```wavedrom ` source — still the single
  editable artifact — collapsed in a `<details>` block directly below the image.
  Sources are **strict JSON** (double-quoted keys). `reg` conventions: fields listed in
  wire order render **bottom lane first** (standard bitfield layout — every caption
  says so); field names carry the byte offset (`@n`) or mask where ambiguity is
  dangerous; keep fields ≤ 64 bits (wider fields leave unlabeled middle lanes); `head`/
  `foot` text is signal-format-only — for `reg`, put it in the caption. Editing a block
  without re-rendering fails `make check` (`wavedrom-check`).
- **draw.io** (the three richest pictures only): source `docs/diagrams/src/<name>.drawio`,
  committed export `docs/diagrams/<name>.svg`, embedded via `![…](../diagrams/<name>.svg)`.
  Regenerate with `make diagrams` (see `docs/diagrams/README.md`).
- Interface waveforms are **class templates** (one per interface class); per-instance
  differences live in signal tables, never in cloned waveforms.

## 4. Spec citation and terminology

- Citations are plain text: `(Milan §5.5.3.5)`, `(IEEE 1722.1 §8.2.1)`, `(IEEE 1722.1
  Table 7-140)` — no links to the (non-distributed, copyrighted) PDFs.
  Milan page references are *printed* pages (printed = PDF − 7).
- **Milan naming is primary**: `BIND_RX` (IEEE `CONNECT_RX`), `UNBIND_RX`
  (`DISCONNECT_RX`), `PROBE_TX` (`CONNECT_TX`). The IEEE name appears in parentheses at
  first use per document. Precedence: where Milan differs from IEEE 1722.1, **Milan wins**
  (Milan §5.5.2.1); every such point is tagged with a `Δn` callout:

  > **Δn — Milan overrides IEEE:** one-line statement. *(template)*

- Bit-numbering warning, repeated verbatim above every mask table:

  > ⚠ Bit tables in Milan and IEEE 1722.1 are **MSB-first** (bit 31 ⇔ mask
  > `0x00000001`). The **hex mask column is authoritative**; never derive shifts from
  > bit-number columns.

- PDU length rule: total PDU size = `control_data_length` **+ 12** (figures in IEEE
  1722.1-2021 printing `+ 8` are an erratum). Padding to minimum Ethernet frame is
  **excluded** from `control_data_length`.

## 5. Sequence-diagram participants (fixed vocabulary)

All `sequenceDiagram` figures use these participant names so separate diagrams compose
into one story:

| Name | Is |
|---|---|
| `CTRL` | remote ATDECC controller |
| `TALKER` | remote talker entity (its ACMP/ADP/SRP behavior) |
| `ADP` / `ACMP` / `AECP` | the corresponding engine inside this processor |
| `NOTIF` | unsolicited-notification engine + controller registry |
| `SRP` | SRP/MSRP + MAAP adapter (and, transitively, the SRP network) |
| `GPTP` / `AVTP` / `MCLK` | gPTP / streaming / media-clock adapters |
| `NVM` | persistence manager |
| `TX` | TX arbiter / wire egress |

## 6. Editing workflow

| To change… | Do |
|---|---|
| A block/FSM/sequence figure | edit the ` ```mermaid ` fence in place; `make lint` |
| A waveform or bit layout | expand the `<details>` under the image, edit the ` ```wavedrom ` JSON in place, run `make wavedrom` (re-renders the SVG); `make check` |
| A top-level picture | edit `docs/diagrams/src/*.drawio` in the draw.io app; `make diagrams`; commit source **and** SVG |
| A timing value | edit `F08.1` only; consumers reference `T-…` IDs |
| A parameter default | edit `F01.5` only |
| Anything | `make check` (lint + links + matrix + stale) must pass before commit |
