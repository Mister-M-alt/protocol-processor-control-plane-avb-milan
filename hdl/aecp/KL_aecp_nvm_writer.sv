/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : KL_aecp_nvm_writer.sv
//  Project     : IEEE 1722.1 protocol processor (docs/architecture/07 §5.2
//                F07.8 record framing, §5.3 F07.9 commit and boot restore;
//                02 §8 F02.8 class-F manager face; 08 §2 T-NVM-DEBOUNCE;
//                parent D3 contract, milan-fpga
//                docs/design/SAVED_STATE_MATERIALIZATION.md §3, §3.1, §6,
//                §7; Milan §5.3.5.1, §5.3.7.1, §5.3.7.6, §5.3.8.1,
//                §5.3.11.1)
//
//  Description : The saved-state record writer for the non-binding groups
//                (D3). It is manager 1 of KL_pp_nvm_mgr_arb, beside the
//                binding manager (KL_acmp_nvm_shadow, manager 0), so its
//                records reach the one NVM port and the backend through the
//                same device face as the binding records.
//
//                OWNERSHIP. `own_o` holds the AECP engine's dispatch and
//                `bus_o` its state bus FROM RESET: no AECP program is
//                dispatched and none runs until this block reaches its
//                restore terminal. A value it restores is therefore written
//                into the reset state, never over a command that ran first.
//                In service it raises `own_o` again only to LATCH one record:
//                it waits in ACQUIRE, holding dispatch but NOT the bus, so a
//                program the engine took earlier runs to its end; once the
//                engine reads idle (`prog_busy_i` 0 while `own_o` is 1: no
//                program runs and none can start) it takes the bus for one
//                read of the row and releases both. A latched value is
//                therefore always one a completed command left.
//
//                THE RESTORE IS A TRANSACTION (parent D3 §6.2, §8). It starts
//                at the binding walk's end (`go_i`, the listener admission
//                gate's release) and proves the image first: a validated
//                image, or a LOCATE of ENTITY 0 that makes the store walk the
//                one the firmware loaded; an error ends CLOSED, cause 7, and
//                CLOSED keeps `own_o` for ever. PASS 0 reads every record
//                through the port and only proves it WHOLE or blank. PASS 1
//                reads them again: a record whole in one pass and not the
//                other aborts (cause 5, the passes agree record by record);
//                a whole record whose frame fails (magic, layout, id,
//                length, crc16) is refused and keeps its default; a framed
//                one is judged by the rule of the SET program that would set
//                it and, passing, written with its valid flag. A port err
//                with nothing forwarded whose cause is UNFRAMED is an erased
//                or unframed record (blank); DEVICE aborts (cause 2), a torn
//                read aborts (cause 1), and a descriptor LOCATE a rule needs
//                that errs aborts (cause 6): nothing was judged, so it is
//                never a refusal. Every wait is watched by one count of
//                cycles without its event; RS_TMO_CYC_P of them abort
//                (cause 3), abandoning a granted READ to the arbiter's drain
//                (`m_abort_o`). An abort in pass 0 has applied nothing: the
//                restore ends done and failed, on defaults. A restore write
//                is never a change (the snoop is the µCPU's).
//
//                THE VALUE RULES, the SET programs' own (gen_ucode.py):
//                configuration index below configurations_count (region
//                0xD); sampling rate on the located AUDIO_UNIT's list, read
//                at 144 with its offset checked, entries 0..count-1 of at
//                most eight, each a whole 32-bit word; clock source index
//                below the located CLOCK_DOMAIN's clock_sources_count
//                (lane 72, bits 47:32); a stream format that the
//                integrator's judge reports supported (Milan-info kind 0
//                selector 15, bit 0 alone: the maps it will be checked
//                against reset EMPTY in this stage, so nothing restored can
//                be orphaned); a presentation offset with bit 31 clear.
//                The LOCATEs use configuration 0, as the SET programs do.
//
//                THE RECORDS (scalar stage). One record per persisted
//                dynamic-state row, one dirty bit per record, NO shadow:
//                  0x00                  configuration index    u16
//                  0x02 + AUDIO_UNIT     sampling rate          u32
//                  0x0A + CLOCK_DOMAIN   clock source index     u16
//                  0x30 + STREAM_INPUT   stream format in       u64
//                  0x40 + STREAM_OUTPUT  stream format out      u64
//                  0x50 + STREAM_OUTPUT  presentation offset    u32
//                each framed as F07.8 {magic 0x1722, LAYOUT_VER_P, record
//                id, payload_length, crc16 CCITT-FALSE over the header
//                without its crc and the payload}, 16-bit header fields and
//                the payload big-endian (the saved-state allocation, parent
//                FASTCONNECT §4.2). Names and channel maps are later stages.
//
//                THE TRIGGER IS THE LIVE WRITE, NEVER A MARK. `chg_i` is the
//                µCPU's accepted state-bus write to a persisted row that
//                changed its projection {value, valid} (KL_aecp_dyn_state
//                wr_chg_o), tapped on the µCPU's side of the engine's
//                state-bus selection so a restore write is never a change.
//                The selector and descriptor index name the record; selector
//                7 (IDENTIFY) and an index past the shape set nothing.
//
//                THE CLEAR RULE. A record's dirty bit is set by a change and
//                cleared only by the port's done of the WRITE that carries a
//                value latched after the last change, or when its bounded
//                attempts are exhausted. A change after the latch TAINTS the
//                write, whose done then clears nothing, and a change on the
//                done edge wins. Set and clear name the record by group AND
//                index. The backend takes the record over at that done
//                (parent D3 §7.1): nothing a slot does later reaches back.
//
//                THE DEBOUNCE is a first-dirty window of DEB_TICKS_P ticks
//                (T-NVM-DEBOUNCE). Its close arms one burst that drains
//                every dirty record in round-robin order.
//
//                RETRY, THIS STAGE. A failed write is relatched and retried
//                at once, at most RETRY_MAX_P more times, then dropped with
//                the sticky `alarm_o`: the landed binding manager's policy.
//                The ruled DR2c backoff lands in the stage after the restore.
//
//                NOT YET HERE. The roll-back of an abort in pass 1 lands in
//                the next stage; until it does, such an abort ends CLOSED,
//                so no partially restored state is ever released.
//---------------------------------------------------------------------------//
`default_nettype none

module KL_aecp_nvm_writer #(
    //! the shape (P-N-STREAM-IN / P-N-STREAM-OUT, F01.5, and the AECP
    //! engine's dynamic-state rows): one record per row of selectors 0 to 5
    parameter int unsigned N_STREAM_IN_P  = 8,
    parameter int unsigned N_STREAM_OUT_P = 8,
    parameter int unsigned N_AUDIO_UNIT_P = 1,
    parameter int unsigned N_CLK_DOMAIN_P = 1,
    //! F07.8 layout_version, the one KL_acmp_nvm_shadow writes and accepts
    parameter logic [7:0]  LAYOUT_VER_P   = 8'h02,
    //! T-NVM-DEBOUNCE in tick_i units (F08.1: 500 ms at a 1 ms tick)
    parameter int unsigned DEB_TICKS_P    = 500,
    //! additional attempts after a failed first write (F07.9)
    parameter int unsigned RETRY_MAX_P    = 2,
    //! T-NVM-RS-DEADLINE (F08.1), P-NVM-RS-TMO-CYC (F01.5): clocks a restore
    //! wait may pass without its event before the restore aborts
    parameter int unsigned RS_TMO_CYC_P   = 2_000_000,
    //! derived — do not override
    localparam int unsigned OFF_RATE_C = 1,
    localparam int unsigned OFF_CLK_C  = OFF_RATE_C + N_AUDIO_UNIT_P,
    localparam int unsigned OFF_FMTI_C = OFF_CLK_C + N_CLK_DOMAIN_P,
    localparam int unsigned OFF_FMTO_C = OFF_FMTI_C + N_STREAM_IN_P,
    localparam int unsigned OFF_PTOF_C = OFF_FMTO_C + N_STREAM_OUT_P,
    localparam int unsigned N_REC_C    = OFF_PTOF_C + N_STREAM_OUT_P,
    localparam int unsigned RW_C       = $clog2(N_REC_C)
) (
    input  wire         clk_i,          //! core clock (P-CLK-HZ domain)
    input  wire         rst_n,          //! synchronous active-low HARD reset
    input  wire         tick_i,         //! debounce timebase tick (1 ms)

    //! ---- boot sequencing ----------------------------------------------------
    input  wire         go_i,           //! the binding walk's drained terminal (level)
    input  wire         img_valid_i,    //! the descriptor store holds a validated image

    //! ---- ownership of the AECP engine (the dispatch hold) -------------------
    //! 1 from reset to the restore terminal (for ever in CLOSED), and in
    //! service from ACQUIRE to the end of one record's latch: the engine
    //! takes no command meanwhile
    output logic        own_o,
    //! the state bus is this block's: from reset to the restore terminal,
    //! and in service only for the latch itself, never while a program the
    //! engine took before ACQUIRE is still running on it
    output logic        bus_o,
    input  wire         prog_busy_i,    //! the engine has a command in flight

    //! ---- the state-bus client (the engine's state port, µCPU contract) -----
    output logic        sb_req_o,
    output logic        sb_we_o,
    output logic [19:0] sb_addr_o,      //! [19:16] region, [15:0] byte offset
    output logic [63:0] sb_wdata_o,     //! write data / LOCATE key
    output logic [15:0] sb_didx_o,      //! descriptor index of a dynamic-state row
    input  wire         sb_ready_i,     //! a write was taken this cycle
    input  wire         sb_rvalid_i,
    input  wire  [63:0] sb_rdata_i,
    input  wire         sb_err_i,

    //! ---- the change snoop (the µCPU's accepted, changing write) -------------
    input  wire         chg_i,
    input  wire  [12:0] chg_sel_i,      //! dynamic-state selector
    input  wire  [15:0] chg_idx_i,      //! descriptor index

    //! ---- manager 1 of KL_pp_nvm_mgr_arb -------------------------------------
    output logic        m_req_o,        //! op request, held until m_gnt_i
    output logic        m_we_o,         //! 1 = commit, 0 = restore
    output logic  [7:0] m_rid_o,        //! record id
    output logic        m_wvalid_o,     //! commit byte present
    output logic  [7:0] m_wdata_o,      //! commit byte
    output logic        m_rready_o,     //! restore byte accepted
    output logic        m_abort_o,      //! abandon the READ it owns
    input  wire         m_gnt_i,        //! the request was issued this cycle
    input  wire         m_wready_i,     //! the port accepts the commit byte
    input  wire         m_done_i,       //! one cycle: its operation completed
    input  wire         m_err_i,        //! one cycle: its operation failed
    input  wire         m_rvalid_i,     //! restore byte present
    input  wire  [7:0]  m_rdata_i,      //! restore byte (framed record, header first)
    input  wire  [1:0]  m_err_cause_i,  //! the port's cause with err: 1 DEVICE, 2 UNFRAMED

    //! ---- the integrator's format judge (Milan-info kind 0 selector 15) ------
    output logic        jd_req_o,       //! the verdict on jd_fmt_o is asked for
    output logic [15:0] jd_type_o,      //! STREAM_INPUT or STREAM_OUTPUT
    output logic [15:0] jd_index_o,     //! the stream index
    output logic [63:0] jd_fmt_o,       //! the saved format
    input  wire  [63:0] jd_data_i,      //! bit 0: supported for that stream
    input  wire         jd_wait_i,      //! HOLD the beat (not a ready)

    //! ---- status (levels) -----------------------------------------------------
    output logic        done_o,         //! the restore is done: COMPLETE or DEFAULTS
    output logic        fail_o,         //! the restore failed, any terminal
    output logic        closed_o,       //! CLOSED: fail, never done, own kept
    //! the first abort's cause: 0 none, 1 torn, 2 device error, 3 deadline,
    //! 5 the passes disagree, 6 descriptor fault, 7 image not proven
    output logic  [2:0] cause_o,
    //! done, not failed, and no record read framed: nothing was restored
    output logic        blank_o,
    output logic        unflushed_o,    //! a record's change is not yet in the window
    output logic        alarm_o         //! sticky: a record's attempts were exhausted
);

  // ---- elaboration guards ----------------------------------------------------
  //! every group stays inside its allocated id block (parent FASTCONNECT §4.2)
  if ((N_AUDIO_UNIT_P < 1) || (N_AUDIO_UNIT_P > 8)
      || (N_CLK_DOMAIN_P < 1) || (N_CLK_DOMAIN_P > 8)
      || (N_STREAM_IN_P < 1) || (N_STREAM_IN_P > 16)
      || (N_STREAM_OUT_P < 1) || (N_STREAM_OUT_P > 16)) begin : g_shape_check
    $error("KL_aecp_nvm_writer: a group outgrows its record-id block");
  end
  //! zero would wrap RS_TMO_CYC_P - 1 below into a 2^32-clock deadline
  if (RS_TMO_CYC_P < 1) begin : g_rs_tmo_check
    $error("KL_aecp_nvm_writer: RS_TMO_CYC_P must be at least 1");
  end

  // ---- constants ---------------------------------------------------------------
  localparam logic [19:0] ADDR_LOCATE_C = 20'hF_0000;  //! desc store: LOCATE
  localparam logic [3:0]  RGN_DYN_C     = 4'h1;        //! dyn store: the value
  localparam logic [63:0] KEY_ENTITY0_C = 64'd0;       //! ENTITY 0, config 0
  localparam logic [7:0]  MAGIC_HI_C    = 8'h17;       //! F07.8 magic 0x1722
  localparam logic [7:0]  MAGIC_LO_C    = 8'h22;

  localparam logic [19:0] ADDR_NCFG_C  = 20'hD_0000;  //! configurations_count
  localparam logic [1:0]  PORT_UNFRAMED_C = 2'd2;     //! KL_pp_nvm_port cause
  //! the SET programs' descriptor geometry (gen_ucode.py): the AUDIO_UNIT's
  //! {current rate, sampling_rates_offset, count} lane and the list it
  //! walks, the CLOCK_DOMAIN's lane holding clock_sources_count at [47:32]
  localparam logic [15:0] AU_RATE_OFF_C  = 16'd136;
  localparam logic [15:0] SSR_LIST_OFF_C = 16'd144;
  localparam logic [3:0]  SSR_WALK_MAX_C = 4'd8;
  localparam logic [15:0] CD_SRCCNT_C    = 16'd72;
  localparam logic [15:0] DT_AUDIO_UNIT_C   = 16'h0002;
  localparam logic [15:0] DT_CLOCK_DOMAIN_C = 16'h0024;
  localparam logic [15:0] DT_STREAM_IN_C    = 16'h0005;
  localparam logic [15:0] DT_STREAM_OUT_C   = 16'h0006;

  localparam logic [2:0] CAUSE_NONE_C     = 3'd0;
  localparam logic [2:0] CAUSE_TORN_C     = 3'd1;
  localparam logic [2:0] CAUSE_DEVICE_C   = 3'd2;
  localparam logic [2:0] CAUSE_DEADLINE_C = 3'd3;
  localparam logic [2:0] CAUSE_PASSES_C   = 3'd5;
  localparam logic [2:0] CAUSE_DESC_C     = 3'd6;
  localparam logic [2:0] CAUSE_IMAGE_C    = 3'd7;

  // ---- the record geometry -----------------------------------------------------
  //! group g of record r is its dynamic-state selector (0 cfg, 1 rate,
  //! 2 clock source, 3 format in, 4 format out, 5 presentation offset)
  function automatic logic [2:0] rec_sel_f(input logic [RW_C-1:0] r);
    if (32'(r) < OFF_RATE_C)      return 3'd0;
    else if (32'(r) < OFF_CLK_C)  return 3'd1;
    else if (32'(r) < OFF_FMTI_C) return 3'd2;
    else if (32'(r) < OFF_FMTO_C) return 3'd3;
    else if (32'(r) < OFF_PTOF_C) return 3'd4;
    else                          return 3'd5;
  endfunction

  function automatic logic [31:0] sel_off_f(input logic [2:0] sel);
    unique case (sel)
      3'd0:    return 32'd0;
      3'd1:    return 32'(OFF_RATE_C);
      3'd2:    return 32'(OFF_CLK_C);
      3'd3:    return 32'(OFF_FMTI_C);
      3'd4:    return 32'(OFF_FMTO_C);
      default: return 32'(OFF_PTOF_C);
    endcase
  endfunction

  function automatic logic [31:0] sel_cnt_f(input logic [2:0] sel);
    unique case (sel)
      3'd0:    return 32'd1;
      3'd1:    return 32'(N_AUDIO_UNIT_P);
      3'd2:    return 32'(N_CLK_DOMAIN_P);
      3'd3:    return 32'(N_STREAM_IN_P);
      default: return 32'(N_STREAM_OUT_P);
    endcase
  endfunction

  function automatic logic [7:0] sel_base_f(input logic [2:0] sel);
    unique case (sel)
      3'd0:    return 8'h00;
      3'd1:    return 8'h02;
      3'd2:    return 8'h0A;
      3'd3:    return 8'h30;
      3'd4:    return 8'h40;
      default: return 8'h50;
    endcase
  endfunction

  //! payload bytes: the AEM field width of the group
  function automatic logic [3:0] sel_plen_f(input logic [2:0] sel);
    unique case (sel)
      3'd0, 3'd2: return 4'd2;
      3'd1, 3'd5: return 4'd4;
      default:    return 4'd8;
    endcase
  endfunction

  // ---- crc16 CCITT-FALSE byte step (07 §5.2 in-band integrity) -----------------
  function automatic logic [15:0] crc16_f(input logic [15:0] c,
                                          input logic [7:0]  d);
    logic [15:0] x;
    x = c ^ {d, 8'h00};
    for (int unsigned i = 0; i < 8; i++) begin
      x = x[15] ? ((x << 1) ^ 16'h1021) : (x << 1);
    end
    return x;
  endfunction

  //! byte k of the framed record {header 0..7, payload 8..}: the crc bytes
  //! read `crc`; the payload is `val`'s low `plen` bytes, big-endian
  function automatic logic [7:0] fr_byte_f(input logic [7:0]  rid,
                                           input logic [3:0]  plen,
                                           input logic [15:0] crc,
                                           input logic [63:0] val,
                                           input logic [4:0]  k);
    logic [7:0] b;
    unique case (k)
      5'd0:    b = MAGIC_HI_C;
      5'd1:    b = MAGIC_LO_C;
      5'd2:    b = LAYOUT_VER_P;
      5'd3:    b = rid;
      5'd4:    b = 8'd0;                          // payload_length[15:8]
      5'd5:    b = {4'd0, plen};                  // payload_length[7:0]
      5'd6:    b = crc[15:8];
      5'd7:    b = crc[7:0];
      default: b = val[8*(32'(plen) - 1 - (32'(k) - 8)) +: 8];
    endcase
    return b;
  endfunction

  // ============================================================================
  // the restore (boot): the image proof, then pass 0 and pass 1
  // ============================================================================
  typedef enum logic [3:0] {
    W_WAITGO,   // own from reset: wait for the binding walk's end
    W_IMG,      // is the image validated? else LOCATE ENTITY 0
    W_IMGLOC,   // the LOCATE is held until the store answers
    W_RQ,       // a record's READ, held until the arbiter grants it
    W_RD,       // its bytes, then the port's done or err
    W_RULE,     // pass 1, a whole framed record: its SET program's rule
    W_NCFG,     // the rule reads configurations_count (region 0xD)
    W_LOC,      // the rule LOCATEs its AUDIO_UNIT or CLOCK_DOMAIN
    W_LANE,     // the rule reads one 64-bit lane of that descriptor
    W_JUDGE,    // the integrator's verdict on a stream format
    W_APPLY,    // the value, and with it its valid flag, into the store
    W_NEXT,     // the next record, the next pass, or the terminal
    W_DONE,     // terminal: COMPLETE, or DEFAULTS after an abort in pass 0
    W_CLOSED    // terminal: fail, never done, own kept until reset
  } wstate_e;

  wstate_e          ws_r;
  logic             done_r, fail_r, closed_r;
  logic [2:0]       cause_r;
  logic             proven_r;       // the image is proven: an abort is no longer CLOSED
  logic             pass_r;         // 0 proves every record whole, 1 applies
  logic [RW_C-1:0]  rec_r;          // the record being read
  logic [N_REC_C-1:0] whole0_r;     // pass 0 read the record whole
  logic             framed_any_r;   // pass 1 read a framed, crc-clean record
  logic [7:0]       n_app_r, n_ref_r, n_blank_r;   // pass 1 verdicts (suite taps)

  // ---- the record being read -------------------------------------------------
  logic [2:0]  rsel_w;
  logic [15:0] ridx_w;
  logic [7:0]  rrid_w;
  logic [3:0]  rplen_w;

  assign rsel_w  = rec_sel_f(rec_r);
  assign ridx_w  = 16'(32'(rec_r) - sel_off_f(rsel_w));
  assign rrid_w  = sel_base_f(rsel_w) + 8'(ridx_w);
  assign rplen_w = sel_plen_f(rsel_w);

  // ---- the bytes the port forwards --------------------------------------------
  logic [16:0] rbcnt_r;             // bytes taken this read
  logic [15:0] rmagic_r, rplen_hdr_r, rcrc_rx_r, rcrc_acc_r;
  logic [7:0]  rver_r, rrid_hdr_r;
  logic [63:0] rval_r;              // the payload's last 8 bytes, big-endian

  logic rd_err_w, rd_done_w, rd_whole_w, rd_blank_w, rd_torn_w, rd_dev_w;
  assign rd_err_w   = (ws_r == W_RD) && m_err_i;
  assign rd_done_w  = (ws_r == W_RD) && m_done_i;
  //! the port ends a read with done only after the header's payload length
  assign rd_whole_w = rd_done_w && (rbcnt_r >= 17'd8)
                      && (rbcnt_r == 17'd8 + 17'(rplen_hdr_r));
  assign rd_blank_w = rd_err_w && (rbcnt_r == 17'd0)
                      && (m_err_cause_i == PORT_UNFRAMED_C);
  assign rd_dev_w   = rd_err_w && (rbcnt_r == 17'd0)
                      && (m_err_cause_i != PORT_UNFRAMED_C);
  assign rd_torn_w  = (rd_err_w && (rbcnt_r != 17'd0)) || (rd_done_w && !rd_whole_w);

  logic frame_ok_w;
  assign frame_ok_w = (rmagic_r == {MAGIC_HI_C, MAGIC_LO_C})
                      && (rver_r == LAYOUT_VER_P) && (rrid_hdr_r == rrid_w)
                      && (rplen_hdr_r == 16'(rplen_w)) && (rcrc_acc_r == rcrc_rx_r);

  // ---- the value rules -----------------------------------------------------------
  logic [15:0] lane_off_r;          // the lane the rule reads next
  logic        walking_r;           // the rate rule is walking its list
  logic [3:0]  walk_k_r;            // entries k and k + 1 share the lane
  logic [15:0] rcount_r;            // the list's count

  //! the rule's verdict on the answer in hand, and whether it needs another
  //! lane first (the rate list's next pair)
  logic lane_accept_w, lane_refuse_w;
  logic [3:0] walk_next_w;
  always_comb begin : lane_rule
    lane_accept_w = 1'b0;
    lane_refuse_w = 1'b0;
    walk_next_w   = walk_k_r + 4'd2;
    if (rsel_w == 3'd2) begin
      lane_accept_w = rval_r[15:0] < sb_rdata_i[47:32];
      lane_refuse_w = !lane_accept_w;
    end else if (!walking_r) begin
      //! the list sits where this AEM version puts it, else every rate is
      //! refused (fail closed); a count of 0 lists nothing
      lane_refuse_w = (sb_rdata_i[31:16] != SSR_LIST_OFF_C)
                      || (sb_rdata_i[15:0] == 16'd0);
    end else if (rval_r[31:0] == sb_rdata_i[63:32]) begin
      lane_accept_w = 1'b1;                           // entry k
    end else if (rcount_r == 16'(walk_k_r) + 16'd1) begin
      lane_refuse_w = 1'b1;                           // the list is spent
    end else if (rval_r[31:0] == sb_rdata_i[31:0]) begin
      lane_accept_w = 1'b1;                           // entry k + 1
    end else begin
      //! past the bound, or the list spent before the next pair
      lane_refuse_w = (walk_next_w == SSR_WALK_MAX_C)
                      || (rcount_r == 16'(walk_next_w));
    end
  end

  // ---- the deadline --------------------------------------------------------------
  //! consecutive cycles a restore wait passes without the event it waits for
  logic [31:0] wd_r;
  logic        stall_w, expire_w;

  always_comb begin : restore_stall
    unique case (ws_r)
      W_IMGLOC, W_NCFG, W_LOC, W_LANE: stall_w = !sb_rvalid_i;
      W_RQ:    stall_w = !m_gnt_i;
      W_RD:    stall_w = !m_rvalid_i && !m_done_i && !m_err_i;
      W_JUDGE: stall_w = jd_wait_i;
      W_APPLY: stall_w = !sb_ready_i;
      default: stall_w = 1'b0;
    endcase
  end
  assign expire_w = stall_w && (wd_r >= 32'(RS_TMO_CYC_P - 1));

  always_ff @(posedge clk_i) begin : deadline_ff
    if (!rst_n)                    wd_r <= 32'd0;
    else if (stall_w && !expire_w) wd_r <= wd_r + 32'd1;
    else                           wd_r <= 32'd0;
  end

  // ---- the abort -----------------------------------------------------------------
  logic       abort_w, pass_disagree_w;
  logic [2:0] abort_cause_w;
  assign pass_disagree_w = pass_r && (rd_whole_w || rd_blank_w)
                           && (rd_whole_w != whole0_r[rec_r]);
  always_comb begin : abort_decode
    abort_w       = 1'b1;
    abort_cause_w = CAUSE_NONE_C;
    if (expire_w)                                 abort_cause_w = CAUSE_DEADLINE_C;
    else if (rd_dev_w)                            abort_cause_w = CAUSE_DEVICE_C;
    else if (rd_torn_w)                           abort_cause_w = CAUSE_TORN_C;
    else if (pass_disagree_w)                     abort_cause_w = CAUSE_PASSES_C;
    else if ((ws_r == W_IMGLOC) && sb_rvalid_i && sb_err_i)
                                                  abort_cause_w = CAUSE_IMAGE_C;
    else if ((ws_r == W_LOC) && sb_rvalid_i && sb_err_i)
                                                  abort_cause_w = CAUSE_DESC_C;
    else                                          abort_w = 1'b0;
  end

  //! the rule's verdict, in the cycle it is reached
  logic accept_w, refuse_w;
  always_comb begin : verdict
    accept_w = 1'b0;
    refuse_w = 1'b0;
    unique case (ws_r)
      W_RULE:  begin accept_w = (rsel_w == 3'd5) && !rval_r[31];
                     refuse_w = (rsel_w == 3'd5) && rval_r[31]; end
      W_NCFG:  begin accept_w = sb_rvalid_i && (rval_r[15:0] < sb_rdata_i[15:0]);
                     refuse_w = sb_rvalid_i && !(rval_r[15:0] < sb_rdata_i[15:0]); end
      W_LANE:  begin accept_w = sb_rvalid_i && lane_accept_w;
                     refuse_w = sb_rvalid_i && lane_refuse_w; end
      W_JUDGE: begin accept_w = !jd_wait_i && jd_data_i[0];
                     refuse_w = !jd_wait_i && !jd_data_i[0]; end
      default: ;
    endcase
  end

  always_ff @(posedge clk_i) begin : restore_ff
    if (!rst_n) begin
      ws_r         <= W_WAITGO;
      done_r       <= 1'b0;
      fail_r       <= 1'b0;
      closed_r     <= 1'b0;
      cause_r      <= CAUSE_NONE_C;
      proven_r     <= 1'b0;
      pass_r       <= 1'b0;
      rec_r        <= '0;
      whole0_r     <= '0;
      framed_any_r <= 1'b0;
      n_app_r      <= 8'd0;
      n_ref_r      <= 8'd0;
      n_blank_r    <= 8'd0;
      rbcnt_r      <= 17'd0;
      rmagic_r     <= 16'd0;
      rplen_hdr_r  <= 16'd0;
      rcrc_rx_r    <= 16'd0;
      rcrc_acc_r   <= 16'd0;
      rver_r       <= 8'd0;
      rrid_hdr_r   <= 8'd0;
      rval_r       <= 64'd0;
      lane_off_r   <= 16'd0;
      walking_r    <= 1'b0;
      walk_k_r     <= 4'd0;
      rcount_r     <= 16'd0;
    end else if (abort_w) begin
      //! the first abort names the restore. Before the image is proven it is
      //! CLOSED; in pass 0 nothing was applied, so it ends done on defaults;
      //! in pass 1 it ends CLOSED until the roll-back lands
      cause_r  <= abort_cause_w;
      fail_r   <= 1'b1;
      if (proven_r && !pass_r) begin
        done_r <= 1'b1;
        ws_r   <= W_DONE;
      end else begin
        closed_r <= 1'b1;
        ws_r     <= W_CLOSED;
      end
    end else begin
      unique case (ws_r)
        W_WAITGO: begin
          if (go_i) ws_r <= W_IMG;
        end
        W_IMG: begin
          if (img_valid_i) begin
            proven_r <= 1'b1;
            ws_r     <= W_RQ;
          end else begin
            ws_r <= W_IMGLOC;
          end
        end
        W_IMGLOC: begin
          if (sb_rvalid_i) begin           // an err answer aborted above
            proven_r <= 1'b1;
            ws_r     <= W_RQ;
          end
        end
        W_RQ: begin
          if (m_gnt_i) begin
            rbcnt_r    <= 17'd0;
            rcrc_acc_r <= 16'hFFFF;
            rval_r     <= 64'd0;
            ws_r       <= W_RD;
          end
        end
        W_RD: begin
          if (m_rvalid_i) begin
            if ((rbcnt_r != 17'd6) && (rbcnt_r != 17'd7))
              rcrc_acc_r <= crc16_f(rcrc_acc_r, m_rdata_i);
            unique case (rbcnt_r)
              17'd0:   rmagic_r    <= {m_rdata_i, rmagic_r[7:0]};
              17'd1:   rmagic_r    <= {rmagic_r[15:8], m_rdata_i};
              17'd2:   rver_r      <= m_rdata_i;
              17'd3:   rrid_hdr_r  <= m_rdata_i;
              17'd4:   rplen_hdr_r <= {m_rdata_i, rplen_hdr_r[7:0]};
              17'd5:   rplen_hdr_r <= {rplen_hdr_r[15:8], m_rdata_i};
              17'd6:   rcrc_rx_r   <= {m_rdata_i, rcrc_rx_r[7:0]};
              17'd7:   rcrc_rx_r   <= {rcrc_rx_r[15:8], m_rdata_i};
              default: rval_r      <= {rval_r[55:0], m_rdata_i};
            endcase
            rbcnt_r <= rbcnt_r + 17'd1;
          end
          if (rd_whole_w || rd_blank_w) begin
            if (!pass_r) begin
              whole0_r[rec_r] <= rd_whole_w;
              ws_r            <= W_NEXT;
            end else if (rd_blank_w) begin
              n_blank_r <= n_blank_r + 8'd1;  // erased or unframed: the default
              ws_r      <= W_NEXT;
            end else if (!frame_ok_w) begin
              n_ref_r <= n_ref_r + 8'd1;      // the frame refuses it
              ws_r    <= W_NEXT;
            end else begin
              framed_any_r <= 1'b1;
              ws_r         <= W_RULE;
            end
          end
        end
        W_RULE: begin
          unique case (rsel_w)
            3'd0:       ws_r <= W_NCFG;
            3'd1, 3'd2: ws_r <= W_LOC;
            3'd3, 3'd4: ws_r <= W_JUDGE;
            default:    ws_r <= accept_w ? W_APPLY : W_NEXT;
          endcase
          if (refuse_w) n_ref_r <= n_ref_r + 8'd1;
        end
        W_LOC: begin
          if (sb_rvalid_i) begin           // an err answer aborted above
            lane_off_r <= (rsel_w == 3'd1) ? AU_RATE_OFF_C : CD_SRCCNT_C;
            walking_r  <= 1'b0;
            ws_r       <= W_LANE;
          end
        end
        W_LANE: begin
          if (sb_rvalid_i && !accept_w && !refuse_w) begin
            //! the rate list: the head lane gave its count; each later lane
            //! holds entries k and k + 1
            if (!walking_r) begin
              rcount_r   <= sb_rdata_i[15:0];
              walk_k_r   <= 4'd0;
              lane_off_r <= SSR_LIST_OFF_C;
            end else begin
              walk_k_r   <= walk_next_w;
              lane_off_r <= SSR_LIST_OFF_C + (16'(walk_next_w) << 2);
            end
            walking_r <= 1'b1;
          end
        end
        W_NCFG, W_JUDGE: ;               // they leave on their verdict, below
        W_APPLY: begin
          if (sb_ready_i) begin
            n_app_r <= n_app_r + 8'd1;
            ws_r    <= W_NEXT;
          end
        end
        W_NEXT: begin
          if (rec_r == RW_C'(N_REC_C - 1)) begin
            rec_r <= '0;
            if (pass_r) begin
              done_r <= 1'b1;             // COMPLETE: every record agreed
              ws_r   <= W_DONE;
            end else begin
              pass_r <= 1'b1;
              ws_r   <= W_RQ;
            end
          end else begin
            rec_r <= rec_r + RW_C'(1);
            ws_r  <= W_RQ;
          end
        end
        W_DONE, W_CLOSED: ;
        default: ws_r <= W_CLOSED;
      endcase
      //! the rule states leave on their verdict
      if ((ws_r == W_NCFG) || (ws_r == W_LANE) || (ws_r == W_JUDGE)) begin
        if (accept_w)      ws_r <= W_APPLY;
        else if (refuse_w) begin
          n_ref_r <= n_ref_r + 8'd1;
          ws_r    <= W_NEXT;
        end
      end
    end
  end

  // ---- the restore's requests on the state bus ----------------------------
  logic        rs_sb_req_w;
  logic [19:0] rs_sb_addr_w;
  logic [63:0] rs_sb_wdata_w;
  always_comb begin : restore_bus
    rs_sb_req_w   = 1'b0;
    rs_sb_addr_w  = ADDR_LOCATE_C;
    rs_sb_wdata_w = KEY_ENTITY0_C;
    unique case (ws_r)
      W_IMGLOC: rs_sb_req_w = 1'b1;
      W_NCFG: begin
        rs_sb_req_w  = 1'b1;
        rs_sb_addr_w = ADDR_NCFG_C;
      end
      W_LOC: begin
        rs_sb_req_w   = 1'b1;
        rs_sb_wdata_w = {16'd0, ridx_w,
                         (rsel_w == 3'd1) ? DT_AUDIO_UNIT_C : DT_CLOCK_DOMAIN_C,
                         16'd0};
      end
      W_LANE: begin
        rs_sb_req_w  = 1'b1;
        rs_sb_addr_w = {4'h0, lane_off_r};
      end
      W_APPLY: begin
        rs_sb_req_w   = 1'b1;
        rs_sb_addr_w  = {RGN_DYN_C, 13'(rsel_w), 3'd0};
        rs_sb_wdata_w = rval_r;
      end
      default: ;
    endcase
  end

  // ============================================================================
  // the writer in service
  // ============================================================================
  typedef enum logic [2:0] {
    S_RUN,      // wait for the armed burst and a dirty record
    S_ACQ,      // own the bus; wait until the engine reads idle
    S_LATCH,    // one state-bus read of the record's row
    S_LATCHW,   // its answer is the value this write carries
    S_CRC,      // the crc over the header without its crc, then the payload
    S_REQ,      // the commit request, held until the arbiter grants it
    S_STREAM,   // the 8 header bytes and the payload
    S_WAIT      // the port's done or err
  } sstate_e;

  sstate_e          ss_r;
  logic [RW_C-1:0]  hand_r;         // the record in hand
  logic [RW_C-1:0]  rr_r;           // round-robin start of the next pick
  logic [63:0]      val_r;          // the latched value, right-justified
  logic [15:0]      crc_r;
  logic [4:0]       cix_r;          // crc byte cursor over header 0..5 + payload
  logic [4:0]       six_r;          // stream byte cursor 0 .. 7 + plen
  logic [31:0]      attempts_r;
  logic             taint_r;
  logic [N_REC_C-1:0] dirty_r;
  logic             alarm_r;

  // ---- the record in hand ----------------------------------------------------
  logic [2:0]  hsel_w;
  logic [15:0] hidx_w;
  logic [7:0]  hrid_w;
  logic [3:0]  hplen_w;

  assign hsel_w  = rec_sel_f(hand_r);
  assign hidx_w  = 16'(32'(hand_r) - sel_off_f(hsel_w));
  assign hrid_w  = sel_base_f(hsel_w) + 8'(hidx_w);
  assign hplen_w = sel_plen_f(hsel_w);

  // ---- the change snoop: one record per accepted changing write -------------
  logic [N_REC_C-1:0] set_w;
  logic               set_any_w;

  always_comb begin : change_decode
    set_w = '0;
    if (chg_i && (chg_sel_i <= 13'd5)
        && (32'(chg_idx_i) < sel_cnt_f(chg_sel_i[2:0]))) begin
      set_w[sel_off_f(chg_sel_i[2:0]) + 32'(chg_idx_i)] = 1'b1;
    end
  end
  assign set_any_w = |set_w;

  // ---- the first-dirty debounce and the armed burst --------------------------
  logic        deb_open_r, armed_r;
  logic [31:0] deb_cnt_r;
  logic        pick_any_w;
  logic [RW_C-1:0] pick_w;

  //! round-robin: the first dirty record at or after rr_r, else the first
  always_comb begin : pick
    logic hi_any;
    logic [RW_C-1:0] hi, lo;
    hi_any = 1'b0; hi = '0; lo = '0; pick_any_w = 1'b0;
    for (int unsigned i = 0; i < N_REC_C; i++) begin
      if (dirty_r[i] && !pick_any_w) begin
        pick_any_w = 1'b1;
        lo = RW_C'(i);
      end
      if (dirty_r[i] && !hi_any && (RW_C'(i) >= rr_r)) begin
        hi_any = 1'b1;
        hi = RW_C'(i);
      end
    end
    pick_w = hi_any ? hi : lo;
  end

  always_ff @(posedge clk_i) begin : debounce_ff
    if (!rst_n) begin
      deb_open_r <= 1'b0;
      deb_cnt_r  <= 32'd0;
      armed_r    <= 1'b0;
    end else begin
      if (set_any_w && !deb_open_r) begin
        deb_open_r <= 1'b1;
        deb_cnt_r  <= DEB_TICKS_P;
      end else if (deb_open_r && tick_i) begin
        if (deb_cnt_r <= 32'd1) begin
          deb_open_r <= 1'b0;
          armed_r    <= 1'b1;            // the window closed: one burst
        end else begin
          deb_cnt_r <= deb_cnt_r - 32'd1;
        end
      end
      if ((ss_r == S_RUN) && armed_r && !pick_any_w) armed_r <= 1'b0;
    end
  end

  // ---- the clear rule ----------------------------------------------------------
  logic write_err_w, done_ok_w, giveup_w;
  logic [N_REC_C-1:0] clr_w;

  assign write_err_w = ((ss_r == S_STREAM) || (ss_r == S_WAIT)) && m_err_i;
  assign done_ok_w   = (ss_r == S_WAIT) && m_done_i && !taint_r;
  assign giveup_w    = write_err_w && (attempts_r >= 32'(1 + RETRY_MAX_P));

  always_comb begin : clear_decode
    clr_w = '0;
    if (done_ok_w || giveup_w) clr_w[hand_r] = 1'b1;
  end

  //! a change on the done edge wins: set outranks the clear
  always_ff @(posedge clk_i) begin : dirty_ff
    if (!rst_n) dirty_r <= '0;
    else        dirty_r <= set_w | (dirty_r & ~clr_w);
  end

  //! a change to the record in hand after its latch began taints the write
  always_ff @(posedge clk_i) begin : taint_ff
    if (!rst_n)                                    taint_r <= 1'b0;
    else if ((ss_r == S_ACQ) && !prog_busy_i)      taint_r <= 1'b0;
    else if (set_w[hand_r] && (ss_r != S_RUN)
             && (ss_r != S_ACQ))                   taint_r <= 1'b1;
  end

  always_ff @(posedge clk_i) begin : alarm_ff
    if (!rst_n)        alarm_r <= 1'b0;
    else if (giveup_w) alarm_r <= 1'b1;            // sticky until reset
  end

  // ---- the flush -------------------------------------------------------------
  logic [4:0] crc_last_w, stream_last_w;
  assign crc_last_w    = 5'd5 + 5'(hplen_w);       // header 0..5, payload
  assign stream_last_w = 5'd7 + 5'(hplen_w);

  //! the crc pass skips the crc field itself: cursor 0..5 -> byte 0..5,
  //! cursor 6.. -> byte 8..
  logic [4:0] cbyte_w;
  assign cbyte_w = (cix_r < 5'd6) ? cix_r : (cix_r + 5'd2);

  always_ff @(posedge clk_i) begin : service_ff
    if (!rst_n) begin
      ss_r       <= S_RUN;
      hand_r     <= '0;
      rr_r       <= '0;
      val_r      <= 64'd0;
      crc_r      <= 16'd0;
      cix_r      <= 5'd0;
      six_r      <= 5'd0;
      attempts_r <= 32'd0;
    end else begin
      unique case (ss_r)
        S_RUN: begin
          if (done_r && armed_r && pick_any_w) begin
            hand_r     <= pick_w;
            attempts_r <= 32'd0;
            ss_r       <= S_ACQ;
          end
        end
        S_ACQ: begin
          if (!prog_busy_i) ss_r <= S_LATCH;
        end
        S_LATCH: begin
          ss_r <= S_LATCHW;
        end
        S_LATCHW: begin
          if (sb_rvalid_i) begin
            val_r <= sb_rdata_i;
            crc_r <= 16'hFFFF;
            cix_r <= 5'd0;
            ss_r  <= S_CRC;
          end
        end
        S_CRC: begin
          crc_r <= crc16_f(crc_r, fr_byte_f(hrid_w, hplen_w, 16'h0000, val_r,
                                            cbyte_w));
          if (cix_r == crc_last_w) begin
            six_r <= 5'd0;
            ss_r  <= S_REQ;
          end else begin
            cix_r <= cix_r + 5'd1;
          end
        end
        S_REQ: begin
          if (m_gnt_i) begin
            attempts_r <= attempts_r + 32'd1;
            ss_r       <= S_STREAM;
          end
        end
        S_STREAM, S_WAIT: begin
          if (write_err_w) begin
            ss_r <= giveup_w ? S_RUN : S_ACQ;   // relatch, bounded; else drop
            if (giveup_w) rr_r <= hand_r + RW_C'(1);
          end else if (ss_r == S_STREAM) begin
            if (m_wready_i) begin
              if (six_r == stream_last_w) ss_r <= S_WAIT;
              else                        six_r <= six_r + 5'd1;
            end
          end else if (m_done_i) begin
            rr_r <= hand_r + RW_C'(1);
            ss_r <= S_RUN;                      // dirty cleared (dirty_ff)
          end
        end
        default: ss_r <= S_RUN;
      endcase
    end
  end

  // ============================================================================
  // the faces
  // ============================================================================
  logic latch_w;
  assign latch_w = (ss_r == S_LATCH) || (ss_r == S_LATCHW);

  //! the boot ownership ends at a done terminal, never in CLOSED; the
  //! service takes the bus again for one latch at a time
  assign own_o = !done_r || (ss_r == S_ACQ) || latch_w;
  assign bus_o = !done_r || latch_w;

  //! the latch reads the row once; the dynamic-state store answers the next
  //! cycle, so its request is one cycle wide. Before the terminal the
  //! restore drives the bus and the port.
  assign sb_req_o   = done_r ? (ss_r == S_LATCH) : rs_sb_req_w;
  assign sb_we_o    = !done_r && (ws_r == W_APPLY);
  assign sb_addr_o  = done_r ? {RGN_DYN_C, 13'(hsel_w), 3'd0} : rs_sb_addr_w;
  assign sb_wdata_o = rs_sb_wdata_w;
  assign sb_didx_o  = done_r ? hidx_w : ridx_w;

  assign m_req_o    = done_r ? (ss_r == S_REQ) : (ws_r == W_RQ);
  assign m_we_o     = done_r;
  assign m_rid_o    = done_r ? hrid_w : rrid_w;
  assign m_wvalid_o = (ss_r == S_STREAM);
  assign m_wdata_o  = fr_byte_f(hrid_w, hplen_w, crc_r, val_r, six_r);
  assign m_rready_o = !done_r && (ws_r == W_RD);
  assign m_abort_o  = expire_w && (ws_r == W_RD);

  assign jd_req_o   = (ws_r == W_JUDGE);
  assign jd_type_o  = (rsel_w == 3'd3) ? DT_STREAM_IN_C : DT_STREAM_OUT_C;
  assign jd_index_o = ridx_w;
  assign jd_fmt_o   = rval_r;

  assign done_o      = done_r;
  assign fail_o      = fail_r;
  assign closed_o    = closed_r;
  assign cause_o     = cause_r;
  assign blank_o     = done_r && !fail_r && !framed_any_r;
  assign unflushed_o = |dirty_r;
  assign alarm_o     = alarm_r;

endmodule

`default_nettype wire
