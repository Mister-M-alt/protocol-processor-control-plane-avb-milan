#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Plant lane C6's negative controls in isolated copies; each must fail its named checks.

Each mutant is one or more exact source edits (every old text must occur exactly
once) planted in a private copy of hdl/, tb/common/ and the one suite that grades
it. The copy is built and run in the foreground. A mutant is KILLED only when its
build succeeds, its run completes with the suite's tally, exits non-zero, and every
check named for it fails. A refused edit, a failed build, a crash or a missing tally
never counts as a kill. A golden copy of every suite in use runs first and must pass;
the tree this script lives in is never touched.

The controls cover the notification lane (processor issues #54, #58, #80, #86):
the identify sequencer in the third build of tb/pp_top (section ID), its one-tick
timer margins in the second build of tb/aecp_notify (section FT, the full
timebase), the parameter's default (section ID0), the command-class pushes (NP),
the STORM and RND sections (ST, RN), the originator's seeded inflight session
(tb/originator section R), and the registry's identity index and the counter
throttle stamps' valid bit in the first build of tb/aecp_notify (sections IX and TS,
issue #232), and the counter rounds' spacing from the previous round's send (issue
#148: tb/pp_top section CS and tb/aecp_notify section TW), a DEREGISTER drained
between two jobs of a round (issue #158: tb/aecp_notify section DR), the Domain
and link-edge triggers of the GET_AVB_INFO notification (issue #42: tb/pp_top section
DN), and the registry port and AVB_INTERFACE counter rows at two AVB interfaces
(issue #69: tb/aecp_notify's third build, sections PT and CK, and tb/pp_top's seventh,
section IF), and there the registry depth per interface and the availability probes
of rows that share a controller or a CA owner (review R512-1: sections PD and CA, and
IF3 and IF3b's held commands), and the originator's withdraw mask one clock late at
the top (issue #163: tb/pp_top section WD and tb/aecp_notify section CX), and
the simultaneous drain and command cancellation at one interface (issue #167:
tb/aecp_notify section SC). The suite
READMEs carry the matching mutation records.

Usage: python3 tb/pp_top/notify_mutants.py --output DIR [--verilator V] [--jobs N]
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


IDENT = Suite("tb/pp_top", ("make", "identify-build"), ("./obj_idn/Vpp_top_idn",))
IDENT_OFF = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--identify-only"))
NOTIFY = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--notify-only"))
SPACING = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--spacing-only"))
ORIGIN = Suite("tb/originator", (), ("make", "run"))
TIMEBASE = Suite("tb/aecp_notify", (), ("make", "identify"))
INDEX = Suite("tb/aecp_notify", (), ("make", "run"))

NTFY = "hdl/aecp/KL_aecp_notify.sv"
ENGINE = "hdl/aecp/KL_aecp_engine.sv"
UCODE = "hdl/aecp/ucode/gen_ucode.py"
TOP = "hdl/top/protocol_processor_top.sv"
ORIG = "hdl/packet_engine/KL_pp_originator.sv"

FRAME2 = ("              end else if (ix_r == 2'd1) begin\n"
          "                ix_r    <= 2'd2;\n"
          "                i_st_r  <= I_GAP;\n")
T0 = "                t0_r    <= now_ms_i + 32'd1;\n"
WAIT = "            if ((btn_q2_r || prs_r) && !gap_r) begin\n              job_r  <= 1'b1;\n"
LATCH = "            if (btn_q2_r && gap_r) prs_r <= 1'b1;\n"
BURST_LATCH = "        if (btn_q2_r && rel_r && (i_st_r != I_WAIT)) prs_r <= 1'b1;\n"
BURST_DL = "    assign id_arm_deadline_w = armb_r ? now_ms_i + 32'(IDENT_BURST_MS_C + 1)\n"
DEPART = "    assign dep_w   = (done_w || left_r) && !uns_tx_busy_i;\n"
HOLD_GO = "            end else if ((rel_r || fired_r || exp_r_w) && !gap_r) begin\n"
HOLD = ("            if (!btn_q2_r) begin\n"
        "              i_st_r <= I_WAIT;        // !identifyButtonPressed\n")
TAKE = ("                  lockx_eid_r <= rgy_eid_i;\n"
        "                  lockx_v_r   <= 1'b1;\n"
        "                end\n"
        "              end else begin\n")
CANCEL = ("        release_slot_o        <= txs_r[cancel_pend_ix_w];\n"
          "        tmr_arm_valid_o       <= 1'b1;\n")


def nop_for(line: str) -> str:
    """The µcode word replaced by a NOP in place, so no branch target moves."""
    return "    u('NOP'),  # planted: " + line.strip().split("#")[0].strip() + "\n"


IDENTIFY = (
    Mutant("ident_two_frames", IDENT, (
        (NTFY, FRAME2, "              end else if (ix_r == 2'd1) begin\n"
                       "                seq_r   <= seq_r + 16'd1;\n"
                       "                i_st_r  <= I_HOLD;\n"),),
        ("ID1: one press sends three",)),
    Mutant("ident_seq_per_frame", IDENT, (
        (NTFY, T0, T0 + "                seq_r   <= seq_r + 16'd1;\n"),),
        ("ID1b:",)),
    Mutant("ident_no_rearm", IDENT, (
        (NTFY, "                armr_r  <= 1'b1;\n", ""),),
        ("ID2: held 2.5 s",)),
    Mutant("ident_rearm_from_third_frame", IDENT, (
        (NTFY, "                                      : t0_r + 32'(IDENT_REARM_MS_C);\n",
         "                                      : t0_r + 32'(IDENT_REARM_MS_C + 2 * IDENT_BURST_MS_C);\n"),),
        ("ID2: held 2.5 s", "ID2d: burst 2 starts")),
    Mutant("ident_burst_100ms", IDENT, (
        (NTFY, "  localparam int unsigned IDENT_BURST_MS_C = 150;\n",
         "  localparam int unsigned IDENT_BURST_MS_C = 100;\n"),),
        ("ID1c:",)),
    Mutant("ident_t0_at_request", IDENT, (
        (NTFY, T0, ""),
        (NTFY, WAIT, "            if ((btn_q2_r || prs_r) && !gap_r) begin\n"
                     "              t0_r   <= now_ms_i + 32'd1;\n              job_r  <= 1'b1;\n")),
        ("ID2d: burst 3 starts",)),
    # round 2 (R420-1 F1, R421-1 F1): every frame from the previous one's departure
    Mutant("ident_burst_from_t0", IDENT, (
        (NTFY, BURST_DL,
         "    assign id_arm_deadline_w = armb_r ? t0_r + ((ix_r == 2'd1) ? 32'(IDENT_BURST_MS_C)\n"
         "                                                             : 32'(2 * IDENT_BURST_MS_C))\n"),),
        ("ID7i:", "ID5k:")),
    Mutant("ident_departure_is_retirement", IDENT, (
        (NTFY, DEPART, "    assign dep_w   = done_w;\n"),),
        ("ID7d:",)),
    Mutant("ident_departure_unwired", IDENT, (
        (TOP, "      .uns_tx_busy_i         (uns_tx_busy_w),\n",
         "      .uns_tx_busy_i         (1'b0),\n"),),
        ("ID7d:",)),
    Mutant("ident_next_burst_at_once", IDENT, (
        (NTFY, HOLD_GO, "            end else if (rel_r || fired_r || exp_r_w) begin\n"),
        (NTFY, WAIT, "            if (btn_q2_r || prs_r) begin\n              job_r  <= 1'b1;\n")),
        ("ID3f:", "ID7r:")),
    # round 3 (R420-2 F1, R421-2 F1): a press inside the gap after a burst is latched
    Mutant("ident_wait_ignores_gap", IDENT, (
        (NTFY, WAIT, "            if (btn_q2_r || prs_r) begin\n              job_r  <= 1'b1;\n"),),
        ("ID8c:", "ID8h:")),
    Mutant("ident_press_not_latched", IDENT, (
        (NTFY, LATCH, ""),),
        ("ID8: a 30 ms press",)),
    Mutant("ident_burst_press_not_latched", IDENT, (
        (NTFY, BURST_LATCH, ""),),
        ("ID8o:", "ID8s:")),
    Mutant("ident_departure_ignores_ready", IDENT, (
        (TOP, "      else if (arb_tx_valid_w && arb_tx_eof_w && arb_tx_ready_w)\n",
         "      else if (arb_tx_valid_w && arb_tx_eof_w)\n"),),
        ("ID9d:", "ID9h:")),
    # the one-tick margins, graded where 1 ms is 100,000 clocks (tb/aecp_notify FT)
    Mutant("ident_burst_deadline_one_tick_short", TIMEBASE, (
        (NTFY, BURST_DL, "    assign id_arm_deadline_w = armb_r ? now_ms_i + 32'(IDENT_BURST_MS_C)\n"),),
        ("FT2:",)),
    Mutant("ident_t0_same_ms", TIMEBASE, (
        (NTFY, T0, "                t0_r    <= now_ms_i;\n"),),
        ("FT4:",)),
    Mutant("ident_cut_on_release", IDENT, (
        (NTFY, "          I_GAP: begin\n            if (!btn_q2_r) rel_r <= 1'b1;\n",
         "          I_GAP: begin\n            if (!btn_q2_r) i_st_r <= I_WAIT;\n"),),
        ("ID1: one press sends three",)),
    Mutant("ident_release_ignored", IDENT, (
        (NTFY, HOLD, "            if (1'b0) begin\n"
                     "              i_st_r <= I_WAIT;        // !identifyButtonPressed\n"),),
        ("ID1: one press sends three", "ID2e:")),
    Mutant("ident_unicast_da", IDENT, (
        (NTFY, "    assign uns_mac_o        = own_r ? PP_IDENT_MCAST_MAC_C : hold_mac_r;\n",
         "    assign uns_mac_o        = hold_mac_r;\n"),),
        ("ID1: one press sends three",)),
    Mutant("ident_face_taken_mid_job", IDENT, (
        (NTFY, "        if (job_r && !own_r && (n_st_r != N_EMIT_WAIT)) own_r <= 1'b1;\n",
         "        if (job_r && !own_r) own_r <= 1'b1;\n"),),
        ("ID5f:",)),
    Mutant("ident_built_at_default", IDENT_OFF, (
        (TOP, "      .TMR_IDENT_SLOT_P  (TMR_MAP_C.single + 1),\n"
              "      .EN_IDENTIFY_NOTIF_P (EN_IDENTIFY_NOTIF_P)\n",
         "      .TMR_IDENT_SLOT_P  (TMR_MAP_C.single + 1),\n"
         "      .EN_IDENTIFY_NOTIF_P (1'b1)\n"),
        (TOP, "      .EN_IDENTIFY_NOTIF_P (EN_IDENTIFY_NOTIF_P)\n  ) u_aecp (\n",
         "      .EN_IDENTIFY_NOTIF_P (1'b1)\n  ) u_aecp (\n")),
        ("ID0: with the parameter at 0",)),
)

PUSHES = (
    Mutant("enq_dropped_configuration", NOTIFY, (
        (UCODE, "    u('NOTIFY_ENQ', imm=1),                      # SET_CONFIGURATION\n",
         nop_for("u('NOTIFY_ENQ', imm=1)")),),
        ("NP1 SET_CONFIGURATION(1): exactly one",)),
    Mutant("enq_dropped_stream_info", NOTIFY, (
        (UCODE, "    u('NOTIFY_ENQ', imm=3),                      # SET_STREAM_INFO\n",
         nop_for("u('NOTIFY_ENQ', imm=3)")),),
        ("NP3 SET_STREAM_INFO: exactly one",)),
    Mutant("class_4_mapped_to_rate", NOTIFY, (
        (NTFY, "        4'd4: pick_kind_w = PP_UNS_CTRL_C;\n",
         "        4'd4: pick_kind_w = PP_UNS_SRATE_C;\n"),),
        ("NP4 SET_CONTROL(IDENTIFY 255): exactly one",)),
    Mutant("class_9_mapped_to_control", NOTIFY, (
        (NTFY, "        4'd9: pick_kind_w = PP_UNS_STRM_C;\n",
         "        4'd9: pick_kind_w = PP_UNS_CTRL_C;\n"),),
        ("NP6 STOP_STREAMING: exactly one",)),
    Mutant("requester_not_excluded", NOTIFY, (
        (NTFY, "                  em_excl_r   <= cmdq_excl_r[cmdq_rd_r];\n"
               "                  em_excl_v_r <= 1'b1;\n",
         "                  em_excl_r   <= cmdq_excl_r[cmdq_rd_r];\n"
         "                  em_excl_v_r <= 1'b0;\n"),),
        ("NP1 SET_CONFIGURATION(1): the requester receives no",)),
    Mutant("entry_seq_not_advanced", NOTIFY, (
        (NTFY, "            wr_row_r <= {hold_eid_r, hold_mac_r, hold_seq_r + 16'd1};\n",
         "            wr_row_r <= {hold_eid_r, hold_mac_r, hold_seq_r};\n"),),
        ("NP1b SET_CONFIGURATION(0): it is byte-exact",)),
    Mutant("stream_info_get_body", NOTIFY, (
        (ENGINE, "                            uns_upc_w = UPC_SINFOUNS_C; end\n",
         "                            uns_upc_w = UPC_GSTRI_C;   end\n"),),
        ("NP3 SET_STREAM_INFO: it is byte-exact",)),
)

STORM_RND = (
    Mutant("counter_limit_500ms", NOTIFY, (
        (NTFY, "                || ((now_ms_i - ctr_last_r[c]) >= 32'd1000))) begin\n",
         "                || ((now_ms_i - ctr_last_r[c]) >= 32'd500))) begin\n"),),
        # since #148 a round's second runs from its last send, so half a second
        # leaves six rounds in ST2's five seconds, inside its count; ST2b holds it
        ("ST2b: descriptor 0005:0",)),
    Mutant("fan_out_skips_row_0", NOTIFY, (
        (NTFY, "  assign em_skip_w = !valid_r[wk_ix_r]\n",
         "  assign em_skip_w = !valid_r[wk_ix_r] || (wk_ix_r == '0)\n"),),
        ("ST1b:",)),
    Mutant("refresh_resets_seq", NOTIFY, (
        (NTFY, "                         wk_match_r ? wk_match_seq_r : 16'd0};\n",
         "                         16'd0};\n"),),
        ("RN: 720 seeded steps",)),
    Mutant("foreign_unlock_allowed", NOTIFY, (
        (NTFY, "  assign lk_denied_w = lk_held_r && !lk_is_holder_w;\n",
         "  assign lk_denied_w = lk_held_r && !lk_is_holder_w && !rgy_op_i[0];\n"),),
        ("RN: 720 seeded steps",)),
    Mutant("lock_taker_notified", NOTIFY, (
        (NTFY, TAKE, TAKE.replace("lockx_v_r   <= 1'b1;", "lockx_v_r   <= 1'b0;")),),
        ("RN: 720 seeded steps",)),
    Mutant("deregister_keeps_row", NOTIFY, (
        (NTFY, "              valid_r[wk_match_ix_r] <= 1'b0;\n", ""),),
        ("RN: 720 seeded steps",)),
    Mutant("registry_holds_15", NOTIFY, (
        (NTFY, "          if (!valid_r[wk_ix_r] && !wk_free_r) begin\n",
         "          if (!valid_r[wk_ix_r] && !wk_free_r && (wk_ix_r != CIX_W_C'(N_CTRL_P - 1))) begin\n"),),
        ("RN: 720 seeded steps",)),
    Mutant("set_control_ignores_lock", NOTIFY, (
        (UCODE, "    u('CHECK_LOCK', ra=15, imm=SCTRL_EMIT),      # held by another controller?\n",
         nop_for("u('CHECK_LOCK', ra=15, imm=SCTRL_EMIT)")),),
        ("RN: 720 seeded steps",)),
)

INFLIGHT = (
    Mutant("inflight_highest_free_id", ORIGIN, (
        (ORIG, "    for (int i = int'(INFLIGHT_P) - 1; i >= 0; i--) begin\n      if (!valid_r[i]) begin\n",
         "    for (int i = 0; i < int'(INFLIGHT_P); i++) begin\n      if (!valid_r[i]) begin\n"),),
        ("R: a seeded session",)),
    Mutant("inflight_match_ignores_seq", ORIGIN, (
        (ORIG, "      if (valid_r[i] && (key_r[i] == rsp_key_i)\n"
               "                     && (seq_r[i] == rsp_seq_i)) begin\n",
         "      if (valid_r[i] && (key_r[i] == rsp_key_i)) begin\n"),),
        ("R: a seeded session",)),
    Mutant("inflight_cancel_keeps_timer", ORIGIN, (
        (ORIG, CANCEL, CANCEL.replace("tmr_arm_valid_o       <= 1'b1;", "tmr_arm_valid_o       <= 1'b0;")),),
        ("R: a seeded session",)),
    Mutant("inflight_shared_seq", ORIGIN, (
        (ORIG, "        seq_ctr_r[iss_owner_i] <= seq_ctr_r[iss_owner_i] + 16'd1;\n",
         "        for (int k = 0; k < int'(IFL_N_C); k++) seq_ctr_r[k] <= seq_ctr_r[iss_owner_i] + 16'd1;\n"),),
        ("R: a seeded session",)),
)

# the registry's identity index and the counter stamps' valid bit (issue #232),
# graded by tb/aecp_notify sections IX and TS
IX_WRITE = "        if (ix_busy_w && (wr_ix_r == CIX_W_C'(i)))\n"
IX_MATCH = ("                        && ((ix_busy_w && (wr_ix_r == CIX_W_C'(i))) ? ix_own_w\n"
            "                                                                     : ix_hit_w[i]);\n")

IDENTITY_INDEX = (
    Mutant("ix_old_identity_kept", INDEX, (
        (NTFY, IX_WRITE, IX_WRITE.replace("ix_busy_w", "ix_set_r")),),
        ("IX1:",)),
    Mutant("ix_new_identity_unset", INDEX, (
        (NTFY, "      ix_set_r        <= ix_clr_r;\n", "      ix_set_r        <= 1'b0;\n"),),
        ("IX3:",)),
    Mutant("ix_last_chunk_ignored", INDEX, (
        (NTFY, "    assign ix_hit_w[i] = &ch_w;\n",
         "    assign ix_hit_w[i] = &ch_w[N_IXC_C-2:0];\n"),),
        ("IX2:",)),
    Mutant("ix_rewrite_unmatched", INDEX, (
        (NTFY, IX_MATCH, "                        && ix_hit_w[i];\n"),),
        ("IX4:",)),
    # the review faults of PR #153 (R452-1, R453-1), each the reviewer's own edit
    Mutant("override_set_only", INDEX, (
        (NTFY, IX_MATCH, IX_MATCH.replace("(ix_busy_w &&", "(ix_set_r &&")),),
        ("IX6:",)),
    Mutant("own_compare_new_row", INDEX, (
        (NTFY, "  assign ix_own_w    = (ix_wr_row_w[127:16] == {rx_cmd_eid_i, rx_cmd_mac_i});\n",
         "  assign ix_own_w    = (wr_row_r[127:16] == {rx_cmd_eid_i, rx_cmd_mac_i});\n"),),
        ("IX5:",)),
    Mutant("stamp_read_without_valid", INDEX, (
        (NTFY, "            && (!ctr_sent_r[c]\n", "            && (1'b0\n"),),
        ("TS3:",)),
)

# a counter round's one-second limit from the previous round's last send (issue
# #148), graded on the wire by tb/pp_top section CS and directly by tb/aecp_notify TW
STAMP = "          if (em_kind_r == PP_UNS_CTRS_C) ctr_last_r[em_ctr_ix_r] <= now_ms_i;\n"
STORM_EDITS = {m.name: m.edits for m in STORM_RND}

COUNTER_SPACING = (
    Mutant("counter_spacing_from_selection", SPACING, (
        (NTFY, STAMP, ""),),
        ("CS2b:", "CS2c:")),
    Mutant("counter_spacing_from_selection_tw", INDEX, (
        (NTFY, STAMP, ""),),
        ("TW1:", "TW2:")),
    Mutant("counter_stamp_at_send_only", INDEX, (
        (NTFY, STAMP, STAMP.replace("(em_kind_r == PP_UNS_CTRS_C)",
                                    "((em_kind_r == PP_UNS_CTRS_C) && core_done_w)")),),
        ("TW2:",)),
    Mutant("counter_stamp_first_job_only", INDEX, (
        (NTFY, STAMP, STAMP.replace("(em_kind_r == PP_UNS_CTRS_C)",
                                    "((em_kind_r == PP_UNS_CTRS_C) && (em_ix_r == '0))")),),
        ("TW1:",)),
    # CS's phase-0 run and its premise, graded with two of STORM_RND's own edits
    Mutant("counter_limit_500ms_cs", SPACING, STORM_EDITS["counter_limit_500ms"],
           ("CS2a:", "CS2b:", "CS2c:")),
    Mutant("registry_holds_15_cs", SPACING, STORM_EDITS["registry_holds_15"],
           ("CS1:",)),
)

# a DEREGISTER drained between two jobs of a round waits for the round's boundary
# (issue #158), graded by tb/aecp_notify section DR
DEREG_HOLD = "            if (dh_v_r && !em_active_r) begin\n"
ROUND_END = "            if (em_ix_r == CIX_W_C'(N_ROW_C - 1)) em_active_r <= 1'b0;\n"

DEREG_MID_ROUND = (
    Mutant("dereg_mid_round_no_hold", INDEX, (
        (NTFY, DEREG_HOLD, "            if (dh_v_r) begin\n"),),
        ("DR1:", "DR2:", "DR3:")),
    # review R477-1 S2 on PR #159: the stamp's follow stops for the DEREGISTER
    Mutant("dereg_pending_stops_follow", INDEX, (
        (NTFY, STAMP, STAMP.replace("(em_kind_r == PP_UNS_CTRS_C)",
                                    "((em_kind_r == PP_UNS_CTRS_C) && !dh_v_r)")),),
        ("DR3:",)),
    Mutant("dereg_lost_at_round_end", INDEX, (
        (NTFY, ROUND_END, "            if (em_ix_r == CIX_W_C'(N_ROW_C - 1)) begin\n"
                          "              em_active_r <= 1'b0;\n"
                          "              dh_v_r      <= 1'b0;\n"
                          "            end\n"),),
        ("DR1b:", "DR2b:")),
)

# the Domain and link-edge GET_AVB_INFO triggers of ev_avb_i (issue #42), graded
# by tb/pp_top section DN
DOMAIN_NOTIFY_SUITE = Suite("tb/pp_top", ("make", "gsi-build"),
                            ("./obj_dir/Vpp_top_sim", "--domain-notify-only"))
SRP_DOMAIN = "hdl/srp/KL_srp_domain.sv"
AVB_OR = ("      .ev_avb_i              (gm_change_i || srp_evt_domain_change_w\n"
          "                              || (link_up_i != link_q_r) || gsi_avb_chg_i),\n")

DOMAIN_NOTIFY = (
    Mutant("avb_domain_term_dropped", DOMAIN_NOTIFY_SUITE, (
        (TOP, AVB_OR, AVB_OR.replace(" || srp_evt_domain_change_w", "")),),
        ("DN1b:", "DN1c:", "DN2b:", "DN2c:")),
    Mutant("avb_link_term_dropped", DOMAIN_NOTIFY_SUITE, (
        (TOP, AVB_OR, AVB_OR.replace(" || (link_up_i != link_q_r)", "")),),
        ("DN4b:", "DN4c:", "DN4d:", "DN4e:")),
    Mutant("asp_takes_domain", DOMAIN_NOTIFY_SUITE, (
        (TOP, "      .ev_asp_i              (gsi_asp_chg_i),\n",
         "      .ev_asp_i              (gsi_asp_chg_i || srp_evt_domain_change_w),\n"),),
        ("DN1b:", "DN2b:")),
    Mutant("avb_notify_not_interface", DOMAIN_NOTIFY_SUITE, (
        (NTFY, "      pick_dt_w   = 16'h0009;           // AVB_INTERFACE\n",
         "      pick_dt_w   = 16'h0024;           // AVB_INTERFACE\n"),),
        ("DN4c:", "DN4e:", "DN1c:", "DN2c:")),
    Mutant("domain_same_readopted", DOMAIN_NOTIFY_SUITE, (
        (SRP_DOMAIN, "          && ({surf_prio_w, rxdom_vid_i} != {decl_prio_r, decl_vid_r})) begin\n",
         "          ) begin\n"),),
        ("DN3:", "DN3b:")),
    Mutant("adoption_no_strobe", DOMAIN_NOTIFY_SUITE, (
        (SRP_DOMAIN, "        rx_pend_v_r         <= 1'b0;\n        evt_domain_change_o <= 1'b1;\n",
         "        rx_pend_v_r         <= 1'b0;\n"),),
        ("DN1:", "DN2:")),
    Mutant("revert_strobes_at_defaults", DOMAIN_NOTIFY_SUITE, (
        (SRP_DOMAIN, "        if (adopted_r) evt_domain_change_o <= 1'b1;\n",
         "        evt_domain_change_o <= 1'b1;\n"),),
        ("DN4:",)),
    Mutant("registry_never_claims", DOMAIN_NOTIFY_SUITE, (
        (NTFY, "          if (!valid_r[wk_ix_r] && !wk_free_r) begin\n",
         "          if (1'b0) begin\n"),),
        ("DN0:",)),
    Mutant("restore_never_done", DOMAIN_NOTIFY_SUITE, (
        (TOP, "  assign restore_done_o   = nvm_walk_done_w && lsn_released_w && d3_done_w;\n",
         "  assign restore_done_o   = 1'b0;\n"),),
        ("notify bench: blank NVM",)),
)

# the originator's withdraw mask registered at the top (issue #163), graded on the
# wire by tb/pp_top section WD, and the notify cancellation's own clock by
# tb/aecp_notify section CX
WITHDRAW = Suite("tb/pp_top", ("make", "gsi-build"), ("./obj_dir/Vpp_top_sim", "--withdraw-only"))
MASK_READS = ("                                   && org_withdraw_mask_r[laneq_org_r[0]];\n",
              "          && !org_withdraw_mask_r[laneq_org_r[i]]) begin\n",
              "  assign arb_start_abort_w = org_withdraw_mask_r[ser_slot_w]\n")
MASK_DECL = "  logic [7:0]              org_withdraw_mask_r;\n"
MASK_STAGE = ("    if (!rst_n) org_withdraw_mask_r <= '0;\n"
              "    else        org_withdraw_mask_r <= org_withdraw_slot_mask_w;\n")
CA_REQUEST = "  always_comb begin : ca_request\n"

WITHDRAW_STAGE = (
    Mutant("withdraw_unregistered", WITHDRAW, tuple(
        (TOP, old, old.replace("org_withdraw_mask_r", "org_withdraw_slot_mask_w"))
        for old in MASK_READS),
        ("WD1:", "WD2:")),
    Mutant("withdraw_abort_ignored", WITHDRAW, (
        (TOP, "      .start_abort_i(arb_start_abort_w),\n", "      .start_abort_i(1'b0),\n"),),
        ("WD2:", "WD3:")),
    Mutant("withdraw_mask_dropped", WITHDRAW, tuple(
        (TOP, old, re.sub(r"org_withdraw_mask_r\[[^]]*\]\]?", "1'b0", old))
        for old in MASK_READS),
        ("WD4:",)),
    Mutant("withdraw_two_clocks", WITHDRAW, (
        (TOP, MASK_DECL, MASK_DECL + "  logic [7:0]              withdraw_pre_r;\n"),
        (TOP, MASK_STAGE,
         "    if (!rst_n) begin org_withdraw_mask_r <= '0; withdraw_pre_r <= '0; end\n"
         "    else begin withdraw_pre_r <= org_withdraw_slot_mask_w;"
         " org_withdraw_mask_r <= withdraw_pre_r; end\n")),
        ("WD4:",)),
    # #69 moved the one-interface choice to g_ca_own; delay the command
    # there, preserving the immediate TIME_LIMITED drain and the owner tuple.
    Mutant("cancel_one_clock_late", INDEX, (
        (NTFY, CA_REQUEST, "  logic               cx_late_r;\n"
                           "  logic [CIX_W_C-1:0] cx_late_ix_r;\n"
                           "  always_ff @(posedge clk_i) begin : cx_late\n"
                           "    cx_late_r    <= ca_cancel_ok_w;\n"
                           "    cx_late_ix_r <= ca_cancel_ix_w;\n"
                           "  end\n" + CA_REQUEST),
        (NTFY, "    assign cx_ok_w   = ca_cancel_ok_w\n", "    assign cx_ok_w   = cx_late_r\n"),
        (NTFY, "                       ? pd_ix_w : ca_cancel_ix_w;\n",
         "                       ? pd_ix_w : cx_late_ix_r;\n")),
        ("CX1:",)),
)

# the registry port and the AVB_INTERFACE counter rows with two AVB interfaces
# (issue #69), graded by tb/aecp_notify's third build (sections PT and CK) and on
# the wire by tb/pp_top's seventh build (section IF)
INTERFACES = Suite("tb/aecp_notify", (), ("make", "interfaces"))
IF_TOP = Suite("tb/pp_top", ("make", "interfaces-build"), ("./obj_if2/Vpp_top_if2",))
PORT_MATCH = ("                      && (row_mac_w == hold_mac_r)\n"
              "                      && (port_r[wk_ix_r] == hold_port_r);\n")
PORT_WRITE = "        port_r[wk_match_r ? wk_match_ix_r : wk_free_ix_r] <= hold_port_r;\n"
AVB_ROW = "            ctr_dirty_r[N_STREAM_IN_P + N_STREAM_OUT_P + 1 + i] <= 1'b1;\n"
AVB_NAME = "            pick_dt_w = DT_AVB_INTERFACE_C;\n            pick_di_w = 16'(i);\n"
AVB_PICK = "        for (int unsigned i = 1; i < N_IF_P; i++) begin\n          if (pick_any_w\n"
AVB_MAP = ("    end else if ((ev_ctr_type_i == DT_AVB_INTERFACE_C)\n"
           "                 && (ev_ctr_index_i == 16'd0)) begin\n")
RGY_PORT = ("      .rgy_port_i            ((N_AVB_IF_P > 1) ? aecp_cmd_if_r : 2'd0),"
            "  // the command's interface (above)\n")

INTERFACE_ROWS = (
    Mutant("port_not_compared", INTERFACES, (
        (NTFY, PORT_MATCH, "                      && (row_mac_w == hold_mac_r);\n"),),
        ("PT2:",)),
    Mutant("port_not_latched", INTERFACES, (
        (NTFY, "        hold_port_r <= PORT_W_C'(rgy_port_i);\n", "        hold_port_r <= '0;\n"),),
        ("PT2:",)),
    Mutant("port_not_stored", INTERFACES, (
        (NTFY, PORT_WRITE, PORT_WRITE.replace("<= hold_port_r", "<= '0")),),
        ("PT3:",)),
    Mutant("avb_counter_row_dropped", INTERFACES, (
        (NTFY, AVB_ROW, "            ;\n"),),
        ("CK1:",)),
    Mutant("avb_counter_row_collapsed", INTERFACES, (
        (NTFY, AVB_ROW, AVB_ROW.replace(" + 1 + i]", "]")),),
        ("CK1:",)),
    Mutant("avb_counter_named_clock", INTERFACES, (
        (NTFY, AVB_NAME, "            pick_dt_w = DT_CLOCK_DOMAIN_C;\n            pick_di_w = 16'd0;\n"),),
        ("CK1:",)),
    Mutant("avb_counter_any_index", INTERFACES, (
        (NTFY, AVB_MAP, "    end else if (ev_ctr_type_i == DT_AVB_INTERFACE_C) begin\n"),),
        ("CK4:",)),
    Mutant("avb_counter_name_overlaps_clock", INTERFACES, (
        (NTFY, AVB_PICK, AVB_PICK.replace("int unsigned i = 1;", "int unsigned i = 0;")),),
        ("CK5:",)),
    Mutant("rgy_port_tied_zero", IF_TOP, (
        (TOP, RGY_PORT, "      .rgy_port_i            (2'd0),\n"),),
        ("IF3:",)),
)

# the registry depth per interface, the probes of a controller on both interfaces and
# of rows sharing a CA owner, and the registry port's source at the top (review R512-1
# F1 to F3), graded by tb/aecp_notify's third build (sections PD and CA) and tb/pp_top's
# seventh (IF3, IF3b)
CANCEL_PEND = "        cx_pend_r <= cx_work_w;\n"
FAIL_LIVE = "            && ca_probe_r[CIX_W_C'(32'(ca_fail_owner_i) * N_IF_P + p)]\n"
RSP_LIVE = "            && ca_probe_r[CIX_W_C'(32'(ca_rsp_owner_i) * N_IF_P + p)]\n"
TURN = "            && !rx_cmd_hit_w[i] && !ca_hold_w[i]) begin\n"
SETTLE = "        if (cx_ok_w)                  cx_settle_r <= CX_SETTLE_C;\n"
OWN_PORT = "            if (!wk_port_ok_w && !wk_free_r) wk_free_r <= 1'b0;\n"
ROWS = "  localparam int unsigned N_ROW_C = N_CTRL_P * N_IF_P;\n"
TL_TAG = ("                                wk_match_r ? wk_match_ix_r[CIX_W_C-1 -: OIX_W_C]\n"
          "                                           : wk_free_ix_r[CIX_W_C-1 -: OIX_W_C]};\n")
MON_TAG = "                                    mon_draw_ix_r[CIX_W_C-1 -: OIX_W_C]};\n"
EXP_PORT = "                       PX_W_C'(32'(tmr_exp_slot_i) - TMR_REGMON_BASE_P)};\n"
RGY_PORT_SRC = "      .rgy_port_i            ((N_AVB_IF_P > 1) ? aecp_cmd_if_r : 2'd0),"

INTERFACE_DEPTH_PROBES = (
    Mutant("cancel_one_per_command", INTERFACES, (
        (NTFY, CANCEL_PEND, "        cx_pend_r <= '0;\n"),),
        ("CA1:", "CA1b:")),
    Mutant("report_fail_ignores_probe", INTERFACES, (
        (NTFY, FAIL_LIVE, ""),),
        ("CA2:", "CA3:")),
    Mutant("report_rsp_ignores_probe", INTERFACES, (
        (NTFY, RSP_LIVE, ""),),
        ("CA2:",)),
    Mutant("owner_turns_dropped", INTERFACES, (
        (NTFY, TURN, "            && !rx_cmd_hit_w[i]) begin\n"),),
        ("CA3:", "CA4:")),
    Mutant("settle_dropped", INTERFACES, (
        (NTFY, SETTLE, SETTLE.replace("<= CX_SETTLE_C;", "<= 2'd0;")),),
        ("CA4:",)),
    Mutant("depth_shared", INTERFACES, (
        (NTFY, OWN_PORT, "            if (1'b0) wk_free_r <= 1'b0;\n"),),
        ("PD1:",)),
    Mutant("depth_not_keyed", INTERFACES, (
        (NTFY, ROWS, "  localparam int unsigned N_ROW_C = N_CTRL_P;\n"),),
        ("PD1:",)),
    Mutant("registry_tag_port_bits", INTERFACES, (
        (NTFY, TL_TAG, TL_TAG.replace("[CIX_W_C-1 -: OIX_W_C]", "[OIX_W_C-1:0]")),),
        ("PD2:",)),
    Mutant("monitor_tag_port_bits", INTERFACES, (
        (NTFY, MON_TAG, MON_TAG.replace("[CIX_W_C-1 -: OIX_W_C]", "[OIX_W_C-1:0]")),),
        ("PD2:",)),
    Mutant("expiry_port_dropped", INTERFACES, (
        (NTFY, EXP_PORT, "                       PX_W_C'(0)};\n"),),
        ("PD3:",)),
    # the reviewer's probes P1 and P2, each now failing an IF check at the top
    Mutant("rgy_port_from_latest_frame", IF_TOP, (
        (TOP, RGY_PORT_SRC, RGY_PORT_SRC.replace("aecp_cmd_if_r", "hdr_if_r")),),
        ("IF3:", "IF3b:")),
    Mutant("dereg_matches_other_port", IF_TOP, (
        (NTFY, PORT_MATCH, PORT_MATCH.replace("(port_r[wk_ix_r] == hold_port_r)",
                                              "(port_r[wk_ix_r] == (op_dereg_r ? ~hold_port_r"
                                              " : hold_port_r))")),),
        ("IF3b:",)),
)

# At one interface a TIME_LIMITED drain can occupy the output in the cycle
# of another controller's command. Dropping the pending bit recreates #167.
COLLISION = Suite("tb/aecp_notify", (), ("make", "collision"))
SAME_CYCLE_CANCEL = (
    Mutant("cancel_collision_drops_command", COLLISION, (
        (NTFY, "        cx_wait_r <= cx_wait_r | (rx_cmd_hit_w & ca_probe_r);\n",
         "        cx_wait_r <= '0;\n"),),
        ("SC1:",)),
)

MUTANTS = (IDENTIFY + PUSHES + STORM_RND + INFLIGHT + IDENTITY_INDEX + COUNTER_SPACING
           + DEREG_MID_ROUND + DOMAIN_NOTIFY + WITHDRAW_STAGE + INTERFACE_ROWS + INTERFACE_DEPTH_PROBES
           + SAME_CYCLE_CANCEL)
TALLY = re.compile(r"^(\[build \w+, SRP_DOM_DEF_VID_P 0x[0-9a-f]+, DESC_LINE_BYTES_P \d+\]"
                   r" \d+ checks, \d+ failures"
                   r"|\[build \w+\] \d+ checks, \d+ failures"
                   r"|\d+ checks: \d+ PASS, \d+ FAIL)$", re.M)


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


def judge(mutant: Mutant, work: tuple[Path, Path, str]) -> dict[str, object]:
    """Build and run one copy (a golden when it plants nothing) and grade the run."""
    root, output, verilator = work
    with tempfile.TemporaryDirectory(prefix="pp-c6-mutant-") as temp:
        tree = Path(temp)
        copy_tree(root, tree, mutant.suite)
        refusal = plant(tree, mutant.edits)
        log = output / f"{mutant.name}.log"
        log.write_text("")
        if refusal:
            return {"mutant": mutant.name, "verdict": "REFUSED", "reason": refusal}
        cwd = tree / mutant.suite.directory
        build_rc = execute(mutant.suite.build, cwd, log, verilator) if mutant.suite.build else 0
        run_rc = execute(mutant.suite.run, cwd, log, verilator) if build_rc == 0 else None
    text = log.read_text(errors="replace")
    fails = [line[len("FAIL: "):] for line in text.splitlines() if line.startswith("FAIL: ")]
    completed = bool(TALLY.search(text))
    missing = [c for c in mutant.checks if not any(f.startswith(c) for f in fails)]
    if not mutant.edits:
        verdict = "PASS" if (run_rc == 0 and completed and not fails) else "BROKEN"
    else:
        verdict = "KILLED" if (run_rc not in (0, None) and completed and not missing) else "SURVIVED"
    return {"mutant": mutant.name, "suite": " ".join(mutant.suite.run), "build_rc": build_rc,
            "run_rc": run_rc, "completed": completed, "named_checks": list(mutant.checks),
            "missing": missing, "failing_checks": fails, "verdict": verdict}


def goldens(chosen: list[Mutant]) -> list[Mutant]:
    """One unplanted copy of every distinct suite run the chosen mutants use."""
    runs = {m.suite: m.suite for m in chosen}
    return [Mutant("golden-" + Path(s.directory).name + "-" + s.run[-1].strip("./-").replace("/", "_"),
                   s, (), ()) for s in sorted(runs, key=lambda s: (s.directory, s.run))]


def main() -> int:
    """Run a golden copy of every suite run in use, then every selected mutant."""
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
    known = {m.name: m for m in MUTANTS}
    unknown = sorted(set(args.only or ()) - set(known))
    if unknown:
        parser.error("unknown mutant(s): " + " ".join(unknown))
    chosen = [known[n] for n in args.only] if args.only else list(MUTANTS)
    work = (root, output, args.verilator)
    records = [judge(g, work) for g in goldens(chosen)]
    for record in records:
        print(json.dumps({k: record[k] for k in ("mutant", "verdict")}), flush=True)
    if all(r["verdict"] == "PASS" for r in records):
        with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
            futures = [pool.submit(judge, m, work) for m in chosen]
            for future in concurrent.futures.as_completed(futures):
                record = future.result()
                records.append(record)
                print(json.dumps({k: record.get(k) for k in ("mutant", "verdict", "missing")}),
                      flush=True)
    (output / "results.json").write_text(json.dumps(records, indent=1) + "\n")
    killed = sum(r["verdict"] == "KILLED" for r in records)
    golden = [r for r in records if r["mutant"].startswith("golden-")]
    ok = all(r["verdict"] in ("PASS", "KILLED") for r in records) and len(records) > len(golden)
    print(f"C6 notification mutations: {killed} of {len(chosen)} KILLED by their named checks; "
          f"goldens {'PASS' if all(g['verdict'] == 'PASS' for g in golden) else 'BROKEN'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
