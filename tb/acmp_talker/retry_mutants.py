#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Kill issue #128 mutants in scratch trees; never edit the source checkout.

Each case (the baseline, every mutant and the final restored run) builds and runs in
its own scratch copy. `--jobs N` runs up to N of them at once; the baseline runs
first and must pass, and the results are read in the declared order.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common"))
from mutant_pool import add_jobs_argument, in_order  # noqa: E402

RTL = "hdl/acmp/KL_acmp_talker.sv"

MUTATIONS = {
    "no_round": ("retry_tick_w ? cfg_src_en_i : '0", "'0"),
    "early_round": ("DA_RETRY_MS_C = 32'd100", "DA_RETRY_MS_C = 32'd50"),
    "late_round": ("DA_RETRY_MS_C = 32'd100", "DA_RETRY_MS_C = 32'd101"),
    "retry_wrap": ("(now_ms_i - retry_t0_r) >= DA_RETRY_MS_C",
                   "now_ms_i >= (retry_t0_r + DA_RETRY_MS_C)"),
    "no_pacing": ("pe_init_r & cfg_src_en_i & ~retry_wait_r", "pe_init_r & cfg_src_en_i"),
    "fixed_priority": ("disp_src_w  = init_src_w", "disp_src_w  = ffs_f(init_ready_w)"),
    "command_monopoly": ("assign txn_eligible_w = txn_turn_r", "assign txn_eligible_w = 1'b1"),
    "retry_monopoly": ("assign txn_eligible_w = txn_turn_r", "assign txn_eligible_w = 1'b0"),
    "disabled_alloc": (
        ("pe_init_r & cfg_src_en_i & ~retry_wait_r", "pe_init_r & ~retry_wait_r"),
        ("(rec_w.gstate == GS_NO_DA_C) && cfg_src_en_i[ev_src_r]",
         "(rec_w.gstate == GS_NO_DA_C)")),
    "no_accept_bound": ("maap_req_ready_i || maap_tmo_w", "maap_req_ready_i"),
    "short_accept_bound": ("MAAP_ACCEPT_CYC_P = 1024", "MAAP_ACCEPT_CYC_P = 1023"),
    "refusal_is_grant": ("maap_rsp_live_w && !maap_rel_r && maap_rsp_ok_i",
                         "maap_rsp_live_w && !maap_rel_r"),
    "obsolete_grant": ("if (maap_kill_r || maap_kill_w)", "if (1'b0)"),
    "obsolete_release_lost": ("ev_relset_w = 1'b1;", "ev_relset_w = 1'b0;"),
    "no_response_bound": ("maap_busy_r && !maap_rsp_live_w", "1'b0 && !maap_rsp_live_w"),
    "response_wrap": ("((now_ms_i - maap_t0_r) >= 32'(MAAP_RSP_MS_P))",
                      "(now_ms_i >= (maap_t0_r + 32'(MAAP_RSP_MS_P)))"),
    "no_stale_swallow": ("maap_rsp_valid_i && (maap_stale_r != '0)", "1'b0"),
    "no_stale_capacity": ("assign maap_stale_full_w = &maap_stale_r",
                          "assign maap_stale_full_w = 1'b0"),
    "no_stale_drain": ("maap_stale_r - MAAP_STALE_W_C'(1)", "maap_stale_r"),
    "backoff_bypass": ("(rec_w.gstate == GS_NO_DA_C) && cfg_src_en_i[ev_src_r]",
                       "!rec_w.da_valid && cfg_src_en_i[ev_src_r]"),
    "reallocate_owned": ("(rec_w.gstate == GS_NO_DA_C) && cfg_src_en_i[ev_src_r]",
                         "cfg_src_en_i[ev_src_r]"),
    "half_backoff": ("{15'd0, prng_draw_ms_i, 1'b0}", "{16'd0, prng_draw_ms_i}"),
    "fresh_forever": ("r.pinged && ((now_ms_i - r.ping_ts) < 32'(DAFRESH_MS_P))", "r.pinged"),
    "declare_without_demand": ("(fresh_f(ev_rec2_w) || lsn_reg_f(ev_src_r))", "1'b1"),
    "source_alias": ("gp_src_r   <= maap_src_r", "gp_src_r   <= '0"),
    "gate_without_ownership": ("(rec_wdata_s.gstate == GS_DECLARING_C)", "1'b1"),
    "no_requests": ("assign maap_req_valid_o   = (state_r == S_EV_MAAP)",
                    "assign maap_req_valid_o   = 1'b0"),
    "no_command_ready": ("assign txn_ready_o = (state_r == S_IDLE) && !gp_valid_r && txn_eligible_w",
                         "assign txn_ready_o = 1'b0"),
    "busy_tracker_blocks_commands": (
        ("assign txn_eligible_w = txn_turn_r", "assign txn_eligible_w = !maap_busy_r && (txn_turn_r"),
        ("|| (maap_avail_w && |init_ready_w));", "|| (maap_avail_w && |init_ready_w)));")),

    # Additional lifecycle, demand-path and exact-edge regressions.
    'no_conflict_wait_clear': (
        ('if (!cfg_src_en_i[i] || set_conflict_w[i])', 'if (!cfg_src_en_i[i])'),
    ),
    'wait_on_release': (
        ('ev_to_maap_w && !ev_maap_rel_w)\n        retry_wait_r', 'ev_to_maap_w)\n        retry_wait_r'),
    ),
    'tick_no_rearm': (
        ('      if (retry_tick_w) begin\n        retry_t0_r   <= now_ms_i;\n', '      if (retry_tick_w) begin\n'),
    ),
    'no_rotate_advance': (
        (
            "          retry_next_r <= (disp_src_w == SRC_W_C'(N_STREAM_OUT_P - 1))\n"
            "                          ? '0 : disp_src_w + SRC_W_C'(1);",
            '          retry_next_r <= retry_next_r;',
        ),
    ),
    'no_sticky_gp_window': (
        ('(maap_busy_r || gp_valid_r) && maap_kill_w', 'maap_busy_r && maap_kill_w'),
    ),
    'grant_kill_reg_only': (
        ('if (maap_kill_r || maap_kill_w)', 'if (maap_kill_r)'),
    ),
    'accept_kill_zero': (
        (
            '        maap_kill_r <= !cfg_src_en_i[mreq_src_r] || pe_off_r[mreq_src_r]\n'
            '                       || pe_conflict_r[mreq_src_r]\n'
            '                       || (maap_conflict_valid_i\n'
            '                           && maap_conflict_src_i == mreq_src_r);',
            "        maap_kill_r <= 1'b0;",
        ),
    ),
    'kill_no_pending_conflict': (
        (
            '  assign maap_kill_w = !cfg_src_en_i[maap_src_r] || pe_off_r[maap_src_r]\n'
            '                       || pe_conflict_r[maap_src_r]\n',
            '  assign maap_kill_w = !cfg_src_en_i[maap_src_r] || pe_off_r[maap_src_r]\n',
        ),
    ),
    'kill_no_live_conflict': (
        (
            '                       || pe_conflict_r[maap_src_r]\n'
            '                       || (maap_conflict_valid_i\n'
            '                           && maap_conflict_src_i == maap_src_r);',
            '                       || pe_conflict_r[maap_src_r];',
        ),
    ),
    'kill_no_disable': (
        (
            '  assign maap_kill_w = !cfg_src_en_i[maap_src_r] || pe_off_r[maap_src_r]',
            '  assign maap_kill_w = pe_off_r[maap_src_r]',
        ),
    ),
    'init_elig_no_avail': (
        ('|| (maap_avail_w && |init_ready_w));', '|| |init_ready_w);'),
    ),
    'no_turn_restore': (
        ("        txn_turn_r <= 1'b1;\n        if (disp_code_w == EVC_INIT)", '        if (disp_code_w == EVC_INIT)'),
    ),
    'elig_drop_rel': (
        ('|| |pe_tmr_r || |pe_lsn_r || |pe_rel_r', '|| |pe_tmr_r || |pe_lsn_r'),
    ),
    'elig_drop_off': (
        ('!(|pe_off_r || |pe_conflict_r', '!(|pe_conflict_r'),
    ),
    'ready_ignores_turn': (
        (
            'assign txn_ready_o = (state_r == S_IDLE) && !gp_valid_r && txn_eligible_w;',
            'assign txn_ready_o = (state_r == S_IDLE) && !gp_valid_r;',
        ),
    ),
    'init_ignores_off_conflict': (
        (
            '(rec_w.gstate == GS_NO_DA_C) && cfg_src_en_i[ev_src_r]\n'
            '            && !pe_off_r[ev_src_r] && !pe_conflict_r[ev_src_r]',
            '(rec_w.gstate == GS_NO_DA_C) && cfg_src_en_i[ev_src_r]',
        ),
    ),
    'retry_period_200': (
        ("DA_RETRY_MS_C = 32'd100", "DA_RETRY_MS_C = 32'd200"),
    ),
    'tick_ge_to_gt': (
        ('(now_ms_i - retry_t0_r) >= DA_RETRY_MS_C', '(now_ms_i - retry_t0_r) > DA_RETRY_MS_C'),
    ),
    'kill_no_conflict_at_all': (
        (
            '                       || pe_conflict_r[maap_src_r]\n'
            '                       || (maap_conflict_valid_i\n'
            '                           && maap_conflict_src_i == maap_src_r);',
            '                       ;',
        ),
        (
            '        maap_kill_r <= !cfg_src_en_i[mreq_src_r] || pe_off_r[mreq_src_r]\n'
            '                       || pe_conflict_r[mreq_src_r]\n'
            '                       || (maap_conflict_valid_i\n'
            '                           && maap_conflict_src_i == mreq_src_r);',
            '        maap_kill_r <= !cfg_src_en_i[mreq_src_r] || pe_off_r[mreq_src_r];',
        ),
    ),
    'kill_no_disable_at_all': (
        (
            '  assign maap_kill_w = !cfg_src_en_i[maap_src_r] || pe_off_r[maap_src_r]\n'
            '                       || pe_conflict_r[maap_src_r]',
            '  assign maap_kill_w = pe_conflict_r[maap_src_r]',
        ),
        (
            '        maap_kill_r <= !cfg_src_en_i[mreq_src_r] || pe_off_r[mreq_src_r]\n'
            '                       || pe_conflict_r[mreq_src_r]',
            '        maap_kill_r <= pe_conflict_r[mreq_src_r]',
        ),
    ),
    'kill_sticky_off': (
        ("      if (!maap_accept_w && (maap_busy_r || gp_valid_r) && maap_kill_w)\n        maap_kill_r <= 1'b1;\n", ''),
    ),
    'no_probe_initset': (
        ("    if ((state_r == S_TXN_ACT) && txn_initset_w) set_init_w[tsrc_w]   = 1'b1;\n", ''),
    ),
    'no_lsn_initset': (
        ("          ev_initset_w = 1'b1;    // a listener appeared: retry allocation", ''),
    ),
    'no_conflict_daok_initset': (
        (
            '          ev_rec2_w.gstate = GS_NO_DA_C;   // nothing declared: no backoff\n'
            "          ev_initset_w     = 1'b1;",
            '          ev_rec2_w.gstate = GS_NO_DA_C;   // nothing declared: no backoff',
        ),
    ),
    'no_backoff_exit_initset': (
        ("            ev_initset_w     = 1'b1;   // re-allocate, then re-declare", ''),
    ),
    'no_enable_wait_clear': (
        ('if (!cfg_src_en_i[i] || set_conflict_w[i])', 'if (set_conflict_w[i])'),
    ),
    'eligible_only_init': (
        (
            '|| !(|pe_off_r || |pe_conflict_r || |pe_pcp_r\n'
            '                              || |pe_tmr_r || |pe_lsn_r || |pe_rel_r\n'
            '                              || (maap_avail_w && |init_ready_w));',
            '|| !(maap_avail_w && |init_ready_w);',
        ),
    ),
    'accept_kill_no_disable': (
        (
            'maap_kill_r <= !cfg_src_en_i[mreq_src_r] || pe_off_r[mreq_src_r]',
            "maap_kill_r <= 1'b0 && pe_off_r[mreq_src_r]",
        ),
    ),
    'init_no_enable_check': (
        ('if ((rec_w.gstate == GS_NO_DA_C) && cfg_src_en_i[ev_src_r]', 'if ((rec_w.gstate == GS_NO_DA_C)'),
    ),
    'eligible_ignores_init': (
        ('|| (maap_avail_w && |init_ready_w));', ');'),
    ),
    'tick_keeps_wait': (
        ("        retry_t0_r   <= now_ms_i;\n        retry_wait_r <= '0;", '        retry_t0_r   <= now_ms_i;'),
    ),
    # Event-class fairness, absent offers, and explicitly classified controls.
    'no_get_tx_state_response': (
        ("MT_GTX_ST_C: begin\n          resp_valid_o = 1'b1;",
         "MT_GTX_ST_C: begin\n          resp_valid_o = 1'b0;"),
    ),
    'elig_drop_conflict': (
        ('!(|pe_off_r || |pe_conflict_r || |pe_pcp_r',
         '!(|pe_off_r || |pe_pcp_r'),
    ),
    'elig_drop_pcp': (
        ('!(|pe_off_r || |pe_conflict_r || |pe_pcp_r',
         '!(|pe_off_r || |pe_conflict_r'),
    ),
    'elig_drop_tmr': (
        ('|| |pe_tmr_r || |pe_lsn_r || |pe_rel_r',
         '|| |pe_lsn_r || |pe_rel_r'),
    ),
    'elig_drop_lsn': (
        ('|| |pe_tmr_r || |pe_lsn_r || |pe_rel_r',
         '|| |pe_tmr_r || |pe_rel_r'),
    ),
    'init_busy_else_removed': (
        ("            ev_initset_w = 1'b1;  // maap busy: keep the request pending",
         ''),
    ),
    'kill_w_no_off': (
        ('  assign maap_kill_w = !cfg_src_en_i[maap_src_r] || pe_off_r[maap_src_r]\n',
         '  assign maap_kill_w = !cfg_src_en_i[maap_src_r]\n'),
    ),
    'init_ready_no_en': (
        ('pe_init_r & cfg_src_en_i & ~retry_wait_r',
         'pe_init_r & ~retry_wait_r'),
    ),
    'wait_only_on_accept': (
        ("if ((state_r == S_EV_ACT) && ev_to_maap_w && !ev_maap_rel_w)\n"
         "        retry_wait_r[ev_src_r] <= 1'b1;",
         "if (maap_accept_w && !mreq_rel_r)\n"
         "        retry_wait_r[mreq_src_r] <= 1'b1;"),
    ),
    'sticky_kill_ignores_accept': (
        ('if (!maap_accept_w && (maap_busy_r || gp_valid_r) && maap_kill_w)',
         'if ((maap_busy_r || gp_valid_r) && maap_kill_w)'),
    ),
}


# Equivalence is a control result, never a killed-mutant witness. These
# single terms overlap by construction; removing the whole cause is tested
# separately (kill_sticky_off / kill_no_conflict_at_all / kill_no_disable_at_all).
EQUIVALENT_MUTATIONS = {
    "sticky_kill_ignores_accept": "An accept requires availability: busy and gp_valid "
        "are both clear, so the sticky-kill condition is false on that edge.",
    "init_busy_else_removed": "INIT dispatch requires availability; no other request "
        "can become accepted or create a grant before this walk reaches its action.",
    "kill_w_no_off": "Disable is captured live while busy or holding a grant; an "
        "older OFF is captured at accept, so its pending copy adds no cancellation.",
    "no_sticky_gp_window": "Until GRANT consumes gp_valid, OFF/CONFLICT cannot dispatch; "
        "their pending bits retain every cancellation through the grant action.",
    "accept_kill_zero": "Cancellation on accept is pending on the next busy edge; "
        "a response cannot be consumed as a grant before that edge latches the kill.",
    "accept_kill_no_disable": "An accept-edge disable sets OFF, which persists and "
        "latches the kill on the next busy edge before grant consumption.",
    "kill_no_pending_conflict": "A conflict after accept latches the live kill; "
        "an older pending conflict is captured by the accept-side kill expression.",
}
# This control may change cycle traces; it is not an equivalence claim.
PERFORMANCE_MUTATIONS = {
    "init_ready_no_en": "The action-side enable guard prevents disabled allocation; "
        "the picker mask only avoids no-op visits and their command latency.",
}
REMOVED_EQUIVALENTS = {
    "no_reenable_wait_clear": "Removed !en_q_r: reset clears wait and each disabled "
        "edge already clears it through !cfg_src_en_i; no enable-edge clear is needed.",
}


def run_case(tree: Path, name: str, log_dir: Path) -> tuple[int, list[str], set[int], set[int]]:
    """Require a completed simulation and read its assertion failures."""
    result = subprocess.run(["make", "-C", str(tree / "tb/acmp_talker")],
                            capture_output=True, text=True, check=False)
    output = result.stdout + result.stderr
    tally = re.search(r"(\d+) checks: (\d+) PASS, (\d+) FAIL", output)
    if tally is None:
        raise RuntimeError(f"{name}: no simulation tally\n{output[-6000:]}")
    failures = [line for line in output.splitlines() if line.startswith("FAIL:")]
    (log_dir / f"{name}.txt").write_text(
        f"return code: {result.returncode}\n{tally.group()}\n" + "\n".join(failures) + "\n")
    seen = {int(line) for line in re.findall(r"CHECK_LOC:.*retry_cases.hpp:(\d+)", output)}
    killed = {int(line) for line in re.findall(r"FAIL_LOC:.*retry_cases.hpp:(\d+)", output)}
    return result.returncode, failures, seen, killed


def mutated_source(source: str, name: str) -> str:
    """The talker RTL with one named mutation planted; each anchor must occur once."""
    replacements = MUTATIONS[name]
    if isinstance(replacements[0], str):
        replacements = (replacements,)
    for old, new in replacements:
        if source.count(old) != 1:
            raise RuntimeError(f"{name}: mutation anchor is not unique")
        source = source.replace(old, new)
    return source


def scratch_case(case: tuple[str, str | None, Path]) -> tuple[int, list[str], set[int], set[int]]:
    """Copy the build inputs to a scratch tree of its own, plant the mutant if any, and run."""
    name, mutant, log_dir = case
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="pp128-mutants-") as scratch:
        tree = Path(scratch)
        for directory in ("hdl", "tb/acmp_talker", "tb/common"):
            shutil.copytree(root / directory, tree / directory,
                            ignore=shutil.ignore_patterns("obj_dir", "__pycache__"))
        rtl = tree / RTL
        if mutant is not None:
            rtl.write_text(mutated_source(rtl.read_text(), mutant))
        # Observe assertion sites in the scratch BFM only. This records a
        # failing witness for every new CHECK, not just one per scenario.
        cpp = tree / "tb/acmp_talker/sim_main.cpp"
        bfm = cpp.read_text()
        bfm = bfm.replace('  ++checks;',
                          '  printf("CHECK_LOC:%s:%d\\n", __FILE__, __LINE__); ++checks;', 1)
        bfm = bfm.replace('if (!(cond)) { ++fails;',
                          'if (!(cond)) { printf("FAIL_LOC:%s:%d\\n", __FILE__, __LINE__); ++fails;', 1)
        cpp.write_text(bfm)
        return run_case(tree, name, log_dir)


def main() -> int:
    """Run selected mutants and require assertion coverage for the full set."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--only", nargs="+", choices=MUTATIONS)
    add_jobs_argument(parser)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)
    rc, failures, seen, _ = scratch_case(("baseline", None, args.logs))
    if rc or failures:
        raise RuntimeError("baseline failed")
    witnesses: dict[int, list[str]] = {line: [] for line in seen}
    selected = args.only or list(MUTATIONS)
    cases = [(name, name, args.logs) for name in selected] + [("restored", None, args.logs)]
    with in_order(scratch_case, cases, args.jobs) as results:
        for name, (rc, failures, _, killed) in zip(selected, results):
            if name in EQUIVALENT_MUTATIONS:
                if rc or failures:
                    raise RuntimeError(f"{name}: equivalence control failed")
                print(f"EQUIVALENT {name}: baseline behavior retained", flush=True)
                continue
            if name in PERFORMANCE_MUTATIONS:
                if rc or failures:
                    raise RuntimeError(f"{name}: performance control failed")
                print(f"PERFORMANCE {name}: documented bounds retained", flush=True)
                continue
            if not rc or not failures:
                raise RuntimeError(f"{name}: survived the committed suite")
            print(f"KILLED {name}: rc={rc}, {len(failures)} assertion failures", flush=True)
            for line in killed:
                witnesses[line].append(name)
        rc, failures, _, _ = next(results)
    if rc or failures:
        raise RuntimeError("restored source failed")
    (args.logs / "coverage.txt").write_text("".join(
        f"retry_cases.hpp:{line}: {', '.join(names) or 'UNCOVERED'}\n"
        for line, names in sorted(witnesses.items())))
    missing = [line for line, names in witnesses.items() if not names]
    if not args.only and missing:
        raise RuntimeError(f"new assertions without a killed witness: {sorted(missing)}")
    equivalents = len(set(selected) & set(EQUIVALENT_MUTATIONS))
    performance = len(set(selected) & set(PERFORMANCE_MUTATIONS))
    print(f"PASS: {len(selected) - equivalents - performance} mutants killed; "
          f"{equivalents} equivalence controls; {performance} performance controls; "
          "baseline and restored rc 0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
