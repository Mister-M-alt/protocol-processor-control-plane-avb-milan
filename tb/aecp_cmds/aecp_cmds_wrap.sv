// SPDX-License-Identifier: CERN-OHL-W-2.0
//
// tb/aecp_cmds harness wrapper: KL_aecp_engine alone, with the 03 §4 record
// packed from plain ports and every gather face answering zero at once.
//
// The one thing the suite reads that is not an engine port is the µCPU's
// dispatch handshake: `obs_disp_o` is high in the cycle the engine hands the
// µCPU a program and `obs_upc_o` is the µPC it hands over (the engine's
// u_ucpu disp_* ports). That is the observed dispatch the AECP command table
// is held to (scripts/check_aecp_commands.py, issue #73).
module aecp_cmds_wrap
  import pp_pkg::*;
#(
    //! the build the unsolicited sweep needs: 1 gives PP_UNS_IDENT_C its
    //! program (the default 0 retires the job on E_NOSEND); no command
    //! decode reads it
    parameter bit EN_IDENTIFY_NOTIF_P = 1'b1
) (
    input  wire         clk_i,
    input  wire         rst_n,

    // ---- the command record, packed below --------------------------------
    input  wire         t_valid_i,
    input  wire  [2:0]  t_protocol_i,
    input  wire  [3:0]  t_msg_type_i,
    input  wire  [10:0] t_cdl_i,
    input  wire  [15:0] t_opcode_i,
    input  wire  [63:0] t_ctlr_eid_i,
    input  wire  [47:0] t_src_mac_i,
    input  wire  [63:0] t_target_eid_i,
    input  wire  [15:0] t_seq_i,
    input  wire  [2:0]  t_slot_i,
    output logic        txn_ready_o,

    // ---- RX slot read --------------------------------------------------------
    output logic [9:0]  rxs_rd_addr_o,
    output logic        rxs_rd_en_o,
    input  wire  [7:0]  rxs_rd_data_i,
    input  wire  [9:0]  rxs_slot_len_i,
    output logic        rxs_free_o,

    // ---- TX slot and lanes ---------------------------------------------------
    output logic        txs_alloc_req_o,
    input  wire         txs_alloc_gnt_i,
    output logic [10:0] txs_wr_addr_o,
    output logic        txs_wr_valid_o,
    output logic [7:0]  txs_wr_data_o,
    output logic        txs_wr_commit_o,
    output logic [10:0] txs_wr_len_o,
    output logic        txreq_valid_o,
    output logic        txreq_uns_valid_o,

    // ---- descriptor memory ---------------------------------------------------
    output logic        mem_req_valid_o,
    input  wire         mem_req_ready_i,
    output logic [31:0] mem_req_addr_o,
    output logic  [8:0] mem_req_beats_o,
    input  wire         mem_rsp_valid_i,
    output logic        mem_rsp_ready_o,
    input  wire  [63:0] mem_rsp_data_i,
    input  wire         mem_rsp_last_i,
    input  wire         mem_rsp_err_i,

    // ---- response-buffer memory ----------------------------------------------
    output logic        rmem_req_valid_o,
    input  wire         rmem_req_ready_i,
    output logic [31:0] rmem_req_addr_o,
    output logic  [8:0] rmem_req_beats_o,
    input  wire         rmem_rsp_valid_i,
    output logic        rmem_rsp_ready_o,
    input  wire  [63:0] rmem_rsp_data_i,
    input  wire         rmem_rsp_last_i,
    input  wire         rmem_rsp_err_i,
    output logic        rmem_wr_valid_o,
    input  wire         rmem_wr_ready_i,
    output logic [31:0] rmem_wr_addr_o,
    output logic [63:0] rmem_wr_data_o,
    output logic  [7:0] rmem_wr_strb_o,
    input  wire         rmem_wr_done_i,

    // ---- the D3 writer's NVM manager face --------------------------------------
    output logic        d3_m_req_o,
    output logic        d3_m_we_o,
    output logic        d3_m_wvalid_o,
    input  wire         d3_m_gnt_i,
    input  wire         d3_m_done_i,
    input  wire         d3_m_err_i,
    input  wire  [1:0]  d3_m_err_cause_i,
    output logic        d3_done_o,
    output logic        d3_fail_o,
    output logic        d3_closed_o,

    // ---- unsolicited jobs ------------------------------------------------------
    input  wire         uns_valid_i,
    input  wire  [3:0]  uns_kind_i,
    input  wire  [15:0] uns_desc_type_i,
    input  wire  [15:0] uns_desc_index_i,
    input  wire         uns_amap_remove_i,
    input  wire  [15:0] uns_amap_count_i,
    input  wire  [15:0] uns_arg0_i,
    input  wire  [15:0] uns_arg1_i,
    output logic        uns_done_o,

    // ---- the running predicate ---------------------------------------------------
    input  wire  [7:0]  strm_bound_i,
    input  wire  [7:0]  strm_streaming_i,

    // ---- observation ---------------------------------------------------------------
    output logic        dbg_busy_o,
    output logic        dbg_img_valid_o,
    output logic        obs_disp_o,
    output logic [10:0] obs_upc_o,
    output logic        obs_batch_o
);

  localparam logic [63:0] ENTITY_ID_C = 64'h0001_02FF_FE03_0405;
  localparam logic [47:0] OWN_MAC_C   = 48'h00_01_02_03_04_05;

  pp_txn_t txn_w;
  always_comb begin
    txn_w                = '0;
    txn_w.origin         = PP_ORIGIN_RX;
    txn_w.protocol       = pp_protocol_e'(t_protocol_i);
    txn_w.msg_type       = t_msg_type_i;
    txn_w.cdl            = t_cdl_i;
    txn_w.src_mac        = t_src_mac_i;
    txn_w.controller_eid = t_ctlr_eid_i;
    txn_w.target_eid     = t_target_eid_i;
    txn_w.sequence_id    = t_seq_i;
    txn_w.opcode         = t_opcode_i;
    txn_w.rx_slot        = t_slot_i;
  end

  KL_aecp_engine #(
      .EN_IDENTIFY_NOTIF_P (EN_IDENTIFY_NOTIF_P)
  ) u_eng (
      .clk_i              (clk_i),
      .rst_n              (rst_n),
      .entity_id_i        (ENTITY_ID_C),
      .own_mac_i          (OWN_MAC_C),
      .txn_valid_i        (t_valid_i),
      .txn_i              (txn_w),
      .txn_ready_o        (txn_ready_o),
      .rxs_rd_slot_o      (),
      .rxs_rd_addr_o      (rxs_rd_addr_o),
      .rxs_rd_en_o        (rxs_rd_en_o),
      .rxs_rd_data_i      (rxs_rd_data_i),
      .rxs_slot_len_i     (rxs_slot_len_i),
      .rxs_free_o         (rxs_free_o),
      .rxs_free_slot_o    (),
      .txs_alloc_req_o    (txs_alloc_req_o),
      .txs_oversize_o     (),
      .txs_alloc_gnt_i    (txs_alloc_gnt_i),
      .txs_alloc_slot_i   ('0),
      .txs_wr_slot_o      (),
      .txs_wr_addr_o      (txs_wr_addr_o),
      .txs_wr_valid_o     (txs_wr_valid_o),
      .txs_wr_data_o      (txs_wr_data_o),
      .txs_wr_commit_o    (txs_wr_commit_o),
      .txs_wr_len_o       (txs_wr_len_o),
      .txreq_valid_o      (txreq_valid_o),
      .txreq_slot_o       (),
      .txreq_ready_i      (1'b1),
      .dl_kill_i          (1'b0),
      .dl_queued_o        (),
      .mem_req_valid_o    (mem_req_valid_o),
      .mem_req_ready_i    (mem_req_ready_i),
      .mem_req_addr_o     (mem_req_addr_o),
      .mem_req_beats_o    (mem_req_beats_o),
      .mem_rsp_valid_i    (mem_rsp_valid_i),
      .mem_rsp_ready_o    (mem_rsp_ready_o),
      .mem_rsp_data_i     (mem_rsp_data_i),
      .mem_rsp_last_i     (mem_rsp_last_i),
      .mem_rsp_err_i      (mem_rsp_err_i),
      .rmem_req_valid_o   (rmem_req_valid_o),
      .rmem_req_ready_i   (rmem_req_ready_i),
      .rmem_req_addr_o    (rmem_req_addr_o),
      .rmem_req_beats_o   (rmem_req_beats_o),
      .rmem_rsp_valid_i   (rmem_rsp_valid_i),
      .rmem_rsp_ready_o   (rmem_rsp_ready_o),
      .rmem_rsp_data_i    (rmem_rsp_data_i),
      .rmem_rsp_last_i    (rmem_rsp_last_i),
      .rmem_rsp_err_i     (rmem_rsp_err_i),
      .rmem_wr_valid_o    (rmem_wr_valid_o),
      .rmem_wr_ready_i    (rmem_wr_ready_i),
      .rmem_wr_addr_o     (rmem_wr_addr_o),
      .rmem_wr_data_o     (rmem_wr_data_o),
      .rmem_wr_strb_o     (rmem_wr_strb_o),
      .rmem_wr_done_i     (rmem_wr_done_i),
      .rmem_wr_err_i      (1'b0),
      .ctr_req_o          (),
      .ctr_desc_type_o    (),
      .ctr_desc_index_o   (),
      .ctr_word_o         (),
      .ctr_data_i         ('0),
      .ctr_wait_i         (1'b0),
      .amap_req_o         (),
      .amap_desc_type_o   (),
      .amap_desc_index_o  (),
      .amap_map_index_o   (),
      .amap_sel_o         (),
      .amap_rec_o         (),
      .amap_data_i        ('0),
      .amap_wait_i        (1'b0),
      .amap_edit_req_o    (),
      .amap_edit_phase_o  (),
      .amap_edit_remove_o (),
      .amap_edit_desc_type_o  (),
      .amap_edit_desc_index_o (),
      .amap_edit_count_o  (),
      .amap_edit_rec_o    (),
      .amap_edit_record_o (),
      .amap_edit_value_o  (),
      .amap_edit_data_i   ('0),
      .amap_edit_wait_i   (1'b0),
      .gsi_req_o          (),
      .gsi_kind_o         (),
      .gsi_desc_type_o    (),
      .gsi_desc_index_o   (),
      .gsi_sel_o          (),
      .gsi_ord_o          (),
      .gsi_prop_fmt_o     (),
      .gsi_data_i         ('0),
      .gsi_wait_i         (1'b0),
      .rgy_req_o          (),
      .rgy_state_o        (),
      .rgy_op_o           (),
      .rgy_eid_o          (),
      .rgy_mac_o          (),
      .rgy_tl_o           (),
      .rgy_flags_o        (),
      .rgy_data_i         ('0),
      .rgy_wait_i         (1'b0),
      .uns_valid_i        (uns_valid_i),
      .uns_kind_i         (uns_kind_i),
      .uns_desc_type_i    (uns_desc_type_i),
      .uns_desc_index_i   (uns_desc_index_i),
      .uns_ctlr_eid_i     (64'h0011_2233_4455_6677),
      .uns_mac_i          (48'h02_11_22_33_44_55),
      .uns_seq_i          (16'h0101),
      .uns_amap_remove_i  (uns_amap_remove_i),
      .uns_amap_count_i   (uns_amap_count_i),
      .uns_arg0_i         (uns_arg0_i),
      .uns_arg1_i         (uns_arg1_i),
      .amap_notify_busy_i (1'b0),
      .uns_done_o         (uns_done_o),
      .txreq_uns_valid_o  (txreq_uns_valid_o),
      .txreq_uns_ready_i  (1'b1),
      .strm_bound_i       (strm_bound_i),
      .strm_started_i     ('0),
      .strm_set_valid_o   (),
      .strm_set_index_o   (),
      .strm_set_val_o     (),
      .strm_set_ready_i   (1'b1),
      .strm_set_error_i   (1'b0),
      .strm_streaming_i   (strm_streaming_i),
      .lock_held_i        (1'b0),
      .lock_ctlr_i        ('0),
      .name_wr_o          (),
      .d3_go_i            (1'b1),
      .d3_rs_go_i         (1'b1),
      .d3_agg_o           (),
      .d3_tick_i          (1'b0),
      .d3_m_req_o         (d3_m_req_o),
      .d3_m_we_o          (d3_m_we_o),
      .d3_m_rid_o         (),
      .d3_m_wvalid_o      (d3_m_wvalid_o),
      .d3_m_wdata_o       (),
      .d3_m_rready_o      (),
      .d3_m_abort_o       (),
      .d3_m_gnt_i         (d3_m_gnt_i),
      .d3_m_wready_i      (1'b1),
      .d3_m_done_i        (d3_m_done_i),
      .d3_m_err_i         (d3_m_err_i),
      .d3_m_rvalid_i      (1'b0),
      .d3_m_rdata_i       ('0),
      .d3_m_err_cause_i   (d3_m_err_cause_i),
      .d3_done_o          (d3_done_o),
      .d3_fail_o          (d3_fail_o),
      .d3_closed_o        (d3_closed_o),
      .d3_cause_o         (),
      .d3_blank_o         (),
      .d3_rb_o            (),
      .d3_desc_debt_i     (1'b0),
      .d3_unflushed_o     (),
      .d3_alarm_o         (),
      .eff_commit_o       (),
      .eff_nvm_mark_o     (),
      .eff_nvm_stb_o      (),
      .eff_notify_class_o (),
      .eff_notify_stb_o   (),
      .eff_notify_type_o  (),
      .eff_notify_index_o (),
      .eff_notify_arg0_o  (),
      .eff_notify_arg1_o  (),
      .eff_notify_excl_eid_o (),
      .dbg_busy_o         (dbg_busy_o),
      .dbg_cmd_cnt_o      (),
      .dbg_resp_cnt_o     (),
      .dbg_drop_cnt_o     (),
      .dbg_status_o       (),
      .dbg_len_o          (),
      .dbg_img_valid_o    (dbg_img_valid_o),
      .dbg_img_fault_o    (),
      .dbg_locate_miss_o  (),
      .dbg_resp_fault_o   (),
      .dbg_resp_err_o     (),
      .dbg_resp_lane_o    (),
      .dyn_cur_config_o   (),
      .dyn_cur_config_v_o (),
      .dyn_identify_o     (),
      .dyn_clk_src_index_o (),
      .dyn_pt_offset_o    (),
      .dyn_pt_offset_v_o  (),
      .dyn_fmt_in_o       (),
      .dyn_fmt_in_v_o     (),
      .dyn_fmt_out_o      (),
      .dyn_fmt_out_v_o    (),
      .dyn_dirty_o        ()
  );

  //! the dispatch handshake into the µCPU (KL_aecp_ucpu disp_*): a program
  //! is handed over in the cycle both are high
  assign obs_disp_o  = u_eng.u_ucpu.disp_valid_i && u_eng.u_ucpu.disp_ready_o;
  assign obs_upc_o   = u_eng.u_ucpu.disp_upc_i;
  assign obs_batch_o = u_eng.u_ucpu.disp_batch_i;

endmodule
