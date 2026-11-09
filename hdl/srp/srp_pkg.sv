/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : srp_pkg.sv
//  Project     : IEEE 1722.1 protocol processor (docs/architecture/10 §3)
//
//  Description : Shared SRP MRPDU constants — the F10.8 attribute-type and
//                FirstValue-length table, the three-packed attribute-event
//                alphabet (802.1Q §35.2.2.7, encoded (((e1*6)+e2)*6)+e3),
//                the Listener four-packed declaration codes, the
//                VectorHeader LeaveAllEvent code (802.1Q §10.8.2.6), the
//                per-AttributeType received-LeaveAll lane indices, the
//                class-A identity the 10 §3 vector-value-k rule keys on
//                (SRclassID 6 = SR class A, 802.1Q Table 35-7), and the
//                802.1Q-2018 Table 10-3 Applicant the Domain and VID
//                participants run (issue #216).
//
//                The one decision that matters: the F10.8 FirstValue
//                lengths are the framing truth for vector walking — the
//                on-wire AttributeLength is VALIDATED against them, and a
//                mismatch is a malformed vector attribute (tolerance
//                discard, Milan §4.2.7.1.2), never a resync hint. The MSRP
//                AttributeListLength is likewise only ever counted; the
//                dual EndMark frames the PDU.
//---------------------------------------------------------------------------//
`default_nettype none

package srp_pkg;

  // ---- MSRP attribute types (802.1Q §35.2.2.4, F10.8) --------------------
  localparam logic [7:0] SRP_MSRP_ATTR_TALKER_ADV_C    = 8'd1;
  localparam logic [7:0] SRP_MSRP_ATTR_TALKER_FAILED_C = 8'd2;
  localparam logic [7:0] SRP_MSRP_ATTR_LISTENER_C      = 8'd3;
  localparam logic [7:0] SRP_MSRP_ATTR_DOMAIN_C        = 8'd4;

  // ---- MVRP attribute types (802.1Q §11.2.3.1.6, F10.8) ------------------
  localparam logic [7:0] SRP_MVRP_ATTR_VID_C           = 8'd1;

  // ---- FirstValue lengths in octets (F10.7 / F10.8) ----------------------
  localparam logic [7:0] SRP_FV_LEN_TALKER_ADV_C    = 8'd25;
  localparam logic [7:0] SRP_FV_LEN_TALKER_FAILED_C = 8'd34;  // + system_id 8 + failure_code 1
  localparam logic [7:0] SRP_FV_LEN_LISTENER_C      = 8'd8;
  localparam logic [7:0] SRP_FV_LEN_DOMAIN_C        = 8'd4;
  localparam logic [7:0] SRP_FV_LEN_VID_C           = 8'd2;

  // ---- three-packed attribute events (10 §3: 0 New .. 5 Lv) --------------
  typedef enum logic [2:0] {
    SRP_EV_NEW    = 3'd0,
    SRP_EV_JOININ = 3'd1,
    SRP_EV_IN     = 3'd2,
    SRP_EV_JOINMT = 3'd3,
    SRP_EV_MT     = 3'd4,
    SRP_EV_LV     = 3'd5
  } srp_event_e;

  // ---- Listener four-packed declaration codes (F10.8 row 3) --------------
  typedef enum logic [1:0] {
    SRP_DECL_IGNORE        = 2'd0,
    SRP_DECL_ASKING_FAILED = 2'd1,
    SRP_DECL_READY         = 2'd2,
    SRP_DECL_READY_FAILED  = 2'd3
  } srp_decl_e;

  // ---- Domain identity (Milan §4.2.7: only SR class A exists) ------------
  localparam logic [7:0] SRP_CLASS_A_ID_C  = 8'd6;

  // ---- VectorHeader LeaveAllEvent code (802.1Q §10.8.2.6) ----------------
  localparam logic [2:0] SRP_LEAVEALL_EV_C = 3'd1;

  // ---- received MSRP LeaveAll lanes, one per AttributeType ---------------
  // lane = AttributeType - 1: the decoder's la_msrp_o and the stream FSMs'
  // leaveall_rx_i carry one bit per MSRP type (802.1Q-2014 §10.8.2.6)
  localparam int unsigned SRP_LA_TALKER_ADV_C    = 0;
  localparam int unsigned SRP_LA_TALKER_FAILED_C = 1;
  localparam int unsigned SRP_LA_LISTENER_C      = 2;
  localparam int unsigned SRP_LA_DOMAIN_C        = 3;

  // ---- Applicant state machine (802.1Q-2018 10.7.7, Table 10-3) ----------
  // The Domain and VID participants' Applicants (issue #216). States in the
  // order the table prints them; events by its rows (a received event's row
  // through srp_app_rx_next).
  localparam logic [3:0] SRP_AP_VO_C = 4'd0;
  localparam logic [3:0] SRP_AP_VP_C = 4'd1;
  localparam logic [3:0] SRP_AP_VN_C = 4'd2;
  localparam logic [3:0] SRP_AP_AN_C = 4'd3;
  localparam logic [3:0] SRP_AP_AA_C = 4'd4;
  localparam logic [3:0] SRP_AP_QA_C = 4'd5;
  localparam logic [3:0] SRP_AP_LA_C = 4'd6;
  localparam logic [3:0] SRP_AP_AO_C = 4'd7;
  localparam logic [3:0] SRP_AP_QO_C = 4'd8;
  localparam logic [3:0] SRP_AP_AP_C = 4'd9;
  localparam logic [3:0] SRP_AP_QP_C = 4'd10;
  localparam logic [3:0] SRP_AP_LO_C = 4'd11;

  localparam logic [3:0] SRP_AE_NEW_C     = 4'd0;   // New!
  localparam logic [3:0] SRP_AE_JOIN_C    = 4'd1;   // Join!
  localparam logic [3:0] SRP_AE_LV_C      = 4'd2;   // Lv!
  localparam logic [3:0] SRP_AE_RNEW_C    = 4'd3;   // rNew!
  localparam logic [3:0] SRP_AE_RJOININ_C = 4'd4;   // rJoinIn!
  localparam logic [3:0] SRP_AE_RIN_C     = 4'd5;   // rIn!
  localparam logic [3:0] SRP_AE_RJOINMT_C = 4'd6;   // rJoinMt!
  localparam logic [3:0] SRP_AE_RMT_C     = 4'd7;   // rMt!
  localparam logic [3:0] SRP_AE_RLV_C     = 4'd8;   // rLv!
  localparam logic [3:0] SRP_AE_RLA_C     = 4'd9;   // rLA!
  localparam logic [3:0] SRP_AE_PER_C     = 4'd10;  // periodic!
  localparam logic [3:0] SRP_AE_TX_C      = 4'd11;  // tx!
  localparam logic [3:0] SRP_AE_TXLA_C    = 4'd12;  // txLA!

  //! Table 10-3 next state, row by row; a cell not named holds ("-"). reg_in
  //! is note 8 (tx! in AN: QA with the Registrar IN, AA otherwise); p2p is
  //! operPointToPointMAC for notes 4 and 5 (rJoinIn! ignored in VO and VP,
  //! rIn! acts in AA only when point-to-point)
  function automatic logic [3:0] srp_app_next(input logic [3:0] st,
                                              input logic [3:0] ev,
                                              input logic       reg_in,
                                              input logic       p2p);
    logic [3:0] nx;
    nx = st;
    case (ev)
      SRP_AE_NEW_C:  if (!(st inside {SRP_AP_VN_C, SRP_AP_AN_C})) nx = SRP_AP_VN_C;
      SRP_AE_JOIN_C: begin
        case (st)
          SRP_AP_VO_C: nx = SRP_AP_VP_C;
          SRP_AP_LA_C: nx = SRP_AP_AA_C;
          SRP_AP_AO_C: nx = SRP_AP_AP_C;
          SRP_AP_QO_C: nx = SRP_AP_QP_C;
          SRP_AP_LO_C: nx = SRP_AP_VP_C;
          default:     nx = st;
        endcase
      end
      SRP_AE_LV_C: begin
        case (st)
          SRP_AP_VP_C: nx = SRP_AP_VO_C;
          SRP_AP_VN_C, SRP_AP_AN_C, SRP_AP_AA_C, SRP_AP_QA_C: nx = SRP_AP_LA_C;
          SRP_AP_AP_C: nx = SRP_AP_AO_C;
          SRP_AP_QP_C: nx = SRP_AP_QO_C;
          default:     nx = st;
        endcase
      end
      SRP_AE_RJOININ_C: begin
        case (st)
          SRP_AP_VO_C: nx = p2p ? st : SRP_AP_AO_C;   // note 4
          SRP_AP_VP_C: nx = p2p ? st : SRP_AP_AP_C;   // note 4
          SRP_AP_AA_C: nx = SRP_AP_QA_C;
          SRP_AP_AO_C: nx = SRP_AP_QO_C;
          SRP_AP_AP_C: nx = SRP_AP_QP_C;
          default:     nx = st;
        endcase
      end
      SRP_AE_RIN_C: if (p2p && (st == SRP_AP_AA_C)) nx = SRP_AP_QA_C;   // note 5
      SRP_AE_RJOINMT_C, SRP_AE_RMT_C: begin
        case (st)
          SRP_AP_QA_C: nx = SRP_AP_AA_C;
          SRP_AP_QO_C: nx = SRP_AP_AO_C;
          SRP_AP_QP_C: nx = SRP_AP_AP_C;
          SRP_AP_LO_C: nx = SRP_AP_VO_C;
          default:     nx = st;
        endcase
      end
      SRP_AE_RLV_C, SRP_AE_RLA_C: begin
        case (st)
          SRP_AP_VO_C, SRP_AP_AO_C, SRP_AP_QO_C: nx = SRP_AP_LO_C;
          SRP_AP_AN_C: nx = SRP_AP_VN_C;
          SRP_AP_AA_C, SRP_AP_QA_C, SRP_AP_AP_C, SRP_AP_QP_C: nx = SRP_AP_VP_C;   // note 9
          default:     nx = st;                // VP, VN, LA (note 10), LO
        endcase
      end
      SRP_AE_PER_C: begin
        case (st)
          SRP_AP_QA_C: nx = SRP_AP_AA_C;
          SRP_AP_QP_C: nx = SRP_AP_AP_C;
          default:     nx = st;
        endcase
      end
      SRP_AE_TX_C: begin
        case (st)
          SRP_AP_VP_C: nx = SRP_AP_AA_C;
          SRP_AP_VN_C: nx = SRP_AP_AN_C;
          SRP_AP_AN_C: nx = reg_in ? SRP_AP_QA_C : SRP_AP_AA_C;   // note 8
          SRP_AP_AA_C: nx = SRP_AP_QA_C;
          SRP_AP_LA_C: nx = SRP_AP_VO_C;
          SRP_AP_AP_C: nx = SRP_AP_QA_C;
          SRP_AP_LO_C: nx = SRP_AP_VO_C;
          default:     nx = st;                // VO, QA, AO, QO, QP
        endcase
      end
      SRP_AE_TXLA_C: begin
        case (st)
          SRP_AP_VO_C, SRP_AP_LA_C, SRP_AP_AO_C, SRP_AP_QO_C: nx = SRP_AP_LO_C;
          SRP_AP_VP_C: nx = SRP_AP_AA_C;
          SRP_AP_VN_C: nx = SRP_AP_AN_C;
          SRP_AP_AN_C, SRP_AP_AA_C, SRP_AP_AP_C, SRP_AP_QP_C: nx = SRP_AP_QA_C;
          default:     nx = st;                // QA, LO
        endcase
      end
      default: nx = st;                        // rNew!: every cell "-"
    endcase
    return nx;
  endfunction

  //! Table 10-3 next state on a received attribute event (10.7.5.14 to
  //! 10.7.5.19; e an srp_event_e, New .. Lv for rNew! .. rLv!), as a choice
  //! between the rows: each row is a small function of the state, where one
  //! function of the state and the event together maps several times larger
  function automatic logic [3:0] srp_app_rx_next(input logic [3:0] st,
                                                 input logic [2:0] e,
                                                 input logic       p2p);
    logic [3:0] nx;
    case (e)
      SRP_EV_JOININ: nx = srp_app_next(st, SRP_AE_RJOININ_C, 1'b0, p2p);
      SRP_EV_IN:     nx = srp_app_next(st, SRP_AE_RIN_C, 1'b0, p2p);
      SRP_EV_JOINMT: nx = srp_app_next(st, SRP_AE_RJOINMT_C, 1'b0, p2p);
      SRP_EV_MT:     nx = srp_app_next(st, SRP_AE_RMT_C, 1'b0, p2p);
      SRP_EV_LV:     nx = srp_app_next(st, SRP_AE_RLV_C, 1'b0, p2p);
      default:       nx = st;                      // rNew!: every cell "-"
    endcase
    return nx;
  endfunction

  //! Table 10-3 action of a transmit opportunity: [3] send, [2:0] the
  //! srp_event_e it encodes. sJ and s encode against reg_in (10.7.6.3,
  //! 10.7.6.5); the bracketed [s], [sJ] are optional and not sent
  function automatic logic [3:0] srp_app_msg(input logic [3:0] st,
                                             input logic       txla,
                                             input logic       reg_in);
    logic [2:0] sj;
    logic [2:0] s;
    logic [3:0] m;
    sj = reg_in ? SRP_EV_JOININ : SRP_EV_JOINMT;
    s  = reg_in ? SRP_EV_IN : SRP_EV_MT;
    m  = 4'd0;
    if (!txla) begin
      case (st)
        SRP_AP_VP_C, SRP_AP_AA_C, SRP_AP_AP_C: m = {1'b1, sj};
        SRP_AP_VN_C, SRP_AP_AN_C:             m = {1'b1, SRP_EV_NEW};
        SRP_AP_LA_C:                          m = {1'b1, SRP_EV_LV};
        SRP_AP_LO_C:                          m = {1'b1, s};
        default:                              m = 4'd0;   // VO, QA, AO, QO, QP
      endcase
    end else begin
      case (st)
        SRP_AP_VP_C:                          m = {1'b1, s};
        SRP_AP_VN_C, SRP_AP_AN_C:             m = {1'b1, SRP_EV_NEW};
        SRP_AP_AA_C, SRP_AP_QA_C, SRP_AP_AP_C, SRP_AP_QP_C: m = {1'b1, sj};
        default:                              m = 4'd0;   // VO, LA, AO, QO, LO
      endcase
    end
    return m;
  endfunction

  //! the Applicant states that declare the attribute: every state but the
  //! observers (VO, AO, QO), LA and LO
  function automatic logic srp_app_declaring(input logic [3:0] st);
    return st inside {SRP_AP_VP_C, SRP_AP_VN_C, SRP_AP_AN_C, SRP_AP_AA_C,
                      SRP_AP_QA_C, SRP_AP_AP_C, SRP_AP_QP_C};
  endfunction

  //! three-packed digit extract — idx 0 = first value (b / 36),
  //! idx 1 = (b / 6) % 6, idx 2 = b % 6. A digit > 5 (reachable only at
  //! idx 0, for b > 215) is outside the event alphabet and is the
  //! malformed-vector signal the decoder's tolerance path keys on.
  function automatic logic [2:0] srp_tp_digit(
      input logic [7:0] b,
      input logic [1:0] idx
  );
    logic [7:0] q;
    unique case (idx)
      2'd0:    q = b / 8'd36;
      2'd1:    q = (b / 8'd6) % 8'd6;
      default: q = b % 8'd6;
    endcase
    return q[2:0];
  endfunction

endpackage : srp_pkg
`default_nettype wire
