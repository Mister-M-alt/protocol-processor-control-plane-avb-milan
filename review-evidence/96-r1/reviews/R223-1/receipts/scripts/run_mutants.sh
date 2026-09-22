#!/usr/bin/env bash
# R223-1: every mutant runs sequentially (Verilator capped at 8 jobs by
# bin/verilator8), each in its own scratch copy under /tmp/r223-96-r1/mut/.
set -uo pipefail
M=/tmp/r223-96-r1/rmut.sh
TOP=hdl/top/protocol_processor_top.sv
DOM=hdl/srp/KL_srp_domain.sv
SRPT=hdl/srp/KL_srp_top.sv
WRAP=tb/pp_top/pp_top_wrap.sv
BIND="      .DOM_DEF_VID_P    (SRP_DOM_DEF_VID_P),"
run() { echo "===== $1 ($2)"; bash "$M" "$@" 2>&1 | grep -vE '^DIFFERS' ; echo "===== $1 done rc=${PIPESTATUS[0]}"; }

# control: the unmutated head through the same runner
run R00_control fixture - - -
# reproductions of the author's M25-M31
run R25_binding_removed both $TOP "$BIND
" ""
run R26_bound_literal_2 fixture $TOP "$BIND" "      .DOM_DEF_VID_P    (16'd2),"
run R27_trunc_12b fixture $TOP "$BIND" "      .DOM_DEF_VID_P    (16'(SRP_DOM_DEF_VID_P[11:0])),"
run R28_wrong_child_param both $TOP "$BIND" "      .DOM_DEF_PRIO_P   (SRP_DOM_DEF_VID_P[7:0]),"
run R29_top_default_3 both $TOP "parameter logic [15:0] SRP_DOM_DEF_VID_P   = 16'd2," "parameter logic [15:0] SRP_DOM_DEF_VID_P   = 16'd3,"
run R30_wrap_override_removed fixture $WRAP '      .SRP_DOM_DEF_VID_P (`PP_TOP_SRP_DOM_DEF_VID),
' ""
run R31_child_default_7_control both $SRPT "parameter logic [15:0] DOM_DEF_VID_P  = 16'd2," "parameter logic [15:0] DOM_DEF_VID_P  = 16'd7,"
# reviewer-designed mutants
run R32_trunc_8b fixture $TOP "$BIND" "      .DOM_DEF_VID_P    (16'(SRP_DOM_DEF_VID_P[7:0])),"
run R33_byte_swap fixture $TOP "$BIND" "      .DOM_DEF_VID_P    ({SRP_DOM_DEF_VID_P[7:0], SRP_DOM_DEF_VID_P[15:8]}),"
run R34_top_param_12b both $TOP "parameter logic [15:0] SRP_DOM_DEF_VID_P   = 16'd2," "parameter logic [11:0] SRP_DOM_DEF_VID_P   = 12'd2,"
run R35_child_reset_literal fixture $DOM "      decl_vid_r          <= DEF_VID_P;" "      decl_vid_r          <= 16'd2;"
run R36_child_revert_literal fixture $DOM "        decl_vid_r    <= DEF_VID_P;" "        decl_vid_r    <= 16'd2;"
run R37_child_linkup_decl_literal fixture $DOM "        q0_val_r   <= {CLASS_A_ID_C, DEF_PRIO_P, DEF_VID_P};" "        q0_val_r   <= {CLASS_A_ID_C, DEF_PRIO_P, 16'd2};"
run R38_srp_top_inner_literal fixture $SRPT "      .DEF_VID_P  (DOM_DEF_VID_P)" "      .DEF_VID_P  (16'd2)"
echo "ALL MUTANTS DONE"
