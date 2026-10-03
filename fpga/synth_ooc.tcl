# B.0 (runs in the Vivado 2020.1 container on the build box): out-of-context synthesis of a generated
# Rocket system for the ZedBoard's xc7z020 -- does it fit, and how fast is it.
#   vivado -mode batch -source synth_ooc.tcl -tclargs <rtl dir> <out dir>
set rtl [lindex $argv 0]
set out [lindex $argv 1]
file mkdir $out
set_part xc7z020clg484-1
foreach f [glob -nocomplain $rtl/*.sv $rtl/*.v] { read_verilog -sv $f }
synth_design -top ExampleRocketSystem -part xc7z020clg484-1 -mode out_of_context -flatten_hierarchy rebuilt -directive AreaOptimized_high
create_clock -name clk -period 20.000 [get_ports -regexp {.*clock$}]
report_utilization -file $out/util.rpt
report_utilization -hierarchical -hierarchical_depth 4 -file $out/util_hier.rpt
report_timing_summary -file $out/timing.rpt
write_checkpoint -force $out/synth.dcp
