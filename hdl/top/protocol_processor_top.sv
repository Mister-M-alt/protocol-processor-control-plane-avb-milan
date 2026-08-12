/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : protocol_processor_top.sv
//  Project     : IEEE 1722.1 protocol processor
//                (docs/architecture/01 §7 F01.5, 02 §2-§6, 03 in full,
//                 04/05/10 engines, 07 §2/§5, 08 §4-§5)
//
//  Description : The processor top — ONE MAC trunk byte stream in, ONE MAC
//                byte stream out, everything of the landed tree wired
//                between them:
//                  RX: trunk -> KL_pp_rx_validator (+ KL_pp_rx_slots pool)
//                      -> parsed-header beat -> KL_pp_normalizer ->
//                      KL_pp_dispatch -> {KL_adp_engine, KL_pp_acmp_listener +
//                      KL_acmp_talker by message-type steer, AECP pop as a
//                      TOP-LEVEL face for the P4 uCPU}.
//                  V9 seam: the validator's MRP pass-through emits the whole
//                      frame from DA byte 0; KL_srp_top wants
//                      ProtocolVersion-first — KL_mrp_strip drops the 14
//                      header bytes, derives the msrp/mvrp select from the
//                      stripped EtherType and buffers P-MRPDU-QUEUE-BYTES.
//                  TX: engine faces -> KL_pp_tx_slots (via the pool-access
//                      arbiter below) -> KL_pp_tx_arbiter -> the ACMP
//                      Ethernet-header prepend shim -> MAC out.
//                  Services: KL_pp_timer_service (arm-port priority mux),
//                      KL_pp_prng (draw-port owner mux), KL_pp_scoreboard,
//                      KL_pp_event_router, KL_pp_originator, KL_pp_trace_ring,
//                      KL_pp_side_port, KL_acmp_nvm_shadow -> KL_pp_nvm_port.
//                  The F02.10 class-D status dictionary is aggregated into
//                      the side-port snapshot window (0x20000).
//
//                Seams recorded as glue here (each bannered at its block):
//                  - hdr-beat latch: the validator's one-cycle parsed-header
//                    beat vs the normalizer's held-until-ready producer face;
//                  - ACMP Ethernet prepend: KL_pp_acmp_listener (and the talker
//                    response builder) commit 56-byte ACMPDUs while ADP/SRP
//                    commit whole wire frames — TX lanes 2/5 get the 14-byte
//                    header injected on the way to the MAC;
//                  - dispatch-ROM stub: hazard class answered from a
//                    protocol-keyed default until the P4 dispatch ROM lands;
//                  - TX-pool access arbiter: one alloc+write port, four
//                    builders — ownership from alloc grant to commit, the
//                    engines' own alloc retry loops absorb the wait;
//                  - timer arm mux: fixed-priority drain of per-engine arm
//                    queues (listener > talker > ADP > SRP > originator),
//                    depth 4 each, drops counted (never silent);
//                  - PRNG draw mux: pending-latch per client with owner
//                    routing of draw_valid (a broadcast valid would complete
//                    the wrong client's draw).
//
//                The one design decision that matters: the RX slot pool is
//                REPLICATED once per reader (ADP / listener / talker / AECP
//                face / ACMP steer prefetch) instead of arbitrating its
//                single read port. Every
//                landed engine assumes byte-data-one-cycle-after-rd_en with
//                no stall path, so a shared port would silently interleave
//                addresses under concurrent pops; replicas receive the
//                identical alloc/write/commit/free stream cycle-by-cycle
//                (frees serialized through one queue and broadcast), so
//                their occupancy state is lock-step by construction and
//                reads never mutate — divergence is structurally impossible,
//                at the price of three extra small RAMs.
//---------------------------------------------------------------------------//
`default_nettype none

module protocol_processor_top
  import pp_pkg::*;
#(
    //! P-N-STREAM-IN (F01.5): sinks / listener SMs
    parameter int unsigned N_STREAM_IN_P       = 8,
    //! P-N-STREAM-OUT (F01.5): sources / talker gates
    parameter int unsigned N_STREAM_OUT_P      = 8,
    //! P-RX-SLOTS x P-RX-SLOT-BYTES (F01.5)
    parameter int unsigned RX_SLOTS_P          = 4,
    parameter int unsigned RX_SLOT_BYTES_P     = 576,
    //! P-TX-STD-SLOTS / P-TX-OVERSIZE-BYTES (F01.5)
    parameter int unsigned TX_STD_SLOTS_P      = 4,
    parameter int unsigned TX_OVERSIZE_BYTES_P = 1600,
    //! P-CLK-HZ (F01.5) — feeds the timer prescaler default
    parameter int unsigned CLK_HZ_P            = 100_000_000,
    //! timer prescaler overrides (09 §3 TIM time compression)
    parameter int unsigned TIM_DIV_US_P        = CLK_HZ_P / 32'd1_000_000,
    parameter int unsigned TIM_DIV_MS_P        = 1000,
    //! ACMP listener transition-ROM image (rom/gen_ltn_rom.py output)
    parameter string       TROM_HEX_P          = "ltn_rom.hex",
    //! derived — do not override
    //! F08.4 timer-slot map for THIS shape. Every base below is the running
    //! sum of the group extents before it (pp_pkg::pp_timer_map is the ONE
    //! place that arithmetic exists), so the map is correct at any
    //! P-N-STREAM-IN / P-N-STREAM-OUT — see the banner at the bases.
    localparam pp_timer_map_t TMR_MAP_C =
        pp_timer_map(PP_N_IF_C, N_STREAM_IN_P, N_STREAM_OUT_P,
                     PP_N_CTRL_C, PP_CA_POOL_C),
    //! P-TIMER-SLOTS for this shape (= pp_timer_slots(..., en_srp = 1)).
    //! At the F01.5 default shape this is exactly PP_TIMER_SLOTS_C = 89.
    localparam int unsigned TMR_SLOTS_C  = TMR_MAP_C.srp_end,
    localparam int unsigned TMR_AW_C     = $clog2(TMR_SLOTS_C),             // 7 at 8x8
    localparam int unsigned RXS_W_C      = (RX_SLOTS_P > 1) ? $clog2(RX_SLOTS_P) : 1,
    //! Sink/source index widths, CLAMPED exactly as the engines clamp their
    //! own (KL_pp_acmp_listener SINK_W_C :76, KL_acmp_talker SRC_W_C :95).
    //! Without the clamp a 1-stream shape declares [-1:0], and the consumer
    //! elaborates this processor at N_STREAMS = 1 on its shipping board.
    localparam int unsigned SINK_IDX_W_C = (N_STREAM_IN_P  > 1) ? $clog2(N_STREAM_IN_P)  : 1,
    localparam int unsigned SRC_IDX_W_C  = (N_STREAM_OUT_P > 1) ? $clog2(N_STREAM_OUT_P) : 1,
    localparam int unsigned RXA_W_C      = $clog2(RX_SLOT_BYTES_P),
    localparam int unsigned RXL_W_C      = $clog2(RX_SLOT_BYTES_P + 1),
    localparam int unsigned TXS_W_C      = $clog2(TX_STD_SLOTS_P + 1),      // 3
    localparam int unsigned TXA_W_C      = $clog2(TX_OVERSIZE_BYTES_P + 1)  // 11
) (
    input  wire         clk_i,                 //! core clock (P-CLK-HZ domain)
    input  wire         rst_n,                 //! synchronous active-low reset

    //! ---- identity + model registers (quasi-static, 02 §2 rule 4) ----
    input  wire  [63:0] entity_id_i,           //! own entity_id
    input  wire  [63:0] entity_model_id_i,     //! entity_model_id
    input  wire  [47:0] own_mac_i,             //! own unicast MAC
    input  wire  [15:0] talker_sources_i,      //! ADPDU talker_stream_sources
    input  wire  [15:0] talker_caps_i,         //! ADPDU talker_capabilities
    input  wire  [15:0] listener_sinks_i,      //! ADPDU listener_stream_sinks
    input  wire  [15:0] listener_caps_i,       //! ADPDU listener_capabilities
    input  wire  [15:0] current_cfg_i,         //! current_configuration_index
    input  wire  [15:0] identify_index_i,      //! identify_control_index

    //! ---- level controls + class-D inputs (02 §6) ----
    input  wire         entity_enable_i,       //! Milan §5.6.1 boot gate (level)
    input  wire         link_up_i,             //! link status (2FF-synced upstream)
    input  wire         gm_change_i,           //! GM_CHANGE strobe (gptp)
    input  wire  [63:0] gm_id_i,               //! gm_id (interface 0)
    input  wire  [7:0]  gptp_domain_i,         //! gptp_domain (interface 0)

    //! ---- SRP quasi-static configuration (10) ----
    input  wire         p2p_i,                 //! operPointToPointMAC (Milan: 1)
    input  wire         cfg_rank_i,            //! declared rank bit (F10.7)
    input  wire  [31:0] cfg_acc_lat_ns_i,      //! initial accumulated_latency
    input  wire  [31:0] port_rate_bps_i,       //! port rate for the admission ceiling
    input  wire  [15:0] cfg_tspec_max_frame_i, //! TSpec MaxFrameSize for engine-driven DECLARE_TALKER

    //! ---- talker source configuration (05 quasi-static) ----
    input  wire  [N_STREAM_OUT_P-1:0]    cfg_src_en_i,     //! source exists in current config
    input  wire  [N_STREAM_OUT_P*2-1:0]  cfg_src_iface_i,  //! per-source AVB interface
    input  wire  [N_STREAM_OUT_P*64-1:0] cfg_stream_id_i,  //! per-source stream_id

    //! ---- MAC trunk RX (byte stream, frames delimited by rx_last_i) ----
    input  wire         rx_valid_i,            //! byte strobe from the RX async FIFO
    input  wire  [7:0]  rx_data_i,             //! frame byte (byte 0 = first DA octet)
    input  wire         rx_last_i,             //! final byte of the frame

    //! ---- MAC TX (merged byte stream, 02 §2 class A) ----
    output logic        tx_valid_o,            //! stream byte valid
    output logic        tx_sof_o,              //! first byte of each frame
    output logic [7:0]  tx_data_o,             //! frame byte (byte 0 = first DA octet)
    output logic        tx_eof_o,              //! final byte of the frame
    input  wire         tx_ready_i,            //! downstream MAC FIFO consumes

    //! ---- AECP pop face (P4 uCPU seam; tie ready 0 until it lands) ----
    output logic                    aecp_txn_valid_o, //! head record valid
    output logic [PP_TXN_W_C-1:0]   aecp_txn_o,       //! head pp_txn_t record
    input  wire                     aecp_txn_ready_i, //! consumer pops the head
    input  wire  [RXS_W_C-1:0]      aecp_rxs_rd_slot_i, //! payload slot to read
    input  wire  [RXA_W_C-1:0]      aecp_rxs_rd_addr_i, //! byte address in the slot
    input  wire                     aecp_rxs_rd_en_i,   //! sync-read enable
    output logic [7:0]              aecp_rxs_rd_data_o, //! byte, one cycle later
    output logic [RXL_W_C-1:0]      aecp_rxs_slot_len_o,//! committed slot length
    input  wire                     aecp_rxs_free_i,    //! return the consumed slot
    input  wire  [RXS_W_C-1:0]      aecp_rxs_free_slot_i, //! which slot

    //! ---- NVM boot restore + alarm (07 §5.3) ----
    input  wire         restore_go_i,          //! start boot restore
    output logic        restore_busy_o,        //! restore walk running
    output logic        restore_done_o,        //! level: restore complete
    output logic        restore_fail_o,        //! level: torn read-back aborted restore
    output logic        nvm_alarm_o,           //! sticky: commit retries exhausted

    //! ---- NVM device face (external backend; KL_pp_nvm_port initiator) ----
    output logic        nvm_dev_req_o,         //! command request, held until gnt
    input  wire         nvm_dev_gnt_i,         //! backend accepts the command
    output logic [1:0]  nvm_dev_op_o,          //! READ / WRITE / ERASE
    output logic [7:0]  nvm_dev_region_o,      //! region id = record id
    output logic [15:0] nvm_dev_offset_o,      //! byte offset within the region
    output logic [15:0] nvm_dev_len_o,         //! byte count
    output logic        nvm_dev_wvalid_o,      //! write byte present
    input  wire         nvm_dev_wready_i,      //! backend accepts the write byte
    output logic [7:0]  nvm_dev_wdata_o,       //! write byte
    input  wire         nvm_dev_rvalid_i,      //! read byte present
    input  wire  [7:0]  nvm_dev_rdata_i,       //! read byte
    output logic        nvm_dev_rready_o,      //! port accepts the read byte
    input  wire         nvm_dev_busy_i,        //! backend busy (informational)
    input  wire         nvm_dev_done_i,        //! one-cycle: command complete
    input  wire         nvm_dev_err_i,         //! one-cycle: command failed

    //! ---- side-port host face (class E, F02.7) ----
    input  wire         host_req_valid_i,      //! request present (hold until rvalid)
    input  wire         host_we_i,             //! 1 = write
    input  wire  [19:0] host_addr_i,           //! 20-bit WORD address
    input  wire  [31:0] host_wdata_i,          //! write data
    output logic [31:0] host_rdata_o,          //! read data (0 on error)
    output logic        host_rvalid_o,         //! completion strobe
    output logic        host_err_o,            //! access refused

    //! ---- SRP service face (02 §4.1 class B; config-plane seam) ----
    input  wire         svc_valid_i,           //! request offered, held until ready
    output logic        svc_ready_o,           //! stage free this cycle
    input  wire  [2:0]  svc_op_i,              //! KL_srp_top req_op_i code
    input  wire  [7:0]  svc_index_i,           //! source/sink index / class id
    input  wire  [63:0] svc_stream_id_i,       //! stream_id
    input  wire  [47:0] svc_da_i,              //! destination MAC
    input  wire  [11:0] svc_vid_i,             //! VLAN id
    input  wire  [15:0] svc_max_frame_i,       //! DECLARE_TALKER TSpec MaxFrameSize
    input  wire  [1:0]  svc_lstn_state_i,      //! DECLARE_LISTENER state
    output logic        svc_rsp_valid_o,       //! one response per svc request
    output logic [1:0]  svc_rsp_status_o,      //! 0 OK / 1 FAIL / 2 UNSUPPORTED
    output logic [31:0] svc_rsp_data_o,        //! GET_DOMAIN answer

    //! ---- class-D status levels (02 §6, F02.10) — THE FABRIC FACE --------
    //! Everything this engine knows used to be reachable only through a
    //! side-port READ TRANSACTION. That is a software-paced path, and the
    //! integrating fabric consumes this state as WIRES, every clock: the
    //! consumer's AAF talker gate is a per-cycle AND of the talker-active and
    //! stream-gate terms, and its CBS slope MUX needs the granted idleSlope.
    //! Publishing nothing here is exactly the NC-5 defect the resource study
    //! called out — "its replacement publishes no granted bandwidth, so the
    //! consumer's CBS slope MUX and its Σ-limit admission decision would have
    //! no source under this contract". These ports are that source.
    //!
    //! Per-index vectors are FLAT packed bit-vectors, index s at [W*s +: W] —
    //! the cfg_stream_id_i / cfg_src_iface_i convention already used above.
    //! All are combinational reads of clk_i-domain registers: a consumer in
    //! another clock domain owns its own 2FF synchroniser.
    output logic [2:0]                   srp_class_a_prio_o,      //! SRclassPriority, DEFAULTS until adopted
    output logic [11:0]                  srp_class_a_vid_o,       //! SRclassVID, DEFAULTS until adopted
    output logic                         srp_domain_adopted_o,    //! F10.2: 1 = ADOPTED a bridge Domain, 0 = still DEFAULTS
    output logic                         srp_domain_change_o,     //! one-cycle DOMAIN_CHANGE, pairs with the two levels above
    output logic [N_STREAM_OUT_P*2-1:0]  srp_tk_decl_state_o,     //! per-source self-declared {0 NONE, 1 ADVERTISE, 2 FAILED}
    output logic [N_STREAM_OUT_P*2-1:0]  srp_lstn_reg_state_o,    //! per-source registered Listener attr (srp_pkg::srp_decl_e)
    //! THE AVTP transmit gate: declaring AND not failed AND a listener is
    //! READY AND admitted. Use THIS — never rebuild it from the terms below,
    //! which carry an optimistic window (see srp_sr_admitted_o).
    output logic [N_STREAM_OUT_P-1:0]    srp_active_o,
    //! RAW Σ-slope verdict. It lags srp_active_o by up to three admission
    //! rounds after a fresh declare, because the FSM admits optimistically
    //! (KL_srp_top: sr_adm_fsm = opt_r | adm_admitted). A consumer that gates
    //! on this instead of srp_active_o mutes a legal stream for those rounds.
    output logic [N_STREAM_OUT_P-1:0]    srp_sr_admitted_o,
    output logic [N_STREAM_OUT_P*32-1:0] srp_granted_slope_bps_o, //! per-source granted idleSlope, 0 when not admitted (802.1Q §34.6.1.1); same optimistic lag
    output logic [N_STREAM_OUT_P*8-1:0]  srp_src_fail_code_o,     //! per-source SELF-declared Failed code; valid only with tk_decl_state == FAILED
    output logic [N_STREAM_OUT_P*64-1:0] srp_src_fail_bridge_o,   //! per-source SELF-declared FailureInformation; same validity
    output logic [31:0]                  srp_sum_slope_bps_o,     //! Σ granted idleSlope over admitted sources — the CBS slope-MUX value
    output logic                         srp_over_limit_o,        //! at least one source was refused against the port ceiling
    output logic [N_STREAM_IN_P*2-1:0]   srp_tk_reg_state_o,      //! per-sink registered Talker attr {0 NONE, 1 ADVERTISE, 2 FAILED}
    output logic [N_STREAM_IN_P*2-1:0]   srp_lstn_decl_state_o,   //! per-sink OUR Listener declaration
    output logic [N_STREAM_IN_P*32-1:0]  srp_acc_latency_o,       //! per-sink registered accumulated_latency, ns, RAW — the consumer adds its own ingress delay
    output logic [N_STREAM_IN_P*8-1:0]   srp_snk_fail_code_o,     //! per-sink registered Failed code; valid only with tk_reg_state == FAILED

    //! ---- class-D ACMP / ADP status levels --------------------------------
    //! per-source DA gate open. THE talker egress gate a fabric ANDs with its
    //! own stream enable every clock.
    output logic [N_STREAM_OUT_P-1:0]    acmp_declaring_o,
    //! per-sink binding installed, DEBOUNCED. The internal binding register
    //! dips 1->0->1 inside a single executor transaction on every re-bind
    //! (the F05.3 BIND_NEW row runs an unbind action before the bind action),
    //! so the raw register is not safe for a fabric to edge-detect. This port
    //! holds through the transaction; the raw one still drives the ADP
    //! engine's own edge detector, which must keep seeing it.
    output logic [N_STREAM_IN_P-1:0]     acmp_bound_o,
    output logic [N_STREAM_IN_P*64-1:0]  acmp_bound_eid_o,        //! per-sink bound talker entity_id, valid with acmp_bound_o
    //! The bound stream's IDENTITY ON THE WIRE. entity_id above says WHO the
    //! talker is; these say WHICH STREAM to receive and on WHAT address — an
    //! integrating fabric needs them to arm its RX filter and its stream
    //! table, and cannot derive either from the entity_id. Latched at A15
    //! settle, held while acmp_bound_o.
    output logic [N_STREAM_IN_P*64-1:0]  acmp_bound_sid_o,        //! per-sink bound stream_id
    output logic [N_STREAM_IN_P*48-1:0]  acmp_bound_dmac_o,       //! per-sink bound stream destination MAC
    output logic [N_STREAM_IN_P*12-1:0]  acmp_bound_vlan_o,       //! per-sink bound stream VLAN id
    //! 32 BITS, not 16. KL_adp_engine keeps a 32-bit counter and puts all
    //! 32 on the wire, and the AEM ENTITY descriptor's available_index is a
    //! 32-bit field that is visible in every READ_DESCRIPTOR response.
    //! Truncating here would wrap the CSR and the AEM readback at 65536
    //! advertisements while the ADPDU kept counting — a controller would see
    //! available_index step BACKWARDS, which is exactly the signal it uses to
    //! decide an entity restarted.
    output logic [31:0]                  adp_next_avail_index_o,  //! available_index the NEXT ENTITY_AVAILABLE will carry

    //! ---- maap face (02 §4.2) — THE ADDRESS ALLOCATOR SEAM --------------
    //! This processor implements no MAAP: 01 §3 puts address allocation
    //! OUTSIDE it, in the integrating fabric (the consumer already ships a
    //! KL_maap engine). Nothing else can open the talker DA gate, because
    //! GS_DECLARING is reachable only through GS_DA_OK and GS_DA_OK is
    //! written only on an ALLOC_DA success (KL_acmp_talker EVC_GRANT): with
    //! this face unconnected acmp_declaring_o above is STRUCTURALLY stuck at
    //! 0 and no source ever declares a Talker attribute to SRP either. So it
    //! is published rather than tied off — a processor whose talker half is
    //! dead by construction is not a contract a fabric can integrate.
    //!
    //! Single-outstanding: one ALLOC_DA / RELEASE_DA at a time, req held
    //! until ready, exactly one rsp per accepted req, conflicts as a sticky
    //! event acked combinationally. An absent OR BROKEN allocator is a LEGAL
    //! wiring, because BOTH halves are bounded in KL_acmp_talker: ready may
    //! sit at 0 (abandoned after P-MAAP-ACCEPT-CYC), and an accepted request
    //! may go unanswered (abandoned after P-MAAP-RSP-MS, a bound derived
    //! from the IEEE 1722-2016 Annex B claim walk). Either way the source
    //! degrades to "no DA" — PROBE_TX then answers TALKER_DEST_MAC_FAILED,
    //! which is the honest answer — instead of wedging the talker walker or,
    //! for the second case, stranding allocation for EVERY source while the
    //! processor keeps answering commands normally. A response arriving
    //! after an abandon is swallowed and can never install a DA.
    output logic                         maap_req_valid_o,      //! ALLOC/RELEASE request, held until ready or the accept window
    input  wire                          maap_req_ready_i,      //! allocator accepts the request (tie 0 = no allocator)
    output logic                         maap_req_release_o,    //! 0 = ALLOC_DA, 1 = RELEASE_DA
    output logic [SRC_IDX_W_C-1:0]       maap_req_src_o,        //! source index of the request
    input  wire                          maap_rsp_valid_i,      //! exactly one response per accepted request
    input  wire                          maap_rsp_ok_i,         //! ALLOC_DA succeeded (ignored on RELEASE_DA)
    input  wire  [47:0]                  maap_rsp_da_i,         //! allocated stream destination MAC (with ok)
    input  wire                          maap_conflict_valid_i, //! MAAP_CONFLICT{source} event, sticky until acked
    input  wire  [SRC_IDX_W_C-1:0]       maap_conflict_src_i,   //! conflicted source
    output logic                         maap_conflict_ack_o,   //! event ack (combinational)

    //! ---- observability ----
    output logic [31:0] dbg_now_ms_o           //! absolute ms timebase
);

  // ---- F08.4 timer-slot allocation (08 §5 order; bases are the contract) --
  //! DERIVED, never literals. The 08 §5 ORDER is the contract; the SIZES are
  //! the shape. A literal map is only correct at the F01.5 default shape:
  //! with SI = SO = 9 (the reference platform's 8x8 build, where the CRF
  //! Media Clock Output is itself a source and a sink) the old literals
  //! aliased listener sink 8 onto the talker base and SRP talker 8 onto the
  //! SRP listener base — the first SILENTLY LOSES a deadline (distinct owner
  //! tags, and the ACMP engines filter by owner) and the second MISDELIVERS
  //! one (ADP and SRP filter by slot, so SRP listener 0 would act on SRP
  //! talker 8's expiry). Neither raises an error or moves a counter; on
  //! silicon they read as an intermittent ACMP timeout and a reservation
  //! that leaves at the wrong moment. The guard below is what makes an
  //! over-large shape fail LOUDLY instead.
  localparam int unsigned TMR_ADP_ADV_BASE_C   = TMR_MAP_C.adp_adv;    // IF slots
  localparam int unsigned TMR_ADP_NOADP_BASE_C = TMR_MAP_C.adp_noadp;  // SI slots
  localparam int unsigned TMR_LSTN_BASE_C      = TMR_MAP_C.lstn;       // SI (shared SM)
  localparam int unsigned TMR_TKR_BASE_C       = TMR_MAP_C.tkr;        // SO (DAFRESH/LA2)
  // TMR_MAP_C.regmon (2*CTRL*IF registry monitors, P4), .capool (CA pool,
  // originator) and .single (5 singletons, P4) are reserved and unused this
  // phase — but they are SPACED, so landing them cannot move anything below.
  localparam int unsigned TMR_SRP_CAD_BASE_C   = TMR_MAP_C.srp_cad;    // 5 cadence + 2 fixed
  localparam int unsigned TMR_SRP_TK_BASE_C    = TMR_MAP_C.srp_tk;     // SO registrar-leave
  localparam int unsigned TMR_SRP_LS_BASE_C    = TMR_MAP_C.srp_ls;     // SI registrar-leave

  // owner tags must be disjoint across the shared expiry bus: the listener
  // (0x20 + sink) and talker (0x50 + source) filter BY OWNER; ADP and SRP
  // filter by slot. KL_srp_top's default cadence owner base 0x20 collides
  // with the listener's range, so it is moved to 0x80 here. Unlike the slot
  // map these are a fixed allocation of a fixed 8-bit space (pp_pkg), so the
  // shape is BOUNDED against them by the guard below rather than re-spacing
  // tags that landed engines already publish as their defaults.
  localparam logic [7:0] SRP_CAD_OWNER_C = PP_OWN_SRP_CAD_C;
  localparam logic [7:0] SRP_TK_OWNER_C  = PP_OWN_SRP_TK_C;
  localparam logic [7:0] SRP_LS_OWNER_C  = PP_OWN_SRP_LS_C;

  // ---- elaboration guard: the map must FIT and stay DISJOINT -------------
  //! The numbers above can be re-derived by hand; what nothing did before is
  //! NOTICE when they collide. These are IEEE 1800 §20.11 elaboration system
  //! tasks: an over-large shape stops the build here instead of aliasing a
  //! timer slot or an owner tag in silence.
  //!
  //! ADP publishes its SLOT as its owner tag (KL_adp_engine tmr_arm_owner_o
  //! is a zero-extension of tmr_arm_slot_o), so the ADP group constrains the
  //! owner space as well as the slot space.
  localparam int unsigned OWN_ADP_END_C   = TMR_MAP_C.lstn;                        // ADP owners = ADP slots
  localparam int unsigned OWN_LSTN_END_C  = 32'(PP_OWN_LSTN_C)   + N_STREAM_IN_P;
  localparam int unsigned OWN_SRPTK_END_C = 32'(PP_OWN_SRP_TK_C) + N_STREAM_OUT_P;
  localparam int unsigned OWN_TKR_END_C   = 32'(PP_OWN_TKR_C)    + N_STREAM_OUT_P;
  localparam int unsigned OWN_SRPLS_END_C = 32'(PP_OWN_SRP_LS_C) + N_STREAM_IN_P;
  localparam int unsigned OWN_SRPCAD_END_C = 32'(PP_OWN_SRP_CAD_C) + PP_SRP_CAD_SLOTS_C;

  if (TMR_SLOTS_C > (32'd1 << TMR_AW_C)) begin : gen_g_tmr_aw
    $error("F08.4: TMR_AW_C=%0d cannot index P-TIMER-SLOTS=%0d",
           TMR_AW_C, TMR_SLOTS_C);
  end
  if (TMR_MAP_C.srp_ls + N_STREAM_IN_P > TMR_SLOTS_C) begin : gen_g_tmr_fit
    $error("F08.4: slot map ends at %0d but P-TIMER-SLOTS=%0d",
           TMR_MAP_C.srp_ls + N_STREAM_IN_P, TMR_SLOTS_C);
  end
  if ((TMR_ADP_NOADP_BASE_C < TMR_ADP_ADV_BASE_C + PP_N_IF_C)
      || (TMR_LSTN_BASE_C    < TMR_ADP_NOADP_BASE_C + N_STREAM_IN_P)
      || (TMR_TKR_BASE_C     < TMR_LSTN_BASE_C      + N_STREAM_IN_P)
      || (TMR_MAP_C.regmon   < TMR_TKR_BASE_C       + N_STREAM_OUT_P)
      || (TMR_SRP_CAD_BASE_C < TMR_MAP_C.base_end)
      || (TMR_SRP_TK_BASE_C  < TMR_SRP_CAD_BASE_C   + PP_SRP_CAD_SLOTS_C)
      || (TMR_SRP_LS_BASE_C  < TMR_SRP_TK_BASE_C    + N_STREAM_OUT_P))
  begin : gen_g_tmr_overlap
    $error("F08.4: timer-slot groups OVERLAP at SI=%0d SO=%0d",
           N_STREAM_IN_P, N_STREAM_OUT_P);
  end
  if ((OWN_ADP_END_C   > 32'(PP_OWN_LSTN_C))
      || (OWN_LSTN_END_C  > 32'(PP_OWN_SRP_TK_C))
      || (OWN_SRPTK_END_C > 32'(PP_OWN_TKR_C))
      || (OWN_TKR_END_C   > 32'(PP_OWN_SRP_LS_C))
      || (OWN_SRPLS_END_C > 32'(PP_OWN_SRP_CAD_C))
      || (OWN_SRPCAD_END_C > 32'd256)) begin : gen_g_owner_overlap
    $error("F08.4: owner tags OVERLAP at SI=%0d SO=%0d (8-bit expiry bus)",
           N_STREAM_IN_P, N_STREAM_OUT_P);
  end

  // ---- 08 §4 class budgets fed to the normalizer --------------------------
  localparam logic [15:0] BUDGET_ADP_MS_C  = 16'd4000;  // T-ADP-DELAY bound
  localparam logic [15:0] BUDGET_ACMP_MS_C = 16'd50;    // T-BUDGET-ACMP-RESP
  localparam logic [15:0] BUDGET_AECP_MS_C = 16'd100;   // T-BUDGET-AECP-WC

  // ---- TX arbiter lane map (F03.5) ----------------------------------------
  localparam int unsigned LANE_AECP_SOL_C = 0;  // idle until P4
  localparam int unsigned LANE_AECP_UNS_C = 1;  // idle until P4
  localparam int unsigned LANE_ACMP_C     = 2;  // listener PDUs (56 B)
  localparam int unsigned LANE_ADP_C      = 3;  // whole 82 B frames
  localparam int unsigned LANE_SRP_C      = 4;  // whole MRPDU frames
  localparam int unsigned LANE_TKRSP_C    = 5;  // talker response PDUs (56 B)

  // =========================================================================
  // shared services: timer, PRNG, scoreboard, trace ring, originator
  // =========================================================================
  logic        tick_ms_w;
  logic [31:0] now_ms_w;
  logic        tmr_arm_valid_w, tmr_arm_cancel_w;
  logic [TMR_AW_C-1:0] tmr_arm_slot_w;
  logic [PP_TIMER_OWNER_W_C-1:0] tmr_arm_owner_w;
  logic [31:0] tmr_arm_deadline_w;
  logic        exp_valid_w;
  logic [TMR_AW_C-1:0] exp_slot_w;
  logic [PP_TIMER_OWNER_W_C-1:0] exp_owner_w;

  KL_pp_timer_service #(
      .CLK_HZ_P (CLK_HZ_P),
      .SLOTS_P  (TMR_SLOTS_C),
      .DIV_US_P (TIM_DIV_US_P),
      .DIV_MS_P (TIM_DIV_MS_P)
  ) u_timer (
      .clk_i             (clk_i),
      .rst_n             (rst_n),
      .tick_ms_o         (tick_ms_w),
      .now_ms_o          (now_ms_w),
      .arm_valid_i       (tmr_arm_valid_w),
      .arm_cancel_i      (tmr_arm_cancel_w),
      .arm_slot_i        (tmr_arm_slot_w),
      .arm_owner_i       (tmr_arm_owner_w),
      .arm_deadline_ms_i (tmr_arm_deadline_w),
      .exp_valid_o       (exp_valid_w),
      .exp_slot_o        (exp_slot_w),
      .exp_owner_o       (exp_owner_w)
  );

  assign dbg_now_ms_o = now_ms_w;

  // =========================================================================
  //  class-D fabric face (02 §6). Flat packing: index s at [W*s +: W].
  // =========================================================================
  //! These are plain reads of state this engine already maintains. The only
  //! reason they did not exist is that the interface published none of it —
  //! see the port banner. A `assign flat = packed;` of a [N-1:0][W-1:0] array
  //! onto a [N*W-1:0] vector is bit-exact in this element order.
  assign srp_class_a_prio_o      = srp_class_a_prio_w;
  assign srp_class_a_vid_o       = srp_class_a_vid_w;
  assign srp_domain_adopted_o    = srp_domain_adopted_w;
  assign srp_domain_change_o     = srp_evt_domain_change_w;
  assign srp_tk_decl_state_o     = srp_tk_decl_state_w;
  assign srp_lstn_reg_state_o    = srp_lstn_reg_state_w;
  assign srp_active_o            = srp_active_w;
  assign srp_sr_admitted_o       = srp_sr_admitted_w;
  assign srp_granted_slope_bps_o = srp_granted_slope_w;
  assign srp_src_fail_code_o     = srp_src_fail_code_nc_w;
  assign srp_src_fail_bridge_o   = srp_src_fail_bridge_nc_w;
  assign srp_sum_slope_bps_o     = srp_sum_slope_w;
  assign srp_over_limit_o        = srp_over_limit_w;
  assign srp_tk_reg_state_o      = srp_tk_reg_state_w;
  assign srp_lstn_decl_state_o   = srp_lstn_decl_state_w;
  assign srp_acc_latency_o       = srp_acc_latency_w;
  assign srp_snk_fail_code_o     = srp_snk_fail_code_w;

  assign acmp_declaring_o = tkr_declaring_w;

  //! Debounced binding view. bound_r dips 1->0->1 within ONE executor
  //! transaction on a re-bind, so it is republished held: it may only fall
  //! while the executor is idle. bound_r itself is untouched — KL_adp_engine
  //! edge-detects it and that behaviour must not change.
  logic [N_STREAM_IN_P-1:0] bound_hold_r;
  always_ff @(posedge clk_i) begin : bound_debounce
    if (!rst_n)                    bound_hold_r <= '0;
    else if (!lstn_dbg_busy_nc_w)  bound_hold_r <= bound_r;   // idle: track
    else                           bound_hold_r <= bound_hold_r | bound_r;
  end
  assign acmp_bound_o     = bound_hold_r;
  assign acmp_bound_eid_o  = bound_eid_r;
  assign acmp_bound_sid_o  = bound_sid_r;
  assign acmp_bound_dmac_o = bound_dmac_r;
  assign acmp_bound_vlan_o = bound_vlan_r;

  //! The ADP engine's dbg_avail_index is the PRE-INCREMENT value — the index
  //! the next ENTITY_AVAILABLE will actually carry, which is what a consumer
  //! wants to publish. Full width: see the port comment.
  assign adp_next_avail_index_o = adp_dbg_aidx_nc_w;

  logic        prng_req_w;
  logic [2:0]  prng_kind_w;
  logic        prng_busy_w, prng_valid_w;
  logic [15:0] prng_ms_w;
  logic [63:0] prng_lfsr_nc_w;
  logic        prng_seeded_w;

  KL_pp_prng u_prng (
      .clk_i        (clk_i),
      .rst_n        (rst_n),
      .entity_id_i  (entity_id_i),
      .link_up_i    (link_up_i),
      .draw_req_i   (prng_req_w),
      .draw_kind_i  (prng_kind_w),
      .draw_busy_o  (prng_busy_w),
      .draw_valid_o (prng_valid_w),
      .draw_ms_o    (prng_ms_w),
      .dbg_lfsr_o   (prng_lfsr_nc_w),
      .dbg_seeded_o (prng_seeded_w)
  );

  // scoreboard: instantiated per contract; the admission port is exercised
  // by the P4 AECP engine — until then the faces are DEFINED-idle and only
  // the status lanes feed the snapshot window.
  logic       sb_gnt_nc_w;
  logic [2:0] sb_id_nc_w;
  logic       sb_kill_ack_nc_w;
  logic [7:0] sb_holds_w;
  logic       sb_full_w, sb_barrier_w;

  KL_pp_scoreboard #(.MAX_HOLDS_P(8)) u_scoreboard (
      .clk_i              (clk_i),
      .rst_n              (rst_n),
      .adm_req_i          (1'b0),
      .adm_class_i        (4'd0),
      .adm_key_i          (16'd0),
      .adm_gnt_o          (sb_gnt_nc_w),
      .adm_id_o           (sb_id_nc_w),
      .rel_valid_i        (1'b0),
      .rel_id_i           (3'd0),
      .kill_valid_i       (1'b0),
      .kill_id_i          (3'd0),
      .kill_resp_queued_i (1'b0),
      .kill_ack_o         (sb_kill_ack_nc_w),
      .holds_o            (sb_holds_w),
      .full_o             (sb_full_w),
      .barrier_pend_o     (sb_barrier_w)
  );

  logic         trc_wr_valid_w;
  logic [127:0] trc_wr_data_w;
  logic [15:0]  trc_wr_count_w;
  logic         trc_rd_en_w;
  logic [7:0]   trc_rd_addr_w;
  logic [1:0]   trc_rd_lane_w;
  logic [31:0]  trc_rd_data_w;

  KL_pp_trace_ring #(.RECORDS_P(256), .RECORD_W_P(128)) u_trace (
      .clk_i      (clk_i),
      .rst_n      (rst_n),
      .wr_valid_i (trc_wr_valid_w),
      .wr_data_i  (trc_wr_data_w),
      .wr_count_o (trc_wr_count_w),
      .rd_en_i    (trc_rd_en_w),
      .rd_addr_i  (trc_rd_addr_w),
      .rd_lane_i  (trc_rd_lane_w),
      .rd_data_o  (trc_rd_data_w)
  );

  // originator: wired per contract, issue side DEFINED-idle — the ACMP
  // listener originates its own probes (05) and CA probing is a P4 flow.
  logic        org_iss_ready_nc_w, org_iss_gnt_nc_w;
  logic [15:0] org_iss_seq_nc_w;
  logic [3:0]  org_iss_id_nc_w;
  logic        org_rt_valid_nc_w, org_fail_valid_nc_w;
  logic [3:0]  org_rt_owner_nc_w, org_rt_id_nc_w;
  logic [3:0]  org_fail_owner_nc_w, org_fail_id_nc_w;
  logic        org_send_valid_nc_w, org_resend_valid_nc_w;
  logic [2:0]  org_send_slot_nc_w, org_resend_slot_nc_w;
  logic        org_hold_valid_nc_w, org_release_valid_nc_w;
  logic [2:0]  org_hold_slot_nc_w, org_release_slot_nc_w;
  logic        org_arm_valid_w, org_arm_cancel_w;
  logic [TMR_AW_C-1:0] org_arm_slot_w;
  logic [PP_TIMER_OWNER_W_C-1:0] org_arm_owner_w;
  logic [31:0] org_arm_deadline_w;
  logic [7:0]  org_rsp_ign_nc_w;
  logic [11:0] org_busy_nc_w;

  KL_pp_originator #(
      .TMR_SLOTS_P (TMR_SLOTS_C)
  ) u_originator (
      .clk_i                 (clk_i),
      .rst_n                 (rst_n),
      .iss_valid_i           (1'b0),
      .iss_owner_i           (4'd0),
      .iss_tx_slot_i         (3'd0),
      .iss_key_i             (16'd0),
      .iss_tmr_slot_i        ({TMR_AW_C{1'b0}}),
      .iss_timeout_ms_i      (16'd0),
      .iss_ready_o           (org_iss_ready_nc_w),
      .iss_gnt_o             (org_iss_gnt_nc_w),
      .iss_seq_o             (org_iss_seq_nc_w),
      .iss_id_o              (org_iss_id_nc_w),
      .rsp_valid_i           (1'b0),
      .rsp_seq_i             (16'd0),
      .rsp_key_i             (16'd0),
      .rt_valid_o            (org_rt_valid_nc_w),
      .rt_owner_o            (org_rt_owner_nc_w),
      .rt_id_o               (org_rt_id_nc_w),
      .fail_valid_o          (org_fail_valid_nc_w),
      .fail_owner_o          (org_fail_owner_nc_w),
      .fail_id_o             (org_fail_id_nc_w),
      .send_valid_o          (org_send_valid_nc_w),
      .send_slot_o           (org_send_slot_nc_w),
      .resend_valid_o        (org_resend_valid_nc_w),
      .resend_slot_o         (org_resend_slot_nc_w),
      .hold_valid_o          (org_hold_valid_nc_w),
      .hold_slot_o           (org_hold_slot_nc_w),
      .release_valid_o       (org_release_valid_nc_w),
      .release_slot_o        (org_release_slot_nc_w),
      .tmr_arm_valid_o       (org_arm_valid_w),
      .tmr_arm_cancel_o      (org_arm_cancel_w),
      .tmr_arm_slot_o        (org_arm_slot_w),
      .tmr_arm_owner_o       (org_arm_owner_w),
      .tmr_arm_deadline_ms_o (org_arm_deadline_w),
      .now_ms_i              (now_ms_w),
      .exp_valid_i           (exp_valid_w),
      .exp_slot_i            (exp_slot_w),
      .exp_owner_i           (exp_owner_w),
      .rsp_ign_cnt_o         (org_rsp_ign_nc_w),
      .inflight_busy_o       (org_busy_nc_w)
  );

  // =========================================================================
  // RX: validator + replicated slot pools
  // =========================================================================
  logic        v_mrp_valid_w, v_mrp_last_w;
  logic [7:0]  v_mrp_data_w;
  logic        v_alloc_req_w, v_alloc_gnt_w;
  logic [RXS_W_C-1:0] v_alloc_slot_w;
  logic        v_wr_valid_w, v_wr_last_w, v_wr_abort_w, v_wr_commit_w;
  logic [7:0]  v_wr_data_w;
  logic        v_hdr_valid_w;
  logic [2:0]  v_hdr_protocol_w;
  logic [3:0]  v_hdr_msg_type_w;
  logic [4:0]  v_hdr_status_w;
  logic [10:0] v_hdr_cdl_w;
  logic [47:0] v_hdr_src_mac_w;
  logic [63:0] v_hdr_ctlr_eid_w, v_hdr_target_eid_w;
  logic [15:0] v_hdr_seq_w, v_hdr_opcode_w;
  logic        v_hdr_u_w, v_hdr_cr_w;
  pp_operands_t v_hdr_operands_w;
  logic [2:0]  v_hdr_rx_slot_w;
  logic [15:0] cnt_rx_da_w, cnt_rx_ethertype_w, cnt_rx_subtype_w;
  logic [15:0] cnt_rx_version_w, cnt_rx_length_w;

  KL_pp_rx_validator #(
      .SLOTS_P (RX_SLOTS_P),
      .BYTES_P (RX_SLOT_BYTES_P)
  ) u_rx_validator (
      .clk_i                (clk_i),
      .rst_n                (rst_n),
      .rx_valid_i           (rx_valid_i),
      .rx_data_i            (rx_data_i),
      .rx_last_i            (rx_last_i),
      .own_mac_i            (own_mac_i),
      .mrp_valid_o          (v_mrp_valid_w),
      .mrp_data_o           (v_mrp_data_w),
      .mrp_last_o           (v_mrp_last_w),
      .alloc_req_o          (v_alloc_req_w),
      .alloc_gnt_i          (v_alloc_gnt_w),
      .alloc_slot_i         (v_alloc_slot_w),
      .wr_valid_o           (v_wr_valid_w),
      .wr_data_o            (v_wr_data_w),
      .wr_last_o            (v_wr_last_w),
      .wr_abort_o           (v_wr_abort_w),
      .wr_commit_o          (v_wr_commit_w),
      .hdr_valid_o          (v_hdr_valid_w),
      .hdr_protocol_o       (v_hdr_protocol_w),
      .hdr_msg_type_o       (v_hdr_msg_type_w),
      .hdr_status_o         (v_hdr_status_w),
      .hdr_cdl_o            (v_hdr_cdl_w),
      .hdr_src_mac_o        (v_hdr_src_mac_w),
      .hdr_controller_eid_o (v_hdr_ctlr_eid_w),
      .hdr_target_eid_o     (v_hdr_target_eid_w),
      .hdr_sequence_id_o    (v_hdr_seq_w),
      .hdr_u_o              (v_hdr_u_w),
      .hdr_cr_o             (v_hdr_cr_w),
      .hdr_opcode_o         (v_hdr_opcode_w),
      .hdr_operands_o       (v_hdr_operands_w),
      .hdr_rx_slot_o        (v_hdr_rx_slot_w),
      .rx_da_count_o        (cnt_rx_da_w),
      .rx_ethertype_count_o (cnt_rx_ethertype_w),
      .rx_subtype_count_o   (cnt_rx_subtype_w),
      .rx_version_count_o   (cnt_rx_version_w),
      .rx_length_count_o    (cnt_rx_length_w)
  );

  // ---- RX free-return queue: 4 clients -> one serialized broadcast --------
  // Each engine frees at most once per processed frame (many cycles apart),
  // so a 1-deep latch per client drains within 4 cycles; a same-client
  // overrun is counted, never silent.
  localparam int unsigned RXF_ADP_C  = 0;
  localparam int unsigned RXF_LSTN_C = 1;
  localparam int unsigned RXF_TKR_C  = 2;
  localparam int unsigned RXF_AECP_C = 3;

  logic [3:0]              rxf_vld_r;
  logic [3:0][RXS_W_C-1:0] rxf_slot_r;
  logic [15:0]             rxf_drop_r;
  logic                    rxf_free_w;
  logic [RXS_W_C-1:0]      rxf_free_slot_w;
  logic [1:0]              rxf_pick_ix_w;
  logic [3:0]              rxf_req_w;
  logic [3:0][RXS_W_C-1:0] rxf_req_slot_w;

  logic adp_rxs_free_w,  lstn_rxs_free_w,  tkr_rxs_free_w;
  logic [RXS_W_C-1:0] adp_rxs_free_slot_w, lstn_rxs_free_slot_w,
                      tkr_rxs_free_slot_w;

  assign rxf_req_w      = {aecp_rxs_free_i, tkr_rxs_free_w,
                           lstn_rxs_free_w, adp_rxs_free_w};
  assign rxf_req_slot_w = {aecp_rxs_free_slot_i, tkr_rxs_free_slot_w,
                           lstn_rxs_free_slot_w, adp_rxs_free_slot_w};

  always_comb begin : rxf_pick
    rxf_free_w      = 1'b0;
    rxf_pick_ix_w   = 2'd0;
    rxf_free_slot_w = '0;
    for (int unsigned i = 0; i < 4; i++) begin
      if (!rxf_free_w && rxf_vld_r[i]) begin
        rxf_free_w      = 1'b1;
        rxf_pick_ix_w   = 2'(i);
        rxf_free_slot_w = rxf_slot_r[i];
      end
    end
  end

  always_ff @(posedge clk_i) begin : rxf_queue
    if (!rst_n) begin
      rxf_vld_r  <= '0;
      rxf_slot_r <= '0;
      rxf_drop_r <= 16'd0;
    end else begin
      // drain the picked entry (lowest valid index; broadcast this cycle)
      if (rxf_free_w) begin
        rxf_vld_r[rxf_pick_ix_w] <= 1'b0;
      end
      // capture new frees (a later assignment wins: a same-cycle refill of
      // the drained entry lands correctly)
      for (int unsigned i = 0; i < 4; i++) begin
        if (rxf_req_w[i]) begin
          if (rxf_vld_r[i] && !(rxf_free_w && (rxf_pick_ix_w == 2'(i)))) begin
            if (rxf_drop_r != 16'hFFFF) rxf_drop_r <= rxf_drop_r + 16'd1;
          end else begin
            rxf_vld_r[i]  <= 1'b1;
            rxf_slot_r[i] <= rxf_req_slot_w[i];
          end
        end
      end
    end
  end

  // ---- the five lock-step pool replicas (see file banner): four readers
  // plus the ACMP steer prefetch below
  localparam int unsigned RXP_ADP_C   = 0;
  localparam int unsigned RXP_LSTN_C  = 1;
  localparam int unsigned RXP_TKR_C   = 2;
  localparam int unsigned RXP_AECP_C  = 3;
  localparam int unsigned RXP_STEER_C = 4;

  logic [4:0]              rxp_alloc_gnt_w;
  logic [4:0][RXS_W_C-1:0] rxp_alloc_slot_w;
  logic [4:0][RXS_W_C-1:0] rxp_rd_slot_w;
  logic [4:0][RXA_W_C-1:0] rxp_rd_addr_w;
  logic [4:0]              rxp_rd_en_w;
  logic [4:0][7:0]         rxp_rd_data_w;
  logic [4:0][RXL_W_C-1:0] rxp_slot_len_w;
  logic [4:0][$clog2(RX_SLOTS_P+1)-1:0] rxp_slots_free_w;
  logic [4:0][15:0]        rxp_overrun_w;

  for (genvar g = 0; g < 5; g++) begin : g_rx_pool
    KL_pp_rx_slots #(
        .SLOTS_P (RX_SLOTS_P),
        .BYTES_P (RX_SLOT_BYTES_P)
    ) u_rx_slots (
        .clk_i              (clk_i),
        .rst_n              (rst_n),
        .alloc_req_i        (v_alloc_req_w),
        .alloc_gnt_o        (rxp_alloc_gnt_w[g]),
        .alloc_slot_o       (rxp_alloc_slot_w[g]),
        .wr_valid_i         (v_wr_valid_w),
        .wr_data_i          (v_wr_data_w),
        .wr_last_i          (v_wr_last_w),
        .wr_abort_i         (v_wr_abort_w),
        .wr_commit_i        (v_wr_commit_w),
        .rd_slot_i          (rxp_rd_slot_w[g]),
        .rd_addr_i          (rxp_rd_addr_w[g]),
        .rd_en_i            (rxp_rd_en_w[g]),
        .rd_data_o          (rxp_rd_data_w[g]),
        .slot_len_o         (rxp_slot_len_w[g]),
        .free_i             (rxf_free_w),
        .free_slot_i        (rxf_free_slot_w),
        .slots_free_o       (rxp_slots_free_w[g]),
        .rx_overrun_count_o (rxp_overrun_w[g])
    );
  end

  assign v_alloc_gnt_w  = rxp_alloc_gnt_w[RXP_ADP_C];
  assign v_alloc_slot_w = rxp_alloc_slot_w[RXP_ADP_C];

  assign aecp_rxs_rd_data_o  = rxp_rd_data_w[RXP_AECP_C];
  assign aecp_rxs_slot_len_o = rxp_slot_len_w[RXP_AECP_C];
  assign rxp_rd_slot_w[RXP_AECP_C] = aecp_rxs_rd_slot_i;
  assign rxp_rd_addr_w[RXP_AECP_C] = aecp_rxs_rd_addr_i;
  assign rxp_rd_en_w[RXP_AECP_C]   = aecp_rxs_rd_en_i;

  // =========================================================================
  // hdr-beat latch -> normalizer -> dispatch
  // =========================================================================
  // The validator emits a ONE-CYCLE parsed-header beat with wr_commit; the
  // normalizer's RX producer face holds until accepted. Frames are >= 26
  // wire bytes apart while the normalizer accepts within a 4-producer
  // round, so one holding register suffices; an overrun is counted.
  logic        hdr_vld_r;
  logic [2:0]  hdr_protocol_r;
  logic [3:0]  hdr_msg_type_r;
  logic [4:0]  hdr_status_r;
  logic [10:0] hdr_cdl_r;
  logic [47:0] hdr_src_mac_r;
  logic [63:0] hdr_ctlr_eid_r, hdr_target_eid_r;
  logic [15:0] hdr_seq_r, hdr_opcode_r;
  logic        hdr_u_r, hdr_cr_r;
  logic [63:0] hdr_operands_r;
  logic [2:0]  hdr_rx_slot_r;
  logic [15:0] hdr_drop_r;
  logic        nrm_rx_ready_w;

  always_ff @(posedge clk_i) begin : hdr_latch
    if (!rst_n) begin
      hdr_vld_r      <= 1'b0;
      hdr_protocol_r <= 3'd0;
      hdr_msg_type_r <= 4'd0;
      hdr_status_r   <= 5'd0;
      hdr_cdl_r      <= 11'd0;
      hdr_src_mac_r  <= 48'd0;
      hdr_ctlr_eid_r <= 64'd0;
      hdr_target_eid_r <= 64'd0;
      hdr_seq_r      <= 16'd0;
      hdr_opcode_r   <= 16'd0;
      hdr_u_r        <= 1'b0;
      hdr_cr_r       <= 1'b0;
      hdr_operands_r <= 64'd0;
      hdr_rx_slot_r  <= 3'd0;
      hdr_drop_r     <= 16'd0;
    end else begin
      if (hdr_vld_r && nrm_rx_ready_w) begin
        hdr_vld_r <= 1'b0;
      end
      if (v_hdr_valid_w) begin
        if (hdr_vld_r && !nrm_rx_ready_w) begin
          if (hdr_drop_r != 16'hFFFF) hdr_drop_r <= hdr_drop_r + 16'd1;
        end else begin
          hdr_vld_r        <= 1'b1;
          hdr_protocol_r   <= v_hdr_protocol_w;
          hdr_msg_type_r   <= v_hdr_msg_type_w;
          hdr_status_r     <= v_hdr_status_w;
          hdr_cdl_r        <= v_hdr_cdl_w;
          hdr_src_mac_r    <= v_hdr_src_mac_w;
          hdr_ctlr_eid_r   <= v_hdr_ctlr_eid_w;
          hdr_target_eid_r <= v_hdr_target_eid_w;
          hdr_seq_r        <= v_hdr_seq_w;
          hdr_opcode_r     <= v_hdr_opcode_w;
          hdr_u_r          <= v_hdr_u_w;
          hdr_cr_r         <= v_hdr_cr_w;
          hdr_operands_r   <= 64'(v_hdr_operands_w);
          hdr_rx_slot_r    <= v_hdr_rx_slot_w;
        end
      end
    end
  end

  // dispatch-ROM stub (banner): hazard class from a protocol-keyed default
  // until the P4 dispatch ROM lands — ACMP serializes per stream class,
  // everything else reads-only. The key spreads protocols apart so classes
  // never serialize across protocols by accident.
  logic        hz_valid_nc_w;
  logic [2:0]  hz_protocol_w;
  logic [15:0] hz_opcode_nc_w;
  logic [3:0]  hz_class_w;
  logic [15:0] hz_key_w;

  always_comb begin : hz_stub
    unique case (hz_protocol_w)
      3'(PP_PROTO_ACMP): hz_class_w = 4'(PP_HZ_STREAM_CFG);
      default:           hz_class_w = 4'(PP_HZ_RO_SNAPSHOT);
    endcase
    hz_key_w = {13'd0, hz_protocol_w};
  end

  logic                  nrm_txn_valid_w;
  logic [PP_TXN_W_C-1:0] nrm_txn_w;
  logic                  nrm_txn_ready_w;
  logic nrm_tmr_ready_nc_w, nrm_self_ready_nc_w, nrm_mgmt_ready_nc_w;

  KL_pp_normalizer u_normalizer (
      .clk_i               (clk_i),
      .rst_n               (rst_n),
      .now_ms_i            (now_ms_w),
      .budget_adp_ms_i     (BUDGET_ADP_MS_C),
      .budget_acmp_ms_i    (BUDGET_ACMP_MS_C),
      .budget_aecp_ms_i    (BUDGET_AECP_MS_C),
      .rx_valid_i          (hdr_vld_r),
      .rx_ready_o          (nrm_rx_ready_w),
      .rx_if_index_i       (2'd0),
      .rx_protocol_i       (hdr_protocol_r),
      .rx_msg_type_i       (hdr_msg_type_r),
      .rx_status_i         (hdr_status_r),
      .rx_cdl_i            (hdr_cdl_r),
      .rx_src_mac_i        (hdr_src_mac_r),
      .rx_controller_eid_i (hdr_ctlr_eid_r),
      .rx_target_eid_i     (hdr_target_eid_r),
      .rx_sequence_id_i    (hdr_seq_r),
      .rx_u_i              (hdr_u_r),
      .rx_cr_i             (hdr_cr_r),
      .rx_opcode_i         (hdr_opcode_r),
      .rx_operands_i       (hdr_operands_r),
      .rx_slot_i           (hdr_rx_slot_r),
      .hz_valid_o          (hz_valid_nc_w),
      .hz_protocol_o       (hz_protocol_w),
      .hz_opcode_o         (hz_opcode_nc_w),
      .hz_class_i          (hz_class_w),
      .hz_key_i            (hz_key_w),
      .tmr_valid_i         (1'b0),                 // TIMER producer: P4
      .tmr_txn_i           ({PP_TXN_W_C{1'b0}}),
      .tmr_ready_o         (nrm_tmr_ready_nc_w),
      .self_valid_i        (1'b0),                 // SELF producer: P4 (CA)
      .self_txn_i          ({PP_TXN_W_C{1'b0}}),
      .self_ready_o        (nrm_self_ready_nc_w),
      .mgmt_valid_i        (1'b0),                 // MGMT producer: P4
      .mgmt_txn_i          ({PP_TXN_W_C{1'b0}}),
      .mgmt_ready_o        (nrm_mgmt_ready_nc_w),
      .txn_valid_o         (nrm_txn_valid_w),
      .txn_o               (nrm_txn_w),
      .txn_ready_i         (nrm_txn_ready_w)
  );

  logic                  adp_txn_valid_w, acmp_txn_valid_w;
  logic [PP_TXN_W_C-1:0] adp_txn_w, acmp_txn_w;
  logic                  adp_txn_ready_w, acmp_txn_ready_w;
  logic [7:0]  disp_adp_level_w, disp_acmp_level_w, disp_aecp_level_w;
  logic [15:0] disp_adp_stall_w, disp_acmp_stall_w, disp_aecp_stall_w;

  KL_pp_dispatch u_dispatch (
      .clk_i              (clk_i),
      .rst_n              (rst_n),
      .enq_valid_i        (nrm_txn_valid_w),
      .enq_txn_i          (nrm_txn_w),
      .enq_ready_o        (nrm_txn_ready_w),
      .adp_txn_valid_o    (adp_txn_valid_w),
      .adp_txn_o          (adp_txn_w),
      .adp_txn_ready_i    (adp_txn_ready_w),
      .acmp_txn_valid_o   (acmp_txn_valid_w),
      .acmp_txn_o         (acmp_txn_w),
      .acmp_txn_ready_i   (acmp_txn_ready_w),
      .aecp_txn_valid_o   (aecp_txn_valid_o),
      .aecp_txn_o         (aecp_txn_o),
      .aecp_txn_ready_i   (aecp_txn_ready_i),
      .adp_level_o        (disp_adp_level_w),
      .acmp_level_o       (disp_acmp_level_w),
      .aecp_level_o       (disp_aecp_level_w),
      .adp_stall_count_o  (disp_adp_stall_w),
      .acmp_stall_count_o (disp_acmp_stall_w),
      .aecp_stall_count_o (disp_aecp_stall_w)
  );

  // ---- ACMP pop steer (RECORDED SEAM): talker commands {0,2,4,12} to the
  // talker, everything else (BIND/UNBIND/GET_RX + probe responses) to the
  // listener. The validator's F03.4 extraction fills txn.target_eid with
  // the ACMPDU's @4 stream_id, while BOTH engines discriminate multicast
  // ACMP traffic on target_eid == the ADDRESSED entity id (listener_
  // entity_id @28 / talker_entity_id @20, PDU offsets). The steer therefore
  // PREFETCHES those eight bytes from its own pool replica and presents the
  // head with target_eid rewritten from the wire — the discriminator stays
  // wire-true, no engine or validator file is touched (doc conflict
  // reported; 03 §4 only says "as applicable").
  pp_txn_t acmp_head_w;
  pp_txn_t steer_txn_w;
  logic    acmp_is_tkr_w;
  logic    lstn_txn_ready_w, tkr_txn_ready_w;

  typedef enum logic [1:0] { PF_WAIT, PF_RD, PF_DONE } pf_state_e;
  pf_state_e   pf_st_r;
  logic [3:0]  pf_idx_r;
  logic [63:0] pf_eid_r;
  logic        pf_ready_w;

  assign acmp_head_w   = pp_txn_t'(acmp_txn_w);
  assign acmp_is_tkr_w = (acmp_head_w.msg_type == 4'd0)
                       || (acmp_head_w.msg_type == 4'd2)
                       || (acmp_head_w.msg_type == 4'd4)
                       || (acmp_head_w.msg_type == 4'd12);
  assign pf_ready_w = (pf_st_r == PF_DONE);

  // engines see the head only once the prefetch stands; their level-high
  // idle readies pop the dispatch head exactly at their consume cycle
  assign acmp_txn_ready_w = pf_ready_w
                          && (acmp_is_tkr_w ? tkr_txn_ready_w
                                            : lstn_txn_ready_w);

  assign rxp_rd_slot_w[RXP_STEER_C] = acmp_head_w.rx_slot[RXS_W_C-1:0];
  assign rxp_rd_addr_w[RXP_STEER_C] =
      RXA_W_C'((acmp_is_tkr_w ? 32'd20 : 32'd28) + 32'(pf_idx_r));
  assign rxp_rd_en_w[RXP_STEER_C]   = (pf_st_r == PF_RD) && (pf_idx_r < 4'd8);

  always_ff @(posedge clk_i) begin : steer_prefetch
    if (!rst_n) begin
      pf_st_r  <= PF_WAIT;
      pf_idx_r <= 4'd0;
      pf_eid_r <= 64'd0;
    end else begin
      unique case (pf_st_r)
        PF_WAIT: begin
          if (acmp_txn_valid_w) begin
            if (acmp_head_w.rx_slot == PP_SLOT_NULL_C) begin
              pf_eid_r <= acmp_head_w.target_eid;  // no payload: pass as-is
              pf_st_r  <= PF_DONE;
            end else begin
              pf_idx_r <= 4'd0;
              pf_st_r  <= PF_RD;
            end
          end
        end
        PF_RD: begin
          pf_idx_r <= pf_idx_r + 4'd1;
          if (pf_idx_r != 4'd0) begin     // sync read: byte k lands at k+1
            pf_eid_r <= {pf_eid_r[55:0], rxp_rd_data_w[RXP_STEER_C]};
          end
          if (pf_idx_r == 4'd8) begin
            pf_st_r <= PF_DONE;
          end
        end
        PF_DONE: begin
          if (acmp_txn_valid_w && acmp_txn_ready_w) begin
            pf_st_r <= PF_WAIT;           // head popped; next head refetches
          end
        end
        default: pf_st_r <= PF_WAIT;
      endcase
    end
  end

  always_comb begin : steer_head
    steer_txn_w            = acmp_head_w;
    steer_txn_w.target_eid = pf_eid_r;
  end

  // =========================================================================
  // engines: ADP, ACMP listener, ACMP talker
  // =========================================================================
  // ---- binding view (07 §4): realized as top-held registers driven by the
  // listener's A4/A9 discovery strobes, read as levels by the ADP engine.
  logic [N_STREAM_IN_P-1:0]        bound_r;
  logic [N_STREAM_IN_P-1:0][63:0]  bound_sid_r;
  logic [N_STREAM_IN_P-1:0][47:0]  bound_dmac_r;
  logic [N_STREAM_IN_P-1:0][11:0]  bound_vlan_r;
  logic [N_STREAM_IN_P-1:0][63:0]  bound_eid_r;

  logic        lstn_disc_arm_w, lstn_disc_disarm_w;
  logic [63:0] lstn_disc_eid_w;
  //! CLAMPED like the engine's own SINK_W_C (KL_pp_acmp_listener.sv:76):
  //! at N_STREAM_IN_P = 1, $clog2(1) = 0 and a bare $clog2-1:0 declares
  //! [-1:0]. The consumer elaborates this processor at N_STREAMS = 1 on the
  //! shipping AX7101 shape, so that is not a hypothetical corner.
  logic [SINK_IDX_W_C-1:0] lstn_act_sink_w;

  always_ff @(posedge clk_i) begin : binding_view
    if (!rst_n) begin
      bound_r      <= '0;
      bound_eid_r  <= '0;
      bound_sid_r  <= '0;
      bound_dmac_r <= '0;
      bound_vlan_r <= '0;
    end else begin
      if (lstn_disc_arm_w) begin
        bound_r[lstn_act_sink_w]     <= 1'b1;
        bound_eid_r[lstn_act_sink_w] <= lstn_disc_eid_w;
      end
      //! The A15 settle carries the stream's wire identity. Latch it beside
      //! the entity_id so a fabric can arm an RX filter from one coherent
      //! view instead of reconstructing it from the ACMPDU itself.
      if (lstn_act_settle_w) begin
        bound_sid_r [lstn_act_sink_w] <= lstn_act_settle_sid_w;
        bound_dmac_r[lstn_act_sink_w] <= lstn_act_settle_da_w;
        bound_vlan_r[lstn_act_sink_w] <= lstn_act_settle_vlan_w;
      end
      if (lstn_disc_disarm_w) begin
        bound_r[lstn_act_sink_w] <= 1'b0;
        //! CLEAR THE STREAM IDENTITY WITH THE BINDING. Leaving it behind is
        //! not a harmless stale debug value: acmp_bound_o is DEBOUNCED, so
        //! across an unbind+rebind it can stay high while these still name
        //! the PREVIOUS stream — and a fabric arms its RX filter, stream
        //! table and CRF receiver from them. Stale stream-X parameters must
        //! never describe stream Y (Milan §5.3.8.9).
        bound_sid_r [lstn_act_sink_w] <= 64'd0;
        bound_dmac_r[lstn_act_sink_w] <= 48'd0;
        bound_vlan_r[lstn_act_sink_w] <= 12'd0;
      end
    end
  end

  // ADP faces
  logic        adp_prng_req_w;
  logic [2:0]  adp_prng_kind_w;
  logic        adp_prng_busy_w, adp_prng_valid_w;
  logic        adp_arm_valid_w, adp_arm_cancel_w;
  logic [TMR_AW_C-1:0] adp_arm_slot_w;
  logic [PP_TIMER_OWNER_W_C-1:0] adp_arm_owner_w;
  logic [31:0] adp_arm_deadline_w;
  logic        adp_txs_alloc_req_w, adp_txs_oversize_nc_w;
  logic [TXS_W_C-1:0] adp_txs_wr_slot_w;
  logic [TXA_W_C-1:0] adp_txs_wr_addr_w, adp_txs_wr_len_w;
  logic        adp_txs_wr_valid_w, adp_txs_wr_commit_w;
  logic [7:0]  adp_txs_wr_data_w;
  logic        adp_txreq_valid_w;
  logic [TXS_W_C-1:0] adp_txreq_slot_w;
  logic [0:0]  adp_txreq_if_nc_w;
  logic        adp_evt_valid_w, adp_evt_departed_w;
  logic [SINK_IDX_W_C-1:0] adp_evt_sink_w;  //! CLAMPED (KL_adp_engine SNK_W_C)
  logic [0:0]  adp_gm_tick_nc_w;
  logic [1:0]  adp_dbg_adv_state_w;
  //! not "nc" any more: this is the live available_index, published below.
  logic [31:0] adp_dbg_aidx_nc_w;
  logic [N_STREAM_IN_P-1:0] adp_dbg_tkdisc_nc_w;
  logic        adp_txs_gnt_w;
  logic [TXS_W_C-1:0] adp_txs_gnt_slot_w;

  KL_adp_engine #(
      .N_IF_P                (1),
      .N_SINK_P              (N_STREAM_IN_P),
      .TMR_SLOTS_P           (TMR_SLOTS_C),
      .TMR_SLOT_ADV_BASE_P   (TMR_ADP_ADV_BASE_C),
      .TMR_SLOT_NOADP_BASE_P (TMR_ADP_NOADP_BASE_C),
      .RX_SLOTS_P            (RX_SLOTS_P),
      .RX_BYTES_P            (RX_SLOT_BYTES_P),
      .TX_STD_SLOTS_P        (TX_STD_SLOTS_P),
      .TX_OVERSIZE_BYTES_P   (TX_OVERSIZE_BYTES_P)
  ) u_adp (
      .clk_i                 (clk_i),
      .rst_n                 (rst_n),
      .entity_enable_i       (entity_enable_i),
      .link_up_i             (link_up_i),
      .gm_change_i           (gm_change_i),
      .gm_id_i               (gm_id_i),
      .gptp_domain_i         (gptp_domain_i),
      .entity_id_i           (entity_id_i),
      .entity_model_id_i     (entity_model_id_i),
      .own_mac_i             (own_mac_i),
      .talker_sources_i      (talker_sources_i),
      .talker_caps_i         (talker_caps_i),
      .listener_sinks_i      (listener_sinks_i),
      .listener_caps_i       (listener_caps_i),
      .current_cfg_i         (current_cfg_i),
      .identify_index_i      (identify_index_i),
      .txn_valid_i           (adp_txn_valid_w),
      .txn_i                 (adp_txn_w),
      .txn_ready_o           (adp_txn_ready_w),
      .rxs_rd_slot_o         (rxp_rd_slot_w[RXP_ADP_C]),
      .rxs_rd_addr_o         (rxp_rd_addr_w[RXP_ADP_C]),
      .rxs_rd_en_o           (rxp_rd_en_w[RXP_ADP_C]),
      .rxs_rd_data_i         (rxp_rd_data_w[RXP_ADP_C]),
      .rxs_free_o            (adp_rxs_free_w),
      .rxs_free_slot_o       (adp_rxs_free_slot_w),
      .prng_draw_req_o       (adp_prng_req_w),
      .prng_draw_kind_o      (adp_prng_kind_w),
      .prng_draw_busy_i      (adp_prng_busy_w),
      .prng_draw_valid_i     (adp_prng_valid_w),
      .prng_draw_ms_i        (prng_ms_w),
      .now_ms_i              (now_ms_w),
      .tmr_arm_valid_o       (adp_arm_valid_w),
      .tmr_arm_cancel_o      (adp_arm_cancel_w),
      .tmr_arm_slot_o        (adp_arm_slot_w),
      .tmr_arm_owner_o       (adp_arm_owner_w),
      .tmr_arm_deadline_ms_o (adp_arm_deadline_w),
      .tmr_exp_valid_i       (exp_valid_w),
      .tmr_exp_slot_i        (exp_slot_w),
      .tmr_exp_owner_i       (exp_owner_w),
      .txs_alloc_req_o       (adp_txs_alloc_req_w),
      .txs_oversize_o        (adp_txs_oversize_nc_w),
      .txs_alloc_gnt_i       (adp_txs_gnt_w),
      .txs_alloc_slot_i      (adp_txs_gnt_slot_w),
      .txs_wr_slot_o         (adp_txs_wr_slot_w),
      .txs_wr_addr_o         (adp_txs_wr_addr_w),
      .txs_wr_valid_o        (adp_txs_wr_valid_w),
      .txs_wr_data_o         (adp_txs_wr_data_w),
      .txs_wr_commit_o       (adp_txs_wr_commit_w),
      .txs_wr_len_o          (adp_txs_wr_len_w),
      .txreq_valid_o         (adp_txreq_valid_w),
      .txreq_slot_o          (adp_txreq_slot_w),
      .txreq_if_o            (adp_txreq_if_nc_w),
      .bound_i               (bound_r),
      .bound_talker_eid_i    (bound_eid_r),
      .evt_valid_o           (adp_evt_valid_w),
      .evt_departed_o        (adp_evt_departed_w),
      .evt_sink_o            (adp_evt_sink_w),
      .gm_changed_tick_o     (adp_gm_tick_nc_w),
      .dbg_adv_state_o       (adp_dbg_adv_state_w),
      .dbg_avail_index_o     (adp_dbg_aidx_nc_w),
      .dbg_tk_discovered_o   (adp_dbg_tkdisc_nc_w)
  );

  // ACMP listener faces
  logic        lstn_evt_tk_valid_w, lstn_evt_tk_ready_w;
  logic [1:0]  lstn_evt_tk_kind_w;
  logic        lstn_evt_tk_failed_w;
  logic [15:0] lstn_evt_tk_sink_w;
  logic        pre_valid_w, pre_ready_w;
  logic [15:0] pre_sink_w, pre_talker_uid_w;
  logic [63:0] pre_talker_eid_w, pre_ctlr_eid_w;
  logic        pre_sw_w, pre_started_w;
  logic        lstn_arm_valid_w, lstn_arm_cancel_w;
  logic [TMR_AW_C-1:0] lstn_arm_slot_w;
  logic [PP_TIMER_OWNER_W_C-1:0] lstn_arm_owner_w;
  logic [31:0] lstn_arm_deadline_w;
  logic        lstn_draw_req_w;
  logic [2:0]  lstn_draw_kind_w;
  logic        lstn_draw_busy_w, lstn_draw_valid_w;
  logic        lstn_txs_alloc_req_w, lstn_txs_oversize_nc_w;
  logic [TXS_W_C-1:0] lstn_txs_wr_slot_w;
  logic [TXA_W_C-1:0] lstn_txs_wr_addr_w, lstn_txs_wr_len_w;
  logic        lstn_txs_wr_valid_w, lstn_txs_wr_commit_w;
  logic [7:0]  lstn_txs_wr_data_w;
  logic        lstn_txreq_valid_w;
  logic [TXS_W_C-1:0] lstn_txreq_slot_w;
  logic        lstn_act_settle_w, lstn_act_teardown_w;
  logic [63:0] lstn_act_settle_sid_w;
  logic [47:0] lstn_act_settle_da_w;
  logic [11:0] lstn_act_settle_vlan_w;
  logic        lstn_act_nvm_nc_w, lstn_act_nvm_set_nc_w, lstn_act_notify_nc_w;
  logic        lstn_dbg_busy_nc_w;
  logic        lstn_recwr_w;
  logic [SINK_IDX_W_C-1:0] lstn_recwr_sink_w;  //! CLAMPED (see SINK_IDX_W_C)
  logic [pp_acmp_pkg::ACMP_REC_W_C-1:0] lstn_recwr_rec_w;
  logic        lstn_txs_gnt_w;
  logic [TXS_W_C-1:0] lstn_txs_gnt_slot_w;

  KL_pp_acmp_listener #(
      .N_SINKS_P           (N_STREAM_IN_P),
      .TROM_HEX_P          (TROM_HEX_P),
      .RX_SLOTS_P          (RX_SLOTS_P),
      .RX_SLOT_BYTES_P     (RX_SLOT_BYTES_P),
      .TX_STD_SLOTS_P      (TX_STD_SLOTS_P),
      .TX_OVERSIZE_BYTES_P (TX_OVERSIZE_BYTES_P),
      .TMR_SLOT_AW_P       (TMR_AW_C),
      .TMR_BASE_SLOT_P     (TMR_LSTN_BASE_C),
      .TMR_OWNER_BASE_P    (32)
  ) u_listener (
      .clk_i                 (clk_i),
      .rst_n                 (rst_n),
      .entity_id_i           (entity_id_i),
      .txn_valid_i           (acmp_txn_valid_w && pf_ready_w
                              && !acmp_is_tkr_w),
      .txn_i                 (steer_txn_w),
      .txn_ready_o           (lstn_txn_ready_w),
      .evt_tk_valid_i        (lstn_evt_tk_valid_w),
      .evt_tk_kind_i         (lstn_evt_tk_kind_w),
      .evt_tk_failed_i       (lstn_evt_tk_failed_w),
      .evt_tk_sink_i         (lstn_evt_tk_sink_w),
      .evt_tk_ready_o        (lstn_evt_tk_ready_w),
      .pre_valid_i           (pre_valid_w),
      .pre_sink_i            (pre_sink_w),
      .pre_talker_eid_i      (pre_talker_eid_w),
      .pre_talker_uid_i      (pre_talker_uid_w),
      .pre_ctlr_eid_i        (pre_ctlr_eid_w),
      .pre_sw_i              (pre_sw_w),
      .pre_started_i         (pre_started_w),
      .pre_ready_o           (pre_ready_w),
      .now_ms_i              (now_ms_w),
      .tmr_arm_valid_o       (lstn_arm_valid_w),
      .tmr_arm_cancel_o      (lstn_arm_cancel_w),
      .tmr_arm_slot_o        (lstn_arm_slot_w),
      .tmr_arm_owner_o       (lstn_arm_owner_w),
      .tmr_arm_deadline_ms_o (lstn_arm_deadline_w),
      .tmr_exp_valid_i       (exp_valid_w),
      .tmr_exp_slot_i        (exp_slot_w),
      .tmr_exp_owner_i       (exp_owner_w),
      .draw_req_o            (lstn_draw_req_w),
      .draw_kind_o           (lstn_draw_kind_w),
      .draw_busy_i           (lstn_draw_busy_w),
      .draw_valid_i          (lstn_draw_valid_w),
      .draw_ms_i             (prng_ms_w),
      .rxs_rd_slot_o         (rxp_rd_slot_w[RXP_LSTN_C]),
      .rxs_rd_addr_o         (rxp_rd_addr_w[RXP_LSTN_C]),
      .rxs_rd_en_o           (rxp_rd_en_w[RXP_LSTN_C]),
      .rxs_rd_data_i         (rxp_rd_data_w[RXP_LSTN_C]),
      .rxs_free_o            (lstn_rxs_free_w),
      .rxs_free_slot_o       (lstn_rxs_free_slot_w),
      .txs_alloc_req_o       (lstn_txs_alloc_req_w),
      .txs_oversize_o        (lstn_txs_oversize_nc_w),
      .txs_alloc_gnt_i       (lstn_txs_gnt_w),
      .txs_alloc_slot_i      (lstn_txs_gnt_slot_w),
      .txs_wr_slot_o         (lstn_txs_wr_slot_w),
      .txs_wr_addr_o         (lstn_txs_wr_addr_w),
      .txs_wr_valid_o        (lstn_txs_wr_valid_w),
      .txs_wr_data_o         (lstn_txs_wr_data_w),
      .txs_wr_commit_o       (lstn_txs_wr_commit_w),
      .txs_wr_len_o          (lstn_txs_wr_len_w),
      .txreq_valid_o         (lstn_txreq_valid_w),
      .txreq_slot_o          (lstn_txreq_slot_w),
      .lock_held_i           (1'b0),                 // lock manager: P4
      .lock_ctlr_i           (64'd0),
      .act_settle_o          (lstn_act_settle_w),
      .act_settle_sid_o      (lstn_act_settle_sid_w),
      .act_settle_da_o       (lstn_act_settle_da_w),
      .act_settle_vlan_o     (lstn_act_settle_vlan_w),
      .act_teardown_o        (lstn_act_teardown_w),
      .act_disc_arm_o        (lstn_disc_arm_w),
      .act_disc_talker_eid_o (lstn_disc_eid_w),
      .act_disc_disarm_o     (lstn_disc_disarm_w),
      .act_nvm_o             (lstn_act_nvm_nc_w),
      .act_nvm_set_o         (lstn_act_nvm_set_nc_w),
      .act_notify_o          (lstn_act_notify_nc_w),
      .act_sink_o            (lstn_act_sink_w),
      .dbg_busy_o            (lstn_dbg_busy_nc_w),
      .dbg_recwr_o           (lstn_recwr_w),
      .dbg_recwr_sink_o      (lstn_recwr_sink_w),
      .dbg_recwr_rec_o       (lstn_recwr_rec_w)
  );

  // ACMP talker faces
  logic        tkr_resp_valid_w;
  logic [3:0]  tkr_resp_msg_type_w;
  logic [4:0]  tkr_resp_status_w;
  logic [63:0] tkr_resp_sid_w, tkr_resp_ctlr_w, tkr_resp_tkeid_w,
               tkr_resp_lseid_w;
  logic [15:0] tkr_resp_tkuid_w, tkr_resp_lsuid_w, tkr_resp_cc_w,
               tkr_resp_seq_w, tkr_resp_flags_w, tkr_resp_vlan_w;
  logic [47:0] tkr_resp_da_w;
  logic [1:0]  tkr_resp_if_nc_w;
  logic [N_STREAM_OUT_P-1:0] tkr_declaring_w;
  logic        tkr_gate_open_w, tkr_gate_close_w;
  //! CLAMPED to match KL_acmp_talker's own SRC_W_C (:95-96). A fixed [2:0]
  //! only happens to be right at N_STREAM_OUT_P = 8; at any other shape it
  //! is a width mismatch against the engine port it connects to.
  logic [SRC_IDX_W_C-1:0] tkr_gate_src_w;
  logic [63:0] tkr_gate_sid_w;
  logic [47:0] tkr_gate_da_w;
  logic [11:0] tkr_gate_vlan_w;
  logic        tkr_arm_valid_w, tkr_arm_cancel_w;
  logic [TMR_AW_C-1:0] tkr_arm_slot_w;
  logic [PP_TIMER_OWNER_W_C-1:0] tkr_arm_owner_w;
  logic [31:0] tkr_arm_deadline_w;
  logic        tkr_draw_req_w;
  logic [2:0]  tkr_draw_kind_w;
  logic        tkr_draw_busy_w, tkr_draw_valid_w;

  // SRP class-D lanes consumed by the talker + snapshot
  logic [2:0]  srp_class_a_prio_w;
  logic [11:0] srp_class_a_vid_w;
  logic        srp_domain_adopted_w;
  logic [N_STREAM_OUT_P-1:0][1:0]  srp_tk_decl_state_w;
  logic [N_STREAM_OUT_P-1:0][1:0]  srp_lstn_reg_state_w;
  logic [N_STREAM_OUT_P-1:0]       srp_active_w;
  logic [N_STREAM_OUT_P-1:0][7:0]  srp_src_fail_code_nc_w;
  logic [N_STREAM_OUT_P-1:0][63:0] srp_src_fail_bridge_nc_w;
  logic [N_STREAM_IN_P-1:0][1:0]   srp_tk_reg_state_w;
  logic [N_STREAM_IN_P-1:0][1:0]   srp_lstn_decl_state_w;
  logic [N_STREAM_IN_P-1:0][31:0]  srp_acc_latency_w;
  logic [N_STREAM_IN_P-1:0][7:0]   srp_snk_fail_code_w;
  logic [N_STREAM_IN_P-1:0][63:0]  srp_snk_fail_bridge_nc_w;
  logic [N_STREAM_OUT_P-1:0][31:0] srp_granted_slope_w;
  logic [N_STREAM_OUT_P-1:0]       srp_sr_admitted_w;
  logic [31:0] srp_sum_slope_w;
  logic        srp_over_limit_w;
  logic        srp_evt_domain_change_w;

  KL_acmp_talker #(
      .N_STREAM_OUT_P   (N_STREAM_OUT_P),
      .RX_SLOTS_P       (RX_SLOTS_P),
      .RX_SLOT_BYTES_P  (RX_SLOT_BYTES_P),
      .TMR_SLOTS_P      (TMR_SLOTS_C),
      .TMR_SLOT_BASE_P  (TMR_TKR_BASE_C),
      .TMR_OWNER_BASE_P (32'h50)
  ) u_talker (
      .clk_i                 (clk_i),
      .rst_n                 (rst_n),
      .own_entity_id_i       (entity_id_i),
      .cfg_src_en_i          (cfg_src_en_i),
      .cfg_src_iface_i       (cfg_src_iface_i),
      .cfg_stream_id_i       (cfg_stream_id_i),
      .srp_lsn_reg_state_i   (srp_lstn_reg_state_w),
      .srp_class_vid_i       (srp_class_a_vid_w),
      .srp_pcp_change_i      (srp_evt_domain_change_w),
      .txn_valid_i           (acmp_txn_valid_w && pf_ready_w
                              && acmp_is_tkr_w),
      .txn_i                 (PP_TXN_W_C'(steer_txn_w)),
      .txn_ready_o           (tkr_txn_ready_w),
      .rxs_rd_slot_o         (rxp_rd_slot_w[RXP_TKR_C]),
      .rxs_rd_addr_o         (rxp_rd_addr_w[RXP_TKR_C]),
      .rxs_rd_en_o           (rxp_rd_en_w[RXP_TKR_C]),
      .rxs_rd_data_i         (rxp_rd_data_w[RXP_TKR_C]),
      .rxs_slot_len_i        (rxp_slot_len_w[RXP_TKR_C]),
      .rxs_free_o            (tkr_rxs_free_w),
      .rxs_free_slot_o       (tkr_rxs_free_slot_w),
      .resp_valid_o          (tkr_resp_valid_w),
      .resp_msg_type_o       (tkr_resp_msg_type_w),
      .resp_status_o         (tkr_resp_status_w),
      .resp_stream_id_o      (tkr_resp_sid_w),
      .resp_controller_eid_o (tkr_resp_ctlr_w),
      .resp_talker_eid_o     (tkr_resp_tkeid_w),
      .resp_listener_eid_o   (tkr_resp_lseid_w),
      .resp_talker_uid_o     (tkr_resp_tkuid_w),
      .resp_listener_uid_o   (tkr_resp_lsuid_w),
      .resp_dest_mac_o       (tkr_resp_da_w),
      .resp_conn_count_o     (tkr_resp_cc_w),
      .resp_seq_id_o         (tkr_resp_seq_w),
      .resp_flags_o          (tkr_resp_flags_w),
      .resp_vlan_id_o        (tkr_resp_vlan_w),
      .resp_if_index_o       (tkr_resp_if_nc_w),
      // the maap face is the top's own port group (see its banner): pure
      // pass-through, the allocator lives in the integrating fabric
      .maap_req_valid_o      (maap_req_valid_o),
      .maap_req_ready_i      (maap_req_ready_i),
      .maap_req_release_o    (maap_req_release_o),
      .maap_req_src_o        (maap_req_src_o),
      .maap_rsp_valid_i      (maap_rsp_valid_i),
      .maap_rsp_ok_i         (maap_rsp_ok_i),
      .maap_rsp_da_i         (maap_rsp_da_i),
      .maap_conflict_valid_i (maap_conflict_valid_i),
      .maap_conflict_src_i   (maap_conflict_src_i),
      .maap_conflict_ack_o   (maap_conflict_ack_o),
      .declaring_o           (tkr_declaring_w),
      .gate_open_o           (tkr_gate_open_w),
      .gate_close_o          (tkr_gate_close_w),
      .gate_src_o            (tkr_gate_src_w),
      .gate_stream_id_o      (tkr_gate_sid_w),
      .gate_da_o             (tkr_gate_da_w),
      .gate_vlan_o           (tkr_gate_vlan_w),
      .now_ms_i              (now_ms_w),
      .tmr_arm_valid_o       (tkr_arm_valid_w),
      .tmr_arm_cancel_o      (tkr_arm_cancel_w),
      .tmr_arm_slot_o        (tkr_arm_slot_w),
      .tmr_arm_owner_o       (tkr_arm_owner_w),
      .tmr_arm_deadline_ms_o (tkr_arm_deadline_w),
      .tmr_exp_valid_i       (exp_valid_w),
      .tmr_exp_slot_i        (exp_slot_w),
      .tmr_exp_owner_i       (exp_owner_w),
      .prng_draw_req_o       (tkr_draw_req_w),
      .prng_draw_kind_o      (tkr_draw_kind_w),
      .prng_draw_busy_i      (tkr_draw_busy_w),
      .prng_draw_valid_i     (tkr_draw_valid_w),
      .prng_draw_ms_i        (prng_ms_w)
  );

  // =========================================================================
  // SRP engine + KL_mrp_strip (THE RECORDED SEAM)
  // =========================================================================
  logic        mrp_valid_w, mrp_last_w, mrp_msrp_w, mrp_ready_w;
  logic [7:0]  mrp_data_w;
  logic [15:0] mrp_drop_w;

  KL_mrp_strip u_mrp_strip (
      .clk_i        (clk_i),
      .rst_n        (rst_n),
      .in_valid_i   (v_mrp_valid_w),
      .in_data_i    (v_mrp_data_w),
      .in_last_i    (v_mrp_last_w),
      .out_valid_o  (mrp_valid_w),
      .out_data_o   (mrp_data_w),
      .out_last_o   (mrp_last_w),
      .out_msrp_o   (mrp_msrp_w),
      .out_ready_i  (mrp_ready_w),
      .drop_count_o (mrp_drop_w)
  );

  logic        srp_req_valid_w, srp_req_ready_w;
  logic [2:0]  srp_req_op_w;
  logic [7:0]  srp_req_index_w;
  logic [63:0] srp_req_sid_w;
  logic [47:0] srp_req_da_w;
  logic [11:0] srp_req_vid_w;
  logic [15:0] srp_req_mfs_w;
  logic [1:0]  srp_req_lstn_w;
  logic        srp_rsp_valid_w;
  logic [1:0]  srp_rsp_status_w;
  logic [31:0] srp_rsp_data_w;
  logic        srp_txs_alloc_req_w, srp_txs_oversize_nc_w;
  logic [TXS_W_C-1:0] srp_txs_wr_slot_w;
  logic [TXA_W_C-1:0] srp_txs_wr_addr_w, srp_txs_wr_len_w;
  logic        srp_txs_wr_valid_w, srp_txs_wr_commit_w;
  logic [7:0]  srp_txs_wr_data_w;
  logic        srp_txreq_valid_w;
  logic [TXS_W_C-1:0] srp_txreq_slot_w;
  logic        srp_txreq_ready_w;
  logic        srp_arm_valid_w, srp_arm_cancel_w;
  logic [TMR_AW_C-1:0] srp_arm_slot_w;
  logic [7:0]  srp_arm_owner_w;
  logic [31:0] srp_arm_deadline_w;
  logic        srp_draw_req_w;
  logic [2:0]  srp_draw_kind_w;
  logic        srp_draw_busy_w, srp_draw_valid_w;
  logic [N_STREAM_IN_P-1:0]  srp_evt_tk_reg_w, srp_evt_tk_unreg_w;
  logic [N_STREAM_OUT_P-1:0] srp_lstn_reg_change_w;
  logic [3:0]  srp_dbg_vid_active_w;
  logic        srp_dbg_vlan_err_nc_w, srp_dbg_adm_round_nc_w;
  logic        srp_dbg_pdu_done_nc_w, srp_dbg_pdu_ok_nc_w;
  logic        srp_dbg_pdu_malformed_nc_w;
  logic        srp_txs_gnt_w;
  logic [TXS_W_C-1:0] srp_txs_gnt_slot_w;

  KL_srp_top #(
      .N_SOURCES_P      (N_STREAM_OUT_P),
      .N_SINKS_P        (N_STREAM_IN_P),
      .TX_STD_BYTES_P   (576),
      .TX_OVERSIZE_BYTES_P (TX_OVERSIZE_BYTES_P),
      .SLOT_AW_P        (TMR_AW_C),
      .CAD_SLOT_BASE_P  (TMR_SRP_CAD_BASE_C),
      .CAD_OWNER_BASE_P (SRP_CAD_OWNER_C),
      .TK_SLOT_BASE_P   (TMR_SRP_TK_BASE_C),
      .TK_OWNER_BASE_P  (SRP_TK_OWNER_C),
      .LS_SLOT_BASE_P   (TMR_SRP_LS_BASE_C),
      .LS_OWNER_BASE_P  (SRP_LS_OWNER_C)
  ) u_srp (
      .clk_i               (clk_i),
      .rst_n               (rst_n),
      .own_mac_i           (own_mac_i),
      .link_up_i           (link_up_i),
      .p2p_i               (p2p_i),
      .cfg_rank_i          (cfg_rank_i),
      .cfg_acc_lat_ns_i    (cfg_acc_lat_ns_i),
      .port_rate_bps_i     (port_rate_bps_i),
      .mrp_valid_i         (mrp_valid_w),
      .mrp_data_i          (mrp_data_w),
      .mrp_last_i          (mrp_last_w),
      .mrp_msrp_i          (mrp_msrp_w),
      .mrp_ready_o         (mrp_ready_w),
      .req_valid_i         (srp_req_valid_w),
      .req_ready_o         (srp_req_ready_w),
      .req_op_i            (srp_req_op_w),
      .req_index_i         (srp_req_index_w),
      .req_stream_id_i     (srp_req_sid_w),
      .req_da_i            (srp_req_da_w),
      .req_vid_i           (srp_req_vid_w),
      .req_max_frame_i     (srp_req_mfs_w),
      .req_max_interval_i  (16'd1),
      .req_lstn_state_i    (srp_req_lstn_w),
      .rsp_valid_o         (srp_rsp_valid_w),
      .rsp_status_o        (srp_rsp_status_w),
      .rsp_data_o          (srp_rsp_data_w),
      .alloc_req_o         (srp_txs_alloc_req_w),
      .oversize_o          (srp_txs_oversize_nc_w),
      .alloc_slot_i        (srp_txs_gnt_slot_w),
      .alloc_gnt_i         (srp_txs_gnt_w),
      .wr_slot_o           (srp_txs_wr_slot_w),
      .wr_addr_o           (srp_txs_wr_addr_w),
      .wr_valid_o          (srp_txs_wr_valid_w),
      .wr_data_o           (srp_txs_wr_data_w),
      .wr_commit_o         (srp_txs_wr_commit_w),
      .wr_len_o            (srp_txs_wr_len_w),
      .txreq_valid_o       (srp_txreq_valid_w),
      .txreq_slot_o        (srp_txreq_slot_w),
      .txreq_ready_i       (srp_txreq_ready_w),
      .now_ms_i            (now_ms_w),
      .arm_valid_o         (srp_arm_valid_w),
      .arm_cancel_o        (srp_arm_cancel_w),
      .arm_slot_o          (srp_arm_slot_w),
      .arm_owner_o         (srp_arm_owner_w),
      .arm_deadline_ms_o   (srp_arm_deadline_w),
      .exp_valid_i         (exp_valid_w),
      .exp_slot_i          (exp_slot_w),
      .draw_req_o          (srp_draw_req_w),
      .draw_kind_o         (srp_draw_kind_w),
      .draw_busy_i         (srp_draw_busy_w),
      .draw_valid_i        (srp_draw_valid_w),
      .draw_ms_i           (prng_ms_w),
      .evt_tk_registered_o   (srp_evt_tk_reg_w),
      .evt_tk_unregistered_o (srp_evt_tk_unreg_w),
      .lstn_reg_change_o     (srp_lstn_reg_change_w),
      .evt_domain_change_o   (srp_evt_domain_change_w),
      .class_a_prio_o      (srp_class_a_prio_w),
      .class_a_vid_o       (srp_class_a_vid_w),
      .domain_adopted_o    (srp_domain_adopted_w),
      .tk_decl_state_o     (srp_tk_decl_state_w),
      .lstn_reg_state_o    (srp_lstn_reg_state_w),
      .active_o            (srp_active_w),
      .src_fail_code_o     (srp_src_fail_code_nc_w),
      .src_fail_bridge_o   (srp_src_fail_bridge_nc_w),
      .tk_reg_state_o      (srp_tk_reg_state_w),
      .lstn_decl_state_o   (srp_lstn_decl_state_w),
      .acc_latency_o       (srp_acc_latency_w),
      .snk_fail_code_o     (srp_snk_fail_code_w),
      .snk_fail_bridge_o   (srp_snk_fail_bridge_nc_w),
      .granted_slope_bps_o (srp_granted_slope_w),
      .sr_admitted_o       (srp_sr_admitted_w),
      .sum_slope_bps_o     (srp_sum_slope_w),
      .over_limit_o        (srp_over_limit_w),
      .dbg_vid_active_o    (srp_dbg_vid_active_w),
      .dbg_vlan_err_o      (srp_dbg_vlan_err_nc_w),
      .dbg_adm_round_o     (srp_dbg_adm_round_nc_w),
      .dbg_pdu_done_o      (srp_dbg_pdu_done_nc_w),
      .dbg_pdu_ok_o        (srp_dbg_pdu_ok_nc_w),
      .dbg_pdu_malformed_o (srp_dbg_pdu_malformed_nc_w)
  );

  // ---- SRP service adapter: three one-deep producer stages (talker gate,
  // listener settle/teardown, external svc face) drained by fixed priority
  // into the single-outstanding class-B port. Engine-stage overruns are
  // counted; the svc stage backpressures via svc_ready_o instead.
  typedef struct packed {
    logic        from_svc;
    logic [2:0]  op;
    logic [7:0]  index;
    logic [63:0] sid;
    logic [47:0] da;
    logic [11:0] vid;
    logic [15:0] mfs;
    logic [1:0]  lstn;
  } srp_svcreq_t;

  srp_svcreq_t st_tk_r, st_ls_r, st_svc_r, inflight_r;
  logic        st_tk_vld_r, st_ls_vld_r, st_svc_vld_r;
  logic        inflight_vld_r;
  logic [15:0] svc_drop_r;

  assign svc_ready_o = !st_svc_vld_r;

  always_ff @(posedge clk_i) begin : srp_svc_adapter
    if (!rst_n) begin
      st_tk_r  <= '0;  st_tk_vld_r  <= 1'b0;
      st_ls_r  <= '0;  st_ls_vld_r  <= 1'b0;
      st_svc_r <= '0;  st_svc_vld_r <= 1'b0;
      inflight_r     <= '0;
      inflight_vld_r <= 1'b0;
      srp_req_valid_w <= 1'b0;
      svc_drop_r      <= 16'd0;
      svc_rsp_valid_o  <= 1'b0;
      svc_rsp_status_o <= 2'd0;
      svc_rsp_data_o   <= 32'd0;
    end else begin
      svc_rsp_valid_o <= 1'b0;

      // capture producers (mutually-exclusive strobes within each producer)
      if (tkr_gate_open_w || tkr_gate_close_w) begin
        if (st_tk_vld_r) begin
          if (svc_drop_r != 16'hFFFF) svc_drop_r <= svc_drop_r + 16'd1;
        end else begin
          st_tk_vld_r <= 1'b1;
          st_tk_r     <= '{from_svc: 1'b0,
                           op:    tkr_gate_open_w ? 3'd0 : 3'd1,
                           index: 8'(tkr_gate_src_w),
                           sid:   tkr_gate_sid_w,
                           da:    tkr_gate_da_w,
                           vid:   tkr_gate_vlan_w,
                           mfs:   cfg_tspec_max_frame_i,
                           lstn:  2'd0};
        end
      end
      if (lstn_act_settle_w || lstn_act_teardown_w) begin
        if (st_ls_vld_r) begin
          if (svc_drop_r != 16'hFFFF) svc_drop_r <= svc_drop_r + 16'd1;
        end else begin
          st_ls_vld_r <= 1'b1;
          st_ls_r     <= '{from_svc: 1'b0,
                           op:    lstn_act_settle_w ? 3'd2 : 3'd3,
                           index: 8'(lstn_act_sink_w),
                           sid:   lstn_act_settle_sid_w,
                           da:    lstn_act_settle_da_w,
                           vid:   lstn_act_settle_vlan_w,
                           mfs:   16'd0,
                           lstn:  2'd2};
        end
      end
      if (svc_valid_i && !st_svc_vld_r) begin
        st_svc_vld_r <= 1'b1;
        st_svc_r     <= '{from_svc: 1'b1,
                          op:    svc_op_i,
                          index: svc_index_i,
                          sid:   svc_stream_id_i,
                          da:    svc_da_i,
                          vid:   svc_vid_i,
                          mfs:   svc_max_frame_i,
                          lstn:  svc_lstn_state_i};
      end

      // issue: one outstanding request at a time
      if (!inflight_vld_r) begin
        if (st_tk_vld_r) begin
          inflight_r <= st_tk_r;  inflight_vld_r <= 1'b1;
          st_tk_vld_r <= 1'b0;    srp_req_valid_w <= 1'b1;
        end else if (st_ls_vld_r) begin
          inflight_r <= st_ls_r;  inflight_vld_r <= 1'b1;
          st_ls_vld_r <= 1'b0;    srp_req_valid_w <= 1'b1;
        end else if (st_svc_vld_r) begin
          inflight_r <= st_svc_r; inflight_vld_r <= 1'b1;
          st_svc_vld_r <= 1'b0;   srp_req_valid_w <= 1'b1;
        end
      end else begin
        if (srp_req_valid_w && srp_req_ready_w) begin
          srp_req_valid_w <= 1'b0;             // accepted; await response
        end
        if (srp_rsp_valid_w) begin
          inflight_vld_r <= 1'b0;
          if (inflight_r.from_svc) begin
            svc_rsp_valid_o  <= 1'b1;
            svc_rsp_status_o <= srp_rsp_status_w;
            svc_rsp_data_o   <= srp_rsp_data_w;
          end
        end
      end
    end
  end

  assign srp_req_op_w    = inflight_r.op;
  assign srp_req_index_w = inflight_r.index;
  assign srp_req_sid_w   = inflight_r.sid;
  assign srp_req_da_w    = inflight_r.da;
  assign srp_req_vid_w   = inflight_r.vid;
  assign srp_req_mfs_w   = inflight_r.mfs;
  assign srp_req_lstn_w  = inflight_r.lstn;

  // =========================================================================
  // NVM: shadow between the listener record shadow and KL_pp_nvm_port
  // =========================================================================
  logic        nvm_req_w, nvm_we_w, nvm_wvalid_w, nvm_wready_w;
  logic [7:0]  nvm_record_id_w, nvm_wdata_w, nvm_rdata_w;
  logic        nvm_rvalid_w, nvm_rready_w, nvm_busy_w, nvm_done_w, nvm_err_w;
  logic [N_STREAM_IN_P-1:0] nvm_dbg_dirty_nc_w, nvm_dbg_valid_nc_w,
                            nvm_dbg_touched_nc_w;

  KL_acmp_nvm_shadow #(
      .N_SINKS_P (N_STREAM_IN_P)
  ) u_nvm_shadow (
      .clk_i            (clk_i),
      .rst_n            (rst_n),
      .tick_i           (tick_ms_w),
      .restore_go_i     (restore_go_i),
      .restore_busy_o   (restore_busy_o),
      .restore_done_o   (restore_done_o),
      .restore_fail_o   (restore_fail_o),
      .alarm_o          (nvm_alarm_o),
      .cap_wr_i         (lstn_recwr_w),
      .cap_sink_i       (lstn_recwr_sink_w),
      .cap_rec_i        (lstn_recwr_rec_w),
      .pre_valid_o      (pre_valid_w),
      .pre_sink_o       (pre_sink_w),
      .pre_talker_eid_o (pre_talker_eid_w),
      .pre_talker_uid_o (pre_talker_uid_w),
      .pre_ctlr_eid_o   (pre_ctlr_eid_w),
      .pre_sw_o         (pre_sw_w),
      .pre_started_o    (pre_started_w),
      .pre_ready_i      (pre_ready_w),
      .nvm_req_o        (nvm_req_w),
      .nvm_we_o         (nvm_we_w),
      .nvm_record_id_o  (nvm_record_id_w),
      .nvm_wvalid_o     (nvm_wvalid_w),
      .nvm_wready_i     (nvm_wready_w),
      .nvm_wdata_o      (nvm_wdata_w),
      .nvm_rvalid_i     (nvm_rvalid_w),
      .nvm_rready_o     (nvm_rready_w),
      .nvm_rdata_i      (nvm_rdata_w),
      .nvm_busy_i       (nvm_busy_w),
      .nvm_done_i       (nvm_done_w),
      .nvm_err_i        (nvm_err_w),
      .dbg_dirty_o      (nvm_dbg_dirty_nc_w),
      .dbg_valid_o      (nvm_dbg_valid_nc_w),
      .dbg_touched_o    (nvm_dbg_touched_nc_w)
  );

  KL_pp_nvm_port u_nvm_port (
      .clk_i           (clk_i),
      .rst_n           (rst_n),
      .nvm_req_i       (nvm_req_w),
      .nvm_we_i        (nvm_we_w),
      .nvm_record_id_i (nvm_record_id_w),
      .nvm_wvalid_i    (nvm_wvalid_w),
      .nvm_wready_o    (nvm_wready_w),
      .nvm_wdata_i     (nvm_wdata_w),
      .nvm_rvalid_o    (nvm_rvalid_w),
      .nvm_rready_i    (nvm_rready_w),
      .nvm_rdata_o     (nvm_rdata_w),
      .nvm_busy_o      (nvm_busy_w),
      .nvm_done_o      (nvm_done_w),
      .nvm_err_o       (nvm_err_w),
      .dev_req_o       (nvm_dev_req_o),
      .dev_gnt_i       (nvm_dev_gnt_i),
      .dev_op_o        (nvm_dev_op_o),
      .dev_region_o    (nvm_dev_region_o),
      .dev_offset_o    (nvm_dev_offset_o),
      .dev_len_o       (nvm_dev_len_o),
      .dev_wvalid_o    (nvm_dev_wvalid_o),
      .dev_wready_i    (nvm_dev_wready_i),
      .dev_wdata_o     (nvm_dev_wdata_o),
      .dev_rvalid_i    (nvm_dev_rvalid_i),
      .dev_rdata_i     (nvm_dev_rdata_i),
      .dev_rready_o    (nvm_dev_rready_o),
      .dev_busy_i      (nvm_dev_busy_i),
      .dev_done_i      (nvm_dev_done_i),
      .dev_err_i       (nvm_dev_err_i)
  );

  // =========================================================================
  // timer arm-port priority mux (banner)
  // =========================================================================
  // Five engine arm faces feed ONE always-accepting arm port. Arms are
  // one-cycle strobes with no ready, so each face gets a 4-deep queue and a
  // fixed-priority drain: listener > talker > ADP > SRP > originator (SM
  // correctness first, cadence last). Per-face order is preserved (each
  // engine owns disjoint slot ranges, so cross-face order is free); a queue
  // overrun drops the NEWEST arm and counts — never silent.
  localparam int unsigned ARM_N_C = 5;
  localparam int unsigned ARM_W_C = 1 + TMR_AW_C + PP_TIMER_OWNER_W_C + 32;

  logic [ARM_N_C-1:0]              armq_in_vld_w;
  logic [ARM_N_C-1:0][ARM_W_C-1:0] armq_in_w;
  logic [ARM_N_C-1:0][3:0][ARM_W_C-1:0] armq_r;
  logic [ARM_N_C-1:0][2:0]         armq_cnt_r;
  logic [15:0]                     arm_drop_r;

  assign armq_in_vld_w = {org_arm_valid_w, srp_arm_valid_w, adp_arm_valid_w,
                          tkr_arm_valid_w, lstn_arm_valid_w};
  assign armq_in_w[0] = {lstn_arm_cancel_w, lstn_arm_slot_w,
                         lstn_arm_owner_w, lstn_arm_deadline_w};
  assign armq_in_w[1] = {tkr_arm_cancel_w, tkr_arm_slot_w,
                         tkr_arm_owner_w, tkr_arm_deadline_w};
  assign armq_in_w[2] = {adp_arm_cancel_w, adp_arm_slot_w,
                         adp_arm_owner_w, adp_arm_deadline_w};
  assign armq_in_w[3] = {srp_arm_cancel_w, srp_arm_slot_w,
                         srp_arm_owner_w, srp_arm_deadline_w};
  assign armq_in_w[4] = {org_arm_cancel_w, org_arm_slot_w,
                         org_arm_owner_w, org_arm_deadline_w};

  logic [ARM_N_C-1:0]      armq_pop_w;
  logic [ARM_N_C-1:0][2:0] armq_mid_w;    // count after the pop
  logic [ARM_N_C-1:0]      armq_push_ok_w;

  always_comb begin : arm_drain_pick
    armq_pop_w = '0;
    for (int unsigned i = 0; i < ARM_N_C; i++) begin
      if ((armq_pop_w == '0) && (armq_cnt_r[i] != 3'd0)) begin
        armq_pop_w[i] = 1'b1;
      end
    end
    for (int unsigned i = 0; i < ARM_N_C; i++) begin
      armq_mid_w[i]     = armq_cnt_r[i] - (armq_pop_w[i] ? 3'd1 : 3'd0);
      armq_push_ok_w[i] = armq_in_vld_w[i] && (armq_mid_w[i] != 3'd4);
    end
  end

  always_ff @(posedge clk_i) begin : arm_mux
    if (!rst_n) begin
      armq_r     <= '0;
      armq_cnt_r <= '0;
      arm_drop_r <= 16'd0;
      tmr_arm_valid_w    <= 1'b0;
      tmr_arm_cancel_w   <= 1'b0;
      tmr_arm_slot_w     <= '0;
      tmr_arm_owner_w    <= '0;
      tmr_arm_deadline_w <= 32'd0;
    end else begin
      tmr_arm_valid_w <= 1'b0;
      for (int unsigned i = 0; i < ARM_N_C; i++) begin
        if (armq_pop_w[i]) begin
          {tmr_arm_cancel_w, tmr_arm_slot_w, tmr_arm_owner_w,
           tmr_arm_deadline_w} <= armq_r[i][0];
          tmr_arm_valid_w      <= 1'b1;
          armq_r[i][0] <= armq_r[i][1];
          armq_r[i][1] <= armq_r[i][2];
          armq_r[i][2] <= armq_r[i][3];
        end
        // the push write goes past the shifted survivors: later assignment
        // to the same index wins, which is exactly the append position
        if (armq_push_ok_w[i]) begin
          armq_r[i][armq_mid_w[i][1:0]] <= armq_in_w[i];
        end
        if (armq_in_vld_w[i] && !armq_push_ok_w[i]) begin
          if (arm_drop_r != 16'hFFFF) arm_drop_r <= arm_drop_r + 16'd1;
        end
        armq_cnt_r[i] <= armq_mid_w[i]
                       + (armq_push_ok_w[i] ? 3'd1 : 3'd0);
      end
    end
  end

  // =========================================================================
  // PRNG draw-port owner mux (banner)
  // =========================================================================
  // Four clients; a broadcast draw_valid would complete the WRONG client's
  // draw, so the mux latches one pending request per client, serves them by
  // fixed priority (listener > talker > ADP > SRP), and routes draw_valid
  // exclusively to the owner. The shared busy shown to every client is
  // "some draw pending or in flight" — each client's own protocol (issue on
  // not-busy, then wait for valid) remains exactly what its own suite
  // proved.
  localparam int unsigned PRNG_N_C = 4;

  logic [PRNG_N_C-1:0]      pr_pend_r;
  logic [PRNG_N_C-1:0][2:0] pr_kind_r;
  logic                     pr_inflight_r;
  logic [1:0]               pr_owner_r;
  logic [PRNG_N_C-1:0]      pr_req_w;
  logic [PRNG_N_C-1:0][2:0] pr_kind_w;
  logic                     pr_any_pend_w;
  logic                     pr_busy_shared_w;

  assign pr_req_w  = {srp_draw_req_w, adp_prng_req_w,
                      tkr_draw_req_w, lstn_draw_req_w};
  assign pr_kind_w = {srp_draw_kind_w, adp_prng_kind_w,
                      tkr_draw_kind_w, lstn_draw_kind_w};
  assign pr_any_pend_w    = |pr_pend_r;
  assign pr_busy_shared_w = pr_inflight_r || pr_any_pend_w || prng_busy_w;

  assign lstn_draw_busy_w = pr_busy_shared_w;
  assign tkr_draw_busy_w  = pr_busy_shared_w;
  assign adp_prng_busy_w  = pr_busy_shared_w;
  assign srp_draw_busy_w  = pr_busy_shared_w;

  assign lstn_draw_valid_w = prng_valid_w && pr_inflight_r && (pr_owner_r == 2'd0);
  assign tkr_draw_valid_w  = prng_valid_w && pr_inflight_r && (pr_owner_r == 2'd1);
  assign adp_prng_valid_w  = prng_valid_w && pr_inflight_r && (pr_owner_r == 2'd2);
  assign srp_draw_valid_w  = prng_valid_w && pr_inflight_r && (pr_owner_r == 2'd3);

  always_ff @(posedge clk_i) begin : prng_mux
    if (!rst_n) begin
      pr_pend_r     <= '0;
      pr_kind_r     <= '0;
      pr_inflight_r <= 1'b0;
      pr_owner_r    <= 2'd0;
      prng_req_w    <= 1'b0;
      prng_kind_w   <= 3'd0;
    end else begin
      prng_req_w <= 1'b0;
      // capture requests — a client holding its req line while it owns the
      // in-flight draw must not queue a phantom second draw
      for (int unsigned i = 0; i < PRNG_N_C; i++) begin
        if (pr_req_w[i]
            && !(pr_inflight_r && (pr_owner_r == 2'(i)))) begin
          pr_pend_r[i] <= 1'b1;
          pr_kind_r[i] <= pr_kind_w[i];
        end
      end
      if (pr_inflight_r) begin
        if (prng_valid_w) pr_inflight_r <= 1'b0;
      end else if (!prng_busy_w && !prng_req_w) begin
        for (int unsigned i = 0; i < PRNG_N_C; i++) begin
          if (!pr_inflight_r && pr_pend_r[i]) begin
            prng_req_w    <= 1'b1;
            prng_kind_w   <= pr_kind_r[i];
            pr_owner_r    <= 2'(i);
            pr_pend_r[i]  <= 1'b0;
            pr_inflight_r <= 1'b1;
          end
        end
      end
    end
  end

  // =========================================================================
  // event router + consumer split (02 §5 catalog)
  // =========================================================================
  // Source map (banner), in this ORDER — which is the contract; the SIZES
  // are the shape, so every base below is DERIVED exactly like the F08.4
  // timer map. At the F01.5 8x8 default this is the historical numbering:
  // 0..7 SRP TK_ATTR_REGISTERED{sink} (payload bit 15 = Talker Failed at
  // strobe time), 8..15 SRP TK_ATTR_UNREGISTERED{sink}, 16/17 ADP
  // EVT_TK_DISCOVERED/DEPARTED{sink}, 18 DOMAIN_CHANGE, 19..26
  // LISTENER_REG_CHANGE{src}, 27 GM_CHANGE, 28 LINK edge — 29 sources. TK
  // sources (0..ADP_DEP) present to the ACMP listener's evt face; every
  // acked event is traced; non-TK sources trace-and-ack immediately.
  //
  // With literal indices this map aliased exactly as the timer map did: at
  // 9 sinks TK_ATTR_UNREGISTERED{sink 7} lands on the literal 16 that ADP
  // EVT_TK_DISCOVERED owns, and the router has no owner tag to tell them
  // apart — the ACMP listener would be handed a DISCOVERED it must treat as
  // an UNREGISTERED.
  localparam pp_evr_map_t EVR_MAP_C = pp_evr_map(N_STREAM_IN_P,
                                                 N_STREAM_OUT_P);
  localparam int unsigned EVR_TKREG_BASE_C  = EVR_MAP_C.tk_reg;    // SI sources
  localparam int unsigned EVR_TKUNR_BASE_C  = EVR_MAP_C.tk_unreg;  // SI sources
  localparam int unsigned EVR_ADP_DISC_C    = EVR_MAP_C.adp_disc;
  localparam int unsigned EVR_ADP_DEP_C     = EVR_MAP_C.adp_dep;
  localparam int unsigned EVR_DOMAIN_C      = EVR_MAP_C.domain;
  localparam int unsigned EVR_LSNCHG_BASE_C = EVR_MAP_C.lsn_chg;   // SO sources
  localparam int unsigned EVR_GM_C          = EVR_MAP_C.gm_chg;
  localparam int unsigned EVR_LINK_C        = EVR_MAP_C.link;
  localparam int unsigned EVR_N_SRC_C       = EVR_MAP_C.n_src;     // = 29 at 8x8
  localparam int unsigned EVR_SRC_W_C       = (EVR_N_SRC_C > 1)
                                              ? $clog2(EVR_N_SRC_C) : 1;

  logic [EVR_N_SRC_C-1:0]       evr_strobe_w;
  logic [EVR_N_SRC_C-1:0][15:0] evr_payload_w;
  logic                         evr_valid_w;
  logic [EVR_SRC_W_C-1:0]       evr_src_w;
  logic [15:0]        evr_pay_w;
  logic               evr_lost_w;
  logic               evr_ack_w;
  logic               link_q_r;

  always_ff @(posedge clk_i) begin : link_edge
    if (!rst_n) link_q_r <= 1'b0;
    else        link_q_r <= link_up_i;
  end

  always_comb begin : evr_sources
    evr_strobe_w  = '0;
    evr_payload_w = '0;
    for (int unsigned k = 0; k < N_STREAM_IN_P; k++) begin
      evr_strobe_w[EVR_TKREG_BASE_C + k]  = srp_evt_tk_reg_w[k];
      evr_payload_w[EVR_TKREG_BASE_C + k] = {(srp_tk_reg_state_w[k] == 2'd2),
                                             7'd0, 8'(k)};
      evr_strobe_w[EVR_TKUNR_BASE_C + k]  = srp_evt_tk_unreg_w[k];
      evr_payload_w[EVR_TKUNR_BASE_C + k] = {8'd0, 8'(k)};
    end
    evr_strobe_w[EVR_ADP_DISC_C]  = adp_evt_valid_w && !adp_evt_departed_w;
    evr_payload_w[EVR_ADP_DISC_C] = 16'(adp_evt_sink_w);
    evr_strobe_w[EVR_ADP_DEP_C]   = adp_evt_valid_w && adp_evt_departed_w;
    evr_payload_w[EVR_ADP_DEP_C]  = 16'(adp_evt_sink_w);
    evr_strobe_w[EVR_DOMAIN_C]    = srp_evt_domain_change_w;
    evr_payload_w[EVR_DOMAIN_C]   = {4'd0, srp_class_a_vid_w};
    for (int unsigned k = 0; k < N_STREAM_OUT_P; k++) begin
      evr_strobe_w[EVR_LSNCHG_BASE_C + k]  = srp_lstn_reg_change_w[k];
      evr_payload_w[EVR_LSNCHG_BASE_C + k] = {8'd0, 8'(k)};
    end
    evr_strobe_w[EVR_GM_C]    = gm_change_i;
    evr_payload_w[EVR_GM_C]   = 16'd0;
    evr_strobe_w[EVR_LINK_C]  = link_up_i != link_q_r;
    evr_payload_w[EVR_LINK_C] = {15'd0, link_up_i};
  end

  logic [EVR_SRC_W_C-1:0] evr_lost_src_w;
  logic [7:0]             evr_lost_cnt_nc_w;

  KL_pp_event_router #(
      .N_SRC_P     (EVR_N_SRC_C),
      .PAYLOAD_W_P (16)
  ) u_event_router (
      .clk_i         (clk_i),
      .rst_n         (rst_n),
      .src_strobe_i  (evr_strobe_w),
      .src_payload_i (evr_payload_w),
      .evt_valid_o   (evr_valid_w),
      .evt_src_o     (evr_src_w),
      .evt_payload_o (evr_pay_w),
      .evt_lost_o    (evr_lost_w),
      .evt_ack_i     (evr_ack_w),
      .lost_src_i    (evr_lost_src_w),
      .lost_count_o  (evr_lost_cnt_nc_w)
  );

  assign evr_lost_src_w = evr_src_w;   // thin: read the presented source

  logic evr_tk_sel_w;
  assign evr_tk_sel_w = evr_valid_w
                        && (evr_src_w <= EVR_SRC_W_C'(EVR_ADP_DEP_C));

  assign lstn_evt_tk_valid_w = evr_tk_sel_w;
  assign lstn_evt_tk_sink_w  = {8'd0, evr_pay_w[7:0]};
  assign lstn_evt_tk_failed_w = evr_pay_w[15];
  always_comb begin : evt_kind_map
    if (evr_src_w < EVR_SRC_W_C'(EVR_TKUNR_BASE_C)) begin
      lstn_evt_tk_kind_w = pp_acmp_pkg::TK_KIND_REG_C;
    end else if (evr_src_w < EVR_SRC_W_C'(EVR_ADP_DISC_C)) begin
      lstn_evt_tk_kind_w = pp_acmp_pkg::TK_KIND_UNREG_C;
    end else if (evr_src_w == EVR_SRC_W_C'(EVR_ADP_DISC_C)) begin
      lstn_evt_tk_kind_w = pp_acmp_pkg::TK_KIND_DISC_C;
    end else begin
      lstn_evt_tk_kind_w = pp_acmp_pkg::TK_KIND_DEP_C;
    end
  end

  assign evr_ack_w = evr_tk_sel_w ? lstn_evt_tk_ready_w
                                  : (evr_valid_w);      // non-TK: trace + ack

  // trace record: [127:96] now_ms, [95:88] source, [87:80] flags (bit 0 =
  // evt_lost), [79:64] payload, [63:0] reserved 0
  assign trc_wr_valid_w = evr_valid_w && evr_ack_w;
  assign trc_wr_data_w  = {now_ms_w, 8'(evr_src_w),
                           {7'd0, evr_lost_w}, evr_pay_w, 64'd0};

  // =========================================================================
  // talker response builder (banner)
  // =========================================================================
  // The stateless talker answers with F05.13 FIELDS; the wire wants a
  // 56-byte ACMPDU in a TX slot like the listener's. This builder latches
  // one response, allocates through the pool-access arbiter (client 3),
  // writes the PDU image bytes 0..55, commits len 56 and holds TX lane 5
  // until granted. A response arriving while one is being built is dropped
  // AND counted (bounded by design: the talker answers faster than 70
  // cycles only under a same-cycle dispatch storm the 4-slot pool already
  // forbids).
  typedef enum logic [2:0] {
    TKB_IDLE, TKB_ALLOC, TKB_WR, TKB_COMMIT, TKB_REQ
  } tkb_state_e;

  tkb_state_e  tkb_st_r;
  logic        tkb_pend_r;
  logic [3:0]  tkb_msg_r;
  logic [4:0]  tkb_status_r;
  logic [63:0] tkb_sid_r, tkb_ctlr_r, tkb_tkeid_r, tkb_lseid_r;
  logic [15:0] tkb_tkuid_r, tkb_lsuid_r, tkb_cc_r, tkb_seq_r, tkb_flags_r,
               tkb_vlan_r;
  logic [47:0] tkb_da_r;
  logic [15:0] tkb_drop_r;
  logic [TXS_W_C-1:0] tkb_slot_r;
  logic [5:0]  tkb_bidx_r;
  logic        tkb_alloc_req_w;
  logic        tkb_gnt_w;
  logic [TXS_W_C-1:0] tkb_gnt_slot_w;
  logic        tkb_lane_valid_r;
  logic        tkb_lane_gnt_w;
  logic [447:0] tkb_pdu_w;
  logic [7:0]   tkb_pdu_byte_w;

  assign tkb_pdu_w = {8'hFC, 4'h0, tkb_msg_r, tkb_status_r, 11'd44,
                      tkb_sid_r, tkb_ctlr_r, tkb_tkeid_r, tkb_lseid_r,
                      tkb_tkuid_r, tkb_lsuid_r, tkb_da_r, tkb_cc_r,
                      tkb_seq_r, tkb_flags_r, tkb_vlan_r, 16'h0000};
  assign tkb_pdu_byte_w  = tkb_pdu_w[(9'd440 - {tkb_bidx_r, 3'b000}) +: 8];
  assign tkb_alloc_req_w = (tkb_st_r == TKB_ALLOC);

  always_ff @(posedge clk_i) begin : tkb_engine
    if (!rst_n) begin
      tkb_st_r   <= TKB_IDLE;
      tkb_pend_r <= 1'b0;
      tkb_msg_r  <= 4'd0;   tkb_status_r <= 5'd0;
      tkb_sid_r  <= 64'd0;  tkb_ctlr_r <= 64'd0;
      tkb_tkeid_r <= 64'd0; tkb_lseid_r <= 64'd0;
      tkb_tkuid_r <= 16'd0; tkb_lsuid_r <= 16'd0;
      tkb_cc_r    <= 16'd0; tkb_seq_r   <= 16'd0;
      tkb_flags_r <= 16'd0; tkb_vlan_r  <= 16'd0;
      tkb_da_r    <= 48'd0;
      tkb_drop_r  <= 16'd0;
      tkb_slot_r  <= '0;
      tkb_bidx_r  <= 6'd0;
      tkb_lane_valid_r <= 1'b0;
    end else begin
      if (tkr_resp_valid_w) begin
        if (tkb_pend_r) begin
          if (tkb_drop_r != 16'hFFFF) tkb_drop_r <= tkb_drop_r + 16'd1;
        end else begin
          tkb_pend_r  <= 1'b1;
          tkb_msg_r   <= tkr_resp_msg_type_w;
          tkb_status_r <= tkr_resp_status_w;
          tkb_sid_r   <= tkr_resp_sid_w;
          tkb_ctlr_r  <= tkr_resp_ctlr_w;
          tkb_tkeid_r <= tkr_resp_tkeid_w;
          tkb_lseid_r <= tkr_resp_lseid_w;
          tkb_tkuid_r <= tkr_resp_tkuid_w;
          tkb_lsuid_r <= tkr_resp_lsuid_w;
          tkb_cc_r    <= tkr_resp_cc_w;
          tkb_seq_r   <= tkr_resp_seq_w;
          tkb_flags_r <= tkr_resp_flags_w;
          tkb_vlan_r  <= tkr_resp_vlan_w;
          tkb_da_r    <= tkr_resp_da_w;
        end
      end
      unique case (tkb_st_r)
        TKB_IDLE: begin
          if (tkb_pend_r) tkb_st_r <= TKB_ALLOC;
        end
        TKB_ALLOC: begin
          if (tkb_gnt_w) begin
            tkb_slot_r <= tkb_gnt_slot_w;
            tkb_bidx_r <= 6'd0;
            tkb_st_r   <= TKB_WR;
          end
        end
        TKB_WR: begin
          if (tkb_bidx_r == 6'd55) tkb_st_r <= TKB_COMMIT;
          else                     tkb_bidx_r <= tkb_bidx_r + 6'd1;
        end
        TKB_COMMIT: begin
          tkb_st_r         <= TKB_REQ;
          tkb_lane_valid_r <= 1'b1;
        end
        TKB_REQ: begin
          if (tkb_lane_gnt_w) begin
            tkb_lane_valid_r <= 1'b0;
            tkb_pend_r       <= 1'b0;
            tkb_st_r         <= TKB_IDLE;
          end
        end
        default: tkb_st_r <= TKB_IDLE;
      endcase
    end
  end

  // =========================================================================
  // TX slot pool + pool-access arbiter (banner)
  // =========================================================================
  // ONE alloc+write port, four builders (ADP / listener / SRP / talker
  // builder). Ownership is taken at selection, the owner's own alloc pulses
  // are forwarded (grant timing stays exactly what each engine's suite
  // proved), the write lane stays muxed to the owner until its commit.
  // Non-owners simply see no grant and keep retrying/holding — that IS
  // their landed behavior. A full pool parks the owner (grant comes when
  // serialization frees a slot); no timeout exists because every landed
  // builder commits unconditionally after grant.
  localparam int unsigned TXC_ADP_C  = 0;
  localparam int unsigned TXC_LSTN_C = 1;
  localparam int unsigned TXC_SRP_C  = 2;
  localparam int unsigned TXC_TKB_C  = 3;

  logic [3:0] txc_req_w;
  logic [3:0] txc_pend_r;
  logic [1:0] txc_owner_r;
  logic       txc_locked_r;
  logic       pool_alloc_req_w;
  logic       pool_alloc_gnt_w;
  logic [TXS_W_C-1:0] pool_alloc_slot_w;
  logic       pool_wr_valid_w, pool_wr_commit_w;
  logic [TXS_W_C-1:0] pool_wr_slot_w;
  logic [TXA_W_C-1:0] pool_wr_addr_w, pool_wr_len_w;
  logic [7:0] pool_wr_data_w;
  logic [3:0] txc_commit_w;

  assign txc_req_w = {tkb_alloc_req_w, srp_txs_alloc_req_w,
                      lstn_txs_alloc_req_w, adp_txs_alloc_req_w};
  assign txc_commit_w = {(tkb_st_r == TKB_COMMIT), srp_txs_wr_commit_w,
                         lstn_txs_wr_commit_w, adp_txs_wr_commit_w};

  always_ff @(posedge clk_i) begin : txc_arbiter
    if (!rst_n) begin
      txc_pend_r   <= '0;
      txc_owner_r  <= 2'd0;
      txc_locked_r <= 1'b0;
    end else begin
      for (int unsigned i = 0; i < 4; i++) begin
        if (txc_req_w[i]) txc_pend_r[i] <= 1'b1;
      end
      if (!txc_locked_r) begin
        // fixed priority: listener (ACMP budget) > talker builder > SRP > ADP
        if (txc_pend_r[TXC_LSTN_C] || txc_req_w[TXC_LSTN_C]) begin
          txc_owner_r <= 2'(TXC_LSTN_C); txc_locked_r <= 1'b1;
        end else if (txc_pend_r[TXC_TKB_C] || txc_req_w[TXC_TKB_C]) begin
          txc_owner_r <= 2'(TXC_TKB_C);  txc_locked_r <= 1'b1;
        end else if (txc_pend_r[TXC_SRP_C] || txc_req_w[TXC_SRP_C]) begin
          txc_owner_r <= 2'(TXC_SRP_C);  txc_locked_r <= 1'b1;
        end else if (txc_pend_r[TXC_ADP_C] || txc_req_w[TXC_ADP_C]) begin
          txc_owner_r <= 2'(TXC_ADP_C);  txc_locked_r <= 1'b1;
        end
      end else begin
        if (pool_alloc_gnt_w) txc_pend_r[txc_owner_r] <= 1'b0;
        if (txc_commit_w[txc_owner_r]) txc_locked_r <= 1'b0;
      end
    end
  end

  always_comb begin : txc_mux
    pool_alloc_req_w = 1'b0;
    pool_wr_valid_w  = 1'b0;
    pool_wr_commit_w = 1'b0;
    pool_wr_slot_w   = '0;
    pool_wr_addr_w   = '0;
    pool_wr_data_w   = 8'd0;
    pool_wr_len_w    = '0;
    if (txc_locked_r) begin
      unique case (txc_owner_r)
        2'(TXC_ADP_C): begin
          pool_alloc_req_w = adp_txs_alloc_req_w;
          pool_wr_valid_w  = adp_txs_wr_valid_w;
          pool_wr_commit_w = adp_txs_wr_commit_w;
          pool_wr_slot_w   = adp_txs_wr_slot_w;
          pool_wr_addr_w   = adp_txs_wr_addr_w;
          pool_wr_data_w   = adp_txs_wr_data_w;
          pool_wr_len_w    = adp_txs_wr_len_w;
        end
        2'(TXC_LSTN_C): begin
          pool_alloc_req_w = lstn_txs_alloc_req_w;
          pool_wr_valid_w  = lstn_txs_wr_valid_w;
          pool_wr_commit_w = lstn_txs_wr_commit_w;
          pool_wr_slot_w   = lstn_txs_wr_slot_w;
          pool_wr_addr_w   = lstn_txs_wr_addr_w;
          pool_wr_data_w   = lstn_txs_wr_data_w;
          pool_wr_len_w    = lstn_txs_wr_len_w;
        end
        2'(TXC_SRP_C): begin
          pool_alloc_req_w = srp_txs_alloc_req_w;
          pool_wr_valid_w  = srp_txs_wr_valid_w;
          pool_wr_commit_w = srp_txs_wr_commit_w;
          pool_wr_slot_w   = srp_txs_wr_slot_w;
          pool_wr_addr_w   = srp_txs_wr_addr_w;
          pool_wr_data_w   = srp_txs_wr_data_w;
          pool_wr_len_w    = srp_txs_wr_len_w;
        end
        default: begin  // TXC_TKB_C
          pool_alloc_req_w = tkb_alloc_req_w;
          pool_wr_valid_w  = (tkb_st_r == TKB_WR);
          pool_wr_commit_w = (tkb_st_r == TKB_COMMIT);
          pool_wr_slot_w   = tkb_slot_r;
          pool_wr_addr_w   = {5'd0, tkb_bidx_r};
          pool_wr_data_w   = tkb_pdu_byte_w;
          pool_wr_len_w    = TXA_W_C'(56);
        end
      endcase
    end
  end

  assign adp_txs_gnt_w  = txc_locked_r && (txc_owner_r == 2'(TXC_ADP_C))
                        && pool_alloc_gnt_w;
  assign lstn_txs_gnt_w = txc_locked_r && (txc_owner_r == 2'(TXC_LSTN_C))
                        && pool_alloc_gnt_w;
  assign srp_txs_gnt_w  = txc_locked_r && (txc_owner_r == 2'(TXC_SRP_C))
                        && pool_alloc_gnt_w;
  assign tkb_gnt_w      = txc_locked_r && (txc_owner_r == 2'(TXC_TKB_C))
                        && pool_alloc_gnt_w;
  assign adp_txs_gnt_slot_w  = pool_alloc_slot_w;
  assign lstn_txs_gnt_slot_w = pool_alloc_slot_w;
  assign srp_txs_gnt_slot_w  = pool_alloc_slot_w;
  assign tkb_gnt_slot_w      = pool_alloc_slot_w;

  logic        ser_req_w, ser_valid_w, ser_last_w, ser_ready_w;
  logic [TXS_W_C-1:0] ser_slot_w;
  logic [7:0]  ser_data_w;
  logic [TX_STD_SLOTS_P:0] txs_ready_nc_w;
  logic [$clog2(TX_STD_SLOTS_P+2)-1:0] txs_free_w;

  KL_pp_tx_slots #(
      .TX_STD_SLOTS_P      (TX_STD_SLOTS_P),
      .TX_STD_BYTES_P      (576),
      .TX_OVERSIZE_BYTES_P (TX_OVERSIZE_BYTES_P)
  ) u_tx_slots (
      .clk_i         (clk_i),
      .rst_n         (rst_n),
      .alloc_req_i   (pool_alloc_req_w),
      .oversize_i    (1'b0),               // no oversize builder until P4 Δ8
      .alloc_gnt_o   (pool_alloc_gnt_w),
      .alloc_slot_o  (pool_alloc_slot_w),
      .wr_slot_i     (pool_wr_slot_w),
      .wr_addr_i     (pool_wr_addr_w),
      .wr_valid_i    (pool_wr_valid_w),
      .wr_data_i     (pool_wr_data_w),
      .wr_commit_i   (pool_wr_commit_w),
      .wr_len_i      (pool_wr_len_w),
      .ser_req_i     (ser_req_w),
      .ser_slot_i    (ser_slot_w),
      .ser_valid_o   (ser_valid_w),
      .ser_data_o    (ser_data_w),
      .ser_last_o    (ser_last_w),
      .ser_ready_i   (ser_ready_w),
      .slots_ready_o (txs_ready_nc_w),
      .slots_free_o  (txs_free_w)
  );

  // ---- TX lane queues: the ADP engine and the listener pulse one-cycle
  // txreq strobes; the arbiter wants requests HELD until grant. Depth-8
  // slot-handle queues are structurally lossless (at most 5 slots exist).
  logic [7:0][TXS_W_C-1:0] laneq_adp_r, laneq_acmp_r;
  logic [3:0]              laneq_adp_cnt_r, laneq_acmp_cnt_r;
  logic [3:0]              laneq_adp_mid_w, laneq_acmp_mid_w;
  logic                    laneq_adp_push_w, laneq_acmp_push_w;

  logic [5:0]              arb_req_w;
  logic [5:0][TXS_W_C-1:0] arb_slot_w;
  logic [5:0]              arb_gnt_w;
  logic [5:0][15:0]        arb_gnt_cnt_w;

  // count after this cycle's pop; a push appends at that position (a queue
  // deeper than the 5 physical slots can never overflow — see banner)
  assign laneq_adp_mid_w  = laneq_adp_cnt_r
                          - (arb_gnt_w[LANE_ADP_C] ? 4'd1 : 4'd0);
  assign laneq_acmp_mid_w = laneq_acmp_cnt_r
                          - (arb_gnt_w[LANE_ACMP_C] ? 4'd1 : 4'd0);
  assign laneq_adp_push_w  = adp_txreq_valid_w  && (laneq_adp_mid_w != 4'd8);
  assign laneq_acmp_push_w = lstn_txreq_valid_w && (laneq_acmp_mid_w != 4'd8);

  always_ff @(posedge clk_i) begin : lane_queues
    if (!rst_n) begin
      laneq_adp_r      <= '0;
      laneq_acmp_r     <= '0;
      laneq_adp_cnt_r  <= 4'd0;
      laneq_acmp_cnt_r <= 4'd0;
    end else begin
      if (arb_gnt_w[LANE_ADP_C]) begin
        for (int unsigned i = 0; i < 7; i++) begin
          laneq_adp_r[i] <= laneq_adp_r[i + 1];
        end
      end
      if (laneq_adp_push_w) begin
        laneq_adp_r[laneq_adp_mid_w[2:0]] <= adp_txreq_slot_w;
      end
      laneq_adp_cnt_r <= laneq_adp_mid_w + (laneq_adp_push_w ? 4'd1 : 4'd0);

      if (arb_gnt_w[LANE_ACMP_C]) begin
        for (int unsigned i = 0; i < 7; i++) begin
          laneq_acmp_r[i] <= laneq_acmp_r[i + 1];
        end
      end
      if (laneq_acmp_push_w) begin
        laneq_acmp_r[laneq_acmp_mid_w[2:0]] <= lstn_txreq_slot_w;
      end
      laneq_acmp_cnt_r <= laneq_acmp_mid_w
                        + (laneq_acmp_push_w ? 4'd1 : 4'd0);
    end
  end

  assign arb_req_w[LANE_AECP_SOL_C] = 1'b0;             // P4 uCPU
  assign arb_req_w[LANE_AECP_UNS_C] = 1'b0;             // P4 notifications
  assign arb_req_w[LANE_ACMP_C]     = laneq_acmp_cnt_r != 4'd0;
  assign arb_req_w[LANE_ADP_C]      = laneq_adp_cnt_r != 4'd0;
  assign arb_req_w[LANE_SRP_C]      = srp_txreq_valid_w;
  assign arb_req_w[LANE_TKRSP_C]    = tkb_lane_valid_r;
  assign arb_slot_w[LANE_AECP_SOL_C] = PP_SLOT_NULL_C;
  assign arb_slot_w[LANE_AECP_UNS_C] = PP_SLOT_NULL_C;
  assign arb_slot_w[LANE_ACMP_C]     = laneq_acmp_r[0];
  assign arb_slot_w[LANE_ADP_C]      = laneq_adp_r[0];
  assign arb_slot_w[LANE_SRP_C]      = srp_txreq_slot_w;
  assign arb_slot_w[LANE_TKRSP_C]    = tkb_slot_r;
  assign srp_txreq_ready_w = arb_gnt_w[LANE_SRP_C];
  assign tkb_lane_gnt_w    = arb_gnt_w[LANE_TKRSP_C];

  logic        arb_tx_valid_w, arb_tx_sof_w, arb_tx_eof_w, arb_tx_ready_w;
  logic [7:0]  arb_tx_data_w;

  KL_pp_tx_arbiter #(
      .TX_STD_SLOTS_P (TX_STD_SLOTS_P)
  ) u_tx_arbiter (
      .clk_i       (clk_i),
      .rst_n       (rst_n),
      .tick_ms_i   (tick_ms_w),
      .req_valid_i (arb_req_w),
      .tx_slot_i   (arb_slot_w),
      .gnt_o       (arb_gnt_w),
      .gnt_count_o (arb_gnt_cnt_w),
      .ser_req_o   (ser_req_w),
      .ser_slot_o  (ser_slot_w),
      .ser_valid_i (ser_valid_w),
      .ser_data_i  (ser_data_w),
      .ser_last_i  (ser_last_w),
      .ser_ready_o (ser_ready_w),
      .tx_valid_o  (arb_tx_valid_w),
      .tx_sof_o    (arb_tx_sof_w),
      .tx_data_o   (arb_tx_data_w),
      .tx_eof_o    (arb_tx_eof_w),
      .tx_ready_i  (arb_tx_ready_w)
  );

  // ---- ACMP Ethernet-header prepend shim (banner): lanes 2/5 carry bare
  // 56-byte ACMPDUs (the listener and the talker builder commit PDUs, ADP
  // and SRP commit whole frames — the recorded asymmetry of the landed
  // tree). Grants are frame-atomic, so the winning lane latched at gnt time
  // tags the very next frame; the shim stalls the arbiter for 14 cycles
  // while it emits DA 91-E0-F0-01-00-00 + own SA + EtherType 0x22F0 (03 §8
  // destination addressing: ALL ACMP traffic is that multicast).
  typedef enum logic [1:0] { SH_IDLE, SH_HDR, SH_BODY } sh_state_e;

  sh_state_e   sh_st_r;
  logic        sh_need_r;
  logic [3:0]  sh_idx_r;
  logic [111:0] sh_hdr_w;
  logic [7:0]   sh_byte_w;

  assign sh_hdr_w  = {48'h91E0_F001_0000, own_mac_i, 16'h22F0};
  assign sh_byte_w = sh_hdr_w[(7'd104 - {sh_idx_r, 3'b000}) +: 8];

  always_ff @(posedge clk_i) begin : prepend_shim
    if (!rst_n) begin
      sh_st_r   <= SH_IDLE;
      sh_need_r <= 1'b0;
      sh_idx_r  <= 4'd0;
    end else begin
      if (|arb_gnt_w) begin
        sh_need_r <= arb_gnt_w[LANE_ACMP_C] || arb_gnt_w[LANE_TKRSP_C];
      end
      unique case (sh_st_r)
        SH_IDLE: begin
          if (arb_tx_valid_w && sh_need_r) begin
            if (tx_ready_i) begin
              sh_idx_r <= 4'd1;          // header byte 0 went out this cycle
              sh_st_r  <= SH_HDR;
            end
          end
        end
        SH_HDR: begin
          if (tx_ready_i) begin
            if (sh_idx_r == 4'd13) sh_st_r <= SH_BODY;
            else                   sh_idx_r <= sh_idx_r + 4'd1;
          end
        end
        SH_BODY: begin
          if (arb_tx_valid_w && arb_tx_eof_w && tx_ready_i) begin
            sh_st_r  <= SH_IDLE;
            sh_idx_r <= 4'd0;
          end
        end
        default: sh_st_r <= SH_IDLE;
      endcase
    end
  end

  always_comb begin : shim_stream
    unique case (sh_st_r)
      SH_IDLE: begin
        if (arb_tx_valid_w && sh_need_r) begin
          tx_valid_o     = 1'b1;
          tx_sof_o       = 1'b1;
          tx_data_o      = sh_hdr_w[111:104];  // header byte 0
          tx_eof_o       = 1'b0;
          arb_tx_ready_w = 1'b0;
        end else begin
          tx_valid_o     = arb_tx_valid_w;
          tx_sof_o       = arb_tx_sof_w;
          tx_data_o      = arb_tx_data_w;
          tx_eof_o       = arb_tx_eof_w;
          arb_tx_ready_w = tx_ready_i;
        end
      end
      SH_HDR: begin
        tx_valid_o     = 1'b1;
        tx_sof_o       = 1'b0;
        tx_data_o      = sh_byte_w;
        tx_eof_o       = 1'b0;
        arb_tx_ready_w = 1'b0;
      end
      SH_BODY: begin
        tx_valid_o     = arb_tx_valid_w;
        tx_sof_o       = 1'b0;               // sof already emitted
        tx_data_o      = arb_tx_data_w;
        tx_eof_o       = arb_tx_eof_w;
        arb_tx_ready_w = tx_ready_i;
      end
      default: begin
        tx_valid_o     = arb_tx_valid_w;
        tx_sof_o       = arb_tx_sof_w;
        tx_data_o      = arb_tx_data_w;
        tx_eof_o       = arb_tx_eof_w;
        arb_tx_ready_w = tx_ready_i;
      end
    endcase
  end

  // =========================================================================
  // side port + backends: snapshot (class D aggregated), ctrl, trace
  // =========================================================================
  logic        sp_img_req_w, sp_img_we_nc_w;
  logic [15:0] sp_img_addr_nc_w;
  logic [31:0] sp_img_wdata_nc_w;
  logic        sp_img_rvalid_r;
  logic        sp_dbg_req_w;
  logic [15:0] sp_dbg_addr_nc_w;
  logic        sp_dbg_rvalid_r;
  logic        sp_snap_req_w;
  logic [15:0] sp_snap_addr_w;
  logic [31:0] sp_snap_rdata_r;
  logic        sp_snap_rvalid_r;
  logic        sp_ctrl_req_w, sp_ctrl_we_w;
  logic [7:0]  sp_ctrl_addr_w;
  logic [31:0] sp_ctrl_wdata_w;
  logic [31:0] sp_ctrl_rdata_r;
  logic        sp_ctrl_rvalid_r;
  logic        sp_trace_req_w;
  logic [15:0] sp_trace_addr_w;
  logic        sp_trace_rvalid_r;
  logic        sp_fw_req_nc_w, sp_fw_we_nc_w;
  logic [15:0] sp_fw_addr_nc_w;
  logic [31:0] sp_fw_wdata_nc_w;

  KL_pp_side_port #(.EN_FW_ASSIST_P(1'b0)) u_side_port (
      .clk_i           (clk_i),
      .rst_n           (rst_n),
      .entity_enable_i (entity_enable_i),
      .req_valid_i     (host_req_valid_i),
      .we_i            (host_we_i),
      .addr_i          (host_addr_i),
      .wdata_i         (host_wdata_i),
      .rdata_o         (host_rdata_o),
      .rvalid_o        (host_rvalid_o),
      .err_o           (host_err_o),
      .img_req_o       (sp_img_req_w),
      .img_we_o        (sp_img_we_nc_w),
      .img_addr_o      (sp_img_addr_nc_w),
      .img_wdata_o     (sp_img_wdata_nc_w),
      .img_rdata_i     (32'd0),              // descriptor image RAM: P4
      .img_rvalid_i    (sp_img_rvalid_r),
      .dbg_req_o       (sp_dbg_req_w),
      .dbg_addr_o      (sp_dbg_addr_nc_w),
      .dbg_rdata_i     (32'd0),              // dynamic overlay view: P4
      .dbg_rvalid_i    (sp_dbg_rvalid_r),
      .snap_req_o      (sp_snap_req_w),
      .snap_addr_o     (sp_snap_addr_w),
      .snap_rdata_i    (sp_snap_rdata_r),
      .snap_rvalid_i   (sp_snap_rvalid_r),
      .ctrl_req_o      (sp_ctrl_req_w),
      .ctrl_we_o       (sp_ctrl_we_w),
      .ctrl_addr_o     (sp_ctrl_addr_w),
      .ctrl_wdata_o    (sp_ctrl_wdata_w),
      .ctrl_rdata_i    (sp_ctrl_rdata_r),
      .ctrl_rvalid_i   (sp_ctrl_rvalid_r),
      .trace_req_o     (sp_trace_req_w),
      .trace_addr_o    (sp_trace_addr_w),
      .trace_rdata_i   (trc_rd_data_w),
      .trace_rvalid_i  (sp_trace_rvalid_r),
      .fw_req_o        (sp_fw_req_nc_w),
      .fw_we_o         (sp_fw_we_nc_w),
      .fw_addr_o       (sp_fw_addr_nc_w),
      .fw_wdata_o      (sp_fw_wdata_nc_w),
      .fw_rdata_i      (32'd0),              // P-EN-FIRMWARE-ASSIST = 0
      .fw_rvalid_i     (1'b0)
  );

  assign trc_rd_en_w   = sp_trace_req_w;
  assign trc_rd_addr_w = sp_trace_addr_w[9:2];
  assign trc_rd_lane_w = sp_trace_addr_w[1:0];

  // snapshot window: the F02.10 class-D dictionary + front-end counters,
  // one registered read mux (word map in tb/pp_top/README.md)
  logic [31:0] ctrl_scratch_r;

  always_ff @(posedge clk_i) begin : side_backends
    if (!rst_n) begin
      sp_img_rvalid_r   <= 1'b0;
      sp_dbg_rvalid_r   <= 1'b0;
      sp_snap_rvalid_r  <= 1'b0;
      sp_snap_rdata_r   <= 32'd0;
      sp_ctrl_rvalid_r  <= 1'b0;
      sp_ctrl_rdata_r   <= 32'd0;
      sp_trace_rvalid_r <= 1'b0;
      ctrl_scratch_r    <= 32'd0;
    end else begin
      sp_img_rvalid_r   <= sp_img_req_w;
      sp_dbg_rvalid_r   <= sp_dbg_req_w;
      sp_trace_rvalid_r <= sp_trace_req_w;
      sp_snap_rvalid_r  <= sp_snap_req_w;
      sp_ctrl_rvalid_r  <= sp_ctrl_req_w;

      if (sp_ctrl_req_w && sp_ctrl_we_w && (sp_ctrl_addr_w == 8'd0)) begin
        ctrl_scratch_r <= sp_ctrl_wdata_w;
      end
      unique case (sp_ctrl_addr_w)
        8'd0:    sp_ctrl_rdata_r <= ctrl_scratch_r;
        8'd1:    sp_ctrl_rdata_r <= {28'd0, restore_fail_o, restore_done_o,
                                     restore_busy_o, entity_enable_i};
        default: sp_ctrl_rdata_r <= 32'd0;
      endcase

      unique case (sp_snap_addr_w[5:0])
        6'd0:  sp_snap_rdata_r <= 32'h4B4C_5050;               // "KLPP"
        6'd1:  sp_snap_rdata_r <= {8'(N_STREAM_IN_P), 8'(N_STREAM_OUT_P),
                                   8'(RX_SLOTS_P), 8'(TX_STD_SLOTS_P)};
        6'd2:  sp_snap_rdata_r <= now_ms_w;
        6'd3:  sp_snap_rdata_r <= {26'd0, nvm_alarm_o, srp_over_limit_w,
                                   srp_domain_adopted_w, prng_seeded_w,
                                   link_up_i, entity_enable_i};
        6'd4:  sp_snap_rdata_r <= {cnt_rx_da_w, cnt_rx_ethertype_w};
        6'd5:  sp_snap_rdata_r <= {cnt_rx_subtype_w, cnt_rx_version_w};
        6'd6:  sp_snap_rdata_r <= {cnt_rx_length_w, rxp_overrun_w[RXP_ADP_C]};
        6'd7:  sp_snap_rdata_r <= {disp_adp_level_w, disp_acmp_level_w,
                                   disp_aecp_level_w, 8'd0};
        6'd8:  sp_snap_rdata_r <= {disp_adp_stall_w, disp_acmp_stall_w};
        6'd9:  sp_snap_rdata_r <= {disp_aecp_stall_w, hdr_drop_r};
        6'd10: sp_snap_rdata_r <= {13'd0, srp_class_a_prio_w, 4'd0,
                                   srp_class_a_vid_w};
        6'd11: sp_snap_rdata_r <= srp_sum_slope_w;
        //! The snapshot window is a FIXED 32-bit register map (07 §5),
        //! so each field keeps its documented lane: sources beyond 8 are
        //! not observable here, they are on the per-source class-D ports.
        6'd12: sp_snap_rdata_r <= {8'(srp_sr_admitted_w), 8'(srp_active_w),
                                   16'(srp_tk_reg_state_w)};
        6'd13: sp_snap_rdata_r <= {16'(srp_tk_decl_state_w),
                                   16'(srp_lstn_reg_state_w)};
        6'd14: sp_snap_rdata_r <= {16'(srp_lstn_decl_state_w), 12'd0,
                                   srp_dbg_vid_active_w};
        6'd15: sp_snap_rdata_r <= {sb_holds_w, 6'd0, sb_full_w, sb_barrier_w,
                                   trc_wr_count_w};
        6'd16, 6'd17, 6'd18, 6'd19, 6'd20, 6'd21, 6'd22, 6'd23:
               sp_snap_rdata_r <= srp_acc_latency_w[sp_snap_addr_w[2:0]];
        6'd24: sp_snap_rdata_r <= {arm_drop_r, mrp_drop_w};
        6'd25: sp_snap_rdata_r <= {13'd0, 16'(rxp_slots_free_w[RXP_ADP_C]),
                                   txs_free_w};
        6'd26: sp_snap_rdata_r <= {arb_gnt_cnt_w[LANE_ACMP_C],
                                   arb_gnt_cnt_w[LANE_ADP_C]};
        6'd27: sp_snap_rdata_r <= {arb_gnt_cnt_w[LANE_SRP_C],
                                   arb_gnt_cnt_w[LANE_TKRSP_C]};
        6'd28: sp_snap_rdata_r <= {8'(bound_r), tkb_drop_r, svc_drop_r[7:0]};
        6'd29: sp_snap_rdata_r <= {rxf_drop_r, 8'd0,
                                   srp_snk_fail_code_w[0]};
        6'd30: sp_snap_rdata_r <= srp_granted_slope_w[0];
        6'd31: sp_snap_rdata_r <= {30'd0, adp_dbg_adv_state_w};
        default: sp_snap_rdata_r <= 32'd0;
      endcase
    end
  end

endmodule : protocol_processor_top
`default_nettype wire
