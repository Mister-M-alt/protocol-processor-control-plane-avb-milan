/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : KL_aecp_engine.sv
//  Project     : IEEE 1722.1 protocol processor
//                (docs/architecture/06 in full, 03 §4 dispatch record,
//                 03 §7 response buffers, 03 §8 TX arbitration + addressing)
//
//  Description : The AECP command path made whole: it POPS the dispatch
//                queue's AECP head, decodes the opcode into a µPC entry and
//                operands, runs KL_aecp_ucpu against KL_aecp_desc_store, and
//                turns the µCPU's response buffer into a byte-exact AECPDU on
//                the TX slot pool. Before this block the AECP head was handed
//                out through a top-level pop face that nothing popped.
//
//                THE DISPATCH DECISION (06 §4 specifies a dispatch ROM per
//                opcode; 06 §8 fixes its 48-bit entry shape but the tree ships
//                no ROM and no generator for it, so this is a documented
//                CHOICE, not an implementation of a written table). What is
//                actually needed today is the µPC entry, so this block uses a
//                DIRECT OPCODE DECODE — three arms, constant-folded, no ROM:
//
//                  0x0004 READ_DESCRIPTOR      -> UPC_RDESC_C  (real answer)
//                  0x0026 IDENTIFY_NOTIFICATION-> UPC_BADARG_C (IEEE §7.4.39.2:
//                                                 as a COMMAND it is
//                                                 BAD_ARGUMENTS — the
//                                                 opcode-specific rule beats
//                                                 §9.3.5.3.3's fallback)
//                  everything else             -> UPC_NOTIMPL_C
//
//                A ROM is the right shape once the hazard class, min-cdl,
//                response-size id, lock/GDI/notify flags and per-profile valid
//                bits of the 06 §8 entry have consumers; three arms of decode
//                is the right shape while they do not, and it cannot go stale
//                against a generator that does not exist. When the ROM lands
//                it replaces `upc_w`/`echo_w` here and nothing else.
//
//                NOT_IMPLEMENTED IS AN ANSWER, NOT SILENCE. Every opcode this
//                block does not implement is ECHOED back with
//                message_type + 1 and status NOT_IMPLEMENTED (F06.14 "echo
//                command", IEEE §9.3.5.3.3): the command payload is copied out
//                of the RX slot into the response buffer BEFORE the µprogram
//                runs, and the emitted payload length is the command's. So an
//                unimplemented opcode produces a well-formed, correctly-sized
//                AECPDU — never a dropped frame and never a malformed one.
//
//                WHO OWNS THE WIRE HEADER. The µCPU's BUILD_HEADER writes a
//                compact {target_eid, seq, status} record into response bytes
//                0..11 and its cursor starts at 12; that record is NOT the
//                24-byte AECPDU header (06 §8's skeleton never claimed to be
//                wire-accurate). This block owns the real header and
//                synthesises it from the 03 §4 transaction plus
//                `resp_status_o`, so response byte 12+k maps to AECPDU byte
//                24+k and `resp_len_o - 12` is the payload length. Response
//                bytes 0..11 are ignored.
//
//                RESPONSE-BUFFER BYTE ORDER. The `rb_*` face carries a field
//                VALUE right-justified in `rb_wdata_o` with a low-contiguous
//                `rb_wstrb_o` giving its WIDTH (1/2/4 bytes). Those m bytes are
//                placed BIG-ENDIAN at `rb_addr_o` here, which is the IEEE
//                1722.1 wire order of every AEM field. The placement rule has
//                to live in the buffer because the µISA has no byte-swap
//                operation — 06 §8 never states an order, and tb/ucpu's C++
//                buffer model uses the opposite (little-endian) convention for
//                its own convenience. A 64-bit field arrives as two 4-byte
//                writes, high word first, so it comes out big-endian too, and
//                COPY_BUFFER lanes arrive in wire order from the store.
//
//                ADDRESSING (03 §8): an AECP response is UNICAST back to the
//                requester's src_mac. A frame shorter than the 60-octet
//                Ethernet minimum is zero-padded; `control_data_length`
//                (= 12 + payload, the offset-from-@12 convention this
//                architecture uses throughout — F06.14's "GET_COUNTERS 160 B,
//                cdl 148") still names the real length.
//
//                WHAT IT REFUSES: an AECP RESPONSE arriving as input
//                (message_type odd) is freed without a reply — this entity
//                originates no AECP command yet, and answering a response is
//                how a control plane builds a storm; and a command whose
//                target_entity_id is not ours is dropped (F06.2 MATCHED arc).
//---------------------------------------------------------------------------//
`default_nettype none

module KL_aecp_engine
  import pp_pkg::*;
  import ucpu_pkg::*;
#(
    //! µcode ROM image (hdl/aecp/ucode/gen_ucode.py). A RELATIVE name resolves
    //! against the TOOL'S RUN DIRECTORY — the parameter exists so an
    //! integrator can hand over an absolute path.
    parameter string       UCODE_HEX_P       = "ucode.hex",
    //! descriptor-image base in the integrator's memory map (compile time)
    parameter logic [31:0] DESC_BASE_P       = 32'h2000_0000,
    parameter int unsigned LINE_BYTES_P      = 576,
    parameter int unsigned IDX_ENTRIES_P     = 32,
    parameter int unsigned NAME_ENTRIES_P    = 16,
    parameter int unsigned MEM_TIMEOUT_CYC_P = 4096,
    //! RX/TX pool geometry (03 §2) — mirrors protocol_processor_top
    parameter int unsigned RX_SLOTS_P          = 4,
    parameter int unsigned RX_SLOT_BYTES_P     = 576,
    parameter int unsigned TX_STD_SLOTS_P      = 4,
    parameter int unsigned TX_STD_BYTES_P      = 576,
    parameter int unsigned TX_OVERSIZE_BYTES_P = 1600,
    //! derived — do not override
    localparam int unsigned RXS_W_C  = (RX_SLOTS_P > 1) ? $clog2(RX_SLOTS_P) : 1,
    localparam int unsigned RXA_W_C  = $clog2(RX_SLOT_BYTES_P),
    localparam int unsigned RXL_W_C  = $clog2(RX_SLOT_BYTES_P + 1),
    localparam int unsigned TXS_W_C  = $clog2(TX_STD_SLOTS_P + 1),
    localparam int unsigned TXA_W_C  = $clog2(TX_OVERSIZE_BYTES_P + 1)
) (
    input  wire         clk_i,                //! core clock (P-CLK-HZ domain)
    input  wire         rst_n,                //! synchronous active-low reset

    //! ---- identity (02 §2 rule 4 quasi-static) ----
    input  wire  [63:0] entity_id_i,          //! own entity_id (F06.2 MATCHED)
    input  wire  [47:0] own_mac_i,            //! response source address

    //! ---- dispatch pop face (03 §4 record, KL_pp_dispatch AECP queue) ----
    input  wire                    txn_valid_i,  //! head record valid
    input  wire  [PP_TXN_W_C-1:0]  txn_i,        //! head pp_txn_t record
    output logic                   txn_ready_o,  //! engine consumes the head

    //! ---- RX slot read + free (its own lock-step pool replica) ----
    output logic [RXS_W_C-1:0] rxs_rd_slot_o,  //! payload slot to read
    output logic [RXA_W_C-1:0] rxs_rd_addr_o,  //! byte address in the slot
    output logic               rxs_rd_en_o,    //! sync-read enable
    input  wire  [7:0]         rxs_rd_data_i,  //! byte, one cycle later
    input  wire  [RXL_W_C-1:0] rxs_slot_len_i, //! committed slot length
    output logic               rxs_free_o,     //! return the consumed slot
    output logic [RXS_W_C-1:0] rxs_free_slot_o,//! which slot

    //! ---- TX slot pool client (03 §7) ----
    output logic               txs_alloc_req_o,//! slot request, held until grant
    output logic               txs_oversize_o, //! Δ8 oversize slot needed
    input  wire                txs_alloc_gnt_i,//! one-cycle grant
    input  wire  [TXS_W_C-1:0] txs_alloc_slot_i,
    output logic [TXS_W_C-1:0] txs_wr_slot_o,
    output logic [TXA_W_C-1:0] txs_wr_addr_o,
    output logic               txs_wr_valid_o,
    output logic [7:0]         txs_wr_data_o,
    output logic               txs_wr_commit_o,
    output logic [TXA_W_C-1:0] txs_wr_len_o,

    //! ---- TX arbiter lane (F03.5 LANE_AECP_SOL); held until granted ----
    output logic               txreq_valid_o,
    output logic [TXS_W_C-1:0] txreq_slot_o,
    input  wire                txreq_ready_i,

    //! ---- descriptor memory master (read-only; see KL_aecp_desc_store) ----
    output logic        mem_req_valid_o,
    input  wire         mem_req_ready_i,
    output logic [31:0] mem_req_addr_o,
    output logic  [8:0] mem_req_beats_o,
    input  wire         mem_rsp_valid_i,
    output logic        mem_rsp_ready_o,
    input  wire  [63:0] mem_rsp_data_i,
    input  wire         mem_rsp_last_i,
    input  wire         mem_rsp_err_i,

    //! ---- lock context (06 §6.4; the lock manager is P4 — tie 0) ----
    input  wire         lock_held_i,
    input  wire  [63:0] lock_ctlr_i,

    //! ---- effect strobes (06 §8; consumers are P4) ----
    output logic        eff_commit_o,
    output logic  [7:0] eff_nvm_mark_o,
    output logic        eff_nvm_stb_o,
    output logic  [3:0] eff_notify_class_o,
    output logic        eff_notify_stb_o,

    //! ---- observability ----
    output logic        dbg_busy_o,          //! a command is in flight
    output logic [15:0] dbg_cmd_cnt_o,       //! commands accepted
    output logic [15:0] dbg_resp_cnt_o,      //! responses committed
    output logic [15:0] dbg_drop_cnt_o,      //! frames freed without a reply
    output logic  [4:0] dbg_status_o,        //! status of the last response
    output logic [10:0] dbg_len_o,           //! AECPDU length of the last response
    output logic        dbg_img_valid_o,     //! store: header validated
    output logic  [3:0] dbg_img_fault_o,     //! store: why not
    output logic [15:0] dbg_locate_miss_o    //! store: locates answered err
);

  // ---- IEEE 1722.1-2021 AEM opcodes this block decodes --------------------
  localparam logic [15:0] OP_READ_DESCRIPTOR_C = 16'h0004;
  localparam logic [15:0] OP_IDENTIFY_NOTIF_C  = 16'h0026;

  // ---- µPC entry points (hdl/aecp/ucode/gen_ucode.py) ---------------------
  localparam logic [10:0] UPC_NOTIMPL_C = 11'd560;   // E_NOTIMPL
  localparam logic [10:0] UPC_RDESC_C   = 11'd640;   // E_RDESC
  localparam logic [10:0] UPC_BADARG_C  = 11'd704;   // E_BADARG

  // ---- geometry -----------------------------------------------------------
  //! header 14 (Ethernet) + 24 (AECPDU) before the first payload byte
  localparam int unsigned ETH_HDR_C    = 14;
  localparam int unsigned AECP_HDR_C   = 24;
  localparam int unsigned FRAME_HDR_C  = ETH_HDR_C + AECP_HDR_C;      // 38
  localparam int unsigned ETH_MIN_C    = 60;
  //! response buffer: the µCPU's 12 header bytes + the 4-byte
  //! {configuration_index, reserved} prefix + one whole descriptor, rounded to
  //! a 4-byte bank stride
  localparam int unsigned RESP_BUF_C   = ((16 + LINE_BYTES_P) + 15) & ~32'd15;
  localparam int unsigned BANK_D_C     = RESP_BUF_C / 4;
  localparam int unsigned BANK_AW_C    = $clog2(BANK_D_C);
  localparam int unsigned PLD_MAX_C    = RESP_BUF_C - 12;
  localparam int unsigned FRAME_MAX_C  = FRAME_HDR_C + PLD_MAX_C;

  if (FRAME_MAX_C > TX_OVERSIZE_BYTES_P) begin : gen_g_frame_fit
    $error("a maximum AECP response (%0d B) exceeds the oversize slot (%0d B)",
           FRAME_MAX_C, TX_OVERSIZE_BYTES_P);
  end

  pp_txn_t txn_w;
  assign txn_w = pp_txn_t'(txn_i);

  // =======================================================================
  // the response buffer: four byte banks, one per address[1:0]
  // =======================================================================
  //! A µCPU write covers at most 4 CONSECUTIVE byte addresses, so it touches
  //! each bank at most once — which is what makes a 4-bank split legal for an
  //! unaligned strobed write (BUILD_FIELD writes a byte field at any cursor).
  logic [7:0] rbuf_r [0:3][0:BANK_D_C-1];
  logic [3:0]              bw_en_w;
  logic [3:0][BANK_AW_C-1:0] bw_addr_w;
  logic [3:0][7:0]         bw_data_w;

  always_ff @(posedge clk_i) begin : resp_buffer
    for (int unsigned b = 0; b < 4; b++) begin
      if (bw_en_w[b]) rbuf_r[b][bw_addr_w[b]] <= bw_data_w[b];
    end
  end

  // ---- µCPU response-buffer face ------------------------------------------
  logic        rb_we_w;
  logic  [9:0] rb_addr_w;
  logic [31:0] rb_wdata_w;
  logic  [3:0] rb_wstrb_w;

  logic [2:0] rb_m_w;                    // strobed width in bytes (0/1/2/4)
  always_comb begin : strobe_width
    if      (rb_wstrb_w[3]) rb_m_w = 3'd4;
    else if (rb_wstrb_w[1]) rb_m_w = 3'd2;
    else if (rb_wstrb_w[0]) rb_m_w = 3'd1;
    else                    rb_m_w = 3'd0;
  end

  // ---- the engine's own byte write (echo pre-load) ------------------------
  logic       ebw_en_r;
  logic [9:0] ebw_addr_r;
  logic [7:0] ebw_data_r;

  always_comb begin : buffer_write_mux
    logic  [1:0] j;
    logic [10:0] a;
    j         = 2'd0;
    a         = 11'd0;
    bw_en_w   = 4'd0;
    bw_addr_w = '0;
    bw_data_w = '0;
    if (rb_we_w) begin
      for (int unsigned b = 0; b < 4; b++) begin
        //! j = the lane of this write that lands in bank b; a write spans at
        //! most 4 CONSECUTIVE addresses, so each bank is touched at most once
        j = 2'(b) - rb_addr_w[1:0];
        a = {1'b0, rb_addr_w} + {9'd0, j};
        if (({1'b0, j} < rb_m_w) && (a < 11'(RESP_BUF_C))) begin
          bw_en_w[b]   = 1'b1;
          bw_addr_w[b] = BANK_AW_C'(a >> 2);
          //! BIG-ENDIAN placement of the m strobed bytes (see the banner)
          unique case (rb_m_w)
            3'd1: bw_data_w[b] = rb_wdata_w[7:0];
            3'd2: bw_data_w[b] = (j == 2'd0) ? rb_wdata_w[15:8]
                                             : rb_wdata_w[7:0];
            default: unique case (j)
                       2'd0:    bw_data_w[b] = rb_wdata_w[31:24];
                       2'd1:    bw_data_w[b] = rb_wdata_w[23:16];
                       2'd2:    bw_data_w[b] = rb_wdata_w[15:8];
                       default: bw_data_w[b] = rb_wdata_w[7:0];
                     endcase
          endcase
        end
      end
    end else if (ebw_en_r && (ebw_addr_r < 10'(RESP_BUF_C))) begin
      bw_en_w[ebw_addr_r[1:0]]   = 1'b1;
      bw_addr_w[ebw_addr_r[1:0]] = BANK_AW_C'(ebw_addr_r >> 2);
      bw_data_w[ebw_addr_r[1:0]] = ebw_data_r;
    end
  end

  logic  [9:0] rd_byte_addr_w;
  logic  [7:0] rd_byte_w;
  assign rd_byte_w = rbuf_r[rd_byte_addr_w[1:0]][BANK_AW_C'(rd_byte_addr_w >> 2)];

  // =======================================================================
  // the command machine
  // =======================================================================
  typedef enum logic [3:0] {
    A_IDLE, A_PLD, A_DISP, A_RUN, A_ALLOC, A_WR, A_CMT, A_TXW, A_FREE
  } a_st_e;
  a_st_e a_st_r;

  pp_txn_t     cmd_r;
  logic [15:0] raw_ct_r;                 // AECPDU @22..@23 echoed verbatim
  logic [15:0] cfg_ix_r, desc_ty_r, desc_ix_r;
  logic [10:0] pld_cmd_r;                // command payload bytes
  logic [10:0] pld_r;                    // payload bytes to emit
  logic [10:0] walk_r;                   // payload walk index
  logic        echo_r, sent_r;
  logic [10:0] upc_r;
  logic [4:0]  status_r;
  logic [10:0] bidx_r;                   // frame byte being written
  logic [10:0] frame_len_r;
  logic [TXS_W_C-1:0] tx_slot_r;
  logic [15:0] cmd_cnt_r, resp_cnt_r, drop_cnt_r;

  // ---- opcode decode = the dispatch step (see the banner) -----------------
  logic [10:0] upc_w;
  logic        echo_w, short_w;
  //! a READ_DESCRIPTOR must carry configuration_index + reserved +
  //! descriptor_type + descriptor_index; a shorter one is BAD_ARGUMENTS, never
  //! a locate of whatever zeros happened to be there
  assign short_w = (txn_w.cdl < 11'd20);
  always_comb begin : dispatch_decode
    if ((txn_w.opcode == OP_READ_DESCRIPTOR_C) && !short_w) begin
      upc_w  = UPC_RDESC_C;
      echo_w = 1'b0;
    end else if ((txn_w.opcode == OP_IDENTIFY_NOTIF_C)
                 || ((txn_w.opcode == OP_READ_DESCRIPTOR_C) && short_w)) begin
      upc_w  = UPC_BADARG_C;
      echo_w = 1'b1;
    end else begin
      upc_w  = UPC_NOTIMPL_C;
      echo_w = 1'b1;
    end
  end

  //! F06.2 MATCHED arc + the response-storm guard (see the banner)
  logic drop_w;
  assign drop_w = txn_w.msg_type[0] || (txn_w.target_eid != entity_id_i);

  // ---- payload sizing ------------------------------------------------------
  //! cdl is the offset-from-@12 length (F06.14), so the command payload is
  //! cdl - 12; the buffer cap and the committed slot length are the two hard
  //! ceilings. The slot arm can only be read once `rxs_rd_slot_o` names this
  //! command's slot (`slot_len_o` is indexed by `rd_slot_i`), so it is applied
  //! on the first walk cycle rather than at pop.
  //! An RX slot holds the AVTPDU, NOT the Ethernet frame: KL_pp_rx_validator
  //! starts writing at frame byte 14, so slot byte k IS AECPDU byte k and the
  //! payload begins at slot byte 24.
  logic [10:0] cdl_pld_w, slot_pld_w, pld_cap_w, pld_trim_w;
  assign cdl_pld_w  = (txn_w.cdl > 11'd12) ? (txn_w.cdl - 11'd12) : 11'd0;
  assign pld_cap_w  = (cdl_pld_w > 11'(PLD_MAX_C)) ? 11'(PLD_MAX_C) : cdl_pld_w;
  assign slot_pld_w = (32'(rxs_slot_len_i) > 32'(AECP_HDR_C))
                      ? 11'(32'(rxs_slot_len_i) - 32'(AECP_HDR_C)) : 11'd0;
  assign pld_trim_w = (pld_r > slot_pld_w) ? slot_pld_w : pld_r;

  // ---- the µCPU ------------------------------------------------------------
  logic        disp_valid_r;
  logic        disp_ready_w, ucpu_busy_w, ucpu_done_w;
  logic        resp_send_w;
  logic [10:0] resp_len_w;
  logic  [4:0] resp_status_w;
  logic [63:0] opd0_w, opd1_w;

  //! r14 = {--, descriptor_index, descriptor_type, configuration_index}: the
  //! low 16 bits are the field BUILD_FIELD emits at @24 and [47:0] is the
  //! store's locate key. r13 = {--, descriptor_type, descriptor_index}: the
  //! 4-byte {type, index} stub IEEE §7.4.5 wants on a failed READ_DESCRIPTOR
  //! (the µISA has no shift, so every field a µprogram emits has to arrive
  //! right-justified in some register).
  assign opd0_w = {16'd0, desc_ix_r, desc_ty_r, cfg_ix_r};
  assign opd1_w = {32'd0, desc_ty_r, desc_ix_r};

  logic        st_req_w, st_we_w, st_name_w;
  logic [19:0] st_addr_w;
  logic [63:0] st_wdata_w, st_rdata_w;
  logic  [7:0] st_wstrb_w;
  logic        st_ready_w, st_rvalid_w, st_err_w;
  logic        gx_req_nc_w;
  logic  [7:0] gx_sel_nc_w;
  logic        ucpu_ovf_nc_w;
  logic [10:0] ucpu_upc_nc_w;
  logic  [4:0] ucpu_st_nc_w;

  KL_aecp_ucpu #(
      .UCODE_HEX_P (UCODE_HEX_P)
  ) u_ucpu (
      .clk_i              (clk_i),
      .rst_n              (rst_n),
      .disp_valid_i       (disp_valid_r),
      .disp_ready_o       (disp_ready_w),
      .disp_upc_i         (upc_r),
      .disp_ctlr_eid_i    (cmd_r.controller_eid),
      .disp_opd0_i        (opd0_w),
      .disp_opd1_i        (opd1_w),
      .st_req_o           (st_req_w),
      .st_we_o            (st_we_w),
      .st_name_o          (st_name_w),
      .st_addr_o          (st_addr_w),
      .st_wdata_o         (st_wdata_w),
      .st_wstrb_o         (st_wstrb_w),
      .st_ready_i         (st_ready_w),
      .st_rvalid_i        (st_rvalid_w),
      .st_rdata_i         (st_rdata_w),
      .st_err_i           (st_err_w),
      .gx_req_o           (gx_req_nc_w),
      .gx_sel_o           (gx_sel_nc_w),
      //! the 06 §6.2/§6.6 gather bus has no sources this phase: every
      //! µprogram that would use it is NOT_IMPLEMENTED, so an always-valid
      //! zero answer can never reach a response — and a never-valid one
      //! would hang the µCPU instead of retiring the command.
      .gx_valid_i         (1'b1),
      .gx_data_i          (64'd0),
      .lock_held_i        (lock_held_i),
      .lock_ctlr_i        (lock_ctlr_i),
      .rb_we_o            (rb_we_w),
      .rb_addr_o          (rb_addr_w),
      .rb_wdata_o         (rb_wdata_w),
      .rb_wstrb_o         (rb_wstrb_w),
      .resp_send_o        (resp_send_w),
      .resp_len_o         (resp_len_w),
      .resp_status_o      (resp_status_w),
      //! the buffer is ours and always able to take the response: the µCPU
      //! never waits on TX, the engine does
      .tx_ready_i         (1'b1),
      .eff_commit_o       (eff_commit_o),
      .eff_nvm_mark_o     (eff_nvm_mark_o),
      .eff_nvm_stb_o      (eff_nvm_stb_o),
      .eff_notify_class_o (eff_notify_class_o),
      .eff_notify_stb_o   (eff_notify_stb_o),
      .busy_o             (ucpu_busy_w),
      .done_o             (ucpu_done_w),
      .dbg_upc_o          (ucpu_upc_nc_w),
      .dbg_status_o       (ucpu_st_nc_w),
      .dbg_ovf_o          (ucpu_ovf_nc_w)
  );

  logic [15:0] store_fetch_nc_w, store_rowr_nc_w, store_dlen_nc_w;

  KL_aecp_desc_store #(
      .DESC_BASE_P       (DESC_BASE_P),
      .LINE_BYTES_P      (LINE_BYTES_P),
      .IDX_ENTRIES_P     (IDX_ENTRIES_P),
      .NAME_ENTRIES_P    (NAME_ENTRIES_P),
      .MEM_TIMEOUT_CYC_P (MEM_TIMEOUT_CYC_P)
  ) u_store (
      .clk_i             (clk_i),
      .rst_n             (rst_n),
      .st_req_i          (st_req_w),
      .st_we_i           (st_we_w),
      .st_name_i         (st_name_w),
      .st_addr_i         (st_addr_w),
      .st_wdata_i        (st_wdata_w),
      .st_wstrb_i        (st_wstrb_w),
      .st_ready_o        (st_ready_w),
      .st_rvalid_o       (st_rvalid_w),
      .st_rdata_o        (st_rdata_w),
      .st_err_o          (st_err_w),
      .mem_req_valid_o   (mem_req_valid_o),
      .mem_req_ready_i   (mem_req_ready_i),
      .mem_req_addr_o    (mem_req_addr_o),
      .mem_req_beats_o   (mem_req_beats_o),
      .mem_rsp_valid_i   (mem_rsp_valid_i),
      .mem_rsp_ready_o   (mem_rsp_ready_o),
      .mem_rsp_data_i    (mem_rsp_data_i),
      .mem_rsp_last_i    (mem_rsp_last_i),
      .mem_rsp_err_i     (mem_rsp_err_i),
      .dbg_img_valid_o   (dbg_img_valid_o),
      .dbg_fault_o       (dbg_img_fault_o),
      .dbg_locate_miss_o (dbg_locate_miss_o),
      .dbg_fetch_cnt_o   (store_fetch_nc_w),
      .dbg_ro_write_o    (store_rowr_nc_w),
      .dbg_desc_len_o    (store_dlen_nc_w)
  );

  // =======================================================================
  // frame assembly: byte `bidx_r` of the response
  // =======================================================================
  logic [15:0] cdl_w;
  assign cdl_w = 16'd12 + {5'd0, pld_r};

  logic [3:0] resp_mt_w;
  assign resp_mt_w = cmd_r.msg_type | 4'd1;   // COMMAND -> its RESPONSE type

  logic [7:0] hdr_byte_w;
  always_comb begin : frame_header_byte
    unique case (bidx_r[5:0])
      6'd0:  hdr_byte_w = cmd_r.src_mac[47:40];
      6'd1:  hdr_byte_w = cmd_r.src_mac[39:32];
      6'd2:  hdr_byte_w = cmd_r.src_mac[31:24];
      6'd3:  hdr_byte_w = cmd_r.src_mac[23:16];
      6'd4:  hdr_byte_w = cmd_r.src_mac[15:8];
      6'd5:  hdr_byte_w = cmd_r.src_mac[7:0];
      6'd6:  hdr_byte_w = own_mac_i[47:40];
      6'd7:  hdr_byte_w = own_mac_i[39:32];
      6'd8:  hdr_byte_w = own_mac_i[31:24];
      6'd9:  hdr_byte_w = own_mac_i[23:16];
      6'd10: hdr_byte_w = own_mac_i[15:8];
      6'd11: hdr_byte_w = own_mac_i[7:0];
      6'd12: hdr_byte_w = 8'h22;                        // EtherType 0x22F0
      6'd13: hdr_byte_w = 8'hF0;
      6'd14: hdr_byte_w = 8'hFB;                        // AVTP subtype AECP
      6'd15: hdr_byte_w = {4'b0000, resp_mt_w};         // sv=0, version=0
      6'd16: hdr_byte_w = {status_r, cdl_w[10:8]};
      6'd17: hdr_byte_w = cdl_w[7:0];
      6'd18: hdr_byte_w = entity_id_i[63:56];           // target_entity_id
      6'd19: hdr_byte_w = entity_id_i[55:48];
      6'd20: hdr_byte_w = entity_id_i[47:40];
      6'd21: hdr_byte_w = entity_id_i[39:32];
      6'd22: hdr_byte_w = entity_id_i[31:24];
      6'd23: hdr_byte_w = entity_id_i[23:16];
      6'd24: hdr_byte_w = entity_id_i[15:8];
      6'd25: hdr_byte_w = entity_id_i[7:0];
      6'd26: hdr_byte_w = cmd_r.controller_eid[63:56];
      6'd27: hdr_byte_w = cmd_r.controller_eid[55:48];
      6'd28: hdr_byte_w = cmd_r.controller_eid[47:40];
      6'd29: hdr_byte_w = cmd_r.controller_eid[39:32];
      6'd30: hdr_byte_w = cmd_r.controller_eid[31:24];
      6'd31: hdr_byte_w = cmd_r.controller_eid[23:16];
      6'd32: hdr_byte_w = cmd_r.controller_eid[15:8];
      6'd33: hdr_byte_w = cmd_r.controller_eid[7:0];
      6'd34: hdr_byte_w = cmd_r.sequence_id[15:8];
      6'd35: hdr_byte_w = cmd_r.sequence_id[7:0];
      //! u = 0 (solicited); the rest of command_type is echoed from the wire,
      //! which keeps a VENDOR_UNIQUE response's protocol_id bytes intact
      6'd36: hdr_byte_w = {1'b0, raw_ct_r[14:8]};
      6'd37: hdr_byte_w = raw_ct_r[7:0];
      default: hdr_byte_w = 8'd0;
    endcase
  end

  //! payload byte n of the response lives at buffer address 12 + n; the guard
  //! keeps the index inside the buffer while the header bytes are streaming
  //! (the value is unused there, but an out-of-range RAM index is not a thing
  //! to leave to chance)
  assign rd_byte_addr_w = (bidx_r >= 11'(FRAME_HDR_C))
                          ? (10'd12 + (bidx_r[9:0] - 10'(FRAME_HDR_C)))
                          : 10'd12;

  logic [7:0] frame_byte_w;
  always_comb begin : frame_byte
    if (bidx_r < 11'(FRAME_HDR_C))                 frame_byte_w = hdr_byte_w;
    else if (bidx_r < (11'(FRAME_HDR_C) + pld_r))  frame_byte_w = rd_byte_w;
    else                                           frame_byte_w = 8'd0; // pad
  end

  // ---- RX payload walk -----------------------------------------------------
  //! the walk starts at AECPDU @22 (= slot byte 22) so bytes @22..@23 are
  //! captured verbatim; payload byte n is walk index n+2
  assign rxs_rd_slot_o = cmd_r.rx_slot[RXS_W_C-1:0];
  assign rxs_rd_addr_o = RXA_W_C'(32'd22 + 32'(walk_r));
  assign rxs_rd_en_o   = (a_st_r == A_PLD) && (walk_r < (pld_r + 11'd2));

  assign txn_ready_o     = (a_st_r == A_IDLE);
  assign txs_alloc_req_o = (a_st_r == A_ALLOC);
  assign txs_oversize_o  = (frame_len_r > 11'(TX_STD_BYTES_P));
  assign txs_wr_slot_o   = tx_slot_r;
  assign txs_wr_addr_o   = TXA_W_C'(bidx_r);
  assign txs_wr_valid_o  = (a_st_r == A_WR);
  assign txs_wr_data_o   = frame_byte_w;
  assign txs_wr_commit_o = (a_st_r == A_CMT);
  assign txs_wr_len_o    = TXA_W_C'(frame_len_r);
  assign txreq_valid_o   = (a_st_r == A_TXW);
  assign txreq_slot_o    = tx_slot_r;
  assign rxs_free_o      = (a_st_r == A_FREE)
                           && (cmd_r.rx_slot != PP_SLOT_NULL_C);
  assign rxs_free_slot_o = cmd_r.rx_slot[RXS_W_C-1:0];

  assign dbg_busy_o     = (a_st_r != A_IDLE) || ucpu_busy_w;
  assign dbg_cmd_cnt_o  = cmd_cnt_r;
  assign dbg_resp_cnt_o = resp_cnt_r;
  assign dbg_drop_cnt_o = drop_cnt_r;
  assign dbg_status_o   = status_r;
  assign dbg_len_o      = frame_len_r;

  logic [10:0] pad_len_w;
  assign pad_len_w = (11'(FRAME_HDR_C) + pld_r < 11'(ETH_MIN_C))
                     ? 11'(ETH_MIN_C) : (11'(FRAME_HDR_C) + pld_r);

  always_ff @(posedge clk_i) begin : command_machine
    if (!rst_n) begin
      a_st_r       <= A_IDLE;
      cmd_r        <= '0;
      raw_ct_r     <= 16'd0;
      cfg_ix_r     <= 16'd0;
      desc_ty_r    <= 16'd0;
      desc_ix_r    <= 16'd0;
      pld_cmd_r    <= 11'd0;
      pld_r        <= 11'd0;
      walk_r       <= 11'd0;
      echo_r       <= 1'b0;
      upc_r        <= 11'd0;
      status_r     <= 5'd0;
      bidx_r       <= 11'd0;
      frame_len_r  <= 11'd0;
      tx_slot_r    <= '0;
      disp_valid_r <= 1'b0;
      ebw_en_r     <= 1'b0;
      ebw_addr_r   <= 10'd0;
      ebw_data_r   <= 8'd0;
      sent_r       <= 1'b0;
      cmd_cnt_r    <= 16'd0;
      resp_cnt_r   <= 16'd0;
      drop_cnt_r   <= 16'd0;
    end else begin
      ebw_en_r <= 1'b0;

      unique case (a_st_r)
        A_IDLE: begin
          if (txn_valid_i) begin
            cmd_r <= txn_w;
            if (drop_w) begin
              if (drop_cnt_r != 16'hFFFF) drop_cnt_r <= drop_cnt_r + 16'd1;
              a_st_r <= A_FREE;
            end else begin
              if (cmd_cnt_r != 16'hFFFF) cmd_cnt_r <= cmd_cnt_r + 16'd1;
              upc_r     <= upc_w;
              echo_r    <= echo_w;
              pld_cmd_r <= pld_cap_w;
              pld_r     <= pld_cap_w;
              cfg_ix_r  <= 16'd0;
              desc_ty_r <= 16'd0;
              desc_ix_r <= 16'd0;
              raw_ct_r  <= 16'd0;
              walk_r    <= 11'd0;
              sent_r    <= 1'b0;
              a_st_r    <= (txn_w.rx_slot == PP_SLOT_NULL_C) ? A_DISP : A_PLD;
            end
          end
        end

        // ---- copy the command payload out of the RX slot -----------------
        A_PLD: begin
          if (walk_r == 11'd0) begin
            pld_r     <= pld_trim_w;      // the committed-slot ceiling
            pld_cmd_r <= pld_trim_w;
          end
          if (walk_r < (pld_r + 11'd2)) begin
            walk_r <= walk_r + 11'd1;
          end else begin
            a_st_r <= A_DISP;
          end
          //! the RX pool answers one cycle after rd_en: byte for index
          //! walk_r-1 lands now
          if (walk_r != 11'd0) begin
            unique case (walk_r - 11'd1)
              11'd0: raw_ct_r[15:8]  <= rxs_rd_data_i;
              11'd1: raw_ct_r[7:0]   <= rxs_rd_data_i;
              11'd2: cfg_ix_r[15:8]  <= rxs_rd_data_i;
              11'd3: cfg_ix_r[7:0]   <= rxs_rd_data_i;
              11'd6: desc_ty_r[15:8] <= rxs_rd_data_i;
              11'd7: desc_ty_r[7:0]  <= rxs_rd_data_i;
              11'd8: desc_ix_r[15:8] <= rxs_rd_data_i;
              11'd9: desc_ix_r[7:0]  <= rxs_rd_data_i;
              default: ;
            endcase
            if ((walk_r - 11'd1) >= 11'd2) begin
              ebw_en_r   <= 1'b1;
              ebw_addr_r <= 10'd12 + 10'(walk_r - 11'd3);
              ebw_data_r <= rxs_rd_data_i;
            end
          end
        end

        A_DISP: begin
          disp_valid_r <= 1'b1;
          if (disp_valid_r && disp_ready_w) begin
            disp_valid_r <= 1'b0;
            a_st_r       <= A_RUN;
          end
        end

        A_RUN: begin
          if (resp_send_w) begin
            sent_r   <= 1'b1;
            status_r <= resp_status_w;
            //! the µCPU owns the payload for a command it really answers; an
            //! echoed one keeps the command's own length (§9.3.5.3.3)
            if (!echo_r) begin
              pld_r <= (resp_len_w > 11'd12)
                       ? ((resp_len_w - 11'd12) > 11'(PLD_MAX_C)
                          ? 11'(PLD_MAX_C) : (resp_len_w - 11'd12))
                       : 11'd0;
            end else begin
              pld_r <= pld_cmd_r;
            end
          end
          if (ucpu_done_w) begin
            bidx_r      <= 11'd0;
            frame_len_r <= pad_len_w;
            //! a µprogram that retired without SEND_RESPONSE has no response
            //! to emit — free the slot rather than send a frame built from
            //! whatever the previous command left behind
            a_st_r      <= (sent_r || resp_send_w) ? A_ALLOC : A_FREE;
            if (!(sent_r || resp_send_w) && (drop_cnt_r != 16'hFFFF)) begin
              drop_cnt_r <= drop_cnt_r + 16'd1;
            end
          end
        end

        A_ALLOC: begin
          if (txs_alloc_gnt_i) begin
            tx_slot_r <= txs_alloc_slot_i;
            a_st_r    <= A_WR;
          end
        end

        A_WR: begin
          if (bidx_r + 11'd1 >= frame_len_r) a_st_r <= A_CMT;
          bidx_r <= bidx_r + 11'd1;
        end

        A_CMT: begin
          if (resp_cnt_r != 16'hFFFF) resp_cnt_r <= resp_cnt_r + 16'd1;
          a_st_r <= A_TXW;
        end

        A_TXW: if (txreq_ready_i) a_st_r <= A_FREE;

        A_FREE: a_st_r <= A_IDLE;

        default: a_st_r <= A_IDLE;
      endcase
    end
  end

endmodule : KL_aecp_engine
`default_nettype wire
