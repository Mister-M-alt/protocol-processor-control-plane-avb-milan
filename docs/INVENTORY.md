<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Documentation inventory

**In short.** One row per Markdown file in this repository: what the page is for, who it
is written for, the code or clause it describes, and whether it describes the tree today.
`make inventory` (part of `make check`) fails on a file without a row, a row without a
file, a status outside the vocabulary below, an empty evidence cell, and a row whose
status disagrees with the page's own banner. Add the row in the same change as the page.

**Status vocabulary.**

| Status | Meaning |
|---|---|
| current | describes the tree at this head; the evidence says how that was checked |
| generated | written by a generator whose freshness gate holds it to the tree |
| stale | known to disagree with the tree; the evidence names the tracking issue |
| superseded | kept for provenance only, under a banner that points at its replacement |
| obsolete | describes removed code or an abandoned design, and is to be deleted |

**Audiences.** *new reader*: anyone starting here. *integrator*: wires the top into an
SoC. *operator*: brings a device up and reads its diagnostics. *HDL engineer*: changes
the RTL. *verification*: runs or extends the suites. *compliance reviewer*: traces the
design to Milan v1.2 and IEEE 1722.1-2021. *author*: edits these pages.

**The sweep checks behind "current"** (2026-10-30, at main `a9e84b66`): every relative
link and anchor resolves under GitHub's own anchor rule; every backticked repository path
exists; every port, parameter and constant name (`*_i`, `*_o`, `*_P`, `*_C`) a page uses
is declared or used in `hdl/` or `tb/`; every clause and table number a page cites under
Milan v1.2, IEEE 1722.1-2021 or IEEE 802.1Q-2014 exists in that standard, and a sample
of each page's citations was read for topic; the F01.5 values were compared with the
RTL's declarations; and every suite but `pp_top` and `srp_top` was run for its tally.
What each row adds is in its evidence cell.

## Start here and conventions

| File | Purpose | Audience | Describes | Status | Evidence |
|---|---|---|---|---|---|
| [`README.md`](../README.md) | what the processor is, the start-here path, repository map, how to build and check | new reader | the repository | current | sweep checks; start-here path added in this sweep |
| [`docs/README.md`](README.md) | documentation conventions: reading order, ID registries, figure rules, citation rules, editing workflow | author | `docs/` and the gates of 09 §7 | current | sweep checks; gate list matches the Makefile `check` target |
| [`docs/GLOSSARY.md`](GLOSSARY.md) | defined terms, each pointing at its home | new reader | the vocabulary of every page | current | written in this sweep; each cited clause number read in its standard |
| [`docs/INVENTORY.md`](INVENTORY.md) | this page: every Markdown file and its status | author, new reader | every Markdown file | current | held by `make inventory` |
| [`docs/guides/README.md`](guides/README.md) | the three task guides and the diagram inventory | new reader | `docs/guides/`, `docs/diagrams/` | current | sweep checks |
| [`docs/guides/hdl-engineer.md`](guides/hdl-engineer.md) | conventions, block structure, shared-service contracts, running the suites, known spec and tree divergences | HDL engineer | `hdl/`, `tb/`, `scripts/` | current | sweep checks; §9 divergence table re-read against the RTL: the shutdown row is resolved by 01 §5 and removed, the dispatch row reworded to the engine banner, the on-chip image row narrowed to F03.1 (F07.1 is already current) |
| [`docs/guides/integrator.md`](guides/integrator.md) | the instantiation contract: ports, parameters, clocking and reset, memory masters, tie-offs, bring-up order | integrator | `hdl/top/protocol_processor_top.sv` | current | sweep checks; `make params` holds its parameter table to the top; two shortened port names spelled out |
| [`docs/guides/operator.md`](guides/operator.md) | what appears on the wire, what a controller sees, the snapshot and control windows, bring-up ladder | operator | the side port (`KL_pp_side_port`), the wire behaviour | current | sweep checks |

## Architecture (the specification of record)

| File | Purpose | Audience | Describes | Status | Evidence |
|---|---|---|---|---|---|
| [`docs/architecture/01_overview.md`](architecture/01_overview.md) | scope, top level, operational model, Milan deltas (F01.4), parameter master table (F01.5) | new reader, HDL engineer | the whole processor; `protocol_processor_top` parameters | current | sweep checks; F01.5 compared with the top, `pp_pkg`, `ucpu_pkg`, `KL_pp_trace_ring`, `KL_mrp_strip`, `KL_acmp_talker` and the NVM producers: the stale timer-slot example removed (08 §5 owns it), P-DISPATCH-ROM-W marked as having no RTL consumer, the shutdown model corrected to the enable's fall (`protocol_processor_top.sv:2027`); F01.4 clause topics read in Milan v1.2 |
| [`docs/architecture/02_interfaces.md`](architecture/02_interfaces.md) | every external contract, interface classes A to F, the status dictionary F02.10 | integrator, HDL engineer | the top's ports | current | sweep checks; two 802.1Q citations corrected (§35.2.2.7.2, §34.6.1) |
| [`docs/architecture/03_packet_engine.md`](architecture/03_packet_engine.md) | the shared RX and TX datapath: validation, slots, dispatch, scoreboard, originator, TX arbitration | HDL engineer | `hdl/packet_engine/` | current | sweep checks; a citation of a clause IEEE 1722.1-2021 does not have corrected to `T-BUDGET-AECP-WC` and §9.3.2.6 |
| [`docs/architecture/04_adp_engine.md`](architecture/04_adp_engine.md) | discovery: the advertise and talker-discovery state machines | HDL engineer, compliance reviewer | `hdl/adp/KL_adp_engine.sv` | current | sweep checks |
| [`docs/architecture/05_acmp_engine.md`](architecture/05_acmp_engine.md) | Milan connection management: the listener machine, its transition matrix F05.3, the stateless talker | HDL engineer, compliance reviewer | `hdl/acmp/` | current | sweep checks; anchors repointed to GitHub's rule |
| [`docs/architecture/06_aecp_engine.md`](architecture/06_aecp_engine.md) | AEM and MVU commands, the command master table F06.14, registry, notifications, lock, µcode | HDL engineer, compliance reviewer | `hdl/aecp/` | current | sweep checks; F06.14 is held to the command table by `make aecpcmd` |
| [`docs/architecture/07_memory_maps.md`](architecture/07_memory_maps.md) | the entity model, records, NVM layout and flows, side-port map | HDL engineer, integrator | `hdl/aecp/desc/`, the NVM producers, `KL_pp_side_port` | current | sweep checks; F07.1's ROM node corrected: no dispatch or response-size ROM ships (06 §8.1) |
| [`docs/architecture/08_timing.md`](architecture/08_timing.md) | every timing constant (F08.1), timer hardware, budgets, the timer slot map F08.4 | HDL engineer, compliance reviewer | `hdl/common/KL_pp_timer_service.sv`, `pp_pkg::pp_timer_map()` | current | sweep checks; F08.4's 68 and 91 slots recomputed from `pp_pkg.sv`; F08.1 clause topics and the MRP and AECP values read in Milan v1.2 Table 4.3 and IEEE 1722.1-2021 §9.3.2.6 |
| [`docs/architecture/09_verification.md`](architecture/09_verification.md) | test categories, traceability, the documentation gates (§7), the suites and their evidence (§8) | verification | `tb/`, `scripts/` | current | sweep checks; §7 lists every `make check` target, `inventory` included |
| [`docs/architecture/10_srp_engine.md`](architecture/10_srp_engine.md) | the MSRP and MVRP endpoint participant | HDL engineer, compliance reviewer | `hdl/srp/` | current | sweep checks; one 802.1Q citation corrected (§34.6.1) |
| [`docs/architecture/11_maap_engine.md`](architecture/11_maap_engine.md) | the IEEE 1722-2016 Annex B block claim | HDL engineer, compliance reviewer | `hdl/maap/KL_pp_maap.sv` | current | sweep checks |
| [`docs/00_MILAN_COMPLIANCE_REVIEW.md`](00_MILAN_COMPLIANCE_REVIEW.md) | the review of the concept document (§1 to §4), the gap register (§5), the requirement matrix (§6) and dispositions (§7, §8) | compliance reviewer | Milan v1.2 against the architecture | current | sweep checks; `make matrix` and `make reqtags` hold §5 and §6; its §1 to §4 review the superseded concept document and say so |
| [`docs/traceability/MODULE_MATRIX.md`](traceability/MODULE_MATRIX.md) | which suites compile each module | verification | `hdl/` against `tb/` | generated | `scripts/gen_matrix.py --check` (`make modmatrix`) |
| [`docs/diagrams/README.md`](diagrams/README.md) | the diagram classes, sources, exports and toolchain | author | `docs/diagrams/` | current | sweep checks; `make figures` holds the file classes |

## Retired

| File | Purpose | Audience | Describes | Status | Evidence |
|---|---|---|---|---|---|
| [`IEEE_1722_1_Hardware_Protocol_Processor.md`](../IEEE_1722_1_Hardware_Protocol_Processor.md) | the original concept sketch the compliance review read | compliance reviewer | nothing in the tree | superseded | banner points at `docs/architecture/` |
| [`docs/10_RESOURCE_AND_EFFORT.md`](10_RESOURCE_AND_EFFORT.md) | the planning analysis of 2026-08-11 and the measured substitution outcome of 2026-08-14 | maintainer | the decision to build this processor | superseded | bannered in this sweep: its estimates and recommendations predate the build, and the processor now also serves SRP, which its §10 advised against; points at `syn/ooc/README.md` and the architecture |
| [`docs/history/02-class-a-word-stream.md`](history/02-class-a-word-stream.md) | the 32-bit class-A word stream the first design specified and no port carries | HDL engineer | nothing in the tree | superseded | banner points at 02 §3 and the integrator guide §3 |

## Implementation and measurement

| File | Purpose | Audience | Describes | Status | Evidence |
|---|---|---|---|---|---|
| [`hdl/README.md`](../hdl/README.md) | the RTL layout, coding rules and the submodule consumption contract | HDL engineer, integrator | `hdl/` | current | sweep checks |
| [`syn/ooc/README.md`](../syn/ooc/README.md) | out-of-context synthesis recipes and the recorded area measurements, each with its date and trees | HDL engineer | `syn/ooc/`, the processor's area | current | sweep checks; its measurements are dated records of the trees they name |

## Suite READMEs (`tb/`)

Each says what its suite proves, how to run it, and its mutation record. A "run" figure is
the tally of `make` in that suite at main `a9e84b66`.

| File | Purpose | Audience | Describes | Status | Evidence |
|---|---|---|---|---|---|
| [`tb/acmp_listener/README.md`](../tb/acmp_listener/README.md) | the listener executor against the F05.3 matrix | verification | `KL_pp_acmp_listener` | current | run 3,170 checks, as stated |
| [`tb/acmp_nvm/README.md`](../tb/acmp_nvm/README.md) | the binding NVM shadow and the manager arbiter | verification | `KL_acmp_nvm_shadow`, `KL_pp_nvm_mgr_arb` | current | run 388 checks, as stated |
| [`tb/acmp_talker/README.md`](../tb/acmp_talker/README.md) | the stateless talker responder and its DA gate | verification | `KL_acmp_talker` | current | run 1,561 checks, as stated |
| [`tb/adp_engine/README.md`](../tb/adp_engine/README.md) | the ADP engine, with the walk of F04.2 and F04.3 | verification | `KL_adp_engine` | current | run 1,359 checks, as stated |
| [`tb/aecp_cmds/README.md`](../tb/aecp_cmds/README.md) | what the AECP engine decodes and dispatches, observed, and the record the command gate reads | verification | `KL_aecp_engine`, `tb/aecp_cmds/decode.json` | current | run 181 checks; `make aecpcmd` holds its record |
| [`tb/aecp_notify/README.md`](../tb/aecp_notify/README.md) | the registry monitor lifecycle and notification controls | verification | `KL_aecp_notify` | current | run 131 checks, as stated |
| [`tb/ca_originator/README.md`](../tb/ca_originator/README.md) | the CONTROLLER_AVAILABLE frame builder | verification | `KL_aecp_ca_originator` | current | run 16 checks |
| [`tb/desc_mem_guard/README.md`](../tb/desc_mem_guard/README.md) | descriptor memory response isolation | verification | `KL_aecp_desc_mem_guard` | current | run 78 checks |
| [`tb/desc_store/README.md`](../tb/desc_store/README.md) | the entity-model store and the descriptor model lint | verification | `KL_aecp_desc_store`, `hdl/aecp/desc/` | current | run 586 checks; headline corrected from 584 |
| [`tb/dispatch/README.md`](../tb/dispatch/README.md) | the normalized-transaction path into dispatch | verification | `KL_pp_normalizer`, `KL_pp_dispatch` | current | run 211 checks; headline corrected from 179, mutation record dated |
| [`tb/dyn_state/README.md`](../tb/dyn_state/README.md) | the dynamic-state store | verification | `KL_aecp_dyn_state` | current | written in this sweep (the suite had none); run 118 checks |
| [`tb/elab_guards/README.md`](../tb/elab_guards/README.md) | every elaboration guard stops every front end | verification, HDL engineer | the parameter guards in `hdl/` | current | run 155 checks |
| [`tb/event_router/README.md`](../tb/event_router/README.md) | the event fan-in and fan-out | verification | `KL_pp_event_router` | current | run 81 checks, as stated |
| [`tb/lsn_admit/README.md`](../tb/lsn_admit/README.md) | the listener admission gate | verification | `KL_pp_acmp_lsn_admit` | current | run 18 checks, as stated |
| [`tb/maap/README.md`](../tb/maap/README.md) | the MAAP engine against IEEE 1722-2016 Annex B | verification | `KL_pp_maap` | current | run 196 checks |
| [`tb/name_state/README.md`](../tb/name_state/README.md) | the complete saved-name inventory through real frames, port and reset | verification | the D3 writer's name records | current | run 1,236 checks, as stated |
| [`tb/nvm_port/README.md`](../tb/nvm_port/README.md) | the class F NVM port, its deadline, resets and device models | verification | `KL_pp_nvm_port` | current | run 393 checks in each of three builds, as stated; its figures gate (`make -C tb/nvm_port figures`, CI) holds the rest |
| [`tb/originator/README.md`](../tb/originator/README.md) | the originator and inflight table | verification | `KL_pp_originator` | current | run 107 checks, as stated |
| [`tb/pp_top/README.md`](../tb/pp_top/README.md) | the processor top end to end, section by section, with its mutation campaigns | verification | `protocol_processor_top` | current | sweep checks; not re-run here (run time); one clause citation corrected |
| [`tb/prng/README.md`](../tb/prng/README.md) | the PRNG | verification | `KL_pp_prng` | current | run 76 checks; headline corrected from 57, mutation record dated |
| [`tb/release_merge/README.md`](../tb/release_merge/README.md) | the TX slot release merge | verification | `KL_pp_release_merge` | current | run 18 checks |
| [`tb/resp_buf/README.md`](../tb/resp_buf/README.md) | the AECP response buffer in main memory | verification | `KL_aecp_resp_buf` | current | run 64 checks, as stated; one clause citation corrected |
| [`tb/rx_slots/README.md`](../tb/rx_slots/README.md) | the RX slot pool | verification | `KL_pp_rx_slots` | current | run 130 checks, as stated |
| [`tb/rx_validator/README.md`](../tb/rx_validator/README.md) | the RX validator | verification | `KL_pp_rx_validator` | current | run 555 checks, as stated |
| [`tb/scoreboard/README.md`](../tb/scoreboard/README.md) | the admission scoreboard against F03.7 | verification | `KL_pp_scoreboard` | current | run 3,705 checks, as stated |
| [`tb/side_port/README.md`](../tb/side_port/README.md) | the management side port and trace ring | verification | `KL_pp_side_port`, `KL_pp_trace_ring` | current | run 368 checks, as stated |
| [`tb/srp_admission/README.md`](../tb/srp_admission/README.md) | SRP admission freshness across shapes | verification | `KL_srp_admission` | current | run 991,231 checks at eight sources |
| [`tb/srp_decoder/README.md`](../tb/srp_decoder/README.md) | the MRPDU vector decoder | verification | `KL_srp_decoder` | current | run 190 checks, as stated |
| [`tb/srp_encoder/README.md`](../tb/srp_encoder/README.md) | the MRPDU encoder with the Domain and VLAN machines | verification | `KL_srp_encoder`, `KL_srp_domain`, `KL_srp_vlan` | current | run 593 checks, as stated |
| [`tb/srp_stream_fsms/README.md`](../tb/srp_stream_fsms/README.md) | the per-stream talker and listener machines | verification | `KL_srp_talker_fsm`, `KL_srp_listener_fsm` | current | run 1,435 checks, as stated |
| [`tb/srp_top/README.md`](../tb/srp_top/README.md) | the assembled SRP engine end to end | verification | `KL_srp_top` | current | sweep checks; not re-run here (run time) |
| [`tb/timer_map/README.md`](../tb/timer_map/README.md) | the F08.4 slot map and the 02 §5 event-router map | verification | `pp_pkg::pp_timer_map()`, `pp_pkg::pp_evr_map()` | current | run 1,360 checks; headline corrected from 1,187 |
| [`tb/timer_service/README.md`](../tb/timer_service/README.md) | the timer service | verification | `KL_pp_timer_service` | current | run 48 checks, as stated |
| [`tb/tx_arbiter/README.md`](../tb/tx_arbiter/README.md) | the TX arbiter | verification | `KL_pp_tx_arbiter` | current | run 67 checks, as stated |
| [`tb/tx_slots/README.md`](../tb/tx_slots/README.md) | the TX slot pool | verification | `KL_pp_tx_slots` | current | run 95 checks, as stated |
| [`tb/ucpu/README.md`](../tb/ucpu/README.md) | the µCPU executes the 06 §8 µISA | verification | `KL_aecp_ucpu` | current | run 447 checks, as stated |
