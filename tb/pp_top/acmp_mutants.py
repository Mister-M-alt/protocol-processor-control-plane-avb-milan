#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant the ACMP lane's negative controls in isolated copies; each must fail its named checks.

Each mutant is one or more exact source edits (every old text must occur exactly
once) planted in a private copy of hdl/, tb/common/ and the one suite that grades
it. The copy is built and run in the foreground. A mutant is KILLED only when its
build succeeds, its run completes with the suite's tally, exits non-zero, and every
check named for it fails. A refused edit, a failed build, a crash or a missing tally
never counts as a kill. A golden copy of every suite in use runs first and must pass;
the tree this script lives in is never touched.

The controls are those of processor issues #47 (ACMP messages outside the listener
and talker sets are inert; the probe-response guard per term), #45 (ACMPDUs longer
than 56 bytes are accepted), #48 (the integrated settle path from the listener
through the top's SRP service stage to the SRP listener matcher and back), #639
(the listener records in distributed RAM, and the top's timer arm-port queues as
rings in distributed RAM), #188 (a rebind settled before tx! still sends the old
Listener Leave), #189 (REGISTERING_FAILED follows the in-place Talker
Advertise<->Failed swap, also when two swaps coalesce in the event router) and
#190 (the same service stage takes each engine-driven DECLARE_TALKER's TSpec
MaxFrameSize from its own source's slot) and #202 (after a MAAP conflict the
talker declares a new, distinct Stream ID at the grant, publishes it and
answers with it, while the same-Stream-ID PCP path keeps its gap, no move
takes an ID declared inside the quarantine, and a fast return still sends
the withdrawn ID's Leave). The suite READMEs carry the matching mutation
records.

Usage: python3 tb/pp_top/acmp_mutants.py --output DIR [--verilator V] [--jobs N]
                                         [--only NAME ...]
"""

import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import NamedTuple


class Suite(NamedTuple):
    """Where a suite lives, how it builds, and how its graded run starts."""
    directory: str
    build: tuple[str, ...]
    run: tuple[str, ...]


class Mutant(NamedTuple):
    """One planted defect: its exact edits and the checks that must fail."""
    name: str
    suite: Suite
    edits: tuple[tuple[str, str, str], ...]
    checks: tuple[str, ...]


ACMP_LISTENER = Suite("tb/acmp_listener", (), ("make", "run"))
RX_VALIDATOR = Suite("tb/rx_validator", (), ("make", "run"))
PP_TOP = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--acmp-only"))
PP_TOP_AQ = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--arm-queue-only"))

LISTENER = "hdl/acmp/KL_pp_acmp_listener.sv"
VALIDATOR = "hdl/packet_engine/KL_pp_rx_validator.sv"

MSG_OK = "  assign txn_msg_ok_w   = (txn_i.msg_type == AMSG_PROBE_TX_RESP_C)\n"
GUARD = ("  assign probe_match_w = probe_ctlr_match_w\n"
         "                      && (tk_eid_f_r == rec_r.talker_eid)\n"
         "                      && (tk_uid_f_r == rec_r.talker_uid)\n"
         "                      && (seq_x_r == rec_r.probe_seq);\n")


def guard_without(term: str) -> str:
    """The probe guard with one of its four terms replaced by a constant true."""
    return GUARD.replace(term, "1'b1", 1)


# issue #47: messages outside the listener set, and the guard per term
INERT = (
    Mutant("msg_ok_forced", ACMP_LISTENER, (
        (LISTENER, MSG_OK,
         "  assign txn_msg_ok_w   = 1'b1 || (txn_i.msg_type == AMSG_PROBE_TX_RESP_C)\n"),),
        ("B13 msg 7 status 0 in PWR", "B13 msg 14 status 0 in PWR",
         "B13 msg 3 status 5 in PW2", "B13 msg 15 status 5 in PW2")),
    Mutant("msg_ok_forced", PP_TOP, (
        (LISTENER, MSG_OK,
         "  assign txn_msg_ok_w   = 1'b1 || (txn_i.msg_type == AMSG_PROBE_TX_RESP_C)\n"),),
        ("AI3: the sink never left PRB_W_RESP",)),
    Mutant("guard_ctlr_dropped", ACMP_LISTENER, (
        (LISTENER, GUARD, guard_without("probe_ctlr_match_w")),),
        ("B14 wrong controller_entity_id in PWR", "B14 wrong controller_entity_id in PW2")),
    Mutant("guard_talker_eid_dropped", ACMP_LISTENER, (
        (LISTENER, GUARD, guard_without("(tk_eid_f_r == rec_r.talker_eid)")),),
        ("B14 wrong talker_entity_id in PWR", "B14 wrong talker_entity_id in PW2")),
    Mutant("guard_talker_uid_dropped", ACMP_LISTENER, (
        (LISTENER, GUARD, guard_without("(tk_uid_f_r == rec_r.talker_uid)")),),
        ("B14 wrong talker_unique_id in PWR", "B14 wrong talker_unique_id in PW2")),
)

# issue #45: a front end that accepts ACMP only at cdl 44 (the 56-B form)
V1_END = "                      && (lim_w <= 12'(BYTES_P));\n"
CDL_44_ONLY = ("                      && (lim_w <= 12'(BYTES_P))\n"
               "                      && !((subtype_r == SUB_ACMP_C) && (cdl_r != 11'd44));\n")

LONG_FORM = (
    Mutant("cdl_not_44_rejected", RX_VALIDATOR, ((VALIDATOR, V1_END, CDL_44_ONLY),),
           ("F30 BIND_RX cdl 84", "F30 PROBE_TX cdl 84")),
    Mutant("cdl_not_44_rejected", PP_TOP, ((VALIDATOR, V1_END, CDL_44_ONLY),),
           ("AL1: a 96-B UNBIND_RX", "AL2: a 96-B BIND_RX", "AL3: the 96-B PROBE_TX",
            "AL4: no 96-B frame was dropped")),
)

# issue #48: the top's SRP service stage for the listener (st_ls_r), the
# class-D bound view, and the SRP listener matcher the near misses grade
TOP = "hdl/top/protocol_processor_top.sv"
SRP_LISTENER = "hdl/srp/KL_srp_listener_fsm.sv"
READY_NEW = "AS4: the matching Talker Advertise yields Listener Ready New"
READY_CLASS_D = "AS4: class-D: Listener READY declared"
READY_TRACED = "AS4: exactly one TK_ATTR_REGISTERED{1}"
NOTK_HELD = "AS5: 11.5 s after the settle"
LV_ON_WIRE = "AS6: the Listener attribute is withdrawn on the wire"
VLAN_RELEASED = "AS6: the sink's VLAN membership is released"

SETTLE_PATH = (
    Mutant("st_ls_settle_as_withdraw", PP_TOP, (
        (TOP, "op:    lstn_act_settle_w ? 3'd2 : 3'd3,",
         "op:    lstn_act_settle_w ? 3'd3 : 3'd3,"),),
        (READY_NEW, READY_CLASS_D, NOTK_HELD)),
    # since issue #188 a settle withdraws the Listener declaration too, so
    # the Lv on the wire no longer tells the two apart: the VLAN release does
    Mutant("st_ls_teardown_as_declare", PP_TOP, (
        (TOP, "op:    lstn_act_settle_w ? 3'd2 : 3'd3,",
         "op:    lstn_act_settle_w ? 3'd2 : 3'd2,"),),
        (VLAN_RELEASED,)),
    Mutant("st_ls_sid_da_swapped", PP_TOP, (
        (TOP, "sid:   lstn_act_settle_sid_w,\n                           da:    lstn_act_settle_da_w,",
         "sid:   64'(lstn_act_settle_da_w),\n                           da:    lstn_act_settle_sid_w[47:0],"),),
        (READY_NEW, READY_CLASS_D, NOTK_HELD)),
    Mutant("st_ls_state_none", PP_TOP, (
        (TOP, "lstn:  2'd2};", "lstn:  2'd0};"),),
        (READY_NEW, READY_CLASS_D, NOTK_HELD)),
    Mutant("st_ls_vid_dropped", PP_TOP, (
        (TOP, "vid:   lstn_act_settle_vlan_w[11:0],", "vid:   12'd0,"),),
        (READY_NEW, READY_CLASS_D, NOTK_HELD)),
    Mutant("st_ls_index_zero", PP_TOP, (
        (TOP, "index: 8'(lstn_act_sink_w),", "index: 8'd0,"),),
        (READY_CLASS_D, READY_TRACED, NOTK_HELD)),
    Mutant("st_ls_teardown_lost", PP_TOP, (
        (TOP, "if (lstn_act_settle_w || lstn_act_teardown_w) begin",
         "if (lstn_act_settle_w) begin"),),
        (LV_ON_WIRE, "AS6: no Listener declared and no match held")),
    Mutant("bound_view_not_latched", PP_TOP, (
        (TOP, "      if (lstn_act_settle_w) begin\n", "      if (1'b0) begin\n"),),
        ("AS2: acmp_bound_o/eid/sid/dmac/vlan_o[1] carry the settled stream",
         "AS5: the bound view still carries the settled stream")),
    Mutant("bound_dmac_from_sid", PP_TOP, (
        (TOP, "bound_dmac_r[lstn_act_sink_w] <= lstn_act_settle_da_w;",
         "bound_dmac_r[lstn_act_sink_w] <= 48'(lstn_act_settle_sid_w);"),),
        ("AS2: acmp_bound_o/eid/sid/dmac/vlan_o[1] carry the settled stream",)),
    Mutant("bound_view_not_cleared", PP_TOP, (
        (TOP, "        bound_sid_r [lstn_act_sink_w] <= 64'd0;\n"
              "        bound_dmac_r[lstn_act_sink_w] <= 48'd0;\n"
              "        bound_vlan_r[lstn_act_sink_w] <= 16'd0;\n", ""),),
        ("AS6: the bound view is cleared with the binding",)),
    Mutant("matcher_da_ignored", PP_TOP, (
        (SRP_LISTENER, "(evt_da_i == da_r[s])", "1'b1"),),
        ("AS3: near misses (DA, VLAN, stream_id) put no Listener declaration",
         "AS3: near misses register nothing", "AS3: no TK_ATTR_REGISTERED{1}")),
    Mutant("matcher_vid_ignored", PP_TOP, (
        (SRP_LISTENER, "&& (evt_vid_i == {4'd0, vid_r[s]});", ";"),),
        ("AS3: near misses (DA, VLAN, stream_id) put no Listener declaration",
         "AS3: near misses register nothing", "AS3: no TK_ATTR_REGISTERED{1}")),
)

# issue #639: the listener records in distributed RAM, read in the cycle after
# the walk's issue state, and the top's eight timer arm-port queues as 4-entry
# rings in distributed RAM. Traffic at the top never holds two arms in a queue
# (section AQ prints the coverage), so section AQ's drive fills the queues from
# the bench. Its AQ3 grades every armq_ arm, and is the only check that reaches
# the full-queue path, where the last four are planted (R462-1's controls).
REC_READ = "  assign rec_rd_w     = acmp_rec_t'(rec_ram_r[sink_r]);\n"
REC_WRITE = "      rec_ram_r[recwr_addr_w] <= recwr_data_w;\n"
RING_WRITE = "    assign wr_ix_w = armq_hd_r[g] + armq_cnt_r[g][1:0];\n"
RING_ADVANCE = "          armq_hd_r[i]         <= armq_hd_r[i] + 2'd1;\n"
RING_READ = "    assign armq_hd_w[g] = mem_r[armq_hd_r[g]];\n"
RING_STORE = "      if (armq_push_ok_w[g]) mem_r[wr_ix_w] <= armq_in_w[g];\n"
PUSH_OK = "      armq_push_ok_w[i] = armq_in_vld_w[i] && (armq_mid_w[i] != 3'd4);\n"
DROP_COUNT = "          if (arm_drop_r != 16'hFFFF) arm_drop_r <= arm_drop_r + 16'd1;\n"
AQ_MODEL = ("AQ2: the arm port and the drop counter equal the eight-queue model",)
AQ_DRIVE = ("AQ3: driven from the bench, the arm port and the drop counter equal",)
AQ_BOTH = AQ_MODEL + AQ_DRIVE
PARKED = "B12: parked sink 7 record untouched"


def rec_write_without(bit: int) -> str:
    """The record write with one record bit never stored."""
    return f"      rec_ram_r[recwr_addr_w] <= recwr_data_w & ~(ACMP_REC_W_C'(1) << {bit});\n"


STORAGE = (
    Mutant("rec_read_sink_zero", ACMP_LISTENER, (
        (LISTENER, REC_READ, "  assign rec_rd_w     = acmp_rec_t'(rec_ram_r[0]);\n"),),
        (PARKED, "F05.3 BIND_SAME x PWA: sm_state")),
    Mutant("rec_read_in_idle", ACMP_LISTENER, (
        (LISTENER, REC_READ,
         "  logic [ACMP_REC_W_C-1:0] rec_idle_r;\n"
         "  always_ff @(posedge clk_i) if (xs_r == X_IDLE) rec_idle_r <= rec_ram_r[sink_r];\n"
         "  assign rec_rd_w     = acmp_rec_t'(rec_idle_r);\n"),),
        ("RV8(setup): state sync",)),
    Mutant("rec_started_unstored", ACMP_LISTENER, (
        (LISTENER, REC_WRITE, rec_write_without(12)),),
        ("S1d: STOP through the request face cleared the bit", "F05.3 GETRX x PWA: flags")),
    Mutant("rec_settled_vlan_unstored", ACMP_LISTENER, (
        (LISTENER, REC_WRITE, rec_write_without(305)),),
        ("F05.3 GETRX x SOK: settled", PARKED)),
    Mutant("rec_sweep_misaddressed", ACMP_LISTENER, (
        (LISTENER, REC_WRITE, "      rec_ram_r[sink_r] <= recwr_data_w;\n"),),
        ("RS GET_RX_STATE after the reset: binding",)),
    Mutant("armq_read_tail", PP_TOP_AQ, (
        (TOP, RING_READ, "    assign armq_hd_w[g] = mem_r[wr_ix_w];\n"),), AQ_BOTH),
    Mutant("armq_head_stuck", PP_TOP_AQ, (
        (TOP, RING_ADVANCE, "          armq_hd_r[i]         <= armq_hd_r[i];\n"),), AQ_BOTH),
    Mutant("armq_ring_of_three", PP_TOP_AQ, (
        (TOP, RING_ADVANCE, "          armq_hd_r[i]         <= (armq_hd_r[i] == 2'd2) ? 2'd0"
                            " : armq_hd_r[i] + 2'd1;\n"),), AQ_BOTH),
    Mutant("armq_write_at_head", PP_TOP_AQ, (
        (TOP, RING_WRITE, "    assign wr_ix_w = armq_hd_r[g];\n"),), AQ_BOTH),
    Mutant("armq_write_at_mid", PP_TOP_AQ, (
        (TOP, RING_WRITE, "    assign wr_ix_w = armq_hd_r[g] + armq_mid_w[g][1:0];\n"),),
        AQ_BOTH),
    Mutant("armq_write_refused", PP_TOP_AQ, (
        (TOP, RING_STORE, "      if (armq_in_vld_w[g]) mem_r[wr_ix_w] <= armq_in_w[g];\n"),),
        AQ_DRIVE),
    Mutant("armq_write_wrap_hi", PP_TOP_AQ, (
        (TOP, RING_WRITE, "    assign wr_ix_w = (armq_cnt_r[g] >= 3'd3) ? armq_hd_r[g] + 2'd2"
                          " : armq_hd_r[g] + armq_cnt_r[g][1:0];\n"),), AQ_DRIVE),
    Mutant("armq_full_pop_refuses", PP_TOP_AQ, (
        (TOP, PUSH_OK,
         "      armq_push_ok_w[i] = armq_in_vld_w[i] && (armq_cnt_r[i] != 3'd4);\n"),),
        AQ_DRIVE),
    Mutant("armq_drop_skip_sat", PP_TOP_AQ, (
        (TOP, DROP_COUNT, "          arm_drop_r <= arm_drop_r + 16'd1;\n"),), AQ_DRIVE),
)

ACMP_TALKER = Suite("tb/acmp_talker", (), ("make", "run"))
PP_TOP_VLAN = Suite("tb/pp_top", ("make", "gsi-build"),
                    ("./obj_dir/Vpp_top_sim", "--gsi-internal-only"))

# Issue 168: each arm restores one clause defect and must fail its own check.
FIELDS = (
    Mutant("field_reset_blocked", ACMP_LISTENER, (
        (LISTENER, "init_cnt_r <= init_cnt_r + (SINK_W_C+1)'(1);",
         "init_cnt_r <= init_cnt_r;"),),
        ("field check reset completes", "field check stimulus completes",
         "field bind responds", "VLAN response settles",
         "VLAN GET_RX_STATE response exists", "duplicate probe exists",
         "LD1 response exists",
         "same-talker rebind changes binding controller while probe remains pending")),
    Mutant("disconnect_not_accepted", ACMP_TALKER, (
        ("hdl/acmp/KL_acmp_talker.sv",
         "assign txn_ready_o = (state_r == S_IDLE) && !gp_valid_r && txn_eligible_w;",
         "assign txn_ready_o = (state_r == S_IDLE) && !gp_valid_r && txn_eligible_w"
         " && (txn_in_w.msg_type != MT_DISC_TX_C);"),),
        ("TD1 consumed",)),
    Mutant("settled_vlan_truncated", PP_TOP_VLAN, (
        (LISTENER, "rec_r.settled_vlan      <= vlan_f_r;",
         "rec_r.settled_vlan      <= {4'd0, vlan_f_r[11:0]};"),),
        ("VLAN168 GET_RX_STATE retains all received bits",)),
    Mutant("unbind_talker_echo", ACMP_LISTENER, (
        (LISTENER, "b_msg_w   = AMSG_UNBIND_RX_RESP_C;",
         "b_msg_w   = AMSG_UNBIND_RX_RESP_C;\n"
         "        b_tkeid_w = tk_eid_f_r; b_tkuid_w = tk_uid_f_r;"),),
        ("LD1 successful unbind has zero talker fields",)),
    Mutant("retry_status_cleared", ACMP_LISTENER, (
        (LISTENER, "if (evt_r != LEV_TMR_RETRY) rec_r.acmpsta <= 5'd0;",
         "rec_r.acmpsta <= 5'd0;"),),
        ("LD2 discovered retry preserves received status",)),
    Mutant("lock_status_13", ACMP_LISTENER, (
        ("hdl/acmp/pp_acmp_pkg.sv", "AST_CONTROLLER_NOT_AUTH_C     = 5'd16",
         "AST_CONTROLLER_NOT_AUTH_C     = 5'd13"),),
        ("LD3 lock refusal status 16 for message 6",
         "LD3 lock refusal status 16 for message 8")),
    Mutant("disconnect_invalid_success", ACMP_TALKER, (
        ("hdl/acmp/KL_acmp_talker.sv",
         "uid_valid_w ? ST_SUCCESS_C : ST_TALKER_UNKNOWN_C", "ST_SUCCESS_C"),),
        ("TD1 invalid disconnect",)),
    Mutant("disconnect_changes_gate", ACMP_TALKER, (
        ("hdl/acmp/KL_acmp_talker.sv", "gate_open_o      = txn_open_w;",
         "gate_open_o      = txn_open_w || (txn_r.msg_type == MT_DISC_TX_C);"),),
        ("TD1 invalid disconnect leaves source state unchanged",)),
    Mutant("settled_vlan_truncated", ACMP_LISTENER, (
        (LISTENER, "rec_r.settled_vlan      <= vlan_f_r;",
         "rec_r.settled_vlan      <= {4'd0, vlan_f_r[11:0]};"),),
        ("VLAN full field in GET_RX_STATE",)),
    Mutant("settlement_vlan_truncated", ACMP_LISTENER, (
        (LISTENER, "assign act_settle_vlan_o = vlan_f_r;",
         "assign act_settle_vlan_o = {4'd0, vlan_f_r[11:0]};"),),
        ("VLAN full field at settle action",)),
    Mutant("gsi_vlan_external", PP_TOP_VLAN, (
        (TOP, "? bound_vlan_r[gsi_sink_w] : 16'd0, gsi_data_i[47:0]}",
         "? gsi_data_i[63:48] : gsi_data_i[63:48], gsi_data_i[47:0]}"),),
        ("VLAN168 GET_STREAM_INFO retains all received bits",
         "VLAN168 unbind clears the full stored VLAN")),
    Mutant("parent_vlan_shifted", PP_TOP_VLAN, (
        (TOP, "bound_vlan_r[v][11:0]", "bound_vlan_r[v][15:4]"),),
        ("VLAN168 parent VID remains the low 12 bits",)),
    Mutant("retry_probe_status_cleared", ACMP_LISTENER, (
        (LISTENER, "if (evt_r != LEV_TMR_DELAY) rec_r.acmpsta <= 5'd0;",
         "rec_r.acmpsta <= 5'd0;"),),
        ("LD2 retry probe preserves received status",)),
    Mutant("retry_probe_status_cleared", PP_TOP_VLAN, (
        (LISTENER, "if (evt_r != LEV_TMR_DELAY) rec_r.acmpsta <= 5'd0;",
         "rec_r.acmpsta <= 5'd0;"),),
        ("GI RETRY-RETAIN solicited: acmpsta sink 0 = 7",)),
    Mutant("lock_gate_bypassed", ACMP_LISTENER, (
        (LISTENER, "res_acts_w[ACT_A1_C] && lock_block_w",
         "res_acts_w[ACT_A1_C] && 1'b0"),),
        ("LD3 lock refusal preserves binding for message 6",
         "LD3 lock refusal preserves binding for message 8")),
    Mutant("stored_vlan_truncated", PP_TOP_VLAN, (
        ("hdl/top/protocol_processor_top.sv",
         "bound_vlan_r[lstn_act_sink_w] <= lstn_act_settle_vlan_w;",
         "bound_vlan_r[lstn_act_sink_w] <= {4'd0, lstn_act_settle_vlan_w[11:0]};"),),
        ("VLAN168 GET_STREAM_INFO retains all received bits",)),
    Mutant("probe_guard_current_controller", ACMP_LISTENER, (
        (LISTENER, "assign probe_match_w = probe_ctlr_match_w",
         "assign probe_match_w = (ctlr_x_r == rec_r.bind_ctlr_eid)"),),
        ("guard accepts controller from sent probe after same-talker rebind",
         "guard rejects controller absent from sent probe after same-talker rebind")),
    Mutant("probe_retry_current_controller", ACMP_LISTENER, (
        (LISTENER, "rec_r.settled_stream_id[ctlr_shift_w +: 8]",
         "rec_r.bind_ctlr_eid[ctlr_shift_w +: 8]"),),
        ("retry preserves original probe after same-talker rebind",)),
)
# issue #188: the SRP listener never abandons the owed Listener Leave
OWED_LEAVE = "    set_app_w  = set_owes_w ? set_lv_w : A_VO_C;\n"
PARK_RETIRE = "            lvp_r[wsrc_r]      <= 1'b0;\n"

REBIND = (
    Mutant("rebind_abandons_leave", PP_TOP, (
        (SRP_LISTENER, OWED_LEAVE,
         "    set_app_w  = (set_owes_w && set_same_w) ? set_lv_w : A_VO_C;\n"),),
        ("AS7: a rebind settled",)),
    Mutant("park_never_retires", PP_TOP, ((SRP_LISTENER, PARK_RETIRE, ""),),
           ("AS7: the MRPDU after it declares the new stream",)),
)

# issue #189: in SETTLED_RSV_OK the TK_ATTR_REGISTERED of the in-place swap
# takes no arc ('-' cell) but REGISTERING_FAILED follows the registered type
SWAP_RF = ("          if ((evt_r == LEV_TK_REG) && (rec_r.sm_state == 3'(LSM_SOK))) begin\n"
           "            rec_r.f_srp_decl[1] <= tkfail_r;\n")
RF_KEPT = SWAP_RF.replace("<= tkfail_r;", "<= rec_r.f_srp_decl[1];")
RF_STICKY = SWAP_RF.replace("<= tkfail_r;", "<= rec_r.f_srp_decl[1] | tkfail_r;")

SWAP = (
    Mutant("rf_swap_unlatched", ACMP_LISTENER, ((LISTENER, SWAP_RF, RF_KEPT),),
           ("B4: no invented arc; the swap to Talker Failed latches RF",
            "B4: after the swap to Talker Failed GET_RX_STATE carries REGISTERING_FAILED")),
    Mutant("rf_swap_unlatched", PP_TOP, ((LISTENER, SWAP_RF, RF_KEPT),),
           ("AS5b: after the in-place swap to Talker Failed",)),
    Mutant("rf_swap_sticky", ACMP_LISTENER, ((LISTENER, SWAP_RF, RF_STICKY),),
           ("B4b: no invented arc; the swap back to Talker Advertise clears RF",
            "B4b: after the swap back GET_RX_STATE clears REGISTERING_FAILED")),
    Mutant("rf_swap_sticky", PP_TOP, ((LISTENER, SWAP_RF, RF_STICKY),),
           ("AS5c: after the swap back to Talker Advertise",
            "AS5d precondition: the start type taken alone")),
)

# issue #189: two swaps coalesced into one router event, whose payload is the
# first type; the top reads the Failed bit from the type registered now
TK_FAILED_LIVE = "        lstn_evt_tk_failed_w = (srp_tk_reg_state_w[k] == 2'd2);\n"
LAST_ADV = "AS5d: after the queue drains GET_RX_STATE follows the last swap"
LAST_FAILED = "AS5e: after the queue drains GET_RX_STATE follows the last swap"

COALESCE = (
    Mutant("tk_failed_first_payload", PP_TOP, (
        (TOP, TK_FAILED_LIVE, "        lstn_evt_tk_failed_w = evr_pay_w[15];\n"),),
        (LAST_ADV, LAST_FAILED)),
    Mutant("tk_failed_or_payload", PP_TOP, (
        (TOP, TK_FAILED_LIVE, TK_FAILED_LIVE.replace(";", " || evr_pay_w[15];")),),
        (LAST_ADV,)),
    Mutant("tk_failed_and_payload", PP_TOP, (
        (TOP, TK_FAILED_LIVE, TK_FAILED_LIVE.replace(";", " && evr_pay_w[15];")),),
        (LAST_FAILED,)),
    # the preconditions that make the two cases coalesce: the router flags the
    # re-fire, and the SRP listener registers each type change
    Mutant("router_coalesce_unflagged", PP_TOP, (
        ("hdl/packet_engine/KL_pp_event_router.sv", "coalesced_r[s] <= 1'b1;",
         "coalesced_r[s] <= 1'b0;"),),
        ("AS5d precondition: the listener took the two strobes",
         "AS5e precondition: the listener took the two strobes")),
    Mutant("tk_type_unlatched", PP_TOP, (
        (SRP_LISTENER, "          rtype_r[s] <= rx_is_failed_w;\n",
         "          rtype_r[s] <= rtype_r[s];\n"),),
        ("AS5d precondition: with the four ordinary TX slots full",
         "AS5e precondition: the start type taken alone")),
)

# issue #190: the top's talker stage (st_tk_r) captures each engine-driven
# DECLARE_TALKER's TSpec MaxFrameSize from its own source's slot of
# cfg_tspec_max_frame_i; section TS gives every slot a different Milan v1.2
# Table 4.4 value (source 0 the 8-channel AAF 217, source 1 the CRF 29)
PP_TOP_TS = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--tspec-only"))
TS_SLOT = "mfs:   cfg_tspec_max_frame_i[32'(tkr_gate_src_w) * 16 +: 16],"
TSPEC = (
    Mutant("tspec_slot_zero", PP_TOP_TS, (
        (TOP, TS_SLOT, "mfs:   cfg_tspec_max_frame_i[15:0],"),),
        ("TS1: source 1 declares its own slot's MaxFrameSize",)),
    Mutant("tspec_slot_last", PP_TOP_TS, (
        (TOP, TS_SLOT, "mfs:   cfg_tspec_max_frame_i[N_STREAM_OUT_P*16-16 +: 16],"),),
        ("TS1: source 0 declares its own slot's MaxFrameSize",)),
    Mutant("tspec_no_allocator", PP_TOP_TS, (
        (TOP, "                                                     : maap_req_ready_i;\n",
         "                                                     : 1'b0;\n"),),
        ("TS0: AECP is released and every source's PROBE_TX_RESPONSE is SUCCESS",)),
)
# issue #202: after a MAAP conflict on a declaring source the next grant moves
# it to a new Stream ID, distinct from the withdrawn one and from every other
# source's current and configured ID, and declares it at once; src_stream_id_o,
# PROBE_TX, GET_TX_STATE and the Talker Advertise carry it. A disable of a
# declared source moves it too, and a PCP change after a withdrawal keeps the
# ID and waits two LeaveAllTime periods (IEEE 802.1Q-2018 35.2.2.8). The
# talker suite grades the rule; the processor-top suite grades the whole path
# through the in-scope MAAP engine and the SRP encoder.
TALKER = "hdl/acmp/KL_acmp_talker.sv"
PP_TOP_MAAP = Suite("tb/pp_top", ("make", "gsi-build"),
                    ("./obj_dir/Vpp_top_sim", "--maap-internal-only"))
MOVE_ARC = ("          // withdraw, then re-allocate at once: no backoff\n"
            "          ev_close_w       = 1'b1;\n"
            "          ev_rec2_w.gstate = GS_NO_DA_C;\n"
            "          ev_initset_w     = 1'b1;\n")
OLD_WAIT = ("          ev_close_w       = 1'b1;\n"
            "          ev_rec2_w.gstate = GS_BACKOFF_C;\n"
            "          ev_to_draw_w     = 1'b1;\n")
OWE = "        ev_sid_owe_w       = sid_used_r[ev_src_r];\n"
OPEN_USED = "        if (ev_open_w) sid_used_r[ev_src_r] <= 1'b1;\n"
PROBE_USED = "      if ((state_r == S_TXN_ACT) && txn_open_w) sid_used_r[tsrc_w] <= 1'b1;\n"
PUBLISH = "         sid_ovr_v_r[g] ? sid_ovr_r[g] : cfg_stream_id_i[g*64 +: 16]};\n"
PUB_UPPER = "        {cfg_stream_id_i[g*64+16 +: 48],\n"
OFF_OWE = "        ev_sid_owe_w = sid_used_r[ev_src_r];\n"
PCP_LAPSED = ("        end else if ((rec_w.gstate == GS_DA_OK_C) && sid_used_r[ev_src_r]) begin\n"
              "          // the ID was declared and withdrawn on a lapsed gate: the next\n"
              "          // declaration waits the same gap, counted from this change, which\n"
              "          // comes after the withdrawal (issue #202, REQ-ACMP-010)\n"
              "          ev_rec2_w.gstate = GS_BACKOFF_C;\n"
              "          ev_we_w          = 1'b1;\n"
              "          ev_to_draw_w     = 1'b1;\n"
              "        end\n")
PCP_HOLD = ("  assign pcp_hold_w = (pe_pcp_r | (srp_pcp_change_i ? cfg_src_en_i : '0))\n"
            "                      & sid_used_r;\n")
CFG_TERM = "          && ((cfg_stream_id_i[j*64 +: 16] == sid_cand_r)\n"
CUR_TERM = "              || (sid_ovr_v_r[j] && (sid_ovr_r[j] == sid_cand_r)))) begin\n"
UPPER = "      if ((cfg_stream_id_i[j*64+16 +: 48] == mv_sid_w[63:16])\n"
STREAM_ID = (
    Mutant("conflict_keeps_old_wait", ACMP_TALKER, ((TALKER, MOVE_ARC, OLD_WAIT),),
           ("E1 no backoff", "S1 ALLOC_DA offered in the conflict's millisecond",
            "S1 conflict to new declaration under 2 s")),
    Mutant("conflict_keeps_old_wait", PP_TOP_MAAP, ((TALKER, MOVE_ARC, OLD_WAIT),),
           ("MP8: conflict to new declaration under 2 s",)),
    Mutant("conflict_reuses_old_id", ACMP_TALKER, (
        (TALKER, OWE, "        ev_sid_owe_w       = 1'b0;\n"),),
           ("E3 re-declare new DA open sid", "S1 the declaration carries a new Stream ID")),
    Mutant("conflict_reuses_old_id", PP_TOP_MAAP, (
        (TALKER, OWE, "        ev_sid_owe_w       = 1'b0;\n"),),
           ("MP8: the Talker Advertise carries the new Stream ID",)),
    Mutant("move_only_when_declaring", ACMP_TALKER, (
        (TALKER, OWE, "        ev_sid_owe_w       = (rec_w.gstate == GS_DECLARING_C);\n"),),
           ("S6 a conflict after a listener withdrawal moves the ID",
            "S6 a conflict after a freshness lapse moves the ID",
            "R6 after backoff src0 status/tuple")),
    Mutant("used_not_marked_by_event", ACMP_TALKER, ((TALKER, OPEN_USED, ""),),
           ("S6 a conflict after a listener withdrawal moves the ID",)),
    Mutant("used_not_marked_by_probe", ACMP_TALKER, ((TALKER, PROBE_USED, ""),),
           ("S1 the declaration carries a new Stream ID",
            "S6 a conflict after a freshness lapse moves the ID")),
    Mutant("used_kept_after_move", ACMP_TALKER, (
        (TALKER, "          sid_used_r[ev_src_r]  <= 1'b0;\n", ""),),
           ("S6 an ID not declared since its move is not moved again",)),
    Mutant("output_ignores_move", ACMP_TALKER, (
        (TALKER, PUBLISH, "         cfg_stream_id_i[g*64 +: 16]};\n"),),
           ("E3 src_stream_id_o follows the move", "S1 src_stream_id_o follows the move")),
    Mutant("output_ignores_move", PP_TOP_MAAP, (
        (TALKER, PUBLISH, "         cfg_stream_id_i[g*64 +: 16]};\n"),),
           ("MP8: src_stream_id_o names the new Stream ID",)),
    Mutant("move_takes_colliding_id", ACMP_TALKER, (
        (TALKER, "        sid_hit_w = 1'b1;\n", "        sid_hit_w = 1'b0;\n"),),
           ("S2 a provisioned collision with the first candidate is skipped",)),
    Mutant("move_ignores_configured_ids", ACMP_TALKER, (
        (TALKER, CFG_TERM,
         "          && ((!sid_ovr_v_r[j] && (cfg_stream_id_i[j*64 +: 16] == sid_cand_r))\n"),),
           ("S3 a move never takes another source's configured ID",)),
    Mutant("move_ignores_moved_ids", ACMP_TALKER, (
        (TALKER, CUR_TERM, "              )) begin\n"),),
           ("S5 after the cursor wraps a move skips",)),
    Mutant("move_compares_unique_id_only", ACMP_TALKER, (
        (TALKER, UPPER, "      if (1'b1\n"),),
           ("S4 an equal unique ID under another MAC is not a collision",)),
    Mutant("move_cursor_stuck", ACMP_TALKER, (
        (TALKER, "          sid_cur_r             <= sid_cand_r + 16'd1;\n",
         "          sid_cur_r             <= sid_cur_r;\n"),),
           ("S5 no move repeats a unique ID inside one lap",)),
    Mutant("move_lost_on_disable", ACMP_TALKER, (
        (TALKER, "        if (ev_sid_owe_w) sid_move_r[ev_src_r] <= 1'b1;\n",
         "        if (ev_code_r == EVC_OFF) sid_ovr_v_r[ev_src_r] <= 1'b0;\n"
         "        if (ev_sid_owe_w) sid_move_r[ev_src_r] <= 1'b1;\n"),),
           ("S1 the move survives a disable",)),
    Mutant("probe_answers_configured_id", ACMP_TALKER, (
        (TALKER, "              resp_stream_id_o = cur_sid_f(tsrc_w);\n",
         "              resp_stream_id_o = src_sid_f(tsrc_w);\n"),),
           ("E5 probe after the grant", "S1 PROBE_TX after the grant")),
    Mutant("tx_state_answers_configured_id", ACMP_TALKER, (
        (TALKER, "            resp_stream_id_o = cur_sid_f(tsrc_w);\n"
                 "            resp_dest_mac_o  = da_ok_w ? rec_w.da : 48'd0;\n",
         "            resp_stream_id_o = src_sid_f(tsrc_w);\n"
         "            resp_dest_mac_o  = da_ok_w ? rec_w.da : 48'd0;\n"),),
           ("E4 new DA answered", "S1 GET_TX_STATE after the grant")),
    Mutant("declare_carries_configured_id", ACMP_TALKER, (
        (TALKER, "      gate_stream_id_o = ev_sid_move_w ? mv_sid_w : cur_sid_f(ev_src_r);\n",
         "      gate_stream_id_o = src_sid_f(ev_src_r);\n"),),
           ("E3 re-declare new DA open sid", "F5 re-declare src3 same DA open sid")),
    # the same-Stream-ID path: a PCP change re-declares without the gap
    Mutant("pcp_skips_the_gap", ACMP_TALKER, (
        (TALKER, "          ev_rec2_w.gstate = GS_BACKOFF_C;  // DA kept: only the class moved\n"
                 "          ev_we_w          = 1'b1;\n"
                 "          ev_to_draw_w     = 1'b1;\n",
         "          ev_rec2_w.gstate = GS_DA_OK_C;  // DA kept: only the class moved\n"
         "          ev_we_w          = 1'b1;\n"),),
           ("F2 leaveall2 src0", "R6 full two-LeaveAll backoff armed")),
    # the same-Stream-ID path after a withdrawal: a PCP change on a source
    # withdrawn on a lapsed gate starts no gap, so the next demand declares
    # the changed FirstValue under the same ID at once
    Mutant("pcp_after_withdrawal_skips_the_gap", ACMP_TALKER, (
        (TALKER, PCP_LAPSED, "        end\n"),),
           ("S7 a PCP change after a listener withdrawal arms T-SRP-LEAVEALL2",
            "S7 two LeaveAllTime periods separate the withdrawal and the same-ID declaration",
            "S8 two LeaveAllTime periods separate the withdrawal and the moved ID's declaration")),
    # ...and a probe served ahead of the pending PCP change declares it
    Mutant("pcp_pending_not_held", ACMP_TALKER, (
        (TALKER, PCP_HOLD, "  assign pcp_hold_w = '0;\n"),),
           ("S9 a probe served with a PCP change pending does not declare the same ID",)),
    # a disable of a declared source books no move: the return grant's DA is
    # declared under the ID that was on the wire with the old one
    Mutant("disable_keeps_declared_id", ACMP_TALKER, ((TALKER, OFF_OWE, ""),),
           ("S10 a declared source returns from a disable under a new Stream ID",
            "J7 grant re-declares open sid")),
    # the re-open after a move is a New! instead of the applicant's Join!: the
    # new ID's first Talker Advertise is a New, which 10 F10.4 says it is not
    Mutant("reopen_sends_new", PP_TOP_MAAP, (
        ("hdl/srp/KL_srp_talker_fsm.sv",
         "        app_ev_w[s]   = gate_open_i ? (rec_valid_r[s] ? AE_JOIN_C : AE_NEW_C)\n",
         "        app_ev_w[s]   = gate_open_i ? AE_NEW_C\n"),),
           ("MP8: the new Stream ID's first Talker Advertise is a JoinMt, not a New",)),
    # every slot publishes slot 0's upper 48 bits
    Mutant("publish_upper_from_slot0", ACMP_TALKER, (
        (TALKER, PUB_UPPER, "        {cfg_stream_id_i[16 +: 48],\n"),),
           ("S4 after reset every slot publishes its own whole configured ID",
            "S4 a slot under other upper 48 bits keeps them when it moves")),
    # the quarantine: no move takes an ID declared within 30 s. The records
    # refuse nothing, so a move takes the ID another source left moments
    # before and declares it with another DA inside the gap
    Mutant("move_ignores_left_ids", ACMP_TALKER, (
        (TALKER, "        sid_qhit_w = 1'b1;\n", "        sid_qhit_w = 1'b0;\n"),),
           ("S5 no ID is declared again with another DA inside two LeaveAllTime periods",
            "S5 after the cursor wraps a move skips every configured ID, the IDs")),
    # ...a record refuses its ID for one epoch only, under 30 s
    Mutant("record_lapses_in_one_epoch", ACMP_TALKER, (
        (TALKER, "      if (sid_rec_v_r[j] && (sid_rec_age_r[j] != 2'd2)\n",
         "      if (sid_rec_v_r[j] && (sid_rec_age_r[j] == 2'd0)\n"),),
           ("S5 after the cursor wraps a move skips every configured ID, the IDs",)),
    # ...a younger move replaces the record, losing the ID left before it
    Mutant("record_overwritten_while_young", ACMP_TALKER, (
        (TALKER, "        if (ev_sid_move_w && !(sid_rec_v_r[ev_src_r]\n"
                 "                               && (sid_rec_age_r[ev_src_r] != 2'd2))) begin\n",
         "        if (ev_sid_move_w) begin\n"),),
           ("S5 after the cursor wraps a move skips every configured ID, the IDs",)),
    # ...a move once the record is one epoch old replaces it, so the ID left
    # before it is free again 10 s after its withdrawal and declared with
    # another DA inside the gap
    Mutant("record_overwritten_after_one_epoch", ACMP_TALKER, (
        (TALKER, "                               && (sid_rec_age_r[ev_src_r] != 2'd2))) begin\n",
         "                               && (sid_rec_age_r[ev_src_r] == 2'd0))) begin\n"),),
           ("S5 a record one epoch old is kept when its source moves again",
            "S5 no ID is declared again with another DA inside two LeaveAllTime periods")),
    # ...the cursor guard is gone, so a fast disable lap re-enters the lower
    # half at once and re-declares an ID withdrawn moments before
    Mutant("cursor_reenters_early", ACMP_TALKER, (
        (TALKER, "    sid_block_w = (sid_cand_r[15] != sid_half_r) && sid_oth_v_r\n",
         "    sid_block_w = 1'b0 && (sid_cand_r[15] != sid_half_r) && sid_oth_v_r\n"),),
           ("S11 a return the cursor guard blocks keeps its ID and waits T-SRP-LEAVEALL2",
            "S11 no ID is declared again with another DA inside two LeaveAllTime periods")),
    # ...the guard lets the cursor back after three epochs, not four
    Mutant("cursor_guard_three_epochs", ACMP_TALKER, (
        (TALKER, "                  && (sid_oth_age_r != 3'd4);\n",
         "                  && (sid_oth_age_r < 3'd3);\n"),),
           ("S11 three quarantine epochs after the cursor left the lower half a return "
            "still keeps its ID",)),
    # ...a blocked search declares the kept ID's new DA at once, no gap
    Mutant("blocked_move_declares_at_once", ACMP_TALKER, (
        (TALKER, "          ev_sid_keep_w      = sid_move_r[ev_src_r] && sid_none_r;\n",
         "          ev_sid_keep_w      = 1'b0;\n"),),
           ("S11 a return the cursor guard blocks keeps its ID and waits T-SRP-LEAVEALL2",
            "S11 no ID is declared again with another DA inside two LeaveAllTime periods")),
    # a re-open under the moved ID while the withdrawn ID's Leave is owed
    # takes the applicant from LA to AA: the Leave never reaches the wire
    Mutant("moved_open_skips_leave", PP_TOP_MAAP, (
        ("hdl/srp/KL_srp_talker_fsm.sv",
         "  assign gpark_w = gate_open_i && rec_valid_r[gate_src_i]\n",
         "  assign gpark_w = 1'b0 && gate_open_i && rec_valid_r[gate_src_i]\n"),),
           ("MP9: a disable and return granted at once still sends the withdrawn ID's Lv",)),
)
MUTANTS = (INERT + LONG_FORM + SETTLE_PATH + STORAGE + FIELDS + REBIND + SWAP + COALESCE
           + TSPEC + STREAM_ID)
TALLY = re.compile(
    r"^((?:ACMP|AQ|GI|TS): \d+ checks, \d+ failures|"
    r"\[build [^\n]+\] \d+ checks, \d+ failures|"
    r"\d+ checks: \d+ PASS, \d+ FAIL)$", re.M)


def plant(tree: Path, edits: tuple[tuple[str, str, str], ...]) -> str:
    """Apply every exact replacement; return a refusal reason, or '' when all applied."""
    for rel, old, new in edits:
        path = tree / rel
        text = path.read_text()
        if text.count(old) != 1:
            return f"{rel}: the planted text occurs {text.count(old)} times"
        path.write_text(text.replace(old, new, 1))
    return ""


def copy_tree(root: Path, tree: Path, suite: Suite) -> None:
    """Copy the sources one suite needs into a private tree, no build products."""
    skip = shutil.ignore_patterns("obj*", "*.hex", "__pycache__")
    for directory in ("hdl", "tb/common", suite.directory):
        shutil.copytree(root / directory, tree / directory, ignore=skip)


def execute(command: tuple[str, ...], cwd: Path, log: Path, verilator: str) -> int:
    """Run one command in the foreground, appending its output to the log."""
    argv = list(command) + (["VERILATOR=" + verilator] if command[0] == "make" else [])
    with log.open("a") as stream:
        return subprocess.run(argv, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                              check=False).returncode


def judge(label: str, suite: Suite, edits: tuple, checks: tuple[str, ...],
          work: tuple[Path, Path, str]) -> dict[str, object]:
    """Build and run one copy (a golden when `edits` is empty) and grade the run."""
    root, output, verilator = work
    with tempfile.TemporaryDirectory(prefix="pp-acmp-mutant-") as temp:
        tree = Path(temp)
        copy_tree(root, tree, suite)
        refusal = plant(tree, edits)
        log = output / f"{label}.log"
        log.write_text("")
        if refusal:
            return {"mutant": label, "verdict": "REFUSED", "reason": refusal}
        cwd = tree / suite.directory
        build_rc = execute(suite.build, cwd, log, verilator) if suite.build else 0
        run_rc = execute(suite.run, cwd, log, verilator) if build_rc == 0 else None
    text = log.read_text(errors="replace")
    fails = [line[len("FAIL: "):] for line in text.splitlines() if line.startswith("FAIL: ")]
    completed = bool(TALLY.search(text))
    missing = [c for c in checks if not any(f.startswith(c) for f in fails)]
    if not edits:
        verdict = "PASS" if (run_rc == 0 and completed and not fails) else "BROKEN"
    else:
        verdict = "KILLED" if (run_rc not in (0, None) and completed and not missing) else "SURVIVED"
    return {"mutant": label, "suite": suite.directory, "build_rc": build_rc, "run_rc": run_rc,
            "completed": completed, "named_checks": list(checks), "missing": missing,
            "failing_checks": fails, "verdict": verdict}


def label_of(mutant: Mutant) -> str:
    """One mutant may be graded by several suites; its record names both."""
    return f"{mutant.name}@{Path(mutant.suite.directory).name}"


def golden_label(suite: Suite) -> str:
    """One golden per suite and run mode; the run's flags name the mode."""
    flags = "".join(arg for arg in suite.run if arg.startswith("--"))
    return f"golden-{Path(suite.directory).name}{flags}"


def main() -> int:
    """Run a golden copy of every suite in use, then every selected mutant."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verilator", default="verilator")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--only", nargs="*", default=None)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    known = sorted({m.name for m in MUTANTS})
    unknown = sorted(set(args.only or ()) - set(known))
    if unknown:
        parser.error("unknown mutant(s): " + " ".join(unknown))
    chosen = [m for m in MUTANTS if not args.only or m.name in args.only]
    work = (root, output, args.verilator)
    suites = {(m.suite.directory, m.suite.run): m.suite for m in chosen}
    records = [judge(golden_label(s), s, (), (), work) for _, s in sorted(suites.items())]
    for record in records:
        print(json.dumps({k: record[k] for k in ("mutant", "verdict")}), flush=True)
    if all(r["verdict"] == "PASS" for r in records):
        with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
            futures = [pool.submit(judge, label_of(m), m.suite, m.edits, m.checks, work)
                       for m in chosen]
            for future in concurrent.futures.as_completed(futures):
                record = future.result()
                records.append(record)
                print(json.dumps({k: record.get(k) for k in ("mutant", "verdict", "missing")}),
                      flush=True)
    (output / "results.json").write_text(json.dumps(records, indent=1) + "\n")
    killed = sum(r["verdict"] == "KILLED" for r in records)
    goldens = [r for r in records if r["mutant"].startswith("golden-")]
    ok = all(r["verdict"] in ("PASS", "KILLED") for r in records) and len(records) > len(goldens)
    print(f"ACMP mutations: {killed} of {len(chosen)} KILLED by their named checks; "
          f"goldens {'PASS' if all(g['verdict'] == 'PASS' for g in goldens) else 'BROKEN'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
