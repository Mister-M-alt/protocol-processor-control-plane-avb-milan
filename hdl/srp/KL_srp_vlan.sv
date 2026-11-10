/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : KL_srp_vlan.sv
//  Project     : IEEE 1722.1 protocol processor
//                (docs/architecture/10 §6.2 F10.3 per-VID refcounted MVRP
//                 membership, Milan §4.3.2 join-before-streaming,
//                 Milan Table 5.3 frozen declaring VID)
//
//  Description : The single MVRP participant: one FSM over a small
//                refcounted per-VID membership table. Users (talker and
//                listener stream FSMs) register with THEIR OWN stream VID —
//                the first user of a VID declares it (New!), later users of
//                the same VID only increment the count, and the LAST user
//                leaving withdraws it (Lv!). Because the table is keyed by
//                the VID each user brought, a Domain default-VID change
//                never moves an existing membership: a declaring talker's
//                VID stays frozen until its last user deregisters (Milan
//                Table 5.3 / §4.3.2), and across the F05.12 withdraw ->
//                re-declare flow two VIDs are briefly live — the steady
//                state is one. Each entry carries the value's 802.1Q-2018
//                Table 10-3 Applicant and a one-bit Table 10-4 Registrar
//                (IN, or MT and LV, which sJ encodes alike) that received
//                MVRP events of its VID move (issue #216). Every message is
//                the one an entry's state gives at a tx! opportunity: a
//                tx_tick_i walks the entries (the txLA! row when it is the
//                own sLA's, tx_la_i, which also ages every Registrar to
//                LV), and tx_done_o strobes once each message it made is in
//                the encoder, so that tx!'s MVRPDU carries them. New! takes
//                a VID to VN: New from VN and again from AN, then sJ from
//                AA, JoinIn only with the Registrar IN (§10.7.6.3). Lv!
//                takes it to LA, and the entry stays until its tx! sends
//                the Lv (or, after a txLA!, the s it then owes). periodic!
//                and a received LeaveAll (rLA!) move every entry by their
//                rows. LINK_DOWN (Flush!) and LINK_UP (Begin!) take every
//                Registrar to MT (Table 10-4), and LINK_UP is Begin! on
//                every Applicant too, then New! on each VID with users,
//                so the new link partner gets New, New, then the Join;
//                a VID with no user left is VO there and is discarded
//                (Table 10-3 note 11). Each live VID also records whether
//                its declaration has left in an MVRP MRPDU the TX arbiter
//                accepted since the VID was allocated (vid_sent_o): the
//                talker's streaming licence waits for it, because a
//                Talker PAAD "shall join the relevant VLAN via MVRP prior
//                to sending any Stream frames" (Milan §4.3.2, 10 §6.3).
//
//  Decision    : the design decision that matters — the membership store
//                follows the KL_pp_timer_service split: {vid, refcount}
//                lives in a 1W1R sync-read RAM walked one entry per cycle
//                (lookup is a serial scan — membership ops are protocol-
//                rate, never wire-rate), while the per-entry bits, the
//                Applicant and the Registrar live in flops so received
//                values, LeaveAll and periodic! reach every entry in their
//                cycle with no second RAM port. The one flop copy of the
//                VID is the one written at allocation (vid_val_o): the
//                talker FSM and the received-event match compare it with
//                every entry in the same cycle, which a serial RAM lookup
//                cannot serve, and the tx! walk reads it.
//---------------------------------------------------------------------------//
`default_nettype none

module KL_srp_vlan
  import srp_pkg::*;
#(
    //! membership table entries; steady state is ONE live VID and a Domain
    //! VID change adds one more (10 §6.2) — 4 leaves margin for overlap
    parameter int unsigned N_VIDS_P   = 4,
    //! refcount width: must hold P-N-STREAM-IN + P-N-STREAM-OUT users
    parameter int unsigned REFCNT_W_P = 5
) (
    input  wire         clk_i,            //! core clock (P-CLK-HZ domain)
    input  wire         rst_n,            //! synchronous active-low reset
    input  wire         link_up_i,        //! link status level (2FF-synced upstream): its edges are Flush! and Begin!
    input  wire         p2p_i,            //! operPointToPointMAC (Table 10-3 notes 4/5); Milan ties 1

    // ---- user registration face (stream FSMs not yet landed) -------------
    input  wire         user_valid_i,     //! op offered, held until ready
    input  wire         user_join_i,      //! 1 = join user_vid_i, 0 = leave it
    input  wire  [11:0] user_vid_i,       //! the USER'S stream VID (frozen-VID key)
    output logic        user_ready_o,     //! op accepted this cycle
    output logic        user_err_o,       //! strobe: join with a full table / leave of an unknown VID

    // ---- received MVRP values (KL_srp_decoder, one per cycle) ------------
    input  wire         rx_valid_i,       //! one-cycle strobe: an attribute event of one MVRP VID value
    input  wire  [15:0] rx_vid_i,         //! that value (FirstValue + k)
    input  wire  [2:0]  rx_ev_i,          //! its event (srp_event_e)

    // ---- MVRP participant cadence (timer expiries via the event router) ---
    input  wire         periodic_tick_i,  //! T-MRP-PERIODIC: periodic! on every entry
    input  wire         leaveall_tick_i,  //! received MVRP LeaveAll: rLA! on every entry
    input  wire         tx_tick_i,        //! MVRP tx! opportunity: walk every entry
    input  wire         tx_la_i,          //! with tx_tick_i: the own sLA, so the walk takes the txLA! row
    output logic        tx_done_o,        //! one-cycle strobe: the walk is done and its messages accepted

    // ---- declaration events toward KL_srp_encoder -------------------------
    output logic        vlan_ev_valid_o,  //! event pending, held until ready
    output logic [2:0]  vlan_ev_event_o,  //! attribute event (0 New, 1 JoinIn, 2 In, 3 JoinMt, 4 Mt, 5 Lv)
    output logic [15:0] vlan_ev_vid_o,    //! MVRP FirstValue (VID)
    input  wire         vlan_ev_ready_i,  //! encoder accepts the event
    input  wire         mvrp_tx_i,        //! strobe: the TX arbiter accepted an MVRP MRPDU

    // ---- declaration-on-the-wire view (talker licence, Milan §4.3.2) ------
    output logic [N_VIDS_P-1:0]        vid_sent_o, //! per entry: live, and its New/JoinIn has been transmitted
    output logic [N_VIDS_P-1:0][11:0]  vid_val_o,  //! per entry: the VID it holds (valid with vid_sent_o)

    // ---- observability ----------------------------------------------------
    output logic [N_VIDS_P-1:0]        vid_active_o,    //! per-entry membership-live bits
    output logic [N_VIDS_P-1:0][3:0]   dbg_app_state_o, //! per-entry Table 10-3 Applicant state
    output logic [N_VIDS_P-1:0]        dbg_reg_in_o     //! per-entry Registrar IN
);

  localparam int unsigned IDX_W_C  = (N_VIDS_P > 1) ? $clog2(N_VIDS_P) : 1;
  localparam int unsigned ENT_W_C  = 12 + REFCNT_W_P;   // {vid, refcount}
  localparam logic [REFCNT_W_P-1:0] CNT_ONE_C = REFCNT_W_P'(1);
  localparam logic [REFCNT_W_P-1:0] CNT_MAX_C = '1;

  // ----------------------------------------------------- membership store
  // {vid, refcount} in a 1W1R sync-read RAM; the per-entry state in flops
  logic [ENT_W_C-1:0] tbl_r [0:N_VIDS_P-1];
  logic [ENT_W_C-1:0] tbl_q_r;
  // alloc: the entry holds a VID's Applicant (declared, or a Leave owed);
  // active: the VID has users. Declaration-on-the-wire state: a declaring
  // message of the entry was handed to the encoder since the last MVRP
  // transmission (queued), and one has been transmitted since the entry was
  // allocated (sent); VID copy (banner)
  logic [N_VIDS_P-1:0]       alloc_r;
  logic [N_VIDS_P-1:0]       active_r;
  logic [N_VIDS_P-1:0]       queued_r;
  logic [N_VIDS_P-1:0]       sent_r;
  logic [N_VIDS_P-1:0][11:0] vid_q_r;
  logic [N_VIDS_P-1:0][3:0]  app_r;
  logic [N_VIDS_P-1:0]       reg_r;

  logic [IDX_W_C-1:0] rd_addr_w;
  logic               tbl_we_w;
  logic [IDX_W_C-1:0] tbl_wix_w;
  logic [ENT_W_C-1:0] tbl_wdat_w;

  always_ff @(posedge clk_i) begin : tbl_write
    if (tbl_we_w) tbl_r[tbl_wix_w] <= tbl_wdat_w;
  end

  always_ff @(posedge clk_i) begin : tbl_read
    tbl_q_r <= tbl_r[rd_addr_w];
  end

  // ------------------------------------------------------------ FSM state
  typedef enum logic [2:0] {
    V_IDLE  = 3'd0,   // start a tx! walk, or accept a user op
    V_SCAN  = 3'd1,   // serial lookup of the op's VID (+ first free slot)
    V_APPLY = 3'd2,   // refcount update / allocate (New!) / last user (Lv!)
    W_EVAL  = 3'd3,   // tx! walk: one entry's tx! (or txLA!)
    W_PUSH  = 3'd4,   // hold its message on the encoder intake
    W_DONE  = 3'd5    // strobe tx_done_o
  } vlan_st_e;

  vlan_st_e            st_r;
  logic                op_join_r;
  logic [11:0]         op_vid_r;
  logic [IDX_W_C-1:0]  scan_ix_r;      // address being issued
  logic [IDX_W_C-1:0]  chk_ix_r;       // address whose data is in tbl_q_r
  logic                chk_v_r;
  logic                found_v_r;
  logic [IDX_W_C-1:0]  found_ix_r;
  logic [REFCNT_W_P-1:0] found_cnt_r;
  logic                free_v_r;
  logic [IDX_W_C-1:0]  free_ix_r;
  logic [IDX_W_C-1:0]  widx_r;         // tx! walk index
  logic                txp_r;          // a tx! owed, the walk not started
  logic                txp_la_r;       // ...and it is the own sLA's
  logic                wla_r;          // the running walk takes the txLA! row
  logic [2:0]          ev_code_r;
  logic [15:0]         ev_vid_r;
  logic                ev_decl_r;      // the pushed message declares (New, JoinIn, JoinMt)
  logic                link_q_r;       // the link level of the previous clock

  logic [11:0]           q_vid_w;
  logic [REFCNT_W_P-1:0] q_cnt_w;
  logic                  scan_done_w;
  logic                  walk_start_w;
  logic                  link_ev_w;     // LINK_DOWN (Flush!) or LINK_UP (Begin!) this clock
  logic                  link_rise_w;   // LINK_UP

  assign {q_vid_w, q_cnt_w} = tbl_q_r;
  assign link_ev_w   = link_up_i != link_q_r;
  assign link_rise_w = link_up_i && !link_q_r;
  assign scan_done_w = chk_v_r && (chk_ix_r == IDX_W_C'(N_VIDS_P - 1));
  assign walk_start_w = (st_r == V_IDLE) && (txp_r || tx_tick_i);

  assign rd_addr_w = scan_ix_r;

  // ----------------------------------------------------- table write mux
  always_comb begin : tbl_wmux
    tbl_we_w   = 1'b0;
    tbl_wix_w  = '0;
    tbl_wdat_w = '0;
    if (st_r == V_APPLY) begin
      if (op_join_r && found_v_r && active_r[found_ix_r]) begin
        // another user of a live VID: count up, no wire action (F10.3)
        tbl_we_w   = 1'b1;
        tbl_wix_w  = found_ix_r;
        tbl_wdat_w = {op_vid_r, (found_cnt_r == CNT_MAX_C)
                                ? CNT_MAX_C
                                : found_cnt_r + CNT_ONE_C};
      end else if (op_join_r && found_v_r) begin
        // the first user of a VID whose Leave is still owed: declare again
        tbl_we_w   = 1'b1;
        tbl_wix_w  = found_ix_r;
        tbl_wdat_w = {op_vid_r, CNT_ONE_C};
      end else if (op_join_r && free_v_r) begin
        // first user of a VID: allocate and declare it
        tbl_we_w   = 1'b1;
        tbl_wix_w  = free_ix_r;
        tbl_wdat_w = {op_vid_r, CNT_ONE_C};
      end else if (!op_join_r && found_v_r && active_r[found_ix_r]
                   && (found_cnt_r != CNT_ONE_C)) begin
        tbl_we_w   = 1'b1;
        tbl_wix_w  = found_ix_r;
        tbl_wdat_w = {op_vid_r, found_cnt_r - CNT_ONE_C};
      end
    end
  end

  // ------------------------------------------------- Applicant + Registrar
  // Each entry takes the events of one clock in turn: a received value of
  // its VID, a received LeaveAll (rLA!), periodic!, then the one MAD request
  // (New!, Lv!) or the one tx!/txLA! this clock's state gives it. A received
  // event of the clock wins over the LeaveAll of that clock in the
  // Registrar, as on the MSRP registrars (10 §6.5); tx_la_i ages every
  // Registrar to LV at the walk's start (Table 10-4 txLA!), and each
  // message's sJ, s and note 8 read its Registrar after the clock's events.
  // A link change wins over the clock's received events, LeaveAll and
  // periodic!, as on the Domain and the listener registrars, and comes
  // before its MAD request or tx!: LINK_DOWN (Flush!) and LINK_UP (Begin!)
  // take every Registrar to MT (Table 10-4), and LINK_UP is Begin! on every
  // Applicant (Table 10-3: VO), then New! on each entry with users (VN), so
  // that clock's MAD request or tx! moves the restarted state (issue #216).
  // A received value hits at most one entry (each VID is in one entry), and
  // one MAD request or one tx! moves one entry a clock, so each of those two
  // rows is one transition shared by the entries, on the moved entry's state.
  logic [N_VIDS_P-1:0]       rx_hit_w;
  logic [N_VIDS_P-1:0]       reg_nx_w;
  logic [N_VIDS_P-1:0][3:0]  app_ev_w;   // after the received events
  logic [N_VIDS_P-1:0][3:0]  app_nx_w;
  logic [3:0]                wmsg_w;
  logic                      apply_new_w, apply_lv_w;
  logic [IDX_W_C-1:0]        apply_ix_w;
  logic [IDX_W_C-1:0]        rx_ix_w;    // the entry a received value hits
  logic [3:0]                rx_nx_w;
  logic                      one_v_w;    // a MAD request or a tx! moves one entry
  logic [IDX_W_C-1:0]        one_ix_w;
  logic [1:0]                one_k_w;    // its row: 0 New!, 1 Lv!, 2 tx!, 3 txLA!
  logic [3:0]                one_st_w, one_nx_w;

  assign apply_new_w = (st_r == V_APPLY) && op_join_r
                    && ((found_v_r && !active_r[found_ix_r]) || (!found_v_r && free_v_r));
  assign apply_lv_w  = (st_r == V_APPLY) && !op_join_r && found_v_r
                    && active_r[found_ix_r] && (found_cnt_r == CNT_ONE_C);
  assign apply_ix_w  = (op_join_r && !found_v_r) ? free_ix_r : found_ix_r;
  assign one_v_w     = apply_new_w || apply_lv_w || ((st_r == W_EVAL) && alloc_r[widx_r]);
  assign one_ix_w    = (st_r == W_EVAL) ? widx_r : apply_ix_w;
  assign one_k_w     = (st_r == W_EVAL) ? (wla_r ? 2'd3 : 2'd2) : apply_new_w ? 2'd0 : 2'd1;

  always_comb begin : entry_next
    rx_ix_w = '0;
    for (int unsigned i = 0; i < N_VIDS_P; i++) begin
      rx_hit_w[i] = rx_valid_i && alloc_r[i] && (rx_vid_i == {4'd0, vid_q_r[i]});
      if (rx_hit_w[i]) rx_ix_w = IDX_W_C'(i);
      reg_nx_w[i] = reg_r[i];
      if (leaveall_tick_i || (walk_start_w && (txp_la_r || tx_la_i)))
        reg_nx_w[i] = 1'b0;                               // rLA! / txLA!: IN -> LV
      if (rx_hit_w[i] && ((rx_ev_i == SRP_EV_NEW) || (rx_ev_i == SRP_EV_JOININ)
                          || (rx_ev_i == SRP_EV_JOINMT)))
        reg_nx_w[i] = 1'b1;                               // rNew / rJoinIn / rJoinMt
      if (rx_hit_w[i] && (rx_ev_i == SRP_EV_LV))
        reg_nx_w[i] = 1'b0;                               // rLv!: IN -> LV
      if (link_ev_w)
        reg_nx_w[i] = 1'b0;                               // Flush! / Begin!: MT
    end
    rx_nx_w = srp_app_rx_next(app_r[rx_ix_w], rx_ev_i, p2p_i);
    for (int unsigned i = 0; i < N_VIDS_P; i++) begin
      app_ev_w[i] = rx_hit_w[i] ? rx_nx_w : app_r[i];
      if (leaveall_tick_i && alloc_r[i])
        app_ev_w[i] = srp_app_next(app_ev_w[i], SRP_AE_RLA_C, reg_r[i], p2p_i);
      if (periodic_tick_i && alloc_r[i])
        app_ev_w[i] = srp_app_next(app_ev_w[i], SRP_AE_PER_C, reg_r[i], p2p_i);
      if (link_rise_w)                                    // Begin! (VO), then New! (VN)
        app_ev_w[i] = active_r[i] ? SRP_AP_VN_C : SRP_AP_VO_C;
    end
    // a new entry's Applicant starts in VO (Begin!)
    one_st_w = (apply_new_w && !alloc_r[one_ix_w]) ? SRP_AP_VO_C : app_ev_w[one_ix_w];
    case (one_k_w)          // a choice between rows maps smaller than one function of both
      2'd0:    one_nx_w = srp_app_next(one_st_w, SRP_AE_NEW_C, reg_nx_w[one_ix_w], p2p_i);
      2'd1:    one_nx_w = srp_app_next(one_st_w, SRP_AE_LV_C, reg_nx_w[one_ix_w], p2p_i);
      2'd2:    one_nx_w = srp_app_next(one_st_w, SRP_AE_TX_C, reg_nx_w[one_ix_w], p2p_i);
      default: one_nx_w = srp_app_next(one_st_w, SRP_AE_TXLA_C, reg_nx_w[one_ix_w], p2p_i);
    endcase
    for (int unsigned i = 0; i < N_VIDS_P; i++)
      app_nx_w[i] = (one_v_w && (one_ix_w == IDX_W_C'(i))) ? one_nx_w : app_ev_w[i];
    wmsg_w = srp_app_msg(app_ev_w[widx_r], wla_r, reg_nx_w[widx_r]);
  end

  //! an observer state with nothing owed: the entry can be discarded (Table
  //! 10-3 note 11)
  function automatic logic app_idle_f(input logic [3:0] st);
    return st inside {SRP_AP_VO_C, SRP_AP_AO_C, SRP_AP_QO_C};
  endfunction

  // -------------------------------------------------------------- main SM
  always_ff @(posedge clk_i) begin : vlan_sm
    if (!rst_n) begin
      st_r          <= V_IDLE;
      alloc_r       <= '0;
      active_r      <= '0;
      queued_r      <= '0;
      sent_r        <= '0;
      vid_q_r       <= '0;
      app_r         <= '0;               // Begin! = VO everywhere
      reg_r         <= '0;
      op_join_r     <= 1'b0;
      op_vid_r      <= 12'd0;
      scan_ix_r     <= '0;
      chk_ix_r      <= '0;
      chk_v_r       <= 1'b0;
      found_v_r     <= 1'b0;
      found_ix_r    <= '0;
      found_cnt_r   <= '0;
      free_v_r      <= 1'b0;
      free_ix_r     <= '0;
      widx_r        <= '0;
      txp_r         <= 1'b0;
      txp_la_r      <= 1'b0;
      wla_r         <= 1'b0;
      ev_code_r     <= 3'd0;
      ev_vid_r      <= 16'd0;
      ev_decl_r     <= 1'b0;
      link_q_r      <= 1'b0;
      tx_done_o     <= 1'b0;
      user_err_o    <= 1'b0;
    end else begin
      user_err_o <= 1'b0;
      tx_done_o  <= 1'b0;
      link_q_r   <= link_up_i;
      app_r      <= app_nx_w;
      reg_r      <= reg_nx_w;
      // a tx! that cannot start its walk this clock is owed
      if (tx_tick_i && !walk_start_w) begin
        txp_r    <= 1'b1;
        txp_la_r <= txp_la_r || tx_la_i;
      end
      // An accepted MVRP MRPDU carries every declaration queued before its
      // drain started (the encoder holds MVRP pushes while that drain runs);
      // per-entry writes below land after this and win.
      if (mvrp_tx_i) begin
        sent_r   <= sent_r | queued_r;
        queued_r <= '0;
      end

      unique case (st_r)
        V_IDLE: begin
          chk_v_r <= 1'b0;
          if (walk_start_w) begin
            txp_r    <= 1'b0;
            txp_la_r <= 1'b0;
            wla_r    <= txp_la_r || tx_la_i;
            widx_r   <= '0;
            st_r     <= W_EVAL;
          end else if (user_valid_i) begin
            op_join_r   <= user_join_i;
            op_vid_r    <= user_vid_i;
            scan_ix_r   <= '0;
            found_v_r   <= 1'b0;
            free_v_r    <= 1'b0;
            st_r        <= V_SCAN;
          end
        end
        V_SCAN: begin
          // issue addr scan_ix; data for chk_ix lands in tbl_q_r
          chk_ix_r <= scan_ix_r;
          chk_v_r  <= 1'b1;
          if (scan_ix_r != IDX_W_C'(N_VIDS_P - 1)) begin
            scan_ix_r <= scan_ix_r + IDX_W_C'(1);
          end
          if (chk_v_r) begin
            if (alloc_r[chk_ix_r] && (vid_q_r[chk_ix_r] == op_vid_r)) begin
              found_v_r   <= 1'b1;
              found_ix_r  <= chk_ix_r;
              found_cnt_r <= q_cnt_w;
            end
            if (!alloc_r[chk_ix_r] && !free_v_r) begin
              free_v_r  <= 1'b1;
              free_ix_r <= chk_ix_r;
            end
          end
          if (scan_done_w) st_r <= V_APPLY;
        end
        V_APPLY: begin
          st_r <= V_IDLE;
          if (op_join_r) begin
            if (found_v_r && active_r[found_ix_r]) begin
              // another user of a live VID: the count goes up (tbl_wmux), silent
            end else if (found_v_r) begin
              active_r[found_ix_r] <= 1'b1;        // New! on the VID's Applicant
              queued_r[found_ix_r] <= 1'b0;
              sent_r[found_ix_r]   <= 1'b0;
            end else if (!found_v_r && free_v_r) begin
              alloc_r[free_ix_r]  <= 1'b1;
              active_r[free_ix_r] <= 1'b1;
              queued_r[free_ix_r] <= 1'b0;         // nothing of this VID has left yet
              sent_r[free_ix_r]   <= 1'b0;
              vid_q_r[free_ix_r]  <= op_vid_r;
              reg_r[free_ix_r]    <= 1'b0;         // a new Registrar: MT (Begin!)
            end else if (!found_v_r) begin
              user_err_o <= 1'b1;                  // table full
            end
          end else begin
            if (found_v_r && active_r[found_ix_r] && (found_cnt_r == CNT_ONE_C)) begin
              active_r[found_ix_r] <= 1'b0;        // last user gone: Lv! (F10.3)
              queued_r[found_ix_r] <= 1'b0;
              sent_r[found_ix_r]   <= 1'b0;
              // nothing owed (Lv! from VP): the entry is free at once
              if (app_idle_f(app_nx_w[found_ix_r])) alloc_r[found_ix_r] <= 1'b0;
            end else if (!found_v_r || !active_r[found_ix_r]) begin
              user_err_o <= 1'b1;                  // unknown VID
            end
          end
        end
        W_EVAL: begin
          // the entry's tx!: its message, if any, then the next entry; an
          // entry with no user and nothing owed afterwards is discarded
          if (alloc_r[widx_r] && !active_r[widx_r] && app_idle_f(app_nx_w[widx_r]))
            alloc_r[widx_r] <= 1'b0;
          if (alloc_r[widx_r] && wmsg_w[3]) begin
            ev_code_r <= wmsg_w[2:0];
            ev_vid_r  <= {4'd0, vid_q_r[widx_r]};
            ev_decl_r <= active_r[widx_r]
                      && ((wmsg_w[2:0] == SRP_EV_NEW) || (wmsg_w[2:0] == SRP_EV_JOININ)
                          || (wmsg_w[2:0] == SRP_EV_JOINMT));
            st_r      <= W_PUSH;
          end else if (widx_r == IDX_W_C'(N_VIDS_P - 1)) begin
            st_r <= W_DONE;
          end else begin
            widx_r <= widx_r + IDX_W_C'(1);
          end
        end
        W_PUSH: begin
          // the held message was decided before LINK_UP: it is withdrawn
          // (vlan_ev_valid_o), and the entry takes this walk's tx! in its
          // restarted state
          if (link_rise_w) begin
            st_r <= W_EVAL;
          end else if (vlan_ev_ready_i) begin
            if (ev_decl_r) queued_r[widx_r] <= 1'b1;   // its declaration handed over
            if (widx_r == IDX_W_C'(N_VIDS_P - 1)) begin
              st_r <= W_DONE;
            end else begin
              widx_r <= widx_r + IDX_W_C'(1);
              st_r   <= W_EVAL;
            end
          end
        end
        default: begin   // W_DONE
          tx_done_o <= 1'b1;
          st_r      <= V_IDLE;
        end
      endcase
    end
  end

  // -------------------------------------------------------------- outputs
  assign user_ready_o    = (st_r == V_IDLE) && !walk_start_w;
  assign vlan_ev_valid_o = (st_r == W_PUSH) && !link_rise_w;
  assign vlan_ev_event_o = ev_code_r;
  assign vlan_ev_vid_o   = ev_vid_r;
  assign vid_active_o    = active_r;
  assign vid_sent_o      = active_r & sent_r;
  assign vid_val_o       = vid_q_r;
  assign dbg_app_state_o = app_r;
  assign dbg_reg_in_o    = reg_r;

endmodule : KL_srp_vlan
`default_nettype wire
