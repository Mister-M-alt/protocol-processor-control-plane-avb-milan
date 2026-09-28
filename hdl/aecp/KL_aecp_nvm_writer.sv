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
//                THE WALK STARTS AT THE BINDING WALK'S END (`go_i`, the
//                listener admission gate's release) and proves the image
//                first: a validated image, or a LOCATE of ENTITY 0 that makes
//                the store walk the one the firmware loaded. An error ends
//                the restore CLOSED, cause 7, and CLOSED keeps `own_o` for
//                ever. Every restore wait is watched by one count of cycles
//                without its event; RS_TMO_CYC_P of them abort (cause 3).
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
//                NOT YET HERE. The two restore passes over the records and
//                their value rules, and the roll-back, land in the next
//                stages; COMPLETE follows the image proof directly.
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

    //! ---- status (levels) -----------------------------------------------------
    output logic        done_o,         //! the restore reached COMPLETE
    output logic        fail_o,         //! the restore failed, any terminal
    output logic        closed_o,       //! CLOSED: fail, never done, own kept
    //! the first abort's cause: 0 none, 3 the deadline, 7 image not proven
    output logic  [2:0] cause_o,
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

  localparam logic [2:0] CAUSE_NONE_C     = 3'd0;
  localparam logic [2:0] CAUSE_DEADLINE_C = 3'd3;
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
  // the restore (boot)
  // ============================================================================
  typedef enum logic [2:0] {
    W_WAITGO,   // own from reset: wait for the binding walk's end
    W_IMG,      // is the image validated? else LOCATE ENTITY 0
    W_IMGLOC,   // the LOCATE is held until the store answers
    W_COMPLETE, // terminal: done, service runs
    W_CLOSED    // terminal: fail, never done, own kept until reset
  } wstate_e;

  wstate_e    ws_r;
  logic       done_r, fail_r, closed_r;
  logic [2:0] cause_r;

  //! consecutive cycles a restore wait passes without its event
  logic [31:0] wd_r;
  logic        stall_w, expire_w;

  assign stall_w  = (ws_r == W_IMGLOC) && !sb_rvalid_i;
  assign expire_w = stall_w && (wd_r >= 32'(RS_TMO_CYC_P - 1));

  always_ff @(posedge clk_i) begin : deadline_ff
    if (!rst_n)                    wd_r <= 32'd0;
    else if (stall_w && !expire_w) wd_r <= wd_r + 32'd1;
    else                           wd_r <= 32'd0;
  end

  always_ff @(posedge clk_i) begin : restore_ff
    if (!rst_n) begin
      ws_r     <= W_WAITGO;
      done_r   <= 1'b0;
      fail_r   <= 1'b0;
      closed_r <= 1'b0;
      cause_r  <= CAUSE_NONE_C;
    end else begin
      unique case (ws_r)
        W_WAITGO: begin
          if (go_i) ws_r <= W_IMG;
        end
        W_IMG: begin
          ws_r <= img_valid_i ? W_COMPLETE : W_IMGLOC;
        end
        W_IMGLOC: begin
          if (expire_w) begin
            fail_r   <= 1'b1;               // no image proven: nothing can be
            closed_r <= 1'b1;               // judged, so the walk ends CLOSED
            cause_r  <= CAUSE_DEADLINE_C;
            ws_r     <= W_CLOSED;
          end else if (sb_rvalid_i && sb_err_i) begin
            fail_r   <= 1'b1;
            closed_r <= 1'b1;
            cause_r  <= CAUSE_IMAGE_C;
            ws_r     <= W_CLOSED;
          end else if (sb_rvalid_i) begin
            ws_r <= W_COMPLETE;
          end
        end
        W_COMPLETE: begin
          done_r <= 1'b1;
        end
        W_CLOSED: ;
        default: ws_r <= W_CLOSED;
      endcase
    end
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
  //! cycle, so its request is one cycle wide
  assign sb_req_o   = done_r ? (ss_r == S_LATCH) : (ws_r == W_IMGLOC);
  assign sb_we_o    = 1'b0;
  assign sb_addr_o  = done_r ? {RGN_DYN_C, 13'(hsel_w), 3'd0} : ADDR_LOCATE_C;
  assign sb_wdata_o = KEY_ENTITY0_C;
  assign sb_didx_o  = hidx_w;

  assign m_req_o    = (ss_r == S_REQ);
  assign m_we_o     = 1'b1;
  assign m_rid_o    = hrid_w;
  assign m_wvalid_o = (ss_r == S_STREAM);
  assign m_wdata_o  = fr_byte_f(hrid_w, hplen_w, crc_r, val_r, six_r);
  assign m_rready_o = 1'b0;
  assign m_abort_o  = 1'b0;

  assign done_o      = done_r;
  assign fail_o      = fail_r;
  assign closed_o    = closed_r;
  assign cause_o     = cause_r;
  assign unflushed_o = |dirty_r;
  assign alarm_o     = alarm_r;

endmodule

`default_nettype wire
