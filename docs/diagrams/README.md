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

# lint every embedded mermaid/wavedrom block in docs/ and README.md
make lint

# check exports are newer than sources
make stale
```

## Style guide

- **draw.io**: snap to a 10 px grid; sans-serif 12 pt labels; clock/reset domains as
  tinted container groups; orthogonal edge routing; edge labels name the interface
  class (A–F per `architecture/02_interfaces.md`); no embedded bitmaps.
- **Mermaid**: theme-neutral (no `%%{init}%%`), stable lower-kebab node IDs, no HTML
  labels; FSM arcs labeled with `T-…`/`A…` IDs, not literal values.
- **WaveDrom**: strict JSON (double-quoted keys); MSB left; in `reg` figures, field
  names carry byte offsets (`seq_id@48`) or masks (`REGISTERING_FAILED (0x40)`) where
  misreading is dangerous.
