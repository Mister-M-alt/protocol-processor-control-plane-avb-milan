/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : KL_srp_domain.sv
//  Project     : IEEE 1722.1 protocol processor
//                (docs/architecture/10 §6.1 F10.2 Domain FSM, §3 Domain
//                 vector surfacing rule, 02 §4.1 srp contract class-C/D)
//
//  Description : The single Class A Domain participant (Milan §4.2.7.2.1).
//                Declares the default Domain {SRclassID 6, priority 3,
//                VID 2} at startup and on every LINK_UP; a received Class A
//                Domain declaration with differing parameters is ADOPTED —
//                the old declaration is withdrawn (Lv) and the adopted one
//                declared (New!) — with a one-cycle DOMAIN_CHANGE strobe
//                and updated class-D levels {class_a_prio, class_a_vid};
//                reverts to the defaults on LINK_DOWN ONLY (F10.2: periodic
//                and LeaveAll cycles re-join, never revert). Received
//                vectors are surfaced by the 10 §3 range rule — Class A is
//                present when SRclassID_first <= 6 < SRclassID_first + NoV
//                and its priority is prio_first + (6 - SRclassID_first) —
//                so the certified two-class bridge shape (FirstValue
//                {5, 2, VID}, NumberOfValues 2) adopts correctly; equality
//                matching on FirstValue alone is a wire defect.
//                The declared value has an 802.1Q-2018 Table 10-3
//                Applicant (issue #216): LINK_UP (Begin!, New!) and an
//                adoption (New!) put it in VN, and every message is the
//                one its state gives at a tx! opportunity (tx_tick_i; the
//                txLA! row when it is the own sLA's, leaveall_own_i): New
//                from VN and again from AN, then sJ from AA, nothing from
//                QA until periodic! (QA -> AA) or a received message or
//                LeaveAll moves it. sJ is JoinIn only while the Registrar
//                of the declared value is IN, JoinMt in MT or LV
//                (§10.7.6.3). That one-bit Registrar follows Table 10-4
//                with the Milan §4.2.7.2.2 cell: a registering event (New,
//                JoinIn, JoinMt) of the declared value sets IN; its Lv
//                clears IN at once; a LeaveAll (received on the Domain
//                lane, or the own sLA) clears IN to LV, and LV encodes as
//                MT so no leavetimer is kept; LINK_UP and LINK_DOWN clear
//                it. An adoption takes the adopted value's Registrar
//                along. A tx! is served once its message fits the queue,
//                and tx_done_o strobes when the queue has handed every
//                message to the encoder, so the round's MRPDU carries it.
//
//  Decision    : the design decision that matters — Domain transmission is
//                independent of gPTP port state STRUCTURALLY: this module
//                has no gPTP input to get it wrong (Milan §4.2.7.2.1). The
//                Lv of an adoption and the messages of tx! ride a two-deep
//                event queue toward the encoder so the order holds across
//                encoder backpressure; a newer received Domain simply
//                overwrites the one-deep adoption-pending latch (newest
//                wins) and is applied when the queue drains.
//---------------------------------------------------------------------------//
`default_nettype none

module KL_srp_domain
  import srp_pkg::*;
#(
    //! default SR class A priority (Milan §4.2.7.2.1 default = 3)
    parameter logic [7:0]  DEF_PRIO_P = 8'd3,
    //! default SR class A VID (Milan §4.2.7.2.1 default = 2)
    parameter logic [15:0] DEF_VID_P  = 16'd2
) (
    input  wire         clk_i,               //! core clock (P-CLK-HZ domain)
    input  wire         rst_n,               //! synchronous active-low reset

    input  wire         link_up_i,           //! link status level (2FF-synced upstream)
    input  wire         p2p_i,               //! operPointToPointMAC (Table 10-3 notes 4/5); Milan ties 1

    // ---- received Domain vector (decoder not yet landed: plain ports) ----
    input  wire         rxdom_valid_i,       //! one-cycle strobe: Domain vector registered
    input  wire  [7:0]  rxdom_class_id_i,    //! FirstValue SRclassID
    input  wire  [7:0]  rxdom_prio_i,        //! FirstValue SRclassPriority
    input  wire  [15:0] rxdom_vid_i,         //! FirstValue SRclassVID (pinned across the vector)
    input  wire  [12:0] rxdom_nov_i,         //! NumberOfValues of the vector
    input  wire         rxdom_lv_i,          //! one-cycle strobe: Domain vector withdrawn (rLv!)
    input  wire         rxdom_any_i,         //! one-cycle strobe: any attribute event of a Class A Domain value
    input  wire  [2:0]  rxdom_ev_i,          //! that event (srp_event_e), valid with rxdom_any_i

    // ---- MSRP participant cadence (timer expiries via the event router) ---
    input  wire         periodic_tick_i,     //! T-MRP-PERIODIC: periodic! (QA -> AA)
    input  wire         leaveall_tick_i,     //! MSRP LeaveAll (own or received): Registrar to LV, rLA! unless own
    input  wire         leaveall_own_i,      //! the own sLA, with its tx_tick_i: that tx! takes the txLA! row
    input  wire         tx_tick_i,           //! MSRP tx! opportunity (join round, own sLA round)
    output logic        tx_done_o,           //! one-cycle strobe: the tx! is served and its message accepted

    // ---- declaration events toward KL_srp_encoder -------------------------
    output logic        dom_ev_valid_o,      //! event pending, held until ready
    output logic [2:0]  dom_ev_event_o,      //! attribute event (0 New, 1 JoinIn, 3 JoinMt, 5 Lv)
    output logic [31:0] dom_ev_value_o,      //! Domain FirstValue {SRclassID, prio, VID}
    input  wire         dom_ev_ready_i,      //! encoder accepts the event

    // ---- class-C event (router not yet landed: strobe + payload) ----------
    output logic        evt_domain_change_o, //! one-cycle DOMAIN_CHANGE strobe

    // ---- class-D levels (F02.10) ------------------------------------------
    output logic [2:0]  class_a_prio_o,      //! operating Class A priority
    output logic [11:0] class_a_vid_o,       //! operating Class A default VID
    output logic        adopted_o,           //! 0 = DEFAULTS, 1 = ADOPTED (F10.2 state)

    // ---- observability ----------------------------------------------------
    output logic [3:0]  dbg_app_state_o      //! Table 10-3 Applicant state of the declared value
);

  localparam logic [7:0] CLASS_A_ID_C = 8'd6;
  localparam logic [2:0] EV_LV_C      = 3'd5;

  // ------------------------------------------------------- operating state
  logic [7:0]  decl_prio_r;
  logic [15:0] decl_vid_r;
  logic        adopted_r;
  logic        declared_r;    // a declaration is on the wire (link is up)
  logic        link_q_r;

  // two-deep ordered event queue toward the encoder
  logic        q0_valid_r, q1_valid_r;
  logic [2:0]  q0_ev_r,    q1_ev_r;
  logic [31:0] q0_val_r,   q1_val_r;

  // pending latches serviced when the queue is empty
  logic        rx_pend_v_r;
  logic [7:0]  rx_pend_prio_r;
  logic [15:0] rx_pend_vid_r;

  // the declared value's Applicant (Table 10-3), the tx! it owes (served
  // when its message fits the queue) and the tx! waiting for the queue to
  // hand its message to the encoder
  logic [3:0]  app_r;
  logic        txp_r;
  logic        txp_la_r;
  logic        txw_r;

  // Registrar of the declared value, and of the pending adoption's value
  // (1 = IN; 0 = MT or LV, which sJ encodes alike)
  logic        reg_in_r;
  logic        pend_in_r;

  // ------------------------------------------------------ receive surfacing
  logic        covers6_w;
  logic [7:0]  surf_prio_w;
  logic        link_rise_w, link_fall_w, q_empty_w;
  logic        rx_is_decl_w, rx_is_pend_w;
  logic        reg_in_nx_w, pend_in_nx_w;

  // Class A present iff SRclassID_first <= 6 < SRclassID_first + NoV (10 §3)
  assign covers6_w   = (rxdom_class_id_i <= CLASS_A_ID_C)
                    && (({6'd0, rxdom_class_id_i} + {1'b0, rxdom_nov_i})
                        > 14'd6);
  assign surf_prio_w = rxdom_prio_i + (CLASS_A_ID_C - rxdom_class_id_i);

  assign link_rise_w = link_up_i && !link_q_r;
  assign link_fall_w = !link_up_i && link_q_r;
  assign q_empty_w   = !q0_valid_r && !q1_valid_r;

  // the received value is the declared one / the pending adoption's one
  assign rx_is_decl_w = declared_r && covers6_w
                     && ({surf_prio_w, rxdom_vid_i} == {decl_prio_r, decl_vid_r});
  assign rx_is_pend_w = declared_r && covers6_w && rx_pend_v_r
                     && ({surf_prio_w, rxdom_vid_i} == {rx_pend_prio_r, rx_pend_vid_r});

  // Table 10-4 with Milan §4.2.7.2.2. A received event of the clock wins
  // over the LeaveAll of that clock, as on the stream registrars (10 §6.5).
  always_comb begin : registrar_next
    reg_in_nx_w  = reg_in_r;
    pend_in_nx_w = pend_in_r;
    if (leaveall_tick_i && declared_r) begin
      reg_in_nx_w  = 1'b0;                         // rLA! / txLA!: IN -> LV
      pend_in_nx_w = 1'b0;
    end
    if (rxdom_valid_i && rx_is_decl_w) reg_in_nx_w  = 1'b1;  // rNew / rJoinIn / rJoinMt
    if (rxdom_valid_i && rx_is_pend_w) pend_in_nx_w = 1'b1;
    if (rxdom_lv_i && rx_is_decl_w)    reg_in_nx_w  = 1'b0;  // rLv!: IN -> MT at once
    if (rxdom_lv_i && rx_is_pend_w)    pend_in_nx_w = 1'b0;
  end

  // Table 10-3 for the declared value, the events of one clock in turn: a
  // received message of the value, a received LeaveAll (rLA!; the own sLA
  // is the txLA! row instead), periodic!, then the tx! served this clock,
  // whose sJ, s and note 8 read the Registrar after the clock's events
  // (an own LeaveAll's message reads it aged to LV, as Table 10-4 txLA!).
  logic        tx_serve_w;
  logic        tx_la_w;
  logic [3:0]  app_rx_w, app_la_w, app_per_w, app_tx_w, app_nx_w;
  logic [3:0]  tx_msg_w;

  assign tx_serve_w = (txp_r || tx_tick_i) && !q1_valid_r
                   && !link_fall_w && !link_rise_w;

  always_comb begin : applicant_next
    app_rx_w  = (rxdom_any_i && rx_is_decl_w)
              ? srp_app_rx_next(app_r, rxdom_ev_i, p2p_i) : app_r;
    app_la_w  = (leaveall_tick_i && !leaveall_own_i && declared_r)
              ? srp_app_next(app_rx_w, SRP_AE_RLA_C, reg_in_r, p2p_i) : app_rx_w;
    app_per_w = (periodic_tick_i && declared_r)
              ? srp_app_next(app_la_w, SRP_AE_PER_C, reg_in_r, p2p_i) : app_la_w;
    tx_la_w   = txp_la_r || leaveall_own_i;
    tx_msg_w  = srp_app_msg(app_per_w, tx_la_w, reg_in_nx_w);
    app_tx_w  = tx_la_w ? srp_app_next(app_per_w, SRP_AE_TXLA_C, reg_in_nx_w, p2p_i)
                        : srp_app_next(app_per_w, SRP_AE_TX_C, reg_in_nx_w, p2p_i);
    app_nx_w  = (tx_serve_w && declared_r) ? app_tx_w : app_per_w;
  end

  always_ff @(posedge clk_i) begin : domain_sm
    if (!rst_n) begin
      decl_prio_r         <= DEF_PRIO_P;
      decl_vid_r          <= DEF_VID_P;
      adopted_r           <= 1'b0;
      declared_r          <= 1'b0;
      link_q_r            <= 1'b0;
      q0_valid_r          <= 1'b0;
      q1_valid_r          <= 1'b0;
      q0_ev_r             <= 3'd0;
      q1_ev_r             <= 3'd0;
      q0_val_r            <= 32'd0;
      q1_val_r            <= 32'd0;
      rx_pend_v_r         <= 1'b0;
      rx_pend_prio_r      <= 8'd0;
      rx_pend_vid_r       <= 16'd0;
      app_r               <= SRP_AP_VO_C;   // Begin!
      txp_r               <= 1'b0;
      txp_la_r            <= 1'b0;
      txw_r               <= 1'b0;
      tx_done_o           <= 1'b0;
      reg_in_r            <= 1'b0;
      pend_in_r           <= 1'b0;
      evt_domain_change_o <= 1'b0;
    end else begin
      evt_domain_change_o <= 1'b0;
      tx_done_o           <= 1'b0;
      link_q_r            <= link_up_i;
      reg_in_r            <= reg_in_nx_w;
      pend_in_r           <= pend_in_nx_w;
      app_r               <= app_nx_w;

      // ---- queue pop toward the encoder ---------------------------------
      if (q0_valid_r && dom_ev_ready_i) begin
        q0_valid_r <= q1_valid_r;
        q0_ev_r    <= q1_ev_r;
        q0_val_r   <= q1_val_r;
        q1_valid_r <= 1'b0;
      end

      // ---- tx!: owed until its message fits the queue, then done once the
      // queue has handed every message to the encoder ---------------------
      if (tx_tick_i && !tx_serve_w) begin
        txp_r    <= 1'b1;
        txp_la_r <= txp_la_r || leaveall_own_i;
      end
      if (tx_serve_w) begin
        txp_r    <= 1'b0;
        txp_la_r <= 1'b0;
        txw_r    <= 1'b1;
        if (declared_r && tx_msg_w[3]) begin
          // the message goes behind whatever the queue holds (an
          // adoption's Lv): into q0 when q0 is free or pops this clock (q1
          // is free whenever a tx! is served)
          if (!q0_valid_r || dom_ev_ready_i) begin
            q0_valid_r <= 1'b1;
            q0_ev_r    <= tx_msg_w[2:0];
            q0_val_r   <= {CLASS_A_ID_C, decl_prio_r, decl_vid_r};
          end else begin
            q1_valid_r <= 1'b1;
            q1_ev_r    <= tx_msg_w[2:0];
            q1_val_r   <= {CLASS_A_ID_C, decl_prio_r, decl_vid_r};
          end
        end
      end
      if (txw_r && !tx_serve_w && q_empty_w) begin
        txw_r     <= 1'b0;
        tx_done_o <= 1'b1;
      end

      // ---- adoption request capture -------------------------------------
      if (rxdom_valid_i && declared_r && covers6_w
          && ({surf_prio_w, rxdom_vid_i} != {decl_prio_r, decl_vid_r})) begin
        rx_pend_prio_r <= surf_prio_w;    // newest received Domain wins
        rx_pend_vid_r  <= rxdom_vid_i;
        rx_pend_v_r    <= 1'b1;
        pend_in_r      <= 1'b1;           // registered by this very event
      end

      // ---- link transitions own the state (F10.2 revert rule) -----------
      if (link_fall_w) begin
        // revert to defaults on LINK_DOWN only; nothing goes on the wire
        if (adopted_r) evt_domain_change_o <= 1'b1;
        decl_prio_r   <= DEF_PRIO_P;
        decl_vid_r    <= DEF_VID_P;
        adopted_r     <= 1'b0;
        declared_r    <= 1'b0;
        q0_valid_r    <= 1'b0;
        q1_valid_r    <= 1'b0;
        rx_pend_v_r   <= 1'b0;
        app_r         <= SRP_AP_VO_C;      // the declaration is gone with the link
        reg_in_r      <= 1'b0;             // Flush!: every Registrar to MT
        pend_in_r     <= 1'b0;
      end else if (link_rise_w) begin
        // declare the defaults: Begin! then New! (VN) — startup lands here too
        app_r      <= SRP_AP_VN_C;
        declared_r <= 1'b1;
        reg_in_r   <= 1'b0;                // Begin!: nothing registered yet
        pend_in_r  <= 1'b0;
      end else if (rx_pend_v_r && q_empty_w && !tx_serve_w) begin
        // adopt: withdraw the old declaration, declare the adopted one
        q0_valid_r          <= 1'b1;
        q0_ev_r             <= EV_LV_C;
        q0_val_r            <= {CLASS_A_ID_C, decl_prio_r, decl_vid_r};
        app_r               <= SRP_AP_VN_C;   // New! on the adopted value
        decl_prio_r         <= rx_pend_prio_r;
        decl_vid_r          <= rx_pend_vid_r;
        reg_in_r            <= pend_in_nx_w;  // the adopted value's Registrar
        adopted_r           <= 1'b1;
        rx_pend_v_r         <= 1'b0;
        evt_domain_change_o <= 1'b1;
      end
    end
  end

  // -------------------------------------------------------------- outputs
  assign dom_ev_valid_o  = q0_valid_r;
  assign dom_ev_event_o  = q0_ev_r;
  assign dom_ev_value_o  = q0_val_r;
  assign class_a_prio_o  = decl_prio_r[2:0];
  assign class_a_vid_o   = decl_vid_r[11:0];
  assign adopted_o       = adopted_r;
  assign dbg_app_state_o = app_r;

endmodule : KL_srp_domain
`default_nettype wire
