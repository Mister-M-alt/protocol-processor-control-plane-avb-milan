<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Diagram Sources & Toolchain

Three figures are rich enough to warrant free-form drawing; they are draw.io sources
with committed SVG exports. **Everything else** (Mermaid, WaveDrom) lives as editable
text inside the host Markdown documents — see `docs/README.md` §3.

## Inventory

| Source (`src/`) | Export | Figure | Host document |
|---|---|---|---|
| `01-system-context.drawio` | `01-system-context.svg` | F01.1 — PAAD system context | `architecture/01_overview.md` §2 |
| `01-top-level.drawio` | `01-top-level.svg` | F01.2 — processor top level | `architecture/01_overview.md` §4 |
| `03-shared-datapath.drawio` | `03-shared-datapath.svg` | F03.1 — shared datapath & memory interconnect | `architecture/03_packet_engine.md` §2 |

Rule: **source and export are committed together**; `make stale` fails the tree if an
SVG is older than its `.drawio`.

## Commands

```sh
# regenerate all exports (from repository root)
make diagrams

# one file, manually
drawio -x -f svg --crop -o docs/diagrams/01-top-level.svg docs/diagrams/src/01-top-level.drawio
# headless fallback (no display):
xvfb-run -a drawio --no-sandbox -x -f svg --crop -o <out.svg> <in.drawio>

# full documentation gate: mermaid/wavedrom lint + links + compliance matrix + SVG freshness
make check
```

## Style guide

- **draw.io**: snap to a 10 px grid; sans-serif 11–12 pt labels; clock/reset domains as
  tinted container groups; no embedded bitmaps.

  **Routing discipline — follow this or edges will overlap boxes.** draw.io's automatic
  routing sends edges *through* shapes and drops labels at the geometric midpoint of an
  edge, which lands them on unrelated blocks. Therefore:

  1. **Every edge declares `exitX/exitY/entryX/entryY`.** Never leave a connection
     floating; pick the side deliberately, and give parallel edges distinct fractions
     (`exitX=0.3`, `0.5`, `0.7`) so they fan out instead of stacking.
  2. **Every edge that leaves its row or column carries explicit waypoints**
     (`<Array as="points">`) routed through a **gutter** — a deliberate empty lane
     between blocks. The current layouts reserve horizontal gutters between block rows
     and vertical gutters between block columns; keep them empty.
  3. **Two edges never share a lane.** Parallel runs are spaced ≥ 20 px
     (e.g. one vertical at x=1120, the next at x=1170).
  4. **Labels are short and opaque**: `labelBackgroundColor=#ffffff;fontSize=10`, one or
     two words. Anything longer belongs in the host document's prose or in the
     figure's legend box.
  5. Interface-class tags (A–F per `architecture/02_interfaces.md`) go on short external
     edges or in the legend — never on long internal edges.

  After any edit, re-export and **look at the result** (`drawio -x -f png --scale 1.1 -o
  /tmp/check.png <src>`); overlap is invisible in the XML.
- **Mermaid**: theme-neutral (no `%%{init}%%`), stable lower-kebab node IDs, no HTML
  labels; FSM arcs labeled with `T-…`/`A…` IDs, not literal values.
- **WaveDrom**: strict JSON (double-quoted keys); MSB left; in `reg` figures, field
  names carry byte offsets (`seq_id@48`) or masks (`REGISTERING_FAILED (0x40)`) where
  misreading is dangerous.
