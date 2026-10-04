/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : srp_store_wrap.sv
//  Project     : IEEE 1722.1 protocol processor — srp_top suite
//
//  Description : Harness for the timer-arm FIFO arms (issue #230): the REAL
//                KL_srp_top at any shape (N_SOURCES_P sources, N_SINKS_P
//                sinks) with the REAL KL_pp_tx_slots, KL_pp_timer_service
//                and KL_pp_prng around it, time-compressed as in
//                srp_top_wrap (1 ms = 40 clk, 32 timer slots: cadence 0..4,
//                talker 8.., listener 8 + N_SOURCES_P..). Every face the C++
//                side reads has a width that does not depend on the shape.
//                Read-only probes give the scoreboard both FSMs' timer-arm
//                faces (what each FIFO is offered), the merged arm face
//                (what leaves the FIFOs) and the FIFO occupancy.
//
//  Decision    : ONE forced state, test-only: while tm_stall_i is high the
//                merged arm issue is held in TM_SEL, so neither FIFO drains.
//                The ports alone never fill a FIFO (the issue drains one word
//                every two clocks and a walk offers at most one op per source
//                or sink), and the full guard is the one FIFO path no port
//                sequence reaches; the stall makes it observable. With
//                tm_stall_i low the force is released and the engine runs
//                unforced.
//---------------------------------------------------------------------------//
`default_nettype none

module srp_store_wrap #(
    parameter int unsigned N_SOURCES_P = 2,
    parameter int unsigned N_SINKS_P   = 2
) (
    input  wire         clk_i,
    input  wire         rst_n,

    // static identity / config
    input  wire  [47:0] own_mac_i,
    input  wire  [63:0] entity_id_i,
    input  wire         link_up_i,

    // MRP byte stream in (header-stripped: ProtocolVersion first)
    input  wire         mrp_valid_i,
    input  wire  [7:0]  mrp_data_i,
    input  wire         mrp_last_i,
    input  wire         mrp_msrp_i,
    output logic        mrp_ready_o,

    // class-B service port
    input  wire         req_valid_i,
    output logic        req_ready_o,
    input  wire  [2:0]  req_op_i,
    input  wire  [7:0]  req_index_i,
    input  wire  [63:0] req_stream_id_i,
    input  wire  [47:0] req_da_i,
    input  wire  [11:0] req_vid_i,
    input  wire  [15:0] req_max_frame_i,
    input  wire  [15:0] req_max_interval_i,
    input  wire  [1:0]  req_lstn_state_i,
    output logic        rsp_valid_o,
    output logic [1:0]  rsp_status_o,

    // TX capture (the C++ side is the 03 §8 arbiter)
    output logic        txreq_valid_o,
    output logic [2:0]  txreq_slot_o,
    input  wire         txreq_ready_i,
    input  wire         ser_req_i,
    input  wire  [2:0]  ser_slot_i,
    output logic        ser_valid_o,
    output logic [7:0]  ser_data_o,
    output logic        ser_last_o,
    input  wire         ser_ready_i,

    // timebase view
    output logic [31:0] now_ms_o,

    // test-only: hold the merged arm issue (see Decision)
    input  wire         tm_stall_i,

    // read-only probes: arm words are {cancel, slot[4:0], owner, deadline}
    output wire         dbg_tk_arm_valid_o,
    output wire  [45:0] dbg_tk_arm_word_o,
    output wire         dbg_ls_arm_valid_o,
    output wire  [45:0] dbg_ls_arm_word_o,
    output wire         dbg_arm_valid_o,
    output wire  [45:0] dbg_arm_word_o,
    output wire         dbg_la_action_o,
    output wire         dbg_tm_select_o,
    output wire         dbg_tm_rr_o,
    output wire  [5:0]  dbg_tf_cnt_tk_o,
    output wire  [5:0]  dbg_tf_cnt_ls_o,
    output wire  [31:0] dbg_t_reg_o,
    output wire  [31:0] dbg_l_reg_o
);

  // time compression: 1 ms = DIV_US x DIV_MS = 40 clk cycles; the 32-slot
  // sweep (34 cycles) fits inside it (KL_pp_timer_service constraint)
  localparam int unsigned TB_SLOTS_C   = 32;
  localparam int unsigned TB_DIV_US_C  = 1;
  localparam int unsigned TB_DIV_MS_C  = 40;
  localparam int unsigned TB_SLOT_AW_C = $clog2(TB_SLOTS_C);   // 5
  localparam int unsigned TB_TK_BASE_C = 8;
  localparam int unsigned TB_LS_BASE_C = TB_TK_BASE_C + N_SOURCES_P;
  // KL_srp_top's tm_st_e encodes TM_SEL as 1'b0. The wrap names the
  // encoding: Verilator 5.050 faults on a hierarchical enum-item reference.
  localparam logic TB_TM_SEL_C = 1'b0;

  // ---- timer service faces ------------------------------------------------
  logic                    arm_valid_w;
  logic                    arm_cancel_w;
  logic [TB_SLOT_AW_C-1:0] arm_slot_w;
  logic [7:0]              arm_owner_w;
  logic [31:0]             arm_deadline_w;
  logic                    exp_valid_w;
  logic [TB_SLOT_AW_C-1:0] exp_slot_w;
  logic [7:0]              exp_owner_w;
  logic                    tick_ms_w;

  // ---- prng faces -----------------------------------------------------------
  logic        draw_req_w;
  logic [2:0]  draw_kind_w;
  logic        draw_busy_w;
  logic        draw_valid_w;
  logic [15:0] draw_ms_w;
  logic [63:0] dbg_lfsr_w;
  logic        dbg_seeded_w;

  // ---- tx slot pool faces ---------------------------------------------------
  logic        alloc_req_w;
  logic        oversize_w;
  logic [2:0]  alloc_slot_w;
  logic        alloc_gnt_w;
  logic [2:0]  wr_slot_w;
  logic [10:0] wr_addr_w;
  logic        wr_valid_w;
  logic [7:0]  wr_data_w;
  logic        wr_commit_w;
  logic [10:0] wr_len_w;
  logic [4:0]  slots_ready_w;
  logic [2:0]  slots_free_w;

  logic [N_SOURCES_P-1:0][1:0] t_reg_w;
  logic [N_SINKS_P-1:0][1:0]   l_reg_w;

  assign dbg_tk_arm_valid_o = u_dut.tk_arm_v_w;
  assign dbg_tk_arm_word_o  = {u_dut.tk_arm_cancel_w, u_dut.tk_arm_slot_w,
                               u_dut.tk_arm_owner_w, u_dut.tk_arm_dl_w};
  assign dbg_ls_arm_valid_o = u_dut.ls_arm_v_w;
  assign dbg_ls_arm_word_o  = {u_dut.ls_arm_cancel_w, u_dut.ls_arm_slot_w,
                               u_dut.ls_arm_owner_w, u_dut.ls_arm_dl_w};
  assign dbg_arm_valid_o    = arm_valid_w;
  assign dbg_arm_word_o     = {arm_cancel_w, arm_slot_w, arm_owner_w, arm_deadline_w};
  assign dbg_la_action_o    = u_dut.p_la_msrp_w;
  assign dbg_tm_select_o    = (u_dut.tm_st_r == TB_TM_SEL_C);
  assign dbg_tm_rr_o        = u_dut.tm_rr_r;
  assign dbg_tf_cnt_tk_o    = u_dut.tf_cnt_r[0];
  assign dbg_tf_cnt_ls_o    = u_dut.tf_cnt_r[1];
  assign t_reg_w = u_dut.u_talker.dbg_reg_state_o;
  assign l_reg_w = u_dut.u_listener.dbg_reg_state_o;
  assign dbg_t_reg_o = 32'(t_reg_w);
  assign dbg_l_reg_o = 32'(l_reg_w);

  // The one forced state (Decision): TM_SEL issues nothing while a FIFO
  // holds a word, so holding it there stops both FIFOs draining.
  always_comb begin : tm_stall
    if (tm_stall_i) force u_dut.tm_st_r = type(u_dut.tm_st_r)'(TB_TM_SEL_C);
    else release u_dut.tm_st_r;
  end

  KL_srp_top #(
      .N_SOURCES_P    (N_SOURCES_P),
      .N_SINKS_P      (N_SINKS_P),
      .SLOT_AW_P      (TB_SLOT_AW_C),
      .TK_SLOT_BASE_P (TB_TK_BASE_C),
      .LS_SLOT_BASE_P (TB_LS_BASE_C)
  ) u_dut (
      .clk_i               (clk_i),
      .rst_n               (rst_n),
      .own_mac_i           (own_mac_i),
      .link_up_i           (link_up_i),
      .p2p_i               (1'b1),
      .cfg_rank_i          (1'b1),
      .cfg_acc_lat_ns_i    (32'h000186A0),
      .port_rate_bps_i     (32'd100000000),
      .mrp_valid_i         (mrp_valid_i),
      .mrp_data_i          (mrp_data_i),
      .mrp_last_i          (mrp_last_i),
      .mrp_msrp_i          (mrp_msrp_i),
      .mrp_ready_o         (mrp_ready_o),
      .req_valid_i         (req_valid_i),
      .req_ready_o         (req_ready_o),
      .req_op_i            (req_op_i),
      .req_index_i         (req_index_i),
      .req_stream_id_i     (req_stream_id_i),
      .req_da_i            (req_da_i),
      .req_vid_i           (req_vid_i),
      .req_max_frame_i     (req_max_frame_i),
      .req_max_interval_i  (req_max_interval_i),
      .req_lstn_state_i    (req_lstn_state_i),
      .rsp_valid_o         (rsp_valid_o),
      .rsp_status_o        (rsp_status_o),
      .rsp_data_o          (),
      .alloc_req_o         (alloc_req_w),
      .oversize_o          (oversize_w),
      .alloc_slot_i        (alloc_slot_w),
      .alloc_gnt_i         (alloc_gnt_w),
      .wr_slot_o           (wr_slot_w),
      .wr_addr_o           (wr_addr_w),
      .wr_valid_o          (wr_valid_w),
      .wr_data_o           (wr_data_w),
      .wr_commit_o         (wr_commit_w),
      .wr_len_o            (wr_len_w),
      .txreq_valid_o       (txreq_valid_o),
      .txreq_slot_o        (txreq_slot_o),
      .txreq_ready_i       (txreq_ready_i),
      .now_ms_i            (now_ms_o),
      .arm_valid_o         (arm_valid_w),
      .arm_cancel_o        (arm_cancel_w),
      .arm_slot_o          (arm_slot_w),
      .arm_owner_o         (arm_owner_w),
      .arm_deadline_ms_o   (arm_deadline_w),
      .exp_valid_i         (exp_valid_w),
      .exp_slot_i          (exp_slot_w),
      .draw_req_o          (draw_req_w),
      .draw_kind_o         (draw_kind_w),
      .draw_busy_i         (draw_busy_w),
      .draw_valid_i        (draw_valid_w),
      .draw_ms_i           (draw_ms_w),
      .evt_tk_registered_o (),
      .evt_tk_unregistered_o (),
      .evt_tk_fail_chg_o   (),
      .evt_tk_latency_chg_o (),
      .lstn_reg_change_o   (),
      .evt_domain_change_o (),
      .class_a_prio_o      (),
      .class_a_vid_o       (),
      .domain_adopted_o    (),
      .tk_decl_state_o     (),
      .lstn_reg_state_o    (),
      .active_o            (),
      .src_fail_code_o     (),
      .src_fail_bridge_o   (),
      .tk_reg_state_o      (),
      .lstn_decl_state_o   (),
      .acc_latency_o       (),
      .snk_fail_code_o     (),
      .snk_fail_bridge_o   (),
      .granted_slope_bps_o (),
      .sr_admitted_o       (),
      .sum_slope_bps_o     (),
      .over_limit_o        (),
      .dbg_vid_active_o    (),
      .dbg_vlan_err_o      (),
      .dbg_adm_round_o     (),
      .dbg_pdu_done_o      (),
      .dbg_pdu_ok_o        (),
      .dbg_pdu_malformed_o ()
  );

  KL_pp_tx_slots u_tx_slots (
      .clk_i         (clk_i),
      .rst_n         (rst_n),
      .alloc_req_i   (alloc_req_w),
      .oversize_i    (oversize_w),
      .alloc_gnt_o   (alloc_gnt_w),
      .alloc_slot_o  (alloc_slot_w),
      .wr_slot_i     (wr_slot_w),
      .wr_addr_i     (wr_addr_w),
      .wr_valid_i    (wr_valid_w),
      .wr_data_i     (wr_data_w),
      .wr_commit_i   (wr_commit_w),
      .wr_len_i      (wr_len_w),
      .hold_valid_i  (1'b0),
      .hold_slot_i   ('0),
      .release_valid_i(1'b0),
      .release_slot_i('0),
      .ser_req_i     (ser_req_i),
      .ser_slot_i    (ser_slot_i),
      .ser_valid_o   (ser_valid_o),
      .ser_data_o    (ser_data_o),
      .ser_last_o    (ser_last_o),
      .ser_ready_i   (ser_ready_i),
      .slots_ready_o (slots_ready_w),
      .slots_free_o  (slots_free_w)
  );

  KL_pp_timer_service #(
      .SLOTS_P  (TB_SLOTS_C),
      .DIV_US_P (TB_DIV_US_C),
      .DIV_MS_P (TB_DIV_MS_C)
  ) u_timer (
      .clk_i             (clk_i),
      .rst_n             (rst_n),
      .tick_ms_o         (tick_ms_w),
      .now_ms_o          (now_ms_o),
      .arm_valid_i       (arm_valid_w),
      .arm_cancel_i      (arm_cancel_w),
      .arm_slot_i        (arm_slot_w),
      .arm_owner_i       (arm_owner_w),
      .arm_deadline_ms_i (arm_deadline_w),
      .exp_valid_o       (exp_valid_w),
      .exp_slot_o        (exp_slot_w),
      .exp_owner_o       (exp_owner_w)
  );

  KL_pp_prng u_prng (
      .clk_i        (clk_i),
      .rst_n        (rst_n),
      .entity_id_i  (entity_id_i),
      .link_up_i    (link_up_i),
      .draw_req_i   (draw_req_w),
      .draw_kind_i  (draw_kind_w),
      .draw_busy_o  (draw_busy_w),
      .draw_valid_o (draw_valid_w),
      .draw_ms_o    (draw_ms_w),
      .dbg_lfsr_o   (dbg_lfsr_w),
      .dbg_seeded_o (dbg_seeded_w)
  );

endmodule : srp_store_wrap
`default_nettype wire
