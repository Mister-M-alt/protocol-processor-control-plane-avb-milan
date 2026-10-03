#!/usr/bin/env bash
# SPDX-License-Identifier: CERN-OHL-W-2.0
# Device portability: sv2v + yosys must elaborate every top (hdl/README rule 2).
# The tops array is the authoritative list, and the census below is what makes
# that sentence true rather than aspirational: six modules were absent from it
# and no run ever elaborated them, `protocol_processor_top` among them (#25).
# `pipefail` completes the trio. It reaches the `find | sort` pairs on the sv2v
# line and the census's `grep | awk | sort`, each of which must succeed for its
# line to mean anything; the yosys verdicts are read from the markers in
# gate.log and AND-OR lists that `set -e` deliberately leaves alone.
set -euo pipefail

# ---------------------------------------------------------------------------
# Allocator for the yosys processes below. See the block at its use site.
# ---------------------------------------------------------------------------
ldconfig_path() {
  local p
  for p in ldconfig /usr/sbin/ldconfig /sbin/ldconfig; do
    command -v "$p" >/dev/null 2>&1 && { printf '%s\n' "$p"; return 0; }
  done
  return 1
}

abs_path() {
  local p="$1" out d b
  if out="$(readlink -f -- "$p" 2>/dev/null)" && [ -n "$out" ]; then
    printf '%s\n' "$out"; return 0
  fi
  d="$(cd -- "$(dirname -- "$p")" 2>/dev/null && pwd)" || return 1
  b="$(basename -- "$p")"
  printf '%s/%s\n' "$d" "$b"
}

# EXISTING IS NOT LOADABLE. The loader refuses an unusable LD_PRELOAD by
# IGNORING it: the process still exits 0 and the complaint goes to stderr,
# which for a yosys worker here lands in that top's log and is never read. So
# the test is "stderr stayed empty", never "the command succeeded".
TRUE_BIN="$(type -P true 2>/dev/null || printf '')"
preload_is_usable() {
  local lib="$1" err
  [ -n "$TRUE_BIN" ] && [ -x "$TRUE_BIN" ] || return 1
  err="$(LD_PRELOAD="$lib" "$TRUE_BIN" 2>&1 >/dev/null)" || return 1
  [ -z "$err" ]
}
preload_refusal() {
  [ -n "$TRUE_BIN" ] && [ -x "$TRUE_BIN" ] || { printf 'no external true(1) to probe with\n'; return 0; }
  LD_PRELOAD="$1" "$TRUE_BIN" 2>&1 >/dev/null | head -1
}

select_malloc() {
  local want="${YOSYS_MALLOC-}" cand lc abs
  case "$want" in
    none) return 0 ;;
    "")   ;;
    *)    [ -e "$want" ] || { echo "YOSYS_MALLOC=$want: no such file" >&2; return 2; }
          abs="$(abs_path "$want")" || {
            echo "YOSYS_MALLOC=$want: cannot be resolved to an absolute path" >&2; return 2; }
          preload_is_usable "$abs" || {
            echo "YOSYS_MALLOC=$want: the loader will not preload it ($abs)" >&2
            echo "  $(preload_refusal "$abs")" >&2
            return 2; }
          printf '%s\n' "$abs"; return 0 ;;
  esac
  # An auto-detected candidate the loader will not take is SKIPPED, not
  # refused: the default is "use jemalloc when it is installed", and a broken
  # one is not installed. An explicit request is refused above, because there
  # the caller named it and is owed an answer.
  if lc="$(ldconfig_path)"; then
    while IFS= read -r cand; do
      [ -e "$cand" ] && preload_is_usable "$cand" && { printf '%s\n' "$cand"; return 0; }
    done < <("$lc" -p 2>/dev/null | sed -n 's/^.* => //p' | grep -F libjemalloc.so.2)
  fi
  for cand in /usr/lib/libjemalloc.so.2 /usr/lib64/libjemalloc.so.2 \
              /usr/local/lib/libjemalloc.so.2; do
    [ -e "$cand" ] && preload_is_usable "$cand" && { printf '%s\n' "$cand"; return 0; }
  done
  return 0
}

# An EMPTY selection must actively UNSET LD_PRELOAD, not merely decline to set
# it: a caller who already exported one would otherwise have it inherited while
# this script reported "system".
apply_malloc_env() {
  local lib="${1-}"
  if [ -n "$lib" ]; then export LD_PRELOAD="$lib"; else unset LD_PRELOAD; fi
}

selftest_alloc() {
  local rc=0 skipped=0 out probe found dir base
  out="$(YOSYS_MALLOC=none select_malloc)" || rc=1
  [ -z "$out" ] || { echo "selftest: YOSYS_MALLOC=none selected '$out'" >&2; rc=1; }

  probe="$(mktemp)"
  if out="$(YOSYS_MALLOC="$probe" select_malloc 2>/dev/null)"; then
    echo "selftest: an unloadable YOSYS_MALLOC was accepted as '$out'" >&2; rc=1
  fi
  if out="$(YOSYS_MALLOC="$probe.absent" select_malloc 2>/dev/null)"; then
    echo "selftest: a missing YOSYS_MALLOC was accepted as '$out'" >&2; rc=1
  fi
  rm -f "$probe"

  found="$(unset YOSYS_MALLOC; select_malloc)" || rc=1
  if [ -n "$found" ]; then
    { [ -e "$found" ] && preload_is_usable "$found"; } || {
      echo "selftest: the default selected an unusable '$found'" >&2; rc=1; }
    dir="$(dirname -- "$found")"; base="$(basename -- "$found")"
    out="$(cd "$dir" && YOSYS_MALLOC="./$base" select_malloc)" || rc=1
    [ "$out" = "$found" ] || {
      echo "selftest: relative path resolved to '$out', expected '$found'" >&2; rc=1; }
  else
    echo "selftest: SKIPPED the relative-path and usable-default arms" \
         "(no preloadable jemalloc on this machine)"
    skipped=1
  fi

  out="$(export LD_PRELOAD=/inherited/from/the/caller.so
         apply_malloc_env ""; printf '%s' "${LD_PRELOAD-<unset>}")"
  [ "$out" = "<unset>" ] || {
    echo "selftest: an inherited LD_PRELOAD survived an empty selection as '$out'" >&2; rc=1; }
  out="$(unset LD_PRELOAD; apply_malloc_env /x/y.so; printf '%s' "${LD_PRELOAD-<unset>}")"
  [ "$out" = "/x/y.so" ] || {
    echo "selftest: a selected library did not reach the child env ('$out')" >&2; rc=1; }

  [ "$rc" -eq 0 ] && echo "allocator selection self-test: PASS$([ "$skipped" -eq 1 ] && echo ' (with skips)')"
  return "$rc"
}

case "${1-}" in
  --selftest-alloc) selftest_alloc; exit $? ;;
  "") ;;
  *) echo "usage: $0 [--selftest-alloc]" >&2; exit 2 ;;
esac

# RESOLVED BEFORE THE FIRST `cd`, so a relative YOSYS_MALLOC still means what it
# meant in the caller's directory: this script changes directory twice.
MALLOC_LIB="$(select_malloc)" || exit 2

cd "$(dirname "$0")/../.."

# ONE ENTRY PER MODULE UNDER hdl/. The census after it fails the gate when that
# stops holding, so a new module cannot be added without a top.
tops=(KL_aecp_desc_mem_guard KL_aecp_ucpu KL_aecp_desc_store KL_aecp_dyn_state KL_aecp_resp_buf KL_aecp_nvm_writer KL_aecp_engine KL_aecp_notify KL_aecp_ca_originator KL_pp_timer_service KL_pp_prng KL_pp_rx_slots KL_pp_tx_slots KL_pp_release_merge
      KL_pp_rx_validator KL_pp_normalizer KL_pp_dispatch KL_pp_scoreboard
      KL_pp_event_router KL_pp_originator KL_pp_tx_arbiter KL_pp_trace_ring
      KL_pp_side_port KL_pp_nvm_port KL_pp_nvm_mgr_arb KL_adp_engine KL_pp_acmp_listener
      KL_pp_acmp_lsn_admit
      KL_acmp_talker KL_pp_maap KL_srp_decoder KL_srp_encoder KL_srp_domain
      KL_srp_vlan KL_srp_talker_fsm KL_srp_listener_fsm
      KL_acmp_nvm_shadow KL_mrp_strip KL_pp_dispatch_fifo KL_srp_admission
      KL_srp_top protocol_processor_top)

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
# The two substitutions below are split on purpose - sv2v takes one file per
# word - and stay on one line, which is the form Rule 9 kept for source lists.
sv2v $(find hdl -name '*_pkg.sv' | sort) $(find hdl -name '*.sv' ! -name '*_pkg.sv' | sort) > "$work/all.v"  # shellcheck disable=SC2086 # deliberate word split of two source lists

# THE ARRAY IS ONLY AUTHORITATIVE IF SOMETHING ENFORCES IT. Declared modules are
# read from the sources, never from a second hand-written list, so the two
# cannot drift the way they already had: the union of what `hierarchy -top`
# reached over the previous 32 entries was exactly those 32, so the six missing
# modules were not covered transitively either. They are read from all.v, the
# sv2v lowering of every source that yosys reads below, and not from the .sv
# text: sv2v writes each module as `module NAME` at the start of a line however
# its source lays the header out, so a header split across lines is counted. It
# keeps a lifetime keyword (`module automatic NAME`), so the name is the last word.
census() {
  local declared missing stale
  declared="$(grep -oE '^module[[:space:]]+((automatic|static)[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*' \
                "$work/all.v" | awk '{print $NF}' | sort -u)"
  missing="$(comm -23 <(printf '%s\n' "$declared") \
                      <(printf '%s\n' "${tops[@]}" | sort -u))"
  if [ -n "$missing" ]; then
    echo "modules declared under hdl/ with no entry in the tops array:" >&2
    sed 's/^/  /' <<<"$missing" >&2
    echo "extend tops (hdl/README rule 2) - the array is the authoritative list" >&2
    exit 1
  fi
  stale="$(comm -13 <(printf '%s\n' "$declared") \
                    <(printf '%s\n' "${tops[@]}" | sort -u))"
  if [ -n "$stale" ]; then
    echo "tops array names modules that no longer exist under hdl/:" >&2
    sed 's/^/  /' <<<"$stale" >&2
    exit 1
  fi
}
census

( cd hdl/aecp/ucode && python3 gen_ucode.py -o "$work/ucode.hex" >/dev/null )
( cd hdl/acmp/rom && python3 gen_ltn_rom.py -o "$work/ltn_rom.hex" >/dev/null 2>&1 || python3 gen_ltn_rom.py > /dev/null; cp ltn_rom.hex "$work/" 2>/dev/null || true )
cd "$work"

# THIS GATE IS ALLOCATION-BOUND TOO, and the allocator is the one lever that
# costs nothing: 48.12 s -> 35.97 s end to end on one machine (-25.2%, median of
# three, parsed once) with an identical verdict for every top. The integrator's own gate measured the same class of
# win and proved the netlist byte-identical across glibc, tcmalloc, jemalloc
# and mimalloc (kebag-logic/milan-fpga#286, #288). Speed only, never results.
#
# APPLIED HERE, after sv2v and the ROM generators have already run, so it
# reaches every yosys below - the elaboration run and the synth_xilinx
# regression - and neither sv2v (a GHC binary) nor the python3 generators, none
# of which were measured under a replacement allocator. Optional in both
# directions: this gate still needs only sv2v, yosys and python3 on PATH.
#
#   YOSYS_MALLOC=<path>   preload that library
#   YOSYS_MALLOC=none     run yosys under the system allocator
#   unset                 use jemalloc when it is installed
apply_malloc_env "$MALLOC_LIB"
echo "yosys allocator: ${MALLOC_LIB:-system}"

# PARSED ONCE (#25). This script lowers the whole tree into one all.v, and it
# used to start one yosys per top, each beginning with `read_verilog all.v`:
# the design was parsed once per top. Now one yosys reads all.v once, keeps
# that parse with `design -save`, and elaborates each top from a `design -load`
# of it, so every top still starts from the same untouched design and runs the
# same `hierarchy -check; proc; opt_clean` it always did.
#
# `-defer` keeps the parse free of elaboration: each module is elaborated under
# the top that reaches it, never at read time, so an elaboration error is raised
# inside a named top's step. It costs no coverage, because every module is a
# top (the census above): #25's second correction measured that an unresolved
# instance in a module no top names is missed with or without `-defer`, and that
# the tops array, not the reading mode, is what elaborates a module.
#
# A FAILURE NAMES ITS MODULE. yosys stops at its first error, so each top's step
# is bracketed by two markers on stderr (stdout is block-buffered into the log
# and lost when yosys exits on an error; stderr is not): the top whose `@@begin`
# is in the log and whose `@@ok` is not is the one that failed, and the log's
# first ERROR line says why. A fresh run then takes the tops after it, which
# parses again, so a red gate still gives every top a verdict, in inventory
# order; a green gate parses exactly once.
gate_script() {
  local t
  printf 'read_verilog -defer all.v\ndesign -save parsed\n'
  for t in "$@"; do
    printf 'log -stderr @@begin %s\ndesign -load parsed\n' "$t"
    printf 'hierarchy -check -top %s\nproc\nopt_clean\nlog -stderr @@ok %s\n' "$t" "$t"
  done
}

# The module of all.v holding the line a parse error cites, when it cites one.
parse_site() {
  local n
  n="$(grep -oE 'all\.v:[0-9]+' <<<"$1" | head -1 | cut -d: -f2)" || return 0
  [ -n "$n" ] || return 0
  awk -v n="$n" 'NR > n { exit } /^module[ \t]/ { m = $2 }
                 END { sub(/[^A-Za-z0-9_].*/, "", m); if (m != "") print m }' all.v
}

elaborate_tops() {
  local fail=0 parses=0 red yrc why i t site
  local -a todo=("${tops[@]}") run=()
  while [ "${#todo[@]}" -gt 0 ]; do
    parses=$((parses + 1))
    gate_script "${todo[@]}" > gate.ys
    yrc=0; yosys -q -s gate.ys > gate.log 2>&1 || yrc=$?
    why="$(grep -m1 'ERROR' gate.log)" || why="yosys exited $yrc with no ERROR line"
    run=("${todo[@]}"); todo=(); red=0
    for i in "${!run[@]}"; do
      t="${run[$i]}"
      if grep -qxF "@@ok $t" gate.log; then
        echo "YOSYS OK  $t"
      elif grep -qxF "@@begin $t" gate.log; then
        echo "YOSYS FAIL $t: $why"
        red=1; todo=("${run[@]:i+1}"); break
      else
        # Not even this top began, so the parse itself failed and no top is proven.
        site="$(parse_site "$why")"
        echo "YOSYS FAIL all.v${site:+ in module $site}: $why"
        printf 'YOSYS FAIL %s: not elaborated, the parse failed\n' "${run[@]:i}"
        red=1; break
      fi
    done
    [ "$red" -eq 1 ] || [ "$yrc" -eq 0 ] || {
      echo "YOSYS FAIL every top passed, but yosys exited $yrc: $why"; red=1; }
    [ "$red" -eq 0 ] || fail=1
  done
  echo "YOSYS ${#tops[@]} tops, all.v parsed $parses time(s)"
  [ "$fail" -eq 0 ] || exit 1
}
elaborate_tops

# Elaboration alone does not prove that inferred memories map onto the target
# FPGA. KL_aecp_engine contains the largest mixed-control RAM in this block, so
# carry it through the complete Xilinx memory-mapping flow as a regression gate.
# Six RAMB36 is the current engine shape: five existing stores plus exactly
# one staging store. Assert both facts so losing inference and expanding byte
# lanes fail in opposite directions.
yosys -q -p "read_verilog all.v; hierarchy -check -top KL_aecp_engine; synth_xilinx -family xc7 -flatten -top KL_aecp_engine; select -module KL_aecp_engine; select -assert-count 1 c:amap_stage_r* t:RAMB36E1 %i; select -assert-count 6 t:RAMB36E1" \
  && echo "YOSYS XILINX OK  KL_aecp_engine" \
  || { echo "YOSYS XILINX FAIL KL_aecp_engine"; exit 1; }
