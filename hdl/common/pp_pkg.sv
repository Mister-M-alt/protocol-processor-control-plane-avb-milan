/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : pp_pkg.sv
//  Project     : IEEE 1722.1 protocol processor (docs/architecture/03, 08)
//
//  Description : Shared protocol-processor types — the normalized-transaction
//                record of 03 §4 as ONE packed struct, the nine scoreboard
//                hazard classes of 03 §6, the origin/protocol/disposition
//                enums, and the P-TIMER-SLOTS allocation formula of 08 §5.
//
//                The one design decision that matters: the slot handles are
//                FIXED narrow indices with an explicit null code
//                (PP_SLOT_NULL_C), sized from the F01.5 defaults
//                (P-RX-SLOTS = 4, P-TX-STD-SLOTS = 4 + 1 oversize), not
//                parameterized widths — the record must be one packed shape
//                every dispatch FIFO and queue of 03 §2 can carry unchanged.
//
//                Field-width notes against 03 §4:
//                  - interface_index is fixed at 2 bits (covers
//                    P-N-AVB-INTERFACES up to 4; the F01.5 default is 1).
//                  - operands is the 03 §4 "struct" row, folded to a 64-bit
//                    packed quartet {desc_type, desc_index, config_index,
//                    unique_id} — the same 64-bit opd lane KL_aecp_ucpu
//                    preloads at dispatch (06 §8).
//---------------------------------------------------------------------------//
`default_nettype none

package pp_pkg;

  // ---- origins (03 §5: the four producers into dispatch) -----------------
  typedef enum logic [1:0] {
    PP_ORIGIN_RX    = 2'd0,
    PP_ORIGIN_TIMER = 2'd1,
    PP_ORIGIN_SELF  = 2'd2,
    PP_ORIGIN_MGMT  = 2'd3
  } pp_origin_e;

  // ---- protocols (03 §4: ADP / ACMP / AEM / MVU / AA) --------------------
  typedef enum logic [2:0] {
    PP_PROTO_ADP  = 3'd0,
    PP_PROTO_ACMP = 3'd1,
    PP_PROTO_AEM  = 3'd2,
    PP_PROTO_MVU  = 3'd3,
    PP_PROTO_AA   = 3'd4
  } pp_protocol_e;

  // ---- the NINE hazard classes of 03 §6 (F03.7 row order) ----------------
  typedef enum logic [3:0] {
    PP_HZ_RO_SNAPSHOT = 4'd0,  // parallel reads; blocked only vs same-key write
    PP_HZ_CFG_BARRIER = 4'd1,  // SET_CONFIGURATION: global drain
    PP_HZ_STREAM_CFG  = 4'd2,  // per stream index; doubles as sink-SM serial
    PP_HZ_MAP_CFG     = 4'd3,  // per stream port; cross-locked w/ STREAM_CFG
    PP_HZ_CLOCK_CFG   = 4'd4,  // per audio unit / clock domain
    PP_HZ_NAME_WR     = 4'd5,  // per descriptor
    PP_HZ_LOCK_OP     = 4'd6,  // global vs every lock-protected member
    PP_HZ_REGISTRY_OP = 4'd7,  // serialized on the registry
    PP_HZ_IDENTIFY    = 4'd8   // identify bursts serialized
  } pp_hazard_e;

  // ---- response disposition (03 §4 / §8 addressing) ----------------------
  typedef enum logic [1:0] {
    PP_RESP_UNICAST     = 2'd0,  // AECP: back to src_mac / registry MAC
    PP_RESP_ACMP_MCAST  = 2'd1,  // ACMP: always 91-E0-F0-01-00-00
    PP_RESP_IDENT_MCAST = 2'd2   // identify: 91-E0-F0-01-00-01 (Annex B)
  } pp_resp_disp_e;

  // ---- slot handles ------------------------------------------------------
  // 3 bits: rx encodes 0..P-RX-SLOTS-1 (F01.5: 4); tx encodes
  // 0..P-TX-STD-SLOTS-1 (F01.5: 4) plus PP_TX_OVERSIZE_C for the one
  // full-frame slot (03 §7). PP_SLOT_NULL_C = no payload (TIMER/SELF).
  localparam logic [2:0] PP_SLOT_NULL_C   = 3'd7;
  localparam logic [2:0] PP_TX_OVERSIZE_C = 3'd4;

  // ---- operands sub-struct (03 §4 "operands" row) ------------------------
  typedef struct packed {
    logic [15:0] desc_type;     // addressed descriptor_type
    logic [15:0] desc_index;    // addressed descriptor_index
    logic [15:0] config_index;  // configuration index where applicable
    logic [15:0] unique_id;     // talker/listener unique_id (ACMP)
  } pp_operands_t;

  // ---- the normalized-transaction record (03 §4, one shape for all) ------
  typedef struct packed {
    pp_origin_e    origin;            // RX / TIMER / SELF / MGMT
    logic [1:0]    interface_index;   // ingress port (target port for SELF)
    logic [31:0]   arrival_ts;        // ms; deadline base = end of reception
    pp_protocol_e  protocol;          // ADP / ACMP / AEM / MVU / AA
    logic [3:0]    msg_type;          // header message_type
    logic [4:0]    status_in;         // header status / valid_time
    logic [10:0]   cdl;               // validated control_data_length (V1/V2)
    logic [47:0]   src_mac;           // response addressing + registry tuple
    logic [63:0]   controller_eid;    // as applicable
    logic [63:0]   target_eid;        // as applicable
    logic [15:0]   sequence_id;       // echoed in responses
    logic          u_flag;            // AECP header u bit
    logic          cr;                // AECP header cr bit
    logic [15:0]   opcode;            // AEM/MVU command_type, ACMP/ADP msg_type
    pp_operands_t  operands;          // 03 §4 operands struct (64 b)
    logic [2:0]    rx_slot;           // payload handle; PP_SLOT_NULL_C = none
    pp_hazard_e    hazard_class;      // from the dispatch ROM (03 §6)
    logic [15:0]   hazard_key;        // serialization key within the class
    logic [2:0]    tx_slot;           // allocated at response build
    logic [31:0]   deadline;          // ms absolute: arrival + class budget
    pp_resp_disp_e resp_disposition;  // unicast / ACMP mcast / identify mcast
  } pp_txn_t;

  localparam int unsigned PP_TXN_W_C = $bits(pp_txn_t);  // 393 bits

  // ---- timer geometry (08 §5) --------------------------------------------
  // Deadline RAM record is {owner[7:0], deadline_ms[31:0]} = 40 bits — the
  // 89 x 40 b sizing of 08 §5. The armed bit lives OUTSIDE the RAM (see
  // KL_pp_timer_service.sv).
  localparam int unsigned PP_TIMER_OWNER_W_C = 8;
  localparam int unsigned PP_TIMER_SLOT_DW_C = PP_TIMER_OWNER_W_C + 32;

  // P-TIMER-SLOTS allocation formula (08 §5, F08.4):
  //
  //   slots = IF                       (ADP advertise/delay, shared SM slot)
  //         + SI                       (T-ADP-NOADP, per sink)
  //         + SI                       (ACMP shared SM slot, per sink)
  //         + SO                       (T-SRP-DAFRESH/LEAVEALL2, per source)
  //         + 2*CTRL*IF                (registry monitor + TIME_LIMITED)
  //         + CA_POOL                  (T-AECP-TIMEOUT inflight pool)
  //         + 5                        (LOCK, IDENT-BURST, IDENT-REARM,
  //                                     CTR-OBSERVE, NVM-DEBOUNCE singletons)
  //         [+ (7 + SI + SO)*IF with the SRP engine: T-MRP-{JOIN,LEAVEALL}
  //            x 2 participants + T-MRP-PERIODIC + registrar-leave pool
  //            (SI + SO streams + Domain + MVRP VID)]
  //
  //   baseline (IF=1, SI=SO=8, CTRL=16, CA=4): 1+8+8+8+32+4+5      = 66
  //   with the SRP engine:                      66 + (7 + 8 + 8)    = 89
  function automatic int unsigned pp_timer_slots(
      input int unsigned n_if,     // P-N-AVB-INTERFACES
      input int unsigned si,       // P-N-STREAM-IN
      input int unsigned so,       // P-N-STREAM-OUT
      input int unsigned n_ctrl,   // P-N-CONTROLLERS
      input int unsigned ca_pool,  // P-CA-POOL
      input bit          en_srp    // P-EN-SRP-ENGINE
  );
    return n_if + si + si + so + (32'd2 * n_ctrl * n_if) + ca_pool + 32'd5
         + (en_srp ? ((32'd7 + si + so) * n_if) : 32'd0);
  endfunction

  // F01.5 defaults through the formula — never restated as literals.
  localparam int unsigned PP_TIMER_SLOTS_BASE_C =
      pp_timer_slots(1, 8, 8, 16, 4, 1'b0);                    // = 66
  localparam int unsigned PP_TIMER_SLOTS_C =
      pp_timer_slots(1, 8, 8, 16, 4, 1'b1);                    // = 89

endpackage : pp_pkg
`default_nettype wire
