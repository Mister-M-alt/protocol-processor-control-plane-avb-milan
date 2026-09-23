/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : acmp_nvm_wrap.sv
//  Project     : IEEE 1722.1 protocol processor — acmp_nvm suite wrap
//                (docs/architecture/05 §5, 07 §5, 02 §8)
//
//  Description : Integration wrap for the KL_acmp_nvm_shadow suite: the
//                shadow, the REAL KL_pp_nvm_port, the REAL
//                KL_pp_acmp_listener and the REAL KL_pp_acmp_lsn_admit wired
//                at their landed faces, the way protocol_processor_top wires
//                them — the shadow's capture face on the listener's record
//                write port, the shadow's pre_* driving the listener's
//                preload face, the shadow's class-F face on the port's
//                manager face, and the admission gate between the
//                listener's four work faces and their producers. The port's
//                device face, the producers' side of the gate and the
//                listener's TX, RX-slot, timer, PRNG and action faces
//                surface to the harness, which plays the dispatch queue, the
//                event router, the AECP engine, the timer service, the slot
//                pools and the NVM device. tb_cap_* lets the harness inject
//                record writes (started-change and executor traffic
//                stand-ins) ORed behind the listener's own writes;
//                pre_hold_i holds the preload handshake at the manager
//                (valid AND ready masked), the suite's preload-backpressure
//                lever now that the gate lets nothing else hold it.
//
//                The one design decision that matters: the suite compiles
//                the landed modules TOGETHER so face compatibility is proven
//                by elaboration, not by transcription — a port rename or
//                width change in any neighbor breaks this build.
//
//                `ACMP_NVM_PINNED_WIRING` (the Makefile's `pinned` target,
//                never the suite's own run) leaves the gate out and wires
//                the producers straight to the listener, as the top did
//                before issue #92: the build that reproduces the recorded
//                L05 binding loss.
//---------------------------------------------------------------------------//
`default_nettype none

module acmp_nvm_wrap
  import pp_pkg::*;
  import pp_acmp_pkg::*;
#(
    parameter int unsigned  N_SINKS_P     = 8,
    parameter logic [7:0]   REC_ID_BASE_P = 8'h20,
    parameter int unsigned  DEB_TICKS_P   = 500,
    parameter int unsigned  RETRY_MAX_P   = 2,
    parameter string        TROM_HEX_P    = "ltn_rom.hex",
    localparam int unsigned SINK_W_C = (N_SINKS_P > 1) ? $clog2(N_SINKS_P) : 1
) (
    input  wire                      clk_i,           //! core clock
    input  wire                      rst_n,           //! sync active-low reset

    //! ---- shadow control -------------------------------------------------
    input  wire                      tick_i,          //! debounce tick
    input  wire                      restore_go_i,    //! start boot restore
    output logic                     restore_busy_o,  //! restore running
    output logic                     restore_done_o,  //! restore complete level
    output logic                     restore_fail_o,  //! whole-restore abort level
    output logic                     restore_blank_o, //! completed walk validated ZERO records
    output logic                     alarm_o,         //! commit-retry alarm

    //! ---- harness capture injection (behind the listener's writes) ------
    input  wire                      tb_cap_wr_i,     //! inject a record write
    input  wire  [SINK_W_C-1:0]      tb_cap_sink_i,   //! injected sink
    input  wire  [ACMP_REC_W_C-1:0]  tb_cap_rec_i,    //! injected F07.6 image

    //! ---- preload backpressure lever (harness only) ----------------------
    input  wire                      pre_hold_i,      //! hold the preload handshake

    //! ---- producers' side of the admission gate --------------------------
    input  wire                      p_txn_valid_i,   //! dispatch head present
    input  wire  [PP_TXN_W_C-1:0]    p_txn_i,         //! the head (03 §4 record)
    output logic                     p_txn_ready_o,   //! dispatch pop
    input  wire                      p_tk_valid_i,    //! router event present
    input  wire  [1:0]               p_tk_kind_i,     //! TK_KIND_*_C
    input  wire                      p_tk_failed_i,   //! with REG: Talker Failed
    input  wire  [15:0]              p_tk_sink_i,     //! sink payload
    output logic                     p_tk_ready_o,    //! router acknowledge
    input  wire                      p_strm_valid_i,  //! AECP START/STOP held
    input  wire  [15:0]              p_strm_sink_i,   //! Stream Input index
    input  wire                      p_strm_val_i,    //! 1 = started
    output logic                     strm_ready_o,    //! listener completion
    output logic                     strm_error_o,    //! listener bounded-wait error
    input  wire                      p_exp_valid_i,   //! timer expiry strobe
    input  wire  [6:0]               p_exp_slot_i,    //! expired slot
    input  wire  [PP_TIMER_OWNER_W_C-1:0] p_exp_owner_i, //! expired owner

    //! ---- the gate ---------------------------------------------------------
    output logic                     gate_own_o,      //! gate owns the faces
    output logic                     gate_released_o, //! binding walk drained
    output logic [15:0]              gate_exp_drop_o, //! expiries refused while owned
    output logic                     l_txn_valid_o,   //! listener's txn_valid_i
    output logic                     l_txn_ready_o,   //! listener's txn_ready_o
    output logic                     l_tk_valid_o,    //! listener's evt_tk_valid_i
    output logic                     l_tk_ready_o,    //! listener's evt_tk_ready_o
    output logic                     l_strm_valid_o,  //! listener's strm_set_valid_i
    output logic                     l_exp_valid_o,   //! listener's tmr_exp_valid_i

    //! ---- listener TX / RX-slot / timer / PRNG faces (harness pools) -----
    output logic                     txs_alloc_req_o, //! TX slot request
    input  wire                      txs_alloc_gnt_i, //! grant pulse
    input  wire  [2:0]               txs_alloc_slot_i,//! granted slot
    output logic [2:0]               txs_wr_slot_o,   //! slot written
    output logic [10:0]              txs_wr_addr_o,   //! byte offset
    output logic                     txs_wr_valid_o,  //! byte strobe
    output logic [7:0]               txs_wr_data_o,   //! ACMPDU byte
    output logic                     txs_wr_commit_o, //! commit
    output logic [10:0]              txs_wr_len_o,    //! committed length
    output logic                     txreq_valid_o,   //! TX request strobe
    output logic [2:0]               txreq_slot_o,    //! committed slot
    output logic [1:0]               rxs_rd_slot_o,   //! RX payload slot
    output logic [9:0]               rxs_rd_addr_o,   //! ACMPDU byte offset
    output logic                     rxs_rd_en_o,     //! sync-read enable
    input  wire  [7:0]               rxs_rd_data_i,   //! byte, one cycle later
    output logic                     rxs_free_o,      //! slot returned
    output logic [1:0]               rxs_free_slot_o, //! which slot
    output logic                     tmr_arm_valid_o, //! arm/cancel strobe
    output logic                     tmr_arm_cancel_o,//! 1 = cancel
    output logic [PP_TIMER_OWNER_W_C-1:0] tmr_arm_owner_o, //! owner tag
    output logic                     draw_req_o,      //! PRNG draw request
    input  wire                      draw_busy_i,     //! draw in progress
    input  wire                      draw_valid_i,    //! draw result valid
    input  wire  [15:0]              draw_ms_i,       //! drawn delay

    //! ---- listener action strobes ------------------------------------------
    output logic                     lsn_settle_o,    //! A15
    output logic                     lsn_teardown_o,  //! A8
    output logic                     lsn_disarm_o,    //! A9
    output logic                     lsn_notify_o,    //! committed-change trigger
    output logic [N_SINKS_P-1:0]     lsn_started_o,   //! started/stopped mirror

    //! ---- device face (harness region-store model) -----------------------
    output logic                     dev_req_o,       //! command request
    input  wire                      dev_gnt_i,       //! command accept
    output logic [1:0]               dev_op_o,        //! READ/WRITE/ERASE
    output logic [7:0]               dev_region_o,    //! region id = record id
    output logic [15:0]              dev_offset_o,    //! byte offset
    output logic [15:0]              dev_len_o,       //! byte count
    output logic                     dev_wvalid_o,    //! write byte present
    input  wire                      dev_wready_i,    //! backend accepts
    output logic [7:0]               dev_wdata_o,     //! write byte
    input  wire                      dev_rvalid_i,    //! read byte present
    input  wire  [7:0]               dev_rdata_i,     //! read byte
    output logic                     dev_rready_o,    //! port accepts
    input  wire                      dev_busy_i,      //! backend busy
    input  wire                      dev_done_i,      //! command complete
    input  wire                      dev_err_i,       //! command failed

    //! ---- observability out ----------------------------------------------
    output logic                     pre_valid_o,     //! shadow preload valid
    output logic [15:0]              pre_sink_o,      //! shadow preload sink
    output logic [63:0]              pre_talker_eid_o,//! shadow preload talker EID
    output logic [15:0]              pre_talker_uid_o,//! shadow preload talker uid
    output logic [63:0]              pre_ctlr_eid_o,  //! shadow preload ctlr EID
    output logic                     pre_sw_o,        //! shadow preload SW
    output logic                     pre_started_o,   //! shadow preload started
    output logic                     pre_ready_o,     //! listener's pre_ready
    output logic                     lsn_recwr_o,     //! listener record write
    output logic [SINK_W_C-1:0]      lsn_recwr_sink_o,//! listener written sink
    output logic [ACMP_REC_W_C-1:0]  lsn_recwr_rec_o, //! listener written record
    output logic                     lsn_disc_arm_o,  //! listener A4 strobe
    output logic [63:0]              lsn_disc_eid_o,  //! listener A4 talker EID
    output logic                     lsn_busy_o,      //! listener executor busy
    output logic [4:0]               lsn_state_o,     //! listener executor state
    output logic [3:0]               mgr_state_o,     //! shadow engine state
    output logic [SINK_W_C-1:0]      mgr_rs_sink_o,   //! shadow restore cursor
    output logic [N_SINKS_P-1:0]     dbg_dirty_o,     //! shadow dirty bits
    output logic [N_SINKS_P-1:0]     dbg_valid_o,     //! shadow valid bits
    output logic [N_SINKS_P-1:0]     dbg_touched_o,   //! shadow touched bits
    //! the port's MANAGER-face completion, the pulse the shadow clears a
    //! dirty bit on. The device face's own dev_done_i is two cycles earlier
    //! (KL_pp_nvm_port S_RPWAIT -> S_FIN), so a suite grading WHEN the
    //! unflushed export falls has to see this one.
    output logic                     dbg_port_done_o
);

  localparam logic [63:0] ENTITY_ID_C = 64'h0A0B_0C0D_0E0F_1011;

  // ---- shadow <-> port manager face ---------------------------------------
  logic        nvm_req_w, nvm_we_w;
  logic [7:0]  nvm_record_id_w;
  logic        nvm_wvalid_w, nvm_wready_w;
  logic [7:0]  nvm_wdata_w;
  logic        nvm_rvalid_w, nvm_rready_w;
  logic [7:0]  nvm_rdata_w;
  logic        nvm_busy_w, nvm_done_w, nvm_err_w;

  // ---- shadow <-> listener preload face -----------------------------------
  logic        pre_valid_w;
  logic [15:0] pre_sink_w;
  logic [63:0] pre_talker_eid_w;
  logic [15:0] pre_talker_uid_w;
  logic [63:0] pre_ctlr_eid_w;
  logic        pre_sw_w, pre_started_w, pre_ready_w;

  // ---- listener record write port + harness injection ---------------------
  logic                    lsn_recwr_w;
  logic [SINK_W_C-1:0]     lsn_recwr_sink_w;
  logic [ACMP_REC_W_C-1:0] lsn_recwr_rec_w;

  logic                    cap_wr_w;
  logic [SINK_W_C-1:0]     cap_sink_w;
  logic [ACMP_REC_W_C-1:0] cap_rec_w;

  assign cap_wr_w   = lsn_recwr_w || tb_cap_wr_i;
  assign cap_sink_w = lsn_recwr_w ? lsn_recwr_sink_w : tb_cap_sink_i;
  assign cap_rec_w  = lsn_recwr_w ? lsn_recwr_rec_w : tb_cap_rec_i;

  // ---- DUT ---------------------------------------------------------------
  KL_acmp_nvm_shadow #(
      .N_SINKS_P    (N_SINKS_P),
      .REC_ID_BASE_P(REC_ID_BASE_P),
      .DEB_TICKS_P  (DEB_TICKS_P),
      .RETRY_MAX_P  (RETRY_MAX_P)
  ) u_shadow (
      .clk_i           (clk_i),
      .rst_n           (rst_n),
      .tick_i          (tick_i),
      .restore_go_i    (restore_go_i),
      .restore_busy_o  (restore_busy_o),
      .restore_done_o  (restore_done_o),
      .restore_fail_o  (restore_fail_o),
      .restore_blank_o (restore_blank_o),
      .alarm_o         (alarm_o),
      .cap_wr_i        (cap_wr_w),
      .cap_sink_i      (cap_sink_w),
      .cap_rec_i       (cap_rec_w),
      .pre_valid_o     (pre_valid_w),
      .pre_sink_o      (pre_sink_w),
      .pre_talker_eid_o(pre_talker_eid_w),
      .pre_talker_uid_o(pre_talker_uid_w),
      .pre_ctlr_eid_o  (pre_ctlr_eid_w),
      .pre_sw_o        (pre_sw_w),
      .pre_started_o   (pre_started_w),
      .pre_ready_i     (pre_ready_w && !pre_hold_i),
      .nvm_req_o       (nvm_req_w),
      .nvm_we_o        (nvm_we_w),
      .nvm_record_id_o (nvm_record_id_w),
      .nvm_wvalid_o    (nvm_wvalid_w),
      .nvm_wready_i    (nvm_wready_w),
      .nvm_wdata_o     (nvm_wdata_w),
      .nvm_rvalid_i    (nvm_rvalid_w),
      .nvm_rready_o    (nvm_rready_w),
      .nvm_rdata_i     (nvm_rdata_w),
      .nvm_busy_i      (nvm_busy_w),
      .nvm_done_i      (nvm_done_w),
      .nvm_err_i       (nvm_err_w),
      .dbg_dirty_o     (dbg_dirty_o),
      .dbg_valid_o     (dbg_valid_o),
      .dbg_touched_o   (dbg_touched_o)
  );

  // ---- the real class-F port ----------------------------------------------
  KL_pp_nvm_port #(
      .MAX_PAYLOAD_P(1024)
  ) u_port (
      .clk_i          (clk_i),
      .rst_n          (rst_n),
      .nvm_req_i      (nvm_req_w),
      .nvm_we_i       (nvm_we_w),
      .nvm_record_id_i(nvm_record_id_w),
      .nvm_wvalid_i   (nvm_wvalid_w),
      .nvm_wready_o   (nvm_wready_w),
      .nvm_wdata_i    (nvm_wdata_w),
      .nvm_rvalid_o   (nvm_rvalid_w),
      .nvm_rready_i   (nvm_rready_w),
      .nvm_rdata_o    (nvm_rdata_w),
      .nvm_busy_o     (nvm_busy_w),
      .nvm_done_o     (nvm_done_w),
      .nvm_err_o      (nvm_err_w),
      .dev_req_o      (dev_req_o),
      .dev_gnt_i      (dev_gnt_i),
      .dev_op_o       (dev_op_o),
      .dev_region_o   (dev_region_o),
      .dev_offset_o   (dev_offset_o),
      .dev_len_o      (dev_len_o),
      .dev_wvalid_o   (dev_wvalid_o),
      .dev_wready_i   (dev_wready_i),
      .dev_wdata_o    (dev_wdata_o),
      .dev_rvalid_i   (dev_rvalid_i),
      .dev_rdata_i    (dev_rdata_i),
      .dev_rready_o   (dev_rready_o),
      .dev_busy_i     (dev_busy_i),
      .dev_done_i     (dev_done_i),
      .dev_err_i      (dev_err_i)
  );

  // ---- the admission gate, wired as protocol_processor_top wires it --------
  logic lsn_txn_ready_w, lsn_tk_ready_w;
  logic lsn_arm_w;

`ifdef ACMP_NVM_PINNED_WIRING
  assign gate_own_o      = 1'b0;
  assign gate_released_o = 1'b1;
  assign gate_exp_drop_o = 16'd0;
  assign l_txn_valid_o   = p_txn_valid_i;
  assign p_txn_ready_o   = lsn_txn_ready_w;
  assign l_tk_valid_o    = p_tk_valid_i;
  assign p_tk_ready_o    = lsn_tk_ready_w;
  assign l_strm_valid_o  = p_strm_valid_i;
  assign l_exp_valid_o   = p_exp_valid_i;
`else
  KL_pp_acmp_lsn_admit #(
      .N_SINKS_P        (N_SINKS_P),
      .TMR_OWNER_BASE_P (32),
      .OWNER_W_P        (PP_TIMER_OWNER_W_C)
  ) u_admit (
      .clk_i          (clk_i),
      .rst_n          (rst_n),
      .walk_done_i    (restore_done_o),
      .pre_valid_i    (pre_valid_w),
      .lsn_busy_i     (lsn_busy_o),
      .lsn_arm_i      (lsn_arm_w),
      .own_o          (gate_own_o),
      .released_o     (gate_released_o),
      .p_txn_valid_i  (p_txn_valid_i),
      .p_txn_ready_o  (p_txn_ready_o),
      .l_txn_valid_o  (l_txn_valid_o),
      .l_txn_ready_i  (lsn_txn_ready_w),
      .p_tk_valid_i   (p_tk_valid_i),
      .p_tk_ready_o   (p_tk_ready_o),
      .l_tk_valid_o   (l_tk_valid_o),
      .l_tk_ready_i   (lsn_tk_ready_w),
      .p_strm_valid_i (p_strm_valid_i),
      .l_strm_valid_o (l_strm_valid_o),
      .p_exp_valid_i  (p_exp_valid_i),
      .p_exp_owner_i  (p_exp_owner_i),
      .l_exp_valid_o  (l_exp_valid_o),
      .dbg_exp_drop_o (gate_exp_drop_o)
  );
`endif

  assign l_txn_ready_o = lsn_txn_ready_w;
  assign l_tk_ready_o  = lsn_tk_ready_w;

  // ---- the real listener ----------------------------------------------------
  KL_pp_acmp_listener #(
      .N_SINKS_P (N_SINKS_P),
      .TROM_HEX_P(TROM_HEX_P)
  ) u_listener (
      .clk_i                (clk_i),
      .rst_n                (rst_n),
      .entity_id_i          (ENTITY_ID_C),
      .txn_valid_i          (l_txn_valid_o),
      .txn_i                (pp_txn_t'(p_txn_i)),
      .txn_ready_o          (lsn_txn_ready_w),
      .evt_tk_valid_i       (l_tk_valid_o),
      .evt_tk_kind_i        (p_tk_kind_i),
      .evt_tk_failed_i      (p_tk_failed_i),
      .evt_tk_sink_i        (p_tk_sink_i),
      .evt_tk_ready_o       (lsn_tk_ready_w),
      .pre_valid_i          (pre_valid_w && !pre_hold_i),
      .pre_sink_i           (pre_sink_w),
      .pre_talker_eid_i     (pre_talker_eid_w),
      .pre_talker_uid_i     (pre_talker_uid_w),
      .pre_ctlr_eid_i       (pre_ctlr_eid_w),
      .pre_sw_i             (pre_sw_w),
      .pre_started_i        (pre_started_w),
      .pre_ready_o          (pre_ready_w),
      .strm_set_valid_i     (l_strm_valid_o),
      .strm_set_sink_i      (p_strm_sink_i),
      .strm_set_val_i       (p_strm_val_i),
      .strm_set_ready_o     (strm_ready_o),
      .strm_set_error_o     (strm_error_o),
      .strm_started_o       (lsn_started_o),
      .now_ms_i             (32'd0),
      .tmr_arm_valid_o      (tmr_arm_valid_o),
      .tmr_arm_cancel_o     (tmr_arm_cancel_o),
      .tmr_arm_slot_o       (),
      .tmr_arm_owner_o      (tmr_arm_owner_o),
      .tmr_arm_deadline_ms_o(),
      .tmr_exp_valid_i      (l_exp_valid_o),
      .tmr_exp_slot_i       (p_exp_slot_i),
      .tmr_exp_owner_i      (p_exp_owner_i),
      .draw_req_o           (draw_req_o),
      .draw_kind_o          (),
      .draw_busy_i          (draw_busy_i),
      .draw_valid_i         (draw_valid_i),
      .draw_ms_i            (draw_ms_i),
      .rxs_rd_slot_o        (rxs_rd_slot_o),
      .rxs_rd_addr_o        (rxs_rd_addr_o),
      .rxs_rd_en_o          (rxs_rd_en_o),
      .rxs_rd_data_i        (rxs_rd_data_i),
      .rxs_free_o           (rxs_free_o),
      .rxs_free_slot_o      (rxs_free_slot_o),
      .txs_alloc_req_o      (txs_alloc_req_o),
      .txs_oversize_o       (),
      .txs_alloc_gnt_i      (txs_alloc_gnt_i),
      .txs_alloc_slot_i     (txs_alloc_slot_i),
      .txs_wr_slot_o        (txs_wr_slot_o),
      .txs_wr_addr_o        (txs_wr_addr_o),
      .txs_wr_valid_o       (txs_wr_valid_o),
      .txs_wr_data_o        (txs_wr_data_o),
      .txs_wr_commit_o      (txs_wr_commit_o),
      .txs_wr_len_o         (txs_wr_len_o),
      .txreq_valid_o        (txreq_valid_o),
      .txreq_slot_o         (txreq_slot_o),
      .lock_held_i          (1'b0),
      .lock_ctlr_i          (64'd0),
      .act_settle_o         (lsn_settle_o),
      .act_settle_sid_o     (),
      .act_settle_da_o      (),
      .act_settle_vlan_o    (),
      .act_teardown_o       (lsn_teardown_o),
      .act_disc_arm_o       (lsn_arm_w),
      .act_disc_talker_eid_o(lsn_disc_eid_o),
      .act_disc_disarm_o    (lsn_disarm_o),
      .act_nvm_o            (),
      .act_nvm_set_o        (),
      .act_notify_o         (lsn_notify_o),
      .act_sink_o           (),
      .dbg_busy_o           (lsn_busy_o),
      .dbg_strq_drop_o      (),
      .act_strt_chg_o       (),
      .act_strt_cmd_chg_o   (),
      .dbg_recwr_o          (lsn_recwr_w),
      .dbg_recwr_sink_o     (lsn_recwr_sink_w),
      .dbg_recwr_rec_o      (lsn_recwr_rec_w)
  );

  // ---- observability mirrors ----------------------------------------------
  assign pre_valid_o      = pre_valid_w;
  assign pre_sink_o       = pre_sink_w;
  assign pre_talker_eid_o = pre_talker_eid_w;
  assign pre_talker_uid_o = pre_talker_uid_w;
  assign pre_ctlr_eid_o   = pre_ctlr_eid_w;
  assign pre_sw_o         = pre_sw_w;
  assign pre_started_o    = pre_started_w;
  assign pre_ready_o      = pre_ready_w;
  assign lsn_recwr_o      = lsn_recwr_w;
  assign lsn_recwr_sink_o = lsn_recwr_sink_w;
  assign lsn_recwr_rec_o  = lsn_recwr_rec_w;
  assign lsn_disc_arm_o   = lsn_arm_w;
  //! suite taps on two engines' state registers, read by hierarchical
  //! reference so neither module grows a port for a testbench
  assign lsn_state_o      = 5'(u_listener.xs_r);
  assign mgr_state_o      = 4'(u_shadow.hs_r);
  assign mgr_rs_sink_o    = u_shadow.rs_k_r;
  assign dbg_port_done_o  = nvm_done_w;

endmodule

`default_nettype wire
