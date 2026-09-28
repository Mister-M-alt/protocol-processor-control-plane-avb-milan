#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Own LeaveAll controls and mutants; build products stay in temporary storage."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TOP = "hdl/srp/KL_srp_top.sv"
ENC = "hdl/srp/KL_srp_encoder.sv"
TK = "hdl/srp/KL_srp_talker_fsm.sv"
LS = "hdl/srp/KL_srp_listener_fsm.sv"

# label, suite, group, exact edits, required failing assertion prefix.
MUTANTS = [
    ("bad-listener-length", "srp_top", "phases", [
        ("hdl/srp/KL_srp_decoder.sv", "len_ok_w = (mrp_data_i == SRP_FV_LEN_LISTENER_C);", "len_ok_w = 1'b1;")], "K12:"),
    ("peer-restarts-timer", "srp_top", "peer", [
        (TOP, "if (|dec_la_msrp_w) begin", "if (|dec_la_msrp_w) begin\n        need_draw_r[0] <= 1'b1;")], "M4:"),
    ("repeated-action", "srp_top", "congestion", [
        (ENC, "assign la_tx_o = la_prepare_done_o && ((st_r == E_COLLECT) || la_act_r) && !la_cancel_i;", "assign la_tx_o = (la_prepare_done_o || ((st_r == E_TXREQ) && preparing_r && !cur_app_r)) && ((st_r == E_COLLECT) || la_act_r) && !la_cancel_i;")], "N4:"),
    ("expiry-event", "srp_top", "phases", [
        (TOP, ".la_tx_o        (p_la_msrp_w)", ".la_tx_o        ()"),
        (TOP, "assign join_fsm_w =", "assign p_la_msrp_w = cad_hit_w && (cad_exp_ix_w == CAD_LA_MSRP_C);\n  assign join_fsm_w =")], "K2:"),
    ("before-slot", "srp_top", "edge", [
        (ENC, "assign la_tx_o = la_prepare_done_o", "assign la_tx_o = ((st_r == E_ALLOC) && preparing_r)")], "L1:"),
    ("talker-no-own", "srp_top", "phases", [
        (TK, "assign leaveall_any_w = leaveall_rx_i[SRP_LA_LISTENER_C] || leaveall_own_i;", "assign leaveall_any_w = leaveall_rx_i[SRP_LA_LISTENER_C];")], "K2:"),
    ("listener-no-own", "srp_top", "phases", [
        (LS, "leaveall_any_w[s] = leaveall_own_i", "leaveall_any_w[s] = 1'b0")], "K11:"),
    ("talker-lost-txla", "srp_stream_fsms", "", [
        (TK, "<= laown_pend_r || leaveall_own_i;", "<= laown_pend_r;")], "T QA txla=2:"),
    ("listener-lost-txla", "srp_stream_fsms", "", [
        (LS, "<= laown_pend_r || leaveall_own_i;", "<= laown_pend_r;")], "L QA txla=2:"),
    ("talker-strict-lv", "srp_top", "phases", [
        (TK, "if (reg_r[s] == R_IN_C) begin", "if (reg_r[s] != R_MT_C) begin")], "K2:"),
    ("listener-strict-lv", "srp_top", "phases", [
        (LS, "if (reg_r[s] == R_IN_C) begin", "if (reg_r[s] != R_MT_C) begin")], "K11:"),
    ("listener-sid-ignored", "srp_top", "phases", [
        (TK, "&& (evt_stream_id_i == sid_r[s]);", ";")], "K5:"),
    ("talker-no-renewal", "srp_top", "phases", [
        (TK, "reg_r[s]      <= R_IN_C;", "reg_r[s]      <= (reg_r[s] == R_LV_C) ? R_LV_C : R_IN_C;")], "K7:"),
    ("listener-no-renewal", "srp_top", "phases", [
        (LS, "reg_r[s]   <= R_IN_C;", "reg_r[s]   <= (reg_r[s] == R_LV_C) ? R_LV_C : R_IN_C;")], "K7:"),
    ("talker-no-expiry", "srp_top", "phases", [
        (TK, "assign exp_hit_w = exp_valid_i", "assign exp_hit_w = 1'b0 && exp_valid_i")], "K8:"),
    ("listener-no-expiry", "srp_top", "phases", [
        (LS, "assign exp_hit_w = exp_valid_i", "assign exp_hit_w = 1'b0 && exp_valid_i")], "K8:"),
    ("pending-peer-ignored", "srp_top", "peer", [
        (TOP, "if (|dec_la_msrp_w) begin", "if (1'b0) begin")], "M1:"),
    ("encoder-peer-ignored", "srp_top", "peer", [
        (ENC, "&& !la_cancel_i;", ";"),
        (ENC, "la_prepare_i ? !la_cancel_i :", "la_prepare_i ? 1'b1 :"),
        (ENC, "if (preparing_r && la_cancel_i)", "if (1'b0)")], "M6:"),
    ("mvrp-supersedes-msrp", "srp_top", "peer", [
        (TOP, "if (|dec_la_msrp_w) begin", "if ((|dec_la_msrp_w) || dec_la_mvrp_w) begin")], "M1:"),
    ("reset-retains-intent", "srp_top", "phases", [
        (TOP, "      la_wait_r         <= 1'b0;", "      // pending preparation incorrectly retained at reset")], "K10:"),
    ("full-drain-missing", "srp_top", "congestion", [
        (TOP, "if ((rnd_act_r || la_done_w) && enc_msrp_full_w", "if (1'b0 && (rnd_act_r || la_done_w) && enc_msrp_full_w")], "N6:"),
    ("already-full-missed", "srp_top", "congestion", [
        (TOP, "&& (!hw_full_q_r || la_done_w)", "&& !hw_full_q_r")], "N10:"),
    ("round-completion-lost", "srp_top", "congestion", [
        (TOP, "if (rnd_act_r || la_wait_r) join_msrp_pend_r <= 1'b1;", "if (rnd_act_r || la_wait_r) join_msrp_pend_r <= 1'b1;\n            td_tk_r <= 1'b0; td_ls_r <= 1'b0;")], "N13:"),
    ("own-flags-missing", "srp_top", "congestion", [
        (ENC, "(la_act_r && ((la_types_w", "(1'b0 && la_act_r && ((la_types_w")], "N5:"),
]


# Additional failing arms make every named new integration check observable.
by_name = {m[0]: m for m in MUTANTS}
for label, original, group, expected in [
    ("expiry-congestion", "expiry-event", "congestion", "N1:"),
    ("missing-registrar-edge", "talker-no-own", "edge", "L3:"),
]:
    _, suite, _, edits, _ = by_name[original]
    MUTANTS.append((label, suite, group, edits, expected))
MUTANTS += [
    ("empty-canceled-pdu", "srp_top", "peer", [
        (ENC, "else if (go0_w && ((cnt_msrp_r != '0) || la_act_r))", "else if (go0_w)")], "M11:"),
    ("reserved-slot-not-reused", "srp_top", "peer", [
        (ENC, "|| ((st_r == E_COLLECT) && la_prepare_i);", "|| (1'b0 && (st_r == E_COLLECT) && la_prepare_i);")], "M12:"),
    ("renewal-congestion", "srp_top", "congestion", [
        (TK, "reg_r[s]      <= R_IN_C;", "reg_r[s]      <= (reg_r[s] == R_LV_C) ? R_MT_C : R_IN_C;")], "N7:"),
    ("leaveall-expiry-lost", "srp_top", "peer", [
        (TOP, "assign cad_hit_w       = exp_valid_i &&", "assign cad_hit_w       = exp_valid_i && (cad_exp_ix_w != CAD_LA_MSRP_C) &&")], "M9:"),
    ("expiry-outranks-peer", "srp_top", "peer", [
        (TOP, "if (|dec_la_msrp_w) begin", "if ((|dec_la_msrp_w) && !(cad_hit_w && (cad_exp_ix_w == CAD_LA_MSRP_C))) begin")], "M10:"),
    ("repeated-expiry-queued", "srp_top", "congestion", [
        (TOP, "          la_cancel_r    <= 1'b0;", "          la_cancel_r    <= 1'b0;\n          la_msrp_pend_r <= 1'b0;"),
        (TOP, "        if (!la_cancel_r) la_msrp_pend_r <= 1'b0;", "        // timer intent incorrectly retired before acceptance")], "N12:"),
    ("receive-priority-lost", "srp_top", "edge", [
        (TK, "        if (reg_rx_hit_w[s] && ((evt_mrp_event_i", "        if (leaveall_any_w && (reg_r[s] == R_IN_C)) begin\n          reg_r[s] <= R_LV_C; tpend_r[s] <= T_ARM_C;\n        end else if (reg_rx_hit_w[s] && ((evt_mrp_event_i")], "L2:"),
    ("action-omitted", "srp_top", "congestion", [
        (ENC, "assign la_tx_o = la_prepare_done_o", "assign la_tx_o = 1'b0 && la_prepare_done_o")], "N3:"),
    ("preparation-before-slot", "srp_top", "peer", [
        (ENC, "&& preparing_r && alloc_gnt_i)", "&& preparing_r)")], "M5:"),
    ("table-one-short", "srp_top", "congestion", [
        (ENC, "cnt_msrp_r == CNT_W_C'(DEPTH_P)", "cnt_msrp_r == CNT_W_C'(DEPTH_P - 1)")], "N9:"),
]


def run(tree, suite, group, log):
    with log.open("w") as stream:
        result = subprocess.run(["make", "-C", str(tree / "tb" / suite),
                                 "RUN_ARGS=" + group], stdout=stream,
                                stderr=subprocess.STDOUT, timeout=900)
    return result.returncode, log.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--only", default="")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    selected = [m for m in MUTANTS if not args.only or m[0] in args.only.split(",")]
    original = {path: (ROOT / path).read_text() for m in selected for path, _, _ in m[3]}
    passed = 0
    total = 0
    covered = set()
    with tempfile.TemporaryDirectory(prefix="srp-leaveall-") as tmp:
        tree = Path(tmp)
        shutil.copytree(ROOT / "hdl", tree / "hdl")
        for suite in ("common", "srp_top", "srp_stream_fsms"):
            shutil.copytree(ROOT / "tb" / suite, tree / "tb" / suite,
                            ignore=shutil.ignore_patterns("obj_*", "__pycache__"))
        controls = sorted({(m[1], m[2]) for m in selected})
        for suite, group in controls:
            rc, contents = run(tree, suite, group, args.output / f"control-{suite}-{group}.log")
            ok = rc == 0 and " 0 FAIL" in contents
            total += 1; passed += ok
            print(f"control {suite} {group}: rc={rc} {'PASS' if ok else 'FAIL'}", flush=True)
            if not ok:
                print(contents[-4000:]); return 1
        for label, suite, group, edits, expected in selected:
            for path, source in original.items():
                (tree / path).write_text(source)
            for path, anchor, replacement in edits:
                target = tree / path
                source = target.read_text()
                if source.count(anchor) != 1:
                    raise RuntimeError(f"{label}: anchor count {source.count(anchor)} for {anchor}")
                target.write_text(source.replace(anchor, replacement))
            rc, contents = run(tree, suite, group, args.output / f"{label}.log")
            failures = [line for line in contents.splitlines() if line.startswith("FAIL:")]
            ok = rc != 0 and "checks:" in contents and any(expected in line for line in failures)
            total += 1; passed += ok
            tags = sorted({line.split(":", 2)[1].strip() for line in failures})
            if ok: covered.update(tags)
            print(f"{label}: rc={rc} failures={len(failures)} {'KILLED' if ok else 'SURVIVED'} tags={','.join(tags)}", flush=True)
            if not ok: print(contents[-2500:], flush=True)
        for path, source in original.items():
            if (ROOT / path).read_text() != source:
                raise RuntimeError(f"working source changed: {path}")
        manifest = "\n".join(f"{hashlib.sha256((ROOT / p).read_bytes()).hexdigest()} {p}" for p in sorted(original))
        (args.output / "source-sha256.txt").write_text(manifest + "\n")
    if not args.only:
        expected = {f"{group}{i}" for group, count in [("K",12),("L",4),("M",12),("N",13)]
                    for i in range(1,count+1)}
        missing = expected - covered
        total += 1; passed += not missing
        print(f"assertion coverage: {len(expected-missing)}/{len(expected)}; missing={sorted(missing)}")
    print(f"{total} checks: {passed} PASS, {total-passed} FAIL")
    return int(passed != total)


if __name__ == "__main__":
    raise SystemExit(main())
