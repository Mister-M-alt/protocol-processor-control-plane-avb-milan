<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Glossary

**In short.** The terms this repository's pages use without explaining them, each with a
one-line meaning and the place that defines it. A definition here never carries a value:
timing values live in [F08.1](architecture/08_timing.md#fig-08-constants), parameter
values in [F01.5](architecture/01_overview.md#fig-01-params), and status signal names in
[F02.10](architecture/02_interfaces.md#fig-02-statusdict). Where Milan and IEEE 1722.1 name
the same thing differently, Milan's name is used ([docs/README.md §4](README.md#4-spec-citation-and-terminology)).

## Standards and protocols

| Term | Meaning | Defined in |
|---|---|---|
| ATDECC | audio/video bridging and time-sensitive networking discovery, enumeration, connection management and control: the protocol family of IEEE 1722.1-2021, which this processor implements as one entity | IEEE 1722.1-2021 §1 |
| AVB | Audio Video Bridging: the IEEE 802.1 time-sensitive networking set (802.1AS time, 802.1Q stream reservation and shaping) this control plane runs on | IEEE 802.1Q-2014, IEEE 802.1AS-2011 |
| Milan | the AVnu Milan Specification, Consolidated v1.2: a profile of ATDECC and AVB for professional audio. Where it differs from IEEE 1722.1, Milan wins (Milan §5.5.2.1); every such point is a delta, `Δn` | [01 F01.4](architecture/01_overview.md#fig-01-deltas) |
| PAAD | Professional Audio AVB Device, the Milan device class. This processor is the control plane of a non-redundant PAAD; its ATDECC entity is the PAAD-AE | Milan §1 |
| ADP | ATDECC Discovery Protocol: entity advertisement (ENTITY_AVAILABLE, ENTITY_DEPARTING) and discovery (ENTITY_DISCOVER) | IEEE 1722.1-2021 §6; Milan §5.6; [04](architecture/04_adp_engine.md) |
| ACMP | ATDECC Connection Management Protocol: binding a listener's Stream Input to a talker's Stream Output | IEEE 1722.1-2021 §8; Milan §5.5; [05](architecture/05_acmp_engine.md) |
| AECP | ATDECC Enumeration and Control Protocol: commands and responses between a controller and an entity | IEEE 1722.1-2021 §9; [06](architecture/06_aecp_engine.md) |
| AEM | ATDECC Entity Model: the descriptor tree and the AECP command set that reads and changes it | IEEE 1722.1-2021 §7 |
| MVU | Milan Vendor Unique: the AECP vendor-unique commands Milan defines (GET_MILAN_INFO and others) | Milan §5.4.3; [06 §6.9](architecture/06_aecp_engine.md#69-mvu-commands) |
| ADPDU, ACMPDU, AECPDU | the protocol data unit of ADP, ACMP and AECP | IEEE 1722.1-2021 §6.2, §8.2, §9.2 |
| cdl | `control_data_length`, the AVTP control header's length field. The PDU is cdl + 12 octets; padding to the Ethernet minimum is not counted | [docs/README.md §4](README.md#4-spec-citation-and-terminology); IEEE 1722-2016 §4.4.5.4 |
| SRP | Stream Reservation Protocol: MSRP plus MVRP, which reserve bandwidth and a VLAN along a stream's path | IEEE 802.1Q-2014 §35; Milan §4.2.7, §4.3, §4.4; [10](architecture/10_srp_engine.md) |
| MSRP, MVRP, MRP | Multiple Stream Registration Protocol, Multiple VLAN Registration Protocol, and the Multiple Registration Protocol both are built on. An MRPDU is one MRP frame | IEEE 802.1Q-2014 §10, §11, §35 |
| MAAP | MAC Address Acquisition Protocol: claims a block of multicast destination addresses for an entity's streams | IEEE 1722-2016 Annex B; [11](architecture/11_maap_engine.md) |
| gPTP | generalized Precision Time Protocol, the 802.1AS time base. Outside this processor: it reads gPTP state on level and read faces | IEEE 802.1AS-2011; [02 §4.3](architecture/02_interfaces.md#43-gptp--time-sync-data) |
| AVTP | Audio Video Transport Protocol, the stream and control frame format. The streaming datapath is outside this processor | IEEE 1722-2016; [02 §4.4](architecture/02_interfaces.md#44-avtp--streaming-engine-control) |

## The entity and its peers

| Term | Meaning | Defined in |
|---|---|---|
| entity, `entity_id` (EID) | the ATDECC end point this processor implements, and its 64-bit identifier | IEEE 1722.1-2021 §3.1, §6.2.2.7 |
| controller | a remote ATDECC entity that sends AECP and ACMP commands to this one, for example a control application | IEEE 1722.1-2021 §3.1 |
| talker, listener | an entity that sources a stream, and one that sinks it | IEEE 1722.1-2021 §3.1 |
| Stream Input (sink), Stream Output (source) | the descriptors of a listener's and a talker's stream ends. These pages say sink and source, sized by `P-N-STREAM-IN` and `P-N-STREAM-OUT` | IEEE 1722.1-2021 §7.2.6; [07 §3.1](architecture/07_memory_maps.md#31-descriptor-tree) |
| bind, unbind, probe | Milan's names for IEEE's CONNECT_RX, DISCONNECT_RX and CONNECT_TX: a listener binds to a talker, then probes it until the stream is settled | Milan §5.5.2.2; [05 §6](architecture/05_acmp_engine.md#6-listener-behavior--the-four-view-package) |
| descriptor | one node of the entity model (ENTITY, CONFIGURATION, STREAM_INPUT, AVB_INTERFACE and so on), read by READ_DESCRIPTOR | IEEE 1722.1-2021 §7.2 |
| registry | the list of controllers registered for unsolicited notifications, keyed by Milan's {EID, MAC, port} tuple | Milan §5.3.4.2; [06 §7](architecture/06_aecp_engine.md#7-registry-notifications-liveness-identify) |
| unsolicited notification | an AECP response the entity sends without a command, to every registered controller, when its state changes | IEEE 1722.1-2021 §7.5; Milan §5.4.5 |

## Inside the processor

| Term | Meaning | Defined in |
|---|---|---|
| packet engine | the shared RX and TX datapath every protocol engine sits on: validation, slots, dispatch, scoreboard, originator and TX arbiter | [03](architecture/03_packet_engine.md) |
| RX slot, TX slot | a fixed-size frame buffer the RX side fills and an engine reads in place, and one an engine fills for the TX arbiter to send | [03 §3](architecture/03_packet_engine.md#3-rx-pipeline), [03 §7](architecture/03_packet_engine.md#7-response-building-and-buffers) |
| normalized transaction | the one record shape every received command becomes before dispatch, whatever its protocol | [03 §4](architecture/03_packet_engine.md#4-normalized-transaction) |
| scoreboard, hazard class | the interlock that orders transactions which touch the same state; each command has a class and a key | [03 §6](architecture/03_packet_engine.md#fig-03-hazards) |
| originator | the path for frames the entity starts itself (probes, CONTROLLER_AVAILABLE checks), with its table of commands waiting for a response | [03 §5](architecture/03_packet_engine.md#5-origins-originator-and-event-router) |
| TX arbiter, TX lane | the block that merges every engine's frames onto the one MAC transmit face; each producer has its own request lane | [03 §8](architecture/03_packet_engine.md#8-tx-arbitration) |
| event router | the fan-out of the integrator's strobes and the engines' events to the state machines that consume them | [03 §5](architecture/03_packet_engine.md#5-origins-originator-and-event-router) |
| timer service, deadline RAM | the one shared timer: a RAM of armed deadlines swept every millisecond, each slot owned by one engine instance | [08 §3](architecture/08_timing.md#3-timer-hardware), [08 §5](architecture/08_timing.md#5-timer-allocation-and-sizing) |
| µCPU, µcode, µPC | the AECP engine's microsequencer, its microprogram ROM (generated by `hdl/aecp/ucode/gen_ucode.py`), and its program counter | [06 §8](architecture/06_aecp_engine.md#8-µcode-architecture) |
| listener transition ROM | the ROM the ACMP listener walks instead of hand-written state arcs, generated by `hdl/acmp/rom/gen_ltn_rom.py` | [05 F05.3](architecture/05_acmp_engine.md#fig-05-listener-matrix) |
| descriptor image | the packed entity model the integrator places in main memory, which the AECP engine reads through `desc_mem_*` | [07 §3.3](architecture/07_memory_maps.md#33-static-image) |
| dynamic overlay | the processor-held state that overrides fields of the descriptor image (names, current format, sampling rate and so on) | [07 §3.4](architecture/07_memory_maps.md#34-dynamic-overlay) |
| binding manager, D3 writer | the two persistence producers: the binding manager saves sink bindings, the D3 writer saves the scalar and name records | [07 §5](architecture/07_memory_maps.md#5-persistence) |
| restore walk; COMPLETE, DEFAULTS, CLOSED | the boot-time read-back of the saved records, and the three ways the D3 walk ends | [07 §5.3](architecture/07_memory_maps.md#fig-07-nvmflow) |

## The faces the integrator wires

| Term | Meaning | Defined in |
|---|---|---|
| integrator | whoever instantiates `protocol_processor_top` in an SoC and wires its faces | [integrator guide](guides/integrator.md) |
| consumer, parent, reference platform | the repository that consumes this one as a git submodule: [milan-fpga](https://github.com/kebag-logic/milan-fpga) on the Artix-7 xc7a100t | [`hdl/README.md`](../hdl/README.md) |
| interface classes A to F | the six kinds of external contract: A packet streaming, B request and response APIs, C events, D level status, E the management side port, F the NVM port | [02 §1](architecture/02_interfaces.md#1-interface-taxonomy) |
| class-D level | a status wire held at its current value every clock (not a strobe), listed in F02.10 | [02 §6](architecture/02_interfaces.md#6-class-d--level-status-dictionary) |
| side port | the class E management face: image load, debug snapshot, trace ring and NVM backing, for a soft CPU or a test harness | [02 §7](architecture/02_interfaces.md#7-class-e--management-side-port); [operator guide §4](guides/operator.md#4-the-diagnostic-side-port) |
| NVM port | the class F face through which both persistence producers read and write their records | [02 §8](architecture/02_interfaces.md#8-class-f--nvm-port) |
| tie-off | an optional face left unconnected at a documented constant; the guide states what the processor then does | [integrator guide §7](guides/integrator.md#7-the-optional-faces-and-what-a-tie-off-does) |
| D3 contract; DR2a, DR2c, DR3a | the consumer's saved-state contract (`docs/design/SAVED_STATE_MATERIALIZATION.md` in milan-fpga), and the rulings in its §15.1 that fix the persistence times | [08 §2](architecture/08_timing.md#sec-08-nvm) |

## Identifiers and evidence

| Term | Meaning | Defined in |
|---|---|---|
| `F<doc>.<n>` | a figure or table, numbered by its host page (F08.1 is page 08's first) | [docs/README.md §2](README.md#2-identifier-registries) |
| `P-` IDs, `T-` IDs | a synthesis-time parameter (row of F01.5) and a timing constant (row of F08.1). Other pages cite the ID, never the value | [docs/README.md §2](README.md#2-identifier-registries) |
| `REQ-`, `GAP-`, `Δn` | a requirement row of the compliance matrix, a review finding, and a Milan-over-IEEE delta | [docs/README.md §2](README.md#2-identifier-registries) |
| suite | one self-checking Verilator testbench under `tb/<suite>/`, with its own README; `scripts/run_suites.sh` runs them all | [09 §8](architecture/09_verification.md#8-the-suites-that-exist-today) |
| golden | a suite run with no change planted, which must pass before any control is graded | [`tb/pp_top/README.md`](../tb/pp_top/README.md) |
| control, arm, KILLED | a deliberate defect planted in a scratch copy of the tree (an arm, or control), and the verdict when the named checks fail as they must. A record of them is how a suite shows it can fail | [HDL engineer guide §6](guides/hdl-engineer.md#6-running-the-testbenches) |
| time compression | the `TIM_DIV_US_P` and `TIM_DIV_MS_P` prescaler overrides that make a simulated millisecond cost a hundred clocks, so long timers run in a suite | [HDL engineer guide §7](guides/hdl-engineer.md#7-testbench-conventions-worth-knowing-before-you-write-one) |
