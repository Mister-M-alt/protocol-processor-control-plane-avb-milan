/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : pp_top_wrap.sv
//  Project     : IEEE 1722.1 protocol processor — pp_top suite
//
//  Description : End-to-end harness around the REAL protocol_processor_top:
//                nothing but the top's own external contract is exposed —
//                the MAC trunk byte streams, the side-port host face, the
//                SRP service face, the NVM device face, the AECP pop face
//                and the level controls. Time is compressed: 1 ms = 100 clk
//                (DIV_US 2 x DIV_MS 50), which keeps the 89-slot deadline
//                sweep (91 cycles) inside the ms tick while T-ADP-DELAY(-
//                START), T-ADP-ADV, T-ACMP-DELAY, T-MRP-JOIN and the NVM
//                debounce all run for real. The restore deadlines are not
//                compressed that way: the top derives them from CLK_HZ_P,
//                set here to 1,000,001 Hz, and no override stands in for
//                the derivation.
//
//                The one decision that matters: the talker source shape is
//                tied HERE (all 8 sources enabled, stream_id k =
//                {own_mac, k}) because the suite drives SRP declarations
//                through the svc face with its own stream_ids — the tied
//                lanes only feed talker-command flows this suite does not
//                byte-check.
//
//                The three parameters the suite builds again:
//                SRP_DOM_DEF_VID_P is overridden ONLY when the second build
//                defines PP_TOP_SRP_DOM_DEF_VID, EN_IDENTIFY_NOTIF_P is set
//                to 1 ONLY when the third build defines PP_TOP_EN_IDENT, and
//                DESC_LINE_BYTES_P ONLY when the fourth defines
//                PP_TOP_DESC_LINE_BYTES (Makefile), so the first build grades
//                the top's own defaults and never a copy of them.
//                The fifth build defines PP_TOP_TIM_REAL and runs the
//                timebase at 1 ms = 1,000 clk, the nominal clock's own, for
//                section TB's response budgets. The sixth defines
//                PP_TOP_TIM_DEFAULTS and drops the two 400 ms timeout
//                overrides below, so section TD grades the top's own
//                T-NOTIF-TIMELIMITED and T-LOCK-UNLOCK on this timebase.
//                The seventh defines PP_TOP_IF2: P-N-AVB-INTERFACES = 2, with
//                rx_if_index_i connected, for section IF. Every other build
//                leaves the top's interface port unconnected, as a
//                one-interface integration does, so it reads its default.
//---------------------------------------------------------------------------//
`default_nettype none

module pp_top_wrap (
    input  wire         clk_i,
    input  wire         rst_n,

    // identity + model
    input  wire  [63:0] entity_id_i,
    input  wire  [63:0] entity_model_id_i,
    input  wire  [47:0] own_mac_i,
    input  wire  [15:0] talker_sources_i,
    input  wire  [15:0] talker_caps_i,
    input  wire  [15:0] listener_sinks_i,
    input  wire  [15:0] listener_caps_i,
    input  wire  [15:0] current_cfg_i,
    input  wire  [15:0] identify_index_i,
    //! identifyButtonPressed; the third build (PP_TOP_EN_IDENT) is the only
    //! one whose top reads it
    input  wire         identify_button_i,

    // level controls + class-D in
    input  wire         entity_enable_i,
    input  wire         link_up_i,
    input  wire         gm_change_i,
    input  wire  [63:0] gm_id_i,
    input  wire  [7:0]  gptp_domain_i,

    // SRP quasi-static
    input  wire         p2p_i,
    input  wire         cfg_rank_i,
    input  wire  [31:0] cfg_acc_lat_ns_i,
    input  wire  [31:0] port_rate_bps_i,
    input  wire  [15:0] cfg_tspec_max_frame_i,

    // MAC trunk RX
    input  wire         rx_valid_i,
    input  wire  [7:0]  rx_data_i,
    input  wire         rx_last_i,
    //! the frame's AVB interface, connected in the seventh build alone
    input  wire  [1:0]  rx_if_index_i,

    // MAC TX
    output logic        tx_valid_o,
    output logic        tx_sof_o,
    output logic [7:0]  tx_data_o,
    output logic        tx_eof_o,
    input  wire         tx_ready_i,

    // AECP pop face (kept live: an integrator may still observe/drain it)
    output logic        aecp_txn_valid_o,
    input  wire         aecp_txn_ready_i,
    //! the optional external drain's slot return, 0 but where a case steals
    //! a record (D3O7), and the RX slot of the head record it pops
    input  wire         aecp_rxs_free_i,
    input  wire  [1:0]  aecp_rxs_free_slot_i,
    output logic [2:0]  aecp_txn_slot_o,

    // GET_COUNTERS read face (06 §6.6) — the C++ harness plays the
    // integrator's counter store behind it, so the counter VALUES this suite
    // checks are the model's and never the DUT's own
    output logic        ctr_req_o,
    output logic [15:0] ctr_desc_type_o,
    output logic [15:0] ctr_desc_index_o,
    output logic  [5:0] ctr_word_o,
    input  wire  [31:0] ctr_data_i,
    input  wire         ctr_wait_i,
    input  wire         ctr_change_i,
    input  wire  [15:0] ctr_change_desc_type_i,
    input  wire  [15:0] ctr_change_desc_index_i,

    // GET_AUDIO_MAP read face (06 §6.5) - same bargain: the harness plays
    // the integrator's dynamic-mapping store, so every geometry word and
    // every record this suite checks on the wire is the model's
    output logic        amap_req_o,
    output logic [15:0] amap_desc_type_o,
    output logic [15:0] amap_desc_index_o,
    output logic [15:0] amap_map_index_o,
    output logic  [1:0] amap_sel_o,
    output logic  [7:0] amap_rec_o,
    input  wire  [63:0] amap_data_i,
    input  wire         amap_wait_i,

    // ADD/REMOVE_AUDIO_MAPPINGS transaction face
    output logic        amap_edit_req_o,
    output logic  [2:0] amap_edit_phase_o,
    output logic        amap_edit_remove_o,
    output logic [15:0] amap_edit_desc_type_o,
    output logic [15:0] amap_edit_desc_index_o,
    output logic [15:0] amap_edit_count_o,
    output logic  [7:0] amap_edit_rec_o,
    output logic [63:0] amap_edit_record_o,
    output logic [63:0] amap_edit_value_o,
    input  wire  [63:0] amap_edit_data_i,
    input  wire         amap_edit_wait_i,

    // Milan-info gather face (06 SS6.2/SS6.10) - the harness is the integrator
    output logic        gsi_req_o,
    output logic [1:0]  gsi_kind_o,
    output logic [15:0] gsi_desc_type_o,
    output logic [15:0] gsi_desc_index_o,
    output logic  [3:0] gsi_sel_o,
    output logic  [7:0] gsi_ord_o,
    //! the proposed format under a SET_STREAM_FORMAT: the harness integrator
    //! model reads it to answer the kind-0 selector-15 verdict
    output logic [63:0] gsi_prop_fmt_o,
    input  wire  [63:0] gsi_data_i,
    input  wire         gsi_wait_i,
    input  wire         gsi_avb_chg_i,
    input  wire         gsi_asp_chg_i,
    output logic [255:0] srp_acc_latency_o, //! real per-sink latency for the harness integrator

    // descriptor-image memory master (07 §3.3) — the C++ harness plays a
    // latency-injecting DRAM behind it
    output logic        desc_mem_debt_o,
    output logic        desc_mem_req_valid_o,
    input  wire         desc_mem_req_ready_i,
    output logic [31:0] desc_mem_req_addr_o,
    output logic  [8:0] desc_mem_req_beats_o,
    input  wire         desc_mem_rsp_valid_i,
    output logic        desc_mem_rsp_ready_o,
    input  wire  [63:0] desc_mem_rsp_data_i,
    input  wire         desc_mem_rsp_last_i,
    input  wire         desc_mem_rsp_err_i,

    // AECP response-buffer memory master (03 §7, read/write)
    output logic        resp_mem_req_valid_o,
    input  wire         resp_mem_req_ready_i,
    output logic [31:0] resp_mem_req_addr_o,
    output logic  [8:0] resp_mem_req_beats_o,
    input  wire         resp_mem_rsp_valid_i,
    output logic        resp_mem_rsp_ready_o,
    input  wire  [63:0] resp_mem_rsp_data_i,
    input  wire         resp_mem_rsp_last_i,
    input  wire         resp_mem_rsp_err_i,
    output logic        resp_mem_wr_valid_o,
    input  wire         resp_mem_wr_ready_i,
    output logic [31:0] resp_mem_wr_addr_o,
    output logic [63:0] resp_mem_wr_data_o,
    output logic  [7:0] resp_mem_wr_strb_o,
    input  wire         resp_mem_wr_done_i,
    input  wire         resp_mem_wr_err_i,

    // NVM restore + device face
    input  wire         restore_go_i,
    output logic        restore_busy_o,
    output logic        restore_done_o,
    output logic        restore_fail_o,
    output logic        restore_blank_o,
    output logic        restore_closed_o,
    output logic        restore_rb_o,
    output logic  [2:0] rs_cause_o,
    output logic  [1:0] restore_cause_o,
    output logic        nvm_alarm_o,
    output logic  [7:0] nvm_unflushed_o,
    output logic        d3_unflushed_o,
    output logic        nvm_dev_req_o,
    input  wire         nvm_dev_gnt_i,
    output logic [1:0]  nvm_dev_op_o,
    output logic [7:0]  nvm_dev_region_o,
    output logic [15:0] nvm_dev_offset_o,
    output logic [15:0] nvm_dev_len_o,
    output logic        nvm_dev_wvalid_o,
    input  wire         nvm_dev_wready_i,
    output logic [7:0]  nvm_dev_wdata_o,
    input  wire         nvm_dev_rvalid_i,
    input  wire  [7:0]  nvm_dev_rdata_i,
    output logic        nvm_dev_rready_o,
    input  wire         nvm_dev_busy_i,
    input  wire         nvm_dev_done_i,
    input  wire         nvm_dev_err_i,

    // side-port host face
    input  wire         host_req_valid_i,
    input  wire         host_we_i,
    input  wire  [19:0] host_addr_i,
    input  wire  [31:0] host_wdata_i,
    output logic [31:0] host_rdata_o,
    output logic        host_rvalid_o,
    output logic        host_err_o,

    // SRP service face
    input  wire         svc_valid_i,
    output logic        svc_ready_o,
    input  wire  [2:0]  svc_op_i,
    input  wire  [7:0]  svc_index_i,
    input  wire  [63:0] svc_stream_id_i,
    input  wire  [47:0] svc_da_i,
    input  wire  [11:0] svc_vid_i,
    input  wire  [15:0] svc_max_frame_i,
    input  wire  [1:0]  svc_lstn_state_i,
    output logic        svc_rsp_valid_o,
    output logic [1:0]  svc_rsp_status_o,
    output logic [31:0] svc_rsp_data_o,

    // maap face (02 §4.2): with cfg_maap_internal_i = 0 the C++ harness
    // plays the allocator, INCLUDING the "no allocator at all" wiring; with
    // 1 the internal KL_pp_maap engine answers and this group is quiesced
    output logic        maap_req_valid_o,
    input  wire         maap_req_ready_i,
    output logic        maap_req_release_o,
    output logic [2:0]  maap_req_src_o,
    input  wire         maap_rsp_valid_i,
    input  wire         maap_rsp_ok_i,
    input  wire  [47:0] maap_rsp_da_i,
    input  wire         maap_conflict_valid_i,
    input  wire  [2:0]  maap_conflict_src_i,
    output logic        maap_conflict_ack_o,

    // internal MAAP engine (11): quasi-static config + the published claim
    input  wire         cfg_maap_internal_i,
    input  wire  [7:0]  cfg_maap_count_i,
    input  wire  [15:0] cfg_maap_seed_offset_i,
    input  wire         cfg_maap_seed_valid_i,
    output logic [47:0] maap_addr_o,
    output logic        maap_addr_valid_o,
    output logic [1:0]  maap_state_o,
    output logic [7:0]  maap_conflicts_o,
    output logic [7:0]  maap_defends_o,

    // the per-source DA gate a fabric ANDs with its own stream enable
    output logic [7:0]  acmp_declaring_o,
    //! the published (debounced) binding view an integrator folds into
    //! GET_STREAM_INFO's BOUND/STREAMING_WAIT flags (06 F06.13)
    output logic [7:0]  acmp_bound_o,
    //! ...and the bound stream's identity a fabric arms its RX filter from
    //! (class-D, integrator guide section 8): talker entity_id, stream_id,
    //! destination MAC and VLAN per sink, 64/64/48/12 bits each. Section AC
    //! grades the A15 latch and the clear with the binding (issue #48)
    output logic [8*64-1:0] acmp_bound_eid_o,
    output logic [8*64-1:0] acmp_bound_sid_o,
    output logic [8*48-1:0] acmp_bound_dmac_o,
    output logic [8*12-1:0] acmp_bound_vlan_o,

    // the Class A Domain in force (class-D, F02.10), passed through by name:
    // these ports are where an integrator reads P-SRP-DOM-DEF-VID's effect
    output logic  [2:0] srp_class_a_prio_o,
    output logic [11:0] srp_class_a_vid_o,
    output logic        srp_domain_adopted_o,
    output logic        srp_domain_change_o,

    // observability
    output logic [31:0] dbg_now_ms_o,
    // suite taps (cross-module refs into the DUT; observe-only)
    output logic        dbg_acmp_valid_o,
    output logic [3:0]  dbg_acmp_msg_o,
    output logic        dbg_is_tkr_o,
    output logic        dbg_lstn_pop_o,
    output logic        dbg_lstn_busy_o,
    //! the binding manager's OWN terminal (KL_acmp_nvm_shadow restore_done_o),
    //! which the top's restore_done_o follows once the listener admission
    //! gate releases and the D3 walk is done
    output logic        dbg_walk_done_o,
    //! the listener admission gate's release (KL_pp_acmp_lsn_admit
    //! released_o), and the listener writing a preload record (X_PRELOAD)
    //! or raising its A4 discovery arm: section BW4 grades the top's
    //! restore_done_o and restore_busy_o against them every cycle
    output logic        dbg_lsn_released_o,
    output logic        dbg_lsn_preload_o,
    output logic        dbg_lsn_arm_o,
    output logic        dbg_evr_valid_o,
    output logic [4:0]  dbg_evr_src_o,
    output logic        dbg_evr_ack_o,
    //! the STREAM_IS_RUNNING predicate, OBSERVED not driven: the bind and the
    //! SRP declaration are made the real way (an ACMP BIND_RX and a talker
    //! declaration), and these two only let the bench prove the cause took
    //! effect before it grades the refusal. Forcing them would test the
    //! dispatch arm against a fiction.
    output logic        dbg_bound0_o,
    output logic        dbg_streaming0_o,
    output logic        dbg_trc_wr_o,
    output logic        dbg_adp_evt_o,
    output logic        dbg_evt_tk_v_o,
    output logic        dbg_evt_tk_rdy_o,
    output logic        dbg_img_valid_o,
    output logic  [3:0] dbg_img_fault_o,
    output logic [15:0] dbg_aecp_cmd_o,
    output logic [15:0] dbg_aecp_resp_o,
    output logic [15:0] dbg_aecp_drop_o,
    output logic  [2:0] dbg_resp_fault_o,
    output logic [15:0] dbg_resp_err_o,
    output logic [15:0] dbg_resp_lane_o,
    output logic  [2:0] dbg_ca_state_o,
    output logic        dbg_ca_cancel_o,
    output logic        dbg_txc_locked_o,
    output logic  [2:0] dbg_txs_free_o,
    output logic  [3:0] dbg_org_busy_o,
    output logic  [3:0] dbg_org_queue_o,
    output logic  [3:0] dbg_org_second_owner_o,
    output logic        dbg_txs_release_valid_o,
    //! the per-sink started/stopped view the FABRIC admission gate reads
    //! (Milan 5.3.8.7). It is exposed because a START/STOP_STREAMING that
    //! answers SUCCESS without moving this bit is the exact defect the
    //! command's response shape cannot show.
    output logic  [7:0] aecp_strm_started_o,
    //! the per-row settings faces, exposed for the same reason: a
    //! SET_STREAM_FORMAT / SET_STREAM_INFO that answers SUCCESS without
    //! moving its row (or raising its valid bit) is invisible to any
    //! response-shape check
    output logic [8*32-1:0] aecp_pt_offset_o,
    output logic  [7:0] aecp_pt_offset_v_o,
    output logic [8*64-1:0] aecp_fmt_in_o,
    output logic  [7:0] aecp_fmt_in_v_o,
    output logic [8*64-1:0] aecp_fmt_out_o,
    output logic  [7:0] aecp_fmt_out_v_o,
    //! CLOCK_DOMAIN 0's clock_source_index as the top exports it, the face
    //! the integrator decodes into a media-clock source (section D3C)
    output logic [15:0] aecp_clk_src_index_o,
    //! Processor event on clk_i: one cycle per accepted live 64-bit name
    //! lane at the write edge, no ready/ack; boot, unchanged, refused and
    //! pre-acceptance aborted writes remain silent.
    output logic        aecp_name_wr_o,
    //! Independent clk_i observation of the actual name RAM write enable
    //! while the store accepts live requests; excludes boot loading. Sample
    //! each write edge alongside aecp_name_wr_o, with no ready/ack.
    output logic        dbg_name_live_we_o,

    //! the SET_CLOCK_SOURCE refusal contract (06 section 6.4: nothing
    //! stored, marked or notified) has no wire shape: a write of the value
    //! the row already holds, an NVM mark and a notification enqueue all
    //! leave the response and GET as they were. These three observe the
    //! effects themselves: the dynamic store's accepted-write counter, the
    //! OP_NVM_MARK strobe and the OP_NOTIFY_ENQ strobe (06 section 8). The
    //! mark strobe and its code are TOP-LEVEL PORTS now (issue #90), so they
    //! are passed through by name and not peeked at inside the DUT.
    output logic [15:0] dbg_dyn_writes_o,
    output logic        aecp_nvm_stb_o,
    output logic  [7:0] aecp_nvm_mark_o,
    output logic        dbg_notify_enq_o,

    //! the D3 writer inside the AECP engine (KL_aecp_nvm_writer), observed
    //! for section D3: its ownership of the state bus and dispatch, its
    //! restore verdicts, and whether the engine has a command in flight
    output logic        dbg_d3_own_o,
    output logic        dbg_d3_done_o,
    output logic        dbg_d3_fail_o,
    output logic        dbg_d3_closed_o,
    output logic  [2:0] dbg_d3_cause_o,
    output logic        dbg_aecp_busy_o,
    //! the AECP dispatch queue's head is present (before the scoreboard
    //! admission that aecp_txn_valid_o is gated by)
    output logic        dbg_aecp_head_o,
    //! the arbiter's manager-1 grant and done (the D3 writer's own), and
    //! the writer's per-record dirty vector (59 records at this shape: the
    //! 27 dynamic-state rows, then the 32 name-table entries)
    output logic        dbg_d3_mgnt_o,
    output logic        dbg_d3_mdone_o,
    output logic        dbg_d3_merr_o,
    output logic [58:0] dbg_d3_dirty_o,
    //! the writer is latching a record over the state bus in service
    output logic        dbg_d3_latch_o,
    //! the dynamic-state rows the fabric does not publish with their valid
    //! flag (configuration, sampling rate and clock source, row 0), for the
    //! D3 cleared-first and restored checks; and the writer's pass-1 counts
    output logic [15:0] dbg_dyn_cfg_o,
    output logic        dbg_dyn_cfg_v_o,
    //! the configuration row's valid flag as the AECP engine publishes it
    //! (dyn_cur_config_v_o), the one the ADPDU's index selection reads, for
    //! section AD's every-cycle comparison with the store's own above
    output logic        dbg_adp_cfg_v_o,
    output logic [31:0] dbg_dyn_rate_o,
    output logic        dbg_dyn_rate_v_o,
    output logic [15:0] dbg_dyn_clk_o,
    output logic        dbg_dyn_clk_v_o,
    output logic  [7:0] dbg_d3_applied_o,
    output logic  [7:0] dbg_d3_refused_o,
    output logic  [7:0] dbg_d3_blank_o,
    //! one 64-bit lane of the descriptor store's name table, read without the
    //! bus the restore owns: `dbg_name_lane_i` is the lane, entry * 8 + k
    input  wire   [7:0] dbg_name_lane_i,
    output logic [63:0] dbg_name_o,
    //! the ADP engine's enable input: the requested enable once the
    //! restore released it
    output logic        dbg_adp_enable_o,
    //! the D3 roll-back strobe to both stores
    output logic        dbg_d3_rb_rst_o,
    //! the volatile set D3R1 populates before a saved-set cycle: the ENTITY
    //! lock and the IDENTIFY value, the top's own outputs
    output logic        dbg_lock_held_o,
    output logic  [7:0] dbg_identify_o,
    //! the two restore walks' no-progress counters (the D3 writer's wd_r,
    //! the binding manager's rs_wd_r): the DR3a measurement reads their
    //! maxima, the longest single wait each walk met
    output logic [31:0] dbg_d3_wd_o,
    output logic [31:0] dbg_bind_wd_o,
    //! the NVM arbiter is draining an abandoned READ (a restore deadline's)
    output logic        dbg_nvm_drain_o,
    //! the binding manager's one-cycle READ strobe and its abort (manager 0
    //! of the arbiter), the arbiter's owner (0 none, 1 manager 0, 2 manager
    //! 1) and the port's busy: D3R18 lands the strobe on agg_o's first clock
    output logic        dbg_bind_req_o,
    output logic        dbg_bind_abort_o,
    output logic  [1:0] dbg_nvm_own_o,
    output logic        dbg_nvm_busy_o,
    //! the binding manager holds a restore byte (its nvm_rvalid_i), and the
    //! D3 walk proves the image this cycle (the writer's proof_w): D3R19 to
    //! D3R21 place them against the aggregate bound
    output logic        dbg_bind_rvalid_o,
    output logic        dbg_d3_proof_o,
    //! the D3 writer's aggregate count (agg_r, 0 in the accepted start's own
    //! cycle) and its fired level (agg_o): a case that must land an event on
    //! the bound's own cycle reads them to prove it did
    output logic [31:0] dbg_d3_agg_o,
    output logic        dbg_d3_agg_fired_o,
    //! the IDENT-BURST singleton (owner PP_OWN_IDENT_C) on the shared timer
    //! service's arm and expiry buses: its arm with the absolute ms deadline,
    //! and its expiry, which ends the T-IDENT-BURST gap after a frame. Section
    //! ID times a press against the gap's end with them; never set in a
    //! build whose top has no identify sequencer
    output logic        dbg_ident_gap_arm_o,
    output logic [31:0] dbg_ident_gap_deadline_o,
    output logic        dbg_ident_gap_end_o,
    //! the AECP transaction deadline (section DL, 03 §6 rule (e)): the top's
    //! kill of the AECP hold, the engine's solicited-response hand-off, the
    //! scoreboard's honoured kill, its normal release port, the AECP hold's id
    //! and the live-hold mask, and the µCPU's redirect level
    output logic        dbg_aecp_dl_kill_o,
    output logic        dbg_aecp_dl_queued_o,
    output logic        dbg_sb_kill_ack_o,
    output logic        dbg_sb_rel_o,
    output logic  [2:0] dbg_sb_rel_id_o,
    output logic  [2:0] dbg_aecp_sb_id_o,
    output logic  [7:0] dbg_sb_holds_o,
    output logic        dbg_ucpu_pre_o,
    //! the scoreboard's admission port (section HZ, 03 §6 F03.7): the class
    //! and key presented, the AECP and ACMP heads it accepted this clock, the
    //! head it was asked about and refused this clock, the two owners' live
    //! holds, and the pending CFG_BARRIER drain
    output logic  [3:0] dbg_sb_class_o,
    output logic [15:0] dbg_sb_key_o,
    output logic        dbg_sb_acc_aecp_o,
    output logic        dbg_sb_acc_acmp_o,
    output logic        dbg_sb_ref_aecp_o,
    output logic        dbg_sb_ref_acmp_o,
    output logic        dbg_aecp_sb_active_o,
    output logic        dbg_acmp_sb_active_o,
    output logic        dbg_sb_barrier_o,
    //! section AX: the AECP engine's TX-slot grant, the slot it names and the
    //! engine's own Delta-8 oversize request beside it; the pool's serializer
    //! start and the slot it streams; and slot 4 (the oversize slot) FREE, so
    //! an oversize response is seen to leave through slot 4 and free it
    output logic        dbg_aecp_txs_gnt_o,
    output logic  [2:0] dbg_aecp_txs_slot_o,
    output logic        dbg_aecp_txs_ovs_o,
    output logic        dbg_ser_start_o,
    output logic  [2:0] dbg_ser_slot_o,
    output logic        dbg_txs_slot4_free_o,
    //! section AQ (issue #639): the eight engine arm faces as the top's
    //! timer arm-port mux receives them, in its drain order (listener,
    //! talker, ADP, SRP, originator, MAAP, notify, notify monitor), each
    //! {cancel, slot, owner, deadline} zero-extended to 64 bits; the arm port
    //! the timer service sees, packed the same way; and the arm-drop counter
    //! (snapshot word 24, bits 31:16). Read from the faces' own nets, so the
    //! bench's model checks the mux from the engines to the timer service
    output logic  [7:0] dbg_aq_vld_o,
    output logic [511:0] dbg_aq_arm_o,
    output logic        dbg_aq_port_valid_o,
    output logic [63:0] dbg_aq_port_o,
    output logic [15:0] dbg_aq_drop_o,
    //! section AQ's drive (issue #639): from the clock `dbg_aq_drive_i`
    //! rises, the eight faces carry these arms instead of the engines' (per
    //! face, in drain order: valid, cancel, slot cut to the top's slot width,
    //! owner, deadline) and the timer service's arm port is held idle, so no
    //! arm the drive queues reaches an engine. Raised once, by a run's last
    //! section, and never lowered: the wrap releases nothing
    input  wire         dbg_aq_drive_i,
    input  wire   [7:0] dbg_aq_drv_vld_i,
    input  wire   [7:0] dbg_aq_drv_cancel_i,
    input  wire   [7:0][7:0] dbg_aq_drv_slot_i,
    input  wire   [7:0][7:0] dbg_aq_drv_owner_i,
    input  wire   [7:0][31:0] dbg_aq_drv_deadline_i,
    //! section AX: the DESC_LINE_BYTES_P the top elaborated, in bytes, so the
    //! bench bounds response writes by the reservation (16 + it) the top
    //! really has, the default in the first build and the line build's own
    output logic [15:0] dbg_desc_line_bytes_o,
    //! section IF: each interface's advertise machine, interface 0 in bits
    //! 1:0 (the top's adp_dbg_adv_state_w, 2 bits wide but in the seventh build)
    output logic  [3:0] dbg_adp_adv_state_o
);

`ifdef PP_TOP_TIM_REAL
  // section TB's build (the Makefile's fifth): 1 ms = 1 x 1000 = 1,000 clk,
  // the nominal TB_CLK_HZ_C's own rate, so no response budget measured there
  // is cut short by the compressed AECP deadline
  localparam int unsigned TB_DIV_US_C = 1;
  localparam int unsigned TB_DIV_MS_C = 1000;
`else
  // 1 ms = 2 x 50 = 100 clk; the 91-slot sweep (93 cycles) fits inside
  localparam int unsigned TB_DIV_US_C = 2;
  localparam int unsigned TB_DIV_MS_C = 50;
`endif
  //! the nominal P-CLK-HZ the saved-state times are DERIVED from (the
  //! prescaler above is overridden, so the timebase does not use it). A
  //! 1 MHz clock, the parent D3 model's, makes the ratified 20 ms per-wait
  //! deadline about 20,000 clocks and the 1,000 ms aggregate 50 of them;
  //! the extra hertz is odd on purpose: ceil(CLK_HZ_P x t / 1000) then
  //! differs by one clock from a floor or from a count of 1 ms ticks, so
  //! the sections that time them grade the top's own derivation
  localparam int unsigned TB_CLK_HZ_C = 1_000_001;

  // talker source shape (see banner)
  logic [7:0]       cfg_src_en_w;
  logic [15:0]      cfg_src_iface_w;
  logic [8*64-1:0]  cfg_stream_id_w;

  assign cfg_src_en_w    = 8'hFF;
  assign cfg_src_iface_w = 16'd0;
  for (genvar g = 0; g < 8; g++) begin : g_sid
    assign cfg_stream_id_w[g*64 +: 64] = {own_mac_i, 16'(g)};
  end

  // AECP pop face: record lane observed, payload faces DEFINED-idle
  logic [pp_pkg::PP_TXN_W_C-1:0] aecp_txn_nc_w;
  pp_pkg::pp_txn_t aecp_txn_rec_w;
  assign aecp_txn_rec_w  = pp_pkg::pp_txn_t'(aecp_txn_nc_w);
  assign aecp_txn_slot_o = aecp_txn_rec_w.rx_slot;
  logic [7:0]                    aecp_rd_data_nc_w;
  logic [9:0]                    aecp_slot_len_nc_w;

  protocol_processor_top #(
`ifdef PP_TOP_IF2
      //! the seventh build: two AVB interfaces (see the banner)
      .N_AVB_IF_P        (2),
`endif
`ifdef PP_TOP_SRP_DOM_DEF_VID
      //! the second build's verification-only fixture (see the banner)
      .SRP_DOM_DEF_VID_P (`PP_TOP_SRP_DOM_DEF_VID),
`endif
`ifdef PP_TOP_EN_IDENT
      //! the third build: P-EN-IDENTIFY-NOTIFICATION = 1 (see the banner);
      //! every other build grades the top's own default, 0
      .EN_IDENTIFY_NOTIF_P (1'b1),
`endif
`ifdef PP_TOP_DESC_LINE_BYTES
      //! the fourth build's verification-only fixture (see the banner)
      .DESC_LINE_BYTES_P (`PP_TOP_DESC_LINE_BYTES),
`endif
      .CLK_HZ_P     (TB_CLK_HZ_C),
      .TIM_DIV_US_P (TB_DIV_US_C),
      .TIM_DIV_MS_P (TB_DIV_MS_C),
`ifndef PP_TOP_TIM_DEFAULTS
      //! TIM compression for the registration/lock deadlines, same reason
      //! as the DIV overrides: the suite must SEE IEEE 7.4.37.2's 300 s
      //! TIME_LIMITED expiry and 7.4.2's 60 s lock expiry, not wait 30
      //! million compressed cycles for them. The sixth build alone waits
      //! them out, at the top's defaults (section TD)
      .REG_TL_TIMEOUT_MS_P (400),
      .LOCK_TIMEOUT_MS_P   (400),
`endif
      //! NVM_RS_TMO_CYC_P, NVM_RS_AGG_CYC_P and NVM_RETRY_BACKOFF_CYC_P are
      //! NOT overridden: the top derives 20,001, 1,000,001 and 500,001
      //! clocks from TB_CLK_HZ_C, and sections D3R8, D3R13 and D3S10 time
      //! them
      .TROM_HEX_P   ("ltn_rom.hex"),
      .UCODE_HEX_P  ("ucode.hex")
  ) u_dut (
      .clk_i                 (clk_i),
      .rst_n                 (rst_n),
      .entity_id_i           (entity_id_i),
      .entity_model_id_i     (entity_model_id_i),
      .own_mac_i             (own_mac_i),
      .talker_sources_i      (talker_sources_i),
      .talker_caps_i         (talker_caps_i),
      .listener_sinks_i      (listener_sinks_i),
      .listener_caps_i       (listener_caps_i),
      .current_cfg_i         (current_cfg_i),
      .identify_index_i      (identify_index_i),
      .identify_button_i     (identify_button_i),
      .entity_enable_i       (entity_enable_i),
      .link_up_i             (link_up_i),
      .gm_change_i           (gm_change_i),
      .gm_id_i               (gm_id_i),
      .gptp_domain_i         (gptp_domain_i),
      .p2p_i                 (p2p_i),
      .cfg_rank_i            (cfg_rank_i),
      .cfg_acc_lat_ns_i      (cfg_acc_lat_ns_i),
      .port_rate_bps_i       (port_rate_bps_i),
      .cfg_tspec_max_frame_i (cfg_tspec_max_frame_i),
      .cfg_src_en_i          (cfg_src_en_w),
      .cfg_src_iface_i       (cfg_src_iface_w),
      .cfg_stream_id_i       (cfg_stream_id_w),
      .rx_valid_i            (rx_valid_i),
      .rx_data_i             (rx_data_i),
      .rx_last_i             (rx_last_i),
`ifdef PP_TOP_IF2
      .rx_if_index_i         (rx_if_index_i),
`endif
      .tx_valid_o            (tx_valid_o),
      .tx_sof_o              (tx_sof_o),
      .tx_data_o             (tx_data_o),
      .tx_eof_o              (tx_eof_o),
      .tx_ready_i            (tx_ready_i),
      .aecp_txn_valid_o      (aecp_txn_valid_o),
      .aecp_txn_o            (aecp_txn_nc_w),
      .aecp_txn_ready_i      (aecp_txn_ready_i),
      .aecp_rxs_rd_slot_i    (2'd0),
      .aecp_rxs_rd_addr_i    (10'd0),
      .aecp_rxs_rd_en_i      (1'b0),
      .aecp_rxs_rd_data_o    (aecp_rd_data_nc_w),
      .aecp_rxs_slot_len_o   (aecp_slot_len_nc_w),
      .aecp_rxs_free_i       (aecp_rxs_free_i),
      .aecp_rxs_free_slot_i  (aecp_rxs_free_slot_i),
      //! Dynamic state is verified through AECP response traffic here. Keep
      //! every unused publication explicit so newly added state cannot leave
      //! a silent harness integration gap.
      .aecp_cur_config_o     (),
      .aecp_identify_o       (dbg_identify_o),
      .aecp_clk_src_index_o  (aecp_clk_src_index_o),
      .aecp_strm_started_o   (aecp_strm_started_o),
      .aecp_pt_offset_o      (aecp_pt_offset_o),
      .aecp_pt_offset_v_o    (aecp_pt_offset_v_o),
      .aecp_fmt_in_o         (aecp_fmt_in_o),
      .aecp_fmt_in_v_o       (aecp_fmt_in_v_o),
      .aecp_fmt_out_o        (aecp_fmt_out_o),
      .aecp_fmt_out_v_o      (aecp_fmt_out_v_o),
      .aecp_dyn_dirty_o      (),
      .aecp_name_wr_o        (aecp_name_wr_o),
      .aecp_nvm_stb_o        (aecp_nvm_stb_o),
      .aecp_nvm_mark_o       (aecp_nvm_mark_o),
      .aecp_lock_held_o      (dbg_lock_held_o),
      .ctr_req_o             (ctr_req_o),
      .ctr_desc_type_o       (ctr_desc_type_o),
      .ctr_desc_index_o      (ctr_desc_index_o),
      .ctr_word_o            (ctr_word_o),
      .ctr_data_i            (ctr_data_i),
      .ctr_wait_i            (ctr_wait_i),
      .ctr_change_i          (ctr_change_i),
      .ctr_change_desc_type_i(ctr_change_desc_type_i),
      .ctr_change_desc_index_i(ctr_change_desc_index_i),
      .amap_req_o            (amap_req_o),
      .amap_desc_type_o      (amap_desc_type_o),
      .amap_desc_index_o     (amap_desc_index_o),
      .amap_map_index_o      (amap_map_index_o),
      .amap_sel_o            (amap_sel_o),
      .amap_rec_o            (amap_rec_o),
      .amap_data_i           (amap_data_i),
      .amap_wait_i           (amap_wait_i),
      .amap_edit_req_o       (amap_edit_req_o),
      .amap_edit_phase_o     (amap_edit_phase_o),
      .amap_edit_remove_o    (amap_edit_remove_o),
      .amap_edit_desc_type_o (amap_edit_desc_type_o),
      .amap_edit_desc_index_o(amap_edit_desc_index_o),
      .amap_edit_count_o     (amap_edit_count_o),
      .amap_edit_rec_o       (amap_edit_rec_o),
      .amap_edit_record_o    (amap_edit_record_o),
      .amap_edit_value_o     (amap_edit_value_o),
      .amap_edit_data_i      (amap_edit_data_i),
      .amap_edit_wait_i      (amap_edit_wait_i),
      .gsi_req_o             (gsi_req_o),
      .gsi_kind_o            (gsi_kind_o),
      .gsi_desc_type_o       (gsi_desc_type_o),
      .gsi_desc_index_o      (gsi_desc_index_o),
      .gsi_sel_o             (gsi_sel_o),
      .gsi_ord_o             (gsi_ord_o),
      .gsi_prop_fmt_o        (gsi_prop_fmt_o),
      .gsi_data_i            (gsi_data_i),
      .gsi_wait_i            (gsi_wait_i),
      .gsi_avb_chg_i         (gsi_avb_chg_i),
      .gsi_asp_chg_i         (gsi_asp_chg_i),
      .desc_mem_req_valid_o  (desc_mem_req_valid_o),
      .desc_mem_req_ready_i  (desc_mem_req_ready_i),
      .desc_mem_req_addr_o   (desc_mem_req_addr_o),
      .desc_mem_req_beats_o  (desc_mem_req_beats_o),
      .desc_mem_rsp_valid_i  (desc_mem_rsp_valid_i),
      .desc_mem_rsp_ready_o  (desc_mem_rsp_ready_o),
      .desc_mem_rsp_data_i   (desc_mem_rsp_data_i),
      .desc_mem_rsp_last_i   (desc_mem_rsp_last_i),
      .desc_mem_rsp_err_i    (desc_mem_rsp_err_i),
      .resp_mem_req_valid_o  (resp_mem_req_valid_o),
      .resp_mem_req_ready_i  (resp_mem_req_ready_i),
      .resp_mem_req_addr_o   (resp_mem_req_addr_o),
      .resp_mem_req_beats_o  (resp_mem_req_beats_o),
      .resp_mem_rsp_valid_i  (resp_mem_rsp_valid_i),
      .resp_mem_rsp_ready_o  (resp_mem_rsp_ready_o),
      .resp_mem_rsp_data_i   (resp_mem_rsp_data_i),
      .resp_mem_rsp_last_i   (resp_mem_rsp_last_i),
      .resp_mem_rsp_err_i    (resp_mem_rsp_err_i),
      .resp_mem_wr_valid_o   (resp_mem_wr_valid_o),
      .resp_mem_wr_ready_i   (resp_mem_wr_ready_i),
      .resp_mem_wr_addr_o    (resp_mem_wr_addr_o),
      .resp_mem_wr_data_o    (resp_mem_wr_data_o),
      .resp_mem_wr_strb_o    (resp_mem_wr_strb_o),
      .resp_mem_wr_done_i    (resp_mem_wr_done_i),
      .resp_mem_wr_err_i     (resp_mem_wr_err_i),
      .restore_go_i          (restore_go_i),
      .restore_busy_o        (restore_busy_o),
      .restore_done_o        (restore_done_o),
      .restore_fail_o        (restore_fail_o),
      .restore_blank_o       (restore_blank_o),
      .restore_closed_o      (restore_closed_o),
      .restore_rb_o          (restore_rb_o),
      .rs_cause_o            (rs_cause_o),
      .restore_cause_o       (restore_cause_o),
      .nvm_alarm_o           (nvm_alarm_o),
      .nvm_unflushed_o       (nvm_unflushed_o),
      .d3_unflushed_o        (d3_unflushed_o),
      .nvm_dev_req_o         (nvm_dev_req_o),
      .nvm_dev_gnt_i         (nvm_dev_gnt_i),
      .nvm_dev_op_o          (nvm_dev_op_o),
      .nvm_dev_region_o      (nvm_dev_region_o),
      .nvm_dev_offset_o      (nvm_dev_offset_o),
      .nvm_dev_len_o         (nvm_dev_len_o),
      .nvm_dev_wvalid_o      (nvm_dev_wvalid_o),
      .nvm_dev_wready_i      (nvm_dev_wready_i),
      .nvm_dev_wdata_o       (nvm_dev_wdata_o),
      .nvm_dev_rvalid_i      (nvm_dev_rvalid_i),
      .nvm_dev_rdata_i       (nvm_dev_rdata_i),
      .nvm_dev_rready_o      (nvm_dev_rready_o),
      .nvm_dev_busy_i        (nvm_dev_busy_i),
      .nvm_dev_done_i        (nvm_dev_done_i),
      .nvm_dev_err_i         (nvm_dev_err_i),
      .host_req_valid_i      (host_req_valid_i),
      .host_we_i             (host_we_i),
      .host_addr_i           (host_addr_i),
      .host_wdata_i          (host_wdata_i),
      .host_rdata_o          (host_rdata_o),
      .host_rvalid_o         (host_rvalid_o),
      .host_err_o            (host_err_o),
      .svc_valid_i           (svc_valid_i),
      .svc_ready_o           (svc_ready_o),
      .svc_op_i              (svc_op_i),
      .svc_index_i           (svc_index_i),
      .svc_stream_id_i       (svc_stream_id_i),
      .svc_da_i              (svc_da_i),
      .svc_vid_i             (svc_vid_i),
      .svc_max_frame_i       (svc_max_frame_i),
      .svc_lstn_state_i      (svc_lstn_state_i),
      .svc_rsp_valid_o       (svc_rsp_valid_o),
      .svc_rsp_status_o      (svc_rsp_status_o),
      .svc_rsp_data_o        (svc_rsp_data_o),
      .srp_acc_latency_o     (srp_acc_latency_o),
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
      .cfg_maap_internal_i   (cfg_maap_internal_i),
      .cfg_maap_count_i      (cfg_maap_count_i),
      .cfg_maap_seed_offset_i (cfg_maap_seed_offset_i),
      .cfg_maap_seed_valid_i (cfg_maap_seed_valid_i),
      .maap_addr_o           (maap_addr_o),
      .maap_addr_valid_o     (maap_addr_valid_o),
      .maap_state_o          (maap_state_o),
      .maap_conflicts_o      (maap_conflicts_o),
      .maap_defends_o        (maap_defends_o),
      .acmp_declaring_o      (acmp_declaring_o),
      .acmp_bound_o          (acmp_bound_o),
      .acmp_bound_eid_o      (acmp_bound_eid_o),
      .acmp_bound_sid_o      (acmp_bound_sid_o),
      .acmp_bound_dmac_o     (acmp_bound_dmac_o),
      .acmp_bound_vlan_o     (acmp_bound_vlan_o),
      .srp_class_a_prio_o    (srp_class_a_prio_o),
      .srp_class_a_vid_o     (srp_class_a_vid_o),
      .srp_domain_adopted_o  (srp_domain_adopted_o),
      .srp_domain_change_o   (srp_domain_change_o),
      .dbg_now_ms_o          (dbg_now_ms_o)
  );

  assign dbg_acmp_valid_o = u_dut.acmp_txn_valid_w;
  assign dbg_acmp_msg_o   = u_dut.acmp_head_w.msg_type;
  assign dbg_is_tkr_o     = u_dut.acmp_is_tkr_w;
  assign dbg_lstn_pop_o   = u_dut.lstn_txn_ready_w;
  assign dbg_lstn_busy_o  = u_dut.lstn_dbg_busy_w;
  assign dbg_walk_done_o  = u_dut.nvm_walk_done_w;
  assign dbg_lsn_released_o = u_dut.lsn_released_w;
  assign dbg_lsn_preload_o  = (5'(u_dut.u_listener.xs_r) == 5'd2);   // X_PRELOAD
  assign dbg_lsn_arm_o      = u_dut.lstn_disc_arm_w;
  assign dbg_evr_valid_o  = u_dut.evr_valid_w;
  assign dbg_evr_src_o    = u_dut.evr_src_w;
  assign dbg_evr_ack_o    = u_dut.evr_ack_w;
  assign dbg_bound0_o     = u_dut.bound_hold_r[0];
  assign dbg_streaming0_o = u_dut.aecp_streaming_w[0];
  assign dbg_trc_wr_o     = u_dut.trc_wr_valid_w;
  assign dbg_adp_evt_o    = u_dut.adp_evt_valid_w;
  assign dbg_evt_tk_v_o   = u_dut.lstn_evt_tk_valid_w;
  assign dbg_evt_tk_rdy_o = u_dut.lstn_evt_tk_ready_w;
  assign dbg_img_valid_o  = u_dut.aecp_dbg_img_valid_w;
  // Observe the D3 guard interface without extending the product top ports.
  assign desc_mem_debt_o = u_dut.u_desc_mem_guard.debt_o;
  assign dbg_img_fault_o  = u_dut.aecp_dbg_fault_w;
  assign dbg_aecp_cmd_o   = u_dut.aecp_dbg_cmd_w;
  assign dbg_aecp_resp_o  = u_dut.aecp_dbg_resp_w;
  assign dbg_aecp_drop_o  = u_dut.aecp_dbg_drop_w;
  assign dbg_resp_fault_o = u_dut.aecp_dbg_rfault_w;
  assign dbg_resp_err_o   = u_dut.aecp_dbg_rerr_w;
  assign dbg_resp_lane_o  = u_dut.aecp_dbg_rlane_w;
  assign dbg_ca_state_o   = u_dut.u_ca_builder.c_st_r;
  assign dbg_ca_cancel_o  = u_dut.ntfy_ca_cancel_valid_w;
  assign dbg_txc_locked_o = u_dut.txc_locked_r;
  assign dbg_txs_free_o   = u_dut.txs_free_w;
  assign dbg_org_busy_o   = u_dut.org_busy_nc_w;
  assign dbg_org_queue_o  = u_dut.laneq_org_cnt_r;
  assign dbg_txs_release_valid_o = u_dut.txs_release_valid_w;
  assign dbg_dyn_writes_o = u_dut.u_aecp.dyn_writes_nc_w;
  assign dbg_name_live_we_o = u_dut.u_aecp.u_store.name_we_w
                              && u_dut.u_aecp.u_store.st_ready_o;
  assign dbg_notify_enq_o = u_dut.aecp_eff_notify_stb_nc_w;
  assign dbg_d3_own_o     = u_dut.u_aecp.d3_own_w;
  assign dbg_d3_done_o    = u_dut.d3_done_w;
  assign dbg_d3_fail_o    = u_dut.d3_fail_w;
  assign dbg_d3_closed_o  = u_dut.d3_closed_w;
  assign dbg_d3_cause_o   = u_dut.rs_cause_o;
  assign dbg_aecp_busy_o  = u_dut.aecp_dbg_busy_nc_w;
  assign dbg_aecp_head_o  = u_dut.aecp_txn_valid_w;
  assign dbg_d3_mgnt_o    = u_dut.d3_m_gnt_w;
  assign dbg_d3_mdone_o   = u_dut.d3_m_done_w;
  assign dbg_d3_merr_o    = u_dut.d3_m_err_w;
  assign dbg_d3_dirty_o   = u_dut.u_aecp.u_d3.dirty_r;
  assign dbg_d3_latch_o   = u_dut.u_aecp.u_d3.latch_w;
  assign dbg_dyn_cfg_o    = u_dut.u_aecp.u_dyn.cfg_r;
  assign dbg_dyn_cfg_v_o  = u_dut.u_aecp.u_dyn.cfg_v_r;
  assign dbg_adp_cfg_v_o  = u_dut.aecp_cur_cfg_v_w;
  assign dbg_dyn_rate_o   = u_dut.u_aecp.u_dyn.rate_r[0];
  assign dbg_dyn_rate_v_o = u_dut.u_aecp.u_dyn.rate_v_r[0];
  assign dbg_dyn_clk_o    = u_dut.u_aecp.u_dyn.clksrc_r[0];
  assign dbg_dyn_clk_v_o  = u_dut.u_aecp.u_dyn.clksrc_v_r[0];
  assign dbg_d3_applied_o = u_dut.u_aecp.u_d3.n_app_r;
  assign dbg_d3_refused_o = u_dut.u_aecp.u_d3.n_ref_r;
  assign dbg_d3_blank_o   = u_dut.u_aecp.u_d3.n_blank_r;
  assign dbg_name_o       = u_dut.u_aecp.u_store.name_r[dbg_name_lane_i];
  assign dbg_adp_enable_o = u_dut.u_adp.entity_enable_i;
  assign dbg_d3_rb_rst_o  = u_dut.u_aecp.d3_rb_rst_w;
  assign dbg_d3_wd_o      = u_dut.u_aecp.u_d3.wd_r;
  assign dbg_bind_wd_o    = u_dut.u_nvm_shadow.rs_wd_r;
  assign dbg_nvm_drain_o  = u_dut.nvm_drain_nc_w;
  assign dbg_bind_req_o   = u_dut.nvm_req_w;
  assign dbg_bind_abort_o = u_dut.nvm_abort_w;
  assign dbg_nvm_own_o    = 2'(u_dut.u_nvm_arb.own_r);
  assign dbg_nvm_busy_o   = u_dut.np_busy_w;
  assign dbg_bind_rvalid_o = u_dut.nvm_rvalid_w;
  assign dbg_d3_proof_o   = u_dut.u_aecp.u_d3.proof_w;
  assign dbg_d3_agg_o     = u_dut.u_aecp.u_d3.agg_r;
  assign dbg_d3_agg_fired_o = u_dut.d3_agg_w;
  assign dbg_ident_gap_arm_o = u_dut.tmr_arm_valid_w && !u_dut.tmr_arm_cancel_w
                               && (u_dut.tmr_arm_owner_w == pp_pkg::PP_OWN_IDENT_C);
  assign dbg_ident_gap_deadline_o = u_dut.tmr_arm_deadline_w;
  assign dbg_ident_gap_end_o = u_dut.exp_valid_w
                               && (u_dut.exp_owner_w == pp_pkg::PP_OWN_IDENT_C);
  assign dbg_aecp_dl_kill_o   = u_dut.aecp_dl_kill_w;
  assign dbg_aecp_dl_queued_o = u_dut.aecp_dl_queued_w;
  assign dbg_sb_kill_ack_o    = u_dut.sb_kill_ack_w;
  assign dbg_sb_rel_o         = u_dut.sb_rel_valid_w;
  assign dbg_sb_rel_id_o      = u_dut.sb_rel_id_w;
  assign dbg_aecp_sb_id_o     = u_dut.aecp_sb_id_r;
  assign dbg_sb_holds_o       = u_dut.sb_holds_w;
  assign dbg_ucpu_pre_o       = u_dut.u_aecp.ucpu_pre_w;
  assign dbg_sb_class_o       = u_dut.sb_adm_class_w;
  assign dbg_sb_key_o         = u_dut.sb_adm_key_w;
  assign dbg_sb_acc_aecp_o    = u_dut.aecp_sb_accept_w;
  assign dbg_sb_acc_acmp_o    = u_dut.acmp_sb_accept_w;
  assign dbg_sb_ref_aecp_o    = u_dut.sb_pick_aecp_w && !u_dut.sb_gnt_w;
  assign dbg_sb_ref_acmp_o    = u_dut.sb_pick_acmp_w && !u_dut.sb_gnt_w;
  assign dbg_aecp_sb_active_o = u_dut.aecp_sb_active_r;
  assign dbg_acmp_sb_active_o = u_dut.acmp_sb_active_r;
  assign dbg_sb_barrier_o     = u_dut.sb_barrier_w;
  assign dbg_aecp_txs_gnt_o  = u_dut.aecp_txs_gnt_w;
  assign dbg_aecp_txs_slot_o = 3'(u_dut.aecp_txs_gnt_slot_w);
  assign dbg_aecp_txs_ovs_o  = u_dut.aecp_txs_oversize_w;
  assign dbg_ser_start_o     = u_dut.u_tx_slots.ser_start_w;
  assign dbg_ser_slot_o      = 3'(u_dut.ser_slot_w);
  assign dbg_txs_slot4_free_o = (u_dut.u_tx_slots.st_r[4] == 2'd0);
  assign dbg_desc_line_bytes_o = 16'(u_dut.DESC_LINE_BYTES_P);
  assign dbg_adp_adv_state_o   = 4'(u_dut.adp_dbg_adv_state_w);
  assign dbg_aq_vld_o = {u_dut.ntfy_mon_arm_valid_w, u_dut.ntfy_arm_valid_w,
                         u_dut.maapeng_arm_valid_w, u_dut.org_arm_valid_w,
                         u_dut.srp_arm_valid_w, u_dut.adp_arm_valid_w,
                         u_dut.tkr_arm_valid_w, u_dut.lstn_arm_valid_w};
  assign dbg_aq_arm_o = {
      64'({u_dut.ntfy_mon_arm_cancel_w, u_dut.ntfy_mon_arm_slot_w,
           u_dut.ntfy_mon_arm_owner_w, u_dut.ntfy_mon_arm_deadline_w}),
      64'({u_dut.ntfy_arm_cancel_w, u_dut.ntfy_arm_slot_w,
           u_dut.ntfy_arm_owner_w, u_dut.ntfy_arm_deadline_w}),
      64'({u_dut.maapeng_arm_cancel_w, u_dut.maapeng_arm_slot_w,
           u_dut.maapeng_arm_owner_w, u_dut.maapeng_arm_deadline_w}),
      64'({u_dut.org_arm_cancel_w, u_dut.org_arm_slot_w,
           u_dut.org_arm_owner_w, u_dut.org_arm_deadline_w}),
      64'({u_dut.srp_arm_cancel_w, u_dut.srp_arm_slot_w,
           u_dut.srp_arm_owner_w, u_dut.srp_arm_deadline_w}),
      64'({u_dut.adp_arm_cancel_w, u_dut.adp_arm_slot_w,
           u_dut.adp_arm_owner_w, u_dut.adp_arm_deadline_w}),
      64'({u_dut.tkr_arm_cancel_w, u_dut.tkr_arm_slot_w,
           u_dut.tkr_arm_owner_w, u_dut.tkr_arm_deadline_w}),
      64'({u_dut.lstn_arm_cancel_w, u_dut.lstn_arm_slot_w,
           u_dut.lstn_arm_owner_w, u_dut.lstn_arm_deadline_w})};
  assign dbg_aq_port_valid_o = u_dut.tmr_arm_valid_w;
  assign dbg_aq_port_o = 64'({u_dut.tmr_arm_cancel_w, u_dut.tmr_arm_slot_w,
                              u_dut.tmr_arm_owner_w, u_dut.tmr_arm_deadline_w});
  assign dbg_aq_drop_o = u_dut.arm_drop_r;
  // section AQ's drive: forced on the faces' own nets, so the mux and the
  // taps above both read the bench's arms; the timer is held idle on its
  // own input port, so the arm port the taps read still shows the mux
  always @(posedge dbg_aq_drive_i) begin : aq_drive
    force u_dut.lstn_arm_valid_w        = dbg_aq_drv_vld_i[0];
    force u_dut.lstn_arm_cancel_w       = dbg_aq_drv_cancel_i[0];
    force u_dut.lstn_arm_slot_w         = dbg_aq_drv_slot_i[0];
    force u_dut.lstn_arm_owner_w        = dbg_aq_drv_owner_i[0];
    force u_dut.lstn_arm_deadline_w     = dbg_aq_drv_deadline_i[0];
    force u_dut.tkr_arm_valid_w         = dbg_aq_drv_vld_i[1];
    force u_dut.tkr_arm_cancel_w        = dbg_aq_drv_cancel_i[1];
    force u_dut.tkr_arm_slot_w          = dbg_aq_drv_slot_i[1];
    force u_dut.tkr_arm_owner_w         = dbg_aq_drv_owner_i[1];
    force u_dut.tkr_arm_deadline_w      = dbg_aq_drv_deadline_i[1];
    force u_dut.adp_arm_valid_w         = dbg_aq_drv_vld_i[2];
    force u_dut.adp_arm_cancel_w        = dbg_aq_drv_cancel_i[2];
    force u_dut.adp_arm_slot_w          = dbg_aq_drv_slot_i[2];
    force u_dut.adp_arm_owner_w         = dbg_aq_drv_owner_i[2];
    force u_dut.adp_arm_deadline_w      = dbg_aq_drv_deadline_i[2];
    force u_dut.srp_arm_valid_w         = dbg_aq_drv_vld_i[3];
    force u_dut.srp_arm_cancel_w        = dbg_aq_drv_cancel_i[3];
    force u_dut.srp_arm_slot_w          = dbg_aq_drv_slot_i[3];
    force u_dut.srp_arm_owner_w         = dbg_aq_drv_owner_i[3];
    force u_dut.srp_arm_deadline_w      = dbg_aq_drv_deadline_i[3];
    force u_dut.org_arm_valid_w         = dbg_aq_drv_vld_i[4];
    force u_dut.org_arm_cancel_w        = dbg_aq_drv_cancel_i[4];
    force u_dut.org_arm_slot_w          = dbg_aq_drv_slot_i[4];
    force u_dut.org_arm_owner_w         = dbg_aq_drv_owner_i[4];
    force u_dut.org_arm_deadline_w      = dbg_aq_drv_deadline_i[4];
    force u_dut.maapeng_arm_valid_w     = dbg_aq_drv_vld_i[5];
    force u_dut.maapeng_arm_cancel_w    = dbg_aq_drv_cancel_i[5];
    force u_dut.maapeng_arm_slot_w      = dbg_aq_drv_slot_i[5];
    force u_dut.maapeng_arm_owner_w     = dbg_aq_drv_owner_i[5];
    force u_dut.maapeng_arm_deadline_w  = dbg_aq_drv_deadline_i[5];
    force u_dut.ntfy_arm_valid_w        = dbg_aq_drv_vld_i[6];
    force u_dut.ntfy_arm_cancel_w       = dbg_aq_drv_cancel_i[6];
    force u_dut.ntfy_arm_slot_w         = dbg_aq_drv_slot_i[6];
    force u_dut.ntfy_arm_owner_w        = dbg_aq_drv_owner_i[6];
    force u_dut.ntfy_arm_deadline_w     = dbg_aq_drv_deadline_i[6];
    force u_dut.ntfy_mon_arm_valid_w    = dbg_aq_drv_vld_i[7];
    force u_dut.ntfy_mon_arm_cancel_w   = dbg_aq_drv_cancel_i[7];
    force u_dut.ntfy_mon_arm_slot_w     = dbg_aq_drv_slot_i[7];
    force u_dut.ntfy_mon_arm_owner_w    = dbg_aq_drv_owner_i[7];
    force u_dut.ntfy_mon_arm_deadline_w = dbg_aq_drv_deadline_i[7];
    force u_dut.u_timer.arm_valid_i     = 1'b0;
  end
  always_comb begin : second_originator_owner
    dbg_org_second_owner_o = 4'hF;
    if (u_dut.laneq_org_cnt_r > 4'd1) begin
      for (int unsigned i = 0; i < 16; i++) begin
        if (u_dut.u_originator.valid_r[i]
            && (u_dut.u_originator.txs_r[i] == u_dut.laneq_org_r[1])) begin
          dbg_org_second_owner_o = u_dut.u_originator.owner_r[i];
        end
      end
    end
  end

endmodule : pp_top_wrap
`default_nettype wire
