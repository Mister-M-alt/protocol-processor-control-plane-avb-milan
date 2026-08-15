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
//                  0x0029 GET_COUNTERS         -> UPC_GCTRS_C  (real answer)
//                  0x002B GET_AUDIO_MAP        -> UPC_GAMAP_C  (real answer,
//                                                 STREAM_PORT_INPUT only -
//                                                 see the audio-map note)
//                  0x0026 IDENTIFY_NOTIFICATION-> UPC_BADARG_C (IEEE §7.4.39.2:
//                                                 as a COMMAND it is
//                                                 BAD_ARGUMENTS — the
//                                                 opcode-specific rule beats
//                                                 §9.3.5.3.3's fallback)
//                  everything else             -> UPC_NOTIMPL_C
//
//                WHERE GET_COUNTERS PUTS ITS ARGUMENTS. §7.4.42.1 puts
//                descriptor_type at @24 and descriptor_index at @26, where
//                §7.4.5's READ_DESCRIPTOR puts configuration_index and a
//                reserved field. The payload walk therefore captures @26..@27
//                into `desc_ix_r` for a counters command and @28..@31 for
//                everything else, so the SAME two operand registers serve both
//                shapes and neither costs a register of its own. A counters
//                command whose control_data_length is short of the §7.4.42.1
//                four bytes is BAD_ARGUMENTS, on the same reasoning as the
//                truncated READ_DESCRIPTOR: answering ENTITY-index-0 out of
//                whatever zeros happened to be there is a silent
//                misinterpretation, not an answer.
//
//                GET_AUDIO_MAP IS A THIRD SHAPE IN THE SAME REGISTERS.
//                §7.4.44.1 puts descriptor_type at @24, descriptor_index at
//                @26, map_index at @28 and a reserved word at @30, so an
//                `amap_r` command captures @26..@27 into `desc_ix_r` like a
//                counters command and @28..@29 into `desc_ty_r` - which for
//                this one command MEANS map_index - while @30..@31 is walked
//                and kept by nobody (without the guard the reserved word
//                would trample `desc_ix_r` through READ_DESCRIPTOR's arms).
//                A command short of the §7.4.44.1 eight bytes is
//                BAD_ARGUMENTS, same reasoning as above.
//
//                THE COUNTERS FACE IS NOT A COUNTER. This block holds no
//                counters and never will: the events Milan Table 5.6 counts
//                (media lock, sequence mismatch, late/early presentation time)
//                happen in the integrator's stream datapath, cycles and clock
//                domains away from an AEM command parser. So `ctr_*` is a READ
//                face — {descriptor_type, descriptor_index, quadlet} in,
//                32 bits out — and the integrator serves it however its
//                counters already exist: combinationally from flops, or out of
//                the context RAM they already live in, where the quadlet index
//                IS the RAM address and the mux costs nothing.
//
//                `ctr_wait_i` IS ASSERTED TO HOLD, and the polarity is the
//                point: an integrator who has not wired this face leaves it 0,
//                the gather beat completes immediately with `ctr_data_i` = 0,
//                and quadlet 32 — the counters_valid word — reads 0. That is
//                SUCCESS with an EMPTY mask: "this entity keeps no counters for
//                that object", which §7.4.42.2 defines exactly ("a bit that is
//                set means the associated quadlet exists and is valid"). Never
//                a hang, and never a block of zeros advertised as valid.
//
//                THE AUDIO-MAP FACE IS THE SAME BARGAIN (06 §6.5; IEEE
//                §7.4.44, Milan §5.4.2.26). The dynamic mappings live in the
//                integrator's routing fabric - for the reference platform,
//                the render crossbar's map RAM - so `amap_*` is a second READ
//                face on the same gather bus: {descriptor_type,
//                descriptor_index, map_index, record ordinal} in, a
//                geometry word or one §7.4.44.2.1 record out. The bus is
//                routed BY COMMAND (`amap_r`), never by selector value, so
//                the two faces own disjoint commands and the whole selector
//                space each. Milan §5.4.2.26 puts the partition law on the
//                integrator ("disjoint subsets whose size does not exceed
//                176 ... fixed for a given Configuration"), and §7.4.44.1's
//                page rule is the µprogram's: map_index >= number_of_maps is
//                BAD_ARGUMENTS. An unwired face answers number_of_maps 0 for
//                every port, which the µprogram reads as NO_SUCH_DESCRIPTOR
//                only when the descriptor store AGREES the port is absent -
//                the store, not the face, is the existence authority, so
//                GET_AUDIO_MAP refuses exactly the indices READ_DESCRIPTOR
//                refuses.
//
//                ONLY STREAM_PORT_INPUT IS DISPATCHED. Milan §5.4.2.26 also
//                demands GET_AUDIO_MAP on every Stream Port Output with no
//                static map; this build's talker-side mappings live in a
//                differently-shaped store (the capture mux's source buckets,
//                not a cluster-indexed RAM), and serving them is a RECORDED
//                GAP, not an oversight: a command naming any other
//                descriptor_type keeps the NOT_IMPLEMENTED echo, decided at
//                the A_PLD exit exactly like the MVU sub-decode - the type
//                field is walked, not trusted at pop.
//
//                A ROM is the right shape once the hazard class, min-cdl,
//                response-size id, lock/GDI/notify flags and per-profile valid
//                bits of the 06 §8 entry have consumers; three arms of decode
//                is the right shape while they do not, and it cannot go stale
//                against a generator that does not exist. When the ROM lands
//                it replaces `upc_w`/`echo_w` here and nothing else.
//
//                THE MVU SUB-DECODE (06 §4 "MVU sub-decoder (protocol_id
//                match)") CANNOT RUN AT POP, and that is why it is a second
//                decode rather than a fourth arm above. A Milan Vendor Unique
//                command carries no AEM command_type: Milan v1.2 §5.4.3.2 puts
//                a 48-bit protocol_id at @22..@27 and the MVU command_type at
//                @28..@29, so the 03 §4 record's `opcode` field — which the
//                validator fills from @22..@23 — holds the first two bytes of
//                that protocol_id and nothing that identifies the command. The
//                bytes that do identify it are read by the payload walk, so
//                the pop-time decode stands (an MVU command starts out heading
//                for the generic echo) and A_PLD OVERRIDES it once every byte
//                is settled:
//
//                  msg_type 6 + protocol_id 00-1B-C5-0A-C1-00
//                             + r 0 + command_type 0x0000
//                             + a Figure 5.3 payload (8 B)  -> UPC_MVUINFO_C
//
//                Anything else with message_type 6 — a foreign protocol_id, an
//                MVU command_type this build does not implement, a truncated
//                one — keeps the echo and answers MVU status 1
//                NOT_IMPLEMENTED (Milan Table 5.19 = IEEE Table 9-6, which is
//                the same code the AEM path uses, so the status register needs
//                no remapping).
//
//                NOT_IMPLEMENTED IS AN ANSWER, NOT SILENCE. Every opcode this
//                block does not implement is ECHOED back with
//                message_type + 1 and status NOT_IMPLEMENTED (F06.14 "echo
//                command", IEEE §9.3.5.3.3): the emitted payload is the
//                command's own, read straight back out of its RX slot, and the
//                emitted length is the command's. So an unimplemented opcode
//                produces a well-formed, correctly-sized AECPDU — never a
//                dropped frame and never a malformed one.
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
//                WHERE THE RESPONSE BUFFER LIVES. Not here. The µCPU's `rb_*`
//                writes go to KL_aecp_resp_buf, which gathers them into
//                64-bit lanes and holds them in the integrator's MAIN MEMORY
//                at the compile-time `RESP_BASE_P`; the frame builder streams
//                the payload back out of it. As fabric state this buffer
//                measured 5,079 flip-flops and 3,495 LUTs on the reference
//                part and was the thing the placer could not pack. The
//                BIG-ENDIAN placement rule (a field VALUE right-justified in
//                `rb_wdata_o`, its WIDTH given by a low-contiguous
//                `rb_wstrb_o`, those m bytes laid out from `rb_addr_o` upward)
//                moved with it — the rule has to live in the buffer because
//                the µISA has no byte-swap operation, 06 §8 never states an
//                order, and tb/ucpu's C++ model uses the opposite convention
//                for its own convenience.
//
//                AN ECHOED PAYLOAD NEVER TOUCHES MEMORY. §9.3.5.3.3's echo is
//                a verbatim copy of the command payload, and the command is
//                still sitting in its RX slot until this block frees it — so
//                the frame builder reads those bytes straight out of the slot
//                (slot byte 24+n IS payload byte n) instead of staging them
//                through the response buffer. Only a µprogram-BUILT payload
//                is worth a memory round trip.
//
//                WHEN A GATHER FACE WEDGES. `ctr_wait_i` or `amap_wait_i`
//                held forever is the one way either face can stop a command
//                retiring, and a stalled µCPU stops READ_DESCRIPTOR too - the
//                descriptor path is the thing that must not regress. So ONE
//                shared watchdog bounds whichever face the in-flight command
//                routed to, budgeted at MEM_TIMEOUT_CYC_P like the descriptor
//                and response bridges, and expiry VOIDS the response through
//                the ENTITY_MISBEHAVING rebuild below rather than letting
//                zeros out under a mask or a mapping count that was already
//                emitted. A face that answers (including one that answers 0)
//                never arms it.
//
//                WHEN THE RESPONSE MEMORY FAILS. A wedged or absent bridge
//                raises `err_o` on the buffer. The TX slot is already
//                allocated by then and KL_pp_tx_slots has no abort arc — an
//                uncommitted slot is leaked and a zero-length commit hangs the
//                arbiter — so the builder REWRITES the frame in place as a
//                60-byte ENTITY_MISBEHAVING response (IEEE §7.4 status 10)
//                with an empty payload, built entirely from registers. Never a
//                leak, never a hang, and never a SUCCESS carrying bytes this
//                block did not read.
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
    //! response-buffer base in the integrator's memory map (compile time).
    //! WRITTEN by this processor — it must not overlap the descriptor image.
    parameter logic [31:0] RESP_BASE_P       = 32'h2010_0000,
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

    //! ---- response-buffer memory master (read/write; see KL_aecp_resp_buf) ----
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
    input  wire         rmem_wr_err_i,

    //! ---- GET_COUNTERS read face (06 §6.6; IEEE §7.4.42, Milan §5.4.2.25) ----
    //! Asked once per quadlet of the response. `ctr_word_o` 0..31 is the
    //! counters_block quadlet at block byte 4·n; `ctr_word_o` = 32 is the
    //! counters_valid word itself, so ONE face carries the mask and the data
    //! and the integrator has one place to be honest about what it measures.
    output logic        ctr_req_o,            //! a quadlet is being asked for
    output logic [15:0] ctr_desc_type_o,      //! AECPDU @24 (ENTITY, STREAM_INPUT, …)
    output logic [15:0] ctr_desc_index_o,     //! AECPDU @26
    output logic  [5:0] ctr_word_o,           //! 0..31 = block quadlet, 32 = counters_valid
    input  wire  [31:0] ctr_data_i,           //! that quadlet, 1722.1 value order
    //! HOLD, not ready: 0 means "answer is on ctr_data_i now", so an unwired
    //! face answers 0 and the response carries an empty counters_valid mask
    //! rather than hanging or advertising zeros (see the banner)
    input  wire         ctr_wait_i,

    //! ---- GET_AUDIO_MAP read face (06 §6.5; IEEE §7.4.44, Milan §5.4.2.26) ----
    //! The integrator's dynamic-mapping store, asked one word at a time while
    //! a GET_AUDIO_MAP is in flight. `amap_sel_o` names the word:
    //!   0 NMAPS  - {16'd0, number_of_maps}: the addressed port's fixed
    //!              Milan §5.4.2.26 partition count; 0 = no such port here
    //!   1 GEOM   - {16'd0, number_of_maps, number_of_mappings}: the page
    //!              named by amap_map_index_o; number_of_mappings MUST read 0
    //!              for a page the store has no data for (unknown port,
    //!              map_index out of range) - the wrong-object guard
    //!   2 RECORD - mapping record `amap_rec_o` of that page as ONE
    //!              big-endian qword {stream_index, stream_channel,
    //!              cluster_offset, cluster_channel} (§7.4.44.2.1)
    //! `amap_wait_i` is the same HOLD `ctr_wait_i` is: an unwired face
    //! answers number_of_maps 0, and the µprogram turns that into
    //! NO_SUCH_DESCRIPTOR only where the descriptor store agrees.
    output logic        amap_req_o,           //! a word is being asked for
    output logic [15:0] amap_desc_type_o,     //! AECPDU @24 (STREAM_PORT_INPUT)
    output logic [15:0] amap_desc_index_o,    //! AECPDU @26
    output logic [15:0] amap_map_index_o,     //! AECPDU @28 - the page
    output logic  [1:0] amap_sel_o,           //! 0 NMAPS, 1 GEOM, 2 RECORD
    output logic  [7:0] amap_rec_o,           //! record ordinal within the page
    input  wire  [63:0] amap_data_i,          //! the word (upper 32 zero unless RECORD)
    input  wire         amap_wait_i,          //! HOLD the beat (not a ready)

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
    output logic [15:0] dbg_locate_miss_o,   //! store: locates answered err
    output logic  [2:0] dbg_resp_fault_o,    //! response buffer: last fault code
    output logic [15:0] dbg_resp_err_o,      //! responses voided by that memory
    output logic [15:0] dbg_resp_lane_o      //! response lanes written to memory
);

  // ---- IEEE 1722.1-2021 AEM opcodes this block decodes --------------------
  localparam logic [15:0] OP_READ_DESCRIPTOR_C = 16'h0004;
  localparam logic [15:0] OP_IDENTIFY_NOTIF_C  = 16'h0026;
  localparam logic [15:0] OP_GET_COUNTERS_C    = 16'h0029;
  localparam logic [15:0] OP_GET_AUDIO_MAP_C   = 16'h002B;
  //! Table 7-1: the one descriptor type the audio-map µprogram serves (see
  //! the banner - every other type keeps the NOT_IMPLEMENTED echo)
  localparam logic [15:0] DT_STREAM_PORT_IN_C  = 16'h000E;

  // ---- Milan Vendor Unique (Milan v1.2 §5.4.3.2, §5.4.4.1) ---------------
  //! protocol_id is the Avnu OUI-36 00-1B-C5-0A-C appended with MVU's 12-bit
  //! protocol unique identifier 0x100 (§5.4.3.2.1) = 00-1B-C5-0A-C1-00. It
  //! spans @22..@27, so its first two bytes land in `raw_ct_r` (the field an
  //! AEM command calls command_type) and its last four in the payload walk.
  localparam logic [15:0] MVU_PID_HI_C = 16'h001B;   // @22..@23
  localparam logic [15:0] MVU_PID_MD_C = 16'hC50A;   // @24..@25
  localparam logic  [7:0] MVU_PID_L1_C = 8'hC1;      // @26
  localparam logic  [7:0] MVU_PID_L0_C = 8'h00;      // @27
  //! the @28..@29 word: r = 0 (§5.4.3.2.2) + Table 5.18 command_type 0x0000
  localparam logic [15:0] MVU_GET_MILAN_INFO_C = 16'h0000;
  //! Figure 5.3 fixes the command at protocol_id + r/command_type + reserved,
  //! so its payload is 8 bytes. The walk captures index n from @22+n and the
  //! LAST capture lands in the same cycle the walk exits, so demanding the
  //! whole Figure 5.3 payload is also what guarantees @28..@29 is settled by
  //! the time the sub-decode below reads it.
  localparam logic [10:0] MVU_CMD_PLD_C = 11'd8;

  // ---- µPC entry points (hdl/aecp/ucode/gen_ucode.py) ---------------------
  localparam logic [10:0] UPC_NOTIMPL_C = 11'd560;   // E_NOTIMPL
  localparam logic [10:0] UPC_RDESC_C   = 11'd640;   // E_RDESC
  localparam logic [10:0] UPC_BADARG_C  = 11'd704;   // E_BADARG
  localparam logic [10:0] UPC_MVUINFO_C = 11'd736;   // E_MVUINFO
  localparam logic [10:0] UPC_GCTRS_C   = 11'd768;   // E_GCTRS
  localparam logic [10:0] UPC_GAMAP_C   = 11'd800;   // E_GAMAP

  // ---- geometry -----------------------------------------------------------
  //! header 14 (Ethernet) + 24 (AECPDU) before the first payload byte
  localparam int unsigned ETH_HDR_C    = 14;
  localparam int unsigned AECP_HDR_C   = 24;
  localparam int unsigned FRAME_HDR_C  = ETH_HDR_C + AECP_HDR_C;      // 38
  localparam int unsigned ETH_MIN_C    = 60;
  //! response buffer: the µCPU's 12 header bytes + the 4-byte
  //! {configuration_index, reserved} prefix + one whole descriptor, rounded to
  //! the 8-byte lane the memory face moves
  localparam int unsigned RESP_BUF_C   = ((16 + LINE_BYTES_P) + 15) & ~32'd15;
  localparam int unsigned PLD_MAX_C    = RESP_BUF_C - 12;
  localparam int unsigned FRAME_MAX_C  = FRAME_HDR_C + PLD_MAX_C;

  if (FRAME_MAX_C > TX_OVERSIZE_BYTES_P) begin : gen_g_frame_fit
    $error("a maximum AECP response (%0d B) exceeds the oversize slot (%0d B)",
           FRAME_MAX_C, TX_OVERSIZE_BYTES_P);
  end

  pp_txn_t txn_w;
  assign txn_w = pp_txn_t'(txn_i);

  // ---- µCPU response-buffer face (served by KL_aecp_resp_buf below) -------
  logic        rb_we_w;
  logic  [9:0] rb_addr_w;
  logic [31:0] rb_wdata_w;
  logic  [3:0] rb_wstrb_w;
  logic        rb_ready_w;

  // ---- response-buffer lifecycle + payload read stream --------------------
  logic        rsp_open_w, rsp_seal_w;
  logic [10:0] rsp_seal_len_w;
  logic        rsp_rd_valid_w, rsp_rd_take_w;
  logic  [7:0] rsp_rd_data_w;
  logic        rsp_err_w, rsp_busy_w;

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
  logic  [1:0] pid_lo_r;                 // AECPDU @26,@27 matched MVU's tail
  logic        echo_r, sent_r;
  logic        ctrs_r;                   // this command is a GET_COUNTERS
  logic        amap_r;                   // this command is a GET_AUDIO_MAP
  logic  [7:0] amap_rec_r;               // records handed out this command
  logic [10:0] upc_r;
  logic [4:0]  status_r;
  logic [10:0] bidx_r;                   // frame byte being written
  logic [10:0] frame_len_r;
  logic        err_mode_r;               // rebuilding as ENTITY_MISBEHAVING
  logic [TXS_W_C-1:0] tx_slot_r;
  logic [15:0] cmd_cnt_r, resp_cnt_r, drop_cnt_r, rerr_cnt_r;

  // ---- opcode decode = the dispatch step (see the banner) -----------------
  logic [10:0] upc_w;
  logic        echo_w, short_w, short_ct_w, short_am_w, ctrs_w, amap_w;
  //! a READ_DESCRIPTOR must carry configuration_index + reserved +
  //! descriptor_type + descriptor_index; a shorter one is BAD_ARGUMENTS, never
  //! a locate of whatever zeros happened to be there
  assign short_w = (txn_w.cdl < 11'd20);
  //! §7.4.42.1's command payload is descriptor_type + descriptor_index and
  //! nothing else, so cdl 16 is the whole command (F06.14's offset-from-@12)
  assign short_ct_w = (txn_w.cdl < 11'd16);
  //! §7.4.44.1's command runs through the reserved word at @30, so cdl 20 is
  //! the whole command; shorter never reached map_index and is BAD_ARGUMENTS
  assign short_am_w = (txn_w.cdl < 11'd20);
  //! and it must really BE an AEM command: the 03 §4 record fills `opcode`
  //! from AECPDU @22..@23, which on a VENDOR_UNIQUE message is the first two
  //! bytes of a 48-bit protocol_id, not a command_type at all
  assign ctrs_w = (txn_w.protocol == PP_PROTO_AEM)
                  && (txn_w.opcode == OP_GET_COUNTERS_C) && !short_ct_w;
  assign amap_w = (txn_w.protocol == PP_PROTO_AEM)
                  && (txn_w.opcode == OP_GET_AUDIO_MAP_C) && !short_am_w;
  always_comb begin : dispatch_decode
    if ((txn_w.opcode == OP_READ_DESCRIPTOR_C) && !short_w) begin
      upc_w  = UPC_RDESC_C;
      echo_w = 1'b0;
    end else if (ctrs_w) begin
      upc_w  = UPC_GCTRS_C;
      echo_w = 1'b0;
    end else if (amap_w) begin
      upc_w  = UPC_GAMAP_C;
      echo_w = 1'b0;
    end else if ((txn_w.opcode == OP_IDENTIFY_NOTIF_C)
                 || ((txn_w.opcode == OP_READ_DESCRIPTOR_C) && short_w)
                 || ((txn_w.protocol == PP_PROTO_AEM)
                     && (txn_w.opcode == OP_GET_COUNTERS_C) && short_ct_w)
                 || ((txn_w.protocol == PP_PROTO_AEM)
                     && (txn_w.opcode == OP_GET_AUDIO_MAP_C) && short_am_w)) begin
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

  //! the MVU sub-decode (see the banner): read at the A_PLD exit, where the
  //! four captured words below are settled. `pid_lo_r` is the only new state
  //! the match needs — @22..@25 and @28..@29 already have registers, held for
  //! the header echo and for READ_DESCRIPTOR's operands — and it is two
  //! comparison RESULTS rather than the two bytes, so a walk that stops before
  //! @27 leaves it 0 and cannot look like a match.
  logic mvu_get_milan_info_w;
  assign mvu_get_milan_info_w = (cmd_r.protocol == PP_PROTO_MVU)
                                && (pld_cmd_r  >= MVU_CMD_PLD_C)
                                && (raw_ct_r   == MVU_PID_HI_C)
                                && (cfg_ix_r   == MVU_PID_MD_C)
                                && (pid_lo_r   == 2'b11)
                                && (desc_ty_r  == MVU_GET_MILAN_INFO_C);

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
  //! GET_COUNTERS reuses both without a mux: §7.4.42.1 has descriptor_type at
  //! @24 and descriptor_index at @26, the walk below puts them in `cfg_ix_r`
  //! and `desc_ix_r`, and `desc_ty_r` stays 0 — so r14[15:0] is the type it
  //! emits at @24 and r13[15:0] is the index it emits at @26.
  //! GET_AUDIO_MAP is the one shape that needs a mux, because its µprogram
  //! consumes the registers TWO ways at once: r14 is the store's locate key
  //! ({index, type, cfg} - GET_AUDIO_MAP names no configuration_index and
  //! configuration 0 is current by construction, SET_CONFIGURATION being
  //! unimplemented), and r13 packs {descriptor_index, map_index} so ONE
  //! FMT_D BUILD_FLD lays @26..@29 in wire order while r13[15:0] is the
  //! right-justified map_index CHECK_ARG compares (the µISA has no shift).
  //! `desc_ty_r` holds map_index for this command - see the payload walk.
  //! The muxes below land in opd0_r/opd1_r on the A_DISP cycle that raises
  //! the dispatch strobe (stage-0 pipeline: operand shaping must reach the
  //! µCPU's register file from flops, never as a live mux cone - the walk
  //! registers are settled a full state earlier, so the latch costs nothing)
  assign opd0_w = amap_r ? {16'd0, desc_ix_r, cfg_ix_r, 16'd0}
                         : {16'd0, desc_ix_r, desc_ty_r, cfg_ix_r};
  assign opd1_w = amap_r ? {32'd0, desc_ix_r, desc_ty_r}
                         : {32'd0, desc_ty_r, desc_ix_r};

  logic [63:0] opd0_r, opd1_r;

  logic        st_req_w, st_we_w, st_name_w;
  logic [19:0] st_addr_w;
  logic [63:0] st_wdata_w, st_rdata_w;
  logic  [7:0] st_wstrb_w;
  logic        st_ready_w, st_rvalid_w, st_err_w;
  logic        gx_req_w, gx_valid_w;
  logic  [7:0] gx_sel_w;
  logic [63:0] gx_data_w;
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
      .disp_opd0_i        (opd0_r),
      .disp_opd1_i        (opd1_r),
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
      //! the 06 §6.6/§6.5 gather bus, with TWO sources routed by command:
      //! the GET_COUNTERS read face and the GET_AUDIO_MAP read face (see
      //! the gather-faces section below). §6.2's GET_STREAM_INFO gather has
      //! none, and no µprogram that would use it is dispatched.
      .gx_req_o           (gx_req_w),
      .gx_sel_o           (gx_sel_w),
      .gx_valid_i         (gx_valid_w),
      .gx_data_i          (gx_data_w),
      .lock_held_i        (lock_held_i),
      .lock_ctlr_i        (lock_ctlr_i),
      .rb_we_o            (rb_we_w),
      .rb_addr_o          (rb_addr_w),
      .rb_wdata_o         (rb_wdata_w),
      .rb_wstrb_o         (rb_wstrb_w),
      .rb_ready_i         (rb_ready_w),
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

  logic [15:0] resp_burst_nc_w, resp_drop_nc_w;

  KL_aecp_resp_buf #(
      .RESP_BASE_P       (RESP_BASE_P),
      .RESP_BYTES_P      (RESP_BUF_C),
      .MEM_TIMEOUT_CYC_P (MEM_TIMEOUT_CYC_P)
  ) u_resp (
      .clk_i           (clk_i),
      .rst_n           (rst_n),
      .open_i          (rsp_open_w),
      .seal_i          (rsp_seal_w),
      .seal_len_i      (rsp_seal_len_w),
      .wr_we_i         (rb_we_w),
      .wr_addr_i       (rb_addr_w),
      .wr_wdata_i      (rb_wdata_w),
      .wr_wstrb_i      (rb_wstrb_w),
      .wr_ready_o      (rb_ready_w),
      .rd_valid_o      (rsp_rd_valid_w),
      .rd_data_o       (rsp_rd_data_w),
      .rd_take_i       (rsp_rd_take_w),
      .mem_req_valid_o (rmem_req_valid_o),
      .mem_req_ready_i (rmem_req_ready_i),
      .mem_req_addr_o  (rmem_req_addr_o),
      .mem_req_beats_o (rmem_req_beats_o),
      .mem_rsp_valid_i (rmem_rsp_valid_i),
      .mem_rsp_ready_o (rmem_rsp_ready_o),
      .mem_rsp_data_i  (rmem_rsp_data_i),
      .mem_rsp_last_i  (rmem_rsp_last_i),
      .mem_rsp_err_i   (rmem_rsp_err_i),
      .mem_wr_valid_o  (rmem_wr_valid_o),
      .mem_wr_ready_i  (rmem_wr_ready_i),
      .mem_wr_addr_o   (rmem_wr_addr_o),
      .mem_wr_data_o   (rmem_wr_data_o),
      .mem_wr_strb_o   (rmem_wr_strb_o),
      .mem_wr_done_i   (rmem_wr_done_i),
      .mem_wr_err_i    (rmem_wr_err_i),
      .busy_o          (rsp_busy_w),
      .err_o           (rsp_err_w),
      .dbg_fault_o     (dbg_resp_fault_o),
      .dbg_lane_wr_o   (dbg_resp_lane_o),
      .dbg_burst_o     (resp_burst_nc_w),
      .dbg_drop_o      (resp_drop_nc_w)
  );

  // =======================================================================
  // the gather faces: GET_COUNTERS (06 §6.6) + GET_AUDIO_MAP (06 §6.5)
  // =======================================================================
  //! ONE bus, routed BY COMMAND: `amap_r` is the only discriminator, so each
  //! face owns the whole 8-bit selector space while its command is in flight
  //! and neither can ever see the other's query (a GET_COUNTERS and a
  //! GET_AUDIO_MAP are never in flight together - the engine runs one
  //! command at a time by construction).
  //!
  //! Counters selector-to-quadlet is pure wiring on purpose. READ_CTRS drives
  //! gx_sel = {cnd, beat} with beat 0..3, and the µprogram walks cnd 0..7, so
  //! {cnd[2:0], beat[1:0]} IS the counters_block quadlet index 0..31 and
  //! sel[7] is free to mean "the counters_valid word instead" (GATHER_EXT
  //! cnd = 8). No decoder, no per-word ROM, no state.
  assign ctr_req_o        = gx_req_w && !amap_r;
  assign ctr_desc_type_o  = cfg_ix_r;
  assign ctr_desc_index_o = desc_ix_r;
  assign ctr_word_o       = gx_sel_w[7] ? 6'd32
                                        : {1'b0, gx_sel_w[6:4], gx_sel_w[1:0]};

  //! ...and the audio-map selectors are three points of it (gen_ucode.py
  //! AM_NMAPS 0x00 / AM_GEOM 0x01 / AM_REC 0x10): sel[4] alone separates a
  //! record fetch from the two geometry words, sel[0] the two geometry words
  //! from each other. `desc_ty_r` HOLDS map_index for this command (see the
  //! payload walk), which is why the page rides it here.
  assign amap_req_o        = gx_req_w && amap_r;
  assign amap_desc_type_o  = cfg_ix_r;
  assign amap_desc_index_o = desc_ix_r;
  assign amap_map_index_o  = desc_ty_r;
  assign amap_sel_o        = gx_sel_w[4] ? 2'd2 : {1'b0, gx_sel_w[0]};
  assign amap_rec_o        = amap_rec_r;

  //! bounded wait (see the banner), shared by both faces: expiry unsticks the
  //! µCPU with a zero and marks the response void, because by then the
  //! counters_valid word - or the mapping count - is already in the buffer,
  //! and zeros behind an emitted claim are the one answer worse than none
  localparam int unsigned CTO_W_C = $clog2(MEM_TIMEOUT_CYC_P + 1);
  logic [CTO_W_C-1:0] gxf_tmo_r;
  logic               gxf_fail_r;
  logic               ctr_hold_w, amap_hold_w;
  assign ctr_hold_w  = gx_req_w && !amap_r && ctr_wait_i  && !gxf_fail_r;
  assign amap_hold_w = gx_req_w &&  amap_r && amap_wait_i && !gxf_fail_r;

  //! REGISTERED gather answer - the stage-0 pipeline cut. The integrator's
  //! wait/data cone (ctr_wait_i / amap_wait_i arrive combinationally from
  //! OUTSIDE this processor) used to run through gx_valid_w into the µCPU's
  //! E-stage stall and from there into the µcode ROM's address-register
  //! enable: measured on the reference part as the failing 12-plus-level
  //! path into u_ucpu upc_r (ENARDEN/ADDRARDADDR). The answer is now
  //! latched HERE, so the µCPU's stall sees one flop. `!gxr_valid_r` in the
  //! arm makes the valid a one-cycle pulse per answered beat - without it a
  //! multi-beat READ_CTRS would take beat n's registered data a second time
  //! while the selector was still moving to beat n+1. One extra cycle per
  //! gather beat, invisible at AECP rates.
  logic        gxr_valid_r;
  logic [63:0] gxr_data_r;

  always_ff @(posedge clk_i) begin : gather_answer
    if (!rst_n) begin
      gxr_valid_r <= 1'b0;
      gxr_data_r  <= 64'd0;
    end else begin
      gxr_valid_r <= gx_req_w && !(ctr_hold_w || amap_hold_w) && !gxr_valid_r;
      gxr_data_r  <= gxf_fail_r ? 64'd0
                   : amap_r     ? amap_data_i
                                : {32'd0, ctr_data_i};
    end
  end

  assign gx_valid_w = gxr_valid_r;
  assign gx_data_w  = gxr_data_r;

  always_ff @(posedge clk_i) begin : gather_watchdog
    if (!rst_n) begin
      gxf_tmo_r  <= '0;
      gxf_fail_r <= 1'b0;
    end else if (a_st_r == A_IDLE) begin
      gxf_tmo_r  <= '0;
      gxf_fail_r <= 1'b0;
    end else if (ctr_hold_w || amap_hold_w) begin
      if (gxf_tmo_r == CTO_W_C'(MEM_TIMEOUT_CYC_P)) gxf_fail_r <= 1'b1;
      else                                          gxf_tmo_r  <= gxf_tmo_r + CTO_W_C'(1);
    end else begin
      gxf_tmo_r <= '0;
    end
  end

  //! the record ordinal: one per COMPLETED record gather, so the loop's k-th
  //! GATHER_EXT asks for record k and the face stays stateless. Reset with
  //! the command, like the watchdog.
  always_ff @(posedge clk_i) begin : amap_record_ordinal
    if (!rst_n)                 amap_rec_r <= 8'd0;
    else if (a_st_r == A_IDLE)  amap_rec_r <= 8'd0;
    else if (amap_req_o && gx_sel_w[4] && gx_valid_w
             && (amap_rec_r != 8'hFF)) amap_rec_r <= amap_rec_r + 8'd1;
  end

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
      //! @22, AND ONLY AN AEM MESSAGE HAS A u BIT THERE. 1722.1-2021 9.2.1.7
      //! puts `u` in the top bit of an AEM AECPDU's command_type field, and a
      //! solicited response clears it. Every other message_type defines @22
      //! for itself: 9.6.2 Figure 9-12 gives a VENDOR_UNIQUE AECPDU a 48-bit
      //! protocol_id running @22..@27 with NO u bit in it, so @22 there is
      //! protocol_id[47:40] whole.
      //!
      //! This used to clear bit 7 unconditionally, under a comment claiming
      //! it "keeps a VENDOR_UNIQUE response's protocol_id bytes intact". It
      //! did the opposite. MEASURED on the AX7101 before the fix, by driving
      //! four vendor-unique commands and reading back the NOT_IMPLEMENTED
      //! echo:
      //!     sent 00-1B-C5-0A-C1-00 -> echoed 00 1b c5 0a c1 00   intact
      //!     sent 7F-1B-C5-0A-C1-00 -> echoed 7f 1b c5 0a c1 00   intact
      //!     sent FC-1B-C5-0A-C1-00 -> echoed 7c 1b c5 0a c1 00   CORRUPT
      //!     sent 80-1B-C5-0A-C1-00 -> echoed 00 1b c5 0a c1 00   CORRUPT
      //! Every OUI whose first octet has bit 7 set - half of the space - got
      //! a mangled protocol_id back, so a vendor could not match our refusal
      //! to the protocol it asked about. Milan's own 00-1B-C5 has bit 7
      //! clear, which is why GET_MILAN_INFO never showed it and why nothing
      //! in the suite caught it: every protocol_id ever tested was immune.
      6'd36: hdr_byte_w = (cmd_r.protocol == PP_PROTO_AEM)
                          ? {1'b0, raw_ct_r[14:8]}   // AEM: u = 0, solicited
                          : raw_ct_r[15:8];          // everything else: verbatim
      6'd37: hdr_byte_w = raw_ct_r[7:0];
      default: hdr_byte_w = 8'd0;
    endcase
  end

  //! payload byte `bidx_r - FRAME_HDR_C` comes from the RX slot for an echoed
  //! response (§9.3.5.3.3 copies the command verbatim and the slot still holds
  //! it) and from the memory-backed response buffer for a µprogram-built one
  logic pay_w;
  assign pay_w = (bidx_r >= 11'(FRAME_HDR_C))
                 && (bidx_r < (11'(FRAME_HDR_C) + pld_r));

  logic [7:0] frame_byte_w;
  always_comb begin : frame_byte
    if (bidx_r < 11'(FRAME_HDR_C))  frame_byte_w = hdr_byte_w;
    else if (pay_w)                 frame_byte_w = echo_r ? rxs_rd_data_i
                                                          : rsp_rd_data_w;
    else                            frame_byte_w = 8'd0;   // pad
  end

  //! the builder advances only when the byte it is about to write EXISTS: the
  //! response buffer's read burst paces itself against main memory
  logic byte_ok_w;
  assign byte_ok_w = !pay_w || echo_r || rsp_rd_valid_w;
  assign rsp_rd_take_w = (a_st_r == A_WR) && pay_w && !echo_r
                         && rsp_rd_valid_w;

  // ---- RX payload walk -----------------------------------------------------
  //! A_PLD: the walk starts at AECPDU @22 (= slot byte 22) so bytes @22..@23
  //! are captured verbatim; the parsed fields sit at walk indices 2..9.
  //! A_WR: the same port PREFETCHES the echo payload one byte ahead — the pool
  //! answers a cycle after `rd_en`, and payload byte n is slot byte 24 + n.
  logic [10:0] pref_ix_w;
  logic        pref_en_w;
  assign pref_ix_w = bidx_r + 11'd1 - 11'(FRAME_HDR_C);
  assign pref_en_w = (a_st_r == A_WR) && echo_r
                     && ((bidx_r + 11'd1) >= 11'(FRAME_HDR_C))
                     && ((bidx_r + 11'd1) < (11'(FRAME_HDR_C) + pld_r));

  assign rxs_rd_slot_o = cmd_r.rx_slot[RXS_W_C-1:0];
  assign rxs_rd_addr_o = (a_st_r == A_WR)
                         ? RXA_W_C'(32'd24 + 32'(pref_ix_w))
                         : RXA_W_C'(32'd22 + 32'(walk_r));
  assign rxs_rd_en_o   = ((a_st_r == A_PLD) && (walk_r < (pld_r + 11'd2)))
                         || pref_en_w;

  //! a command is only taken once the PREVIOUS response has let go of main
  //! memory: `open_i` re-arms the buffer, and re-arming it under a burst that
  //! is still in flight would leave the bridge holding a beat nobody sinks.
  //! The buffer's watchdog bounds this wait, so it can never become a hang.
  assign txn_ready_o     = (a_st_r == A_IDLE) && !rsp_busy_w;
  assign txs_alloc_req_o = (a_st_r == A_ALLOC);
  assign txs_oversize_o  = (frame_len_r > 11'(TX_STD_BYTES_P));
  assign txs_wr_slot_o   = tx_slot_r;
  assign txs_wr_addr_o   = TXA_W_C'(bidx_r);
  assign txs_wr_valid_o  = (a_st_r == A_WR) && byte_ok_w;
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
  assign dbg_resp_err_o = rerr_cnt_r;

  logic [10:0] pad_len_w;
  assign pad_len_w = (11'(FRAME_HDR_C) + pld_r < 11'(ETH_MIN_C))
                     ? 11'(ETH_MIN_C) : (11'(FRAME_HDR_C) + pld_r);

  // ---- the response-buffer lifecycle strobes -------------------------------
  //! opened when a command is accepted, sealed when its µprogram retires. An
  //! echoed payload never entered the buffer, so it is sealed with length 0
  //! and costs no read burst at all.
  assign rsp_open_w     = (a_st_r == A_IDLE) && txn_valid_i && !drop_w
                          && !rsp_busy_w;
  //! only a µprogram that actually SENT a response is worth sealing: a
  //! retirement without SEND_RESPONSE emits no frame, so it must not leave a
  //! read burst in flight for the next command's `open_i` to trample
  assign rsp_seal_w     = (a_st_r == A_RUN) && ucpu_done_w
                          && (sent_r || resp_send_w);
  //! ... and a response a gather face already voided has no payload to
  //! read back either: sealing it with its INTENDED length would start a
  //! 136-byte read burst that the builder — now emitting a bare 60-byte
  //! ENTITY_MISBEHAVING frame — never consumes, and the buffer would sit in
  //! its read state until its own watchdog fired, holding the next command out
  assign rsp_seal_len_w = (echo_r || gxf_fail_r) ? 11'd0 : pld_r;

  //! the response memory failed under a frame that is already half written:
  //! rebuild it in place as a bare ENTITY_MISBEHAVING answer (see the banner)
  logic rsp_fail_w;
  assign rsp_fail_w = (rsp_err_w || gxf_fail_r) && !err_mode_r && !echo_r;

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
      pid_lo_r     <= 2'b00;
      echo_r       <= 1'b0;
      ctrs_r       <= 1'b0;
      amap_r       <= 1'b0;
      upc_r        <= 11'd0;
      status_r     <= 5'd0;
      bidx_r       <= 11'd0;
      frame_len_r  <= 11'd0;
      err_mode_r   <= 1'b0;
      tx_slot_r    <= '0;
      disp_valid_r <= 1'b0;
      opd0_r       <= 64'd0;
      opd1_r       <= 64'd0;
      sent_r       <= 1'b0;
      cmd_cnt_r    <= 16'd0;
      resp_cnt_r   <= 16'd0;
      drop_cnt_r   <= 16'd0;
      rerr_cnt_r   <= 16'd0;
    end else begin
      unique case (a_st_r)
        A_IDLE: begin
          if (txn_valid_i) begin
            cmd_r <= txn_w;
            if (drop_w) begin
              if (drop_cnt_r != 16'hFFFF) drop_cnt_r <= drop_cnt_r + 16'd1;
              a_st_r <= A_FREE;
            end else begin
              if (cmd_cnt_r != 16'hFFFF) cmd_cnt_r <= cmd_cnt_r + 16'd1;
              upc_r      <= upc_w;
              echo_r     <= echo_w;
              ctrs_r     <= ctrs_w;
              amap_r     <= amap_w;
              err_mode_r <= 1'b0;
              //! an echo with no RX slot has NO payload to echo: emitting
              //! `cdl - 12` bytes of whatever the slot pool last held would put
              //! another command's bytes on the wire
              pld_cmd_r <= (txn_w.rx_slot == PP_SLOT_NULL_C) ? 11'd0
                                                             : pld_cap_w;
              pld_r     <= (txn_w.rx_slot == PP_SLOT_NULL_C) ? 11'd0
                                                             : pld_cap_w;
              cfg_ix_r  <= 16'd0;
              desc_ty_r <= 16'd0;
              desc_ix_r <= 16'd0;
              raw_ct_r  <= 16'd0;
              walk_r    <= 11'd0;
              pid_lo_r  <= 2'b00;
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
            //! the MVU sub-decode, now that @22..@29 are settled: an MVU
            //! GET_MILAN_INFO leaves the generic echo for its own µprogram
            if (mvu_get_milan_info_w) begin
              upc_r  <= UPC_MVUINFO_C;
              echo_r <= 1'b0;
            end
            //! ...and the audio-map TYPE gate at the same seam, for the same
            //! reason: descriptor_type is at @24 and cannot be judged at pop.
            //! Milan §5.4.2.26 also asks for Stream Port OUTPUT service; this
            //! build's talker-side map store has a different shape and the
            //! honest refusal for it - and for any other type - is the
            //! NOT_IMPLEMENTED echo, a RECORDED gap (see the banner).
            //! `amap_r` stays set: E_NOTIMPL runs no gathers, so the gx
            //! routing it selects is never consulted on this arm.
            if (amap_r && (cfg_ix_r != DT_STREAM_PORT_IN_C)) begin
              upc_r  <= UPC_NOTIMPL_C;
              echo_r <= 1'b1;
            end
            a_st_r <= A_DISP;
          end
          //! the RX pool answers one cycle after rd_en: byte for index
          //! walk_r-1 lands now.
          //! §7.4.42.1, §7.4.44.1 and §7.4.5 disagree about what lives where,
          //! so the three shapes are captured into the SAME registers under
          //! `ctrs_r`/`amap_r`:
          //! a counters command has {type @24, index @26} and stops there,
          //! an audio-map command has {type @24, index @26, map_index @28,
          //! reserved @30} - `desc_ty_r` MEANS map_index for it - and
          //! a READ_DESCRIPTOR has {configuration_index @24, reserved @26,
          //! type @28, index @30}. Guarding both directions matters — a
          //! counters command padded past @27 must not walk on into
          //! `desc_ty_r` and change the descriptor it is asked about, and an
          //! audio-map command's reserved word at @30 must not trample the
          //! `desc_ix_r` it captured at @26.
          if (walk_r != 11'd0) begin
            unique case (walk_r - 11'd1)
              11'd0: raw_ct_r[15:8]  <= rxs_rd_data_i;
              11'd1: raw_ct_r[7:0]   <= rxs_rd_data_i;
              11'd2: cfg_ix_r[15:8]  <= rxs_rd_data_i;
              11'd3: cfg_ix_r[7:0]   <= rxs_rd_data_i;
              //! @26..@27 carry FOUR different things and each arm takes
              //! only its own: MVU's protocol_id tail (kept as the COMPARISON,
              //! not the bytes), GET_COUNTERS' and GET_AUDIO_MAP's
              //! descriptor_index, and READ_DESCRIPTOR's reserved field,
              //! which nobody keeps
              11'd4: begin
                pid_lo_r[1] <= (rxs_rd_data_i == MVU_PID_L1_C);
                if (ctrs_r || amap_r) desc_ix_r[15:8] <= rxs_rd_data_i;
              end
              11'd5: begin
                pid_lo_r[0] <= (rxs_rd_data_i == MVU_PID_L0_C);
                if (ctrs_r || amap_r) desc_ix_r[7:0]  <= rxs_rd_data_i;
              end
              11'd6: if (!ctrs_r) desc_ty_r[15:8] <= rxs_rd_data_i;
              11'd7: if (!ctrs_r) desc_ty_r[7:0]  <= rxs_rd_data_i;
              11'd8: if (!ctrs_r && !amap_r) desc_ix_r[15:8] <= rxs_rd_data_i;
              11'd9: if (!ctrs_r && !amap_r) desc_ix_r[7:0]  <= rxs_rd_data_i;
              default: ;
            endcase
          end
        end

        A_DISP: begin
          //! stage-0 operand latch: the strobe rises one cycle into A_DISP,
          //! so the operand muxes settle into flops before the µCPU's
          //! preload (S_PRE1/S_PRE0) ever reads them
          if (!disp_valid_r) begin
            opd0_r <= opd0_w;
            opd1_r <= opd1_w;
          end
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
          if (rsp_fail_w) begin
            err_mode_r  <= 1'b1;
            status_r    <= ST_ENTITY_MISBEHAVING_C;
            pld_r       <= 11'd0;
            frame_len_r <= 11'(ETH_MIN_C);
            bidx_r      <= 11'd0;
            if (rerr_cnt_r != 16'hFFFF) rerr_cnt_r <= rerr_cnt_r + 16'd1;
          end else if (txs_alloc_gnt_i) begin
            tx_slot_r <= txs_alloc_slot_i;
            a_st_r    <= A_WR;
          end
        end

        A_WR: begin
          //! the response memory died under a frame that is already partly
          //! written — rewrite it from byte 0 as a bare error answer built
          //! entirely from registers (see the banner)
          if (rsp_fail_w) begin
            err_mode_r  <= 1'b1;
            status_r    <= ST_ENTITY_MISBEHAVING_C;
            pld_r       <= 11'd0;
            frame_len_r <= 11'(ETH_MIN_C);
            bidx_r      <= 11'd0;
            if (rerr_cnt_r != 16'hFFFF) rerr_cnt_r <= rerr_cnt_r + 16'd1;
          end else if (byte_ok_w) begin
            if (bidx_r + 11'd1 >= frame_len_r) a_st_r <= A_CMT;
            bidx_r <= bidx_r + 11'd1;
          end
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
