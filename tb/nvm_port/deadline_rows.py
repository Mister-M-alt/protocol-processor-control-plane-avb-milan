# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""
The figures gate's rows for the deadline's pause, its randomized harness and
its bounds: one subject of their own, so they live beside `measure_figures.py`
rather than inside it, as the pre-fix matrix's forms do.

`deadline_rows` returns rows in the shape of that script's MUTATIONS table,
(name, README claim, [(file, old, new)]), for the working copy it is handed:

  * the round-2 reviews' planted defects in the pause logic, each with the
    reviewer's own edit text, so a row here is the plant the review ran:
    R436-2's Q1-Q10 and R437-2's Y1-Y16, four of each pair the same defect
    spelled twice, seven of them measured equivalent;
  * two of them again on the randomized harness at bound 3, where the reviews
    found the most false DEADLINEs: `make primary` is edited to build that
    instead of the suite;
  * the coincident model at TMO = 4096, with T6 as it is and as round 2 had it:
    the Makefile's TMO is edited, so `make primary` builds the suite there;
  * the owed READ's drain bounded by what the READ still owes (D27-D30), the
    unbounded drain on the randomized harness too;
  * a late grant whose err rides it made owed anyway (D31, R437-1's X20).

It reads no RTL and runs no build; the figures gate imports it.
"""

from pathlib import Path

Edit = tuple[Path, str, str]

_WAITS = ("                  || (((state_r == S_WEWAIT) || (state_r == S_WWAIT) || (state_r == S_RHWAIT)\n"
          "                       || (state_r == S_RPWAIT)) && !done_seen_r)")
_ALL_WAITS = ("S_WEWAIT", "S_WWAIT", "S_RHWAIT", "S_RPWAIT")
_TMO_CLR = "    else if (prog_w || (state_r == S_IDLE)) tmo_r <= '0;"
_TMO_INC = "    else if (owe_w && !tmo_hit_w)           tmo_r <= tmo_r + TMO_W_C'(1);"
_VERDICT = "  assign dl_w = owe_w && !prog_w && tmo_hit_w;"
_OWE_PUMPS = "                  || (state_r == S_WHPUMP) || (state_r == S_RHCOLL)\n"


def _q_wait(state: str) -> list[tuple[str, str]]:
    """R436-2's plant: the latched-terminal exemption taken from one wait state."""
    rest = [s for s in _ALL_WAITS if s != state]
    return [(_WAITS, f"                  || (state_r == {state})\n"
                     f"                  || (((state_r == {rest[0]}) || (state_r == {rest[1]})"
                     f" || (state_r == {rest[2]})) && !done_seen_r)")]


def _y_wait(state: str) -> list[tuple[str, str]]:
    """R437-2's plant: the same, its own spelling."""
    keep = " || ".join(f"(state_r == {s})" for s in _ALL_WAITS if s != state)
    return [(_WAITS + "\n",
             f"                  || (({keep}) && !done_seen_r) || (state_r == {state})\n")]


_Q9 = [(_VERDICT, "  assign dl_w = !prog_w && tmo_hit_w && (state_r != S_IDLE) && (state_r != S_FIN);")]
_RREADY_CLEARS = [(_TMO_CLR, "    else if (prog_w || (state_r == S_IDLE) || "
                             "((state_r == S_RPPUMP) && !nvm_rready_i)) tmo_r <= '0;")]
_WVALID_CLEARS = [(_TMO_CLR, "    else if (prog_w || (state_r == S_IDLE) || "
                             "((state_r == S_WDPUMP) && !nvm_wvalid_i)) tmo_r <= '0;")]

#: (name, [(old, new)] on the port's RTL), in the reviews' own order.
PLANTS: list[tuple[str, list[tuple[str, str]]]] = [
    ("Q1", _q_wait("S_WEWAIT")), ("Q2", _q_wait("S_RHWAIT")),
    ("Q3", _q_wait("S_WWAIT")), ("Q4", _q_wait("S_RPWAIT")),
    ("Q5", [(_TMO_INC, "    else if (!tmo_hit_w)                    tmo_r <= tmo_r + TMO_W_C'(1);")]),
    ("Q6", _RREADY_CLEARS), ("Q7", _WVALID_CLEARS),
    ("Q8", [(_TMO_CLR, "    else if (prog_w || (state_r == S_IDLE) || (state_r == S_WHDR)"
                       " || (state_r == S_RHFWD)) tmo_r <= '0;")]),
    ("Q9", _Q9),
    ("Q10", [(_TMO_CLR, "    else if (prog_w || (state_r == S_FIN))  tmo_r <= '0;")]),
    ("Y1", _y_wait("S_WEWAIT")), ("Y2", _y_wait("S_RHWAIT")),
    ("Y3", _y_wait("S_WWAIT")), ("Y4", _y_wait("S_RPWAIT")),
    ("Y5", [(_TMO_INC, "    else if (!tmo_hit_w && (state_r != S_FIN)) tmo_r <= tmo_r + TMO_W_C'(1);")]),
    ("Y6", [(_OWE_PUMPS, _OWE_PUMPS[:-1] + " || (state_r == S_RHFWD)\n")]),
    ("Y7", [(_OWE_PUMPS, _OWE_PUMPS[:-1] + " || (state_r == S_WHDR)\n")]),
    ("Y8", [("  assign owe_w    = req_st_w\n", "  assign owe_w    = (req_st_w && !owed_r)\n")]),
    ("Y9", [("                  || ((state_r == S_WDPUMP) && nvm_wvalid_i)\n",
             "                  || ((state_r == S_WDPUMP) && nvm_wvalid_i && dev_wready_i)\n")]),
    ("Y10", [("                  || ((state_r == S_RPPUMP) && nvm_rready_i);",
              "                  || ((state_r == S_RPPUMP) && nvm_rready_i && dev_rvalid_i);")]),
    ("Y11", [(_VERDICT, "  assign dl_w = (owe_w || (state_r == S_WDPUMP) || (state_r == S_RPPUMP))"
                        " && !prog_w && tmo_hit_w;")]),
    ("Y12", [("                  || (dev_rvalid_i && dev_rready_o);",
              "                  || (dev_rvalid_i && dev_rready_o)\n"
              "                  || (nvm_rvalid_o && nvm_rready_i) || (nvm_wvalid_i && nvm_wready_o);")]),
    ("Y13", _RREADY_CLEARS), ("Y14", _WVALID_CLEARS),
    ("Y15", [("  logic [TMO_W_C-1:0] tmo_r;        // owed cycles without the owed event",
              "  logic [TMO_W_C-2:0] tmo_r;        // owed cycles without the owed event")]),
    ("Y16", [(_VERDICT,
              "  assign dl_w = (state_r != S_IDLE) && (state_r != S_FIN) && !prog_w && tmo_hit_w;")]),
]

_TMO_LINE = ("TMO       = 100\n", "TMO       = {bound}\n")
_FUZZ_AT_3 = ("primary:\n\t@mkdir -p obj_dir\n\t$(call suite,$(TMO),obj_dir)\n",
              "primary:\n\t@mkdir -p obj_dir\n\t$(call fuzz_at,3)\n")

#: The owed READ's drain: unbounded, as round 2 had it (D27); what the READ
#: still owes taken as its whole length (D28); a drained byte not counted
#: (D29); what is owed taken again at a later deadline (D30).
_UNBOUNDED = [("  assign drain_w = owed_r && owed_rd_r && (owed_left_r != 16'd0);",
               "  assign drain_w = owed_r && owed_rd_r;")]
_WHOLE_LENGTH = [("  assign left_w  = (state_r == S_RHCOLL) ? (HDR_LEN_C - 16'(hidx_r))",
                  "  assign left_w  = (state_r == S_RHCOLL) ? HDR_LEN_C"),
                 ("                 : (state_r == S_RPPUMP) ? (plen_r - bcnt_r)",
                  "                 : (state_r == S_RPPUMP) ? plen_r")]
_UNCOUNTED = [("    else if (drain_w && dev_rvalid_i) owed_left_r <= owed_left_r - 16'd1;\n", "")]
_RETAKEN = [("    else if (dl_w && !owed_r)         owed_left_r <= left_w;",
             "    else if (dl_w)                    owed_left_r <= left_w;")]

#: R437-1's X20: a late grant whose err rides it made owed anyway (D31).
_LATE_ERR_OWED = [("      end else if (lg_r && dev_gnt_i && !dev_done_i && !dev_err_i) begin",
                   "      end else if (lg_r && dev_gnt_i && !dev_done_i) begin")]

#: The round-2 T6, its poke a fixed 2 * TMO / 5 cycles after the accept, which
#: under the coincident model fell after the commit had ended from TMO = 500 up.
_T6_TIMED = ("  for (long i = 0; i < kOpTimeoutCycles && !(h.ops.size() == 1 && h.d_busy); ++i) h.tick();\n"
             "  CHECK(dut->nvm_busy_o && h.ops.size() == 1 && h.d_busy,\n"
             "        \"T6 op still in flight at the poke, its ERASE taken and not done\");",
             "  for (int i = 0; i < kStageCycles; ++i) h.tick();\n"
             "  CHECK(dut->nvm_busy_o, \"T6 op still in flight at the poke\");")


def deadline_rows(rtl: Path, sim: Path, mk: Path,
                  coincident: list[Edit]) -> list[tuple[str, str, list[Edit]]]:
    """The rows, as edits of the working copy's RTL, harness and Makefile;
    `coincident` is the gate's own coincident-completion model."""
    def on_rtl(pairs: list[tuple[str, str]]) -> list[Edit]:
        return [(rtl, old, new) for old, new in pairs]

    plants = {name: on_rtl(pairs) for name, pairs in PLANTS}

    at_4096 = [(mk, _TMO_LINE[0], _TMO_LINE[1].format(bound=4096))]
    fuzz_at_3 = [(mk, *_FUZZ_AT_3)]
    return [
        # each plant against the suite, where T30 is what fails the two that
        # passed round 2's; the equivalent ones are measured as such
        *[(name, rf"\| {name} \|[^|]*\| fails (\d+) of", edits) for name, edits in plants.items()],
        # ...those two on the randomized harness at bound 3
        ("Q1/fuzz", r"Q1 under the randomized harness at bound 3 \*\*fails (\d+) of",
         fuzz_at_3 + plants["Q1"]),
        ("Q9/fuzz", r"Q9 under the randomized harness at bound 3 \*\*fails (\d+) of",
         fuzz_at_3 + plants["Q9"]),
        # T6 at a bound far above the Makefile's, under the model that broke it
        ("coincident/4096", r"coincident model at `TMO` = 4096 \*\*fails (\d+) of",
         coincident + at_4096),
        ("T6-timed/coincident/4096", r"with round 2's T6 \*\*fails (\d+) of",
         coincident + at_4096 + [(sim, *_T6_TIMED)]),
        # the owed READ's drain, bounded by what the READ still owes
        ("D27", r"\*\*D27\*\*.*?\*\*fails (\d+) of", on_rtl(_UNBOUNDED)),
        ("D28", r"\*\*D28\*\*.*?\*\*fails (\d+) of", on_rtl(_WHOLE_LENGTH)),
        ("D29", r"\*\*D29\*\*.*?\*\*fails (\d+) of", on_rtl(_UNCOUNTED)),
        ("D30", r"\*\*D30\*\*.*?\*\*fails (\d+) of", on_rtl(_RETAKEN)),
        ("D27/fuzz", r"D27 under the randomized harness at bound 3 \*\*fails (\d+) of",
         fuzz_at_3 + on_rtl(_UNBOUNDED)),
        # the late grant that carries an err
        ("D31", r"\*\*D31\*\*.*?\*\*fails (\d+) of", on_rtl(_LATE_ERR_OWED)),
    ]
