#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant the D3 lane's negative controls in isolated copies; each must fail its named checks.

Each mutant is one or more exact source edits (every old text must occur exactly
once) planted in a private copy of hdl/, tb/common/ and the one suite that grades
it. The copy is built and run in the foreground. A mutant is KILLED only when its
build succeeds, its run completes with the suite's tally, exits non-zero, and every
check named for it fails. A refused edit, a failed build, a crash or a missing tally
never counts as a kill. A golden copy of every suite in use runs first and must pass;
the tree this script lives in is never touched.

The controls are those of the parent D3 contract section 18.1 (processor issue #131)
and the review rounds on it: the aggregate restore deadline (DR3a), the AECP hold
admission, the DR2c backoff derivation and its freedom of dispatch, both directions of
the pass agreement, the roll-back strobe, the pass-1 drain, the watched format judge,
the rate walk, the aggregate's terminals, span and inertness, its pre-proof
variants, the drain of a READ abandoned in the arbiter's issue cycle, the arbiter's
own contract for inputs no in-tree manager presents, and the admission's resident
count; issue #141 adds the clock-source row and restore rule over a ten-source
domain. The suite READMEs carry the matching mutation records.
Issues #59 and #62 add the volatile set's reset arms, graded by section D3V in a run
of its own (`--volatile-only`), whose watch outlasts the controller monitor; issues #52
and #63 the unrestorable records; issues #61 and #83 the name stage (section D3N); and
issue #83 the seeded-random reset-cut campaign (section D3KR, `--cuts-only`).

Usage: python3 tb/pp_top/d3_mutants.py --output DIR [--verilator V] [--jobs N]
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


PP_TOP = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--d3-only"))
PP_TOP_VOLATILE = Suite("tb/pp_top", ("make", "gsi-build"),
                        ("./obj_dir/Vpp_top_sim", "--volatile-only"))
PP_TOP_ADP = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--adp-only"))
PP_TOP_CUTS = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--cuts-only"))
ACMP_NVM = Suite("tb/acmp_nvm", ("make", "ltn_rom.hex"), ("make", "run"))
RX_VALIDATOR = Suite("tb/rx_validator", (), ("make", "run"))

WRITER = "hdl/aecp/KL_aecp_nvm_writer.sv"
ENGINE = "hdl/aecp/KL_aecp_engine.sv"
DYN = "hdl/aecp/KL_aecp_dyn_state.sv"
NOTIFY = "hdl/aecp/KL_aecp_notify.sv"
TOP = "hdl/top/protocol_processor_top.sv"
SHADOW = "hdl/acmp/KL_acmp_nvm_shadow.sv"
ARB = "hdl/packet_engine/KL_pp_nvm_mgr_arb.sv"
VALIDATOR = "hdl/packet_engine/KL_pp_rx_validator.sv"

OWN = "  assign own_o = !done_r || (ss_r == S_ACQ) || latch_w;\n"
CHG = "    if (chg_i && (chg_sel_i <= 13'd5)\n"
CLEAR = "    if (done_ok_w || giveup_w) clr_w[hand_r] = 1'b1;\n"
VERDICT_END = ("      W_NNAME: begin accept_w = sb_rvalid_i && (ridx_w < sb_rdata_i[15:0]);\n"
               "                     refuse_w = sb_rvalid_i && !(ridx_w < sb_rdata_i[15:0]); end\n"
               "      default: ;\n    endcase\n")
BLANK = "                      && (m_err_cause_i == PORT_UNFRAMED_C);\n"
DEVICE = "                      && (m_err_cause_i != PORT_UNFRAMED_C);\n"
GROUPS = ("cfg", "rate", "clks", "fmti", "fmto", "ptof")
AGG_LIVE = "  assign agg_live_w   = (agg_run_r || rs_go_i) && !agg_fired_r && !done_r && !closed_r;\n"
ARMS = ("  assign arm0_w = (iss0_w && !m0_we_i && m0_abort_i)\n"
        "                  || ((own_r == O_M0) && !we_r && m0_abort_i);\n"
        "  assign arm1_w = (iss1_w && !m1_we_i && m1_abort_i)\n"
        "                  || ((own_r == O_M1) && !we_r && m1_abort_i);\n")


def trigger_deleted(sel: int) -> Mutant:
    """One group's trigger deleted: its selector no longer sets a record."""
    new = f"    if (chg_i && (chg_sel_i <= 13'd5) && (chg_sel_i != 13'd{sel})\n"
    return Mutant(f"TRG_{GROUPS[sel]}", PP_TOP, ((WRITER, CHG, new),), (f"D3S1 {GROUPS[sel]}",))


def replay_deleted(sel: int) -> Mutant:
    """One group's replay deleted: its framed, rule-accepted record is refused."""
    new = (VERDICT_END + f"    if (rsel_w == 3'd{sel}) begin\n"
           "      refuse_w = accept_w || refuse_w;\n      accept_w = 1'b0;\n    end\n")
    return Mutant(f"RPL_{GROUPS[sel]}", PP_TOP, ((WRITER, VERDICT_END, new),),
                  (f"D3R1 {GROUPS[sel]}",))


OWNERSHIP = (
    Mutant("hold_released_at_go", PP_TOP, (
        (WRITER, OWN, "  assign own_o = (ws_r == W_WAITGO) || (ss_r == S_ACQ) || latch_w;\n"),),
        ("D3O1: released at",)),
    Mutant("dispatch_not_held", PP_TOP, (
        (ENGINE, "                           && !amap_notify_busy_i && !d3_own_w;\n",
         "                           && !amap_notify_busy_i;\n"),
        (ENGINE, "  assign rsp_open_w     = (a_st_r == A_IDLE) && !rsp_busy_w && !d3_own_w\n",
         "  assign rsp_open_w     = (a_st_r == A_IDLE) && !rsp_busy_w\n"),
        (ENGINE, "          if (txn_valid_i && !rsp_busy_w && !amap_notify_busy_i\n"
                 "              && !d3_own_w) begin\n",
         "          if (txn_valid_i && !rsp_busy_w && !amap_notify_busy_i) begin\n")),
        ("D3O1: without the walk the writer owns every cycle",)),
    Mutant("own_taken_at_the_walk", PP_TOP, (
        (WRITER, OWN + "  assign bus_o = !done_r || latch_w;\n",
         "  assign own_o = ((ws_r != W_WAITGO) && !done_r) || (ss_r == S_ACQ) || latch_w;\n"
         "  assign bus_o = ((ws_r != W_WAITGO) && !done_r) || latch_w;\n"),),
        ("D3R9: the held SET",)),
    Mutant("image_unproven_continues", PP_TOP, (
        (WRITER, "    else if ((ws_r == W_IMGLOC) && sb_rvalid_i && sb_err_i)\n",
         "    else if (1'b0)\n"),),
        ("D3O2: CLOSED at", "D3O3: CLOSED")),
    Mutant("latch_ignores_program", PP_TOP, (
        (WRITER, "          if (!prog_busy_i) begin\n", "          begin\n"),),
        ("D3S9",)),
)

SERVICE = (
    *(trigger_deleted(sel) for sel in range(6)),
    Mutant("taint_ignored", PP_TOP, (
        (WRITER, "  assign done_ok_w   = (ss_r == S_WAIT) && m_done_i && !taint_r;\n",
         "  assign done_ok_w   = (ss_r == S_WAIT) && m_done_i;\n"),),
        ("D3S4 taint", "D3N6 taint")),
    Mutant("clear_wins_same_edge", PP_TOP, (
        (WRITER, "    else        dirty_r <= set_w | (dirty_r & ~clr_w);\n",
         "    else        dirty_r <= (set_w | dirty_r) & ~clr_w;\n"),),
        ("D3S5 same edge",)),
    Mutant("clear_by_group", PP_TOP, (
        (WRITER, CLEAR, "    if (done_ok_w || giveup_w)\n"
                        "      for (int unsigned i = 0; i < N_REC_C; i++)\n"
                        "        if (rec_sel_f(RW_C'(i)) == hsel_w) clr_w[i] = 1'b1;\n"),),
        ("D3S6 group",)),
    Mutant("clear_by_index", PP_TOP, (
        (WRITER, CLEAR, "    if (done_ok_w || giveup_w)\n"
                        "      for (int unsigned i = 0; i < N_REC_C; i++)\n"
                        "        if (16'(32'(i) - sel_off_f(rec_sel_f(RW_C'(i)))) == hidx_w)\n"
                        "          clr_w[i] = 1'b1;\n"),),
        ("D3S6 index",)),
    Mutant("identify_is_a_change", PP_TOP, (
        (WRITER, CHG, "    if (chg_i && (chg_sel_i <= 13'd7)\n"),
        (DYN, "      default:           fmask_w = 64'd0;   // IDENTIFY is volatile\n",
         "      default:           fmask_w = 64'hFF;\n")),
        ("D3S7",)),
    Mutant("unchanged_compare_ignores_validity", PP_TOP, (
        (DYN, "                    && (!vld_w[0] || ((st_wdata_i & fmask_w) != val_w));\n",
         "                    && ((st_wdata_i & fmask_w) != val_w);\n"),),
        ("D3S8 validity",)),
)

RESTORE = (
    *(replay_deleted(sel) for sel in range(6)),
    Mutant("rule_ignored", PP_TOP, (
        (WRITER, VERDICT_END, VERDICT_END + "    if (refuse_w) begin\n      accept_w = 1'b1;\n"
                                            "      refuse_w = 1'b0;\n    end\n"),),
        ("D3R2: COMPLETE",)),
    Mutant("passes_may_disagree", PP_TOP, (
        (WRITER, "  assign pass_disagree_w = pass_r && (rd_whole_w || rd_blank_w)\n",
         "  assign pass_disagree_w = 1'b0 && (rd_whole_w || rd_blank_w)\n"),),
        ("D3R4:", "D3R4b")),
    Mutant("device_error_reads_as_blank", PP_TOP, (
        (WRITER, BLANK, ";\n"), (WRITER, DEVICE, " && 1'b0;\n")),
        ("D3R5 device error on the header", "D3R6: the one saved record")),
    Mutant("unframed_reads_as_device_error", PP_TOP, (
        (WRITER, BLANK, " && 1'b0;\n"), (WRITER, DEVICE, ";\n")),
        ("D3R6: an erased device restores blank",)),
    Mutant("desc_error_is_a_refusal", PP_TOP, (
        (WRITER, "    else if ((ws_r == W_LOC) && sb_rvalid_i && sb_err_i)\n",
         "    else if (1'b0)\n"),),
        ("D3R7",)),
    Mutant("no_restore_watchdog", PP_TOP, (
        (WRITER, "  assign wait_expire_w = stall_w && (wd_r >= 32'(RS_TMO_CYC_P - 1));\n",
         "  assign wait_expire_w = 1'b0 && (wd_r >= 32'(RS_TMO_CYC_P - 1));\n"),),
        ("D3R8: a READ granted", "D3R8b")),
    Mutant("restore_writes_are_changes", PP_TOP, (
        (ENGINE, "  assign d3_chg_w = dyn_chg_w && !d3_bus_w;\n", "  assign d3_chg_w = dyn_chg_w;\n"),),
        ("D3R1: no restore write is a change",)),
    Mutant("enable_not_released_by_restore", PP_TOP, (
        (TOP, "  assign adp_enable_w = entity_enable_i && restore_done_o;\n",
         "  assign adp_enable_w = entity_enable_i;\n"),),
        ("D3R1: the enable requested from reset", "D3C6")),
    Mutant("done_without_d3", PP_TOP, (
        (TOP, "  assign restore_done_o   = nvm_walk_done_w && lsn_released_w && d3_done_w;\n",
         "  assign restore_done_o   = nvm_walk_done_w && lsn_released_w;\n"),),
        ("D3R1: the enable requested from reset", "D3R1: COMPLETE")),
    Mutant("blank_ignores_d3", PP_TOP, (
        (TOP, "                            && nvm_walk_blank_w && d3_blank_w;\n",
         "                            && nvm_walk_blank_w;\n"),),
        ("D3R1: COMPLETE",)),
    Mutant("store_not_cleared", PP_TOP, (
        (DYN, "      rate_v_r   <= '0;\n", ""),
        (DYN, "      for (int unsigned i = 0; i < N_AUDIO_UNIT_P; i++) rate_r[i]   <= 32'd0;\n", "")),
        ("D3R1: every row at its reset value",)),
    Mutant("valid_not_cleared", PP_TOP, ((DYN, "      rate_v_r   <= '0;\n", ""),),
           ("D3R1: every row at its reset value",)),
    Mutant("quarantine_released_by_time", PP_TOP, (
        (ARB, "  assign end_w  = (own_r != O_NONE) && (p_done_i || p_err_i);\n",
         "  logic [15:0] mut_dcnt_r;\n"
         "  always_ff @(posedge clk_i) mut_dcnt_r <= (!rst_n || !drain_r) ? 16'd0 : mut_dcnt_r + 16'd1;\n"
         "  assign end_w  = (own_r != O_NONE)\n"
         "                  && (p_done_i || p_err_i || (drain_r && (mut_dcnt_r == 16'd1000)));\n"),),
        ("D3R5: once the device ends the drained read a later SET persists",)),
)

ROLLBACK = (
    Mutant("no_rollback", PP_TOP, (
        (WRITER, "        rb_min_r <= 1'b0;\n        ws_r     <= W_RB;\n",
         "        done_r   <= 1'b1;\n        ws_r     <= W_DONE;\n"),),
        ("D3R4:",)),
    Mutant("dyn_not_rolled_back", PP_TOP, (
        (ENGINE, "  ) u_dyn (\n      .clk_i           (clk_i),\n      .rst_n           (store_rst_n_w),\n",
         "  ) u_dyn (\n      .clk_i           (clk_i),\n      .rst_n           (rst_n),\n"),),
        ("D3R4:",)),
    Mutant("store_not_rolled_back", PP_TOP, (
        (ENGINE, "  ) u_store (\n      .clk_i             (clk_i),\n"
                 "      .rst_n             (store_rst_n_w),\n",
         "  ) u_store (\n      .clk_i             (clk_i),\n      .rst_n             (rst_n),\n"),),
        ("D3R10 5000", "D3N5")),
    Mutant("rollback_ignores_debt", PP_TOP, (
        (WRITER, "          if (rb_min_r && !desc_debt_i) ws_r <= W_RELOC;\n",
         "          if (rb_min_r) ws_r <= W_RELOC;\n"),),
        ("D3R10 16000",)),
    Mutant("closed_releases_the_entity", PP_TOP, (
        (WRITER, "            closed_r <= 1'b1;             // the image walked again is not proven\n"
                 "            ws_r     <= W_CLOSED;\n",
         "            done_r   <= 1'b1;\n            ws_r     <= W_DONE;\n"),),
        ("D3R12",)),
)

DR2C = (
    Mutant("no_backoff_d3", PP_TOP, (
        (WRITER, "          if (bo_cnt_r <= 32'd1) ss_r <= S_ACQ;  // a fresh latch, attempts kept\n"
                 "          else                   bo_cnt_r <= bo_cnt_r - 32'd1;\n",
         "          ss_r <= S_ACQ;\n"),),
        ("D3S10 timing",)),
    Mutant("fourth_attempt", PP_TOP, (
        (WRITER, "  assign giveup_w    = write_err_w && (attempts_r >= 32'(1 + RETRY_MAX_P));\n",
         "  assign giveup_w    = write_err_w && (attempts_r >= 32'(2 + RETRY_MAX_P));\n"),),
        ("D3S10 count",)),
    Mutant("alarm_forgiven_by_success", PP_TOP, (
        (WRITER, "    else if (giveup_w) alarm_r <= 1'b1;            // sticky until reset\n",
         "    else if (giveup_w) alarm_r <= 1'b1;\n    else if (done_ok_w) alarm_r <= 1'b0;\n"),),
        ("D3S10 revocation",)),
    Mutant("no_backoff_binding", ACMP_NVM, (
        (SHADOW, "          if (fl_bo_r <= 32'd1) hs_r    <= H_FL_RD;\n"
                 "          else                  fl_bo_r <= fl_bo_r - 32'd1;\n",
         "          hs_r <= H_FL_RD;\n"),),
        ("E9 DR2c timing",)),
    Mutant("fourth_attempt_binding", ACMP_NVM, (
        (SHADOW, "  assign fl_giveup_w = fl_err_w && (fl_retry_r >= RETRY_MAX_P);\n",
         "  assign fl_giveup_w = fl_err_w && (fl_retry_r >= RETRY_MAX_P + 1);\n"),
        (SHADOW, "          if (nvm_err_i) begin\n            if (fl_retry_r >= RETRY_MAX_P) begin\n",
         "          if (nvm_err_i) begin\n            if (fl_retry_r >= RETRY_MAX_P + 1) begin\n"),
        (SHADOW, "end else if (nvm_err_i) begin\n            if (fl_retry_r >= RETRY_MAX_P) begin\n",
         "end else if (nvm_err_i) begin\n            if (fl_retry_r >= RETRY_MAX_P + 1) begin\n")),
        ("E8 DR2c count",)),
    Mutant("alarm_forgiven_binding", ACMP_NVM, (
        (SHADOW, "      alarm_r <= 1'b1;                             // sticky until reset\n    end\n",
         "      alarm_r <= 1'b1;\n    end else if (fl_done_w && !fl_taint_r) begin\n"
         "      alarm_r <= 1'b0;\n    end\n"),),
        ("E11 DR2c revocation",)),
    # R390-1 F3: the product derivation of the backoff, and BACKOFF holding dispatch
    Mutant("backoff_derivation", PP_TOP, (
        (TOP, "NVM_RETRY_BACKOFF_CYC_P = (CLK_HZ_P / 32'd2) + (CLK_HZ_P % 32'd2),",
         "NVM_RETRY_BACKOFF_CYC_P = (CLK_HZ_P / 32'd200),"),),
        ("D3S10 timing",)),
    Mutant("backoff_holds_dispatch", PP_TOP, (
        (WRITER, OWN, "  assign own_o = !done_r || (ss_r == S_ACQ) || latch_w || (ss_r == S_BACKOFF);\n"),),
        ("D3S10 backoff",)),
)

# R390-1 F3 (agreement and strobe) and R391-1 F3 (drain, judge, rate walk)
REVIEW = (
    Mutant("disagree_one_direction", PP_TOP, (
        (WRITER, "&& (rd_whole_w != whole0_r[rec_r]);", "&& (!rd_whole_w && whole0_r[rec_r]);"),),
        ("D3R4b",)),
    Mutant("rollback_one_cycle", PP_TOP, (
        (WRITER, "if (rb_min_r && !desc_debt_i) ws_r <= W_RELOC;", "if (!desc_debt_i) ws_r <= W_RELOC;"),),
        ("D3R4 strobe",)),
    Mutant("pass1_read_not_drained", PP_TOP, (
        (WRITER, "assign m_abort_o  = expire_w && (ws_r == W_RD);",
         "assign m_abort_o  = expire_w && (ws_r == W_RD) && !pass_r;"),),
        ("D3R5b: once the device ends the drained pass-1 READ",)),
    Mutant("judge_wait_unwatched", PP_TOP, (
        (WRITER, "W_JUDGE: stall_w = jd_wait_i;", "W_JUDGE: stall_w = 1'b0;"),),
        ("D3R8b",)),
    Mutant("rate_walk_stuck_on_first_lane", PP_TOP, (
        (WRITER, "lane_off_r <= SSR_LIST_OFF_C + (16'(walk_next_w) << 2);",
         "lane_off_r <= SSR_LIST_OFF_C;"),),
        ("D3R3b entry 7",)),
    Mutant("rate_walk_unbounded", PP_TOP, (
        (WRITER, "lane_refuse_w = (walk_next_w == SSR_WALK_MAX_C)\n"
                 "                      || (rcount_r == 16'(walk_next_w));",
         "lane_refuse_w = (rcount_r == 16'(walk_next_w));"),),
        ("D3R3b entry 8",)),
)

# DR3a, ratified: the aggregate restore deadline and the per-wait derivation
AGGREGATE = (
    Mutant("no_aggregate_deadline", PP_TOP, (
        (WRITER, "  assign agg_expire_w = agg_live_w && (agg_r >= 32'(RS_AGG_CYC_P - 1))\n",
         "  assign agg_expire_w = 1'b0 && (agg_r >= 32'(RS_AGG_CYC_P - 1))\n"),),
        ("D3R13 pass 0: DEFAULTS at clock", "D3R13 pass 1")),
    Mutant("aggregate_mirrored", PP_TOP, (
        (TOP, "    parameter int unsigned NVM_RS_AGG_CYC_P    = CLK_HZ_P,\n",
         "    parameter int unsigned NVM_RS_AGG_CYC_P    = 32'd100_000_000,\n"),),
        ("D3R13 pass 0: DEFAULTS at clock",)),
    Mutant("aggregate_from_the_walk", PP_TOP, (
        (WRITER, "(agg_run_r || rs_go_i)", "(agg_run_r || go_i)"),
        (WRITER, "      if (rs_go_i)      agg_run_r   <= 1'b1;\n",
         "      if (go_i)         agg_run_r   <= 1'b1;\n")),
        ("D3R13 pass 0: DEFAULTS at clock",)),
    Mutant("per_wait_floor", PP_TOP, (
        (TOP, "    parameter int unsigned NVM_RS_TMO_CYC_P    = (CLK_HZ_P / 32'd50)\n"
              "                                                 + (((CLK_HZ_P % 32'd50) != 32'd0)"
              " ? 32'd1 : 32'd0),\n",
         "    parameter int unsigned NVM_RS_TMO_CYC_P    = (CLK_HZ_P / 32'd50),\n"),),
        ("D3R8 deadline",)),
    # the clarification (issue #131, 5876655419): an aggregate expiry never
    # closes a provable image
    Mutant("agg_closes_before_proof", PP_TOP, (
        (WRITER, "  assign expire_w      = wait_expire_w || (agg_expire_w && proven_r);\n",
         "  assign expire_w      = wait_expire_w || agg_expire_w;\n"),),
        ("D3R14 image valid: DEFAULTS",)),
    Mutant("binding_walk_ignores_aggregate", PP_TOP, (
        (SHADOW, "  assign rs_tmo_w   = (rs_stall_w && ((rs_wd_r >= 32'(RS_TMO_CYC_P - 1)) || rs_agg_i))\n"
                 "                    || ((hs_r == H_RS_REQ) && rs_agg_i);\n",
         "  assign rs_tmo_w   = rs_stall_w && (rs_wd_r >= 32'(RS_TMO_CYC_P - 1));\n"),),
        ("D3R14 image valid: the binding walk", "D3R14 image refused: the binding walk")),
    Mutant("proof_reads_records_past_bound", PP_TOP, (
        (WRITER, "      if (proof_w && agg_past_w) begin\n",
         "      if (1'b0 && proof_w && agg_past_w) begin\n"),),
        ("D3R14 image valid: DEFAULTS",)),
    # R390-2 F3: the aggregate spans the roll-back (the reviewer's own edit)
    Mutant("agg_not_in_rollback", PP_TOP, (
        (WRITER, AGG_LIVE, AGG_LIVE[:-2] + "\n                        && (ws_r != W_RB) && (ws_r != W_RELOC);\n"),),
        ("D3R15 debt wait: CLOSED", "D3R15 re-LOCATE: CLOSED")),
    # R391-2 F1: inert after the terminal, never fired with an event in hand
    # (the reviewer's own edits)
    Mutant("agg_not_stopped_at_terminal", PP_TOP, (
        (WRITER, AGG_LIVE, "  assign agg_live_w   = (agg_run_r || rs_go_i) && !agg_fired_r;\n"),),
        ("D3R16 COMPLETE", "D3R16 DEFAULTS", "D3R16 CLOSED")),
    Mutant("agg_fires_with_event_in_hand", PP_TOP, (
        (WRITER, "  assign agg_expire_w = agg_live_w && (agg_r >= 32'(RS_AGG_CYC_P - 1))\n"
                 "                        && (stall_w || !wait_w);\n",
         "  assign agg_expire_w = agg_live_w && (agg_r >= 32'(RS_AGG_CYC_P - 1));\n"),),
        ("D3R17: the writer's grant", "D3R17: once the device ends")),
    # R390-3 F1: the head's arbiter, which arms the drain only for a READ it
    # already owns, so an abort in the issue cycle is lost; graded for the
    # binding walk (manager 0) and for a manager 1 driven by acmp_nvm
    *(Mutant(name, suite, (
        (ARB, ARMS,
         "  assign arm0_w = (own_r == O_M0) && !we_r && m0_abort_i;\n"
         "  assign arm1_w = (own_r == O_M1) && !we_r && m1_abort_i;\n"),), checks)
      for name, suite, checks in (
          ("drain_misses_issue_cycle", PP_TOP,
           ("D3R18: the READ abandoned in its issue cycle", "D3R18: once the device ends")),
          ("drain_misses_issue_cycle_m1", ACMP_NVM,
           ("N10 a manager-1 READ abandoned in its issue cycle",)))),
    # R390-4 S1 = R391-4 S1: the arbiter's own contract for inputs neither
    # in-tree manager presents (the reviewers' own edits), graded by acmp_nvm's
    # N11 with manager 1 driven freely
    Mutant("issue_arm_cross_intent", ACMP_NVM, (
        (ARB, ARMS,
         "  assign arm0_w = (iss0_w && !m0_we_i && (m0_abort_i || m1_abort_i))\n"
         "                  || ((own_r == O_M0) && !we_r && m0_abort_i);\n"
         "  assign arm1_w = (iss1_w && !m1_we_i && (m1_abort_i || m0_abort_i))\n"
         "                  || ((own_r == O_M1) && !we_r && m1_abort_i);\n"),),
        ("N11b manager 1's abort in the issue cycle of each of the walk's READs",)),
    Mutant("issue_arm_ignores_we", ACMP_NVM, (
        (ARB, ARMS,
         "  assign arm0_w = (iss0_w && m0_abort_i)\n"
         "                  || ((own_r == O_M0) && !we_r && m0_abort_i);\n"
         "  assign arm1_w = (iss1_w && m1_abort_i)\n"
         "                  || ((own_r == O_M1) && !we_r && m1_abort_i);\n"),),
        ("N11a a manager-1 WRITE presented with its abort",)),
    Mutant("issue_arm_write_too", ACMP_NVM, (
        (ARB, ARMS,
         "  assign arm0_w = (iss0_w && m0_abort_i) || ((own_r == O_M0) && !we_r && m0_abort_i);\n"
         "  assign arm1_w = (iss1_w && m1_abort_i) || ((own_r == O_M1) && !we_r && m1_abort_i);\n"),),
        ("N11a a manager-1 WRITE presented with its abort",)),
    Mutant("issue_arm_stale_we", ACMP_NVM, (
        (ARB, ARMS,
         "  assign arm0_w = (iss0_w && !we_r && m0_abort_i) || ((own_r == O_M0) && !we_r && m0_abort_i);\n"
         "  assign arm1_w = (iss1_w && !we_r && m1_abort_i) || ((own_r == O_M1) && !we_r && m1_abort_i);\n"),),
        ("N11a a manager-1 WRITE presented with its abort",
         "N11c a manager-1 READ abandoned in its issue cycle after a WRITE")),
    # the owned half of the same banner rule: an abort while a WRITE is owned is
    # ignored, which N11a's abort, held to the write's end, grades
    Mutant("owned_arm_write_too", ACMP_NVM, (
        (ARB, ARMS,
         "  assign arm0_w = (iss0_w && !m0_we_i && m0_abort_i)\n"
         "                  || ((own_r == O_M0) && m0_abort_i);\n"
         "  assign arm1_w = (iss1_w && !m1_we_i && m1_abort_i)\n"
         "                  || ((own_r == O_M1) && m1_abort_i);\n"),),
        ("N11a a manager-1 WRITE presented with its abort",)),
    # R390-5 F1 = R391-5 S1: the owned half of the owner-matched drain, which
    # N11d grades with manager 1's abort held while manager 0 owns each READ
    # (the reviewers' own edits: both owned terms, and manager 0's alone)
    *(Mutant(name, ACMP_NVM, ((ARB, ARMS, arms),),
             ("N11d manager 1's abort held while manager 0 owns each of the walk's READs",))
      for name, arms in (
          ("owned_arm_cross_intent",
           "  assign arm0_w = (iss0_w && !m0_we_i && m0_abort_i)\n"
           "                  || ((own_r == O_M0) && !we_r && (m0_abort_i || m1_abort_i));\n"
           "  assign arm1_w = (iss1_w && !m1_we_i && m1_abort_i)\n"
           "                  || ((own_r == O_M1) && !we_r && (m1_abort_i || m0_abort_i));\n"),
          ("cross_own_m1_drains_m0",
           "  assign arm0_w = (iss0_w && !m0_we_i && m0_abort_i)\n"
           "                  || ((own_r == O_M0) && !we_r && (m0_abort_i || m1_abort_i));\n"
           "  assign arm1_w = (iss1_w && !m1_we_i && m1_abort_i)\n"
           "                  || ((own_r == O_M1) && !we_r && m1_abort_i);\n"))),
    # R390-3 F2 and R391-3 F2: the aggregate's pre-proof variants (the
    # reviewers' own edits)
    Mutant("agg_closes_during_proof", PP_TOP, (
        (WRITER, "  assign expire_w      = wait_expire_w || (agg_expire_w && proven_r);\n",
         "  assign expire_w      = wait_expire_w || (agg_expire_w && (proven_r || (ws_r == W_IMGLOC)));\n"),),
        ("D3R19 inside the LOCATE: DEFAULTS",)),
    Mutant("proof_past_needs_fire", PP_TOP, (
        (WRITER, "      if (proof_w && agg_past_w) begin\n",
         "      if (proof_w && agg_fired_r) begin\n"),),
        ("D3R20 W_IMG: DEFAULTS", "D3R20 W_IMGLOC: DEFAULTS")),
    Mutant("proof_default_only_from_img", PP_TOP, (
        (WRITER, "      if (proof_w && agg_past_w) begin\n",
         "      if (proof_w && agg_past_w && (ws_r == W_IMG)) begin\n"),),
        ("D3R19 past the bound: DEFAULTS", "D3R19 inside the LOCATE: DEFAULTS",
         "D3R20 W_IMGLOC: DEFAULTS")),
    Mutant("proof_past_bound_needs_fired", PP_TOP, (
        (WRITER, "  assign agg_past_w   = agg_run_r && (agg_r >= 32'(RS_AGG_CYC_P - 1));\n",
         "  assign agg_past_w   = agg_fired_r;\n"),),
        ("D3R20 W_IMG: DEFAULTS", "D3R20 W_IMGLOC: DEFAULTS")),
    Mutant("agg_o_pulse", PP_TOP, (
        (WRITER, "  assign agg_o        = agg_fired_r;\n", "  assign agg_o        = agg_expire_w;\n"),),
        ("D3R21: the binding walk fails whole",)),
)

# the AECP hold admission (issue #131 ruling 5873580386)
ADMISSION = (
    Mutant("aecp_hold_unbounded", PP_TOP, (
        (TOP, "  assign aecp_rx_hold_w = !d3_done_w && ((aecp_rx_res_r != '0) || aecp_rx_in_w);\n",
         "  assign aecp_rx_hold_w = 1'b0 && ((aecp_rx_res_r != '0) || aecp_rx_in_w);\n"),),
        ("D3O5: in CLOSED each GET_RX_STATE", "D3O6: during the slowed walk")),
    Mutant("held_drop_uncounted", PP_TOP, (
        (VALIDATOR, "      if (held_fail_w && (cnt_held_r != CNT_MAX_C)) cnt_held_r <= cnt_held_r + 16'd1;\n",
         ""),),
        ("D3O5: one AECP command held", "D3O6: at the terminal")),
    # R391-2 S1 (taken): the resident count returned (the reviewer's own edit)
    Mutant("resident_never_returned", PP_TOP, (
        (TOP, "  assign aecp_rx_out_w = {1'b0, aecp_rxs_free_w} + {1'b0, aecp_rxs_free_i};\n",
         "  assign aecp_rx_out_w = 2'd0;\n"),),
        ("D3O7: the returned slot frees the share",)),
    Mutant("validator_admits_held_aecp", RX_VALIDATOR, (
        (VALIDATOR, "  assign held_fail_w = pdu_byte_w && (pidx_w == 11'd0) && aecp_hold_i\n",
         "  assign held_fail_w = 1'b0 && (pidx_w == 11'd0) && aecp_hold_i\n"),),
        ("F28 held AECP", "F28 rx_aecp_held counts both")),
)

# issue #141: the clock-source row and the restore rule over a ten-source domain
# (section D3C); of the D3 checks outside D3C, only the inclusive bound fails one (D3R2)
CLOCK_RULE = "      lane_accept_w = rval_r[15:0] < sb_rdata_i[47:32];\n"
CLOCK_SOURCES = (
    Mutant("clks_row_two_bits", PP_TOP, (
        (DYN, "          13'(SEL_CLKSRC_C): begin clksrc_r[cd_ix_w]  <= st_wdata_i[15:0];\n",
         "          13'(SEL_CLKSRC_C): begin clksrc_r[cd_ix_w]  <= {14'd0, st_wdata_i[1:0]};\n"),),
        ("D3C1 readback", "D3C3 save")),
    Mutant("clks_restore_count_narrowed", PP_TOP, (
        (WRITER, CLOCK_RULE,
         "      lane_accept_w = rval_r[15:0] < {13'd0, sb_rdata_i[34:32]};\n"),),
        ("D3C3 restore",)),
    Mutant("clks_restore_index_narrowed", PP_TOP, (
        (WRITER, CLOCK_RULE,
         "      lane_accept_w = {13'd0, rval_r[2:0]} < sb_rdata_i[47:32];\n"),),
        ("D3C4 at the count", "D3C4 above the count")),
    Mutant("clks_restore_bound_inclusive", PP_TOP, (
        (WRITER, CLOCK_RULE, "      lane_accept_w = rval_r[15:0] <= sb_rdata_i[47:32];\n"),),
        ("D3C4 at the count",)),
)

# issues #59 and #62: the volatile set's reset arms (Milan 5.3.4.1, 5.3.4.2, 5.3.12),
# each deleted from its reset branch, so the state a controller set survives rst_n
VOLATILE = (
    Mutant("registry_survives_reset", PP_TOP_VOLATILE, (
        (NOTIFY, "      valid_r     <= '0;\n", ""),),
        ("D3V5", "D3V8", "D3V9")),
    Mutant("lock_survives_reset", PP_TOP_VOLATILE, (
        (NOTIFY, "      lk_held_r   <= 1'b0;\n", ""),),
        ("D3V4", "D3V7")),
    Mutant("identify_survives_reset", PP_TOP_VOLATILE, (
        (DYN, "      for (int unsigned i = 0; i < N_CONTROL_P;    i++) ident_r[i]  <= 8'd0;\n", ""),),
        ("D3V3",)),
)

# issues #52 and #63: a record that cannot be restored keeps the image's value,
# graded per record by D3C5 (clock source) and AD7 to AD9 (configuration, whose
# section runs alone with --adp-only)
FRAME_CRC = "                      && (rplen_hdr_r == 16'(rplen_w)) && (rcrc_acc_r == rcrc_rx_r);\n"
TORN = "    else if (rd_torn_w)                           abort_cause_w = CAUSE_TORN_C;\n"
BLANK_NEXT = ("              n_blank_r <= n_blank_r + 8'd1;  // erased or unframed: the default\n"
              "              ws_r      <= W_NEXT;\n")


def crc_ignored(group: str, sel: int, suite: Suite, check: str) -> Mutant:
    """One group's crc compare bypassed: a corrupt record of it is framed."""
    new = FRAME_CRC.replace("&& (rcrc_acc_r == rcrc_rx_r);",
                            f"&& ((rcrc_acc_r == rcrc_rx_r) || (rsel_w == 3'd{sel}));")
    return Mutant(f"{group}_crc_ignored", suite, ((WRITER, FRAME_CRC, new),), (check,))


UNRESTORABLE = (
    crc_ignored("cfg", 0, PP_TOP_ADP, "AD8: the first ENTITY_AVAILABLE"),
    crc_ignored("clks", 2, PP_TOP, "D3C5 corrupt"),
    *(Mutant(name, suite, ((WRITER, TORN, TORN.replace("(rd_torn_w)   ", "(1'b0)       ")),),
             checks)
      for name, suite, checks in (("torn_read_not_an_abort", PP_TOP, ("D3C5 torn",)),
                                  ("torn_read_not_an_abort_cfg", PP_TOP_ADP, ("AD9: the torn read",)))),
    # issues #61 and #83: the cut sections' torn records (D3K), refused by the
    # frame's crc alone where the value rule would take the torn bytes
    Mutant("frame_crc_ignored", PP_TOP, (
        (WRITER, FRAME_CRC, FRAME_CRC.replace("(rcrc_acc_r == rcrc_rx_r)", "1'b1")),),
        ("D3K ptof cut at byte 11 of 12: the restore", "D3K name cut at byte 71 of 72: the restore")),
    *(Mutant(name, suite, ((WRITER, BLANK_NEXT, BLANK_NEXT.replace("W_NEXT", "W_APPLY")),), checks)
      for name, suite, checks in (("blank_applies_zero", PP_TOP, ("D3C5 blank",)),
                                  ("blank_applies_zero_cfg", PP_TOP_ADP,
                                   ("AD7: the first ENTITY_AVAILABLE",)))),
)

# issues #61 and #83: the name stage (section D3N; parent D3 section 18.3's controls)
NAME_RULE = ("      W_NNAME: begin accept_w = sb_rvalid_i && (ridx_w < sb_rdata_i[15:0]);\n"
             "                     refuse_w = sb_rvalid_i && !(ridx_w < sb_rdata_i[15:0]); end\n")
NAMES = (
    Mutant("TRG_name", PP_TOP, (
        (WRITER, "    if (nchg_i && (32'(nchg_ord_i) < N_NAME_P)) begin\n",
         "    if (1'b0 && nchg_i && (32'(nchg_ord_i) < N_NAME_P)) begin\n"),),
        ("D3N1 ordinal",)),
    Mutant("RPL_name", PP_TOP, (
        (WRITER, NAME_RULE, "      W_NNAME: begin accept_w = 1'b0;\n"
                            "                     refuse_w = sb_rvalid_i; end\n"),),
        ("D3N3 ordinal",)),
    Mutant("name_rule_ignored", PP_TOP, (
        (WRITER, NAME_RULE, "      W_NNAME: begin accept_w = sb_rvalid_i;\n"
                            "                     refuse_w = 1'b0; end\n"),),
        ("D3N4: COMPLETE",)),
    Mutant("name_empty_refused", PP_TOP, (
        (WRITER, NAME_RULE,
         "      W_NNAME: begin accept_w = sb_rvalid_i && (ridx_w < sb_rdata_i[15:0])\n"
         "                                && (nb_rlane_w != 64'd0);\n"
         "                     refuse_w = sb_rvalid_i && !((ridx_w < sb_rdata_i[15:0])\n"
         "                                && (nb_rlane_w != 64'd0)); end\n"),),
        ("D3N3: COMPLETE", "D3N3 ordinal 2")),
    Mutant("name_record_id_shifted", PP_TOP, (
        (WRITER, "      default: return 8'h80;\n", "      default: return 8'h81;\n"),),
        ("D3N1 ordinal",)),
    Mutant("name_entry_shifted", PP_TOP, (
        (WRITER, "        rs_sb_addr_w  = name_addr_f(ridx_w, nlane_r);\n",
         "        rs_sb_addr_w  = name_addr_f(ridx_w + 16'd1, nlane_r);\n"),),
        ("D3N3 ordinal",)),
    Mutant("name_lanes_partial", PP_TOP, (
        (WRITER, "            if (nlane_r == 3'd7) begin\n              n_app_r <= n_app_r + 8'd1;\n",
         "            if (nlane_r == 3'd6) begin\n              n_app_r <= n_app_r + 8'd1;\n"),),
        ("D3N3 ordinal 1",)),
    Mutant("name_taint_ignored", PP_TOP, (
        (WRITER, "    else if (set_w[hand_r] && (ss_r != S_RUN)\n"
                 "             && (ss_r != S_ACQ))                   taint_r <= 1'b1;\n",
         "    else if (set_w[hand_r] && (ss_r != S_RUN) && (hsel_w != GRP_NAME_C)\n"
         "             && (ss_r != S_ACQ))                   taint_r <= 1'b1;\n"),),
        ("D3N6 taint",)),
    Mutant("name_restore_pulses", PP_TOP, (
        (ENGINE, "  assign name_wr_o = d3_nchg_w;\n", "  assign name_wr_o = store_name_wr_w;\n"),),
        ("D3N3: the restore's name writes",)),
    Mutant("name_restore_is_a_change", PP_TOP, (
        (ENGINE, "  assign d3_nchg_w = store_name_wr_w && !d3_bus_w;\n",
         "  assign d3_nchg_w = store_name_wr_w;\n"),),
        ("D3N3: the restore's name writes",)),
    Mutant("names_before_the_image", PP_TOP, (
        (WRITER, "        W_IMG: begin\n          if (img_valid_i) begin\n            proven_r <= 1'b1;\n"
                 "            ws_r     <= W_RQ;\n          end else begin\n            ws_r <= W_IMGLOC;\n"
                 "          end\n        end\n",
         "        W_IMG: begin\n          proven_r <= 1'b1;\n          ws_r     <= W_RQ;\n        end\n"),),
        ("D3N7",)),
)

# issue #83 and the manager's ruling on it: the standing seeded-random reset-cut
# campaign (section D3KR, run --cuts-only), which cuts the binding record too. A torn
# record whose crc is no longer compared is restored instead of the default
SHADOW_CRC = "                      && (rcrc_acc_r == rcrc_rx_r);\n"
CUTS = (
    Mutant("cut_binding_crc_ignored", PP_TOP_CUTS, (
        (SHADOW, SHADOW_CRC, SHADOW_CRC.replace("(rcrc_acc_r == rcrc_rx_r)", "1'b1")),),
        ("D3KR bind seed",)),
    Mutant("cut_frame_crc_ignored", PP_TOP_CUTS, (
        (WRITER, FRAME_CRC, FRAME_CRC.replace("(rcrc_acc_r == rcrc_rx_r)", "1'b1")),),
        ("D3KR ptof seed", "D3KR name seed")),
)

MUTANTS = (OWNERSHIP + SERVICE + RESTORE + ROLLBACK + DR2C + REVIEW + AGGREGATE + ADMISSION
           + CLOCK_SOURCES + VOLATILE + UNRESTORABLE + NAMES + CUTS)
TALLY = re.compile(r"^((D3V?|D3KR|AD): \d+ checks, \d+ failures|\d+ checks: \d+ PASS, \d+ FAIL)$",
                   re.M)


def golden(suite: Suite) -> str:
    """A golden's name: its suite, and the section a second run of that suite selects."""
    extra = "" if suite.run[-1] in ("--d3-only", "run") else "-" + suite.run[-1].strip("-")
    return "golden-" + Path(suite.directory).name + extra


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


def judge(name: str, suite: Suite, edits: tuple, checks: tuple[str, ...],
          work: tuple[Path, Path, str]) -> dict[str, object]:
    """Build and run one copy (a golden when `edits` is empty) and grade the run."""
    root, output, verilator = work
    with tempfile.TemporaryDirectory(prefix="pp-d3-mutant-") as temp:
        tree = Path(temp)
        copy_tree(root, tree, suite)
        refusal = plant(tree, edits)
        log = output / f"{name}.log"
        log.write_text("")
        if refusal:
            return {"mutant": name, "verdict": "REFUSED", "reason": refusal}
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
    return {"mutant": name, "suite": suite.directory, "build_rc": build_rc, "run_rc": run_rc,
            "completed": completed, "named_checks": list(checks), "missing": missing,
            "failing_checks": fails, "verdict": verdict}


def main() -> int:
    """Run a golden copy of every suite in use, then every selected mutant."""
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verilator", default="verilator")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--only", nargs="*", default=None)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    known = {m.name: m for m in MUTANTS}
    unknown = sorted(set(args.only or ()) - set(known))
    if unknown:
        parser.error("unknown mutant(s): " + " ".join(unknown))
    chosen = [known[n] for n in args.only] if args.only else list(MUTANTS)
    work = (root, output, args.verilator)
    suites = {(m.suite.directory, m.suite.run): m.suite for m in chosen}
    records = [judge(golden(s), s, (), (), work) for _, s in sorted(suites.items())]
    for record in records:
        print(json.dumps({k: record[k] for k in ("mutant", "verdict")}), flush=True)
    if all(r["verdict"] == "PASS" for r in records):
        with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
            futures = [pool.submit(judge, m.name, m.suite, m.edits, m.checks, work) for m in chosen]
            for future in concurrent.futures.as_completed(futures):
                record = future.result()
                records.append(record)
                print(json.dumps({k: record.get(k) for k in ("mutant", "verdict", "missing")}),
                      flush=True)
    (output / "results.json").write_text(json.dumps(records, indent=1) + "\n")
    killed = sum(r["verdict"] == "KILLED" for r in records)
    goldens = [r for r in records if r["mutant"].startswith("golden-")]
    ok = all(r["verdict"] in ("PASS", "KILLED") for r in records) and len(records) > len(goldens)
    print(f"D3 mutations: {killed} of {len(chosen)} KILLED by their named checks; "
          f"goldens {'PASS' if all(g['verdict'] == 'PASS' for g in goldens) else 'BROKEN'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
