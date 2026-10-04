/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : srp_walk_wrap.sv
//  Project     : IEEE 1722.1 protocol processor — srp_stream_fsms suite
//
//  Description : Harness wrap for the walk-record arms (issue #230):
//                KL_srp_talker_fsm at M = N_SOURCES_P sources and
//                KL_srp_listener_fsm at N = N_SINKS_P sinks, bound as in
//                srp_stream_fsms_wrap (one decoder event bus and one cadence
//                fan out to both) but at any shape, so the walk's copy of
//                each record is elaborated in both arms: the flops at one
//                and two contexts, distributed RAM from three. Every face
//                the C++ side reads has a width that does not depend on
//                the shape: index inputs are 4 bits wide (M, N <= 16) and
//                the per-stream declaration levels are padded to 16
//                streams. The timer-arm faces are left open: these arms
//                read the encoder intake and the VLAN user faces only.
//---------------------------------------------------------------------------//
`default_nettype none

module srp_walk_wrap #(
    parameter int unsigned N_SOURCES_P = 2,
    parameter int unsigned N_SINKS_P   = 2,
    //! derived index widths — do not override
    localparam int unsigned SRC_W_C = (N_SOURCES_P > 1) ? $clog2(N_SOURCES_P) : 1,
    localparam int unsigned SNK_W_C = (N_SINKS_P > 1) ? $clog2(N_SINKS_P) : 1
) (
    input  wire         clk_i,
    input  wire         rst_n,
    input  wire         p2p_i,
    input  wire  [47:0] own_mac_i,

    // shared decoder event bus (fans to both modules)
    input  wire         evt_valid_i,
    input  wire         evt_msrp_i,
    input  wire  [7:0]  evt_attr_type_i,
    input  wire  [63:0] evt_stream_id_i,
    input  wire  [47:0] evt_da_i,
    input  wire  [15:0] evt_vid_i,
    input  wire  [2:0]  evt_mrp_event_i,
    input  wire  [1:0]  evt_fourpacked_i,
    input  wire  [31:0] evt_acc_latency_i,

    // shared cadence + timebase
    input  wire         join_tick_i,
    input  wire         leaveall_own_i,
    input  wire  [31:0] now_ms_i,

    // talker DA-gate
    input  wire         gate_valid_i,
    output logic        gate_ready_o,
    input  wire         gate_open_i,
    input  wire  [3:0]  gate_src_i,
    input  wire  [63:0] gate_stream_id_i,
    input  wire  [47:0] gate_da_i,
    input  wire  [11:0] gate_vid_i,
    input  wire  [15:0] gate_max_frame_i,
    input  wire  [15:0] gate_max_interval_i,
    input  wire  [2:0]  gate_prio_i,
    input  wire         gate_rank_i,
    input  wire  [31:0] gate_acc_lat_i,

    // listener settle/teardown
    input  wire         ctl_valid_i,
    output logic        ctl_ready_o,
    input  wire         ctl_settle_i,
    input  wire  [3:0]  ctl_sink_i,
    input  wire  [63:0] ctl_stream_id_i,
    input  wire  [47:0] ctl_da_i,
    input  wire  [11:0] ctl_vid_i,

    // talker faces
    output logic         t_txop_done_o,
    output logic         t_ev_valid_o,
    output logic [7:0]   t_ev_attr_type_o,
    output logic [2:0]   t_ev_event_o,
    output logic [271:0] t_ev_value_o,
    output logic         t_user_valid_o,
    output logic         t_user_join_o,
    output logic [11:0]  t_user_vid_o,
    output logic [15:0][1:0] t_tk_decl_state_o,

    // listener faces
    output logic         l_txop_done_o,
    output logic         l_ev_valid_o,
    output logic [7:0]   l_ev_attr_type_o,
    output logic [2:0]   l_ev_event_o,
    output logic [1:0]   l_ev_fourpack_o,
    output logic [271:0] l_ev_value_o,
    output logic [15:0][1:0] l_lstn_decl_state_o
);

  logic [N_SOURCES_P-1:0][1:0] t_decl_w;
  logic [N_SINKS_P-1:0][1:0]   l_decl_w;

  always_comb begin : pad_levels
    t_tk_decl_state_o   = '0;
    l_lstn_decl_state_o = '0;
    for (int unsigned s = 0; s < N_SOURCES_P; s++) t_tk_decl_state_o[s] = t_decl_w[s];
    for (int unsigned k = 0; k < N_SINKS_P; k++) l_lstn_decl_state_o[k] = l_decl_w[k];
  end

  KL_srp_talker_fsm #(
      .N_SOURCES_P (N_SOURCES_P),
      .LEAVE_MS_P  (5000),
      .SLOT_BASE_P (16),
      .SLOT_AW_P   (7),
      .OWNER_BASE_P(8'h40),
      .N_VIDS_P    (4)
  ) u_talker (
      .clk_i              (clk_i),
      .rst_n              (rst_n),
      .own_mac_i          (own_mac_i),
      .p2p_i              (p2p_i),
      .gate_valid_i       (gate_valid_i),
      .gate_ready_o       (gate_ready_o),
      .gate_open_i        (gate_open_i),
      .gate_src_i         (gate_src_i[SRC_W_C-1:0]),
      .gate_stream_id_i   (gate_stream_id_i),
      .gate_da_i          (gate_da_i),
      .gate_vid_i         (gate_vid_i),
      .gate_max_frame_i   (gate_max_frame_i),
      .gate_max_interval_i(gate_max_interval_i),
      .gate_prio_i        (gate_prio_i),
      .gate_rank_i        (gate_rank_i),
      .gate_acc_lat_i     (gate_acc_lat_i),
      .sr_admitted_i      ({N_SOURCES_P{1'b1}}),
      .evt_valid_i        (evt_valid_i),
      .evt_msrp_i         (evt_msrp_i),
      .evt_attr_type_i    (evt_attr_type_i),
      .evt_stream_id_i    (evt_stream_id_i),
      .evt_da_i           (evt_da_i),
      .evt_vid_i          (evt_vid_i),
      .evt_mrp_event_i    (evt_mrp_event_i),
      .evt_fourpacked_i   (evt_fourpacked_i),
      .join_tick_i        (join_tick_i),
      .periodic_tick_i    (1'b0),
      .leaveall_rx_i      (4'd0),
      .leaveall_own_i     (leaveall_own_i),
      .txop_done_o        (t_txop_done_o),
      .ev_valid_o         (t_ev_valid_o),
      .ev_ready_i         (1'b1),
      .ev_app_o           (),
      .ev_attr_type_o     (t_ev_attr_type_o),
      .ev_event_o         (t_ev_event_o),
      .ev_fourpack_o      (),
      .ev_value_o         (t_ev_value_o),
      .user_valid_o       (t_user_valid_o),
      .user_join_o        (t_user_join_o),
      .user_vid_o         (t_user_vid_o),
      .user_ready_i       (1'b1),
      .vid_sent_i         (4'd0),
      .vid_val_i          ('0),
      .now_ms_i           (now_ms_i),
      .arm_valid_o        (),
      .arm_cancel_o       (),
      .arm_slot_o         (),
      .arm_owner_o        (),
      .arm_deadline_ms_o  (),
      .exp_valid_i        (1'b0),
      .exp_slot_i         ('0),
      .lstn_reg_change_o  (),
      .tk_decl_state_o    (t_decl_w),
      .lstn_reg_state_o   (),
      .active_o           (),
      .msrp_fail_code_o   (),
      .msrp_fail_bridge_o (),
      .dbg_app_state_o    (),
      .dbg_reg_state_o    ()
  );

  KL_srp_listener_fsm #(
      .N_SINKS_P   (N_SINKS_P),
      .LEAVE_MS_P  (5000),
      .SLOT_BASE_P (32),
      .SLOT_AW_P   (7),
      .OWNER_BASE_P(8'h60)
  ) u_listener (
      .clk_i                  (clk_i),
      .rst_n                  (rst_n),
      .p2p_i                  (p2p_i),
      .ctl_valid_i            (ctl_valid_i),
      .ctl_ready_o            (ctl_ready_o),
      .ctl_settle_i           (ctl_settle_i),
      .ctl_sink_i             (ctl_sink_i[SNK_W_C-1:0]),
      .ctl_stream_id_i        (ctl_stream_id_i),
      .ctl_da_i               (ctl_da_i),
      .ctl_vid_i              (ctl_vid_i),
      .evt_valid_i            (evt_valid_i),
      .evt_msrp_i             (evt_msrp_i),
      .evt_attr_type_i        (evt_attr_type_i),
      .evt_stream_id_i        (evt_stream_id_i),
      .evt_da_i               (evt_da_i),
      .evt_vid_i              (evt_vid_i),
      .evt_mrp_event_i        (evt_mrp_event_i),
      .evt_acc_latency_i      (evt_acc_latency_i),
      .evt_failure_system_id_i(64'd0),
      .evt_failure_code_i     (8'd0),
      .join_tick_i            (join_tick_i),
      .periodic_tick_i        (1'b0),
      .leaveall_rx_i          (4'd0),
      .leaveall_own_i         (leaveall_own_i),
      .txop_done_o            (l_txop_done_o),
      .ev_valid_o             (l_ev_valid_o),
      .ev_ready_i             (1'b1),
      .ev_app_o               (),
      .ev_attr_type_o         (l_ev_attr_type_o),
      .ev_event_o             (l_ev_event_o),
      .ev_fourpack_o          (l_ev_fourpack_o),
      .ev_value_o             (l_ev_value_o),
      .user_valid_o           (),
      .user_join_o            (),
      .user_vid_o             (),
      .user_ready_i           (1'b1),
      .now_ms_i               (now_ms_i),
      .arm_valid_o            (),
      .arm_cancel_o           (),
      .arm_slot_o             (),
      .arm_owner_o            (),
      .arm_deadline_ms_o      (),
      .exp_valid_i            (1'b0),
      .exp_slot_i             ('0),
      .evt_tk_registered_o    (),
      .evt_tk_unregistered_o  (),
      .evt_tk_fail_chg_o      (),
      .evt_tk_latency_chg_o   (),
      .tk_reg_state_o         (),
      .lstn_decl_state_o      (l_decl_w),
      .acc_latency_o          (),
      .msrp_fail_code_o       (),
      .msrp_fail_bridge_o     (),
      .dbg_app_state_o        (),
      .dbg_reg_state_o        ()
  );

endmodule : srp_walk_wrap
`default_nettype wire
