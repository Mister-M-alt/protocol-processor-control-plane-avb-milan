# SPDX-License-Identifier: CERN-OHL-W-2.0
# The same OOC area instrument as ucpu_ooc.tcl, over the complete processor.
# Run in an empty build directory. Optional arguments: source tree for A/B,
# then N_STREAM_IN_P and N_STREAM_OUT_P for a non-default stream shape.
set SPEC [file normalize [file dirname [info script]]/../..]
if {$argc > 0} { set SPEC [file normalize [lindex $argv 0]] }
set SHAPE {}
if {$argc > 2} {
    set SHAPE [list -generic N_STREAM_IN_P=[lindex $argv 1] \
                    -generic N_STREAM_OUT_P=[lindex $argv 2]]
}
set_param general.maxThreads 8
set pkgs [lsort [glob $SPEC/hdl/*/*_pkg.sv]]
read_verilog -sv $pkgs
foreach src [lsort [glob $SPEC/hdl/*/*.sv]] {
    if {[lsearch -exact $pkgs $src] < 0} { read_verilog -sv $src }
}
exec python3 $SPEC/hdl/aecp/ucode/gen_ucode.py -o ucode.hex
exec python3 $SPEC/hdl/acmp/rom/gen_ltn_rom.py -o ltn_rom.hex
synth_design -mode out_of_context -top protocol_processor_top -part xc7a100tfgg484-2 {*}$SHAPE
create_clock -period 10.000 -name clk [get_ports clk_i]
report_utilization -hierarchical -file util_hier.rpt
report_utilization -file util.rpt
report_timing_summary -delay_type max -max_paths 3 -file timing.rpt
