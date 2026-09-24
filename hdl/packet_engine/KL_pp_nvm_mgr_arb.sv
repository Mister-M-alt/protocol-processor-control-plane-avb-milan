/*
 * SPDX-FileCopyrightText: 2026 Kebag Logic
 * SPDX-License-Identifier: CERN-OHL-W-2.0
 */
//---------------------------------------------------------------------------//
//  File        : KL_pp_nvm_mgr_arb.sv
//  Project     : IEEE 1722.1 protocol processor (docs/architecture/02 §8
//                F02.8 manager face, 07 §5.3 F07.9 restore; processor issue
//                #93, S3)
//
//  Description : Two record managers in front of ONE KL_pp_nvm_port. The
//                port has one manager face and serves one operation at a
//                time; this block puts the binding manager
//                (KL_acmp_nvm_shadow, manager 0) and a second record writer
//                (manager 1, the saved-state writer of the integrating
//                platform's contract; tied idle until it lands) in front of
//                it, so the device face keeps exactly one sequential
//                initiator and the backend sees nothing new.
//
//                OWNERSHIP IS PER OPERATION. The manager whose request the
//                port accepted owns every data phase and the done or err
//                (with its cause) that ends it; the other manager sees none
//                of them.
//
//                THE ONE RULE THAT MATTERS. Manager 0 raises a one-cycle
//                REGISTERED request after it reads the port idle, so its
//                request lands one cycle after the sample. The busy it reads
//                must therefore cover every cycle in which a request landing
//                next cycle could not be issued: while manager 1 owns the
//                port AND in the cycle manager 1 is granted. With that, a
//                manager-0 request only ever meets an idle port and is issued
//                at once; nothing is held here. Without the grant-cycle term
//                a request that lands on manager 1's grant is lost and the
//                binding manager waits for its data phase for ever. Manager 1
//                holds its request until m1_gnt_o. On a tie at an idle port
//                manager 0 wins.
//
//                AN ABANDONED READ IS DRAINED, NEVER HANDED ON. The port
//                answers untagged: its bytes and its done or err name no
//                operation. So when the manager that owns a READ abandons it
//                (m0_abort_i, the binding walk's deadline; m1_abort_i,
//                manager 1's), this block keeps that operation as its OWN: it
//                holds rready so the port can move every late byte, discards
//                them, swallows the done or err that ends it, and keeps both
//                managers off the port until then. A late response can only
//                ever end the operation it belongs to. The drain ends ONLY on
//                that operation's own done or err, never on time: a device
//                that never answers keeps the port QUARANTINED until reset,
//                and every later change reads pending, never durable. Nothing
//                here makes the port reusable before the device ends the
//                operation; that needs a real cancellation, which is the
//                port's own open recovery contract (processor issue #15).
//                Only reads are abandoned: an abort presented while a WRITE
//                is owned is ignored, and a write stream is never cut here.
//---------------------------------------------------------------------------//
`default_nettype none

module KL_pp_nvm_mgr_arb (
    input  wire        clk_i,            //! core clock (P-CLK-HZ)
    input  wire        rst_n,            //! sync active-low reset

    //! ---- manager 0: the binding manager (one-cycle request) --------------
    input  wire        m0_req_i,         //! registered one-cycle op request
    input  wire        m0_we_i,          //! 1 = commit, 0 = restore
    input  wire [7:0]  m0_rid_i,         //! record id
    input  wire        m0_wvalid_i,      //! commit byte present
    input  wire [7:0]  m0_wdata_i,       //! commit byte
    input  wire        m0_rready_i,      //! restore byte accepted
    output logic       m0_wready_o,      //! port accepts the commit byte
    output logic       m0_rvalid_o,      //! restore byte present
    output logic [7:0] m0_rdata_o,       //! restore byte
    output logic       m0_busy_o,        //! the port cannot take a request next cycle
    output logic       m0_done_o,        //! its operation completed
    output logic       m0_err_o,         //! its operation failed
    output logic [1:0] m0_err_cause_o,   //! the port's cause with m0_err_o
    input  wire        m0_abort_i,       //! abandon the READ it owns: drain it

    //! ---- manager 1: a second record writer (request held until granted) --
    input  wire        m1_req_i,         //! op request, held until m1_gnt_o
    input  wire        m1_we_i,          //! 1 = commit, 0 = restore
    input  wire [7:0]  m1_rid_i,         //! record id
    input  wire        m1_wvalid_i,      //! commit byte present
    input  wire [7:0]  m1_wdata_i,       //! commit byte
    input  wire        m1_rready_i,      //! restore byte accepted
    output logic       m1_gnt_o,         //! its request was issued this cycle
    output logic       m1_wready_o,      //! port accepts the commit byte
    output logic       m1_rvalid_o,      //! restore byte present
    output logic [7:0] m1_rdata_o,       //! restore byte
    output logic       m1_done_o,        //! its operation completed
    output logic       m1_err_o,         //! its operation failed
    output logic [1:0] m1_err_cause_o,   //! the port's cause with m1_err_o
    input  wire        m1_abort_i,       //! abandon the READ it owns: drain it

    //! ---- the port's manager face (KL_pp_nvm_port) --------------------------
    output logic       p_req_o,          //! op request (port idle only)
    output logic       p_we_o,           //! 1 = commit, 0 = restore
    output logic [7:0] p_rid_o,          //! record id
    output logic       p_wvalid_o,       //! commit byte present
    output logic [7:0] p_wdata_o,        //! commit byte
    output logic       p_rready_o,       //! restore byte accepted
    input  wire        p_wready_i,       //! port accepts the commit byte
    input  wire        p_rvalid_i,       //! restore byte present
    input  wire  [7:0] p_rdata_i,        //! restore byte
    input  wire        p_busy_i,         //! op in flight
    input  wire        p_done_i,         //! one-cycle pulse: op complete
    input  wire        p_err_i,          //! one-cycle pulse: op failed
    input  wire  [1:0] p_err_cause_i,    //! the port's terminal cause

    //! ---- observability ------------------------------------------------------
    output logic       dbg_drain_o       //! an abandoned read is being drained
);

  typedef enum logic [1:0] { O_NONE, O_M0, O_M1 } own_e;

  own_e       own_r;
  logic       we_r;        // the owned operation is a commit
  logic       drain_r;     // the owned READ was abandoned and is being drained
  logic       idle_w, iss0_w, iss1_w, end_w;

  //! the port takes a request only in S_IDLE, which follows its done or err
  //! pulse by one cycle; ownership retires on that pulse
  assign idle_w = (own_r == O_NONE) && !p_busy_i && !p_done_i && !p_err_i;
  assign iss0_w = idle_w && m0_req_i;
  assign iss1_w = idle_w && !m0_req_i && m1_req_i;
  assign end_w  = (own_r != O_NONE) && (p_done_i || p_err_i);

  always_ff @(posedge clk_i) begin : own_ff
    if (!rst_n) begin
      own_r <= O_NONE;
      we_r  <= 1'b0;
    end else if (iss0_w) begin
      own_r <= O_M0;
      we_r  <= m0_we_i;
    end else if (iss1_w) begin
      own_r <= O_M1;
      we_r  <= m1_we_i;
    end else if (end_w) begin
      own_r <= O_NONE;
    end
  end

  //! the drain starts on its owner's abort of a READ still open and ends
  //! with that operation's own done or err, never on time
  always_ff @(posedge clk_i) begin : drain_ff
    if (!rst_n) begin
      drain_r <= 1'b0;
    end else if (end_w) begin
      drain_r <= 1'b0;
    end else if (!we_r && (((own_r == O_M0) && m0_abort_i)
                           || ((own_r == O_M1) && m1_abort_i))) begin
      drain_r <= 1'b1;
    end
  end

  assign p_req_o    = iss0_w || iss1_w;
  assign p_we_o     = iss0_w ? m0_we_i  : m1_we_i;
  assign p_rid_o    = iss0_w ? m0_rid_i : m1_rid_i;
  assign p_wvalid_o = (own_r == O_M0) ? m0_wvalid_i
                    : (own_r == O_M1) ? m1_wvalid_i : 1'b0;
  assign p_wdata_o  = (own_r == O_M1) ? m1_wdata_i : m0_wdata_i;
  assign p_rready_o = (own_r == O_M0) ? (drain_r || m0_rready_i)
                    : (own_r == O_M1) ? (drain_r || m1_rready_i) : 1'b0;

  //! manager 0 strobes only when it reads idle; any other owner reads busy,
  //! and so does the cycle manager 1 is granted (the banner's one rule)
  assign m0_busy_o      = p_busy_i || (own_r == O_M1) || iss1_w;
  assign m0_wready_o    = (own_r == O_M0) && !drain_r && p_wready_i;
  assign m0_rvalid_o    = (own_r == O_M0) && !drain_r && p_rvalid_i;
  assign m0_rdata_o     = p_rdata_i;
  assign m0_done_o      = (own_r == O_M0) && !drain_r && p_done_i;
  assign m0_err_o       = (own_r == O_M0) && !drain_r && p_err_i;
  assign m0_err_cause_o = m0_err_o ? p_err_cause_i : 2'd0;

  //! a drained operation's bytes and its ending reach no manager
  assign m1_gnt_o       = iss1_w;
  assign m1_wready_o    = (own_r == O_M1) && !drain_r && p_wready_i;
  assign m1_rvalid_o    = (own_r == O_M1) && !drain_r && p_rvalid_i;
  assign m1_rdata_o     = p_rdata_i;
  assign m1_done_o      = (own_r == O_M1) && !drain_r && p_done_i;
  assign m1_err_o       = (own_r == O_M1) && !drain_r && p_err_i;
  assign m1_err_cause_o = m1_err_o ? p_err_cause_i : 2'd0;

  assign dbg_drain_o = drain_r;

endmodule

`default_nettype wire
