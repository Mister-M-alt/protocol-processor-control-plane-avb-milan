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
the identify sequencer in the third build of tb/pp_top (section ID) and the
parameter's default (section ID0), the command-class pushes (NP), the STORM and
RND sections (ST, RN), and the originator's seeded inflight session (tb/originator
section R). The suite READMEs carry the matching mutation records.

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
ORIGIN = Suite("tb/originator", (), ("make", "run"))

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
        ("ST2: descriptor 0005:0", "ST2b: descriptor 0005:0")),
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
        (UCODE, "    u('CHECK_LOCK', ra=15, imm=E_LOCKED1),       # held by another controller?\n",
         nop_for("u('CHECK_LOCK', ra=15, imm=E_LOCKED1)")),),
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

MUTANTS = IDENTIFY + PUSHES + STORM_RND + INFLIGHT
TALLY = re.compile(r"^(\[build \w+, SRP_DOM_DEF_VID_P 0x[0-9a-f]+\] \d+ checks, \d+ failures"
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
