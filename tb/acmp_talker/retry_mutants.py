#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Kill issue #128 mutants in a scratch tree; never edit the source checkout."""

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


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
}


def run_case(tree: Path, name: str, log_dir: Path) -> tuple[int, list[str], set[int], set[int]]:
    """Require a completed simulation and read its assertion failures."""
    result = subprocess.run(["make", "-C", str(tree / "tb/acmp_talker")],
                            capture_output=True, text=True, timeout=900, check=False)
    output = result.stdout + result.stderr
    tally = re.search(r"(\d+) checks: (\d+) PASS, (\d+) FAIL", output)
    if tally is None:
        raise RuntimeError(f"{name}: no simulation tally\n{output[-6000:]}")
    failures = [line for line in output.splitlines() if line.startswith("FAIL: R")]
    (log_dir / f"{name}.txt").write_text(
        f"return code: {result.returncode}\n{tally.group()}\n" + "\n".join(failures) + "\n")
    seen = {int(line) for line in re.findall(r"CHECK_LOC:.*retry_cases.hpp:(\d+)", output)}
    killed = {int(line) for line in re.findall(r"FAIL_LOC:.*retry_cases.hpp:(\d+)", output)}
    return result.returncode, failures, seen, killed


def main() -> int:
    """Run selected mutants and require assertion coverage for the full set."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--only", nargs="+", choices=MUTATIONS)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="pp128-mutants-") as scratch:
        tree = Path(scratch)
        for directory in ("hdl", "tb/acmp_talker", "tb/common"):
            shutil.copytree(root / directory, tree / directory,
                            ignore=shutil.ignore_patterns("obj_dir", "__pycache__"))
        rtl = tree / "hdl/acmp/KL_acmp_talker.sv"
        source = rtl.read_text()
        # Observe assertion sites in the scratch BFM only. This records a
        # failing witness for every new CHECK, not just one per scenario.
        cpp = tree / "tb/acmp_talker/sim_main.cpp"
        bfm = cpp.read_text()
        bfm = bfm.replace('  ++checks;',
                          '  printf("CHECK_LOC:%s:%d\\n", __FILE__, __LINE__); ++checks;', 1)
        bfm = bfm.replace('if (!(cond)) { ++fails;',
                          'if (!(cond)) { printf("FAIL_LOC:%s:%d\\n", __FILE__, __LINE__); ++fails;', 1)
        cpp.write_text(bfm)
        rc, failures, seen, _ = run_case(tree, "baseline", args.logs)
        if rc or failures:
            raise RuntimeError("baseline failed")
        witnesses: dict[int, list[str]] = {line: [] for line in seen}
        selected = args.only or list(MUTATIONS)
        for name in selected:
            replacements = MUTATIONS[name]
            if isinstance(replacements[0], str):
                replacements = (replacements,)
            mutated = source
            for old, new in replacements:
                if mutated.count(old) != 1:
                    raise RuntimeError(f"{name}: mutation anchor is not unique")
                mutated = mutated.replace(old, new)
            rtl.write_text(mutated)
            rc, failures, _, killed = run_case(tree, name, args.logs)
            if not rc or not failures:
                raise RuntimeError(f"{name}: survived the new checks")
            print(f"KILLED {name}: rc={rc}, {len(failures)} new assertion failures", flush=True)
            for line in killed:
                witnesses[line].append(name)
        rtl.write_text(source)
        rc, failures, _, _ = run_case(tree, "restored", args.logs)
        if rc or failures:
            raise RuntimeError("restored source failed")
        (args.logs / "coverage.txt").write_text("".join(
            f"retry_cases.hpp:{line}: {', '.join(names) or 'UNCOVERED'}\n"
            for line, names in sorted(witnesses.items())))
        missing = [line for line, names in witnesses.items() if not names]
        if not args.only and missing:
            raise RuntimeError(f"new assertions without a killed witness: {sorted(missing)}")
    print(f"PASS: {len(selected)} mutants killed; baseline and restored rc 0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
