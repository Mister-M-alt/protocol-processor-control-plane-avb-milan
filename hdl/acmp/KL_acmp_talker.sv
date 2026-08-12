/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : KL_acmp_talker.sv
//  Project     : IEEE 1722.1 protocol processor
//                (docs/architecture/05 §6bis stateless talker responder
//                 F05.11 + per-source DA-gate F05.12; 08 §2 T-SRP-DAFRESH /
//                 T-SRP-LEAVEALL2; 08 §5 per-source shared timer slot;
//                 02 §4.1/§4.2 srp + maap faces; 03 §4 pp_txn_t dispatch-in)
//
//  Description : The Milan stateless talker responder and the per-source
//                dest-MAC lifecycle gate, for P-N-STREAM-OUT sources.
//
//                Responder (F05.11) — answers PROBE_TX / DISCONNECT_TX /
//                GET_TX_STATE / GET_TX_CONNECTION from CURRENT state only;
//                no per-controller state is ever stored (Milan §5.5.2.7,
//                Δ4). The two response tables are implemented EXACTLY as 05
//                records them, including their deliberate difference:
//                  - PROBE_TX_RESPONSE echoes FAST_CONNECT+STREAMING_WAIT
//                    from the command and FORCES REGISTERING_FAILED = 0
//                    (the flag law the pipewire reference inverted);
//                  - GET_TX_STATE zeroes the listener fields and reads
//                    REGISTERING_FAILED LIVE (= 1 iff the registered
//                    Listener attribute is ASKING_FAILED).
//                DISCONNECT_TX is always SUCCESS and changes nothing;
//                GET_TX_CONNECTION is NOT_SUPPORTED (Table 5.48). A PROBE_TX
//                whose ingress interface is not the output's AVB interface
//                is SILENTLY ignored (the doc's impl choice, taken here as
//                the cheaper of the two permitted behaviors).
//
//                DA-gate (F05.12) — per source: NO_DA → DA_OK on maap
//                ALLOC_DA success; declares the Talker attribute (gate-open
//                strobe) while DA_OK AND (T-SRP-DAFRESH alive OR a Listener
//                attribute is registered); withdraws (gate-close) when the
//                gate goes false. MAAP conflict / SR-class PCP change while
//                declaring: withdraw → BACKOFF for T-SRP-LEAVEALL2 (2 x a
//                T-MRP-LEAVEALL PRNG draw, kind 3) → re-declare (with the
//                new DA after a conflict — the conflicted DA is invalidated
//                and re-allocated at backoff expiry).
//
//                The one design decision that matters: FRESHNESS IS A
//                DERIVED PREDICATE over a stored ping timestamp — never a
//                strobe-set/expiry-cleared flag. DAFRESH and LEAVEALL2
//                share ONE timer slot per source (08 §5), so a conflict
//                backoff necessarily evicts the freshness deadline from the
//                timer; with a flag design the freshness window would be
//                destroyed (or worse, frozen stale-true) by every backoff.
//                Here fresh = pinged && (now - ping_ts < T-SRP-DAFRESH),
//                the timer expiry is only a RE-EVALUATION TRIGGER, and any
//                arm/cancel of a source's slot clears that source's pending
//                expiry (the arm supersedes it, mirroring the timer
//                service's own arm-wins rule). Leaving BACKOFF re-arms the
//                slot to the ABSOLUTE ping_ts + T-SRP-DAFRESH, so a probe
//                received during backoff keeps its exact remaining window.
//
//                Doc-gap decisions (F05.12 shows conflict only from
//                DECLARING): a conflict outside DECLARING invalidates the
//                DA and, from DA_OK, drops to NO_DA + re-alloc without
//                backoff (nothing was on the wire to withdraw). A source
//                disabled by configuration withdraws, releases its DA and
//                returns to NO_DA. Backoff expiry with the DA still invalid
//                re-enters through NO_DA + ALLOC_DA (F05.12 names the
//                target DA_OK; the alloc round-trip is the same arc split
//                over the maap response).
//
//                An ABSENT maap must DEGRADE, never deadlock. The request
//                face is a held valid/ready handshake, so a shim that never
//                asserts ready would otherwise park the single walker in
//                S_EV_MAAP forever — and that walker also serves PROBE_TX /
//                DISCONNECT_TX / GET_TX_STATE for EVERY source, so one
//                un-accepted allocation would silence the whole talker half
//                of ACMP (and, through the gate strobes, of SRP). The
//                request is therefore ABANDONED after P-MAAP-ACCEPT-CYC
//                cycles; the source is left in exactly the state a REFUSED
//                allocation leaves it in (see the S_EV_MAAP arm).
//
//                Storage: one 1W1R sync-read record RAM (gstate, da_valid,
//                pinged, ping_ts, da — the 16 B/source of 07 §6), walked by
//                a single event-serialized FSM — never a flop mirror + wide
//                mux. RX payload fields (listener EID/unique_id, flags) are
//                read from the RX slot per F05.13 offsets with the V3 rule:
//                bytes beyond the committed slot length read as 0.
//---------------------------------------------------------------------------//
`default_nettype none

module KL_acmp_talker
  import pp_pkg::*;
#(
    //! P-N-STREAM-OUT (F01.5 default 8): talker sources / DA gates
    parameter int unsigned N_STREAM_OUT_P  = 8,
    //! P-RX-SLOTS of the attached KL_pp_rx_slots (slot handle range)
    parameter int unsigned RX_SLOTS_P      = 4,
    //! P-RX-SLOT-BYTES of the attached KL_pp_rx_slots (address width)
    parameter int unsigned RX_SLOT_BYTES_P = 576,
    //! timer slots of the attached KL_pp_timer_service (index width only)
    parameter int unsigned TMR_SLOTS_P     = pp_pkg::PP_TIMER_SLOTS_C,
    //! first per-source shared slot: IF + 2*SI by the 08 §5 F08.4 order
    parameter int unsigned TMR_SLOT_BASE_P = 17,
    //! owner-tag base echoed at expiry (this engine's owner space)
    parameter int unsigned TMR_OWNER_BASE_P = 32'h50,
    //! T-SRP-DAFRESH in ms (F08.1: 15 s)
    parameter int unsigned DAFRESH_MS_P    = 15000,
    //! P-MAAP-ACCEPT-CYC: cycles a maap request is held before it is
    //! ABANDONED. Bounded from both sides. Lower bound: the face is a
    //! ready handshake into an adjacent fabric block, so even a maap engine
    //! that is busy with another source's allocation asserts ready in tens
    //! of cycles — 1024 never cuts a live shim short. Upper bound: this is
    //! the worst case a talker COMMAND waits behind an allocation, and at
    //! P-CLK-HZ = 100 MHz 1024 cycles is 10.24 us, ~5000x inside the 50 ms
    //! T-BUDGET-ACMP-RESP of 08 §4. NOTE it times the request HANDSHAKE
    //! only — the allocation itself (maap_rsp_valid_i, legitimately seconds
    //! of MAAP probing) is never timed out.
    parameter int unsigned MAAP_ACCEPT_CYC_P = 1024,
    //! derived source-index width — do not override
    localparam int unsigned SRC_W_C = (N_STREAM_OUT_P > 32'd1)
                                      ? $clog2(N_STREAM_OUT_P) : 32'd1,
    //! derived RX slot-handle width — do not override
    localparam int unsigned RXS_W_C = (RX_SLOTS_P > 32'd1)
                                      ? $clog2(RX_SLOTS_P) : 32'd1,
    //! derived RX byte-address / length widths — do not override
    localparam int unsigned RXA_W_C = $clog2(RX_SLOT_BYTES_P),
    localparam int unsigned RXL_W_C = $clog2(RX_SLOT_BYTES_P + 1),
    //! derived timer slot-index width — do not override
    localparam int unsigned TMR_AW_C = (TMR_SLOTS_P > 32'd1)
                                       ? $clog2(TMR_SLOTS_P) : 32'd1,
    //! derived maap-accept counter width — do not override
    localparam int unsigned MTMO_W_C = (MAAP_ACCEPT_CYC_P > 32'd1)
                                       ? $clog2(MAAP_ACCEPT_CYC_P) : 32'd1
) (
    input  wire                          clk_i,   //! core clock (P-CLK-HZ)
    input  wire                          rst_n,   //! sync active-low reset

    //! ---- identity + quasi-static configuration (02 §2 rule 4) ----
    input  wire [63:0]                   own_entity_id_i,  //! own entity_id (= talker_entity_id guard)
    input  wire [N_STREAM_OUT_P-1:0]     cfg_src_en_i,     //! source exists in the current configuration
    input  wire [N_STREAM_OUT_P*2-1:0]   cfg_src_iface_i,  //! per-source AVB interface (2 b each, F05.11 v2 check)
    input  wire [N_STREAM_OUT_P*64-1:0]  cfg_stream_id_i,  //! per-source stream_id (64 b each, declared/answered)

    //! ---- srp declaration-state faces (02 §6 dictionary, live levels) ----
    input  wire [N_STREAM_OUT_P*2-1:0]   srp_lsn_reg_state_i, //! lstn_reg_state[src]: 0 NONE 1 READY 2 READY_FAILED 3 ASKING_FAILED
    input  wire [11:0]                   srp_class_vid_i,     //! SR-class VID (declared/answered stream_vlan_id)
    input  wire                          srp_pcp_change_i,    //! DOMAIN_CHANGE strobe: SR-class PCP changed (backoff trigger)

    //! ---- dispatch-in (03 §4; ready = consume strobe, record held until then) ----
    input  wire                          txn_valid_i,  //! transaction presented
    input  wire [PP_TXN_W_C-1:0]         txn_i,        //! pp_txn_t; target_eid = talker_entity_id for talker cmds
    output logic                         txn_ready_o,  //! one-cycle consume strobe

    //! ---- RX slot read/free (matches KL_pp_rx_slots exactly) ----
    output logic [RXS_W_C-1:0]           rxs_rd_slot_o,   //! slot handle to read
    output logic [RXA_W_C-1:0]           rxs_rd_addr_o,   //! byte address within the slot
    output logic                         rxs_rd_en_o,     //! sync-read enable
    input  wire  [7:0]                   rxs_rd_data_i,   //! read byte, one cycle after rd_en
    input  wire  [RXL_W_C-1:0]           rxs_slot_len_i,  //! committed slot length (V3 zero-fill line)
    output logic                         rxs_free_o,      //! return the consumed slot
    output logic [RXS_W_C-1:0]           rxs_free_slot_o, //! which slot to return

    //! ---- response build request (F05.13 fields; one-cycle strobe) ----
    output logic                         resp_valid_o,          //! response request strobe
    output logic [3:0]                   resp_msg_type_o,       //! response message_type (cmd + 1)
    output logic [4:0]                   resp_status_o,         //! IEEE Table 8-3 status
    output logic [63:0]                  resp_stream_id_o,      //! stream_id @4
    output logic [63:0]                  resp_controller_eid_o, //! controller_entity_id @12
    output logic [63:0]                  resp_talker_eid_o,     //! talker_entity_id @20 (= own)
    output logic [63:0]                  resp_listener_eid_o,   //! listener_entity_id @28
    output logic [15:0]                  resp_talker_uid_o,     //! talker_unique_id @36
    output logic [15:0]                  resp_listener_uid_o,   //! listener_unique_id @38
    output logic [47:0]                  resp_dest_mac_o,       //! stream_dest_mac @40
    output logic [15:0]                  resp_conn_count_o,     //! connection_count @46 (always 0, Δ4)
    output logic [15:0]                  resp_seq_id_o,         //! sequence_id @48 (echoed)
    output logic [15:0]                  resp_flags_o,          //! flags @50 (per-table law)
    output logic [15:0]                  resp_vlan_id_o,        //! stream_vlan_id @52
    output logic [1:0]                   resp_if_index_o,       //! egress = ingress interface

    //! ---- maap face (02 §4.2: single-outstanding ALLOC/RELEASE + conflict event) ----
    output logic                         maap_req_valid_o,   //! request strobe (held until ready, or P-MAAP-ACCEPT-CYC)
    input  wire                          maap_req_ready_i,   //! maap accepts the request
    output logic                         maap_req_release_o, //! 0 = ALLOC_DA, 1 = RELEASE_DA
    output logic [SRC_W_C-1:0]           maap_req_src_o,     //! source index of the request
    input  wire                          maap_rsp_valid_i,   //! one response per request
    input  wire                          maap_rsp_ok_i,      //! ALLOC_DA success
    input  wire  [47:0]                  maap_rsp_da_i,      //! allocated dest MAC (with ok)
    input  wire                          maap_conflict_valid_i, //! MAAP_CONFLICT{source} event (sticky until ack)
    input  wire  [SRC_W_C-1:0]           maap_conflict_src_i,   //! conflicted source
    output logic                         maap_conflict_ack_o,   //! event ack

    //! ---- srp talker-face gate strobes (02 §4.1 DECLARE/WITHDRAW_TALKER) ----
    //! LEVEL: per-source DA gate is open (record gstate == GS_DECLARING).
    //! The gate_open_o/gate_close_o strobes below cannot be integrated into
    //! this by a consumer, because their predicates carry a da_valid term the
    //! level does not — which is why this is published rather than inferred.
    //! An integrating fabric gates stream egress on it every clock.
    output logic [N_STREAM_OUT_P-1:0]    declaring_o,
    output logic                         gate_open_o,      //! one-cycle: DECLARE_TALKER{src, sid, da, vid}
    output logic                         gate_close_o,     //! one-cycle: WITHDRAW_TALKER{src}
    output logic [SRC_W_C-1:0]           gate_src_o,       //! source index of the strobe
    output logic [63:0]                  gate_stream_id_o, //! declared stream_id (with open)
    output logic [47:0]                  gate_da_o,        //! declared dest MAC (with open)
    output logic [11:0]                  gate_vlan_o,      //! declared VLAN (with open)

    //! ---- timer service face (matches KL_pp_timer_service exactly) ----
    input  wire  [31:0]                  now_ms_i,             //! absolute ms timebase
    output logic                         tmr_arm_valid_o,      //! arm/cancel strobe
    output logic                         tmr_arm_cancel_o,     //! 1 = cancel
    output logic [TMR_AW_C-1:0]          tmr_arm_slot_o,       //! per-source shared slot (BASE + src)
    output logic [PP_TIMER_OWNER_W_C-1:0] tmr_arm_owner_o,     //! owner tag (OWNER_BASE + src)
    output logic [31:0]                  tmr_arm_deadline_ms_o,//! absolute deadline
    input  wire                          tmr_exp_valid_i,      //! expiry event
    input  wire  [TMR_AW_C-1:0]          tmr_exp_slot_i,       //! expired slot
    input  wire  [PP_TIMER_OWNER_W_C-1:0] tmr_exp_owner_i,     //! owner tag of the expiry

    //! ---- prng draw face (matches KL_pp_prng: kind 3 = T-MRP-LEAVEALL) ----
    output logic                         prng_draw_req_o,  //! one-cycle draw request
    output logic [2:0]                   prng_draw_kind_o, //! always 3 (10..15 s)
    input  wire                          prng_draw_busy_i, //! draw in progress
    input  wire                          prng_draw_valid_i,//! result strobe
    input  wire  [15:0]                  prng_draw_ms_i    //! drawn ms (x2 = T-SRP-LEAVEALL2)
);

  // ------------------------------------------------------------ constants
  //! ACMP message types (05 §3, Milan names)
  localparam logic [3:0] MT_PROBE_TX_C = 4'd0;
  localparam logic [3:0] MT_DISC_TX_C  = 4'd2;
  localparam logic [3:0] MT_GTX_ST_C   = 4'd4;
  localparam logic [3:0] MT_GTX_CN_C   = 4'd12;

  //! IEEE Table 8-3 status codes used by F05.11
  localparam logic [4:0] ST_SUCCESS_C        = 5'd0;
  localparam logic [4:0] ST_TALKER_UNKNOWN_C = 5'd2;
  localparam logic [4:0] ST_DEST_MAC_FAIL_C  = 5'd3;
  localparam logic [4:0] ST_NOT_SUPPORTED_C  = 5'd31;

  //! flags masks (05 §3; MSB-first wire warning applies to the builder)
  localparam logic [15:0] FLG_ECHO_MASK_C  = 16'h000A; // FAST_CONNECT | STREAMING_WAIT
  localparam logic [15:0] FLG_REG_FAILED_C = 16'h0040; // REGISTERING_FAILED

  //! lstn_reg_state encodings (02 §6 F02.10 listed order)
  localparam logic [1:0] LSN_NONE_C          = 2'd0;
  localparam logic [1:0] LSN_ASKING_FAILED_C = 2'd3;

  //! DA-gate states (F05.12)
  localparam logic [1:0] GS_NO_DA_C     = 2'd0;
  localparam logic [1:0] GS_DA_OK_C     = 2'd1;
  localparam logic [1:0] GS_DECLARING_C = 2'd2;
  localparam logic [1:0] GS_BACKOFF_C   = 2'd3;

  // ------------------------------------------------------- record storage
  //! per-source DA-gate record (07 §6 "source DA gates")
  typedef struct packed {
    logic [1:0]  gstate;    // GS_*
    logic        da_valid;  // MAAP allocated AND no conflict
    logic        pinged;    // ping_ts holds a real probe timestamp
    logic [31:0] ping_ts;   // ms timestamp of the last accepted PROBE_TX
    logic [47:0] da;        // allocated dest MAC
  } tk_rec_t;

  localparam int unsigned REC_W_C = $bits(tk_rec_t); // 84

  // 1W1R sync-read RAM; the walker FSM is the only writer. No reset on the
  // array or its read register (RAM inference); the S_INIT sweep writes
  // every record before any consumer can read one.
  logic [REC_W_C-1:0] rec_ram_r [0:N_STREAM_OUT_P-1];
  logic [REC_W_C-1:0] rec_q_r;

  logic                rec_we_w;
  logic [SRC_W_C-1:0]  rec_waddr_w;
  logic [REC_W_C-1:0]  rec_wdata_w;
  logic                rec_re_w;
  logic [SRC_W_C-1:0]  rec_raddr_w;

  always_ff @(posedge clk_i) begin : rec_ram_write
    if (rec_we_w) begin
      rec_ram_r[rec_waddr_w] <= rec_wdata_w;
    end
  end

  //! Flop mirror of the record RAM's gstate, published as declaring_o. The
  //! RAM is 1W1R sync-read, so a consumer cannot see all N gates at once —
  //! but an integrating fabric needs every gate every clock. Mirroring on the
  //! write port keeps this exactly in step with the RAM (same address, same
  //! cycle, one writer) at the cost of N flops, and the RAM stays the single
  //! source of gate truth for everything inside this engine.
  //! a field select directly on a cast is not parseable here, so name the
  //! view: this is rec_wdata_w read as the record it is about to become.
  tk_rec_t rec_wdata_s;
  assign rec_wdata_s = tk_rec_t'(rec_wdata_w);

  logic [N_STREAM_OUT_P-1:0] declaring_r;
  always_ff @(posedge clk_i) begin : declaring_mirror
    if (!rst_n) begin
      declaring_r <= '0;
    end else if (rec_we_w) begin
      declaring_r[rec_waddr_w] <= (rec_wdata_s.gstate == GS_DECLARING_C);
    end
  end
  assign declaring_o = declaring_r;

  always_ff @(posedge clk_i) begin : rec_ram_read
    if (rec_re_w) begin
      rec_q_r <= rec_ram_r[rec_raddr_w];
    end
  end

  tk_rec_t rec_w;
  assign rec_w = tk_rec_t'(rec_q_r);

  // ------------------------------------------------------------- helpers
  //! per-source AVB interface slice
  function automatic logic [1:0] src_iface_f(input logic [SRC_W_C-1:0] s);
    return cfg_src_iface_i[32'(s) * 2 +: 2];
  endfunction

  //! per-source configured stream_id slice
  function automatic logic [63:0] src_sid_f(input logic [SRC_W_C-1:0] s);
    return cfg_stream_id_i[32'(s) * 64 +: 64];
  endfunction

  //! per-source live Listener registration state slice
  function automatic logic [1:0] lsn_state_f(input logic [SRC_W_C-1:0] s);
    return srp_lsn_reg_state_i[32'(s) * 2 +: 2];
  endfunction

  //! a Listener attribute is registered toward this source (any state)
  function automatic logic lsn_reg_f(input logic [SRC_W_C-1:0] s);
    return (lsn_state_f(s) != LSN_NONE_C);
  endfunction

  //! derived freshness: probed within T-SRP-DAFRESH (wrap-safe mod-2^32)
  function automatic logic fresh_f(input tk_rec_t r);
    return r.pinged && ((now_ms_i - r.ping_ts) < 32'(DAFRESH_MS_P));
  endfunction

  //! lowest set index
  function automatic logic [SRC_W_C-1:0] ffs_f(
      input logic [N_STREAM_OUT_P-1:0] v);
    ffs_f = '0;
    for (int i = int'(N_STREAM_OUT_P) - 1; i >= 0; i--) begin
      if (v[i]) ffs_f = SRC_W_C'(i);
    end
  endfunction

  //! F05.13 byte offsets fetched from the RX slot (echo + flag fields)
  function automatic logic [RXA_W_C-1:0] pdu_off_f(input logic [3:0] ix);
    unique case (ix)
      4'd0:    pdu_off_f = RXA_W_C'(28); // listener_entity_id MSB
      4'd1:    pdu_off_f = RXA_W_C'(29);
      4'd2:    pdu_off_f = RXA_W_C'(30);
      4'd3:    pdu_off_f = RXA_W_C'(31);
      4'd4:    pdu_off_f = RXA_W_C'(32);
      4'd5:    pdu_off_f = RXA_W_C'(33);
      4'd6:    pdu_off_f = RXA_W_C'(34);
      4'd7:    pdu_off_f = RXA_W_C'(35); // listener_entity_id LSB
      4'd8:    pdu_off_f = RXA_W_C'(38); // listener_unique_id hi
      4'd9:    pdu_off_f = RXA_W_C'(39); // listener_unique_id lo
      4'd10:   pdu_off_f = RXA_W_C'(50); // flags hi
      default: pdu_off_f = RXA_W_C'(51); // flags lo
    endcase
  endfunction

  // ------------------------------------------------------------ event set
  // per-source pending-event flags (flop vectors — event STATE, not the
  // record storage; the record RAM stays the single source of gate truth)
  logic [N_STREAM_OUT_P-1:0] pe_conflict_r;
  logic [N_STREAM_OUT_P-1:0] pe_pcp_r;
  logic [N_STREAM_OUT_P-1:0] pe_tmr_r;
  logic [N_STREAM_OUT_P-1:0] pe_lsn_r;
  logic [N_STREAM_OUT_P-1:0] pe_init_r;
  logic [N_STREAM_OUT_P-1:0] pe_off_r;

  logic [N_STREAM_OUT_P*2-1:0] lsn_q_r;   // edge detector
  logic [N_STREAM_OUT_P-1:0]   en_q_r;    // edge detector

  // maap single-outstanding tracker + grant holding register
  logic               maap_busy_r;
  logic [SRC_W_C-1:0] maap_src_r;
  logic               maap_rel_r;
  logic               gp_valid_r;
  logic [SRC_W_C-1:0] gp_src_r;
  logic [47:0]        gp_da_r;

  logic maap_avail_w;
  assign maap_avail_w = !maap_busy_r && !gp_valid_r;

  // timer expiry -> source match (slot AND owner tag must agree)
  logic               exp_hit_w;
  logic [SRC_W_C-1:0] exp_src_w;
  logic [31:0]        exp_rel_w;

  assign exp_rel_w = {{(32 - TMR_AW_C){1'b0}}, tmr_exp_slot_i}
                     - TMR_SLOT_BASE_P;
  assign exp_hit_w = tmr_exp_valid_i
                     && (exp_rel_w < N_STREAM_OUT_P)
                     && (tmr_exp_owner_i ==
                         PP_TIMER_OWNER_W_C'(TMR_OWNER_BASE_P + exp_rel_w));
  assign exp_src_w = exp_rel_w[SRC_W_C-1:0];

  // conflict event: ack combinationally, latch the pending bit
  assign maap_conflict_ack_o = maap_conflict_valid_i;

  // --------------------------------------------------------------- walker
  typedef enum logic [3:0] {
    S_INIT         = 4'd0,   // record-RAM init sweep after reset
    S_IDLE         = 4'd1,
    S_TXN_REC      = 4'd2,   // record read for the dispatched transaction
    S_TXN_PDU      = 4'd3,   // RX-slot byte fetch (echo + flags fields)
    S_TXN_ACT      = 4'd4,   // respond + ping + pop + free (one cycle)
    S_EV_REC       = 4'd5,   // record read for a pending event
    S_EV_ACT       = 4'd6,   // apply the event (one cycle)
    S_EV_DRAW_REQ  = 4'd7,   // request the T-MRP-LEAVEALL draw
    S_EV_DRAW_WAIT = 4'd8,   // wait for the draw result
    S_EV_ARM       = 4'd9,   // arm T-SRP-LEAVEALL2 (= 2 x draw)
    S_EV_MAAP      = 4'd10   // hold the maap request until accepted
  } state_e;

  typedef enum logic [2:0] {
    EVC_GRANT    = 3'd0,
    EVC_OFF      = 3'd1,
    EVC_CONFLICT = 3'd2,
    EVC_PCP      = 3'd3,
    EVC_TMR      = 3'd4,
    EVC_LSN      = 3'd5,
    EVC_INIT     = 3'd6
  } evc_e;

  state_e             state_r;
  evc_e               ev_code_r;
  logic [SRC_W_C-1:0] ev_src_r;
  pp_txn_t            txn_r;
  logic [3:0]         fetch_ix_r;
  logic [RXL_W_C-1:0] slot_len_r;
  logic [63:0]        pdu_leid_r;
  logic [15:0]        pdu_luid_r;
  logic [15:0]        pdu_flags_r;
  logic [31:0]        arm_deadline_r;
  logic [SRC_W_C-1:0] mreq_src_r;
  logic               mreq_rel_r;
  logic [MTMO_W_C-1:0] mreq_tmo_r;   // cycles the request has been offered
  logic [SRC_W_C:0]   init_ix_r;

  //! the offered request has waited P-MAAP-ACCEPT-CYC cycles: abandon it
  logic maap_tmo_w;
  assign maap_tmo_w = (state_r == S_EV_MAAP)
                      && (mreq_tmo_r == MTMO_W_C'(MAAP_ACCEPT_CYC_P - 32'd1));

  // ------------------------------------------------------ dispatch picker
  typedef enum logic [1:0] { DK_NONE = 2'd0, DK_TXN = 2'd1, DK_EV = 2'd2 }
      dk_e;

  dk_e                disp_kind_w;
  evc_e               disp_code_w;
  logic [SRC_W_C-1:0] disp_src_w;

  always_comb begin : dispatcher
    disp_kind_w = DK_NONE;
    disp_code_w = EVC_GRANT;
    disp_src_w  = '0;
    if (state_r == S_IDLE) begin
      if (gp_valid_r) begin
        disp_kind_w = DK_EV;  disp_code_w = EVC_GRANT;
        disp_src_w  = gp_src_r;
      end else if (txn_valid_i) begin
        disp_kind_w = DK_TXN;
      end else if (|pe_off_r) begin
        disp_kind_w = DK_EV;  disp_code_w = EVC_OFF;
        disp_src_w  = ffs_f(pe_off_r);
      end else if (|pe_conflict_r) begin
        disp_kind_w = DK_EV;  disp_code_w = EVC_CONFLICT;
        disp_src_w  = ffs_f(pe_conflict_r);
      end else if (|pe_pcp_r) begin
        disp_kind_w = DK_EV;  disp_code_w = EVC_PCP;
        disp_src_w  = ffs_f(pe_pcp_r);
      end else if (|pe_tmr_r) begin
        disp_kind_w = DK_EV;  disp_code_w = EVC_TMR;
        disp_src_w  = ffs_f(pe_tmr_r);
      end else if (|pe_lsn_r) begin
        disp_kind_w = DK_EV;  disp_code_w = EVC_LSN;
        disp_src_w  = ffs_f(pe_lsn_r);
      end else if (|pe_init_r) begin
        disp_kind_w = DK_EV;  disp_code_w = EVC_INIT;
        disp_src_w  = ffs_f(pe_init_r);
      end
    end
  end

  // ------------------------------------------------------ txn-side decode
  // dispatch-in record view (for the same-cycle record-read address)
  pp_txn_t txn_in_w;
  assign txn_in_w = pp_txn_t'(txn_i);

  logic [15:0]        uid16_w;
  logic [SRC_W_C-1:0] tsrc_w;
  logic               uid_valid_w;
  logic               talker_cmd_w;
  logic               iface_ok_w;
  logic               ping_w;
  logic               da_ok_w;
  logic               slot_ok_w;
  logic               needs_pdu_w;

  assign uid16_w  = txn_r.operands.unique_id;
  assign tsrc_w   = uid16_w[SRC_W_C-1:0];

  assign uid_valid_w  = (uid16_w < 16'(N_STREAM_OUT_P))
                        && cfg_src_en_i[tsrc_w];
  assign talker_cmd_w = (txn_r.protocol == PP_PROTO_ACMP)
                        && (txn_r.target_eid == own_entity_id_i)
                        && ((txn_r.msg_type == MT_PROBE_TX_C)
                         || (txn_r.msg_type == MT_DISC_TX_C)
                         || (txn_r.msg_type == MT_GTX_ST_C)
                         || (txn_r.msg_type == MT_GTX_CN_C));
  assign iface_ok_w   = (src_iface_f(tsrc_w) == txn_r.interface_index);
  assign ping_w       = talker_cmd_w && (txn_r.msg_type == MT_PROBE_TX_C)
                        && uid_valid_w && iface_ok_w;
  assign da_ok_w      = rec_w.da_valid && !pe_conflict_r[tsrc_w];
  assign slot_ok_w    = (txn_r.rx_slot < 3'(RX_SLOTS_P));
  // PDU bytes are needed wherever the response echoes listener fields or
  // flags: PROBE (incl. unknown-id echo), DISCONNECT, GET_TX_CONNECTION.
  // GET_TX_STATE zeroes the listener fields by table — nothing to fetch.
  assign needs_pdu_w  = talker_cmd_w && slot_ok_w
                        && (((txn_r.msg_type == MT_PROBE_TX_C)
                             && (!uid_valid_w || iface_ok_w))
                         || (txn_r.msg_type == MT_DISC_TX_C)
                         || (txn_r.msg_type == MT_GTX_CN_C));

  // txn commit-cycle derived actions (S_TXN_ACT)
  tk_rec_t txn_rec2_w;
  logic    txn_open_w;
  logic    txn_initset_w;

  always_comb begin : txn_commit
    txn_rec2_w         = rec_w;
    txn_open_w         = 1'b0;
    txn_initset_w      = 1'b0;
    if (ping_w) begin
      txn_rec2_w.pinged  = 1'b1;
      txn_rec2_w.ping_ts = now_ms_i;
      if ((rec_w.gstate == GS_DA_OK_C) && da_ok_w) begin
        txn_rec2_w.gstate = GS_DECLARING_C;   // gate true: fresh just set
        txn_open_w        = 1'b1;
      end
      if (rec_w.gstate == GS_NO_DA_C) begin
        txn_initset_w = 1'b1;                 // retry the allocation
      end
    end
  end

  // ------------------------------------------------------ event-side eval
  tk_rec_t ev_rec2_w;
  logic    ev_we_w;
  logic    ev_open_w;
  logic    ev_close_w;
  logic    ev_arm_fresh_w;   // re-arm DAFRESH to ping_ts + T (backoff exit)
  logic    ev_cancel_w;      // cancel the shared slot (source removed)
  logic    ev_initset_w;
  logic    ev_to_draw_w;
  logic    ev_to_maap_w;
  logic    ev_maap_rel_w;

  logic ev_gate_w;           // gate predicate on the POST-mutation record
  logic ev_daok2_w;

  always_comb begin : ev_eval
    ev_rec2_w      = rec_w;
    ev_we_w        = 1'b0;
    ev_open_w      = 1'b0;
    ev_close_w     = 1'b0;
    ev_arm_fresh_w = 1'b0;
    ev_cancel_w    = 1'b0;
    ev_initset_w   = 1'b0;
    ev_to_draw_w   = 1'b0;
    ev_to_maap_w   = 1'b0;
    ev_maap_rel_w  = 1'b0;
    ev_daok2_w     = 1'b0;
    ev_gate_w      = 1'b0;

    unique case (ev_code_r)
      EVC_GRANT: begin
        ev_rec2_w.da       = gp_da_r;
        ev_rec2_w.da_valid = 1'b1;
        if (rec_w.gstate == GS_NO_DA_C) begin
          ev_rec2_w.gstate = GS_DA_OK_C;
        end
        ev_we_w    = 1'b1;
        ev_daok2_w = ev_rec2_w.da_valid && !pe_conflict_r[ev_src_r];
        ev_gate_w  = ev_daok2_w
                     && (fresh_f(ev_rec2_w) || lsn_reg_f(ev_src_r));
        if ((ev_rec2_w.gstate == GS_DA_OK_C) && ev_gate_w) begin
          ev_rec2_w.gstate = GS_DECLARING_C;
          ev_open_w        = 1'b1;
        end
      end

      EVC_OFF: begin
        if (rec_w.gstate == GS_DECLARING_C) begin
          ev_close_w = 1'b1;
        end
        ev_rec2_w   = '{gstate: GS_NO_DA_C, da_valid: 1'b0, pinged: 1'b0,
                        ping_ts: 32'd0, da: 48'd0};
        ev_we_w     = 1'b1;
        ev_cancel_w = 1'b1;
        if (rec_w.da_valid && maap_avail_w) begin
          ev_to_maap_w  = 1'b1;
          ev_maap_rel_w = 1'b1;
        end
      end

      EVC_CONFLICT: begin
        ev_rec2_w.da_valid = 1'b0;
        ev_we_w            = 1'b1;
        if (rec_w.gstate == GS_DECLARING_C) begin
          ev_close_w       = 1'b1;
          ev_rec2_w.gstate = GS_BACKOFF_C;
          ev_to_draw_w     = 1'b1;
        end else if (rec_w.gstate == GS_DA_OK_C) begin
          ev_rec2_w.gstate = GS_NO_DA_C;   // nothing declared: no backoff
          ev_initset_w     = 1'b1;
        end
      end

      EVC_PCP: begin
        if (rec_w.gstate == GS_DECLARING_C) begin
          ev_close_w       = 1'b1;
          ev_rec2_w.gstate = GS_BACKOFF_C;  // DA kept: only the class moved
          ev_we_w          = 1'b1;
          ev_to_draw_w     = 1'b1;
        end
      end

      EVC_TMR: begin
        if (rec_w.gstate == GS_BACKOFF_C) begin
          if (rec_w.da_valid) begin
            ev_rec2_w.gstate = GS_DA_OK_C;
            ev_we_w          = 1'b1;
            ev_daok2_w       = !pe_conflict_r[ev_src_r];
            ev_gate_w        = ev_daok2_w
                               && (fresh_f(rec_w) || lsn_reg_f(ev_src_r));
            if (ev_gate_w) begin
              ev_rec2_w.gstate = GS_DECLARING_C;
              ev_open_w        = 1'b1;
            end
            if (fresh_f(rec_w)) begin
              ev_arm_fresh_w = 1'b1;   // restore the exact remaining window
            end
          end else begin
            ev_rec2_w.gstate = GS_NO_DA_C;
            ev_we_w          = 1'b1;
            ev_initset_w     = 1'b1;   // re-allocate, then re-declare
          end
        end else begin
          // freshness lapse: re-evaluate the gate on the live inputs
          ev_daok2_w = rec_w.da_valid && !pe_conflict_r[ev_src_r];
          ev_gate_w  = ev_daok2_w
                       && (fresh_f(rec_w) || lsn_reg_f(ev_src_r));
          if ((rec_w.gstate == GS_DECLARING_C) && !ev_gate_w) begin
            ev_close_w       = 1'b1;
            ev_rec2_w.gstate = GS_DA_OK_C;
            ev_we_w          = 1'b1;
          end
        end
      end

      EVC_LSN: begin
        ev_daok2_w = rec_w.da_valid && !pe_conflict_r[ev_src_r];
        ev_gate_w  = ev_daok2_w
                     && (fresh_f(rec_w) || lsn_reg_f(ev_src_r));
        if ((rec_w.gstate == GS_DA_OK_C) && ev_gate_w) begin
          ev_rec2_w.gstate = GS_DECLARING_C;
          ev_open_w        = 1'b1;
          ev_we_w          = 1'b1;
        end else if ((rec_w.gstate == GS_DECLARING_C) && !ev_gate_w) begin
          ev_rec2_w.gstate = GS_DA_OK_C;
          ev_close_w       = 1'b1;
          ev_we_w          = 1'b1;
        end else if (rec_w.gstate == GS_NO_DA_C) begin
          ev_initset_w = 1'b1;    // a listener appeared: retry allocation
        end
      end

      default: begin  // EVC_INIT
        if (rec_w.gstate == GS_NO_DA_C) begin
          if (maap_avail_w) begin
            ev_to_maap_w = 1'b1;
          end else begin
            ev_initset_w = 1'b1;  // maap busy: keep the request pending
          end
        end
      end
    endcase
  end

  // ------------------------------------------------------------- FSM regs
  always_ff @(posedge clk_i) begin : walker
    if (!rst_n) begin
      state_r        <= S_INIT;
      ev_code_r      <= EVC_GRANT;
      ev_src_r       <= '0;
      txn_r          <= pp_txn_t'({PP_TXN_W_C{1'b0}});
      fetch_ix_r     <= 4'd0;
      slot_len_r     <= '0;
      pdu_leid_r     <= 64'd0;
      pdu_luid_r     <= 16'd0;
      pdu_flags_r    <= 16'd0;
      arm_deadline_r <= 32'd0;
      mreq_src_r     <= '0;
      mreq_rel_r     <= 1'b0;
      mreq_tmo_r     <= '0;
      init_ix_r      <= '0;
    end else begin
      unique case (state_r)
        S_INIT: begin
          if (init_ix_r == (SRC_W_C+1)'(N_STREAM_OUT_P - 32'd1)) begin
            state_r <= S_IDLE;
          end
          init_ix_r <= init_ix_r + (SRC_W_C+1)'(1);
        end

        S_IDLE: begin
          if (disp_kind_w == DK_TXN) begin
            txn_r       <= pp_txn_t'(txn_i);
            pdu_leid_r  <= 64'd0;
            pdu_luid_r  <= 16'd0;
            pdu_flags_r <= 16'd0;
            fetch_ix_r  <= 4'd0;
            state_r     <= S_TXN_REC;
          end else if (disp_kind_w == DK_EV) begin
            ev_code_r <= disp_code_w;
            ev_src_r  <= disp_src_w;
            state_r   <= S_EV_REC;
          end
        end

        S_TXN_REC: begin
          state_r <= needs_pdu_w ? S_TXN_PDU : S_TXN_ACT;
        end

        S_TXN_PDU: begin
          if (fetch_ix_r == 4'd0) begin
            slot_len_r <= rxs_slot_len_i;
          end
          if (fetch_ix_r != 4'd0) begin : capture
            logic [7:0] byte_w;
            byte_w = ({{(RXL_W_C - RXA_W_C){1'b0}},
                       pdu_off_f(fetch_ix_r - 4'd1)} < slot_len_r)
                     ? rxs_rd_data_i : 8'h00;
            unique case (fetch_ix_r)
              4'd1:    pdu_leid_r[63:56]  <= byte_w;
              4'd2:    pdu_leid_r[55:48]  <= byte_w;
              4'd3:    pdu_leid_r[47:40]  <= byte_w;
              4'd4:    pdu_leid_r[39:32]  <= byte_w;
              4'd5:    pdu_leid_r[31:24]  <= byte_w;
              4'd6:    pdu_leid_r[23:16]  <= byte_w;
              4'd7:    pdu_leid_r[15:8]   <= byte_w;
              4'd8:    pdu_leid_r[7:0]    <= byte_w;
              4'd9:    pdu_luid_r[15:8]   <= byte_w;
              4'd10:   pdu_luid_r[7:0]    <= byte_w;
              4'd11:   pdu_flags_r[15:8]  <= byte_w;
              default: pdu_flags_r[7:0]   <= byte_w;
            endcase
          end
          if (fetch_ix_r == 4'd12) begin
            state_r <= S_TXN_ACT;
          end else begin
            fetch_ix_r <= fetch_ix_r + 4'd1;
          end
        end

        S_TXN_ACT: begin
          state_r <= S_IDLE;
        end

        S_EV_REC: begin
          state_r <= S_EV_ACT;
        end

        S_EV_ACT: begin
          if (ev_to_draw_w) begin
            state_r <= S_EV_DRAW_REQ;
          end else if (ev_to_maap_w) begin
            mreq_src_r <= ev_src_r;
            mreq_rel_r <= ev_maap_rel_w;
            mreq_tmo_r <= '0;
            state_r    <= S_EV_MAAP;
          end else begin
            state_r <= S_IDLE;
          end
        end

        S_EV_DRAW_REQ: begin
          if (!prng_draw_busy_i) begin
            state_r <= S_EV_DRAW_WAIT;
          end
        end

        S_EV_DRAW_WAIT: begin
          if (prng_draw_valid_i) begin
            // T-SRP-LEAVEALL2 = 2 x T-MRP-LEAVEALL (F08.1)
            arm_deadline_r <= now_ms_i + {15'd0, prng_draw_ms_i, 1'b0};
            state_r        <= S_EV_ARM;
          end
        end

        S_EV_ARM: begin
          state_r <= S_IDLE;
        end

        default: begin  // S_EV_MAAP
          // The request is offered until maap accepts it OR the accept
          // window closes. Abandoning is SAFE without touching the record:
          // every path into this state has already written the source's
          // record to GS_NO_DA — EVC_OFF resets the whole record before
          // asking for RELEASE_DA, and EVC_INIT only asks for ALLOC_DA from
          // GS_NO_DA — so a dropped request leaves the source exactly where
          // a refused allocation leaves it, with no DA and no declaration.
          // The retry is stimulus-driven (probe / listener change / timer),
          // identical to the refused-ALLOC path of the maap tracker below.
          mreq_tmo_r <= mreq_tmo_r + MTMO_W_C'(1);
          if (maap_req_ready_i || maap_tmo_w) begin
            state_r <= S_IDLE;
          end
        end
      endcase
    end
  end

  // ------------------------------------------------- maap tracker + grant
  always_ff @(posedge clk_i) begin : maap_track
    if (!rst_n) begin
      maap_busy_r <= 1'b0;
      maap_src_r  <= '0;
      maap_rel_r  <= 1'b0;
      gp_valid_r  <= 1'b0;
      gp_src_r    <= '0;
      gp_da_r     <= 48'd0;
    end else begin
      if ((state_r == S_EV_MAAP) && maap_req_ready_i) begin
        maap_busy_r <= 1'b1;
        maap_src_r  <= mreq_src_r;
        maap_rel_r  <= mreq_rel_r;
      end else if (maap_busy_r && maap_rsp_valid_i) begin
        maap_busy_r <= 1'b0;
        if (!maap_rel_r && maap_rsp_ok_i) begin
          gp_valid_r <= 1'b1;      // a failed ALLOC retries on next stimulus
          gp_src_r   <= maap_src_r;
          gp_da_r    <= maap_rsp_da_i;
        end
      end
      if ((state_r == S_EV_ACT) && (ev_code_r == EVC_GRANT)) begin
        gp_valid_r <= 1'b0;
      end
    end
  end

  // ------------------------------------------------------- pending events
  logic [N_STREAM_OUT_P-1:0] set_conflict_w, set_pcp_w, set_tmr_w;
  logic [N_STREAM_OUT_P-1:0] set_lsn_w, set_init_w, set_off_w;
  logic [N_STREAM_OUT_P-1:0] clr_disp_w;
  logic [N_STREAM_OUT_P-1:0] clr_arm_w;
  logic [31:0]               arm_rel_w;

  always_comb begin : pe_sets
    set_conflict_w = '0;
    set_pcp_w      = '0;
    set_tmr_w      = '0;
    set_lsn_w      = '0;
    set_init_w     = '0;
    set_off_w      = '0;
    clr_disp_w     = '0;

    if (maap_conflict_valid_i) begin
      set_conflict_w[maap_conflict_src_i] = 1'b1;
    end
    if (srp_pcp_change_i) begin
      set_pcp_w = cfg_src_en_i;
    end
    if (exp_hit_w) begin
      set_tmr_w[exp_src_w] = 1'b1;
    end
    for (int i = 0; i < int'(N_STREAM_OUT_P); i++) begin
      if (srp_lsn_reg_state_i[32'(i)*2 +: 2] != lsn_q_r[32'(i)*2 +: 2]) begin
        set_lsn_w[i] = 1'b1;
      end
      if (cfg_src_en_i[i] && !en_q_r[i])  set_init_w[i] = 1'b1;
      if (!cfg_src_en_i[i] && en_q_r[i])  set_off_w[i]  = 1'b1;
    end
    // walker-requested allocation retries
    if ((state_r == S_TXN_ACT) && txn_initset_w) set_init_w[tsrc_w]   = 1'b1;
    if ((state_r == S_EV_ACT)  && ev_initset_w)  set_init_w[ev_src_r] = 1'b1;

    // the dispatched event bit is consumed
    if ((state_r == S_IDLE) && (disp_kind_w == DK_EV)) begin
      clr_disp_w[disp_src_w] = 1'b1;
    end
  end

  // any arm/cancel of a source's shared slot supersedes its pending expiry
  assign arm_rel_w = {{(32 - TMR_AW_C){1'b0}}, tmr_arm_slot_o}
                     - TMR_SLOT_BASE_P;
  always_comb begin : arm_clr
    clr_arm_w = '0;
    if (tmr_arm_valid_o && (arm_rel_w < N_STREAM_OUT_P)) begin
      clr_arm_w[arm_rel_w[SRC_W_C-1:0]] = 1'b1;
    end
  end

  always_ff @(posedge clk_i) begin : pe_flags
    if (!rst_n) begin
      pe_conflict_r <= '0;
      pe_pcp_r      <= '0;
      pe_tmr_r      <= '0;
      pe_lsn_r      <= '0;
      pe_init_r     <= '0;
      pe_off_r      <= '0;
      lsn_q_r       <= '0;
      en_q_r        <= '0;
    end else begin
      pe_conflict_r <= (pe_conflict_r
                        & ~((disp_code_w == EVC_CONFLICT) ? clr_disp_w : '0))
                       | set_conflict_w;
      pe_pcp_r      <= (pe_pcp_r
                        & ~((disp_code_w == EVC_PCP) ? clr_disp_w : '0))
                       | set_pcp_w;
      pe_tmr_r      <= (pe_tmr_r
                        & ~((disp_code_w == EVC_TMR) ? clr_disp_w : '0)
                        & ~clr_arm_w)
                       | set_tmr_w;
      pe_lsn_r      <= (pe_lsn_r
                        & ~((disp_code_w == EVC_LSN) ? clr_disp_w : '0))
                       | set_lsn_w;
      pe_init_r     <= (pe_init_r
                        & ~((disp_code_w == EVC_INIT) ? clr_disp_w : '0))
                       | set_init_w;
      pe_off_r      <= (pe_off_r
                        & ~((disp_code_w == EVC_OFF) ? clr_disp_w : '0))
                       | set_off_w;
      lsn_q_r       <= srp_lsn_reg_state_i;
      en_q_r        <= cfg_src_en_i;
    end
  end

  // -------------------------------------------------- record port muxing
  always_comb begin : rec_ports
    rec_re_w    = 1'b0;
    rec_raddr_w = '0;
    rec_we_w    = 1'b0;
    rec_waddr_w = '0;
    rec_wdata_w = '0;

    if (state_r == S_INIT) begin
      rec_we_w    = 1'b1;
      rec_waddr_w = init_ix_r[SRC_W_C-1:0];
      rec_wdata_w = REC_W_C'({GS_NO_DA_C, 1'b0, 1'b0, 32'd0, 48'd0});
    end else if (state_r == S_IDLE) begin
      if (disp_kind_w == DK_TXN) begin
        rec_re_w    = 1'b1;
        rec_raddr_w = txn_in_w.operands.unique_id[SRC_W_C-1:0];
      end else if (disp_kind_w == DK_EV) begin
        rec_re_w    = 1'b1;
        rec_raddr_w = disp_src_w;
      end
    end else if (state_r == S_TXN_ACT) begin
      if (ping_w) begin
        rec_we_w    = 1'b1;
        rec_waddr_w = tsrc_w;
        rec_wdata_w = REC_W_C'(txn_rec2_w);
      end
    end else if (state_r == S_EV_ACT) begin
      if (ev_we_w) begin
        rec_we_w    = 1'b1;
        rec_waddr_w = ev_src_r;
        rec_wdata_w = REC_W_C'(ev_rec2_w);
      end
    end
  end

  // ----------------------------------------------------- response compose
  // Both F05.11 tables live here — and ONLY here. The deliberate
  // difference: PROBE echoes FC+SW and forces RF = 0; GET_TX_STATE zeroes
  // the listener fields and reads RF live from the srp face.
  logic rf_live_w;
  assign rf_live_w = (lsn_state_f(tsrc_w) == LSN_ASKING_FAILED_C);

  always_comb begin : respond
    resp_valid_o          = 1'b0;
    resp_msg_type_o       = 4'd0;
    resp_status_o         = ST_SUCCESS_C;
    resp_stream_id_o      = 64'd0;
    resp_controller_eid_o = 64'd0;
    resp_talker_eid_o     = 64'd0;
    resp_listener_eid_o   = 64'd0;
    resp_talker_uid_o     = 16'd0;
    resp_listener_uid_o   = 16'd0;
    resp_dest_mac_o       = 48'd0;
    resp_conn_count_o     = 16'd0;
    resp_seq_id_o         = 16'd0;
    resp_flags_o          = 16'd0;
    resp_vlan_id_o        = 16'd0;
    resp_if_index_o       = 2'd0;

    if ((state_r == S_TXN_ACT) && talker_cmd_w) begin
      resp_msg_type_o       = txn_r.msg_type | 4'd1;
      resp_controller_eid_o = txn_r.controller_eid;
      resp_talker_eid_o     = own_entity_id_i;
      resp_talker_uid_o     = uid16_w;
      resp_seq_id_o         = txn_r.sequence_id;
      resp_if_index_o       = txn_r.interface_index;

      unique case (txn_r.msg_type)
        MT_PROBE_TX_C: begin
          if (!uid_valid_w) begin
            resp_valid_o        = 1'b1;
            resp_status_o       = ST_TALKER_UNKNOWN_C;
            resp_listener_eid_o = pdu_leid_r;
            resp_listener_uid_o = pdu_luid_r;
            resp_flags_o        = pdu_flags_r & FLG_ECHO_MASK_C;
          end else if (iface_ok_w) begin
            resp_valid_o        = 1'b1;
            resp_listener_eid_o = pdu_leid_r;
            resp_listener_uid_o = pdu_luid_r;
            resp_flags_o        = pdu_flags_r & FLG_ECHO_MASK_C; // RF = 0
            if (da_ok_w) begin
              resp_status_o    = ST_SUCCESS_C;
              resp_stream_id_o = src_sid_f(tsrc_w);
              resp_dest_mac_o  = rec_w.da;
              resp_vlan_id_o   = {4'd0, srp_class_vid_i};
            end else begin
              resp_status_o = ST_DEST_MAC_FAIL_C;
            end
          end
          // ingress interface mismatch: silently ignored (see banner)
        end

        MT_DISC_TX_C: begin
          resp_valid_o        = 1'b1;   // always SUCCESS, changes nothing
          resp_status_o       = ST_SUCCESS_C;
          resp_listener_eid_o = pdu_leid_r;
          resp_listener_uid_o = pdu_luid_r;
        end

        MT_GTX_ST_C: begin
          resp_valid_o = 1'b1;
          if (uid_valid_w) begin
            resp_status_o    = ST_SUCCESS_C;
            resp_flags_o     = rf_live_w ? FLG_REG_FAILED_C : 16'd0;
            resp_stream_id_o = src_sid_f(tsrc_w);
            resp_dest_mac_o  = da_ok_w ? rec_w.da : 48'd0;
            resp_vlan_id_o   = {4'd0, srp_class_vid_i};
          end else begin
            resp_status_o = ST_TALKER_UNKNOWN_C;
          end
        end

        default: begin  // MT_GTX_CN_C — Table 5.48
          resp_valid_o        = 1'b1;
          resp_status_o       = ST_NOT_SUPPORTED_C;
          resp_listener_eid_o = pdu_leid_r;
          resp_listener_uid_o = pdu_luid_r;
        end
      endcase
    end
  end

  // -------------------------------------------------------- gate strobes
  always_comb begin : gate_face
    gate_open_o      = 1'b0;
    gate_close_o     = 1'b0;
    gate_src_o       = '0;
    gate_stream_id_o = 64'd0;
    gate_da_o        = 48'd0;
    gate_vlan_o      = 12'd0;

    if (state_r == S_TXN_ACT) begin
      gate_open_o      = txn_open_w;
      gate_src_o       = tsrc_w;
      gate_stream_id_o = src_sid_f(tsrc_w);
      gate_da_o        = rec_w.da;
      gate_vlan_o      = srp_class_vid_i;
    end else if (state_r == S_EV_ACT) begin
      gate_open_o      = ev_open_w;
      gate_close_o     = ev_close_w;
      gate_src_o       = ev_src_r;
      gate_stream_id_o = src_sid_f(ev_src_r);
      gate_da_o        = ev_rec2_w.da;
      gate_vlan_o      = srp_class_vid_i;
    end
  end

  // ----------------------------------------------------------- timer face
  always_comb begin : timer_face
    tmr_arm_valid_o       = 1'b0;
    tmr_arm_cancel_o      = 1'b0;
    tmr_arm_slot_o        = '0;
    tmr_arm_owner_o       = '0;
    tmr_arm_deadline_ms_o = 32'd0;

    if ((state_r == S_TXN_ACT) && ping_w
        && (rec_w.gstate != GS_BACKOFF_C)) begin
      tmr_arm_valid_o       = 1'b1;
      tmr_arm_slot_o        = TMR_AW_C'(TMR_SLOT_BASE_P + 32'(tsrc_w));
      tmr_arm_owner_o       = PP_TIMER_OWNER_W_C'(TMR_OWNER_BASE_P
                                                  + 32'(tsrc_w));
      tmr_arm_deadline_ms_o = now_ms_i + 32'(DAFRESH_MS_P);
    end else if (state_r == S_EV_ACT) begin
      if (ev_arm_fresh_w) begin
        tmr_arm_valid_o       = 1'b1;
        tmr_arm_slot_o        = TMR_AW_C'(TMR_SLOT_BASE_P + 32'(ev_src_r));
        tmr_arm_owner_o       = PP_TIMER_OWNER_W_C'(TMR_OWNER_BASE_P
                                                    + 32'(ev_src_r));
        tmr_arm_deadline_ms_o = rec_w.ping_ts + 32'(DAFRESH_MS_P);
      end else if (ev_cancel_w) begin
        tmr_arm_valid_o  = 1'b1;
        tmr_arm_cancel_o = 1'b1;
        tmr_arm_slot_o   = TMR_AW_C'(TMR_SLOT_BASE_P + 32'(ev_src_r));
        tmr_arm_owner_o  = PP_TIMER_OWNER_W_C'(TMR_OWNER_BASE_P
                                               + 32'(ev_src_r));
      end
    end else if (state_r == S_EV_ARM) begin
      tmr_arm_valid_o       = 1'b1;
      tmr_arm_slot_o        = TMR_AW_C'(TMR_SLOT_BASE_P + 32'(ev_src_r));
      tmr_arm_owner_o       = PP_TIMER_OWNER_W_C'(TMR_OWNER_BASE_P
                                                  + 32'(ev_src_r));
      tmr_arm_deadline_ms_o = arm_deadline_r;
    end
  end

  // ------------------------------------------------------- rx-slot + misc
  assign rxs_rd_slot_o   = txn_r.rx_slot[RXS_W_C-1:0];
  assign rxs_rd_addr_o   = pdu_off_f(fetch_ix_r);
  assign rxs_rd_en_o     = (state_r == S_TXN_PDU) && (fetch_ix_r != 4'd12);
  assign rxs_free_o      = (state_r == S_TXN_ACT) && slot_ok_w;
  assign rxs_free_slot_o = txn_r.rx_slot[RXS_W_C-1:0];

  assign txn_ready_o = (state_r == S_TXN_ACT);

  assign maap_req_valid_o   = (state_r == S_EV_MAAP);
  assign maap_req_release_o = mreq_rel_r;
  assign maap_req_src_o     = mreq_src_r;

  assign prng_draw_req_o  = (state_r == S_EV_DRAW_REQ) && !prng_draw_busy_i;
  assign prng_draw_kind_o = 3'd3;   // T-MRP-LEAVEALL range

endmodule : KL_acmp_talker
`default_nettype wire
