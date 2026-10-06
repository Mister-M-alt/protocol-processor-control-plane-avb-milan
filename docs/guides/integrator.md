<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# Integrator guide — dropping this processor into an SoC

You have a fabric with a MAC, a memory system and probably a soft CPU, and you want a
Milan control plane in it. This page is the contract:
[`hdl/top/protocol_processor_top.sv`](../../hdl/top/protocol_processor_top.sv) is the one
instantiation boundary, and everything below describes it.

**Read the picture first:**
**[`../diagrams/21-integration-faces.svg`](../diagrams/21-integration-faces.svg)** is the
whole contract on one page, including what happens when you do not connect something.
For what is inside the box, see
[`../diagrams/20-rtl-dataflow.svg`](../diagrams/20-rtl-dataflow.svg).

> The port and parameter names on this page describe the RTL; section 2 links to
> their authoritative homes instead of copying defaults. Where the architecture
> documents describe an older interface, the RTL defines the implemented interface and the
> divergence is listed in the
> [HDL engineer guide](hdl-engineer.md#9-where-the-specification-and-the-tree-still-disagree).

---

## 1. Clocking and reset

There is exactly one clock and one reset.

| Port | Contract |
|---|---|
| `clk_i` | the single core clock. `CLK_HZ_P` tells the timer prescaler what it is. |
| `rst_n` | **synchronous**, active low. It needs a running clock to take effect. |

**No clock-domain crossing lives inside this processor but one.** Every crossing is yours:

- the MAC boundaries — put your asynchronous FIFOs outside, and present the byte streams
  below in the `clk_i` domain; the RX one hands over whole, FCS-good frames only
  ([section 3](#rx-frame-atomic));
- `link_up_i` and `gm_change_i` — level and strobe inputs from other domains. Give them a
  two-flop synchroniser before they arrive;
- the one exception is `identify_button_i` (section 6): with `EN_IDENTIFY_NOTIF_P` = 1 the
  processor passes it through its own two-flop synchroniser, so it may come straight
  from a pin. Debouncing it is still yours, and so is the timing constraint: the two
  flops (`u_notify.gen_ident.btn_q1_r`, `btn_q2_r`) carry `ASYNC_REG`, and the path from
  the pin to `btn_q1_r` is an asynchronous input to declare as such (a false path or a
  max-delay exception);
- the class-D status outputs are combinational reads of `clk_i`-domain registers. A
  consumer in another domain owns its own synchroniser.

[`KL_pp_side_port`](../../hdl/packet_engine/KL_pp_side_port.sv) is documented as being
able to run from a bridged management clock; the top instantiates it on `clk_i`.

The talker's automatic destination-address retry rounds require a running
`now_ms_i` timebase. After a refused attempt, a stalled timebase prevents further
attempts in that round, including probe-triggered requests. See
[the retry bound](../architecture/05_acmp_engine.md#6bis-talker-side-stateless-responder).

---

## 2. Parameters — the shape is fixed when you build the bitstream

<a id="integration-parameters"></a>

This is the **complete inventory of overridable parameters** on
[`protocol_processor_top`](../../hdl/top/protocol_processor_top.sv), in declaration
order. Nothing here is writable at runtime. Derived `localparam` declarations in
the header are internal calculations, not integration overrides.

The top's parameter declarations give the exact implemented default expressions.
The table below names each parameter's documented owner: architectural parameter
values and constraints stay in [F01.5](../architecture/01_overview.md#fig-01-params),
protocol timing values stay in [F08.1](../architecture/08_timing.md#fig-08-constants),
and implementation settings without a master-table entry stay in the top's RTL
declaration and banner. These are references, not another table of values
([single-source rule](../README.md#2-identifier-registries)). In particular, the
stream counts in F01.5 are product choices; the top supplies implementation defaults.

| Parameter | Authoritative home / documented owner | What it sets |
|---|---|---|
| `N_AVB_IF_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-N-AVB-INTERFACES` | AVB interfaces, the redundancy seam: 1 (the default, and every shipping build) or 2. At 2 each interface has its own advertise machine, every received frame names its interface on `rx_if_index_i` (section 3), and a controller's registration is kept per interface. The trunks, the class-D levels, the SRP engine and the side-port snapshot stay one per top; [REQ-SCP-003](../00_MILAN_COMPLIANCE_REVIEW.md#fig-00-matrix) lists everything that is and is not keyed |
| `N_STREAM_IN_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-N-STREAM-IN` | Stream Inputs: sinks, listener machines, per-sink records |
| `N_STREAM_OUT_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-N-STREAM-OUT` | Stream Outputs: sources, talker gates, SRP declarations |
| `N_AUDIO_UNIT_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-N-AUDIO-UNITS` | Audio Unit rows in the AECP dynamic-state store, including sampling-rate settings; match the entity model |
| `N_CLK_DOMAIN_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-N-CLOCK-DOMAINS` | Clock Domain rows in the AECP dynamic-state store, including clock-source selections; match the entity model |
| `N_CONTROL_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [AECP dynamic-state store](../../hdl/aecp/KL_aecp_dyn_state.sv) | IDENTIFY CONTROL rows and identify settings; match the entity model. This counts CONTROL descriptors, not registered controllers (`P-N-CONTROLLERS`). |
| `RX_SLOTS_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-RX-SLOTS` | Number of RX payload slots |
| `RX_SLOT_BYTES_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-RX-SLOT-BYTES` | Capacity of each RX payload slot |
| `TX_STD_SLOTS_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-TX-STD-SLOTS` | Standard TX slots; the oversize slot is additional |
| `TX_OVERSIZE_BYTES_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-TX-OVERSIZE-BYTES` | Oversize TX slot capacity for responses Milan permits beyond the normal cap |
| `CLK_HZ_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-CLK-HZ` | Core clock frequency used to derive the timer prescaler, the restore deadline and the record-write backoff defaults |
| `TIM_DIV_US_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [F08.2 timebase](../architecture/08_timing.md#fig-08-timerhw) | Core-clock divider for the microsecond tick. **Simulation time compression only** when overriding the clock-derived expression. |
| `TIM_DIV_MS_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [F08.2 timebase](../architecture/08_timing.md#fig-08-timerhw) | Microsecond-tick divider for the millisecond tick. **Simulation time compression only** when overriding. |
| `TROM_HEX_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [listener ROM generator](../../hdl/acmp/rom/gen_ltn_rom.py) | File path to the ACMP listener transition-ROM image |
| `UCODE_HEX_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [AECP µcode generator](../../hdl/aecp/ucode/gen_ucode.py) | File path to the AECP µcode ROM image |
| `DESC_BASE_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [07 §3.3.1 memory contract](../architecture/07_memory_maps.md#sec-desc-memory) | Base of the descriptor image in **your** memory |
| `DESC_LINE_BYTES_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [07 §3.3.1 descriptor store](../architecture/07_memory_maps.md#sec-desc-memory) | On-chip line capacity for one located descriptor; also sizes the response-buffer reservation, `16 + DESC_LINE_BYTES_P` bytes (section 5). Legal: a multiple of 8 from 576 to 1008 (`P-DESC-LINE-BYTES`, [F01.5](../architecture/01_overview.md#fig-01-params)); elaboration refuses any other line with a message naming `DESC_LINE_BYTES_P`. Below 576 the reservation could not carry a whole 71-record GET_AUDIO_MAP page (`P-MAP-SUBSET-CH-MAX`, [06 §3](../architecture/06_aecp_engine.md#3-pdu-handling)); above 1008 it would pass the 1024 bytes the response cursor addresses |
| `DESC_IDX_ENTRIES_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [07 §3.3.1 descriptor store](../architecture/07_memory_maps.md#sec-desc-memory) | Cached index-map capacity, per (configuration, descriptor type) |
| `DESC_NAME_ENTRIES_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [07 §3.3.1 descriptor store](../architecture/07_memory_maps.md#sec-desc-memory) | Name-table capacity on chip; size from the generated image's `n_names` within the store's supported limits. It is also the D3 writer's name-record count (`0x80` + ordinal, [07 §5.2](../architecture/07_memory_maps.md#fig-07-nvmrec)), so at most 128, the size of that id block, and your backend must back that many name records |
| `DESC_MEM_TMO_CYC_P` | [Top declaration and bindings](../../hdl/top/protocol_processor_top.sv); [AECP engine](../../hdl/aecp/KL_aecp_engine.sv) | Watchdog budget in core clocks for descriptor and response memory, AECP gather waits, and the listener's stream-command handshake |
| `NVM_RS_TMO_CYC_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-NVM-RS-TMO-CYC`; [F08.1](../architecture/08_timing.md#fig-08-constants), `T-NVM-RS-DEADLINE` | Per-wait no-progress deadline of both boot restore walks (the binding walk's reads; every wait of the D3 walk, the roll-back's debt wait included), in core clocks; size above the slowest record read on the NVM device face and the descriptor image walk. The default is the ratified 20 ms, ceil(`CLK_HZ_P` / 50) ([08 §2](../architecture/08_timing.md#sec-08-nvm)) |
| `NVM_RS_AGG_CYC_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-NVM-RS-AGG-CYC`; [F08.1](../architecture/08_timing.md#fig-08-constants), `T-NVM-RS-AGGREGATE` | Aggregate restore deadline in core clocks from `restore_go_i`, both walks and the roll-back included, however promptly each single wait is answered. At it the phase the restore is in ends as a stalled wait would, and a provable image is never closed: a binding walk still reading fails whole and the D3 walk then proves the image and ends DEFAULTS; in the D3 walk's first pass DEFAULTS; in its second pass a roll-back to DEFAULTS; CLOSED only if the image cannot be proven, if the bound falls inside a roll-back another fault had started, or if the roll-back the bound started cannot prove the image again (bring-up step 3). The default is the ratified 1,000 ms, `CLK_HZ_P` clocks; keep it derived from your clock. Shortened overrides are for verification. |
| `NVM_RETRY_BACKOFF_CYC_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-NVM-RETRY-BACKOFF-CYC`; [F08.1](../architecture/08_timing.md#fig-08-constants), `T-NVM-RETRY-BACKOFF` | Wait after a failed record write before the next attempt, in core clocks, for the binding manager and the D3 writer alike; the default is ceil(`CLK_HZ_P` / 2) = 500 ms. Each record gets three **attempts** in all: the first write and two **retries** (`RETRY_MAX_P` = 2 inside each producer, not a top parameter), then the reset-sticky `nvm_alarm_o`. Shortened overrides are for verification. |
| `NVM_MEM_TMO_CYC_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-NVM-MEM-TMO-CYC`; [F08.1](../architecture/08_timing.md#fig-08-constants), `T-NVM-PORT-DEADLINE` | The NVM port's device-face deadline, in core clocks: your device face must present each event it owes the port (a grant, a byte, the terminal of a command whose data phase is over) within this many clocks of the previous one, not counting clocks in which the port or your manager holds the operation (those pause the count; they never restart it), or the operation ends with `err`, cause DEADLINE, and the port waits for the abandoned command's own end, or a reset, before it issues another ([02 §8](../architecture/02_interfaces.md#sec-02-nvm-deadline)). The default is 1,000 ms, `CLK_HZ_P` clocks, derived as 20 × the reference backend's longest legal stall (its 50 ms grant hold); keep it derived from your clock, and raise it if one command of your device can stall longer (a direct-flash erase of a large region, say). Legal 1 to 2^31 - 1; the port refuses any other at elaboration. |
| `REG_TL_TIMEOUT_MS_P` | [F08.1](../architecture/08_timing.md#fig-08-constants), `T-NOTIF-TIMELIMITED`; [top declaration](../../hdl/top/protocol_processor_top.sv) | TIME_LIMITED registration expiry in the AECP notification registry, in milliseconds of the possibly compressed timebase. Shortened overrides are for verification. |
| `LOCK_TIMEOUT_MS_P` | [F08.1](../architecture/08_timing.md#fig-08-constants), `T-LOCK-UNLOCK`; [top declaration](../../hdl/top/protocol_processor_top.sv) | ENTITY lock auto-unlock deadline in the AECP notification block, in milliseconds of the possibly compressed timebase. Shortened overrides are for verification. |
| `RESP_BASE_P` | [Top declaration and banner](../../hdl/top/protocol_processor_top.sv); [07 §3.3.2 response-buffer contract](../architecture/07_memory_maps.md#sec-resp-memory) | Base of the AECP response buffer in **your** memory |
| `SRP_DOM_DEF_VID_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-SRP-DOM-DEF-VID`; [F10.2 Domain FSM](../architecture/10_srp_engine.md#fig-10-domsm) | Class A Domain VID declared at startup and link-up, and restored on link-down. A bridge's differing Class A Domain is adopted at runtime. **Keep the F01.5 Milan value in a product build**; alternatives are verification fixtures. |
| `EN_IDENTIFY_NOTIF_P` | [F01.5](../architecture/01_overview.md#fig-01-params), `P-EN-IDENTIFY-NOTIFICATION`; [06 §7, F06.16](../architecture/06_aecp_engine.md#fig-06-identify) | 1 builds the identify sequencer behind `identify_button_i` (section 6): a press sends IDENTIFY_NOTIFICATION three times, 150 ms apart, to 91-E0-F0-01-00-01, and again every second while held (IEEE 1722.1-2021 §7.5.1; Milan §5.4.5.4). 0 builds none of it and never reads the button. Set it to 1 only when your product gives the user a way to ask the entity to report itself. |

Three traps worth stating plainly:

1. **The ROM parameters are file paths, and a relative name resolves against the tool's
   run directory** — not against the source file. That is the entire reason they are
   parameters. Hand over absolute paths.
2. **An over-large stream shape stops the build.** The top carries elaboration guards
   over the timer-slot map and the owner-tag space; exceeding either raises `$error`
   rather than silently aliasing a timer slot. A too-large shape is a build failure, not
   a field fault.
3. **A one-stream shape is supported.** Index widths are clamped so a shape of one does
   not declare a negative-width vector.

[`scripts/check-integrator-params.py`](../../scripts/check-integrator-params.py)
compares this table and diagram 21's parameter inventory with the top's declarations.
It runs in the CI documentation gates and in `make check`.

---

## 3. The MAC faces

One frame stream in, one frame stream out, both **byte-wide**, both in the `clk_i`
domain. Byte 0 of each frame is the first destination-address octet.

| Direction | Ports | Backpressure |
|---|---|---|
| RX | `rx_valid_i`, `rx_data_i[7:0]`, `rx_last_i`; `rx_if_index_i[1:0]`, the frame's AVB interface, read with its last byte and only when `N_AVB_IF_P` is 2 (leave it unconnected at 1: it defaults to 0) | **none.** There is no `rx_ready`. The RX side cannot be stalled — feed it from a FIFO that can absorb a frame. |
| TX | `tx_valid_o`, `tx_sof_o`, `tx_data_o[7:0]`, `tx_eof_o`, `tx_ready_i` | `tx_ready_i` is real. A granted frame streams from `tx_sof_o` to `tx_eof_o` with no preemption, so holding `tx_ready_i` low stalls that frame in place; it never truncates. |

<a id="rx-frame-atomic"></a>**Your RX FIFO must deliver only complete, FCS-good frames.** The
byte face has no `err` and no abort input, so nothing can poison a frame once its first
byte is in: the processor parses every frame it is given as a good one. The dual-clock
FIFO between your MAC and this face is yours, and the handoff must be frame-atomic
([02 §2 rule 2](../architecture/02_interfaces.md#2-clocking-reset-cdc)):

- hold each frame whole, and start it on `rx_valid_i` only once its last byte and your
  MAC's FCS verdict are in the FIFO;
- drop every frame that failed its FCS, was aborted by the MAC, or was cut short by a
  full FIFO, with every byte of it;
- never present part of a frame: a truncated frame that still carries a whole PDU is
  parsed as one.

This is a control-plane trunk. It sees the frames a Milan control plane needs — the
ATDECC multicast and this station's own unicast, plus the two MRP group addresses — and
it emits control frames only. Streaming data never passes through here.

---

## 4. The two main-memory masters

The entity model and the AECP response buffer do **not** live on chip. This is the single
most important thing to plan for. On the reference part the generated entity model was
already tens of kilobytes and the die's block RAM was effectively fully spoken for, and
the response buffer as fabric state was the flop group the placer could not pack.

Both masters are **vendor-neutral by contract** — this repository does not know what is
behind them. You bridge them to whatever you have.

<a id="sec-desc-memory"></a>
### 4.1 `desc_mem_*` — read only

| Port | |
|---|---|
| out | `desc_mem_req_valid_o`, `desc_mem_req_addr_o[31:0]`, `desc_mem_req_beats_o[8:0]`, `desc_mem_rsp_ready_o` |
| in | `desc_mem_req_ready_i`, `desc_mem_rsp_valid_i`, `desc_mem_rsp_data_i[63:0]`, `desc_mem_rsp_last_i`, `desc_mem_rsp_err_i` |

One outstanding request. Responses arrive **in order**; `rsp_last` marks the final beat.
Addresses are byte addresses, 8-byte aligned. A beat carries its lowest byte address in
bits [63:56] — IEEE 1722.1 wire order, so a descriptor byte can be handed to the µCPU
unswapped.

`KL_aecp_desc_mem_guard` sits between the AECP engine's descriptor-store master
and this face. It remembers an accepted burst until a response beat with `last`
or `err` is consumed, even when the store has timed out. While that debt exists,
the next request is held on both sides of the guard; responses pass through
unchanged and the store discards beats it no longer awaits. The first response
must arrive after the request-acceptance cycle. An error terminates the burst:
the bridge must not emit further beats for that request.

The guard's own module port `debt_o` is the **D3 interface**
([memory contract](../architecture/07_memory_maps.md#sec-desc-memory)). It is not
exposed by `protocol_processor_top`: the D3 writer consumes it inside the top
(`d3_desc_debt_i`), so the top-level interface carries no debt port.
Drive `protocol_processor_top.rst_n`, its synchronous active-low reset, only
from a **hard reset** that also flushes the descriptor-memory path, including
any CDC queues. Never drive it from an entity disable, store-only reset, or
future rollback reset: no pre-reset response may arrive after debt is forgotten.

If a burst never terminates, the guard keeps requests held; the store's watchdog
still answers each locate with an error in bounded time. The existing immediate
error on the first locate following a fetch-response timeout is unchanged.
The D3 writer holds its roll-back of both AECP stores in reset while `debt_o` is
set, and ends the restore CLOSED if the debt outlasts `NVM_RS_TMO_CYC_P` or the
aggregate bound falls while it waits (bring-up step 3). The guard itself
implements no roll-back and releases no owner.

### 4.2 `resp_mem_*` — read and write

| Port | |
|---|---|
| out | `resp_mem_req_valid_o`, `resp_mem_req_addr_o[31:0]`, `resp_mem_req_beats_o[8:0]`, `resp_mem_rsp_ready_o`, `resp_mem_wr_valid_o`, `resp_mem_wr_addr_o[31:0]`, `resp_mem_wr_data_o[63:0]`, `resp_mem_wr_strb_o[7:0]` |
| in | `resp_mem_req_ready_i`, `resp_mem_rsp_valid_i`, `resp_mem_rsp_data_i[63:0]`, `resp_mem_rsp_last_i`, `resp_mem_rsp_err_i`, `resp_mem_wr_ready_i`, `resp_mem_wr_done_i`, `resp_mem_wr_err_i` |

Same read contract, with **one difference that matters**: here `resp_mem_rsp_ready_o` is
real backpressure. The buffer takes a beat only once the frame builder has consumed the
previous one, so your bridge shall hold a beat until it is taken.

The write channel is one outstanding single-beat write. `wr_data` is a 64-bit lane in the
same big-endian order as a read beat — byte `addr + n` is bits `[63-8n -: 8]`. `wr_strb`
bit *n* enables byte *n*, and a byte whose strobe is 0 **shall not be modified**.
`wr_done_i` is a one-cycle pulse when the write is committed; it may be the same cycle as
`wr_ready_i` for a posted bridge, or later for an acknowledged one. No further write is
issued until it arrives.

Ordering: a read request accepted after a write reported done shall observe that write.
Nothing else in this processor addresses the region, so no further rule is needed.

### 4.3 Why two masters and not one

Both are watchdog-bounded clients with one outstanding transaction each. Sharing a single
channel would mean an arbiter whose grant has to be released correctly on every watchdog
of both. Your memory system already arbitrates — let it.

---

## 5. What you must reserve in your memory map

| Region | Size | Who writes it |
|---|---|---|
| `DESC_BASE_P` | your descriptor image, sized by your entity model | **your software**, before `restore_go_i` (the restore judges saved values against it) and so before `entity_enable_i`. The processor only reads it. |
| `RESP_BASE_P` | `16 + DESC_LINE_BYTES_P` bytes (592 at the default line) | **the processor.** Nothing else may write here, and the processor writes nothing past it: its response buffer is exactly this reservation. |

Both are 8-byte aligned, and `RESP_BASE_P` must not overlap `DESC_BASE_P`.

The image opens with a magic, a layout version and a checksum. Until all three agree,
the store reports zero configurations. `READ_DESCRIPTOR` validates the requested
configuration before it locates a descriptor, so every request against an invalid or
unloaded image answers `BAD_ARGUMENTS`, never a garbage descriptor on the wire.
Uninitialised memory is not a recognisable zero, which is exactly why the header check
exists. A **late** load heals it: the next locate re-arms the header probe, so software
that loads the image after reset does not need a reset to recover.

Layout of the image itself, and its generator, are in
[`07_memory_maps.md` §3](../architecture/07_memory_maps.md).

The whole path, with its failure modes, is
[`../diagrams/22-aecp-descriptor-fetch.svg`](../diagrams/22-aecp-descriptor-fetch.svg).

---

## 6. Configuration and identity inputs

The identity, capability, SRP and talker-source groups are quasi-static: set
them before `entity_enable_i` and leave them alone. The two dynamic rows are
updated at runtime under the event contracts below.

| Group | Ports |
|---|---|
| Identity and model | `entity_id_i[63:0]`, `entity_model_id_i[63:0]`, `own_mac_i[47:0]`, `current_cfg_i[15:0]`, `identify_index_i[15:0]` |
| Advertised capability | `talker_sources_i[15:0]`, `talker_caps_i[15:0]`, `listener_sinks_i[15:0]`, `listener_caps_i[15:0]` |
| Dynamic gPTP state | `gm_change_i`, `gm_id_i[63:0]`, `gptp_domain_i[7:0]` |
| Level controls | `entity_enable_i`, `link_up_i` |
| Identification button | `identify_button_i` (read only with `EN_IDENTIFY_NOTIF_P` = 1) |
| SRP | `p2p_i`, `cfg_rank_i`, `cfg_acc_lat_ns_i[31:0]`, `port_rate_bps_i[31:0]`, `cfg_tspec_max_frame_i[15:0]` |
| Talker sources | `cfg_src_en_i`, `cfg_src_iface_i`, `cfg_stream_id_i` |

Drive `entity_model_id_i` with the ENTITY descriptor's `entity_model_id`: the
ADPDU and the descriptor carry the same field (IEEE 1722.1-2021 Table 7-2). Zero
and all-ones are invalid (Milan v1.2 §5.3.3.1, printed p. 25, and §5.6.2). A
static-model change requires a new model identity, subject to IEEE 1722.1 §6.2.2.8's
exclusions.

Drive `current_cfg_i` with the image's configuration, the ENTITY descriptor's
`current_configuration`. It is the ADPDU's `current_configuration_index` while
the dynamic overlay's configuration row is unset: from reset until a
controller's SET_CONFIGURATION writes the row or the boot restore writes a saved
configuration back into it
([07 §5.3](../architecture/07_memory_maps.md#fig-07-nvmflow)), and again after a
restore roll-back. While the row is written the processor advertises the overlay
(`aecp_cur_config_o`), the same value GET_CONFIGURATION answers, and does not
read `current_cfg_i`. No loopback of `aecp_cur_config_o` is needed. Every other
ADPDU field is independent of the configuration (Milan §5.6.2).

Drive `talker_sources_i` with the most STREAM_OUTPUT descriptors any supported
configuration holds, and `listener_sinks_i` with the most STREAM_INPUT descriptors.
The ENTITY descriptor's `talker_stream_sources` and `listener_stream_sinks` must
carry those same maxima (Milan §5.3.3.1, §5.6.2). Drive `identify_index_i` with the
primary IDENTIFY CONTROL index present in every configuration (Milan §5.3.3.10).

These are integrator obligations, not properties proved by ADP transport. The
descriptor packer checks the image side of each one, by default, with its
[model lint](../architecture/07_memory_maps.md#model-lint):

- It refuses an `entity_model_id` of zero or all-ones (L9), ENTITY stream counts
  that are not the maxima over every configuration (L11), and a model with no
  IDENTIFY CONTROL at one index in every configuration (L8).
- Its layout report prints the four values to drive:

  ```text
  ADP inputs this model requires (Milan v1.2 §5.3.3.1, §5.6.2):
    entity_model_id_i  0x020000FFFE00C801
    talker_sources_i   1
    listener_sinks_i   2
    identify_index_i   0
  ```

- Pass the values you drive and it refuses any that disagree with the image:
  `build(model, adp={"entity_model_id": ..., "talker_sources": ...,
  "listener_sinks": ..., "identify_index": ...})`, or `--adp-entity-model-id`,
  `--adp-talker-sources`, `--adp-listener-sinks` and `--adp-identify-index` on the
  command line.
- The report also prints the model digest (SHA-256, §6.2.2.8 exclusions zeroed).
  Record it beside your `entity_model_id` and pass the record as `model_ids` /
  `--model-ids`. A later change to the model's structure that keeps the id is then
  refused.

The processor drives none of these inputs itself. The
[descriptor ownership contract](../architecture/07_memory_maps.md#31-descriptor-tree)
identifies the parent shipping checks and what the processor's lint adds to them.

The three per-source vectors `cfg_src_en_i`, `cfg_src_iface_i` and
`cfg_stream_id_i` are **flat packed bit vectors**: index *s* occupies
`[W*s +: W]`. The same convention is used by every per-index status output.

`entity_enable_i` is the boot gate of Milan §5.6.1, as a **request**. The ADP engine's
effective enable is `entity_enable_i && restore_done_o`: the processor releases your
request only once both restore walks are done ([07 §5.3](../architecture/07_memory_maps.md#fig-07-nvmflow)),
and never after a CLOSED restore. While the effective enable is low the advertise machine
is held in DOWN and the entity is silent — because an entity must already be able to
answer commands before it announces itself. Deasserting the request later **is** the
shutdown: it emits ENTITY_DEPARTING and resets `available_index`. There is no separate
shutdown port. The side port's image-window write lock follows the request itself, not
the effective enable, so a readback of your control register shows what you asked for.

`identify_button_i` is IEEE 1722.1-2021 Figure 7-142's `identifyButtonPressed`: hold it
at 1 while the user wants the entity to report itself to the controllers (Milan
§5.4.5.4, the entity-to-controller direction; the IDENTIFY control is the other one and
does not use this pin). It is a level, taken through a two-flop synchroniser inside the
processor. **Debounce it yourself**: a bounce reads as a release and a new press, and
every new press after a release is answered with a burst. No press is lost, however
short: one made while a burst is still going out, or in the `T-IDENT-BURST` gap after
its third frame, is remembered, and its burst starts when that gap ends; a press made
while a burst is already owed adds none. A stalled `tx_ready_i` delays the frames
behind it and never bunches them: each frame is due `T-IDENT-BURST` after the previous
one's last byte left. With
`EN_IDENTIFY_NOTIF_P` = 0, the default, the pin is never read: tie it to `1'b0`. A burst
needs no registered controller and is not refused by a controller's lock; it goes to
the multicast address with the IDENTIFY control's index from `identify_index_i`.

`gm_change_i` is a one-cycle **ADP / GET_AVB_INFO** event, not a
`GET_AS_PATH` event. Raise it after atomically publishing a changed `gm_id_i`
or `gptp_domain_i`; both fields are advertised and both are returned by
`GET_AVB_INFO`. The path has its own publication edge, `gsi_asp_chg_i`. Raise
that strobe after atomically publishing any changed PathTrace sequence,
including entry 0 when a new grandmaster identity changes it. Consequently a
GM identity update normally raises both strobes, a domain-only update raises
only `gm_change_i`, and a tail-only PathTrace update raises only
`gsi_asp_chg_i`. The processor keeps the two events separate so a domain
change cannot claim that the path changed.

---

## 7. The optional faces, and what a tie-off does

Every one of these is watchdog-bounded. Leaving one unconnected degrades that function to
an honest answer on the wire; **it never wedges the control plane.** That is a design
property, not an accident, and it is regression-tested. It covers the faces, not the boot
controls: `restore_go_i` is not part of the NVM tie-off, and a boot that never pulses it
leaves the ACMP listener, AECP and ADP held until the next reset
([05 §5.1](../architecture/05_acmp_engine.md#sec-05-boot-admission)). And it has one
exception: the saved-state restore judges values against the descriptor image, so a
descriptor image it **cannot prove** (no image, a bad header, a memory that never
answers) ends the restore **CLOSED**: AECP commands and ADP stay held until reset, while
the ACMP listener keeps serving at its normal latency: one AECP command stays held in the
ingress and every further one is dropped at the slot gate and counted (snapshot word 37). Erased NVM behind a proven image is not a failure: the
restore ends on defaults and everything is served.

**Slow but live is bounded too.** A watchdog catches a face that stops answering, not
one that answers every request just inside it. For AECP that case is bounded by the
transaction deadline instead ([03 §6](../architecture/03_packet_engine.md) rule (e),
[08 §4](../architecture/08_timing.md#4-deadline-budgets)): a command still executing
`T-BUDGET-AECP-WC` after its reception is answered at its next instruction boundary with a
well-formed 60-byte `ENTITY_MISBEHAVING` (a command that is not an AEM command,
Milan Vendor Unique, ADDRESS_ACCESS or AV/C, with `NOT_IMPLEMENTED`, the command
echoed), inside `T-AECP-RESP`. A command that had already
changed state when the deadline passed answers for itself instead, so nothing is left
half-committed. Size your faces so a command's worst case stays well inside the budget:
`tb/pp_top` section TB measures the worst stimuli at the reference 143 clocks per memory
access ([08 §4](../architecture/08_timing.md#4-deadline-budgets)).

| Face | Ports | Tie it off and… |
|---|---|---|
| MAAP allocation | `maap_req_valid_o`, `maap_req_release_o`, `maap_req_src_o`, `maap_conflict_ack_o` / `maap_req_ready_i`, `maap_rsp_valid_i`, `maap_rsp_ok_i`, `maap_rsp_da_i[47:0]`, `maap_conflict_valid_i`, `maap_conflict_src_i` | **no source ever declares.** `acmp_declaring_o` is structurally 0 and PROBE_TX answers `TALKER_DEST_MAC_FAILED`. Commands are still answered normally. |
| Descriptor memory | `desc_mem_*` | the saved-state restore cannot prove the image and ends **CLOSED** (`restore_closed_o`, `rs_cause_o` 7): AECP dispatch and ADP stay held until reset, so no AECP command is answered. The ACMP listener keeps serving at its normal latency: at most one AECP command occupies the shared ingress, and every further one is dropped at the slot gate, counted in snapshot word 37 and never answered, so ACMP, ADP and MAAP keep the other `RX_SLOTS_P` − 1 slots. (Without the restore's hold, the failed header probe leaves zero configurations and every `READ_DESCRIPTOR` would answer `BAD_ARGUMENTS`; the store still behaves so.) |
| Response memory | `resp_mem_*` | every built response becomes a well-formed 60-byte `ENTITY_MISBEHAVING`, and a Milan Vendor Unique one `NOT_IMPLEMENTED` with the command echoed. |
| NVM device | `nvm_dev_*`, plus `restore_go_i`, `restore_busy_o`, `restore_done_o`, `restore_fail_o`, `restore_blank_o`, `restore_closed_o`, `restore_rb_o`, `rs_cause_o`, `restore_cause_o`, `nvm_alarm_o`, `nvm_unflushed_o`, `d3_unflushed_o` | no saved state survives a power cycle: neither bindings, the scalar settings nor the user names. **`restore_go_i` is not part of the tie-off:** pulse it on every boot. The ACMP listener serves nothing until the binding walk has ended ([05 §5.1](../architecture/05_acmp_engine.md#sec-05-boot-admission)), AECP nothing until the D3 walk has, and what the walks report depends on how you tie the face off. Tie it off **as erased media**: grant each READ, deliver the bytes it asks for as `0xFF`, then `done`. Both walks then end with `restore_done_o`, no `restore_fail_o` and `restore_blank_o`, the same done-without-fail as a successful restore, so publish `restore_blank_o` beside them and report not-successful when you know there is no media. A face that answers with `err`, or with `done` before the eight header bytes, is a failing device: the walks fail (`restore_fail_o`, a device error, with `restore_done_o` on defaults). A face that never answers fails each walk at `NVM_RS_TMO_CYC_P` (`restore_fail_o`, the deadline); the port then answers its abandoned request at `NVM_MEM_TMO_CYC_P` with `err`, cause DEADLINE, and every later change is given up with `nvm_alarm_o` after three attempts, each ended DEADLINE, its pending bit dropped. A command the face did accept stays owed: the port issues nothing over it until the face ends it or a reset ([07 §5.3](../architecture/07_memory_maps.md#fig-07-nvmflow), [02 §8](../architecture/02_interfaces.md#sec-02-nvm-deadline)). Nothing else changes **in this plane**. |
| Management side port | `host_*` | you lose all diagnostics. The plane still runs. |
| Counters | `ctr_req_o`, `ctr_desc_type_o[15:0]`, `ctr_desc_index_o[15:0]`, `ctr_word_o[5:0]` / `ctr_data_i[31:0]`, `ctr_wait_i`, `ctr_change_i`, `ctr_change_desc_type_i[15:0]`, `ctr_change_desc_index_i[15:0]` | every GET_COUNTERS on a supported object answers SUCCESS with `counters_valid` 0 and a zero block, and no counter notification is ever sent. That is honest (IEEE 1722.1-2021 §7.4.42.2: no bit set, no counter claimed) and **not Milan-conformant**: Milan v1.2 §5.4.2.25 requires the counters of [section 7.1](#counters-face). |
| SRP service | `svc_*` | nothing declares through the configuration plane. When it is driven, each accepted declaration holds every admission verdict until its new slope has been evaluated, which takes up to three rounds (`3*N_STREAM_OUT_P` clocks), and each declaration or withdrawal restarts the partial round. If they keep arriving faster than that, no verdict publishes for any source until they pause ([10 §6.3](../architecture/10_srp_engine.md#sec-10-admission-freshness)). |
| AECP pop face | `aecp_txn_*`, `aecp_rxs_*` | **tie `aecp_txn_ready_i` low.** The internal AECP engine already drains this queue; this face is an *additional* observer. Driving it steals records from the engine. |

**MAAP is yours to place.** With `cfg_maap_internal_i` tied 0 (the default), the
processor disables its internal allocator and selects the external allocation seam.
Address allocation then stays in the integrating fabric, which must provide the
claim/defend/announce machine from IEEE 1722-2016 Annex B. Tie
`cfg_maap_internal_i` to 1 (quasi-static, set before `entity_enable_i`)
and the in-scope `KL_pp_maap` engine
([11](../architecture/11_maap_engine.md)) provides it instead: give it
`cfg_maap_count_i` (block size; `N_STREAM_OUT_P` covers one DA per source) and
optionally a persistence seed (`cfg_maap_seed_offset_i` + `cfg_maap_seed_valid_i`),
gate talker egress on `maap_addr_valid_o`, read source s's DA as `maap_addr_o + s`
(`maap_state_o`/`maap_conflicts_o`/`maap_defends_o` are the observability trio), leave
the whole external `maap_*` port group unconnected — it is quiesced — and retire your
allocator.

<a id="counters-face"></a>
### 7.1 `ctr_*` — the counters face, and what you must count

GET_COUNTERS (IEEE 1722.1-2021 §7.4.42, Milan v1.2 §5.4.2.25) is the processor's
command, and **the counters are yours**. The processor parses the command, proves the
object exists in the descriptor image, lays out Figure 7-67's fixed 160-byte response
and asks you for one word at a time. It keeps no counter, no mask and no observation
tick of its own: every event these counters count happens in your datapath (the PHY
link, gPTP, the media clock, the AVTP talkers and listeners), so you already have it.

| Port | Dir | Contract |
|---|---|---|
| `ctr_req_o` | out | a word of one GET_COUNTERS response is being asked for; a level, held while the beat waits |
| `ctr_desc_type_o[15:0]`, `ctr_desc_index_o[15:0]` | out | the object: the command's `descriptor_type` and `descriptor_index` (AECPDU @24, @26, Figure 7-66) |
| `ctr_word_o[5:0]` | out | 32 = `counters_valid`; 0 to 31 = quadlet *n* of `counters_block`, block byte 4·*n* |
| `ctr_data_i[31:0]` | in | that word: an unsigned 32-bit count, or the mask |
| `ctr_wait_i` | in | **hold**, not ready: 1 keeps the beat, 0 says `ctr_data_i` is the answer now |
| `ctr_change_i`, `ctr_change_desc_type_i[15:0]`, `ctr_change_desc_index_i[15:0]` | in | one `clk_i` cycle per served descriptor whose counters changed, named by the same {type, index} pair |

The read face's rules:

1. **Which objects reach you.** Only STREAM_INPUT (0x0005), STREAM_OUTPUT (0x0006),
   AVB_INTERFACE (0x0009) and CLOCK_DOMAIN (0x0024), and only an index the descriptor
   image holds: the processor locates the descriptor first and answers
   NO_SUCH_DESCRIPTOR without asking you otherwise. Every other type is refused
   NOT_SUPPORTED without asking (ENTITY has only ENTITY_SPECIFIC counters, Table 7-150,
   and PTP_PORT is outside Milan's set).
2. **Order.** For each response the mask first (`ctr_word_o` = 32), then quadlets 0 to 31
   in order. While the response buffer pushes back, the same word is asked again; the
   word never moves under a held beat.
3. **Wrong object.** Answer zero data and a zero mask for any {type, index} you keep
   nothing for. Never answer one object's counters for another's.
4. **Hold.** `ctr_wait_i` may stay 1 as long as you need, up to the `DESC_MEM_TMO_CYC_P`
   watchdog; past it the response is voided and the command answered
   ENTITY_MISBEHAVING, and the descriptor path keeps working. A registered answer
   server is fine: register the selectors, answer from a register a cycle or two later,
   and drop `ctr_wait_i` only while the word presented is still the one your register
   holds the answer for.
5. **`counters_valid`.** Bit *n* set says quadlet *n* is a counter you keep: mask value
   `1 << n`, so Milan's `0x00000001` is quadlet 0 (the MSB-first bit numbers in the
   tables are not shift counts). Claim exactly what you keep, and a quadlet whose bit
   is clear reads 0. The processor carries your mask unchanged and never invents one.

What each type must carry, the floor set by Milan v1.2 §5.4.2.25 (quadlet = block
index; the IEEE offsets are four times it):

| Type | `counters_valid` | Quadlets |
|---|---|---|
| AVB_INTERFACE (Table 5.13) | `0x00000023`; add `0x04`, `0x08`, `0x10` for the optional FRAMES_TX, FRAMES_RX, RX_CRC_ERROR (Table 5.14) | 0 LINK_UP, 1 LINK_DOWN, 5 GPTP_GM_CHANGED (IEEE Table 7-153 offsets 0, 4, 20) |
| CLOCK_DOMAIN (Table 5.15) | `0x00000003` | 0 LOCKED, 1 UNLOCKED (Table 7-155) |
| STREAM_INPUT (Table 5.16), every input of the current configuration, a CRF media-clock input included | `0x00000F3F`, or `0x00000FFF` with the IEEE-only TIMESTAMP_VALID and TIMESTAMP_NOT_VALID | 0 MEDIA_LOCKED, 1 MEDIA_UNLOCKED, 2 STREAM_INTERRUPTED, 3 SEQ_NUM_MISMATCH, 4 MEDIA_RESET, 5 TIMESTAMP_UNCERTAIN, 6 TIMESTAMP_VALID and 7 TIMESTAMP_NOT_VALID (with `0x00000FFF` only; zero under `0x00000F3F`), 8 UNSUPPORTED_FORMAT, 9 LATE_TIMESTAMP, 10 EARLY_TIMESTAMP, 11 FRAMES_RX (Table 7-157) |
| STREAM_OUTPUT (Table 5.17) | `0x0000001F` | 0 STREAM_START, 1 STREAM_STOP, 2 MEDIA_RESET, 3 TIMESTAMP_UNCERTAIN, 4 FRAMES_TX: Milan's compacted layout, **not** IEEE Tables 7-158 and 7-159 ([Δ9](../architecture/01_overview.md#fig-01-deltas)) |

What each counter counts. Every counter is a 32-bit unsigned integer that **wraps** to
zero past its maximum, never saturating (Milan v1.2 §5.3.6.3, §5.3.7.7, §5.3.8.10,
§5.3.11.2), and every counter resets to zero with your reset ("since boot").

| Counter | Counts | Also reset to zero |
|---|---|---|
| LINK_UP / LINK_DOWN (Table 5.1) | each down-to-up / up-to-down change of the level you drive on `link_up_i` | never |
| GPTP_GM_CHANGED (Table 5.1) | each grandmaster identity you publish on `gm_id_i` that differs from the one in force | never |
| LOCKED / UNLOCKED (Table 5.7) | each lock / unlock of the clock domain's media clock, as you define locked | never |
| MEDIA_LOCKED / MEDIA_UNLOCKED, STREAM_INTERRUPTED (Table 5.6) | each lock / unlock of the input's media clock; each playback interruption that is not a controller unbind | the whole input bank, each time that input goes from not bound to bound (`acmp_bound_o` rising), never on unbind (§5.3.8.10) |
| SEQ_NUM_MISMATCH, MEDIA_RESET, TIMESTAMP_UNCERTAIN, UNSUPPORTED_FORMAT, LATE_TIMESTAMP, EARLY_TIMESTAMP, FRAMES_RX (Table 5.6) | one at the end of every observation interval in which the event was seen at least once; the interval is yours, at most 1 s (`T-CTR-OBSERVE`) | as the row above |
| TIMESTAMP_VALID / TIMESTAMP_NOT_VALID (IEEE 1722.1-2021 Tables 7-156 and 7-157; not Milan's, kept only under `0x00000FFF`) | as IEEE Table 7-157 defines them: one for each received stream data AVTPDU with the tv bit set / clear, per frame, not per observation interval | the whole input bank, as the rows above |
| STREAM_START / STREAM_STOP (Table 5.4) | each start / stop of the talker's stream | never |
| MEDIA_RESET, TIMESTAMP_UNCERTAIN, FRAMES_TX (Table 5.4) | one at the end of every observation interval, at most 1 s, in which a transmitted AVTPDU toggled mr, set tu, or was sent | each time the talker starts streaming |

The four pairs keep Milan's invariants by construction when each pair counts the two
edges of one level and the edge detector's previous value resets to the inactive state
(link down, clock unlocked, not streaming): LINK_UP = LINK_DOWN or LINK_DOWN + 1, and
likewise LOCKED / UNLOCKED, STREAM_START / STREAM_STOP and MEDIA_LOCKED /
MEDIA_UNLOCKED. A link already up when reset releases is then one LINK_UP.

**The AVB_INTERFACE duty** (processor issue #44). The processor hands you no counter
tick, because the events are your own inputs to it:

- LINK_UP and LINK_DOWN count the level you drive on `link_up_i`: the level the
  advertise machine, the SRP Domain and the GET_AVB_INFO notification see, so the
  counters and the entity's behaviour never disagree.
- GPTP_GM_CHANGED counts grandmaster changes and nothing else (Table 5.1: "Number of
  gPTP GM changes, since boot"; IEEE 1722.1-2021 Table 7-153, offset 20: "gPTP
  grandmaster change count"). The rule is one identity comparison: at each update you
  publish, count one when the grandmaster identity on `gm_id_i` differs from the
  identity in force before that update. The identity in force out of reset is the first
  one you publish, and it counts nothing. **Neither strobe identifies a grandmaster
  change, and neither does their coincidence**: `gm_change_i` also marks a domain-only
  update (section 6), and `gsi_asp_chg_i` marks any changed PathTrace, a tail-only one
  included, and never rises if you publish no path. So a domain-only update counts
  nothing, and a grandmaster change counts one whichever strobes you raise for it.
  (The ADP engine used to carry a one-clock-late copy of `gm_change_i` as a
  GPTP_GM_CHANGED tick. It reached no port, counted domain-only changes, and is
  removed.)
- One bank per AVB_INTERFACE descriptor, answered at its own `ctr_desc_index_o`. This
  build has one interface (`link_up_i`, `gm_id_i` and `gptp_domain_i` are interface 0);
  the read face needs no change for a second.

**The change strobe.** Milan Table 5.22 sends an unsolicited GET_COUNTERS "when one of
the counters is updated", at most once per descriptor per second. Pulse `ctr_change_i`
for one cycle, with the descriptor's type and index, whenever a quadlet you serve for it
changes: an increment, or a reset rule that clears a non-zero count. One descriptor per
cycle: serialise simultaneous changes yourself, losing none. The processor marks the
descriptor, coalesces repeats and sends one GET_COUNTERS response with u = 1 to every
registered controller. It gathers that response from this face when it is emitted, so it
carries the counts of that moment; a change inside the second after an emission waits
and goes out once, when the second has passed (`T-CTR-NOTIF`). Descriptors are throttled
independently. A strobe the processor has no slot for is ignored: it keeps one for every
Stream Input and Stream Output index of the shape (`N_STREAM_IN_P`, `N_STREAM_OUT_P`),
for AVB_INTERFACE 0 and for CLOCK_DOMAIN 0.

On the reference platform every counter above lives in `milan_datapath`, behind this
same face.

---

## 8. The class-D status wires — read them every clock

These are the reason the processor is worth integrating rather than polling. They are
combinational reads of `clk_i` registers, published continuously, so your datapath can
gate on them per cycle.

| Output | Use |
|---|---|
| `acmp_declaring_o` | **the talker egress gate.** AND it with your own stream enable. |
| `acmp_bound_o` | per-sink binding installed, **debounced** — safe to edge-detect. The raw internal register dips low and high again inside a single rebind transaction; this port does not. |
| `acmp_bound_eid_o`, `acmp_bound_sid_o`, `acmp_bound_dmac_o`, `acmp_bound_vlan_o` | the bound stream's identity on the wire: who the talker is, which stream, on what address and VLAN. Arm your RX filter and stream table from these — you cannot derive them from the entity id. |
| `srp_active_o` | declaring Advertise, a Listener is Ready/ReadyFailed, optimistic or real admission, and the stream VID's MVRP join has left through the TX arbiter (Milan §4.3.2; [10 §6.2](../architecture/10_srp_engine.md#sec-10-join-before-stream)). For confirmed admission use **ACTIVE AND `srp_sr_admitted_o`** (parent issue #551 decision). |
| `srp_sr_admitted_o` | real Σ-slope verdict for the current declaration; low after every accepted declaration until its new slope completes an admission round. No optimistic term. **Cross-source rule:** while any source's new declaration is pending, no bit rises, and a bit falls only with its own source's declaration or withdrawal. A pending declaration never frees its capacity for another source; only a withdrawal or an evaluated shrink does ([10 §6.3](../architecture/10_srp_engine.md#sec-10-admission-cross-source)). Latency is at most three rounds after the last declaration/withdrawal, or four clocks for one source; [10 §6.3](../architecture/10_srp_engine.md#sec-10-admission-freshness) gives the measured cases. |
| `srp_granted_slope_bps_o`, `srp_sum_slope_bps_o` | current per-source granted idleSlope (zero while pending or unadmitted; follows `srp_sr_admitted_o`) and the sum for the shaper, latched when a round publishes. The sum holds its previous value from a declaration or withdrawal until every pending declaration has been evaluated. |
| `srp_over_limit_o` | latched with the sum: at least one evaluated source was refused against the port ceiling; pending evaluation is not refusal. |
| `srp_class_a_prio_o`, `srp_class_a_vid_o`, `srp_domain_adopted_o`, `srp_domain_change_o` | the Class A identity in force. The two values are defaults until `srp_domain_adopted_o` says a bridge Domain was adopted. The VID default is `SRP_DOM_DEF_VID_P`, and a link-down restores both defaults. |
| `srp_tk_decl_state_o`, `srp_lstn_reg_state_o`, `srp_tk_reg_state_o`, `srp_lstn_decl_state_o` | the four declaration/registration state vectors, two bits per index. Read each one against its port comment, never against a listed order: `srp_lstn_reg_state_o` carries `srp_pkg::srp_decl_e` (1 Asking Failed, 2 Ready, 3 Ready Failed, [02 F02.10](../architecture/02_interfaces.md#fig-02-statusdict)), so "a Listener is registered" is any non-zero code and "Ready or Ready Failed" is bit 1 |
| `srp_acc_latency_o` | per-sink registered accumulated latency in nanoseconds, **raw** — add your own ingress delay |
| `srp_src_fail_code_o`, `srp_src_fail_bridge_o`, `srp_snk_fail_code_o` | failure codes, valid only while the matching state vector says FAILED |
| `adp_next_avail_index_o` | 32 bits, deliberately. Truncating it would make a controller see `available_index` step backwards, which is exactly the signal it uses to decide an entity restarted. |
| `nvm_unflushed_o` | per-sink binding state the binding manager has accepted and not yet committed. 1 from the accepted change until its record commits with `done`, or until its three attempts are exhausted and `nvm_alarm_o` rises on the same cycle. |
| `d3_unflushed_o` | the D3 writer's pending: 1 while any scalar record (configuration, sampling rate, clock source, stream formats, presentation offsets) or user-name record holds an accepted change not yet committed with an untainted `done`, or until its three attempts are exhausted. Your "saved state pending" bit is `(|nvm_unflushed_o) | d3_unflushed_o`; without both, a change taken inside the commit debounce reads durable. |
| `aecp_dyn_dirty_o` | a sticky **diagnostic**: some dynamic-state row was written since reset. It is not pending and must not feed your pending bit. |
| `nvm_alarm_o` | a record producer (binding or D3) exhausted one record's three write attempts; set until reset, whatever later writes do. |
| `restore_done_o`, `restore_busy_o`, `restore_fail_o`, `restore_blank_o` | the combined verdicts of both restore walks ([07 §5.3](../architecture/07_memory_maps.md#fig-07-nvmflow)). Blank reads 1 only for a restore that did not fail. |
| `restore_closed_o`, `rs_cause_o[2:0]`, `restore_rb_o`, `restore_cause_o[1:0]` | the D3 walk's CLOSED terminal (never done), its cause (1 torn, 2 device, 3 deadline, 5 passes disagree, 6 descriptor read, 7 image unproven), its roll-back to DEFAULTS; and the binding walk's own cause. |
| `aecp_name_wr_o` | one `clk_i` cycle at each accepted live 64-bit name-lane write into the descriptor store. A multi-lane `SET_NAME` pulses once per written lane; unchanged lanes, boot loading, the D3 writer's restore of a saved name, refused/out-of-range commands and writes aborted before acceptance produce no pulse. Earlier accepted writes still count if a command later aborts. Sample at the accepting rising edge; no ready/ack. Leave unused with an explicit `.aecp_name_wr_o()` connection. |
| `aecp_nvm_stb_o`, `aecp_nvm_mark_o` | one cycle per committed command that marked a record group, and the group: 1 a dynamic-state field, 6 channel maps, 7 user names. A **completion notification**: it selects no record and is not a persistence trigger. Groups 1 and 7's records are written by the D3 writer from the accepted change itself (group 7's trigger is `aecp_name_wr_o`). Group 6's records, the channel maps, are **yours** (the manager's ruling on processor issue #83): write `0x60`/`0x70` + port from the accepted phase-5 edit beat (`amap_edit_req_o` with `amap_edit_phase_o == 5`), restore them and reset them on a roll-back (§9 step 4, [07 §5.1](../architecture/07_memory_maps.md#51-persisted-vs-volatile-normative-set-req-per-001002)). Your pending covers them; `d3_unflushed_o` never does. |
| `dbg_now_ms_o` | the free-running millisecond timebase |

The status dictionary these implement is catalogued in
[`02_interfaces.md` F02.10](../architecture/02_interfaces.md#fig-02-statusdict).

GET_STREAM_INFO input failure/probing fields are resolved **inside the processor**
from its SRP registrar and ACMP listener record. Kind 0 selectors 5 and 7 never
raise `gsi_req_o` for STREAM_INPUT; external answers for those cases are unused.
Selector 4 still requests the destination MAC, but its failure-code byte is
replaced internally. Keep serving the other selectors and the existing
STREAM_OUTPUT words. No additional port or instantiation connection is needed.
The internal fields and their notification events have one state owner; do not
derive a second probing status from bound/settled flags. The internal fields are
read live at each gather beat, like your own words; the authoritative gather
contract and its coherence bound are [06 F06.13](../architecture/06_aecp_engine.md#fig-06-lineage).

---

## 9. Bring-up order

1. Load the descriptor image into your memory at `DESC_BASE_P` and check its CRC
   **before** you start the restore: the saved-state restore judges values against it,
   and an image it cannot prove ends the restore CLOSED.
2. Release `rst_n` with `clk_i` running. From here AECP dispatch and the ACMP listener
   are held until the restore releases them.
3. Pulse `restore_go_i`, **on every boot**; wait for `restore_done_o` or
   `restore_closed_o`, then read `restore_fail_o` **and** `restore_blank_o`. Three
   releases follow, each its own ([07 §5.3](../architecture/07_memory_maps.md#fig-07-nvmflow)):
   - the **binding walk's** drained terminal releases the ACMP listener: its saved
     bindings are restored (or all at their defaults if the walk failed whole: a torn
     read-back, a device error, or a device silent for `NVM_RS_TMO_CYC_P` clocks);
   - the **D3 walk** then restores the scalar settings and the user names, and its
     COMPLETE or DEFAULTS terminal releases AECP dispatch. A D3 failure rolls the AECP
     stores back to their defaults (`restore_rb_o`) and **keeps the bindings the
     binding walk restored**; your map plane is not in it (step 4);
   - `restore_done_o` (both walks done) releases your requested enable to ADP.

   `restore_done_o` says the walks sequenced, not that anything came back: every
   per-record default sets it. `restore_blank_o` separates a restore from walks over
   blank, unframed or absent media, and reads 0 for any failed restore.
   `restore_closed_o` without `restore_done_o` is CLOSED: AECP and ADP stay held until
   reset, and the listener keeps serving. It has two causes, which `rs_cause_o` tells
   apart ([07 §5.3](../architecture/07_memory_maps.md#fig-07-nvmflow)):
   - the descriptor image could not be proven (`rs_cause_o` 7): fix the image load and
     reset;
   - a D3 walk that failed in its second pass rolled back and could not prove the image
     again: the re-LOCATE missed or erred, or the roll-back's debt wait or re-LOCATE
     outlasted `NVM_RS_TMO_CYC_P`, or the aggregate bound below fell inside it.
     `rs_cause_o` is then the second pass's cause (1, 2, 3, 5 or 6) and `restore_rb_o`
     reads 0: a device or descriptor-memory fault met during the restore; check both
     and reset.

   One more CLOSED needs a misconfiguration: the image proof's own LOCATE outlasting
   `NVM_RS_TMO_CYC_P` (`rs_cause_o` 3, no roll-back), reachable only with that
   parameter below the descriptor image walk, which its row above rules out.

   The saved records stay on the media after any failure: only a later change replaces
   one, so the next boot on a healthy device restores them. Budget the restore: each
   wait is bounded by `NVM_RS_TMO_CYC_P` (20 ms of `CLK_HZ_P`; size it above the slowest
   single record read your device face can take and above the descriptor image walk),
   and the whole restore from `restore_go_i` by `NVM_RS_AGG_CYC_P` (1,000 ms of
   `CLK_HZ_P`). A device slow enough to reach that bound, while answering every wait
   in time, ends the restore where the bound finds it, and it never closes a provable
   image:
   - the binding walk still reading (it alone outlasts 1,000 ms, e.g. a device slow per
     byte over saved bindings): the binding walk fails whole as at its own deadline
     (`restore_cause_o` 3, every binding at its default), the listener is released, and
     the D3 walk proves the image, reads no record and ends **DEFAULTS**, `rs_cause_o`
     3 (CLOSED, `rs_cause_o` 7, only if the image cannot be proven);
   - the D3 walk's first pass: **DEFAULTS**, `rs_cause_o` 3, nothing applied;
   - its second pass: a roll-back to **DEFAULTS** (`restore_rb_o`), `rs_cause_o` 3;
   - inside a roll-back that another fault had started: **CLOSED**, with that fault's
     cause (above).

   The terminal follows the bound within one per-wait deadline plus a few clocks, or
   within two plus a few clocks when a roll-back runs after it (its debt wait and its
   re-LOCATE are each bounded by one). The processor counts from the clock it accepts
   `PP_CTRL[1]`, a few clocks after your write. So let your bounded wait for
   `restore_done_o` or `restore_closed_o` cover 1,000 ms plus two per-wait deadlines
   plus a few clocks; 1,060 ms, one more per-wait deadline, is a stated margin that
   covers the clocks. The wait decides nothing: the processor ends the restore itself.
   DR3a ratified both numbers ([08 §2](../architecture/08_timing.md#sec-08-nvm)).
4. **Restore the channel maps yourself**: the processor neither saves nor restores them
   ([07 §5.1](../architecture/07_memory_maps.md#51-persisted-vs-volatile-normative-set-req-per-001002)).
   After `restore_done_o`, apply each port's saved set from records `0x60`/`0x70`,
   judged against the formats the D3 walk restored (`aecp_fmt_in_o`, `aecp_fmt_out_o`
   and their valid bits): the walk judged them by your format judge alone, without the
   maps, so the coupled format/map decision is yours. When the walk rolled back
   (`restore_rb_o`), keep every port's reset set: the processor's roll-back resets its
   two AECP stores, never your map plane. AECP dispatch is already released, so hold a
   map command that reaches your faces before your restore ends on their waits
   (`amap_wait_i`, `amap_edit_wait_i` before phase 5), within the AECP watchdog.
5. Present identity, capability and configuration inputs.
6. Assert `entity_enable_i` when you are ready, after step 4; the processor forwards it
   to ADP only once `restore_done_o` is 1. Only then may the entity advertise.

If it does not come up, hand the board to the [operator guide](operator.md) — its
[bring-up ladder](../diagrams/23-bringup-decision.svg) is written for exactly this moment.

---

## 10. Consuming this repository

The reference platform includes this repository as a **git submodule** and instantiates
the top from here. The HDL is authored here and only consumed there — never copied. The
pin is moved only to a commit where the documentation gates, the lint gate and the full
suite sweep are green. Interface stability at `hdl/top/` follows
[`02_interfaces.md`](../architecture/02_interfaces.md): a breaking port change is a
documented interface change *before* it is an RTL change.

Details of that contract are in [`../../hdl/README.md`](../../hdl/README.md).
