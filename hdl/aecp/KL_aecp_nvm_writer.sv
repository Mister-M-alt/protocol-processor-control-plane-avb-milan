/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : KL_aecp_nvm_writer.sv
//  Project     : IEEE 1722.1 protocol processor (docs/architecture/07 §5.3
//                F07.9 boot restore before enable; 02 §8 F02.8 class-F
//                manager face; parent D3 contract, milan-fpga
//                docs/design/SAVED_STATE_MATERIALIZATION.md §3, §6.2, §8.1;
//                Milan §5.3.5.1, §5.3.7.1, §5.3.7.6, §5.3.8.1, §5.3.11.1)
//
//  Description : The saved-state record writer for the non-binding groups
//                (D3). It is manager 1 of KL_pp_nvm_mgr_arb, beside the
//                binding manager (KL_acmp_nvm_shadow, manager 0), so its
//                records reach the one NVM port and the backend through the
//                same device face as the binding records.
//
//                OWNERSHIP IS THE WHOLE POINT OF THIS STAGE. `own_o` holds
//                the AECP engine's state bus and its dispatch FROM RESET: no
//                AECP program is dispatched and none runs until this block
//                reaches its restore terminal. A value it restores is
//                therefore written into the reset state, never over a
//                command that ran first, and a later command always follows
//                the restore instead of being overwritten by it. The engine
//                drives nothing on the state bus while `own_o` is 1; this
//                block drives it instead.
//
//                THE WALK STARTS AT THE BINDING WALK'S END. `go_i` is the
//                listener admission gate's release (KL_pp_acmp_lsn_admit
//                released_o): the binding walk's drained terminal, so the
//                device face has no binding restore read in flight and every
//                preload's record write and discovery arm precede it. A boot
//                that never starts the binding walk never releases AECP.
//
//                THE IMAGE IS PROVEN FIRST. A restored value is judged
//                against the descriptor image, and a name is initialized
//                from it, so the walk proceeds only over a VALIDATED image
//                (`img_valid_i`, KL_aecp_desc_store's validated-image
//                level). When the store does not hold one, a LOCATE of
//                ENTITY 0 makes it walk the image the firmware loaded
//                (heal before answer) and the answer decides: a hit proves
//                the image, an error ends the restore CLOSED, cause 7.
//                CLOSED keeps `own_o` for ever, so AECP stays held and no
//                command is served from an image nothing proved.
//
//                THE DEADLINE. Every wait of the restore is watched by one
//                count of consecutive cycles without the awaited event;
//                reaching RS_TMO_CYC_P aborts the restore (cause 3). Before
//                the image is proven an abort ends CLOSED.
//
//                NOT YET HERE. The per-record dirty bits, the change snoop
//                and the flush, the two restore passes over the records and
//                their value rules, and the roll-back land in the stages
//                after this one; the manager-1 face below is held idle until
//                then, and COMPLETE follows the image proof directly.
//---------------------------------------------------------------------------//
`default_nettype none

module KL_aecp_nvm_writer #(
    //! T-NVM-RS-DEADLINE (F08.1), P-NVM-RS-TMO-CYC (F01.5): clocks a restore
    //! wait may pass without its event before the restore aborts
    parameter int unsigned RS_TMO_CYC_P = 2_000_000
) (
    input  wire         clk_i,          //! core clock (P-CLK-HZ domain)
    input  wire         rst_n,          //! synchronous active-low HARD reset

    //! ---- boot sequencing ----------------------------------------------------
    input  wire         go_i,           //! the binding walk's drained terminal (level)
    input  wire         img_valid_i,    //! the descriptor store holds a validated image

    //! ---- ownership of the AECP engine (the dispatch hold) -------------------
    //! 1 from reset to the restore terminal, for ever in CLOSED: the engine
    //! dispatches no program and routes the state bus to this block
    output logic        own_o,

    //! ---- the state-bus client (the engine's state port, µCPU contract) -----
    //! the request is held until its answer: a read answers with one cycle of
    //! sb_rvalid_i, and sb_err_i qualifies a LOCATE's answer
    output logic        sb_req_o,
    output logic        sb_we_o,
    output logic [19:0] sb_addr_o,      //! [19:16] region, [15:0] byte offset
    output logic [63:0] sb_wdata_o,     //! write data / LOCATE key
    input  wire         sb_rvalid_i,
    input  wire         sb_err_i,

    //! ---- manager 1 of KL_pp_nvm_mgr_arb (idle in this stage) ---------------
    output logic        m_req_o,        //! op request, held until m_gnt_i
    output logic        m_we_o,         //! 1 = commit, 0 = restore
    output logic  [7:0] m_rid_o,        //! record id
    output logic        m_wvalid_o,     //! commit byte present
    output logic  [7:0] m_wdata_o,      //! commit byte
    output logic        m_rready_o,     //! restore byte accepted
    output logic        m_abort_o,      //! abandon the READ it owns

    //! ---- restore verdicts (levels) ------------------------------------------
    output logic        done_o,         //! COMPLETE (or, later, DEFAULTS)
    output logic        fail_o,         //! the restore failed, any terminal
    output logic        closed_o,       //! CLOSED: fail, never done, own kept
    //! the first abort's cause: 0 none, 3 the deadline, 7 image not proven
    output logic  [2:0] cause_o
);

  // ---- the elaboration guard ------------------------------------------------
  //! zero would wrap RS_TMO_CYC_P - 1 below into a 2^32-clock deadline
  if (RS_TMO_CYC_P < 1) begin : g_rs_tmo_check
    $error("KL_aecp_nvm_writer: RS_TMO_CYC_P must be at least 1");
  end

  // ---- the descriptor store's state-bus regions (KL_aecp_desc_store) -------
  localparam logic [19:0] ADDR_LOCATE_C = 20'hF_0000;  //! region 0xF: LOCATE
  //! the LOCATE key {descriptor_index, descriptor_type, configuration_index}:
  //! ENTITY 0 of configuration 0, the descriptor every image carries
  localparam logic [63:0] KEY_ENTITY0_C = 64'd0;

  // ---- abort causes (cause_o) ------------------------------------------------
  localparam logic [2:0] CAUSE_NONE_C     = 3'd0;
  localparam logic [2:0] CAUSE_DEADLINE_C = 3'd3;
  localparam logic [2:0] CAUSE_IMAGE_C    = 3'd7;

  typedef enum logic [2:0] {
    W_WAITGO,   // own from reset: wait for the binding walk's end
    W_IMG,      // is the image validated? else LOCATE ENTITY 0
    W_IMGLOC,   // the LOCATE is held until the store answers
    W_COMPLETE, // terminal: done, own released
    W_CLOSED    // terminal: fail, never done, own kept until reset
  } wstate_e;

  wstate_e    ws_r;
  logic       done_r, fail_r, closed_r;
  logic [2:0] cause_r;

  // ---- the deadline ------------------------------------------------------------
  //! consecutive cycles a restore wait passes without its event. Only the
  //! image LOCATE waits in this stage; its event is the store's answer.
  logic [31:0] wd_r;
  logic        stall_w, expire_w;

  assign stall_w  = (ws_r == W_IMGLOC) && !sb_rvalid_i;
  assign expire_w = stall_w && (wd_r >= 32'(RS_TMO_CYC_P - 1));

  always_ff @(posedge clk_i) begin : deadline_ff
    if (!rst_n)                   wd_r <= 32'd0;
    else if (stall_w && !expire_w) wd_r <= wd_r + 32'd1;
    else                          wd_r <= 32'd0;
  end

  // ---- the restore ------------------------------------------------------------
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

  // ---- the faces ----------------------------------------------------------------
  //! released only by a done terminal: COMPLETE here
  assign own_o = !done_r;

  assign sb_req_o   = (ws_r == W_IMGLOC);
  assign sb_we_o    = 1'b0;
  assign sb_addr_o  = ADDR_LOCATE_C;
  assign sb_wdata_o = KEY_ENTITY0_C;

  assign m_req_o    = 1'b0;
  assign m_we_o     = 1'b0;
  assign m_rid_o    = 8'd0;
  assign m_wvalid_o = 1'b0;
  assign m_wdata_o  = 8'd0;
  assign m_rready_o = 1'b0;
  assign m_abort_o  = 1'b0;

  assign done_o   = done_r;
  assign fail_o   = fail_r;
  assign closed_o = closed_r;
  assign cause_o  = cause_r;

endmodule

`default_nettype wire
