/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : KL_pp_acmp_lsn_admit.sv
//  Project     : IEEE 1722.1 protocol processor (docs/architecture/05 §5
//                NVM shadow, 07 §5.3 F07.9 boot restore before enable;
//                Milan §5.3.8 saved binding parameters, §5.5.3 listener
//                state machine)
//
//  Description : Boot-owned admission of the ACMP listener's four work
//                faces. KL_pp_acmp_listener serves one work item at a time
//                and ranks a START/STOP holder, a dispatch transaction, a
//                pending timer expiry and a talker event above the boot
//                restore's preload, so pre_ready_o is low while any of them
//                is present. Every walk it runs also ends in a record
//                write-back that KL_acmp_nvm_shadow captures as a live
//                change. During the binding walk, then, even a read-only
//                GET_RX_STATE of a sink whose saved record was stored and
//                not yet preloaded withdraws that sink's restored binding,
//                and the listener's still-unbound record is flushed over the
//                saved one (processor issue #92). A talker event held as a
//                level holds the preload phase for ever (issue #93, S4).
//
//                This gate OWNS the four faces from the hard reset to the
//                binding walk's drained terminal and admits nothing but the
//                preload meanwhile:
//                - the dispatch transaction and the talker event are HELD at
//                  their producers. On those two faces ready is an
//                  acceptance, so the valid towards the listener AND the
//                  ready towards the producer are masked: the ACMP head
//                  stays in its dispatch queue (it is not admitted to the
//                  scoreboard either, whose candidacy reads this ready) and
//                  the event router's sticky latch stays set. Nothing is
//                  consumed that the listener did not see.
//                - the AECP engine's START/STOP request has its VALID
//                  masked; the listener's completion passes unmasked. That
//                  face is not an acceptance handshake: the engine holds its
//                  request until the completion, the listener captures a
//                  presented request into its holder in any state, and the
//                  holder and its done/fail flags reset to 0 and fill only
//                  from a presented request. The gate and the listener take
//                  the same hard reset, so while the gate owns the faces the
//                  holder is empty and no completion can fire.
//                - a timer expiry is not admitted. The listener arms timers
//                  only in the walks the gate holds off, and the timer
//                  service's armed bits reset to 0, so no expiry of a
//                  listener owner can be legitimate while the gate owns the
//                  faces. Each one that arrives is counted, never queued.
//                With nothing else admitted, the listener's reachable states
//                are X_INIT, X_IDLE and X_PRELOAD, so pre_ready_o is 1 in
//                every X_IDLE cycle and a preload is taken in the cycle it
//                is presented: the only record writes the binding manager
//                can capture during its walk are the preloads' own, which
//                compare equal to the restored image.
//
//                RELEASE is one-way until the next hard reset. It needs the
//                binding manager's walk at its terminal (restore_done_o,
//                failed or not), no preload presented, the listener idle and
//                its last preload's A4 discovery-arm strobe gone. The
//                manager never walks again before a reset, so the gate never
//                owns the faces again before one. released_o is the binding
//                walk's END: the listener's live work starts on it, and the
//                top's restore_done_o (which releases the entity enable)
//                takes it, so no enable precedes the last preload's record
//                write and discovery arm.
//
//                Consequences, stated so no integrator relies on the
//                opposite: an ACMP command, a talker event or a START/STOP
//                request arriving before the release is served AFTER it,
//                ordered after the restored image, so a live change still
//                wins by coming later and a read-only command no longer
//                withdraws a restored binding. A boot that never asserts
//                restore_go_i leaves the listener owned until reset. There
//                is no fairness promise among the listener's sources after
//                the release: its own priorities apply again. What a
//                producer does with arrivals behind a held head is that
//                producer's own queue policy, unchanged.
//---------------------------------------------------------------------------//
`default_nettype none

module KL_pp_acmp_lsn_admit #(
    //! P-N-STREAM-IN (F01.5): the listener's sinks, its timer owner range
    parameter int unsigned N_SINKS_P        = 8,
    //! the listener's TMR_OWNER_BASE_P: its owners are BASE .. BASE+N-1
    parameter int unsigned TMR_OWNER_BASE_P = 32,
    //! the timer service's owner tag width (pp_pkg PP_TIMER_OWNER_W_C)
    parameter int unsigned OWNER_W_P        = 8
) (
    input  wire                 clk_i,           //! core clock (P-CLK-HZ)
    input  wire                 rst_n,           //! sync active-low HARD reset

    //! ---- release: the binding walk's drained terminal --------------------
    input  wire                 walk_done_i,     //! KL_acmp_nvm_shadow restore_done_o
    input  wire                 pre_valid_i,     //! its preload request
    input  wire                 lsn_busy_i,      //! listener dbg_busy_o (not in X_IDLE)
    input  wire                 lsn_arm_i,       //! listener act_disc_arm_o (A4 strobe)
    output logic                own_o,           //! the gate owns the listener's faces
    output logic                released_o,      //! the binding walk's drained terminal

    //! ---- dispatch transaction face (valid AND ready held) ----------------
    input  wire                 p_txn_valid_i,   //! the producer's head present
    output logic                p_txn_ready_o,   //! the producer's pop
    output logic                l_txn_valid_o,   //! to the listener's txn_valid_i
    input  wire                 l_txn_ready_i,   //! the listener's txn_ready_o

    //! ---- talker event face (valid AND ready held) ------------------------
    input  wire                 p_tk_valid_i,    //! the router's sticky event
    output logic                p_tk_ready_o,    //! the router's acknowledge
    output logic                l_tk_valid_o,    //! to the listener's evt_tk_valid_i
    input  wire                 l_tk_ready_i,    //! the listener's evt_tk_ready_o

    //! ---- START/STOP face (valid masked, completion passes) ---------------
    input  wire                 p_strm_valid_i,  //! the AECP engine's held request
    output logic                l_strm_valid_o,  //! to the listener's strm_set_valid_i

    //! ---- timer expiry bus ------------------------------------------------
    input  wire                 p_exp_valid_i,   //! the timer service's expiry strobe
    input  wire [OWNER_W_P-1:0] p_exp_owner_i,   //! its owner tag
    output logic                l_exp_valid_o,   //! to the listener's tmr_exp_valid_i

    //! expiries of a listener owner that arrived while the gate owned the
    //! faces (saturating). Only an arm the listener never issued makes one,
    //! so a permanent 0 is the only healthy reading.
    output logic [15:0]         dbg_exp_drop_o
);

  logic        own_r;
  logic        drained_w;
  logic        lsn_exp_w;
  logic [15:0] drop_r;

  //! KL_acmp_nvm_shadow raises its terminal one cycle after its last
  //! preload was taken (H_FIN), by which time the listener's X_PRELOAD
  //! record write is done and its A4 strobe is up, so with that manager only
  //! walk_done_i and lsn_arm_i decide the cycle. The pre_valid and busy terms
  //! state the contract rather than lean on that timing: a manager whose
  //! terminal coincided with its last offer or with X_PRELOAD would still be
  //! released after them. tb/lsn_admit grades every term on its own.
  assign drained_w = walk_done_i && !pre_valid_i && !lsn_busy_i && !lsn_arm_i;

  always_ff @(posedge clk_i) begin : own_ff
    if (!rst_n) begin
      own_r <= 1'b1;
    end else if (drained_w) begin
      own_r <= 1'b0;
    end
  end

  assign own_o      = own_r;
  assign released_o = !own_r;

  //! held at the producer: masked both ways, so a pop is exactly a take
  assign l_txn_valid_o  = p_txn_valid_i  && !own_r;
  assign p_txn_ready_o  = l_txn_ready_i  && !own_r;
  assign l_tk_valid_o   = p_tk_valid_i   && !own_r;
  assign p_tk_ready_o   = l_tk_ready_i   && !own_r;
  assign l_strm_valid_o = p_strm_valid_i && !own_r;

  //! the expiry bus reaches the listener only once it is released
  assign l_exp_valid_o = p_exp_valid_i && !own_r;
  assign lsn_exp_w     = p_exp_valid_i
                         && (32'(p_exp_owner_i) >= TMR_OWNER_BASE_P)
                         && (32'(p_exp_owner_i) <  TMR_OWNER_BASE_P + N_SINKS_P);

  always_ff @(posedge clk_i) begin : drop_ff
    if (!rst_n) begin
      drop_r <= 16'd0;
    end else if (own_r && lsn_exp_w && (drop_r != 16'hFFFF)) begin
      drop_r <= drop_r + 16'd1;
    end
  end

  assign dbg_exp_drop_o = drop_r;

endmodule

`default_nettype wire
