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
through the top's SRP service stage to the SRP listener matcher and back) and #639
(the listener records in distributed RAM, and the top's timer arm-port queues as
rings in distributed RAM). The suite READMEs carry the matching mutation records.

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

SETTLE_PATH = (
    Mutant("st_ls_settle_as_withdraw", PP_TOP, (
        (TOP, "op:    lstn_act_settle_w ? 3'd2 : 3'd3,",
         "op:    lstn_act_settle_w ? 3'd3 : 3'd3,"),),
        (READY_NEW, READY_CLASS_D, NOTK_HELD)),
    Mutant("st_ls_teardown_as_declare", PP_TOP, (
        (TOP, "op:    lstn_act_settle_w ? 3'd2 : 3'd3,",
         "op:    lstn_act_settle_w ? 3'd2 : 3'd2,"),),
        (LV_ON_WIRE,)),
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
MUTANTS = INERT + LONG_FORM + SETTLE_PATH + STORAGE + FIELDS
TALLY = re.compile(
    r"^((?:ACMP|AQ|GI): \d+ checks, \d+ failures|"
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
